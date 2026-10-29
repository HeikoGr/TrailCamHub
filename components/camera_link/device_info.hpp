#pragma once

// Typed view of the GET_DEV_INFO (cmdId 512) response: the camera's identity
// and a little state. `dev_id` is the stable per-camera key used to organise
// stored media; `bat_percent` is the battery metadata we report onward. Pure
// logic, host-testable.

#include <cstdint>
#include <string>

namespace camera::artemis
{

struct DeviceInfo {
    std::string dev_id;             // "devId" — stable camera identity
    std::string model_name;         // "modelName"
    std::string fw_version;         // "fwVerName"
    std::int64_t bat_percent = -1;  // "batPercent" (-1 = unknown)
    std::int64_t sd_total_mb = -1;  // "sdTotalMB"
    std::int64_t sd_free_mb = -1;   // "sdFreeMB"
};

// Parses a device-info response. Returns false only if no device identity
// could be read (`devId` absent); other fields default when missing. `devId`
// is accepted as either a JSON string or a number.
bool parse_device_info(const std::string &json, DeviceInfo &out);

}  // namespace camera::artemis
