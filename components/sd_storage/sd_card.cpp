/*
 * This module mounts the board's microSD card, which buffers camera media
 * before upload, and reports its capacity and free space.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#if CONFIG_SD_CARD_SPI_MODE
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#else
#include "driver/sdmmc_host.h"
#endif

#include "sd_card.h"

#define SD_CARD_MAX_OPEN_FILES 4
#define SD_CARD_ALLOCATION_UNIT_SIZE (16 * 1024)
#if CONFIG_SD_CARD_SPI_MODE
#define SD_CARD_SPI_HOST SPI2_HOST
// The bus may be shared with a display that sends whole frames (160x80
// pixels in RGB565 on the T-Dongle-C5), so allow transfers of that size.
#define SD_CARD_SPI_MAX_TRANSFER (32 * 1024)
#endif

static const char *TAG = "trailcamhub_sd";
#if CONFIG_SD_CARD_ENABLE
static sdmmc_card_t *s_card;
#endif
#if CONFIG_SD_CARD_SPI_MODE
static bool s_spi_bus_ready;
#endif

bool sd_card_spi_bus_ready(void)
{
#if CONFIG_SD_CARD_SPI_MODE
    return s_spi_bus_ready;
#else
    return false;
#endif
}

esp_err_t sd_card_mount(void)
{
#if !CONFIG_SD_CARD_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = SD_CARD_MAX_OPEN_FILES;
    mount_config.allocation_unit_size = SD_CARD_ALLOCATION_UNIT_SIZE;

#if CONFIG_SD_CARD_SPI_MODE
    // The bus is left initialized even if no card is found, so that other
    // devices on it keep working.
    spi_bus_config_t bus = {};
    bus.mosi_io_num = CONFIG_SD_CARD_CMD_GPIO;
    bus.miso_io_num = CONFIG_SD_CARD_D0_GPIO;
    bus.sclk_io_num = CONFIG_SD_CARD_CLK_GPIO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = SD_CARD_SPI_MAX_TRANSFER;
    esp_err_t error =
        spi_bus_initialize(SD_CARD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Could not initialize the SD card SPI bus: %s",
                 esp_err_to_name(error));
        return error;
    }
    s_spi_bus_ready = true;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_CARD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_CARD_SPI_HOST;
    slot.gpio_cs = (gpio_num_t)CONFIG_SD_CARD_CS_GPIO;
    ESP_LOGI(TAG, "Mounting SD card (SPI, SCK=%d MOSI=%d MISO=%d CS=%d)",
             CONFIG_SD_CARD_CLK_GPIO, CONFIG_SD_CARD_CMD_GPIO,
             CONFIG_SD_CARD_D0_GPIO, CONFIG_SD_CARD_CS_GPIO);
    error = esp_vfs_fat_sdspi_mount(SD_CARD_MOUNT_POINT, &host, &slot,
                                    &mount_config, &s_card);
#else
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = (gpio_num_t)CONFIG_SD_CARD_CLK_GPIO;
    slot.cmd = (gpio_num_t)CONFIG_SD_CARD_CMD_GPIO;
    slot.d0 = (gpio_num_t)CONFIG_SD_CARD_D0_GPIO;
    ESP_LOGI(TAG, "Mounting SD card (1-bit SDMMC, CLK=%d CMD=%d D0=%d)",
             CONFIG_SD_CARD_CLK_GPIO, CONFIG_SD_CARD_CMD_GPIO,
             CONFIG_SD_CARD_D0_GPIO);
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t error = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot,
                                              &mount_config, &s_card);
#endif
    if (error == ESP_FAIL) {
        ESP_LOGE(TAG, "SD card found, but its file system could not be "
                      "mounted; format it as FAT on a computer");
        return error;
    }
    if (error != ESP_OK) {
        ESP_LOGE(TAG,
                 "Could not initialize SD card (%s); check that a card "
                 "is inserted",
                 esp_err_to_name(error));
        return error;
    }

    sdmmc_card_print_info(stdout, s_card);

    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    error = esp_vfs_fat_info(SD_CARD_MOUNT_POINT, &total_bytes, &free_bytes);
    if (error == ESP_OK) {
        ESP_LOGI(TAG,
                 "SD card mounted at %s: %" PRIu64 " MB total, %" PRIu64
                 " MB free",
                 SD_CARD_MOUNT_POINT, total_bytes / (1024 * 1024),
                 free_bytes / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "SD card mounted at %s; free space unknown (%s)",
                 SD_CARD_MOUNT_POINT, esp_err_to_name(error));
    }
    return ESP_OK;
#endif
}
