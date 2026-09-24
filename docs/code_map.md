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
- `examples/` — sample CSV inputs (channel maps, groups, source IDs, keys) used by the config system and tooling
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
    `ENGINE_NO_CARRIER_RESET`, `ENGINE_TRUNK_SCAN`). The same file owns the scan-timing publication
    (`dsd_scan_timing_clear()` / `dsd_scan_timing_publish()`) and the -Y timing tick
    `dsd_engine_scan_y_timing_tick()`, which stamps `dsd_state::scan_timing` with the stay reason and the absolute
    monotonic deadline of the window that is running (issue #508); the deadline it publishes is the same instant
    `dsd_scan_voice_gate_should_step()` flips, so the readout cannot drift from the rotation it describes
  - Stepped slicer threshold refresh after each getFrameSync() return: `src/engine/slicer_thresholds.c` behind
    `include/dsd-neo/engine/slicer_thresholds.h` (test: `ENGINE_SLICER_THRESHOLDS`)
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
target-list CSV (P25 trunk/conventional, DMR trunk/conventional, and NXDN96/NXDN48 trunk/conventional). Operator-facing
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
- Conventional activity reports: `dsd_engine_trunk_scan_dmr_conventional_activity()`,
  `dsd_engine_trunk_scan_nxdn_conventional_activity()`, and `dsd_engine_trunk_scan_p25_conventional_activity()`,
  reached from protocol code through the runtime hooks.

Beside the active-target publication the coordinator also publishes the stay reason and live timing for the parked
target into `dsd_state::scan_timing` once per tick (issue #508), from the same effective dwell/hold resolvers the
rotation itself uses. It is a readout, not a decision: the reason function is pure and the old held/not-held verdict
is a one-line wrapper over it, so no rotation, hold or step moves because of it. `app_control/scan_timing_view`
turns it into a row (see below).

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

Recovery ownership is checked at the P25 watchdog, DMR tick and engine frame-sync callback boundaries. Runtime trunk-scan
hooks identify the parked protocol. Outside trunk scan, validated control/grant evidence retains a protocol owner
across sync loss; unrelated protocol decodes and raw sync cannot evict it. Ownership writes and watchdog reads share the decoder/SM guard, so the
watchdog predicate does not read the sync or CC-format hints modified by frame acquisition. DMR owns its decoded
CC heartbeat, six-second loss grace, two-second probe window and five-second backend deadline inside each
target's `dmr_sm_ctx_t`. Probes honor avoids and revisit the saved CC anchor between alternate frequencies.
Probe frequency is separate from the saved anchor until control or grant evidence confirms it. Target entry and accepted CC return restart acquisition, preventing a previous call or probe from holding
or retuning a newly parked target. Pending backend probes hold the target and disarm idle dwell; completion or timeout starts
a fresh dwell. The subsequent decoded-acquisition wait counts toward dwell. DMR heartbeats use the completed
tuning generation and frequency, avoiding blocking rigctl queries in the CSBK path.

Tests: `tests/engine/test_engine_trunk_scan.c` (`ENGINE_TRUNK_SCAN`) and
`tests/engine/test_engine_synced_trunk_scan_tick.c` (`ENGINE_SYNCED_TRUNK_SCAN_TICK`), and
`tests/engine/test_engine_dmr_cc_recovery.c` (`ENGINE_DMR_CC_RECOVERY`, real protocol/coordinator with simulated tuning).

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
  `dsd_scan_mode_row_options()` borrows the installed nonsecret row options (valid while suspended and on held
  snapshots). Row options are applied through a per-field table (`scan_option_appliers[]`), and the row squelch is
  pushed to the RTL demodulator from the scope's entry points only, once per row change. `dsd_scan_mode_enter()`
  never pushes, so every caller must follow it with `dsd_scan_mode_options()` (NULL for a row without options);
  `dsd_scan_mode_set_configured_squelch()` edits the configured default without suspending (see Scoped scan options).
- Engine `channel_scan.c` (extension slot 7) stages typed `-Y` entries for automatic, manual, and avoid stepping through
  tracked tuning. It commits mode/keys only after success and retains generation protection across pending requests.
  Configuration edits retry pending tunes on a later service pass; live output-rate changes do not trigger another tune.
  Failed rows advance to the next candidate; a later successful tune recovers any gate held by a partial backend failure.
  `dsd_engine_channel_scan_waiting()` only inspects ownership; `dsd_engine_channel_scan_service_sync()` services pending
  work and invalidates sync gathered before the transaction. Pending rows defer no-carrier call finalization until commit.
  `dsd_engine_reset_no_carrier_state()` shares decoder cleanup without recursively stepping or changing tuner ownership.
  `trunk_scan.c` selects the same classes from target types while retaining target snapshots and modulation/gain ownership.
  Leaving the scan (`dsd_engine_channel_scan_leave()`) restores the configured RTL receive family through the metrics
  hooks: under `-fA` the configured analog profile (`apply_analog_profile`, analog family, demodulator kind and
  channel width with 0 meaning the default), otherwise the digital family first and then the restored symbol profile
  (`apply_demod_profile`), so the demod thread switches family before it applies the profile. When the front end still
  runs the analog family there (`analog_family_active`: an `-fA` session whose configured mode was changed to a digital
  one while a row ran, saving its timing for the analog output rate), the leave times the decoder, and the profile it
  publishes, for the rate the digital family lands on (`output_rate_for_family`). A leave that switches the front
  end's family either way also drops the analog monitor block the decoder has part-collected
  (`dsd_symbol_analog_block_reset()`). The M17 encoder is not the analog family. Test: `ENGINE_CHANNEL_SCAN`.
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
- `src/core/file/dsd_file.c` reads SDRTrunk JSON MBE exports. Its file-local crypto context owns NXDN SACCH
  position/session tracking and DMR AES late-entry context; playback does not call protocol-layer state-mutating
  IV expansion. Voice bits are FEC-decoded before decryption and then passed to the normal vocoder/output path.
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
- API note: source ID aliases live in the opaque store declared by `<dsd-neo/core/source_alias.h>` and
  implemented in `src/core/util/source_alias.c`, attached to state extension slot 8
  (`DSD_STATE_EXT_CORE_SOURCE_ALIAS`). `dsd_source_alias_store_create()`/`dsd_source_alias_store_append()` build
  a store; `dsd_source_alias_install()` takes ownership and replaces it, and `dsd_source_alias_clear()` removes it.
  `dsd_source_alias_loaded()` distinguishes an empty imported store from no store; `dsd_source_alias_count()`
  reports its row count. All import entry points accept a loaded empty list; startup failures warn without
  stopping decoding. `src/core/file/source_alias_csv.c` implements `dsd_source_alias_load(path, &out)`
  (fresh candidate, output untouched on open/read/allocation failure), `csvSrcImport()`/`csvSrcImportPath()`
  from `<dsd-neo/core/csv_import.h>`, and `dsd_csv_validate_src_file()` from `<dsd-neo/core/csv_validate.h>`.
  `dsd_source_alias_lookup()` uses exact-before-range matching (first exact row wins) and narrowest-range
  matching (last row wins ties);
  `dsd_source_label_lookup()` prefers aliases over the active group-list exact label, with mode only from
  group policy and OTA alias text untouched. The list is global across scan rows and immutable once installed;
  live access belongs to the decoder thread. `dsd_source_alias_copy_snapshot(dst, src)` deep-copies it for
  both snapshot hops, reuses an unchanged clone, and preserves an owned destination store on allocation failure.
  A shallow alias of the source is detached so destination cleanup cannot free the source. `CORE_SOURCE_ALIAS`
  tests matching, precedence, lifecycle and policy isolation; `CORE_SOURCE_ALIAS_FAIL` covers allocation/read failures and physical-line length boundaries.
- Invariant (patched P25 calls): a call snapshot's `policy_target_id` is the member WG the grant matched, while
  `ota_target_id` is the supergroup every frontend shows. `DSD_TG_POLICY_BLOCK_OTA_FINAL` in
  `<dsd-neo/core/talkgroup_policy.h>` names the supergroup blocks no member overrides (call skip, encryption
  lockout, session avoid, mode B/DE). The P25 grant path stops at them before considering members, and every
  consumer that judges a live call on its policy target also calls `dsd_tg_policy_apply_ota_final_blocks()` with
  the over-the-air target: the audio/record/P25p2 media gates, the row-edit release in app-control and the Qt block
  label. User blocks (Lock out, Avoid TG, Skip) are written against the over-the-air target.
- API note (audio output and WAV gates, `<dsd-neo/core/audio.h>`): every voice output path applies the talkgroup
  gate (`dsd_audio_group_gate_mono()`/`_dual()`). The short mono path and the legacy short output that SDRTrunk JSON
  playback uses share `dsd_audio_mono_output_muted()` (module-private, `src/core/audio/dsd_audio_internal.h`), the
  same gate plus live P25 Phase 1 crypto and reverse-mute rule as the float and stereo paths; that P25 rule is
  `p25_crypto_audio_output_permitted()` in `<dsd-neo/protocol/p25/p25_crypto.h>`, shared with the record gate.
  Per-call WAV writers use `dsd_audio_record_gate_mono()` (slot crypto, P25 reverse mute and policy), or
  `dsd_audio_record_policy_gate_slot()` (policy alone, on the slot the protocol publishes) where the decode path
  settles crypto without setting the DMR slot flags: X2-TDMA, D-STAR, YSF V/D2, EDACS analog and SDRTrunk JSON
  playback (YSF EHR and full-rate frames go through the vocoder's full record gate). Every MBE capture save point (`src/core/vocoder/dsd_mbe.c`, JSON playback in `dsd_file.c`) applies the
  same policy gate; live P25 Phase 1 capture also applies the P25 speaker rule, and
  `dsd_audio_p25p1_live_voice()` tells a live P25 call from a protocol borrowing the Phase 1 decoder (YSF full
  rate). The static WAV writes only recordable slots: the mono paths ask about the slot being played (X2-TDMA may
  play slot 1), and the stereo mixes write silent any channel whose routed source slot, as the output policy
  mirrors it, is record-blocked (`dsd_stereo_wav_channel_mask()`). The vocoder writes each MBE frame's per-call
  WAV block; YSF writes only its V/D2 frames, which decode straight through mbelib. FDMA dispatchers (NXDN, YSF, D-STAR, ProVoice, dPMR) set
  `currentslot` to 0, since these paths read it. EDACS analog voice applies the talkgroup gate at its own output.
  M17 is outside talkgroup policy (callsign addresses). DMR and P25 Phase 2 MBE capture save in
  `mbe_finalize_slot_left/right()` before `mbe_post_left/right_audio()` recomputes the slot mute flags, so the
  first frame after a mute change (reverse mute included) follows the previous frame's state.
- API note (analog receive options, `<dsd-neo/core/opts.h>`): `analog_demod` (`dsd_analog_demod`) is set by the
  decode presets (and restored with the scan-settings snapshot when a typed row is left); no width, CLI or menu code
  writes it directly. The analog preset selects FM and every other preset puts it back to FM.
  `analog_nfm_bandwidth_hz`/`analog_am_bandwidth_hz` hold the configured channel widths, where 0 means the kind's
  default rather than an explicit request. An explicit width, or the AM default, forces the channel filter on; only
  the unset NFM default keeps the historical enable rule (`rtl_demod_analog_requested_width_hz()`). The presets
  never touch the widths. `dsd_opts_is_analog_family()` names the `-fA` receive family; the M17 encoder shares the analog
  front end but is not part of it. `dsd_opts_analog_width_hz()` returns the explicit width for the active kind.
- API note (runtime sink changes, `<dsd-neo/core/audio.h>`): `dsd_audio_ensure_analog_output()` and
  `dsd_audio_ensure_digital_output()` open the sink a new receive family writes to (the raw monitor stream; the digital
  voice stream, plus the raw stream for ProVoice and `-8`) with the parameters `openAudioOutput()` uses, when the
  session plays to an unmuted local device and the sink is not open. With UDP output the raw sink is the analog socket
  on port + 2 (`udp_sockfdA`), opened through the `connect_analog` member of the runtime UDP audio hook table
  (`<dsd-neo/runtime/udp_audio_hooks.h>`, installed by the engine), muted or not: the mute toggle reopens local
  devices only, so a socket skipped while muted would stay closed. `udp_socket_blasterA()` skips an invalid socket.
  A new UDP target (`svc_udp_output_config()`) closes an open analog socket, which still sends to the old host and
  port, and reopens it for the new target at once, since the `-8` toggle opens no socket; with none open it opens one
  only when the current mode writes to it. Idempotent, decoder thread only; a failure is logged once and leaves that
  family silent. `DSD_APP_CMD_DECODE_MODE_SET` and the RadioReference import (both through
  `decode_mode_apply_value()`) call them, and so does `DSD_APP_CMD_CONFIG_APPLY` when its `[mode]` moves the session
  between the analog and digital families or lands on a digital mode that writes raw audio (ProVoice, `-8`). Tests:
  `CORE_AUDIO_ENSURE_OUTPUT`, `APP_COMMAND_QUEUE`, `APP_CONTROL_RR_APPLY`, `UI_MENU_SERVICES`.
- Build files: `src/core/CMakeLists.txt`

## Runtime

- Path: `src/runtime`, `include/dsd-neo/runtime`
- Target: `dsd-neo_runtime`
- Responsibilities:
  - Config system (schema, expansion, user config), logging, memory helpers, rings, worker pools, RT scheduling
  - CLI parsing and interactive/bootstrap helpers (`include/dsd-neo/runtime/cli.h`)
  - Hook interfaces that let DSP/protocol code publish state without depending on UI internals
  - Analog channel contract shared by the CLI, config, app commands, scan rows and the demodulator
    (`include/dsd-neo/runtime/analog_channel.h`, `src/runtime/analog_channel.c`): `dsd_analog_demod` (FM = 0,
    AM = 1), `dsd_rx_family`, per-kind width ranges and defaults (NFM 8000–25000 Hz, default 16000; AM
    5000–20000 Hz, default 6000), the strict whole-Hz parser, `dsd_analog_width_check()` and the kHz formatter. A
    width is accepted only when it is in range, width/2 + 600 Hz stays within 0.45 × the DSP rate, and the Blackman
    tap count fits the 288-tap analog capacity; the refusal names the width, the rate, the largest width that rate
    fits and the RTL DSP bandwidths that would fit. It is pure integer arithmetic so callers need not link the DSP;
    `src/dsp/demod_pipeline.cpp` static-asserts its design constants against the ones here. Tests:
    `RUNTIME_ANALOG_CHANNEL`, `DSP_CHANNEL_FILTERS` (validator and design agree across a width/rate grid; the
    analog design gates on `dsd_analog_width_realizable()` itself).
  - The RTL metrics hook table (`include/dsd-neo/runtime/rtl_stream_metrics_hooks.h`) also carries the receive-family
    request (`apply_analog_profile`), the published analog profile (`analog_profile`), whether the analog family runs
    (`analog_family_active`) and the output rate a family switch lands on (`output_rate_for_family`); the engine
    installs `rtl_stream_request_analog_profile()`, `rtl_stream_get_analog_profile()`,
    `rtl_stream_analog_family_active()` and `rtl_stream_output_rate_for_family()` behind them.
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
- `dsd_trunk_scan_hook_dmr_conventional_activity()` / `dsd_trunk_scan_hook_nxdn_conventional_activity()` /
  `dsd_trunk_scan_hook_p25_conventional_activity()` — report decoded conventional activity so the parked target keeps
  its park. Pass only identity that has already cleared the protocol's FEC/CRC gate; the coordinator runs it through
  the talkgroup policy before refreshing the hold, and ignores it unless the parked target is of the matching
  conventional family. P25 reports voice starts only, not PDU data, and treats decryptable calls as clear for this policy.
  Phase 2 XCCH reports only after MAC_PTT crypto resolution; Phase 1 late joins use the decoded service encryption
  bit while crypto classification is unknown. `p25_sm_note_conventional_activity()` rejects stale/unidentified calls.
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
  - Command queue dispatch and menu service helpers. Decoder-owner runtime start/stop opens/closes admission under
    the queue mutex; stop securely cancels pending payloads. Evicted or cancelled talkgroup exports publish failed
    completions. Successful inherited-policy scan exports update the configured persistence path.
    `app_command_queue.c` uses `command_writes_policy_store()` to guard live policy transactions against the P25
    watchdog's release-time audio flush; callees must use `_locked` forms to avoid acquiring the guard again.
  - Bootstrap retains only positional playback filenames in argv storage, preserving argument indexes with empty
    placeholders. State snapshots exclude argv ownership; teardown securely erases retained strings.
  - Source ID imports: `DSD_APP_CMD_IMPORT_SRC_LIST = 572` carries a path string;
    `DSD_APP_CMD_IMPORT_SRC_LIST_CLEAR = 573` has no payload. Import validates one candidate, refuses zero usable
    rows, and adopts that same store and path on success; failure preserves both.
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
- Behavior note: `channel_bandwidth_hz` on the analog monitor is the published analog width (the configured width
  while the width-driven channel filter runs), and `channel_bandwidth_dsp_limited` is set when the DSP rate rather than
  that filter bounds the channel (the historical default below a 20 kHz DSP rate, or a width the rate cannot realize);
  the reported width is then the one the rate leaves: the passband of the legacy WIDE plan when that plan runs
  (`dsd_channel_lpf_legacy_wide_width_hz()`), otherwise the DSP rate. Digital output and the M17 encoder's monitor
  path keep twice the profile's protected edge. Test: `APP_CONTROL_FRONTEND_METRICS`.
- Receive family on a live mode change: `svc_publish_symbol_profile()` (`src/app_control/symbol_profile.c`)
  publishes the configured analog profile for the analog family, which is what moves a running digital RTL front end
  onto the analog monitor. For a digital configured mode it requests the digital family before the symbol profile;
  while the front end still runs the analog family (`rtl_stream_analog_family_active()`, which also covers a CQPSK
  toggle or a typed row's profile under it) it times the decoder with `rtl_stream_output_rate_for_family()`, because
  the switch lands on the demod thread after the command returns and the analog family's output rate is not the
  digital stream's. Under a scan row's constraint the configured mode is the scan baseline
  (`dsd_scan_mode_configured_view()`), so republishing a typed digital row's profile on an analog session queues that
  profile alone and does not switch the front end in the middle of the row.
  `DSD_APP_CMD_DECODE_MODE_SET` also opens the new family's sink (`dsd_audio_ensure_*_output()`). Before it changes
  anything it asks a running RTL front end whether it takes the analog profile the mode will publish
  (`svc_check_mode_receive_profile()`, `rtl_stream_check_analog_profile()`: kind, range, `DSD_NEO_CHANNEL_LPF` and the
  published demod rate, refusal logged with the validator's text); a refusal fails the command with a toast and leaves
  the mode, options, sinks and front end as they were. The request it publishes after committing is held to the same
  rules again (at the published rate, and at the rate the demod thread applies it at): only a retune that moves the
  rate in between gets it refused there, logged, with the front end kept on its receive profile.
  `DSD_APP_CMD_CONFIG_APPLY` runs the same sink and publish sequence when its `[mode]` moves the session between the
  analog and digital families, after the same check when the move is onto the analog family (a refusal leaves the
  whole config unapplied); a digital-to-digital `[mode]` change keeps its earlier behaviour, except that ProVoice
  (or `-8`) also gets the raw sink it writes to. Like `DECODE_MODE_SET` it keeps the session's audio output layout
  (`pulse_digi_out_channels`, `pulse_digi_rate_out`) through any `[mode]`: the output streams were opened with it, so
  a digital sink a family change opens gets it too, and a later preset cannot leave the options on another layout
  than the open stream's. A `DECODE_MODE_SET`, RadioReference import or `[mode]` that moves the decoder between the
  analog and digital families drops the analog monitor block it has part-collected (`dsd_symbol_analog_block_reset()`),
  whose samples are the old family's; a change inside a family keeps it. Tests:
  `APP_COMMAND_QUEUE`, `APP_CONTROL_ACTIONS_RTL`.
- Shared display decisions, so no frontend has to restate one: `include/dsd-neo/app_control/call_view.h` and
  `src/app_control/call_view.c` fold the canonical call state into a per-slot line, and
  `include/dsd-neo/app_control/scan_timing_view.h` and `src/app_control/scan_timing_view.c` fold
  `dsd_state::scan_timing` into the Scan Timing row — the stay phrase, the remaining/total of the window that is
  running, and which of dwell/hold/hang is worth printing (issue #508). The decoder owns every deadline; these
  views only difference it against the caller's monotonic clock, which is what keeps the terminal row, the Qt panel
  and the Android app from drifting on what "suspended" or "hold" means. Tests: `APP_CONTROL_CALL_VIEW`,
  `APP_CONTROL_SCAN_TIMING_VIEW`, and the terminal goldens in `UI_NCURSES_PRINTER_HELPERS`.
  `include/dsd-neo/app_control/squelch_view.h` and `src/app_control/squelch_view.c` (issue #521) pair the squelch in
  force with the configured default, say whether a scan row overrides it and whether each level is off: the terminal
  SQL field and M17 VOX field (`-60.0 dB (row; default -80.0 dB)`), the DSP panel's `(row)` mark, the shadowed-edit
  toast, and Qt's `configuredSquelchDb`/`effectiveSquelchDb`, `configuredSquelchOff`/`effectiveSquelchOff`,
  `squelchRowOverride` and `squelchReadout` all come from it. The Qt radio panel lays those out as its whole-dB
  stepper reading, a `row` badge and `default X`, and uses `squelchReadout` as the reading's accessible name. It
  reads the row's value from `dsd_scan_mode_row_options()`, so it is also right on the decoder thread while a command
  has the scope suspended. Test: `APP_CONTROL_SQUELCH_VIEW`.
- Decode quality: `include/dsd-neo/app_control/p25_metrics.h` and `src/app_control/p25_metrics.c`
  copy FEC ok percentages, populated P25 voice-error averages, and non-P25 last-frame
  errors from the caller's held snapshot. The core vocoder maintains ring counts;
  canonical call starts reset the affected slot. Qt publishes one `qualityChanged`
  group, and the terminal average helpers wrap the same arithmetic. Counts are
  corrected errors per voice frame, not BER; 0/0 FEC ratios are invalid.
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
- The analog monitor's audible output leaves `dsd_symbol.c` in `symbol_output_unsynced_analog()`, after the voice
  filters and the gain stage (`symbol_apply_unsynced_filters()`: a fixed `analog_gain_f()` gain for any `-n`
  above 0, 12000x for RTL/I-Q input at the default 50 and 2.5x for PCM inputs, and the per-block `agsm_f()` AGC only
  at `-n 0`) and only while the squelch gate is open; for `audio_out_type == 8` it goes through `dsd_udp_audio_hook_blast_analog()` with a byte count of int16
  mono samples.
  The block (`dsd_state::analog_out_f`) collects unsynced samples in a digital session too, monitored or not (the
  CQPSK symbol-rate output excepted). `dsd_symbol_analog_block_reset()` (`<dsd-neo/dsp/symbol.h>`, decoder thread)
  drops a part-collected block; app-control and the channel-scan leave call it when the receive family changes.
  `tests/engine/analog_replay.c` (`dsd-neo_test_analog_replay`, the `DECODE_IQ_ANALOG_*` cases) captures and scores
  exactly that output through the hook, and times it with a wrapped RTL stream read hook, so changes to the monitor
  chain are measured against what a listener hears; back them with `tools/replay_ab.sh --metric analog` evidence
  (`docs/testing.md`). The host's own options are the `--analog-*` names it lists; other `--analog-*` arguments pass
  through to the CLI parser.
- `frame_sync_maybe_auto_switch_modulation()` (`dsd_frame_sync.c`) votes the C4FM/CQPSK/GFSK choice from SNR and
  sync hamming and applies the winner's demod profile to the RTL front end. It stands down under a modulation lock
  (`mod_cli_lock`) and in the analog family (`dsd_opts_is_analog_family()`), which has no digital modulation to
  choose: a CQPSK vote there (a carrier near 0 Hz) took the front end off the monitor path. Tests:
  `FRAME_SYNC_INTERNAL_HELPERS`, `DECODE_IQ_ANALOG_NO_MOD_AUTO_SWITCH`.
- Channel LPF (`demod_pipeline.cpp`): digital profiles design from their protected edge, capped at 144 taps with the
  63-tap fallback. The analog family (`demod_state::analog_family`, not the WIDE profile, which is also the digital
  fallback and the M17 encoder's) designs from `channel_lpf_width_hz` instead: cutoff width/2 + 600 Hz, the fixed
  1200 Hz Blackman transition, up to `DSD_CHANNEL_LPF_MAX_TAPS` (288), with no Nyquist clamp and no fallback — an
  unrealizable width leaves no plan (`dsd_channel_lpf_design_analog()` returns -1) and the block runs with no channel
  filter at all. The stream layer does not run such a width: it refuses one at start, on every request and on a retune
  that lands on a rate that cannot realize it, and stops the stream when the device does not return to a capture
  where it runs (see the IO notes). 16000 Hz runs
  the same design call as WIDE, so its taps are bit-identical wherever WIDE's design succeeds.
  `dsd_channel_lpf_legacy_wide_width_hz()` reports the passband the legacy WIDE plan has at a rate (the 144-tap design,
  its cutoff held to 0.9 x Nyquist, or above ~51.4 kHz the 63-tap fallback prototype, cut at a third of the rate), the
  width published for an unset default that plan runs.
  The plan cache key is (rate_out, profile, width). The SIMD complex FIR kernels size their scratch per call, so the
  288-tap capacity needs no kernel change. Tests: `DSP_CHANNEL_FILTERS`, `DSP_DEMOD_MISC`.

Runtime controls (via `include/dsd-neo/io/rtl_stream_c.h`):

- Receive family: `rtl_stream_request_analog_profile()` (family, analog kind, channel width; an analog request, a
  change on the running monitor and a switch onto it alike, is validated on the caller's thread against the running
  stream's published rate, or only for kind, range and `DSD_NEO_CHANNEL_LPF` with no stream running, and checked
  again on the demod thread at the rate it lands on; a refusal logged with the validator's text once per kind, width
  and rate, and applied on the demod thread ahead of any demod profile queued after it; a demod profile queued
  before it is dropped), `rtl_stream_check_analog_profile()`, `rtl_stream_get_analog_profile()`,
  `rtl_stream_analog_family_active()` (the analog family, including
  while a CQPSK toggle or a typed row's profile has moved the front end off the monitor output),
  `rtl_stream_output_rate_for_family()` (the output rate a pending switch will produce),
  `rtl_stream_set_digital_decode_modes()` (the decoder's configured digital modes, which pick the FSK channel profile
  a CQPSK toggle returns to once a live switch has moved the stream onto the digital family), and
  `rtl_stream_prepare_retune_analog_profile_for_target()` (the same fields bound to a retune target).
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
  - `dsd-neo_io_radio` — radio front-end and orchestrator for RTL-SDR (USB), RTL-TCP, SoapySDR, and native Airspy backends; provides
    constellation/eye/spectrum snapshots, optional bias-tee (RTL path), and auto-PPM hooks
    - Built when `DSD_HAS_RADIO` is true (RTL, Soapy, or Airspy available, or the pipeline explicitly forced on); otherwise provided as an INTERFACE stub target
  - `dsd-neo_io_audio` — network audio/input backends: UDP PCM16LE input, TCP PCM16LE input, UDP audio output helpers,
    and M17 UDP helpers
  - `dsd-neo_io_udp_control` — UDP retune control server (used by the RTL-SDR/FM helpers)
  - `dsd-neo_io_control` — rigctl/serial control interfaces

Key public headers:

- RTL stream C API: `include/dsd-neo/io/rtl_stream_c.h`
- RTL C++ orchestrator: `include/dsd-neo/io/rtl_stream.h` (class `RtlSdrOrchestrator`)
- Native Airspy adapter: `src/io/radio/airspy_source.cpp` owns SDK calls and USB lifecycle;
  `rtl_device.cpp` connects its CF32 callbacks to the shared generation-checked input ring.
  Value-only settings and identity/rate snapshots use `core/airspy_config.h`.
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

- Analog receive path (`rtl_demod_config.cpp`, `rtl_sdr_fm.cpp`; audit in `docs/rtl-demod-pipeline-audit.md`):
  - Stream start configures the analog channel from the options and validates it once the rate chain is final
    (`rtl_demod_finalize_analog_channel()`), including a rate the device forced. An explicit width the rate cannot
    realize, an explicit width with `DSD_NEO_CHANNEL_LPF=0`, an explicit width on an IQ replay whose sidecar decimates
    after the demodulator (`post_downsample` above 1, `rtl_demod_check_analog_post_decimation()`), or AM (no front-end
    AM demodulator yet) fails the start with the validator's text. The unset NFM default never fails: it keeps the
    `rate_in >= 20000` / `DSD_NEO_CHANNEL_LPF` enable rule and falls back to the legacy WIDE design where the rate
    cannot fit 16 kHz, published as DSP-limited at the width that plan passes.
  - The monitor's legacy `low_pass_real()` stage (`rate_in` to `rate_out2`) passes audio through: a live open sets both
    to the DSP bandwidth, and IQ replay (`controller_apply_replay_settings()`) sets `rate_out2` to the `rate_in` it
    takes from the capture, so only the rational resampler converts `rate_out` to the output rate. Test:
    `IO_RTL_ANALOG_OPEN` (a 78,125 Hz capture).
  - De-emphasis and audio-LPF settings are stored in `demod_state` (`deemph_tau_us`, `audio_lpf_cutoff_hz`) and their
    coefficients recomputed every time the rate chain is finalized (`rtl_demod_refresh_audio_coefficients()`).
  - A retune on `AUDIO_MONITOR` (the M17 encoder's monitor stream included) returns the de-emphasis, DC, audio-LPF
    and squelch-envelope state and the channel, half-band and resampler histories to fresh-open values. A retune that
    leaves the stream on another demod rate resolves the analog channel again for that rate
    (`rtl_demod_refresh_analog_channel_for_rate()`, from `demod_state::analog_width_request_hz`): the unset default
    moves between 16 kHz and the legacy WIDE design. An explicit width the new rate cannot realize is never clamped or
    run without its channel filter: `controller_refuse_retune_for_analog_width()` refuses the retune once the device is
    programmed and before it finalizes (both reconfigure paths), logs the validator's text once per kind, width and
    rate, puts the device back on the capture frequency and rate it had (`CaptureSettingsSnapshot`, which keeps the
    device-forced flag too), and finalizes on the centre it left without the retune's profile, so the tune fails
    (`controller_apply_reconfigure()` reports the refusal, and the manual retune completes as failed even when the
    centre it kept is its target). A retune profile for the target that switches to the digital family, or to an
    analog width the new rate fits, is not refused for the monitor's width. A device that refuses the capture
    frequency or rate it is put back on stops the stream (`controller_stop_for_unrestored_capture()`: logged,
    `DSD_INPUT_FAILURE_DEVICE` with the device's return code, exit flag), since it may still run the retune's capture
    while the stream finalizes on the one it kept. A device that still reports a rate the
    width cannot run at after it was put back stops the stream (`controller_refresh_analog_channel_for_rate()`:
    logged, `DSD_INPUT_FAILURE_CONFIGURATION`, exit flag), as a start at that rate fails. Only while the monitor
    output runs on the analog channel: CQPSK toggled on under `-fA`, or a typed digital scan row's profile, keeps its
    own profile filter across the rate change.
  - The width-driven filter and the published analog profile follow `dsd_demod_analog_monitor_active()` (analog
    family, `AUDIO_MONITOR` output, CQPSK off, the analog WIDE channel profile), so CQPSK toggled on under `-fA` keeps
    its P25 CQPSK profile filter, and a typed digital scan row's symbol profile on an analog session keeps the monitor
    output but filters with the row's channel profile and publishes no analog profile. An IQ replay that decimates
    after the demodulator (`post_downsample` above 1) runs the channel filter at that multiple of the rate it was
    designed for, so its analog width is published as DSP-limited, scaled by `post_downsample`.
  - While the pipeline runs, `demod_state` is written by the demod thread, or by a thread holding the reconfigure or
    family-switch gate while the demod thread is parked (a retune's finalize on the controller thread, a gated CQPSK
    toggle). Receive-family requests are queued and applied by the demod thread between blocks; a retune profile's
    family fields apply with the rest of the retune under the reconfigure gate, as its symbol profile and CQPSK toggle
    always have, and an analog one applies no symbol profile, CQPSK toggle or timing queued for the same target. An
    analog width is checked again against the demod rate it lands on, both a live request when the demod thread
    consumes it and a retune profile when the retune lands (a retune can move the rate after the request was checked
    against the published one); a width that rate cannot realize is refused (logged once per kind, width and rate) and
    the front end keeps its receive profile. That includes a live request that moves the stream onto the analog
    monitor output, whose caller has already put the decoder on Analog (after `rtl_stream_check_analog_profile()` held
    it to the published rate, or as the session's configured family): a retune that moved the rate since gets it
    refused, and the front end stays where it was rather than run the width without its channel filter; the log is
    all the decoder hears of it. The unset NFM default is never refused for its rate. A digital family
    request leaves the analog family whenever the stream
    runs it (`demod_state::analog_family`, published as `rtl_stream_analog_family_active()`), including after a symbol
    profile applied on its own (a typed digital scan row under `-fA`, a CQPSK toggle) has moved the front end off the
    analog monitor: such a profile never leaves the family, and the decoder asks for the digital family only when its
    configured mode is digital. A family request drops any demod profile queued before it (a CQPSK toggle drained in
    the same pass of the command queue), and a digital family request the demod thread finds with no symbol profile
    queued while the stream runs the analog family stays queued until the profile arrives, so a switch to digital
    requested as two calls (`svc_publish_symbol_profile()`, the channel-scan leave) always lands on its own profile.
    The stream records the
    family each switch lands on (`RtlSdrInternals::rx_family_switch`): its options are the orchestrator's copy from
    before the open, which a decoder-side mode change never reaches, so after a switch that record, not the options,
    decides whether a symbol profile without CQPSK runs the FSK discriminator or monitor audio. For the same reason,
    once a switch has moved the stream onto the digital family, the FSK channel profile a symbol profile without one
    of its own lands on (the DSP menu's CQPSK toggle turning CQPSK off) comes from the digital modes the decoder
    noted (`rtl_stream_set_digital_decode_modes()`, from `svc_publish_symbol_profile()` with the configured options,
    also for a mode picked under a scan row, never a running row's constraint) rather than from the options, as an
    open with those modes picks it; a stream still on the family it opened on, or with no note since its open (the
    open drops it), keeps picking from its options. Entering or leaving
    the analog family re-applies that family's fresh-open defaults (`rtl_demod_enter_analog_family()`/
    `_digital_family()`), restarts the carrier and timing loops (Costas, band-edge FLL, Gardner TED) and zeroes the
    I/Q DC and balance estimates, the squelch dwell toward a multi-frequency hop and a replay's post-demod decimator as
    an open does, clears the output ring and bumps
    the output generation; a width-only change redesigns the filter from empty histories. A switch to digital also
    keeps the open's floor of two samples per symbol for the TED, which the symbol-profile setter it applies does not
    (ProVoice at a 12 kHz DSP rate). The analog family never runs
    CQPSK: a `-fA` open demodulates FM whatever `DSD_NEO_CQPSK` or the modulation say, as a switch to analog does. The
    CQPSK family after a switch to digital follows the symbol profile requested with it unless `DSD_NEO_CQPSK` is set,
    which decides it as it does at stream open, the channel filter following the family it lands on and the open's
    enable rule (an FSK landing keeps the WIDE profile while the channel filter is off: `DSD_NEO_CHANNEL_LPF=0`, or
    below a 20 kHz DSP rate by default) (`rtl_demod_open_cqpsk_request()`, `rtl_demod_open_channel_profile()`, also
    behind `rtl_stream_output_rate_for_family()`), and the digital resampler
    and output rate are decided for that profile when the switch is made (`rtl_demod_enter_digital_family()` takes its
    CQPSK flag and symbol rate), so a forced rate lands where an open of the profile would. Tests:
    `IO_RTL_ANALOG_FAMILY_SWITCH` (digital → analog → digital, and a `-fA` start switched to digital, each equal to a
    fresh open, loop state, monitor audio state, I/Q corrections and filter histories included, also under
    `DSD_NEO_CQPSK=0` and `=1` and with the channel filter off (`DSD_NEO_CHANNEL_LPF=0`, a 12 kHz DSP rate), with the
    stream keeping the options snapshot it opened with, for P25
    C4FM/CQPSK, DMR, NXDN48, dPMR and ProVoice (also at a 12 kHz DSP rate) at unforced and forced rates and D-STAR,
    the DSP menu's CQPSK toggle made twice after the switch landing where it lands on a fresh open, including
    from a `-fA` session a CQPSK toggle or a
    typed digital row had moved off the monitor output, or with a CQPSK toggle still queued when the digital mode is
    picked; a typed digital row under `-fA` and on a DMR session switched
    to analog; width-only changes; requests with no stream; live requests and
    retune profiles refused against a running stream's rate and a replay's `post_downsample`, and a live request
    refused at the rate a retune moved the stream to before it was consumed, a DMR session's switch onto the monitor
    included (also one requested after a retune moved the rate its check accepted), with the unset default switching
    at a rate that cannot fit 16 kHz instead; a demod
    block boundary
    between the family request and its symbol profile; a switch whose ring clear meets a decoder read between its
    copy and its tail store, or a read that loaded the clear's first generation bump and reached the ring before the
    clear; noted digital modes deciding only after a switch to digital and dropped by a new open; the baselines run
    the same demod configuration functions as
    `dsd_rtl_stream_open()`), `IO_RTL_ANALOG_OPEN` (the start-time check against the rate an IQ replay delivers, and a
    `-fA` replay switched to DMR and back through the stream API), plus
    `IO_RTL_DEMOD_CONFIG` and `IO_RTL_RETUNE_PREPARE`.
  - `output_state::rate` (`<dsd-neo/runtime/ring.h>`) is atomic: the controller and demod threads write it and the
    decoder and UI threads read it through `dsd_rtl_stream_output_rate()`.
  - The output ring is cleared from the demod thread (family switch, reacquire) and the controller (reconfigure gate)
    while the decoder reads it. The readers (`ring_read_available()`, `ring_read_batch()`) hold `ready_m` from their
    tail snapshot to their tail store, and `rtl_stream_clear_output_ring()` clears under it, so a read in flight
    cannot store its old tail over the cleared indices (which reads as a ring full of the old stream's samples).
    The clear bumps the output generation before it takes `ready_m`, and again under it once the ring is empty: a
    read that loaded the first bump can still reach the ring before the clear and take samples the clear drops, and
    the second bump keeps the stream from running on the generation that read carried them under.
    Tests: `RUNTIME_RINGS`, `IO_RTL_ANALOG_FAMILY_SWITCH`.
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
- `nxdn_keystream.c` / `<dsd-neo/crypto/nxdn_keystream.h>` share the NXDN TS 1-D scrambler and AES IV
  expansion between protocol data handling and MBE playback.
- `dmr_mi.c` / `<dsd-neo/crypto/dmr_keystream.h>` own RC4 MI advancement and the pure DMR AES
  `dmr_aes_expand_iv()` helper. The live protocol wrapper `LFSR128d()` retains slot-state updates and logging;
  MBE playback uses the same expansion without those side effects.
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

- After the session's first decoder redraw, `UiController` refreshes live metrics on every timer tick so scan
  countdowns and the sync-loss hold continue aging if input stalls. History, network and policy models still
  refresh on decoder redraws; session lifecycle clears live metrics and prevents stale snapshots from restoring them.
- `dsd_app_lead_slot()` in `app_control/call_view.c` selects the earliest exact start among identified active
  calls for the Monitor hero, its quality row and the Android notification. Later calls on the other slot do not
  displace it. Exact ties prefer the lower slot; when no call is active, the lowest ended slot wins. An earlier
  call gaining identity late takes the headline using its original start.
- QML plus C++ view-models (metrics, call history + per-view filters, saved systems, imported CSV files, app
  preferences, command bridge) that poll app-control on a timer; used by the Android app today and intended as the
  shared basis for a desktop GUI. `imported_files_model.{h,cpp}` is the library behind the CSV pickers: it copies
  picked documents into durable app storage through `DecoderHost::importDocument()` and dry-run validates them via
  `<dsd-neo/core/csv_validate.h>` (`src/core/file/dsd_import.c`, `src/core/file/p25_bandplan_csv.c`, and
  `src/core/file/source_alias_csv.c`) for row-count feedback. Kinds are `chan`, `group`, `keysDec`, `keysHex`,
  `p25Bandplan`, and `src` (source radio ID names), with `src` appended after `p25Bandplan`.
  `radio_reference_model.{h,cpp}` plus `qml/RadioReferenceScreen.qml` are the RadioReference import: the model drives
  the runtime client, previews what an import would produce, and writes the generated CSVs into that same library with
  provenance, while the add-system wizard stays the single writer of a saved system. See
  `docs/radioreference-import.md`. Nearby import uses `DecoderHost` request IDs and
  generation retirement; Android's `LocationSupport.kt` owns coarse permission, fix
  acquisition and geocoding, with `decoder_host_android` polling terminal results.
  `AppPrefs::setLocationFix` retains the private fix and accuracy for 24 hours.
- `talkgroup_list_model.{h,cpp}` polls the effective core policy's source-context/generation pair after call-history
  ingestion, merging listed rows with uncovered talkgroups heard this session. `talkgroup_filter_model.{h,cpp}`
  filters by category and name/ID for `qml/TalkgroupsScreen.qml`, opened by the monitor's **TG list** action.
  `CommandBridge` submits `TG_LISTEN_SET`/`TG_LISTEN_SET_ALL` through app-control; only the decoder thread mutates
  policy and atomically rewrites a configured group file. Scan-row lists remain session-only. **Lock out** shares
  this mutation path, preserving labels; with `persist_tg_lockouts` off it becomes **Avoid TG**, adding a session
  avoid without changing rows. **Skip** submits `DSD_APP_CMD_SKIP_SLOT`, arms the core call-skip ledger and leaves
  the call without changing rows or saving. P25 group skips refresh while the receiver sees the call and expire
  at `DSD_TG_CALL_SKIP_QUIET_S` of quiet or `DSD_TG_CALL_SKIP_MAX_AGE_S` from the press; private and non-P25 skips
  use the fixed quiet-window duration from the press. ProVoice has no skip. The active policy context retains
  unexpired skips across scan visits; **Clear temporary avoids and call skips — current list** clears them with
  session avoids. Runtime's `dsd_rr_talkgroups_apply_categories()` supplies category names to both RadioReference
  frontends before CSV generation.
- `p25_network_model.{h,cpp}` copies four bounded lists through the frontend-neutral
  `app_control/p25_network.h` facade using `UiController::tick`'s held snapshot.
  `qml/NetworkSheet.qml` activates refresh only while visible; session edges and
  scan-target changes clear it through `clearLiveModels()`. The C facade sorts
  recent-first, resolves candidate flags from the snapshot's deep-copied extension,
  formats CFVA through P25, and filters expired patches without mutating the snapshot.
- `diagnostics_log.{h,cpp}` owns the process ring, the sole redaction/capture stage,
  and the asynchronous bounded tail writer. `DiagnosticsLogModel` refreshes before
  the redraw consume in `UiController::tick`; it persists across decoder session
  transitions. `qml/DiagnosticsScreen.qml` provides pause, copy, clear and host share.
  Runtime's install-once log tap and Android's stderr/host paths feed `submit()`;
  platform sharing remains behind `DecoderHost` in Android's diagnostics provider.
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
  `dsd_tg_policy_restore()` preserves active calls across temporary suspend/resume; install resets them for target
  transitions. The CSV importer loads standalone stores through core-private helpers without a decoder-state allocation.
  Slot 5 holds the saved global group context and suspend/resume ownership. Clearing metadata restores it first;
  moving metadata unwinds both source and destination scopes before transferring the row definitions.
- `dsd_scan_key_change_prepare()` allocates the incoming key copy and any needed baseline before a conventional tune;
  commit consumes the change without allocation. Engine checks the map generation and key epoch before committing,
  restaging the tune (and re-preparing against the current globals) when the keyring changed in the window. Both
  coordinators reserve every scope and key allocation before saving the outgoing snapshot, so a failed preparation
  leaves the receiver and snapshot untouched. Conventional retries keep the frame gate closed until a row commits,
  including when a later tune is deferred or rejected; the scanner can still advance to a working row. Separately,
  trunk-scan failures re-arm dwell or retry timers so memory pressure cannot cause a retry on every tick.
- `dsd_channel_modes_present()` is true for declared modes and for option-bearing profiles alike, so option-only
  channel maps run the typed scanner. `dsd_scan_settings_equal()` compares acquisition settings only; the row-scoped
  options (forcing, CRC, mutes, voice gate, group file) are folded through `dsd_scan_mode_resume()` without
  resetting acquisition. Conventional trunk-scan targets take their voice-gate hold/qualify from the row profile.
  `DSD_SCAN_OPT_MAX_VISIT` is the one **scan-timing** row option accepted on trunked types as well, and its
  `scan_max_visit_ms` travels in the same row-option block, outside `dsd_scan_settings_equal()`, so a row cap never
  restages a tune.
- Long DMR base-station decode loops consult the runtime frame hook `scan_visit_should_yield` at burst boundaries.
  It maintains the owning scanner's visit clock and reports expiry without tuning. The decoder finishes its cleanup
  and releases the SM guard before the engine advances, so cleanup cannot overwrite the next target's state.
- App-control scopes force/CRC/voice configuration commands and group imports, while live row policy mutations stay
  with the active context. Configuration export reads saved group paths and voice settings from the configured scope.
- `DSD_SCAN_OPT_SQUELCH` (`--squelch-db`, issue #521) is the one row option with hardware behind it. The parser
  accepts whole dB `-100..0` (0 = off) on every class and target type (`ANY_MODES`, not the `DIGITAL_MODES` of the
  protocol switches); its spec sets `signed_numeric`, the single exception to "a following `-` token is a missing
  value", applied in `option_argument()` so the parser and `dsd_scan_options_visit_files()` agree. The level lives in
  `dsd_scan_settings::rtl_squelch_level` (a double, first in the struct, compared with a tolerance) beside the other
  row options, outside `dsd_scan_settings_equal()`. It reaches the demod through the runtime metrics hook
  `set_channel_squelch`, only for `AUDIO_IN_RTL`, and at most once per row change: `dsd_scan_mode_enter` pushes
  nothing and records the level the demod held; the `dsd_scan_mode_options` call that completes the row pushes when
  the level in force differs from it (a `-60 -> -60` move pushes nothing, and the configured default is never pushed
  in between). Every enter caller must install the row's options (NULL for none) right after it. `options` on its
  own and `leave` push on a change; `dsd_scan_mode_resume`, and a `leave` that finds the scope suspended (a command
  stopping the scanner), always push, because a command run while suspended (`CONFIG_APPLY`) may have pushed the
  configured default itself, or the demod may still hold the row's. After an enter that found the scope suspended,
  the `options` call that completes the row pushes unconditionally for the same reason. `dsd_scan_mode_prepare` and
  `scan_scope_apply` never push.
  `RTL_SET_SQL_DB` is not a scoped command: `svc_rtl_set_sql_db()` edits the configured default through
  `dsd_scan_mode_set_configured_squelch()`, which touches no acquisition setting (so a squelch nudge can never read
  as a decoder change that ends the call) and leaves `dsd_opts` and the demod on the row's value while a row
  overrides it. `AIRSPY_SET` and `RTL_ENABLE_INPUT`/`AIRSPY_ENABLE_INPUT` rewrite no squelch and stay unscoped, so
  a stream they reopen starts on the row's acquisition and threshold, the ones in force. Saves read `rtl_sql` from
  `dsd_scan_mode_configured_view()`. On non-radio inputs nothing gates digital acquisition; the level only reaches
  the unsynced analog input monitor in `dsd_symbol.c` (`-8`, with audio output on) and the carrier activity it
  stamps. Channel and trunk scan start warn once per affected row or target. Tests: `RUNTIME_SCAN_MODE`,
  `ENGINE_CHANNEL_SCAN`, `ENGINE_TRUNK_SCAN` (levels and pushes per row and target), `ENGINE_SCAN_SQUELCH_GATE` and
  `ENGINE_CHANNEL_SCAN_SQUELCH_GATE` (trunk-scan targets and `-Y` rows through the production frame-sync power gate),
  `APP_COMMAND_QUEUE`, and the `DECODE_IQ_SCAN_NXDN48_SQUELCH_*` replays (a row through the real demod gate).
- Adding a row option: add the `DSD_SCAN_OPT_*` bit (reserved values only), a `dsd_scan_option_values` field and a
  `specifications[]` row with its setter in `runtime/scan_options.c` (use `ANY_MODES` only for options that mean the
  same on every class); add a `scan_option_appliers[]` row in `runtime/scan_mode.c`; if it lands in `dsd_opts`, add
  the field to the leading row block of `dsd_scan_settings` and to `dsd_scan_settings_capture()` (which copies each
  field by name, so a missed field captures a zero baseline that leaving the row would restore),
  `scan_settings_restore_row_opts()` and `scan_settings_copy_row_opts()` (not the equality list, unless it changes
  acquisition). If the value also lives in hardware, push it through a runtime hook from the scope's entry points
  (the `options` call that completes a row, `leave`, and always after `resume`), never from `enter` alone or
  `prepare`. Saves and editors read the configured view, and an editor that must not disturb acquisition edits the
  configured baseline directly, as `dsd_scan_mode_set_configured_squelch()` does, rather than suspending the scope.
  Frontends get their decisions and text from an app_control view. For previews, add the field to
  `dsd_csv_channel_profile` (filled by `csv_describe_channel_profile()`) and `dsd_app_scan_csv_target` (filled by
  `trunk_scan_document.cpp`'s `describe()`), publish it from `ImportedFilesModel`'s `appendChannelProfile()` and
  `appendTargetPreview()`, and show it in the `ImportsScreen.qml` channel review and the `ScanListScreen.qml`
  target preview. Then `docs/csv-formats.md`, and the examples the CSV import tests parse.

### Android foundation integration contracts

Shared command IDs 592/593/594 reserve talkgroup row set/remove/export, using
`policy_context` and `policy_generation` to reject edits from a stale list. The
row `fields` mask selects listen/priority/preempt/name/tags. IDs 652/653 reserve a
typed, bounded direct key and an int32 force-key mode. These five handlers currently
report “not implemented”; force-key is registered as a configured scan-mode setting.
Queue storage and drain temporaries are securely erased after use and on discard,
including eviction, coalescing and rejection. Submitters own and must erase their
original secret payloads.

Saved-system UUIDs are persisted on migration and stable across updates. `rowForUid`
and `getByUid` are the lookup boundary for work that can outlive a row index. The
optional direct-key/site fields are private saved configuration, not diagnostics.
`DecoderHost` owns the platform location/share/device interfaces; `UiController::tick`
remains the single snapshot consumer. Its existing same-thread history flush before
a new session label is the documented exception, not permission for another reader.

WP0 owns integration edits to `qt_ui.cpp`, `ui_controller.*`, `decoder_host_android.*`,
`UsbSourceManager.kt`, `app_command_queue.c`, `command_bridge.*`, `Main.qml`,
`qml_test_context.h`, both Qt/test CMake lists and `android-ci.yaml`. Later packages
supply focused wiring patches for serial application in orchestrator order. New
live models join `clearLiveModels()` for lifecycle and trunk-scan target edges.
Context registration placeholders in `qt_ui.cpp` keep those ownership decisions
in one place. Every new UI_QT target also belongs in Android CI's explicit build list.

### Direct key application

`dsd_key_apply_direct` in `src/core/util/key_set.c` owns strict direct-key parsing
and overlay/replace semantics. `dsd_key_apply_mute_policy` reconciles startup and
live mute behavior; `dsd_key_apply_force` maps the three configured force modes.
The CLI and `KEY_DIRECT_SET`/`FORCE_KEY_SET` handlers share these helpers.
`dsd_scan_keys_apply_direct` validates a direct edit before changing the global
baseline without allocating or swapping live keys. An active row retains its
signalled key IDs and the scalar/AES state activated since row entry. `CORE_KEY_DIRECT` exercises the
helper, real CLI parser, real command queue, and keyed/unkeyed rotation together.
Qt's `session_args.cpp` validates saved string key types and emits discrete argv;
`SessionArgsBuilder::build` returns only non-secret validation metadata. Its `start` operation resolves retained
keys and sends the arguments directly to `DecoderHost` in C++, returning only validation and acceptance status.

### Qt QML editing convention

Preserve each file's existing QML formatting and use focused manual edits. Do not
run whole-file `qmlformat` during a functional change; no automatic QML formatting
settings are prescribed. MonitorScreen's existing expanded style is retained.
Text sizes use `Theme.fontSize(pixels)`, with the platform scale bounded to 1–1.6.

#### Qt scan-list start path

`scan_lists_model.{h,cpp}` persists list/entry UIDs and saved-system references
through `json_store` (`scan_lists.json`). `scan_list_targets.{h,cpp}` is a pure
QtCore generator with no filesystem or engine dependency. `scan_list_starter`
checks imported paths, atomically writes the private CSV, validates through
`app_control/trunk_scan_validate.h`, and delegates argv assembly to
`session_args.cpp`. `Main.qml` shares the existing USB permission gate and extends
the single `sessionInitialized` recency handler. `metrics_model` copies target
identity from the tick's held snapshot; no extra snapshot reader is introduced.

`UI_QT_SCAN_LIST_TARGETS` covers preservation/rejection and option screening;
`UI_QT_SCAN_LIST_ROUNDTRIP` exercises persistence and the real facade.
`ENGINE_TRUNK_SCAN_SCAN_LIST` sends a generated three-target CSV through the real
coordinator, group-policy and key ownership code, replacing only tuning side
effects. It verifies policy/keys on rotation and baseline restoration on shutdown.
The scan-list, Home and Monitor QML cases run in `UI_QT_QML_CALL_LISTS`.
