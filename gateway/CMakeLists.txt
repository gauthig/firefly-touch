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
# This first image is BRING-UP ONLY (#98 step 2a): it reports the board,
# listens on CAN without ever transmitting, and scans BLE (scan requests only,
# no connections), so it can run beside the proxy and hvac_panel.
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
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/rvc_protocol"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firefly_gateway)
