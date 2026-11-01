idf_component_register(
    SRCS "speaker_selftest.cc" "status_led.cc" "wifi_setup.cc" "captive_dns.cc"
         "../tests/ptt_button_test.cc" "../tests/led_pattern_test.cc" "../tests/wifi_rules_test.cc"
         "../tests/wifi_history_test.cc" "../tests/captive_dns_test.cc"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2s esp_psram spi_flash esp_timer esp_driver_ledc
                  esp_wifi esp_netif esp_event esp_http_server nvs_flash lwip
    EMBED_TXTFILES "setup.html"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
