#
# firefly-hvac-capture — read-only bench capture tool for the Micro-Air
# EasyTouch RV thermostat (issue #70).
#
# A SEPARATE ESP-IDF project from the panel firmware, same reasoning as
# proxy/CMakeLists.txt and valves/CMakeLists.txt: it has no display, no
# touch, no button grid, none of panels/*.h applies, and pulling in
# components/ as a whole would drag board + ui_common (and therefore LVGL)
# into a build that never draws anything.
#
# Deliberately READ-ONLY: it authenticates and polls status, and never
# writes a "Change" command, so it cannot accidentally toggle the A/C or
# heat while capturing. Mode/fan changes for the capture are made by hand
# on the physical touchscreen while this logs the resulting status.
#
# Board: spare ESP32-S3-Touch-LCD-4.3B on COM23 (any ESP32-S3 works; no
# display/touch/CAN pins are touched by this firmware at all).
#
# Build:
#   idf.py -C hvac_capture -B hvac_capture/build set-target esp32s3
#   idf.py -C hvac_capture -B hvac_capture/build build
#   idf.py -C hvac_capture -B hvac_capture/build -p COM23 flash monitor
#
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ble_host"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firefly_hvac_capture)
