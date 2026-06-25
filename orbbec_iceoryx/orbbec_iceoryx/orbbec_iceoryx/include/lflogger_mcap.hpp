#pragma once

#include "lflogger.hpp"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mcap/writer.hpp>
#include <mutex>
#include <random>
#include <sstream>
#include <sys/stat.h>

namespace trigger_with_comp {

inline std::string generateUUID() {
  static std::random_device rd;
  static std::mt19937 gen(rd());
  static std::uniform_int_distribution<> dis(0, 15);
  static const char *chars = "0123456789abcdef";
  std::string uuid;
  uuid.reserve(36);
  for (int i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      uuid += '-';
    } else if (i == 14) {
      uuid += '4';
    } else if (i == 19) {
      uuid += chars[(dis(gen) & 0x3) | 0x8];
    } else {
      uuid += chars[dis(gen)];
    }
  }
  return uuid;
}

class McapLogWriter : public ConsoleAndFileLogWriter {
public:
  explicit McapLogWriter(const std::string &prefix = "watchdog",
                         const std::string &topic = "logs",
                         const std::string &filepath = "")
      : ConsoleAndFileLogWriter(filepath), m_topic(topic) {

    // Create log folder using POSIX mkdir
    const std::string logDir = "log";
    if (::mkdir(logDir.c_str(), 0755) != 0 && errno != EEXIST) {
      std::cerr << "[WARN] Could not create log directory: " << strerror(errno)
                << std::endl;
    }

    std::string uuid = generateUUID();

    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_r(&tt, &tm_info);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm_info);

    // Format: <PREFIX>_mission_<UUID>_<DATETIME>.mcap
    std::string filename = (prefix.empty() ? "" : (prefix + "_")) + "mission_" +
                           uuid + "_" + ts + ".mcap";
    m_mcapPath = logDir + "/" + filename;

    std::cout << "[INFO] Watchdog log: " << m_mcapPath << std::endl;

    mcap::McapWriterOptions opts("");
    opts.noChunking = 1;

    auto status = m_writer.open(m_mcapPath, opts);
    if (!status.ok()) {
      std::cerr << "[McapLogWriter] Failed to open MCAP file: "
                << status.message << std::endl;
      return;
    }

    mcap::Schema schema("foxglove.Log", "jsonschema", R"({
          "type": "object",
          "properties": {
            "timestamp": { "type": "integer" },
            "level": { "type": "integer" },
            "message": { "type": "string" },
            "name": { "type": "string" }
          }
        })");
    m_writer.addSchema(schema);

    mcap::Channel channel(m_topic, "json", schema.id);
    m_writer.addChannel(channel);
    m_channelId = channel.id;
    m_opened = true;
  }

  ~McapLogWriter() override { close(); }

  void write(LogLevel level, const std::string &timestamp,
             const std::string &prefix, const std::string &message) override {
    // Log to console using base class ConsoleAndFileLogWriter
    ConsoleAndFileLogWriter::write(level, timestamp, prefix, message);

    // DEBUG logs go to console only — do not persist to MCAP file
    // if (level == LogLevel::DEBUG)
    //   return;

    if (!m_opened)
      return;

    // Convert LogLevel to Foxglove Log level:
    // 1 = UNKNOWN, 2 = DEBUG, 3 = INFO, 4 = WARNING, 5 = ERROR, 6 = FATAL
    int mcapLevel = 1;
    switch (level) {
    case LogLevel::DEBUG:
      mcapLevel = 2;
      break;
    case LogLevel::INFO:
      mcapLevel = 3;
      break;
    case LogLevel::WARN:
      mcapLevel = 4;
      break;
    case LogLevel::ERROR:
      mcapLevel = 5;
      break;
    }

    uint64_t nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();

    // Escape JSON payload message and prefix
    std::string escapedMsg;
    for (char c : message) {
      if (c == '"')
        escapedMsg += "\\\"";
      else if (c == '\\')
        escapedMsg += "\\\\";
      else if (c == '\n')
        escapedMsg += "\\n";
      else if (c == '\r')
        escapedMsg += "\\r";
      else if (c == '\t')
        escapedMsg += "\\t";
      else
        escapedMsg += c;
    }

    std::string escapedPrefix;
    for (char c : prefix) {
      if (c == '"')
        escapedPrefix += "\\\"";
      else if (c == '\\')
        escapedPrefix += "\\\\";
      else
        escapedPrefix += c;
    }

    std::string jsonPayload = fmt::format(
        R"({{"timestamp":{},"level":{},"message":"{}","name":"{}"}})", nowNs,
        mcapLevel, escapedMsg, escapedPrefix);

    mcap::Message msg;
    msg.channelId = m_channelId;
    msg.sequence = m_seq++;
    msg.logTime = nowNs;
    msg.publishTime = nowNs;
    msg.data = reinterpret_cast<const std::byte *>(jsonPayload.data());
    msg.dataSize = jsonPayload.size();

    std::lock_guard<std::mutex> lock(m_mutex);
    auto status = m_writer.write(msg);
    if (!status.ok()) {
      std::cerr << "[McapLogWriter] Failed to write log message: "
                << status.message << std::endl;
    }
  }

  void flush() override { ConsoleAndFileLogWriter::flush(); }

  void close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_opened) {
      m_writer.close();
      m_opened = false;
    }
  }

private:
  std::string m_mcapPath;
  std::string m_topic;
  mcap::McapWriter m_writer;
  mcap::ChannelId m_channelId{0};
  uint32_t m_seq{0};
  bool m_opened{false};
  std::mutex m_mutex;
};

} // namespace trigger_with_comp
