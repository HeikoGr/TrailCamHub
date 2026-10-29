#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

/*
 * Mounts the FAT file system of the SD card at SD_CARD_MOUNT_POINT using the
 * SDMMC peripheral in 1-bit mode or, with CONFIG_SD_CARD_SPI_MODE, on the
 * SPI2 bus.
 * The card is never formatted automatically.
 */
esp_err_t sd_card_mount(void);

/*
 * True once sd_card_mount() has set up the SPI2 bus in SPI mode (even if no
 * card was found), so that other devices on the bus can use it without
 * initializing it again.
 */
bool sd_card_spi_bus_ready(void);
