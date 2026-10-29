#include "device_info.hpp"

#include "artemis_json.hpp"

namespace camera::artemis
{

bool parse_device_info(const std::string &json, DeviceInfo &out)
{
    out = DeviceInfo{};

    std::string s;
    std::int64_t n = 0;
    if (json_find_string(json, "devId", s)) {
        out.dev_id = s;
    } else if (json_find_int(json, "devId", n)) {
        out.dev_id = std::to_string(n);
    } else {
        return false;
    }

    if (json_find_string(json, "modelName", s))
        out.model_name = s;
    if (json_find_string(json, "fwVerName", s))
        out.fw_version = s;
    if (json_find_int(json, "batPercent", n))
        out.bat_percent = n;
    if (json_find_int(json, "sdTotalMB", n))
        out.sd_total_mb = n;
    if (json_find_int(json, "sdFreeMB", n))
        out.sd_free_mb = n;

    return true;
}

}  // namespace camera::artemis
