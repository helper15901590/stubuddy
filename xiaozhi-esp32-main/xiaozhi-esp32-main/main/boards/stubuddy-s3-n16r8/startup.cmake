idf_component_register(
    SRCS "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/stubuddy_board.cc"
         "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/stubuddy_audio_codec.cc"
         "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8/startup_main.cc"
         "${COMPONENT_DIR}/boards/common/board.cc" "${COMPONENT_DIR}/audio/audio_codec.cc" "${COMPONENT_DIR}/settings.cc"
    INCLUDE_DIRS "${COMPONENT_DIR}" "${COMPONENT_DIR}/boards/common" "${COMPONENT_DIR}/audio" "${COMPONENT_DIR}/boards/stubuddy-s3-n16r8"
    PRIV_REQUIRES esp_driver_gpio esp_driver_i2s esp_psram spi_flash esp_timer nvs_flash
                  esp_app_format
    WHOLE_ARCHIVE
)

if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CONFIG_BOARD_TYPE_STUBUDDY_S3_N16R8 OR NOT CONFIG_XIAOZHI_HEADLESS_STARTUP)
        message(FATAL_ERROR "The startup variant requires the stubuddy S3 N16R8 board")
    endif()
    set(_identity_file "${CMAKE_CURRENT_LIST_DIR}/config.json")
    file(READ "${_identity_file}" _identity)
    string(JSON _board_type GET "${_identity}" type)
    if(NOT BOARD_NAME STREQUAL "stubuddy-s3-n16r8-startup" OR
       NOT _board_type STREQUAL "stubuddy-s3-n16r8")
        message(FATAL_ERROR "Invalid stubuddy startup board identity")
    endif()
    target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BOARD_TYPE="${_board_type}" BOARD_NAME="${BOARD_NAME}" BOARD_MANUFACTURER="stubuddy")
endif()
