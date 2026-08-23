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
else()
    set(board_src "board_4_3b.c")
endif()

idf_component_register(
    SRCS "${board_src}"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd ch422g
)

# Picks which header include/board.h forwards to. PUBLIC so dependents (main,
# which includes board.h) see it too.
if(BOARD STREQUAL "lcd7")
    target_compile_definitions(${COMPONENT_LIB} PUBLIC BOARD_LCD7=1)
endif()
