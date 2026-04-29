idf_component_register(
    SRCS
        "app_main.cpp"
        "board_audio.cpp"
        "board_status.cpp"
        "device_identity.cpp"
        "gateway_client.cpp"
        "hermes_media.cpp"
        "hermes_webrtc.cpp"
        "timer_stub.cpp"
        "wake_word.cpp"
        "wifi_manager.cpp"
    INCLUDE_DIRS "."
    REQUIRES driver esp_event esp_http_client esp_lcd esp_lcd_st7789 esp_netif esp_psram esp_wifi esp_codec_dev esp-libopus esp-sr json nvs_flash peer
)

idf_component_get_property(peer_lib peer COMPONENT_LIB)
target_compile_options(${peer_lib} PRIVATE -Wno-error=restrict -Wno-error=stringop-truncation)

idf_component_get_property(srtp_lib srtp COMPONENT_LIB)
target_compile_options(${srtp_lib} PRIVATE -Wno-error=incompatible-pointer-types)

idf_component_get_property(opus_lib esp-libopus COMPONENT_LIB)
target_compile_options(${opus_lib} PRIVATE -Wno-error=maybe-uninitialized -Wno-error=stringop-overread)
