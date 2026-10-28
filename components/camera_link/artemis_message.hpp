#pragma once

// ARTEMIS application-layer framing: a 20-byte header
// ("ARTEMIS\0" + MsgType + AppSeq + PayloadLen, little-endian) followed by a
// NUL-terminated Base64 string that carries the encrypted JSON body. This
// module ties the header to the command-channel crypto (artemis_crypto) so
// callers work with plain JSON. Pure logic, host-testable. The resulting
// bytes are carried as the payload of an RUDP DATA packet (pppp::Session).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "artemis_aes128.hpp"

namespace camera::artemis
{

constexpr std::uint32_t kMsgRequest = 2;
constexpr std::uint32_t kMsgResponse = 3;

struct Message {
    std::uint32_t msg_type = 0;      // kMsgRequest / kMsgResponse
    std::uint32_t app_sequence = 0;  // application sequence counter
    std::string json;                // decrypted JSON body
};

// Builds a request message (MsgType 2) for the given JSON command and
// application sequence: ARTEMIS header + Base64(AES-128-CBC(JSON)) + NUL.
// Returns an empty vector if the encoded body does not fit a single packet.
std::vector<std::uint8_t> build_request(const Aes128 &cipher,
                                        std::uint32_t app_sequence,
                                        const std::string &json);

// Parses an ARTEMIS message carried in an RUDP DATA payload. Returns false if
// the signature or length fields are invalid or the body cannot be decoded.
bool parse(const Aes128 &cipher, const std::uint8_t *bytes, std::size_t size,
           Message &out);

}  // namespace camera::artemis
