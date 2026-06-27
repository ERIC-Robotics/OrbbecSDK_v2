// orbbec_gst_publisher.cpp
//
// V4L2 camera -> GStreamer pipeline -> iceoryx (Orbbec/<name>/MJPEG)
//                                   -> HTTP MJPEG server
//
// If the camera already outputs MJPEG the frames are passed through
// directly (no decode/re-encode). Otherwise a software or hardware
// JPEG encoder is inserted.
//
// Encoder backend (auto-detected, override with flags):
//   1. nvvidconv + nvjpegenc — Jetson (--jetson)
//   2. nvjpegenc             — desktop NVIDIA GPU
//   3. jpegenc               — CPU software fallback (--cpu)
//
// Usage:
//   orbbec_gst_publisher                           (interactive)
//   orbbec_gst_publisher --device /dev/video0 --name cam0
//   orbbec_gst_publisher --device /dev/video0 --name cam0 --format MJPEG
//   orbbec_gst_publisher --jetson
//   orbbec_gst_publisher --cpu

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <fmt/format.h>

#include "iceoryx_posh/capro/service_description.hpp"
#include "iceoryx_posh/popo/publisher_options.hpp"
#include "iceoryx_posh/popo/untyped_publisher.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"
#include "iox/string.hpp"

#include "lflogger.hpp"
#include "orbbec_data.hpp"
#include "v4l2_probe.hpp"

std::atomic<bool> gRun{true};
void onSignal(int) { gRun.store(false); }

enum class Encoder {
  PASSTHROUGH,
  NVJPEGENC_JETSON,
  NVJPEGENC_DESKTOP,
  JPEGENC_CPU
};

bool gstHasElement(const char *name) {
  GstElementFactory *f = gst_element_factory_find(name);
  if (f) {
    gst_object_unref(f);
    return true;
  }
  return false;
}

bool checkNvjpegencWorking() {
  GstElement *p =
      gst_parse_launch("videotestsrc num-buffers=1 ! videoconvert ! "
                       "video/x-raw,format=I420 ! nvjpegenc ! fakesink",
                       nullptr);
  if (!p)
    return false;
  GstStateChangeReturn ret = gst_element_set_state(p, GST_STATE_PAUSED);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    gst_object_unref(p);
    return false;
  }
  GstBus *bus = gst_element_get_bus(p);
  GstMessage *msg = gst_bus_timed_pop_filtered(
      bus, 1000 * GST_MSECOND,
      static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_ASYNC_DONE));
  bool ok = true;
  if (msg) {
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
      ok = false;
    gst_message_unref(msg);
  }
  gst_object_unref(bus);
  gst_element_set_state(p, GST_STATE_NULL);
  gst_object_unref(p);
  return ok;
}

Encoder detectEncoder(bool forceJetson, bool forceCpu,
                      bool isMjpegPassthrough) {
  if (isMjpegPassthrough)
    return Encoder::PASSTHROUGH;
  if (forceJetson)
    return Encoder::NVJPEGENC_JETSON;
  if (forceCpu)
    return Encoder::JPEGENC_CPU;
  if (gstHasElement("nvjpegenc")) {
    if (checkNvjpegencWorking()) {
      LockFreeLogger::getInstance().info(
          "publisher", "nvjpegenc detected — using hardware JPEG encoder.");
      return Encoder::NVJPEGENC_DESKTOP;
    }
    LockFreeLogger::getInstance().warn(
        "publisher", "nvjpegenc found but failed. Falling back to jpegenc.");
  } else {
    LockFreeLogger::getInstance().warn("publisher",
                                       "nvjpegenc not found. Use --jetson for "
                                       "Jetson. Falling back to jpegenc.");
  }
  return Encoder::JPEGENC_CPU;
}

const char *encoderName(Encoder e) {
  switch (e) {
  case Encoder::PASSTHROUGH:
    return "MJPEG passthrough (no re-encoding)";
  case Encoder::NVJPEGENC_JETSON:
    return "nvvidconv + nvjpegenc (Jetson)";
  case Encoder::NVJPEGENC_DESKTOP:
    return "nvjpegenc (NVIDIA GPU)";
  case Encoder::JPEGENC_CPU:
    return "jpegenc (CPU)";
  }
  return "unknown";
}

