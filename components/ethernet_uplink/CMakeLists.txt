# Only boards with an Ethernet controller (on-chip MAC on the ESP32-P4, W5500 on
# the ESP32-S3-ETH) build the driver.
set(srcs)
if(CONFIG_ETH_UPLINK_ENABLE)
    list(APPEND srcs "ethernet_uplink.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_gpio esp_driver_spi esp_eth
                                     esp_event esp_hw_support esp_netif)
