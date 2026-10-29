#pragma once

// Splits the playback stream of a video (bulk subtype 0x02, started with
// cmdId 769) into its ARTEMIS records. Unlike a photo, the stream is a
// sequence of records, one per video or audio frame, that follow each other
// in the byte stream regardless of frame boundaries. FrameReorder puts the
// frames in order; each complete record is passed to the sink. The stream
// starts at the first frame that begins with a record header; frames before
// it are not acknowledged, so the camera sends them again. Pure logic,
// host-testable.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "frame_reorder.hpp"

namespace camera::artemis
{

class PlaybackStream
{
  public:
    // Receives one record: version, type and payload (after the 20-byte
    // header). The payload may be modified. Returns false to abort.
    using RecordSink = std::function<bool(std::uint32_t, std::uint32_t,
                                          std::uint8_t *, std::size_t)>;

    enum class Status {
        ok,
        invalid,  // lost track of the records or the sink failed
    };

    PlaybackStream(std::size_t reorder_frames, std::size_t max_record_size);

    void reset();

    Status feed(std::uint16_t seq, const std::uint8_t *payload,
                std::size_t size, const RecordSink &sink, bool &ack);

    bool started() const
    {
        return reorder_.started();
    }
    std::uint64_t frames() const
    {
        return reorder_.next_frame();
    }
    std::uint64_t records() const
    {
        return records_;
    }
    const FrameReorder::Stats &reorder_stats() const
    {
        return reorder_.stats();
    }

  private:
    // Appends in-order bytes and passes on every complete record.
    bool consume(const std::uint8_t *data, std::size_t size,
                 const RecordSink &sink);

    FrameReorder reorder_;
    std::size_t max_record_size_;
    std::vector<std::uint8_t> record_;  // bytes of the record being assembled
    std::size_t record_size_ = 0;       // 0 until its header is complete
    std::uint64_t records_ = 0;
    bool failed_ = false;
};

}  // namespace camera::artemis
