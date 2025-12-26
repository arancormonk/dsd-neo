# Code Map and Modules

High‑level layout with module responsibilities and libraries. All public headers live under `include/dsd-neo/...` and should be included via `#include <dsd-neo/...>`.

## Apps

- `apps/dsd-cli` — CLI entrypoint
  - Target: `dsd-neo`
  - Links against all module libraries; keep main thin (arg parsing, wiring)
  - Build files: `apps/dsd-cli/CMakeLists.txt`

## Engine

- Path: `src/engine`, `include/dsd-neo/engine`
- Target: `dsd-neo_engine`
- Responsibilities: top-level decode/encode runner and lifecycle (wires core/runtime/IO/protocol state machines)
  - Build files: `src/engine/CMakeLists.txt`

## Platform

- Path: `src/platform`, `include/dsd-neo/platform`
- Target: `dsd-neo_platform`
- Responsibilities: cross-platform primitives (audio backend, sockets, threading, timing, filesystem/curses compatibility)
  - Audio backends: PulseAudio (default on Unix-like systems) or PortAudio (Windows and optional elsewhere)
  - Build files: `src/platform/CMakeLists.txt`

## Core

- Path: `src/core`, `include/dsd-neo/core`
- Target: `dsd-neo_core`
- Responsibilities: cross‑protocol glue (audio output helpers, vocoder glue, frame dispatch, GPS, file import), misc/util
  - Build files: `src/core/CMakeLists.txt`

## Runtime

- Path: `src/runtime`, `include/dsd-neo/runtime`
- Target: `dsd-neo_runtime`
- Responsibilities: config, logging, aligned memory, rings, worker pools, RT scheduling, git version
  - Build files: `src/runtime/CMakeLists.txt`

### Telemetry Hooks (DSP/Protocol → UI)

The runtime module defines telemetry hook interfaces in `include/dsd-neo/runtime/telemetry.h` that allow DSP and protocol code to publish state snapshots without depending on UI internals. DSP and protocol code should include this header rather than UI headers directly.

**Available hooks:**

- `ui_publish_snapshot(state)` — publish demod state for UI rendering
- `ui_publish_opts_snapshot(opts)` — publish options when they change
- `ui_request_redraw()` — request UI refresh
- `ui_publish_both_and_redraw(opts, state)` — convenience combo

**Weak stub pattern:** The runtime provides weak no‑op implementations (`src/runtime/ui_async_stubs.c`) so headless builds and unit tests link without requiring the UI module. When the terminal UI is linked, strong definitions in `src/ui/terminal/` override these stubs.

**Dependency direction:** DSP/Protocol → Runtime (hooks) ← UI (implementations). This keeps DSP UI‑agnostic while allowing state propagation.

## DSP

- Path: `src/dsp`, `include/dsd-neo/dsp`
- Target: `dsd-neo_dsp`
- Responsibilities: demodulation pipeline, cascaded decimation/resampler, filters, FLL/TED, CQPSK helpers (matched/RRC), and SIMD helpers. Exposes runtime‑tunable parameters consumed by the UI.
  - Build files: `src/dsp/CMakeLists.txt`

Runtime controls (via `include/dsd-neo/io/rtl_stream_c.h`):

- Coarse toggles: `rtl_stream_toggle_cqpsk`, `rtl_stream_toggle_fll`, `rtl_stream_toggle_ted`; snapshot via `rtl_stream_dsp_get`.
- TED: `rtl_stream_set_ted_sps`/`rtl_stream_get_ted_sps`, `rtl_stream_set_ted_gain`/`rtl_stream_get_ted_gain`, `rtl_stream_set_ted_force`/`rtl_stream_get_ted_force`, residual via `rtl_stream_ted_bias`.
- C4FM helpers: clock assist `rtl_stream_set_c4fm_clk`/`rtl_stream_get_c4fm_clk`, sync assist `rtl_stream_set_c4fm_clk_sync`/`rtl_stream_get_c4fm_clk_sync`.
- FM/FSK conditioning: FM AGC get/set + params, FM limiter, I/Q DC blocker get/set.
- Spectral/diagnostics: constellation/eye/spectrum getters, spectrum FFT size set/get, SNR getters/estimates for C4FM/CQPSK/GFSK.
- Front‑end assists: tuner autogain get/set, IQ balance toggle/get, resampler target set, auto‑PPM query/lock/toggle.

