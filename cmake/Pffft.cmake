# SPDX-License-Identifier: GPL-3.0-or-later
# Compile the pinned pffft-neo sources into DSD-neo; do not configure its CMake project.
include(FetchContent)

set(_pffft_pins "${CMAKE_CURRENT_LIST_DIR}/../tools/ci-dependency-pins.env")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pffft_pins}")
foreach(_pin PFFFT_SHA PFFFT_ARCHIVE_SHA256)
    file(STRINGS "${_pffft_pins}" _lines REGEX "^${_pin}=")
    list(LENGTH _lines _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "Expected one ${_pin} in ${_pffft_pins}")
    endif()
    string(REPLACE "${_pin}=" "" ${_pin} "${_lines}")
    string(LENGTH "${${_pin}}" _length)
    if(_pin STREQUAL "PFFFT_SHA")
        set(_expected 40)
    else()
        set(_expected 64)
    endif()
    if(NOT _length EQUAL _expected OR NOT "${${_pin}}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Invalid ${_pin} in ${_pffft_pins}")
    endif()
endforeach()

if(FETCHCONTENT_SOURCE_DIR_DSD_PFFFT)
    if(
        NOT IS_ABSOLUTE "${FETCHCONTENT_SOURCE_DIR_DSD_PFFFT}"
        OR NOT IS_DIRECTORY "${FETCHCONTENT_SOURCE_DIR_DSD_PFFFT}"
    )
        message(
            FATAL_ERROR
            "FETCHCONTENT_SOURCE_DIR_DSD_PFFFT must name an existing absolute PFFFT source directory"
        )
    endif()
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(
    dsd_pffft
    URL "https://codeload.github.com/arancormonk/pffft-neo/tar.gz/${PFFFT_SHA}"
    URL_HASH "SHA256=${PFFFT_ARCHIVE_SHA256}"
    TLS_VERIFY TRUE
    # Supported since CMake 3.18: a directory without CMakeLists.txt populates
    # sources without add_subdirectory(), preserving our C-only build.
    SOURCE_SUBDIR include
)
FetchContent_MakeAvailable(dsd_pffft)

set(DSD_PFFFT_SOURCE_DIR
    "${dsd_pffft_SOURCE_DIR}"
    CACHE INTERNAL
    "PFFFT source directory for analyzers"
    FORCE
)
set(PFFFT_SOURCES
    "${DSD_PFFFT_SOURCE_DIR}/src/pffft.c"
    "${DSD_PFFFT_SOURCE_DIR}/src/pffft_common.c"
)
set(PFFFT_INCLUDE_DIR "${DSD_PFFFT_SOURCE_DIR}/include/pffft")
# Every file pffft.c, pffft_common.c and pffft_wide_tu.c compile from, so an
# incomplete offline source fails here rather than mid-build.
set(_pffft_sources
    src/pffft.c
    src/pffft_common.c
    src/pffft_wide_tu.c
    src/pffft_impl.h
    src/pffft_internal.h
    src/pffft_setup.h
    src/pffft_priv_impl.h
    src/pffft_passes_impl.h
    src/pffft_pass_config.h
    src/pffft_cplx_impl.h
    src/pffft_real_impl.h
    src/pffft_cpu_x86.h
    src/pffft_wide_config.h
    src/pffft_wide_impl.h
    src/pffft_wide_select.h
    src/pffft_wide_sizes.h
    src/pffft_wide_api.h
    src/simd/pf_float.h
    src/simd/pf_scalar_float.h
    src/simd/pf_sse1_float.h
    src/simd/pf_fma_x86.h
    src/simd/pf_neon_float.h
    include/pffft/pffft.h
)
set(_pffft_notices
    README.md
    LICENSE
    third_party/pffft/LICENSE.txt
    third_party/pffft/LicenseRef-PFFFT.txt
)
foreach(_file IN LISTS _pffft_sources _pffft_notices)
    if(NOT EXISTS "${DSD_PFFFT_SOURCE_DIR}/${_file}")
        message(
            FATAL_ERROR
            "Incompatible PFFFT source directory ${DSD_PFFFT_SOURCE_DIR}: missing ${_file}. Use the pinned pffft-neo revision; for offline builds set FETCHCONTENT_SOURCE_DIR_DSD_PFFFT."
        )
    endif()
endforeach()
file(READ "${DSD_PFFFT_SOURCE_DIR}/src/simd/pf_fma_x86.h" _pffft_fma)
if(NOT _pffft_fma MATCHES "PFFFT_DISABLE_FMA")
    message(
        FATAL_ERROR
        "Incompatible PFFFT revision: PFFFT_DISABLE_FMA support is required. Use the pinned pffft-neo revision."
    )
endif()

