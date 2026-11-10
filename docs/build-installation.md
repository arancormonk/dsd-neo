# Build and Installation

DSD-neo uses CMake as its primary build and installation system. Most users do not need to build it: the
[README](../README.md#download) links ready-to-run packages for Linux, macOS, Windows and Android.

## Requirements

- A C11 compiler and a C++14 compiler.
- CMake 3.20 or newer.
- Required libraries: libsndfile; OpenSSL 3.0 or newer (libcrypto); a curses library (ncursesw, or PDCurses on
  Windows) unless you turn the terminal UI off; and an audio library (PulseAudio by default, PortAudio on Windows,
  AAudio on Android, none with `-DDSD_AUDIO_BACKEND=none`).
- [mbelib-neo](https://github.com/arancormonk/mbelib-neo) 2.6 or newer, which provides the voice codecs (the
  `mbe-neo` CMake package). See [Build mbelib-neo](#build-mbelib-neo). The Windows and Android builds get it through
  vcpkg (`vcpkg.json` and the `vcpkg-ports/mbe-neo` overlay), so they need no separate install.
- Qt 6 (Core, Gui, Qml, Quick, QuickControls2, QuickDialogs2), only for the Qt frontend (`-DDSD_ENABLE_QT_UI=ON`).
  The Android build pins the version in `tools/ci-dependency-pins.env`.
- Optional libraries, each detected when it is installed:
  - librtlsdr for RTL-SDR dongles (2.0.3 or newer for the RTL-SDR Blog V4/V4L), plus the libusb-1.0 headers for
    RTL-SDR stall recovery (usually installed with librtlsdr's development package).
  - libairspy for Airspy R2 and Mini receivers ([docs/airspy.md](airspy.md)).
  - SoapySDR 0.8.1 or newer for other SDRs ([docs/soapysdr.md](soapysdr.md)).
  - Codec2 for M17 voice.
  - libcurl 7.56.0 or newer for rdio-scanner API uploads.
  - expat 2.x for RadioReference import.
- PortAudio off Windows, only when you select it with `-DDSD_AUDIO_BACKEND=portaudio`.
- The help2man program, to generate the man page (skipped when cross-compiling).

Package hints:

- Linux: `tools/install_linux.sh --yes` installs your distribution's build dependencies, builds the pinned
  `mbelib-neo`, builds DSD-neo, tests that it runs and installs it. See
  [Linux Bootstrap Script](#linux-bootstrap-script) and `docs/linux-installation.md`.
- Ubuntu/Debian:
  `sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build libssl-dev libsndfile1-dev libpulse-dev libncurses-dev pkg-config librtlsdr-dev libusb-1.0-0-dev libairspy-dev libsoapysdr-dev libcodec2-dev libcurl4-openssl-dev libexpat1-dev`.
  Older releases may ship SoapySDR 0.7.x, which is too old; install or build SoapySDR 0.8.1 or newer if you want that
  backend. Their `librtlsdr-dev` may also be older than 2.0.3, which an RTL-SDR Blog V4 needs.
- macOS (Homebrew): `brew install cmake ninja openssl libsndfile ncurses pulseaudio rtl-sdr airspy soapysdr codec2`.
- Windows: source builds use the `win-msvc-*` presets in `CMakePresets.json` with vcpkg; set `VCPKG_ROOT` first.
- Android: see [Android Cross Builds](#android-cross-builds).

## Build mbelib-neo

If configure stops because it cannot find the `mbe-neo` package, build and install mbelib-neo first. Releases before
2.6 are not supported.

```sh
git clone https://github.com/arancormonk/mbelib-neo
cmake -S mbelib-neo -B mbelib-neo/build -DCMAKE_BUILD_TYPE=Release
cmake --build mbelib-neo/build -j
cmake --install mbelib-neo/build --prefix "$HOME/.local"

# Linux, user-prefix install: let installed binaries find libmbe-neo.so.2.
export LD_LIBRARY_PATH="$HOME/.local/lib:$HOME/.local/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Linux, install to /usr or /usr/local instead: run `sudo ldconfig`.
```

Then point DSD-neo's configure step at that prefix with `-DCMAKE_PREFIX_PATH="$HOME/.local"`.

## Build Recipes

Run these from the repository root. Presets build out of source under `build/<preset>/`, and the program lands in
`build/<preset>/apps/dsd-cli/dsd-neo`. You can run it from there without installing.

Release build (recommended):

```sh
cmake --preset dev-release            # add -DCMAKE_PREFIX_PATH="$HOME/.local" for a user-prefix mbelib-neo
cmake --build --preset dev-release -j
build/dev-release/apps/dsd-cli/dsd-neo -h
```

Debug build and tests:

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug -j
ctest --preset dev-debug --output-on-failure

# Optional race-detection pass, kept separate from ASan/UBSan.
# The preset applies tools/tsan.supp for known PulseAudio runtime reports.
cmake --preset tsan-debug
cmake --build --preset tsan-debug -j
ctest --preset tsan-debug --output-on-failure
```

Without presets:

```sh
cmake -S . -B build/manual -DCMAKE_BUILD_TYPE=Release
cmake --build build/manual -j
build/manual/apps/dsd-cli/dsd-neo -h
```

Coverage: `tools/coverage.sh` writes an HTML report to `build/coverage-debug/coverage_html` (it needs lcov and
genhtml).

`cmake --install <build_dir>` works only on a directory you configured. If `cmake --install build/dev-release` fails
and `build/dev-release/` does not exist, you probably did a manual build: install from that directory instead. See
[Installation Conventions](#installation-conventions).

## Build Options

These are CMake cache options, set at configure time with `-D...`.

Build hygiene and optimization:

- `-DDSD_ENABLE_WARNINGS=ON` — Enable common warnings (default ON).
- `-DDSD_WARNINGS_AS_ERRORS=ON|OFF` — Treat warnings as errors (default ON).
- `-DDSD_ENABLE_FAST_MATH=ON` — Enable fast-math (`-ffast-math`/`/fp:fast`) across targets (default OFF; the
  `dev-release` and `win-msvc-release` presets turn it on).
- `-DDSD_ENABLE_LTO=ON` — Enable IPO/LTO in Release and RelWithDebInfo builds when supported (default OFF; the
  `dev-release` and `win-msvc-release` presets turn it on).
- `-DBUILD_TESTING=OFF` — Skip building the tests (the release presets set it).
- `-DDSD_ENABLE_HARDENING=ON|OFF` — Enable supported Release-like compiler/linker hardening (default ON).
- `-DDSD_ENABLE_NATIVE=ON|OFF` — Tune Release, RelWithDebInfo and MinSizeRel builds for the build machine's CPU
  (default ON): `-march=native` on x86, `-mcpu=native` on ARM, and the highest `/arch:` level (`AVX512`, `AVX2`,
  `AVX`) a configure-time CPU probe finds on MSVC. Debug builds are never tuned. It is skipped, with the reason in the
  configure output, when cross-compiling, for Android/iOS, for a universal or non-host `CMAKE_OSX_ARCHITECTURES`, or
  when your compiler flags already pick a target (`-march=`, `-mcpu=`, `/arch:`). The result runs only on CPUs like
  the build machine: packagers and anyone building binaries for other machines pass `-DDSD_ENABLE_NATIVE=OFF` (the
  release assets do).
- `-DDSD_PFFFT_ENABLE_SIMD=ON|OFF` — Use compiler-target SIMD in the embedded FFT (default ON).
- `-DDSD_ENABLE_ASAN=ON` — AddressSanitizer in Debug builds.
- `-DDSD_ENABLE_UBSAN=ON` — UndefinedBehaviorSanitizer in Debug builds.
- `-DDSD_ENABLE_TSAN=ON` — ThreadSanitizer in Debug builds; use a separate build from ASan/UBSan.
- `-DDSD_ENABLE_FUZZING=ON` — Enable libFuzzer instrumentation and fuzz targets (Clang/libFuzzer builds).

Audio backend:

- `-DDSD_AUDIO_BACKEND=auto|pulse|portaudio|aaudio|none` — Select the audio backend (default `auto`: PortAudio on
  Windows, PulseAudio elsewhere). `none` builds a discard/silence backend that needs no audio library: device-audio
  output becomes a no-op instead of a configure failure. `aaudio` is Android-only; it plays through AAudio, resamples
  when the device refuses the requested rate, and reopens the stream by itself across route changes (headphones,
  Bluetooth).
- `DSD_NEO_AUDIO_STATS=1` in the environment prints per-stream output counters (underruns, drops) when a stream
  closes; the AAudio backend also reports the format the device actually granted, plus `in_frames` (what the decoder
  handed over, against `frames` = what the device accepted), `underruns_partial` (a fragment of speech padded out to a
  full chunk) and `underruns_midspeech` (concealment landing inside a call rather than after one). On a healthy stream
  the last two stay at or near zero; concealment during idle gaps is expected and is just silence.
- `-DDSD_USE_PORTAUDIO=ON` — Deprecated alias for `-DDSD_AUDIO_BACKEND=portaudio`.

Radio backends:

- `-DDSD_ENABLE_RTLSDR=ON|OFF`, `-DDSD_ENABLE_AIRSPY=ON|OFF`, `-DDSD_ENABLE_SOAPYSDR=ON|OFF` — Enable or disable
  detection of each backend (default ON).
- `-DDSD_REQUIRE_RTLSDR=ON`, `-DDSD_REQUIRE_AIRSPY=ON`, `-DDSD_REQUIRE_SOAPYSDR=ON` — Fail configure unless that
  backend is enabled and its library is found (default OFF; SoapySDR must be 0.8.1 or newer, found through a CMake
  package that exports an imported target).
- `-DDSD_FORCE_RADIO_PIPELINE=ON` — Build the radio pipeline even with no SDR library present. Keeps `rtl_tcp` input
  and radio-path I/Q replay working (real USB opens fail cleanly); used by targets that cannot link
  librtlsdr/SoapySDR.
- `-DDSD_ANDROID_VENDORED_RTLSDR=ON` — Android only: build the vendored libusb/librtlsdr under `android/third_party`
  and satisfy the RTL-SDR backend from them (needed for USB-OTG dongles).

Frontends and behavior:

- `-DDSD_ENABLE_TERMINAL_UI=ON|OFF` — Build the ncurses/PDCurses terminal frontend (default ON).
- `-DDSD_ENABLE_QT_UI=ON|OFF` — Build the Qt Quick frontend in `src/ui/qt` (default OFF; requires Qt 6). Used by the
  Android app today and shared with the planned desktop GUI.
- `-DDSD_ENABLE_API=ON|OFF` — Build the TCP JSON control and telemetry API (`--api`, [docs/api.md](api.md); default
  ON, always OFF on Android).
- `-DCOLORS=OFF` — Disable ncurses color output.
- `-DCOLORSLOGS=OFF` — Disable colored terminal/log output.

Protocol knobs:

- `-DPVC=ON` — Enable ProVoice Conventional Frame Sync.
- `-DLZ=ON` — Enable LimaZulu-requested NXDN tweaks.
- `-DSID=ON` — Enable experimental P25p1 Soft ID decoding.

Optional libraries (detected automatically):

- Codec2, libcurl and expat degrade silently when missing: the build still links, but M17 voice, rdio API uploads or
  RadioReference import go missing. `-DDSD_REQUIRE_CODEC2=ON`, `-DDSD_REQUIRE_CURL=ON` and `-DDSD_REQUIRE_EXPAT=ON`
  fail configure instead; `-DCMAKE_DISABLE_FIND_PACKAGE_CODEC2=ON` (and `_CURL`, `_EXPAT`) turns detection off.
- `DSD_RR_APP_KEY` (environment variable or cache value) bakes a RadioReference application key into the build;
  without one the app asks for the user's own. Prefer the environment variable, since `-D` writes the value into
  `CMakeCache.txt`.

## Radio Backend Combinations

CI treats backend availability as a build contract, not a best-effort option. Release and packaging jobs configure
with `-DDSD_REQUIRE_RTLSDR=ON -DDSD_REQUIRE_SOAPYSDR=ON -DDSD_REQUIRE_AIRSPY=ON`, and the static-analysis jobs
(CodeQL, scan-build, warnings-as-errors) require RTL-SDR and SoapySDR, so a missing backend fails configure. The Linux
backend matrix (`docs/testing.md`, "Continuous Integration") builds every combination. To reproduce one locally:

These commands share `build/dev-debug`, and a `DSD_REQUIRE_*` value from an earlier run stays in the cache. Pass
`-DDSD_REQUIRE_<BACKEND>=OFF` for each backend you turn off, or add `--fresh`.

```sh
# both: every backend required
cmake --preset dev-debug \
  -DDSD_ENABLE_RTLSDR=ON -DDSD_REQUIRE_RTLSDR=ON \
  -DDSD_ENABLE_SOAPYSDR=ON -DDSD_REQUIRE_SOAPYSDR=ON \
  -DDSD_ENABLE_AIRSPY=ON -DDSD_REQUIRE_AIRSPY=ON

# soapy_only
cmake --preset dev-debug \
  -DDSD_ENABLE_RTLSDR=OFF -DDSD_ENABLE_AIRSPY=OFF \
  -DDSD_ENABLE_SOAPYSDR=ON -DDSD_REQUIRE_SOAPYSDR=ON

# rtl_only
cmake --preset dev-debug \
  -DDSD_ENABLE_RTLSDR=ON -DDSD_REQUIRE_RTLSDR=ON \
  -DDSD_ENABLE_AIRSPY=OFF -DDSD_ENABLE_SOAPYSDR=OFF

# airspy_only
cmake --preset dev-debug \
  -DDSD_ENABLE_RTLSDR=OFF -DDSD_ENABLE_SOAPYSDR=OFF \
  -DDSD_ENABLE_AIRSPY=ON -DDSD_REQUIRE_AIRSPY=ON

# neither
cmake --preset dev-debug \
  -DDSD_ENABLE_RTLSDR=OFF -DDSD_ENABLE_SOAPYSDR=OFF -DDSD_ENABLE_AIRSPY=OFF
```

Build each with `cmake --build --preset dev-debug -j`. The CI-like strict scan-build run is:

```sh
tools/scan_build.sh --strict \
  --cmake-arg -DDSD_REQUIRE_RTLSDR=ON \
  --cmake-arg -DDSD_REQUIRE_SOAPYSDR=ON
```

## Offline and Packaging Builds

The build downloads two pinned, checksum-verified sources and compiles them into DSD-neo, so neither needs a runtime
package:

- a PFFFT fork (the FFT). Offline builds pass `-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT=/absolute/path/to/pffft`.
- a TETRA_crypto revision (the TEA1/TEA2/TEA3 keystream generators). Offline builds pass
  `-DFETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO=/absolute/path/to/TETRA_crypto`.

A fresh offline build passes both overrides and `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`. `docs/dependencies.md`
("Pinned PFFFT source", "Pinned TETRA_crypto source") has the pin and update procedure, scalar builds and AUR
packaging; the checksums are in `tools/ci-dependency-pins.env`.

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

Release-like builds are tuned for the build host's CPU by default
(`DSD_ENABLE_NATIVE=ON`: `-march=native`, `-mcpu=native`, or the MSVC `/arch:`
level a configure-time CPUID probe finds). Packagers building binaries for other
machines pass `-DDSD_ENABLE_NATIVE=OFF`; the project's release assets do. A
target ISA already present in `CMAKE_C_FLAGS`, `CMAKE_CXX_FLAGS` or a
release-like `CMAKE_<LANG>_FLAGS_<CONFIG>` (`-march=`, `-mcpu=`, `/arch:`, for
example from distribution `CFLAGS` such as makepkg's `-march=x86-64`) turns the
tuning off, as do cross-compiling and Android/iOS targets.

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

They also set `DSD_REQUIRE_CODEC2=ON`, `DSD_REQUIRE_CURL=ON` and
`DSD_REQUIRE_EXPAT=ON`. Codec2, libcurl and expat are otherwise auto-detected
and degrade silently — a build that lost Codec2 still links and simply stops
emitting M17 voice — which nothing on the build host would catch before the APK
reached a device. vcpkg supplies all three for Android. The `win-msvc-*` presets
set the same three options for the same reason.
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

For a manual install to a user prefix such as `$HOME/.local`, set `LD_LIBRARY_PATH` instead (see
[Build mbelib-neo](#build-mbelib-neo)). `tools/install_linux.sh` embeds the library path in the binary, so its installs
need no `LD_LIBRARY_PATH` (`docs/linux-installation.md`).

On POSIX systems, staged packaging can use `DESTDIR`:

```sh
DESTDIR="$PWD/pkgroot" cmake --install build/dev-release --prefix /usr
```

Multi-config generators (Visual Studio, Xcode; the `win-msvc-*` presets use Visual Studio) also need the
configuration:

```sh
cmake --install build/win-msvc-release --config Release --prefix "$HOME/.local"
```

Uninstall from the same build directory you installed from:

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
