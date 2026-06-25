#pragma once

#include <cstdint>

namespace orbbec {

// Stats published once per second by orbbec_saver per camera on:
//   service="Orbbec" / instance="<name>" / event="SaverStats"
//
// Fixed-size struct — no heap allocation, safe for iceoryx shared memory.
struct alignas(64) SaverStatsMsg {
    uint64_t timestamp_ns;       // wall clock at publish time
    uint64_t frames_saved;       // total frames written to MCAP since start
    float    save_fps;           // measured over last second
    float    write_latency_ms;   // avg MCAP write latency over last second
    float    e2e_latency_ms;     // avg end-to-end latency over last second
    float    throughput_mbps;    // MB/s written over last second
    uint64_t transmission_drops; // sequence number gaps detected
    uint32_t current_segment;    // current segment index (1-based)
    uint64_t segment_bytes;      // bytes written in current segment
    uint8_t  saver_ok;           // 1=running  0=error
    uint8_t  _pad[3];
    char     last_error[256];    // null-terminated, last error if any
};

} // namespace orbbec
