#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * Commands on the serial console (USB-Serial-JTAG), so that a test run can be
 * driven and its results read from a computer without pressing the button or
 * taking out the SD card: start and stop the camera search, show the status,
 * and list, read and delete files on the SD card. Type "help" for the list.
 *
 * Every command ends with a line "@OK" or "@ERR <reason>", and file contents
 * are sent as "@D <offset> <base64>" lines, so that a script can pick them out
 * of the log output (tools/dongle_console.py does). The component knows
 * nothing about the camera workflow: the application reports the status and
 * receives the commands through callbacks.
 */

/* Called from the console task; they must return quickly. */
typedef struct {
    void (*on_search)(void);
    void (*on_stop)(void);
} debug_console_callbacks_t;

/*
 * Installs the USB-Serial-JTAG driver (the log output then goes through it as
 * well) and starts the console task.
 */
esp_err_t debug_console_start(const debug_console_callbacks_t *callbacks);

/*
 * Reports the camera workflow status, with the phases of web_ui_set_status().
 * Files cannot be deleted while the phase is "connecting", "connected" or
 * "uploading", because the camera session may be writing them or the backend
 * sync reading them. Safe to call from any task.
 */
void debug_console_set_status(const char *phase, const char *ssid,
                              uint32_t retry_delay_ms);

/* Reports the media download progress, as web_ui_set_media_progress(). */
void debug_console_set_media_progress(uint32_t done, uint32_t stored,
                                      uint32_t total);
