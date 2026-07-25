#pragma once

#include <mcap/writer.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace orbbec {

// ── VideoFrameData ─────────────────────────────────────────────────────────
// Lightweight frame descriptor passed to VideoMcapWriter.
// The actual pixel/encoded data is passed separately as a raw pointer.
struct VideoFrameData {
    uint64_t timestamp_ns{0};    // frame capture time (nanoseconds since epoch)
    uint64_t sequence_number{0}; // monotonic frame counter
    uint32_t is_keyframe{0};     // 1 if IDR / full frame (always 1 for MJPEG)
    uint32_t data_size{0};       // size of the encoded frame in bytes
    const void* data{nullptr};   // pointer to encoded data (used by write_frame)
};

// ── VideoMcapWriter ─────────────────────────────────────────────────────────
// Writes encoded video frames (MJPEG, H.264, etc.) to an MCAP file using the
// foxglove.CompressedVideo protobuf schema.
//
// Usage:
//   VideoMcapWriter w("out.mcap", "OrbbecVideo/SN12345/MJPEG", 64*1024*1024);
//   w.open();
//   w.write_frame_raw(&hdr, data_ptr, data_size);   // per-frame
//   w.close();
class VideoMcapWriter {
public:
    // topic     : MCAP channel topic (e.g. "OrbbecVideo/SN12345/MJPEG")
    // chunk_size: MCAP chunk size in bytes (default 64 MiB)
    // frame_id  : foxglove CompressedVideo frame_id field (defaults to topic)
    explicit VideoMcapWriter(std::filesystem::path output_path,
                             std::string           topic      = "OrbbecVideo/Camera",
                             uint64_t              chunk_size = 64ULL * 1024 * 1024,
                             std::string           frame_id   = "");

    ~VideoMcapWriter();

    // Opens the file and registers schema + channel. Throws on failure.
    void open();

    // Write one frame from a fully-populated VideoFrameData (data pointer inside hdr).
    void write_frame(const VideoFrameData* frame);

    // Write one frame with header + separate data pointer (avoids copy if already mapped).
    void write_frame_raw(const VideoFrameData* hdr, const void* data, size_t data_size);

    // Flushes and closes. Idempotent.
    void close();

    bool     is_open()        const { return opened_ && !closed_; }
    uint64_t frames_written() const { return frames_written_; }
    uint64_t bytes_written()  const { return bytes_written_; }
    const std::filesystem::path& path() const { return path_; }

private:
    void serialize_video(const VideoFrameData* frame);
    void serialize_video_raw(const VideoFrameData* hdr, const void* data, size_t data_size);

    std::filesystem::path path_;
    std::string           topic_;
    uint64_t              chunk_size_;
    std::string           frame_id_;

    mcap::McapWriter writer_;
    mcap::ChannelId  channel_id_{0};
    bool             opened_{false};
    bool             closed_{false};

    std::vector<std::byte> proto_buf_;

    uint64_t frames_written_{0};
    uint64_t bytes_written_{0};
    uint64_t seq_{0};
};

} // namespace orbbec
