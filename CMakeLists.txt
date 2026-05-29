cmake_minimum_required(VERSION 3.20)
project(dsd-neo VERSION 1.0.0 LANGUAGES C CXX)
if(POLICY CMP0083)
    cmake_policy(SET CMP0083 NEW)
endif()
include(CTest) # provides BUILD_TESTING and enables tests when on
enable_testing()
include(GNUInstallDirs)

# Prefer modern list operations to update module path
list(PREPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Platform detection and configuration
include(Platform)
# Curses/PDCurses selection - set early before find_package
if(NOT WIN32)
    # Prefer ncurses wide-character support on POSIX.
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

# MSVC: enable C11 atomics for <stdatomic.h>
#
# MSVC currently requires explicit opt-in for C11 atomics; otherwise it defines
# __STDC_NO_ATOMICS__ and <stdatomic.h> hard-errors (vcruntime_c11_stdatomic.h).
if(MSVC)
    # Visual Studio generators are more reliable when these are applied via the
    # language-specific C flags rather than directory compile options.
    if(NOT CMAKE_C_FLAGS MATCHES "/experimental:c11atomics")
        string(
            APPEND CMAKE_C_FLAGS
            " /std:c11 /Zc:__STDC__ /experimental:c11atomics"
        )
    endif()
    # MSVC does not define common math constants (e.g., M_PI) unless explicitly enabled.
    add_compile_definitions(_USE_MATH_DEFINES)
endif()

# Modern build options (aligned with mbelib-neo style)
option(DSD_ENABLE_WARNINGS "Enable common warning flags" ON)
option(DSD_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(
    DSD_ENABLE_FAST_MATH
    "Enable fast-math optimizations (may relax IEEE semantics)"
    OFF
)
option(
    DSD_ENABLE_LTO
    "Enable Link Time Optimization (IPO/LTO) for Release builds"
    OFF
)
option(
    DSD_ENABLE_HARDENING
    "Enable Release-like compiler and linker hardening when supported"
    ON
)
option(DSD_ENABLE_NATIVE "Enable native CPU tuning (-march/-mtune=native)" OFF)
option(DSD_ENABLE_ASAN "Enable AddressSanitizer in Debug builds" OFF)
option(DSD_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer in Debug builds" OFF)
option(DSD_ENABLE_TSAN "Enable ThreadSanitizer in Debug builds" OFF)
option(
    DSD_ENABLE_FUZZING
    "Enable libFuzzer instrumentation and fuzz targets"
    OFF
)
option(DSD_ENABLE_RTLSDR "Enable RTL-SDR backend support" ON)
option(DSD_ENABLE_SOAPYSDR "Enable SoapySDR backend support" ON)
option(
    DSD_REQUIRE_RTLSDR
    "Fail configure when RTL-SDR is enabled but not found"
    OFF
)
option(
    DSD_REQUIRE_SOAPYSDR
    "Fail configure when SoapySDR is enabled but not found"
    OFF
)

# -----------------------------------------------------------------------------
# Common warning flags (target-scoped)
#
# This project keeps warnings target-scoped (not global add_compile_options) so
# third-party code and unrelated targets aren't affected. Targets should link
# this helper target PRIVATE.
# -----------------------------------------------------------------------------

add_library(dsd-neo_safe_api INTERFACE)
target_include_directories(
    dsd-neo_safe_api
    INTERFACE ${PROJECT_SOURCE_DIR}/include
)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(dsd-neo_safe_api INTERFACE _GNU_SOURCE)
endif()
if(MSVC)
    target_compile_options(
        dsd-neo_safe_api
        INTERFACE
            $<$<COMPILE_LANGUAGE:C>:/FI${PROJECT_SOURCE_DIR}/include/dsd-neo/core/safe_api.h>
            $<$<COMPILE_LANGUAGE:CXX>:/FI${PROJECT_SOURCE_DIR}/include/dsd-neo/core/safe_api.h>
    )
else()
    target_compile_options(
        dsd-neo_safe_api
        INTERFACE
            $<$<COMPILE_LANGUAGE:C>:-include>
            $<$<COMPILE_LANGUAGE:C>:dsd-neo/core/safe_api.h>
            $<$<COMPILE_LANGUAGE:CXX>:-include>
            $<$<COMPILE_LANGUAGE:CXX>:dsd-neo/core/safe_api.h>
    )
endif()

add_library(dsd-neo_sanitizers INTERFACE)
target_link_libraries(dsd-neo_sanitizers INTERFACE dsd-neo_safe_api)

if(DSD_ENABLE_TSAN AND (DSD_ENABLE_ASAN OR DSD_ENABLE_UBSAN))
    message(
        FATAL_ERROR
        "DSD_ENABLE_TSAN must use a separate build from ASan/UBSan."
    )
endif()

if(DSD_ENABLE_FUZZING AND DSD_ENABLE_TSAN)
    message(
        FATAL_ERROR
        "DSD_ENABLE_FUZZING must not be combined with ThreadSanitizer."
    )
endif()

if(
    (DSD_ENABLE_ASAN OR DSD_ENABLE_UBSAN OR DSD_ENABLE_TSAN)
    AND (
        NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"
    )
)
    message(FATAL_ERROR "DSD sanitizer builds require Clang or GCC.")
endif()

if(
    DSD_ENABLE_FUZZING
    AND (
        NOT CMAKE_C_COMPILER_ID MATCHES "Clang"
        OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"
    )
)
    message(FATAL_ERROR "DSD_ENABLE_FUZZING requires Clang libFuzzer support.")
endif()

if(DSD_ENABLE_TSAN AND WIN32)
    message(
        FATAL_ERROR
        "DSD_ENABLE_TSAN is supported only on POSIX Clang/GCC builds."
    )
endif()

if(CMAKE_BUILD_TYPE MATCHES "Debug")
    set(_dsd_san_cflags "")
    set(_dsd_san_ldflags "")

    if(DSD_ENABLE_ASAN)
        list(APPEND _dsd_san_cflags -fsanitize=address -fno-omit-frame-pointer)
        list(APPEND _dsd_san_ldflags -fsanitize=address)
    endif()

    if(DSD_ENABLE_UBSAN)
        list(APPEND _dsd_san_cflags -fsanitize=undefined)
        list(APPEND _dsd_san_ldflags -fsanitize=undefined)
    endif()

    if(DSD_ENABLE_TSAN)
        list(APPEND _dsd_san_cflags -fsanitize=thread -fno-omit-frame-pointer)
        list(APPEND _dsd_san_ldflags -fsanitize=thread)
    endif()

    if(_dsd_san_cflags)
        target_compile_options(dsd-neo_sanitizers INTERFACE ${_dsd_san_cflags})
    endif()
    if(_dsd_san_ldflags)
        target_link_options(dsd-neo_sanitizers INTERFACE ${_dsd_san_ldflags})
    endif()
    if(DSD_ENABLE_FUZZING)
        target_compile_options(
            dsd-neo_sanitizers
            INTERFACE
                $<$<COMPILE_LANGUAGE:C>:-fsanitize=fuzzer-no-link>
                $<$<COMPILE_LANGUAGE:CXX>:-fsanitize=fuzzer-no-link>
        )
    endif()
elseif(
    DSD_ENABLE_ASAN
    OR DSD_ENABLE_UBSAN
    OR DSD_ENABLE_TSAN
    OR DSD_ENABLE_FUZZING
)
    message(
        WARNING
        "DSD sanitizer/fuzzing options are enabled only for Debug builds."
    )
endif()

add_library(dsd-neo_hardening INTERFACE)
target_link_libraries(dsd-neo_hardening INTERFACE dsd-neo_sanitizers)

function(_dsd_flags_define_fortify out_var lang)
    set(_dsd_fortify_defined OFF)
    foreach(
        _dsd_flag_var
        IN
        ITEMS
            CMAKE_${lang}_FLAGS
            CMAKE_${lang}_FLAGS_RELEASE
            CMAKE_${lang}_FLAGS_RELWITHDEBINFO
            CMAKE_${lang}_FLAGS_MINSIZEREL
    )
        if(
            DEFINED ${_dsd_flag_var}
            AND "${${_dsd_flag_var}}" MATCHES "_FORTIFY_SOURCE"
        )
            set(_dsd_fortify_defined ON)
            break()
        endif()
    endforeach()
    set(${out_var} "${_dsd_fortify_defined}" PARENT_SCOPE)
endfunction()

set(_dsd_release_like_config
    "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>"
)

if(DSD_ENABLE_HARDENING)
    if(
        CMAKE_C_COMPILER_ID MATCHES "Clang|GNU"
        AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"
    )
        _dsd_flags_define_fortify(_dsd_c_flags_define_fortify C)
        if(NOT _dsd_c_flags_define_fortify)
            target_compile_definitions(
                dsd-neo_hardening
                INTERFACE
                    "$<$<AND:${_dsd_release_like_config},$<COMPILE_LANGUAGE:C>>:_FORTIFY_SOURCE=2>"
            )
        endif()
        _dsd_flags_define_fortify(_dsd_cxx_flags_define_fortify CXX)
        if(NOT _dsd_cxx_flags_define_fortify)
            target_compile_definitions(
                dsd-neo_hardening
                INTERFACE
                    "$<$<AND:${_dsd_release_like_config},$<COMPILE_LANGUAGE:CXX>>:_FORTIFY_SOURCE=2>"
            )
        endif()

        include(CheckCCompilerFlag)
        include(CheckCXXCompilerFlag)
        check_c_compiler_flag(
            "-fstack-protector-strong"
            DSD_C_HAS_STACK_PROTECTOR_STRONG
        )
        check_cxx_compiler_flag(
            "-fstack-protector-strong"
            DSD_CXX_HAS_STACK_PROTECTOR_STRONG
        )
        if(DSD_C_HAS_STACK_PROTECTOR_STRONG)
            target_compile_options(
                dsd-neo_hardening
                INTERFACE
                    "$<$<AND:${_dsd_release_like_config},$<COMPILE_LANGUAGE:C>>:-fstack-protector-strong>"
            )
        endif()
        if(DSD_CXX_HAS_STACK_PROTECTOR_STRONG)
            target_compile_options(
                dsd-neo_hardening
                INTERFACE
                    "$<$<AND:${_dsd_release_like_config},$<COMPILE_LANGUAGE:CXX>>:-fstack-protector-strong>"
            )
        endif()

        if(NOT APPLE)
            check_c_compiler_flag(
                "-fstack-clash-protection"
                DSD_C_HAS_STACK_CLASH_PROTECTION
            )
            check_cxx_compiler_flag(
                "-fstack-clash-protection"
                DSD_CXX_HAS_STACK_CLASH_PROTECTION
            )
            if(DSD_C_HAS_STACK_CLASH_PROTECTION)
                target_compile_options(
                    dsd-neo_hardening
                    INTERFACE
                        "$<$<AND:${_dsd_release_like_config},$<COMPILE_LANGUAGE:C>>:-fstack-clash-protection>"
                )
            endif()
            if(DSD_CXX_HAS_STACK_CLASH_PROTECTION)
                target_compile_options(
                    dsd-neo_hardening
                    INTERFACE
                        "$<$<AND:${_dsd_release_like_config},$<COMPILE_LANGUAGE:CXX>>:-fstack-clash-protection>"
                )
            endif()
        endif()

        if(UNIX AND NOT APPLE)
            include(CheckLinkerFlag)
            check_linker_flag(C "-Wl,-z,relro" DSD_LD_HAS_RELRO)
            check_linker_flag(C "-Wl,-z,now" DSD_LD_HAS_NOW)
            check_linker_flag(C "-pie" DSD_LD_HAS_PIE)
            if(DSD_LD_HAS_RELRO)
                target_link_options(
                    dsd-neo_hardening
                    INTERFACE "$<${_dsd_release_like_config}:-Wl,-z,relro>"
                )
            endif()
            if(DSD_LD_HAS_NOW)
                target_link_options(
                    dsd-neo_hardening
                    INTERFACE "$<${_dsd_release_like_config}:-Wl,-z,now>"
                )
            endif()
            if(DSD_LD_HAS_PIE)
                target_link_options(
                    dsd-neo_hardening
                    INTERFACE
                        "$<$<AND:${_dsd_release_like_config},$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>>:-pie>"
                )
            endif()
        endif()
    elseif(MSVC)
        set(_dsd_linkable_target
            "$<AND:$<NOT:$<STREQUAL:$<TARGET_PROPERTY:TYPE>,STATIC_LIBRARY>>,$<NOT:$<STREQUAL:$<TARGET_PROPERTY:TYPE>,OBJECT_LIBRARY>>>"
        )
        target_link_options(
            dsd-neo_hardening
            INTERFACE
                "$<$<AND:${_dsd_release_like_config},${_dsd_linkable_target}>:/DYNAMICBASE>"
                "$<$<AND:${_dsd_release_like_config},${_dsd_linkable_target}>:/NXCOMPAT>"
        )
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            target_link_options(
                dsd-neo_hardening
                INTERFACE
                    "$<$<AND:${_dsd_release_like_config},${_dsd_linkable_target}>:/HIGHENTROPYVA>"
            )
        endif()
    endif()
endif()

add_library(dsd-neo_warnings INTERFACE)
target_link_libraries(dsd-neo_warnings INTERFACE dsd-neo_hardening)

if(DSD_ENABLE_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(
            dsd-neo_warnings
            INTERFACE -Wall -Wextra -Wpedantic
        )
        target_compile_options(
            dsd-neo_warnings
            INTERFACE
                -Wunused-but-set-variable
                -Wunused-variable
                -Wunused-parameter
                -Wempty-body
                -Wunused-label
                -Wformat=2
                -Wvla
                $<$<COMPILE_LANGUAGE:C>:-Wpointer-sign>
                $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
                $<$<COMPILE_LANGUAGE:C>:-Wmissing-prototypes>
                $<$<COMPILE_LANGUAGE:C>:-Wold-style-definition>
                -Wmisleading-indentation
                -Wparentheses
                -Wunused-value
                -Wreturn-type
                -Wtautological-compare
        )
    elseif(MSVC)
        target_compile_options(
            dsd-neo_warnings
            INTERFACE
                /W4
                # Match the GCC/Clang warning set (these are off unless explicitly enabled).
                /wd4244 # conversion, possible loss of data
                /wd4245 # signed/unsigned mismatch in assignment
                /wd4267 # conversion from 'size_t', possible loss of data
                /wd4389 # signed/unsigned mismatch
                /wd4456 # declaration hides previous local declaration
                /wd4459 # declaration hides global declaration
                /wd4127 # conditional expression is constant
        )
    endif()
    if(DSD_WARNINGS_AS_ERRORS)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(dsd-neo_warnings INTERFACE -Werror)
        elseif(MSVC)
            target_compile_options(dsd-neo_warnings INTERFACE /WX)
        endif()
    endif()
endif()

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
option(COLORS "Build with Colors Enabled" ON)

#use cmake option -DCOLORSLOGS=OFF to disable color output (terminal/logs)
option(COLORSLOGS "Enable colored terminal/log output" ON)

#use cmake option -DPVC=ON to enable Provoice Conventional Frame Sync
option(PVC "Build with Provoice Conventional Frame Sync Enabled" OFF)

#use cmake option -DLZ=ON to enable LimaZulu Requested NXDN Tweaks
option(LZ "Build with new LimaZulu Requested NXDN Tweaks Enabled" OFF)

#use cmake option -DSID=ON to enable P25p1 Soft ID decoding -- Experimental
option(SID "Build with P25p1 LSD/Soft ID Enabled" OFF)

include(git_revision)
# Gather git info: semantic-like tag and short hash
get_git_head_revision(GIT_REFSPEC GIT_HASH)
git_describe(GIT_TAG --tags --match "v[0-9]*.[0-9]*.[0-9]*")

# Sanitize/fallbacks for non-git tarballs or archives
if("${GIT_TAG}" MATCHES "NOTFOUND" OR "${GIT_TAG}" STREQUAL "")
    set(GIT_TAG "v${PROJECT_VERSION}")
endif()
if("${GIT_HASH}" MATCHES "NOTFOUND" OR "${GIT_HASH}" STREQUAL "")
    set(GIT_HASH "unknown")
else()
    string(SUBSTRING "${GIT_HASH}" 0 7 GIT_HASH_SHORT)
    set(GIT_HASH "${GIT_HASH_SHORT}")
endif()

# Dependencies
find_package(LibSndFile REQUIRED)

# Require mbelib-neo 2.x (no legacy mbelib fallback). The 2.x ABI provides the
# soft-decision frame decode and result-based processing APIs used by dsd-neo.
set(_DSD_MBE_REQUIRED_VERSION 2.0)
find_package(mbe-neo ${_DSD_MBE_REQUIRED_VERSION} CONFIG REQUIRED)
if(TARGET mbe_neo::mbe_shared)
    set(MBE_LINK_TARGET mbe_neo::mbe_shared)
else()
    message(
        FATAL_ERROR
        "mbe-neo package found but exported targets are missing"
    )
endif()
# Ensure includes for legacy-style #include <mbelib.h>
# by adding the directory that directly contains mbelib.h
get_target_property(
    _MBE_TARGET_INCLUDE_DIRS
    ${MBE_LINK_TARGET}
    INTERFACE_INCLUDE_DIRECTORIES
)
if(NOT _MBE_TARGET_INCLUDE_DIRS)
    set(_MBE_TARGET_INCLUDE_DIRS "")
endif()
set(_MBE_INCLUDE_HINTS "")
foreach(_MBE_TARGET_INCLUDE_DIR IN LISTS _MBE_TARGET_INCLUDE_DIRS)
    if(
        IS_ABSOLUTE "${_MBE_TARGET_INCLUDE_DIR}"
        AND NOT _MBE_TARGET_INCLUDE_DIR MATCHES "^\\$<"
    )
        list(
            APPEND _MBE_INCLUDE_HINTS
            "${_MBE_TARGET_INCLUDE_DIR}"
            "${_MBE_TARGET_INCLUDE_DIR}/mbelib-neo"
        )
    endif()
endforeach()
find_path(
    MBE_INCLUDE_DIR
    NAMES mbelib.h
    HINTS ${_MBE_INCLUDE_HINTS}
    PATH_SUFFIXES mbelib-neo
)
if(NOT MBE_INCLUDE_DIR)
    message(FATAL_ERROR "mbe-neo 2.x package found, but mbelib.h was not found")
endif()

include(CheckCSourceCompiles)
set(_DSD_SAVED_CMAKE_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
set(_DSD_SAVED_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
if(DEFINED CMAKE_TRY_COMPILE_TARGET_TYPE)
    set(_DSD_HAD_CMAKE_TRY_COMPILE_TARGET_TYPE TRUE)
    set(_DSD_SAVED_CMAKE_TRY_COMPILE_TARGET_TYPE
        "${CMAKE_TRY_COMPILE_TARGET_TYPE}"
    )
endif()
set(CMAKE_REQUIRED_INCLUDES "${MBE_INCLUDE_DIR}")
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    string(APPEND CMAKE_REQUIRED_FLAGS " -Werror=implicit-function-declaration")
endif()
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
unset(DSD_HAVE_MBE_NEO_SOFT_API CACHE)
check_c_source_compiles(
    [[
#include <stddef.h>
#include <stdint.h>
#include <mbelib.h>

int main(void) {
    const mbe_soft_bit imbe_soft[8][23] = {{{0}}};
    const mbe_soft_bit ambe_soft[4][24] = {{{0}}};
    const char imbe_fr[8][23] = {{0}};
    const char imbe7100_fr[7][24] = {{0}};
    const char ambe_fr[4][24] = {{0}};
    char imbe_d[88] = {0};
    char ambe_d[49] = {0};
    char err_str[16] = {0};
    float audio[160] = {0.0f};
    mbe_parms cur = {0};
    mbe_parms prev = {0};
    mbe_parms prev_enhanced = {0};
    mbe_process_result result;

    mbe_initProcessResult(&result);
    result.c0_errors = 0;
    result.total_errors = 0;
    result.flags = MBE_PROCESS_FLAG_SOFT_INPUT;
    (void)mbe_decodeImbe7200x4400Frame(imbe_fr, imbe_d, &result);
    (void)mbe_decodeImbe7100x4400Frame(imbe7100_fr, imbe_d, &result);
    (void)mbe_decodeAmbe3600x2450Frame(ambe_fr, ambe_d, &result);
    (void)mbe_decodeImbe7200x4400SoftFrame(imbe_soft, imbe_d, &result);
    (void)mbe_decodeAmbe3600x2450SoftFrame(ambe_soft, ambe_d, &result);
    (void)mbe_processImbe4400Dataf(audio, &result, imbe_d, &cur, &prev, &prev_enhanced);
    (void)mbe_processAmbe2450Dataf(audio, &result, ambe_d, &cur, &prev, &prev_enhanced);
    (void)mbe_processAmbe2400Dataf(audio, &result, ambe_d, &cur, &prev, &prev_enhanced);
    (void)mbe_processAmbe3600x2450Framef(audio, &result, ambe_fr, ambe_d, &cur, &prev, &prev_enhanced);
    (void)mbe_processAmbe3600x2450SoftFramef(audio, &result, ambe_soft, ambe_d, &cur, &prev, &prev_enhanced);
    mbe_synthesizeSilencef(audio);
    mbe_formatProcessResult(err_str, sizeof(err_str), &result);
    return (int)(sizeof(mbe_soft_bit) + sizeof(mbe_process_result));
}
]]
    DSD_HAVE_MBE_NEO_SOFT_API
)
set(CMAKE_REQUIRED_INCLUDES "${_DSD_SAVED_CMAKE_REQUIRED_INCLUDES}")
set(CMAKE_REQUIRED_FLAGS "${_DSD_SAVED_CMAKE_REQUIRED_FLAGS}")
if(_DSD_HAD_CMAKE_TRY_COMPILE_TARGET_TYPE)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE
        "${_DSD_SAVED_CMAKE_TRY_COMPILE_TARGET_TYPE}"
    )
else()
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
endif()
unset(_DSD_HAD_CMAKE_TRY_COMPILE_TARGET_TYPE)
unset(_DSD_SAVED_CMAKE_TRY_COMPILE_TARGET_TYPE)
unset(_DSD_SAVED_CMAKE_REQUIRED_INCLUDES)
unset(_DSD_SAVED_CMAKE_REQUIRED_FLAGS)
if(NOT DSD_HAVE_MBE_NEO_SOFT_API)
    message(
        FATAL_ERROR
        "mbe-neo 2.x with the soft-decision/result API is required; update mbelib-neo"
    )
endif()
if(DSD_ENABLE_RTLSDR)
    find_package(RTLSDR)
endif()

if(DSD_ENABLE_SOAPYSDR)
    # Ubuntu 20.04's SoapySDRConfig.cmake (0.7.x) sets CMAKE_CXX_STANDARD=11
    # as a side effect. Preserve this project's configured language standard.
    set(_dsd_saved_cxx_standard "${CMAKE_CXX_STANDARD}")
    set(_dsd_saved_cxx_standard_required "${CMAKE_CXX_STANDARD_REQUIRED}")
    set(_dsd_saved_cxx_extensions "${CMAKE_CXX_EXTENSIONS}")

    find_package(SoapySDR CONFIG QUIET)

    set(CMAKE_CXX_STANDARD "${_dsd_saved_cxx_standard}")
    set(CMAKE_CXX_STANDARD_REQUIRED "${_dsd_saved_cxx_standard_required}")
    set(CMAKE_CXX_EXTENSIONS "${_dsd_saved_cxx_extensions}")

    unset(_dsd_saved_cxx_standard)
    unset(_dsd_saved_cxx_standard_required)
    unset(_dsd_saved_cxx_extensions)
endif()

set(DSD_HAS_RTLSDR OFF)
if(DSD_ENABLE_RTLSDR AND RTLSDR_FOUND)
    set(DSD_HAS_RTLSDR ON)
endif()

set(DSD_HAS_SOAPYSDR OFF)
if(DSD_ENABLE_SOAPYSDR)
    if(TARGET SoapySDR OR TARGET SoapySDR::SoapySDR)
        set(DSD_HAS_SOAPYSDR ON)
    elseif(SoapySDR_INCLUDE_DIRS AND SoapySDR_LIBRARIES)
        set(DSD_HAS_SOAPYSDR ON)
    endif()
endif()

if(DSD_REQUIRE_RTLSDR AND NOT DSD_ENABLE_RTLSDR)
    message(FATAL_ERROR "DSD_REQUIRE_RTLSDR=ON requires DSD_ENABLE_RTLSDR=ON.")
endif()

if(DSD_REQUIRE_SOAPYSDR AND NOT DSD_ENABLE_SOAPYSDR)
    message(
        FATAL_ERROR
        "DSD_REQUIRE_SOAPYSDR=ON requires DSD_ENABLE_SOAPYSDR=ON."
    )
endif()

if(DSD_REQUIRE_RTLSDR AND NOT DSD_HAS_RTLSDR)
    message(
        FATAL_ERROR
        "RTL-SDR backend was required but not found. Install librtlsdr or disable DSD_REQUIRE_RTLSDR."
    )
endif()

if(DSD_REQUIRE_SOAPYSDR AND NOT DSD_HAS_SOAPYSDR)
    message(
        FATAL_ERROR
        "SoapySDR backend was required but not found. Install SoapySDR or disable DSD_REQUIRE_SOAPYSDR."
    )
endif()

if(DSD_ENABLE_RTLSDR AND NOT DSD_HAS_RTLSDR AND NOT DSD_REQUIRE_RTLSDR)
    message(
        STATUS
        "RTL-SDR not found; building without RTL-SDR backend (set DSD_REQUIRE_RTLSDR=ON to fail)."
    )
endif()

if(DSD_ENABLE_SOAPYSDR AND NOT DSD_HAS_SOAPYSDR AND NOT DSD_REQUIRE_SOAPYSDR)
    message(
        STATUS
        "SoapySDR not found; building without SoapySDR backend (set DSD_REQUIRE_SOAPYSDR=ON to fail)."
    )
endif()

set(DSD_HAS_RADIO OFF)
if(DSD_HAS_RTLSDR OR DSD_HAS_SOAPYSDR)
    set(DSD_HAS_RADIO ON)
endif()

message(
    STATUS
    "RTL-SDR backend enabled: ${DSD_ENABLE_RTLSDR} (available: ${DSD_HAS_RTLSDR})"
)
message(
    STATUS
    "SoapySDR backend enabled: ${DSD_ENABLE_SOAPYSDR} (available: ${DSD_HAS_SOAPYSDR})"
)
message(STATUS "Radio pipeline available: ${DSD_HAS_RADIO}")

# Curses library selection:
# - Windows: PDCurses
# - POSIX: ncurses via CMake's FindCurses
if(WIN32)
    find_package(PDCurses REQUIRED)
    set(CURSES_LIBRARIES PDCurses::PDCurses)
    set(CURSES_INCLUDE_DIRS ${PDCURSES_INCLUDE_DIRS})
    add_compile_definitions(DSD_USE_PDCURSES)
    include(CheckCSourceCompiles)
    set(_CMAKE_REQUIRED_DEFINITIONS_SAVE "${CMAKE_REQUIRED_DEFINITIONS}")
    set(_CMAKE_REQUIRED_INCLUDES_SAVE "${CMAKE_REQUIRED_INCLUDES}")
    set(_CMAKE_REQUIRED_LIBRARIES_SAVE "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_DEFINITIONS -DPDC_WIDE)
    set(CMAKE_REQUIRED_INCLUDES ${PDCURSES_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES PDCurses::PDCurses)
    unset(DSD_HAS_PDCURSES_WIDE_API CACHE)
    check_c_source_compiles(
        "
        #include <wchar.h>
        #include <curses.h>
        int main(void) {
            static const wchar_t glyph[] = {0x2588, 0};
            return addwstr(glyph);
        }
    "
        DSD_HAS_PDCURSES_WIDE_API
    )
    set(CMAKE_REQUIRED_DEFINITIONS "${_CMAKE_REQUIRED_DEFINITIONS_SAVE}")
    set(CMAKE_REQUIRED_INCLUDES "${_CMAKE_REQUIRED_INCLUDES_SAVE}")
    set(CMAKE_REQUIRED_LIBRARIES "${_CMAKE_REQUIRED_LIBRARIES_SAVE}")
    unset(_CMAKE_REQUIRED_DEFINITIONS_SAVE)
    unset(_CMAKE_REQUIRED_INCLUDES_SAVE)
    unset(_CMAKE_REQUIRED_LIBRARIES_SAVE)
    if(DSD_HAS_PDCURSES_WIDE_API)
        add_compile_definitions(DSD_HAS_PDCURSES_WIDE_API=1 PDC_WIDE)
    endif()
    message(STATUS "Curses backend: PDCurses")
    message(STATUS "PDCurses wide-character API: ${DSD_HAS_PDCURSES_WIDE_API}")
else()
    find_package(Curses REQUIRED)
    # Some ncurses installs do not provide a top-level curses.h shim in the
    # include root. Our code includes <curses.h>, so add the ncursesw subdir
    # when needed.
    if(CURSES_NEED_WIDE)
        foreach(_curses_inc IN LISTS CURSES_INCLUDE_DIRS)
            if(
                EXISTS "${_curses_inc}/ncursesw/curses.h"
                AND NOT EXISTS "${_curses_inc}/curses.h"
            )
                list(APPEND CURSES_INCLUDE_DIRS "${_curses_inc}/ncursesw")
            endif()
        endforeach()
        list(REMOVE_DUPLICATES CURSES_INCLUDE_DIRS)
    endif()
    message(STATUS "Curses backend: ncurses")
endif()

find_package(CODEC2)
find_package(CURL)

# Audio backend selection
option(
    DSD_USE_PORTAUDIO
    "Use PortAudio for cross-platform audio (required on Windows)"
    OFF
)

# Windows requires PortAudio (no PulseAudio available)
if(WIN32)
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
    if(WIN32)
        list(APPEND AUDIO_SYSTEM_LIBS winmm ole32 uuid setupapi)
    endif()
else()
    list(APPEND _PUBLIC_INCLUDES ${PULSEAUDIO_INCLUDE_DIRS})
    set(AUDIO_LIBS ${PULSEAUDIO_SIMPLE_LIBRARY} ${PULSEAUDIO_LIBRARY})
endif()

set(LIBS
    ${MBE_LINK_TARGET}
    ${LIBSNDFILE_LIBRARIES}
    ${AUDIO_LIBS}
    ${CURSES_LIBRARIES}
)

# -----------------------------------------------------------------------------
# Optional feature interface targets
#
# These INTERFACE libraries carry compile definitions and include paths for
# optional features. Link them only to targets that require the feature,
# avoiding global add_definitions() that leak into unrelated libraries/tests.
# -----------------------------------------------------------------------------

add_library(dsd-neo_feature_colors INTERFACE)
target_compile_definitions(
    dsd-neo_feature_colors
    INTERFACE $<$<BOOL:${COLORS}>:PRETTY_COLORS>
)

add_library(dsd-neo_feature_colors_logs INTERFACE)
target_compile_definitions(
    dsd-neo_feature_colors_logs
    INTERFACE $<$<BOOL:${COLORSLOGS}>:PRETTY_COLORS_LOGS>
)

add_library(dsd-neo_feature_pvc INTERFACE)
target_compile_definitions(
    dsd-neo_feature_pvc
    INTERFACE $<$<BOOL:${PVC}>:PVCONVENTIONAL>
)

add_library(dsd-neo_feature_lz INTERFACE)
target_compile_definitions(
    dsd-neo_feature_lz
    INTERFACE $<$<BOOL:${LZ}>:LIMAZULUTWEAKS>
)

add_library(dsd-neo_feature_sid INTERFACE)
target_compile_definitions(
    dsd-neo_feature_sid
    INTERFACE $<$<BOOL:${SID}>:SOFTID>
)

add_library(dsd-neo_feature_radio INTERFACE)
target_compile_definitions(
    dsd-neo_feature_radio
    INTERFACE $<$<BOOL:${DSD_HAS_RADIO}>:USE_RADIO>
)

add_library(dsd-neo_feature_soapy INTERFACE)
if(DSD_HAS_SOAPYSDR)
    target_compile_definitions(dsd-neo_feature_soapy INTERFACE USE_SOAPYSDR)
    if(TARGET SoapySDR)
        target_link_libraries(dsd-neo_feature_soapy INTERFACE SoapySDR)
    elseif(TARGET SoapySDR::SoapySDR)
        target_link_libraries(
            dsd-neo_feature_soapy
            INTERFACE SoapySDR::SoapySDR
        )
    elseif(SoapySDR_INCLUDE_DIRS AND SoapySDR_LIBRARIES)
        target_include_directories(
            dsd-neo_feature_soapy
            SYSTEM
            INTERFACE ${SoapySDR_INCLUDE_DIRS}
        )
        target_link_libraries(
            dsd-neo_feature_soapy
            INTERFACE ${SoapySDR_LIBRARIES}
        )
    endif()
endif()

if(DSD_HAS_RTLSDR)
    find_package(Threads)
    list(APPEND LIBS ${RTLSDR_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})
    list(APPEND _PUBLIC_INCLUDES ${RTLSDR_INCLUDE_DIRS})

    # Interface target for USE_RTLSDR compile definition
    add_library(dsd-neo_feature_rtlsdr INTERFACE)
    target_compile_definitions(dsd-neo_feature_rtlsdr INTERFACE USE_RTLSDR)
    target_include_directories(
        dsd-neo_feature_rtlsdr
        SYSTEM
        INTERFACE ${RTLSDR_INCLUDE_DIRS}
    )

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
        target_compile_definitions(
            dsd-neo_feature_rtlsdr
            INTERFACE USE_RTLSDR_BIAS_TEE
        )
    endif()
else()
    # Provide a stub interface target so linking remains unconditional upstream
    add_library(dsd-neo_feature_rtlsdr INTERFACE)
endif()

if(CODEC2_FOUND)
    list(APPEND LIBS ${CODEC2_LIBRARIES})

    # Interface target for USE_CODEC2 compile definition
    add_library(dsd-neo_feature_codec2 INTERFACE)
    target_compile_definitions(dsd-neo_feature_codec2 INTERFACE USE_CODEC2)
    target_include_directories(
        dsd-neo_feature_codec2
        SYSTEM
        INTERFACE ${CODEC2_INCLUDE_DIRS}
    )
else()
    # Provide a stub interface target so linking remains unconditional upstream
    add_library(dsd-neo_feature_codec2 INTERFACE)
endif()

if(CURL_FOUND)
    add_library(dsd-neo_feature_curl INTERFACE)
    target_compile_definitions(dsd-neo_feature_curl INTERFACE USE_CURL)
    target_link_libraries(dsd-neo_feature_curl INTERFACE CURL::libcurl)
else()
    add_library(dsd-neo_feature_curl INTERFACE)
endif()

if(AUDIO_SYSTEM_LIBS)
    list(APPEND LIBS ${AUDIO_SYSTEM_LIBS})
endif()

## Executable target is defined in apps/dsd-cli

# uninstall target
configure_file(
    "cmake/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE
    @ONLY
)

add_custom_target(
    uninstall
    COMMAND
        ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
)

# Install license/notice files with binaries
install(
    FILES LICENSE COPYRIGHT THIRD_PARTY.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(
    FILES src/third_party/ezpwd/lesser.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
    RENAME ezpwd-LGPL-2.1-or-later.txt
)
install(
    FILES src/third_party/pffft/COPYING
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/licenses
    RENAME pffft-FFTPACK.txt
)

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
add_subdirectory(src/engine)
add_subdirectory(apps)
# Unit tests are built when BUILD_TESTING is enabled.
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
