# SPDX-License-Identifier: GPL-3.0-or-later AND MIT
# This file, vcpkg.json, jack.diff and fix-guid-linker-errors.patch (PortAudio's
# own upstream commits) are copied from vcpkg's builtin port, ports/portaudio at
# port-version 8 (git tree 47dce0abd35ae2790f4fa16d09279a80f601db93, the last
# without the asio feature), Copyright (c) Microsoft Corporation, MIT license in
# vcpkg-ports/LICENSE-vcpkg.txt. dsd-neo's changes (PA_USE_ASIOSDK declared
# MAYBE_UNUSED_VARIABLES, which keeps vcpkg from warning about it, the
# formatting and these comments) are GPL-3.0-or-later.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO PortAudio/portaudio
    REF 147dd722548358763a8b649b3e4b41dfffbcfbb6
    SHA512 0f56e5f5b004f51915f29771b8fc1fe886f1fef5d65ab5ea1db43f43c49917476b9eec14b36aa54d3e9fb4d8bdf61e68c79624d00b7e548d4c493395a758233a
    PATCHES
        jack.diff
        fix-guid-linker-errors.patch
)

string(
    COMPARE EQUAL
    "${VCPKG_CRT_LINKAGE}"
    "static"
    PA_DLL_LINK_WITH_STATIC_RUNTIME
)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" PA_BUILD_SHARED)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" PA_BUILD_STATIC)

vcpkg_list(SET options)
if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_list(APPEND options
        -DPA_USE_ASIOSDK=OFF
        -DPA_DLL_LINK_WITH_STATIC_RUNTIME=${PA_DLL_LINK_WITH_STATIC_RUNTIME}
        -DPA_LIBNAME_ADD_SUFFIX=OFF
    )
elseif(VCPKG_TARGET_IS_IOS OR VCPKG_TARGET_IS_OSX)
    vcpkg_list(APPEND options
        # Avoid absolute framework paths.
        -DCOREAUDIO_LIBRARY:STRING=-Wl,-framework,CoreAudio
        -DAUDIOTOOLBOX_LIBRARY:STRING=-Wl,-framework,AudioToolbox
        -DAUDIOUNIT_LIBRARY:STRING=-Wl,-framework,AudioUnit
        -DCOREFOUNDATION_LIBRARY:STRING=-Wl,-framework,CoreFoundation
        -DCORESERVICES_LIBRARY:STRING=-Wl,-framework,CoreServices
    )
else()
    vcpkg_list(APPEND options
        -DPA_USE_JACK=ON
        -DCMAKE_REQUIRE_FIND_PACKAGE_Jack=ON
        -DPA_USE_ALSA=OFF
    )
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${options}
        -DPA_BUILD_SHARED=${PA_BUILD_SHARED}
        -DPA_BUILD_STATIC=${PA_BUILD_STATIC}
    MAYBE_UNUSED_VARIABLES
        PA_USE_ASIOSDK
    OPTIONS_DEBUG
        -DPA_ENABLE_DEBUG_OUTPUT:BOOL=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/${PORT})
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(
    REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/share/doc"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.txt")
