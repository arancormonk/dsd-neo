# SPDX-License-Identifier: GPL-3.0-or-later
# Pinned for reproducible Windows/vcpkg builds. See docs/supply-chain-guardrails.md
# for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO arancormonk/mbelib-neo
    REF 6fef211cfbc4e050cf3901be83ba68cfbd891021
    SHA512 583e20b08a80678ceb4df4d27a4c99ff446aada27dcf91a2f7021120d96a080f6e13d5cf38c5593abca374986d722b1d90693de2e0ef0a643ef1d0d40b2a2dd9
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
