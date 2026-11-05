#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/*
 * Called from the Bluetooth task when the camera has returned Wi-Fi
 * credentials. The strings are only valid during the call, and the callback
 * must return quickly.
 */
typedef esp_err_t (*camera_ble_credentials_cb_t)(const char *ssid,
                                                 const char *password);

/*
 * Prepares the camera activation. Activation starts paused; call
 * camera_ble_resume() to start the BLE stack, scan for a camera (the first
 * one not done in this scan), enable its Wi-Fi and receive the credentials
 * through `on_credentials`.
 */
esp_err_t camera_ble_start(camera_ble_credentials_cb_t on_credentials);

/*
 * Called with true when a connection to the camera is being set up, and with
 * false once the BLE stack has been released (or, without
 * CONFIG_CAMERA_BLE_RELEASE_AFTER_ACTIVATION, once the connection attempt or
 * connection has ended). Further attempts in between are not reported. For
 * example, the Wi-Fi access point is closed in between, because on the
 * ESP32-C5 it left the BLE connection setup no air time. Called from the
 * Bluetooth task or from the task that calls camera_ble_pause(); must return
 * quickly.
 */
typedef void (*camera_ble_link_cb_t)(bool active);

/* Registers the link callback; call before camera_ble_resume(). */
void camera_ble_set_link_callback(camera_ble_link_cb_t callback);

/*
 * Starts (or restarts) the camera activation: start the BLE stack if needed,
 * scan, connect, send the Wi-Fi-enable command. A still-open camera
 * connection is closed first so the command is sent again.
 */
void camera_ble_resume(void);

/*
 * A scan can read several cameras: after one is done, camera_ble_mark_done()
 * makes the following activations skip it, so camera_ble_resume() reaches
 * the next one. camera_ble_clear_done() starts a new scan with no camera
 * done. Up to 8 cameras are remembered.
 */
void camera_ble_clear_done(void);
/* Marks the camera of the last connection as done for this scan. */
void camera_ble_mark_done(void);

/*
 * Copies the advertised name of the camera of the last connection, e.g.
 * "TC100_ED3B" ("unknown" if none).
 */
void camera_ble_last_name(char *out, size_t size);

/*
 * Stops scanning and ignores further camera advertisements. With
 * CONFIG_CAMERA_BLE_RELEASE_AFTER_ACTIVATION the camera connection is closed
 * and the BLE stack and controller are shut down to free their memory;
 * otherwise an existing camera connection is kept open.
 */
void camera_ble_pause(void);
