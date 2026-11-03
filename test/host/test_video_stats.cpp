#include <doctest/doctest.h>

#include <string>

#include "video_stats.hpp"

using camera::artemis::FrameReorder;
using camera::artemis::VideoEndInfo;
using camera::artemis::VideoTransferStats;
using Kind = VideoTransferStats::GapChange::Kind;

TEST_CASE("video_stats.csv line matches the one written on hardware")
{
    // The values of a line that the T-Dongle-C5 wrote for a video on
    // 2026-11-03 (firmware acd93ce); the line must stay the same so that
    // runs of different firmware can be compared.
    VideoTransferStats s;
    s.frames = 458;
    s.bytes = 9792827;
    s.gaps = 62;
    s.missing = 382;
    s.missing_runs[0] = 16;
    s.missing_runs[3] = 5;
    s.gap_total_us = 378 * 62 * 1000LL + 999;
    s.repeats_without_gap = 1143;
    s.ack_failures = 0;
    s.write_us = 5917 * 1000LL + 1;
    s.card_us = 25617 * 1000LL;
    s.loop_max_us = 137 * 1000LL + 500;
    s.loop_over_10ms = 250;
    s.loop_over_50ms = 21;
    s.rssi_min = -46;
    s.rssi_last = -45;
    FrameReorder::Stats frames;
    frames.duplicates = 4029;
    frames.buffered = 4331;
    VideoEndInfo info;
    info.version = "acd93ce";
    info.date = "Nov  3 2026";
    info.time = "17:30:17";
    info.dir_num = 100;
    info.media_num = 54;
    info.complete = true;
    info.elapsed_ms = 34136;
    info.heap_min = 984;
    info.discard_test = false;
    info.cpu_idle_percent = -1;

    CHECK(camera::artemis::video_stats_csv_line(s, frames, info) ==
          "acd93ce Nov  3 2026 17:30:17,100,54,1,458,9792827,34136,286,62,382,"
          "16,5,378,4029,1143,4331,0,5917,25617,137,250,21,984,-46,-45,0,-1");
}

TEST_CASE("video_stats.csv header is unchanged")
{
    CHECK(std::string(camera::artemis::kVideoStatsCsvHeader) ==
          "firmware,dir,num,complete,video_frames,bytes,ms,kb_per_s,gaps,"
          "missing,single_losses,runs_over_16,gap_ms_avg,repeats,"
          "repeats_no_gap,out_of_order,ack_failures,waited_for_sd_ms,"
          "sd_busy_ms,loop_max_ms,loop_over_10ms,loop_over_50ms,heap_min,"
          "rssi_min,rssi_last,discard_test,cpu_idle_percent");
}

TEST_CASE("video_stats.csv line without elapsed time or gaps")
{
    VideoTransferStats s;
    s.bytes = 1000;
    VideoEndInfo info;
    info.version = "v";
    info.date = "d";
    info.time = "t";
    info.discard_test = true;
    info.cpu_idle_percent = 42;
    // kb_per_s and gap_ms_avg are 0 instead of a division by zero.
    CHECK(
        camera::artemis::video_stats_csv_line(s, FrameReorder::Stats(), info) ==
        "v d t,0,0,0,0,1000,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,42");
}

TEST_CASE("a gap opens, counts the missing frames by run length and closes")
{
    VideoTransferStats s;
    // Frame 10 arrives while 7 is expected: 3 frames missing.
    VideoTransferStats::GapChange change = s.note_pending(0, 1, 10, 7, 1000);
    CHECK(change.kind == Kind::opened);
    CHECK(s.gaps == 1);
    CHECK(s.gap_seq == 7);
    CHECK(s.gap_missing == 3);
    CHECK(s.missing == 3);
    CHECK(s.missing_runs[1] == 1);

    // More frames wait: the gap stays open.
    change = s.note_pending(1, 2, 11, 7, 2000);
    CHECK(change.kind == Kind::none);
    CHECK(s.gaps == 1);

    // The missing frames arrive and the waiting ones are passed on.
    change = s.note_pending(2, 0, 7, 12, 26000);
    CHECK(change.kind == Kind::closed);
    CHECK(change.open_us == 25000);
    CHECK(s.gap_total_us == 25000);
    CHECK(s.gap_max_us == 25000);
    CHECK(s.gap_average_ms() == 25);
}

TEST_CASE("missing frames are sorted into runs of 1, 2-4, 5-16 and more")
{
    VideoTransferStats s;
    const struct {
        std::uint16_t seq;
        std::uint16_t expected;
        int run;
    } cases[] = {
        {101, 100, 0}, {102, 100, 1}, {104, 100, 1}, {105, 100, 2},
        {116, 100, 2}, {117, 100, 3}, {3, 65530, 2},
    };
    std::uint32_t total = 0;
    for (const auto &c : cases) {
        const std::uint32_t before = s.missing_runs[c.run];
        CHECK(s.note_pending(0, 1, c.seq, c.expected, 0).kind == Kind::opened);
        CHECK(s.missing_runs[c.run] == before + 1);
        total += static_cast<std::uint16_t>(c.seq - c.expected);
        // Close it again.
        CHECK(s.note_pending(1, 0, c.expected, c.seq, 0).kind == Kind::closed);
    }
    CHECK(s.missing == total);
    CHECK(s.gaps == 7);
}

TEST_CASE("loop, write, card and RSSI figures")
{
    VideoTransferStats s;
    s.note_loop_busy(5 * 1000);
    s.note_loop_busy(60 * 1000);
    s.note_loop_busy(20 * 1000);
    CHECK(s.loop_max_us == 60 * 1000);
    CHECK(s.loop_over_10ms == 2);
    CHECK(s.loop_over_50ms == 1);

    s.note_write(300);
    s.note_write(100);
    CHECK(s.write_us == 400);
    CHECK(s.write_max_us == 300);

    s.add_card_time(1000, 400);
    s.add_card_time(500, 200);
    CHECK(s.card_us == 1500);
    CHECK(s.card_max_us == 400);

    s.note_rssi(-50);
    s.note_rssi(-60);
    s.note_rssi(-55);
    CHECK(s.rssi_min == -60);
    CHECK(s.rssi_last == -55);

    s.reset();
    CHECK(s.loop_max_us == 0);
    CHECK(s.rssi_min == 0);
    CHECK(s.missing_runs[3] == 0);
}
