#pragma once

// ARTEMIS command-channel encoding: AES-128-CBC with a zero IV, the JSON body
// NUL-terminated and zero-padded to the block size, then Base64-encoded. See
// docs/protocol-analysis.md. Pure logic, host-testable.

#include <array>
#include <cstdint>
#include <string>

#include "artemis_aes128.hpp"

namespace camera::artemis
{

// AES-128 command-channel key for this camera family, published at
// https://github.com/pwnb0t/trailcam-automation (src/constants.py).
constexpr std::array<std::uint8_t, 16> kCommandKey = {
    'x', 's', '3', '8', 'n', 'u', 'l', '7',
    'c', 'q', 'f', '7', 'm', '1', 'v', 'a'};

// A cipher initialised with kCommandKey.
Aes128 command_cipher();

// Encodes a JSON command into the command-channel payload: appends a NUL
// terminator, zero-pads to the AES block size, encrypts with AES-128-CBC
// (zero IV) and Base64-encodes the result.
std::string encode(const Aes128 &cipher, const std::string &json);

// Reverses encode(): Base64-decodes, decrypts with AES-128-CBC (zero IV) and
// returns the plaintext up to the NUL terminator. Returns false if the input
// is not valid Base64 or not a whole number of 16-byte blocks.
bool decode(const Aes128 &cipher, const std::string &base64,
            std::string &json_out);

}  // namespace camera::artemis
