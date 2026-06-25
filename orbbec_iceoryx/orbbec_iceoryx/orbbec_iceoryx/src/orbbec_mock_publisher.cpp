#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <linux/videodev2.h>

#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "iceoryx_posh/capro/service_description.hpp"
#include "iceoryx_posh/popo/publisher_options.hpp"
#include "iceoryx_posh/popo/untyped_publisher.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"
#include "iox/string.hpp"

#include "lflogger.hpp"
#include "orbbec_data.hpp"
#include "performance_metrics.hpp"

namespace fs = std::filesystem;

namespace {

std::atomic<bool> gRun{true};
void onSignal(int) { gRun.store(false); }

struct MockConfig {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t fps = 10;
};

MockConfig loadConfig(const fs::path &cfgPath) {
  MockConfig cfg;
  try {
    YAML::Node node = YAML::LoadFile(cfgPath.string());
    if (node["width"])
      cfg.width = node["width"].as<uint32_t>();
    if (node["height"])
      cfg.height = node["height"].as<uint32_t>();
    if (node["fps"])
      cfg.fps = node["fps"].as<uint32_t>();
  } catch (const std::exception &e) {
    LockFreeLogger::getInstance().warn(
        "mock_publisher",
        fmt::format("Could not load config: {}. Using defaults.", e.what()));
  }
  return cfg;
}

void mockWorker(const std::string &name, const MockConfig &cfg) {
  iox::popo::PublisherOptions pubOpts;
  pubOpts.historyCapacity = 1U;
  pubOpts.nodeName = iox::NodeName_t(iox::TruncateToCapacity, name.c_str());

  iox::popo::UntypedPublisher publisher(
      iox::capro::ServiceDescription{
          iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
          iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
          iox::capro::IdString_t(iox::TruncateToCapacity, "Frame")},
      pubOpts);

  // YUYV: 2 bytes per pixel
  const uint64_t dataSize = static_cast<uint64_t>(cfg.width) * cfg.height * 2;
  const uint64_t payloadSize = orbbec::kHeaderSize + dataSize;
  const auto framePeriod = std::chrono::microseconds(1'000'000 / cfg.fps);

  uint64_t idx = 0;

  auto &LFL = LockFreeLogger::getInstance();
  LFL.info(name, fmt::format("Starting | {}x{} YUYV | {} fps | {} bytes/frame",
                             cfg.width, cfg.height, cfg.fps, payloadSize));

  orbbec::PublisherMetrics metrics(name, static_cast<double>(cfg.fps));

  while (gRun.load()) {
    auto frameStart = std::chrono::steady_clock::now();
    bool loanOk = false;

    publisher.loan(static_cast<uint32_t>(payloadSize))
        .and_then([&](auto &userPayload) {
          auto *msg = static_cast<orbbec::FrameData *>(userPayload);

          msg->timestamp_ns = static_cast<uint64_t>(
              std::chrono::system_clock::now().time_since_epoch().count());
          msg->sequence_number = idx;
          msg->width = cfg.width;
          msg->height = cfg.height;
          msg->pixel_format = V4L2_PIX_FMT_YUYV;
          msg->_pad = 0U;
          msg->data_size = dataSize;

          const uint8_t fill = static_cast<uint8_t>(idx % 256);
          std::memset(msg->data, fill, dataSize);

          publisher.publish(userPayload);
          loanOk = true;
          if (idx % 100 == 0) {
            LFL.info(name, fmt::format("Published frame {}.", idx));
          }
        })
        .or_else([&](auto &error) {
          LFL.error(name, fmt::format("loan failed (err={}). "
                                      "Check iox_config.toml pool sizes.",
                                      static_cast<int>(error)));
        });

    auto elapsed = std::chrono::steady_clock::now() - frameStart;
    double latencyMs =
        std::chrono::duration<double, std::milli>(elapsed).count();
    metrics.update(idx, loanOk ? payloadSize : 0, latencyMs, loanOk);

    if (metrics.shouldReport(2.0)) {
      LFL.info(name, fmt::format("[METRICS] {}", metrics.generateReport()));
    }

    ++idx;

    elapsed = std::chrono::steady_clock::now() - frameStart;
    if (elapsed < framePeriod)
      std::this_thread::sleep_for(framePeriod - elapsed);
  }

  LFL.info(name, fmt::format("Stopped after {} frames.", idx));
}

} // namespace

int main(int argc, char **argv) {
  auto &log = LockFreeLogger::getInstance();
  log.initialize(std::make_unique<ConsoleAndFileLogWriter>(),
                 QueueMode::IMMEDIATE);

  if (argc < 2) {
    log.error(
        "mock_publisher",
        "Usage: ./orbbec_mock_publisher <name1> [name2 ...] [--config <path>]");
    log.shutdown();
    return 1;
  }

  fs::path cfgPath = "config/orbbec_publisher.yaml";
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--config" && i + 1 < argc)
      cfgPath = argv[++i];

  const MockConfig cfg = loadConfig(cfgPath);

  std::vector<std::string> names;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--config") {
      ++i;
      continue;
    }
    names.emplace_back(a);
  }

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  iox::runtime::PoshRuntime::initRuntime("orbbec_mock_publisher");

  std::vector<std::thread> workers;
  workers.reserve(names.size());
  for (const auto &name : names)
    workers.emplace_back(mockWorker, name, cfg);

  for (auto &t : workers)
    if (t.joinable())
      t.join();

  log.shutdown();
  return 0;
}
