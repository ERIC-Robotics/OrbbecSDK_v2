#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace lucid {

struct MjpegStreamTimes {
  double demosaicMs;
  double jpegEncodeMs;
};

class MjpegStreamer {
public:
  MjpegStreamer(int port, int quality)
      : port_(port), quality_(quality), running_(false), listenFd_(-1),
        currentFrameId_(0) {}

  ~MjpegStreamer() { stop(); }

  void start() {
    if (running_.exchange(true))
      return;
    acceptThread_ = std::thread([this] { acceptLoop(); });
  }

  void stop() {
    if (!running_.exchange(false))
      return;
    if (listenFd_ >= 0) {
      ::shutdown(listenFd_, SHUT_RDWR);
    }
    frameCv_.notify_all();
    if (acceptThread_.joinable()) {
      acceptThread_.join();
    }
    if (listenFd_ >= 0) {
      ::close(listenFd_);
      listenFd_ = -1;
    }
  }

  int getPort() const { return port_; }

  MjpegStreamTimes pushFrame(const uint8_t *srcData, uint32_t frameW,
                             uint32_t frameH) {
    MjpegStreamTimes times{0.0, 0.0};

    if (srcData == nullptr || frameW == 0 || frameH == 0) {
      return times;
    }

    // 1. Demosaic: BayerRG8 -> BGR
    auto tStart = std::chrono::steady_clock::now();
    cv::Mat bayer(static_cast<int>(frameH), static_cast<int>(frameW), CV_8UC1,
                  const_cast<uint8_t *>(srcData));
    cv::Mat bgr;
    cv::cvtColor(bayer, bgr, cv::COLOR_BayerRG2BGR);
    auto tDemosaic = std::chrono::steady_clock::now();
    times.demosaicMs =
        std::chrono::duration<double, std::milli>(tDemosaic - tStart).count();

    // 2. JPEG Encode
    std::vector<uint8_t> jpegBuf;
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, quality_};
    cv::imencode(".jpg", bgr, jpegBuf, params);
    auto tEncode = std::chrono::steady_clock::now();
    times.jpegEncodeMs =
        std::chrono::duration<double, std::milli>(tEncode - tDemosaic).count();

    // 3. Update frame buffer for HTTP stream
    {
      std::lock_guard<std::mutex> lk(frameMtx_);
      frame_ = std::move(jpegBuf);
      currentFrameId_++;
    }
    frameCv_.notify_all();

    return times;
  }

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
      ::close(listenFd_);
      listenFd_ = -1;
      return;
    }
    ::listen(listenFd_, 8);

    while (running_.load()) {
      sockaddr_in ca{};
      socklen_t cl = sizeof(ca);
      int fd = ::accept(listenFd_, reinterpret_cast<sockaddr *>(&ca), &cl);
      if (fd < 0) {
        break;
      }
      // Drain the initial request header (not strictly parsed, just cleared)
      char rbuf[2048];
      int received = ::recv(fd, rbuf, sizeof(rbuf) - 1, 0);
      if (received > 0) {
        std::thread([this, fd] { serveClient(fd); }).detach();
      } else {
        ::close(fd);
      }
    }
  }

  void serveClient(int fd) {
    if (::send(fd, kHttpHdr, strlen(kHttpHdr), MSG_NOSIGNAL) < 0) {
      ::close(fd);
      return;
    }
    uint64_t lastSentId = 0;
    while (running_.load()) {
      std::vector<uint8_t> jpeg;
      {
        std::unique_lock<std::mutex> lk(frameMtx_);
        frameCv_.wait_for(lk, std::chrono::seconds(2), [this, lastSentId] {
          return currentFrameId_ > lastSentId || !running_.load();
        });
        if (!running_.load())
          break;
        if (currentFrameId_ <= lastSentId)
          continue;
        jpeg = frame_; // snapshot
        lastSentId = currentFrameId_;
      }
      std::string hdr = "--mjpeg_boundary\r\n"
                        "Content-Type: image/jpeg\r\n"
                        "Content-Length: " +
                        std::to_string(jpeg.size()) +
                        "\r\n"
                        "\r\n";
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
  int quality_;
  std::atomic<bool> running_;
  int listenFd_;
  std::thread acceptThread_;
  std::mutex frameMtx_;
  std::condition_variable frameCv_;
  std::vector<uint8_t> frame_;
  uint64_t currentFrameId_;
};

} // namespace lucid
