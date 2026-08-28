#
# firefly-ble-proxy — headless BLE-to-ESP-NOW telemetry bridge.
#
# A SEPARATE ESP-IDF project from the panel firmware at the repo root, for
# two reasons that both matter:
#   1. Different chip. The panels are ESP32-S3; this runs on a classic
#      ESP32, so it needs its own target and sdkconfig. (BLE 4.2 vs 5.0 is
#      not just a config difference -- see try_connect() in
#      components/jbd_bms/jbd_bms_client.c, which has to pick a different
#      connect API per chip.)
#   2. It is not a panel. No display, no touch, no button grid, no RV-C
#      source address — none of panels/*.h applies to it.
#
# It reuses the shared components by pointing at them individually rather
# than at components/ as a whole: pulling in the whole directory would drag
# board and ui_common (and therefore LVGL) into a build that has no
# display at all.
#
# Build:
#   idf.py -C proxy -B proxy/build set-target esp32
#   idf.py -C proxy -B proxy/build build
#   idf.py -C proxy -B proxy/build -p COM4 flash monitor
#
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/ble_host"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/espnow_link"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/hughes_watchdog"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/jbd_bms"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/renogy_solar"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/rvc_protocol"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firefly_ble_proxy)
