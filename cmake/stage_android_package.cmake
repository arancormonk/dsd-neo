# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# Stages the directory androiddeployqt packages into the APK: android/package plus the
# license/notice set every shipped asset carries (docs/release-assets-checklist.md): the
# project's legal files, LICENSES/, the selected mbelib's notices, and the notices the
# android_release map of each record in legal/components.json names
# (cmake/stage_release_notices.cmake).
#
# Usage:
#   cmake -DREPO_DIR=<repo> -DSOURCE_DIR=<android/package> -DSTAGE_DIR=<out> \
#         -DBUILD_DIR=<build> \
#         -DMBELIB_NOTICE_DIR=<prefix>/share/doc/mbelib-neo \
#         -DVCPKG_SHARE_DIR=<vcpkg_installed>/<triplet>/share \
#         -P cmake/stage_android_package.cmake
#
# Run as a build step rather than at configure time so edits to android/package/ reach the
# APK without re-running CMake. file(COPY) and configure_file(COPYONLY) both skip files
# that are already current, so a no-op build does not make Gradle repackage.

foreach(
    _required
    IN
    ITEMS
        REPO_DIR
        SOURCE_DIR
        STAGE_DIR
        BUILD_DIR
        MBELIB_NOTICE_DIR
        VCPKG_SHARE_DIR
)
    if(NOT DEFINED ${_required})
        message(
            FATAL_ERROR
            "stage_android_package.cmake requires -D${_required}=<path>"
        )
    endif()
endforeach()

set(_doc_dir "${STAGE_DIR}/assets/doc/dsd-neo")
set(DSD_MBELIB_NOTICE_DIR "${MBELIB_NOTICE_DIR}")
include("${REPO_DIR}/cmake/MbelibNotices.cmake")

file(COPY "${SOURCE_DIR}/" DESTINATION "${STAGE_DIR}")
file(COPY "${REPO_DIR}/legal" DESTINATION "${_doc_dir}")
file(COPY "${REPO_DIR}/LICENSES/" DESTINATION "${_doc_dir}/licenses")
file(COPY "${MBELIB_NOTICE_DIR}/" DESTINATION "${_doc_dir}/licenses/mbelib-neo")

file(
    COPY
        "${REPO_DIR}/LICENSE"
        "${REPO_DIR}/COPYRIGHT"
        "${REPO_DIR}/THIRD_PARTY.md"
    DESTINATION "${_doc_dir}"
)

# The notices of every component the APK carries, under the names its record gives them:
# the vendored USB libraries, the fonts compiled into the Qt resource bundle, the
# downloaded PFFFT and TETRA_crypto, and the libraries vcpkg links into the app.
set(PLATFORM android)
set(DOC_DIR "${_doc_dir}")
include("${REPO_DIR}/cmake/stage_release_notices.cmake")
