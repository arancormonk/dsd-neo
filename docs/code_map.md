# Code Map and Modules

High-level layout with module responsibilities and libraries. All public headers live under `include/dsd-neo/...` and
should be included via `#include <dsd-neo/...>`.

## Top-Level Layout

- `apps/` — executable targets (currently `apps/dsd-cli` → `dsd-neo`)
- `src/<module>/` — implementation code for each module
- `include/dsd-neo/<module>/` — public headers for each module
- `tests/<area>/` — unit tests (CTest)
- `cmake/` — CMake helper modules and install/uninstall scripts
- `tools/` — development scripts (formatting, analysis, coverage)
- `docs/` — documentation
- `examples/` — sample CSV inputs (channel maps, groups, keys) used by the config system and tooling
- `packaging/` — packaging assets/scripts (AppImage, macOS)
- `android/` — Android app shell: Kotlin foreground service, JNI lifecycle glue, and vendored libusb/librtlsdr
  (`android/README.md`); built only for `ANDROID` with `DSD_ENABLE_QT_UI=ON`
- `images/` — screenshots and other project assets used by docs/README
- `vcpkg.json`, `vcpkg-configuration.json`, `vcpkg-ports/`, `vcpkg-triplets/` — vcpkg dependency management

Generated (do not edit/commit):

- `build/`, `vcpkg_installed/`, `compile_commands.json`

## Apps

- Path: `apps/dsd-cli`
- Target: `dsd-neo` (executable)
- Responsibilities: argument parsing + wiring; keep `main.c` thin and push logic into module libraries
  - Runtime CLI/bootstrap helpers: `include/dsd-neo/runtime/cli.h`
- Build files: `apps/dsd-cli/CMakeLists.txt`

## Engine

- Path: `src/engine` (including `src/engine/dispatch`), `include/dsd-neo/engine`
- Targets: `dsd-neo_engine`, `dsd-neo_dispatch`
- Responsibilities:
  - Top-level decode runner and lifecycle (wires core/runtime/IO/protocol state machines)
  - Protocol/frame dispatch glue
  - Trunk retune policy and bookkeeping: `src/engine/trunk_tuning.c` implements the tune-to-frequency,
    tune-to-control-channel and return-to-control-channel requests behind
    `include/dsd-neo/engine/trunk_tuning.h`, and `src/engine/trunk_tuning_hooks_install.c` installs them into the
    runtime table (`include/dsd-neo/runtime/trunk_tuning_hooks.h`) so protocol state machines can retune without
    depending on IO
  - Single-tuner trunk scan coordinator: `src/engine/trunk_scan.c` behind
    `include/dsd-neo/engine/trunk_scan.h` (see below)
  - Voice-gated scan for -Y and trunk scan (issue #381): `src/engine/scan_voice_gate.c` behind
    `include/dsd-neo/engine/scan_voice_gate.h`; its probe returns separate active and retained last-media clocks so
    a protocol terminator cannot erase the scanner's tail anchor (tests: `ENGINE_SCAN_VOICE_GATE`,
    `ENGINE_NO_CARRIER_RESET`, `ENGINE_TRUNK_SCAN`)
  - Installs runtime hook tables used by DSP/frame-sync code
    (`src/engine/frame_sync_hooks_install.c`, `include/dsd-neo/runtime/frame_sync_hooks.h`)
- Build files: `src/engine/CMakeLists.txt`

Key public headers:

- Decode runner and lifecycle: `include/dsd-neo/engine/engine.h`, `include/dsd-neo/engine/frame_processing.h`
- Protocol/frame dispatch: `include/dsd-neo/engine/protocol_dispatch.h`
- Trunk tuning policy: `include/dsd-neo/engine/trunk_tuning.h`
- Single-tuner trunk scan: `include/dsd-neo/engine/trunk_scan.h`

### Single-Tuner Trunk Scan

`src/engine/trunk_scan.c` owns the coordinator that rotates one retunable receiver across the explicit targets of a
target-list CSV (P25 trunk, DMR trunk, DMR conventional, NXDN trunk, and NXDN96/NXDN48 conventional). Operator-facing
behavior, the CSV columns, and the CLI/config options live in `docs/trunk-scan.md`;
`include/dsd-neo/engine/trunk_scan.h` is the whole public surface:

- Target list and loader: `dsd_trunk_scan_target` / `dsd_trunk_scan_target_list`,
  `dsd_trunk_scan_load_targets_csv()`, `dsd_trunk_scan_target_list_reset()`, and `dsd_trunk_scan_max_targets()` — the
  target count is bounded by a memory budget divided by the per-target snapshot size, so the cap tracks the struct
  instead of being a constant that goes stale.
- Lifecycle: `dsd_engine_trunk_scan_init()`, `dsd_engine_trunk_scan_tick()`, `dsd_engine_trunk_scan_shutdown()`, all
  driven from `src/engine/engine.c`. The coordinator hangs off `dsd_state` as the `DSD_STATE_EXT_ENGINE_TRUNK_SCAN`
  state extension; init installs the runtime hook table below and shutdown clears it, so a build that never starts a
  scan runs on the no-op defaults.
- Active-target queries used by the tuning layer and protocol code: `dsd_engine_trunk_scan_active_p25_ctx()`,
  `dsd_engine_trunk_scan_active_dmr_ctx()`, `dsd_engine_trunk_scan_active_chan_csv()`,
  `dsd_engine_trunk_scan_active_gfsk_symbol_rate()`, `dsd_engine_trunk_scan_active_p25_cqpsk_request()`,
  `dsd_engine_trunk_scan_saved_tuner_autogain()`, and `dsd_engine_trunk_scan_target_count()`.
- Conventional activity reports: `dsd_engine_trunk_scan_dmr_conventional_activity()` and
  `dsd_engine_trunk_scan_nxdn_conventional_activity()`, reached from protocol code through the runtime hooks.

