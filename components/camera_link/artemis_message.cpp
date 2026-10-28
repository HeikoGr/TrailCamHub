#include "artemis_message.hpp"

#include <cstring>

#include "artemis_crypto.hpp"
#include "camera_udp_packets.hpp"

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

std::vector<std::uint8_t> build_request(const Aes128 &cipher,
                                        std::uint32_t app_sequence,
                                        const std::string &json)
{
    const std::string body = encode(cipher, json);
    camera::ArtemisCommandRequest request;
    if (!request.build(app_sequence, body.c_str(), body.size()))
        return {};
    return std::vector<std::uint8_t>(request.data(),
                                     request.data() + request.size());
}

bool parse(const Aes128 &cipher, const std::uint8_t *bytes, std::size_t size,
           Message &out)
{
    if (bytes == nullptr || size < kHeaderSize)
        return false;
    if (std::memcmp(bytes, kSignature, sizeof(kSignature)) != 0)
        return false;

    const std::uint32_t payload_len = read_u32_le(bytes + 16);
    if (payload_len == 0 || payload_len > size - kHeaderSize)
        return false;

    const std::string body(reinterpret_cast<const char *>(bytes + kHeaderSize),
                           payload_len);
    std::string json;
    if (!decode(cipher, body, json))
        return false;

    out.msg_type = read_u32_le(bytes + 8);
    out.app_sequence = read_u32_le(bytes + 12);
    out.json = std::move(json);
    return true;
}

}  // namespace camera::artemis
