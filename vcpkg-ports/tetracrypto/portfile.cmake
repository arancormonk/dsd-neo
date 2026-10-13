# Pinned for reproducible vcpkg builds. See docs/supply-chain-guardrails.md
# for the refresh process.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO MidnightBlueLabs/TETRA_crypto
    REF defed030f6155ac70ff5d8bea97b348cb92ee1f6
    SHA512 3079aa4f6c4869e2bea06a5d4447ede287674f5fe5cd72ec6afb8a896f1034c34d33e9a1708e414154f5cbce9c49342736f80ce26c54bb9c02e17ef134add1b6
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/gcc_compat.h" DESTINATION "${SOURCE_PATH}")

vcpkg_cmake_configure(SOURCE_PATH "${SOURCE_PATH}")
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME tetracrypto CONFIG_PATH share/tetracrypto)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