Every parked target keeps its own snapshot of decoder state — channel map, trunking/LCN state, call and P25 identity
metadata (the IDEN tables and the user band plan behind them), the encrypted-target lockout ledger, and the NXDN
missing-channel ledger from `<dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>` — so a channel number or learned
control-channel state from one system is never reused on another. That is also why trunk scan rejects a global `-C`
channel map or `--p25-bandplan` and imports each target's `chan_csv` through throwaway options and its
`p25_bandplan_csv` straight from the path, so neither touches the live options.
The one deliberate crossing is by provenance: when the parked target's WACN/SYS is known and another target's
snapshot holds a P25 IDEN entry learned on that same WACN/SYS, the coordinator copies it into an empty slot at
trust 1 (`trunk_scan_share_peer_idens()`), and `dsd_engine_p25_bandplan_export()` in
`src/engine/p25_bandplan_export.c` merges every target's tables into one band-plan CSV. The keyring is not snapshotted: each target instead carries a static
`dsd_key_set` (key-file or direct-key columns) that the switch installs through the scan key swap in
`<dsd-neo/core/key_set.h>`, restoring the globals on unkeyed targets and at shutdown without touching the key epoch.

Tests: `tests/engine/test_engine_trunk_scan.c` (`ENGINE_TRUNK_SCAN`) and
`tests/engine/test_engine_synced_trunk_scan_tick.c` (`ENGINE_SYNCED_TRUNK_SCAN_TICK`).

### Per-channel decoder modes

- Core owns positional channel-map modes through `core/channel_mode.h`, implemented beside the LCN heap stores in
  `core/util/dsd_state_trunk_lcn.c`. Extension slot 5 transfers with map adoption and is cleared on map teardown.
- Runtime owns the exact configured decoder baseline and temporary class through `runtime/scan_mode.h` and
  `runtime/scan_mode.c` (extension slot 6). It uses the existing preset definitions while keeping the audio sink fixed.
  Suspend/update/resume supports global commands; scalar snapshot copies keep frontend state independent of live storage.
  Blank rows retain scope ownership. `dsd_scan_mode_configured_view()` borrows the baseline without copying its output
  label; consumers use their published snapshot, and persistence uses the exact configured preset (including custom sets).
  `dsd_scan_mode_apply_modulation()` owns target flags/locks for both entry and scope updates. Inherited profiles use the
  restored SPS hunt index, so AUTO's saved timing and the frontend's rate/levels agree after leaving a row.
- Engine `channel_scan.c` (extension slot 7) stages typed `-Y` entries for automatic, manual, and avoid stepping through
  tracked tuning. It commits mode/keys only after success and retains generation protection across pending requests.
  Configuration edits retry pending tunes on a later service pass; live output-rate changes do not trigger another tune.
  Failed rows advance to the next candidate; a later successful tune recovers any gate held by a partial backend failure.
  `dsd_engine_channel_scan_waiting()` only inspects ownership; `dsd_engine_channel_scan_service_sync()` services pending
  work and invalidates sync gathered before the transaction. Pending rows defer no-carrier call finalization until commit.
  `dsd_engine_reset_no_carrier_state()` shares decoder cleanup without recursively stepping or changing tuner ownership.
  `trunk_scan.c` selects the same classes from target types while retaining target snapshots and modulation/gain ownership.
- DSP `dsd_frame_sync_reset_acquisition()` drops outgoing profile proof, modulation votes, symbol history, hunt budgets,
  and slicer windows at committed row boundaries. Conventional rows also discard learned P25 modulation; trunk targets
  retain their own learned modulation. Normal no-carrier protocol confirmation resets remain in use.
  Unlocked RTL P25 trunk-scan targets try CQPSK after their first unproductive 4800-symbol/s dwell, before visiting 6000,
  so the default three-second target visit includes the trial. This changes acquisition scheduling, not symbol timing.
- Frontend snapshots deep-copy only scalar scope metadata. `dsd_app_snapshot_configured_mode()` exposes the baseline;
  `dsd_scan_mode_active()` and `dsd_scan_mode_effective_profile()` distinguish combined P25 from global AUTO.


## Platform

- Path: `src/platform`, `include/dsd-neo/platform`
- Target: `dsd-neo_platform`
- Responsibilities: cross-platform primitives (audio backend, sockets, threading, timing, filesystem/curses
  compatibility)
  - Directory listing: `dsd_dir_list()` in `include/dsd-neo/platform/file_compat.h`, implemented once per
    platform in `file_compat_posix.c` and `file_compat_win32.c`
  - Audio backends: selected by `DSD_AUDIO_BACKEND` (`auto` → PortAudio on Windows, PulseAudio
    elsewhere; `none` → `audio_null.c` discard/silence backend; `aaudio` → Android). Exactly one
    backend translation unit is compiled per build; the shared last-error store lives in
    `src/platform/audio_error_internal.h`
- Build files: `src/platform/CMakeLists.txt`

## Core

- Path: `src/core`, `include/dsd-neo/core`
- Target: `dsd-neo_core`
- Responsibilities: cross-protocol glue (audio output helpers, vocoder glue, frame helpers, GPS, file import),
  misc/util
- API note: the high-pass filter in `<dsd-neo/core/audio_filters.h>` is `dsd_hpf()`. It was renamed from
  `hpf()` because codec2 exports a symbol of that name and Android links codec2 statically, which turns the
  duplicate into a link error. It is the only filter in that header that is prefixed: codec2 exports none of
  the others (`lpf`, `lpf_f`, `hpf_f`, `hpf_dL`, `hpf_dR`, `pbf`), so renaming them would break out-of-tree
  callers for no benefit. Out-of-tree callers of `hpf()` need updating
