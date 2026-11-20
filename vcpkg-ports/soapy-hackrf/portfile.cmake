# SPDX-License-Identifier: GPL-3.0-or-later
# SoapyHackRF 0.3.6 for the Windows ZIP, pinned to SOAPY_HACKRF_SHA in
# tools/ci-dependency-pins.env (see docs/supply-chain-guardrails.md). Built
# against the hackrf overlay.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO pothosware/SoapyHackRF
    REF 0b8d3f691ed53eae8e9837804a5dcecd313bd15a
    SHA512 d9590bacb4dd8a17dd15c98d34b96a75c383c5b1a1f01bc62df7c278450a767280f4c525e7e53b78175fb8a9ba0f1506e40e79fe1691f5b0e1b577ac6a0b0f83
)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON"
        "-DLIBHACKRF_INCLUDE_DIR=${CURRENT_INSTALLED_DIR}/include/libhackrf"
    OPTIONS_RELEASE
        "-DLIBHACKRF_LIBRARY=${CURRENT_INSTALLED_DIR}/lib/hackrf.lib"
    OPTIONS_DEBUG
        "-DLIBHACKRF_LIBRARY=${CURRENT_INSTALLED_DIR}/debug/lib/hackrf.lib"
)
vcpkg_cmake_install()

# A SoapySDR module is a DLL with no exports, no import library and no headers,
# installed to lib/SoapySDR/modules<ABI>/. Provisional: these are the policies
# such a port is expected to need; keep only those a Windows build shows it does.
set(VCPKG_POLICY_ALLOW_DLLS_IN_LIB enabled)
set(VCPKG_POLICY_DLLS_WITHOUT_LIBS enabled)
set(VCPKG_POLICY_DLLS_WITHOUT_EXPORTS enabled)
set(VCPKG_POLICY_EMPTY_INCLUDE_FOLDER enabled)
file(
    INSTALL "${CMAKE_CURRENT_LIST_DIR}/copyright"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)
