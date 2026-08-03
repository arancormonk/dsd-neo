# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
#
# Stages the directory androiddeployqt packages into the APK: android/package plus the
# license/notice set every shipped asset carries (docs/release-assets-checklist.md).
#
# Usage:
#   cmake -DREPO_DIR=<repo> -DSOURCE_DIR=<android/package> -DSTAGE_DIR=<out> \
#         -P cmake/stage_android_package.cmake
#
# Run as a build step rather than at configure time so edits to android/package/ reach the
# APK without re-running CMake. file(COPY) and configure_file(COPYONLY) both skip files
# that are already current, so a no-op build does not make Gradle repackage.

foreach(_required IN ITEMS REPO_DIR SOURCE_DIR STAGE_DIR)
    if(NOT DEFINED ${_required})
        message(
            FATAL_ERROR
            "stage_android_package.cmake requires -D${_required}=<path>"
        )
    endif()
endforeach()

set(_doc_dir "${STAGE_DIR}/assets/doc/dsd-neo")

file(COPY "${SOURCE_DIR}/" DESTINATION "${STAGE_DIR}")

file(
    COPY
        "${REPO_DIR}/LICENSE"
        "${REPO_DIR}/COPYRIGHT"
        "${REPO_DIR}/THIRD_PARTY.md"
    DESTINATION "${_doc_dir}"
)

# Renamed so the notice names the project it belongs to, matching the layout the desktop
# packages install to share/doc/dsd-neo/licenses.
configure_file(
    "${REPO_DIR}/src/third_party/ezpwd/lesser.txt"
    "${_doc_dir}/licenses/ezpwd-LGPL-2.1-or-later.txt"
    COPYONLY
)
configure_file(
    "${REPO_DIR}/src/third_party/pffft/COPYING"
    "${_doc_dir}/licenses/pffft-FFTPACK.txt"
    COPYONLY
)

# Compiled into the Qt resource bundle (src/ui/qt/CMakeLists.txt), so every APK
# redistributes the font and has to carry its notice.
configure_file(
    "${REPO_DIR}/src/ui/qt/fonts/DejaVu-LICENSE.txt"
    "${_doc_dir}/licenses/dejavu-fonts-Bitstream-Vera.txt"
    COPYONLY
)

# Only the Android build links these two, so only this asset ships their notices.
configure_file(
    "${REPO_DIR}/android/third_party/libusb/COPYING"
    "${_doc_dir}/licenses/libusb-LGPL-2.1-or-later.txt"
    COPYONLY
)
configure_file(
    "${REPO_DIR}/android/third_party/librtlsdr/COPYING"
    "${_doc_dir}/licenses/librtlsdr-GPL-2.0-or-later.txt"
    COPYONLY
)
