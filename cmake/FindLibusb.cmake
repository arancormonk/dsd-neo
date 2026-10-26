# SPDX-License-Identifier: GPL-3.0-or-later
# - Find libusb-1.0, for the RTL-SDR USB identity and reset unit
#   (src/io/radio/rtl_usb_identity.cpp). librtlsdr links it already; dsd-neo
#   calls it directly only to identify and reset the running dongle.
# Once done this will define
#  LIBUSB_FOUND - System has libusb-1.0
#  LIBUSB_INCLUDE_DIR - The directory holding libusb.h
#  LIBUSB_LIBRARY - The libusb-1.0 library (FreeBSD's base libusb as "usb")
#  Libusb::Libusb - Imported target for both
#
# Searched with the hints FindRTLSDR.cmake uses (pkg-config for libusb-1.0 and
# librtlsdr, RTLSDR_ROOT), LIBUSB_ROOT, and the Homebrew, vcpkg and MSYS2/MinGW
# install roots.

if(NOT LIBUSB_FOUND)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(PC_LIBUSB libusb-1.0 QUIET)
        pkg_check_modules(PC_RTLSDR librtlsdr QUIET)
    endif()

    set(_LIBUSB_ROOT_HINTS ${LIBUSB_ROOT} ${RTLSDR_ROOT})
    if(DEFINED ENV{LIBUSB_ROOT})
        list(APPEND _LIBUSB_ROOT_HINTS "$ENV{LIBUSB_ROOT}")
    endif()
    if(DEFINED ENV{RTLSDR_ROOT})
        list(APPEND _LIBUSB_ROOT_HINTS "$ENV{RTLSDR_ROOT}")
    endif()
    # Homebrew (Apple silicon, then Intel).
    list(APPEND _LIBUSB_ROOT_HINTS /opt/homebrew /usr/local)
    # vcpkg (the toolchain also puts this on CMAKE_PREFIX_PATH).
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(
            APPEND _LIBUSB_ROOT_HINTS
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        )
    endif()
    # MSYS2/MinGW: the active environment, then the default install's.
    if(DEFINED ENV{MINGW_PREFIX})
        list(APPEND _LIBUSB_ROOT_HINTS "$ENV{MINGW_PREFIX}")
    endif()
    if(WIN32)
        list(
            APPEND _LIBUSB_ROOT_HINTS
            C:/msys64/ucrt64
            C:/msys64/mingw64
            C:/msys64/clang64
        )
    endif()

    find_path(
        LIBUSB_INCLUDE_DIR
        NAMES libusb.h
        HINTS
            ${PC_LIBUSB_INCLUDEDIR}
            ${PC_LIBUSB_INCLUDE_DIRS}
            ${PC_RTLSDR_INCLUDEDIR}
            ${PC_RTLSDR_INCLUDE_DIRS}
            ${_LIBUSB_ROOT_HINTS}
        PATH_SUFFIXES include/libusb-1.0 libusb-1.0 include
    )

    find_library(
        LIBUSB_LIBRARY
        NAMES usb-1.0 libusb-1.0 usb
        HINTS
            ${PC_LIBUSB_LIBDIR}
            ${PC_LIBUSB_LIBRARY_DIRS}
            ${PC_RTLSDR_LIBDIR}
            ${PC_RTLSDR_LIBRARY_DIRS}
            ${_LIBUSB_ROOT_HINTS}
        PATH_SUFFIXES lib lib64
    )
    unset(_LIBUSB_ROOT_HINTS)

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(
        Libusb
        DEFAULT_MSG
        LIBUSB_LIBRARY
        LIBUSB_INCLUDE_DIR
    )
    mark_as_advanced(LIBUSB_INCLUDE_DIR LIBUSB_LIBRARY)

    if(LIBUSB_FOUND AND NOT TARGET Libusb::Libusb)
        add_library(Libusb::Libusb UNKNOWN IMPORTED)
        set_target_properties(
            Libusb::Libusb
            PROPERTIES
                IMPORTED_LOCATION "${LIBUSB_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${LIBUSB_INCLUDE_DIR}"
        )
    endif()
endif(NOT LIBUSB_FOUND)
