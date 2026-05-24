# Pinned for reproducible Windows/vcpkg builds. See docs/supply-chain-guardrails.md
# for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO arancormonk/codec2
    REF ad5e23bfdf93896a7344e06a5ff65aa76bcc5a44
    SHA512 57ccdacb2f6b7716fab642b3a401399f84b982b4a5edbd644931a4db6588e1f1ba2ea033537eebd5938a163c6c98522213ab6a987a1c3ed5c3c36fdab87773a7
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUNITTEST=OFF
        -DLPCNET=OFF
        -DCODEC2_BUILD_PROGRAMS=OFF
        -DCODEC2_BUILD_DEMOS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/${PORT})
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
