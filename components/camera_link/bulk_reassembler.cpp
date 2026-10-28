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

void BulkReassembler::reset()
{
    buffer_.clear();
    seen_.clear();
    received_frames_ = 0;
    needed_frames_ = 0;
    received_bytes_ = 0;
    total_ = 0;
    total_frames_ = 0;
    ver_ = 0;
    typ_ = 0;
    len_ = 0;
    have_header_ = false;
    invalid_ = false;
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

    const std::size_t offset = static_cast<std::size_t>(seq) * kFramePayload;
    const std::size_t need = offset + size;
    if (need > kMaxRecordSize) {
        invalid_ = true;
        return Status::invalid;
    }
    if (buffer_.size() < need) {
        buffer_.resize(need, 0);
    }
    std::memcpy(buffer_.data() + offset, payload, size);

    bool is_new = false;
    if (seen_.size() <= seq) {
        seen_.resize(static_cast<std::size_t>(seq) + 1, false);
    }
    if (!seen_[seq]) {
        seen_[seq] = true;
        is_new = true;
        ++received_frames_;
        received_bytes_ += size;
    }

    bool just_parsed_header = false;
    if (seq == 0 && !have_header_) {
        if (size < 20) {
            return Status::incomplete;  // header not fully present yet
        }
        if (std::memcmp(buffer_.data(), kSignature, sizeof(kSignature)) != 0) {
            invalid_ = true;
            return Status::invalid;
        }
        ver_ = read_u32_le(buffer_.data() + 8);
        typ_ = read_u32_le(buffer_.data() + 12);
        len_ = read_u32_le(buffer_.data() + 16);
        total_ = 20 + static_cast<std::size_t>(len_);
        if (total_ == 0 || total_ > kMaxRecordSize) {
            invalid_ = true;
            return Status::invalid;
        }
        total_frames_ = (total_ + kFramePayload - 1) / kFramePayload;
        have_header_ = true;
        just_parsed_header = true;
        // Count frames already buffered that fall within the record.
        needed_frames_ = 0;
        for (std::size_t i = 0; i < total_frames_ && i < seen_.size(); ++i) {
            if (seen_[i]) {
                ++needed_frames_;
            }
        }
    }

    if (have_header_ && !just_parsed_header && is_new && seq < total_frames_) {
        ++needed_frames_;
    }

    if (have_header_ && needed_frames_ >= total_frames_) {
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
