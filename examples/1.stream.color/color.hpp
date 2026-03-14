#ifndef ORBBEC_ICEORYX_COLOR_HPP
#define ORBBEC_ICEORYX_COLOR_HPP

#include <cstdint>

namespace orbbec_iceoryx {

struct alignas(64) ColorFrameData {
    uint64_t timestamp;
    uint32_t width;
    uint32_t height;
    uint32_t encoding; // Format encoding
    uint64_t data_size;
    uint8_t data[1];
};

}

#endif // ORBBEC_ICEORYX_COLOR_HPP
