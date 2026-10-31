/*
 * Reads commands from the console UART and executes them: settings are
 * stored through hub_config, scans and uploads are handed to the session
 * controller, which reports their progress as "TCH EVENT" lines.
 */

#include "hub_console.h"

#include <stdio.h>
#include <string.h>
#include <string>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "command_parser.hpp"
#include "hub_config.hpp"
#include "session_controller.h"

#define CONSOLE_TASK_STACK 6144
#define CONSOLE_TASK_PRIORITY 4
#define CONSOLE_LINE_MAX 512
#define CONSOLE_RX_BUFFER 1024

namespace
{

const char *TAG = "trailcamhub_console";

void reply(const char *kind, const std::string &message)
{
    printf("TCH %s %s\n", kind, message.c_str());
    fflush(stdout);
}

void print_settings()
{
    hub_config::Settings settings;
    if (hub_config::load(settings) != ESP_OK) {
        reply("ERR", "could not read settings");
        return;
    }
    for (hub_config::Key key : hub_config::kAllKeys) {
        reply("CONFIG", std::string(hub_config::key_name(key)) + " " +
                            hub_config::display_value(
                                key, hub_config::field(settings, key)));
    }
    reply("CONFIG",
          std::string("uplink ") + (hub_config::uplink_configured(settings)
                                        ? "configured"
                                        : "incomplete"));
}

void print_status()
{
    reply("STATE", session_controller_state_name());
    print_settings();
}

void request_cycle(esp_err_t error, const char *what)
{
    switch (error) {
    case ESP_OK:
        reply("OK", std::string(what) + " started");
        break;
    case ESP_ERR_INVALID_STATE:
        reply("ERR",
              std::string("busy (") + session_controller_state_name() + ")");
        break;
    case ESP_ERR_NOT_FOUND:
        reply("ERR", "uplink settings incomplete (see 'config show')");
        break;
    default:
        reply("ERR", std::string(what) + " failed: " + esp_err_to_name(error));
        break;
    }
}

void set_setting(const hub_console::Command &command)
{
    hub_config::Key key;
    if (!hub_config::key_from_name(command.key, key)) {
        reply("ERR", "unknown setting '" + command.key + "'");
        return;
    }
    std::string reason;
    const esp_err_t error = hub_config::set(key, command.value, &reason);
    if (error == ESP_OK) {
        reply("OK", std::string(hub_config::key_name(key)) +
                        (command.value.empty() ? " erased" : " stored"));
    } else if (error == ESP_ERR_INVALID_ARG) {
        reply("ERR", reason);
    } else {
        reply("ERR", std::string("could not store setting: ") +
                         esp_err_to_name(error));
    }
}

void execute(const hub_console::Command &command)
{
    using hub_console::CommandType;
    switch (command.type) {
    case CommandType::empty:
        break;
    case CommandType::help: {
        const std::string text = hub_console::help_text();
        std::size_t start = 0;
        std::size_t end;
        while ((end = text.find('\n', start)) != std::string::npos) {
            reply("HELP", text.substr(start, end - start));
            start = end + 1;
        }
        break;
    }
    case CommandType::status:
        print_status();
        break;
    case CommandType::scan:
        request_cycle(session_controller_request_scan(), "scan");
        break;
    case CommandType::upload:
        request_cycle(session_controller_request_upload(), "upload");
        break;
    case CommandType::config_show:
        print_settings();
        break;
    case CommandType::config_clear: {
        const esp_err_t error = hub_config::clear();
        if (error == ESP_OK) {
            reply("OK", "settings erased");
        } else {
            reply("ERR", std::string("could not erase settings: ") +
                             esp_err_to_name(error));
        }
        break;
    }
    case CommandType::config_set:
        set_setting(command);
        break;
    case CommandType::invalid:
        reply("ERR", command.error);
        break;
    }
}

void console_task(void *arg)
{
    (void)arg;
    static char line[CONSOLE_LINE_MAX];
    while (true) {
        if (fgets(line, sizeof(line), stdin) == NULL) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        const size_t length = strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n') {
            // Overlong line: drop the rest of it and reject the command.
            int c;
            while ((c = fgetc(stdin)) != '\n' && c != EOF) {
            }
            reply("ERR", "line too long");
            continue;
        }
        execute(hub_console::parse_command(line));
        // The line may hold a secret (password, device key).
        memset(line, 0, sizeof(line));
    }
}

}  // namespace

esp_err_t hub_console_start(void)
{
    const uart_port_t port = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;
    // Blocking reads on stdin need the UART driver behind the console VFS.
    esp_err_t error =
        uart_driver_install(port, CONSOLE_RX_BUFFER, 0, 0, NULL, 0);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not install the console UART driver: %s",
                 esp_err_to_name(error));
        return error;
    }
    uart_vfs_dev_port_set_rx_line_endings(port, ESP_LINE_ENDINGS_CRLF);
    uart_vfs_dev_use_driver(port);

    if (xTaskCreate(console_task, "hub_console", CONSOLE_TASK_STACK, NULL,
                    CONSOLE_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Serial commands ready; type 'help'");
    reply("READY", session_controller_state_name());
    return ESP_OK;
}
