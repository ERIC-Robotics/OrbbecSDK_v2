// orbbec_publisher.cpp
//
// V4L2 camera publisher — opens /dev/video* devices and publishes frames
// over iceoryx for downstream consumers (savers, viewers, monitors).
//
// If the camera outputs MJPEG natively the frames are forwarded directly
// (no re-encoding) on the MJPEG iceoryx topic.  Raw formats (YUYV, etc.)
// are published on the Frame topic.
//
// Topics:
//   Raw  : Orbbec / <name> / Frame   (orbbec::FrameData)
//   MJPEG: Orbbec / <name> / MJPEG   (orbbec::JpegFrameData)
//   Health: Orbbec / <name> / Health  (orbbec::HealthMsg)
//
// Usage:
//   orbbec_publisher [--config orbbec_publisher.yaml]
//   orbbec_publisher --device /dev/video0 cam0 [--width 1280 --height 720
//   --format MJPEG]

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "iceoryx_posh/capro/service_description.hpp"
#include "iceoryx_posh/popo/publisher.hpp"
#include "iceoryx_posh/popo/publisher_options.hpp"
#include "iceoryx_posh/popo/untyped_publisher.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"
#include "iox/string.hpp"

#include "lflogger.hpp"
#include "orbbec_data.hpp"
#include "orbbec_health.hpp"
#include "performance_metrics.hpp"
#include "v4l2_probe.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> gRun{true};
void onSignal(int) { gRun.store(false); }

constexpr int kWatchdogIntervalMs = 2000;
constexpr int kWatchdogMissLimit = 10;
constexpr int kResetStormCount = 10;
constexpr double kResetStormWindowSec = 180.0;

struct WorkerHandle {
  std::atomic<int64_t> lastHeartbeatNs{0};
  std::atomic<bool> resetRequested{false};
  std::atomic<bool> shouldStop{false};
  int missCount{0};
  std::deque<double> resetTimestamps;
  bool declaredFatal{false};
};

struct WorkerStats {
  std::atomic<uint64_t> frames_published{0};
  std::atomic<uint64_t> loan_failures{0};
  std::atomic<uint8_t> stream_ok{1};
  std::atomic<uint8_t> watchdog_resets{0};
  std::atomic<float> actual_fps{0.0f};

  std::atomic<uint64_t> fps_frame_count{0};
  std::atomic<int64_t> fps_window_start_ns{0};

  std::mutex error_mutex;
  std::string last_error;
  uint64_t last_error_timestamp_ns{0};

  void setError(const std::string &msg) {
    std::lock_guard<std::mutex> lk(error_mutex);
    last_error = msg.substr(0, 255);
    last_error_timestamp_ns = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
  }

  void tickFps() {
    const int64_t nowNs =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const int64_t windowStart = fps_window_start_ns.load();
    const double elapsedSec = static_cast<double>(nowNs - windowStart) / 1e9;
    if (elapsedSec >= 1.0) {
      const uint64_t count = fps_frame_count.exchange(0);
      actual_fps.store(
          static_cast<float>(static_cast<double>(count) / elapsedSec));
      fps_window_start_ns.store(nowNs);
    } else {
      fps_frame_count.fetch_add(1);
    }
  }
};

struct DeviceConfig {
  std::string path;
  std::string name;
};

struct PublisherConfig {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t fps = 30;
  uint32_t queue_capacity = 500;
  uint32_t image_timeout_ms = 2000;
  std::string pixel_format = "MJPEG";
  std::vector<DeviceConfig> devices;
};

PublisherConfig loadConfig(const fs::path &cfgPath) {
  PublisherConfig cfg;
  try {
    YAML::Node node = YAML::LoadFile(cfgPath.string());
    if (node["width"])
      cfg.width = node["width"].as<uint32_t>();
    if (node["height"])
      cfg.height = node["height"].as<uint32_t>();
    if (node["fps"])
      cfg.fps = node["fps"].as<uint32_t>();
    if (node["queue_capacity"])
      cfg.queue_capacity = node["queue_capacity"].as<uint32_t>();
    if (node["image_timeout_ms"])
      cfg.image_timeout_ms = node["image_timeout_ms"].as<uint32_t>();
    if (node["pixel_format"])
      cfg.pixel_format = node["pixel_format"].as<std::string>();
    if (node["devices"] && node["devices"].IsSequence()) {
      for (const auto &d : node["devices"]) {
        DeviceConfig dev;
        dev.path = d["path"].as<std::string>();
        dev.name = d["name"] ? d["name"].as<std::string>() : dev.path;
        cfg.devices.emplace_back(dev);
      }
    }
  } catch (const std::exception &e) {
    LockFreeLogger::getInstance().warn(
        "publisher",
        fmt::format("Could not load publisher config ({}): {}. Using defaults.",
                    cfgPath.string(), e.what()));
  }
  return cfg;
}

