#include "artemis_json.hpp"

#include <cctype>

namespace camera::artemis
{

bool json_find_int(const std::string &json, const std::string &key,
                   std::int64_t &out, std::size_t from)
{
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle, from);
    if (pos == std::string::npos)
        return false;
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    if (pos >= json.size() || json[pos] != ':')
        return false;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
        ++pos;
    bool negative = false;
    if (pos < json.size() && (json[pos] == '-' || json[pos] == '+')) {
        negative = json[pos] == '-';
        ++pos;
    }
    if (pos >= json.size() ||
        !std::isdigit(static_cast<unsigned char>(json[pos]))) {
        return false;
    }
    std::int64_t value = 0;
    while (pos < json.size() &&
           std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + (json[pos] - '0');
        ++pos;
    }
    out = negative ? -value : value;
    return true;
}

}  // namespace camera::artemis