## IO

- Path: `src/io`, `include/dsd-neo/io`
- Targets:
  - `dsd-neo_io_radio` — RTL‑SDR front‑end and orchestrator for USB and rtl_tcp; provides constellation/eye/spectrum snapshots, optional bias‑tee, fs/4 capture shift, and auto‑PPM hooks.
  - `dsd-neo_io_audio` — network audio inputs: UDP PCM16LE input and TCP PCM16LE input.
  - `dsd-neo_io_control` — control interfaces: UDP retune control server, rigctl/serial, and UDP audio “socket blaster” output.

Key public headers:

- RTL shim API: `include/dsd-neo/io/rtl_stream_c.h` (C API for stream lifecycle, tuning, SNR, constellation/eye, spectrum, and DSP runtime controls).
- RTL device I/O: `include/dsd-neo/io/rtl_device.h` (USB and rtl_tcp backends, tuner gain/PPM helpers, FS/4 capture shift pipeline hooks).
- C++ orchestrator RAII: `include/dsd-neo/io/rtl_stream.h` (class `RtlSdrOrchestrator`, used by the C shim and the radio pipeline).
- UDP control API: `include/dsd-neo/io/udp_control.h` (retune command listener with callback).
- UDP audio output: `include/dsd-neo/io/udp_audio.h` (PCM16 “socket blaster”; implemented in `src/io/control/dsd_rigctl.c`).
- UDP PCM input: `include/dsd-neo/io/udp_input.h` (PCM16LE UDP input backend helpers).
- TCP PCM input: `include/dsd-neo/io/tcp_input.h` (PCM16LE TCP input backend helpers).

Notes:

- Local audio backends (PulseAudio/PortAudio) and device listing live in `dsd-neo_platform` (see `src/platform/audio_*.c`).
- M17 UDP/IP framing and sockets are implemented in the protocol module (`src/protocol/m17/m17.c`) and surfaced via UI/services.

Build files: `src/io/CMakeLists.txt` (defines radio/audio/control subtargets)

## FEC

- Path: `src/fec`, `include/dsd-neo/fec`
- Target: `dsd-neo_fec`
- Responsibilities: BCH, Golay, Hamming, RS, BPTC, CRC/FCS
  - Build files: `src/fec/CMakeLists.txt`

## Crypto

- Path: `src/crypto`, `include/dsd-neo/crypto`
- Target: `dsd-neo_crypto`
- Responsibilities: stream/block ciphers and helpers (RC2/RC4/DES/AES/etc)
  - Build files: `src/crypto/CMakeLists.txt`

## Protocols

- Path: `src/protocol`, `include/dsd-neo/protocol`
- Targets (one per protocol):
  - `dsd-neo_proto_dmr`, `dsd-neo_proto_dpmr`, `dsd-neo_proto_dstar`, `dsd-neo_proto_nxdn`, `dsd-neo_proto_p25` (phase1/phase2), `dsd-neo_proto_m17`, `dsd-neo_proto_x2tdma`, `dsd-neo_proto_edacs`, `dsd-neo_proto_provoice`, `dsd-neo_proto_ysf`

Key public headers (selection):

- DMR: `<dsd-neo/protocol/dmr/dmr_const.h>`, `<dsd-neo/protocol/dmr/dmr_utils_api.h>`, `<dsd-neo/protocol/dmr/dmr_trunk_sm.h>`
- P25: `<dsd-neo/protocol/p25/p25p1_const.h>`, `<dsd-neo/protocol/p25/p25_trunk_sm.h>`, `<dsd-neo/protocol/p25/p25_sm_watchdog.h>`
- NXDN: `<dsd-neo/protocol/nxdn/nxdn_const.h>`
- D‑STAR: `<dsd-neo/protocol/dstar/dstar_const.h>`, `<dsd-neo/protocol/dstar/dstar_header.h>`
- ProVoice/EDACS/X2: `<dsd-neo/protocol/provoice/provoice_const.h>`, `<dsd-neo/protocol/x2tdma/x2tdma_const.h>`

Build files: `src/protocol/CMakeLists.txt` and per‑protocol `src/protocol/<name>/CMakeLists.txt`

## Third‑Party

- Paths:
  - `src/third_party/ezpwd` — Target: `dsd-neo_ezpwd` (INTERFACE; headers included via `src/third_party` path)
  - `src/third_party/pffft` — Target: `dsd-neo_pffft` (STATIC; FFT helper for spectrum/diagnostics)
