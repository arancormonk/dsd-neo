<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DSD-neo on Android

An arm64-v8a app: a Qt Quick UI (shared with the future desktop GUI, `src/ui/qt/`),
a foreground service that owns the decoder, and a small JNI surface between them.
The engine is linked into the app process as native code — there is no data path
across JNI, only lifecycle and platform glue.

Supported inputs: a directly attached RTL-SDR over USB-OTG, `rtl_tcp`, UDP PCM,
TCP PCM, and local files.

## Build

Prerequisites:

- Android SDK with build-tools and platform 36
- The NDK the chosen Qt release is qualified with (Qt 6.9–6.11: r27+; r28 is what
  this tree is built and tested with)
- Qt 6.11 `android_arm64_v8a` kit **and** the matching host kit (`gcc_64` on Linux)
- vcpkg (the repo's manifest and overlay ports cross-compile mbe-neo, OpenSSL and
  libsndfile)

```sh
export VCPKG_ROOT=$HOME/vcpkg
export ANDROID_SDK_ROOT=/opt/android-sdk
export ANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/28.2.13676358
export QT_ANDROID_ROOT=$HOME/Qt/6.11.1/android_arm64_v8a
export QT_HOST_ROOT=$HOME/Qt/6.11.1/gcc_64

cmake --preset android-app
cmake --build --preset android-app -j          # builds the apk target
adb install -r build/android-app/android/android-build/build/outputs/apk/**/*.apk
```

CMake is the outer build: vcpkg chainloads Qt's `qt.toolchain.cmake`, which
chainloads the NDK toolchain, and androiddeployqt generates and drives the Gradle
project from `android/package/`.

For a headless CLI binary (no UI, no APK) use the `android-arm64-release` preset;
it needs only `ANDROID_NDK_HOME` and `VCPKG_ROOT`.

The NDK, SDK platform, build-tools and Qt versions CI builds against are pinned in
`tools/ci-dependency-pins.env` (`ANDROID_NDK_VERSION`, `ANDROID_COMPILE_SDK`,
`ANDROID_BUILD_TOOLS_VERSION`, `ANDROID_QT_VERSION`). Newer ones generally work;
those are the combination the tree is known good with.

## Continuous integration

Three jobs keep this path from rotting, none of which needs a device. They run on
pull requests as well as pushes to `main`:

- **`android-ci` / arm64 CLI (NDK cross build)** — cross-compiles the headless
  `android-arm64-release` preset: engine, AAudio backend, and the vendored libusb
  and librtlsdr below. Asserts the binary is AArch64 with 16 KB page alignment.
