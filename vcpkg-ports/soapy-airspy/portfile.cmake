# SPDX-License-Identifier: GPL-3.0-or-later
# SoapyAirspy 0.2.1 for the Windows ZIP, pinned to SOAPY_AIRSPY_SHA in
# tools/ci-dependency-pins.env (see docs/supply-chain-guardrails.md). Built
# against the airspy overlay, so the module and dsd-neo's native Airspy input
# share one patched airspy.dll.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

# dsd-neo's SoapyAirspy patch: the module reports a frequency, sample-rate,
# gain or stream start the device refuses, and reports a setting only once the
# device has taken it (the patch header has the details; upstream has no fix at
# this commit). It is the series tools/usb_dependency_patches.sh applies to the
# source the DESKTOP_SOAPYAIRSPY_ERRORS test builds, so the digest is recorded
# here: tools/check_vcpkg_overlay_pins.sh (guardrails CI and pre-push) fails
# while it is stale and --fix records the new one; the check below only runs
# when the binary cache misses. The series is that one patch, so its digest is
# the series digest.
set(DSD_NEO_PATCH_SERIES 0001-soapyairspy-report-device-errors.patch)
set(DSD_NEO_PATCH_SERIES_SHA256
    d5421404b541c5f09a115b6e4c6034269abcbd3cc7e1034a68fa467e38a84087
)
set(dsd_neo_patches "${CMAKE_CURRENT_LIST_DIR}/${DSD_NEO_PATCH_SERIES}")
file(SHA256 "${dsd_neo_patches}" dsd_neo_series_sha256)
if(NOT dsd_neo_series_sha256 STREQUAL DSD_NEO_PATCH_SERIES_SHA256)
    message(
        FATAL_ERROR
        "The SoapyAirspy patch series changed (SHA256 ${dsd_neo_series_sha256}); "
        "run tools/check_vcpkg_overlay_pins.sh --fix"
    )
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO pothosware/SoapyAirspy
    REF 9a906cac218bf07a7acd31efa1f2954c3a44ea4b
    SHA512 4a43eff97085cc8322ddbefa36d871cf73ee76a21ac667dfe5d3546d105b0aecd4a07091b6e4a624f899531656a11b1c49e4199283b2a3d59c4283604daa0fda
    PATCHES ${dsd_neo_patches}
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
