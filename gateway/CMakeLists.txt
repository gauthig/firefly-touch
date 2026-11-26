#
# firefly-gateway — the stool-room RV-C gateway (architecture v2, epic #98).
#
# A SEPARATE ESP-IDF project from the panel firmware, like proxy/ and valves/:
# headless, its own chip config, and it pulls shared components individually
# rather than pointing at components/ (which would drag board, ui_common and
# LVGL into a build with no display).
#
# Hardware: Waveshare ESP32-S3-RS485-CAN (-U, external antenna). ESP32-S3 with
# 8 MB embedded octal PSRAM and 16 MB flash; the USB-C port is the chip's
# native USB-Serial/JTAG (VID 303A), so there is no UART bridge.
#
# Current image: the SIX-LINK test (#98 step 2b, issue #105) -- all six BLE
# devices held from this one board, CAN listen-only, no ESP-NOW, publishes
# nothing. Run it with the basement proxy and hvac_panel powered off.
#
# Build:
#   idf.py -C gateway -B gateway/build set-target esp32s3     # once
#   idf.py -C gateway -B gateway/build build
#   idf.py -C gateway -B gateway/build -p COM4 flash
#
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ble_host"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/bus_survey"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/easytouch"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/hughes_watchdog"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/jbd_bms"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/renogy_solar"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/rvc_protocol"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firefly_gateway)
