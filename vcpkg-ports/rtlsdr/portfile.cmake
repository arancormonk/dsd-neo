# Overlay of vcpkg's builtin rtlsdr port (stuck at 2.0.2, which predates the
# RTL-SDR Blog V4 Lite). Pinned to the arancormonk/rtl-sdr mirror at the same
# commit as RTL_SDR_SHA in tools/ci-dependency-pins.env: osmocom v2.0.3, the
# first release with RTL-SDR Blog V4 and V4 Lite (R828D/R828S) support.
# dependencies.diff/library-linkage.diff/tools.diff are vcpkg's port patches
# rebased onto that snapshot (the builtin ones no longer apply cleanly).
# Drop this overlay once the builtin port ships >= 2.0.3.
# See docs/supply-chain-guardrails.md for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO arancormonk/rtl-sdr
    REF 797f8143266d983c56d8f35d2d442527529dd8a5
    SHA512 7a5885ea217dd848eb9a7eec97c1e55e3ca6e2a2163971237d95e55e2ab9ef957b8f7c7f91b35b5410fd68d8e54d3868ff48bd016a3d675ba9a41e85b144f2de
    PATCHES
        dependencies.diff
        library-linkage.diff
        tools.diff
)

vcpkg_check_features(OUT_FEATURE_OPTIONS options
    FEATURES
        tools   BUILD_TOOLS
)

vcpkg_find_acquire_program(PKGCONFIG)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${options}
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
        "-DCMAKE_REQUIRE_FIND_PACKAGE_PkgConfig=1"
        "-DCMAKE_DISABLE_FIND_PACKAGE_Git=1"
    OPTIONS_DEBUG
        -DBUILD_TOOLS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/rtlsdr)
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

if(VCPKG_TARGET_IS_WINDOWS AND VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/lib/pkgconfig/librtlsdr.pc" " -lrtlsdr" " -lrtlsdr_static")
    if(NOT VCPKG_BUILD_TYPE)
        vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/librtlsdr.pc" " -lrtlsdr" " -lrtlsdr_static")
    endif()
endif()

if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES rtl_adsb rtl_biast rtl_eeprom rtl_fm rtl_power rtl_sdr rtl_tcp rtl_test  AUTO_CLEAN)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(
    INSTALL "${CURRENT_PORT_DIR}/usage"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
