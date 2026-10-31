#pragma once

// One sync with the TrailCamHub backend over the home Wi-Fi (already
// joined): check in with the camera's telemetry and media list, upload the
// thumbnails the backend is missing, and acknowledge uploads and commands.

#include <cstdint>
#include <string>

#include "esp_err.h"

#include "hub_config_rules.hpp"

namespace uplink
{

struct Report {
    std::string camera_id;      // camera whose data was sent (may be empty)
    int media_reported = 0;     // media entries in the check-in
    int thumbs_requested = 0;   // thumbnails the backend asked for
    int thumbs_uploaded = 0;    // ... uploaded successfully
    int thumbs_not_on_sd = 0;   // ... not stored on the SD card yet
    int commands_received = 0;  // commands handed out by the backend
    std::int64_t server_time = 0;
    std::string error;  // set when the sync failed
};

esp_err_t run(const hub_config::Settings &settings, Report &report);

}  // namespace uplink
