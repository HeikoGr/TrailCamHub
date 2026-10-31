# Commands on the serial console; empty unless enabled.
set(srcs)
if(CONFIG_DEBUG_CONSOLE_ENABLE)
    list(APPEND srcs "debug_console.cpp")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES console esp_driver_usb_serial_jtag
                                     esp_rom esp_timer fatfs heap mbedtls
                                     sd_log sd_storage)
