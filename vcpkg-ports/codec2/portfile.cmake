# Always track the arancormonk/codec2 main branch for Windows builds.
set(VCPKG_USE_HEAD_VERSION ON)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO arancormonk/codec2
    HEAD_REF main
    PATCHES
        fix-msvc-libm.patch
        fix-msvc-flags.patch
        fix-msvc-m-pi.patch
        fix-msvc-vla-all.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUNITTEST=OFF
        -DLPCNET=OFF
)

vcpkg_cmake_install()
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
