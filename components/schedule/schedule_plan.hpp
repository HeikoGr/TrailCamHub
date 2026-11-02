#pragma once

// Wake-up schedule of the hub: when to read the cameras and when to check in
// with the backend. Pure logic, host-testable; the state kept across deep
// sleep, the clock and the stored settings live in schedule.cpp, and the task
// that requests the cycles and puts the hub to sleep in the scheduler
// component.
//
// Times are "schedule seconds": a monotonic count that keeps running across
// deep sleep and is not changed when the clock is set (see schedule.hpp).
// Local times of the day are only used for the read windows, and only when
// the clock holds the time.
//
// Camera reads follow a plan of time windows, each with its own interval, and
// an interval for the rest of the day:
//
//   "05:00-09:00=30,17:00-22:00=30,*=120"
//
// reads every 30 minutes from 5 to 9 and from 17 to 22 o'clock and every two
// hours otherwise. A read is due as soon as the time since the last one has
// reached the interval that applies at that moment, so the first read in a
// window comes at its start if the last one is older than the window's
// interval. Without a valid clock the windows are ignored and the "*"
// interval applies (if it is 0 or missing, the longest window interval).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace schedule
{

constexpr std::int64_t kNever = -1;
constexpr std::size_t kMaxWindows = 8;
// Longest interval in a plan and for check-ins (minutes).
constexpr std::uint32_t kMaxIntervalMin = 1440;
// Back-off and the low-battery stretch do not extend an interval beyond this
// (minutes), unless the interval itself is longer.
constexpr std::uint32_t kMaxStretchedMin = 1440;
// How far next_due() looks ahead (seconds).
constexpr std::int64_t kHorizonS = 3 * 24 * 3600;

struct Window {
    // Minutes of the day in local time. `end` is exclusive and may be 1440
    // (24:00); an end before the start spans midnight.
    std::uint16_t start = 0;
    std::uint16_t end = 0;
    // Interval in minutes; 0: no reads in this window.
    std::uint16_t interval_min = 0;
};

struct Plan {
    // The first window that contains a time applies.
    std::vector<Window> windows;
    // Interval outside the windows; 0: no reads.
    std::uint16_t default_min = 0;

    // True if the plan has any read at all.
    bool reads() const;
};

// Parses a plan: entries "HH:MM-HH:MM=MIN" and "*=MIN", separated by commas
// or spaces. "off" or an empty text is a plan without reads. Returns false
// with a short English reason in `error`.
bool parse_plan(const std::string &text, Plan &out, std::string *error);

// The plan in the form parse_plan() reads ("off" for no reads).
std::string format_plan(const Plan &plan);

// Interval in minutes at a local minute of the day (0: no reads).
std::uint32_t interval_at(const Plan &plan, int minute_of_day);

// Interval in minutes when the clock does not hold the time.
std::uint32_t clockless_interval(const Plan &plan);

// Maps schedule seconds to the local minute of the day (0-1439). Empty when
// the clock does not hold the time.
using LocalMinute = std::function<int(std::int64_t)>;

// 1, 2, 4, 8, 16 for 0, 1, 2, 3, 4 or more failures in a row.
std::uint32_t backoff_factor(std::uint32_t failures);

// `interval_min` times `factor`, but not beyond kMaxStretchedMin (unless the
// interval itself is longer).
std::uint32_t stretched(std::uint32_t interval_min, std::uint32_t factor);

// The earliest time t >= now at which t - last has reached the interval (times
// `factor`) that applies at t. `last` is kNever if there was no read yet: then
// any time with an interval counts. kNever if there is none within kHorizonS.
std::int64_t next_due(const Plan &plan, std::int64_t last, std::int64_t now,
                      std::uint32_t factor, const LocalMinute &local);

// --- Camera reads and check-ins ---

struct Settings {
    Plan camera;
    // Interval of the backend check-ins in minutes; 0: none on schedule.
    std::uint32_t checkin_min = 0;
    // The hub has complete uplink settings.
    bool uplink = false;
    // Camera battery below this percentage stretches the read interval by
    // low_battery_factor; 0: off.
    int low_battery_percent = 0;
    std::uint32_t low_battery_factor = 1;
};

// Kept in RTC memory across deep sleep: only plain fields.
struct State {
    // Start of the last cycle with a camera part.
    std::int64_t last_camera;
    // Start of the last cycle with a backend sync (scan or upload).
    std::int64_t last_checkin;
    // Start of the running cycle; kNever when idle.
    std::int64_t cycle_start;
    std::uint32_t camera_failures;
    std::uint32_t checkin_failures;
    // Battery of the camera read last; -1: unknown.
    std::int32_t camera_battery;
    // The running cycle has a camera part.
    bool cycle_camera;
    // A read was asked for (by the backend) and is due now.
    bool camera_requested;
};

State initial_state();

struct Times {
    std::int64_t camera = kNever;
    std::int64_t checkin = kNever;
};

// Factor for the camera read interval: back-off after failed reads and the
// stretch for a low camera battery.
std::uint32_t camera_factor(const Settings &settings, const State &state);

Times next_times(const Settings &settings, const State &state, std::int64_t now,
                 const LocalMinute &local);

enum class Task {
    none,
    camera,   // a scan: camera, then the backend sync if configured
    checkin,  // an upload: backend sync only
};

// What is due at `now`; a camera read comes first (it syncs as well).
Task due(const Times &times, std::int64_t now);

// A cycle started; `camera` if it has a camera part.
void cycle_started(State &state, bool camera, std::int64_t now);

struct CycleEnd {
    // The camera part ended with a camera session.
    bool camera_ok = false;
    // The camera part failed (all attempts); not set for a stopped cycle.
    bool camera_failed = false;
    // A backend sync was tried (the home Wi-Fi was joined or tried).
    bool sync_attempted = false;
    bool sync_ok = false;
};

void cycle_finished(State &state, const CycleEnd &end);

// The state as it will be once the running cycle's sync succeeds: what the
// hub tells the backend while it checks in.
State after_successful_sync(const State &state);

// --- Reads asked for by the backend ("readCamera" commands) ---

// Commands come with every check-in until they are acknowledged. A new one
// asks for a read; once a read has been tried, the next check-in acknowledges
// the command with its outcome. Kept in RTC memory: only plain fields.
struct ReadRequests {
    static constexpr std::size_t kMax = 4;
    static constexpr std::size_t kIdSize = 48;

    enum class Reply {
        pending,  // no read since the command arrived: do not acknowledge
        done,     // a read succeeded since: acknowledge as done
        failed,   // a read failed since: acknowledge as failed
    };

    struct Entry {
        char id[kIdSize];
        // 0 free, 1 pending, 2 done, 3 failed.
        std::uint8_t state;
    };
    Entry entries[kMax];

    // A command arrived. `is_new` is set when it was not known: a read is
    // needed then. An answer other than pending forgets the command.
    Reply on_command(const std::string &id, bool *is_new);

    // Records the outcome of a camera read for all pending commands.
    void on_camera_result(bool ok);

    bool any_pending() const;
    void clear();
};

// --- Where a setting comes from ---

enum class Origin {
    built_in,  // Kconfig default of the profile
    local,     // stored on the hub (console, web installer)
    remote,    // set from the backend
};

const char *origin_name(Origin origin);

// The remote value if given, else the local one if given, else the built-in.
const std::string &pick(const std::string *remote, const std::string *local,
                        const std::string &built_in, Origin *origin);

}  // namespace schedule
