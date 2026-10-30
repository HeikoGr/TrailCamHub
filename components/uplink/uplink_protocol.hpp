#pragma once

// Wire format of the backend's device API v1 (see backend/README.md):
// building the check-in and acknowledgement bodies and reading the check-in
// response. Pure logic, host-testable; HTTP lives in uplink.cpp.

#include <cstdint>
#include <string>
#include <vector>

namespace uplink
{

struct CameraTelemetry {
    std::string model;              // empty = unknown
    std::string firmware;           // empty = unknown
    std::int64_t bat_percent = -1;  // -1 = unknown
    std::int64_t sd_free_mb = -1;   // -1 = unknown
};

struct MediaRef {
    std::int64_t dir = 0;
    std::int64_t num = 0;
    std::int64_t file_type = 0;   // 0 = photo, 2 = video
    std::int64_t media_time = 0;  // Unix seconds
};

struct CheckinRequest {
    std::string hub_firmware;
    std::int64_t hub_bat_percent = -1;  // -1 = unknown
    bool camera_known = false;
    CameraTelemetry camera;
    std::vector<MediaRef> media;
};

struct ThumbUpload {
    std::int64_t dir = 0;
    std::int64_t num = 0;
    std::string url;
};

struct Command {
    std::string id;
    std::string type;
    std::int64_t dir = 0;
    std::int64_t num = 0;
};

struct CheckinResponse {
    std::int64_t server_time = 0;
    std::int64_t sleep_interval_s = 0;
    std::vector<ThumbUpload> thumb_uploads;
    std::vector<Command> commands;
};

struct CommandResult {
    std::string id;
    bool done = false;
    std::string error;  // for failed commands
};

struct AckRequest {
    std::vector<MediaRef> uploaded_thumbs;  // dir/num used
    std::vector<CommandResult> commands;
};

// Escapes a string for use inside JSON double quotes.
std::string json_escape(const std::string &text);

// Media entries other than photos (0) and videos (2) are left out, because
// the backend rejects them.
std::string build_checkin_json(const CheckinRequest &request);
std::string build_ack_json(const AckRequest &request);

// Returns false if the response is not a check-in response (no serverTime).
// Malformed list entries are skipped.
bool parse_checkin_response(const std::string &json, CheckinResponse &out);

}  // namespace uplink
