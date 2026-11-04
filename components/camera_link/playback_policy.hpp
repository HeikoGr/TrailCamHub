#pragma once

// Decisions of a video download by playback (cmdId 769) that do not depend on
// ESP-IDF, so that they are unit-tested on the host: when the stream has
// ended, which session's records belong to it, and which fileType to try when
// the camera refuses the playback.

#include <cstdint>

namespace camera::artemis
{

// A video stream has ended when no frame arrived for this long.
constexpr std::int64_t kVideoEndIdleUs = 3 * 1000 * 1000;
// Once all video frames the camera announced have arrived, only audio frames
// may still follow, about 16 per second: the stream has ended after this
// long. (The last video frame was the last frame of every traced video.)
constexpr std::int64_t kVideoEndIdleCompleteUs = 500 * 1000;
// Records of another session number before the stream's number is adopted
// (in case the camera ignores the requested one).
constexpr std::uint32_t kVideoAdoptSessionRecords = 30;
// fileType of videos in the app's playback request. Our camera's media list
// reports videos as 2 and refuses playback with the app's value 1 (startPbRet
// -3), so the list's own value is tried first and this one as the fallback.
constexpr std::int64_t kVideoAppFileType = 1;

// The camera does not mark the end of the stream; it stops sending. True if
// no data arrived since `last_data_us` for long enough: `frames` video frames
// were received of the `total_frames` the camera announced (0: unknown).
bool video_stream_ended(std::int64_t now_us, std::int64_t last_data_us,
                        std::uint32_t frames, std::int64_t total_frames);

// Which playback records belong to the stream: those of the requested
// session. A camera that ignores the requested number uses its own; it is
// adopted once it is clearly the stream's, not a leftover of an old one.
class PlaybackSession
{
  public:
    enum class Verdict {
        accept,  // the stream's session
        skip,    // another session's record
        adopt,   // another session, now taken as the stream's
    };

    void start(std::uint32_t requested);
    std::uint32_t number() const
    {
        return number_;
    }
    std::uint32_t foreign_records() const
    {
        return foreign_records_;
    }

    // A record of `session_no` arrived; `frames_written` frames of the
    // stream were written so far.
    Verdict check(std::uint32_t session_no, std::uint32_t frames_written);

  private:
    std::uint32_t number_ = 0;
    std::uint32_t foreign_records_ = 0;
};

// The fileType for the next playback request after the camera refused one
// with `current`: the other of the media list's `list_type` and the app's
// value. Returns false if there is none left to try (already retried, or
// both are the same).
bool next_playback_file_type(std::int64_t current, std::int64_t list_type,
                             bool retried, std::int64_t *next);

}  // namespace camera::artemis
