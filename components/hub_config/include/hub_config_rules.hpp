#pragma once

// Names, validation and display rules for the hub's stored settings (home
// Wi-Fi and backend access). Pure logic, host-testable; storage in NVS lives
// in hub_config.cpp.

#include <cstddef>
#include <string>

namespace hub_config
{

enum class Key {
    wifi_ssid,
    wifi_password,
    api_url,
    device_key,
};

constexpr Key kAllKeys[] = {Key::wifi_ssid, Key::wifi_password, Key::api_url,
                            Key::device_key};

struct Settings {
    std::string wifi_ssid;
    std::string wifi_password;
    std::string api_url;
    std::string device_key;
};

// Name used on the serial console and as the NVS key (max. 15 characters).
const char *key_name(Key key);
bool key_from_name(const std::string &name, Key &out);
bool is_secret(Key key);

std::string &field(Settings &settings, Key key);
const std::string &field(const Settings &settings, Key key);

// Returns an empty string if `value` is acceptable for `key`, otherwise a
// short English reason.
std::string validate(Key key, const std::string &value);

// Value as shown on the console: secrets are never echoed, only whether they
// are set (and, for the device key, the camera id part before the dot).
std::string display_value(Key key, const std::string &value);

// True if the hub has everything it needs to reach the backend.
bool uplink_configured(const Settings &settings);

// The backend URL without trailing slashes, ready for appending "/v1/...".
std::string api_base(const std::string &api_url);

}  // namespace hub_config
