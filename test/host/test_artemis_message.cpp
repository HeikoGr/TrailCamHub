#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "artemis_aes128.hpp"
#include "artemis_crypto.hpp"
#include "artemis_message.hpp"

using namespace camera::artemis;

namespace
{

void append_u32_le(std::vector<std::uint8_t> &v, std::uint32_t n)
{
    v.push_back(static_cast<std::uint8_t>(n & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 16) & 0xff));
    v.push_back(static_cast<std::uint8_t>((n >> 24) & 0xff));
}

// Assembles an ARTEMIS message around an already-Base64-encoded body.
std::vector<std::uint8_t> frame(std::uint32_t msg_type,
                                std::uint32_t app_sequence,
                                const std::string &base64_body)
{
    std::vector<std::uint8_t> v = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    append_u32_le(v, msg_type);
    append_u32_le(v, app_sequence);
    append_u32_le(v, static_cast<std::uint32_t>(base64_body.size()));
    v.insert(v.end(), base64_body.begin(), base64_body.end());
    v.push_back(0);  // NUL terminator
    return v;
}

}  // namespace

TEST_CASE("build_request() produces a parseable request")
{
    Aes128 cipher = command_cipher();
    const std::string json = "{\"cmdId\":512,\"token\":113034621}";

    std::vector<std::uint8_t> msg = build_request(cipher, 7, json);
    REQUIRE_FALSE(msg.empty());

    // Signature and little-endian header fields.
    CHECK(std::string(msg.begin(), msg.begin() + 7) == "ARTEMIS");
    CHECK(msg[7] == 0);

    Message parsed;
    REQUIRE(parse(cipher, msg.data(), msg.size(), parsed));
    CHECK(parsed.msg_type == kMsgRequest);
    CHECK(parsed.app_sequence == 7);
    CHECK(parsed.json == json);
}

TEST_CASE("parse() decodes a captured login response message")
{
    Aes128 cipher = command_cipher();
    // Real login-response body (MsgType 3) wrapped in an ARTEMIS header.
    const std::string body =
        "7sQ3+pH/khrx+xvDdflBd8zgJF1T4ImASXycUhSI4p9PW9TYvHzT6Tuh"
        "7qFyCvRTmd4sQESImycJCGKho43h1WULW93yE+lNbmV4tSFUgyA=";
    std::vector<std::uint8_t> msg = frame(kMsgResponse, 1, body);

    Message parsed;
    REQUIRE(parse(cipher, msg.data(), msg.size(), parsed));
    CHECK(parsed.msg_type == kMsgResponse);
    CHECK(parsed.app_sequence == 1);
    CHECK(parsed.json ==
          "{\"cmdId\":0,\"result\":0,\"token\":113034621,\"bat_percent\":80,"
          "\"errorMsg\":\"Success\"}");
}

TEST_CASE("parse() rejects malformed messages")
{
    Aes128 cipher = command_cipher();
    const std::string body = encode(cipher, "{\"cmdId\":525}");
    std::vector<std::uint8_t> good = frame(kMsgRequest, 1, body);
    Message parsed;

    SUBCASE("too short")
    {
        CHECK_FALSE(parse(cipher, good.data(), 10, parsed));
    }
    SUBCASE("wrong signature")
    {
        std::vector<std::uint8_t> bad = good;
        bad[0] = 'X';
        CHECK_FALSE(parse(cipher, bad.data(), bad.size(), parsed));
    }
    SUBCASE("payload length beyond the buffer")
    {
        std::vector<std::uint8_t> bad = good;
        bad[16] = 0xff;  // inflate PayloadLen (little-endian LSB)
        bad[17] = 0xff;
        CHECK_FALSE(parse(cipher, bad.data(), bad.size(), parsed));
    }
    SUBCASE("body not valid Base64 blocks")
    {
        std::vector<std::uint8_t> bad = frame(kMsgRequest, 1, "AAAA");
        CHECK_FALSE(parse(cipher, bad.data(), bad.size(), parsed));
    }
}
