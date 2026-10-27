#pragma once

// Minimal, self-contained AES-128 block cipher (no ESP-IDF or mbedTLS
// dependency) so the ARTEMIS command-channel logic stays host-testable and
// identical on host and device. Only the raw 16-byte block operations live
// here; chaining, padding and Base64 are in artemis_crypto.
//
// If binary size ever matters more than a shared code path, this class can be
// reimplemented on top of the mbedTLS AES already linked into the firmware,
// without changing artemis_crypto.

#include <cstdint>

namespace camera::artemis
{

class Aes128
{
  public:
    static constexpr std::size_t kBlockSize = 16;
    static constexpr std::size_t kKeySize = 16;

    explicit Aes128(const std::uint8_t key[kKeySize]);

    void encrypt_block(const std::uint8_t in[kBlockSize],
                       std::uint8_t out[kBlockSize]) const;
    void decrypt_block(const std::uint8_t in[kBlockSize],
                       std::uint8_t out[kBlockSize]) const;

  private:
    std::uint8_t round_key_[176];
};

}  // namespace camera::artemis