// ── HTTP MJPEG server ─────────────────────────────────────────────────────
class MjpegServer {
public:
  explicit MjpegServer(int port) : port_(port) {}
  ~MjpegServer() { stop(); }

  void start() {
    running_.store(true);
    acceptThread_ = std::thread([this] { acceptLoop(); });
  }
  void stop() {
    if (!running_.exchange(false))
      return;
    if (listenFd_ >= 0)
      ::shutdown(listenFd_, SHUT_RDWR);
    frameCv_.notify_all();
    if (acceptThread_.joinable())
      acceptThread_.join();
    if (listenFd_ >= 0) {
      ::close(listenFd_);
      listenFd_ = -1;
    }
  }
  void pushJpeg(const uint8_t *data, size_t size) {
    {
      std::lock_guard<std::mutex> lk(frameMtx_);
      frame_.assign(data, data + size);
      frameReady_ = true;
    }
    frameCv_.notify_all();
  }
  int port() const { return port_; }

private:
  static constexpr const char *kHttpHdr =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=mjpeg_boundary\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: close\r\n"
      "\r\n";

  void acceptLoop() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
      perror("[MJPEG] socket");
      return;
    }
    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
        0) {
      perror("[MJPEG] bind");
      ::close(listenFd_);
      listenFd_ = -1;
      return;
    }
    ::listen(listenFd_, 8);
    LockFreeLogger::getInstance().info(
        "MJPEG", fmt::format("HTTP server listening on port {}", port_));
    while (running_.load()) {
      sockaddr_in ca{};
      socklen_t cl = sizeof(ca);
      int fd = ::accept(listenFd_, reinterpret_cast<sockaddr *>(&ca), &cl);
      if (fd < 0) {
        if (running_.load())
          perror("[MJPEG] accept");
        break;
      }
      char rbuf[2048];
      ::recv(fd, rbuf, sizeof(rbuf) - 1, 0);
      std::thread([this, fd] { serveClient(fd); }).detach();
    }
  }
  void serveClient(int fd) {
    if (::send(fd, kHttpHdr, strlen(kHttpHdr), MSG_NOSIGNAL) < 0) {
      ::close(fd);
      return;
    }
    while (running_.load()) {
      std::vector<uint8_t> jpeg;
      {
        std::unique_lock<std::mutex> lk(frameMtx_);
        frameCv_.wait_for(lk, std::chrono::seconds(2),
                          [this] { return frameReady_ || !running_.load(); });
        if (!running_.load())
          break;
        if (!frameReady_)
          continue;
        jpeg = frame_;
        frameReady_ = false;
      }
      std::string hdr = fmt::format("--mjpeg_boundary\r\n"
                                    "Content-Type: image/jpeg\r\n"
                                    "Content-Length: {}\r\n"
                                    "\r\n",
                                    jpeg.size());
      if (::send(fd, hdr.data(), hdr.size(), MSG_NOSIGNAL) < 0)
        break;
      if (::send(fd, jpeg.data(), jpeg.size(), MSG_NOSIGNAL) < 0)
        break;
      if (::send(fd, "\r\n", 2, MSG_NOSIGNAL) < 0)
        break;
    }
    ::close(fd);
  }

  int port_;
  int listenFd_{-1};
  std::atomic<bool> running_{false};
  std::thread acceptThread_;
  std::mutex frameMtx_;
  std::condition_variable frameCv_;
  std::vector<uint8_t> frame_;
  bool frameReady_{false};
};

struct Config {
  std::string device = ""; // empty = interactive
  std::string name = "cam0";
  std::string format = ""; // empty = interactive / auto
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t fps = 30;
  uint32_t quality = 85;
  uint32_t http_port = 9000;
  bool forceJetson = false;
  bool forceCpu = false;
  bool stream = false;         // enable HTTP MJPEG server
  std::string raw_stream = ""; // Unix socket for nvunixfdsink; empty = disabled
};

