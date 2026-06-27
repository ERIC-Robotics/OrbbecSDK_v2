#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "iceoryx_posh/popo/publisher.hpp"
#include "iceoryx_posh/popo/subscriber_options.hpp"
#include "iceoryx_posh/popo/untyped_subscriber.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"

#include "lflogger.hpp"
#include "mcap_writer.hpp"
#include "orbbec_data.hpp"
#include "orbbec_saver_stats.hpp"
#include "performance_metrics.hpp"

namespace fs = std::filesystem;

std::atomic<bool> gRun{true};

void onSignal(int) { gRun.store(false); }

struct SaverStats {
  std::atomic<uint64_t> frames_saved{0};
  std::atomic<uint64_t> segment_bytes{0};
  std::atomic<uint32_t> current_segment{1};
  std::atomic<uint8_t> saver_ok{1};

  std::atomic<uint64_t> fps_frame_count{0};
  std::atomic<int64_t> fps_window_start_ns{0};
  std::atomic<float> save_fps{0.0f};

  std::atomic<float> write_latency_ms{0.0f};
  std::atomic<float> e2e_latency_ms{0.0f};
  std::atomic<float> throughput_mbps{0.0f};
  std::atomic<uint64_t> transmission_drops{0};

  std::mutex error_mutex;
  std::string last_error;

  void setError(const std::string &msg) {
    std::lock_guard<std::mutex> lk(error_mutex);
    last_error = msg.substr(0, 255);
    saver_ok.store(0);
  }

  void tickFps() {
    const int64_t nowNs =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const int64_t windowStart = fps_window_start_ns.load();
    const double elapsedSec = static_cast<double>(nowNs - windowStart) / 1e9;
    if (elapsedSec >= 1.0) {
      const uint64_t count = fps_frame_count.exchange(0);
      save_fps.store(
          static_cast<float>(static_cast<double>(count) / elapsedSec));
      fps_window_start_ns.store(nowNs);
    } else {
      fps_frame_count.fetch_add(1);
    }
  }
};

