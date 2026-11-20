# SPDX-License-Identifier: GPL-3.0-or-later
# SoapyAirspy 0.2.1 for the Windows ZIP, pinned to SOAPY_AIRSPY_SHA in
# tools/ci-dependency-pins.env (see docs/supply-chain-guardrails.md). Built
# against the airspy overlay, so the module and dsd-neo's native Airspy input
# share one patched airspy.dll.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO pothosware/SoapyAirspy
    REF 9a906cac218bf07a7acd31efa1f2954c3a44ea4b
    SHA512 4a43eff97085cc8322ddbefa36d871cf73ee76a21ac667dfe5d3546d105b0aecd4a07091b6e4a624f899531656a11b1c49e4199283b2a3d59c4283604daa0fda
)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON"
        "-DLibAIRSPY_INCLUDE_DIRS=${CURRENT_INSTALLED_DIR}/include"
    OPTIONS_RELEASE
        "-DLibAIRSPY_LIBRARIES=${CURRENT_INSTALLED_DIR}/lib/airspy.lib"
    OPTIONS_DEBUG
        "-DLibAIRSPY_LIBRARIES=${CURRENT_INSTALLED_DIR}/debug/lib/airspy.lib"
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
