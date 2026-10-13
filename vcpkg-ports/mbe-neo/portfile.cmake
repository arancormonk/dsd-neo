# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned for reproducible Windows/vcpkg builds. See docs/supply-chain-guardrails.md
# for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO arancormonk/mbelib-neo
    REF 63fb9dd029209f23a5912e01ccd92c0efa54587c
    SHA512 ec1539f78aac1c4afa3280d9031952f8115dcca1d56fedb8a6746a60a9dba65a6484d6e4af2fd7149121fa1c57fec786038f9702ef4041ca8e59c5901e23a025
)

set(_mbe_pffft_options)
set(_mbe_notices "${SOURCE_PATH}/LICENSE")
# Releases from 2.4.1 need an offline override; retain support for rollback.
if(EXISTS "${SOURCE_PATH}/cmake/Pffft.cmake")
    include("${CMAKE_CURRENT_LIST_DIR}/pffft.cmake")
    list(
        APPEND _mbe_pffft_options
        "-DFETCHCONTENT_SOURCE_DIR_MBELIB_PFFFT=${MBELIB_PFFFT_SOURCE_PATH}"
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON
    )
    list(
        APPEND _mbe_notices
        "${MBELIB_PFFFT_SOURCE_PATH}/LICENSE.txt"
        "${CURRENT_PACKAGES_DIR}/share/doc/mbelib-neo/pffft/PFFFT-NOTICES.txt"
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMBELIB_BUILD_TESTS=OFF
        -DMBELIB_BUILD_EXAMPLES=OFF
        -DMBELIB_BUILD_DOCS=OFF
        ${_mbe_pffft_options}
)

vcpkg_cmake_install()

vcpkg_cmake_config_fixup(
    PACKAGE_NAME mbe-neo
    CONFIG_PATH lib/cmake/mbe-neo
)

vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST ${_mbe_notices})
