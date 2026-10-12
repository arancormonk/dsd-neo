# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned for reproducible Windows/vcpkg builds. See docs/supply-chain-guardrails.md
# for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO arancormonk/mbelib-neo
    REF fb25420a7542a5d22bfd070191b79d1200b029f7
    SHA512 ec4070b494327f8baac7ba5a4cfc06e87c435893e614b1ea5b541a5ad387367b35856c02efb5ef030dc8492f4f35331b70e8ead0fd3b2666f2f7a0e969170e4f
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
