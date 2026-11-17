# Overlay of vcpkg's builtin soapysdr port (0.8.1 at the manifest baseline),
# which never builds SoapySDR's apps. The tools feature adds SoapySDRUtil: the
# Windows ZIP ships it next to SoapySDR.dll, so a user can list the modules that
# copy of SoapySDR loads (--info) and check a driver (--check=<driver>).
# Pinned to the soapy-sdr-0.8.1 tag's commit, SOAPY_SDR_SHA in
# tools/ci-dependency-pins.env, the same commit the AppImage builds. The
# library is built as the builtin port builds it. Drop this overlay once the
# builtin port has a tools feature.
# See docs/supply-chain-guardrails.md for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO pothosware/SoapySDR
    REF 1cf5a539a21414ff509ff7d0eedfc5fa8edb90c6
    SHA512 b31d344c43e3be47cc8d00fbd0af10691c85c353f5c0e46aa1ae6751f21e257347b94933f347cd7123f2aaf67af7d7ec8cfffe6f04b03b73979bac5d5a17a588
)

vcpkg_check_features(OUT_FEATURE_OPTIONS options
    FEATURES
        tools   ENABLE_APPS
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${options}
        -DENABLE_DOCS=OFF
        -DENABLE_PYTHON=OFF
        -DENABLE_PYTHON3=OFF
        -DENABLE_TESTS=OFF
    OPTIONS_DEBUG
        -DENABLE_APPS=OFF
)

vcpkg_cmake_install()

vcpkg_fixup_pkgconfig()

vcpkg_cmake_config_fixup(CONFIG_PATH cmake)

if("tools" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES SoapySDRUtil AUTO_CLEAN)
    # Its manual page is of no use on Windows, the only platform that ships it.
    file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/man")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(
    INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE_1_0.txt")
