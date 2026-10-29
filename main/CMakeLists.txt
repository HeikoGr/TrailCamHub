idf_component_register(SRCS "main.cpp"
                       PRIV_REQUIRES diagnostics esp_event esp_netif
                                     ethernet_uplink nvs_flash sd_storage
                                     session_controller)
