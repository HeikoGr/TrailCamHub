#pragma once

#include <stdint.h>

#include "esp_err.h"

/*
 * The session controller runs the camera workflow in its own task: activate
 * the camera over BLE, join its Wi-Fi, run the camera session, and retry
 * after failures with an increasing delay.
 */

typedef enum {
    SESSION_STATUS_IDLE,
    SESSION_STATUS_SEARCHING,
    SESSION_STATUS_CONNECTING,
    SESSION_STATUS_CONNECTED,
    SESSION_STATUS_RETRY_WAIT,
} session_status_t;

typedef struct {
    session_status_t status;
    /* Camera Wi-Fi name once the camera has reported it, otherwise empty. */
    char ssid[33];
    /* Delay until the next attempt for SESSION_STATUS_RETRY_WAIT. */
    uint32_t retry_delay_ms;
} session_status_info_t;

/* Called from the controller task after each state change; must return quickly.
 */
typedef void (*session_status_cb_t)(const session_status_info_t *info);

/* Registers the status callback; call before session_controller_start(). */
void session_controller_set_status_callback(session_status_cb_t callback);

/* Searches for the camera now (leaves idle or a retry delay). */
void session_controller_request_search(void);

/* Stops searching or ends the running session and returns to idle. */
void session_controller_request_stop(void);

/* Creates the controller's queues and timer and initializes camera Wi-Fi. */
esp_err_t session_controller_init(void);

/* Prepares camera BLE and starts the controller tasks. */
esp_err_t session_controller_start(void);
