cmake_minimum_required(VERSION 3.16)

# Panel selection: idf.py -DPANEL=mid_coach build   (or ent_center, ...)
# Each PANEL value must have a matching header in panels/<name>.h.
# NOTE: use a separate build dir per panel (idf.py -B build_mid_coach ...)
# or run `idf.py fullclean` when switching panels — the value is cached.
set(PANEL "mid_coach" CACHE STRING "Panel configuration (basename of a header in panels/)")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/panels/${PANEL}.h")
    message(FATAL_ERROR "Unknown panel '${PANEL}': expected panels/${PANEL}.h to exist")
endif()

# ---------------------------------------------------------------------------
# Board selection is DERIVED from the panel, never passed on the command line.
#
# The boards differ in ways that fail silently rather than loudly: the
# Waveshare 7" puts CAN on GPIO20/19 where the 4.3B has RS485, so a panel
# built for the wrong board comes up looking healthy with a dead bus. Making
# BOARD a function of PANEL means there is no -DBOARD= to forget or mistype.
#
# Add a line here when adding a panel on a non-default board;
# tools/check_panels.py cross-checks this against panels/REGISTRY.md.
# ---------------------------------------------------------------------------
set(PANEL_BOARD_main_cabinet "lcd7b")

if(DEFINED PANEL_BOARD_${PANEL})
    set(_panel_board "${PANEL_BOARD_${PANEL}}")
else()
    set(_panel_board "4_3b")
endif()
set(BOARD "${_panel_board}" CACHE STRING "Display board component (derived from PANEL)" FORCE)
message(STATUS "firefly-touch: panel '${PANEL}' on board '${BOARD}'")

# ---------------------------------------------------------------------------
# Per-panel sdkconfig defaults, layered on top of the shared
# sdkconfig.defaults when a build dir's sdkconfig is first generated. IDF
# applies these only when no sdkconfig exists yet, so existing build dirs
# (and their in-place secret edits) are untouched.
#
# hvac_panel needs the BLE 4.2 host stack — components/ble_host's legacy GAP
# scan calls are not linked under the S3 default BT_BLE_50_FEATURES_SUPPORTED
# — plus BT/WiFi RAM tuning to fit Bluedroid + WiFi + LVGL in internal RAM.
# CI builds every panel from a fresh sdkconfig, so this must be committed,
# not merely hand-edited into build_hvac_panel/sdkconfig.
# ---------------------------------------------------------------------------
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/panels/sdkconfig.${PANEL}.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "panels/sdkconfig.${PANEL}.defaults")
    message(STATUS "firefly-touch: + panels/sdkconfig.${PANEL}.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firefly_touch)
