#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * Web UI of the dongle: the device opens its own Wi-Fi access point next to
 * the camera station (AP+STA) and serves a page to start and stop the camera
 * search, follow the status and the live log, and browse, view and download
 * the files on the SD card. The component knows nothing about the camera
 * workflow: the application reports the status and receives the button
 * actions through callbacks.
 *
 * When the station joins the camera, the access point moves to the camera's
 * channel, so clients are disconnected briefly and usually reconnect on their
 * own.
 */

/* Called from the HTTP server task; they must return quickly. */
typedef struct {
    void (*on_search)(void);
    void (*on_stop)(void);
} web_ui_callbacks_t;

/*
 * Opens the access point and starts the HTTP server. Requires an initialized
 * and started Wi-Fi driver and NVS (for the generated password).
 */
esp_err_t web_ui_start(const web_ui_callbacks_t *callbacks);

/* Access point name, password and page address; valid after web_ui_start. */
const char *web_ui_ssid(void);
const char *web_ui_password(void);
const char *web_ui_url(void);

/*
 * Reports the camera workflow status shown on the page. `phase` is one of
 * "idle", "searching", "connecting", "connected", "retry_wait", "finished";
 * `ssid` may be empty. Safe to call from any task.
 */
void web_ui_set_status(const char *phase, const char *ssid,
                       uint32_t retry_delay_ms);

/*
 * Reports the media download progress: `done` of `total` files handled,
 * `stored` of them saved. Kept until the next search starts.
 */
void web_ui_set_media_progress(uint32_t done, uint32_t stored, uint32_t total);
