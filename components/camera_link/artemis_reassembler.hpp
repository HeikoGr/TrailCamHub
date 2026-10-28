#pragma once

// Reassembles a (possibly fragmented) ARTEMIS message from the payloads of
// consecutive RUDP DATA/fragment packets. The first payload carries the
// 20-byte ARTEMIS header, whose PayloadLen field gives the total body length;
// the reassembler is therefore length-driven and works regardless of how many
// packets (and whether DATA or fragment type) the camera splits a response
// into. Pure logic, host-testable.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace camera::artemis
{

class Reassembler
{
  public:
    enum class Status {
        incomplete,  // more packets needed
        complete,    // message() holds a full ARTEMIS message
        invalid,     // the stream does not start with a valid header
    };

    // Discards any partial message.
    void reset();

    // Appends one RUDP payload. Returns complete once the full message has
    // arrived (then reset() before the next message), incomplete while more is
    // needed, or invalid if the accumulated start is not a valid ARTEMIS
    // header (the buffer is cleared).
    Status feed(const std::uint8_t *payload, std::size_t size);

    // The reassembled message; valid after feed() returns complete.
    const std::vector<std::uint8_t> &message() const
    {
        return buffer_;
    }

  private:
    std::vector<std::uint8_t> buffer_;
};

}  // namespace camera::artemis
