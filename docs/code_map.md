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
- Responsibilities: demodulation pipeline, cascaded decimation/resampler, filters, FLL/TED, CQPSK helpers (LMS/DFE, matched/RRC), and SIMD helpers. Exposes runtime‑tunable parameters consumed by the UI.

## IO

- Path: `src/io`, `include/dsd-neo/io`
- Targets:
  - `dsd-neo_io_radio` — RTL‑SDR front‑end and orchestrator for USB and rtl_tcp; provides constellation/eye/spectrum snapshots, optional bias‑tee, fs/4 capture shift, and auto‑PPM hooks.
  - `dsd-neo_io_audio` — audio I/O backends: PulseAudio playback and UDP PCM input. PortAudio device listing is available when enabled.
  - `dsd-neo_io_control` — UDP retune control server and control interfaces (rigctl/serial); also brokers M17 UDP/IP when configured.

Key public headers:

- RTL shim API: `include/dsd-neo/io/rtl_stream_c.h` (C API for stream lifecycle, tuning, SNR, constellation/eye, spectrum, and DSP runtime controls).

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

## UI

- Path: `src/ui`, `include/dsd-neo/ui`
- Target: `dsd-neo_ui_terminal`
- Responsibilities: ncurses terminal UI. The menu system is moving to a data‑driven core in `menu_core.[ch]` with standardized prompt helpers and declarative menu tables. Legacy menus remain functional during migration. Includes live visualizers (constellation, eye diagram, spectrum, FSK histogram) driven by the RTL shim API.

### Adding Menu Items
- Define a handler:
  - Prefer a service in `include/dsd-neo/ui/menu_services.h` with implementation in `src/ui/terminal/menu_services.c` for side effects (I/O, mode switches, file ops).
  - UI handlers in `menu_core.c` should be thin wrappers that call service helpers and use `ui_prompt_*` to gather input.
- Extend a menu table:
  - Add an `NcMenuItem` entry to the relevant table (e.g., `ui_menu_main`, `ui_menu_io_options`, `ui_menu_dsp_options`). Set `id`, `label`, optional `help`, and `.on_select`.
  - For nested menus, set `.submenu` and `.submenu_len` to a child array.
- Keep UI/business logic separate:
  - Do not perform device or file operations directly in `dsd_ncurses_menu.c`. Use services instead to make behavior testable and reusable by other front‑ends.
- Prompts and exit:
  - Use `ui_prompt_string/int/double/confirm`. Handlers can set `exitflag` to request immediate exit; the loop will return.

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
- Tests live under `tests/<area>` and are wired with CTest; run with `ctest --preset dev-debug -V`.
