# SPDX-License-Identifier: GPL-3.0-or-later
# Acquired by vcpkg before configuring mbelib, including offline asset-cache use.
# These pins are mirrored in tools/ci-dependency-pins.env independently of DSD's FFT.
vcpkg_from_github(
    OUT_SOURCE_PATH MBELIB_PFFFT_SOURCE_PATH
    REPO arancormonk/pffft
    REF 316f0b01e3bdd327e89933891ea2c0be2d10c75e
    SHA512 0f3c7076ec57ef92a8b68b22263c520060560fea41a838e7438950045b2aa9bff233cb21f74a980b6c48afd74f197527e88a07e1b9c8ba1d6c593b3cc6a2ca3b
)
file(
    STRINGS "${SOURCE_PATH}/tools/ci-dependency-pins.env"
    _mbe_fft_pin
    REGEX "^PFFFT_SHA="
)
file(READ "${CMAKE_CURRENT_LIST_FILE}" _mbe_fft_port)
string(REGEX MATCH "REF ([0-9a-f]+)" _mbe_fft_ref "${_mbe_fft_port}")
if(NOT _mbe_fft_pin STREQUAL "PFFFT_SHA=${CMAKE_MATCH_1}")
    message(
        FATAL_ERROR
        "Refresh the mbe-neo PFFFT overlay pin to match the selected mbelib revision"
    )
endif()
