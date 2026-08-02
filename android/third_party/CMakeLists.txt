# SPDX-License-Identifier: GPL-3.0-or-later
#
# Vendored USB stack for the Android app: libusb + librtlsdr, built as static
# libraries inside this build tree. Android has neither, and an app cannot open
# /dev/bus/usb nodes anyway — see patches/ for the rtlsdr_open_fd delta that lets
# the Java side hand over an already-open descriptor.
#
# These are upstream sources kept in upstream style. The repo guardrails
# (clang-format, semgrep, dsd_safe_*) do not apply here, and the targets
# deliberately do not link dsd-neo_warnings.

# --- libusb ------------------------------------------------------------------
# The source list is upstream's own Android build (android/jni/libusb.mk): the
# linux_usbfs backend plus the POSIX event/thread shims. config.h is upstream's
# android/config.h verbatim.
add_library(
    dsd-neo_vendor_libusb
    STATIC
    libusb/libusb/core.c
    libusb/libusb/descriptor.c
    libusb/libusb/hotplug.c
    libusb/libusb/io.c
    libusb/libusb/strerror.c
    libusb/libusb/sync.c
    libusb/libusb/os/events_posix.c
    libusb/libusb/os/linux_netlink.c
    libusb/libusb/os/linux_usbfs.c
    libusb/libusb/os/threads_posix.c
)

target_include_directories(
    dsd-neo_vendor_libusb
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libusb"
        "${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb/os"
)
target_include_directories(
    dsd-neo_vendor_libusb
    SYSTEM
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/libusb/libusb"
)

target_compile_options(dsd-neo_vendor_libusb PRIVATE -fvisibility=hidden)
set_target_properties(
    dsd-neo_vendor_libusb
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)

# USE_SYSTEM_LOGGING_FACILITY in the vendored config.h routes libusb's own log
# messages to liblog.
target_link_libraries(dsd-neo_vendor_libusb PUBLIC log)

# --- librtlsdr ---------------------------------------------------------------
# Library sources only; the rtl_* command-line tools and their getopt/convenience
# helpers are not vendored.
add_library(
    dsd-neo_vendor_rtlsdr
    STATIC
    librtlsdr/src/librtlsdr.c
    librtlsdr/src/tuner_e4k.c
    librtlsdr/src/tuner_fc0012.c
    librtlsdr/src/tuner_fc0013.c
    librtlsdr/src/tuner_fc2580.c
    librtlsdr/src/tuner_r82xx.c
)

target_include_directories(
    dsd-neo_vendor_rtlsdr
    SYSTEM
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/librtlsdr/include"
)

# rtlsdr_STATIC empties RTLSDR_API; DETACH_KERNEL_DRIVER stays off deliberately —
# a descriptor from UsbDeviceConnection grants no usbfs detach rights.
target_compile_definitions(dsd-neo_vendor_rtlsdr PUBLIC rtlsdr_STATIC)
target_compile_options(dsd-neo_vendor_rtlsdr PRIVATE -fvisibility=hidden)
set_target_properties(
    dsd-neo_vendor_rtlsdr
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(dsd-neo_vendor_rtlsdr PUBLIC dsd-neo_vendor_libusb)

# --- what the root CMakeLists consumes ---------------------------------------
# Same variable names FindRTLSDR.cmake would define, so nothing downstream has to
# know where the library came from.
set(RTLSDR_FOUND TRUE PARENT_SCOPE)
set(RTLSDR_LIBRARIES dsd-neo_vendor_rtlsdr PARENT_SCOPE)
set(RTLSDR_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/librtlsdr/include"
    PARENT_SCOPE
)
