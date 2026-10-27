#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "artemis_reassembler.hpp"

using namespace camera::artemis;
using Status = Reassembler::Status;

namespace
{

// A synthetic ARTEMIS message: header + a body of `payload_len` filler bytes.
std::vector<std::uint8_t> message(std::uint32_t payload_len)
{
    std::vector<std::uint8_t> v = {'A', 'R', 'T', 'E', 'M', 'I', 'S', 0};
    auto push_u32 = [&](std::uint32_t n) {
        v.push_back(static_cast<std::uint8_t>(n & 0xff));
        v.push_back(static_cast<std::uint8_t>((n >> 8) & 0xff));
        v.push_back(static_cast<std::uint8_t>((n >> 16) & 0xff));
        v.push_back(static_cast<std::uint8_t>((n >> 24) & 0xff));
    };
    push_u32(3);  // MsgType
    push_u32(1);  // AppSeq
    push_u32(payload_len);
    for (std::uint32_t i = 0; i < payload_len; ++i)
        v.push_back(static_cast<std::uint8_t>('a' + (i % 26)));
    return v;
}

Status feed(Reassembler &r, const std::vector<std::uint8_t> &v, std::size_t a,
            std::size_t b)
{
    return r.feed(v.data() + a, b - a);
}

}  // namespace

TEST_CASE("a single payload completes immediately")
{
    const std::vector<std::uint8_t> msg = message(40);
    Reassembler r;
    CHECK(r.feed(msg.data(), msg.size()) == Status::complete);
    CHECK(r.message() == msg);
}

TEST_CASE("a message split across packets is reassembled")
{
    const std::vector<std::uint8_t> msg = message(100);  // header + 100 body
    Reassembler r;
    // Three fragments: header-plus-part, middle, tail.
    CHECK(feed(r, msg, 0, 30) == Status::incomplete);
    CHECK(feed(r, msg, 30, 80) == Status::incomplete);
    CHECK(feed(r, msg, 80, msg.size()) == Status::complete);
    CHECK(r.message() == msg);
}

TEST_CASE("the header itself may be split")
{
    const std::vector<std::uint8_t> msg = message(16);
    Reassembler r;
    CHECK(feed(r, msg, 0, 10) == Status::incomplete);  // partial header
    CHECK(feed(r, msg, 10, 20) ==
          Status::incomplete);  // header complete, no body
    CHECK(feed(r, msg, 20, msg.size()) == Status::complete);
    CHECK(r.message() == msg);
}

TEST_CASE("a bad signature is rejected and the buffer cleared")
{
    std::vector<std::uint8_t> msg = message(16);
    msg[0] = 'X';
    Reassembler r;
    CHECK(r.feed(msg.data(), msg.size()) == Status::invalid);
    // After the reset a valid message still works.
    const std::vector<std::uint8_t> good = message(16);
    CHECK(r.feed(good.data(), good.size()) == Status::complete);
}

TEST_CASE("a non-ARTEMIS prefix such as a handshake ACK is rejected")
{
    Reassembler r;
    const std::uint8_t ack[] = {'A', 'C', 'K'};
    CHECK(r.feed(ack, sizeof(ack)) == Status::invalid);
    // A real message fed afterwards still reassembles cleanly.
    const std::vector<std::uint8_t> msg = message(16);
    CHECK(r.feed(msg.data(), msg.size()) == Status::complete);
    CHECK(r.message() == msg);
}

TEST_CASE("reset discards a partial message")
{
    const std::vector<std::uint8_t> msg = message(100);
    Reassembler r;
    CHECK(feed(r, msg, 0, 30) == Status::incomplete);
    r.reset();
    // Feeding the tail alone now looks like a fresh (invalid) start.
    CHECK(feed(r, msg, 30, msg.size()) == Status::invalid);
}
