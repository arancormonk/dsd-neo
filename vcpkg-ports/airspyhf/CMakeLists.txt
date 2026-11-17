# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(airspyhf C)
find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 REQUIRED)
find_package(PThreads4W REQUIRED)
# airspyhf.h declares the API __declspec(dllexport) on Windows by itself.
add_library(airspyhf libairspyhf/src/airspyhf.c libairspyhf/src/iqbalancer.c)
target_include_directories(airspyhf PRIVATE "${LIBUSB_INCLUDE_DIR}")
target_link_libraries(
    airspyhf
    PRIVATE "${LIBUSB_LIBRARY}" PThreads4W::PThreads4W
)
set_target_properties(airspyhf PROPERTIES C_STANDARD 17)
include(GNUInstallDirs)
install(
    TARGETS airspyhf
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(
    FILES
        libairspyhf/src/airspyhf.h
        libairspyhf/src/airspyhf_commands.h
        libairspyhf/src/iqbalancer.h
    DESTINATION include/libairspyhf
)
