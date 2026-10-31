#pragma once

#include "esp_err.h"

/*
 * The session controller runs TrailCamHub cycles in its own task. It is idle
 * until a cycle is requested; a scan activates the camera over BLE, joins its
 * Wi-Fi, runs the camera session and then, if the uplink settings are
 * complete, syncs with the backend over the home Wi-Fi. Camera failures are
 * retried a bounded number of times per scan. Progress is printed as
 * "TCH EVENT ..." lines on the console.
 */

/* Creates the controller's queues and timers and initializes Wi-Fi. */
esp_err_t session_controller_init(void);

/* Prepares camera BLE and starts the controller tasks. */
esp_err_t session_controller_start(void);

/*
 * Requests a scan (camera, then backend sync if configured). Returns
 * ESP_ERR_INVALID_STATE while a cycle is running.
 */
esp_err_t session_controller_request_scan(void);

/*
 * Requests a backend sync of the data already on the SD card. Returns
 * ESP_ERR_NOT_FOUND if the uplink settings are incomplete and
 * ESP_ERR_INVALID_STATE while a cycle is running.
 */
esp_err_t session_controller_request_upload(void);

/* Name of the current controller state, e.g. "IDLE" or "SESSION". */
const char *session_controller_state_name(void);