- Build files: `src/third_party/CMakeLists.txt` and subdirectory `CMakeLists.txt` files

## UI

- Path: `src/ui`, `include/dsd-neo/ui`
- Target: `dsd-neo_ui_terminal`
- Responsibilities: ncurses terminal UI. The menu system is moving to a data‑driven core in `menu_core.[ch]` with standardized prompt helpers and declarative menu tables. Legacy menus remain functional during migration. Includes live visualizers (constellation, eye diagram, spectrum, FSK histogram) driven by the RTL shim API.

Build files: `src/ui/CMakeLists.txt`, `src/ui/terminal/CMakeLists.txt`

Key public headers:

- Menu core/services: `include/dsd-neo/ui/menu_core.h`, `include/dsd-neo/ui/menu_defs.h`, `include/dsd-neo/ui/menu_services.h`
- Async/UI plumbing: `include/dsd-neo/ui/ui_async.h`, `include/dsd-neo/ui/ui_cmd.h`, `include/dsd-neo/ui/ui_cmd_dispatch.h`, `include/dsd-neo/ui/ui_dsp_cmd.h`, `include/dsd-neo/ui/ui_snapshot.h`, `include/dsd-neo/ui/ui_opts_snapshot.h`, `include/dsd-neo/ui/ui_prims.h`, `include/dsd-neo/ui/keymap.h`, `include/dsd-neo/ui/panels.h`

### Adding Menu Items

- Define a handler:
  - Prefer a service in `include/dsd-neo/ui/menu_services.h` with implementation in `src/ui/terminal/menu_services.c` for side effects (I/O, mode switches, file ops).
  - UI handlers in `menu_core.c` should be thin wrappers that call service helpers and use `ui_prompt_*` to gather input.
- Extend a menu table:
  - Add an `NcMenuItem` entry to the relevant menu array in `menu_core.c`. Set `id`, `label`, optional `help`, and `.on_select`.
  - For nested menus, set `.submenu` and `.submenu_len` to a child array.
- Keep UI/business logic separate:
  - Do not perform device or file operations directly in `dsd_ncurses_menu.c`. Use services instead to make behavior testable and reusable by other front‑ends.
- Prompts and exit:
  - Use the nonblocking prompt overlays provided by the menu core (string/int/double/confirm equivalents handled asynchronously). Handlers can set `exitflag` to request immediate exit; the loop will return.

## Include Prefix Summary

- Core: `<dsd-neo/core/...>`
- Engine: `<dsd-neo/engine/...>`
- Platform: `<dsd-neo/platform/...>`
- Runtime: `<dsd-neo/runtime/...>`
- DSP: `<dsd-neo/dsp/...>`
- IO: `<dsd-neo/io/...>`
- FEC: `<dsd-neo/fec/...>`
- Crypto: `<dsd-neo/crypto/...>`
- Protocols: `<dsd-neo/protocol/<name>/...>`

Additional includes of interest:

- IO: `<dsd-neo/io/rtl_stream_c.h>`, `<dsd-neo/io/rtl_stream.h>`, `<dsd-neo/io/rtl_device.h>`, `<dsd-neo/io/rtl_demod_config.h>`, `<dsd-neo/io/rtl_metrics.h>`, `<dsd-neo/io/udp_audio.h>`, `<dsd-neo/io/udp_control.h>`, `<dsd-neo/io/udp_input.h>`, `<dsd-neo/io/tcp_input.h>`
- UI: `<dsd-neo/ui/menu_core.h>`, `<dsd-neo/ui/menu_defs.h>`, `<dsd-neo/ui/menu_services.h>`

## Build Targets

- Libraries build under `src/...`; the CLI builds under `apps/dsd-cli` as `dsd-neo`.
- Use CMake presets (see `CMakePresets.json`).
- Tests live under `tests/<area>` and are wired with CTest; run with `ctest --preset dev-debug -V`.

Top‑level build files: `CMakeLists.txt`, `CMakePresets.json`, `apps/CMakeLists.txt`, `tests/CMakeLists.txt`

External dependencies (resolved via CMake):

- Required: LibSndFile; curses (ncursesw/PDCurses); an audio backend (PulseAudio by default, PortAudio on Windows); MBE vocoder (`mbe-neo`).
- Optional: RTL‑SDR, CODEC2.
