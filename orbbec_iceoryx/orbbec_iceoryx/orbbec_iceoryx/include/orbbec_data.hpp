#pragma once

#include <cstddef>
#include <cstdint>

namespace orbbec {

// ── Raw frame (iceoryx shared memory) ─────────────────────────────────────
// Layout: [header: kHeaderSize bytes] [raw pixel data: data_size bytes]
// Total iceoryx loan = kHeaderSize + data_size.
struct alignas(64) FrameData {
    uint64_t timestamp_ns;    // [0 ..7 ]  system_clock at capture time (ns)
    uint64_t sequence_number; // [8 ..15]  monotonic per-camera counter
    uint32_t width;           // [16..19]  pixels
    uint32_t height;          // [20..23]  pixels
    uint32_t pixel_format;    // [24..27]  V4L2 fourcc (e.g. V4L2_PIX_FMT_YUYV)
    uint32_t _pad;            // [28..31]  explicit padding -> data_size at offset 32
    uint64_t data_size;       // [32..39]  bytes of pixel data
    uint8_t data[1];          // [40]      variable-length payload start
};

// Byte offset of data[0] within FrameData.
// Total iceoryx loan to request: kHeaderSize + data_size.
inline constexpr std::size_t kHeaderSize = 40U;

// ── JPEG/compressed frame (iceoryx shared memory) ──────────────────────────
// Layout: [header: kJpegHeaderSize bytes] [encoded JPEG bytes: data_size bytes]
// Total iceoryx loan = kJpegHeaderSize + data_size.
struct alignas(64) JpegFrameData {
    uint64_t timestamp_ns;    // [0 ..7 ]  capture time (ns since epoch)
    uint64_t sequence_number; // [8 ..15]  monotonic per-camera counter
    uint32_t is_keyframe;     // [16..19]  always 1 for MJPEG
    uint32_t data_size;       // [20..23]  bytes of encoded JPEG data
    uint8_t  data[1];         // [24]      variable-length payload start
};

inline constexpr std::size_t kJpegHeaderSize = 24U;

} // namespace orbbec
