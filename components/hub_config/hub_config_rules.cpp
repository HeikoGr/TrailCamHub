#include "hub_config_rules.hpp"

namespace hub_config
{

namespace
{

bool is_key_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool has_control_chars(const std::string &value)
{
    for (unsigned char c : value) {
        if (c < 0x20 || c == 0x7f) {
            return true;
        }
    }
    return false;
}

}  // namespace

const char *key_name(Key key)
{
    switch (key) {
    case Key::wifi_ssid:
        return "wifi_ssid";
    case Key::wifi_password:
        return "wifi_password";
    case Key::api_url:
        return "api_url";
    case Key::device_key:
        return "device_key";
    }
    return "unknown";
}

bool key_from_name(const std::string &name, Key &out)
{
    for (Key key : kAllKeys) {
        if (name == key_name(key)) {
            out = key;
            return true;
        }
    }
    return false;
}

bool is_secret(Key key)
{
    return key == Key::wifi_password || key == Key::device_key;
}

std::string &field(Settings &settings, Key key)
{
    switch (key) {
    case Key::wifi_ssid:
        return settings.wifi_ssid;
    case Key::wifi_password:
        return settings.wifi_password;
    case Key::api_url:
        return settings.api_url;
    case Key::device_key:
        break;
    }
    return settings.device_key;
}

const std::string &field(const Settings &settings, Key key)
{
    return field(const_cast<Settings &>(settings), key);
}

std::string validate(Key key, const std::string &value)
{
    switch (key) {
    case Key::wifi_ssid:
        if (value.empty() || value.size() > 32) {
            return "SSID must be 1-32 bytes";
        }
        return "";
    case Key::wifi_password:
        // Empty = open network; WPA passphrases are 8-63 characters, a raw
        // PSK is 64 hex digits.
        if (!value.empty() && (value.size() < 8 || value.size() > 64)) {
            return "password must be empty or 8-64 characters";
        }
        return "";
    case Key::api_url:
        if (value.rfind("https://", 0) != 0 || value.size() <= 8) {
            return "API URL must start with https://";
        }
        if (value.size() > 200 || has_control_chars(value) ||
            value.find(' ') != std::string::npos) {
            return "API URL is too long or contains invalid characters";
        }
        return "";
    case Key::device_key: {
        // <hubId>.<secret>, as printed by backend/scripts/register-hub.mjs
        const std::size_t dot = value.find('.');
        if (dot == std::string::npos || dot == 0 || dot > 64) {
            return "device key must look like <hubId>.<secret>";
        }
        const std::size_t secret_len = value.size() - dot - 1;
        if (secret_len < 16 || secret_len > 128) {
            return "device key secret must be 16-128 characters";
        }
        for (std::size_t i = 0; i < value.size(); i++) {
            if (i != dot && !is_key_char(value[i])) {
                return "device key contains invalid characters";
            }
        }
        return "";
    }
    }
    return "unknown setting";
}

std::string display_value(Key key, const std::string &value)
{
    if (value.empty()) {
        return "(not set)";
    }
    if (key == Key::wifi_password) {
        return "(set)";
    }
    if (key == Key::device_key) {
        const std::size_t dot = value.find('.');
        return value.substr(0, dot) + ".*** (set)";
    }
    return value;
}

bool uplink_configured(const Settings &settings)
{
    return validate(Key::wifi_ssid, settings.wifi_ssid).empty() &&
           validate(Key::wifi_password, settings.wifi_password).empty() &&
           validate(Key::api_url, settings.api_url).empty() &&
           validate(Key::device_key, settings.device_key).empty();
}

std::string api_base(const std::string &api_url)
{
    std::string base = api_url;
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

}  // namespace hub_config