- **`android-ci` / APK (Qt Quick app)** — builds the `android-app` preset through
  androiddeployqt, then unpacks the APK and asserts the same alignment for every
  packaged `.so` (ours and Qt's) and that `arm64-v8a` is the only ABI inside. The
  APK is uploaded as a build artifact.
- **`linux-ci` / android shape (headless, forced radio pipeline)** — the same
  option set on the host without an NDK, and deliberately without PulseAudio or
  ncurses installed. This is the only place the Android configuration gets test
  coverage: `--iq-replay` needs the radio pipeline, so the `DECODE_IQ_*` cases
  only register when it is forced on.

`tools/check_android_elf_alignment.sh` is the alignment check and runs locally
against any ELF (it finds `llvm-readelf` in `$ANDROID_NDK_HOME`):

```sh
tools/check_android_elf_alignment.sh build/android-arm64-release/apps/dsd-cli/dsd-neo
```

What CI does not cover is everything that needs hardware — decoding, audio, the
USB descriptor path, and battery/thermal behavior are verified by hand on a
device.

## USB-OTG: how the descriptor gets to librtlsdr

An Android application cannot open `/dev/bus/usb` nodes, so librtlsdr cannot
enumerate or open a dongle the way it does everywhere else. The descriptor is
obtained in Java and injected:

1. `UsbSourceManager.kt` matches an attached device against the RTL2832U
   vendor/product table, requests permission if it does not already have it, calls
   `UsbManager.openDevice()` and takes `UsbDeviceConnection.getFileDescriptor()`.
2. That descriptor goes to `DsdNative.nativeSetUsbFd(fd)` →
   `rtl_device_set_preopened_fd()` (`src/io/radio/rtl_device.cpp`).
3. `rtl_device_create()` sees the injected descriptor and calls `rtlsdr_open_fd()`
   instead of `rtlsdr_open()`. That entry point — added by the patch below — turns
   libusb device discovery off and wraps the descriptor with
   `libusb_wrap_sys_device()`, then runs the normal claim/probe sequence.
4. Because discovery is off, `rtlsdr_get_device_count()` reports zero. The engine
   would read that as "no supported devices" and abort the run, so
   `dsd_engine_setup_enumerate_rtl_devices()` short-circuits to a single device at
   index 0 whenever a descriptor is set. The app is what selected the device.

Java keeps ownership of the descriptor throughout: `libusb_wrap_sys_device()` does
not take it over, and `DecoderService.onDestroy()` releases the connection only
after the engine thread has unwound. A detach event stops the engine first and
clears the slot afterwards, which also puts enumeration back the way it was.

Attaching a listed dongle launches the app through the manifest's
`USB_DEVICE_ATTACHED` filter, which is the only way Android grants device
permission without a prompt. `res/xml/device_filter.xml` holds the same ids as the
Kotlin table; add rebadged dongles to both.

## Power

An RTL-SDR draws roughly 300 mA, and many phones cap what they will supply over
OTG — some silently, some by dropping the device mid-stream. If the dongle
enumerates but the run dies or never starts, try a **powered OTG hub** before
suspecting the software. A powered hub is the recommended setup for anything
longer than a quick test; it also keeps the phone from discharging into the
dongle. Bias-tee power for an external LNA comes out of the same budget — the UI
exposes it as a checkbox on the USB source, and it is off by default.

## Vendored third-party code

`third_party/` carries trimmed snapshots of two upstream projects. They are **not**
under `src/third_party/`: that perimeter has repo-wide guardrails, and these build
privately for the Android app only.

| Project | Version | Upstream commit | What is vendored |
| --- | --- | --- | --- |
| libusb | v1.0.30 | `87a55632db62c9bdc58cd31d3ccfa673f1bb017f` | The exact source list upstream's own `android/jni/libusb.mk` builds — the `linux_usbfs` backend plus the POSIX event/thread shims — with upstream's `android/config.h` |
| librtlsdr | v2.0.2 (osmocom) | `619ac3186ea0ffc092615e1f59f7397e5e6f668c` | The library only: `librtlsdr.c`, the five tuner drivers and `include/`; the `rtl_*` command-line tools and their helpers are dropped |

librtlsdr is GPL-2.0-or-later and libusb is LGPL-2.1-or-later; both upstream
license texts travel with the snapshots (`*/COPYING`) and both are listed in the
repository's `THIRD_PARTY.md`.

These trees keep upstream formatting and are excluded from every repo tool
(`.clang-format-ignore`, `tools/format.sh`, `.githooks/pre-push`), so do not
reformat or "fix" them — the point is that they stay diffable against the release
they came from.

### The librtlsdr patch

`third_party/patches/0001-librtlsdr-add-rtlsdr_open_fd.patch` is already applied to
the vendored copy; it is kept as a file so the delta stays reviewable and can be
re-applied when the snapshot is refreshed. It adds `rtlsdr_open_fd()` and splits
the claim/probe tail of `rtlsdr_open()` into a shared helper so both open paths run
identical initialisation.

To move to a newer librtlsdr:

```sh
git clone https://gitea.osmocom.org/sdr/rtl-sdr.git /tmp/rtlsdr
git -C /tmp/rtlsdr checkout <new-tag>
cd /tmp/rtlsdr && patch -p1 < <repo>/android/third_party/patches/0001-librtlsdr-add-rtlsdr_open_fd.patch
# then copy src/librtlsdr.c, src/tuner_*.c and include/* over the vendored tree,
# regenerate the patch against the new baseline, and update the table above
```

## Known limits

- **arm64-v8a only.** armv7 would run scalar-only DSP and is out of scope.
- **One decoder per process.** The service rejects a start unless it is idle.
- **SoapySDR is not supported** and is not planned.
- **No mic input in the UI.** The AAudio backend implements capture, but
  `RECORD_AUDIO` is deliberately not requested.
- The launcher label reads `dsd-neo-app`: androiddeployqt uses the CMake target
  name rather than `strings.xml`.
