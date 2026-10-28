#include "artemis_reassembler.hpp"

#include <cstring>

namespace camera::artemis
{
namespace
{

constexpr std::size_t kHeaderSize = 20;
constexpr std::uint8_t kSignature[8] = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};

std::uint32_t read_u32_le(const std::uint8_t *p)
{
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace

void Reassembler::reset()
{
    buffer_.clear();
}

Reassembler::Status Reassembler::feed(const std::uint8_t *payload,
                                      std::size_t size)
{
    if (payload != nullptr && size > 0) {
        buffer_.insert(buffer_.end(), payload, payload + size);
    }

    // Need the header before the total length is known.
    if (buffer_.size() < kHeaderSize)
        return Status::incomplete;

    if (std::memcmp(buffer_.data(), kSignature, sizeof(kSignature)) != 0) {
        buffer_.clear();
        return Status::invalid;
    }

    const std::uint32_t payload_len = read_u32_le(buffer_.data() + 16);
    if (buffer_.size() < kHeaderSize + payload_len)
        return Status::incomplete;

    return Status::complete;
}

}  // namespace camera::artemis
