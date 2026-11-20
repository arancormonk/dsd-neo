# SPDX-License-Identifier: GPL-3.0-or-later
#
# The macOS portable DMG: Apple silicon, shared libraries, release builds only.
#
# Shared, not static: SoapySDR is shared-only (its port refuses a static
# build), and librtlsdr and libairspy, which the DMG job builds itself with
# dsd-neo's patches, load the same libusb as dsd-neo, which needs one shared
# copy. The DMG bundles each library as a dylib, as the Windows ZIP bundles a DLL.
#
# The deployment target is the oldest macOS the DMG runs on. The macos-arm64-release
# preset, and the job's own librtlsdr and libairspy builds, use the same value
# (cmake/vcpkg_dependency_contract.cmake checks the preset), and
# tools/check_macos_portable_deps.sh checks every staged file against it.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "26.0")
set(VCPKG_BUILD_TYPE release)
