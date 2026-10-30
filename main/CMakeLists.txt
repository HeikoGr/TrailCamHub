idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES diagnostics hub_console nvs_flash
                                     sd_storage session_controller)
