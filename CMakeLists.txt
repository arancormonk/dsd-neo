cmake_minimum_required(VERSION 3.20)
project(dsd-neo)
include(CTest) # provides BUILD_TESTING and enables tests when on
enable_testing()
include(GNUInstallDirs)

# Prefer modern list operations to update module path
list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Platform detection and configuration
include(Platform)
# Curses/PDCurses selection - set early before find_package
if(MINGW OR NOT (WIN32 AND NOT CYGWIN))
    # Prefer ncurses wide-character support (POSIX + MinGW/vcpkg ships ncursesw)
    set(CURSES_NEED_NCURSES TRUE)
    set(CURSES_NEED_WIDE TRUE)
endif()

# Language and build defaults
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Ensure that non-ASCII UTF-8 string literals (used for UI glyphs and help text)
# are interpreted consistently across compilers. This primarily affects MinGW
# builds on Windows where the default input charset may not be UTF-8, causing
# mojibake like "â€“" instead of "–".
if(MINGW)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(
            $<$<COMPILE_LANGUAGE:C>:-finput-charset=UTF-8>
            $<$<COMPILE_LANGUAGE:C>:-fexec-charset=UTF-8>
            $<$<COMPILE_LANGUAGE:CXX>:-finput-charset=UTF-8>
            $<$<COMPILE_LANGUAGE:CXX>:-fexec-charset=UTF-8>
        )
    endif()
endif()

# MSVC: enable C11 atomics for <stdatomic.h>
#
# MSVC currently requires explicit opt-in for C11 atomics; otherwise it defines
# __STDC_NO_ATOMICS__ and <stdatomic.h> hard-errors (vcruntime_c11_stdatomic.h).
if(MSVC)
    # Visual Studio generators are more reliable when these are applied via the
    # language-specific C flags rather than directory compile options.
    if(NOT CMAKE_C_FLAGS MATCHES "/experimental:c11atomics")
        string(APPEND CMAKE_C_FLAGS " /std:c11 /Zc:__STDC__ /experimental:c11atomics")
    endif()
    # MSVC does not define common math constants (e.g., M_PI) unless explicitly enabled.
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

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
    set(GIT_TAG "0.1.0")
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
    if(TARGET mbe_neo::mbe_shared)
        set(MBE_LINK_TARGET mbe_neo::mbe_shared)
    else()
        message(FATAL_ERROR "mbe-neo package found but exported targets are missing")
    endif()
    # Ensure includes for legacy-style #include <mbelib.h>
    # by adding the directory that directly contains mbelib.h
    find_path(MBE_INCLUDE_DIR NAMES mbelib.h PATH_SUFFIXES mbelib-neo)
else()
    find_package(MBE REQUIRED)
    set(MBE_LINK_TARGET ${MBE_LIBRARY})
endif()
find_package(RTLSDR)

# Curses library selection:
# - MSVC/native Windows: PDCurses
# - MinGW/Cygwin/POSIX: ncurses via CMake's FindCurses
if(WIN32 AND NOT CYGWIN AND NOT MINGW)
    find_package(PDCurses REQUIRED)
    set(CURSES_LIBRARIES ${PDCURSES_LIBRARIES})
    set(CURSES_INCLUDE_DIRS ${PDCURSES_INCLUDE_DIRS})
    add_compile_definitions(PDC_WIDE)  # Wide character support for PDCurses
    add_compile_definitions(DSD_USE_PDCURSES)
    message(STATUS "Curses backend: PDCurses")
else()
    find_package(Curses REQUIRED)
    # Some ncurses installs (notably vcpkg's ncursesw on MinGW) do not provide a
    # top-level curses.h shim in the include root. Our code includes <curses.h>,
    # so add the ncursesw subdir when needed.
    if(CURSES_NEED_WIDE)
        foreach(_curses_inc IN LISTS CURSES_INCLUDE_DIRS)
            if(EXISTS "${_curses_inc}/ncursesw/curses.h" AND NOT EXISTS "${_curses_inc}/curses.h")
                list(APPEND CURSES_INCLUDE_DIRS "${_curses_inc}/ncursesw")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES CURSES_INCLUDE_DIRS)
    endif()
    message(STATUS "Curses backend: ncurses")
endif()

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
set(_PUBLIC_INCLUDES ${LIBSNDFILE_INCLUDE_DIR} ${CURSES_INCLUDE_DIRS})
if(MBE_INCLUDE_DIR)
    list(APPEND _PUBLIC_INCLUDES ${MBE_INCLUDE_DIR})
endif()

# Audio backend includes and libraries
set(AUDIO_SYSTEM_LIBS "")
if(DSD_USE_PORTAUDIO)
    list(APPEND _PUBLIC_INCLUDES ${PORTAUDIO_INCLUDE_DIRS})
    set(AUDIO_LIBS ${PORTAUDIO_LIBRARY})
    if(WIN32 AND NOT CYGWIN)
        list(APPEND AUDIO_SYSTEM_LIBS winmm ole32 uuid setupapi)
    endif()
else()
    list(APPEND _PUBLIC_INCLUDES ${PULSEAUDIO_INCLUDE_DIRS})
    set(AUDIO_LIBS ${PULSEAUDIO_SIMPLE_LIBRARY} ${PULSEAUDIO_LIBRARY})
endif()

set(LIBS ${MBE_LINK_TARGET} ${LIBSNDFILE_LIBRARIES} ${AUDIO_LIBS} ${CURSES_LIBRARIES})

if(RTLSDR_FOUND)
    find_package(Threads)
    list(APPEND LIBS ${RTLSDR_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
    add_definitions(-DUSE_RTLSDR)
    # Detect rtlsdr_set_bias_tee availability in the installed librtlsdr
    include(CheckSymbolExists)
    set(_CMAKE_REQUIRED_INCLUDES_SAVE "${CMAKE_REQUIRED_INCLUDES}")
    set(_CMAKE_REQUIRED_LIBRARIES_SAVE "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_INCLUDES ${RTLSDR_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${RTLSDR_LIBRARIES})
    unset(HAVE_RTLSDR_BIAS_TEE CACHE)
    check_symbol_exists(rtlsdr_set_bias_tee "rtl-sdr.h" HAVE_RTLSDR_BIAS_TEE)
    set(CMAKE_REQUIRED_INCLUDES "${_CMAKE_REQUIRED_INCLUDES_SAVE}")
    set(CMAKE_REQUIRED_LIBRARIES "${_CMAKE_REQUIRED_LIBRARIES_SAVE}")
    unset(_CMAKE_REQUIRED_INCLUDES_SAVE)
    unset(_CMAKE_REQUIRED_LIBRARIES_SAVE)
    if(HAVE_RTLSDR_BIAS_TEE)
        add_definitions(-DUSE_RTLSDR_BIAS_TEE)
    endif()
endif(RTLSDR_FOUND)

if(CODEC2_FOUND)
    list(APPEND LIBS ${CODEC2_LIBRARIES})
    add_definitions(-DUSE_CODEC2)
endif(CODEC2_FOUND)

if(AUDIO_SYSTEM_LIBS)
    list(APPEND LIBS ${AUDIO_SYSTEM_LIBS})
endif()

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
# Unit tests are only built on Linux debug configurations
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_subdirectory(tests)
endif()
