idf_component_register(SRCS "camera_protocol.cpp" "camera_prelogin.cpp"
                            "camera_udp_packets.cpp" "camera_udp_transport.cpp"
                            "pppp_session.cpp"
                            "artemis_aes128.cpp" "artemis_crypto.cpp"
                            "artemis_json.cpp" "artemis_message.cpp"
                            "artemis_login.cpp" "artemis_reassembler.cpp"
                            "media_list.cpp" "media_transfer.cpp"
                            "bulk_reassembler.cpp" "bulk_stream.cpp"
                            "frame_reorder.cpp" "playback_decoder.cpp" "playback_record.cpp"
                            "playback_stream.cpp"
                            "device_info.cpp"
                            "media_catalog.cpp" "media_store.cpp" "sd_writer.cpp"
                            "task_load.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_app_format esp_ringbuf esp_timer esp_wifi lwip mbedtls
                                     sd_storage)
