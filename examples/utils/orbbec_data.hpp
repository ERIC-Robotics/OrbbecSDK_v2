#ifndef ORBBEC_DATA_HPP
#define ORBBEC_DATA_HPP

#include <cstdint>

namespace orbbec_iceoryx {

struct alignas(64) ColorFrameData {
    uint64_t timestamp;
    uint64_t sequence_number;
    uint32_t width;
    uint32_t height;
    uint32_t encoding; // e.g., OB_FORMAT_RGB
    uint64_t data_size;
    uint8_t data[1];
};

}

#endif // ORBBEC_DATA_HPP
