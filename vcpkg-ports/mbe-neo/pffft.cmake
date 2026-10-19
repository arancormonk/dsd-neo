# SPDX-License-Identifier: GPL-3.0-or-later
# Acquired by vcpkg before configuring mbelib, including offline asset-cache use.
# These pins are mirrored in tools/ci-dependency-pins.env independently of DSD's FFT.
vcpkg_from_github(
    OUT_SOURCE_PATH MBELIB_PFFFT_SOURCE_PATH
    REPO arancormonk/pffft
    REF 77a7ecda1f58f041168636c99be3e6e968d984d7
    SHA512 946faeab54f77e3a716384635bef50daecd67a8c6a634cae54de70ba7f93c52ec86ef55ade02c0600c01f8c30b7b3a26a88c52a83a0522488f8b9e3df984fb0b
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
