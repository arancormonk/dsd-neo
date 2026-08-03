# Build and Installation Policy

DSD-neo uses CMake as its primary build and installation system.

## Standard Build Variables

CMake honors standard compiler and linker selection mechanisms. Users may pass
toolchain choices through environment variables or CMake cache variables,
including:

- `CC`
- `CXX`
- `CFLAGS`
- `CXXFLAGS`
- `LDFLAGS`
- `CMAKE_C_COMPILER`
- `CMAKE_CXX_COMPILER`
- `CMAKE_C_FLAGS`
- `CMAKE_CXX_FLAGS`
- `CMAKE_EXE_LINKER_FLAGS`
- `CMAKE_SHARED_LINKER_FLAGS`

The project may add warning, sanitizer, SIMD, LTO, or platform flags based on
explicit CMake options, but should not discard user-supplied compiler or linker
settings.

## Debug Information

The install rules do not strip binaries. If users request debug information
through their compiler flags or build type, the build and installation process
should preserve it.

## Non-Recursive Build Structure

The project uses one top-level CMake build graph with explicit source lists. It
does not rely on recursive make invocations to build cross-dependent
subdirectories.

## Repeatable Builds

To repeat a build from the same source tree, toolchain, options, and
environment:

```sh
rm -rf build/repeatable
cmake -S . -B build/repeatable -DCMAKE_BUILD_TYPE=Release
cmake --build build/repeatable -j
ctest --test-dir build/repeatable --output-on-failure
```

For release packages, use the tagged release workflows or reproduce their
documented steps from `.github/workflows/`.

Known limits:

- Floating-point optimization, SIMD choices, SDR driver versions, and system
  audio stacks can affect generated code and output across architectures.
- Binary identity across different compilers, operating systems, CMake
  generators, or vcpkg baselines is not guaranteed.
- Release artifacts should be compared within the same source, toolchain,
  platform, build options, and packaging environment.

## Android Cross Builds

Android is built as a cross-compile from a Linux (or macOS) host, not installed
through the conventions below: the deliverable is an APK, and the CMake install
rules do not target it.

- `android-arm64-release` — headless arm64-v8a CLI. Needs `ANDROID_NDK_HOME` and
  `VCPKG_ROOT`; vcpkg chainloads the NDK toolchain via the
  `arm64-android-static` overlay triplet.
- `android-app` — the Qt Quick app. Needs a Qt for Android kit and its matching
  host kit; vcpkg chainloads Qt's toolchain, which chainloads the NDK, and
  androiddeployqt drives the Gradle side.

Both presets are release-only, set `BUILD_TESTING=OFF` (the test suite cannot run
on the build host), and select the Android-specific option shape: AAudio audio
backend, no terminal UI, and the RTL-SDR backend satisfied from the vendored
libusb/librtlsdr under `android/third_party`. The same option shape minus the NDK
is built and tested on the host by CI so the configuration keeps test coverage.

They also set `DSD_REQUIRE_CODEC2=ON` and `DSD_REQUIRE_CURL=ON`. Codec2 and
libcurl are otherwise auto-detected and degrade silently — a build that lost
Codec2 still links and simply stops emitting M17 voice — which nothing on the
build host would catch before the APK reached a device. vcpkg supplies both for
Android. The `win-msvc-*` presets set the same two options for the same reason.
`VCPKG_DEPENDENCY_CONTRACT` (a CTest case) pins the manifest filters and the
preset settings that keep it that way.

Toolchain versions CI pins are in `tools/ci-dependency-pins.env`. Build details,
prerequisites, and known limits: `android/README.md`.

## Installation Conventions

Install with CMake:

```sh
cmake --install build/dev-release --prefix /usr/local
```

On Linux, refresh the dynamic linker cache after installing source-built
dependencies such as `mbelib-neo` into `/usr` or `/usr/local`:

```sh
sudo ldconfig
```

For user prefixes such as `$HOME/.local`, set `LD_LIBRARY_PATH` instead; see
`docs/linux-installation.md`.

On POSIX systems, staged packaging can use `DESTDIR`:

```sh
DESTDIR="$PWD/pkgroot" cmake --install build/dev-release --prefix /usr
```

Uninstall from the same build directory:

```sh
cmake --build build/dev-release --target uninstall
```

## Linux Bootstrap Script

Linux source installs can use the distro-aware helper:

```sh
tools/install_linux.sh --yes
```

It installs build dependencies for apt, dnf, zypper, apk, or pacman systems,
builds pinned `mbelib-neo`, builds DSD-neo, smoke-tests the CLI, and installs
through CMake. Docker validation for the supported distro matrix is available
with:

```sh
tools/docker_linux_install_matrix.sh --distro ubuntu-26.04
```

See `docs/linux-installation.md` for options, distro coverage, and derivative
mapping.

## Developer Setup

For local development:

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug -j
ctest --preset dev-debug --output-on-failure
```

Optional local hooks:

```sh
tools/install-git-hooks.sh
```