struct V4l2Buffer {
  void *start = nullptr;
  size_t length = 0;
};

static int xioctl(int fd, unsigned long req, void *arg) {
  int r;
  do {
    r = ioctl(fd, req, arg);
  } while (r == -1 && errno == EINTR);
  return r;
}

bool interactiveSetup(PublisherConfig &cfg,
                      std::vector<DeviceConfig> &devices) {
  auto sel = v4l2probe::interactiveSelect(fmt::format("cam{}", devices.size()));
  if (!sel.valid)
    return false;

  cfg.pixel_format = v4l2probe::fourccToString(sel.format.fourcc);
  cfg.width = sel.size.width > 0 ? sel.size.width : cfg.width;
  cfg.height = sel.size.height > 0 ? sel.size.height : cfg.height;
  cfg.fps = sel.fps;
  devices.push_back({sel.device.path, sel.name});

  LockFreeLogger::getInstance().info(
      "publisher", fmt::format("Launching: {} ({})  format={}  {}x{}  @{}fps",
                               sel.name, sel.device.path, cfg.pixel_format,
                               cfg.width, cfg.height, cfg.fps));
  return true;
}

// ── Query and print device capability info ────────────────────────────────
void printDeviceCaps(int fd, const std::string &label) {
  auto &log = LockFreeLogger::getInstance();
  v4l2_capability cap{};
  if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
    log.warn(label, fmt::format("VIDIOC_QUERYCAP: {}", strerror(errno)));
    return;
  }
  log.info(label, fmt::format("driver={} card={} bus={} version={}.{}.{}",
                              reinterpret_cast<const char *>(cap.driver),
                              reinterpret_cast<const char *>(cap.card),
                              reinterpret_cast<const char *>(cap.bus_info),
                              (cap.version >> 16) & 0xFF,
                              (cap.version >> 8) & 0xFF, (cap.version) & 0xFF));
  if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
    log.warn(label, "device does not support VIDEO_CAPTURE");
}

// ── List all supported formats/resolutions for a device ───────────────────
void listDeviceFormats(int fd, const std::string &name) {
  auto &log = LockFreeLogger::getInstance();
  log.info(name, "Supported formats:");

  v4l2_fmtdesc fmtdesc{};
  fmtdesc.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

  while (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
    log.info(
        name,
        fmt::format(
            "  [{}] {} {}", v4l2probe::fourccToString(fmtdesc.pixelformat),
            reinterpret_cast<const char *>(fmtdesc.description),
            (fmtdesc.flags & V4L2_FMT_FLAG_COMPRESSED) ? "(compressed)" : ""));

    // Enumerate frame sizes for this format
    v4l2_frmsizeenum frmsize{};
    frmsize.pixel_format = fmtdesc.pixelformat;
    frmsize.index = 0;
    while (xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &frmsize) == 0) {
      if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        // Enumerate frame intervals (frame rates) for this size
        v4l2_frmivalenum frmival{};
        frmival.pixel_format = fmtdesc.pixelformat;
        frmival.width = frmsize.discrete.width;
        frmival.height = frmsize.discrete.height;
        frmival.index = 0;
        std::string fps_list;
        while (xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &frmival) == 0) {
          if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
            if (!fps_list.empty())
              fps_list += ",";
            fps_list += fmt::format("{}", frmival.discrete.denominator /
                                              frmival.discrete.numerator);
          }
          ++frmival.index;
        }
        log.info(name,
                 fmt::format("       {}x{}  fps=[{}]", frmsize.discrete.width,
                             frmsize.discrete.height,
                             fps_list.empty() ? "?" : fps_list));
      } else if (frmsize.type == V4L2_FRMSIZE_TYPE_STEPWISE) {
        log.info(name, fmt::format("       {}x{} .. {}x{} (step {}x{})",
                                   frmsize.stepwise.min_width,
                                   frmsize.stepwise.min_height,
                                   frmsize.stepwise.max_width,
                                   frmsize.stepwise.max_height,
                                   frmsize.stepwise.step_width,
                                   frmsize.stepwise.step_height));
        break;
      }
      ++frmsize.index;
    }
    ++fmtdesc.index;
  }
}

