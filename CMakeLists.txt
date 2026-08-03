cmake_minimum_required(VERSION 3.16)
project(led_display LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(EXT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rpi-rgb-led-matrix-extensions")
set(RGB_DIR "${EXT_DIR}/third_party/rpi-rgb-led-matrix")
set(JSON_DIR "${EXT_DIR}/third_party/json")

foreach(d "${RGB_DIR}/include" "${JSON_DIR}")
  if(NOT EXISTS "${d}")
    message(FATAL_ERROR "Missing ${d}. Run: git submodule update --init --recursive")
  endif()
endforeach()

# ---------------------------------------------------------------------------
# led_common -- shm transport, gamma, dither. No GPIO, no graphics: portable,
# so the maths can be unit-tested on a laptop with no panel attached.
# ---------------------------------------------------------------------------
add_library(led_common STATIC
  src/common/frame_shm.cpp
  src/common/gamma.cpp
  src/common/dither.cpp
)
target_include_directories(led_common PUBLIC src/common)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(led_common PUBLIC rt pthread)   # shm_open lives in librt
endif()

# ---------------------------------------------------------------------------
# led_gfx -- text and font rendering only. graphics.cc and bdf-font.cc from
# rpi-rgb-led-matrix pull in no GPIO code (verified: they include only
# graphics.h/utf8-internal.h and libc), so the renderer links these directly
# instead of librgbmatrix and stays buildable off the Pi.
# ---------------------------------------------------------------------------
add_library(led_gfx STATIC
  ${RGB_DIR}/lib/graphics.cc
  ${RGB_DIR}/lib/bdf-font.cc
  ${EXT_DIR}/src/scrolling_textbox.cpp
  ${EXT_DIR}/src/scrolling_box.cpp
)
target_include_directories(led_gfx PUBLIC
  ${RGB_DIR}/include
  ${RGB_DIR}/lib
  ${EXT_DIR}/include
  ${JSON_DIR}
)
target_compile_options(led_gfx PRIVATE -Wno-unused-but-set-variable)

# ---------------------------------------------------------------------------
# librgbmatrix -- the real driver library. Built by its own Makefile; Linux and
# a Pi only, so the driver target is guarded on it.
# ---------------------------------------------------------------------------
set(RGB_LIB "${RGB_DIR}/lib/librgbmatrix.a")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_custom_command(
    OUTPUT ${RGB_LIB}
    COMMAND ${CMAKE_MAKE_PROGRAM} -C ${RGB_DIR}/lib
    COMMENT "Building librgbmatrix"
    VERBATIM)
  add_custom_target(rgbmatrix_build DEPENDS ${RGB_LIB})
  add_library(rgbmatrix STATIC IMPORTED GLOBAL)
  set_target_properties(rgbmatrix PROPERTIES IMPORTED_LOCATION ${RGB_LIB})
  add_dependencies(rgbmatrix rgbmatrix_build)
endif()

# ---------------------------------------------------------------------------
# libpq, for the renderer's departure fetcher. No pkg-config on the Pi, so
# fall back to pg_config the way the old Makefile did.
# ---------------------------------------------------------------------------
find_program(PG_CONFIG pg_config)
if(PG_CONFIG)
  execute_process(COMMAND ${PG_CONFIG} --includedir OUTPUT_VARIABLE PQ_INC
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${PG_CONFIG} --libdir OUTPUT_VARIABLE PQ_LIBDIR
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
find_path(PQ_INCLUDE_DIR libpq-fe.h HINTS ${PQ_INC} /usr/include/postgresql /usr/local/include)
find_library(PQ_LIBRARY NAMES pq HINTS ${PQ_LIBDIR} /usr/lib /usr/local/lib)

# ===========================================================================
# Targets
# ===========================================================================

# --- driver: Linux + Pi only -----------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(led_driver
    src/driver/main.cpp
    src/driver/driver_config.cpp
    src/driver/rgbmatrix_backend.cpp
    src/driver/hot_loop.cpp
    src/driver/rt_setup.cpp
  )
  target_include_directories(led_driver PRIVATE
    ${RGB_DIR}/include ${EXT_DIR}/include ${JSON_DIR})
  target_link_libraries(led_driver PRIVATE led_common rgbmatrix pthread rt)
else()
  message(STATUS "led_driver skipped: needs Linux + rpi-rgb-led-matrix")
endif()

# --- renderer: portable ----------------------------------------------------
set(RENDERER_SRC
  src/renderer/main.cpp
  src/renderer/renderer_config.cpp
  src/renderer/layout_config.cpp
  src/renderer/layout_watcher.cpp
  src/renderer/cpp_backend.cpp
  src/renderer/departures.cpp
  src/renderer/data/db_fetcher.cpp
)
add_executable(led_renderer ${RENDERER_SRC})
target_include_directories(led_renderer PRIVATE src/renderer ${JSON_DIR})
target_link_libraries(led_renderer PRIVATE led_common led_gfx)
if(PQ_INCLUDE_DIR AND PQ_LIBRARY)
  target_include_directories(led_renderer PRIVATE ${PQ_INCLUDE_DIR})
  target_link_libraries(led_renderer PRIVATE ${PQ_LIBRARY})
  target_compile_definitions(led_renderer PRIVATE HAVE_LIBPQ=1)
else()
  message(WARNING "libpq not found -- renderer builds without live data (--demo only)")
endif()

# --- shmgen: synthetic frames, so the driver can be validated alone --------
add_executable(led_shmgen src/tools/shmgen.cpp)
target_link_libraries(led_shmgen PRIVATE led_common)

# --- tests: portable, no panel needed --------------------------------------
add_executable(led_test_common src/tests/test_common.cpp)
target_link_libraries(led_test_common PRIVATE led_common)
enable_testing()
add_test(NAME common COMMAND led_test_common)
