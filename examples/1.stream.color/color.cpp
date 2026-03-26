#include "libobsensor/ObSensor.hpp"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <chrono>

#include "iceoryx_posh/popo/untyped_publisher.hpp"
#include "iceoryx_posh/popo/publisher_options.hpp"
#include "iceoryx_posh/runtime/posh_runtime.hpp"
#include "iceoryx_posh/capro/service_description.hpp"
#include "iceoryx_hoofs/cxx/types.hpp"

#include "color.hpp"
int main() {
    iox::runtime::PoshRuntime::initRuntime("ob_color_publisher");

    iox::popo::PublisherOptions pubOptions;
    pubOptions.historyCapacity = 16U;  

    iox::popo::UntypedPublisher publisher(iox::capro::ServiceDescription{ iox::capro::IdString_t(iox::cxx::TruncateToCapacity, "Orbbec"),
                                                                          iox::capro::IdString_t(iox::cxx::TruncateToCapacity, "Camera"),
                                                                          iox::capro::IdString_t(iox::cxx::TruncateToCapacity, "ColorStream") },
                                          pubOptions);

    try {
        ob::Pipeline pipe;
        auto         device = pipe.getDevice();

        // Helper lambda: only set a property if the device supports writing it
        auto trySetBool = [&](OBPropertyID id, bool val, const char* name) {
            if(device->isPropertySupported(id, OB_PERMISSION_WRITE))
                device->setBoolProperty(id, val);
            else
                std::cerr << "[WARN] Property not writable, skipping: " << name << std::endl;
        };
        auto trySetInt = [&](OBPropertyID id, int32_t val, const char* name) {
            if(device->isPropertySupported(id, OB_PERMISSION_WRITE))
                device->setIntProperty(id, val);
            else
                std::cerr << "[WARN] Property not writable, skipping: " << name << std::endl;
        };

        // ── Auto Controls OFF ────────────────────────────────────────
        trySetBool(OB_PROP_COLOR_AUTO_EXPOSURE_BOOL,      false, "AUTO_EXPOSURE");
        trySetBool(OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL, false, "AUTO_WHITE_BALANCE");
        trySetInt (OB_PROP_COLOR_BACKLIGHT_COMPENSATION_INT, 0,  "BACKLIGHT_COMPENSATION");
        trySetInt (OB_PROP_COLOR_DENOISING_LEVEL_INT,        0,  "DENOISING_LEVEL");

        // ── Exposure & Gain ──────────────────────────────────────────
        trySetInt(OB_PROP_COLOR_EXPOSURE_INT, 3, "EXPOSURE");
        trySetInt(OB_PROP_COLOR_GAIN_INT,      6, "GAIN");

        // ── White Balance ────────────────────────────────────────────
        trySetInt(OB_PROP_COLOR_WHITE_BALANCE_INT, 4800, "WHITE_BALANCE");

        // ── Image Quality ────────────────────────────────────────────
        trySetInt(OB_PROP_COLOR_SHARPNESS_INT, 32, "SHARPNESS");
        trySetInt(OB_PROP_COLOR_CONTRAST_INT,  47, "CONTRAST");

        // ── Stream Config ────────────────────────────────────────────
        auto config = std::make_shared<ob::Config>();
        config->enableVideoStream(OB_SENSOR_COLOR, 640, 400, 90, OB_FORMAT_RGB);

        pipe.start(config);

        while(true) {
            auto frameSet = pipe.waitForFrameset(1000);  // Increased timeout to 1000ms
            if(!frameSet)
                continue;

            auto colorFrame = frameSet->getFrame(OB_FRAME_COLOR)->as<ob::VideoFrame>();
            if(!colorFrame)
                continue;

            cv::Mat img(colorFrame->height(), colorFrame->width(), CV_8UC3, colorFrame->data());

            // ── Publish to Iceoryx ───────────────────────────────────────
            uint32_t width    = colorFrame->width();
            uint32_t height   = colorFrame->height();
            uint32_t dataSize = colorFrame->dataSize();
            uint32_t format   = colorFrame->format();

            uint64_t payloadSize = sizeof(orbbec_iceoryx::ColorFrameData) + dataSize - 1;

            publisher.loan(static_cast<uint32_t>(payloadSize))
                .and_then([&](auto &userPayload) {
                    auto *data      = static_cast<orbbec_iceoryx::ColorFrameData *>(userPayload);
                    data->timestamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
                    data->width     = width;
                    data->height    = height;
                    data->encoding  = format;
                    data->data_size = dataSize;

                    std::memcpy(data->data, colorFrame->data(), dataSize);
                    publisher.publish(userPayload);
                })
                .or_else([&](auto &error) { std::cerr << "Failed to loan Iceoryx sample: " << static_cast<int>(error) << std::endl; });
        }

        pipe.stop();
    }
    catch(ob::Error &e) {
        std::cerr << "OB Error: " << e.what() << std::endl;
        return -1;
    }
    catch(std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }

    return 0;
}