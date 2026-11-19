# Release Asset License Checklist

Required license/notice files in every shipped asset:

- `LICENSE`
- `COPYRIGHT`
- `THIRD_PARTY.md`
- `legal/` (component inventory, current findings, review history and source-delivery status)
- `licenses/ISC.txt`, `licenses/Unlicense.txt`, `licenses/gqrx-scanner-MIT.txt`,
  `licenses/BSL-1.0.txt` and `licenses/SoapySDR-COPYRIGHT.txt` (SoapySDR), `licenses/LicenseRef-Simon-Rockliff.txt` and
  `licenses/LicenseRef-BCH3-Noncommercial.txt`
- `licenses/mbelib-neo/` (the complete notice directory from the selected mbelib
  installation, including ISC/PFFFT/ETSI texts and `legal/status.md`/`patents.md`)
- `licenses/ezpwd-LGPL-2.1-or-later.txt` (from `src/third_party/ezpwd/lesser.txt`)
- `licenses/pffft-LICENSE.txt` and `licenses/pffft-NOTICES.txt` (from the configured build tree's `pffft-licenses/`, including the pinned revision and backend copyright notices)
- `licenses/tetra_crypto-LICENSE.txt` (the Apache-2.0 text of the pinned TETRA_crypto revision, from the configured build tree's `tetra_crypto-licenses/`)

Assets that bundle the Qt Quick frontend (`DSD_ENABLE_QT_UI=ON`, today the APK)
additionally require `licenses/ibm-plex-OFL-1.1.txt` (from
`src/ui/qt/fonts/IBMPlex-LICENSE.txt`): the fonts are compiled into the frontend's
resource bundle, so they travel with the binary rather than as a separate file.

## TODOs per uploaded asset

### Linux (AppImage)

- [ ] Download `dsd-neo-linux-x86_64-portable-*.AppImage`.
  - [ ] Extract with `./dsd-neo-linux-x86_64-portable-*.AppImage --appimage-extract`.
  - [ ] Confirm `squashfs-root/usr/share/doc/dsd-neo/` contains all required files, plus
        `licenses/codec2-LGPL-2.1.txt`, `licenses/codec2-NOTICES.txt` and `licenses/LicenseRef-codec2-varicode.txt` for
        the codec2 library the AppImage bundles, and `licenses/librtlsdr-GPL-2.0-or-later.txt`,
        `licenses/librtlsdr-PATCHES.txt` and `licenses/librtlsdr-NOTICES.txt` for its librtlsdr (the AppImage job fails
        the build if any is missing).
- [ ] Download `dsd-neo-linux-aarch64-portable-*.AppImage`.
  - [ ] Extract with `--appimage-extract` on aarch64 or in a container.
  - [ ] Confirm `squashfs-root/usr/share/doc/dsd-neo/` contains all required files and the two codec2 notices.

### macOS (DMG)

- [ ] Download `dsd-neo-macos-arm64-portable-*.dmg`.
  - [ ] Mount the DMG.
  - [ ] Confirm `dsd-neo-macos/share/doc/dsd-neo/` contains all required files, plus every notice the records in
        `legal/components.json` name under `macos_release` (`python3 tools/legal_inventory.py --release-notices
        macos` lists them): among them the codec2 and librtlsdr notices, `licenses/<port>-copyright.txt` for each
        library vcpkg builds, `licenses/libusb-macos-NOTICES.txt`, `licenses/portaudio-macos-NOTICES.txt`,
        `licenses/curl-macos-NOTICES.txt`, `licenses/X11-distribute-modifications-variant.txt` and
        `licenses/LicenseRef-ncurses-terminfo.txt`. The DMG job stages them from those maps
        (`cmake/stage_release_notices.cmake`) and refuses a staged tree or a mounted DMG without one, or with one
        that differs from its source (`tools/check_release_notices.py`).
  - [ ] Confirm every dylib in `dsd-neo-macos/lib/` comes from vcpkg or is the patched librtlsdr or libairspy (no
        PulseAudio, libintl or other Homebrew library), and that `share/terminfo/` holds the bundled database.

### Windows (ZIP)

- [ ] Download `dsd-neo-msvc-x86_64-native-*.zip`.
  - [ ] Unzip and confirm `share/doc/dsd-neo/` contains all required files, plus the notices of the SoapySDR load
        path: `licenses/SoapyAirspy-MIT.txt`, `licenses/SoapyHackRF-MIT.txt`, `licenses/libhackrf-BSD-3-Clause.txt`,
        `licenses/libusb-LGPL-2.1-or-later.txt`, `licenses/libusb-COPYRIGHT.txt`, `licenses/PThreads4W-Apache-2.0.txt`,
        `licenses/PThreads4W-NOTICE.txt` and `licenses/PThreads4W-CONTRIBUTORS.txt` (the release job refuses a staged
        tree or an unpacked ZIP without any of them).
  - [ ] Confirm every file in `bin/` and `lib/` (except `bin/dlls-manifest.txt`) is one a record in
        `legal/components.json` lists under `windows_release`, and that `share/doc/dsd-neo/` holds every notice
        those records name (for the libraries vcpkg builds, `licenses/<port>-copyright.txt`, the copyright file the
        port installs, and the notices of their sources listed below); `tools/windows/verify_release_tree.ps1` refuses
        a file without a record or a missing notice.
  - [ ] Confirm `share/doc/dsd-neo/licenses/` holds the notices of the sources the vcpkg-built libraries are compiled
        from that their ports' copyright files lack: `codec2-LGPL-2.1.txt`, `codec2-NOTICES.txt`,
        `LicenseRef-codec2-varicode.txt`, `librtlsdr-NOTICES.txt`,
        `portaudio-NOTICES.txt`, `LicenseRef-PDCurses-Public-Domain.txt`, `OpenSSL-AUTHORS.md`, `OpenSSL-NOTICES.txt`,
        `LicenseRef-OpenSSL-KISA-SEED.txt`, `curl-NOTICES.txt`, `expat-NOTICES.txt`,
        `libsndfile-GSM610-COPYRIGHT.txt`, `libsndfile-ALAC-Apache-2.0.txt`, `LicenseRef-Sun-G72x.txt`,
        `libsndfile-NOTICES.txt`, `libvorbis-lpc-NOTICE.txt`, `opus-NOTICES.txt`, `mp3lame-LGPL-2.1.txt`,
        `mp3lame-README.txt`, `mp3lame-LICENSE.txt`, `mp3lame-NOTICES.txt`, `mpg123-AUTHORS.txt` and
        `mpg123-NOTICES.txt`. When a vcpkg baseline or overlay pin moves, review these against the new sources
        (`legal/components.json` names where each comes from).
  - [ ] Confirm `lib/SoapySDR/modules0.8/` holds exactly `airspySupport.dll` and `HackRFSupport.dll`, and that `bin/`
        holds `hackrf.dll` and `SoapySDRUtil.exe` beside `SoapySDR.dll`, `airspy.dll`, `libusb-1.0.dll` and
        `pthreadVC3.dll`.
  - [ ] Confirm the only Visual C++ runtime DLLs in `bin/` (`vcruntime140*.dll`, `msvcp140*.dll` and the like) are
        ones the `msvc-runtime` record in `legal/components.json` lists, and that none is a debug build (`*d.dll`,
        `ucrtbased.dll`); `tools/windows/verify_release_tree.ps1` refuses any other.
  - [ ] When `bin/` holds any of those runtime DLLs, confirm `share/doc/dsd-neo/` holds
        `licenses/LicenseRef-Microsoft-Visual-CPP-Runtime.txt`, the end-user notice of Microsoft's terms for them, and
        `legal/msvc-runtime.md` (the release job refuses a tree without either). Before tagging, check that the Distributable Code section and the REDIST list it
        quotes are unchanged at <https://aka.ms/VisualStudioLicenses>.
  - [ ] On a Windows machine, if one is at hand, `bin\SoapySDRUtil.exe --info` from a fresh unzip lists the two
        modules and `Available factories... airspy, hackrf` (`tools/windows/verify_release_tree.ps1` checks this in
        CI).

### Android (APK)

- [ ] Download `dsd-neo-android-arm64-app-*.apk`.
  - [ ] `apksigner verify --print-certs` reports the project release key.
  - [ ] `unzip -l` shows all required files under `assets/doc/dsd-neo/`, plus
        `licenses/libusb-LGPL-2.1-or-later.txt`, `licenses/libusb-android-NOTICES.txt`,
        `licenses/librtlsdr-GPL-2.0-or-later.txt` and `licenses/librtlsdr-NOTICES.txt` for the vendored Android
        libraries, `licenses/codec2-LGPL-2.1.txt`,
        `licenses/codec2-NOTICES.txt` and
        `licenses/LicenseRef-codec2-varicode.txt` for the codec2 library it links, and
        `licenses/ibm-plex-OFL-1.1.txt` for the fonts compiled into the Qt
        resource bundle, and for the vcpkg libraries linked into the app the
        same notices the Windows ZIP carries for them: `licenses/<port>-copyright.txt`
        for `openssl`, `curl`, `zlib`, `expat`, `libsndfile`, `libflac`, `libogg`,
        `libvorbis`, `opus`, `mp3lame` and `mpg123`, and `OpenSSL-AUTHORS.md`,
        `OpenSSL-NOTICES.txt`, `LicenseRef-OpenSSL-KISA-SEED.txt`, `curl-NOTICES.txt`,
        `expat-NOTICES.txt`, `libsndfile-GSM610-COPYRIGHT.txt`,
        `libsndfile-ALAC-Apache-2.0.txt`, `LicenseRef-Sun-G72x.txt`,
        `libsndfile-NOTICES.txt`, `libvorbis-lpc-NOTICE.txt`, `opus-NOTICES.txt`,
        `mp3lame-LGPL-2.1.txt`, `mp3lame-README.txt`, `mp3lame-LICENSE.txt`,
        `mp3lame-NOTICES.txt`, `mpg123-AUTHORS.txt` and `mpg123-NOTICES.txt`; for
        Qt, the NDK C++ runtime and the AndroidX and Kotlin libraries,
        `LGPL-3.0-only.txt`, `Qt-NOTICES.txt`, `Apache-2.0.txt`, `MPL-2.0.txt`,
        `Unicode-3.0.txt`, `LicenseRef-SHA1-Public-Domain.txt`,
        `LicenseRef-ICC-License.txt`, `LLVM-exception.txt`,
        `NDK-runtime-NOTICES.txt`, `AndroidX-NOTICES.txt` and `Kotlin-NOTICES.txt`
        (each record's `android_release` map in `legal/components.json` names the
        notices the APK carries for it and where each comes from;
        `cmake/stage_android_package.cmake` stages them through
        `cmake/stage_release_notices.cmake`, and `android-ci` fails the build if
        any notice `tools/legal_inventory.py --release-notices android` lists is
        missing).
  - [ ] `aapt2 dump badging` reports `application-label:'DSD-neo'`, a non-empty
        `application-icon-*`, `versionName` equal to the tag without its `v`, and
        `versionCode` equal to `(major * 10000 + minor * 100 + patch) * 1000` —
        a tagged release is always a round thousand, since its commit distance is
        zero (`android-ci` asserts the shape, so this is a spot check that the
        published asset is the one CI built from the tag and not from `main`).

### Android (AAB — workflow artifact, not a release asset)

- [ ] Download the `dsd-neo-android-arm64-app-<tag>.aab` artifact from the tag's
      `android-ci` run within its 90-day retention window (re-running the
      workflow after expiry rebuilds against the current toolchain; it does not
      restore the same bytes).
  - [ ] `jarsigner -verify -verbose:summary` prints `jar verified.` with the
        project release (upload) key.
  - [ ] `gh attestation verify <file>.aab --repo arancormonk/dsd-neo` passes
        before the bundle goes to the Play Console.
  - [ ] The bundle came from a release tag, never from a nightly: a nightly
        built between the version bump and the tag carries a `versionCode`
        above the release's, and uploading it to any track would burn that code
        permanently (see `android/README.md`, Google Play section).

## CI-side sanity

- [ ] Resolve the applicable open findings in `legal/status.md` before asserting
      legal clearance or complete corresponding-source compliance. A passing
      notice checklist and signed SBOM establish neither.
- [ ] Review the actual bundled runtime closure for additional notices and
      source obligations; this common file list is not an exhaustive dependency audit.

- [ ] Release tags use `vX.Y.Z`, match `project(dsd-neo VERSION X.Y.Z ...)` in `CMakeLists.txt`, and verify with `git tag -v` against one of the trusted keys in `release-keys/` (`arancormonk-desktop-2026.pgp` or `arancormonk-laptop-2026.pgp`).
- [ ] For each workflow run, verify the staging steps did not emit “missing required license file” errors.
- [ ] Spot-check nightly and tag builds across all OSes after any packaging changes.
