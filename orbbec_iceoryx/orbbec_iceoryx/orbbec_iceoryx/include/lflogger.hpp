// David Noronha
#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fmt/color.h>
#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// ── Log Levels ─────────────────────────────────────────────────────────────
enum class LogLevel { DEBUG, INFO, WARN, ERROR };

// ── Log Writer Interface ───────────────────────────────────────────────────
// Implement this interface to easily change where or how logs are processed
// (e.g., sending to syslog, database, network, custom UI, etc.).
class ILogWriter {
public:
  virtual ~ILogWriter() = default;

  // Called sequentially by the background flusher thread.
  virtual void write(LogLevel level, const std::string &timestamp,
                     const std::string &prefix, const std::string &message) = 0;

  // Optional flush command run periodically or on termination.
  virtual void flush() {}
};

// ── Default Writer: Console + Local File Output ────────────────────────────
class ConsoleAndFileLogWriter : public ILogWriter {
private:
  std::ofstream m_file;

public:
  explicit ConsoleAndFileLogWriter(const std::string &filepath = "") {
    if (!filepath.empty()) {
      m_file.open(filepath, std::ios::app);
    }
  }

  ~ConsoleAndFileLogWriter() override = default;

  void write(LogLevel level, const std::string &timestamp,
             const std::string &prefix, const std::string &message) override {
    if (m_file.is_open()) {
      std::string levelStr;
      switch (level) {
      case LogLevel::DEBUG:
        levelStr = "DEBUG";
        break;
      case LogLevel::INFO:
        levelStr = "INFO";
        break;
      case LogLevel::WARN:
        levelStr = "WARN";
        break;
      case LogLevel::ERROR:
        levelStr = "ERROR";
        break;
      }
      std::string pfx = prefix.empty() ? "" : "[" + prefix + "] ";
      m_file << "[" << timestamp << "] [" << levelStr << "] " << pfx << message
             << "\n";
    }
  }

  void flush() override {
    if (m_file.is_open()) {
      m_file.flush();
    }
  }
};

enum class QueueMode {
  IMMEDIATE, // Write to file/other log writer synchronously on calling thread
  THREADED,  // Write to file/other log writer via a background thread
  MANUAL     // Write to file/other log writer when ioTick() is called manually
};

// ── Lock-Free Log Dispatcher (MPSC Stack Queue) ────────────────────────────
class LockFreeLogger {
public:
  struct LogMessage {
    LogLevel level;
    std::string timestamp;
    std::string prefix;
    std::string message;
  };

  static LockFreeLogger &getInstance() {
    static LockFreeLogger instance;
    return instance;
  }

  // Initialize with a custom log writer and queue mode.
  void initialize(std::unique_ptr<ILogWriter> writer,
                  QueueMode mode = QueueMode::THREADED) {
    m_writer = std::move(writer);
    m_mode = mode;
    if (m_mode == QueueMode::THREADED) {
      m_shouldStop.store(false);
      m_flushThread = std::thread(&LockFreeLogger::flushLoop, this);
    }
  }

  ~LockFreeLogger() {
    m_shouldStop.store(true);
    if (m_flushThread.joinable()) {
      m_flushThread.join();
    }
    // Drain any remaining queued logs
    ioTick();
  }

  // High performance logging: always logs to console immediately,
  // and routes to ILogWriter according to the selected mode.
  void log(LogLevel level, const std::string &prefix,
           const std::string &message) {
    // 1. Log to console immediately (thread-safe using console mutex)
    logToConsole(level, prefix, message);

    // 2. Queue or write immediately to ILogWriter if registered
    if (!m_writer)
      return;

    auto timestamp = getTimestamp();

    if (m_mode == QueueMode::IMMEDIATE) {
      std::lock_guard<std::mutex> lock(m_writerMutex);
      m_writer->write(level, timestamp, prefix, message);
      m_writer->flush();
    } else {
      auto msg = std::make_shared<LogMessage>(
          LogMessage{level, std::move(timestamp), prefix, message});
      Node *newNode = new Node{std::move(msg), nullptr};
      Node *oldHead = m_head.load(std::memory_order_relaxed);
      do {
        newNode->next = oldHead;
      } while (!m_head.compare_exchange_weak(oldHead, newNode,
                                             std::memory_order_release,
                                             std::memory_order_relaxed));
    }
  }

