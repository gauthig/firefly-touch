#
# firefly-valves — headless dump-valve relay controller.
#
# A SEPARATE ESP-IDF project from the panel firmware at the repo root, same
# reasoning as proxy/CMakeLists.txt:
#   1. It is not a panel. No display, no touch, no button grid, no RV-C
#      source address — none of panels/*.h applies to it.
#   2. It pulls shared components individually rather than pointing at
#      components/ as a whole: that would drag board and ui_common (and
#      therefore LVGL) into a build that has no display at all.
#
# Unlike proxy/, this board IS an ESP32-S3 (confirmed via `esptool flash-id`
# against the real board: ESP32-S3, 16 MB flash, 8 MB PSRAM — same profile
# as the panels), so it shares the panels' target rather than proxy's
# classic-ESP32 one.
#
# espnow_link (and its own rvc_protocol dependency, for the shared
# id-pack/dimmer-cmd types it still references even though this project
# never sends a dimmer command) receives real valve command/position frames
# now -- see valve_drive.c. Both are listed here for the same reason
# proxy/CMakeLists.txt lists its shared components individually.
#
# Build:
#   idf.py -C valves -B valves/build set-target esp32s3
#   idf.py -C valves -B valves/build build
#   idf.py -C valves -B valves/build -p COM20 flash monitor
#
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/espnow_link"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/rvc_protocol"
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/valve_control"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firefly_valves)
