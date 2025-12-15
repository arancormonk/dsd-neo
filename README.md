# DSD-neo

A modular and performance‑enhanced version of the well-known Digital Speech Decoder (DSD) with a modern CMake build, split into focused libraries (runtime, DSP, IO, FEC, crypto, protocol, core, ui) and a thin CLI.

Project homepage: https://github.com/arancormonk/dsd-neo

[![linux-ci](https://github.com/arancormonk/dsd-neo/actions/workflows/linux-ci.yaml/badge.svg)](https://github.com/arancormonk/dsd-neo/actions/workflows/linux-ci.yaml)
[![windows-ci](https://github.com/arancormonk/dsd-neo/actions/workflows/windows-ci.yaml/badge.svg)](https://github.com/arancormonk/dsd-neo/actions/workflows/windows-ci.yaml)
[![macos-ci](https://github.com/arancormonk/dsd-neo/actions/workflows/macos-ci.yaml/badge.svg)](https://github.com/arancormonk/dsd-neo/actions/workflows/macos-ci.yaml)

![DSD-neo](images/dsd-neo_const_view.png)

## Downloads

- Stable releases (**TBD**):
  - Linux AppImage (x86_64): dsd-neo-linux-x86_64-portable-<version>.AppImage
  - Linux AppImage (aarch64): dsd-neo-linux-aarch64-portable-<version>.AppImage
  - macOS DMG (arm64): dsd-neo-macos-arm64-portable-<version>.dmg
  - Windows native ZIP (MSVC x86_64, **recommended**): dsd-neo-msvc-x86_64-native-<version>.zip
  - Windows native ZIP (MinGW x86_64, alternative): dsd-neo-mingw-x86_64-native-<version>.zip
  - Windows portable ZIP (Cygwin x86_64, fallback/portable): dsd-neo-cygwin-x86_64-portable-<version>.zip
- Nightly builds:
  - Linux AppImage (x86_64): [dsd-neo-linux-x86_64-portable-nightly.AppImage](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-linux-x86_64-portable-nightly.AppImage)
  - Linux AppImage (aarch64): [dsd-neo-linux-aarch64-portable-nightly.AppImage](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-linux-aarch64-portable-nightly.AppImage)
  - macOS DMG (arm64): [dsd-neo-macos-arm64-portable-nightly.dmg](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-macos-arm64-portable-nightly.dmg)
  - Windows native ZIP (MSVC x86_64, **recommended**): [dsd-neo-msvc-x86_64-native-nightly.zip](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-msvc-x86_64-native-nightly.zip)
  - Windows native ZIP (MinGW x86_64, alternative): [dsd-neo-mingw-x86_64-native-nightly.zip](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-mingw-x86_64-native-nightly.zip)
  - Windows portable ZIP (Cygwin x86_64, fallback/portable): [dsd-neo-cygwin-x86_64-portable-nightly.zip](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-cygwin-x86_64-portable-nightly.zip)
- Arch Linux (AUR): [dsd-neo-git](https://aur.archlinux.org/packages/dsd-neo-git)

On Windows, the native MSVC ZIP is the preferred download for best integration with the Windows console and audio stack. The MinGW ZIP is a fully native alternative. The Cygwin portable ZIP bundles a POSIX runtime and launchers for users who want a self‑contained environment.

## Project Status

This project is an active work in progress as we decouple from the upstream fork and continue modularization. Expect breaking changes to build presets, options, CLI flags, and internal library boundaries while this stabilization work proceeds. The main branch may be volatile; for deployments, prefer building a known commit. Issues and PRs are welcome—please include logs and reproduction details when reporting regressions.

## Overview

- A performance‑enhanced fork of [lwvmobile/dsd-fme](https://github.com/lwvmobile/dsd-fme), which is a fork of [szechyjs/dsd](https://github.com/szechyjs/dsd)
- Modularized fork with clear boundaries: `runtime`, `dsp`, `io`, `fec`, `crypto`, `protocol`, `core`, plus `ui` and a CLI app.
- Protocol coverage: DMR, dPMR, D‑STAR, NXDN, P25 Phase 1/2, X2‑TDMA, EDACS, ProVoice, M17, YSF.
- Requires [arancormonk/mbelib-neo](https://github.com/arancormonk/mbelib-neo) for IMBE/AMBE vocoder primitives.
- Public headers live under `include/dsd-neo/...` and are included as `#include <dsd-neo/<module>/<header>>`.

## How DSD‑neo Is Different

- More input and streaming options

  - Direct RTL‑SDR USB, plus RTL‑TCP (`-i rtltcp[:host:port]`) and generic IQ TCP (`-i tcp[:host:port]`, SDR++/GRC 7355).
  - UDP audio in/out: receive PCM16 over UDP as an input, and send decoded audio to UDP sinks for easy piping to other apps or hosts.
  - M17 UDP/IP in/out: dedicated M17 frame input/output over UDP (`-i m17udp[:bind:17000]`, `-o m17udp[:host:17000]`).

- Built‑in trunking workflow

  - Follow P25 and DMR trunked voice automatically using channel maps and group lists (`-C ...csv`, `-G group.csv`, `-T`, `-N`).
  - On‑the‑fly control via UDP retune or rigctl; pairs well with TCP/RTL‑TCP inputs.

- RTL‑SDR quality‑of‑life features

  - Bias‑tee control (when supported by your librtlsdr), manual or auto gain, power squelch, adjustable tuner bandwidth, and per‑run PPM correction.
  - Optional spectrum‑based auto‑PPM drift correction with SNR/power gating and short training/lock, for long unattended runs.
  - rtl_tcp niceties: configurable prebuffering to reduce dropouts and settings tuned for stable network use.

- RTL‑SDR optimizations and diagnostics

  - Real‑time visual aids in the terminal for faster setup and troubleshooting:
    - Constellation view with adjustable gating and normalization.
    - Eye diagram (Unicode/ASCII, optional color) with adaptive scales and level guides.
    - Spectrum analyzer with adjustable FFT size.
    - FSK 4‑level histogram and live per‑modulation SNR readouts.
  - Heavily optimized RTL path for smoother audio and fewer dropouts:
    - One‑pass byte→I/Q widening with optional 90° rotation and DC‑spur fs/4 capture shift (configurable).
    - Cascaded decimation and an optional rational resampler to keep processing efficient and responsive.
    - Optional auto‑PPM correction from the timing error detector for long unattended runs.
  - Device control from the UI: toggle bias‑tee, switch AGC/manual gain, adjust bandwidth and squelch, and retune quickly.

- Expanded DSP controls for power users

  - Changes apply instantly from the UI and persist across retunes.
  - See [docs/cli.md](docs/cli.md) for environment variable reference.

- Portable, ready‑to‑run builds
  - Linux AppImage, macOS DMG, and Windows portable ZIP releases.

How this compares at a glance

- Versus DSD‑FME: similar protocol coverage and UI heritage, but DSD‑neo adds network‑friendly I/O (UDP audio in), refined RTL‑TCP handling (prebuffer, tuned defaults), optional auto‑PPM, and packaged cross‑platform binaries.
- Versus the original DSD: more protocols (notably P25 Phase 2, M17, YSF, EDACS), built‑in trunking, network inputs, device control, and an interactive UI.

## Build From Source

Requirements

- C compiler with C11 and C++ compiler with C++14 support.
- CMake ≥ 3.20.
- Dependencies:
  - Required: libsndfile, ncurses (wide), PulseAudio.
  - Optional: librtlsdr (RTL‑SDR support), Codec2 (additional vocoder paths), help2man (man page generation).
  - Vocoder: mbelib-neo (`mbe-neo` CMake package) is required.

OS package hints

- Ubuntu/Debian (apt):
  - `sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build libsndfile1-dev libpulse-dev libncurses-dev librtlsdr-dev`
- macOS (Homebrew):
  - `brew install cmake ninja libsndfile ncurses pulseaudio librtlsdr codec2`
- Windows:
  - Preferred binary: the native MSVC ZIP. The MinGW ZIP is an alternative native build.
  - For a bundled POSIX runtime and launchers, use the portable Cygwin ZIP; see `packaging/windows-cygwin/README-windows.txt`.
  - Source builds generally target POSIX layers (Cygwin/MSYS2).

Using CMake presets (recommended)

```
# From the repository root

# Debug build
cmake --preset dev-debug
cmake --build --preset dev-debug -j

# Release build (can enable fast-math/LTO/IPO)
cmake --preset dev-release
cmake --build --preset dev-release -j

# Run tests
ctest --preset dev-debug -V

# Coverage (optional)
tools/coverage.sh  # generates build/coverage-debug/coverage_html
```

Notes

- Presets create out‑of‑source builds under `build/<preset>/`. Run from the repo root.
- The CLI binary outputs to `build/<preset>/apps/dsd-cli/dsd-neo`.

Quick examples

- UDP in → Pulse out with UI: `dsd-neo -i udp -o pulse -N`
- DMR trunking from TCP IQ (with rigctl): `dsd-neo -fs -i tcp -U 4532 -T -C dmr_t3_chan.csv -G group.csv -N`

Manual configure/build

```
mkdir -p build && cd build
cmake ..
cmake --build . -j
```

## Install / Uninstall

```
# Single-config generators (Unix Makefiles/Ninja):
cmake --install build/dev-release

# Multi-config generators (Visual Studio/Xcode):
cmake --install build/dev-release --config Release

# Uninstall from the same build directory
cmake --build build/dev-release --target uninstall
```

## Configuration Options

- Build hygiene and optimization:
  - `-DDSD_ENABLE_WARNINGS=ON` — Enable common warnings (default ON).
  - `-DDSD_WARNINGS_AS_ERRORS=ON` — Treat warnings as errors.
  - `-DDSD_ENABLE_FAST_MATH=ON` — Enable fast‑math (`-ffast-math`/`/fp:fast`) across targets.
  - `-DDSD_ENABLE_LTO=ON` — Enable IPO/LTO in Release builds (when supported).
  - `-DDSD_ENABLE_NATIVE=ON` — Enable `-march=native -mtune=native` (non‑portable binaries).
  - `-DDSD_ENABLE_ASAN=ON` — AddressSanitizer in Debug builds.
  - `-DDSD_ENABLE_UBSAN=ON` — UndefinedBehaviorSanitizer in Debug builds.
- UI and behavior toggles:
  - `-DCOLORS=OFF` — Disable ncurses color output.
  - `-DCOLORSLOGS=OFF` — Disable colored terminal/log output.
- Protocol and feature knobs:
  - `-DPVC=ON` — Enable ProVoice Conventional Frame Sync.
  - `-DLZ=ON` — Enable LimaZulu‑requested NXDN tweaks.
  - `-DSID=ON` — Enable experimental P25p1 Soft ID decoding.
- Optional backends (auto‑detected):
  - `RTLSDR_FOUND` — Builds RTL‑SDR radio front‑end (`-DUSE_RTLSDR`).
  - `CODEC2_FOUND` — Enables Codec2 support (`-DUSE_CODEC2`).

## Runtime Tuning

Most users can run with defaults. For advanced tuning, see [docs/cli.md](docs/cli.md).

Common options:

- Auto‑PPM drift correction (RTL‑SDR): `--auto-ppm`
- RTL‑TCP adaptive buffering: `--rtltcp-autotune`
- Rig control (SDR++): `-U 4532` (default port), `-B <Hz>` (bandwidth)

## Using The CLI

- See the friendly CLI guide: [docs/cli.md](docs/cli.md)
  - Or run `dsd-neo -h` for quick usage in your terminal.
  - Digital/analog output gain: `-g <float>` (digital; `0` = auto, `1` ≈ 2%, `50` = 100%) and `-n <float>` (analog 0–100%).
  - DMR mono helpers:
    - Modern form: `-fs -nm` (DMR BS/MS simplex + mono audio).
    - Legacy alias: `-fr` (kept as a shorthand for the same DMR‑mono profile).

## Configuration

- INI‑style user config is implemented for stable defaults (input/output/mode/trunking); see `docs/config-system.md`.
- Default path: `${XDG_CONFIG_HOME:-$HOME/.config}/dsd-neo/config.ini`. Override with `--config <path>` or `DSD_NEO_CONFIG`.
- Disable config loading with `--no-config` or `DSD_NEO_NO_CONFIG`.
- `--interactive-setup` forces the bootstrap wizard even when a config exists; `--print-config` dumps the effective config as INI.
- When a config path is known, the final settings are autosaved on exit. A no‑arg run uses the config if present; otherwise it
  falls back to the interactive setup (unless disabled).

## Tests and Examples

- Run all tests: `ctest --preset dev-debug -V` (or `ctest --test-dir build/dev-debug -V`).
- Scope: unit tests for protocol helpers (P25 p1/p2, IDEN maps, CRC/RS), crypto, and FEC primitives are included and run via CTest.
- Contributions: prefer small, testable helpers and add focused tests under `tests/<area>`.

## Documentation

- Module overview and targets are documented in `docs/code_map.md`.
- Build presets are defined in `CMakePresets.json`.

## Project Layout

- Apps: `apps/dsd-cli` — CLI entrypoint, target `dsd-neo`.
- Core: `src/core`, headers `<dsd-neo/core/...>` — glue (audio, vocoder, frame dispatch, GPS, file import).
- Runtime: `src/runtime`, headers `<dsd-neo/runtime/...>` — config, logging, aligned memory, rings, worker pool, RT scheduling, git version.
- DSP: `src/dsp`, headers `<dsd-neo/dsp/...>` — demod pipeline, resampler, filters, FLL/TED, SIMD helpers.
- IO: `src/io`, headers `<dsd-neo/io/...>` — radio (RTL‑SDR), audio (PulseAudio + UDP PCM input/output), control (UDP/rigctl/serial).
- FEC: `src/fec`, headers `<dsd-neo/fec/...>` — BCH, Golay, Hamming, RS, BPTC, CRC/FCS.
- Crypto: `src/crypto`, headers `<dsd-neo/crypto/...>` — RC2/RC4/DES/AES and helpers.
- Protocols: `src/protocol/<name>`, headers `<dsd-neo/protocol/<name>/...>` — DMR, dPMR, D‑STAR, NXDN, P25, X2‑TDMA, EDACS, ProVoice, M17, YSF.
- Third‑party: `src/third_party/ezpwd` — INTERFACE target `dsd-neo_ezpwd`.

## Tooling

- Format: `tools/format.sh` (requires `clang-format`; see `.clang-format`).
- Static analysis: `tools/clang_tidy.sh` (use `--strict` for extra checks) or `clang-tidy -p build/dev-debug <files>`.
- Git hooks: `tools/install-git-hooks.sh` enables auto‑format on commit.

## Contributing

- Languages: C (C11) and C++ (C++14). Indent width 4 spaces; no tabs; brace all control statements; line length ≤ 120.
- Use project‑prefixed includes only: `#include <dsd-neo/...>`.
- Before sending changes: build presets you touched, run `tools/format.sh`, address feasible clang‑tidy warnings.

## License

- Project license: GPL‑3.0‑or‑later (see `LICENSE`).
- Portions remain under ISC per the original DSD author (see `COPYRIGHT`).
- Third-party notices live in `THIRD_PARTY.md` (ezpwd LGPL text: `src/third_party/ezpwd/lesser.txt`).
- Source files carry SPDX identifiers reflecting their license.
