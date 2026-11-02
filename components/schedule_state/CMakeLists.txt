# The schedule at run time: state across deep sleep, settings from the
# backend, the hub and the build.
idf_component_register(SRCS "schedule_state.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES schedule
                       PRIV_REQUIRES esp_system esp_timer hub_config nvs_flash
                                     time_sync)
