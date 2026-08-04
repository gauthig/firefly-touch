cmake_minimum_required(VERSION 3.16)

# Panel selection: idf.py -DPANEL=living_room build   (or ent_center)
# Each PANEL value must have a matching header in panels/<name>.h.
# NOTE: use a separate build dir per panel (idf.py -B build_living_room ...)
# or run `idf.py fullclean` when switching panels — the value is cached.
set(PANEL "living_room" CACHE STRING "Panel configuration (basename of a header in panels/)")

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/panels/${PANEL}.h")
    message(FATAL_ERROR "Unknown panel '${PANEL}': expected panels/${PANEL}.h to exist")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firefly_touch)
