# Code Map and Modules

High‑level layout with module responsibilities and libraries. All public headers live under `include/dsd-neo/...` and should be included via `#include <dsd-neo/...>`.

## Apps

- `apps/dsd-cli` — CLI entrypoint
  - Target: `dsd-neo`
  - Links against all module libraries; keep main thin (arg parsing, wiring)

## Core

- Path: `src/core`, `include/dsd-neo/core`
- Target: `dsd-neo_core`
- Responsibilities: cross‑protocol glue (audio output helpers, vocoder glue, frame dispatch, GPS, file import), misc/util

## Runtime

- Path: `src/runtime`, `include/dsd-neo/runtime`
- Target: `dsd-neo_runtime`
- Responsibilities: config, logging, aligned memory, rings, worker pools, RT scheduling, git version

## DSP

- Path: `src/dsp`, `include/dsd-neo/dsp`
- Target: `dsd-neo_dsp`
- Responsibilities: demodulation pipeline, resampler, filters, FLL, TED, SIMD helpers

## IO

- Path: `src/io`, `include/dsd-neo/io`
- Targets:
  - `dsd-neo_io_radio` — RTL‑SDR front‑end and orchestrator (optional; stubbed if not found)
  - `dsd-neo_io_audio` — audio device discovery/helpers (PortAudio, PulseAudio)
  - `dsd-neo_io_control` — UDP control server, rigctl/serial

## FEC

- Path: `src/fec`, `include/dsd-neo/fec`
- Target: `dsd-neo_fec`
- Responsibilities: BCH, Golay, Hamming, RS, BPTC, CRC/FCS

## Crypto

- Path: `src/crypto`, `include/dsd-neo/crypto`
- Target: `dsd-neo_crypto`
- Responsibilities: stream/block ciphers and helpers (RC2/RC4/DES/AES/etc)

## Protocols

- Path: `src/protocol`, `include/dsd-neo/protocol`
- Targets (one per protocol):
  - `dsd-neo_proto_dmr`, `dsd-neo_proto_dpmr`, `dsd-neo_proto_dstar`, `dsd-neo_proto_nxdn`, `dsd-neo_proto_p25` (phase1/phase2), `dsd-neo_proto_m17`, `dsd-neo_proto_x2tdma`, `dsd-neo_proto_edacs`, `dsd-neo_proto_provoice`, `dsd-neo_proto_ysf`

## Third‑Party

- Path: `src/third_party/ezpwd`
- Target: `dsd-neo_ezpwd` (INTERFACE; headers included via `src/third_party` path)

## Include Prefix Summary

- Core: `<dsd-neo/core/...>`
- Runtime: `<dsd-neo/runtime/...>`
- DSP: `<dsd-neo/dsp/...>`
- IO: `<dsd-neo/io/...>`
- FEC: `<dsd-neo/fec/...>`
- Crypto: `<dsd-neo/crypto/...>`
- Protocols: `<dsd-neo/protocol/<name>/...>`

## Build Targets

- Libraries build under `src/...`; the CLI builds under `apps/dsd-cli` as `dsd-neo`.
- Use CMake presets (see `CMakePresets.json`).
