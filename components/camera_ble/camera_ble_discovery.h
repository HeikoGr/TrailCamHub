#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Advertised names of compatible cameras start with this, e.g. TC100_ED3B. */
#define CAMERA_BLE_NAME_PREFIX "TC100_"
#define CAMERA_BLE_SERVICE_UUID 0x8801

/*
 * Returns true if one advertising report (advertisement or scan response
 * data) carries a camera name (CAMERA_BLE_NAME_PREFIX) or the camera service
 * UUID.
 */
bool camera_ble_is_candidate_advertisement(const uint8_t *data, size_t length);