// ── GStreamer pipeline builder ────────────────────────────────────────────
std::string buildPipeline(const Config &c, Encoder enc) {
  const bool hasRaw = !c.raw_stream.empty();

  if (enc == Encoder::PASSTHROUGH) {
    // Camera outputs MJPEG directly — no re-encoding needed.
    // If raw_stream is set, tee: one branch stays compressed (appsink),
    // the other decodes to NV12/RGB and feeds nvunixfdsink.
    const std::string base =
        fmt::format("v4l2src device={device} "
                    "! image/jpeg,width={w},height={h},framerate={fps}/1 "
                    "! jpegparse ",
                    fmt::arg("device", c.device), fmt::arg("w", c.width),
                    fmt::arg("h", c.height), fmt::arg("fps", c.fps));

    if (!hasRaw) {
      return base + "! appsink name=mjpeg_sink sync=false "
                    "emit-signals=false max-buffers=2 drop=true";
    }

    // Decode MJPEG → raw for the nvunixfdsink branch.
    // On Jetson use nvjpegdec + nvvidconv → NV12(NVMM); otherwise CPU path.
    const std::string decode_seg =
        c.forceJetson ? "jpegdec ! nvvideoconvert ! "
                        "video/x-raw(memory:NVMM),format=NV12"
                      : "jpegdec ! videoconvert ! video/x-raw,format=RGB";

    return fmt::format("{base}! tee name=t "
                       "t. ! queue ! appsink name=mjpeg_sink sync=false "
                       "emit-signals=false max-buffers=2 drop=true "
                       "t. ! queue ! {decode} "
                       "! nvunixfdsink socket-path={socket} sync=false",
                       fmt::arg("base", base), fmt::arg("decode", decode_seg),
                       fmt::arg("socket", c.raw_stream));
  }

  // ── Non-passthrough: raw camera → encode → JPEG ───────────────────────
  // JPEG branch (to appsink) and raw branch (to nvunixfdsink) per encoder.
  std::string enc_branch;
  std::string raw_branch;

  switch (enc) {
  case Encoder::NVJPEGENC_JETSON:
    enc_branch =
        fmt::format("videoconvert ! video/x-raw,format=I420 "
                    "! nvvidconv ! video/x-raw(memory:NVMM),format=I420 "
                    "! nvjpegenc quality={q} ! jpegparse "
                    "! appsink name=mjpeg_sink sync=false emit-signals=false "
                    "max-buffers=2 drop=true",
                    fmt::arg("q", c.quality));
    raw_branch = fmt::format("nvvidconv ! video/x-raw(memory:NVMM),format=NV12 "
                             "! nvunixfdsink socket-path={socket} sync=false",
                             fmt::arg("socket", c.raw_stream));
    break;
  case Encoder::NVJPEGENC_DESKTOP:
    enc_branch =
        fmt::format("videoconvert ! video/x-raw,format=I420 "
                    "! nvjpegenc quality={q} ! jpegparse "
                    "! appsink name=mjpeg_sink sync=false emit-signals=false "
                    "max-buffers=2 drop=true",
                    fmt::arg("q", c.quality));
    raw_branch = fmt::format("videoconvert ! video/x-raw,format=NV12 "
                             "! nvunixfdsink socket-path={socket} sync=false",
                             fmt::arg("socket", c.raw_stream));
    break;
  default: // JPEGENC_CPU
    enc_branch =
        fmt::format("videoconvert ! video/x-raw,format=I420 "
                    "! jpegenc quality={q} ! jpegparse "
                    "! appsink name=mjpeg_sink sync=false emit-signals=false "
                    "max-buffers=2 drop=true",
                    fmt::arg("q", c.quality));
    raw_branch = fmt::format("videoconvert ! video/x-raw,format=RGB "
                             "! nvunixfdsink socket-path={socket} sync=false",
                             fmt::arg("socket", c.raw_stream));
    break;
  }

  const std::string source =
      fmt::format("v4l2src device={device} ! decodebin "
                  "! videoconvert ! videoscale ! videorate "
                  "! video/x-raw,width={w},height={h},framerate={fps}/1 ",
                  fmt::arg("device", c.device), fmt::arg("w", c.width),
                  fmt::arg("h", c.height), fmt::arg("fps", c.fps));

  if (!hasRaw) {
    return fmt::format("{src}! {enc}", fmt::arg("src", source),
                       fmt::arg("enc", enc_branch));
  }

  return fmt::format("{src}! tee name=t "
                     "t. ! queue ! {enc} "
                     "t. ! queue ! {raw}",
                     fmt::arg("src", source), fmt::arg("enc", enc_branch),
                     fmt::arg("raw", raw_branch));
}

