// orbbec_saver_gst.cpp
//
// Subscribes to iceoryx MJPEG frames (published by orbbec_gst_publisher) and
// saves them to MCAP as foxglove.CompressedImage messages (format="jpeg").
//
// Service: Orbbec / <name> / MJPEG
//
// Usage:
//   orbbec_saver_gst --name cam0 [--name cam1 ...]

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

#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "iceoryx_posh/popo/subscriber_options.hpp"
#include "iceoryx_posh/popo/untyped_subscriber.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"

#include "lflogger.hpp"
#include "orbbec_data.hpp"
#include "performance_metrics.hpp"
#include "video_mcap_writer.hpp"

namespace fs = std::filesystem;

std::atomic<bool> gRun{true};

void onSignal(int) { gRun.store(false); }

void saverWorker(const std::string &name, const fs::path &outDir,
                 const std::string &timestamp, uint64_t chunkSize,
                 uint64_t segmentSizeBytes) {
  auto &LFL = LockFreeLogger::getInstance();
  fs::path deviceDir = outDir / name;
  try {
    fs::create_directories(deviceDir);
  } catch (const std::exception &e) {
    LFL.error(name, fmt::format("Cannot create output dir: {}", e.what()));
    return;
  }

  uint32_t segIndex = 1;

  auto makeSegmentPath = [&]() -> fs::path {
    std::ostringstream n;
    n << timestamp << "_seg" << std::setw(3) << std::setfill('0') << segIndex
      << ".mcap";
    return deviceDir / n.str();
  };

  auto openWriter = [&]() -> std::unique_ptr<orbbec::VideoMcapWriter> {
    auto writer = std::make_unique<orbbec::VideoMcapWriter>(
        makeSegmentPath(), "OrbbecVideo/" + name + "/MJPEG", chunkSize);
    try {
      writer->open();
      LFL.info(name, fmt::format("Segment {} opened: {}", segIndex,
                                 makeSegmentPath().string()));
    } catch (const std::exception &e) {
      LFL.error(name, fmt::format("Failed to open segment: {}", e.what()));
      return nullptr;
    }
    return writer;
  };

  // iceoryx subscriber — service: "Orbbec" / "<name>" / "MJPEG"
  iox::popo::SubscriberOptions options;
  options.queueCapacity = 256U;
  options.historyRequest = 0U;
  options.queueFullPolicy = iox::popo::QueueFullPolicy::DISCARD_OLDEST_DATA;

  iox::popo::UntypedSubscriber subscriber(
      iox::capro::ServiceDescription{
          iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
          iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
          iox::capro::IdString_t(iox::TruncateToCapacity, "MJPEG")},
      options);

  subscriber.subscribe();
  LFL.info(name,
           fmt::format("Subscribed to Orbbec/{}/MJPEG. Waiting for frames...",
                       name));

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
        LFL.info(name, "Successfully subscribed.");
      lastState = subState;
    }

    if (segmentSizeBytes > 0 &&
        mcapWriter->bytes_written() >= segmentSizeBytes) {
      LFL.info(name, fmt::format("Closing segment {} | frames={} | size={} MiB",
                                 segIndex, mcapWriter->frames_written(),
                                 mcapWriter->bytes_written() / 1024 / 1024));
      mcapWriter->close();
      ++segIndex;
      mcapWriter = openWriter();
      if (!mcapWriter)
        return;
    }

    bool gotAny = false;
    while (true) {
      bool tookOne = false;

      subscriber.take()
          .and_then([&](const void *payload) {
            tookOne = true;
            gotAny = true;

            const auto *frame =
                static_cast<const orbbec::JpegFrameData *>(payload);

            if (!frame || frame->data_size == 0) {
              subscriber.release(payload);
              return;
            }

            orbbec::VideoFrameData hdr{};
            hdr.timestamp_ns = frame->timestamp_ns;
            hdr.sequence_number = frame->sequence_number;
            hdr.is_keyframe = frame->is_keyframe;
            hdr.data_size = frame->data_size;

            auto writeStart = std::chrono::steady_clock::now();
            mcapWriter->write_frame_raw(&hdr, frame->data, frame->data_size);
            double writeLatencyMs =
                std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - writeStart)
                    .count();

            uint64_t nowNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count());
            double e2eLatencyMs = static_cast<double>(static_cast<int64_t>(
                                      nowNs - frame->timestamp_ns)) /
                                  1e6;

            metrics.update(static_cast<int64_t>(frame->sequence_number),
                           frame->data_size, writeLatencyMs, e2eLatencyMs);

            subscriber.release(payload);
            ++frameIndex;
          })
          .or_else([](auto & /*err*/) {});

      if (!tookOne)
        break;
    }

    if (metrics.shouldReport(2.0))
      LFL.info(name, fmt::format("[METRICS] {}", metrics.generateReport()));

    if (!gotAny)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  mcapWriter->close();
  LFL.info(name, fmt::format("Worker done. total_frames={}  segments={}",
                             frameIndex, segIndex));
}

