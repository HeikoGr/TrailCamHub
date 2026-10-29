#include "playback_stream.hpp"

#include <cstring>

namespace camera::artemis
{

namespace
{

constexpr std::uint8_t kSignature[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
constexpr std::size_t kHeaderSize = 20;

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

PlaybackStream::PlaybackStream(std::size_t reorder_frames,
                               std::size_t max_record_size)
    : reorder_(reorder_frames), max_record_size_(max_record_size)
{
}

void PlaybackStream::reset()
{
    reorder_.reset();
    record_.clear();
    record_size_ = 0;
    records_ = 0;
    failed_ = false;
}

bool PlaybackStream::consume(const std::uint8_t *data, std::size_t size,
                             const RecordSink &sink)
{
    record_.insert(record_.end(), data, data + size);
    while (true) {
        if (record_size_ == 0) {
            if (record_.size() < kHeaderSize) {
                return true;
            }
            if (std::memcmp(record_.data(), kSignature, sizeof(kSignature)) !=
                0) {
                return false;
            }
            const std::uint32_t length = read_u32_le(record_.data() + 16);
            if (length == 0 || length > max_record_size_) {
                return false;
            }
            record_size_ = kHeaderSize + length;
        }
        if (record_.size() < record_size_) {
            return true;
        }
        if (!sink(read_u32_le(record_.data() + 8),
                  read_u32_le(record_.data() + 12),
                  record_.data() + kHeaderSize, record_size_ - kHeaderSize)) {
            return false;
        }
        ++records_;
        // Usually only a short tail of the last frame is left to move.
        record_.erase(record_.begin(), record_.begin() + record_size_);
        record_size_ = 0;
    }
}

PlaybackStream::Status PlaybackStream::feed(std::uint16_t seq,
                                            const std::uint8_t *payload,
                                            std::size_t size,
                                            const RecordSink &sink, bool &ack)
{
    ack = false;
    if (failed_) {
        return Status::invalid;
    }
    if (payload == nullptr || size == 0) {
        return Status::ok;
    }
    if (!reorder_.started()) {
        if (size < kHeaderSize ||
            std::memcmp(payload, kSignature, sizeof(kSignature)) != 0) {
            return Status::ok;  // not the first frame; it comes again
        }
        reorder_.start(seq);
    }
    const FrameReorder::Outcome outcome =
        reorder_.feed(seq, payload, size,
                      [this, &sink](std::uint64_t, const std::uint8_t *frame,
                                    std::size_t frame_size) {
                          return consume(frame, frame_size, sink);
                      });
    if (outcome == FrameReorder::Outcome::failed) {
        failed_ = true;
        return Status::invalid;
    }
    ack = FrameReorder::acknowledge(outcome);
    return Status::ok;
}

}  // namespace camera::artemis
