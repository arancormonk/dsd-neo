# SPDX-License-Identifier: GPL-3.0-or-later
vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO airspy/airspyone_host
    REF bd15be38e91ebaa3e0bebb1e320255bde4ccf059
    SHA512 a6f302459d0a0a5bdff83b5ca24186b795969beed52e98e58e2cdf9e867b9c39c626f98c7bb228dc367fb6174475b099b4bbecfa876df6983651ed063fd7c5ab
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