void saverStatsWorker(const std::string &name, SaverStats &stats) {
  iox::popo::Publisher<orbbec::SaverStatsMsg> publisher(
      iox::capro::ServiceDescription{
          iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
          iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
          iox::capro::IdString_t(iox::TruncateToCapacity, "SaverStats")});

  while (gRun.load()) {
    publisher.loan()
        .and_then([&](auto &msg) {
          msg->timestamp_ns = static_cast<uint64_t>(
              std::chrono::system_clock::now().time_since_epoch().count());
          msg->frames_saved = stats.frames_saved.load();
          msg->save_fps = stats.save_fps.load();
          msg->write_latency_ms = stats.write_latency_ms.load();
          msg->e2e_latency_ms = stats.e2e_latency_ms.load();
          msg->throughput_mbps = stats.throughput_mbps.load();
          msg->transmission_drops = stats.transmission_drops.load();
          msg->current_segment = stats.current_segment.load();
          msg->segment_bytes = stats.segment_bytes.load();
          msg->saver_ok = stats.saver_ok.load();
          {
            std::lock_guard<std::mutex> lk(stats.error_mutex);
            std::strncpy(msg->last_error, stats.last_error.c_str(), 255);
            msg->last_error[255] = '\0';
          }
          publisher.publish(std::move(msg));
        })
        .or_else([](auto &) {});

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void saverWorker(const std::string &name, const fs::path &outDir,
                 const std::string &timestamp, uint64_t chunkSize,
                 uint64_t segmentSizeBytes) {
  auto &log = LockFreeLogger::getInstance();
  fs::path deviceDir = outDir / name;
  try {
    fs::create_directories(deviceDir);
  } catch (const std::exception &e) {
    log.error(name, fmt::format("Cannot create output dir: {}", e.what()));
    return;
  }

  SaverStats stats;
  stats.fps_window_start_ns.store(
      std::chrono::steady_clock::now().time_since_epoch().count());

  std::thread statsTh(saverStatsWorker, name, std::ref(stats));
  struct StatsGuard {
    std::thread &t;
    ~StatsGuard() {
      if (t.joinable())
        t.join();
    }
  } statsGuard{statsTh};

  uint32_t segIndex = 1;

  auto makeSegmentPath = [&]() -> fs::path {
    std::ostringstream n;
    n << timestamp << "_seg" << std::setw(3) << std::setfill('0') << segIndex
      << ".mcap";
    return deviceDir / n.str();
  };

  auto openWriter = [&]() -> std::unique_ptr<orbbec::McapWriter> {
    auto writer = std::make_unique<orbbec::McapWriter>(
        makeSegmentPath(), "Orbbec/" + name + "/Frame", chunkSize);
    try {
      writer->open();
      stats.current_segment.store(segIndex);
      stats.segment_bytes.store(0);
      log.info(name, fmt::format("Segment {} opened: {}", segIndex,
                                 makeSegmentPath().string()));
    } catch (const std::exception &e) {
      const std::string msg =
          fmt::format("Failed to open segment: {}", e.what());
      log.error(name, msg);
      stats.setError(msg);
      return nullptr;
    }
    return writer;
  };

  iox::popo::SubscriberOptions options;
  options.queueCapacity = 256U;
  options.historyRequest = 0U;
  options.queueFullPolicy = iox::popo::QueueFullPolicy::DISCARD_OLDEST_DATA;

  iox::popo::UntypedSubscriber subscriber(
      iox::capro::ServiceDescription{
          iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
          iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
          iox::capro::IdString_t(iox::TruncateToCapacity, "Frame")},
      options);

  subscriber.subscribe();
  log.info(name, "Subscribed. Waiting for frames...");

  auto mcapWriter = openWriter();
  if (!mcapWriter)
    return;

  iox::SubscribeState lastState = iox::SubscribeState::NOT_SUBSCRIBED;
  uint64_t frameIndex = 0;

  orbbec::SubscriberMetrics metrics(name);

  while (gRun.load()) {
    auto subState = subscriber.getSubscriptionState();
    if (subState != lastState) {
      if (subState == iox::SubscribeState::SUBSCRIBED)
        log.info(name, "Successfully subscribed.");
      lastState = subState;
    }

    if (segmentSizeBytes > 0 &&
        mcapWriter->bytes_written() >= segmentSizeBytes) {
      log.info(name, fmt::format("Closing segment {} | frames={} | size={} MiB",
                                 segIndex, mcapWriter->frames_written(),
                                 mcapWriter->bytes_written() / 1024 / 1024));
      mcapWriter->close();
      ++segIndex;
      mcapWriter = openWriter();
      if (!mcapWriter)
        return;
    }

    bool gotAny = false;
    int drainLimit = 256;

    while (drainLimit-- > 0) {
      bool tookOne = false;

      subscriber.take()
          .and_then([&](const void *payload) {
            tookOne = true;
            gotAny = true;

            const auto *frame = static_cast<const orbbec::FrameData *>(payload);

            if (!frame || frame->width == 0 || frame->height == 0) {
              subscriber.release(payload);
              return;
            }

            const std::size_t payloadBytes =
                orbbec::kHeaderSize + frame->data_size;

            auto writeStart = std::chrono::steady_clock::now();
            mcapWriter->write_frame(frame, payloadBytes);
            auto writeEnd = std::chrono::steady_clock::now();
            double writeLatencyMs =
                std::chrono::duration<double, std::milli>(writeEnd - writeStart)
                    .count();

            uint64_t nowNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
            double e2eLatencyMs = static_cast<double>(static_cast<int64_t>(
                                      nowNs - frame->timestamp_ns)) /
                                  1e6;

            metrics.update(frame->sequence_number, payloadBytes, writeLatencyMs,
                           e2eLatencyMs);

            stats.frames_saved.fetch_add(1);
            stats.segment_bytes.store(mcapWriter->bytes_written());
            stats.write_latency_ms.store(static_cast<float>(writeLatencyMs));
            stats.e2e_latency_ms.store(static_cast<float>(e2eLatencyMs));
            stats.throughput_mbps.store(static_cast<float>(
                static_cast<double>(payloadBytes) / (1024.0 * 1024.0)));
            stats.tickFps();

            subscriber.release(payload);

            if (frameIndex % 100 == 0) {
              log.info(name, fmt::format("frame={}  seq={}  seg={}  bytes={}",
                                         frameIndex, frame->sequence_number,
                                         segIndex, payloadBytes));
            }
            ++frameIndex;
          })
          .or_else([](auto & /*err*/) {});

      if (!tookOne)
        break;
    }

    if (metrics.shouldReport(2.0))
      log.info(name, fmt::format("[METRICS] {}", metrics.generateReport()));

    if (!gotAny)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  mcapWriter->close();
  log.info(name, fmt::format("Worker done. total_frames={}  segments={}",
                             frameIndex, segIndex));
}

int main(int argc, char **argv) {
  auto &LFL = LockFreeLogger::getInstance();
  LFL.initialize(std::make_unique<ConsoleAndFileLogWriter>(),
                 QueueMode::IMMEDIATE);

  fs::path cfgPath =
      fs::path(argv[0]).parent_path() / "config/orbbec_saver.yaml";
  std::vector<std::string> cliNames;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc)
      cfgPath = argv[++i];
    else if (a == "--name" && i + 1 < argc)
      cliNames.emplace_back(argv[++i]);
  }

  fs::path outDir("data");
  uint64_t chunkSize = 128ULL * 1024 * 1024;
  uint64_t segmentSizeBytes = 10ULL * 1024 * 1024 * 1024;
  std::vector<std::string> configNames;

  try {
    YAML::Node cfg = YAML::LoadFile(cfgPath.string());

    if (cfg["output_dir"])
      outDir = fs::path(cfg["output_dir"].as<std::string>());
    if (cfg["chunk_size_mib"])
      chunkSize = cfg["chunk_size_mib"].as<uint64_t>() * 1024 * 1024;
    if (cfg["segment_size_gib"])
      segmentSizeBytes =
          cfg["segment_size_gib"].as<uint64_t>() * 1024 * 1024 * 1024;
    if (cfg["devices"] && cfg["devices"].IsSequence())
      for (const auto &s : cfg["devices"])
        configNames.emplace_back(s.as<std::string>());

    LFL.info("saver",
             fmt::format("Config | Out: {}  Chunk: {} MiB  Segment: {} GiB",
                         outDir.string(), chunkSize / 1024 / 1024,
                         segmentSizeBytes / 1024 / 1024 / 1024));
  } catch (const std::exception &e) {
    LFL.warn("saver",
             fmt::format("Could not load config ({}): {}. Using defaults.",
                         cfgPath.string(), e.what()));
  }

  const std::vector<std::string> names =
      !cliNames.empty() ? cliNames : configNames;

  if (names.empty()) {
    LFL.error("saver",
              "No device names specified. Add them to orbbec_saver.yaml "
              "under 'devices:' or pass --name <name>");
    LFL.shutdown();
    return 1;
  }

  LFL.info("saver", fmt::format("Recording from {} camera(s):", names.size()));
  for (const auto &n : names)
    LFL.info("saver", fmt::format("  {}", n));

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  iox::runtime::PoshRuntime::initRuntime("orbbec_saver");

  auto now = std::chrono::system_clock::now();
  auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S");
  const std::string launchTimestamp = ss.str();

  std::vector<std::thread> workers;
  workers.reserve(names.size());
  for (const auto &name : names) {
    LFL.info("saver", fmt::format("Spawning saver worker for: {}", name));
    workers.emplace_back(saverWorker, name, outDir, launchTimestamp, chunkSize,
                         segmentSizeBytes);
  }

  for (auto &t : workers)
    if (t.joinable())
      t.join();

  LFL.info("saver", "orbbec_saver shutting down.");
  LFL.shutdown();
  return 0;
}