  // Drain the lock-free queue and write messages to file/other log writer
  void ioTick() {
    if (!m_writer)
      return;

    // Atomically retrieve and clear the accumulated lock-free list
    Node *current = m_head.exchange(nullptr, std::memory_order_acquire);
    if (!current)
      return;

    // Reverse the stack nodes to process in original chronological order (FIFO)
    Node *prev = nullptr;
    Node *next = nullptr;
    while (current != nullptr) {
      next = current->next;
      current->next = prev;
      prev = current;
      current = next;
    }

    // Flush entries out using the assigned LogWriter strategy under mutex
    std::lock_guard<std::mutex> lock(m_writerMutex);
    Node *printer = prev;
    while (printer != nullptr) {
      if (printer->data) {
        m_writer->write(printer->data->level, printer->data->timestamp,
                        printer->data->prefix, printer->data->message);
      }
      Node *temp = printer;
      printer = printer->next;
      delete temp;
    }
    m_writer->flush();
  }

  void shutdown() {
    ioTick();
    m_writer.reset();
  }

  // Semantic helper interface
  void debug(const std::string &prefix, const std::string &msg) {
    log(LogLevel::DEBUG, prefix, msg);
  }
  void info(const std::string &prefix, const std::string &msg) {
    log(LogLevel::INFO, prefix, msg);
  }
  void warn(const std::string &prefix, const std::string &msg) {
    log(LogLevel::WARN, prefix, msg);
  }
  void error(const std::string &prefix, const std::string &msg) {
    log(LogLevel::ERROR, prefix, msg);
  }

  static std::string getTimestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    struct tm tm_info;
    localtime_r(&tt, &tm_info);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_info);
    return buf;
  }

private:
  LockFreeLogger()
      : m_head(nullptr), m_shouldStop(false), m_mode(QueueMode::THREADED) {
    // const char *env_debug = std::getenv("ERIC_DEBUG");
    m_debugEnabled =
        true; // (env_debug != nullptr && std::string(env_debug) == "1");
  }

  void logToConsole(LogLevel level, const std::string &prefix,
                    const std::string &message) {

    std::string levelStr;
    fmt::text_style style;
    switch (level) {
    case LogLevel::DEBUG:
      levelStr = "DEBUG";
      style = fg(fmt::color::green);
      break;
    case LogLevel::INFO:
      levelStr = "INFO";
      style = fg(fmt::color::blue);
      break;
    case LogLevel::WARN:
      levelStr = "WARN";
      style = fg(fmt::color::yellow);
      break;
    case LogLevel::ERROR:
      levelStr = "ERROR";
      style = fg(fmt::color::red);
      break;
    }

    std::string pfx = prefix.empty() ? "" : fmt::format("[{}] ", prefix);
    std::string tagStr = fmt::format(style, "[{}]", levelStr);
    std::string formattedLog = fmt::format("{} {}{}", tagStr, pfx, message);

    std::lock_guard<std::mutex> lock(m_consoleMutex);
    if (level == LogLevel::ERROR) {
      std::cerr << formattedLog << "\n";
    } else {
      std::cout << formattedLog << "\n";
    }
  }

  struct Node {
    std::shared_ptr<LogMessage> data;
    Node *next;
  };

  std::atomic<Node *> m_head;
  std::thread m_flushThread;
  std::unique_ptr<ILogWriter> m_writer;
  std::atomic<bool> m_shouldStop;
  QueueMode m_mode;
  bool m_debugEnabled;
  std::mutex m_consoleMutex;
  std::mutex m_writerMutex;

  void flushLoop() {
    while (!m_shouldStop.load(std::memory_order_relaxed) ||
           m_head.load(std::memory_order_relaxed) != nullptr) {
      ioTick();
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
};