// ── Health worker ─────────────────────────────────────────────────────────
void healthWorker(const std::string &name, WorkerStats &stats,
                  const PublisherConfig &cfg) {
  iox::popo::Publisher<orbbec::HealthMsg> publisher(
      iox::capro::ServiceDescription{
          iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
          iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
          iox::capro::IdString_t(iox::TruncateToCapacity, "Health")});

  while (gRun.load()) {
    publisher.loan()
        .and_then([&](auto &msg) {
          msg->timestamp_ns = static_cast<uint64_t>(
              std::chrono::system_clock::now().time_since_epoch().count());
          msg->frames_published = stats.frames_published.load();
          msg->loan_failures = stats.loan_failures.load();
          msg->actual_fps = stats.actual_fps.load();
          msg->exposure_time_us = 0.0f;
          msg->gain_db = 0.0f;
          msg->width = cfg.width;
          msg->height = cfg.height;
          msg->stream_ok = stats.stream_ok.load();
          msg->watchdog_resets = stats.watchdog_resets.load();
          {
            std::lock_guard<std::mutex> lk(stats.error_mutex);
            msg->last_error_timestamp_ns = stats.last_error_timestamp_ns;
            std::strncpy(msg->last_error, stats.last_error.c_str(), 255);
            msg->last_error[255] = '\0';
          }
          publisher.publish(std::move(msg));
        })
        .or_else([](auto &) {});

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

// ── Capture worker ────────────────────────────────────────────────────────
void captureWorker(const DeviceConfig &dev, const PublisherConfig &cfg,
                   WorkerHandle *handle, WorkerStats &stats) {
  const std::string &name = dev.name;
  const uint32_t pixFmt = v4l2probe::fourccFromString(cfg.pixel_format);
  const bool isMjpeg = (pixFmt == V4L2_PIX_FMT_MJPEG);

  auto &LFL = LockFreeLogger::getInstance();
  LFL.info(name,
           fmt::format("Opening {} (format={})", dev.path, cfg.pixel_format));

  // ── Open device
  int fd = open(dev.path.c_str(), O_RDWR | O_NONBLOCK);
  if (fd < 0) {
    const std::string msg =
        fmt::format("Cannot open {}: {}", dev.path, strerror(errno));
    LFL.error(name, msg);
    stats.setError(msg);
    stats.stream_ok.store(0);
    return;
  }

  // ── Print device info and supported formats
  printDeviceCaps(fd, name);
  listDeviceFormats(fd, name);

  // ── Set format
  {
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cfg.width;
    fmt.fmt.pix.height = cfg.height;
    fmt.fmt.pix.pixelformat = pixFmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
      LFL.warn(name, fmt::format("VIDIOC_S_FMT: {}", strerror(errno)));
  }

  // ── Set frame rate
  {
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = cfg.fps;
    if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0)
      LFL.warn(name, fmt::format("VIDIOC_S_PARM: {}", strerror(errno)));
  }

  // ── Log actual negotiated format
  uint32_t actualPixFmt = pixFmt;
  uint32_t actualW = cfg.width, actualH = cfg.height;
  {
    v4l2_format actual{};
    actual.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_G_FMT, &actual) == 0) {
      actualPixFmt = actual.fmt.pix.pixelformat;
      actualW = actual.fmt.pix.width;
      actualH = actual.fmt.pix.height;
      LFL.info(name,
               fmt::format("Negotiated: {}x{} format={}", actualW, actualH,
                           v4l2probe::fourccToString(actualPixFmt)));
    }
  }

  // ── Request MMAP buffers
  std::vector<V4l2Buffer> buffers;
  {
    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
      const std::string msg =
          fmt::format("VIDIOC_REQBUFS: {}", strerror(errno));
      LFL.error(name, msg);
      stats.setError(msg);
      close(fd);
      return;
    }
    buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        LFL.error(name, fmt::format("VIDIOC_QUERYBUF: {}", strerror(errno)));
        close(fd);
        return;
      }
      buffers[i].length = buf.length;
      buffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, buf.m.offset);
      if (buffers[i].start == MAP_FAILED) {
        LFL.error(name, fmt::format("mmap: {}", strerror(errno)));
        close(fd);
        return;
      }
    }
  }

  auto freeMmap = [&]() {
    for (auto &b : buffers)
      if (b.start && b.start != MAP_FAILED)
        munmap(b.start, b.length);
    buffers.clear();
  };

  auto queueAll = [&]() -> bool {
    for (size_t i = 0; i < buffers.size(); ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = static_cast<uint32_t>(i);
      if (xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
        stats.setError(fmt::format("VIDIOC_QBUF: {}", strerror(errno)));
        return false;
      }
    }
    return true;
  };

  auto startStream = [&]() -> bool {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
      const std::string msg =
          fmt::format("VIDIOC_STREAMON: {}", strerror(errno));
      LFL.error(name, msg);
      stats.setError(msg);
      stats.stream_ok.store(0);
      return false;
    }
    stats.stream_ok.store(1);
    LFL.info(name, fmt::format("Stream started ({})",
                               isMjpeg ? "MJPEG passthrough" : "raw"));
    return true;
  };

  auto stopStream = [&]() {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    stats.stream_ok.store(0);
    LFL.info(name, "Stream stopped.");
  };

  if (!queueAll() || !startStream()) {
    freeMmap();
    close(fd);
    return;
  }

  // ── iceoryx publishers
  iox::popo::PublisherOptions pubOpts;
  pubOpts.historyCapacity = 1U;
  pubOpts.nodeName = iox::NodeName_t(iox::TruncateToCapacity, name.c_str());

  // Raw frame publisher (used when format != MJPEG)
  std::unique_ptr<iox::popo::UntypedPublisher> rawPublisher;
  // MJPEG publisher (used when format == MJPEG)
  std::unique_ptr<iox::popo::UntypedPublisher> mjpegPublisher;

  if (isMjpeg) {
    mjpegPublisher = std::make_unique<iox::popo::UntypedPublisher>(
        iox::capro::ServiceDescription{
            iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
            iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
            iox::capro::IdString_t(iox::TruncateToCapacity, "MJPEG")},
        pubOpts);
    LFL.info(name, fmt::format("Publishing on Orbbec/{}/MJPEG", name));
  } else {
    rawPublisher = std::make_unique<iox::popo::UntypedPublisher>(
        iox::capro::ServiceDescription{
            iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
            iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
            iox::capro::IdString_t(iox::TruncateToCapacity, "Frame")},
        pubOpts);
    LFL.info(name, fmt::format("Publishing on Orbbec/{}/Frame", name));
  }

  // ── Health worker thread
  stats.fps_window_start_ns.store(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::thread health(healthWorker, name, std::ref(stats), std::cref(cfg));
  struct HealthGuard {
    std::thread &t;
    ~HealthGuard() {
      if (t.joinable())
        t.join();
    }
  } healthGuard{health};

  uint64_t idx = 0;
  orbbec::PublisherMetrics metrics(name, static_cast<double>(cfg.fps));

  handle->lastHeartbeatNs.store(
      std::chrono::steady_clock::now().time_since_epoch().count());

  auto doReset = [&]() -> bool {
    LFL.info(name, "Executing stream reset.");
    stopStream();
    freeMmap();

    v4l2_requestbuffers req{};
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0)
      return false;
    buffers.resize(req.count);
    for (uint32_t i = 0; i < req.count; ++i) {
      v4l2_buffer buf{};
      buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index = i;
      if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0) {
        freeMmap();
        return false;
      }
      buffers[i].length = buf.length;
      buffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, buf.m.offset);
      if (buffers[i].start == MAP_FAILED) {
        freeMmap();
        return false;
      }
    }

    if (!queueAll() || !startStream()) {
      freeMmap();
      return false;
    }

    stats.watchdog_resets.fetch_add(1);
    handle->lastHeartbeatNs.store(
        std::chrono::steady_clock::now().time_since_epoch().count());
    handle->resetRequested.store(false);
    return true;
  };

  while (gRun.load() && !handle->shouldStop.load()) {
    if (handle->resetRequested.load()) {
      if (!doReset())
        break;
    }

    pollfd pfd{fd, POLLIN, 0};
    int ret = poll(&pfd, 1, static_cast<int>(cfg.image_timeout_ms));
    if (ret < 0) {
      if (errno == EINTR)
        continue;
      const std::string msg = fmt::format("poll: {}", strerror(errno));
      LFL.error(name, msg);
      stats.setError(msg);
      stats.stream_ok.store(0);
      handle->lastHeartbeatNs.store(0);
      continue;
    }
    if (ret == 0) {
      stats.stream_ok.store(0);
      handle->lastHeartbeatNs.store(0);
      continue;
    }

    v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
      if (errno == EAGAIN)
        continue;
      const std::string msg = fmt::format("VIDIOC_DQBUF: {}", strerror(errno));
      LFL.error(name, msg);
      stats.setError(msg);
      stats.stream_ok.store(0);
      handle->lastHeartbeatNs.store(0);
      continue;
    }

    auto acquireTime = std::chrono::steady_clock::now();
    handle->lastHeartbeatNs.store(acquireTime.time_since_epoch().count());
    stats.stream_ok.store(1);

    const uint64_t dataSize = buf.bytesused;
    const uint8_t *srcData =
        static_cast<const uint8_t *>(buffers[buf.index].start);
    const uint64_t tsNs = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());

    bool loanOk = false;

    if (isMjpeg) {
      // ── MJPEG passthrough: publish directly as JpegFrameData
      const uint64_t payloadSize = orbbec::kJpegHeaderSize + dataSize;
      mjpegPublisher->loan(static_cast<uint32_t>(payloadSize))
          .and_then([&](auto &userPayload) {
            auto *msg = static_cast<orbbec::JpegFrameData *>(userPayload);
            msg->timestamp_ns = tsNs;
            msg->sequence_number = idx;
            msg->is_keyframe = 1;
            msg->data_size = static_cast<uint32_t>(dataSize);
            std::memcpy(msg->data, srcData, dataSize);
            mjpegPublisher->publish(userPayload);
            loanOk = true;
            stats.frames_published.fetch_add(1);
            stats.tickFps();
            metrics.update(idx, payloadSize, 0.0, loanOk);
          })
          .or_else([&](auto &error) {
            const std::string msg = fmt::format(
                "iceoryx MJPEG loan failed (err={}). Pool may be undersized.",
                static_cast<int>(error));
            LFL.error(name, msg);
            stats.setError(msg);
            stats.loan_failures.fetch_add(1);
          });
    } else {
      // ── Raw frame: publish as FrameData
      const uint64_t payloadSize = orbbec::kHeaderSize + dataSize;
      rawPublisher->loan(static_cast<uint32_t>(payloadSize))
          .and_then([&](auto &userPayload) {
            auto *msg = static_cast<orbbec::FrameData *>(userPayload);
            msg->timestamp_ns = tsNs;
            msg->sequence_number = idx;
            msg->width = actualW;
            msg->height = actualH;
            msg->pixel_format = actualPixFmt;
            msg->_pad = 0U;
            msg->data_size = dataSize;
            std::memcpy(msg->data, srcData, dataSize);
            rawPublisher->publish(userPayload);
            loanOk = true;
            stats.frames_published.fetch_add(1);
            stats.tickFps();
          })
          .or_else([&](auto &error) {
            const std::string msg = fmt::format(
                "iceoryx Frame loan failed (err={}). Pool may be undersized.",
                static_cast<int>(error));
            LFL.error(name, msg);
            stats.setError(msg);
            stats.loan_failures.fetch_add(1);
          });

      auto publishTime = std::chrono::steady_clock::now();
      double latencyMs =
          std::chrono::duration<double, std::milli>(publishTime - acquireTime)
              .count();
      metrics.update(idx, loanOk ? (orbbec::kHeaderSize + dataSize) : 0,
                     latencyMs, loanOk);
    }

    if (xioctl(fd, VIDIOC_QBUF, &buf) < 0)
      LFL.warn(name, fmt::format("VIDIOC_QBUF: {}", strerror(errno)));

    if (metrics.shouldReport(2.0))
      LFL.info(name, fmt::format("[METRICS] {}", metrics.generateReport()));

    ++idx;
    handle->lastHeartbeatNs.store(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }

  stopStream();
  freeMmap();
  close(fd);
  LFL.info(
      name,
      fmt::format("Capture worker stopped. Total frames published: {}", idx));
}

