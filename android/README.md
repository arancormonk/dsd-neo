<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# DSD-neo on Android

An arm64-v8a app: a Qt Quick UI (shared with the future desktop GUI, `src/ui/qt/`),
a foreground service that owns the decoder, and a small JNI surface between them.
The engine is linked into the app process as native code — there is no data path
across JNI, only lifecycle and platform glue.

Supported inputs: a directly attached RTL-SDR over USB-OTG, `rtl_tcp`, UDP PCM,
TCP PCM, and local files.

## The two-mode UI

One screen with two jobs, switched on `DecoderHost::sessionState` — a phone cannot
do both at once:

- **Setup** (`Idle`, `Failed`) — input and decode options, the primary action, a
  failure banner when a start was abandoned, and a row that reopens the last
  session's event log.
- **Monitor** (`Starting`, `Running`, `Stopping`) — the settings collapse to a
  summary chip, and status and events take the screen.

Both panes stay instantiated and cross-fade, so the setup form keeps whatever was
typed into it. Status and events exist *only* in the monitoring view: nothing
upstream invalidates the published snapshot on stop, so `MetricsModel::clear()`
and `EventLogModel::clear()` are what stop the last live SNR and carrier lock from
sitting on screen for a decoder that is no longer running. `UiController` drives
both from the session-state edges — events are cleared when a new session starts,
not when one ends, so the finished session's log stays reachable.

The service state machine, the native `g_running` atomic and the failure path are
folded into that one phase by `session_state_map.h`, which is deliberately free of
Qt and JNI so `UI_QT_SESSION_STATE` can test it on the host.

## Build

Prerequisites:

- Android SDK with build-tools and platform 36
- The NDK the chosen Qt release is qualified with (Qt 6.9–6.11: r27+; r28 is what
  this tree is built and tested with)
- Qt 6.11 `android_arm64_v8a` kit **and** the matching host kit (`gcc_64` on Linux)
- vcpkg (the repo's manifest and overlay ports cross-compile mbe-neo, OpenSSL,
  libsndfile, Codec2 and libcurl)
- a host C compiler: Codec2 builds `generate_codebook` for the build machine and
  runs it to generate its codebooks, so a container without one cannot build it

```sh
export VCPKG_ROOT=$HOME/vcpkg
export ANDROID_SDK_ROOT=/opt/android-sdk
export ANDROID_NDK_ROOT=$ANDROID_SDK_ROOT/ndk/28.2.13676358
export QT_ANDROID_ROOT=$HOME/Qt/6.11.1/android_arm64_v8a
export QT_HOST_ROOT=$HOME/Qt/6.11.1/gcc_64

cmake --preset android-app
cmake --build --preset android-app -j          # builds the apk target
```

