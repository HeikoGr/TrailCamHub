#include "bulk_reassembler.hpp"

#include <cstring>

namespace camera::artemis
{

namespace
{

constexpr std::uint8_t kSignature[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

const char *BulkReassembler::reason_name(InvalidReason reason)
{
    switch (reason) {
    case InvalidReason::none:
        return "none";
    case InvalidReason::short_header_frame:
        return "short header frame";
    case InvalidReason::bad_signature:
        return "bad signature";
    case InvalidReason::bad_length:
        return "bad length";
    }
    return "unknown";
}

void BulkReassembler::reset()
{
    buffer_.clear();
    seen_.clear();
    received_frames_ = 0;
    received_bytes_ = 0;
    total_ = 0;
    total_frames_ = 0;
    ver_ = 0;
    typ_ = 0;
    len_ = 0;
    base_seq_ = 0;
    have_header_ = false;
    invalid_ = false;
    invalid_reason_ = InvalidReason::none;
}

void BulkReassembler::place_frame(std::uint16_t seq,
                                  const std::uint8_t *payload, std::size_t size)
{
    // The camera's bulk sequence is a running counter, not record-relative, so
    // address each frame by its distance from the header frame's sequence.
    const std::uint16_t rel = static_cast<std::uint16_t>(seq - base_seq_);
    if (rel >= total_frames_) {
        return;  // stale, wrapped, or belongs to another record
    }
    const std::size_t offset = static_cast<std::size_t>(rel) * kFramePayload;
    if (buffer_.size() < offset + size) {
        buffer_.resize(offset + size, 0);
    }
    std::memcpy(buffer_.data() + offset, payload, size);
    if (!seen_[rel]) {
        seen_[rel] = true;
        ++received_frames_;
        received_bytes_ += size;
    }
}

BulkReassembler::Status BulkReassembler::feed(std::uint16_t seq,
                                              const std::uint8_t *payload,
                                              std::size_t size)
{
    if (invalid_) {
        return Status::invalid;
    }
    if (payload == nullptr || size == 0) {
        return Status::incomplete;
    }

    if (!have_header_) {
        // The first frame of a record carries the 20-byte ARTEMIS header and
        // sets the base sequence; anything else here is not a record start.
        if (size < 20) {
            invalid_ = true;
            invalid_reason_ = InvalidReason::short_header_frame;
            return Status::invalid;
        }
        if (std::memcmp(payload, kSignature, sizeof(kSignature)) != 0) {
            invalid_ = true;
            invalid_reason_ = InvalidReason::bad_signature;
            return Status::invalid;
        }
        ver_ = read_u32_le(payload + 8);
        typ_ = read_u32_le(payload + 12);
        len_ = read_u32_le(payload + 16);
        total_ = 20 + static_cast<std::size_t>(len_);
        if (total_ == 0 || total_ > kMaxRecordSize) {
            invalid_ = true;
            invalid_reason_ = InvalidReason::bad_length;
            return Status::invalid;
        }
        total_frames_ = (total_ + kFramePayload - 1) / kFramePayload;
        base_seq_ = seq;
        have_header_ = true;
        seen_.assign(total_frames_, false);
    }

    place_frame(seq, payload, size);

    if (received_frames_ >= total_frames_) {
        buffer_.resize(total_);  // trim any over-allocation
        return Status::complete;
    }
    return Status::incomplete;
}

const std::uint8_t *BulkReassembler::payload() const
{
    if (buffer_.size() < 20) {
        return nullptr;
    }
    return buffer_.data() + 20;
}

}  // namespace camera::artemis
