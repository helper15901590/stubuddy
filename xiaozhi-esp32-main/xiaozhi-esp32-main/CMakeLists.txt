# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# Make the release variant available during IDF's early dependency expansion.
# Kconfig alone is not available in that pass. Other variants keep the full app.
set(ENV{XIAOZHI_HEADLESS_STARTUP_PROFILE} "0")
set(ENV{XIAOZHI_STUBUDDY_WIFI_PROFILE} "0")
set(ENV{XIAOZHI_STUBUDDY_OPUS_PROFILE} "0")
set(ENV{XIAOZHI_STUBUDDY_WS_PROFILE} "0")
idf_build_set_property(XIAOZHI_HEADLESS_STARTUP OFF)
if(BOARD_NAME STREQUAL "stubuddy-s3-n16r8-startup" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-ptt" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-ptt-led" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wifi-ptt" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wifi-opus-ptt" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wss-echo" OR
   BOARD_NAME MATCHES "^stubuddy-s3-n16r8-ai-ptt(-controls)?$")
    idf_build_set_property(XIAOZHI_HEADLESS_STARTUP ON)
    idf_build_set_property(STUBUDDY_VALIDATION_VARIANT "${BOARD_NAME}")
    set(ENV{XIAOZHI_HEADLESS_STARTUP_PROFILE} "1")
endif()
if(BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wifi-ptt" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wifi-opus-ptt" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wss-echo" OR
   BOARD_NAME MATCHES "^stubuddy-s3-n16r8-ai-ptt(-controls)?$")
    set(ENV{XIAOZHI_STUBUDDY_WIFI_PROFILE} "1")
endif()
if(BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wifi-opus-ptt" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wss-echo" OR
   BOARD_NAME MATCHES "^stubuddy-s3-n16r8-ai-ptt(-controls)?$")
    set(ENV{XIAOZHI_STUBUDDY_OPUS_PROFILE} "1")
endif()
if(BOARD_NAME STREQUAL "stubuddy-s3-n16r8-ws-echo" OR
   BOARD_NAME STREQUAL "stubuddy-s3-n16r8-wss-echo" OR
   BOARD_NAME MATCHES "^stubuddy-s3-n16r8-ai-ptt(-controls)?$")
    set(ENV{XIAOZHI_STUBUDDY_WS_PROFILE} "1")
endif()

set(PROJECT_VER "2.5.2")
project(xiaozhi)