- API note: `<dsd-neo/core/channel_label.h>`'s `dsd_channel_label_current()` resolves the one label a frontend
  should show for the channel being listened to: the active `--trunk-scan` target id, else the name of the `-Y`
  scan-list row the receiver is parked on; `dsd_channel_label_current_source()` says which of the two it is, so a
  frontend words it as a target or a channel and never names both at once. Those names come from a channel-map CSV
  that opts in with a `name` header column and live in a heap store beside the scan list, reached through
  `dsd_state_trunk_lcn_name_get()`/`_set()`/`_reserve()`/`_free()` in `src/core/util/dsd_state_trunk_lcn.c` and
  released by `dsd_state_trunk_lcn_free()`. Per-row key sets (key-file or direct-key columns, `-Y` only) live in a
  sibling store with the same shape (`dsd_state_trunk_lcn_keys_*`), swapped by `dsd_scan_keys_enter()`/
  `dsd_scan_keys_leave()` in `src/core/util/key_set.c`, and are likewise never deep-copied into the UI snapshot.
- API note: text arriving as UTF-16 code units (DMR UDT/SMS, talker aliases) is decoded with
  `<dsd-neo/core/utf16.h>` and printed one scalar value at a time through `dsd_unicode_fput_scalar()` in
  `<dsd-neo/runtime/unicode.h>`. Never pass a code unit to `%lc`: a lone surrogate has no encoding, and the
  Windows CRT turns that failed conversion into an unbounded write of the stack. Semgrep blocks `%lc`/`%ls`