// ── Watchdog ─────────────────────────────────────────────────────────────
void watchdogTick(std::vector<WorkerHandle> &handles,
                  std::vector<std::future<void>> &futures,
                  const std::vector<std::string> &names,
                  std::vector<WorkerStats> & /*stats*/) {
  const double nowSec = std::chrono::duration<double>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
  const int64_t nowNs = static_cast<int64_t>(nowSec * 1e9);
  const int64_t staleThresholdNs =
      static_cast<int64_t>(kWatchdogIntervalMs) * 1'000'000LL;

  for (size_t i = 0; i < handles.size(); ++i) {
    if (!futures[i].valid() || handles[i].declaredFatal)
      continue;
    if (handles[i].resetRequested.load())
      continue;

    const int64_t hb = handles[i].lastHeartbeatNs.load();
    const bool stale = (nowNs - hb) > staleThresholdNs;

    if (stale)
      handles[i].missCount++;
    else
      handles[i].missCount = 0;

    if (handles[i].missCount < kWatchdogMissLimit)
      continue;

    handles[i].resetTimestamps.push_back(nowSec);
    while (!handles[i].resetTimestamps.empty() &&
           (nowSec - handles[i].resetTimestamps.front()) > kResetStormWindowSec)
      handles[i].resetTimestamps.pop_front();

    const int recentResets =
        static_cast<int>(handles[i].resetTimestamps.size());

    if (recentResets >= kResetStormCount) {
      LockFreeLogger::getInstance().error(
          "WATCHDOG",
          fmt::format(
              "Device [{}] {} hit reset storm limit ({} resets in {}s). "
              "Disabling auto-recovery.",
              i, names[i], recentResets, kResetStormWindowSec));
      handles[i].declaredFatal = true;
      handles[i].shouldStop.store(true);
      continue;
    }

    LockFreeLogger::getInstance().warn(
        "WATCHDOG",
        fmt::format("Device [{}] {} missed {} consecutive heartbeats. "
                    "Requesting stream reset (storm count: {}).",
                    i, names[i], kWatchdogMissLimit, recentResets));
    handles[i].missCount = 0;
    handles[i].resetRequested.store(true);
  }
}

} // namespace

