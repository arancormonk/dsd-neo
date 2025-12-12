cmake_minimum_required(VERSION 3.20)
project(dsd-neo)
include(CTest) # provides BUILD_TESTING and enables tests when on
enable_testing()
include(GNUInstallDirs)

# Prefer modern list operations to update module path
list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Platform detection and configuration
include(Platform)
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
option(DSD_ENABLE_NATIVE "Enable native CPU tuning (-march/-mtune=native)" OFF)
option(DSD_ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)
option(DSD_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer in Debug builds" OFF)

#
# Global optimization knobs for Release-like builds
# Place early so they affect subsequently created targets
#

# Optional fast-math across all targets (use with care)
if(DSD_ENABLE_FAST_MATH)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C>:-ffast-math>
            $<$<COMPILE_LANGUAGE:C>:-fno-math-errno>
            $<$<COMPILE_LANGUAGE:CXX>:-ffast-math>
            $<$<COMPILE_LANGUAGE:CXX>:-fno-math-errno>
        )
    elseif(MSVC)
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C>:/fp:fast>
            $<$<COMPILE_LANGUAGE:CXX>:/fp:fast>
        )
    endif()
endif()

# Optional native CPU tuning across all targets (non-portable binaries)
if(DSD_ENABLE_NATIVE)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C>:-march=native>
            $<$<COMPILE_LANGUAGE:C>:-mtune=native>
            $<$<COMPILE_LANGUAGE:CXX>:-march=native>
            $<$<COMPILE_LANGUAGE:CXX>:-mtune=native>
        )
    endif()
endif()

# Optional LTO/IPO for Release/RelWithDebInfo (when supported)
if(DSD_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_ok OUTPUT _ipo_msg)
    if(_ipo_ok)
        # Affect targets created after this point for Release-like configs
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
    else()
        message(STATUS "IPO/LTO not supported: ${_ipo_msg}")
    endif()
endif()

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
    if(WIN32 OR CYGWIN)
        set(MBE_LINK_TARGET mbe_neo::mbe_static)
        # When consuming the static mbelib-neo on Windows/Cygwin, ensure
        # headers don't apply __declspec(dllimport) decorations.
        add_compile_definitions(MBE_STATIC)
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
find_package(CODEC2)

# Audio backend selection
option(DSD_USE_PORTAUDIO "Use PortAudio for cross-platform audio (required on Windows)" OFF)

# Windows requires PortAudio (no PulseAudio available)
if(WIN32 AND NOT CYGWIN)
    set(DSD_USE_PORTAUDIO ON CACHE BOOL "Use PortAudio on Windows" FORCE)
endif()

if(DSD_USE_PORTAUDIO)
    find_package(PortAudio REQUIRED)
    add_compile_definitions(DSD_USE_PORTAUDIO)
    message(STATUS "Audio backend: PortAudio")
else()
    find_package(PulseAudio REQUIRED)
    message(STATUS "Audio backend: PulseAudio")
endif()

# Aggregate include directories; when using the modern package, we still add
# the mbelib-neo leaf include directory so #include <mbelib.h> continues to work.
set(_PUBLIC_INCLUDES ${LIBSNDFILE_INCLUDE_DIR} ${ITPP_INCLUDE_DIR} ${CURSES_INCLUDE_DIRS})
if(MBE_INCLUDE_DIR)
    list(APPEND _PUBLIC_INCLUDES ${MBE_INCLUDE_DIR})
endif()

# Audio backend includes and libraries
if(DSD_USE_PORTAUDIO)
    list(APPEND _PUBLIC_INCLUDES ${PORTAUDIO_INCLUDE_DIRS})
    set(AUDIO_LIBS ${PORTAUDIO_LIBRARIES})
else()
    list(APPEND _PUBLIC_INCLUDES ${PULSEAUDIO_INCLUDE_DIRS})
    set(AUDIO_LIBS ${PULSEAUDIO_SIMPLE_LIBRARY} ${PULSEAUDIO_LIBRARY})
endif()

set(LIBS ${MBE_LINK_TARGET} ${LIBSNDFILE_LIBRARY} ${ITPP_LIBRARY} ${AUDIO_LIBS} ${CURSES_LIBRARIES})

if(RTLSDR_FOUND)
    find_package(Threads)
    list(APPEND LIBS ${RTLSDR_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
    add_definitions(-DUSE_RTLSDR)
    # Detect rtlsdr_set_bias_tee availability in the installed librtlsdr
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_INCLUDES ${RTLSDR_INCLUDE_DIRS})
    check_symbol_exists(rtlsdr_set_bias_tee "rtl-sdr.h" HAVE_RTLSDR_BIAS_TEE)
    if(HAVE_RTLSDR_BIAS_TEE)
        add_definitions(-DUSE_RTLSDR_BIAS_TEE)
    endif()
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

# Install license/notice files with binaries
install(FILES LICENSE COPYRIGHT THIRD_PARTY.md src/third_party/ezpwd/lesser.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR})

add_subdirectory(src/platform)
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
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()
