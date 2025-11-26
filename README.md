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
  - Windows ZIP (x86_64): dsd-neo-cygwin-x86_64-portable-<version>.zip
- Nightly builds:
  - Linux AppImage (x86_64): [dsd-neo-linux-x86_64-portable-nightly.AppImage](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-linux-x86_64-portable-nightly.AppImage)
  - Linux AppImage (aarch64): [dsd-neo-linux-aarch64-portable-nightly.AppImage](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-linux-aarch64-portable-nightly.AppImage)
  - macOS DMG (arm64): [dsd-neo-macos-arm64-portable-nightly.dmg](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-macos-arm64-portable-nightly.dmg)
  - Windows ZIP (x86_64): [dsd-neo-cygwin-x86_64-portable-nightly.zip](https://github.com/arancormonk/dsd-neo/releases/download/nightly/dsd-neo-cygwin-x86_64-portable-nightly.zip)

## Project Status

This project is an active work in progress as we decouple from the upstream fork and continue modularization. Expect breaking changes to build presets, options, CLI flags, and internal library boundaries while this stabilization work proceeds. The main branch may be volatile; for deployments, prefer building a known commit. Issues and PRs are welcome—please include logs and reproduction details when reporting regressions.

## Overview

- A performance‑enhanced fork of [lwvmobile/dsd-fme](https://github.com/lwvmobile/dsd-fme), which is a fork of [szechyjs/dsd](https://github.com/szechyjs/dsd)
- Modularized fork with clear boundaries: `runtime`, `dsp`, `io`, `fec`, `crypto`, `protocol`, `core`, plus `ui` and a CLI app.
- Protocol coverage: DMR, dPMR, D‑STAR, NXDN, P25 Phase 1/2, X2‑TDMA, EDACS, ProVoice, M17, YSF.
- Integrates with [arancormonk/mbelib-neo](https://github.com/arancormonk/mbelib-neo) for IMBE/AMBE vocoder primitives; falls back to legacy MBE if needed.
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

- RTL‑SDR advanced driver options (env)

  - Direct sampling selection: `DSD_NEO_RTL_DIRECT=0|1|2|I|Q` (0 off, 1 I‑ADC, 2 Q‑ADC)
  - Offset tuning control: `DSD_NEO_RTL_OFFSET_TUNING=0|1` (default: try enable)
  - Crystal reference overrides: `DSD_NEO_RTL_XTAL_HZ=<Hz>`, `DSD_NEO_TUNER_XTAL_HZ=<Hz>`
  - IF stage gains: `DSD_NEO_RTL_IF_GAINS="stage:gain[,stage:gain]..."` where `gain` is dB or 0.1 dB
  - Test mode (ramp source): `DSD_NEO_RTL_TESTMODE=0|1`

- Expanded DSP controls (power users welcome)

  - Adaptive equalizer and decision‑feedback equalizer toggles with adjustable parameters.
- Matched filters with adjustable RRC parameters (alpha, span).
  - Timing and carrier helpers: enable/disable TED and FLL, tweak TED rate/gain, and force TED when needed.
  - IQ balance prefilter and DQPSK decision mode for tough RF environments.
  - Sane DSP defaults per modulation/standard, with quick toggles for power users.
  - Changes apply instantly from the UI and persist across retunes, so you can iterate quickly without restarting.
  - These controls go beyond what most open DSD projects expose directly.

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
  - Required: libsndfile, ITPP, ncurses (wide), PulseAudio.
  - Optional: librtlsdr (RTL‑SDR support), Codec2 (additional vocoder paths), help2man (man page generation).
  - Vocoder: prefers mbelib‑neo CMake package (`mbe-neo`); otherwise uses legacy `MBE` find module.

OS package hints

- Ubuntu/Debian (apt):
  - `sudo apt-get update && sudo apt-get install -y build-essential cmake ninja-build libsndfile1-dev libpulse-dev libncurses-dev libitpp-dev librtlsdr-dev`
- macOS (Homebrew):
  - `brew install cmake ninja libsndfile itpp ncurses pulseaudio librtlsdr codec2`
- Windows:
  - Source builds generally target POSIX layers (Cygwin/MSYS2). For a portable runtime, see `packaging/windows-cygwin/README-windows.txt`.

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

## Runtime Tuning (Environment/CLI)

- Auto‑PPM (RTL‑SDR only): spectrum‑based drift correction with training/lock.

  - CLI: `--auto-ppm`, `--auto-ppm-snr <dB>` (default 6).
  - Env: `DSD_NEO_AUTO_PPM=1`, `DSD_NEO_AUTO_PPM_SNR_DB=<dB>`,
    `DSD_NEO_AUTO_PPM_PWR_DB=<dB>` (absolute peak gate, default −80),
    `DSD_NEO_AUTO_PPM_ZEROLOCK_PPM=<ppm>` (zero‑step lock guard, default 0.6),
    `DSD_NEO_AUTO_PPM_ZEROLOCK_HZ=<Hz>` (default 60),
    `DSD_NEO_AUTO_PPM_FREEZE=0/1` (freeze/allow retunes during training; default freeze).
  - Behavior: estimates df via parabolic peak interpolation near DC; applies ±1 ppm steps with throttling, direction self‑calibration (SNR/|df| improvement), and locks after short stability.

- Resampler (polyphase L/M):

  - Env: `DSD_NEO_RESAMP=48000` (default) or `off/0` to disable; sets demod/output target rate and L/M design.

- FLL/TED controls:

  - Env: `DSD_NEO_FLL=1`, `DSD_NEO_FLL_ALPHA/BETA/DEADBAND/SLEW` (Q15/Q14 gains),
    `DSD_NEO_TED=1`, `DSD_NEO_TED_GAIN=<float>`, `DSD_NEO_TED_FORCE=1`.

- FM/C4FM stabilization:

  - Env: `DSD_NEO_FM_AGC=1` (default off), `DSD_NEO_FM_AGC_TARGET/MIN/ALPHA_UP/ALPHA_DOWN`,
    `DSD_NEO_FM_LIMITER=1` (constant‑envelope limiter),
    `DSD_NEO_IQ_DC_BLOCK=1`, `DSD_NEO_IQ_DC_SHIFT=<k>`.

- Digital SNR squelch:

  - Env: `DSD_NEO_SNR_SQL_DB=<dB>` — skips expensive sync when estimated SNR below threshold for relevant modes.

- Capture/retune behavior:

  - Env: `DSD_NEO_DISABLE_FS4_SHIFT=1` (disable +fs/4 capture shift when offset tuning is off),
    `DSD_NEO_OUTPUT_CLEAR_ON_RETUNE=1`, `DSD_NEO_RETUNE_DRAIN_MS=<ms>`.

- Rig control defaults:

  - TCP rigctl default port 4532 (SDR++). CLI: `-U <port>`, bandwidth `-B <Hz>`.

- RTL‑TCP networking:
  - Env: `DSD_NEO_TCP_PREBUF_MS=<ms>` — prebuffer before starting demod (default 1000, clamp 5–1000).
  - Behavior: when using RTL‑TCP, the input ring auto‑resizes so that the
    requested prebuffer fits within ~50% of the ring (to leave headroom),
    yielding an effective prebuffer close to the requested duration at the
    active sample rate.
  - Env: `DSD_NEO_TCP_RCVBUF=<bytes>` — OS socket receive buffer (default ~4 MiB, OS‑capped).
  - Env: `DSD_NEO_TCP_BUFSZ=<bytes>` — user‑space read size per `recv` (default ~16 KiB for rtl_tcp).
  - Env: `DSD_NEO_TCP_WAITALL=0/1` — require full reads (`MSG_WAITALL`) for steadier cadence (default off for rtl_tcp).
  - Env: `DSD_NEO_TCP_STATS=1` — print periodic throughput/queue stats.
  - CLI: `--rtltcp-autotune` — enable adaptive tuning of buffering/recv size (BUFSZ/WAITALL) for imperfect networks.
  - Behavior: TCP keepalive is enabled; if the link drops, the client auto‑reconnects and reapplies tuner settings.
  - On reconnect, advanced driver options (direct sampling, offset tuning, testmode, xtal, IF gains) are replayed.

- Additional runtime controls (environment):

  - Deemphasis/post‑filters: `DSD_NEO_DEEMPH=off|50|75|nfm`, `DSD_NEO_AUDIO_LPF=<Hz>`.
  - C4FM helpers and Costas tuning: `DSD_NEO_C4FM_CLK=el|mm`, `DSD_NEO_C4FM_CLK_SYNC=1`,
    `DSD_NEO_COSTAS_BW/DAMPING`.
  - Experimental helpers: `DSD_NEO_CHANNEL_LPF=1`.
  - Misc: `DSD_NEO_MT=1` enables the light worker pool; `DSD_NEO_PDU_JSON=1` emits P25 MAC/VPDU JSON to stdout.

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
