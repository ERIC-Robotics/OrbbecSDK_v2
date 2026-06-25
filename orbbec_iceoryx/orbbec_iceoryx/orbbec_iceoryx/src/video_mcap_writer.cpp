#define MCAP_IMPLEMENTATION
#include "video_mcap_writer.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>

#include "lflogger.hpp"

namespace orbbec {

// ── Minimal protobuf varint helpers ─────────────────────────────────────────
namespace proto {

static void write_varint(std::vector<std::byte>& buf, uint64_t v) {
    while (v > 0x7F) {
        buf.push_back(std::byte((v & 0x7F) | 0x80));
        v >>= 7;
    }
    buf.push_back(std::byte(v));
}

// Write a protobuf field tag: (field_number << 3) | wire_type
// wire_type: 0=varint, 2=length-delimited
static void write_tag(std::vector<std::byte>& buf, int field, int wire) {
    write_varint(buf, static_cast<uint64_t>((field << 3) | wire));
}

static void write_bytes_field(std::vector<std::byte>& buf, int field,
                               const void* data, size_t len) {
    write_tag(buf, field, 2);
    write_varint(buf, len);
    const auto* p = reinterpret_cast<const std::byte*>(data);
    buf.insert(buf.end(), p, p + len);
}

static void write_string_field(std::vector<std::byte>& buf, int field,
                                const std::string& s) {
    write_bytes_field(buf, field, s.data(), s.size());
}

static void write_int64_field(std::vector<std::byte>& buf, int field, int64_t v) {
    write_tag(buf, field, 0);
    write_varint(buf, static_cast<uint64_t>(v));
}

static void write_int32_field(std::vector<std::byte>& buf, int field, int32_t v) {
    write_tag(buf, field, 0);
    write_varint(buf, static_cast<uint64_t>(static_cast<uint32_t>(v)));
}

} // namespace proto

// ── foxglove.CompressedImage FileDescriptorSet ───────────────────────────────
// Binary FileDescriptorSet describing foxglove.CompressedImage using the
// OFFICIAL field numbers from foxglove-schemas:
//
//   syntax = "proto3";
//   package foxglove;
//   import "google/protobuf/timestamp.proto";
//   message CompressedImage {
//     google.protobuf.Timestamp timestamp = 1;  // capture time
//     string frame_id                    = 2;  // coordinate frame
//     bytes  data                        = 3;  // compressed image bytes
//     string format                      = 4;  // "jpeg", "png", etc.
//   }
//
// Generated with protobuf Python library and verified by round-trip decode.
// IMPORTANT: field numbers MUST match the serializer below or Foxglove will
// decode data/frame_id into the wrong fields and show no image.
static const uint8_t kCompressedImageFDS[] = {
    0x0a,0x67,0x0a,0x1f,0x67,0x6f,0x6f,0x67,0x6c,0x65,0x2f,0x70,0x72,0x6f,0x74,0x6f,
    0x62,0x75,0x66,0x2f,0x74,0x69,0x6d,0x65,0x73,0x74,0x61,0x6d,0x70,0x2e,0x70,0x72,
    0x6f,0x74,0x6f,0x12,0x0f,0x67,0x6f,0x6f,0x67,0x6c,0x65,0x2e,0x70,0x72,0x6f,0x74,
    0x6f,0x62,0x75,0x66,0x22,0x2b,0x0a,0x09,0x54,0x69,0x6d,0x65,0x73,0x74,0x61,0x6d,
    0x70,0x12,0x0f,0x0a,0x07,0x73,0x65,0x63,0x6f,0x6e,0x64,0x73,0x18,0x01,0x20,0x01,
    0x28,0x03,0x12,0x0d,0x0a,0x05,0x6e,0x61,0x6e,0x6f,0x73,0x18,0x02,0x20,0x01,0x28,
    0x05,0x62,0x06,0x70,0x72,0x6f,0x74,0x6f,0x33,0x0a,0xc5,0x01,0x0a,0x1e,0x66,0x6f,
    0x78,0x67,0x6c,0x6f,0x76,0x65,0x2f,0x43,0x6f,0x6d,0x70,0x72,0x65,0x73,0x73,0x65,
    0x64,0x49,0x6d,0x61,0x67,0x65,0x2e,0x70,0x72,0x6f,0x74,0x6f,0x12,0x08,0x66,0x6f,
    0x78,0x67,0x6c,0x6f,0x76,0x65,0x1a,0x1f,0x67,0x6f,0x6f,0x67,0x6c,0x65,0x2f,0x70,
    0x72,0x6f,0x74,0x6f,0x62,0x75,0x66,0x2f,0x74,0x69,0x6d,0x65,0x73,0x74,0x61,0x6d,
    0x70,0x2e,0x70,0x72,0x6f,0x74,0x6f,0x22,0x70,0x0a,0x0f,0x43,0x6f,0x6d,0x70,0x72,
    0x65,0x73,0x73,0x65,0x64,0x49,0x6d,0x61,0x67,0x65,0x12,0x2d,0x0a,0x09,0x74,0x69,
    0x6d,0x65,0x73,0x74,0x61,0x6d,0x70,0x18,0x01,0x20,0x01,0x28,0x0b,0x32,0x1a,0x2e,
    0x67,0x6f,0x6f,0x67,0x6c,0x65,0x2e,0x70,0x72,0x6f,0x74,0x6f,0x62,0x75,0x66,0x2e,
    0x54,0x69,0x6d,0x65,0x73,0x74,0x61,0x6d,0x70,0x12,0x10,0x0a,0x08,0x66,0x72,0x61,
    0x6d,0x65,0x5f,0x69,0x64,0x18,0x02,0x20,0x01,0x28,0x09,0x12,0x0c,0x0a,0x04,0x64,
    0x61,0x74,0x61,0x18,0x03,0x20,0x01,0x28,0x0c,0x12,0x0e,0x0a,0x06,0x66,0x6f,0x72,
    0x6d,0x61,0x74,0x18,0x04,0x20,0x01,0x28,0x09,0x62,0x06,0x70,0x72,0x6f,0x74,0x6f,
    0x33
};

static const size_t kCompressedImageFDS_len = sizeof(kCompressedImageFDS);

// ── Implementation ───────────────────────────────────────────────────────────

VideoMcapWriter::VideoMcapWriter(std::filesystem::path output_path,
                                 std::string           topic,
                                 uint64_t              chunk_size,
                                 std::string           frame_id)
    : path_(std::move(output_path)),
      topic_(std::move(topic)),
      chunk_size_(chunk_size),
      frame_id_(std::move(frame_id)) {
    if (frame_id_.empty())
        frame_id_ = topic_;
}

VideoMcapWriter::~VideoMcapWriter() { close(); }

void VideoMcapWriter::open() {
    if (opened_) return;

    mcap::McapWriterOptions opts("protobuf");
    opts.chunkSize   = chunk_size_;
    opts.compression = mcap::Compression::None;

    const auto st = writer_.open(path_.string(), opts);
    if (!st.ok())
        throw std::runtime_error("[VideoMcapWriter] Failed to open '" +
                                 path_.string() + "': " + st.message);

    // Schema: foxglove.CompressedImage (protobuf FileDescriptorSet)
    std::vector<std::byte> fds_bytes(kCompressedImageFDS_len);
    std::memcpy(fds_bytes.data(), kCompressedImageFDS, kCompressedImageFDS_len);

    mcap::Schema schema("foxglove.CompressedImage", "protobuf", fds_bytes);
    writer_.addSchema(schema);

    mcap::Channel channel(topic_, "protobuf", schema.id);
    writer_.addChannel(channel);
    channel_id_ = channel.id;

    opened_ = true;
    LockFreeLogger::getInstance().info("VideoMcapWriter",
        fmt::format("Opened: {}  topic={}  schema=foxglove.CompressedImage  format=jpeg",
                    path_.string(), topic_));
}

// Serialise a VideoFrameData into a foxglove.CompressedImage protobuf message.
// We hand-write the protobuf binary to avoid any generated-code dependency.
void VideoMcapWriter::serialize_video(const VideoFrameData* frame) {
    proto_buf_.clear();

    // field 1: google.protobuf.Timestamp timestamp (message, wire=2)
    {
        std::vector<std::byte> ts_buf;
        int64_t secs  = static_cast<int64_t>(frame->timestamp_ns / 1'000'000'000ULL);
        int32_t nanos = static_cast<int32_t>(frame->timestamp_ns % 1'000'000'000ULL);
        proto::write_int64_field(ts_buf, 1, secs);
        proto::write_int32_field(ts_buf, 2, nanos);
        proto::write_bytes_field(proto_buf_, 1, ts_buf.data(), ts_buf.size());
    }

    // field 2: string frame_id  (official foxglove field ordering)
    proto::write_string_field(proto_buf_, 2, frame_id_);

    // field 3: bytes data (JPEG frame)
    proto::write_bytes_field(proto_buf_, 3, frame->data,
                             static_cast<size_t>(frame->data_size));

    // field 4: string format = "jpeg"
    proto::write_string_field(proto_buf_, 4, "jpeg");
}

void VideoMcapWriter::write_frame(const VideoFrameData* frame) {
    if (!frame || !is_open()) return;

    serialize_video(frame);

    mcap::Message msg;
    msg.channelId   = channel_id_;
    msg.sequence    = static_cast<uint32_t>(seq_++);
    msg.logTime     = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    msg.publishTime = frame->timestamp_ns;
    msg.data        = proto_buf_.data();
    msg.dataSize    = proto_buf_.size();

    const auto ws = writer_.write(msg);
    if (!ws.ok()) {
        LockFreeLogger::getInstance().error("VideoMcapWriter",
            fmt::format("write error (frame {}): {}", seq_ - 1, ws.message));
    } else {
        ++frames_written_;
        bytes_written_ += proto_buf_.size();
    }
}

void VideoMcapWriter::write_frame_raw(const VideoFrameData* hdr,
                                       const void* data, size_t data_size) {
    if (!hdr || !is_open()) return;

    proto_buf_.clear();

    // field 1: google.protobuf.Timestamp
    {
        std::vector<std::byte> ts_buf;
        int64_t secs  = static_cast<int64_t>(hdr->timestamp_ns / 1'000'000'000ULL);
        int32_t nanos = static_cast<int32_t>(hdr->timestamp_ns % 1'000'000'000ULL);
        proto::write_int64_field(ts_buf, 1, secs);
        proto::write_int32_field(ts_buf, 2, nanos);
        proto::write_bytes_field(proto_buf_, 1, ts_buf.data(), ts_buf.size());
    }
    // field 2: string frame_id  (official foxglove field ordering)
    proto::write_string_field(proto_buf_, 2, frame_id_);
    // field 3: bytes data (JPEG frame — direct from GstBuffer map)
    proto::write_bytes_field(proto_buf_, 3, data, data_size);
    // field 4: string format
    proto::write_string_field(proto_buf_, 4, "jpeg");

    mcap::Message msg;
    msg.channelId   = channel_id_;
    msg.sequence    = static_cast<uint32_t>(seq_++);
    msg.logTime     = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    msg.publishTime = hdr->timestamp_ns;
    msg.data        = proto_buf_.data();
    msg.dataSize    = proto_buf_.size();

    const auto ws = writer_.write(msg);
    if (!ws.ok()) {
        LockFreeLogger::getInstance().error("VideoMcapWriter",
            fmt::format("write_frame_raw error: {}", ws.message));
    } else {
        ++frames_written_;
        bytes_written_ += proto_buf_.size();
    }
}

void VideoMcapWriter::close() {
    if (!opened_ || closed_) return;
    writer_.close();
    closed_ = true;
    LockFreeLogger::getInstance().info("VideoMcapWriter",
        fmt::format("Closed: {}  frames={}  bytes={}",
                    path_.string(), frames_written_, bytes_written_));
}

} // namespace orbbec