The APK lands at `build/android-app/android/android-build/dsd-neo-app.apk` and is
unsigned; see [Release signing](#release-signing) for signing and installing it.

CMake is the outer build: vcpkg chainloads Qt's `qt.toolchain.cmake`, which
chainloads the NDK toolchain, and androiddeployqt generates and drives the Gradle
project from `android/package/`.

androiddeployqt is pointed at a staged copy of that directory
(`build/android-app/android/package`) rather than the source tree, because it
packages nothing else and the APK has to carry the same license/notice set as
every other release asset. `cmake/stage_android_package.cmake` copies
`android/package/` plus `LICENSE`, `COPYRIGHT`, `THIRD_PARTY.md` and the vendored
notices into `assets/doc/dsd-neo/`, and runs on every build, so edits under
`android/package/` reach the APK without re-running CMake. Add new package files
to `android/package/` as usual — never to the staged copy, which is overwritten.

For a headless CLI binary (no UI, no APK) use the `android-arm64-release` preset;
it needs only `ANDROID_NDK_HOME` and `VCPKG_ROOT`.

### App identity

The launcher shows **DSD-neo**, from `@string/app_name` in
`android/package/res/values/strings.xml`. That indirection only works because
`android/CMakeLists.txt` sets `QT_ANDROID_APP_NAME` and `QT_ANDROID_APP_ICON`:
without them androiddeployqt substitutes the CMake target name into
`android:label` and deletes `android:icon` outright, so the app installs as
`dsd-neo-app` with the stock Android robot.

The version surfaces both come from `GIT_TAG`, the same `git describe` string the
CLI banner and terminal UI header print — the Qt UI shows no version of its own,
so `android:versionName` is all the app has:

| build | `versionName` | `versionCode` |
| --- | --- | --- |
| tag `v2.5.1` | `2.5.1` | `20501000` |
| nightly, 6 commits later | `2.5.1-6-gfa336d4` | `20501006` |
| next tag `v2.5.2` | `2.5.2` | `20502000` |

`PROJECT_VERSION` only moves at release time and the tag sits on the bump commit
itself, so deriving the code from it alone would give every nightly in a release
window the same code as the release it follows — installable, but invisible to
anything that compares version codes to detect an update. Scaling the release
code up and adding `git describe`'s commit distance keeps tagged builds round,
lets nightlies increment, and has the next release clear every nightly before it.

Minor and patch are assumed to stay below 100 and a release window below 1000
commits; configure fails loudly if the latter is ever exceeded. The scheme tops
out at major version 210, against the platform's 2100000000 ceiling. A tarball
with no git metadata falls back to the plain release version.

The launcher, themed, notification and splash bitmaps under
`android/package/res/mipmap-*` and `res/drawable-*` are generated from
`images/dsd-neo.png` and committed — the Android CI job has no ImageMagick, and
androiddeployqt packages `res/` as-is. Regenerate them only when the logo
changes:

```sh
tools/gen_android_icons.sh
```

The hand-written pieces live alongside them: `mipmap-anydpi-v26/ic_launcher.xml`
(the adaptive icon, including the Android 13 `<monochrome>` layer),
`values/colors.xml` (the one near-black backdrop the transparent logo is drawn
on), `drawable/splash.xml` and `values{,-v31}/themes.xml`.

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
  androiddeployqt, signs the APK, then unpacks it and asserts the same alignment
  for every packaged `.so` (ours and Qt's), that `arm64-v8a` is the only ABI
  inside, that the license files are present, and that the launcher identity is
  intact (label `DSD-neo`, a declared icon, a non-placeholder version code and
  the icon/splash/theme resources). The APK is uploaded as a build
  artifact, and on `main` and release tags the `Publish APK` job attaches it to a
  release (see [Release signing](#release-signing)).
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

## Release signing

androiddeployqt emits a release APK that Gradle leaves **unsigned**
(`build/outputs/apk/release/android-build-release-unsigned.apk`, copied alongside
it as `dsd-neo-app.apk`), and an unsigned APK cannot be installed. Sign it with
the release keystore before pushing it to a device:

```sh
"$ANDROID_SDK_ROOT/build-tools/$ANDROID_BUILD_TOOLS_VERSION/apksigner" sign \
  --ks /path/to/dsd-neo-release.jks --ks-key-alias dsd-neo \
  --out /tmp/dsd-neo-app.apk \
  build/android-app/android/android-build/dsd-neo-app.apk
adb install -r /tmp/dsd-neo-app.apk
```

`apksigner` prompts for the keystore password when no `--ks-pass` is given, which
keeps it out of shell history; `--ks-pass env:VAR` works for scripted signing.
Prefer `env:` over `file:` — `file:` sources are read sequentially, so pointing
both `--ks-pass` and `--key-pass` at one file fails with "end of file reached"
unless the password is repeated on a second line.
Use the release key rather than `~/.android/debug.keystore` even locally, so a
device tracking local builds keeps upgrading in place instead of hitting a
signature mismatch against published APKs. Switching an already-installed
debug-signed build over is a one-time
`adb uninstall io.github.arancormonk.dsdneo` first — Android refuses an in-place
update across a signing identity change.

CI signs the same way with the project release key and publishes the result as
`dsd-neo-android-arm64-app-<version>.apk` (`-nightly` off `main`). It needs four
repository secrets:

| Secret | Value |
| --- | --- |
| `ANDROID_KEYSTORE_BASE64` | `base64 -w0 release.jks` of the release keystore |
| `ANDROID_KEYSTORE_PASSWORD` | keystore password |
| `ANDROID_KEY_ALIAS` | key alias inside the keystore |
| `ANDROID_KEY_PASSWORD` | key password |

Generate the keystore once, outside the working tree so no `git add` can reach
it, and back it up somewhere durable before uploading anything. Android has no
key rotation: losing the keystore or its password means every installed copy has
to be uninstalled before an upgrade will apply, and there is no recovery path.

```sh
keytool -genkeypair -v -keystore dsd-neo-release.jks -alias dsd-neo \
  -keyalg RSA -keysize 4096 -validity 10000
chmod 600 dsd-neo-release.jks
base64 -w0 dsd-neo-release.jks | gh secret set ANDROID_KEYSTORE_BASE64
```

Piping into `gh secret set` keeps the encoded key out of shell history and off
disk; the three remaining secrets read from stdin the same way (`gh secret set
ANDROID_KEYSTORE_PASSWORD`, …) rather than taking `--body`. `keytool` accepts an
empty key password to reuse the store password, in which case
`ANDROID_KEY_PASSWORD` is that same value — `apksigner` still wants both.

Without the secrets the workflow still builds and uploads the APK as a build
artifact, but it stays unsigned, logs a warning, and the publish job is skipped;
on a release tag the missing keystore is a hard failure instead, because a
release must not ship an APK nobody can install. CI deliberately does not re-run
`zipalign`: the Gradle output is already 16 KB page aligned and `apksigner`
preserves that, so it only verifies the alignment (`zipalign -c -P 16 4`) after
signing.

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
not take it over. Closing the `UsbDeviceConnection` while the engine still has the
descriptor wrapped is a use-after-close, so `UsbSourceManager.release()` clears the
native slot immediately — that only affects the *next* open — and then hands the
close to a background thread that waits for `nativeIsRunning()` to go false. Both
callers (a detach broadcast, `DecoderService.onDestroy()`) therefore return without
blocking. If the engine never stops, the connection is deliberately leaked: one
descriptor held for the rest of the process's life beats pulling it out from under
an in-flight USB transfer.

Note that discovery does not come back. `rtlsdr_open_fd()` sets libusb's
process-global `LIBUSB_OPTION_NO_DEVICE_DISCOVERY`, which has no counterpart —
and costs nothing here, since an app cannot enumerate `/dev/bus/usb` anyway.

`rtlsdr_open_fd()` exists only in the vendored tree, so configuring an Android
build with `DSD_ENABLE_RTLSDR=ON` and `DSD_ANDROID_VENDORED_RTLSDR=OFF` fails at
configure time rather than at link time or on the device.

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

## Codec2 and libcurl

Both come from vcpkg and both are required by the Android presets
(`DSD_REQUIRE_CODEC2`, `DSD_REQUIRE_CURL`), so a detection regression fails
configure rather than producing an APK that quietly decodes no M17 voice. The
triplet links statically, so they end up inside
`libdsd-neo-app_arm64-v8a.so` — there is no extra `.so` to package and no
`System.loadLibrary` change.

libcurl backs the rdio-scanner upload path. `android.permission.INTERNET` is
already declared, and because OpenSSL's compiled-in `/etc/ssl/certs` does not
exist on Android, `rdio_export.c` points libcurl at the system trust store
(`/apex/com.android.conscrypt/cacerts`, falling back to
`/system/etc/security/cacerts`) so `https://` endpoints verify normally.

## Known limits

- **arm64-v8a only.** armv7 would run scalar-only DSP and is out of scope.
- **One decoder per process.** The service rejects a start unless it is idle.
- **SoapySDR is not supported** and is not planned.
- **No mic input in the UI.** The AAudio backend implements capture, but
  `RECORD_AUDIO` is deliberately not requested.
- **An open output stream is never idle.** Once a chunk of real audio has played,
  `conceal_has_good` stays set for the life of the stream, so the pump keeps waking
  on its 20 ms cadence and topping the device up with concealment whenever the ring
  is empty and the device queue falls below `DSD_AAUDIO_CONCEAL_LOW_WATER_MS`. That
  is what stops a decoder gap from becoming an audible stutter, and the writes are
  paced by the blocking `AAudioStream_write`, but it does mean the pump plus the
  AAudio mixer stay resident for as long as a run lasts rather than only while a
  call is on air. This is the first thing to measure in the battery soak below.
