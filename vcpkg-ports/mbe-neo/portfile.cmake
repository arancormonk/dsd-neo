# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned for reproducible Windows/vcpkg builds. See docs/supply-chain-guardrails.md
# for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO arancormonk/mbelib-neo
    REF 45af667f92f97f4daa5b76f4ec5f9fc4f7909654
    SHA512 b1c2d373894e2a0248fe90fd42be0eb061193d81bb76f032baa9edab8c3a743b3dca99f4249b54ebb0a930f3b3cce3890aeab6253ba1b0415dd38846eff6bbda
)

set(_mbe_pffft_options)
set(_mbe_notices
    "${SOURCE_PATH}/LICENSE"
    "${SOURCE_PATH}/LICENSES/GPL-2.0-or-later.txt"
    "${SOURCE_PATH}/LICENSES/ISC.txt"
    "${SOURCE_PATH}/LICENSES/LicenseRef-PFFFT.txt"
    "${SOURCE_PATH}/LICENSES/LicenseRef-ETSI-EN-300-395-2-data.txt"
    "${SOURCE_PATH}/docs/upstream-mbelib-attribution.md"
    "${SOURCE_PATH}/legal/status.md"
    "${SOURCE_PATH}/legal/patents.md"
)
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
        # Binary packages run on other machines; never tune for the build host.
        -DMBELIB_ENABLE_NATIVE=OFF
        ${_mbe_pffft_options}
    MAYBE_UNUSED_VARIABLES
        MBELIB_ENABLE_NATIVE
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
