# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(airspy C)
find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)
find_library(LIBUSB_LIBRARY NAMES usb-1.0 libusb-1.0 REQUIRED)
find_package(PThreads4W REQUIRED)
add_library(
    airspy
    libairspy/src/airspy.c
    libairspy/src/iqconverter_float.c
    libairspy/src/iqconverter_int16.c
)
target_compile_definitions(airspy PRIVATE ADD_EXPORTS)
target_include_directories(airspy PRIVATE "${LIBUSB_INCLUDE_DIR}")
target_link_libraries(airspy PRIVATE "${LIBUSB_LIBRARY}" PThreads4W::PThreads4W)
set_target_properties(airspy PROPERTIES C_STANDARD 17)
include(GNUInstallDirs)
install(
    TARGETS airspy
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(
    FILES libairspy/src/airspy.h libairspy/src/airspy_commands.h
    DESTINATION include/libairspy
)
