# SPDX-License-Identifier: GPL-3.0-or-later
# Acquired by vcpkg before configuring mbelib, including offline asset-cache use.
# These pins are mirrored in tools/ci-dependency-pins.env independently of DSD's FFT.
vcpkg_from_github(
    OUT_SOURCE_PATH MBELIB_PFFFT_SOURCE_PATH
    REPO arancormonk/pffft
    REF f5ecc890d185eb33507b79d2e96f5310f9062d7f
    SHA512 07b34618fc301d18a8d89b34359da9a2a496fc4655edb897f8f6f2e8070be670a671056286c1aec23c6ca780cbf3175fe7a200e551abd3495dd02a881ef5ab0f
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
