# SPDX-License-Identifier: GPL-3.0-or-later
# Acquired by vcpkg before configuring mbelib, including offline asset-cache use.
# These pins are mirrored in tools/ci-dependency-pins.env independently of DSD's FFT.
vcpkg_from_github(
    OUT_SOURCE_PATH MBELIB_PFFFT_SOURCE_PATH
    REPO arancormonk/pffft
    REF 80b9c0139c91b684579115f482f90db97abffdb2
    SHA512 3b353cf869ede78af70bcd08161ea512821d2a3c729c9767c6d40c8cb6478ab06967926cfb0cdfe0710a8d054914b53c089974152703d0d26d2ff582eecc76ad
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
