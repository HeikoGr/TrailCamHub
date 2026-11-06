#pragma once

#include "esp_err.h"

/*
 * Starts the board's Ethernet controller (the on-chip MAC with its PHY, or a
 * W5500 on SPI) and requests an address over DHCP. Link and address changes are
 * logged. The interface has a higher route priority than the camera Wi-Fi, so
 * it stays the default route while the camera network is joined; the camera
 * remains reachable through its own subnet.
 *
 * Requires esp_netif_init() and the default event loop.
 */
esp_err_t ethernet_uplink_start(void);