// ── iceoryx + HTTP publish callback ──────────────────────────────────────
struct AppsinkCtx {
  MjpegServer *http_server{nullptr};
  iox::popo::UntypedPublisher *publisher{nullptr};
  std::string name;
  uint64_t frames_published{0};

  // FPS tracking
  uint64_t fps_count{0};
  std::chrono::steady_clock::time_point fps_window_start{
      std::chrono::steady_clock::now()};
};

GstFlowReturn onNewSample(GstAppSink *sink, gpointer ud) {
  auto *ctx = static_cast<AppsinkCtx *>(ud);

  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_ERROR;

  GstBuffer *buf = gst_sample_get_buffer(sample);
  GstMapInfo map;
  if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
    const uint64_t ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    const auto payloadSize =
        static_cast<uint32_t>(orbbec::kJpegHeaderSize + map.size);

    ctx->publisher->loan(payloadSize)
        .and_then([&](auto &userPayload) {
          auto *frame = static_cast<orbbec::JpegFrameData *>(userPayload);
          frame->timestamp_ns = ts_ns;
          frame->sequence_number = ctx->frames_published;
          frame->is_keyframe = 1;
          frame->data_size = static_cast<uint32_t>(map.size);
          std::memcpy(frame->data, map.data, map.size);
          ctx->publisher->publish(userPayload);
        })
        .or_else([](auto &err) {
          LockFreeLogger::getInstance().error(
              "publisher",
              fmt::format(
                  "iceoryx loan failed (err={}). Pool may be undersized.",
                  static_cast<int>(err)));
        });

    if (ctx->http_server)
      ctx->http_server->pushJpeg(map.data, map.size);

    ++ctx->frames_published;
    ++ctx->fps_count;

    // Print FPS every second
    auto now = std::chrono::steady_clock::now();
    double elapsed =
        std::chrono::duration<double>(now - ctx->fps_window_start).count();
    if (elapsed >= 1.0) {
      double fps = static_cast<double>(ctx->fps_count) / elapsed;
      LockFreeLogger::getInstance().info(
          ctx->name,
          fmt::format("FPS: {:.1f}  published={}", fps, ctx->frames_published));
      ctx->fps_count = 0;
      ctx->fps_window_start = now;
    }

    gst_buffer_unmap(buf, &map);
  }
  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

