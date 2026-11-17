# SPDX-License-Identifier: GPL-3.0-or-later
# libairspyhf for the SoapyAirspyHF module in the Windows ZIP; vcpkg has no
# airspyhf port. Pinned to a master commit, AIRSPYHF_SHA in
# tools/ci-dependency-pins.env (see docs/supply-chain-guardrails.md): the only
# release tag is 1.6.8 from 2020, and this commit reports 1.8.1. The
# CMakeLists.txt beside this file replaces upstream's and builds only the
# BSD-3-Clause library in libairspyhf/src, linked to the same libusb and
# PThreads4W as libairspy; the GPL-2.0 tools and their LGPL getopt are not built.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO airspy/airspyhf
    REF 24fe8ffcb00b14f827268bbad89ae1392de055e5
    SHA512 5aaf05b3817159419268937f5ae99201b5ea81fbcd8c58b5102d83874aec93e5e18be518959d4495ef387a542362db6818212a599a89f96cc6736734c70f77a6
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
