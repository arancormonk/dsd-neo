# Dependency Management

DSD-neo keeps runtime, packaging, and analysis dependencies explicit so changes
are reviewable.

## Compiled Dependencies

Required dependencies are:

- C compiler with C11 support
- C++ compiler with C++14 support
- CMake 3.20 or newer
- OpenSSL 3.x libcrypto
- `mbe-neo` 2.3+ CMake package from mbelib-neo
- `tetracrypto` and `tetraetee` CMake packages. On Windows and Android they
  come from the vcpkg overlay ports. Other builds install the same ports with
  `tools/install_tetra_crypto_deps.sh`.
- libsndfile
- curses backend: ncursesw/PDCurses. macOS ships no `libncursesw` — its unified
  `libncurses` carries the wide entry points behind `_XOPEN_SOURCE_EXTENDED` —
  so configure prefers a wide ncurses when the host has one (including
  Homebrew's keg-only `ncurses`, which is on no default search path) and
  otherwise falls back to the SDK's curses, reporting which it took. Nothing in
  the ncurses path calls the wide API, so the fallback is a complete build.
- audio backend: PulseAudio by default on Unix-like systems, PortAudio on
  Windows

Optional compiled dependencies are:

- librtlsdr for RTL-SDR input. RTL-SDR Blog V4 and V4 Lite dongles need
  librtlsdr 2.0.3 or newer (older releases misdetect their R828D/R828S tuners
  as an R820T); the versions this project pins for CI, Windows and Android
  builds are 2.0.3
- SoapySDR 0.8.1 or newer for non-RTL SDR devices; the CMake package must
  export an imported target (`SoapySDR` or `SoapySDR::SoapySDR`)
- Codec2 for additional vocoder paths. On Windows and Android it comes from the
  vcpkg manifest, and those presets set `DSD_REQUIRE_CODEC2=ON` (with
  `DSD_REQUIRE_CURL=ON`) so a missing dependency fails configure instead of
  silently dropping M17 voice or rdio uploads.
- libcurl 7.56.0 or newer for rdio-scanner API uploads. Builds older than
  7.85 use the integer protocol-mask option needed by the Ubuntu 20.04
  AppImage toolchain; remove that branch when portable packaging no longer
  supports libcurl below 7.85.
- expat 2.x for RadioReference trunking-data import. On Windows and Android it
  comes from the vcpkg manifest, and those presets set `DSD_REQUIRE_EXPAT=ON`.
- PortAudio on non-Windows builds when selected
- help2man for generated man pages

Vendored and embedded third-party components include:

- ezpwd Reed-Solomon under `src/third_party/ezpwd/`
- PFFFT from the immutable `arancormonk/pffft` fork revision in
  `tools/ci-dependency-pins.env`, compiled into the internal `dsd-neo_pffft` target
- Tiny AES code in `src/crypto/crypt-aes.c`

Vendored code and embedded upstream-derived snippets retain upstream notices.
License and attribution details are in `THIRD_PARTY.md`.

Registry-managed vcpkg dependencies are pinned by the manifest
`builtin-baseline` (`9e593bb18ea69cc5095e012465dcd675a822ed0d` in the current
manifest). The baseline, overlay ports, and triplets are the source of truth
for exact registry versions; system-package builds enforce the OpenSSL
requirement through `find_package(OpenSSL 3.0 REQUIRED)`.

The `vcpkg-ports/mpg123` overlay follows the upstream vcpkg 1.33.7 recipe and
pins its SourceForge archive by SHA-512. Its local CMake patch includes
`sys/types.h` while probing `off_t`; this is required when configuring with
Visual Studio 2026, where the unqualified probe cannot declare the type.

## Packaging Dependencies

Windows builds use vcpkg overlays under `vcpkg-ports/` and triplets under
`vcpkg-triplets/`. AppImage builds use pinned CI source checkouts for compiled
dependencies that are not suitable from the Ubuntu 20.04 base image, including
SoapySDR 0.8.1 or newer. Linux source-install validation uses pinned Docker
images from `tools/ci-dependency-pins.env` to exercise apt, dnf, zypper, apk,
and pacman bootstrap paths. Overlay ports, CI source checkouts, and validation
container images must use immutable source references and hashes as described in
`docs/supply-chain-guardrails.md`.

AppImage's pinned libcrypto tracks OpenSSL 3.5 LTS (supported through April 2030),
independently of the vcpkg registry's OpenSSL version. Keep each runtime's
`libssl` and `libcrypto` compatible: a newer `libssl.so.3` can require symbols
that an older `libcrypto.so.3` does not provide despite the shared major version.
The AppImage CMake installer stays on 3.31 because pinned SoapySDR and AppImage
helper sources still declare policy versions rejected by CMake 4.

## Tooling Dependencies

CI and local quality tools are tracked through:

- `.github/requirements/*.in`
- `.github/requirements/*.txt`
- `.github/dependabot.yml`
- `.github/workflows/*.yml`
- `tools/*.sh` and `tools/lib/*.sh`

Hashed Python requirements are used where Python tooling is installed in CI.
GitHub Actions are pinned to immutable commit SHAs by policy.

The C/C++ quality jobs run in the pinned Arch Linux image
(`ARCHLINUX_BASE_DEVEL_IMAGE`) through `tools/ci_arch_toolchain.sh`, which takes
clang, clang-tidy, clang-format and scan-build from the Arch repositories at run
time (check the LLVM major with `clang --version` in the image) and builds
include-what-you-use from `IWYU_SHA`. IWYU compiles against the LLVM it is built
with and each IWYU commit supports one LLVM major, so when Arch moves to a new
LLVM major, move `IWYU_SHA` to a commit built for it in the same change, along
with the image digest. The inline `archlinux:base-devel@sha256:` copies in
`.github/workflows/linux-ci.yaml` (a container image cannot read the env file)
must move together with `ARCHLINUX_BASE_DEVEL_IMAGE` in
`tools/ci-dependency-pins.env`; `tools/check_workflow_download_pins.sh` fails
when any of them differs.

`KOTLIN_COMPILER_VERSION` and `KOTLIN_COMPILER_SHA256` pin the Kotlin compiler
that the arm64 ctest legs download for the `ANDROID_*_JVM` tests, since their
runner image ships Java but no Kotlin. Move both together, taking the digest from
the release's `kotlin-compiler-<version>.zip.sha256` and checking it against the
downloaded archive; `tools/check_workflow_download_pins.sh` fails if a workflow
unpacks the compiler without checking it against `KOTLIN_COMPILER_SHA256`.

## Monitoring

The project monitors dependencies through:

- Dependabot for GitHub Actions and Python requirements
- GitHub dependency review on pull requests
- OSV-Scanner through `tools/osv_scan.sh`
- scheduled guardrail CI
- OpenSSF Scorecard

If a vulnerability is reported in a dependency:

1. Determine whether the vulnerable code is present and reachable.
2. Update or patch the dependency when exploitable.
3. If not exploitable, document the reason in the narrowest available
   suppression, VEX note, or advisory note.
4. Add tests or checks when the issue could regress.

## Update Policy

Dependency updates should:

- preserve license notices
- avoid unrelated refactors
- update documentation when dependency behavior or requirements change
- run focused protocol, DSP, runtime, or packaging tests as applicable
- run `tools/osv_scan.sh`
- receive human review when they affect compiled code, workflows, packaging, or
  release behavior

Native Airspy R2/Mini support is optional (`DSD_ENABLE_AIRSPY`, `DSD_REQUIRE_AIRSPY`) and uses libairspy directly. Install `libairspy-dev` on Debian/Ubuntu or `airspy` with Homebrew. Windows uses the pinned overlay; Android builds the vendored driver against its shared libusb. See [Airspy](airspy.md).

## Pinned PFFFT source

`cmake/Pffft.cmake` uses CMake 3.20-compatible FetchContent to acquire the fixed
commit archive with TLS and SHA-256 verification. Only `pffft.c` and
`pffft_common.c` are compiled; the fork's top-level project and FFTPACK reference
implementation are not built. The first configure needs HTTPS access to
codeload.github.com. PFFFT stays embedded, with no additional runtime package.

For fresh offline builds, extract the pinned archive and configure with
`-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT=/absolute/path/to/pffft` and
`-DFETCHCONTENT_FULLY_DISCONNECTED=ON`. The override accepts a checkout or source
archive without Git metadata; its provenance is the packager's responsibility.
Cached build trees can reconfigure with disconnected mode alone. Turn that mode
off before updating a downloaded pin. The fork selects SIMD only when supported
by the compiler target; `-DDSD_PFFFT_ENABLE_SIMD=OFF` selects scalar-one code.
Debug builds disable explicit FMA and floating-point contraction in the FFT.

To update, run `python3 tools/update_pffft.py FULL_COMMIT_SHA`. Review the fork
changes and both updated pins, then run FFT/spectrum and I/Q replay tests,
sanitizers, acquisition checks, and quality preflight. Roll back by restoring
both pin lines and reconfiguring. No patches are applied to downloaded sources.
`tests/tools/test_pffft_acquisition.py` exercises network, cached/offline, invalid
source-layout, missing-path and checksum-error cases, including source archives.

The only project header that includes PFFFT is `src/io/radio/rtl_fft_cache.h`.
Analyzers exclude the resolved dependency location, including a source override.
For cppcheck without a configured DSD build, set `DSD_PFFFT_SOURCE_DIR` to the
pinned fork checkout; the CI cppcheck job acquires it with the pinned-Git helper.
OSV scans the pinned commit explicitly; release SBOMs record its repository,
revision and checksum before upload and attestation. Installed, Qt, Android and
portable packages carry `pffft-LICENSE.txt` and `pffft-NOTICES.txt`, generated
under the build tree's `pffft-licenses/` directory.

### AUR and mbelib packaging

Run `python3 tools/update_aur_pffft.py /path/to/PKGBUILD`, then regenerate
`.SRCINFO` with `makepkg --printsrcinfo`. The helper adds the archive and checksum
to makepkg's source acquisition, supplies an offline CMake override, and rejects
stale pins for migrated sources. It also accepts older releases that still carry
their FFT. Both AUR CI jobs use it; recipe-only updates increment `pkgrel`.
Neither DSD nor mbelib needs a separate PFFFT dependency package.

The mbelib vcpkg overlay pins release 2.4.2. It acquires
mbelib's FFT through vcpkg's verified download/cache mechanism and passes an
offline override. Its independent `MBELIB_PFFFT_*` pins are mirrored in
`vcpkg-ports/mbe-neo/pffft.cmake` by `tools/check_vcpkg_overlay_pins.sh --fix`.
When changing the mbelib revision, align these with that revision's PFFFT pin.
