#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "artemis_aes128.hpp"
#include "camera_prelogin.hpp"

using namespace camera;

namespace
{

const std::uint8_t kCapturedNonce[kPreLoginNonceSize] = {
    0xaa, 0xb2, 0xea, 0x38, 0xba, 0x65, 0x47, 0x6d};

}  // namespace

TEST_CASE("builds the PRE_LOGIN plaintext observed on hardware")
{
    // A PRE_LOGIN captured on hardware decrypts (AES-128-ECB) to exactly
    // this JSON followed by seven bytes of PKCS#7 padding.
    const std::uint8_t nonce[kPreLoginNonceSize] = {0xaa, 0xb2, 0xea, 0x38,
                                                    0xba, 0x65, 0x47, 0x6d};
    std::uint8_t out[96];
    std::size_t json_len = 0;
    const std::size_t padded =
        build_prelogin_plaintext(39, nonce, out, sizeof(out), &json_len);
    const std::string expected =
        "{\"utcTime\":39,\"nonce\":\"aab2ea38ba65476d\"}";
    REQUIRE(json_len == expected.size());
    CHECK(std::string(reinterpret_cast<char *>(out), json_len) == expected);
    REQUIRE(padded == 48);
    for (std::size_t i = json_len; i < padded; i++) {
        CHECK(out[i] == 7);
    }
}

TEST_CASE("PKCS#7 adds a full block to aligned data")
{
    std::uint8_t data[32] = {};
    CHECK(pkcs7_pad(data, 16, sizeof(data)) == 32);
    CHECK(data[16] == 16);
    CHECK(data[31] == 16);
}

TEST_CASE("PKCS#7 and plaintext builder respect the capacity")
{
    std::uint8_t data[16] = {};
    CHECK(pkcs7_pad(data, 16, sizeof(data)) == 0);

    const std::uint8_t nonce[kPreLoginNonceSize] = {};
    std::uint8_t small[40];
    CHECK(build_prelogin_plaintext(1700000000, nonce, small, sizeof(small),
                                   nullptr) == 0);
}

TEST_CASE("builds the encrypted PRE_LOGIN payload")
{
    // The header from the captures, then the plaintext of the first test
    // case encrypted with AES-128-ECB under the key "a01bc23ed45fF56A"; the
    // ciphertext was computed independently with
    //   openssl enc -aes-128-ecb -K 61303162633233656434356646353641 -nopad
    // clang-format off
    const std::uint8_t expected[] = {
        0x0c, 0xcb, 0x9a, 0x2b, 0x5f, 0x95, 0x1e, 0xb6,
        0x69, 0xdf, 0xaa, 0x37, 0x5a, 0x6b, 0xbe, 0x3e,
        0x76, 0x20, 0x2e, 0x13, 0xc9, 0xd1, 0xaa, 0x36,
        0x31, 0xbe, 0x74, 0xe5,
        0x27, 0x4c, 0x21, 0x2e, 0x6a, 0x79, 0xea, 0xd1,
        0x65, 0x08, 0xc5, 0x03, 0x10, 0xe0, 0x25, 0x64,
        0x42, 0x97, 0x43, 0x65, 0xe2, 0x24, 0x78, 0x5a,
        0x6a, 0x31, 0x28, 0xaa, 0x48, 0x7a, 0x4b, 0x09,
        0xe4, 0xf3, 0x31, 0x37, 0x00, 0x3c, 0x7b, 0x6c,
        0xa7, 0x34, 0xc7, 0x02, 0xa0, 0xe1, 0x0e, 0x88,
    };
    // clang-format on
    std::uint8_t body[kPreLoginMaxBodySize];
    std::size_t json_len = 0;
    const std::size_t size =
        build_prelogin_body(39, kCapturedNonce, body, sizeof(body), &json_len);
    REQUIRE(size == sizeof(expected));
    CHECK(json_len == 41);
    CHECK(std::memcmp(body, expected, sizeof(expected)) == 0);
}

TEST_CASE("the PRE_LOGIN payload decrypts to the plaintext")
{
    std::uint8_t body[kPreLoginMaxBodySize];
    const std::size_t size = build_prelogin_body(1793727344, kCapturedNonce,
                                                 body, sizeof(body), nullptr);
    REQUIRE(size > kPreLoginHeaderSize);
    REQUIRE((size - kPreLoginHeaderSize) % kAesBlockSize == 0);

    std::uint8_t plaintext[kPreLoginMaxBodySize];
    std::size_t json_len = 0;
    const std::size_t padded = build_prelogin_plaintext(
        1793727344, kCapturedNonce, plaintext, sizeof(plaintext), &json_len);
    REQUIRE(padded == size - kPreLoginHeaderSize);

    const std::uint8_t key[] = {'a', '0', '1', 'b', 'c', '2', '3', 'e',
                                'd', '4', '5', 'f', 'F', '5', '6', 'A'};
    const camera::artemis::Aes128 cipher(key);
    for (std::size_t offset = 0; offset < padded; offset += kAesBlockSize) {
        std::uint8_t block[kAesBlockSize];
        cipher.decrypt_block(body + kPreLoginHeaderSize + offset, block);
        CHECK(std::memcmp(block, plaintext + offset, kAesBlockSize) == 0);
    }
}

TEST_CASE("the PRE_LOGIN payload respects the capacity")
{
    std::uint8_t body[kPreLoginMaxBodySize];
    // The header and one block fit, but this plaintext needs three blocks.
    CHECK(build_prelogin_body(39, kCapturedNonce, body,
                              kPreLoginHeaderSize + kAesBlockSize,
                              nullptr) == 0);
    CHECK(build_prelogin_body(39, kCapturedNonce, body, 4, nullptr) == 0);
    CHECK(build_prelogin_body(39, kCapturedNonce, nullptr, sizeof(body),
                              nullptr) == 0);
    CHECK(build_prelogin_body(39, nullptr, body, sizeof(body), nullptr) == 0);
}
