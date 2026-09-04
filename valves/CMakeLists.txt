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
# ESP-NOW valve command/position frames are deliberately out of scope for
# this scaffold (see docs/DRAINMASTER-VALVES.md open items) — a later issue
# adds espnow_link (and its own rvc_protocol dependency) here. Not listed
# yet: ESP-IDF builds every component it finds via EXTRA_COMPONENT_DIRS
# whether or not anything REQUIRES it, so pulling espnow_link in unused
# would drag its whole WiFi/lwIP dependency tree into this build and force
# main/Kconfig.projbuild to stub out its Kconfig symbols for nothing.
#
# Build:
#   idf.py -C valves -B valves/build set-target esp32s3
#   idf.py -C valves -B valves/build build
#   idf.py -C valves -B valves/build -p COM20 flash monitor
#
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/../components/valve_control"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firefly_valves)
