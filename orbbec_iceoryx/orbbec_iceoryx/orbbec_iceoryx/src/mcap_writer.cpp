#define MCAP_IMPLEMENTATION
#include "mcap_writer.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>

#include "lflogger.hpp"

namespace orbbec {

namespace {

inline uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

// ── Minimal CDR writer (ROS 2 / DDS, little-endian) ────────────────────────
// ROS 2 uses CDR with these rules:
//   - Each primitive is aligned to its own size, relative to the start of the
//     CDR body (i.e. AFTER the 4-byte encapsulation header).
//   - Strings: uint32 length (includes the NUL terminator) + bytes + NUL.
//   - Sequences (uint8[] data): uint32 length (element count) + raw bytes.
// We track alignment from the start of the body, which begins right after the
// 4-byte encapsulation header, so the "origin" for alignment is offset 4.
class CdrWriter {
public:
    explicit CdrWriter(std::vector<std::byte>& buf) : buf_(buf) {
        buf_.clear();
        // Encapsulation header: 0x00 0x01 = CDR little-endian, then 2 bytes options.
        const std::byte hdr[4] = {std::byte{0x00}, std::byte{0x01},
                                  std::byte{0x00}, std::byte{0x00}};
        buf_.insert(buf_.end(), hdr, hdr + 4);
        // Alignment origin is the byte right after the encapsulation header.
        origin_ = buf_.size(); // = 4
    }

    void u8(uint8_t v) {
        align(1);
        buf_.push_back(std::byte{v});
    }

    void u32(uint32_t v) {
        align(4);
        appendRaw(&v, sizeof(v));
    }

    void i32(int32_t v) {
        align(4);
        appendRaw(&v, sizeof(v));
    }

    void str(const char* s) {
        const uint32_t len = static_cast<uint32_t>(std::strlen(s)) + 1U; // incl NUL
        u32(len);
        appendRaw(s, len); // copies the trailing NUL too
    }

    // uint8[] sequence: count prefix + raw bytes.
    void bytes(const void* p, uint32_t count) {
        u32(count);
        if (count > 0)
            appendRaw(p, count);
    }

private:
    void align(std::size_t a) {
        const std::size_t pos = buf_.size() - origin_;
        const std::size_t pad = (a - (pos % a)) % a;
        for (std::size_t i = 0; i < pad; ++i)
            buf_.push_back(std::byte{0});
    }

    void appendRaw(const void* p, std::size_t n) {
        const auto* b = reinterpret_cast<const std::byte*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }

    std::vector<std::byte>& buf_;
    std::size_t origin_{0};
};

} // namespace

McapWriter::McapWriter(std::filesystem::path output_path,
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

McapWriter::~McapWriter() { close(); }

void McapWriter::open() {
    if (opened_)
        return;

    mcap::McapWriterOptions opts("ros2");
    opts.chunkSize = chunk_size_;
    opts.compression = mcap::Compression::None;

    const auto st = writer_.open(path_.string(), opts);
    if (!st.ok())
        throw std::runtime_error("[McapWriter] Failed to open '" +
                                 path_.string() + "': " + st.message);

    // Schema + channel use ros2msg / cdr encoding so the file is a valid rosbag2.
    mcap::Schema schema(kSchemaName, "ros2msg", kSchemaDef);
    writer_.addSchema(schema);

    mcap::Channel channel(topic_, "cdr", schema.id);
    writer_.addChannel(channel);
    channel_id_ = channel.id;

    opened_ = true;
    LockFreeLogger::getInstance().info("McapWriter",
        fmt::format("Opened: {}  topic={}  schema=sensor_msgs/msg/Image  encoding={}",
                    path_.string(), topic_, kImageEncoding));
}

void McapWriter::serialize_image(const FrameData* frame) {
    CdrWriter cdr(cdr_buf_);

    // std_msgs/Header
    //   builtin_interfaces/Time stamp { int32 sec; uint32 nanosec; }
    const int32_t  sec     = static_cast<int32_t>(frame->timestamp_ns / 1000000000ULL);
    const uint32_t nanosec = static_cast<uint32_t>(frame->timestamp_ns % 1000000000ULL);
    cdr.i32(sec);
    cdr.u32(nanosec);
    //   string frame_id
    cdr.str(frame_id_.c_str());

    // uint32 height, uint32 width
    cdr.u32(frame->height);
    cdr.u32(frame->width);

    // string encoding
    cdr.str(kImageEncoding);

    // uint8 is_bigendian
    cdr.u8(0);

    // uint32 step — bytes per row. BayerRG8 is 1 byte/px, so step = width.
    cdr.u32(frame->width);

    // uint8[] data
    cdr.bytes(frame->data, static_cast<uint32_t>(frame->data_size));
}

void McapWriter::write_frame(const FrameData* frame, std::size_t /*payload_bytes*/) {
    if (!frame || !is_open())
        return;

    serialize_image(frame);

    mcap::Message msg;
    msg.channelId   = channel_id_;
    msg.sequence    = static_cast<uint32_t>(seq_++);
    msg.logTime     = now_ns();
    msg.publishTime = frame->timestamp_ns;
    msg.data        = cdr_buf_.data();
    msg.dataSize    = cdr_buf_.size();

    const auto ws = writer_.write(msg);
    if (!ws.ok()) {
        ++errors_;
        LockFreeLogger::getInstance().error("McapWriter",
            fmt::format("write error (frame {}): {}", seq_ - 1, ws.message));
    } else {
        ++frames_written_;
        bytes_written_ += cdr_buf_.size();
    }
}

void McapWriter::close() {
    if (!opened_ || closed_)
        return;
    writer_.close();
    closed_ = true;
    LockFreeLogger::getInstance().info("McapWriter",
        fmt::format("Closed: {}  frames={}  bytes={}  errors={}",
                    path_.string(), frames_written_, bytes_written_, errors_));
}

} // namespace orbbec
