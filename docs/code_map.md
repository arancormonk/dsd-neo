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
    `include/dsd-neo/engine/scan_voice_gate.h`; its probe returns separate active and retained last-media clocks so a
    protocol terminator cannot erase the scanner's tail anchor (tests: `ENGINE_SCAN_VOICE_GATE`,
    `ENGINE_NO_CARRIER_RESET`, `ENGINE_TRUNK_SCAN`). The same file owns the scan-timing publication
    (`dsd_scan_timing_clear()` / `dsd_scan_timing_publish()`) and the -Y timing tick `dsd_engine_scan_y_timing_tick()`,
    which stamps `dsd_state::scan_timing` with the stay reason and the absolute monotonic deadline of the window that is
    running (issue #508); the deadline it publishes is the same instant `dsd_scan_voice_gate_should_step()` flips, so
    the readout cannot drift from the rotation it describes. It also owns the analog carrier probe both scanners hold
    analog rows on, `dsd_scan_analog_carrier_open()` (issue #526): the received-tone tap's carrier held to the channel
    on air (`dsd_analog_rx_carrier_open_now()`) while the analog FM monitor runs, never on a stale publication, a
    flagged digital carrier or a trunking-owned channel, and independent of audio output. The -Y voice gate never owns
    an analog row (`scan_voice_gate_enabled()` is false under the analog family), and the -Y timing tick reports
    `DSD_SCAN_STAY_CARRIER` for the hangtime window while that probe is open
  - Stepped slicer threshold refresh after each getFrameSync() return: `src/engine/slicer_thresholds.c` behind
    `include/dsd-neo/engine/slicer_thresholds.h` (test: `ENGINE_SLICER_THRESHOLDS`)
  - Installs runtime hook tables used by DSP/frame-sync code
    (`src/engine/frame_sync_hooks_install.c`, `include/dsd-neo/runtime/frame_sync_hooks.h`)
  - Input setup refuses an explicit analog channel width the DSP bandwidth of an RTL-SDR or rtl_tcp input cannot
    filter (`dsd_engine_setup_check_analog_width()`, issue #525), from the bandwidth the input spec sets and before an
    RTL-SDR input looks for its device, so the refusal depends on no dongle; other radio inputs are held to the rate
    they deliver at stream start. Tests: `RUNTIME_ANALOG_WIDTH_RATE_REFUSED` and `_RTL` (`tests/cmake/RunCliSmoke.cmake`:
    the message, a non-zero exit, and for `rtl` no device enumeration before it)
- Build files: `src/engine/CMakeLists.txt`

Key public headers:

- Decode runner and lifecycle: `include/dsd-neo/engine/engine.h`, `include/dsd-neo/engine/frame_processing.h`
- Protocol/frame dispatch: `include/dsd-neo/engine/protocol_dispatch.h`
- Trunk tuning policy: `include/dsd-neo/engine/trunk_tuning.h`
- Single-tuner trunk scan: `include/dsd-neo/engine/trunk_scan.h`

### Single-Tuner Trunk Scan

`src/engine/trunk_scan.c` owns the coordinator that rotates one retunable receiver across the explicit targets of a
target-list CSV (P25 trunk/conventional, DMR trunk/conventional, NXDN96/NXDN48 trunk/conventional, and analog
`nfm-conventional`). Operator-facing behavior, the CSV columns, and the CLI/config options live in `docs/trunk-scan.md`;
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
- Analog targets (`DSD_TRUNK_SCAN_TARGET_NFM_CONVENTIONAL`, issue #526): every exhaustive type switch classifies the
  type (conventional, no GFSK or P25 symbol rate, no conventional family a protocol report can claim), and
  `trunk_scan_type_is_analog()` gates the rest. The type column is parsed from the `k_trunk_scan_types[]` table, which
  also lists the accepted spellings in the invalid-type diagnostic. An analog target's retune passes no timing
  (`trunk_scan_retune_active()`), so no zero symbol rate reaches `dsd_opts_compute_sps_rate()`; its tick refreshes
  `last_allowed_activity_m` from `dsd_scan_analog_carrier_open()` instead of the voice-media hold
  (`trunk_scan_refresh_activity()`), and its stay reason reads `CARRIER` while the carrier is open, then
  `ACTIVITY_HOLD` for the tail. The parser refuses key columns, `modulation`, `chan_csv` and `p25_bandplan_csv` on
  it, and the live decryption command refuses it.
- Configured-width use across both scanners: `dsd_engine_scan_runs_configured_nfm_width()` answers whether the scan
  running now, the trunk-scan coordinator's target list or else the `-Y` channel map (whose rows
  `channel_rows_run_configured_nfm_width()` walks), has an analog row without a width of its own, which runs the
  configured NFM width whenever it comes on air. App-control holds that width to the DSP rate on any session while it
  does (see Per-channel decoder modes). It lives here rather than in `channel_scan.h` because the coordinator's list is
  the one it reads first, and the question is asked of whichever scanner owns the tuner.

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
- Classes: `dsd_scan_mode` runs INHERIT..M17 and the analog `DSD_SCAN_MODE_NFM` (9, issue #526), whose preset is the
  analog FM monitor (`DSDCFG_MODE_ANALOG`). `DSD_SCAN_MODE_LAST` is the one bound every range check uses (the option
  parser, `dsd_channel_mode_set()`), `dsd_scan_mode_is_analog()` the one analog predicate (key compatibility, the
  importer, trunk-scan target classes), and appending a class keeps stored values and `MODE_BIT()` masks stable.
  `dsd_scan_mode_alias_hint()` names the class to suggest for an alias (`fm`, `analog`, `wfm`, `nbfm`, `fm-conventional`
  -> `nfm`; none is accepted) and `dsd_scan_mode_names_list()` the accepted spellings for a diagnostic. The analog
  channel widths (`analog_nfm_bandwidth_hz`, `analog_am_bandwidth_hz`) are acquisition fields of `dsd_scan_settings`:
  captured, restored, compared by `dsd_scan_settings_equal()` for the analog family (so a width change restages a parked
  analog row, while a digital row, which runs its own channel profile, is not disturbed by a configured width edit) and
  restored again before a row's options install; a row width (`DSD_SCAN_OPT_BANDWIDTH`) lands there through its applier.
  `channel_scan.c` restages an outstanding row's tune when a configured width that tune carries changes
  (`channel_scan_configured_changed()`): an analog row's without a width of its own, whatever the configured family,
  and an untyped row's on the analog family. A typed digital row, or an nfm row with its own width, commits as staged.
  `dsd_scan_mode_prepare()` takes the row's option values (NULL = none) so the prepared settings a scanner tunes with
  already carry the row width; its callers are `channel_scan.c` and the `scan_mode_replay` / `analog_replay` hosts.
  A row's symbol timing is computed for the output rate its tune lands on, `dsd_scan_mode_symbol_timing_rate_hz()`:
  the live rate, except while the RTL front end still runs the analog family after an analog row and the configured
  mode is digital (`dsd_scan_mode_configured_digital()`), when a digital row's tune switches the family and the rate
  is the digital family's (`output_rate_for_family`), not the monitor's resampled audio rate. The -Y scope timing and
  the trunk-scan target timing (`trunk_scan_p25_cc_sps()`, `trunk_scan_gfsk_sps()`) read it, and `trunk_tuning.c`
  decides the family switch and the GFSK chain's TED by the same predicate, so the decoder and the TED the retune
  profile carries are timed for one family. The configured baseline follows the same rule: a scoped command (a
  decode-mode or modulation change, a config apply) times the configured decoder at the live rate, the monitor's
  while an analog row is on air, so `dsd_scan_mode_resume()` retimes it for the digital family before saving it
  (`scan_configured_retime()`) whenever the front end still runs the analog family and the configured mode is
  digital; an untyped row's tune and the leave then land the digital family on the timing it runs at.
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
  `dsd_scan_mode_set_configured_squelch()` and `dsd_scan_mode_set_configured_nfm_bandwidth()` edit the configured
  default without suspending (see Scoped scan options), and `dsd_scan_mode_configured_analog_width()` is the one reader
  of a configured channel width (the configured view while a scope is live, `dsd_opts` otherwise), which the scanners,
  app-control's width services and view, the config save and the terminal menu share.
- Engine `channel_scan.c` (extension slot 7) stages typed `-Y` entries for automatic, manual, and avoid stepping through
  tracked tuning. It commits mode/keys only after success and retains generation protection across pending requests.
  Configuration edits retry pending tunes on a later service pass; live output-rate changes do not trigger another tune.
  Failed rows advance to the next candidate; a later successful tune recovers any gate held by a partial backend failure.
  `dsd_engine_channel_scan_waiting()` only inspects ownership; `dsd_engine_channel_scan_service_sync()` services pending
  work and invalidates sync gathered before the transaction. Pending rows defer no-carrier call finalization until commit.
  `dsd_engine_reset_no_carrier_state()` shares decoder cleanup without recursively stepping or changing tuner ownership.
  `trunk_scan.c` selects the same classes from target types while retaining target snapshots and modulation/gain ownership.
  Each row commit (and trunk-scan target switch) opens the sink the row plays through, `dsd_engine_scan_ensure_output()`
  (analog or digital, idempotent; it, the two warnings below, the width rule `dsd_engine_scan_width_refused()` and its
  quiet per-row form `dsd_engine_scan_analog_width_skipped()`, and the skipped-row count `dsd_engine_scan_skipped` /
  `dsd_engine_scan_note_skipped_rows()` are private to the engine, `src/engine/scan_analog_internal.h`), and scan start
  logs what an analog row owes the operator on two schedules. An open squelch, which lets noise hold the row until the
  visit cap or the operator moves on, `dsd_engine_scan_warn_analog_squelch()`, is said once per map (-Y) or list (trunk
  scan). A width on audio input, or
  one the front end refuses, `dsd_engine_scan_warn_analog_width()`, is said once per map or list and DSP rate. The front
  end refuses a width the DSP rate (`dsd_engine_scan_dsp_rate_hz()`, the rate the RTL stream holds analog requests to,
  `rtl_stream_get_request_rate_hz()`) cannot filter, worded with the fix the input allows as the stream start's refusal
  is (`dsd_analog_width_check_at()`: an RTL DSP bandwidth, a wider DSP bandwidth or a narrower width on a SoapySDR or
  Airspy device, a narrower width on an I/Q replay), and any explicit width while `DSD_NEO_CHANNEL_LPF=0` turns the
  channel filter off (`dsd_engine_scan_width_refused()`). An RTL stream that has published no rate defers the check, and
  a changed rate repeats it, because the tune refuses such a row without calling into the stream (whose refusal log a
  valid row's request would re-arm at every rotation) and the trunk-scan coordinator logs no retune failure for it
  (`trunk_scan_analog_width_refused()`, which holds the width in force once the target's options apply). Each check that
  finds rows skipped at every visit also puts the first of them, and how many more, on the status line every frontend
  shows (`dsd_engine_scan_note_skipped_rows()`: `ui_msg`, as the input-level advisories are), so Android, which has no
  log view, learns why a row is never on air. A row without a width of its own is held with the configured NFM width it
  runs (`dsd_scan_mode_configured_analog_width()`), and a changed configured width names those rows again, while the
  status line still counts the rows whose own width is skipped. A placeholder `-Y` row (frequency 0) is never tuned, so
  none of these checks names or counts it. While the scan has such a row or target
  (`dsd_engine_scan_runs_configured_nfm_width()`, public in `trunk_scan.h`), app-control holds the configured width to
  the rate on any session, as under -fA: the width command, a config apply, `RTL_SET_BW` and Input > Switch source
  refuse a width or bandwidth that cannot run it, whichever row is on air. What is left to warn about is a list loaded
  over such a width, or a rate a device forced. A trunk-scan retune
  in flight on an analog target keeps the width it queued; a width edit made meanwhile reaches the front end as a live
  request the landing retune can land over, so the coordinator requests the width in force again once the retune lands
  wherever it differs (`trunk_scan_reapply_analog_width()`), as the `-Y` scanner restages such a tune. A channel map
  loaded at runtime is held to the DSP rate as it loads: `svc_channel_map_refused_rows()` asks
  `dsd_engine_channel_scan_refused_rows()` (public in `channel_scan.h`, read-only) for the rows the front end would
  refuse at the running stream's rate, or on an RTL-SDR or rtl_tcp input with none running at the rate its DSP bandwidth
  sets, and the import command's toast names the first and counts the rest. A map `-C` or `[trunking] chan_csv` loads at
  startup is held to the rate at scan start, once the stream has published it. The map still loads, since the DSP
  bandwidth that fits it can be set afterwards, and a width the rate cannot fit stays a warning and a skipped row, never
  an import error: the rate a SoapySDR or Airspy device, or an I/Q replay, runs at is certain only once its stream runs,
  so their rows are held to it at scan start, and the Qt/Android review and target-list preview (dry-run loaders with no
  session) check the width's range only. The describe records carry each row's width (`bandwidth_hz`) for the Qt/Android
  review to show it (see Scoped scan options).
  The receive family a row runs on is queued with its tune in `trunk_tuning.c` (`dsd_engine_prepare_scan_profile()`,
  issue #526): an analog row attaches the analog family, demodulator and width to the retune profile
  (`rtl_stream_prepare_retune_analog_profile_for_target()`) with no symbol profile, and a width the front end refuses
  fails the tune before any backend moves, dropping only the retune profile queued for it; a digital row attaches the
  digital family ahead of its symbol profile only while the front end runs the analog family and the configured mode is
  digital (a typed digital row on an `-fA` session keeps the monitor output). A failed hop off the analog monitor
  re-requests no symbol profile over it. A live
  receive-family request accepted while a `-Y` row's retune is outstanding supersedes the family, and the symbol
  profile, that retune carries; it acts for the row still in scope (a width edit or a config apply republishing the
  outgoing row's receive profile), so the commit restages the row rather than run it on the outgoing row's family
  (`channel_scan_staged_stale()`, comparing `dsd_engine_scan_family_requests()`, the stream's
  `rtl_stream_live_family_request_count()`, with the count when the tune was queued). Only a retune that carries a
  family is superseded (`dsd_engine_scan_retune_attaches_family()`); one without lands its symbol profile whatever the
  requests, and its row commits as staged.
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
- State extension slots (`<dsd-neo/core/state_ext.h>`): engine owns 0, 1, 3 and 7; core 2, 4, 5, 8, 10 and 11;
  runtime 6; DSP 9 (`DSD_STATE_EXT_DSP_ANALOG_RX`, taken from the core range as runtime took 6 from the engine's), the
  analog receive working state. `CORE_STATE_EXT` pins slot 9.
- API note: `dsd_state::analog_rx` (`dsd_analog_rx_publication` in `<dsd-neo/core/state.h>`, issue #522) is the
  received-tone publication every frontend reads: int-only (`carrier_open`, `tone_kind`, `tone_state`,
  `ctcss_tenths_hz`, `dcs_code` and `dcs_inverted` reserved for #523, `gate` reserved for #527 and always OFF, a
  `generation` bumped by every reset, input switch and input-rate change, and `stale_after_ms`, the monotonic deadline
  past which the publication of an input that may pause (stdin, UDP, TCP, a live RTL-family radio stream) no longer
  describes the channel, 0 on inputs that never pause: files, Pulse and IQ replay).
  It rides the `vertex_ks_count..ui_msg` snapshot range beside `scan_timing`, pinned by a `_Static_assert` in
  `ui_snapshot.c`; no float, so the semgrep float-field list is unchanged. Only the DSP tap writes it. `tone_state`
  INACTIVE means nothing has been processed since the last reset, or detection is not running; it is not the
  "detection off" signal. Whether detection runs is `dsd_analog_tone_detection_active()` (runtime), which frontends
  and receive policy ask instead. UNAVAILABLE means detection is on but the input rate is one the front end cannot
  use; `carrier_open` is still kept there (issue #526).
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
    fits and the RTL DSP bandwidths that would fit (`dsd_analog_width_fitting_rtl_bandwidths()`, which the short
    refusals reuse; `dsd_analog_rtl_dsp_bw_is_selectable()` is the list of selectable DSP bandwidths that the services,
    config apply and validation share). `dsd_analog_width_check_at()` words a rate refusal for what sets the rate
    (`dsd_analog_rate_source`, fix text from `dsd_analog_width_rate_fix()`): the RTL DSP bandwidth, a SoapySDR or Airspy
    device (whose capture rate is halved down to the first rate at or above the DSP bandwidth: raise the DSP bandwidth or
    narrow the width) or an I/Q replay's capture (narrow the width); where the rate filters no width of the kind it
    never names narrowing, and for NFM names leaving the width unset. `dsd_analog_channel_lpf_off_check()` is the
    `DSD_NEO_CHANNEL_LPF=0` rule (an explicit width, or the AM default, cannot run), taking that state as an argument.
    It is pure integer arithmetic so callers need not link the DSP;
    `src/dsp/demod_pipeline.cpp` static-asserts its design constants against the ones here. Tests:
    `RUNTIME_ANALOG_CHANNEL`, `DSP_CHANNEL_FILTERS` (validator and design agree across a width/rate grid; the
    analog design gates on `dsd_analog_width_realizable()` itself).
  - The configured NFM width (issue #525): `--nfm-bandwidth-hz` (`src/runtime/cli/args.c`; `compact.c` consumes its
    value before getopt; a warning on PCM input) and the `[analog]` INI section (`has_analog`,
    `analog_nfm_bandwidth_hz`) parse through `dsd_analog_width_parse()` and never clamp: the loader warns and keeps the
    default, `--validate-config` reports an error, and also one when the width does not fit the DSP rate `rtl_bw_khz`
    gives an `rtl`/`rtltcp` input that startup builds with it (`rtl_freq` set) under `decode = "analog"`. A save writes
    the key only for an explicit width (0 is the default), but always the `[analog]` header: the loader marks a present
    section (`note_section_present()`) even with no key under it, so a config saved at the default loads back as the
    default over an explicit width. The saved width comes from `dsd_scan_mode_configured_view()`, since an nfm scan
    row's own `--nfm-bandwidth-hz` (issue #526) runs over `dsd_opts` while the row is on air.
    `[analog]` is the home of the analog keys, in the order `nfm_bandwidth_hz`, `am_bandwidth_hz`, `tone_filter`,
    `tone_list`. `dsd_user_config_radio_input_spec()` returns the radio input spec (`rtl`, `rtltcp`, `soapy` or
    `airspy`) an `[input]` builds without applying it, so a live config apply can tell whether it reopens the device and
    what then sets the rate. Tests: `RUNTIME_CLI_PARSE`, `CONFIG_VALIDATION`, `CONFIG_TEMPLATE`, `RUNTIME_CONFIG_USER`.
  - The RTL metrics hook table (`include/dsd-neo/runtime/rtl_stream_metrics_hooks.h`) also carries the receive-family
    request (`apply_analog_profile`), the published analog profile (`analog_profile`), whether the analog family runs
    (`analog_family_active`) and the output rate a family switch lands on (`output_rate_for_family`); the engine
    installs `rtl_stream_request_analog_profile()`, `rtl_stream_get_analog_profile()`,
    `rtl_stream_analog_family_active()` and `rtl_stream_output_rate_for_family()` behind them.
  - Sub-audible signalling tables and text (`include/dsd-neo/runtime/analog_tones.h`, `src/runtime/analog_tones.c`):
    the standard 50-tone CTCSS table in tenths of a hertz (150.0 Hz deliberately absent), index lookup and the
    `100.0` / `CTCSS 100.0 Hz` formatters (issue #522). Runtime owns it because the frontends format these values and
    the receive policy parses them, and neither may depend on DSP. It also holds `dsd_analog_tone_detection_active()`,
    the one answer to "does received-tone detection run": the analog FM monitor on PCM input, or on an RTL stream whose
    output kind (the stream-metrics hook) is monitor audio. The DSP tap and `app_control/rx_tone_view` both ask it, so
    a frontend row is shown exactly while the tap listens. `RUNTIME_ANALOG_TONES` pins the table value by value, and
    the predicate case by case.
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
  rate in between gets it refused there, logged, with the front end kept on the digital family. The decoder then goes
  back to the configured settings it had before the switch (`ui_revert_analog_entry()`, from a snapshot
  `ui_arm_analog_entry()` takes when the switch commits, keeping the row-scoped options in force and only while the
  configured settings are still the ones the switch left; under a scan row through the scope's suspend and resume):
  at once for a refusal by the request, which fails the command (`svc_publish_symbol_profile()` returns -1), and at
  the next command drain for one where it landed (`svc_take_monitor_request_outcome()` reports the front end kept the
  digital family, `ui_settle_receive_requests()`).
  `DSD_APP_CMD_CONFIG_APPLY` runs the same sink and publish sequence when its `[mode]` moves the session between the
  analog and digital families, after the same check when the move is onto the analog family (a refusal leaves the
  whole config unapplied); a digital-to-digital `[mode]` change keeps its earlier behaviour, except that ProVoice
  (or `-8`) also gets the raw sink it writes to, and that an RTL front end is told the digital modes it now configures
  (`svc_note_digital_decode_modes()`, which `svc_publish_symbol_profile()` also calls). Like `DECODE_MODE_SET` it keeps the session's audio output layout
  (`pulse_digi_out_channels`, `pulse_digi_rate_out`) through any `[mode]`: the output streams were opened with it, so
  a digital sink a family change opens gets it too, and a later preset cannot leave the options on another layout
  than the open stream's. A `DECODE_MODE_SET`, RadioReference import or `[mode]` that moves the decoder between the
  analog and digital families drops the analog monitor block it has part-collected (`dsd_symbol_analog_block_reset()`),
  whose samples are the old family's; a change inside a family keeps it. Tests:
  `APP_COMMAND_QUEUE`, `APP_CONTROL_ACTIONS_RTL`.
- NFM channel width (issue #525): `DSD_APP_CMD_NFM_BANDWIDTH_SET` (508, int32 Hz, 0 = default; a coalescible setter)
  runs `svc_set_nfm_bandwidth()`: a width outside 0 or 8000..25000 Hz is refused, and while the configured -fA preset
  uses it (the scan scope's configured view) `svc_check_nfm_bandwidth()` holds it to `DSD_NEO_CHANNEL_LPF` on a radio
  input (PCM input runs no channel filter, so the width is only stored there) and to the running stream
  (`rtl_stream_check_analog_profile()`) or, with none, to an RTL-SDR/rtl_tcp input's DSP bandwidth; a refusal is a toast
  naming the width, the rate, the limit and the fix, and changes nothing (the validator's full text is logged).
  `svc_describe_nfm_refusal()` words it: a DSP bandwidth on an RTL-SDR or rtl_tcp input; on a SoapySDR, Airspy or I/Q
  replay stream the demod rate the stream publishes (`rtl_stream_get_demod_rate_hz()`) with the fix for that source. The
  command is not scoped (`command_updates_scan_mode()`): like squelch, it edits the configured width through
  `dsd_scan_mode_set_configured_nfm_bandwidth()` instead of suspending and re-applying a row, which would read the row's
  live acquisition (a detected Phase 2 polarity, a followed call) as a change and end it. An nfm scan row that runs the
  configured width (no width of its own) holds the edit to the rate on any session, on air or waiting in the scan
  (`dsd_engine_scan_runs_configured_nfm_width()`). While the row on air sets its own width (issue #526), that width
  stays in force: the edit lands on the configured baseline only, reaches the front end with the next row that takes it
  or the leave, and the toast says the row overrides it (`dsd_app_analog_width_edit_notice()`). A refused width is put
  back through `svc_restore_nfm_width()`: outside a scan the configured width takes the one the front end kept; under a
  scan row, where that can be a row's own width (the row on air's, or one a retune in flight had not moved the front
  end off yet), the configured width goes back to its value from before the refused change, which each monitor request
  records (`svc_monitor_refusal::configured_before_hz`), and only a row with its own width takes the kept width in
  force. An accepted width in force goes to a running front end whose options in force are -fA as a live
  analog profile request (`svc_publish_nfm_bandwidth()` in `symbol_profile.c`), which also replaces the width a queued
  switch onto analog carries; a typed digital row keeps its profile until its leave, and under a scan row's suspended
  scope (a config apply) the request waits for `apply_cmd_scoped()` to publish it after the resume. CQPSK toggled on
  under -fA holds it too, and the DSP op that turns CQPSK off (`svc_toggle_rtl_cqpsk()`) returns to the monitor through
  the analog profile with it. Whether CQPSK holds the front end is what was last queued, not only what the stream
  publishes, since a toggle, a switch or a scan row's leave (queued through the runtime hooks) drained with the width
  has not landed yet: the stream answers for every request queued, whoever queued it (`rtl_stream_requested_cqpsk()`),
  falling back to the published state once every request has settled (`rtl_stream_receive_request_outcome()`), never on
  the output generation, which the demod thread moves before it publishes and a retune moves without taking a request.
  The toggle flips that requested state too. A width the front end refuses after the check is never left configured:
  refused by its request (a retune moved the published rate), the change is refused with the previous width put back;
  refused by the demod thread where it lands, the request reads `RTL_STREAM_RX_REQUEST_REFUSED`, and the next
  `dsd_app_drain_cmds()` puts back the width the front end kept, as the stream recorded it when it refused
  (`rtl_stream_receive_request_refusal()`, so an earlier width that landed in between stands), and toasts why
  (`svc_take_monitor_request_outcome()`, which follows the last analog monitor request `symbol_profile.c` queued, and
  `ui_settle_receive_requests()`). A switch to Analog (`DECODE_MODE_SET`, a config's `[mode]`) holds an explicit width
  to the rate first, under a scan row as well (`ui_check_mode_receive_profile()`); a `[mode]` without a decode key keeps
  the session's family. `DSD_APP_CMD_CONFIG_APPLY` holds the explicit width it leaves in force to the rate it will run
  at before applying anything: `svc_check_nfm_bandwidth_for_rtl_bw()` at the `rtl_bw_khz` a hot restart stores, as
  given, when the `[input]` builds an RTL-SDR or rtl_tcp spec other than the running RTL-family input's
  (`cfg_radio_reopen()`), otherwise the check above; one that reopens a SoapySDR or Airspy device (an Airspy `[input]`
  over a running Airspy reopens it for a new sample rate, serial, DSP bandwidth or volume,
  `svc_airspy_settings_reopen()`) is held only to the rules every rate shares
  (`svc_check_nfm_bandwidth_at_device_rate()`) and left to that stream's start, which checks the width at the rate the
  device delivers. Since the stream a reopen starts runs the scan row on air again once the scope resumes, an nfm row's
  own width is held to the reopened rate the same way (`cfg_check_scan_row_width()`), and on a session that stays
  digital the configured width is held while the scan has an nfm row or target without a width of its own. An
  `rtl_bw_khz` above every selectable bandwidth that no width fits is refused as the setting, not as a rate.
  `svc_rtl_set_bandwidth()` (RTL_SET_BW) refuses a DSP bandwidth the configured preset's explicit width cannot run at on
  an RTL-SDR or rtl_tcp input, naming both and saying to narrow the width first (or, at a bandwidth that filters no NFM
  width, to leave it unset first), and `DSD_APP_CMD_RTL_ENABLE_INPUT` (Input > Switch source > RTL-SDR) asks
  `svc_check_rtl_input_analog_width()` before it rewrites the input and tears the running stream down, refusing a width
  the DSP bandwidth of the device it opens cannot filter. Every change that commits to a radio stream start with an
  explicit width refuses it first while `DSD_NEO_CHANNEL_LPF=0` (`svc_analog_width_env_allows()`), since the start would
  refuse it at any rate. Every one of these classifies the input as the stream's `detect_radio_source()` does
  (`dsd_app_analog_rtl_bw_rate_hz()`): an `rtl`/`rtltcp` spec, or any device string on an RTL input that names no
  SoapySDR, Airspy or replay device (Input > Switch source > RTL-SDR leaves `pulse` there), runs at `rtl_dsp_bw_khz`,
  saturated rather than overflowed for a loaded config's out-of-range `rtl_bw_khz`. Tests: `APP_COMMAND_QUEUE`,
  `UI_MENU_SERVICES`, `IO_RTL_DEMOD_CONFIG` (request numbering and outcomes).
- Shared display decisions, so no frontend has to restate one: `include/dsd-neo/app_control/call_view.h` and
  `src/app_control/call_view.c` fold the canonical call state into a per-slot line, and
  `include/dsd-neo/app_control/scan_timing_view.h` and `src/app_control/scan_timing_view.c` fold
  `dsd_state::scan_timing` into the Scan Timing row — the stay phrase, the remaining/total of the window that is
  running, and which of dwell/hold/hang is worth printing (issue #508), including `Carrier` for an analog row's carrier
  hold (issue #526). The decoder owns every deadline; these views only difference it against the caller's monotonic
  clock, which is what keeps the terminal row, the Qt panel and the Android app from drifting on what "suspended" or
  "hold" means. Tests: `APP_CONTROL_CALL_VIEW`, `APP_CONTROL_SCAN_TIMING_VIEW`, and the terminal goldens in
  `UI_NCURSES_PRINTER_HELPERS`.
  `include/dsd-neo/app_control/squelch_view.h` and `src/app_control/squelch_view.c` (issue #521) pair the squelch in
  force with the configured default, say whether a scan row overrides it and whether each level is off: the terminal
  SQL field and M17 VOX field (`-60.0 dB (row; default -80.0 dB)`), the DSP panel's `(row)` mark, the shadowed-edit
  toast, and Qt's `configuredSquelchDb`/`effectiveSquelchDb`, `configuredSquelchOff`/`effectiveSquelchOff`,
  `squelchRowOverride` and `squelchReadout` all come from it. The Qt radio panel lays those out as its whole-dB
  stepper reading, a `row` badge and `default X`, and uses `squelchReadout` as the reading's accessible name. It
  reads the row's value from `dsd_scan_mode_row_options()`, so it is also right on the decoder thread while a command
  has the scope suspended. Test: `APP_CONTROL_SQUELCH_VIEW`.
  `include/dsd-neo/app_control/rx_tone_view.h` and `src/app_control/rx_tone_view.c` fold `dsd_state::analog_rx` into
  the received-tone text (issue #522): hidden unless `dsd_analog_tone_detection_active()` says the tap listens
  (decided from the options and the RTL output kind, not from INACTIVE in the publication, so a reset does not blink
  the row) and hidden while the publication reads UNAVAILABLE (an input rate the front end cannot use, where an em
  dash would claim no carrier), then `CTCSS 100.0 Hz`, `detecting`, `none` or an em dash (the terminal prints a hyphen
  without UTF-8). Like the scan timing view it takes the caller's monotonic clock: a publication past its
  `stale_after_ms` deadline (a stdin, UDP or TCP producer that stopped sending, or a live radio stream whose source
  stopped, while the decoder waits for samples and cannot say so itself) reads as the em dash. A locked value this
  build cannot name (an unsupported frequency, DCS until #523) reads `detecting`, never a value. The same view carries
  `configured_text`, the configured tone policy, which reads `off` until #527 and is never derived from the received
  tone. Tests: `APP_CONTROL_RX_TONE_VIEW`, the terminal goldens, `UI_QT_METRICS_MODEL`.
  `include/dsd-neo/app_control/analog_width_view.h` and `src/app_control/analog_width_view.c` (issue #525) decide the
  analog channel width in force under the configured analog preset (the scan scope's configured view, so a typed digital
  row does not hide it; never for the M17 encoder): the front end's reported width while a running stream's options in
  force run the monitor, flagged DSP-limited when the rate bounds it, otherwise the configured width, and none on PCM
  input. With no stream running on an input whose RTL DSP bandwidth sets the rate, that rate bounds the widths offered
  (`max_hz`); an unset default reads as what the monitor runs at that rate, as the next start publishes it: the rate
  itself where no channel filter runs (below 20 kHz, or as `DSD_NEO_CHANNEL_LPF` says), the legacy WIDE plan's passband
  (`dsd_channel_lpf_legacy_wide_width_hz()`) where the filter runs but the rate cannot realize the default, both
  DSP-limited, and so it does at a running stream's demod rate while the front end is off the monitor (a typed digital
  row, CQPSK toggled on under -fA), as the monitor it returns to publishes it. There whether the filter runs is the
  stream's own decision (`rtl_stream_channel_lpf_default()`, carried as `dsd_frontend_metrics::channel_lpf_default`),
  which its configuration made from the rate it started at and keeps whatever rate the device then delivers (a forced
  rate, a replay's capture rate), so the view does not re-derive it from the rate. The configured width comes from
  the scan scope's configured view while one is live. An analog scan row on air (issue #526) shows its width on any
  session (`row_analog`), and a row that sets its own width (`row_override`, `row_hz`) is the width in force, read as
  `12.5 kHz (row; default 16 kHz)` with the configured width of its demodulator, which it overrides (on an AM session
  its leave returns to the AM width instead). The services that hold a DSP rate to an analog session's width hold the
  configured preset's own kind and width, not the row's. They spell the reading (`12.5 kHz`, `16 kHz (default)`,
  `12 kHz (DSP-limited)`, `not used on PCM input`), the width command's notice
  (`dsd_app_analog_width_edit_notice()`, for the kind the command edits whatever kind the configured preset runs:
  `Applied: NFM bandwidth -> 12.5 kHz`, or `Default NFM bandwidth -> 16 kHz; this channel overrides it (12.5 kHz)`
  under a row width) and the configured setting (`12.5 kHz`, `default`). The terminal's `Analog:` status field, the
  `rtl.nfm_bw` row's label and predicate, the width command's toast, RTL_SET_BW's configured-width check and Qt's
  `analogBandwidth*` properties (the row flags as `analogBandwidthRowActive`/`analogBandwidthRowOverride`) all come
  from it. Test: `APP_CONTROL_ANALOG_WIDTH_VIEW`.
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
- Received-tone detection (issue #522), public entry points in `include/dsd-neo/dsp/analog_rx.h`: `dsd_analog_rx_tap()`,
  `dsd_analog_rx_tap_partial()`, `dsd_analog_rx_block_restart()`, `dsd_analog_rx_reset()`,
  `dsd_analog_rx_carrier_open_now()`, `dsd_analog_rx_block_straddles_boundary()` and the monitor playback
  bracket `dsd_analog_rx_playback_begin()` / `dsd_analog_rx_playback_end()`. The same header holds the CTCSS timing
  contract in sample time: p95 targets `DSD_ANALOG_CTCSS_LOCK_P95_MS` (400) and `DSD_ANALOG_CTCSS_LOSS_P95_MS` (350) and
  per-event ceilings `DSD_ANALOG_CTCSS_LOCK_CEILING_MS` (700) and `DSD_ANALOG_CTCSS_LOSS_CEILING_MS` (800).
  `DSP_ANALOG_CTCSS` asserts them on every timing row, and a tone policy's acquisition window must exceed the lock
  ceiling by at least 100 ms. Each ceiling sits above the slowest event of the long-run sweeps in `docs/testing.md`
  (1,000,000 starts per condition at 0 dB, 800,000 stops), as the header states; change them only with new sweeps. The
  header also states the measured wrong-tone rates (neighbour locks near 0 dB, talk-off), which a policy acting on the
  first lock has to budget for. `dsd_symbol.c` taps each unsynced analog block while it is still raw:
  `symbol_process_unsynced_analog()` offers the tap the block after every sample it adds, the one that completes the
  block included (`dsd_analog_rx_tap_partial()`), and the tap reads what is waiting once
  `DSD_ANALOG_RX_TAP_READ_MS` (20 ms) of input, at the input's current rate, has built up;
  `symbol_finalize_unsynced_analog_block()` hands it the rest after the raw WAV write and before
  `symbol_apply_unsynced_filters()`, whose in-place `hpf_f` (960 Hz) and `pbf_f` would remove every CTCSS tone. The
  block is 20 ms on RTL but 960 samples on PCM at any rate (384 ms at 2500 Hz), and read only at block ends the
  publication would trail the sample-time contract by up to a block. One decoder-thread tap covers RTL and PCM, sees
  only live (not seam-replayed) samples, only reads the block, and runs whatever `audio_out` says.
  `symbol_output_unsynced_analog()` is its monitor gate, sink and carrier stamp (issue #526): the stamp that holds a -Y
  row under the hangtime rule follows the tap's carrier (`dsd_analog_rx_carrier_open_now()`, which reports none once the
  tap's own generation check sees a retune or profile change since its last read, and while a retune is unresolved, so
  a channel a failed retune left the receiver on lets the row's hangtime run out) while the analog FM monitor runs,
  whether or not the block plays (the `-8` monitor under digital decoding keeps stamping only what it plays), and the
  gate writes nothing while a retune is unresolved (`dsd_trunk_tuning_pending_request()`: in flight, or failed after
  the scanner moved on, until a later retune lands or the scan's end retires it, as for digital frames) or, on the
  analog monitor, from a block that began before a retune, profile change or reset the tap noticed, before detection
  started, or before a boundary the tap has not read past yet (`dsd_analog_rx_block_straddles_boundary()`, 0 while
  detection is not running, so the `-8` monitor under digital decoding plays every block as before). It is active only
  while `dsd_analog_tone_detection_active()` (runtime, above) says so: the analog FM monitor on PCM input or on RTL with
  an AUDIO_MONITOR output kind. The rate comes from the RTL output-rate hook or `dsd_opts_current_input_timing_rate()`.
  The sync hunt keeps that output kind: the analog family stands the modulation auto-switch down, and in analog-only
  mode `dsd_frame_sync.c` never sends the RTL front end any symbol profile the hunt requests, the profile a two-level
  hunt re-normalises included (a CQPSK one would turn the monitor audio into symbols and silence the tap, and a digital
  channel profile would narrow it), as `app_control/symbol_profile.c` does not.
  - `src/dsp/analog_rx.c` holds the pure core behind the module-private `src/dsp/analog_rx_internal.h` (no `dsd_state`,
    no clock, so `DSP_ANALOG_CTCSS` drives it in sample time): the shared sub-audible front end (stage 1 a Blackman FIR
    decimating by `floor(fs / 2400)`, evaluated only when an output is due; stage 2 a Blackman LPF at the ~2.4 kHz rate,
    290 Hz cutoff, 60 Hz transition; a 10 Hz DC blocker), the per-read carrier test (the caller's squelch reading plus
    an absolute mean-square floor that only rejects zeroed or squelched blocks) with the 200 ms sample-time hangover
    (`DSD_ANALOG_CARRIER_HANGOVER_MS`), and the detector plug-in table `k_detectors`: each row is a
    `dsd_analog_rx_detector_ops` (configure/reset/process/report) and the core member holding that detector's state, so
    the DCS detector (#523) is one more row. Detectors get the band stream, a time-aligned stage-1 "wide" stream (about
    0-1 kHz, for the harmonic test) and a "full" stream aligned the same way: the raw input's mean square over each
    decimated sample's span, before any filter. Decimation folds a residue of voice-band content into the band (stage 1
    leaves it at least 58 dB down, 74 dB from 20 kHz inputs up), and on a carrier with nothing else below 290 Hz that
    residue alone looks like a pure tone; the full stream is how a detector tells it from one. The carrier test reads
    the raw samples' mean square, and each detector's thresholds are ratios against those streams' energy. Reports merge
    in table order: the first LOCKED report names the tone; otherwise the verdict is ACQUIRING while any detector still
    is, and NONE once all have said so. The front end accepts 2400 Hz up to `DSD_ANALOG_RX_MAX_RATE_HZ` (320 kHz, below
    the ~333 kHz its tap budget can design), logs which side of that range an unusable rate is on (once for each stretch
    of input at such a rate: a usable block ends the stretch, a reset does not) and publishes UNAVAILABLE there (after a
    reset, from the next block on), keeping the carrier (floor, test and hangover) at every rate all the same, which the
    scanners hold analog rows on (issue #526). The core, not a detector, owns the absolute floor and the carrier test;
    `process(band, wide, full, count, freeze)` is the whole interface a detector gets. It also holds the decoder-thread
    glue: the working state in `DSD_STATE_EXT_DSP_ANALOG_RX` (slot 9, heap, never deep-copied), the publication
    `dsd_state::analog_rx`, and the `Received tone:` LOG_INFO line on each change of verdict (every reset moves the
    publication's generation on and starts a new reception, which logs its verdict again, the same tone included). The
    glue judges the squelch for each read: on RTL from the receiver power the stream keeps current, on PCM from the
    read's own level (`dsd_input_level_metrics_from_pcm_f32_i16_scale()`, the measurement that sets `opts->rtl_pwr` for
    each whole block only once the block is complete). The carrier hangover counts samples, which only works while
    samples arrive: on stdin, UDP and TCP input, whose producer may squelch by sending nothing, and on a live RTL-family
    radio stream, which stops when its source does (an `rtl_tcp` server that went away, whose client retries without
    end; a stalled device), each read also sets a monotonic deadline (its arrival plus its own duration and the
    hangover, at least `DSD_ANALOG_STREAM_PAUSE_MIN_MS`), published as `stale_after_ms`. A read arriving past it follows
    a pause as long as a dropped carrier: it starts a new reception, and is itself dropped, since its first samples may
    have arrived before the pause. The frontends age the row against the same deadline meanwhile. Files, Pulse and IQ
    replay deliver continuously and never set one, so a replay stays deterministic however slowly it is read.
  - `src/dsp/analog_ctcss.c` is the CTCSS detector: one continuously running phasor per table tone, 50 ms sub-blocks
    with absolute phase in a 250 ms window, one hop per sub-block. Each hop fits every bin's sub-block phases (a
    pulse-pair estimate refined by weighted least squares), snaps the fine estimate to the table within +/-0.8 Hz,
    rejects aliases (an estimate more than 5 Hz from its bin) and scores rho, the share of the sub-audible band energy
    the tone explains. A tone locks after two consecutive hops qualify it (rho >= 0.35, an estimate within 0.5 Hz of the
    table value, at least 1e-5 (-50 dB) of the raw input's full-band power, which no folded voice-band residue reaches
    and every tone the tests lock exceeds by 24 dB or more, a phase fit whose reduced chi-square against the band's
    own noise stays under 6, estimates within 0.5 Hz of each other, and less than 0.08 of phase-locked second and
    third harmonic power: a voice fundamental has harmonics, a tone does not). It holds while its own bin's estimate,
    re-measured every hop, stays within the 0.8 Hz snap gate and the newest 100 ms keep rho >= 0.15 at the locked
    frequency and the same -50 dB of the full band; it is lost after four failing hops or at once on a reverse burst
    (a >100 degree phase jump between strong sub-blocks, which catches the 180 degree burst and the 120 and 240 degree
    variants). The jump is measured against the locked frequency as it stood two hops earlier: a 120 or 240 degree step
    inside a sub-block that stays strong puts part of the step into that sub-block's phase, and a newer estimate fits it
    as a steeper slope, against which the step reads short of 100 degrees. On the first hop after a lock, or a relock
    onto another tone, the reference is the candidate's estimate from the qualifying hop before the lock, whose window
    ends a sub-block before the lock hop's, so a step inside the sub-block a tone locks on is caught on the next hop
    too. The two frequency gates are hysteresis: at
    0 dB the estimate scatters by about 0.19 Hz, so an off-table tone 1.1 Hz from a neighbour reaches the 0.8 Hz gate on
    several percent of hops but the 0.5 Hz one almost never, and the per-hop check drops a lock the tone has moved away
    from. The price of the tighter acquisition gate is tolerance of transmitter encoder error: `DSP_ANALOG_CTCSS` pins
    tones 0.2 and 0.35 Hz off their table value locking within 400 ms at +10 dB on every one of its 200 seeded starts,
    and 0.2 Hz off at 0 dB within 400 ms on at least 95% of them (all within 500 ms); over 10,000 starts, 1.55% of
    0.2 Hz-off tones at 0 dB take longer than 400 ms (`docs/testing.md`). From about 0.5 Hz off a tone locks late or not
    at all. A carrier with no lock after 500 ms of evaluation reads `NONE`, on the first hop after it however the input
    is blocked (carrier time is counted per sample), and a tone that starts later still locks. Late acquisition keeps
    lock time in noise inside the lock ceiling: the ring holds 12 sub-blocks, and while nothing is locked each hop also
    measures its newest 600 and 400 ms (`ctcss_step_late()`), over no more than has closed since the last reset or
    loss (`fresh`), so a lost tone is never in them and neither runs before 400 ms have. They apply the same tests with
    rho down to 0.25 (noise alone puts about 1/116 of the band into a bin over 400 ms) and one more: the newest 250 ms
    must still carry the tone at that rho, within the 0.8 Hz gate, which keeps a voice that held a pitch near a table
    tone for most of a longer window and then moved on from locking. Two agreeing hops lock, as for the 250 ms window.
    On its own the 250 ms window passed the 700 ms ceiling on about one start in 125,000 at 0 dB (the slowest after
    1,128 ms), when noise kept every pair of its hops from qualifying the tone; the longer windows average that noise
    down. Samples from inside the
    carrier hangover keep the correlators' time, but a hop whose newest 100 ms (what the hold test reads) holds nothing
    else keeps the verdict, so a dropout's silence alone never ends a lock or makes one. Deciding by the sample that
    closes a hop instead would let a carrier that keeps dropping out keep a stopped tone for good, once its openings
    missed every hop's end; this way each opening makes the two hops that read it count, and a dropout the hangover
    allows leaves at most three hops in a row without one. Every threshold is a ratio, so the RTL live (~1/pi), replay
    and int16 PCM scales read the same.
  - Invariant: resets never happen in `noCarrier()` / `dsd_engine_reset_no_carrier_state()` (they run every ~375 ms in
    analog mode and would stop any tone locking). They happen in `dsd_frame_sync_reset_acquisition()` (row commit and
    leave, trunk-scan target switch, decode-mode change, scope resume, RR apply), on an RTL stream-generation or
    `dsd_trunk_tuning_generation()` move, a change of the analog profile the RTL stream publishes
    (`dsd_rtl_stream_metrics_hook_analog_profile()`: kind, width, channel filter on) or an input-rate change seen by
    the tap (the read is discarded), at the legacy untyped `-Y` step (also when the step failed after its rigctl leg
    moved the radio) and at engine stop (`engine.c`),
    on accepted `RTL_SET_FREQ` / `MANUAL_TUNE` commands, on every tune `request_manual_tune()` accepts (manual channel
    cycle and scan avoid on an untyped list, candidate cycle, return-to-CC, lockout and skip: `io_control_set_freq()`
    moves a rigctl radio without advancing the trunk-tuning generation), on every input switch (`ui_input_switched()`,
    stop-playback even when the Pulse open fails, and the config apply's input comparison in `app_command_queue.c`), on
    a config apply that changes the decode mode (the same comparison: out of the analog monitor no monitor block need
    arrive to forget the tone before the row comes back), when the symbol path falls back to Pulse at the end of a WAV
    file or after a lost TCP connection (`symbol_open_pulse_input_and_reconfigure_output()` in `dsd_symbol.c`), whenever
    `symbol_read_sample_tcp()` finds its connection interrupted, before it reconnects (the reconnect's 300 ms default
    backoff is shorter than `DSD_ANALOG_STREAM_PAUSE_MIN_MS`, and the new connection may carry another source), after
    the carrier hangover, and on the first read after an input that may pause (stdin, UDP, TCP, a live radio stream)
    paused past its deadline (the read is discarded). Every reset bumps `analog_rx.generation`. Every
    `dsd_analog_rx_reset()` also sets aside what the monitor block `dsd_symbol.c` is part-way through assembling
    (`analog_out_f`) already holds: the tap reads on from the next sample, so none of those samples, which arrived
    before the boundary, opens the new reception, where at a low PCM rate one block (960 samples, 384 ms at 2500 Hz) is
    enough to lock the old channel's tone again. The block itself is left alone, so the raw WAV and the monitor output
    keep every sample across the boundary; resets run in digital sessions too (every
    `dsd_frame_sync_reset_acquisition()`, a lost TCP connection). Detection that starts part-way through a block, with
    no session yet, likewise reads from the sample it started on, also when that sample completes the block: the symbol
    path offers every sample to `dsd_analog_rx_tap_partial()` before it finalizes the block. Whenever the symbol path
    empties the block (`symbol_reset_analog_buffers()`: after each block, and when `dsd_symbol_analog_block_reset()` or
    a receive-family switch landing in `symbol_refresh_rtl_profile()` drops a part-collected one) it calls
    `dsd_analog_rx_block_restart()`, so the tap's next read starts at the new block's first sample. A receive-family
    switch (`DSD_APP_CMD_DECODE_MODE_SET`, a config apply's `[mode]`, the channel-scan leave) forgets the tone through
    the boundary resets above, and on RTL input the switch that lands later on the demod thread also clears the output
    ring and moves the stream generation the tap watches. An analog profile the demod thread applies to a running
    monitor without a family switch (a width-only change, the channel filter turning on or off) keeps the generation
    and the output ring, so the tap watches the published profile too. On Pulse, stdin, UDP and TCP input the input's
    own queue holds more of the old channel, which kept arriving while a rigctl retune held the
    decoder, so every `dsd_analog_rx_reset()` and every generation move the tap sees also arms a backlog skip, and so
    does detection that starts with no session after a reset (the publication's generation is no longer 0: a retune in a
    digital mode, then the switch to the analog monitor). The tap skips its reads until one shows the input ran dry (a
    span of `DSD_ANALOG_RX_TAP_READ_MS` or more of input that took at least half as long to arrive on the monotonic
    clock, where a backlog drains at the decoder's own speed), that read included, or until
    `DSD_ANALOG_RX_BACKLOG_MAX_MS` (2 s) of input, after which stdin fed from a file faster than real time is heard
    again. Time the symbol path spends playing monitor audio (`dsd_analog_rx_playback_begin()` / `_end()` around the
    output write) is not waiting: synchronous playback of stdin input holds the decoder for each block's playing time
    once its buffer is full, and it then reads a backlog at real-time pace. The first read after the boundary only
    starts that clock. Files and RTL-family streams are not skipped (the RTL stream clears its own output at a retune),
    and the skip only reads: the monitor output plays the backlog as before. The tap's own resets act on the read in
    hand instead: a generation move, a pause or a change of input rate since the previous read drops it (after a rate
    change, samples taken at the old rate are another signal at the new one: 1920 Hz at 48 kHz read as 2500 Hz input is
    a 100 Hz tone), and the hangover expires on it.
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
  drops a part-collected block; app-control and the channel-scan leave call it when the receive family changes. On an
  RTL front end the switch itself lands later, at the demod thread's next block boundary, so `getSymbol()` also
  follows the output it reads: an analog-family decoder does not collect a direct (digital) output's samples while
  the front end has yet to switch, and a move between the monitor output and a direct one drops the part-collected
  block (`symbol_refresh_rtl_profile()`), so the first block the new family plays holds only its own samples.
  `tests/engine/analog_replay.c` (`dsd-neo_test_analog_replay`, the `DECODE_IQ_ANALOG_*` audio cases) captures and
  scores exactly that output through the hook, and times it with a wrapped RTL stream read hook, so changes to the
  monitor chain are measured against what a listener hears; back them with `tools/replay_ab.sh --metric analog` evidence
  (`docs/testing.md`). The host's own options are the `--analog-*` names it lists; other `--analog-*` arguments pass
  through to the CLI parser. After each delivered block it also reads the received-tone publication
  (`dsd_state::analog_rx`, which the tap updated from the same block) into its `tone`, `tone_lock_ms` and
  `tone_lock_pct` fields, which the `DECODE_IQ_ANALOG_REAL_CTCSS_*` cases and `tools/replay_ab.sh` read.
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
  before it is dropped), `rtl_stream_check_analog_profile()`, `rtl_stream_get_request_rate_hz()` (the published rate
  those caller-thread checks use, from the stream's start; 0 with no stream), `rtl_stream_get_analog_profile()`,
  `rtl_stream_analog_family_active()` (the analog family, including
  while a CQPSK toggle or a typed row's profile has moved the front end off the monitor output),
  `rtl_stream_output_rate_for_family()` (the output rate a pending switch will produce),
  `rtl_stream_set_digital_decode_modes()` (the decoder's configured digital modes, which pick the FSK channel profile
  a CQPSK toggle returns to once a live switch has moved the stream onto the digital family),
  `rtl_stream_prepare_retune_analog_profile_for_target()` (the same fields bound to a retune target), and
  `rtl_stream_live_family_request_count()` (the live family requests accepted so far; one accepted after a retune
  profile's family was attached supersedes that family).
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
    always have, and an analog one applies no symbol profile, CQPSK toggle or timing queued for the same target. A live
    family request accepted after a retune profile's family was attached supersedes it (`g_live_family_requests`, read
    by `rtl_stream_live_family_request_count()`, which the `-Y` scanner compares across a row's retune): the
    retune lands on its target with neither that family nor the symbol profile queued with it, so a scanner that leaves
    while its row's retune is still in flight (the configured family put back by live requests) is not switched back
    to the row's family when the device finishes the retune. The other way round, a family that lands retires the live
    requests still queued from before it was attached (`rtl_stream_retire_requests_before_family()`, by the request
    number it records): a width or mode command drained just before the scan advanced would otherwise be taken at the
    demod thread's next block boundary and put the front end back on the family or width the row just left. They
    settle as replaced; a symbol profile queued after the attach is the row's own and still applies. An
    analog width is checked again against the demod rate it lands on, both a live request when the demod thread
    consumes it and a retune profile when the retune lands (a retune can move the rate after the request was checked
    against the published one); a width that rate cannot realize is refused (logged once per kind, width and rate) and
    the front end keeps its receive profile. A retune whose own analog profile is refused that way reports itself
    refused (`controller_finalize_rate_chain()`'s return, which `controller_apply_reconfigure()` turns into a failed
    completion), so a scanner does not commit a row whose channel the front end is not running. That includes a live request that moves the stream onto the analog
    monitor output, whose caller has already put the decoder on Analog (after `rtl_stream_check_analog_profile()` held
    it to the published rate, or as the session's configured family): a retune that moved the rate since gets it
    refused, and the front end stays where it was rather than run the width without its channel filter; the log
    reports it, and the request reads `RTL_STREAM_RX_REQUEST_REFUSED` (below). The unset NFM default is never refused
    for its rate. Every queued receive request (analog profile or demod profile) is numbered
    (`rtl_stream_receive_request_seq()`), and `rtl_stream_receive_request_outcome()` says whether the demod thread has
    settled it: pending until it takes it and has published what it applied (the consume publishes the demod snapshot
    before it settles), settled with a later request that replaced it, and settled by a stream open (which drops the
    queue) or a request with no pipeline to take the queue; an analog request refused where it landed reads refused,
    with the family and analog width the stream kept recorded before the settlement
    (`rtl_stream_receive_request_refusal()`), until the next stream open forgets it. Each numbered request also notes
    the CQPSK state it leaves the stream on (an analog family request turns it off, a demod profile sets or leaves it),
    which `rtl_stream_requested_cqpsk()` answers with while any request is unsettled.
    That, not the output generation (which the clear for a request moves before the publish, and a retune moves without
    taking a request), is what tells the decoder the published CQPSK state includes its request. Test:
    `IO_RTL_DEMOD_CONFIG` (`rtl_stream_test_rx_request_outcomes()`). A digital family
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
    also for a mode picked under a scan row, never a running row's constraint, and from a config apply whose `[mode]`
    stays digital: `svc_note_digital_decode_modes()`) rather than from the options, as an
    open with those modes picks it; a stream still on the family it opened on, or with no note since its open (the
    open drops it), keeps picking from its options. Entering or leaving
    the analog family re-applies that family's fresh-open defaults (`rtl_demod_enter_analog_family()`/
    `_digital_family()`), restarts the carrier and timing loops (Costas, band-edge FLL, Gardner TED) and zeroes the
    I/Q DC and balance estimates, the squelch dwell toward a multi-frequency hop and a replay's post-demod decimator as
    an open does, clears the output ring and bumps
    the output generation; a width-only change redesigns the filter from empty histories. A switch to digital also
    keeps the open's floor of two samples per symbol for the TED, which the symbol-profile setter it applies does not
    (ProVoice at a 12 kHz DSP rate), and times a profile it does not override for the demod rate it lands on, as an
    open does, not for the rate the decoder read when it queued the profile: a retune can settle the device on
    another rate in between, and the CQPSK timing loop takes the SPS it is given (`rtl_stream_landing_ted_sps()`). The analog family never runs
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
    between the family request and its symbol profile; a retune that settles a forced 78125 Hz between a `-fA`
    session's digital requests, timed at its 48 kHz, and their consume; a switch whose ring clear meets a decoder read between its
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
  - Analog channel width (issue #525): the RTL-SDR menu's `rtl.nfm_bw` row (`NFM bandwidth... [12.5 kHz]`, or
    `[default]`, on a radio input while the configured -fA preset runs FM or an explicit width is set under another
    preset, `is_nfm_width_editable()`) prompts for Hz and submits `DSD_APP_CMD_NFM_BANDWIDTH_SET` as typed; the DSP rate
    row reads `DSP bandwidth...`, and the rtl_tcp adaptive buffering toggle lives in `Auto-PPM & rtl_tcp`. The input
    status line prints `Analog: NFM 16 kHz (default);` beside `DSP-BW:` (`(DSP-limited)` when the rate bounds the
    channel, `(row; default 16 kHz)` while an nfm scan row sets its own width, issue #526, and under an nfm row on a
    digital session too). The row's label and prompt read the configured width, the one the command edits. Both
    follow app-control's analog width view (above), and so does Qt:
    `MetricsModel::analogBandwidthHz`/`analogBandwidthDspLimited`/`analogBandwidthReading` (0 and `not used on PCM
    input` on PCM), `analogBandwidthMaxHz` is the widest width the running stream's demod rate filters (with none
    running, the rate an RTL-SDR or rtl_tcp input's DSP bandwidth sets), and `analogBandwidthConfiguredHz` is the value
    `qml/RadioSheet.qml`'s NFM width stepper (`radioAnalogBandwidth`, presets in `Util.NFM_WIDTHS_HZ`, stepping from the
    width in force when the default is set and skipping presets above the max) edits through
    `CommandBridge::setNfmBandwidthHz()`; `radioAnalogBandwidthDefault` sends 0 to return an explicit width to the
    default, and the controls are disabled on PCM input with the reason shown. Under another preset the section stays on
    a radio input while an explicit width is set (`analogWidthOffered`), reading the setting, so a width that blocks a
    switch to NFM can be narrowed first. An nfm scan row on air (`analogBandwidthRowActive`, issue #526) keeps the
    section, the width in force and the stepper on any preset (`analogWidthInForce`); a row's own width
    (`analogBandwidthRowOverride`) reads first with a `row` badge and the configured default beside it
    (`radioAnalogBandwidthRowNote`, as `radioSquelchRowNote` does for a row squelch), the value is read aloud as the
    view's full reading, and the stepper steps the configured default (from 16 kHz when it is unset, never from the
    row's width). The `NFM` decode chip (`-fA`) sits in `Util.DECODE_MODES`, so the setup wizard
    offers it too (it suggests no trunking). Tests: `UI_MENU_TREE_AUDIT`, `UI_MENU_ACTIONS`, `UI_MENU_LABELS_RADIO`,
    `UI_NCURSES_PRINTER_HELPERS`, `UI_QT_METRICS_MODEL`, `UI_QT_SESSION_ARGS`, `UI_QT_QML_CALL_LISTS`
    (`tst_radio_analog.qml`, `tst_wizard_decode_chip.qml`).

Qt Quick frontend (`src/ui/qt`):

- After the session's first decoder redraw, `UiController` refreshes live metrics on every timer tick so scan
  countdowns and the sync-loss hold continue aging if input stalls. History, network and policy models still
  refresh on decoder redraws; session lifecycle clears live metrics and prevents stale snapshots from restoring them.
- Received tone (issue #522): `MetricsModel` publishes the `rxTone*` group (`rxToneVisible`, `rxToneStatus`,
  `rxToneText`, `rxToneKind`, `rxToneTenthsHz`, `rxToneCarrier`) with its own `rxToneChanged` signal, filled from
  `app_control/rx_tone_view` in `fillRxToneView()` against the frame's one clock reading, and returned to unknown by
  `clear()` on stop. `rxToneConfiguredText` sits beside it with a signal of its own, `rxToneConfiguredTextChanged`,
  because it is configuration rather than session state: it reads the view's `off` from construction, keeps its value
  across a stop, and no received-tone change announces it. `qml/MonitorScreen.qml` shows it as the `RECEIVED TONE` row
  (`monitorRxTone`) and reserves a hidden `TONE FILTER` row (`monitorToneFilter`) bound only to `rxToneConfiguredText`,
  so the configured policy (#527) can never be mistaken for, or feed, the received tone. The terminal shows the same
  view as the Call Info `Rx tone:` line (`ui_format_rx_tone_line()`, compact view included). The Android notification is
  unchanged.
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
- `DSD_SCAN_OPT_BANDWIDTH` (`--nfm-bandwidth-hz`, issue #526) is an analog row option: `ANALOG_MODES` (the analog
  classes) keep it off digital and blank rows, which are told `needs mode nfm` (`option_mode_allowed()` names the analog
  classes an analog-only switch serves), while `ANY_MODES` (digital plus analog) carries `--squelch-db` and
  `--scan-max-visit-ms` onto analog rows and every other switch stays `DIGITAL_MODES`. The diagnostic names only the
  classes the switch serves (`needs mode nfm`), not every analog class. The value is whole Hz in the NFM range
  (`dsd_analog_width_parse()`); `dsd_scan_option_width_check()` holds it to a DSP rate with the validator's message.
  Unlike squelch it is an acquisition setting (see Per-channel decoder modes), so a width change restages a parked row
  and the configured width is never replaced by a row's in the configured view: saves write the configured width, and
  the width command edits it through `dsd_scan_mode_set_configured_nfm_bandwidth()`, which leaves a row's own width in
  force. `RTL_SET_BW` stays unscoped, so the stream it reopens starts on the width in force, a row's included;
  `svc_rtl_set_bandwidth()` refuses a bandwidth whose DSP rate cannot filter that width, as well as the configured one
  (on RTL-SDR and rtl_tcp, where the bandwidth sets the rate), rather than let the start refuse it and leave no radio
  input; the toast names the scan row's width (`DSP BW 12 kHz cannot filter the scan row's NFM 12.5 kHz (max 9.6 kHz);
  keep a wider DSP bandwidth`), or gives the width's own fix where the row runs the configured width. Input > Switch
  source > RTL-SDR (`svc_check_rtl_input_analog_width()`), unscoped too, holds the same two widths. A config apply is
  scoped, so it holds a row's own width explicitly at a reopen (`cfg_check_scan_row_width()`). On a digital session the
  configured width counts as in use for all of these, and for the width command, while the scan has an nfm row or target
  without a width of its own (`dsd_engine_scan_runs_configured_nfm_width()`). The option's preview fields,
  `bandwidth_hz` in `dsd_csv_channel_profile` and `dsd_app_scan_csv_target` (-1 when the row inherits), reach the
  Qt/Android channel-map review and target preview as `bandwidthHz` (invalid when the row inherits), which read
  `NFM bandwidth: 12.5 kHz`, or `NFM bandwidth: inherit` on an nfm row without one (`Util.nfmBandwidthSummary()`).
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
