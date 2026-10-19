# SPDX-License-Identifier: GPL-3.0-or-later
# Minimal project for tests/tools/test_native_arch.py: applies
# cmake/NativeArch.cmake the way the top-level CMakeLists.txt does.
cmake_minimum_required(VERSION 3.20)
project(native_arch_test LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include("${DSD_SOURCE_DIR}/cmake/NativeArch.cmake")
option(DSD_ENABLE_NATIVE "Tune Release-like builds for the build host" ON)
dsd_native_arch_apply("${DSD_ENABLE_NATIVE}")
file(
    WRITE "${CMAKE_CURRENT_BINARY_DIR}/native_arch_result.txt"
    "flags=${DSD_NATIVE_ARCH_FLAGS}\nreason=${DSD_NATIVE_ARCH_SKIP_REASON}\n"
)

file(
    WRITE "${CMAKE_CURRENT_BINARY_DIR}/native_c.c"
    "int native_c_value(void) { return 1; }\n"
)
file(
    WRITE "${CMAKE_CURRENT_BINARY_DIR}/native_cxx.cpp"
    "int native_cxx_value() { return 2; }\n"
)
add_library(native_c OBJECT "${CMAKE_CURRENT_BINARY_DIR}/native_c.c")
add_library(native_cxx OBJECT "${CMAKE_CURRENT_BINARY_DIR}/native_cxx.cpp")
