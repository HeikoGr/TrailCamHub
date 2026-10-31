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

namespace
{
std::string quoted(const std::string &text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        if (static_cast<unsigned char>(c) >= 0x20) {
            out.push_back(c);
        }
    }
    return out + "\"";
}
}  // namespace

std::string serialize_device_info(const DeviceInfo &info)
{
    std::string out = "{\"devId\":" + quoted(info.dev_id) +
                      ",\"modelName\":" + quoted(info.model_name) +
                      ",\"fwVerName\":" + quoted(info.fw_version);
    if (info.bat_percent >= 0)
        out += ",\"batPercent\":" + std::to_string(info.bat_percent);
    if (info.sd_total_mb >= 0)
        out += ",\"sdTotalMB\":" + std::to_string(info.sd_total_mb);
    if (info.sd_free_mb >= 0)
        out += ",\"sdFreeMB\":" + std::to_string(info.sd_free_mb);
    return out + "}";
}

}  // namespace camera::artemis
