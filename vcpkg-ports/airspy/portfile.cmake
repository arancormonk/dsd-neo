# SPDX-License-Identifier: GPL-3.0-or-later
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

# dsd-neo's libairspy patch, the file the Android app's vendored copy carries
# (tools/usb_dependency_patches.sh). vcpkg keys its binary cache on this port's
# own files, so the series digest is recorded here, which makes a patch change a
# new cache key. tools/check_vcpkg_overlay_pins.sh (guardrails CI and pre-push)
# fails while the digest is stale and --fix records the new one; the check below
# only runs when the cache misses, since a hit reuses the old build.
set(DSD_NEO_PATCH_SERIES 0001-libairspy-wait-for-cancelled-transfers.patch)
set(DSD_NEO_PATCH_SERIES_SHA256
    c9077bc980b1e2ad07131c9203bea3ea43096bc4605610225dd256a135b57573
)
set(dsd_neo_patches "")
set(dsd_neo_series_text "")
foreach(name IN LISTS DSD_NEO_PATCH_SERIES)
    set(path
        "${CMAKE_CURRENT_LIST_DIR}/../../android/third_party/patches/${name}"
    )
    file(READ "${path}" text)
    string(APPEND dsd_neo_series_text "${text}")
    list(APPEND dsd_neo_patches "${path}")
endforeach()
string(SHA256 dsd_neo_series_sha256 "${dsd_neo_series_text}")
if(NOT dsd_neo_series_sha256 STREQUAL DSD_NEO_PATCH_SERIES_SHA256)
    message(
        FATAL_ERROR
        "The libairspy patch series changed (SHA256 ${dsd_neo_series_sha256}); "
        "run tools/check_vcpkg_overlay_pins.sh --fix"
    )
endif()

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO airspy/airspyone_host
    REF bd15be38e91ebaa3e0bebb1e320255bde4ccf059
    SHA512 a6f302459d0a0a5bdff83b5ca24186b795969beed52e98e58e2cdf9e867b9c39c626f98c7bb228dc367fb6174475b099b4bbecfa876df6983651ed063fd7c5ab
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
