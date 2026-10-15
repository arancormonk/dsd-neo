# SPDX-License-Identifier: GPL-3.0-or-later
# Compile the pinned TEA keystream generators into DSD-neo. Upstream ships only
# a Makefile, so nothing is configured from the archive.
include(FetchContent)

set(_tetra_crypto_pins
    "${CMAKE_CURRENT_LIST_DIR}/../tools/ci-dependency-pins.env"
)
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS "${_tetra_crypto_pins}"
)
foreach(_pin TETRA_CRYPTO_SHA TETRA_CRYPTO_ARCHIVE_SHA256)
    file(STRINGS "${_tetra_crypto_pins}" _lines REGEX "^${_pin}=")
    list(LENGTH _lines _count)
    if(NOT _count EQUAL 1)
        message(FATAL_ERROR "Expected one ${_pin} in ${_tetra_crypto_pins}")
    endif()
    string(REPLACE "${_pin}=" "" ${_pin} "${_lines}")
    string(LENGTH "${${_pin}}" _length)
    if(_pin STREQUAL "TETRA_CRYPTO_SHA")
        set(_expected 40)
    else()
        set(_expected 64)
    endif()
    if(NOT _length EQUAL _expected OR NOT "${${_pin}}" MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "Invalid ${_pin} in ${_tetra_crypto_pins}")
    endif()
endforeach()

if(FETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO)
    if(
        NOT IS_ABSOLUTE "${FETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO}"
        OR NOT IS_DIRECTORY "${FETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO}"
    )
        message(
            FATAL_ERROR
            "FETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO must name an existing absolute TETRA_crypto source directory"
        )
    endif()
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(
    dsd_tetra_crypto
    URL
        "https://codeload.github.com/MidnightBlueLabs/TETRA_crypto/tar.gz/${TETRA_CRYPTO_SHA}"
    URL_HASH "SHA256=${TETRA_CRYPTO_ARCHIVE_SHA256}"
    TLS_VERIFY TRUE
    # Supported since CMake 3.18: a SOURCE_SUBDIR that does not exist populates
    # sources without add_subdirectory(), even if upstream gains a CMakeLists.txt.
    SOURCE_SUBDIR no-cmake-project
)
FetchContent_MakeAvailable(dsd_tetra_crypto)

set(DSD_TETRA_CRYPTO_SOURCE_DIR
    "${dsd_tetra_crypto_SOURCE_DIR}"
    CACHE INTERNAL
    "TETRA_crypto source directory for analyzers"
    FORCE
)
set(TETRA_CRYPTO_SOURCES
    "${DSD_TETRA_CRYPTO_SOURCE_DIR}/tea1.c"
    "${DSD_TETRA_CRYPTO_SOURCE_DIR}/tea2.c"
    "${DSD_TETRA_CRYPTO_SOURCE_DIR}/tea3.c"
)
foreach(
    _file
    IN
    ITEMS tea1.c tea1.h tea2.c tea2.h tea3.c tea3.h LICENSE
)
    if(NOT EXISTS "${DSD_TETRA_CRYPTO_SOURCE_DIR}/${_file}")
        message(
            FATAL_ERROR
            "Incompatible TETRA_crypto source directory ${DSD_TETRA_CRYPTO_SOURCE_DIR}: missing ${_file}. Use the pinned revision; for offline builds set FETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO."
        )
    endif()
endforeach()

# Only the three keystream generators are compiled. They include nothing but
# standard headers and their own tea*.h, so the endian helpers and the
# assert-based build_iv() in common.c stay out of the build.
add_library(dsd-neo_tetra_crypto STATIC ${TETRA_CRYPTO_SOURCES})
set_target_properties(
    dsd-neo_tetra_crypto
    PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_VISIBILITY_PRESET hidden
)
target_include_directories(
    dsd-neo_tetra_crypto
    SYSTEM
    PUBLIC "${DSD_TETRA_CRYPTO_SOURCE_DIR}"
)
# Upstream code is not held to the project warning set; it still gets the
# release hardening flags.
target_link_libraries(dsd-neo_tetra_crypto PRIVATE dsd-neo_hardening)
if(MSVC)
    # The keystream loops compare an int index with a uint32_t byte count.
    target_compile_options(dsd-neo_tetra_crypto PRIVATE /wd4018)
endif()

# One stable build-tree location serves install(), Qt resources and packaging.
set(DSD_TETRA_CRYPTO_LICENSE_DIR "${PROJECT_BINARY_DIR}/tetra_crypto-licenses")
file(MAKE_DIRECTORY "${DSD_TETRA_CRYPTO_LICENSE_DIR}")
configure_file(
    "${DSD_TETRA_CRYPTO_SOURCE_DIR}/LICENSE"
    "${DSD_TETRA_CRYPTO_LICENSE_DIR}/tetra_crypto-LICENSE.txt"
    COPYONLY
)