- API note: the user-supplied P25 band plan (`--p25-bandplan`, `[trunking] p25_bandplan_csv`, a trunk-scan
  target's `p25_bandplan_csv`, the terminal/Qt imports) lives in `dsd_state` as `p25_bandplan_rows[]` beside the
  IDEN tables it seeds. `src/core/file/p25_bandplan_csv.c` parses and writes the CSV
  (`csvP25BandplanImportPath()`/`csvP25BandplanExportRows()`, dry run `dsd_csv_validate_p25_bandplan_file()`);
  `src/core/util/dsd_state_p25_bandplan.c` holds the seeding rule (`dsd_state_p25_bandplan_seed()`: only empty
  slots, rows naming the current WACN/SYS before rows naming none, an over-the-air entry is never displaced) that
  the importer and `p25_update_system_identity()` both apply, and `dsd_p25_bandplan_append_tables()`, the reverse
  direction the export uses. Every core CSV importer shares its token helpers through the module-private
  `src/core/file/csv_parse_internal.h`; the channel map's key column accepts decimal, `0x` hex and `<iden>-<chan>`
  spellings there.
- Build files: `src/core/CMakeLists.txt`

## Runtime

- Path: `src/runtime`, `include/dsd-neo/runtime`
- Target: `dsd-neo_runtime`
- Responsibilities:
  - Config system (schema, expansion, user config), logging, memory helpers, rings, worker pools, RT scheduling
  - CLI parsing and interactive/bootstrap helpers (`include/dsd-neo/runtime/cli.h`)
  - Hook interfaces that let DSP/protocol code publish state without depending on UI internals
  - RadioReference.com import client (`src/runtime/radioreference/`): SOAP envelope builder, expat response parser,
    worker-thread client with cancellation, and the generators that turn fetched systems into the channel-map and
    talkgroup CSVs `src/core/file/dsd_import.c` already parses. UI-agnostic C API in
    `include/dsd-neo/runtime/radioreference.h` and `radioreference_generate.h`; needs `USE_CURL` and `USE_EXPAT`, and
    reports `dsd_rr_available() == 0` without them. Shared curl setup lives in the module-private
    `src/runtime/curl_common.h`, alongside `rdio_export.c`'s use of it.
    The frontend-agnostic import policy — system classification, the import-plan builder, tune-frequency
    selection, Hz-to-MHz text, the baked-key rule and the output filename stem — lives in
    `radioreference/rr_import.c` behind `include/dsd-neo/runtime/radioreference_import.h` and compiles with
    or without curl and expat, so the no-expat configuration still builds it. `radioreference/rr_provenance.c`
    reads and writes the plain-text `<file>.rr` sidecars that make a generated CSV refreshable. Both
    frontends consume that header, so the Qt model and the terminal wizard cannot drift apart.
- Build files: `src/runtime/CMakeLists.txt`
- Config docs: `docs/config-system.md`, `docs/radioreference-import.md`

### Telemetry Hooks (DSP/Protocol → UI)

The runtime module defines telemetry hook interfaces in `include/dsd-neo/runtime/telemetry.h` that allow DSP and
protocol code to publish state snapshots without depending on UI internals. DSP and protocol code should include this
header rather than UI headers directly.

**Available hooks:**

- `dsd_telemetry_publish_snapshot(state)` — publish demod state for frontend rendering
- `dsd_telemetry_publish_opts_snapshot(opts)` — publish options when they change
- `dsd_telemetry_request_redraw()` — request frontend refresh
- `dsd_telemetry_publish_both_and_redraw(opts, state)` — convenience combo

**Hook registration pattern:** Runtime owns a thread-safe hook table (`src/runtime/telemetry_hooks.c`). App-control
installs frontend callbacks at startup (`src/app_control/telemetry_hooks_install.c`), and headless/test builds simply
run with the default no-callback state.

**Dependency direction:** DSP/Protocol → Runtime (hooks) ← UI (implementations). This keeps DSP UI-agnostic while
allowing state propagation.

### Frame Sync Hooks (DSP → Runtime ← Engine/Protocols)

DSP frame-sync code may need to trigger protocol-specific actions (for example, trunking state machine ticks) without
depending directly on protocol headers. The runtime provides a small hook table in
`include/dsd-neo/runtime/frame_sync_hooks.h`; the engine installs the concrete implementations at startup in
`src/engine/frame_sync_hooks_install.c`.

### Trunk Scan Hooks (Protocol/App-Control → Runtime ← Engine)

Protocol code reports to the single-tuner scan coordinator without depending on engine-owned headers, through the hook
table in `include/dsd-neo/runtime/trunk_scan_hooks.h` (`src/runtime/trunk_scan_hooks.c`). Unlike the other tables there
is no `*_hooks_install.c`: the coordinator's own lifetime is the installation, so `src/engine/trunk_scan.c` installs
the implementations from `dsd_engine_trunk_scan_init()` and clears them again on shutdown.

**Available hooks:**

- `dsd_trunk_scan_hook_p25_ctx()` / `dsd_trunk_scan_hook_dmr_ctx()` — the parked target's trunking state machine
  context, or NULL when trunk scan is not installed (`p25_trunk_sm.c`, `dmr_trunk_sm.c`, `nxdn_element.c`)
- `dsd_trunk_scan_hook_tick()` — step the rotation; called from the engine decode loop
- `dsd_trunk_scan_hook_dmr_conventional_activity()` / `dsd_trunk_scan_hook_nxdn_conventional_activity()` — report
  decoded conventional activity so the parked target keeps its park. Pass only identity that has already cleared the
  protocol's FEC/CRC gate; the coordinator runs it through the talkgroup policy before refreshing the hold, and ignores
  it unless the parked target is of the matching conventional family
- `dsd_trunk_scan_hook_active_chan_csv()` — the parked target's channel-map path, which `opts->chan_in_file` cannot
  answer while scanning (`src/protocol/nxdn/nxdn_trunk_diag.c`)
- `dsd_trunk_scan_hook_enc_lockout_clear_snapshots()` — scrub the encrypted-target lockout ledger parked in every
  target snapshot, so a user purge is not undone by the next rotation (`src/app_control/actions/actions_trunk.c`)
- `dsd_trunk_scan_hook_control()` — operator scan controls on the parked target list, op-coded (hold toggle, avoid the
  active target, clear avoids, advance now); answers "unavailable" when trunk scan is not installed
  (`src/app_control/app_command_queue.c`)

Retune requests use a sibling table, `include/dsd-neo/runtime/trunk_tuning_hooks.h`, whose implementations the engine
installs from `src/engine/trunk_tuning.c` in `src/engine/trunk_tuning_hooks_install.c`.

## App-Control

- Path: `src/app_control`, `include/dsd-neo/app_control`
- Target: `dsd-neo_app_control`
- Responsibilities:
  - Frontend metrics and raw telemetry snapshots used by the terminal renderer
  - Command queue dispatch and menu service helpers
  - Frontend runtime/control-pump glue and telemetry hook installation
  - Public frontend boundary headers under `<dsd-neo/app_control/...>`
  - RadioReference apply: `include/dsd-neo/app_control/rr_import_apply.h` carries the by-value apply payload
    and the pure plan-to-payload mapper; `src/app_control/rr_import_apply.c` implements it, and the
    `DSD_APP_CMD_RR_APPLY_IMPORT` / `DSD_APP_CMD_RR_ACCOUNT_SET` handlers run on the decoder thread so no
    frontend ever writes `dsd_opts` itself
- Behavior note: `dsd_app_frontend_get_metrics*` reports tuner and demodulator readings only for RTL-family
  input (`AUDIO_IN_RTL`, which covers both a local dongle and rtl_tcp). Every one of those readings comes from
  the RTL stream, whose state is process-global and outlives the session that produced it, so on a WAV, stdin,
  UDP, TCP or symbol-file session they would be a previous run's measurements rather than the current one's.
  Such sessions therefore report the defaults — no carrier lock, no CFO, no output/symbol rate, and the
  invalid-SNR sentinel — and a frontend should omit those rows rather than render them as zeros. Applies to
  every frontend, not just the Android app
- Build files: `src/app_control/CMakeLists.txt`

## DSP

- Path: `src/dsp`, `include/dsd-neo/dsp`
- Target: `dsd-neo_dsp`
- Responsibilities: demodulation pipeline, cascaded decimation/resampler, filters, OP25-style CQPSK timing/carrier
  recovery, CQPSK helpers
  (matched/RRC), and SIMD helpers; exposes runtime-tunable parameters consumed by the UI
- Build files: `src/dsp/CMakeLists.txt`
- `symbol_timing_debug.c`: measures the sub-symbol offset the decoder's symbol grid settled on and reports it once
  per accepted frame sync, behind `DSD_NEO_DEBUG_SYMBOL_TIMING` (see `docs/cli.md`). The sample trace it correlates
  over is filled by `dsd_symbol.c` and owned by decoder-state setup/teardown in `src/core/util/dsd_init.c`.
- `dsd_filters.c` owns the per-protocol matched filters, selected by kind rather than by calling one of four
  wrappers, because the symbol grid has to know when the stream it samples changes identity. It reads the raw
  discriminator until a sync names a protocol and the filter's output afterwards, and that output describes the
  signal one group delay in the past — 67 samples for NXDN48 at 20 samples per symbol. What the filter module
  promises the symbolizer is small: the delay, stated without disturbing a running filter, and a way to push
  history into a filter without reading an output. `MATCHED_FILTER_SEAM` pins that promise.
- `dsd_symbol.c` pays for every switch so the grid does not move. It keeps the raw samples it has consumed
  (`dsd_state::matched_filter`, see `core/state.h`); a filter switching on is primed from that history and fed its
  delay's worth of samples whose outputs are discarded, one switching off hands back the samples it still had in
  flight, which the grid re-reads before live input resumes, and between two filters only the difference in delay
  is owed. Without that the switch-on rewound the grid by a third of a symbol on NXDN48 and half a symbol on
  P25p1 roughly once per frame, and the switch-off skipped the same (#444). `SYMBOL_MATCHED_FILTER_SEAM` drives
  `getSymbol()` across each kind of switch and checks the content position never moves.
- `dsd_symbol.c` owns the open-loop FSK symbol grid. Only the inter-frame sync search moves it, by a whole sample at
  a time, on the first zero crossing latched in the previous symbol — a bang-bang loop on one unfiltered sample
  index, and between frames the only thing tracking the sampling instant across a call. Issue #444 documents how
  sensitive that is; the comment above `symbol_adjust_timing_nxdn()` records the five ways of damping it that were
  A/B'd on real captures and measured worse, so change it only with `tools/replay_ab.sh` evidence
  (`docs/testing.md`). The CQPSK path does not use any of this: it has a real timing loop in `costas.cpp`.

Runtime controls (via `include/dsd-neo/io/rtl_stream_c.h`):

- CQPSK control/status: `rtl_stream_toggle_cqpsk`, `rtl_stream_get_cqpsk_status`,
  `rtl_stream_request_cqpsk_reacquire`,
  `rtl_stream_set_ted_sps`/`rtl_stream_get_ted_sps`, `rtl_stream_set_ted_gain`/`rtl_stream_get_ted_gain`,
  and CQPSK timing residual via `rtl_stream_cqpsk_timing_bias`.
- FM/FSK conditioning: I/Q DC blocker get/set.
- Spectral/diagnostics: constellation/eye/spectrum getters, spectrum FFT size set/get, SNR getters/estimates for
  C4FM/CQPSK/GFSK.
- Front-end assists: tuner autogain get/set, IQ balance toggle/get, and auto-PPM query/lock/toggle.

## IO

- Path: `src/io`, `include/dsd-neo/io`
- Targets:
  - `dsd-neo_io_iq` — I/Q capture/replay metadata and file helpers; no SDR dependency
  - `dsd-neo_io_radio` — radio front-end and orchestrator for RTL-SDR (USB), RTL-TCP, and SoapySDR backends; provides
    constellation/eye/spectrum snapshots, optional bias-tee (RTL path), and auto-PPM hooks
    - Built when `DSD_HAS_RADIO` is true (RTL and/or Soapy available); otherwise provided as an INTERFACE stub target
  - `dsd-neo_io_audio` — network audio/input backends: UDP PCM16LE input, TCP PCM16LE input, UDP audio output helpers,
    and M17 UDP helpers
  - `dsd-neo_io_udp_control` — UDP retune control server (used by the RTL-SDR/FM helpers)
  - `dsd-neo_io_control` — rigctl/serial control interfaces

Key public headers:

- RTL stream C API: `include/dsd-neo/io/rtl_stream_c.h`
- RTL C++ orchestrator: `include/dsd-neo/io/rtl_stream.h` (class `RtlSdrOrchestrator`)
- RTL device/config/metrics: `include/dsd-neo/io/rtl_device.h`, `include/dsd-neo/io/rtl_demod_config.h`,
  `include/dsd-neo/io/rtl_metrics.h`
- Rig/control: `include/dsd-neo/io/control.h`, `include/dsd-neo/io/rigctl_client.h`,
  `include/dsd-neo/io/m17_udp.h`
- UDP control API: `include/dsd-neo/io/udp_control.h`
- UDP audio output: `include/dsd-neo/io/udp_audio.h` (implemented in `src/io/audio_backends/udp_audio.c`)
- UDP/TCP PCM input: `include/dsd-neo/io/udp_input.h`, `include/dsd-neo/io/tcp_input.h`
- I/Q capture/replay: `include/dsd-neo/io/iq_capture.h`, `include/dsd-neo/io/iq_replay.h`,
  `include/dsd-neo/io/iq_types.h`

Notes:

- Local audio output backends and audio device listing live in `dsd-neo_platform` (see `src/platform/audio_*.c`).
- Network audio/input backends live in `src/io/audio_backends/` (`udp_input.c`, `tcp_input.c`, `udp_audio.c`,
  `m17_udp.c`, `udp_bind.c`).
- M17 protocol frame packing/parsing lives in `src/protocol/m17/m17.c`; M17 UDP socket helpers are exposed via
  `include/dsd-neo/io/m17_udp.h`.

Build files: `src/io/CMakeLists.txt` (defines radio/audio/control subtargets)

## FEC

- Path: `src/fec`, `include/dsd-neo/fec`
- Target: `dsd-neo_fec`
- Responsibilities: BCH, Golay, Hamming, RS, and BPTC helpers. Protocol-specific CRC/FCS helpers live with the
  corresponding protocol modules under `src/protocol/...`.
- Build files: `src/fec/CMakeLists.txt`

## Crypto

- Path: `src/crypto`, `include/dsd-neo/crypto`
- Target: `dsd-neo_crypto`
- Responsibilities: stream/block ciphers and helpers (RC2/RC4/DES/AES/etc)
- Build files: `src/crypto/CMakeLists.txt`

## Protocols

- Path: `src/protocol`, `include/dsd-neo/protocol`
- Targets (one per protocol):
  - `dsd-neo_proto_dmr`, `dsd-neo_proto_dpmr`, `dsd-neo_proto_dstar`, `dsd-neo_proto_nxdn`, `dsd-neo_proto_p25`
    (phase1/phase2), `dsd-neo_proto_m17`, `dsd-neo_proto_x2tdma`, `dsd-neo_proto_edacs`, `dsd-neo_proto_provoice`,
    `dsd-neo_proto_ysf`

Notes:

- Optional codec integrations are expressed via feature interface targets:
  - `dsd-neo_feature_codec2` → `USE_CODEC2` (used by M17 when available)

P25 manual control-channel selection lives in `src/protocol/p25/p25_cc_selection.c`. The Frequency command routes
active single-system P25 sessions here; the module holds the watchdog guard through the runtime CC tuning hook,
call teardown, and acquisition restart. A learned CC type identifies quiet P25 sessions in mixed modes;
`noCarrier()` clears that evidence after another trunking protocol takes over. Extension ID 26
(`DSD_STATE_EXT_PROTO_P25_CC_SELECTION`) retains the site-specific cache requirement across no-carrier resets,
while network band plans and user settings survive.

Key public headers (selection):

- DMR: `<dsd-neo/protocol/dmr/dmr_utils_api.h>`, `<dsd-neo/protocol/dmr/dmr_trunk_sm.h>`
- P25: `<dsd-neo/protocol/p25/p25p1_const.h>`, `<dsd-neo/protocol/p25/p25_trunk_sm.h>`,
  `<dsd-neo/protocol/p25/p25_sm_watchdog.h>`
- NXDN: `<dsd-neo/protocol/nxdn/nxdn_const.h>`, `<dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>`
  (the movable missing-channel ledger and its exit summary, so trunk scan can park one per target)
- D‑STAR: `<dsd-neo/protocol/dstar/dstar_const.h>`, `<dsd-neo/protocol/dstar/dstar_header.h>`
- ProVoice/EDACS: `<dsd-neo/protocol/provoice/provoice_const.h>`

Private per-protocol modules worth knowing about:

- `src/protocol/dmr/dmr_confidence.{c,h}` — colour-code and voice-burst confidence, so a burst has to be corroborated
  before DMR decodes or unmutes it.
- `src/protocol/nxdn/nxdn_confirm.{c,h}` — its NXDN counterpart, and for the same reason: the 10-symbol sync word and
  one-parity-bit LICH ahead of it are weak enough that receiver noise clears both. Channel decoders report their CRC
  verdicts to it, and `nxdn_frame.c` consults it before refreshing the scan hold or synthesizing voice, as
  `nxdn_deperm.c`/`nxdn_element.c` do before publishing a RAN or a call. The engine clears it with the carrier through
  `nxdn_confirm_reset()`, the one entry point exported in `<dsd-neo/protocol/nxdn/nxdn.h>`.
  `NXDN_Elements_Content_decode()` carries no CRC verdict of its own: the channel decoders in `nxdn_deperm.c` and
  `NXDN_SACCH_Full_decode()` hand it CRC-verified content only, so the gate lives in those callers, with
  `nxdn_confirm_is_confirmed()` as the frame-level check at the sites that publish a call or refresh a scan hold.
- `src/protocol/m17/m17_confirm.{c,h}` — the same module for M17, whose sync chain opens on a preamble that is only an
  alternating symbol run. Frame handlers report `(own check || confirmed)`, `dispatch_m17.c` reports it on an EOT, and
  `dsd_frame_sync.c` reads the raw state to decide whether to keep extending a candidate chain.
- `src/protocol/dstar/dstar_confirm.{c,h}` and `src/protocol/provoice/provoice_confirm.{c,h}` — the same shape again,
  answering only the SPS hunt rather than gating audio (issue #421). Neither protocol has a per-frame check: a D-STAR
  superframe costs 1992 symbols and a ProVoice frame 736, and the AMBE/IMBE error counts they leave in
  `dsd_state::errs`/`errs2` are soft corrections, not verdicts. D-STAR confirms on a CRC-16/X.25 — the RF header via
  `processDSTAR_HD()`, or the header rebroadcast in `dstar_slow_data.c` — and both confirm on a second frame arriving
  behind its own exact sync word before the carrier drops. `processDSTAR()` and `processProVoice()` return the answer,
  the dispatch handlers map it to `dsd_frame_verdict`, and the engine clears it with the carrier through the exported
  `dstar_confirm_reset()`/`provoice_confirm_reset()`.
- `src/protocol/dpmr/dpmr_confirm.{c,h}` — the same shape for dPMR, gating both audio and the hunt (issue #407). The
  check is the CCH CRC-7, which was there all along: it covers the 41 payload bits behind all six Hamming(12,8)
  blocks, so a passing half means the half decoded. What it replaced was `dpmr_ids_are_strong()`, which accepted a
  CCH whose two leading Hamming blocks merely reported correctable — 13 of 16 syndromes are, so it passed 44% of
  noise superframes. One half passing is one chance in 128 and has to repeat; both halves in one frame is one in
  16384 and confirms outright. `processdPMRvoice()` returns how many halves passed, `dsd_dispatch_handle_dpmr()`
  maps that to `DSD_FRAME_VERDICT_PROFILE_PROVEN` (for two seconds after the last decode) or `UNPRODUCTIVE`, and the
  engine clears both through the exported `dpmr_confirm_reset()` in `<dsd-neo/protocol/dpmr/dpmr.h>`.
  `tests/protocol/dpmr/fixtures` holds the CCH reference vectors that prove the pipeline decodes correct dPMR, which
  the off-air `dpmr` capture never established.

Build files: `src/protocol/CMakeLists.txt` and per‑protocol `src/protocol/<name>/CMakeLists.txt`

## Third‑Party

- Paths:
  - `src/third_party/ezpwd` — Target: `dsd-neo_ezpwd` (INTERFACE; headers included via `src/third_party` path)
  - `src/third_party/pffft` — Target: `dsd-neo_pffft` (STATIC; FFT helper for spectrum/diagnostics)
- Build files: `src/third_party/CMakeLists.txt` and subdirectory `CMakeLists.txt` files

## UI

- Path: `src/ui`
- Targets: `dsd-neo_ui_terminal` (option `DSD_ENABLE_TERMINAL_UI`, default ON), `dsd-neo_ui_qt`
  (option `DSD_ENABLE_QT_UI`, default OFF)
- Responsibilities:
  - Terminal frontend implementation (panels, logging, protocol displays, visualizers)
  - Data-driven, nonblocking menu overlay implemented under `src/ui/terminal/` (`menu_*.c`, `menus/menu_defs.c`)
  - RadioReference import wizard: `rr_wizard_core.{h,c}` is the curses-free state machine (headless-testable,
    driven by the `RrWizardHooks` table) and `rr_panel.{h,c}` is the modal presenter that renders it and
    implements those hooks. Both live directly in `src/ui/terminal/` next to `menu_prompts.c`, not in
    `panels/`, which holds only the two non-modal display strips (`header.c`, `footer.c`)
  - Imported RadioReference systems: `rr_library.{h,c}` is the curses-free model of the imports directory
    (folds the group-list and channel-map halves of an import back into one system by RR system id, sorts,
    marks the in-use one, formats a row; capped at `RR_LIBRARY_MAX`) and `rr_panel.c` presents it as the
    **Imported Systems** browser. `csv_picker.{h,c}` offers the same directory's files of one `kind` to the
    "Import channel map/group list CSV..." menu items, with an "Enter a path..." row falling back to the plain
    prompt. Both headers are terminal-private, both modules are filesystem-only (no curses, no app-control),
    and both are tested headless against a scratch directory (`UI_RR_LIBRARY`, `UI_CSV_PICKER`)
  - Frontend-facing controls and DSP/RTL metrics normally flow through app-control commands and
    `include/dsd-neo/app_control/frontend.h`. The terminal frontend retains a small set of terminal-private backend
    integrations.
  - Radio-driven UI controls are gated by `USE_RADIO`; visualizers consume app-control frontend metric APIs.

Qt Quick frontend (`src/ui/qt`):

- QML plus C++ view-models (metrics, call history + per-view filters, saved systems, imported CSV files, app
  preferences, command bridge) that poll app-control on a timer; used by the Android app today and intended as the
  shared basis for a desktop GUI. `imported_files_model.{h,cpp}` is the library behind the CSV pickers: it copies
  picked documents into durable app storage through `DecoderHost::importDocument()` and dry-run validates them via
  `<dsd-neo/core/csv_validate.h>` (`src/core/file/dsd_import.c`, and `src/core/file/p25_bandplan_csv.c` for the
  P25 band plan kind) for row-count feedback.
  `radio_reference_model.{h,cpp}` plus `qml/RadioReferenceScreen.qml` are the RadioReference import: the model drives
  the runtime client, previews what an import would produce, and writes the generated CSVs into that same library with
  provenance, while the add-system wizard stays the single writer of a saved system. See
  `docs/radioreference-import.md`.
- Platform-free by rule: it may include Qt and `include/dsd-neo/app_control/` headers, never engine/io/protocol
  internals, and never platform APIs (`QJniObject`, `<android/*.h>`). Platform specifics live behind the `DecoderHost`
  interface, implemented per host (`android/decoder_host_android.cpp` today).
- Backend code must never include Qt headers; `cmake/arch_rules.cmake` enforces both directions
  (`tools/check_arch_rules.sh`, run by the pre-push hook and the CI guardrails job).

Build files: `src/ui/CMakeLists.txt`, `src/ui/terminal/CMakeLists.txt`, `src/ui/qt/CMakeLists.txt`

Key public headers:

- Frontend commands, history, metrics, and lifecycle: `include/dsd-neo/app_control/commands.h`,
  `include/dsd-neo/app_control/history.h`, `include/dsd-neo/app_control/frontend.h`, and
  `include/dsd-neo/app_control/frontend_runtime.h`
- Terminal-only headers live under `src/ui/terminal/dsd-neo/ui/` and are private to the terminal target/tests.

### Adding Menu Items

- Define a handler:
  - Prefer an app-control service in `src/app_control/services.h` with implementation in `src/app_control/` for side
    effects (I/O, mode switches, file ops).
  - Menu action handlers live in `src/ui/terminal/menu_actions.c` and should be thin wrappers that call service helpers
    and use `ui_prompt_open_*_async` to gather input.
- Find the row's home. The root (`src/ui/terminal/menus/menu_defs.c`) is the receiver's signal chain — Input, Decoder,
  Trunking, Encryption, Audio, Recording & logs — then Display, Config, Advanced. A setting goes where the signal it
  acts on lives, not where the module that implements it lives; every concept has exactly one home.
  - Within a submenu: a status row first (if any), the primary on/off switch, the settings people change often, then
    one-shot actions; destructive actions last, after a separator.
  - No single-item submenus. Nothing deeper than three submenus below the root. No submenu longer than fifteen
    rows (what a 24-row terminal shows).
  - A row that opens a submenu gets a trailing ` >` from the renderer, so its label never ends in `...`; `...` is
    reserved for rows that open a prompt or picker.
- Extend a menu table (`src/ui/terminal/menu_items.c`): add an `NcMenuItem` entry to the submenu array. The fields
  (`src/ui/terminal/dsd-neo/ui/menu_core.h`):
  - `id` — stable identifier; `help` — required on every action row (`h` shows it).
  - `label` or `label_fn` — a static label, or a generator in `src/ui/terminal/menu_labels.c` that renders the live
    state.
  - `on_select` — the action; or `submenu` + `submenu_len` for a nested array.
  - `is_enabled` — optional predicate; a hidden row is not drawn at all.
  - `hotkey` — the main-screen key(s) for the same action, drawn right-aligned on the row (`"t"`, `"+ -"`, `"P/p"`).
    The character must be the one defined in `src/ui/terminal/dsd-neo/ui/keymap.h` and dispatched in
    `src/ui/terminal/dsd_ncurses_handler.c`; a row never invents a key.
  - `kind` — `NC_ITEM_ACTION` (default, selectable), `NC_ITEM_STATUS` (dimmed, read-only, skipped by navigation),
    or `NC_ITEM_SEPARATOR` (a rule; `label` ignored).
- Label grammar: `Noun [State]` for toggles with `On`/`Off` (never `Active`/`Inactive`, never a `Toggle` prefix);
  `Noun... [current]` for rows that open a prompt or picker; an imperative verb (`Import ...`, `Clear ...`,
  `Restart ...`, `Save ...`, `Stop ...`) only for a one-shot action; sentence case; ASCII `...`.
- `tests/ui/test_ui_menu_tree_audit.c` (`UI_MENU_TREE_AUDIT`) walks the whole tree and enforces the above: help text
  on every action row, at least two action rows per submenu, the depth and length limits, no two rows sharing an
  action, no banned words in labels, and a hotkey table cross-checked against `keymap.h`. A new row therefore needs
  help text, a home that fits the rules, and — if it carries a hotkey — an entry in that test's hotkey table.
- Keep UI/business logic separate:
  - Do not perform device or file operations directly in menu callbacks. Use services instead to make behavior
    testable and reusable across command entry points.
- Prompts and exit:
  - Use the nonblocking prompt overlays provided by the menu core (string/int/double/confirm equivalents handled
    asynchronously). Handlers can set `exitflag` to request immediate exit; the loop will return.

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

- Engine: `<dsd-neo/engine/engine.h>`, `<dsd-neo/engine/frame_processing.h>`,
  `<dsd-neo/engine/protocol_dispatch.h>`, `<dsd-neo/engine/trunk_scan.h>`, `<dsd-neo/engine/trunk_tuning.h>`
- Runtime: `<dsd-neo/runtime/cli.h>`, `<dsd-neo/runtime/frame_sync_hooks.h>`, `<dsd-neo/runtime/telemetry.h>`,
  `<dsd-neo/runtime/trunk_scan_hooks.h>`, `<dsd-neo/runtime/trunk_tuning_hooks.h>`,
  `<dsd-neo/runtime/radioreference.h>`, `<dsd-neo/runtime/radioreference_generate.h>`,
  `<dsd-neo/runtime/radioreference_import.h>`
- IO: `<dsd-neo/io/rtl_stream_c.h>`, `<dsd-neo/io/rtl_stream.h>`, `<dsd-neo/io/rtl_device.h>`,
  `<dsd-neo/io/rtl_demod_config.h>`, `<dsd-neo/io/rtl_metrics.h>`, `<dsd-neo/io/control.h>`,
  `<dsd-neo/io/rigctl_client.h>`, `<dsd-neo/io/m17_udp.h>`, `<dsd-neo/io/udp_audio.h>`,
  `<dsd-neo/io/udp_control.h>`, `<dsd-neo/io/udp_input.h>`,
  `<dsd-neo/io/tcp_input.h>`
- App-control/UI: command, history, metrics, and lifecycle APIs live in `include/dsd-neo/app_control`; terminal internals
  are private under `src/ui/terminal/dsd-neo/ui`

## Build Targets

- Libraries build under `src/...`; the CLI builds under `apps/dsd-cli` as `dsd-neo`.
- Use CMake presets (see `CMakePresets.json`).
- Tests live under `tests/<area>` and are wired with CTest; run with `ctest --preset dev-debug --output-on-failure`.

Top‑level build files: `CMakeLists.txt`, `CMakePresets.json`, `apps/CMakeLists.txt`, `tests/CMakeLists.txt`

Common interface targets:

- `dsd-neo_warnings` — common warning flags (optional; controlled by `DSD_ENABLE_WARNINGS`, `DSD_WARNINGS_AS_ERRORS`)
- `dsd-neo_test_support` — test-only compile/link defaults used by `tests/` executables

Optional feature interface targets (compile definitions + include paths; stubbed out when deps are missing):

- `dsd-neo_feature_colors` — `PRETTY_COLORS` when terminal UI colors are enabled (`COLORS`)
- `dsd-neo_feature_colors_logs` — `PRETTY_COLORS_LOGS` when colored terminal/log output is enabled (`COLORSLOGS`)
- `dsd-neo_feature_pvc` — `PVCONVENTIONAL` when ProVoice conventional frame sync is enabled (`PVC`)
- `dsd-neo_feature_lz` — `LIMAZULUTWEAKS` when LimaZulu NXDN tweaks are enabled (`LZ`)
- `dsd-neo_feature_sid` — `SOFTID` when P25p1 soft ID decoding is enabled (`SID`)
- `dsd-neo_feature_radio` — `USE_RADIO` when any radio backend is available (`DSD_HAS_RADIO`)
- `dsd-neo_feature_rtlsdr` — `USE_RTLSDR` (+ `USE_RTLSDR_BIAS_TEE` when supported by librtlsdr)
- `dsd-neo_feature_soapy` — `USE_SOAPYSDR` + SoapySDR >= 0.8.1 imported-target link/includes when available
- `dsd-neo_feature_codec2` — `USE_CODEC2` (require with `DSD_REQUIRE_CODEC2=ON`)
- `dsd-neo_feature_curl` — `USE_CURL` + libcurl link when available (require with `DSD_REQUIRE_CURL=ON`)
- `dsd-neo_feature_expat` — `USE_EXPAT` + expat link when available (require with `DSD_REQUIRE_EXPAT=ON`)

External dependencies (resolved via CMake):

- Required: OpenSSL 3.x libcrypto; LibSndFile; an audio backend (PulseAudio by default, PortAudio on Windows); MBE
  vocoder (`mbe-neo` 2.x).
- Terminal frontend: curses (ncursesw/PDCurses), enabled by default with `DSD_ENABLE_TERMINAL_UI=ON`.
- Optional: RTL‑SDR, SoapySDR >= 0.8.1, CODEC2, libcurl >= 7.56.0.

### Scoped scan options

- Runtime `scan_options` parses the restricted `options` argument grammar into typed values and fixed key/path
  metadata, validates declared modes, and reports errors without echoing raw arguments. It never runs the CLI parser.
- Core `scan_profile` merges legacy key columns, resolves and loads companion files, and materializes direct keys.
  Positional profiles extend the existing channel-mode store in extension slot 5; runtime slot 6 retains nonsecret
  configured/active settings. Extension IDs and the main options/state struct layouts are unchanged.
- The talkgroup policy store supports decoder-thread retain/install/release operations. Profiles retain their own
  context so aliases and session policy changes survive visits; frontend snapshots still clone the effective context.
  Slot 5 holds the saved global group context and suspend/resume ownership. Clearing metadata restores it first.
- `dsd_scan_key_change_prepare()` allocates the incoming key copy and any needed baseline before a conventional tune;
  commit transfers ownership without allocation. Engine checks the map generation and key epoch before committing.
  Both coordinators restore mode, keys, group policy and scalar overrides together at their protected transitions.
- App-control scopes force/CRC/voice configuration commands and group imports, while live row policy mutations stay
  with the active context. Configuration export reads saved group paths and voice settings from the configured scope.