int main(int argc, char **argv) {
  auto &LFL = LockFreeLogger::getInstance();
  LFL.initialize(std::make_unique<ConsoleAndFileLogWriter>(),
                 QueueMode::IMMEDIATE);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  fs::path cfgPath = "config/orbbec_saver.yaml";
  std::string outDirArg;
  std::vector<std::string> names;
  uint64_t chunkSize = 64ULL * 1024 * 1024;
  uint64_t segmentSizeBytes = 10ULL * 1024 * 1024 * 1024;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config" && i + 1 < argc)
      cfgPath = argv[++i];
    else if (a == "--name" && i + 1 < argc)
      names.emplace_back(argv[++i]);
    else if (a == "--out" && i + 1 < argc)
      outDirArg = argv[++i];
    else if (a == "--help" || a == "-h") {
      std::cout << "Usage: orbbec_saver_gst --name <name> [--name <name> ...]\n"
                << "       [--config <yaml>] [--out <dir>]\n"
                << "Subscribes to iceoryx Orbbec/<name>/MJPEG and writes MCAP "
                   "(foxglove.CompressedImage, format=jpeg).\n";
      LFL.shutdown();
      return 0;
    }
  }

  std::string outDir = outDirArg.empty() ? "../data/video" : outDirArg;

  if (names.empty()) {
    try {
      YAML::Node cfg = YAML::LoadFile(cfgPath.string());
      if (cfg["devices"] && cfg["devices"].IsSequence())
        for (const auto &s : cfg["devices"])
          names.emplace_back(s.as<std::string>());
      if (cfg["output_dir"] && outDirArg.empty())
        outDir =
            (fs::path(cfg["output_dir"].as<std::string>()) / "video").string();
      if (cfg["chunk_size_mib"])
        chunkSize = cfg["chunk_size_mib"].as<uint64_t>() * 1024 * 1024;
      if (cfg["segment_size_gib"])
        segmentSizeBytes =
            cfg["segment_size_gib"].as<uint64_t>() * 1024ULL * 1024 * 1024;
    } catch (const std::exception &e) {
      LFL.warn("saver_gst",
               fmt::format("Could not load config ({}): {}. Using defaults.",
                           cfgPath.string(), e.what()));
    }
  }

  if (names.empty()) {
    LFL.error("saver_gst", "No names specified. Use --name <name> or config.");
    LFL.shutdown();
    return 1;
  }

  iox::runtime::PoshRuntime::initRuntime("orbbec_saver_gst");

  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ts;
  ts << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
  const std::string timestamp = ts.str();

  LFL.info("saver_gst",
           fmt::format("Recording {} camera(s) -> {}", names.size(), outDir));

  std::vector<std::thread> workers;
  workers.reserve(names.size());
  for (const auto &name : names)
    workers.emplace_back(saverWorker, name, outDir, timestamp, chunkSize,
                         segmentSizeBytes);

  for (auto &t : workers)
    if (t.joinable())
      t.join();

  LFL.info("saver_gst", "Shutdown complete.");
  LFL.shutdown();
  return 0;
}
