cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3" CACHE STRING "Target for the stubuddy N16R8 hardware")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/config/selftest.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)

set(PROJECT_VER "0.4.3")
project(stubuddy_speaker_selftest)
