#pragma once

// Figures of one video download by playback (cmdId 769): frames, bytes,
// gaps in the stream, repeated frames, time spent waiting for the SD card,
// session loop delays and the signal strength. They are logged during and
// after the video and written as one line to video_stats.csv, so that runs
// of different firmware can be compared. No ESP-IDF dependencies, so the
// counting and the CSV line are unit-tested on the host; the values that
// only the device knows (RSSI, heap, SD writer and CPU figures) are passed
// in.

#include <cstddef>
#include <cstdint>
#include <string>

#include "frame_reorder.hpp"

namespace camera::artemis
{

struct VideoTransferStats {
    // Frames and records of the playback stream.
    std::uint32_t frames = 0;
    std::uint32_t audio_frames = 0;
    std::uint32_t markers = 0;
    std::uint32_t foreign_records = 0;
    std::uint32_t other_records = 0;
    // Bytes of video and audio data received (written, or discarded in the
    // throughput test), and of each stream.
    std::uint64_t bytes = 0;
    std::uint64_t media_bytes = 0;
    std::uint64_t audio_bytes = 0;
    bool complete_logged = false;

    std::uint32_t ack_failures = 0;
    std::uint32_t heartbeats = 0;
    // Frames far from the expected sequence (more than the reorder window
    // ahead or behind) and how many of them were logged.
    std::uint32_t far_behind = 0;
    std::uint32_t far_logged = 0;
    // Gaps in the playback stream: how many opened, and how long they stayed
    // open until the camera sent the missing frame again.
    std::uint32_t gaps = 0;
    std::int64_t gap_started_us = 0;
    std::int64_t gap_total_us = 0;
    std::int64_t gap_max_us = 0;
    // Sequence missing in the open gap, and how many gaps and repeated
    // frames were logged one by one.
    std::uint16_t gap_seq = 0;
    std::uint16_t gap_missing = 0;
    std::uint32_t gap_logged = 0;
    std::uint32_t repeat_logged = 0;
    // Repeated frames that arrived while no gap was open: the camera sent
    // again a frame that had been acknowledged.
    std::uint32_t repeats_without_gap = 0;
    // Frames missing when a gap opened, in total and by size of the loss: one
    // frame, 2-4, 5-16, more. Single frames point to losses on the air, long
    // runs to full receive buffers.
    std::uint32_t missing = 0;
    std::uint32_t missing_runs[4] = {};
    // Time the session task spent between two receive calls (when it did not
    // take frames from the socket): longest, and how often over 10 and 50 ms.
    std::int64_t loop_max_us = 0;
    std::uint32_t loop_over_10ms = 0;
    std::uint32_t loop_over_50ms = 0;
    // Signal strength of the camera's access point during the video.
    int rssi_min = 0;
    int rssi_last = 0;
    // Frames on other bulk subtypes during a video (leftovers of thumbnails).
    std::uint32_t other_subtype_frames = 0;
    // Time the session waited for SD writes (only while the writer's buffer
    // was full), and the time the SD writer spent writing.
    std::int64_t write_us = 0;
    std::int64_t write_max_us = 0;
    std::int64_t card_us = 0;
    std::int64_t card_max_us = 0;

    // A gap that opened or closed with a frame (note_pending()).
    struct GapChange {
        enum class Kind { none, opened, closed } kind = Kind::none;
        // For a closed gap: how long it was open.
        std::int64_t open_us = 0;
    };

    void reset()
    {
        *this = VideoTransferStats();
    }

    // The session task was busy for `busy_us` between two receive calls.
    void note_loop_busy(std::int64_t busy_us);
    // The session waited `took_us` for an SD write.
    void note_write(std::int64_t took_us);
    // The SD writer spent `card_us` writing (longest single write
    // `card_max_us`) since the last call.
    void add_card_time(std::int64_t card_us, std::int64_t card_max_us);
    // The RSSI of the camera's access point.
    void note_rssi(int rssi);

    // Updates the gap figures after frame `seq` was fed to the playback
    // stream: `pending_before` / `pending_after` are the frames waiting for
    // a missing one before and after, `expected_after` the sequence the
    // stream expects after the frame, `now_us` the time.
    GapChange note_pending(std::size_t pending_before,
                           std::size_t pending_after, std::uint16_t seq,
                           std::uint16_t expected_after, std::int64_t now_us);

    // Average time a gap stayed open, in milliseconds (0 without gaps).
    std::int64_t gap_average_ms() const;
};

// What only the device knows at the end of a video, for the CSV line.
struct VideoEndInfo {
    // Firmware version, build date and time (esp_app_desc_t).
    const char *version = "";
    const char *date = "";
    const char *time = "";
    std::int64_t dir_num = 0;
    std::int64_t media_num = 0;
    bool complete = false;
    std::int64_t elapsed_ms = 0;
    std::uint32_t heap_min = 0;
    bool discard_test = false;
    // CPU idle share in percent, or -1 if unknown.
    int cpu_idle_percent = -1;
};

// Header of video_stats.csv.
extern const char *const kVideoStatsCsvHeader;

// One line of video_stats.csv (without line end) for a finished video.
std::string video_stats_csv_line(const VideoTransferStats &stats,
                                 const FrameReorder::Stats &frames,
                                 const VideoEndInfo &info);

}  // namespace camera::artemis
