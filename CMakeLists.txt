cmake_minimum_required(VERSION 3.20)
project(dsd-neo)

SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${PROJECT_SOURCE_DIR}/cmake/")
# Prefer ncurses wide-character support
set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)

# Language and build defaults
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Modern build options (aligned with mbelib-neo style)
option(DSD_ENABLE_WARNINGS "Enable common warning flags" ON)
option(DSD_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(DSD_ENABLE_FAST_MATH "Enable fast-math optimizations (may relax IEEE semantics)" OFF)
option(DSD_ENABLE_LTO "Enable Link Time Optimization (IPO/LTO) for Release builds" OFF)
option(DSD_ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)
option(DSD_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer in Debug builds" OFF)

#use cmake option -DCOLORS=OFF to disable color output (ncurses)
option(COLORS
    "Build with Colors Enabled" ON)
if (COLORS)
    add_definitions(-DPRETTY_COLORS)
endif ()

#use cmake option -DCOLORSLOGS=OFF to disable color output (terminal/logs)
option(COLORSLOGS
    "Enable colored terminal/log output" ON)
if (COLORSLOGS)
    add_definitions(-DPRETTY_COLORS_LOGS)
endif ()

#use cmake option -DPVC=ON to enable Provoice Conventional Frame Sync
option(PVC
    "Build with Provoice Conventional Frame Sync Enabled" OFF)
if (PVC)
    add_definitions(-DPVCONVENTIONAL)
endif ()


#use cmake option -DLZ=ON to enable LimaZulu Requested NXDN Tweaks
option(LZ
    "Build with new LimaZulu Requested NXDN Tweaks Enabled" OFF)
if (LZ)
    add_definitions(-DLIMAZULUTWEAKS)
endif ()

#use cmake option -DSID=ON to enable P25p1 Soft ID decoding -- Experimental
option(SID
    "Build with P25p1 LSD/Soft ID Enabled" OFF)
if (SID)
    add_definitions(-DSOFTID)
endif ()


include(git_revision)
# Gather git info: semantic-like tag and short hash
get_git_head_revision(GIT_REFSPEC GIT_HASH)
git_describe(GIT_TAG --tags --abbrev=0)

# Sanitize/fallbacks for non-git tarballs or archives
if("${GIT_TAG}" MATCHES "NOTFOUND" OR "${GIT_TAG}" STREQUAL "")
    set(GIT_TAG "0.0.0")
endif()
if("${GIT_HASH}" MATCHES "NOTFOUND" OR "${GIT_HASH}" STREQUAL "")
    set(GIT_HASH "unknown")
else()
    string(SUBSTRING "${GIT_HASH}" 0 7 GIT_HASH_SHORT)
    set(GIT_HASH "${GIT_HASH_SHORT}")
endif()

# Dependencies
find_package(LibSndFile REQUIRED)

# Prefer modern mbelib-neo CMake package; fall back to legacy FindMBE
set(USE_MBELIB_NEO FALSE)
find_package(mbe-neo CONFIG QUIET)
if(mbe-neo_FOUND)
    set(USE_MBELIB_NEO TRUE)
    if(WIN32)
        set(MBE_LINK_TARGET mbe_neo::mbe_static)
    else()
        set(MBE_LINK_TARGET mbe_neo::mbe_shared)
    endif()
    # Ensure includes for legacy-style #include <mbelib.h>
    # by adding the directory that directly contains mbelib.h
    find_path(MBE_INCLUDE_DIR NAMES mbelib.h PATH_SUFFIXES mbelib-neo)
else()
    find_package(MBE REQUIRED)
    set(MBE_LINK_TARGET ${MBE_LIBRARY})
endif()
find_package(ITPP REQUIRED)
find_package(RTLSDR)
find_package(Curses REQUIRED)
find_package(PulseAudio REQUIRED)
find_package(CODEC2)

# Aggregate include directories; when using the modern package, we still add
# the mbelib-neo leaf include directory so #include <mbelib.h> continues to work.
set(_PUBLIC_INCLUDES ${LIBSNDFILE_INCLUDE_DIR} ${ITPP_INCLUDE_DIR} ${PULSEAUDIO_INCLUDE_DIRS} ${CURSES_INCLUDE_DIRS})
if(MBE_INCLUDE_DIR)
    list(APPEND _PUBLIC_INCLUDES ${MBE_INCLUDE_DIR})
endif()

set(LIBS ${MBE_LINK_TARGET} ${LIBSNDFILE_LIBRARY} ${ITPP_LIBRARY} ${PULSEAUDIO_SIMPLE_LIBRARY} ${PULSEAUDIO_LIBRARY} ${CURSES_LIBRARIES})

if(RTLSDR_FOUND)
    find_package(Threads)
    list(APPEND LIBS ${RTLSDR_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
    add_definitions(-DUSE_RTLSDR)
endif(RTLSDR_FOUND)

if(CODEC2_FOUND)
    list(APPEND LIBS ${CODEC2_LIBRARIES})
    add_definitions(-DUSE_CODEC2)
endif(CODEC2_FOUND)

## Executable target is defined in apps/dsd-cli

# uninstall target
configure_file(
    "cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

add_subdirectory(src/core)
add_subdirectory(src/dsp)
add_subdirectory(src/io)
add_subdirectory(src/runtime)
add_subdirectory(src/ui)
add_subdirectory(src/protocol)
add_subdirectory(src/crypto)
add_subdirectory(src/fec)
add_subdirectory(src/third_party)
add_subdirectory(apps)
