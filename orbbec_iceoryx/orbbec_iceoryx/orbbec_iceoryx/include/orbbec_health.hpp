#pragma once

#include <cstdint>

namespace orbbec {

// Health message published once per second per camera on iceoryx topic:
//   service="Orbbec" / instance="<name>" / event="Health"
//
// Fixed-size struct — no heap allocation, safe for iceoryx shared memory.
struct alignas(64) HealthMsg {
    uint64_t timestamp_ns;             // wall clock at publish time
    uint64_t frames_published;         // total frames published since start
    uint64_t loan_failures;            // iceoryx pool exhaustion count
    float    actual_fps;               // measured over last second
    float    exposure_time_us;         // unused for V4L2 (always 0)
    float    gain_db;                  // unused for V4L2 (always 0)
    uint32_t width;                    // actual frame width
    uint32_t height;                   // actual frame height
    uint8_t  stream_ok;                // 1=streaming  0=stalled/resetting
    uint8_t  watchdog_resets;          // stream reset count since start
    uint8_t  _pad[2];

    // Last error pinned until replaced by a new one.
    // Fixed-size array — no heap allocation in shared memory.
    uint64_t last_error_timestamp_ns;  // 0 = no error yet
    char     last_error[256];          // null-terminated, truncated to 255 chars
};

} // namespace orbbec