int main(int argc, char **argv) {
  auto &LFL = LockFreeLogger::getInstance();
  LFL.initialize(std::make_unique<ConsoleAndFileLogWriter>(),
                 QueueMode::IMMEDIATE);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  fs::path cfgPath =
      fs::path(argv[0]).parent_path() / "config/orbbec_publisher.yaml";
  std::vector<DeviceConfig> cliDevices;
  std::string cliFormat;
  uint32_t cliWidth = 0;
  uint32_t cliHeight = 0;
  bool listFormats = false;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc) {
      cfgPath = argv[++i];
    } else if (a == "--device" && i + 2 < argc) {
      DeviceConfig d;
      d.path = argv[++i];
      d.name = argv[++i];
      cliDevices.emplace_back(d);
    } else if (a == "--format" && i + 1 < argc) {
      cliFormat = argv[++i];
    } else if (a == "--width" && i + 1 < argc) {
      cliWidth = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--height" && i + 1 < argc) {
      cliHeight = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (a == "--list-formats") {
      listFormats = true;
    } else if (a == "--help" || a == "-h") {
      std::cout
          << "Usage: " << argv[0] << " [OPTIONS]\n\n"
          << "Options:\n"
          << "  --device <path> <name>  Capture from this V4L2 device "
             "(repeatable)\n"
          << "  --format <fmt>           Pixel format: MJPEG, YUYV, NV12 "
             "(default: MJPEG)\n"
          << "  --width  <px>            Frame width  (default: from config)\n"
          << "  --height <px>            Frame height (default: from config)\n"
          << "  --list-formats           Print supported formats for each "
             "device and exit\n"
          << "  --config <path>          YAML config (default: "
             "orbbec_publisher.yaml)\n"
          << "\nWith MJPEG format the camera's compressed frames are forwarded "
             "directly\n"
          << "(no re-encoding) on the Orbbec/<name>/MJPEG iceoryx topic.\n"
          << "Raw formats are published on Orbbec/<name>/Frame.\n"
          << "\nExamples:\n"
          << "  " << argv[0] << " --list-formats\n"
          << "  " << argv[0] << " --device /dev/video0 cam0 --list-formats\n"
          << "  " << argv[0] << " --device /dev/video0 cam0 --format MJPEG\n"
          << "  " << argv[0]
          << " --device /dev/video0 cam0 --format YUYV --width 1920 --height "
             "1080\n";
      LFL.shutdown();
      return 0;
    }
  }

  PublisherConfig cfg = loadConfig(cfgPath);
  if (!cliFormat.empty())
    cfg.pixel_format = cliFormat;
  if (cliWidth > 0)
    cfg.width = cliWidth;
  if (cliHeight > 0)
    cfg.height = cliHeight;

  const std::vector<DeviceConfig> &devices =
      !cliDevices.empty() ? cliDevices : cfg.devices;

  // ── --list-formats: probe each device and exit (no RouDi needed) ─────
  if (listFormats) {
    // If no devices specified on CLI or config, scan /dev/video*
    std::vector<DeviceConfig> probeList = devices;
    if (probeList.empty()) {
      for (int n = 0; n < 16; ++n) {
        std::string path = fmt::format("/dev/video{}", n);
        if (fs::exists(path))
          probeList.push_back({path, fmt::format("video{}", n)});
      }
    }
    if (probeList.empty()) {
      LFL.error("publisher", "No V4L2 devices found under /dev/video*");
      LFL.shutdown();
      return 1;
    }
    for (const auto &d : probeList) {
      LFL.info("publisher", fmt::format("=== {} ({}) ===", d.name, d.path));
      int fd = open(d.path.c_str(), O_RDWR | O_NONBLOCK);
      if (fd < 0) {
        LFL.error("publisher",
                  fmt::format("Cannot open {}: {}", d.path, strerror(errno)));
        continue;
      }
      printDeviceCaps(fd, d.name);
      listDeviceFormats(fd, d.name);
      close(fd);
    }
    LFL.shutdown();
    return 0;
  }

  // ── Device resolution: no devices on CLI or in config ────────────────
  std::vector<DeviceConfig> mutableDevices(devices.begin(), devices.end());
  if (mutableDevices.empty()) {
    const bool hasCliOverrides = !cliFormat.empty() || cliWidth > 0 || cliHeight > 0;
    if (hasCliOverrides) {
      // Non-interactive: CLI overrides present, auto-detect first available camera
      auto found = v4l2probe::scanDevices();
      if (found.empty()) {
        LFL.error("publisher",
                  "No --device specified and no V4L2 devices found. "
                  "Use --device <path> <name> or connect a camera.");
        LFL.shutdown();
        return 1;
      }
      mutableDevices.push_back({found[0].path, "cam0"});
      LFL.info("publisher",
               fmt::format("Auto-detected: {} ({})", found[0].path, found[0].card));
    } else {
      // Fully interactive: no args at all, walk the wizard
      if (!interactiveSetup(cfg, mutableDevices)) {
        LFL.shutdown();
        return 1;
      }
    }
  }

  constexpr char APP_NAME[] = "orbbec_publisher";
  iox::runtime::PoshRuntime::initRuntime(APP_NAME);

  const bool isMjpeg =
      (v4l2probe::fourccFromString(cfg.pixel_format) == V4L2_PIX_FMT_MJPEG);
  LFL.info(
      "publisher",
      fmt::format("{}x{} @ {} fps | format={} | topic={}", cfg.width,
                  cfg.height, cfg.fps, cfg.pixel_format,
                  isMjpeg ? "Orbbec/<name>/MJPEG" : "Orbbec/<name>/Frame"));

  for (size_t i = 0; i < mutableDevices.size(); ++i)
    LFL.info("publisher",
             fmt::format("  [{}] {} ({})", i, mutableDevices[i].name,
                         mutableDevices[i].path));

  const size_t N = mutableDevices.size();
  std::vector<std::string> names(N);
  for (size_t i = 0; i < N; ++i)
    names[i] = mutableDevices[i].name;

  std::vector<WorkerHandle> handles(N);
  std::vector<WorkerStats> stats(N);
  std::vector<std::future<void>> futures(N);

  for (size_t i = 0; i < N; ++i) {
    futures[i] = std::async(
        std::launch::async, [&mutableDevices, i, &cfg, &handles, &stats]() {
          captureWorker(mutableDevices[i], cfg, &handles[i], stats[i]);
        });
  }

  while (gRun.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kWatchdogIntervalMs));
    if (!gRun.load())
      break;
    watchdogTick(handles, futures, names, stats);
  }

  LFL.info("publisher", "Shutdown signal received. Stopping workers.");
  for (auto &h : handles)
    h.shouldStop.store(true);
  for (auto &f : futures)
    if (f.valid())
      f.get();

  LFL.info("publisher", "All devices released. Exiting.");
  LFL.shutdown();
  return 0;
}
