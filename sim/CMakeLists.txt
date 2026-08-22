#
# PC simulator: compiles the real UI sources (ui.c, ui_common, rvc_protocol,
# panel headers) against the same LVGL the firmware uses (managed_components,
# so run an idf.py build once first to populate it), plus SDL2 and the fake
# bus in sim_stubs.c.
#
# Build via build.ps1, or manually:
#   cmake -G Ninja -B build_mid_coach -DPANEL=mid_coach -DCMAKE_C_COMPILER=<gcc> .
#   cmake --build build_mid_coach
#
cmake_minimum_required(VERSION 3.16)
project(firefly_sim C)

set(PANEL "mid_coach" CACHE STRING "Panel configuration (basename of a header in panels/)")

set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(LVGL_DIR ${REPO}/managed_components/lvgl__lvgl)
set(SDL2_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2-2.32.10/x86_64-w64-mingw32)

if(NOT EXISTS ${LVGL_DIR}/lvgl.h)
    message(FATAL_ERROR "LVGL not found at ${LVGL_DIR} — run an idf.py build first to fetch managed components")
endif()
if(NOT EXISTS ${REPO}/panels/${PANEL}.h)
    message(FATAL_ERROR "Unknown panel '${PANEL}': expected panels/${PANEL}.h")
endif()

# LVGL as a plain static lib, configured by sim/lv_conf.h.
file(GLOB_RECURSE LVGL_SRC ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC
    ${LVGL_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}       # lv_conf.h
    ${SDL2_ROOT}/include              # <SDL2/SDL.h>
)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

add_executable(sim
    main_sim.c
    sim_stubs.c
    ${REPO}/main/ui/ui.c
    ${REPO}/components/ui_common/ui_theme.c
    ${REPO}/components/ui_common/ui_dimmer_button.c
    ${REPO}/components/ui_common/ui_tank_wave.c
    ${REPO}/components/ui_common/ui_battery_summary.c
    ${REPO}/components/ui_common/ui_shore_panel.c
    ${REPO}/components/rvc_protocol/rvc_protocol.c
    ${REPO}/components/jbd_bms/jbd_bms_protocol.c
)
# Order matters: stubs/ must shadow the real ESP headers in main/.
target_include_directories(sim PRIVATE
    stubs
    ${REPO}/main
    ${REPO}/main/ui
    ${REPO}/panels
    ${REPO}/components/ui_common/include
    ${REPO}/components/rvc_protocol/include
    ${REPO}/components/jbd_bms/include
)
target_compile_definitions(sim PRIVATE
    PANEL_CONFIG_HEADER="${PANEL}.h"
    SDL_MAIN_HANDLED
)
target_compile_options(sim PRIVATE -Wall -Wextra)
target_link_directories(sim PRIVATE ${SDL2_ROOT}/lib)
target_link_libraries(sim lvgl SDL2 m)
set_target_properties(sim PROPERTIES OUTPUT_NAME "sim_${PANEL}")

add_custom_command(TARGET sim POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SDL2_ROOT}/bin/SDL2.dll $<TARGET_FILE_DIR:sim>
)
