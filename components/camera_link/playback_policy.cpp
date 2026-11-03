#include "playback_policy.hpp"

namespace camera::artemis
{

bool video_stream_ended(std::int64_t now_us, std::int64_t last_data_us,
                        std::uint32_t frames, std::int64_t total_frames)
{
    const bool all_video_frames =
        total_frames > 0 && static_cast<std::int64_t>(frames) >= total_frames;
    const std::int64_t idle_us =
        all_video_frames ? kVideoEndIdleCompleteUs : kVideoEndIdleUs;
    return now_us - last_data_us > idle_us;
}

void PlaybackSession::start(std::uint32_t requested)
{
    number_ = requested;
    foreign_records_ = 0;
}

PlaybackSession::Verdict PlaybackSession::check(std::uint32_t session_no,
                                                std::uint32_t frames_written)
{
    if (session_no == number_) {
        return Verdict::accept;
    }
    if (++foreign_records_ < kVideoAdoptSessionRecords || frames_written > 0) {
        return Verdict::skip;
    }
    number_ = session_no;
    return Verdict::adopt;
}

bool next_playback_file_type(std::int64_t current, std::int64_t list_type,
                             bool retried, std::int64_t *next)
{
    const std::int64_t other =
        current == list_type ? kVideoAppFileType : list_type;
    if (retried || other == current) {
        return false;
    }
    *next = other;
    return true;
}

}  // namespace camera::artemis
