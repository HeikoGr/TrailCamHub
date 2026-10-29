/*
 * This module starts the log copy on the SD card, initializes diagnostics,
 * persistent storage, the SD card and the network stack, then hands over to
 * the session controller, which runs the camera workflow. The BLE stack is
 * started by the camera BLE module when it is needed.
 */

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "sd_card.h"
#include "sd_log.h"
#include "session_controller.h"
#include "system_diagnostics.h"

static const char *TAG = "trailcamhub";

extern "C" void app_main(void)
{
#if CONFIG_SD_LOG_ENABLE
    // First, so that the boot messages are kept until the card is mounted.
    const bool sd_log_ready = sd_log_init() == ESP_OK;
#endif
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        error = nvs_flash_init();
    }
    ESP_ERROR_CHECK(error);

    error = diagnostics_start();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostics are incomplete: %s", esp_err_to_name(error));
    }

#if CONFIG_SD_CARD_ENABLE
    // Media buffering is not implemented yet, so a missing card is not fatal.
    error = sd_card_mount();
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without SD card");
    }
#if CONFIG_SD_LOG_ENABLE
    if (sd_log_ready) {
        if (error != ESP_OK || sd_log_start() != ESP_OK) {
            sd_log_disable();
        }
    }
#endif
#endif

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(session_controller_init());
    error = session_controller_start();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Could not start the session controller: %s",
                 esp_err_to_name(error));
    }
}
