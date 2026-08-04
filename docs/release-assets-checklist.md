# Release Asset License Checklist

Required license/notice files in every shipped asset:

- `LICENSE`
- `COPYRIGHT`
- `THIRD_PARTY.md`
- `licenses/ezpwd-LGPL-2.1-or-later.txt` (from `src/third_party/ezpwd/lesser.txt`)
- `licenses/pffft-FFTPACK.txt` (from `src/third_party/pffft/COPYING`)

Assets that bundle the Qt Quick frontend (`DSD_ENABLE_QT_UI=ON`, today the APK)
additionally require `licenses/dejavu-fonts-Bitstream-Vera.txt` (from
`src/ui/qt/fonts/DejaVu-LICENSE.txt`): the font is compiled into the frontend's
resource bundle, so it travels with the binary rather than as a separate file.

## TODOs per uploaded asset

### Linux (AppImage)

- [ ] Download `dsd-neo-linux-x86_64-portable-*.AppImage`.
  - [ ] Extract with `./dsd-neo-linux-x86_64-portable-*.AppImage --appimage-extract`.
  - [ ] Confirm `squashfs-root/usr/share/doc/dsd-neo/` contains all required files.
- [ ] Download `dsd-neo-linux-aarch64-portable-*.AppImage`.
  - [ ] Extract with `--appimage-extract` on aarch64 or in a container.
  - [ ] Confirm `squashfs-root/usr/share/doc/dsd-neo/` contains all required files.

### macOS (DMG)

- [ ] Download `dsd-neo-macos-arm64-portable-*.dmg`.
  - [ ] Mount the DMG.
  - [ ] Confirm `dsd-neo-macos/share/doc/dsd-neo/` contains all required files.

### Windows (ZIP)

- [ ] Download `dsd-neo-msvc-x86_64-native-*.zip`.
  - [ ] Unzip and confirm `share/doc/dsd-neo/` contains all required files.

### Android (APK)

- [ ] Download `dsd-neo-android-arm64-app-*.apk`.
  - [ ] `apksigner verify --print-certs` reports the project release key.
  - [ ] `unzip -l` shows all required files under `assets/doc/dsd-neo/`, plus
        `licenses/libusb-LGPL-2.1-or-later.txt` and
        `licenses/librtlsdr-GPL-2.0-or-later.txt` for the vendored Android
        libraries, and `licenses/dejavu-fonts-Bitstream-Vera.txt` for the font
        compiled into the Qt resource bundle
        (`cmake/stage_android_package.cmake` stages them; `android-ci` fails the
        build if any are missing).
  - [ ] `aapt2 dump badging` reports `application-label:'DSD-neo'`, a non-empty
        `application-icon-*`, `versionName` equal to the tag without its `v`, and
        `versionCode` equal to `(major * 10000 + minor * 100 + patch) * 1000` —
        a tagged release is always a round thousand, since its commit distance is
        zero (`android-ci` asserts the shape, so this is a spot check that the
        published asset is the one CI built from the tag and not from `main`).

## CI-side sanity

- [ ] Release tags use `vX.Y.Z`, match `project(dsd-neo VERSION X.Y.Z ...)` in `CMakeLists.txt`, and verify with `git tag -v` against one of the trusted keys in `release-keys/` (`arancormonk-desktop-2026.pgp` or `arancormonk-laptop-2026.pgp`).
- [ ] For each workflow run, verify the staging steps did not emit “missing required license file” errors.
- [ ] Spot-check nightly and tag builds across all OSes after any packaging changes.
