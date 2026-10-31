#pragma once

#include "esp_err.h"

/*
 * Serial command console on the console UART (the same port as the logs).
 * Commands are line based (see command_parser.hpp); every reply line starts
 * with "TCH " so the web installer can pick it out of the log stream:
 *
 *   TCH OK <message>          command accepted
 *   TCH ERR <message>         command rejected
 *   TCH CONFIG <key> <value>  one setting (secrets hidden)
 *   TCH STATE <state> ...     controller state
 *   TCH EVENT <name> ...      asynchronous progress: "state <STATE>",
 *                             "stopping", "done <result>", "uplink ..."
 */
esp_err_t hub_console_start(void);
