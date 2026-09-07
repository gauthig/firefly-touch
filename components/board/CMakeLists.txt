# One component, one board compiled.
#
# The board is chosen by BOARD, which the root CMakeLists.txt derives from
# PANEL. Only the SOURCE selection is conditional here -- REQUIRES is a
# constant list -- and that is deliberate:
#
#   ESP-IDF resolves the component dependency graph in an "early expansion"
#   pass that runs this file BEFORE the project's variables exist. Anything
#   conditional on BOARD is therefore evaluated with BOARD unset during that
#   pass. An earlier attempt used two components that each registered empty
#   unless selected; the effect was that every REQUIRES silently evaluated to
#   nothing, and the build failed with "board.h: No such file" while the
#   CMake looked correct. SRCS and INCLUDE_DIRS are read in the real pass,
#   where BOARD is set, so switching sources this way is safe.
if(BOARD STREQUAL "lcd7")
    set(board_src "board_lcd7.c")
elseif(BOARD STREQUAL "lcd7b")
    set(board_src "board_lcd7b.c")
else()
    set(board_src "board_4_3b.c")
endif()

idf_component_register(
    SRCS "${board_src}"
    INCLUDE_DIRS "include"
    # Constant list on purpose -- see the note above. ch422g is the 4.3B /
    # non-B 7" IO expander; ws_io_expander is the 7B's (a different chip).
    # Whichever board is compiled links only the one it #includes; the other
    # is dead-stripped.
    REQUIRES driver esp_lcd ch422g ws_io_expander
)

# Picks which header include/board.h forwards to. PUBLIC so dependents (main,
# which includes board.h) see it too.
if(BOARD STREQUAL "lcd7")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC BOARD_LCD7=1)
elseif(BOARD STREQUAL "lcd7b")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC BOARD_LCD7B=1)
endif()

# hvac_panel's thermostat screen is the deepest object tree in the project and
# overflows the default 8 KiB LVGL task stack during render. Give those panels
# a large LVGL task stack placed in PSRAM: zero internal-DRAM cost (hvac_panel
# is tight there with Bluedroid + WiFi), and it frees the 8 KiB the stack
# otherwise takes from internal RAM. bedroom_remote opts in too -- same 4.3B
# board, comparable screen depth, and it may host the thermostat readout later.
# PANEL is set in this (real) pass, like the UI_METRICS_LARGE gate in
# components/ui_common/CMakeLists.txt.
if(PANEL STREQUAL "hvac_panel" OR PANEL STREQUAL "bedroom_remote")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BOARD_LVGL_TASK_STACK=24576
        BOARD_LVGL_TASK_STACK_PSRAM=1)
endif()
