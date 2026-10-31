#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * The session controller runs TrailCamHub cycles in its own task. It is idle
 * until a cycle is requested; a scan activates the camera over BLE, joins its
 * Wi-Fi, runs the camera session and then, if the uplink settings are
 * complete, syncs with the backend over the home Wi-Fi. Camera failures are
 * retried a bounded number of times per scan.
 *
 * The controller does not know who drives it. Front ends (serial console,
 * display and button, a local web UI, a sleep scheduler) request cycles with
 * the functions below and follow the progress through status listeners or
 * session_controller_get_status().
 */

typedef enum {
    SESSION_PHASE_IDLE,
    /* Waking the camera over BLE and waiting for its Wi-Fi credentials. */
    SESSION_PHASE_CAMERA_WAKE,
    SESSION_PHASE_CAMERA_WIFI_JOIN,
    SESSION_PHASE_CAMERA_SESSION,
    /* A camera attempt failed; the next one starts after retry_delay_ms. */
    SESSION_PHASE_CAMERA_RETRY_WAIT,
    SESSION_PHASE_UPLINK_WIFI_JOIN,
    SESSION_PHASE_UPLINK,
} session_phase_t;

typedef enum {
    SESSION_RESULT_NONE,
    /* Camera session done; no uplink configured. */
    SESSION_RESULT_CAMERA_SCANNED,
    /* Camera session (unless upload only) and backend sync done. */
    SESSION_RESULT_UPLINK_DONE,
    SESSION_RESULT_CAMERA_UNREACHABLE,
    SESSION_RESULT_UPLINK_UNREACHABLE,
    /* Ended early by session_controller_request_stop(). */
    SESSION_RESULT_STOPPED,
} session_result_t;

typedef struct {
    session_phase_t phase;
    /* Stable name of the phase as used on the console, e.g. "IDLE". */
    const char *phase_name;
    /* A stop was requested and the camera session is closing. */
    bool stopping;
    /* Camera attempt within this cycle (1-based; 0 when idle). */
    uint32_t attempt;
    uint32_t max_attempts;
    /* Delay until the next attempt in SESSION_PHASE_CAMERA_RETRY_WAIT. */
    uint32_t retry_delay_ms;
    /*
     * Camera Wi-Fi name once the camera reported it in this cycle. Kept after
     * the cycle ends and cleared when the next cycle starts; empty for an
     * upload-only cycle.
     */
    char camera_ssid[33];
    /* True only on the update that ends a cycle. */
    bool cycle_finished;
    /* Result of the last finished cycle; NONE while a new one runs. */
    session_result_t last_result;
    /* Stable name of last_result, e.g. "uplink-done" or "none". */
    const char *last_result_name;
} session_status_t;

/*
 * Called from the controller task after each change; must return quickly. It
 * may call the request functions, which only queue the request.
 */
typedef void (*session_status_listener_t)(const session_status_t *status,
                                          void *context);

/* Maximum number of status listeners. */
#define SESSION_CONTROLLER_MAX_LISTENERS 4

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

/*
 * Requests the end of the running cycle: the camera part stops (a running
 * camera session closes cleanly first) and the cycle ends without the backend
 * sync. A sync that is already running is not interrupted. Returns
 * ESP_ERR_INVALID_STATE when idle or during the sync.
 */
esp_err_t session_controller_request_stop(void);

/*
 * Registers a listener for status changes; use session_controller_get_status()
 * for the status at registration. Returns ESP_ERR_NO_MEM when all slots are
 * taken.
 */
esp_err_t
session_controller_add_status_listener(session_status_listener_t listener,
                                       void *context);

/* Copies the current status (for polling front ends). */
void session_controller_get_status(session_status_t *out);

/* Name of the current controller state, e.g. "IDLE" or "SESSION". */
const char *session_controller_state_name(void);
