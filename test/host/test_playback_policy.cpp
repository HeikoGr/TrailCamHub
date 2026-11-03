#include <doctest/doctest.h>

#include "playback_policy.hpp"

using camera::artemis::kVideoAdoptSessionRecords;
using camera::artemis::kVideoAppFileType;
using camera::artemis::next_playback_file_type;
using camera::artemis::PlaybackSession;
using camera::artemis::video_stream_ended;
using Verdict = PlaybackSession::Verdict;

TEST_CASE("the stream ends after 3 s without data")
{
    const std::int64_t last = 10 * 1000 * 1000;
    // 100 of 460 frames, total known.
    CHECK_FALSE(video_stream_ended(last + 3000 * 1000, last, 100, 460));
    CHECK(video_stream_ended(last + 3000 * 1000 + 1, last, 100, 460));
    // Total unknown (no totalFrame in the answer).
    CHECK_FALSE(video_stream_ended(last + 2000 * 1000, last, 100, 0));
    CHECK(video_stream_ended(last + 3000 * 1000 + 1, last, 100, 0));
}

TEST_CASE("once all video frames are in, the stream ends after 500 ms")
{
    const std::int64_t last = 10 * 1000 * 1000;
    CHECK_FALSE(video_stream_ended(last + 500 * 1000, last, 460, 460));
    CHECK(video_stream_ended(last + 500 * 1000 + 1, last, 460, 460));
    // More frames than announced count as complete as well.
    CHECK(video_stream_ended(last + 600 * 1000, last, 461, 460));
    // One frame short: the long wait applies.
    CHECK_FALSE(video_stream_ended(last + 600 * 1000, last, 459, 460));
}

TEST_CASE("records of the requested session are accepted")
{
    PlaybackSession session;
    session.start(1234);
    CHECK(session.number() == 1234);
    CHECK(session.check(1234, 0) == Verdict::accept);
    CHECK(session.foreign_records() == 0);
}

TEST_CASE("another session is adopted only before any frame was written")
{
    PlaybackSession session;
    session.start(1234);
    for (std::uint32_t i = 1; i < kVideoAdoptSessionRecords; i++) {
        CHECK(session.check(77, 0) == Verdict::skip);
    }
    CHECK(session.foreign_records() == kVideoAdoptSessionRecords - 1);
    CHECK(session.check(77, 0) == Verdict::adopt);
    CHECK(session.number() == 77);
    CHECK(session.check(77, 0) == Verdict::accept);
    // The old number is now foreign.
    CHECK(session.check(1234, 5) == Verdict::skip);
}

TEST_CASE("leftovers of an old stream are not adopted once frames came")
{
    PlaybackSession session;
    session.start(1234);
    for (std::uint32_t i = 0; i < 2 * kVideoAdoptSessionRecords; i++) {
        CHECK(session.check(77, 3) == Verdict::skip);
    }
    CHECK(session.number() == 1234);

    // A new start forgets the count.
    session.start(99);
    CHECK(session.foreign_records() == 0);
    CHECK(session.check(99, 0) == Verdict::accept);
}

TEST_CASE("a refused playback is tried once with the other fileType")
{
    std::int64_t next = -1;
    // The media list's value (2) was refused: try the app's (1).
    REQUIRE(next_playback_file_type(2, 2, false, &next));
    CHECK(next == kVideoAppFileType);
    // The app's value was refused: try the list's.
    REQUIRE(next_playback_file_type(kVideoAppFileType, 2, false, &next));
    CHECK(next == 2);
    // Only one retry.
    CHECK_FALSE(next_playback_file_type(1, 2, true, &next));
    // A list that reports the app's value leaves nothing else to try.
    CHECK_FALSE(next_playback_file_type(1, 1, false, &next));
}
