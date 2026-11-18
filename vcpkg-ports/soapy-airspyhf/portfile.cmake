# SPDX-License-Identifier: GPL-3.0-or-later
# SoapyAirspyHF 0.2.1 for the Windows ZIP, pinned to SOAPY_AIRSPYHF_SHA in
# tools/ci-dependency-pins.env (see docs/supply-chain-guardrails.md). Built
# against the airspyhf overlay.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

# dsd-neo's SoapyAirspyHF patch: the module reports a frequency, sample-rate,
# gain or stream start the device refuses, and reports a setting only once the
# device has taken it (the patch header has the details; upstream has no fix at
# this commit). It is the series tools/usb_dependency_patches.sh applies to the
# source the DESKTOP_SOAPYAIRSPYHF_ERRORS test builds, so the digest is recorded
# here: tools/check_vcpkg_overlay_pins.sh (guardrails CI and pre-push) fails
# while it is stale and --fix records the new one; the check below only runs
# when the binary cache misses. The series is that one patch, so its digest is
# the series digest.
set(DSD_NEO_PATCH_SERIES 0001-soapyairspyhf-report-device-errors.patch)
set(DSD_NEO_PATCH_SERIES_SHA256
    756e976bc38044d0462325eb614f01132b292436cb4e960f6f1ddeaa7defead1
)
set(dsd_neo_patches "${CMAKE_CURRENT_LIST_DIR}/${DSD_NEO_PATCH_SERIES}")
file(SHA256 "${dsd_neo_patches}" dsd_neo_series_sha256)
if(NOT dsd_neo_series_sha256 STREQUAL DSD_NEO_PATCH_SERIES_SHA256)
    message(
        FATAL_ERROR
        "The SoapyAirspyHF patch series changed (SHA256 ${dsd_neo_series_sha256}); "
        "run tools/check_vcpkg_overlay_pins.sh --fix"
    )
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO pothosware/SoapyAirspyHF
    REF 0b3daf0b47c3c1d0857c873bb184f6a86a37333c
    SHA512 a8af84960cab0df1a9459b38a0b612534c18820062963bab156222e9a4898c3ed2e99e1771985ff03a25bdbe9851ba8ca25118fdfac3191c00613016d346fe9c
    PATCHES ${dsd_neo_patches}
)
# Upstream turns gain control on (HASGAINS) only when
# check_function_exists(airspyhf_set_hf_att) links with -lairspyhf, which is
# not an MSVC library argument, so an MSVC build would silently drop the LNA,
# attenuator and AGC controls. The pinned libairspyhf has those functions, and
# check_function_exists skips its probe when the result is already set.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        "-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON"
        "-DHAVE_GAIN_API=1"
        "-DLibAIRSPYHF_INCLUDE_DIRS=${CURRENT_INSTALLED_DIR}/include"
    OPTIONS_RELEASE
        "-DLibAIRSPYHF_LIBRARIES=${CURRENT_INSTALLED_DIR}/lib/airspyhf.lib"
    OPTIONS_DEBUG
        "-DLibAIRSPYHF_LIBRARIES=${CURRENT_INSTALLED_DIR}/debug/lib/airspyhf.lib"
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