option(DSD_PFFFT_ENABLE_SIMD "Use compiler-target SIMD in the embedded FFT" ON)
add_library(dsd-neo_pffft STATIC ${PFFFT_SOURCES})
set_target_properties(
    dsd-neo_pffft
    PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_VISIBILITY_PRESET hidden
)
target_include_directories(dsd-neo_pffft SYSTEM PUBLIC "${PFFFT_INCLUDE_DIR}")
# Consumers of this internal static target must also avoid Windows dllimport.
target_compile_definitions(dsd-neo_pffft PUBLIC PFFFT_STATIC_DEFINE)
target_compile_definitions(
    dsd-neo_pffft
    PRIVATE $<$<CONFIG:Debug>:PFFFT_DISABLE_FMA>
)
target_link_libraries(dsd-neo_pffft PRIVATE dsd-neo_warnings)
if(DSD_PFFFT_ENABLE_SIMD)
    # pffft-neo selects SSE or NEON from the compiler's target features;
    # PFFFT_ENABLE_NEON is its opt-in for NEON on ARM64EC.
    target_compile_definitions(dsd-neo_pffft PRIVATE PFFFT_ENABLE_NEON)
    # The 8-lane AVX2 FFT backend: GCC and Clang build it inside pffft.c;
    # MSVC (not clang-cl) needs it as a translation unit of its own, compiled
    # with /arch:AVX2 (pffft-neo's src/pffft_wide_tu.c). pffft.c only calls
    # it after its CPU check; /GL- keeps link-time code generation from
    # inlining AVX2 code into code that runs first.
    if(MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
        include(CheckCSourceCompiles)
        check_c_source_compiles(
            [=[
            #if !defined(_M_X64) || defined(_M_ARM64EC)
            #error not an x64 target
            #endif
            int main(void) { return 0; }
          ]=]
            DSD_PFFFT_MSVC_X64
        )
        if(DSD_PFFFT_MSVC_X64)
            set(_dsd_pffft_wide_tu
                "${DSD_PFFFT_SOURCE_DIR}/src/pffft_wide_tu.c"
            )
            target_sources(dsd-neo_pffft PRIVATE "${_dsd_pffft_wide_tu}")
            set_source_files_properties(
                "${_dsd_pffft_wide_tu}"
                PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/GL-"
            )
            target_compile_definitions(dsd-neo_pffft PRIVATE PFFFT_WIDE_TU=1)
            unset(_dsd_pffft_wide_tu)
        endif()
    endif()
else()
    target_compile_definitions(dsd-neo_pffft PRIVATE PFFFT_SIMD_DISABLE)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
        dsd-neo_pffft
        PRIVATE $<$<CONFIG:Debug>:-ffp-contract=off>
    )
elseif(MSVC)
    target_compile_options(dsd-neo_pffft PRIVATE $<$<CONFIG:Debug>:/fp:strict>)
endif()

# One stable build-tree location serves install(), Qt resources and packaging.
set(DSD_PFFFT_LICENSE_DIR "${PROJECT_BINARY_DIR}/pffft-licenses")
file(MAKE_DIRECTORY "${DSD_PFFFT_LICENSE_DIR}")
# pffft-neo's README (its copyright notice and GPL-2.0-or-later grant), its
# GPL text and the inherited PFFFT/FFTPACK license. Its record of every
# retained upstream notice ships from LICENSES/ (below).
foreach(
    _pair
    IN
    ITEMS
        "README.md|pffft-neo-README.md"
        "LICENSE|pffft-neo-LICENSE.txt"
        "third_party/pffft/LICENSE.txt|pffft-LICENSE.txt"
)
    string(REPLACE "|" ";" _pair "${_pair}")
    list(GET _pair 0 _from)
    list(GET _pair 1 _to)
    configure_file(
        "${DSD_PFFFT_SOURCE_DIR}/${_from}"
        "${DSD_PFFFT_LICENSE_DIR}/${_to}"
        COPYONLY
    )
endforeach()

# The component inventory declares pffft-neo as GPL-2.0-or-later AND
# LicenseRef-PFFFT, so the LicenseRef text is tracked in LICENSES/ (installed
# with the other license texts and recorded in SBOMs). It must be the pinned
# source's own record; tools/update_pffft.py refreshes it with the pin.
set(_pffft_licenseref
    "${CMAKE_CURRENT_LIST_DIR}/../LICENSES/LicenseRef-PFFFT.txt"
)
set_property(
    DIRECTORY
    APPEND
    PROPERTY
        CMAKE_CONFIGURE_DEPENDS
            "${_pffft_licenseref}"
            "${DSD_PFFFT_SOURCE_DIR}/third_party/pffft/LicenseRef-PFFFT.txt"
)
file(READ "${_pffft_licenseref}" _pffft_tracked)
file(
    READ "${DSD_PFFFT_SOURCE_DIR}/third_party/pffft/LicenseRef-PFFFT.txt"
    _pffft_pinned
)
string(REPLACE "\r\n" "\n" _pffft_tracked "${_pffft_tracked}")
string(REPLACE "\r\n" "\n" _pffft_pinned "${_pffft_pinned}")
if(NOT _pffft_tracked STREQUAL _pffft_pinned)
    message(
        FATAL_ERROR
        "LICENSES/LicenseRef-PFFFT.txt differs from the pinned pffft-neo's third_party/pffft/LicenseRef-PFFFT.txt; refresh it with tools/update_pffft.py."
    )
endif()
