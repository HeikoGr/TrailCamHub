idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES camera_link diagnostics hub_console nvs_flash
                                     sd_storage session_controller)
