# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(hackrf C)
find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 REQUIRED)
find_package(PThreads4W REQUIRED)
# hackrf.h declares the API __declspec(dllexport) on Windows by itself.
add_library(hackrf host/libhackrf/src/hackrf.c)
# What upstream's host/libhackrf build passes: its project version (0.9.2) and
# the release name. Update both when HACKRF_SHA moves to another release.
target_compile_definitions(
    hackrf
    PRIVATE LIBRARY_VERSION="0.9.2" LIBRARY_RELEASE="2026.01.3"
)
target_include_directories(hackrf PRIVATE "${LIBUSB_INCLUDE_DIR}")
target_link_libraries(hackrf PRIVATE "${LIBUSB_LIBRARY}" PThreads4W::PThreads4W)
set_target_properties(hackrf PROPERTIES C_STANDARD 17)
include(GNUInstallDirs)
install(
    TARGETS hackrf
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES host/libhackrf/src/hackrf.h DESTINATION include/libhackrf)
