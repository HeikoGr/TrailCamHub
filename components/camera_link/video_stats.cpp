#include "video_stats.hpp"

#include <cstdio>

namespace camera::artemis
{

void VideoTransferStats::note_loop_busy(std::int64_t busy_us)
{
    if (busy_us > loop_max_us) {
        loop_max_us = busy_us;
    }
    if (busy_us > 10 * 1000) {
        loop_over_10ms++;
    }
    if (busy_us > 50 * 1000) {
        loop_over_50ms++;
    }
}

void VideoTransferStats::note_write(std::int64_t took_us)
{
    write_us += took_us;
    if (took_us > write_max_us) {
        write_max_us = took_us;
    }
}

void VideoTransferStats::add_card_time(std::int64_t card, std::int64_t max)
{
    card_us += card;
    if (max > card_max_us) {
        card_max_us = max;
    }
}

void VideoTransferStats::note_rssi(int rssi)
{
    rssi_last = rssi;
    if (rssi_min == 0 || rssi < rssi_min) {
        rssi_min = rssi;
    }
}

VideoTransferStats::GapChange VideoTransferStats::note_pending(
    std::size_t pending_before, std::size_t pending_after, std::uint16_t seq,
    std::uint16_t expected_after, std::int64_t now_us)
{
    GapChange change;
    // A gap opens when a frame is kept waiting for a missing one, and closes
    // when the missing frame arrives and the waiting ones are passed on.
    if (pending_before == 0 && pending_after > 0) {
        gaps++;
        gap_started_us = now_us;
        gap_seq = expected_after;
        const std::uint16_t lost = static_cast<std::uint16_t>(seq - gap_seq);
        gap_missing = lost;
        missing += lost;
        missing_runs[lost <= 1 ? 0 : lost <= 4 ? 1 : lost <= 16 ? 2 : 3]++;
        change.kind = GapChange::Kind::opened;
    } else if (pending_before > 0 && pending_after == 0) {
        const std::int64_t open_us = now_us - gap_started_us;
        gap_total_us += open_us;
        if (open_us > gap_max_us) {
            gap_max_us = open_us;
        }
        change.kind = GapChange::Kind::closed;
        change.open_us = open_us;
    }
    return change;
}

std::int64_t VideoTransferStats::gap_average_ms() const
{
    return gaps > 0 ? gap_total_us / gaps / 1000 : 0;
}

const char *const kVideoStatsCsvHeader =
    "firmware,dir,num,complete,video_frames,bytes,ms,kb_per_s,gaps,"
    "missing,single_losses,runs_over_16,gap_ms_avg,repeats,"
    "repeats_no_gap,out_of_order,ack_failures,waited_for_sd_ms,"
    "sd_busy_ms,loop_max_ms,loop_over_10ms,loop_over_50ms,heap_min,"
    "rssi_min,rssi_last,discard_test,cpu_idle_percent";

std::string video_stats_csv_line(const VideoTransferStats &s,
                                 const FrameReorder::Stats &frames,
                                 const VideoEndInfo &info)
{
    char line[320];
    std::snprintf(
        line, sizeof(line),
        "%s %s %s,%lld,%lld,%d,%u,%llu,%lld,%llu,%u,%u,%u,%u,%u,%u,%u,%"
        "u,%u,%lld,%lld,%lld,%u,%u,%u,%d,%d,%u,%d",
        info.version, info.date, info.time, (long long)info.dir_num,
        (long long)info.media_num, info.complete ? 1 : 0, (unsigned)s.frames,
        (unsigned long long)s.bytes, (long long)info.elapsed_ms,
        (unsigned long long)(info.elapsed_ms > 0 ? s.bytes / info.elapsed_ms
                                                 : 0),
        (unsigned)s.gaps, (unsigned)s.missing, (unsigned)s.missing_runs[0],
        (unsigned)s.missing_runs[3], (unsigned)s.gap_average_ms(),
        (unsigned)frames.duplicates, (unsigned)s.repeats_without_gap,
        (unsigned)frames.buffered, (unsigned)s.ack_failures,
        (long long)(s.write_us / 1000), (long long)(s.card_us / 1000),
        (long long)(s.loop_max_us / 1000), (unsigned)s.loop_over_10ms,
        (unsigned)s.loop_over_50ms, (unsigned)info.heap_min, s.rssi_min,
        s.rssi_last, (unsigned)(info.discard_test ? 1 : 0),
        info.cpu_idle_percent);
    return line;
}

}  // namespace camera::artemis
