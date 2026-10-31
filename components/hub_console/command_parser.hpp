#pragma once

// Parses the line-based serial commands of the hub, as sent by the web
// installer or typed into a terminal:
//
//   help | status | scan | upload | stop
//   config show | config clear
//   config set <key> <value...>   value = rest of the line (empty erases)
//   config set-hex <key> <hex>    value as hex-encoded bytes (any characters)
//   config unset <key>
//
// A leading "tch " is accepted and ignored. Pure logic, host-testable.

#include <string>

namespace hub_console
{

enum class CommandType {
    empty,
    help,
    status,
    scan,
    upload,
    stop,
    config_show,
    config_clear,
    config_set,
    invalid,
};

struct Command {
    CommandType type = CommandType::empty;
    std::string key;    // config_set
    std::string value;  // config_set (empty = erase)
    std::string error;  // invalid
};

Command parse_command(const std::string &line);

// Decodes an even-length hex string; returns false on invalid input.
bool hex_decode(const std::string &hex, std::string &out);

// Text for the help command, one command per line.
const char *help_text();

}  // namespace hub_console
