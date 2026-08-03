# Android arm64-v8a, static dependency libraries.
#
# Requires ANDROID_NDK_HOME in the environment; the consuming CMake configure
# chainloads the NDK toolchain via VCPKG_CHAINLOAD_TOOLCHAIN_FILE.
#
# ANDROID_ABI must be passed explicitly: VCPKG_TARGET_ARCHITECTURE does not
# reach the NDK toolchain, which otherwise defaults every port to armeabi-v7a.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 29)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=arm64-v8a)
