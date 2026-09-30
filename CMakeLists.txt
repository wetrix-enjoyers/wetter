cmake_minimum_required(VERSION 3.16)
project(wetter LANGUAGES CXX)

option(WETTER_SOFT "Build the software renderer" ON)
option(WETTER_HARD "Build the GPU renderer (OpenGL ES 3.0 / GL 3.3; the host provides the context)" ON)
option(WETTER_DEBUGINFO "Line tables for the renderer sources (for tools/soft_prof.py)" OFF)

if(NOT WETTER_SOFT AND NOT WETTER_HARD)
    message(FATAL_ERROR "wetter: no backend enabled (WETTER_SOFT, WETTER_HARD)")
endif()

add_library(wetter STATIC)
target_include_directories(wetter PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
                                  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(wetter PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(wetter PUBLIC Threads::Threads)

# What every backend shares: the display-list walker, the RSP, the GBI tables,
# the host's hooks, and the backend factory.
target_sources(wetter PRIVATE
    src/renderer.cpp
    src/hooks.cpp
    src/front/frontend.cpp
    src/front/gbi.cpp
    src/front/rsp.cpp
    src/front/tmem.cpp)

if(WETTER_SOFT)
    target_compile_definitions(wetter PRIVATE WETTER_HAS_SOFT=1)
    target_sources(wetter PRIVATE
        src/soft/soft_renderer.cpp
        src/soft/rdp.cpp
        src/soft/rdp_farm.cpp)
    # The RDP's span stages are written for the vectorizer; SSE4.1 gives it
    # 32-bit multiplies. ARM targets need no flag (NEON is baseline).
    if(NOT MSVC AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64|arm")
        set_property(SOURCE src/soft/rdp.cpp APPEND PROPERTY COMPILE_OPTIONS -msse4.1)
    endif()
endif()

if(WETTER_HARD)
    # GL comes from the host's context through gl_get_proc_address: nothing to link.
    target_compile_definitions(wetter PRIVATE WETTER_HAS_HARD=1)
    target_sources(wetter PRIVATE
        src/hard/hard_renderer.cpp
        src/hard/hard_rdp.cpp
        src/hard/gl.cpp)
endif()

if(WETTER_DEBUGINFO AND NOT MSVC)
    target_compile_options(wetter PRIVATE -g)
endif()
