# SPDX-License-Identifier: GPL-3.0-or-later
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO airspy/airspyone_host
    REF fc61ab6be57ed61f0e2bdd9c6dfae74cacef57d0
    SHA512 c679832a2cbd873974aec8dc2b964d2a56dc604fbc2b4f86ad14d7d9ec026c17ac1832a56808e24b4c9e9767589afc5996021ca15bc5f4d8315b3fa3e0db1143
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
