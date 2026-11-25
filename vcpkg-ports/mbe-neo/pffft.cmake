# SPDX-License-Identifier: GPL-3.0-or-later
# Acquired by vcpkg before configuring mbelib, including offline asset-cache use.
# These pins are mirrored in tools/ci-dependency-pins.env independently of DSD's FFT.
vcpkg_from_github(
    OUT_SOURCE_PATH MBELIB_PFFFT_SOURCE_PATH
    REPO arancormonk/pffft-neo
    REF 0a3abd82194444e6475efac08cfc620f299f3124
    SHA512 184789982ca949d30aeafd198426373ffb1bd3c0791191c746763092e1cfa7b421926c9792a6ae7604744d1483d83478b986e326639f40679c7101b606727d6b
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
        "Refresh the mbe-neo pffft-neo overlay pin to match the selected mbelib revision"
    )
endif()
