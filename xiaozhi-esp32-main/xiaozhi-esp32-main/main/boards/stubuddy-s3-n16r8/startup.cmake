# Early requirements expansion imports IDF build properties, not arbitrary
# project variables such as BOARD_NAME. Select identical sources/dependencies
# in both passes, without relying on Kconfig before it has been loaded.
idf_build_get_property(_variant STUBUDDY_VALIDATION_VARIANT)
set(_entry "startup_main.cc")
set(_ptt_variant OFF)
set(_led_variant OFF)
set(_wifi_variant OFF)
set(_opus_variant OFF)
set(_opus_sources "")
set(_opus_requires "")
set(_ws_variant OFF)
set(_ws_sources "")
set(_ws_requires "")
set(_ws_include "")
set(_led_sources "")
set(_led_requires "")
set(_wifi_sources "")
set(_wifi_requires "")
set(_wifi_embed "")
if(_variant STREQUAL "stubuddy-s3-n16r8-ptt" OR
   _variant STREQUAL "stubuddy-s3-n16r8-ptt-led" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wifi-ptt" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wifi-opus-ptt" OR
   _variant STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wss-echo")
    set(_entry "ptt_main.cc")
    set(_ptt_variant ON)
endif()
if(_variant STREQUAL "stubuddy-s3-n16r8-ptt-led" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wifi-ptt" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wifi-opus-ptt" OR
   _variant STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wss-echo")
    set(_led_variant ON)
    set(_led_sources "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/status_led.cc")
    set(_led_requires esp_driver_ledc)
endif()
if(_variant STREQUAL "stubuddy-s3-n16r8-wifi-ptt" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wifi-opus-ptt" OR
   _variant STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wss-echo")
    set(_wifi_variant ON)
    set(_wifi_sources "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/wifi_setup.cc"
                      "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/captive_dns.cc"
                      "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/network_audio.cc")
    set(_wifi_requires esp_wifi esp_netif esp_event esp_http_server lwip)
    set(_wifi_embed "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/setup.html")
endif()
if(_variant STREQUAL "stubuddy-s3-n16r8-wifi-opus-ptt" OR
   _variant STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wss-echo")
    set(_opus_variant ON)
    set(_opus_sources "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/opus_loopback.cc")
    set(_opus_requires espressif__esp_audio_codec)
endif()
if(_variant STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   _variant STREQUAL "stubuddy-s3-n16r8-wss-echo")
    set(_ws_variant ON)
    set(_ws_sources "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/ws_echo.cc")
    set(_ws_requires espressif__esp_websocket_client)
    set(_ws_include "${PROJECT_DIR}/tmp")
    if(_variant STREQUAL "stubuddy-s3-n16r8-wss-echo")
        list(APPEND _ws_requires mbedtls)
    endif()
endif()

idf_component_register(
    SRCS "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/stubuddy_board.cc"
         "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/stubuddy_audio_codec.cc"
         "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/${_entry}"
         "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/startup_audio.cc"
         ${_led_sources} ${_wifi_sources} ${_opus_sources} ${_ws_sources}
         "${COMPONENT_DIR}/boards/common/board.cc" "${COMPONENT_DIR}/audio/audio_codec.cc" "${COMPONENT_DIR}/settings.cc"
    INCLUDE_DIRS "${COMPONENT_DIR}" "${COMPONENT_DIR}/boards/common" "${COMPONENT_DIR}/audio" "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8" ${_ws_include}
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2s esp_psram spi_flash esp_timer nvs_flash
                  esp_app_format ${_led_requires} ${_wifi_requires} ${_opus_requires} ${_ws_requires}
    EMBED_TXTFILES ${_wifi_embed}
    WHOLE_ARCHIVE
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CONFIG_BOARD_TYPE_STUBUDDY_S3_N16R8 OR NOT CONFIG_XIAOZHI_HEADLESS_STARTUP)
        message(FATAL_ERROR "The startup variant requires the stubuddy S3 N16R8 board")
    endif()
    set(_identity_file "${CMAKE_CURRENT_LIST_DIR}/config.json")
    file(READ "${_identity_file}" _identity)
    string(JSON _board_type GET "${_identity}" type)
    if((NOT BOARD_NAME STREQUAL "stubuddy-s3-n16r8-startup" AND NOT _ptt_variant) OR
       NOT _board_type STREQUAL "stubuddy-s3-n16r8")
        message(FATAL_ERROR "Invalid stubuddy startup board identity")
    endif()
    if((_ptt_variant AND NOT CONFIG_STUBUDDY_PTT_SELFTEST) OR
       (NOT _ptt_variant AND CONFIG_STUBUDDY_PTT_SELFTEST) OR
       (_led_variant AND NOT CONFIG_STUBUDDY_STATUS_LED) OR
       (NOT _led_variant AND CONFIG_STUBUDDY_STATUS_LED) OR
       (_wifi_variant AND NOT CONFIG_STUBUDDY_SOFTAP_SETUP) OR
       (NOT _wifi_variant AND CONFIG_STUBUDDY_SOFTAP_SETUP) OR
       (_opus_variant AND NOT CONFIG_STUBUDDY_OPUS_LOOPBACK) OR
       (NOT _opus_variant AND CONFIG_STUBUDDY_OPUS_LOOPBACK) OR
       (_ws_variant AND NOT CONFIG_STUBUDDY_WS_ECHO) OR
       (NOT _ws_variant AND CONFIG_STUBUDDY_WS_ECHO))
        message(FATAL_ERROR "stubuddy variant and PTT Kconfig disagree; use scripts/build.py")
    endif()
    if((_variant STREQUAL "stubuddy-s3-n16r8-wss-echo" AND NOT CONFIG_STUBUDDY_WSS_ECHO) OR
       (NOT _variant STREQUAL "stubuddy-s3-n16r8-wss-echo" AND CONFIG_STUBUDDY_WSS_ECHO))
        message(FATAL_ERROR "stubuddy TLS variant disagrees with Kconfig; use scripts/build.py")
    endif()
    target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BOARD_TYPE="${_board_type}" BOARD_NAME="${BOARD_NAME}" BOARD_MANUFACTURER="stubuddy")
endif()
