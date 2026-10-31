#include "command_parser.hpp"

#include <cctype>

namespace hub_console
{

namespace
{

std::string trim(const std::string &text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        begin++;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }
    return text.substr(begin, end - begin);
}

// Splits off the first space-separated word; `rest` keeps inner spacing.
std::string next_word(const std::string &text, std::string &rest)
{
    const std::size_t space = text.find(' ');
    if (space == std::string::npos) {
        rest.clear();
        return text;
    }
    rest = text.substr(space + 1);
    std::size_t skip = 0;
    while (skip < rest.size() && rest[skip] == ' ') {
        skip++;
    }
    rest.erase(0, skip);
    return text.substr(0, space);
}

int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

Command invalid(const std::string &error)
{
    Command command;
    command.type = CommandType::invalid;
    command.error = error;
    return command;
}

Command simple(CommandType type)
{
    Command command;
    command.type = type;
    return command;
}

Command parse_config(const std::string &args)
{
    std::string rest;
    const std::string action = next_word(args, rest);
    if (action == "show" && rest.empty()) {
        return simple(CommandType::config_show);
    }
    if (action == "clear" && rest.empty()) {
        return simple(CommandType::config_clear);
    }
    if (action != "set" && action != "set-hex" && action != "unset") {
        return invalid("usage: config show|clear|set|set-hex|unset");
    }

    std::string value;
    Command command;
    command.type = CommandType::config_set;
    command.key = next_word(rest, value);
    if (command.key.empty()) {
        return invalid("missing setting name");
    }
    if (action == "unset") {
        if (!value.empty()) {
            return invalid("usage: config unset <key>");
        }
    } else if (action == "set-hex") {
        if (!hex_decode(value, command.value)) {
            return invalid("value is not valid hex");
        }
    } else {
        command.value = value;
    }
    return command;
}

}  // namespace

bool hex_decode(const std::string &hex, std::string &out)
{
    out.clear();
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = hex_digit(hex[i]);
        const int low = hex_digit(hex[i + 1]);
        if (high < 0 || low < 0) {
            out.clear();
            return false;
        }
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return true;
}

Command parse_command(const std::string &line)
{
    std::string text = trim(line);
    if (text.rfind("tch ", 0) == 0 || text.rfind("TCH ", 0) == 0) {
        text = trim(text.substr(4));
    }
    if (text.empty()) {
        return simple(CommandType::empty);
    }

    std::string rest;
    const std::string word = next_word(text, rest);
    if (word == "config") {
        return parse_config(rest);
    }
    if (!rest.empty()) {
        return invalid("unexpected arguments for '" + word + "'");
    }
    if (word == "help") {
        return simple(CommandType::help);
    }
    if (word == "status") {
        return simple(CommandType::status);
    }
    if (word == "scan") {
        return simple(CommandType::scan);
    }
    if (word == "upload") {
        return simple(CommandType::upload);
    }
    return invalid("unknown command '" + word + "' (try 'help')");
}

const char *help_text()
{
    return "status                     show state and settings\n"
           "scan                       read the camera, then sync with the "
           "backend\n"
           "upload                     sync stored data with the backend "
           "only\n"
           "config show                list settings (secrets hidden)\n"
           "config set <key> <value>   store a setting\n"
           "config set-hex <key> <hex> store a hex-encoded setting\n"
           "config unset <key>         erase a setting\n"
           "config clear               erase all settings\n"
           "keys: wifi_ssid wifi_password api_url device_key\n";
}

}  // namespace hub_console
