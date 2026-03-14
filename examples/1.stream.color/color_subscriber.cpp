#include <opencv2/opencv.hpp>
#include <iostream>
#include <chrono>

#include "iceoryx_posh/popo/untyped_subscriber.hpp"
#include "iceoryx_posh/popo/subscriber_options.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"
#include "iceoryx_hoofs/cxx/types.hpp"

#include "color.hpp" // the struct definition

int main() {
    // 1. Initialize Iceoryx Runtime
    iox::runtime::PoshRuntime::initRuntime("ob_color_subscriber");

    // 2. Setup Iceoryx Subscriber options
    iox::popo::SubscriberOptions subOptions;
    subOptions.queueCapacity = 100U; // Must be at least maximum expected frames to accumulate
    subOptions.historyRequest = 0U;

    // 3. Create the Iceoryx UntypedSubscriber
    iox::popo::UntypedSubscriber subscriber(
        iox::capro::ServiceDescription{
            iox::capro::IdString_t(iox::cxx::TruncateToCapacity, "Orbbec"),
            iox::capro::IdString_t(iox::cxx::TruncateToCapacity, "Camera"),
            iox::capro::IdString_t(iox::cxx::TruncateToCapacity, "ColorStream")
        },
        subOptions
    );

    subscriber.subscribe();

    cv::namedWindow("Iceoryx Color Subscriber", cv::WINDOW_NORMAL);

    while (true) {
        // 4. Try to take a sample
        subscriber.take()
            .and_then([&](const void* userPayload) {
                // 5. Cast and process the data chunk
                const auto* data = static_cast<const orbbec_iceoryx::ColorFrameData*>(userPayload);

                // Reconstruct OpenCV matrix from incoming data
                // Assuming CV_8UC3 standard color format as per our publisher
                cv::Mat img(data->height, data->width, CV_8UC3, (void*)data->data);

                // Because we skipped BGR conversion on the publisher, we must do it here
                cv::cvtColor(img, img, cv::COLOR_RGB2BGR);

                cv::imshow("Iceoryx Color Subscriber", img);

                // Release the chunk explicitly 
                subscriber.release(userPayload);

            })
            .or_else([](auto& error) {
                // Ignore no data error if polled frequently, log otherwise.
                if (error != iox::popo::ChunkReceiveResult::NO_CHUNK_AVAILABLE) {
                    std::cerr << "Failed to receive Iceoryx sample: " << static_cast<int>(error) << std::endl;
                }
            });

        // Add display delay to actually render window events
        if (cv::waitKey(1) == 27) {
            break; // ESC to quit
        }
    }

    subscriber.unsubscribe();

    return 0;
}
