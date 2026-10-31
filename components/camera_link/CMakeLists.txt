idf_component_register(SRCS "camera_protocol.cpp" "camera_prelogin.cpp"
                            "camera_udp_packets.cpp" "camera_udp_transport.cpp"
                            "pppp_session.cpp"
                            "artemis_aes128.cpp" "artemis_crypto.cpp"
                            "artemis_json.cpp" "artemis_message.cpp"
                            "artemis_login.cpp" "artemis_reassembler.cpp"
                            "media_list.cpp" "media_transfer.cpp"
                            "bulk_reassembler.cpp" "device_info.cpp"
                            "media_catalog.cpp" "media_store.cpp"
                       # "." exposes the pure protocol/catalog headers to the
                       # uplink component.
                       INCLUDE_DIRS "include" "."
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_timer lwip mbedtls sd_storage)
