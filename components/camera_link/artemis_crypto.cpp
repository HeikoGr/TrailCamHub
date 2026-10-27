#include "artemis_crypto.hpp"

#include <cstdint>
#include <vector>

namespace camera::artemis
{
namespace
{

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<std::uint8_t> &in)
{
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= in.size(); i += 3) {
        std::uint32_t n = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
        out.push_back(kB64Alphabet[(n >> 18) & 0x3f]);
        out.push_back(kB64Alphabet[(n >> 12) & 0x3f]);
        out.push_back(kB64Alphabet[(n >> 6) & 0x3f]);
        out.push_back(kB64Alphabet[n & 0x3f]);
    }
    const std::size_t rem = in.size() - i;
    if (rem == 1) {
        std::uint32_t n = in[i] << 16;
        out.push_back(kB64Alphabet[(n >> 18) & 0x3f]);
        out.push_back(kB64Alphabet[(n >> 12) & 0x3f]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        std::uint32_t n = (in[i] << 16) | (in[i + 1] << 8);
        out.push_back(kB64Alphabet[(n >> 18) & 0x3f]);
        out.push_back(kB64Alphabet[(n >> 12) & 0x3f]);
        out.push_back(kB64Alphabet[(n >> 6) & 0x3f]);
        out.push_back('=');
    }
    return out;
}

int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

bool base64_decode(const std::string &in, std::vector<std::uint8_t> &out)
{
    if (in.size() % 4 != 0)
        return false;
    out.clear();
    out.reserve(in.size() / 4 * 3);
    for (std::size_t i = 0; i < in.size(); i += 4) {
        int pad = 0;
        std::uint32_t n = 0;
        for (int j = 0; j < 4; ++j) {
            char c = in[i + j];
            if (c == '=') {
                // Padding is only valid in the last one or two positions.
                if (i + 4 != in.size() || j < 2)
                    return false;
                n <<= 6;
                ++pad;
            } else {
                int v = base64_value(c);
                if (v < 0 || pad != 0)
                    return false;
                n = (n << 6) | static_cast<std::uint32_t>(v);
            }
        }
        out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xff));
        if (pad < 2)
            out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
        if (pad < 1)
            out.push_back(static_cast<std::uint8_t>(n & 0xff));
    }
    return true;
}

}  // namespace

Aes128 command_cipher()
{
    return Aes128(kCommandKey.data());
}

std::string encode(const Aes128 &cipher, const std::string &json)
{
    // Body: JSON + NUL, zero-padded up to a whole number of blocks.
    std::vector<std::uint8_t> buf(json.begin(), json.end());
    buf.push_back(0);
    while (buf.size() % Aes128::kBlockSize != 0)
        buf.push_back(0);

    // AES-128-CBC with a zero IV.
    std::uint8_t prev[Aes128::kBlockSize] = {0};
    for (std::size_t off = 0; off < buf.size(); off += Aes128::kBlockSize) {
        std::uint8_t block[Aes128::kBlockSize];
        for (std::size_t k = 0; k < Aes128::kBlockSize; ++k)
            block[k] = buf[off + k] ^ prev[k];
        cipher.encrypt_block(block, &buf[off]);
        for (std::size_t k = 0; k < Aes128::kBlockSize; ++k)
            prev[k] = buf[off + k];
    }
    return base64_encode(buf);
}

bool decode(const Aes128 &cipher, const std::string &base64,
            std::string &json_out)
{
    std::vector<std::uint8_t> buf;
    if (!base64_decode(base64, buf))
        return false;
    if (buf.empty() || buf.size() % Aes128::kBlockSize != 0)
        return false;

    std::uint8_t prev[Aes128::kBlockSize] = {0};
    for (std::size_t off = 0; off < buf.size(); off += Aes128::kBlockSize) {
        std::uint8_t cblock[Aes128::kBlockSize];
        for (std::size_t k = 0; k < Aes128::kBlockSize; ++k)
            cblock[k] = buf[off + k];
        std::uint8_t plain[Aes128::kBlockSize];
        cipher.decrypt_block(cblock, plain);
        for (std::size_t k = 0; k < Aes128::kBlockSize; ++k)
            buf[off + k] = plain[k] ^ prev[k];
        for (std::size_t k = 0; k < Aes128::kBlockSize; ++k)
            prev[k] = cblock[k];
    }

    // Plaintext is the JSON up to the NUL terminator.
    std::size_t end = 0;
    while (end < buf.size() && buf[end] != 0)
        ++end;
    json_out.assign(buf.begin(), buf.begin() + end);
    return true;
}

}  // namespace camera::artemis
