idf_component_register(
    SRCS "speaker_selftest.cc" "status_led.cc"
         "../tests/ptt_button_test.cc" "../tests/led_pattern_test.cc"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2s esp_psram spi_flash esp_timer esp_driver_ledc
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
