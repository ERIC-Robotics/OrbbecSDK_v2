#pragma once

#include "orbbec_data.hpp"

#include <mcap/writer.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace orbbec {

// ── ROS 2 sensor_msgs/Image schema ─────────────────────────────────────────
// Writing frames as sensor_msgs/msg/Image with CDR encoding makes the MCAP a
// first-class rosbag2 file: directly playable in Foxglove Studio, ros2 bag
// play, rqt, etc. The pixel data is stored raw (no debayer, no compression),
// so there is no extra capture-time cost beyond CDR serialization, and zero
// image loss. Foxglove debayers "bayer_rggb8" on display automatically.
//
// The schema text is the concatenation of the message definitions, in the
// format MCAP/rosbag2 expect for the "ros2msg" encoding (msg def, then each
// dependency separated by the standard '=== MSG: ...' delimiter).

inline constexpr const char kSchemaName[] = "sensor_msgs/msg/Image";

inline constexpr const char kSchemaDef[] =
    "std_msgs/Header header\n"
    "uint32 height\n"
    "uint32 width\n"
    "string encoding\n"
    "uint8 is_bigendian\n"
    "uint32 step\n"
    "uint8[] data\n"
    "================================================================================\n"
    "MSG: std_msgs/Header\n"
    "builtin_interfaces/Time stamp\n"
    "string frame_id\n"
    "================================================================================\n"
    "MSG: builtin_interfaces/Time\n"
    "int32 sec\n"
    "uint32 nanosec\n";

// Pixel encoding string for raw frames. Adjust if your V4L2 device outputs
// a different format (e.g. "yuv422" for YUYV, "jpeg" for MJPEG).
inline constexpr const char kImageEncoding[] = "bayer_rggb8";

class McapWriter {
public:
    // topic: e.g. "Orbbec/<name>/Frame"
    // frame_id: value placed in the ROS Header (defaults to topic if empty)
    explicit McapWriter(std::filesystem::path output_path,
                        std::string           topic      = "Orbbec/Camera/Frame",
                        uint64_t              chunk_size = 64 * 1024 * 1024,
                        std::string           frame_id   = "");

    ~McapWriter();

    // Opens the file and registers schema + channel. Throws on failure.
    void open();

    // Writes one FrameData as a sensor_msgs/Image MCAP message.
    // payload_bytes is accepted for interface compatibility but the actual
    // serialized size is computed internally. Computed from struct if 0.
    void write_frame(const FrameData* frame, std::size_t payload_bytes = 0);

    // Flushes and closes. Idempotent.
    void close();

    bool         is_open()       const { return opened_ && !closed_; }
    uint64_t     frames_written()const { return frames_written_; }
    uint64_t     bytes_written() const { return bytes_written_; }
    uint64_t     errors()        const { return errors_; }
    const std::filesystem::path& path() const { return path_; }

private:
    // Serializes `frame` into ROS 2 CDR sensor_msgs/Image bytes in cdr_buf_.
    void serialize_image(const FrameData* frame);

    std::filesystem::path path_;
    std::string           topic_;
    uint64_t              chunk_size_;
    std::string           frame_id_;

    mcap::McapWriter  writer_;
    mcap::ChannelId   channel_id_{0};
    bool              opened_{false};
    bool              closed_{false};

    // Reusable serialization buffer — avoids per-frame allocation in the hot path.
    std::vector<std::byte> cdr_buf_;

    uint64_t frames_written_{0};
    uint64_t bytes_written_{0};
    uint64_t errors_{0};
    uint64_t seq_{0};
};

} // namespace orbbec
