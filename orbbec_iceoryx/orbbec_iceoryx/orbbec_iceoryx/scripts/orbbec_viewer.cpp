#include <atomic>
#include <chrono>
#include <csignal>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/opencv.hpp>
#include <opencv2/videoio.hpp>

#include "iceoryx_posh/popo/subscriber_options.hpp"
#include "iceoryx_posh/popo/untyped_subscriber.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"

#include "lflogger.hpp"
#include "orbbec_data.hpp"

std::atomic<bool> gRun{true};
void onSignal(int) { gRun.store(false); }

int main(int argc, char **argv) {
    auto &LFL = LockFreeLogger::getInstance();
    LFL.initialize(std::make_unique<ConsoleAndFileLogWriter>(), QueueMode::THREADED);

    std::string name         = "cam0";
    bool useMjpeg            = false;
    bool useMjpegStream      = false;
    std::string mjpegUrl     = "http://localhost:9000/";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--name" && i + 1 < argc) {
            name = argv[++i];
        } else if (arg == "--mjpeg") {
            useMjpeg = true;
        } else if (arg == "--mjpeg-stream") {
            useMjpegStream = true;
        } else if (arg == "--mjpeg-url" && i + 1 < argc) {
            mjpegUrl = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n"
                      << "Options:\n"
                      << "  --name <name>        Camera name (default: cam0)\n"
                      << "  --mjpeg              iceoryx MJPEG stream (Orbbec/<name>/MJPEG)\n"
                      << "  --mjpeg-stream       HTTP MJPEG stream (use with --mjpeg-url)\n"
                      << "  --mjpeg-url <url>    HTTP MJPEG URL (default: http://localhost:9000/)\n"
                      << "  Default              Raw iceoryx frame (Orbbec/<name>/Frame)\n";
            LFL.shutdown();
            return 0;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    auto lastFpsTime = std::chrono::steady_clock::now();
    int    frameCount = 0;
    double currentFps = 0.0;

    auto updateFps = [&](const std::string &windowTitle) {
        frameCount++;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - lastFpsTime).count();
        if (elapsed >= 1.0) {
            currentFps  = frameCount / elapsed;
            frameCount  = 0;
            lastFpsTime = now;
            cv::setWindowTitle(windowTitle,
                windowTitle + "  |  " + std::to_string(static_cast<int>(std::round(currentFps))) + " fps");
            LFL.info("viewer", fmt::format("[{}] FPS: {:.1f}", windowTitle, currentFps));
        }
    };

    if (useMjpegStream) {
        LFL.info("viewer", fmt::format("Connecting to HTTP MJPEG stream: {}", mjpegUrl));
        cv::VideoCapture cap(mjpegUrl);
        if (!cap.isOpened()) {
            LFL.error("viewer", fmt::format("Could not open MJPEG stream at {}", mjpegUrl));
            LFL.shutdown();
            return 1;
        }

        cv::namedWindow("Orbbec Viewer (HTTP MJPEG)", cv::WINDOW_NORMAL);
        cv::Mat frame;

        while (gRun.load()) {
            if (!cap.read(frame)) {
                LFL.warn("viewer", "Failed to grab frame from MJPEG stream");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            updateFps("Orbbec Viewer (HTTP MJPEG)");

            cv::putText(frame, cv::format("FPS: %.1f", currentFps), cv::Point(25, 50),
                        cv::FONT_HERSHEY_SIMPLEX, 1.2, cv::Scalar(0, 255, 0), 2);
            cv::putText(frame, "Stream: HTTP MJPEG", cv::Point(25, 95),
                        cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);

            cv::imshow("Orbbec Viewer (HTTP MJPEG)", frame);

            int key = cv::waitKey(1);
            if (key == 27 || key == 'q' || key == 'Q')
                break;
        }
    } else {
        iox::runtime::PoshRuntime::initRuntime("orbbec_viewer");

        iox::popo::SubscriberOptions options;
        options.queueCapacity = 4U;
        options.historyRequest = 0U;
        options.queueFullPolicy = iox::popo::QueueFullPolicy::DISCARD_OLDEST_DATA;

        if (useMjpeg) {
            LFL.info("viewer", fmt::format("Subscribing to iceoryx MJPEG: Orbbec/{}/MJPEG", name));

            iox::popo::UntypedSubscriber subscriber(
                iox::capro::ServiceDescription{
                    iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
                    iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
                    iox::capro::IdString_t(iox::TruncateToCapacity, "MJPEG")},
                options);

            subscriber.subscribe();

            cv::namedWindow("Orbbec Viewer (MJPEG)", cv::WINDOW_NORMAL);

            while (gRun.load()) {
                bool tookOne = false;

                subscriber.take()
                    .and_then([&](const void *payload) {
                        tookOne = true;
                        const auto *frame =
                            static_cast<const orbbec::JpegFrameData *>(payload);

                        if (frame && frame->data_size > 0) {
                            std::vector<uint8_t> jpegBuf(
                                frame->data, frame->data + frame->data_size);
                            cv::Mat img = cv::imdecode(jpegBuf, cv::IMREAD_COLOR);

                            if (!img.empty()) {
                                updateFps("Orbbec Viewer (MJPEG)");

                                cv::putText(img, cv::format("FPS: %.1f", currentFps),
                                            cv::Point(25, 50), cv::FONT_HERSHEY_SIMPLEX, 1.2,
                                            cv::Scalar(0, 255, 0), 2);
                                cv::putText(img, name + " [MJPEG]",
                                            cv::Point(25, 95), cv::FONT_HERSHEY_SIMPLEX, 1.0,
                                            cv::Scalar(0, 255, 0), 2);

                                cv::imshow("Orbbec Viewer (MJPEG)", img);
                            }
                        }
                        subscriber.release(payload);
                    })
                    .or_else([](auto &) {});

                int key = cv::waitKey(1);
                if (key == 27 || key == 'q' || key == 'Q')
                    break;

                if (!tookOne)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            LFL.info("viewer", fmt::format("Subscribing to iceoryx Frame: Orbbec/{}/Frame", name));

            iox::popo::UntypedSubscriber subscriber(
                iox::capro::ServiceDescription{
                    iox::capro::IdString_t(iox::TruncateToCapacity, "Orbbec"),
                    iox::capro::IdString_t(iox::TruncateToCapacity, name.c_str()),
                    iox::capro::IdString_t(iox::TruncateToCapacity, "Frame")},
                options);

            subscriber.subscribe();

            cv::namedWindow("Orbbec Viewer", cv::WINDOW_NORMAL);

            while (gRun.load()) {
                bool tookOne = false;

                subscriber.take()
                    .and_then([&](const void *payload) {
                        tookOne = true;
                        const auto *frame =
                            static_cast<const orbbec::FrameData *>(payload);

                        if (frame && frame->width > 0 && frame->height > 0) {
                            // For YUYV: convert to BGR via OpenCV
                            cv::Mat yuv(frame->height, frame->width, CV_8UC2,
                                        const_cast<uint8_t *>(frame->data));
                            cv::Mat bgr;
                            cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_YUYV);

                            updateFps("Orbbec Viewer");

                            cv::putText(bgr, cv::format("FPS: %.1f", currentFps),
                                        cv::Point(25, 50), cv::FONT_HERSHEY_SIMPLEX, 1.2,
                                        cv::Scalar(0, 255, 0), 2);
                            cv::putText(bgr, name, cv::Point(25, 95),
                                        cv::FONT_HERSHEY_SIMPLEX, 1.0,
                                        cv::Scalar(0, 255, 0), 2);

                            cv::imshow("Orbbec Viewer", bgr);
                        }
                        subscriber.release(payload);
                    })
                    .or_else([](auto &) {});

                int key = cv::waitKey(1);
                if (key == 27 || key == 'q' || key == 'Q')
                    break;

                if (!tookOne)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    cv::destroyAllWindows();
    LFL.info("viewer", "Viewer exiting.");
    LFL.shutdown();
    return 0;
}
