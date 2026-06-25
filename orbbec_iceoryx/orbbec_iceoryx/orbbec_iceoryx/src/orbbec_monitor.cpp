#include <chrono>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <yaml-cpp/yaml.h>

#include "iceoryx_posh/popo/subscriber.hpp"
#include "iceoryx_posh/popo/subscriber_options.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"

#include "lflogger.hpp"
#include "mission_logger.hpp"
#include "orbbec_health.hpp"
#include "orbbec_saver_stats.hpp"

namespace fs = std::filesystem;

namespace {

const char *kGreen = "\033[1;32m";
const char *kCyan = "\033[1;36m";
const char *kBold = "\033[1m";
const char *kClear = "\033[2J\033[H";

std::string wallTime() {
  auto now = std::chrono::system_clock::now();
  auto time_t = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ss;
  ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
  return ss.str();
}

std::string errorTime(uint64_t ts_ns) {
  if (ts_ns == 0)
    return "none";
  auto tp =
      std::chrono::system_clock::time_point(std::chrono::nanoseconds(ts_ns));
  auto time_t = std::chrono::system_clock::to_time_t(tp);
  std::ostringstream ss;
  ss << std::put_time(std::localtime(&time_t), "%H:%M:%S");
  return ss.str();
}

void renderDashboard(
    const std::map<std::string, orbbec::HealthMsg> &pubLatest,
    const std::map<std::string, orbbec::SaverStatsMsg> &savLatest,
    const std::vector<std::string> &names) {
  std::cout << kClear;
  std::cout << kBold << kCyan << "  Orbbec Camera Monitor" << orbbec::kReset
            << "                    " << wallTime() << "\n";
  std::cout << std::string(70, '-') << "\n";

  if (pubLatest.empty() && savLatest.empty()) {
    std::cout << "  Waiting for cameras...\n";
    std::cout << std::string(70, '-') << "\n";
    std::cout << "  Press Ctrl+C to exit\n";
    std::cout.flush();
    return;
  }

  for (const auto &name : names) {
    const bool hasPub = pubLatest.count(name) > 0;
    const bool hasSav = savLatest.count(name) > 0;

    if (hasPub) {
      const auto &pub = pubLatest.at(name);
      const bool ok = pub.stream_ok != 0;

      std::cout << kBold << "  " << name << orbbec::kReset << "  "
                << (ok ? kGreen : orbbec::kRed) << (ok ? "● OK" : "✖ STALLED")
                << orbbec::kReset << "\n";

      std::cout << fmt::format("    pub  fps={:.1f}  {}x{}\n", pub.actual_fps,
                               pub.width, pub.height);

      std::cout << fmt::format(
          "         published={}  loan_failures={}  resets={}\n",
          pub.frames_published, pub.loan_failures,
          static_cast<int>(pub.watchdog_resets));

      if (pub.last_error_timestamp_ns > 0) {
        std::cout << orbbec::kRed << "    pub error ["
                  << errorTime(pub.last_error_timestamp_ns)
                  << "]: " << pub.last_error << orbbec::kReset << "\n";
      }
    } else {
      std::cout << kBold << "  " << name << orbbec::kReset << "  "
                << orbbec::kYellow << "● publisher not seen" << orbbec::kReset
                << "\n";
    }

    if (hasSav) {
      const auto &sav = savLatest.at(name);
      const bool ok = sav.saver_ok != 0;

      std::cout << fmt::format(
          "    sav  fps={:.1f}  saved={}  seg={}  seg_size={} MiB  "
          "write={:.2f}ms  e2e={:.2f}ms  {}\n",
          sav.save_fps, sav.frames_saved, sav.current_segment,
          sav.segment_bytes / 1024 / 1024, sav.write_latency_ms,
          sav.e2e_latency_ms,
          ok ? "" : std::string(orbbec::kRed) + "SAVER ERROR" + orbbec::kReset);

      if (sav.transmission_drops > 0) {
        std::cout << orbbec::kYellow
                  << fmt::format("         transmission_drops={}\n",
                                 sav.transmission_drops)
                  << orbbec::kReset;
      }

      if (sav.last_error[0] != '\0') {
        std::cout << orbbec::kRed << "    sav error: " << sav.last_error
                  << orbbec::kReset << "\n";
      }
    } else {
      std::cout << "    sav  not connected\n";
    }

    std::cout << "\n";
  }

  std::cout << std::string(70, '-') << "\n";
  std::cout << "  Press Ctrl+C to exit\n";
  std::cout.flush();
}

} // namespace

int main(int argc, char **argv) {
  auto &LFL = LockFreeLogger::getInstance();
  LFL.initialize(std::make_unique<ConsoleAndFileLogWriter>(),
                 QueueMode::IMMEDIATE);

  fs::path cfgPath =
      fs::path(argv[0]).parent_path() / "config/orbbec_saver.yaml";
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--config" && i + 1 < argc)
      cfgPath = argv[++i];

  std::vector<std::string> names;
  try {
    YAML::Node cfg = YAML::LoadFile(cfgPath.string());
    if (cfg["devices"] && cfg["devices"].IsSequence())
      for (const auto &s : cfg["devices"])
        names.emplace_back(s.as<std::string>());
  } catch (const std::exception &e) {
    LFL.warn("monitor", fmt::format("Could not load config ({}): {}",
                                    cfgPath.string(), e.what()));
  }

  if (names.empty()) {
    LFL.error("monitor", "No devices found in config. "
                         "Add them to orbbec_saver.yaml under 'devices:'.");
    LFL.shutdown();
    return 1;
  }

  iox::runtime::PoshRuntime::initRuntime("orbbec_monitor");

  using HealthSub = iox::popo::Subscriber<orbbec::HealthMsg>;
  std::vector<std::unique_ptr<HealthSub>> healthSubs;

  using StatsSub = iox::popo::Subscriber<orbbec::SaverStatsMsg>;
  std::vector<std::unique_ptr<StatsSub>> statsSubs;

  for (const auto &name : names) {
    iox::popo::SubscriberOptions opts;
    opts.queueCapacity = 2U;
    opts.historyRequest = 0U;

    healthSubs.emplace_back(std::make_unique<HealthSub>(
        iox::capro::ServiceDescription{
            iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
            iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
            iox::capro::IdString_t(iox::TruncateToCapacity, "Health")},
        opts));

    statsSubs.emplace_back(std::make_unique<StatsSub>(
        iox::capro::ServiceDescription{
            iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
            iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
            iox::capro::IdString_t(iox::TruncateToCapacity, "SaverStats")},
        opts));
  }

  std::map<std::string, orbbec::HealthMsg> pubLatest;
  std::map<std::string, orbbec::SaverStatsMsg> savLatest;

  while (true) {
    for (std::size_t i = 0; i < names.size(); ++i) {
      healthSubs[i]
          ->take()
          .and_then([&](auto &sample) { pubLatest[names[i]] = *sample; })
          .or_else([](auto &) {});

      statsSubs[i]
          ->take()
          .and_then([&](auto &sample) { savLatest[names[i]] = *sample; })
          .or_else([](auto &) {});
    }

    renderDashboard(pubLatest, savLatest, names);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  LFL.shutdown();
  return 0;
}
