#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "artemis_aes128.hpp"
#include "artemis_crypto.hpp"

using namespace camera::artemis;

namespace
{

std::vector<std::uint8_t> bytes(std::initializer_list<int> v)
{
    std::vector<std::uint8_t> out;
    for (int b : v)
        out.push_back(static_cast<std::uint8_t>(b));
    return out;
}

}  // namespace

TEST_CASE("AES-128 matches the FIPS-197 test vector")
{
    // FIPS-197 Appendix B / C.1.
    const std::uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                  0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
                                  0x0c, 0x0d, 0x0e, 0x0f};
    const std::uint8_t pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55,
                                 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
                                 0xcc, 0xdd, 0xee, 0xff};
    const std::uint8_t expected[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b,
                                       0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80,
                                       0x70, 0xb4, 0xc5, 0x5a};
    Aes128 aes(key);
    std::uint8_t ct[16];
    aes.encrypt_block(pt, ct);
    CHECK(std::vector<std::uint8_t>(ct, ct + 16) ==
          std::vector<std::uint8_t>(expected, expected + 16));

    std::uint8_t back[16];
    aes.decrypt_block(expected, back);
    CHECK(std::vector<std::uint8_t>(back, back + 16) ==
          std::vector<std::uint8_t>(pt, pt + 16));
}

TEST_CASE("the real heartbeat block encrypts to the captured ciphertext")
{
    // The on-wire heartbeat block {"cmdId":525}\x00\x35\x0a and its ciphertext
    // MzlB36X/IVo8ZzI5rG9j1w== captured from the camera.
    const std::uint8_t block[16] = {0x7b, 0x22, 0x63, 0x6d, 0x64, 0x49,
                                    0x64, 0x22, 0x3a, 0x35, 0x32, 0x35,
                                    0x7d, 0x00, 0x35, 0x0a};
    Aes128 cipher = command_cipher();
    std::uint8_t ct[16];
    cipher.encrypt_block(block, ct);
    CHECK(std::vector<std::uint8_t>(ct, ct + 16) ==
          bytes({0x33, 0x39, 0x41, 0xdf, 0xa5, 0xff, 0x21, 0x5a, 0x3c, 0x67,
                 0x32, 0x39, 0xac, 0x6f, 0x63, 0xd7}));
}

TEST_CASE("decode() recovers the heartbeat JSON")
{
    std::string json;
    REQUIRE(decode(command_cipher(), "MzlB36X/IVo8ZzI5rG9j1w==", json));
    CHECK(json == "{\"cmdId\":525}");
}

TEST_CASE("decode() recovers a captured login exchange")
{
    Aes128 cipher = command_cipher();

    // Login response captured from the camera, carrying the session token.
    std::string resp;
    REQUIRE(decode(cipher,
                   "7sQ3+pH/khrx+xvDdflBd8zgJF1T4ImASXycUhSI4p9PW9TYvHzT6Tuh"
                   "7qFyCvRTmd4sQESImycJCGKho43h1WULW93yE+lNbmV4tSFUgyA=",
                   resp));
    CHECK(resp ==
          "{\"cmdId\":0,\"result\":0,\"token\":113034621,\"bat_percent\":80,"
          "\"errorMsg\":\"Success\"}");

    // Matching login request.
    std::string req;
    REQUIRE(
        decode(cipher,
               "J8WWuQDPmYSLfu/gXAG+UqbBy55KP2iE25QPNofzn040+NI9g7zeXLkI"
               "pXpC07SXvosrWsc1m8mxnq6hMiKwePbKJUwvSvqZb6s0sl1sfzh3S50pp"
               "0tu2Kewi0PiDcvXqXM2hPlNlhGSi3FUAv+Pdy5h/rx8+Gt77ThE+rd1DmE=",
               req));
    CHECK(req == "{\"cmdId\":0,\"usrName\":\"admin\",\"password\":\"admin\","
                 "\"needVideo\":0,\"needAudio\":0,\"utcTime\":1766505566,"
                 "\"supportHeartBeat\":true}");
}

TEST_CASE("encode()/decode() round-trips JSON of varying lengths")
{
    Aes128 cipher = command_cipher();
    const std::string cases[] = {
        "{\"cmdId\":525}",
        "{\"cmdId\":512,\"token\":113034621}",
        "{}",
        // Exactly one block (16 bytes) before the NUL, to exercise padding.
        "{\"cmdId\":76543}",
    };
    for (const auto &json : cases) {
        std::string out;
        REQUIRE(decode(cipher, encode(cipher, json), out));
        CHECK(out == json);
    }
}

TEST_CASE("encode() is deterministic and Base64-aligned")
{
    const std::string b64 = encode(command_cipher(), "{\"cmdId\":525}");
    CHECK(b64.size() % 4 == 0);
    // Our zero-padded encoding; the app's on-wire heartbeat (MzlB...) differs
    // only in stale padding bytes after the NUL, which the camera ignores.
    CHECK(b64 == "VYoVRA3OqW+wyt+iBfgy2w==");
    std::string out;
    REQUIRE(decode(command_cipher(), b64, out));
    CHECK(out == "{\"cmdId\":525}");
}

TEST_CASE("decode() rejects malformed input")
{
    Aes128 cipher = command_cipher();
    std::string out;
    CHECK_FALSE(decode(cipher, "@@@@", out));  // invalid Base64 characters
    CHECK_FALSE(decode(cipher, "abc", out));   // length not a multiple of 4
    CHECK_FALSE(decode(cipher, "AAAA", out));  // valid Base64, but 3 bytes
    CHECK_FALSE(decode(cipher, "", out));      // empty
}
