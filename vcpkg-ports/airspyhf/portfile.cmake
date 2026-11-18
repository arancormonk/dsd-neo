# SPDX-License-Identifier: GPL-3.0-or-later
# libairspyhf for the SoapyAirspyHF module in the Windows ZIP; vcpkg has no
# airspyhf port. Pinned to a master commit, AIRSPYHF_SHA in
# tools/ci-dependency-pins.env (see docs/supply-chain-guardrails.md): the only
# release tag is 1.6.8 from 2020, and this commit reports 1.8.1. The
# CMakeLists.txt beside this file replaces upstream's and builds only the
# BSD-3-Clause library in libairspyhf/src, linked to the same libusb and
# PThreads4W as libairspy; the GPL-2.0 tools and their LGPL getopt are not built.
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

# dsd-neo's libairspyhf patch: the stop waits for every cancelled USB transfer
# before the transfers are freed or resubmitted, a failed start takes back the
# transfers it submitted, and a retune while streaming changes what the sample
# consumer reads only between two blocks (the patch header has the details;
# upstream has no fix at this commit). It is the series
# tools/usb_dependency_patches.sh applies to the source the
# DESKTOP_LIBAIRSPYHF_TRANSFERS test builds, so the digest is recorded here:
# tools/check_vcpkg_overlay_pins.sh (guardrails CI and pre-push) fails while it
# is stale and --fix records the new one; the check below only runs when the
# binary cache misses. airspyhf.c has CRLF line endings, so the patch carries
# carriage returns, which file(READ) drops: the digest comes from the file
# itself. The series is that one patch, so its digest is the series digest.
set(DSD_NEO_PATCH_SERIES 0001-libairspyhf-wait-for-cancelled-transfers.patch)
set(DSD_NEO_PATCH_SERIES_SHA256
    dece0fe7079d5b717e1801c879561e67e8615f63ff679dcf436ce2c762874725
)
set(dsd_neo_patches "${CMAKE_CURRENT_LIST_DIR}/${DSD_NEO_PATCH_SERIES}")
file(SHA256 "${dsd_neo_patches}" dsd_neo_series_sha256)
if(NOT dsd_neo_series_sha256 STREQUAL DSD_NEO_PATCH_SERIES_SHA256)
    message(
        FATAL_ERROR
        "The libairspyhf patch series changed (SHA256 ${dsd_neo_series_sha256}); "
        "run tools/check_vcpkg_overlay_pins.sh --fix"
    )
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO airspy/airspyhf
    REF 24fe8ffcb00b14f827268bbad89ae1392de055e5
    SHA512 5aaf05b3817159419268937f5ae99201b5ea81fbcd8c58b5102d83874aec93e5e18be518959d4495ef387a542362db6818212a599a89f96cc6736734c70f77a6
    PATCHES ${dsd_neo_patches}
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