int main(int argc, char **argv) {
  auto &log = LockFreeLogger::getInstance();
  log.initialize(std::make_unique<ConsoleAndFileLogWriter>(),
                 QueueMode::MANUAL);

  gst_init(&argc, &argv);
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  Config cfg;
  bool cliFormatSet = false;
  bool cliSizeSet = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--device" && i + 1 < argc)
      cfg.device = argv[++i];
    else if (a == "--name" && i + 1 < argc)
      cfg.name = argv[++i];
    else if (a == "--format" && i + 1 < argc) {
      cfg.format = argv[++i];
      cliFormatSet = true;
    } else if (a == "--width" && i + 1 < argc) {
      cfg.width = std::stoul(argv[++i]);
      cliSizeSet = true;
    } else if (a == "--height" && i + 1 < argc) {
      cfg.height = std::stoul(argv[++i]);
      cliSizeSet = true;
    } else if (a == "--fps" && i + 1 < argc)
      cfg.fps = std::stoul(argv[++i]);
    else if (a == "--quality" && i + 1 < argc)
      cfg.quality = std::stoul(argv[++i]);
    else if (a == "--port" && i + 1 < argc)
      cfg.http_port = std::stoul(argv[++i]);
    else if (a == "--jetson")
      cfg.forceJetson = true;
    else if (a == "--cpu")
      cfg.forceCpu = true;
    else if (a == "--stream")
      cfg.stream = true;
    else if (a == "--raw-stream" && i + 1 < argc)
      cfg.raw_stream = argv[++i];
    else if (a == "--help" || a == "-h") {
      std::cout
          << "Usage: orbbec_gst_publisher [OPTIONS]\n"
          << "  --device      <path>   V4L2 device (default: interactive)\n"
          << "  --name        <str>    Stream name / iceoryx instance "
             "(default: "
             "cam0)\n"
          << "  --format      <fmt>    MJPEG for passthrough, omit for "
             "auto-encode\n"
          << "  --width       <px>     Capture width  (default: 1280)\n"
          << "  --height      <px>     Capture height (default: 720)\n"
          << "  --fps         <n>      Frame rate (default: 30)\n"
          << "  --quality     <1-100>  JPEG quality for encoding (default: "
             "85)\n"
          << "  --port        <n>      HTTP MJPEG server port (default: 9000)\n"
          << "  --jetson               Force Jetson nvvidconv+nvjpegenc\n"
          << "  --cpu                  Force jpegenc (software)\n"
          << "  --stream               Enable HTTP MJPEG server (default: "
             "off)\n"
          << "  --raw-stream  <path>   Enable nvunixfdsink raw NV12/RGB branch "
             "at this Unix socket path\n"
          << "\nOutputs:\n"
          << "  iceoryx    : Orbbec/<name>/MJPEG  (always on)\n"
          << "  HTTP       : http://<host>:<port>/  (--stream only)\n"
          << "  raw frames : nvunixfdsink at --raw-stream path "
             "(NV12 on Jetson/NVJPEG, RGB on CPU)\n";
      log.shutdown();
      return 0;
    }
  }

  // ── Device resolution ────────────────────────────────────────────────
  bool isMjpegPassthrough = false;
  if (cfg.device.empty()) {
    const bool hasCliOverrides = cliFormatSet || cliSizeSet;
    if (hasCliOverrides) {
      // Non-interactive: find first device supporting the requested config
      const uint32_t wantFourcc =
          cliFormatSet ? v4l2probe::fourccFromString(cfg.format) : 0U;
      bool found = false;
      for (const auto &dev : v4l2probe::scanDevices()) {
        int fd = ::open(dev.path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0)
          continue;
        auto fmts = v4l2probe::enumerateFormats(fd);
        ::close(fd);
        for (const auto &f : fmts) {
          if (wantFourcc && f.fourcc != wantFourcc)
            continue;
          bool sizeOk = f.sizes.empty(); // no enumerated sizes → assume ok
          for (const auto &sz : f.sizes) {
            if (!cliSizeSet ||
                (sz.width == cfg.width && sz.height == cfg.height)) {
              sizeOk = true;
              break;
            }
          }
          if (!sizeOk)
            continue;
          cfg.device = dev.path;
          if (!cliFormatSet)
            cfg.format = v4l2probe::fourccToString(f.fourcc);
          isMjpegPassthrough = (f.fourcc == V4L2_PIX_FMT_MJPEG);
          log.info("publisher",
                   fmt::format("Auto-selected: {} ({}) — {} {}x{}", dev.path,
                               dev.card, cfg.format, cfg.width, cfg.height));
          found = true;
          break;
        }
        if (found)
          break;
      }
      if (!found) {
        log.error(
            "publisher",
            fmt::format(
                "No V4L2 device found supporting {}{} — connect a camera or "
                "use --device.",
                cliFormatSet ? cfg.format + " " : "",
                cliSizeSet ? std::to_string(cfg.width) + "x" +
                                 std::to_string(cfg.height)
                           : ""));
        log.shutdown();
        return 1;
      }
    } else {
      // Fully interactive: walk the wizard
      auto sel = v4l2probe::interactiveSelect(cfg.name);
      if (!sel.valid)
        return 1;
      cfg.device = sel.device.path;
      cfg.name = sel.name;
      cfg.width = sel.size.width > 0 ? sel.size.width : cfg.width;
      cfg.height = sel.size.height > 0 ? sel.size.height : cfg.height;
      cfg.fps = sel.fps > 0 ? sel.fps : cfg.fps;
      cfg.format = v4l2probe::fourccToString(sel.format.fourcc);
      isMjpegPassthrough = (sel.format.fourcc == V4L2_PIX_FMT_MJPEG);
    }
  } else if (!cfg.format.empty()) {
    isMjpegPassthrough =
        (v4l2probe::fourccFromString(cfg.format) == V4L2_PIX_FMT_MJPEG);
  }

  if (isMjpegPassthrough)
    log.info("publisher", "Camera supports MJPEG natively — skipping encoder.");

  const Encoder enc =
      detectEncoder(cfg.forceJetson, cfg.forceCpu, isMjpegPassthrough);
  const std::string iceoryx_topic = fmt::format("Orbbec/{}/MJPEG", cfg.name);
  const std::string pipe_str = buildPipeline(cfg, enc);

  iox::runtime::PoshRuntime::initRuntime("orbbec_gst_publisher");

  iox::popo::PublisherOptions pubOpts;
  pubOpts.historyCapacity = 1U;
  auto publisher = std::make_unique<iox::popo::UntypedPublisher>(
      iox::capro::ServiceDescription{
          iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
          iox::capro::IdString_t(iox::TruncateToCapacity, cfg.name.c_str()),
          iox::capro::IdString_t(iox::TruncateToCapacity, "MJPEG")},
      pubOpts);

  log.info("publisher",
           fmt::format("=== orbbec_gst_publisher ===\n"
                       "  Device     : {}\n"
                       "  Name       : {}\n"
                       "  Size       : {}x{}@{} fps\n"
                       "  Encoder    : {}\n"
                       "  Quality    : {}\n"
                       "  iceoryx    : {}\n"
                       "  HTTP       : {}\n"
                       "  Raw stream : {}\n"
                       "============================\n"
                       "Pipeline:\n  {}",
                       cfg.device, cfg.name, cfg.width, cfg.height, cfg.fps,
                       encoderName(enc),
                       isMjpegPassthrough ? std::string("N/A (passthrough)")
                                          : std::to_string(cfg.quality),
                       iceoryx_topic,
                       cfg.stream
                           ? fmt::format("http://0.0.0.0:{}/", cfg.http_port)
                           : std::string("(disabled)"),
                       cfg.raw_stream.empty() ? "(disabled)" : cfg.raw_stream,
                       pipe_str));

  GError *gerr = nullptr;
  GstElement *pipeline = gst_parse_launch(pipe_str.c_str(), &gerr);
  if (!pipeline || gerr) {
    log.error("publisher", fmt::format("gst_parse_launch failed: {}",
                                       gerr ? gerr->message : "unknown"));
    if (gerr)
      g_error_free(gerr);
    log.shutdown();
    return 1;
  }

  std::unique_ptr<MjpegServer> server;
  if (cfg.stream)
    server = std::make_unique<MjpegServer>(static_cast<int>(cfg.http_port));

  AppsinkCtx appsinkCtx{server.get(), publisher.get(), cfg.name};

  GstAppSink *appsink =
      GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "mjpeg_sink"));
  GstAppSinkCallbacks cbs{};
  cbs.new_sample = onNewSample;
  gst_app_sink_set_callbacks(appsink, &cbs, &appsinkCtx, nullptr);

  if (server)
    server->start();
  gst_element_set_state(pipeline, GST_STATE_PLAYING);
  log.info(
      "publisher",
      fmt::format("Pipeline PLAYING. Publishing to {}. Press Ctrl+C to stop.",
                  iceoryx_topic));

  GstBus *bus = gst_element_get_bus(pipeline);
  while (gRun.load()) {
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 200 * GST_MSECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (!msg)
      continue;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
      GError *err = nullptr;
      gchar *dbg = nullptr;
      gst_message_parse_error(msg, &err, &dbg);
      log.error("publisher",
                fmt::format("Pipeline error: {}  Debug: {}",
                            err ? err->message : "?", dbg ? dbg : "none"));
      if (err)
        g_error_free(err);
      if (dbg)
        g_free(dbg);
    }
    gst_message_unref(msg);
    break;
  }

  if (server)
    server->stop();
  gst_object_unref(bus);
  gst_object_unref(appsink);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(pipeline);
  gst_deinit();
  log.info("publisher",
           fmt::format("Stopped. topic={}  published={}", iceoryx_topic,
                       appsinkCtx.frames_published));
  log.shutdown();
  return 0;
}
