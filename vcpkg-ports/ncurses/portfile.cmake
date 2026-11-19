# SPDX-License-Identifier: GPL-3.0-or-later AND MIT
# This file, usage and vcpkg.json are copied from vcpkg's builtin port,
# ports/ncurses at the manifest baseline, vcpkg commit
# 9eec002df1cd109f8f104cd966a763daf9751489; Copyright (c) Microsoft Corporation,
# MIT license in vcpkg-ports/LICENSE-vcpkg.txt. dsd-neo's changes (the terminal
# database, the macOS file-name layout, the port-version, the formatting and
# these comments) are GPL-3.0-or-later.
# Overlay of vcpkg's builtin ncurses port, which the macOS DMG bundles. The
# builtin port installs no terminal database (--disable-db-install,
# --without-progs) and passes --enable-mixed-case, so the library looks for an
# entry such as xterm-256color in a one-letter folder (x/). macOS's own
# database, /usr/share/terminfo, uses the hexadecimal folders (78/) of a file
# system that ignores case, so that library can read neither. This overlay
# builds tic and installs the database from the same pinned archive, in the
# hexadecimal layout on macOS, where the DMG's file system ignores case too.
# Drop it once the builtin port installs a database that macOS can read.
# See docs/supply-chain-guardrails.md for the refresh process.
vcpkg_download_distfile(
    ARCHIVE_PATH
    URLS
        "https://invisible-mirror.net/archives/ncurses/ncurses-${VERSION}.tar.gz"
        "ftp://ftp.invisible-island.net/ncurses/ncurses-${VERSION}.tar.gz"
        "https://ftp.gnu.org/gnu/ncurses/ncurses-${VERSION}.tar.gz"
    FILENAME "ncurses-${VERSION}.tgz"
    SHA512 fc5a13409d2a530a1325776dcce3a99127ddc2c03999cfeb0065d0eee2d68456274fb1c7b3cc99c1937bc657d0e7fca97016e147f93c7821b5a4a6837db821e8
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE_PATH}"
)

vcpkg_list(SET OPTIONS)

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    list(
        APPEND OPTIONS
        --with-cxx-shared
        --with-shared # "lib model"
        --without-normal # "lib model"
    )
endif()

if(VCPKG_TARGET_IS_OSX)
    list(APPEND OPTIONS --disable-mixed-case)
elseif(NOT VCPKG_TARGET_IS_MINGW)
    list(APPEND OPTIONS --enable-mixed-case)
endif()

if(VCPKG_TARGET_IS_MINGW)
    list(
        APPEND OPTIONS
        --disable-home-terminfo
        --enable-term-driver
        --disable-termcap
    )
endif()

if("check-size" IN_LIST FEATURES)
    list(APPEND OPTIONS --enable-check-size)
endif()

vcpkg_cmake_get_vars(cmake_vars_file)
include("${cmake_vars_file}")

# There are compilation errors on gcc 15. adding `-std=c17` to CFLAGS for workaround.
# ref: https://gitlab.archlinux.org/archlinux/packaging/packages/ncurses/-/issues/3
list(APPEND OPTIONS "CFLAGS=\$CFLAGS -std=c17")

vcpkg_make_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    DEFAULT_OPTIONS_EXCLUDE "^--docdir"
    OPTIONS
        ${OPTIONS}
        --disable-pkg-ldflags
        --disable-rpath-hack
        --enable-pc-files
        --without-ada
        --without-debug # "lib model"
        --without-manpages
        --without-tack
        --without-tests
        --with-pkg-config-libdir=libdir
)
vcpkg_make_install()
vcpkg_fixup_pkgconfig()

# Prefer local files over search path
file(GLOB headers "${CURRENT_PACKAGES_DIR}/include/ncursesw/*.h")
foreach(file IN LISTS headers)
    vcpkg_replace_string("${file}" [[#include <ncursesw/([^>]*)>]] [[#include "\1"]] REGEX IGNORE_UNCHANGED
    )
endforeach()

vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/ncurses/bin/ncursesw6-config"  "${CURRENT_INSTALLED_DIR}" "\${prefix}")
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/ncurses/bin/ncursesw6-config"  "\nprefix=\"\${prefix}\"" [=[prefix=$(CDPATH= cd -- "$(dirname -- "$0")"/../../.. && pwd -P)]=])
if(NOT VCPKG_BUILD_TYPE)
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/ncurses/debug/bin/ncursesw6-config"  "${CURRENT_INSTALLED_DIR}" "\${prefix}")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/ncurses/debug/bin/ncursesw6-config"  "\nprefix=\"\${prefix}/debug\"" [=[prefix=$(CDPATH= cd -- "$(dirname -- "$0")"/../../../.. && pwd -P)/debug]=])
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/tools/ncurses/debug/bin/ncursesw6-config"  "\${prefix}/share" "\${prefix}/../share")
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/bin")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/bin")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

file(
    INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage"
    DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}"
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
