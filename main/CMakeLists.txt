idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES camera_ble camera_link coprocessor_update debug_console diagnostics
                                     dongle_ui esp_event
                                     esp_netif ethernet_uplink nvs_flash
                                     sd_log sd_storage session_controller
                                     web_ui)
