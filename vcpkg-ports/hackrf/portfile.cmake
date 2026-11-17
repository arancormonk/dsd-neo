# SPDX-License-Identifier: GPL-3.0-or-later
# libhackrf for the SoapyHackRF module in the Windows ZIP; vcpkg has no hackrf
# port. Pinned to release v2026.01.3, HACKRF_SHA in
# tools/ci-dependency-pins.env (see docs/supply-chain-guardrails.md). The
# CMakeLists.txt beside this file builds only the BSD-3-Clause library in
# host/libhackrf/src, linked to the same libusb and PThreads4W as libairspy,
# instead of upstream's build, which also copies those DLLs into its install.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO greatscottgadgets/hackrf
    REF 1cfe7dfe98d333450217d50e3f3a1ad0702e000f
    SHA512 b981eae0790f81c43bdedb178a1e2ed40e04152febfc6f1c7a0c5ea539fd3fcba059dfcc5ebec847899318d2d61f247f2c9687ac28dd63e5b0e5384dc4f6ea30
)
file(
    COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"
    DESTINATION "${SOURCE_PATH}"
)
vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_copy_pdbs()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(
    INSTALL "${CMAKE_CURRENT_LIST_DIR}/copyright"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)
