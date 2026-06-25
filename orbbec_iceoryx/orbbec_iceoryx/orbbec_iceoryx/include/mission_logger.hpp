#pragma once

// Non-blocking mission logger.
// Calling thread writes to console immediately (with ANSI colour for warn/error),
// then enqueues the message. A background thread drains the queue and writes to
// a timestamped .log file. If the queue is full, the message is dropped rather
// than blocking the caller.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

namespace orbbec {

// ANSI colour codes — used by both logger and other files
inline constexpr const char* kRed    = "\033[1;31m";
inline constexpr const char* kYellow = "\033[1;33m";
inline constexpr const char* kReset  = "\033[0m";

enum class LogLevel { INFO, WARN, ERR };

class MissionLogger {
public:
    explicit MissionLogger(std::size_t capacity = 4096)
        : capacity_(capacity) {}

    ~MissionLogger() { stop(); }

    // Open log file and start background writer thread.
    void start(const std::filesystem::path& log_path) {
        std::filesystem::create_directories(log_path.parent_path());
        file_.open(log_path, std::ios::out | std::ios::app);
        if (!file_.is_open())
            std::cerr << kRed << "[MissionLogger] Failed to open: "
                      << log_path << kReset << "\n";
        running_.store(true);
        writer_ = std::thread(&MissionLogger::writeLoop, this);
    }

    // Flush and stop.
    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (writer_.joinable()) writer_.join();
        if (file_.is_open()) file_.close();
    }

    void info(const std::string& tag, const std::string& msg) {
        log(LogLevel::INFO, tag, msg);
    }
    void warn(const std::string& tag, const std::string& msg) {
        log(LogLevel::WARN, tag, msg);
    }
    void error(const std::string& tag, const std::string& msg) {
        log(LogLevel::ERR, tag, msg);
    }

    uint64_t dropped() const { return dropped_.load(); }

private:
    void log(LogLevel level, const std::string& tag, const std::string& msg) {
        const std::string line = format(level, tag, msg);

        // Console — immediate on calling thread
        if (level == LogLevel::ERR)
            std::cerr << kRed    << line << kReset << "\n";
        else if (level == LogLevel::WARN)
            std::cerr << kYellow << line << kReset << "\n";
        else
            std::cout << line << "\n";

        // Enqueue for file write — non-blocking
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (queue_.size() >= capacity_) { ++dropped_; return; }
            queue_.push(line);
        }
        cv_.notify_one();
    }

    static std::string timestamp() {
        auto now    = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms     = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()) % 1000;
        std::ostringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S")
           << "." << std::setw(3) << std::setfill('0') << ms.count();
        return ss.str();
    }

    static std::string format(LogLevel level, const std::string& tag,
                               const std::string& msg) {
        const char* lvl = level == LogLevel::ERR  ? "ERROR" :
                          level == LogLevel::WARN  ? "WARN " : "INFO ";
        std::ostringstream ss;
        ss << "[" << timestamp() << "] [" << lvl << "] ["
           << std::setw(12) << std::left << tag << "] " << msg;
        return ss.str();
    }

    void writeLoop() {
        while (true) {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [this] {
                return !queue_.empty() || !running_.load();
            });
            while (!queue_.empty()) {
                const std::string line = std::move(queue_.front());
                queue_.pop();
                lk.unlock();
                if (file_.is_open()) file_ << line << "\n";
                lk.lock();
            }
            if (!running_.load() && queue_.empty()) break;
        }
        if (file_.is_open()) file_.flush();
    }

    std::size_t              capacity_;
    std::queue<std::string>  queue_;
    std::mutex               mutex_;
    std::condition_variable  cv_;
    std::atomic<bool>        running_{false};
    std::atomic<uint64_t>    dropped_{0};
    std::ofstream            file_;
    std::thread              writer_;
};

} // namespace orbbec