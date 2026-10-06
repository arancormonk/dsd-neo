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
    which stamps `dsd_state::scan_timing` with the stay reason and the absolute deadline, on the decode clock's
    monotonic reading, of the window that is running (issue #508); the deadline it publishes is the same instant
    `dsd_scan_voice_gate_should_step()` flips, so the readout cannot drift from the rotation it describes. It also owns
    the analog carrier probe both scanners hold analog rows on, `dsd_scan_analog_carrier_open()` (issue #526): the
    received-tone tap's carrier held to the channel on air (`dsd_analog_rx_carrier_open_now()`) while the analog
    monitor runs, FM or AM (`dsd_analog_monitor_tap_active()`, issue #524), never on a stale publication, a flagged
    digital carrier or a trunking-owned channel, and independent of audio output. The -Y voice gate never owns an
    analog row (`scan_voice_gate_enabled()` is false under the analog family), and the -Y timing tick reports
    `DSD_SCAN_STAY_CARRIER` for the hangtime window while that probe is open. Under the CTCSS/DCS receive policy (issue
    #527) the probe is only traffic the policy passes (`dsd_analog_tone_gate_passes()` in `runtime/analog_tones.c`:
    OFF or ALLOWED, the one rule the monitor sink and its -Y hangtime stamp in `dsd_symbol.c` follow too), and
    `dsd_scan_analog_tone_gate()` reads the policy's verdict on the carrier heard (OFF without one). A PENDING carrier
    is no activity: it holds the row only while it lasts, with no window, and leaves every clock where it was, so a
    check its carrier ends before a verdict adds no tail and short bursts the policy never passes cannot park a
    scanner. The -Y tick reports `DSD_SCAN_STAY_TONE_PENDING` ("Tone check") with no deadline while it is PENDING, and
    `no_carrier_scanner_step_is_due()` (`engine.c`) holds a PENDING or ALLOWED carrier's row at every pass, after the
    per-visit cap and ahead of the hangtime rule: the check never stamps its anchor, and the monitor stamps allowed
    traffic only as a block ends while the tap publishes the verdict at each read, so a verdict turned ALLOWED part-way
    through a block would otherwise meet an anchor from before the check. With no policy (OFF) the stamps alone hold.
    The engine acts on a rejection on its own ticks only, never from DSP:
    `no_carrier_scanner_step_is_due()` (`engine.c`) steps a REJECTED row at the next no-carrier pass, whatever `-t`
    says, when the list has another row to go to (`dsd_engine_channel_scan_has_other_row()` in `channel_scan.c`: a row
    other than the one on air that would judge the traffic otherwise -- on another frequency, or on that frequency one
    of another class or whose tone policy, its own or the configured one, passes what the verdict rests on (the locked
    tone or code, or no tone after a "no tone" rejection; `channel_scan_row_rejects_alike()` with
    `dsd_channel_mode_hears_tones()`) -- not avoided, and not an analog row skipped at every visit
    (`dsd_engine_scan_analog_width_skipped()`: the width of the demodulator its class runs, AM or NFM, its own or the
    configured one of that kind, refused at the DSP rate, or an am row on audio input with no rigctl peer); the row on
    air is the typed scanner's last landed tune, which a failed start does not move, else the one before
    `lcn_freq_roll`), and otherwise holds it muted, the `-t` rule included, as a fixed frequency does, so the same
    traffic is not ended and judged again every second; `no_carrier_step_scanner_mode_if_needed()` still lets
    `lcn_scan_hold` keep it (muted); the trunk-scan tick advances a REJECTED analog target
    (`trunk_scan_service_tone_rejection()`, under `p25_sm_tick_guard`) unless the operator holds it, and only when
    another target is there to take it (`trunk_scan_rejection_has_alternate()`: not avoided, not cooling down, and not
    an analog target whose width, of the demodulator its type runs, AM or NFM, the front end refuses at the published
    DSP rate, `trunk_scan_target_width_skipped()`; a target on the frequency on air counts, since the list's
    `scan_has_duplicate_type_freq()` makes it one of another type, which runs no tone check); with none it spends the
    tick with the target muted in place and its idle dwell disarmed, since that rotation could only switch back to it.
    Its analog stay reason reads `TONE_PENDING` the same way, ahead of the activity hold and with no window, and
    `trunk_scan_service_hold()` holds the target for it without disarming the idle dwell (every other reason to stay
    disarms it), and arms a disarmed one at the check's first tick with no activity or operator hold on, as a quiet
    tick would, so the dwell runs through a check as on a quiet channel; `trunk_scan_refresh_analog_carrier_hold()`
    restarts the hold only on the probe, so only for traffic the policy passes. While the verdict is REJECTED and no
    operator hold is on, both the -Y tick and `trunk_scan_timing_select_reason()` publish `DSD_SCAN_STAY_CARRIER` with
    no window, since no hangtime or dwell counts toward a step while rejected traffic is kept. Rejected traffic that
    ended between two passes or ticks releases the same way: `dsd_scan_analog_tone_rejection_ended()` reads the tap's
    `gate_rejected_ended` through `dsd_analog_rx_rejection_ended_now()` (held to the channel on air as the carrier is),
    or a REJECTED verdict an input that stopped delivering left published past its deadline, with no carrier heard; both
    scanners then step or advance when there is somewhere to go, so no window still running from before it (-t since the
    row landed or since traffic the policy passed, this traffic before a blocked or unlisted value was confirmed
    included, or a trunk-scan hold) is waited out, and with nowhere to go leave the ordinary `-t`, hold and dwell rules
    to run.
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
`nfm-conventional` and `am-conventional`). Operator-facing behavior, the CSV columns, and the CLI/config options live in
`docs/trunk-scan.md`; `include/dsd-neo/engine/trunk_scan.h` is the whole public surface:

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
- Configured tuner gain and autogain: the coordinator owns the gain and autogain a target without its own `rtl_gain`
  runs, captured at scan start. The gain is restored at every switch and at shutdown; the autogain is applied by every
  retune's gain profile and, through app-control, after every stream start, but not kept past the scan or saved. A
  profile stamps `g_tuner_autogain_set_seq`, which every `rtl_stream_set_tuner_autogain()` bumps, so an AGC retune
  that lands after a newer explicit setting (one still in flight past its timeout) leaves that setting in force. The
  flag itself is set when a stream opens, before its workers run (`rtl_stream_open_tuner_autogain()`: the last explicit
  setting, or `DSD_NEO_TUNER_AUTOGAIN` when there is none or it changed since; off on a replay or an Airspy); the
  supervisor no longer loads it on its first block.
  App-control's gain service, the tuner
  autogain toggle and a config's `[input]` gain edit them through `dsd_engine_trunk_scan_set_configured_gain()` and
  `dsd_engine_trunk_scan_set_configured_autogain()` (1 in force now, 0 shadowed by the parked target's own gain, -1 no
  scan), and the coordinator publishes `trunk_scan_configured_gain`, `trunk_scan_gain_override` and
  `trunk_scan_configured_autogain` beside the parked target for the frontends and the config save.
- Conventional activity reports: `dsd_engine_trunk_scan_dmr_conventional_activity()`,
  `dsd_engine_trunk_scan_nxdn_conventional_activity()`, and `dsd_engine_trunk_scan_p25_conventional_activity()`,
  reached from protocol code through the runtime hooks.
- Analog targets (`DSD_TRUNK_SCAN_TARGET_NFM_CONVENTIONAL` = 8 and `DSD_TRUNK_SCAN_TARGET_AM_CONVENTIONAL` = 9, issue
  #526): every exhaustive type switch classifies both types (conventional, no GFSK or P25 symbol rate, no conventional
  family a protocol report can claim), `trunk_scan_target_mode()` maps them to `DSD_SCAN_MODE_NFM` and
  `DSD_SCAN_MODE_AM`, and `trunk_scan_type_is_analog()` gates the rest. The type column is parsed from the
  `k_trunk_scan_types[]` table, which also lists the accepted spellings in the invalid-type diagnostic;
  `scan_type_analog_hint()` points an analog class spelling (`am`, `nfm`, the FM aliases) or a trunked one (`am-trunk`)
  at the one conventional type that carries it. An analog target's retune passes no timing
  (`trunk_scan_retune_active()`), so no zero symbol rate reaches `dsd_opts_compute_sps_rate()`; its tick refreshes
  `last_allowed_activity_m` from `dsd_scan_analog_carrier_open()` instead of the voice-media hold
  (`trunk_scan_refresh_activity()`), and its stay reason reads `CARRIER` while the carrier is open, then
  `ACTIVITY_HOLD` for the tail. The parser refuses key columns, `modulation`, `chan_csv` and `p25_bandplan_csv` on
  it, and the live decryption command refuses it.
- Configured-width use across both scanners: `dsd_engine_scan_runs_configured_nfm_width()` and
  `dsd_engine_scan_runs_configured_am_width()` (both `scan_runs_configured_width()` for one analog kind) answer whether
  the scan running now, the trunk-scan coordinator's target list or else the `-Y` channel map (whose rows
  `channel_rows_run_configured_width()` walks), has an analog row of that kind without a width of its own, which runs
  the configured width of that kind whenever it comes on air. App-control holds that width to the DSP rate on any
  session while it does (see Per-channel decoder modes). It lives here rather than in `channel_scan.h` because the
  coordinator's list is the one it reads first, and the question is asked of whichever scanner owns the tuner.

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
- Classes: `dsd_scan_mode` runs INHERIT..M17 and the analog `DSD_SCAN_MODE_NFM` (9) and `DSD_SCAN_MODE_AM` (10, issue
  #526), whose presets are the analog FM monitor (`DSDCFG_MODE_ANALOG`) and the AM monitor (`DSDCFG_MODE_AM`);
  `dsd_scan_mode_analog_kind()` names the demodulator an analog class runs (-1 for a digital class).
  `DSD_SCAN_MODE_LAST` is the one bound every range check uses (the option parser, `dsd_channel_mode_set()`),
  `dsd_scan_mode_is_analog()` the one analog predicate (key compatibility, the importer, trunk-scan target classes), and
  appending a class keeps stored values and `MODE_BIT()` masks stable. `dsd_scan_mode_alias_hint()` names the class to
  suggest for an alias (`fm`, `analog`, `wfm`, `nbfm`, `fm-conventional` -> `nfm`; none is accepted) and
  `dsd_scan_mode_names_list()` the accepted spellings for a diagnostic. The analog channel widths
  (`analog_nfm_bandwidth_hz`, `analog_am_bandwidth_hz`) are acquisition fields of `dsd_scan_settings`:
  captured, restored, and compared by `dsd_scan_settings_equal()` for the analog family, only the width of the kind the
  settings run (`analog_demod`; issue #524), so a change of that width restages a parked analog row, while a digital
  row, which runs its own channel profile, and an analog row of the other kind are not disturbed by a configured width
  edit; `dsd_scan_mode_resume()` keeps such an idle width as the scoped command left it rather than put it back. They
  are restored again before a row's options install; a row width (`DSD_SCAN_OPT_BANDWIDTH`) lands there through its
  applier, on the width of the demodulator `dsd_scan_option_values::channel_bw_kind` records (the parser sets it from
  the spelling, `--nfm-bandwidth-hz` or `--am-bandwidth-hz`), and shadows a configured-width edit of that kind only.
  `channel_scan.c` restages an outstanding row's tune when the configured width that tune carries changes
  (`channel_scan_configured_changed()`, the width of the kind the prepared row runs): an analog row's without a width
  of its own, whatever the configured family (an nfm row's NFM width, on an AM session too, an am row's AM width on any
  session), and an untyped row's on the analog family (the configured kind's). A typed digital row, or an analog row
  with its own width, commits as staged. `dsd_scan_mode_prepare()` takes the row's option values (NULL = none) so the
  prepared settings a scanner tunes with already carry the row width; its callers are `channel_scan.c` and the
  `scan_mode_replay` / `analog_replay` hosts. A row's symbol timing is computed for the output rate its tune lands on,
  `dsd_scan_mode_symbol_timing_rate_hz()`: the live rate, except while the RTL front end still runs the analog family
  after an analog row, or the work already outstanding lands a family (`family_landing_after_pending`, issue #583: an
  analog retune or request, or a retune that carries the digital family, which lands that family's prediction even on
  a front end already digital, the row's own included when a command re-times the row before it lands), and the
  configured mode is digital (`dsd_scan_mode_configured_digital()`), when a digital row's tune lands the digital
  family and the rate is that family's (`output_rate_for_family`), not the monitor's resampled audio rate or the rate
  a digital front end runs before the landing. The -Y scope timing and the trunk-scan target timing
  (`trunk_scan_p25_cc_sps()`, `trunk_scan_gfsk_sps()`) read it, and each timing of a row records in the scope the
  landing decision it used
  (`scan_scope::timed_digital_family`, `dsd_scan_mode_timed_digital_family()`, issue #583; a `-Y` row staged by
  `dsd_scan_mode_prepare()` records in the live scope). A row once timed for the digital family stays timed for it,
  without asking the stream again, until `trunk_tuning.c` spends the decision on the row's retune
  (`dsd_scan_mode_take_timed_digital_family()`); entering or staging a row clears it. The retune's preparation decides
  once (`dsd_engine_retune_lands_digital_family()`): the row's decision, or the stream's answer when the row has none,
  and the GFSK chain's TED, the digital family attached and the `-Y` supersede accounting all follow it, so the
  decoder, the TED the retune profile carries and the front end are timed for one family even when outstanding analog
  work fails between the row's timing and its retune (the answer can only fall there: only the decoder thread queues
  receive work, and it queues analog work only for an analog row, with no row on air, or at a leave, and a retune that
  carries the digital family only as a row's or control channel's own retune). A later retune of the row that no
  timing preceded (a parked trunked target's control channel hunt) decides by the stream's answer, as a retune outside
  a scan does, and a later timing of the row while its retune is outstanding (a resume after a scoped command) finds
  that retune in the stream's answer. The resume times the row once it runs the modulation it keeps: the row's
  decoder puts the preset's modulation back first, and a P25 row's Phase 2 profile and acquired modulation (a
  `modulation=auto` target's learned CQPSK) are restored before the one timing, not after it. The republish that follows the resume
  decides by the row's decision too (`svc_publish_symbol_profile()`, app-control's receive-family notes): it times
  the row for the landing and asks for that landing itself (`rtl_stream_request_digital_family_landing()`), which
  supersedes the outstanding retune, so the front end lands where the row was timed whichever of the two lands
  first. The stream lands a retune that carries the digital
  family on that prediction whether the front end still runs the analog family where it lands or not (the RTL
  stream's family-switch notes). The rate is asked for the CQPSK state the switch lands, which is `DSD_NEO_CQPSK`'s
  when set unless the scope is a `--trunk-scan` target that makes its own choice (`scan_scope_cqpsk_choice()`,
  exposed as `dsd_scan_mode_cqpsk_explicit()` for the republish, issue #583): P25 with a `modulation` value (`auto`,
  `c4fm`, `cqpsk`), or DMR or NXDN at either rate. The same helper gives the CQPSK state that choice is: a P25
  target's is the modulation its decoder runs, and a DMR or NXDN target's is CQPSK off, the FSK its GFSK chain always
  lands, whatever `rf_mod` a `-mq` lock left a target with no modulation on; the row's timing takes it over the CQPSK
  state its caller passes, and the republish times, asks for the landing and publishes the symbol profile with it, so
  none of them can disagree with the engine's GFSK chain. Only the coordinator
  sets a scope's modulation, and it maps exactly its DMR and NXDN target types to
  those modes, so this is the rule the engine attaches the digital family by (`dsd_engine_trunk_scan_cqpsk_explicit()`,
  below); a `-Y` row of the same modes, and the configured baseline, follow the override. The configured baseline follows the same rule: a
  scoped command (a decode-mode or modulation change, a config apply) times the configured decoder at the live rate, the
  monitor's while an analog row is on air, so `dsd_scan_mode_resume()` retimes it for the digital family before saving
  it (`scan_configured_retime()`) whenever the front end still runs the analog family and the configured mode is
  digital; an untyped row's tune and the leave then land the digital family on the timing it runs at.
- Runtime owns the exact configured decoder baseline and temporary class through `runtime/scan_mode.h` and
  `runtime/scan_mode.c` (extension slot 6). It uses the existing preset definitions while keeping the audio sink fixed.
  Suspend/update/resume supports global commands; scalar snapshot copies keep frontend state independent of live
  storage. Blank rows retain scope ownership. `dsd_scan_mode_configured_view()` borrows the baseline without copying its
  output label; consumers use their published snapshot, and persistence uses the exact configured preset (including
  custom sets). `dsd_scan_mode_apply_modulation()` owns target flags/locks for both entry and scope updates. Inherited
  profiles use the restored SPS hunt index, so AUTO's saved timing and the frontend's rate/levels agree after leaving a
  row. `dsd_scan_mode_row_options()` borrows the installed nonsecret row options (valid while suspended and on held
  snapshots), and `dsd_scan_mode_row()` gives the row's class while suspended too, which `dsd_scan_mode_active()`
  reports as INHERIT then (a config apply asks it for the analog monitor the resume puts back, issue #578), and
  `dsd_scan_mode_suspended_effective()` the settings in force when the scope was suspended (the row over the baseline,
  which a rollback restarts the input on, issue #578). Row
  options are applied through a per-field table (`scan_option_appliers[]`), and the row squelch is pushed to the RTL
  demodulator from the scope's entry points only, once per row change. `dsd_scan_mode_enter()` never pushes, so every
  caller must follow it with `dsd_scan_mode_options()` (NULL for a row without options);
  `dsd_scan_mode_set_configured_squelch()`, `dsd_scan_mode_set_configured_analog_width()` (either kind's width, AM
  included; `dsd_scan_mode_set_configured_nfm_bandwidth()` is its NFM call) and
  `dsd_scan_mode_set_configured_tone_policy()` (the CTCSS/DCS policy, issue #527) edit the configured default without
  suspending (see Scoped scan options), and `dsd_scan_mode_configured_analog_width()` is the one reader of a configured
  channel width (the configured view while a scope is live, `dsd_opts` otherwise), which the scanners, app-control's
  width services and view, the config save and the terminal menu share.
- Engine `channel_scan.c` (extension slot 7) stages typed `-Y` entries for automatic, manual, and avoid stepping through
  tracked tuning. It commits mode/keys only after success and retains generation protection across pending requests.
  Configuration edits retry pending tunes on a later service pass; live output-rate changes do not trigger another tune.
  Failed rows advance to the next candidate; a later successful tune recovers any gate held by a partial backend
  failure. `dsd_engine_channel_scan_waiting()` only inspects ownership; `dsd_engine_channel_scan_service_sync()`
  services pending work and invalidates sync gathered before the transaction. Pending rows defer no-carrier call
  finalization until commit. `dsd_engine_reset_no_carrier_state()` shares decoder cleanup without recursively stepping
  or changing tuner ownership. `trunk_scan.c` selects the same classes from target types while retaining target
  snapshots and modulation/gain ownership. Each row commit (and trunk-scan target switch) opens the sink the row plays
  through, `dsd_engine_scan_ensure_output()` (analog or digital, idempotent; it, the two warnings below, the width rule
  `dsd_engine_scan_width_refused()` and its quiet per-row form `dsd_engine_scan_analog_width_skipped()`, and the
  skipped-row count `dsd_engine_scan_skipped` / `dsd_engine_scan_note_skipped_rows()` are private to the engine,
  `src/engine/scan_analog_internal.h`), and scan start logs what an analog row owes the operator on two schedules. An
  open squelch, which lets noise hold the row until the visit cap or the operator moves on,
  `dsd_engine_scan_warn_analog_squelch()`, is said once per map (-Y) or list (trunk scan). A row's own width on audio
  input without a rigctl peer (with one, the peer takes it as its passband), or a width the front end refuses,
  `dsd_engine_scan_warn_analog_width()`, is said once per map or list and DSP rate, and so is an `am` row on audio input
  without a rigctl peer, which nothing demodulates as AM and which is skipped at every visit. The front
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
  log view, learns why a row is never on air. A row without a width of its own is held with the configured width of its
  kind it runs (`dsd_scan_mode_configured_analog_width()`; the AM default, which always runs its channel filter, is
  held like an explicit width, `dsd_engine_scan_held_width_hz()`), and a changed configured width names again the rows
  that run it (`dsd_engine_scan_row_named_again()`, per kind), while the status line still counts the rows whose own
  width is skipped. A placeholder `-Y` row (frequency 0) is never tuned, so none of these checks names or counts it.
  While the scan has such a row or target (`dsd_engine_scan_runs_configured_nfm_width()`, `_am_width()`, public in
  `trunk_scan.h`), app-control holds the configured width of that kind to the rate on any session, as under -fA: the
  width command, a config apply, `RTL_SET_BW` and Input > Switch source refuse a width or bandwidth that cannot run it,
  whichever row is on air (`RTL_SET_BW` and the switch at the rate the DSP bandwidth gives an RTL-SDR or rtl_tcp
  device; on a SoapySDR or Airspy device they leave the rate to the start, which checks only the width it opens on,
  the one in force). What is left to warn about is a list loaded over such a width, or a rate a device forced. A
  trunk-scan retune in flight on an analog target keeps the width it queued; a width edit made meanwhile reaches the
  front end as a live request the landing retune can land over, so the coordinator requests the width in force again
  once the retune lands wherever it differs (`trunk_scan_reapply_analog_width()`), as the `-Y` scanner restages such a
  tune. A channel map loaded at runtime is held to the DSP rate as it loads: `svc_channel_map_refused_rows()` asks
  `dsd_engine_channel_scan_refused_rows()` (public in `channel_scan.h`, read-only) for the rows the front end would
  refuse at the running stream's rate, or on an RTL-SDR or rtl_tcp input with none running at the rate its DSP bandwidth
  sets, and the import command's toast names the first and counts the rest. A map `-C` or `[trunking] chan_csv` loads at
  startup is held to the rate at scan start, once the stream has published it. The map still loads, since the DSP
  bandwidth that fits it can be set afterwards, and a width the rate cannot fit stays a warning and a skipped row, never
  an import error: the rate a SoapySDR or Airspy device, or an I/Q replay, runs at is certain only once its stream runs,
  so their rows are held to it at scan start, and the Qt/Android review and target-list preview (dry-run loaders with no
  session) check the width's range only. The describe records carry each row's width (`bandwidth_hz`) for the Qt/Android
  review to show it (see Scoped scan options). The receive family a row runs on is queued with its tune in
  `trunk_tuning.c` (`dsd_engine_prepare_scan_profile()`, issue #526): an analog row attaches the analog family,
  demodulator and width to the retune profile (`rtl_stream_prepare_retune_analog_profile_for_target()`) with no symbol
  profile, and a width the front end refuses fails the tune before any backend moves, dropping only the retune profile
  queued for it; a digital row attaches the digital family ahead of its symbol profile only while the configured mode
  is digital (a typed digital row on an `-fA` session keeps the monitor output) and the row was timed for that family's
  landing (the decision its timing recorded, spent by this retune; Scoped scan options above), or the front end runs
  the analog family or the work already outstanding lands a family (`dsd_engine_retune_lands_digital_family()`, issue
  #583, decided once per retune preparation). The second
  is the stream's `rtl_stream_family_landing_after_pending()`, a union of the published analog family, a retune the
  controller has taken or holds queued that carries a family, either one (not superseded, and still bound to the
  target it goes to), and live requests still unsettled that leave the stream on the analog family. A retune that
  carries the digital family counts because it lands where that family's prediction says even on a front end already
  digital, so a digital retune queued behind it, or a timing of its own row made before it lands, is timed and
  attached for that landing, not the live rate. The digital retune lands after that
  work, so an Advance or Avoid while an analog target's retune is still in flight, or a width edit for it still queued
  (a retune that completes FAILED included, through the tick's automatic advance), attaches the family: the analog
  retune lands first and the family switches back, and the older width request is retired rather than taken over the
  digital target. A union rather than the newest word, since a queued retune coalesces with the next and takes its
  profile; it can only err towards attaching the family to a retune that finds the digital family running where it
  lands (an analog retune refused there, a request replaced), which the stream still lands as a switch to the digital
  family would, where the decoder, timed by the same answer, expects it. A digital-only session never attaches one.
  The digital family attached also says whether the queued symbol profile's CQPSK state is the target's own choice
  (`rtl_stream_retune_analog_profile::cqpsk_explicit`, `dsd_engine_trunk_scan_cqpsk_explicit()`, issue #583): a
  trunk-scan P25 target with a `modulation` value (`c4fm`, `cqpsk`, or `auto`, answered by
  `dsd_engine_trunk_scan_active_p25_cqpsk_request()`), and every DMR or NXDN target, whose GFSK chain queues CQPSK off.
  The stream then lands that state over `DSD_NEO_CQPSK`, as a retune that stays on the digital family does, so an
  `lsm` target with `modulation=cqpsk` after an nfm target runs CQPSK under `DSD_NEO_CQPSK=0` and a DMR target runs the
  FSK discriminator under `=1`; `-Y` rows, plain `-T` retunes and P25 targets without a modulation land where an open
  of the mode would. `dsd_engine_gfsk_landing_rate()` asks the landing rate by the same rule. A failed hop off the
  analog monitor re-requests no symbol profile over it. A live receive-family request accepted while
  a `-Y` row's retune is outstanding supersedes the family, and the symbol profile, that retune carries; it acts for the
  row still in scope (a width edit or a config apply republishing the outgoing row's receive profile), so the commit
  restages the row rather than run it on the outgoing row's family (`channel_scan_staged_stale()`, comparing
  `dsd_engine_scan_family_requests()`, the stream's `rtl_stream_live_family_request_count()`, with the count when the
  tune was queued). A retune that carries a family is superseded by any of them
  (`dsd_engine_scan_retune_attaches_family()`, by the same rule as the attach). One without (a typed digital row on an
  analog session, or a digital session's row) lands its symbol profile whatever the digital requests; while the scan
  runs only a live analog request supersedes it, since it asked for the monitor that symbol profile would take away
  (issue #582; the scan leave below supersedes it too), and
  the commit then restages its row by the stream's count of such supersedes
  (`dsd_engine_scan_familyless_retune_supersedes()`, the stream's `rtl_stream_familyless_retune_supersedes()`).
  Otherwise the row commits as staged. Leaving the scan
  (`dsd_engine_channel_scan_leave()`) first supersedes the retunes still outstanding that carry no family
  (`dsd_engine_scan_supersede_familyless_retunes()`, issue #582), so a typed row's retune the controller lands late
  moves its centre only instead of putting the row's profile back over the configured decoder's, then
  restores the configured RTL receive family through the metrics hooks: under `-fA`
  the configured analog profile (`apply_analog_profile`, analog family, demodulator kind and channel width with 0
  meaning the default), otherwise the digital family first and then the restored symbol profile (`apply_demod_profile`),
  so the demod thread switches family before it applies the profile. The leave decides the landing once, as a
  republish does (issue #583): the configured mode is digital and either the row on air was timed for the digital
  family's landing (its recorded decision, `dsd_scan_mode_timed_digital_family()`, read before the leave removes the
  scope, so it still counts when the work it was timed behind failed after a scoped command's resume recorded it and
  before the scan stopped) or `family_landing_after_pending` says the front end still runs the analog family there (an
  `-fA` session whose configured mode was changed to a digital one while a row ran, saving its timing for the analog
  output rate) or work outstanding lands a family on a front end already digital (a row's retune that carries the
  digital family, still in flight). The leave then times the
  decoder, and the profile it publishes, for the rate the digital family lands on (`output_rate_for_family`, with no
  target's own CQPSK choice: the configured decoder lands where an open of the mode would), and asks for that landing
  (`request_digital_family_landing`, which lands the prediction on either family: an outstanding retune it supersedes
  lands its centre only, and one that landed before it leaves the landing to it). Otherwise it makes the plain digital
  family request, a no-op on a digital front end, and keeps the saved timing. A leave that switches the front end's
  family either way, or
  its analog kind (an nfm row's FM monitor left for an `-fM` session's AM one, told by the published analog profile),
  also drops the analog monitor block the decoder has part-collected (`dsd_symbol_analog_block_reset()`). A leave from
  a typed digital row back to Analog or AM does neither (the row kept the front end on the analog family, on the row's
  channel, where no monitor is published): `getSymbol()` drops that block, and collects none of the row's audio the
  front end delivers until its monitor request lands (issue #582, see the monitor block in `dsp`). The M17
  encoder is not the analog family. The leave returns what became of its analog profile request, the last receive
  request it makes: 1 queued, -1 refused at once (the published rate, which a row's retune can have moved, cannot filter
  the configured width), 0 when it asked the monitor for nothing (no active scan, not RTL, a digital configured family).
  The engine's own leaves (the decoder's teardown, a trunk scan's shutdown or failed start) ignore it; every interactive
  leave goes through app-control's `svc_leave_channel_scan()`, which records it for the command drain (issue #578,
  below). Tests: `ENGINE_CHANNEL_SCAN` (the leave's landing included: behind a digital-family retune in flight, from
  the analog family, by the row's recorded decision once the stream's answer has fallen, and the plain request with
  neither; and a typed digital row on an analog session restaged after a live analog request, and committed as staged
  after a digital one), and for the attach while analog work is
  outstanding `ENGINE_NO_CARRIER_RESET`
  (a trunk scan's Advance and Avoid behind an nfm target's retune in flight, a retune coalesced behind it, a width
  edit queued behind it, with the retune in flight or completed FAILED, a P25 target without a modulation under
  `-mq` and `DSD_NEO_CQPSK=0` timed for the switch behind it, the nfm retune failing between a P25 or DMR target's
  timing and its retune, a resume re-timing a P25 target while its digital-family retune is still outstanding on a
  front end left digital, and no retune finding a family landing its row's timing had not),
  `ENGINE_TRUNK_RETUNE_REGRESSION`
  (one read per preparation, and a row's decision deciding and being spent), `RUNTIME_SCAN_MODE` (what a row's timing
  records, holds and clears, a resume timing a learned CQPSK, a DMR or NXDN target under `-mq` timed for its FSK
  landing on entry and resume, and what the scope rule `dsd_scan_mode_cqpsk_explicit()` answers, the CQPSK state
  included), `APP_COMMAND_QUEUE` (a scoped command's republish, and a reopen's, through the command drain: timed for
  the landing and asking for it, with the scope rule's flag, or the plain request with nothing outstanding; a DMR
  target under `-mq` timed and published for CQPSK off with the 12.5 kHz filter either way) and
  `IO_RTL_RETUNE_PREPARE`
  (`rtl_stream_test_family_landing_after_pending()`, a digital-family retune queued, in flight, landed, superseded
  and stale on a digital stream, two retunes outstanding at once included, a plain one beside one with a family among
  them, a retune with the digital family landing on a front end already digital, landed by the controller or, with no
  finalize, as an external backend's retune (`rtl_stream_test_retune_profile_sequence_external()`), and a live digital
  landing,
  `rtl_stream_test_live_digital_landing()`, alone, before or after a retune it superseded, after one that landed
  first, replaced by a plain request, and under the target's own CQPSK choice); for a target's own CQPSK choice
  after an nfm target
  `ENGINE_NO_CARRIER_RESET` (P25 `cqpsk` under `DSD_NEO_CQPSK=0`, `auto` and `c4fm` under `=1`, DMR under `=1`, and a
  P25 target without a modulation), `ENGINE_TRUNK_RETUNE_REGRESSION` (a `p25-trunk` and a DMR trunk target's
  control-channel tune), `ENGINE_TRUNK_SCAN` (the switch, a resume and the scope rule against `-Y`) and
  `IO_RTL_RETUNE_PREPARE` (where each lands under `DSD_NEO_CQPSK=0` and `=1`, the predicted rate and the TED).
  A row's retune (`dsd_engine_scan_tune_to_freq()` in
  `trunk_tuning.c`) carries the RTL profile of the settings the row runs: a row that runs the analog family (an nfm row,
  or a row without a decode mode on an `-fA` or `-fM` session, which keeps it) queues its analog profile, kind and
  width, for its target (`dsd_engine_prepare_scan_analog_profile()`,
  `rtl_stream_prepare_retune_analog_profile_for_target()`) and no symbol profile, so the monitor keeps its own channel
  and detector (the AM detector on a blank row of an `-fM` session), and gets them back after a typed digital row; a
  width the front end refuses fails the tune before any backend moves. Test: `ENGINE_TRUNK_RETUNE_REGRESSION` (the `-fA`
  and `-fM` blank rows among them).
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
  - Private file opens: `dsd_fopen_private()` creates a written file owner-only; `dsd_fopen_private_ex()` also says
    whether it opened the path, which for a write mode creates or empties it before the stream over the descriptor is
    made, so an open whose `fdopen()` fails still reports the file it emptied (issue #578: the I/Q capture writer).
    Open raw bytes with a `b` mode: on Windows a text-mode stream writes every LF as CRLF
  - Win32 wrappers keep the POSIX contract callers rely on: errno values, never raw `GetLastError()` codes (the
    thread scheduling and condition calls map them; affinity and realtime priority return ENOSYS where the platform
    has no such control), ENOENT for a missing file from `dsd_fopen_existing_regular_file()`, and temp files
    (`dsd_mkstemp()`, `dsd_mkdtemp()`, `dsd_fopen_private_temp_for_replace()`) named from `dsd_nonce_fill()` with a
    retry when the name is taken (`src/platform/win32_temp_name_internal.h`), never the CRT's `_mktemp_s()`, which
    gives 26 names per template and reuses one as soon as its file is gone
  - Audio backends: selected by `DSD_AUDIO_BACKEND` (`auto` → PortAudio on Windows, PulseAudio
    elsewhere; `none` → `audio_null.c` discard/silence backend; `aaudio` → Android). Exactly one
    backend translation unit is compiled per build; the shared last-error store lives in
    `src/platform/audio_error_internal.h`
  - Fast-math-proof value checks: `dsd_fp_opaque_f()`/`dsd_fp_opaque_d()` in
    `include/dsd-neo/platform/fp_opaque.h` hide a value's origin from the optimizer (a round trip through a volatile
    object, one store and one load) before an IEEE translation unit tests it for NaN or infinity, so Clang's link-time
    inlining into a fast-math caller cannot fold the test away. See "Fast-math and non-finite values" below.
- Build files: `src/platform/CMakeLists.txt`

### Fast-math and non-finite values

The release presets build with `DSD_ENABLE_FAST_MATH` (`-ffast-math`, `/fp:fast` on MSVC), which lets the compiler
assume no value is NaN or infinite: `std::isnan()`, `std::isfinite()`, `x != x` and NaN-catching comparisons fold to
constants, and Clang also folds an integer bit test on a float it computed or received as an argument. A global
`-fno-finite-math-only` measured a 3.4% DSP slowdown, so the rule is per site:

- Code that must reject NaN or infinity and is not on a per-sample path (configuration and CLI parsing, device control,
  per-block metrics, the terminal and Qt UIs) is built with IEEE semantics: `set_source_files_properties(... PROPERTIES
  COMPILE_OPTIONS "-fno-fast-math")` with an `elseif(MSVC)` `/fp:precise` branch, as in `src/runtime/CMakeLists.txt`.
  A source compiled directly into a test from `tests/CMakeLists.txt` does not inherit that directory-scoped property.
- A hot file keeps fast-math and moves its check into a small IEEE translation unit called off the per-sample path
  (`frames/dsd_dibit_reliability.c`, `io/radio/rtl_finite.cpp`); a by-value argument goes through `dsd_fp_opaque_*()`
  first.
- Per-sample loops validate on bits loaded from memory, which no floating-point assumption reaches: external float
  samples are sanitized where they enter -- SoapySDR CF32 reads and cf32 replays through `bound_cf32_to_f32()` and
  `bound_rotate90_cf32_to_f32_phase()` (`include/dsd-neo/dsp/simd_widen.h`, SIMD with runtime AVX2 dispatch), which
  turn any component that is NaN, infinite or at least 2^60 into 0, and soft symbol files in `src/dsp/dsd_symbol.c` --
  and the Costas and Gardner loops check each sample where they load it (`src/dsp/costas.cpp`).
- Arithmetic whose exact rounding is a contract keeps it explicitly: the decode clock's `ns / 1e9` (`decode_clock.c`
  is IEEE), the fallback decimator's block-split-invariant running sum (`#pragma clang fp reassociate(off)`), and the
  auto and noise squelches' window sums, which must not depend on where a block starts (`squelch_floor.c`,
  `nfm_noise_squelch.c`, `noise_squelch_bank.c` and `pcm_noise_squelch.c` are IEEE).
- Tests that feed NaN or infinity, or check results with `std::isnan()`, compile their own sources with IEEE
  semantics (the block near the top of `tests/CMakeLists.txt`), leaving the code under test on the build's flags.

The `Linux • RelWithDebInfo • ctest (x86-64-v3, fast-math, clang)` check enforces the first point: with `-Werror`, Clang's
`-Wnan-infinity-disabled` makes any NaN or infinity test left in fast-math code a build error.

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
- API note: `pbf()`/`pbf_f()` are pass-through shims since issue #518. The one-pole 8 kHz high-pass and 12 kHz
  low-pass they ran was the analog monitor's default voice filter and took 1 kHz 18 dB down; the monitor's voice
  band-pass is now `dsd_analog_audio_process_f()`'s (`<dsd-neo/dsp/analog_audio.h>`). They stay so out-of-tree callers
  still link. `agsm()`, `agsm_f()` and `analog_gain_f()` are gone; `analog_gain()` stays as the M17 encoder's fixed
  input gain.
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
- State extension slots (`<dsd-neo/core/state_ext.h>`): engine owns 0, 1, 3 and 7; core 2, 4, 5, 8 and 11;
  runtime 6; DSP 9 and 10 (taken from the core range as runtime took 6 from the engine's): 9 is
  `DSD_STATE_EXT_DSP_ANALOG_RX`, the analog receive working state, and 10 `DSD_STATE_EXT_DSP_ANALOG_AUDIO`, the analog
  monitor's audio chains. `CORE_STATE_EXT` pins both.
- API note: `dsd_state::analog_rx` (`dsd_analog_rx_publication` in `<dsd-neo/core/state.h>`, issues #522 and #523) is
  the received-tone publication every frontend reads: int-only (`carrier_open`, `tone_kind`, `tone_state`,
  `ctcss_tenths_hz`, `dcs_code` (the code as its value, 023 octal = 19) and `dcs_inverted` for a DCS lock,
  always the canonical member of the code's alias class, `dcs_candidate` (1 while, with nothing locked, the DCS detector
  holds a candidate code it has read once and not yet confirmed: the tone policy waits past its window for it), `gate`
  and `gate_no_tone`, the CTCSS/DCS receive policy's verdict (issue #527: OFF with no policy in force, PENDING while it
  checks, ALLOWED, REJECTED; `gate_no_tone` when no confirmed value decided it), `gate_rejected_ended` (1 once a
  rejected reception has ended with its carrier, until the next carrier or any other reset), a `generation`
  bumped by every reset, input switch and input-rate change, and `stale_after_ms`, the real-time monotonic deadline
  (`dsd_realtime_mono_ms()`) past which the publication of an input that may pause (stdin, UDP, TCP, a live RTL-family
  radio stream) no longer describes the channel, 0 on inputs that never pause: files, Pulse and IQ replay).
  It rides the `vertex_ks_count..ui_msg` snapshot range beside `scan_timing`, pinned by a `_Static_assert` in
  `ui_snapshot.c`; no float, so the semgrep float-field list is unchanged. Only the DSP tap writes it. `tone_state`
  INACTIVE means nothing has been processed since the last reset, or detection is not running; it is not the
  "detection off" signal. Whether detection runs is `dsd_analog_tone_detection_active()` (runtime), which frontends
  and receive policy ask instead. UNAVAILABLE means detection is on but the input rate is one the front end cannot
  use; `carrier_open` is still kept there (issue #526), and on the AM monitor, where no detection runs and `tone_state`
  stays INACTIVE (issue #524).
- API note: `<dsd-neo/core/analog_tone.h>` (issue #527) holds the receive policy's types only: `dsd_tone_filter_mode`
  (off, allow, block) and `dsd_tone_set`, a bounded bitmask (one bit per standard CTCSS tone; one per standard DCS code
  and polarity, as written) that copies by value into `dsd_opts::analog_tone_filter` / `analog_tone_set`, the scan
  scopes and frontend snapshots. Parsing, formatting and matching are runtime's (`runtime/analog_tones.h`). Any analog
  audio writer (the monitor sink in `dsd_symbol.c` today; a future analog `-w`/`-P` writer too) must consult the
  verdict through `dsd_analog_tone_gate_in_force()` and write only OFF and ALLOWED; the `-6` raw WAV stays ungated.
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
  writes it directly. The analog preset selects FM, the AM preset (`DSDCFG_MODE_AM`, `-fM`, issue #524) selects AM,
  and every other preset puts it back to FM (the `-f` selectors the CLI handles itself do the same).
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
  - UTF-8 output (`include/dsd-neo/runtime/unicode.h`): on Windows the first `dsd_unicode_init_locale()` switches the
    console's code pages to UTF-8 and registers `dsd_unicode_restore_console()` with `atexit()`, so the shell gets its
    own code pages back when dsd-neo exits
  - Hook interfaces that let DSP/protocol code publish state without depending on UI internals
  - Decode clock (`include/dsd-neo/runtime/decode_clock.h`, `src/runtime/decode_clock.c`): an injectable clock for
    decode decisions and decoded-output timestamps (`dsd_decode_time()`, `dsd_decode_now_mono_{s,ms,ns}()`,
    `dsd_decode_now_realtime_s()`) with SYSTEM (platform clocks, default), REPLAY (`anchor + media_ns`, anchor floored
    at 2000-01-01Z, media time monotone and advanced only by the decoder thread) and TEST sources, held in
    process-wide atomics. Sleeps, condvar deadlines, pacing, timeouts and metrics use the separate real-time reads
    (`dsd_realtime_mono_{s,ms,ns}()`, `dsd_realtime_time()`, `dsd_realtime_now_s()`). One comparison never mixes the
    two domains. In `src/dsp`, `src/engine`, `src/runtime` and `src/io`, decode time drives the frame-sync and symbol
    sync stamps (and `dsd_mark_cc_sync()`/`dsd_mark_vc_sync()` in `src/core/time/dsd_time_state.c`), the CQPSK dwell,
    the FSK no-sync reacquire watchdog, the no-carrier, stale-follow, scan and trunk timers, the retune completion
    stamps in `src/runtime/trunk_tuning_hooks.c` and the rdio sidecar's fallback start time. Every clock read in
    `src/protocol` (and `p25_sm_note_cc_activity()` in `include/dsd-neo/protocol/p25/p25_cc_activity.h`) is decode
    time: protocol windows (the DMR header repeat, SLCO print throttle and RC repeat dedup, the P25 PTT
    retransmission and FACCH double-END windows, the regroup key TTL, NXDN recent context), trunk SM stamps and ticks,
    sync stamps, and decoded-output timestamps and file names. The P25 SM watchdog thread sleeps in real time, but the
    ticks it drives compare decode time, so where a tick falls among a replay's samples follows the wall clock, one
    reason a trunked (`-T`) replay is outside the replay determinism guarantee (`docs/iq-capture-replay.md`). In
    `src/core`, `src/app_control` and `src/ui` decode time drives the call store's fallback stamps
    (`call_state_observed_m()` and the recent-activity stamps, so the reacquisition window, `started_m` and
    recent-activity TTLs share the protocols' clock), the event layer's VOICE_END alert and drop-hold due times, event
    and history row stamps (merged-row spans), enc-lockout, patch-TTL and call-skip checks, init-time sync seeds,
    output-file names, the symbol-file rotation timer, the frame, P25 SM and LRRP log stamps, the input-level warning
    cooldown (`dsd_input_level_publish()`: whether a repeated RF or input-level warning prints, against the decode
    time of the last one, `input_level_last_toast_time`), the decoder-thread command stamps in `app_command_queue.c`
    (call ends, `mark_cc_sync`, the `-Y` ticks, call-skip arming) and every view that
    ages those stamps (`call_view`, `notification_status`, `p25_network`, the ncurses printer and P25 display,
    the Qt metrics model). Qt and QML read decode time through `MetricsModel::decodeNowMs` (wall-clock ms, the scale of
    a JavaScript time value). Real time drives device, socket and ring waits, replay pacing, auto-gain and auto-PPM, the
    analog tap's input-pause deadline and backlog skip, the input-level snapshot's measurement stamp (`updated`),
    RadioReference dates and perf, and in `src/core`, `src/app_control` and `src/ui` the `ui_msg_expire` (the
    input-level warning's toast included) and terminal status toasts, UI frame and
    publish throttles, the `.bin` symbol-file pacing (`dsd_dibit.c`), the received-tone input-pause check, the Qt
    sync-label hold, import stamps and the Qt frontend's own clock (`src/ui/qt/realtime_clock.h`: last-listened stamps
    and their ages, location fix and diagnostics-tail ages, the history's day sections and midnight timer), the one
    frontend file that reads Qt's clock directly (see "Two clocks" under Qt, which also names the one sanctioned mixed
    comparison, `day_label()`). Two sources below runtime in the link order keep the platform clocks for their real-time
    reads: `src/io/iq/iq_capture.c` (`dsd-neo_io_iq`) and `src/io/radio/tcp_quality_metrics.cpp` (built into
    `dsd-neo_platform`). Semgrep enforces the split: `dsd-neo.no-direct-clock-read` rejects any other clock read in
    C/C++ outside `src/platform/`, `decode_clock.c`, those two sources and `realtime_clock.h`, and
    `dsd-neo.no-direct-clock-read-js` does the same for the frontend's QML and JavaScript (see
    `docs/code-quality-guardrails.md`). Under `--iq-replay` the engine selects REPLAY first in the run, before common
    setup writes its first record (`dsd_engine_decode_clock_enter_replay()`: the sidecar's `capture_started_utc`, parsed
    by `dsd_iq_replay_parse_utc_seconds()`, then `dsd_state_rebase_decode_timestamps()` in `<dsd-neo/core/init.h>`, the
    one list of decode stamps `initOpts()`/`initState()` seed with "now"; a sidecar that does not parse fails the run
    there). REPLAY is entered only there, at a run's start, on a state fresh from `initState()`, and the engine enforces
    that: `dsd_state::engine_fresh`, set by `initState()` and cleared by the run once it has made the choice, says no
    run has used the state. A replay run on a state an earlier run used (the Android service's reuse when a start races
    `stopSelfLatest()`) stays on SYSTEM with one `LOG_WARN`, since the stamps that run left (such as
    `slco_sfrag_last`) are on the system clock and no rebase reaches them; its decode times are then not the
    capture's. The system clock only moves forward, so those stamps stay behind now, with one exception: an earlier
    replay of a capture stamped ahead of the device's clock (recorded elsewhere, or on a clock that is behind) leaves
    its protocol stamps at the capture's later time, since leaving REPLAY rebases only the init seeds. The clock
    returns to
    SYSTEM with the same rebase once the replay no longer feeds the decoder (`dsd_engine_decode_clock_leave_replay()`):
    at the end of the run, when app-control stops the stream to restart it (`svc_rtl_stop_locked()`; the replay it
    restarts runs on SYSTEM, which it logs once at that leave when the input is still the replay), and when the input
    moves away from it (`ui_input_left()` in `app_command_queue.c`: an input switch, a stop of playback, a config
    apply that moves the input). Decode-mono time goes on from the capture
    time the replay reached (a SYSTEM offset over the platform monotonic clock, set only by leaving REPLAY), so the
    replay's stamps keep ageing; wall time returns to real time. Each replay sample moves media time to its own capture
    time as it reaches symbol processing (`dsd_decode_clock_batch_media_ns()` over the batch tag's span): the symbol
    cache's pop, and the one-sample readers (the analog monitor, M17, EDACS analog) through
    `dsd_rtl_stream_metrics_hook_replay_advance_decode_clock()`. Unit tests step decode time on the TEST source
    (`docs/testing.md`, "Decode time in unit tests"). Tests: `RUNTIME_DECODE_CLOCK` (the sources, the anchor floor and
    ceiling, `dsd_decode_clock_batch_media_ns()`, the leave's continuity), `RTL_SYMBOL_REPLAY_CLOCK`,
    `ENGINE_REPLAY_DECODE_CLOCK`, and the leave in `ENGINE_NO_CARRIER_RESET` and `APP_COMMAND_QUEUE`.
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
    `tone_list`. The tone policy (issue #527) parses `tone_filter` with `dsd_tone_filter_mode_parse()` and `tone_list`
    with `dsd_tone_set_parse()`: the loader warns and keeps a refused mode, leaves a refused list empty
    (`analog_tone_list_refused`) and applies a list policy without a usable list as off; `--validate-config` reports
    both and a list policy without a list as errors. A save writes the configured policy from
    `dsd_scan_mode_configured_tone_policy()`, never an nfm row's own. The CLI switches `--tone-allow`, `--tone-block`
    and `--no-tone-filter` (one of them per command line) parse the same way. `dsd_scan_mode_warn_tone_filter_unused()`
    (`scan_mode.c`) says once that a configured list policy has no effect when the caller found nothing that runs
    detection. Outside a trunk scan that is `dsd_channel_modes_conventional_hear_tones()` (core, beside the row modes),
    the one rule the start, the engine's import and `dsd_engine_scan_hears_tones()` share: a `-Y` list with rows by
    `dsd_channel_modes_hear_tones()` (an `nfm` row, or under `-fA` a row that declares no mode, since a typed row runs
    its own), anything else, a `-Y` scan without rows included, by `dsd_scan_mode_configured_fm_monitor()`. It is asked
    by `cli_finish_parse()`, except for a `-Y` list the engine still imports (a config file's `chan_csv`, or a `-C` map
    it read no row from), which the engine's import asks instead. `trunk_scan_warn_targets()` warns for a `--trunk-scan`
    list with no `nfm-conventional` target, whatever the decode mode; and, during the session, `apply_cmd_scoped()`
    (`app_command_queue.c`) after a command that left a list policy where `dsd_engine_scan_hears_tones()` finds nothing,
    when the command set that policy or took the monitor away from it (a loaded config, a decode-mode change, a channel
    map, a scanner toggle), never again for the same policy still unheard.
    `dsd_user_config_radio_input_spec()` returns the radio input spec (`rtl`, `rtltcp`, `soapy` or
    `airspy`) an `[input]` builds without applying it, so a live config apply can tell whether it reopens the device and
    what then sets the rate. `dsd_rtl_input_spec_apply()` (`runtime/input_spec.h`) reads an `rtl:`/`rtltcp:` spec's
    device or endpoint, tuning and `bias` tokens into the options: the engine reads the spec with it when the input
    opens, and `--print-config` before that, so the export carries the spec's tuning rather than the option defaults. Tests: `RUNTIME_CLI_PARSE`, `CONFIG_VALIDATION`, `CONFIG_TEMPLATE`, `RUNTIME_CONFIG_USER`,
    and for the tone filter's "no effect" line `ENGINE_RUN_SETUP` (a config file's `-Y` list, one without rows
    included), `APP_COMMAND_QUEUE` (a running session, a channel map imported into a `-Y` scan included) and
    `ENGINE_TRUNK_SCAN`.
  - The RTL metrics hook table (`include/dsd-neo/runtime/rtl_stream_metrics_hooks.h`) also carries the receive-family
    request (`apply_analog_profile`), the digital family request that lands the digital family's landing whichever
    family the front end runs (`request_digital_family_landing`, issue #583; a table without it falls back to the plain
    request), the published analog profile (`analog_profile`), whether the analog family runs
    (`analog_family_active`), whether it runs or the retunes and requests already outstanding land a family, either
    one (`family_landing_after_pending`, issue #583: the rule the engine attaches the digital family by, which the scan
    timing follows; a table without it falls back to `analog_family_active`) and the output rate a family switch lands
    on (`output_rate_for_family`, told whether the CQPSK state is a trunk-scan target's own choice, which stands over
    `DSD_NEO_CQPSK` there, issue #583), and what the I/Q replay batch the decoder's last read took its samples from ran
    on (`replay_batch`, `dsd_rtl_stream_replay_batch`: generation, output kind, channel profile, symbol rate and
    levels, and the media span, output count and first sample's index the decode clock runs on; decoder thread only,
    while the stream is open; issue #572); the engine installs
    `rtl_stream_request_analog_profile()`, `rtl_stream_request_digital_family_landing()`,
    `rtl_stream_get_analog_profile()`, `rtl_stream_analog_family_active()`, `rtl_stream_family_landing_after_pending()`,
    `rtl_stream_output_rate_for_family()` and `rtl_stream_get_replay_batch()` behind them. It also hands the
    demodulator a whole squelch setting (`set_channel_squelch_setting`, `rtl_stream_set_channel_squelch_setting()`;
    issue #518 follow-up), which a scan scope's push uses for every setting, levels included; a table without it falls
    back to the level call, with an AUTO setting off. Tests: `RUNTIME_RTL_STREAM_METRICS_HOOKS`, `ENGINE_RTL_STREAM_METRICS_HOOKS_INSTALL`.
  - Squelch setting (`include/dsd-neo/runtime/squelch.h`, `src/runtime/squelch.c`, issue #518 follow-up): a
    `dsd_squelch_setting` (`<dsd-neo/core/power.h>`: mode LEVEL, AUTO or NOISE -- the dynamic modes,
    `dsd_squelch_mode_is_dynamic()` -- the AUTO margin or NOISE quieting in whole dB 3..30, the LEVEL threshold in
    mean-power units) and its one grammar, `off | 0 | <negative dB> | <positive linear> | auto[+N] | noise[+N]`
    (`dsd_squelch_setting_parse()`, `dsd_squelch_setting_format()`), which `--squelch`, the `sql` field of every radio
    spec (`dsd_squelch_spec_field_apply()`, which leaves the setting alone once `--squelch` set it:
    `dsd_opts::rtl_squelch_cli_set`) and the terminal prompt share; scan rows (`scan_options.c`, `--squelch` sharing
    `--squelch-db`'s option bit) take whole dB, `off`, `auto[+N]` on analog rows and `noise[+N]` on nfm rows only.
    `dsd_opts` holds it as `rtl_squelch_mode`, `rtl_squelch_margin_db` and `rtl_squelch_level` (the level kept beneath
    a dynamic setting, for a switch back), the config as `[input] rtl_sql_mode`, `rtl_sql_margin_db` and `rtl_sql`.
    `dsd_squelch_setting_resolve()`, the one policy every surface shares, takes the input kind
    (`dsd_squelch_input_kind()`: radio, audio -- Pulse, a file, stdin, TCP, UDP -- or neither), whether the channel is
    digital and whether it is AM, and says why a dynamic setting runs, runs as AUTO (NOISE on a radio input's AM
    channel) or is off (AUTO off a radio input, either off audio and radio input, a digital channel, NOISE on AM audio:
    `DSD_SQUELCH_RESOLVED_AUDIO_AM`), for the views, the engine's startup check and the scan warnings;
    `dsd_squelch_noise_has_no_fm()` is the AM monitor on its own (`-fM`, no scan), where the engine refuses
    `--squelch noise` at startup and `svc_rtl_set_sql_setting()` refuses a NOISE request (`SVC_SQL_NOISE_NEEDS_FM`); it
    refuses AUTO on audio input too (`SVC_SQL_AUTO_NEEDS_RADIO`, issue #628). `dsd_squelch_pcm_noise_in_force()` is
    NOISE on audio input's FM monitor, the PCM noise squelch (DSP, below). The decoder's comparisons go through
    `dsd_squelch_dynamic_in_force()` (an RTL-family input's AUTO or NOISE, or the PCM noise squelch while
    `dsd_state::squelch_noise_state` says it is learning or holds a reference), `dsd_squelch_flags_in_force()` (whether
    the monitor's per-sample flags carry the gate: that, or the PCM noise squelch running at all, whose flags read open
    with no band or no room, so a block shut up to the decision stays shut), `dsd_squelch_level_in_force()`,
    `dsd_squelch_level_open()` and `dsd_squelch_gate_open()` (DSP, below). `dsd_squelch_publish_status()` copies the
    stream's dynamic squelch status on radio input (the RTL IO hook `squelch_status`, `rtl_stream_get_squelch_status()`)
    into int-only `dsd_state::squelch_auto_*` and `squelch_noise_*` fields inside the snapshot range, the floor in
    centi-dB on `rtl_squelch_level`'s scale and the noise squelch's quieting in centi-dB; while the PCM noise squelch
    is in force it leaves the fields its decoder-thread writer set (`squelch_noise_state`, the `dsd_squelch_noise_state`
    learning, provisional, known, no-band or no-room, among them), and elsewhere it clears them; the frame sync's
    throttled UI publish calls it. Tests:
    `RUNTIME_SQUELCH`, `RUNTIME_CLI_PARSE`, `INPUT_SPEC`, `RUNTIME_CONFIG_USER`, `RUNTIME_SCAN_OPTIONS`,
    `ENGINE_IO_HOOKS_INSTALL`.
  - Sub-audible signalling tables and text (`include/dsd-neo/runtime/analog_tones.h`, `src/runtime/analog_tones.c`):
    the standard 50-tone CTCSS table and 150.0 Hz in tenths of a hertz, index lookup and the
    `100.0` / `CTCSS 100.0 Hz` formatters (issue #522). Runtime owns it because the frontends format these values and
    the receive policy parses them, and neither may depend on DSP. It also holds `dsd_analog_monitor_tap_active()`, the
    one answer to "does the receive tap run": the analog monitor of either kind, FM or AM, on PCM input, or on an RTL
    stream whose output kind (the stream-metrics hook) is monitor audio; the tap keeps the carrier the scanners hold a
    row on and the boundaries the monitor output drops a block across. `dsd_analog_tone_detection_active()` is that with
    `analog_demod` FM (issue #524: CTCSS and DCS are FM signalling), the one answer to "does received-tone detection
    run". The DSP tap and `app_control/rx_tone_view` both ask it, so a frontend row is shown exactly while the detectors
    listen. `dsd_analog_tone_gate_in_force()` is the receive policy's verdict on that reception (issue #527): OFF
    wherever detection does not run, else `dsd_state::analog_rx.gate`, failing closed: a published OFF while `dsd_opts`
    holds a list policy with a list is no verdict of it (the tap has no session, or has not read since the policy came
    on) and reads PENDING; the monitor sink and the scanners ask it. `dsd_analog_tone_gate_passes()` is what a verdict
    lets through, OFF and ALLOWED: the one rule for what the monitor plays and what is scan activity (a PENDING carrier
    holds a row only while it lasts, leaving no tail). Tone
    lists (issue #527): `dsd_tone_set_parse()` reads the '/'-separated list the CLI, the INI and scan rows share (a tone
    as `100` or `100.0`, a code as `D023`, `D023N` or `D023I`), refusing commas with a hint, empty entries, nonstandard
    values and a signal listed twice by entry number, never echoing the text; `dsd_tone_set_format()` writes it back as
    written and `dsd_tone_set_format_display()` with units and a `…+N` count for what does not fit
    (`dsd_tone_display_to_ascii()` respells that mark `...` in place, the same three bytes, for ASCII surfaces: the
    terminal's menu label, a toast, a terminal without UTF-8);
    `dsd_tone_set_contains_ctcss()` / `_dcs()` match a code by its signal (`dsd_dcs_canonical()` on both sides), so a
    listed D023I matches a received D047N. `dsd_tone_set_same_signals()` compares two lists by what they pass, a code
    under either spelling, for every question of whether two policies are the same (the tone policy's reconfigure, the
    `-Y` same-frequency row check, the unheard-policy warning); `dsd_tone_set_equal()` compares them as written.
    `dsd_tone_filter_check()` checks a whole policy as the live editor sets it, mode and list: allow and block need a
    list, off takes one or none, the parser refuses the rest with its entry-numbered reason; the tone-filter command and
    the Qt editor's inline message both ask it.
    `RUNTIME_ANALOG_TONES` pins the table value by value, and both predicates case by case. DCS (issue #523):
    the standard 104-code set, `dsd_dcs_word()` (any 9-bit code's 23-bit Golay word: code, the fixed 100, 11 check bits;
    bit 0 sent first, a multiple of `0xC75`; inverted polarity is the complement), `dsd_dcs_match()` (names the signal
    in 23 received bits when some rotation, in either polarity, is a supported code's word), `dsd_dcs_canonical()` (the
    alias rule: normal polarity first, then the lowest code, so every inverted standard code is named by its one normal
    alias, D023I by D047N), `dsd_dcs_alias()` (the signal's other standard spelling, its one inverted code: D047I for
    D023N) and the `D023N` / `DCS D023N / D047I` formatters, leading zeros always. A received code is labelled with both
    spellings, canonical first, because a receiver cannot tell which one a transmitter was set to. The DSP detector
    cannot link `dsd-neo_fec`, so runtime has its own encoder; `DSP_ANALOG_DCS_GOLAY_XCHECK` checks every word against
    `Golay24.hpp` through the explicit layout mapping (reversed 23 bits; Golay24's encoding of the reversed data rotated
    left by 11) and `RUNTIME_ANALOG_TONES` pins the published reference words, the golden alias table for all 104 codes
    and the two spellings of every signal.
  - Decode presets (`include/dsd-neo/runtime/decode_mode.h`, `src/runtime/decode_mode.c`): the `-f` selector map is
    a table (`k_cli_presets`), and AM (`DSDCFG_MODE_AM` = 16, `-fM`, issue #524) is the last preset: the analog monitor
    with `analog_demod` AM, read back as AM by `dsd_infer_decode_mode_preset()`. AM needs an I/Q radio input, one rule
    for two phases: before the engine opens the input, `dsd_decode_mode_input_spec_is_iq()` reads the spec (an rtl,
    rtltcp, soapy, airspy or iqreplay one, or the RTL input type `--iq-replay` sets while the options are parsed) for
    the CLI (which stops on `-fM` with a PCM input, and falls a config's `decode = am` back to Analog with autosave off
    and a toast saying so, `dsd_decode_mode_keep_saved_am()`, which a runtime config apply of one shares) and the
    setup wizard (which asks it about the input it just configured, and also holds the AM width to the DSP bandwidth
    it just read, as the engine's pre-open check does: `interactive_am_refused()` in
    `src/runtime/bootstrap/interactive.c`); once an input is
    open, `dsd_decode_mode_input_is_iq()` and `dsd_decode_mode_runs_on_input()` go by the input type alone
    (`dsd_opts_input_is_radio()`), since a live switch to TCP audio keeps the old device string and a replay session
    switched to Pulse keeps its request. All of them say `DSD_DECODE_MODE_AM_NEEDS_IQ_TEXT`.
    `--am-bandwidth-hz` and `[analog] am_bandwidth_hz` carry the AM width (whole Hz, 5000..20000, 0 = the 6000
    default, saved only when explicit; the section is always saved). Tests: `RUNTIME_DECODE_MODE`,
    `RUNTIME_CLI_PARSE`, `RUNTIME_CONFIG_USER`, `CONFIG_VALIDATION`, `CONFIG_TEMPLATE`,
    `RUNTIME_BOOTSTRAP_INTERACTIVE`.
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
  - Session input failure latch (`include/dsd-neo/runtime/input_failure.h`, `src/runtime/input_failure.c`): the last
    failure of the session's input, kind and native code (`dsd_input_failure_report()`; `dsd_input_failure_clear()`
    at the session's start and when an Airspy opens or an rtl_tcp server connects), which
    `dsd_engine_run_with_lifecycle()` reads at the end to return 1 after a device failure. A mutex guards it, since
    device threads report. `dsd_input_failure_generation()` counts its writes, a clear included, so a caller can tell
    that something wrote the latch even when the write repeats the failure latched (the rollback restart,
    `svc_rtl_restart_recovery_locked()`). Test: `RUNTIME_INPUT_FAILURE`.
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
  path keep twice the profile's protected edge. `channel_analog_kind` names the demodulator kind (FM or AM, issue
  #524) behind an analog width and is -1 for every other width; the analog width view takes the front end's width only
  when that kind is the configured one, so an FM width still in force while a switch to AM lands does not read as the
  AM channel. Test: `APP_CONTROL_FRONTEND_METRICS`.
- Receive family on a live mode change: `svc_publish_symbol_profile()` (`src/app_control/symbol_profile.c`)
  publishes the configured analog profile for the analog family, which is what moves a running digital RTL front end
  onto the analog monitor. For a digital configured mode it requests the digital family before the symbol profile,
  deciding once by the rule a scan row's timing and the engine's attach follow (issue #583): when the row on air was
  timed for the digital family's landing (`dsd_scan_mode_timed_digital_family()`, a scoped command's resume included),
  or the front end still runs the analog family (which also covers a CQPSK toggle or a typed row's profile under it),
  or work outstanding lands a family (`rtl_stream_family_landing_after_pending()`: a retune that carries the digital
  family lands that prediction even on a front end already digital), it times the decoder with
  `rtl_stream_output_rate_for_family()`, with the CQPSK state a trunk-scan target keeps as its own
  (`dsd_scan_mode_cqpsk_explicit()`, 0 for a `-Y` row or a plain session, which also gives the CQPSK state the row
  runs: CQPSK off for a DMR or NXDN target whatever `rf_mod` a `-mq` lock left, which the symbol profile it publishes
  runs too, landing or not, on the GFSK chain's filter), because the landing happens on the demod
  thread after the command returns and the output rate until then is not the one it lands on, and it asks for that
  landing (`rtl_stream_request_digital_family_landing()`) rather than the plain family request, which switches only an
  analog front end: the republish is the newer word on the family, so it lands there whether a retune it supersedes
  lands after it (its centre only) or one landed before it. Otherwise the plain request, a no-op on a digital front
  end, keeps a digital-only session's republish as it always was. Under a scan row's constraint the configured mode is
  the scan baseline
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
  configured settings are still the ones the switch left; under a scan row through the scope's suspend and resume; a
  switch made over one still armed, such as AM then Analog before any command found AM taken, keeps the first one's
  snapshot, since the last-writer-wins requests never ran the settings in between while the first was queued; it also
  keeps the settings the first one left, by kind, and a refusal where the request landed that the stream records as
  keeping the analog family on that kind, the demod thread having taken the first request after the last check and
  before the second was queued, goes back to them instead, `ui_analog_entry_rollback()`; a width of the kind kept that
  changed while the switch was pending, and that the rate now cannot filter, goes back to the width the front end
  kept, `ui_hold_reverted_analog_width()`):
  at once for a refusal by the request, which fails the command (`svc_publish_symbol_profile()` returns -1), and before
  the next command, or at the next drain, for one where it landed (`svc_take_monitor_request_outcome()` reports the
  front end kept the digital family, `ui_settle_receive_requests()`, which `dsd_app_drain_cmds()` runs before each
  command).
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
- Analog channel width (NFM issue #525, AM issue #524): `DSD_APP_CMD_NFM_BANDWIDTH_SET` (508) and
  `DSD_APP_CMD_AM_BANDWIDTH_SET` (509), each int32 Hz with 0 for the default and a coalescible setter, run
  `svc_set_analog_bandwidth()` for their kind: a width outside 0 or the kind's range (NFM 8000..25000, AM 5000..20000
  Hz) is refused, and while the width is in use -- the configured analog preset runs that kind (the scan scope's
  configured view), or, for either width on any session, an analog scan row of that kind that runs the configured width
  (no width of its own) is on air or waiting in the scan (`dsd_engine_scan_runs_configured_nfm_width()`, `_am_width()`,
  through `svc_scan_runs_configured_width()`) -- `svc_check_analog_bandwidth()` holds it to `DSD_NEO_CHANNEL_LPF` on a
  radio input (PCM input runs no channel filter, so the width is only stored there) and to the running stream
  (`rtl_stream_check_analog_profile()`) or, with none, to an RTL-SDR/rtl_tcp input's DSP bandwidth; a refusal is a toast
  naming the width, the rate, the limit and the fix, and changes nothing (the validator's full text is logged). AM
  always runs its channel filter, so every one of these holds AM's unset default (6 kHz) as it holds an explicit width,
  where the unset NFM default is never refused. `svc_describe_analog_refusal()` words it: a DSP bandwidth on an RTL-SDR
  or rtl_tcp input; on a SoapySDR, Airspy or I/Q replay stream the demod rate the stream publishes
  (`rtl_stream_get_demod_rate_hz()`) with the fix for that source. The command is not scoped
  (`command_updates_scan_mode()`): like squelch, it edits the configured width (through
  `dsd_scan_mode_set_configured_nfm_bandwidth()` for NFM) instead of suspending and re-applying a row, which would read
  the row's live acquisition (a detected Phase 2 polarity, a followed call) as a change and end it. While the row on air
  sets its own width of the command's kind (issue #526: an nfm row's NFM width, an am row's AM width,
  `svc_row_sets_width()`), that width stays in force: the edit lands on the configured baseline only,
  reaches the front end with the next row that takes it or the leave, and the toast says the row overrides it
  (`dsd_app_analog_width_edit_notice()`, which names the kind the command edits). A refused width is put back through
  `svc_restore_analog_width()`: outside a scan the configured width takes the one the front end kept; under a scan row,
  where that can be a row's own width (the row on air's, or one a retune in flight had not moved the front end off yet),
  the configured width goes back to its value from before the refused change, which each monitor request records
  (`svc_monitor_refusal::configured_before_hz`), and only a row with its own width takes the kept width in force. A
  request that replaced an earlier change still queued (or refused and not yet collected) also records the width from
  before that one: the front end ran neither, so when it kept another width than the one from before the refused change,
  the configured width goes back to the one from before the first. That width is kept for each analog kind apart, from
  the first change of that kind, including a width of a kind not in force, which asks the front end for nothing
  (`symbol_profile_note_width_change()`), and any width a config apply changes, noted once its requests (the scope's
  resume included) are made (`svc_note_analog_width_change()` from `apply_cmd_scoped()`), since its [analog] can
  change a kind it asks nothing about or one a switch in the same config leaves: a refusal puts back only a width of
  the kind the front end kept, never one of the other kind (issue #524). An accepted width in force goes to a running front
  end whose options in force run that analog kind as a live analog profile request (`svc_publish_analog_bandwidth()` in
  `symbol_profile.c`), which also replaces the width a queued switch onto analog carries; a typed digital row keeps its
  profile until its leave, and under a scan row's suspended scope (a config apply) the request waits for
  `apply_cmd_scoped()` to publish it after the resume, with the width in force from before the command
  (`svc_publish_symbol_profile_changing_width()`). CQPSK toggled on under an analog preset holds it too, and the DSP op
  that turns CQPSK off (`svc_toggle_rtl_cqpsk()`) returns to the monitor through the analog profile alone, which turns
  CQPSK off as it enters the monitor; one the front end refuses, at once or where it lands (the AM default included),
  leaves CQPSK on. No CQPSK-off profile is queued with it: the demod thread could take that on its own at a block
  boundary before the analog request, and a refusal would then leave the FSK channel profile on the monitor output with
  the FM discriminator. Whether CQPSK holds the front end is what was last queued, not only what the stream publishes,
  since a toggle, a switch or a scan row's leave (queued through the runtime hooks) drained with the width has not
  landed yet: the stream answers for every request queued, whoever queued it (`rtl_stream_requested_cqpsk()`), falling
  back to the published state once every request has settled (`rtl_stream_receive_request_outcome()`), never on the
  output generation, which the demod thread moves before it publishes and a retune moves without taking a request. The
  toggle flips that requested state too. A width the front end refuses after the check is never left configured:
  refused by its request (a retune moved the published rate), the
  change is refused with the previous width put back; refused by the demod thread where it lands, the request reads
  `RTL_STREAM_RX_REQUEST_REFUSED`, and `dsd_app_drain_cmds()`, before its next command, puts back the width the front
  end kept, as the stream recorded it when it refused (`rtl_stream_receive_request_refusal()`, so an earlier width that
  landed in between stands; the stream records the configured setting it kept, `demod_state::analog_width_setting_hz`,
  0 for the default, so an unset AM default goes back as the default and an explicit 6000 Hz stays explicit), and
  toasts why
  (`svc_take_monitor_request_outcome()`, which follows the last analog monitor request `symbol_profile.c` queued, and
  `ui_settle_receive_requests()`). A scan leave's return to the monitor is followed the same way (issue #578): every
  interactive leave (a scanner stop, a tuner release, trunking taking the tuner, a manual tune off a typed list, a
  channel-map adopt or clear, a RadioReference import, the leave after a command that stopped the scanner) calls
  `svc_leave_channel_scan()`, which reads whether the last request had reached the front end before the engine's leave
  queues its own (so the `first_configured_before_hz` chain holds) and records a leave that asked for the monitor. A
  leave the front end refuses at once queues nothing: the record keeps the number of the request before it, and once
  that one has settled what the stream publishes says what the front end kept (the monitor's kind and the width its
  channel filter runs, one the DSP rate limits reading as the default; off the monitor, the digital family, or the
  kind and width setting the analog family runs, `rtl_stream_get_analog_setting()`, which a refusal where it lands
  records too). Off the monitor that kind is the family's own, never the one the leave asked for: a mode change under
  a typed row moves the configured kind (AM to Analog, or the reverse) while the front end keeps the kind it ran, so
  a switch armed there goes back in either timing rather than being dropped as made.
  A monitor that runs what the leave asked for (a retune in flight left it there) refused nothing, and a leave for the
  kind's default counts a monitor of that kind whose filter runs the default's design as running it (the AM default
  publishes the 6 kHz it filters, which would otherwise read as another width kept).
  One refused where it lands says it from the stream's record, which notes whether the front end kept the monitor
  output. The record goes with the stream it was made of: once app-control starts another (a restart, a reopen, an
  input switch, a rollback's restart; `svc_rtl_start_count()` moved), it reads taken, since the new stream opened on
  the options it was given and its open settled what the old one left queued, so a leave refused at once is never read
  against what the new stream publishes (whose AM default monitor, publishing the 6 kHz its filter runs, would read as
  another refusal). Either way, a request queued after the leave's, from anywhere, that does not carry it on (a CQPSK toggle made
  before the demod thread took the return, a symbol profile) decides the front end instead
  (`svc_monitor_refusal::superseded`): the refusal is left to it, with nothing asked, the configured width as it is
  and no toast, since asking for the default would turn that CQPSK off again. A switch onto the monitor armed before
  the leave (a mode change onto Analog under a typed digital row, which asks the monitor for nothing until the leave)
  is left to it too, rather than going back and publishing the old mode's profile over it: the later request is no
  monitor request (one would carry the leave on), so the front end has made that switch no more than before, and it
  stays armed for the next monitor request's outcome to settle (dropped once one is taken, gone back when one is
  refused off the kind asked for), or for a revert that finds the configured settings moved on to drop. The same holds
  for a leave whose scan goes on (`ui_scan_goes_on()`: a scan scope in force or updating, or a channel-map adopt or
  RadioReference import that keeps the scanner on with rows to visit): the next row's tune decides the front end, and
  going back to the old mode would undo a switch over a return the scan has moved on from. Both are checked once, before
  anything else the drain does with the refusal (`ui_scan_leave_left_to_later()`), whatever the front end kept: an
  `-fA` session whose typed row runs on the analog family and whose mode went to DMR and back under the row keeps
  analog FM, the kind the leave asked for, which would otherwise drop the switch as made, leaving no mode for a later
  return refused off that kind (the scanner stopped on an AM row) to go back to. A return the demod thread never took
  is no more taken than refused: a later request that does not carry the leave on replaced it in the stream's queue,
  or the next row's tune retired it (a scan going on to an `nfm` or `am` row, whose receive family lands before the
  demod thread takes the return). The stream reads it replaced (`RTL_STREAM_RX_REQUEST_REPLACED`), not settled as one
  the front end ran, and the record superseded (`SVC_MONITOR_REQUEST_SUPERSEDED`): nothing is reconciled and an armed
  switch stays armed, where reading it taken would drop the switch, and a later return refused off the kind asked for
  would ask for the NFM default rather than go back to the old mode. Any monitor request the stream replaced or retired
  reads so, a leave or not. A typed digital
  row's channel profile never touches the width setting, so the drain reconciles "kept width, else default"
  (`ui_settle_refused_scan_leave()`): a front end still on the monitor of the leave's kind gives the configured width
  the width that monitor runs (`svc_restore_analog_width()`); one off it (a typed row's profile, the other kind's
  monitor, the digital family) is asked for the kind's default, which is stored as the configured width, straight
  through `svc_publish_symbol_profile_after_scan_leave()` (`svc_publish_analog_bandwidth()` would wait for the CQPSK a
  kept P25 row requested). The NFM default is never refused; an AM default the rate refuses as well changes nothing.
  That default request carries the leave on, with the width from before it, so an AM default refused where it lands (a
  retune in flight moved the rate again) is reconciled as a refused leave too: the configured width goes back to the
  one from before the fallback and the toast says the default does not fit either, rather than an unarmed analog
  entry reverting nothing; behind a later request it is left to that request as above, keeping the default the
  leave's toast named. The toast (`svc_describe_monitor_return_refusal()`) names the width, the rate the stream
  holds requests to (`rtl_stream_get_request_rate_hz()`: not the one `rtl_dsp_bw_khz` gives, which a row's retune has
  left, nor the metrics rate, which follows only with the next I/Q block) and what the monitor runs: `Refused:
  NFM 16 kHz does not fit the 16 kHz DSP rate; the monitor is back on the NFM default`. It is settled only while the options are still the ones
  the leave put back (the analog family, that kind, that width) and no scan runs: a channel-map adopt or RadioReference
  import that keeps the scanner on gets its front end from the next row's tune, and is deliberately left to it. A width
  change made while the leave was still queued replaces its request and carries the leave on; a switch onto the monitor
  armed before it reverts first (`ui_revert_analog_entry()`) when the front end kept the digital family or the other
  kind, and is dropped when it kept the analog family of the kind asked for, unless a later request superseded the
  leave or the scan goes on, as above. That revert is part of the leave: its request for the kind it puts back carries
  the leave on (`svc_publish_symbol_profile_after_scan_leave()`), so when the rate the row left cannot filter that
  kind's width either (NFM 16 kHz after a switch to AM under an NXDN48 row, left at 7.5 kHz), the refusal, at once
  (`ui_settle_leave_revert_refused_at_once()`, from what the front end kept for the leave) or where it lands, is
  reconciled for that kind by the same rule: the width its monitor kept, else its default (`Refused: NFM 16 kHz does
  not fit the 7.5 kHz DSP rate; the monitor is back on the NFM default`, replacing the revert's `Failed:` toast), an AM
  default the rate refuses too leaving the width as it is, rather than an analog decoder on a width the front end
  cannot filter over the row's digital profile. A width of that kind changed while the switch was pending, which the
  rate cannot filter, goes back to the one the front end holds first (`ui_hold_reverted_analog_width()`), and the rule
  settles what the revert then asked for. A DSP-menu CQPSK-off refusal is no leave and keeps CQPSK on. A typed row's retune still in flight when a leave back to Analog or AM was
  accepted lands its centre only: the leave's analog request supersedes the symbol profile that retune carries, which
  would take the monitor away (Per-channel decoder modes, above; issue #582), so the monitor the leave put back stays.
  A leave to a digital configured decoder makes no analog request, but the leave supersedes such a retune before its
  requests all the same (Per-channel decoder modes, above), so the configured decoder's profile stays too. A switch to Analog or AM (`DECODE_MODE_SET`, a config's `[mode]`) holds an explicit
  width, or the AM width, to the rate first, under a scan row as well (`ui_check_mode_receive_profile()`); a `[mode]`
  without a decode key keeps the session's family and kind. A switch between FM and AM on the monitor is armed like a
  switch onto it (`ui_arm_analog_entry()`), and the stream records the kind it kept with a refusal, so one the front end
  refuses, at once or where it lands, puts the decoder back on the mode it had (`Failed: AM -> ...`), while a width
  refused after the switch landed restores the width. `DSD_APP_CMD_CONFIG_APPLY` holds the width it leaves in force (an
  explicit one, or AM's; `cfg_analog_kind_after()`, `cfg_check_held_analog_width()`) to the rate it will run at before
  applying anything: `svc_check_analog_bandwidth_for_rtl_bw()` at the `rtl_bw_khz` a hot restart stores, as given, when
  the `[input]` builds an RTL-SDR or rtl_tcp spec other than the running RTL-family input's (`cfg_radio_reopen()`),
  otherwise the check above; one that reopens a SoapySDR or Airspy device (an Airspy `[input]` over a running Airspy
  reopens it for a new sample rate, serial or DSP bandwidth, `svc_airspy_settings_reopen()`) is held only to the rules
  every rate shares (`svc_check_analog_bandwidth_at_device_rate()`) and left to that stream's start, which checks the
  width at the rate the device delivers. An Airspy reopened for its monitor volume alone, which the stream copies only
  when it opens, delivers the rate it runs now, so the running front end holds the width as when nothing reopens
  (`CFG_REOPEN_AT_RUNNING_RATE`, issue #578), and holds it even where the config leaves it as it was: the reopened
  stream opens on the configured width, which a typed digital row on air leaves unused, and a DSP bandwidth lowered
  under that row can leave too wide. Since the stream a reopen starts runs the scan row on air again once the
  scope resumes, an analog row's own width is held to the reopened rate the same way (`cfg_check_scan_row_width()`, for
  the row's kind), and on a session the config leaves on another kind or none the configured width of a kind is held
  while the scan has an analog row or target of that kind without a width of its own (`cfg_check_scan_widths()`). An
  `rtl_bw_khz` above every selectable bandwidth that no width fits is refused as the setting, not as a
  rate. `svc_rtl_set_bandwidth()` (RTL_SET_BW) refuses a DSP bandwidth the configured preset's explicit width, or AM's
  width, cannot run at on an RTL-SDR or rtl_tcp input, naming both and saying to narrow the width first (or, at a
  bandwidth that filters no width of the kind, to leave the NFM width unset, or keep a wider bandwidth for AM), and
  `DSD_APP_CMD_RTL_ENABLE_INPUT` (Input > Switch source > RTL-SDR) asks `svc_check_rtl_input_analog_width()` before it
  rewrites the input and tears the running stream down, refusing a width the DSP bandwidth of the device it opens cannot
  filter; `DSD_APP_CMD_AIRSPY_ENABLE_INPUT` (Switch source > Airspy) asks `svc_check_airspy_input_analog_width()`, which
  holds the same widths only to the rules every rate shares, since the Airspy sets its own rate (issue #578). Its start
  then holds only the width it opens on to the delivered rate (`rtl_demod_finalize_analog_channel()`): the switch is
  unscoped, so under a scan row that runs a digital protocol it opens on the row's digital profile and checks no analog
  width, and a configured width that rate cannot filter is left to the scan: its leave's refused return to the monitor
  is reconciled (`ui_settle_refused_scan_leave()`), and an analog row or target that runs it is skipped at every visit.
  Every change that commits to a radio stream start with an explicit width, or with AM, refuses it first while
  `DSD_NEO_CHANNEL_LPF=0` (`svc_analog_width_env_allows()`), since the start would refuse it at any rate. A switch whose
  start fails anyway (a width the rate the device delivered cannot filter, a device that does not open) never leaves
  the session without the input it had: the input a start reads is captured before
  the rewrite (`ui_radio_input`: device string and type, rtl_tcp endpoint, device index, tuning, gain, ppm, squelch,
  volume, resample policy, Airspy and SoapySDR settings) and put back, a PCM input as it was (the switch never closed
  it, and a background RTL stream it left running, which the failed start stopped, is not restarted, its I/Q capture
  stopping with it as below) and an RTL-family input that ran restarted; it is
  not reset as a new input (`ui_input_switched()`). The P25 SM watchdog reads the input, and may retune it, under
  its tick guard, so the switch holds that guard from the copy of the input it would put back (a copy taken before
  it could miss a retune the watchdog completed meanwhile, which the rollback would undo) through its rewrite of the
  input, the start and any rollback (`svc_rtl_enable_input_locked()`, `ui_radio_start_failed_locked()`), as a
  config apply's reopen does; an Airspy settings reopen (`svc_airspy_apply_config()`) holds it across the reopen and
  the restart of the settings it replaced the same way. `RTL_SET_BW` does too (issue #578): its pre-check holds the
  width only to an RTL-SDR or rtl_tcp input's DSP bandwidth, and a SoapySDR or Airspy device delivers a rate the new
  bandwidth sets that no check knows, so the handler copies the input under the guard, stores and reopens through
  `svc_rtl_set_bandwidth_locked()` (which leaves a failed start with no stream and an empty reason), and a start that
  fails puts the bandwidth and input that ran back through `ui_radio_start_failed_locked()`, toasting `Refused: <why>`
  or `Failed: <why>`. These are the commands with a rollback: Input > Switch source, RTL_SET_BW, AIRSPY_SET and a
  config apply's reopen; a gain, device index or explicit stream restart that fails still leaves no stream. Every one
  of these rollback restarts, and the config apply's below, goes through `svc_rtl_restart_recovery_locked()`: the failed change already stopped the stream that ran,
  which closed its I/Q capture with what it had recorded, and a start reopens the capture file for writing, so the
  restart runs with `iq_capture_requested` cleared and leaves the capture off for the session. A PCM input put back
  starts nothing, but a radio stream that ran behind it, recording (a switch to PCM leaves it running;
  `ui_radio_input::stream_running`, the recovery's `stream_stopped`), was stopped the same way, so the capture stops
  then too, rather than let the next radio start of the session write over that stream's recording. The failed start can
  also have latched a failure of its input for the session (an Airspy that did not open latches
  `DSD_INPUT_FAILURE_DEVICE`, which makes `dsd_engine_run_with_lifecycle()` return 1 at a normal end), and an RTL-SDR,
  SoapySDR or PCM input put back clears none, so the restart puts back the failure latched before the change
  (`ui_radio_input` takes it with the input, and `svc_airspy_reopen_impl()` before its reopen) once the stream the
  change left is stopped and before it starts anything (a PCM input starts nothing): a failure the restarted stream
  latches stands, even one an Airspy's monitor thread latches before the start returns, and a restart that fails keeps
  its own failure, even the very one put back (an Airspy that again does not open), or the change's when it latched
  none (it wrote nothing to the latch, or left it clear: an Airspy that opened, then failed), with no input running.
  What the restart wrote is told by the latch's write count (`dsd_input_failure_generation()`), read right after the
  put-back, not by its value. Its log names the file
  as kept only when the start the change made never opened the capture file (`rtl_stream_start_opened_capture()`: a
  width its analog check refused, a device that did not open, a capture writer that failed before it opened the file);
  a start that opened it before failing (its workers, an Airspy that did not stream) or that ran and was undone had
  already written the file anew, and the log says that instead. The toast notes `; I/Q capture stopped` when it fits (`ui_set_rollback_toast()`), AIRSPY_SET's too
  (`Failed: Airspy setting; I/Q capture stopped`, from `svc_airspy_apply()`'s `out_capture_stopped`). The toast is `Refused: <why>` for a width refusal and
  `Failed: <why>` otherwise, from `svc_describe_start_failure()`, which reads the refusal the start recorded
  (`rtl_stream_start_analog_refusal()`) while the options still describe the input that failed: the environment rule,
  or the width against the rate the device delivered with the fix for what sets it, or else the input that did not
  start (Airspy, SoapySDR, rtl_tcp, I/Q replay, RTL-SDR). Every one of
  these classifies the input as the stream's `detect_radio_source()` does (`dsd_app_analog_rtl_bw_rate_hz()`): an
  `rtl`/`rtltcp` spec, or any device string on an RTL input that names no SoapySDR, Airspy or replay device (Input >
  Switch source > RTL-SDR leaves `pulse` there), runs at `rtl_dsp_bw_khz`, saturated rather than overflowed for a loaded
  config's out-of-range `rtl_bw_khz`. `svc_check_analog_bandwidth()` holds a width to no rate on a PCM input, which runs
  no channel filter, whatever device string a live switch from an RTL-SDR left there. A config apply whose reopen of
  the running RTL-family input fails to start (the live Airspy reopen, `svc_airspy_reopen_locked()`, or the hot restart
  for a new spec, `apply_cfg_rtl_hot_restart()`, both from `apply_cfg_radio_input()`) never loses that input either
  (`ui_cfg_settle_reopen()`, issue #578): it takes the reason from `svc_describe_start_failure()`, puts back what the
  apply found once its checks passed (`ui_cfg_rollback`: the `ui_radio_input` above; the configured receive settings,
  meaning `[mode]`, `[demod]` and both `[analog]` widths, restored over the row-scoped options in force by
  `ui_restore_receive_settings()`, which puts back `[analog]`'s tone policy from among those options as well; and what
  `[mode]` sets outside that snapshot that the decoder reads, `ui_cfg_mode_extras`: `ea_mode` and `esk_mask`, which
  `edacs_ea`/`edacs_esk` and the EDACS/ProVoice preset set, and the extra LRRP ports; the audio layout a preset carries
  is the session's anyway), restarts that input, toasts `Config not applied: <why>` and fails the apply. What
  runs after it (the other hot restarts, the output reconfigure, `apply_cfg_receive_family_change()`) sees the receive
  side the session ran, so no analog entry is armed and no profile is published for the refused settings. Under a scan
  row the apply, and so its reopen or rollback restart, runs inside the suspended scope, whose options are the
  configured settings. The reopen's stream opens on those, so its start holds the configured analog width to the rate
  it delivers even while a digital row runs, as every reopen holds that width (the scan leaves back to that monitor),
  and a width it refuses there rolls the config back. The rollback's restart opens on what the stream it replaces ran
  instead (`svc_rtl_restart_recovery_locked()`, from `dsd_scan_mode_suspended_effective()`), with the configured
  settings put back once it has: a typed digital row's profile, say, whose old rate need not run the configured width
  (a DSP bandwidth lowered while that row ran), which a restart on the configured monitor would refuse, leaving no
  input. Either stream opened on settings other than the row the resume puts back, and the row then compares unchanged
  when the scope resumes, so
  `apply_cmd_scoped()` notes any stream started meanwhile (`svc_rtl_start_count()`, which `svc_rtl_restart_locked()`
  counts) and `ui_resume_scope_and_publish()` publishes the row's effective profile to it anyway, without ending the
  decoder's acquisition (issue #578; this also covers a reopen that starts, issue #583's first item). A command that
  leaves the scope in force (Input > Switch source, `RTL_SET_BW`, `AIRSPY_SET`, a gain, device index or explicit
  restart) starts its stream, and the restart its rollback makes, on the row's settings in force, but an open picks its
  CQPSK state by its own rule (`DSD_NEO_CQPSK` when set, else the QPSK flag of the options), not the CQPSK state the
  row's tune applied and the decoder runs (`state->rf_mod`): under `DSD_NEO_CQPSK=1` a C4FM row would come back on
  CQPSK. So `apply_cmd_scoped()` asks any stream such a command started for the row's effective profile at once
  (`ui_publish_row_to_started_stream()`, the same `svc_rtl_start_count()` note); the publish ends no acquisition and
  resets nothing as for a new input (a rollback's restart runs no `ui_input_switched()`), and an analog row's profile
  is the monitor the stream opened on. Each of these publishes,
  and the one for a row the update changed (a `[demod]` that turns a P25 row to CQPSK), is timed by one rule for the
  rate the new stream runs the row at (`ui_started_stream_rate()`), not with the timing the decoder kept from the old
  stream nor the one the resume took from the rate the new stream opened at (a CQPSK row read
  at the symbol-rate output has one sample per symbol, which the request would clamp to 2). A publish that lands the
  digital family's landing (a front end still on the analog family under a digital configured mode, a row timed for
  that landing, or work outstanding that lands a family) lands where an open of the mode would (the CQPSK family
  `DSD_NEO_CQPSK` names when set, unless the row is a trunk-scan target that makes its own choice, with an output
  chain designed for it), and times the row at that rate itself (`svc_publish_symbol_profile()`, which decides the
  landing once and asks for it, issue #583), so `ui_started_stream_rate()` times only a publish that lands none. That
  one applies the row's own CQPSK state (`dsd_scan_mode_cqpsk_explicit()`: the decoder's, except that a trunk-scan DMR
  or NXDN target runs CQPSK off whatever `rf_mod` a `-mq` lock left it on, and is timed as the FSK row it is asked to
  be), whatever `DSD_NEO_CQPSK` says, over the output chain the stream opened
  on, on the digital family or under the analog family a typed row runs on an analog session: a CQPSK row's timing
  loop runs at the demod rate the stream published at its start (`rtl_stream_get_request_rate_hz()`), not at the
  resampled output of a stream that opened on the FSK discriminator or the monitor, and an FSK row's samples leave at
  the rate the stream delivers now (only an open, a family switch, a retune or a landing designs the resampler again),
  which times it and anything else. For the same
  reason a reopen that starts under a scan row held only the configured settings to the rate the new device delivers,
  so before the scope resumes the new stream is asked for the analog monitor the row puts back
  (`ui_scan_row_resumed_monitor()`: the kind of the row's class, read with `dsd_scan_mode_row()` while suspended, and
  the row's own width or the configured one; `svc_check_started_stream_analog()` at the rate the stream published when
  it started). A config that turns the conventional scanner off leaves the scope rather than resume it, by the rule
  `apply_cmd_scoped()` leaves it by (`ui_cmd_leaves_scanner_scope()`: the scanner on before the apply, as
  `ui_cfg_rollback` records it, and neither it nor a trunk scan on after), so the row it leaves is not asked of the new
  stream. A SoapySDR or Airspy device sets a rate no check could know up front, so one that refuses it (an nfm
  row's own 25 kHz at an Airspy's 19,531 Hz) fails the reopen as a failed start does, rolled back with `Config not
  applied: <why>`, rather than leave the decoder on the row's monitor over a front end on the configured settings. The
  rest of the config (output, trunking, logging, alerts, recording and DSP, an imported group list, the environment
  defaults it set) stays applied. The configured PPM never outlives a rollback: an RTL-SDR or rtl_tcp reopen
  opens with the request made before it, which the rollback puts back with the input, and the Airspy path requests it
  only once the reopened Airspy runs. With no stream running before the apply the config's input is still opened (the
  hot restart, or an Airspy source over a stopped Airspy through `svc_airspy_reopen_locked()` rather than the Airspy
  path's own rollback, which would start the settings it replaced); a start that fails has nothing to put back, so the
  config stays applied, the toast is `Config applied; no input running: <why>` and the apply fails, as a failed Switch
  source does, and a start whose stream refuses the scan row keeps the input it opened, with `Config applied; the scan
  row cannot run: <why>`, and fails the apply. That stream opened on the config's receive side, so a switch onto the
  monitor or between FM and AM the same config made is not left armed as one the front end could refuse, and the row's
  own refusal when the scope resumes is the one the toast reports (`ui_cfg_note_refused_row()`, which the scoped
  command's resume reads as `UI_SCOPE_STREAM_REFUSES_ROW`, `ui_scope_stream_of()`): it leaves the configured mode, the
  configured widths (a row without a width of its own runs the one the config set, which may be the width refused) and
  the toast as the config set them. Tests: `APP_COMMAND_QUEUE`, `UI_MENU_SERVICES`, `UI_MENU_AIRSPY_CONFIG_REFUSED_WIDTH`,
  `ENGINE_CHANNEL_SCAN` (the leave's result), `IO_RTL_DEMOD_CONFIG` (request numbering and outcomes, the kept kind and
  monitor output, and the kind and width setting published off the monitor).
- AM (issue #524): `DSD_APP_CMD_DECODE_MODE_SET` takes `DSDCFG_MODE_AM` (the preset ids end there, as do the
  RadioReference import's) and refuses it on a PCM input (`dsd_decode_mode_runs_on_input()`); on a running RTL session
  it holds the AM width to the rate (above) and switches live across AM, Analog and the digital modes. A switch between
  FM and AM, by DECODE_MODE_SET or a config's `[mode]`, drops the analog monitor block the decoder part-collected, as a
  family change does (`decode_mode_drop_old_analog_block()`), and until the demod thread applies it `getSymbol()`
  collects none of the old kind's audio the front end still delivers (issue #582, the monitor block in `dsp`); one
  the front end refuses goes back to the monitor's raw sink, not the digital one (`ui_revert_analog_entry()`), also
  after a toggle that asks the front end for no receive profile (the cosine filter, an inversion, the input monitor)
  changed a setting the switch is compared by before the refusal: what the toggle set stands (issue #582). `[mode] decode = am` in a config applied to a PCM session
  applies, and then falls back. That fallback is `apply_cmd_fall_back_from_am_on_pcm()`, run after every command
  (`apply_cmd_scoped()`): a configured AM preset on an input that is not I/Q (a live input switch to Pulse, a file, TCP
  or UDP audio, or a config's `decode = am` on PCM) becomes the Analog monitor through the scope, as DECODE_MODE_SET
  would make it, whatever its NFM width (no rate holds a width on PCM input), with the reason in the log and the toast.
  After a config apply whose own `[mode]` is `am` it also turns autosave off (`dsd_decode_mode_keep_saved_am()`), as a
  start with that config does, so the loaded file keeps `decode = am`; a live input switch leaves autosave on. Neither
  width command is scoped (`command_updates_scan_mode()`): each edits the configured width as above rather than
  suspending and re-applying the row. Tests: `APP_COMMAND_QUEUE`, `UI_MENU_SERVICES`.
- Tone filter (issue #527, the live editor): `DSD_APP_CMD_TONE_FILTER_SET` (510) carries the whole configured CTCSS/DCS
  policy, `dsd_app_tone_filter_payload` (the `dsd_tone_filter_mode` and its list as typed, NUL-terminated in
  `DSD_APP_TONE_FILTER_LIST_SIZE` bytes), submitted with `dsd_app_command_set_tone_filter()`, which refuses a list the
  payload cannot hold rather than cut it. `dsd_app_command_set_tone_filter_mode()` sends the mode alone (`keep_list`,
  the terminal's Off): the handler takes the configured list on the decoder thread when the edit runs, so an edit or a
  loaded config queued before it is what it keeps, never a frontend's older snapshot of the list; with none configured
  it is judged as an edit without a list. Not a coalescible setter: tone edits are deliberate text entries, and each is
  judged on its own, so a refused edit (a typo) never discards a valid one queued before it.
  `ui_cmd_handle_tone_filter_set()` refuses a malformed payload (no terminator, `keep_list` other than 0 or 1, or with a
  list), and a policy `dsd_tone_filter_check()` refuses with a toast naming why (`Refused: tone filter: ...`, entries
  by number, never the text), changing nothing; it stores an accepted one through
  `dsd_scan_mode_set_configured_tone_policy()` and toasts `dsd_app_tone_filter_edit_notice()`. Off keeps the list it is
  given. The command is not scoped (`command_updates_scan_mode()`), for the reason the width commands are not: it edits
  the configured policy, and an nfm row's own tone options stay in force until the row leaves, the notice saying so. A
  save writes the configured policy (`snapshot_analog_config()`), and `apply_cmd_scoped()`'s unheard-policy warning
  covers it as it covers a loaded config. The DSP tap reconfigures its policy from `dsd_opts` at every read, so the
  edit takes effect at the monitor's next read of audio. Tests: `APP_COMMAND_QUEUE` (allow, block, off with and without
  a list, refusals, malformed and overlong payloads, queued edits judged apart, the mode alone keeping the list a queued
  edit or config set, a shadowed edit under an nfm row and its save, a P25 row's live acquisition kept, the warning),
  `UI_QT_CONTROLLER` (through the Qt bridge).
- Shared display decisions, so no frontend has to restate one: `include/dsd-neo/app_control/call_view.h` and
  `src/app_control/call_view.c` fold the canonical call state into a per-slot line, and
  `include/dsd-neo/app_control/scan_timing_view.h` and `src/app_control/scan_timing_view.c` fold
  `dsd_state::scan_timing` into the Scan Timing row — the stay phrase, the remaining/total of the window that is
  running, and which of dwell/hold/hang is worth printing (issue #508), including `Carrier` for an analog row's carrier
  hold (issue #526). The decoder owns every deadline; these views only difference it against the caller's reading of the
  decode clock's monotonic time, the clock the deadline was stamped on, which is what keeps the terminal row, the Qt
  panel and the Android app from drifting on what "suspended" or "hold" means. Tests: `APP_CONTROL_CALL_VIEW`,
  `APP_CONTROL_SCAN_TIMING_VIEW`, and the terminal goldens in `UI_NCURSES_PRINTER_HELPERS`.
  `include/dsd-neo/app_control/squelch_view.h` and `src/app_control/squelch_view.c` (issue #521) pair the squelch in
  force with the configured default, say whether a scan row overrides it and whether each level is off: the terminal SQL
  field and M17 VOX field (`-60.0 dB (row; default -80.0 dB)`), the DSP panel's `(row)` mark, the shadowed-edit toast,
  and Qt's `configuredSquelchDb`/`effectiveSquelchDb`, `configuredSquelchOff`/`effectiveSquelchOff`,
  `squelchRowOverride` and `squelchReadout` all come from it. The Qt radio panel lays those out as its whole-dB stepper
  reading, a `row` badge and `default X`, and uses `squelchReadout` as the reading's accessible name. It reads the row's
  value from `dsd_scan_mode_row_options()`, so it is also right on the decoder thread while a command has the scope
  suspended. The auto squelch (issue #518 follow-up) adds whether each setting is AUTO and its margin, why AUTO is off
  here, and what the stream shows (`dsd_state::squelch_auto_*`: running, learning, plan, gate, floor): `auto +10 dB
  (floor -78.3 dB)`, `(learning)`, `(off: no radio input)`, `(off on digital)`, `(off: no channel plan)`, and the row
  form `auto +6 dB (floor -81.0 dB; row; default -60.0 dB)`. On audio input (issue #628) the view resolves through
  `dsd_squelch_setting_resolve()` with the audio input kind and adds the PCM noise squelch's state
  (`dsd_app_squelch_view::noise_state`): `noise +10 dB (learning)`, `(quieting 23 dB)`, `(off: no band above voice)`,
  `(off: no room above voice)`, `(off on AM audio)`. `dsd_app_squelch_view_dynamic_status()` is that status
  alone (Qt's `squelchAutoStatus`, the line under the reading; the noise squelch's too, below),
  `dsd_app_squelch_view_configured_text()` the default as the terminal prompt opens on it (`auto+10`, `-60.0`, `off`);
  Qt's `configuredSquelchAuto`/`effectiveSquelchAuto` and their margins (kept under a level setting) drive the radio
  panel's `dB | Auto` choice, whose buttons step the margin in Auto; dB puts the default back on the level it kept
  (`CommandBridge::restoreSquelchLevel()`, the stored level whole, a legacy linear one included;
  `configuredSquelchLevelOff` says whether it is off). The channel-map review and target
  preview carry a row's own auto squelch as `squelch_margin_db` (`dsd_csv_channel_profile`, `dsd_app_scan_csv_target`;
  Qt `squelchMarginDb`). `DSD_APP_CMD_RTL_SET_SQL_SETTING` (`dsd_app_squelch_setting_payload`,
  `svc_rtl_set_sql_setting()`) sets a whole setting on the configured default as `RTL_SET_SQL_DB` sets a level (it
  coalesces only with a queued request of the same mode, so an AUTO request's margin and a LEVEL one's level both land),
  and a scan row edit carries `squelch_mode`/`squelch_margin_db` (AUTO on an nfm or am row only; a level edit without a
  margin keeps the one the row had, its list's or the last edited, for a switch back to Auto). The noise squelch adds
  `effective_noise`/`configured_noise` beside the AUTO flags and what it shows (`dsd_state::squelch_noise_*`):
  `noise +10 dB (quieting 23 dB)`, `(starting)`, and `(as auto: floor -78.3 dB)` where the floor tracker runs a NOISE
  setting (an AM channel, a channel with no band above voice), which `dsd_app_squelch_view_dynamic_status()` gives
  alone as it does AUTO's. Qt adds `configuredSquelchNoise`/`effectiveSquelchNoise` and `squelchNoiseOffered` (false on
  the AM monitor on its own), and the radio panel's choice becomes `dB | Auto | Noise` (Noise on an nfm row only,
  `CommandBridge::setSquelchNoise()`); the previews carry `squelch_mode` (Qt `squelchNoise`) beside the margin. On
  audio input the metrics model publishes the same squelch values, flagged `squelchAudioInput`: the monitor's
  `SQUELCH` row reads `squelchReadout` and opens `SquelchSheet.qml` (`dB | Noise`, no Auto; "This channel" under a
  row that takes the edit), and the terminal's Input > `Squelch...` row (offered on radio and audio input,
  `io_squelch_offered()`) and the SQL field on every PCM input line read it too. The squelch commands and
  `svc_rtl_set_sql_db()`/`svc_rtl_set_sql_setting()` run in every build and on every input; only
  `svc_rtl_push_squelch()`'s body is radio-only. Tests: `APP_CONTROL_SQUELCH_VIEW`, `APP_COMMAND_QUEUE` (the PCM
  case runs in radio-off builds), `UI_MENU_CALLBACKS`, `UI_MENU_LABELS`, `UI_MENU_ACTIONS`, `UI_NCURSES_PRINTER_HELPERS`,
  `UI_QT_METRICS_MODEL`, `UI_QT_QML_CALL_LISTS` (`tst_radio_squelch_auto.qml`, `tst_monitor_squelch.qml`).
  `include/dsd-neo/app_control/rtl_gain_view.h` and `src/app_control/rtl_gain_view.c` do the same for the tuner gain
  under `--trunk-scan` (issue #518 follow-up): the configured gain the controls edit and a save writes, the parked
  target's own `rtl_gain` while it overrides it, the terminal's `Gain... [20] (target: 10)` and Tuner autogain rows,
  the edit toasts, and Qt's `configuredTunerGainDb`/`tunerGainRowOverride` (a `target` badge and `default X` on the
  radio panel). Test: `APP_CONTROL_RTL_GAIN_VIEW`.
  `include/dsd-neo/app_control/rx_tone_view.h` and `src/app_control/rx_tone_view.c` fold `dsd_state::analog_rx` into the
  received-tone text (issues #522, #523): hidden unless `dsd_analog_tone_detection_active()` says the tap listens
  (decided from the options and the RTL output kind, not from INACTIVE in the publication, so a reset does not blink the
  row) and hidden while the publication reads UNAVAILABLE (an input rate the front end cannot use, where an em dash
  would claim no carrier), then `CTCSS 100.0 Hz`, `DCS D023N / D047I`, `detecting`, `none` or an em dash (the terminal
  prints a hyphen without UTF-8). Like the scan timing view it takes the caller's monotonic clock, here the real-time
  one (`dsd_realtime_mono_s()`) the deadline is stamped on: a publication past its `stale_after_ms` deadline (a stdin,
  UDP or TCP producer that stopped sending, or a live radio stream whose source stopped, while the decoder waits for
  samples and cannot say so itself) reads as the em dash. A locked value this build cannot name (an unsupported
  frequency, a DCS code that is unsupported or not the canonical member of its alias class) reads `detecting`, never a
  value. It carries the locked code and polarity in `dcs_code` / `dcs_inverted` beside `ctcss_tenths_hz`, and the same
  signal's other standard spelling in `dcs_alias_code` / `dcs_alias_inverted`: the text names both, canonical first. The
  first polarity reads normal and the second inverted for every standard code, whose
  inverted signal is another standard code's normal one; both are kept because a code is named with its polarity
  everywhere. The same view carries `configured_text`, the CTCSS/DCS receive policy in force (issue #527): `off`, or its
  mode and display list (`allow 100.0 Hz/D023N`, `dsd_tone_set_format_display()`), and while a scan row's own policy
  runs (`dsd_scan_mode_row_options()`) ` (row; default X)` naming the configured policy it shadows
  (`dsd_scan_mode_configured_tone_policy()`, its list summarised shorter), as the squelch and width views read; never
  derived from the received tone; `policy_visible` (detection runs and a policy is in force or a row set its own,
  independent of the received row, since the policy still mutes at a rate detection cannot use); and `gate` /
  `gate_text`, what the policy does with the carrier on air (`passing`, `muted: checking tone`, `muted: not allowed`,
  `muted: no tone`; empty without a carrier or past a stale input's deadline), with `gate_no_tone` saying a decided
  verdict was reached for want of a tone, so a frontend that words the verdict itself (Qt's translated words) reads why
  from the field, never from the English text. The terminal swaps the display list's `…` for `...`
  (`dsd_tone_display_to_ascii()`) where it prints the separator's hyphen (no UTF-8), and always in its menu label.
  `policy_editable` says detection runs, so the policy acts on what the monitor plays
  and its live editor belongs on screen, off included (the Qt Tone filter row's reach). For the live editor (issue
  #527) the same header has `dsd_app_tone_filter_setting_get()`, the configured policy an editor opens on (mode, list as
  the parser reads it back, spelled as written, and `row_override` while a row's own policy shadows an edit), and
  `dsd_app_tone_filter_edit_notice()`, the edit's toast (`Applied: Tone filter -> allow 100.0 Hz/D023N`, or
  `Default tone filter -> ...; this channel overrides it (...)` under such a row, both lists summarised to fit
  `DSD_APP_TONE_FILTER_NOTICE_SIZE`, and ASCII throughout, the mark `...+N`, since the terminal's status line prints a
  toast as is). Given options and no state (a terminal menu before the first snapshot), the view
  still fills `configured_text` from the options, as `dsd_app_tone_filter_setting_get()` reads them, and returns -1.
  Tests: `APP_CONTROL_RX_TONE_VIEW`, the terminal goldens, `UI_QT_METRICS_MODEL`.
  `include/dsd-neo/app_control/analog_width_view.h` and `src/app_control/analog_width_view.c` (issue #525) decide the
  analog channel width in force under the configured analog preset (the scan scope's configured view, so a typed digital
  row does not hide it; never for the M17 encoder): the front end's reported width while a running stream's options in
  force run the monitor, flagged DSP-limited when the rate bounds it, otherwise the configured width, and none on PCM
  input. The running stream's demod rate, or with none running on an input whose RTL DSP bandwidth sets the rate that
  rate, bounds the widths offered (`max_hz`, on a radio input under any preset); an unset default reads as what the
  monitor runs at that rate, as the next start publishes it: the rate itself where no channel filter runs (below 20 kHz,
  or as `DSD_NEO_CHANNEL_LPF` says), the legacy WIDE plan's passband (`dsd_channel_lpf_legacy_wide_width_hz()`) where
  the filter runs but the rate cannot realize the default, both DSP-limited, and so it does at a running stream's demod
  rate while the front end is off the monitor (a typed digital row, CQPSK toggled on under -fA), as the monitor it
  returns to publishes it. There whether the filter runs is the stream's own decision
  (`rtl_stream_channel_lpf_default()`, carried as `dsd_frontend_metrics::channel_lpf_default`), which its configuration
  made from the rate it started at and keeps whatever rate the device then delivers (a forced rate, a replay's capture
  rate), so the view does not re-derive it from the rate. The configured width comes from the scan scope's configured
  view while one is live. An analog scan row on air (issue #526) shows its width on any session (`row_analog`), and a
  row that sets its own width (`row_override`, `row_hz`) is the width in force, read as `12.5 kHz (row; default 16 kHz)`
  with the configured width of its demodulator, which it overrides (on an AM session its leave returns to the AM width
  instead). The services that hold a DSP rate to an analog session's width hold the configured preset's own kind and
  width, not the row's. They spell the reading (`12.5 kHz`, `16 kHz (default)`, `12 kHz (DSP-limited)`,
  `not used on PCM input`), the width command's notice (`dsd_app_analog_width_edit_notice()`, for the kind the command
  edits whatever kind the configured preset runs: `Applied: NFM bandwidth -> 12.5 kHz`, or
  `Default NFM bandwidth -> 16 kHz; this channel overrides it (12.5 kHz)` under a row width) and the configured setting
  (`12.5 kHz`, `default`). The terminal's `Analog:` status field, the `rtl.nfm_bw` row's label and predicate, the width
  command's toast, RTL_SET_BW's configured-width check and Qt's `analogBandwidth*` properties (the row flags as
  `analogBandwidthRowActive`/`analogBandwidthRowOverride`) all come from it. `dsd_app_analog_width_setting_hz()` reads
  either kind's width in `dsd_opts` (0 = default) whichever preset runs, for every caller that only reads the width in
  force (`dsd_scan_mode_configured_analog_width()` reads the configured one); app-control's
  `svc_store_analog_width_setting()` is the one writer. `dsd_app_analog_width_offered()` (issue #524) says which kind's
  width the frontends offer for editing on a radio input: the configured preset's kind whatever an analog scan row on
  air runs (an nfm row runs FM over an AM session), the kind that row runs, the other kind's while an explicit
  configured width of it is set, and AM's unset default where `max_hz` cannot filter it but can filter a narrower AM
  width (a switch to AM is refused there with word to narrow the width); it takes the scan state for the configured
  view. The terminal's width rows and Qt's `nfmBandwidthOffered`/`amBandwidthOffered` use it, and Qt's
  `analogBandwidthAm` says which kind the view's width is. Test: `APP_CONTROL_ANALOG_WIDTH_VIEW`.
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
- Received-tone detection (issues #522 CTCSS, #523 DCS), public entry points in `include/dsd-neo/dsp/analog_rx.h`:
  `dsd_analog_rx_tap()`, `dsd_analog_rx_tap_partial()`, `dsd_analog_rx_block_restart()`, `dsd_analog_rx_reset()`,
  `dsd_analog_rx_carrier_open_now()`, `dsd_analog_rx_block_straddles_boundary()`, the monitor playback
  bracket `dsd_analog_rx_playback_begin()` / `dsd_analog_rx_playback_end()` and the live input read bracket
  `dsd_analog_rx_input_wait_begin()` / `dsd_analog_rx_input_wait_end()`. The same header holds the CTCSS timing
  contract in sample time: p95 targets `DSD_ANALOG_CTCSS_LOCK_P95_MS` (400) and `DSD_ANALOG_CTCSS_LOSS_P95_MS` (350) and
  per-event ceilings `DSD_ANALOG_CTCSS_LOCK_CEILING_MS` (700) and `DSD_ANALOG_CTCSS_LOSS_CEILING_MS` (800).
  `DSP_ANALOG_CTCSS` asserts them on every timing row, and a tone policy's acquisition window must exceed the lock
  ceiling by at least 100 ms. Each ceiling sits above the slowest event of the long-run sweeps in `docs/testing.md`
  (1,000,000 starts per condition at 0 dB, 800,000 stops), as the header states; change them only with new sweeps. The
  header also states the measured wrong-tone rates (neighbour locks near 0 dB, talk-off), which a policy acting on the
  first lock has to budget for. The DCS timing contract sits beside it, built the same way, for the demodulator's DC
  block at 8 to 78.125 kHz: `DSD_ANALOG_DCS_LOCK_MS` (520, every start at 10 dB in-band or better; 7,000,000 starts
  over those rates), at 3 dB the p95 target `DSD_ANALOG_DCS_LOCK_P95_MS` (450) and the ceiling
  `DSD_ANALOG_DCS_LOCK_CEILING_MS` (1,500; lock time in noise has no absolute bound), and for loss the p95 targets
  `DSD_ANALOG_DCS_LOSS_P95_MS` (350) and `DSD_ANALOG_DCS_TURNOFF_LOSS_P95_MS` (150) with the ceilings
  `DSD_ANALOG_DCS_LOSS_CEILING_MS` (600) and `DSD_ANALOG_DCS_TURNOFF_LOSS_CEILING_MS` (400). The bounds hold with a DC
  step at the code's onset (a carrier off frequency) of up to twice the code's deviation, through the demodulator's DC
  block, a DC-coupled PCM input or a 1 Hz coupling; PCM input through a sound card's coupling keeps the 10 dB bound up
  to a 10 Hz corner but not the 3 dB target or ceiling, as the header states. Each DCS ceiling sits above the slowest
  event of the long-run sweeps in `docs/testing.md` (8,500,000 starts at 3 dB, 1,000,000 stops and 1,000,000 turn-offs);
  change them only with new sweeps. `DSP_ANALOG_DCS` holds every fixed-seed case to the p95 targets (at 3 dB through
  the demodulator's DC block, every start within 700 ms; the coupled row has pins of its own). A policy window must
  exceed the DCS lock ceiling by 100 ms too. `dsd_symbol.c` taps each unsynced analog block while it is still raw:
  `symbol_process_unsynced_analog()` offers the tap the block after every sample it adds, the one that completes the
  block included (`dsd_analog_rx_tap_partial()`), and the tap reads what is waiting once
  `DSD_ANALOG_RX_TAP_READ_MS` (20 ms) of input, at the input's current rate, has built up;
  `symbol_finalize_unsynced_analog_block()` hands it the rest after the raw WAV write and before the audio chain
  (`symbol_process_unsynced_audio()`), whose in-place voice band-pass takes every CTCSS tone 40 dB down. The
  block is 20 ms on RTL but 960 samples on PCM at any rate (384 ms at 2500 Hz), and read only at block ends the
  publication would trail the sample-time contract by up to a block. One decoder-thread tap covers RTL and PCM, sees
  only live (not seam-replayed) samples, only reads the block, and runs whatever `audio_out` says.
  `symbol_output_unsynced_analog()` is its monitor gate, sink and carrier stamp (issue #526): the stamp that holds a -Y
  row under the hangtime rule follows the tap's carrier (`dsd_analog_rx_carrier_open_now()`, which reports none once the
  tap's own generation check sees a retune or profile change since its last read, and while a retune is unresolved, so
  a channel a failed retune left the receiver on lets the row's hangtime run out) while the analog monitor runs, FM or
  AM, whether or not the block plays (the `-8` monitor under digital decoding keeps stamping only what it plays), and
  the gate writes nothing while a retune is unresolved (`dsd_trunk_tuning_pending_request()`: in flight, or failed
  after the scanner moved on, until a later retune lands or the scan's end retires it, as for digital frames) or, on
  the analog monitor, from a block that began before a retune, profile change or reset the tap noticed, before the tap
  started, or before a boundary the tap has not read past yet (`dsd_analog_rx_block_straddles_boundary()`, 0 while the
  tap is not running, so the `-8` monitor under digital decoding plays every block as before). The tap runs while
  `dsd_analog_monitor_tap_active()` (runtime, above) says so: the analog monitor of either kind on PCM input or on RTL
  with an AUDIO_MONITOR output kind. On the AM monitor (issue #524) it keeps only the carrier and those boundaries
  (`dsd_analog_rx_core_track_carrier()`), publishing no tone (`tone_state` INACTIVE) and logging nothing; the detectors
  run while `dsd_analog_tone_detection_active()` says so, and a switch between the kinds starts the core over. The rate
  comes from the RTL output-rate hook or `dsd_opts_current_input_timing_rate()`.
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
    `dsd_analog_rx_detector_ops` (configure/reset/process/report) and the core member holding that detector's state:
    DCS, then CTCSS. Detectors get the band stream, a time-aligned stage-1 "wide" stream (about 0-1 kHz, for the
    harmonic test) and a "full" stream aligned the same way: the raw input's mean square over each decimated sample's
    span, before any filter. Decimation folds a residue of voice-band content into the band (stage 1 leaves it at
    least 58 dB down, 74 dB from 20 kHz inputs up), and on a carrier with nothing else below 290 Hz that residue
    alone looks like a pure tone; the full stream is how a detector tells it from one. The carrier test reads the
    raw samples' mean square, and each detector's thresholds are ratios against those streams' energy. Reports
    merge in table order: the first LOCKED report names the code or tone, so a DCS lock outranks a CTCSS one (a
    code's word read twice in a row is far stronger evidence than a tone's correlation); otherwise the verdict is
    ACQUIRING while any detector still is, and NONE once all have said so. So a CTCSS talk-off on a coded channel
    never hides the code, and a tone that a code's own waveform raises in noise before the code locks gives way the
    moment it does. The front end accepts 2400 Hz up to `DSD_ANALOG_RX_MAX_RATE_HZ` (320 kHz, below the ~333 kHz its
    tap budget can design), logs which side of that range an unusable rate is on (once for each stretch of input at
    such a rate: a usable block ends the stretch, a reset does not) and publishes UNAVAILABLE there (after a reset,
    from the next block on), keeping the carrier (floor, test and hangover) at every rate all the same, which the
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
    end; a stalled device), each read also sets a real-time monotonic deadline (its arrival plus its own duration and
    the hangover, at least `DSD_ANALOG_STREAM_PAUSE_MIN_MS`), published as `stale_after_ms`. A read arriving past it
    follows a pause as long as a dropped carrier: it starts a new reception, and is itself dropped, since its first
    samples may have arrived before the pause. The frontends age the row against the same deadline meanwhile. Files,
    Pulse and IQ replay deliver continuously and never set one, so a replay stays deterministic however slowly it is
    read.
  - `src/dsp/analog_ctcss.c` is the CTCSS detector: one continuously running phasor per table tone, 50 ms sub-blocks
    with absolute phase in a 250 ms window, one hop per sub-block. Each hop fits every bin's sub-block phases (a
    pulse-pair estimate refined by weighted least squares), snaps the fine estimate to the table within the tone's gate
    (`ctcss_gate_hz()`: +/-0.8 Hz, or half the distance to its nearest neighbour where that is less -- 0.7 Hz for 150.0
    and 151.4 Hz, 1.4 Hz apart -- so no estimate is within two gates, and one midway between two snaps to neither),
    rejects aliases (an estimate more than 5 Hz from its bin) and scores rho, the share of the sub-audible band energy
    the tone explains. A tone locks after two consecutive hops qualify it (rho >= 0.35, an estimate within 0.5 Hz of the
    table value, at least 1e-5 (-50 dB) of the raw input's full-band power, which no folded voice-band residue reaches
    and every tone the tests lock exceeds by 24 dB or more, a phase fit whose reduced chi-square against the band's
    own noise stays under 6, estimates within 0.5 Hz of each other, and less than 0.08 of phase-locked second and
    third harmonic power: a voice fundamental has harmonics, a tone does not). It holds while its own bin's estimate,
    re-measured every hop, stays within the tone's snap gate and the newest 100 ms keep rho >= 0.15 at the locked
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
    must still carry the tone at that rho, within its snap gate, which keeps a voice that held a pitch near a table
    tone for most of a longer window and then moved on from locking. Two agreeing hops lock, as for the 250 ms window.
    On its own the 250 ms window passed the 700 ms ceiling on about one start in 125,000 at 0 dB (the slowest after
    1,128 ms), when noise kept every pair of its hops from qualifying the tone; the longer windows average that noise
    down. Samples from inside the
    carrier hangover keep the correlators' time, but a hop whose newest 100 ms (what the hold test reads) holds nothing
    else keeps the verdict, so a dropout's silence alone never ends a lock or makes one. Deciding by the sample that
    closes a hop instead would let a carrier that keeps dropping out keep a stopped tone for good, once its openings
    missed every hop's end; this way each opening makes the two hops that read it count, and a dropout the hangover
    allows leaves at most three hops in a row without one. Every threshold is a ratio, so the RTL (~1/pi, live and
    replay) and int16 PCM scales read the same.
  - `src/dsp/analog_dcs.c` is the DCS detector (issue #523), the first row of the table, so its lock outranks a CTCSS
    one. It undoes the front end's known 10 Hz DC blocker (`DSD_ANALOG_RX_DC_CORNER_HZ`) and puts a 0.5 Hz pole in its
    place, integrates each bit (the NRZ matched filter) at bit ends recovered by square-law timing recovery (the edge
    energy's component at 134.4 Hz, averaged over about eight bits; an early/late gate has a stable false lock half a
    bit off), and slices with decision feedback once per droop hypothesis (`k_droop`: none, and one-pole sags of 0.84,
    0.72 and 0.55 per bit for the demodulator's 2^11-sample DC block at 8 to 78 kHz and a sound card's coupling), since
    every DC block upstream sags a run of equal bits. A fifth, balance slicer reads a second re-poling of the band, with
    a 0.01 Hz pole (`DSD_ANALOG_DCS_BALANCE_CORNER_HZ`), and decides each of its two 23-bit windows against that
    window's own mean of bit integrals: every supported word carries 11 or 12 ones (`RUNTIME_ANALOG_TONES` pins it), so
    the mean is the DC offset under the code, which a carrier off frequency steps in with the carrier and which the
    re-poled stream's 0.5 Hz pole takes about 0.3 s per 1/e to remove, holding the droop slicers on one polarity while
    it is larger than the code. A lock is one slicer reading a supported code's word twice in a row, exactly in one
    23-bit window and within one bit in the other (both exact would need 46 clean bits in a row, which put the 3 dB p95
    at 436-666 ms, past the 450 ms target), named by `dsd_dcs_match()`. It holds while some slicer reads the
    expected rotation within one bit (the balance slicer exactly: with a bit of slack, the noise after a stop held a
    stopped code about twice as often), following a one-bit slip either way, or reads the locked class exactly at
    another rotation, which it then follows (the same code starting over elsewhere in its word: a radio re-keying inside
    the hangover, another transmitter behind a repeater). It is lost after 32 bits without, or at the first bit without
    once the 134.4 Hz turn-off tone has carried over a third of the band's power (the newest six bits) for two bits (a
    bit integral over one period of 134.4 Hz is zero, so the slicers hear nothing of it). The tone alone ends nothing: a
    steady component near 134.4 Hz under a code the slicers still read keeps the lock, which is why the threshold can
    sit below the 3 dB level. A window holding a bit read with the carrier closed can hold no word, so the 32 bits count
    only windows read wholly with the carrier open (`since_frozen`), and `DSD_ANALOG_DCS_SPAN_BITS` (64 bits, 476 ms)
    since the lock last held (`since_held`, carrier open or closed) ends it too: long enough for the same code to come
    back at another place after a dropout up to the hangover, and the bound under a carrier that keeps dropping out,
    whose windows are never read wholly open (counting open bits alone, a stopped code stayed shown for up to 1.8 s
    there). The ring holds two bit integrals at the highest decimated rate (a `_Static_assert`); a rate beyond it leaves
    the detector inert and reporting NONE. NONE after 500 ms of carrier without a lock, like CTCSS. Samples inside the
    hangover keep the clock and the windows moving but change no verdict on what they read; only the span, which is
    carrier time, runs out on them. A signal one bit from a supported word can read as that code, the way a DCS decoder
    tolerates a bit error; `DSP_ANALOG_DCS` pins that nothing further away does. While nothing is locked it also keeps a
    candidate (issue #527): some slicer read a supported code's word exactly (the first half of a lock) within the last
    `DSD_ANALOG_DCS_SPAN_BITS` bits read with the carrier open (`candidate_age`), reported through the plug-in report's
    `candidate` and published as `dsd_analog_rx_publication::dcs_candidate`. `DSP_ANALOG_DCS` pins that the two starts
    of 12,000 at 3 dB (78.125 kHz, 750 us) that lock after 800 ms hold it until they lock, and that noise raises it at
    800 ms on at most 5% of receptions.
  - `src/dsp/analog_tone_policy.c` behind the module-private `src/dsp/analog_tone_policy.h` is the CTCSS/DCS receive
    policy (issue #527), pure (no `dsd_state`, no clock): it takes each read's publication and its sample count and
    rate, and keeps its window in exact sample time. OFF with no list policy; else a reception starts PENDING at its
    first read with a carrier, a confirmed value is judged at once (allow: listed passes; block: listed is rejected),
    and `DSD_ANALOG_TONE_WINDOW_MS` (800, in `dsp/analog_rx.h`) without one decides "no tone" (allow rejects, block
    passes), extended to at most `DSD_ANALOG_TONE_WINDOW_DCS_MS` (1,600) while the list holds a code and
    `dcs_candidate` stands. `_Static_assert`s hold both windows to the CTCSS and DCS lock ceilings plus 100 ms. After a
    verdict it goes on judging: an allow list's allowed value lost goes PENDING with a fresh window, a confirmed
    nonpassing value rejects, a block list keeps a pass on loss, and a rejection holds until the reception ends except
    for a newly confirmed passing value; another confirmed nonpassing value becomes the rejection's reason (a "no tone"
    rejection that hears an unlisted tone names it). A locked value outside the standard sets is not confirmed: it is
    judged as no tone at the window's end. The tap's glue (`analog_rx.c`) configures it from `dsd_opts` on every read
    (the FM monitor only; a different policy starts the reception over, and its check is not logged as a tone lost; a
    list that only respells a code is the same policy, `dsd_tone_set_same_signals()`),
    resets it with every reset the publication's generation records (hangover, retune, row change, pause), publishes
    `gate`, `gate_no_tone` and `gate_rejected_ended` (a reception its carrier's hangover, or an input pause, ended under
    REJECTED, from that reset, the core's `carrier_end_reset`, until the next carrier, a policy change or any other
    reset: the scanners' release outlives the carrier), and logs `Tone filter: allowed|rejected (<value>|no tone)` and
    `Tone filter: pending (tone lost)` on a change. `dsd_symbol.c`
    applies it at its one monitor sink: `symbol_unsynced_audio_allowed()` plays only OFF and ALLOWED, muting the local
    stream and the UDP analog socket together, and `symbol_unsynced_carrier_active()` stamps carrier activity only
    for those too (`dsd_analog_tone_gate_passes()`), none for PENDING or REJECTED; the `-6` raw WAV, written before, is
    not gated. DSP never advances a scanner. Tests:
    `DSP_ANALOG_TONE_POLICY` (the state machine in sample time with injected detections), `DSP_SYMBOL_REPLAY` (the sink,
    the stamp, the raw WAV, policy off byte for byte), the `DECODE_IQ_ANALOG_POLICY_*` and
    `DECODE_IQ_ANALOG_REAL_CTCSS_POLICY` replays.
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
    digital mode, then the switch to the analog monitor). The tap skips its reads until one shows the input ran dry,
    that read included, or until `DSD_ANALOG_RX_BACKLOG_MAX_MS` (2 s) of input, after which stdin fed from a file faster
    than real time is heard again. A read shows it with a span of `DSD_ANALOG_RX_TAP_READ_MS` or more of input that
    passes two tests, since a backlog drains at the decoder's own speed. First, the span took at least half as long to
    arrive on the real-time monotonic clock, not counting the time the symbol path spends playing monitor audio
    (`dsd_analog_rx_playback_begin()` / `_end()` around the output write): synchronous playback of stdin input holds the
    decoder for each block's playing time once its buffer is full, and it then reads a backlog at real-time pace.
    Second, the decoder waited inside the input read for at least an eighth of the span: `symbol_take_sample()` brackets
    the live input read with `dsd_analog_rx_input_wait_begin()` / `_end()`, and a read of what the input already holds
    comes back at once. A backlog read with other decoder stalls in between (issue #576: the raw WAV's per-block sync to
    disk, command handling, scheduling delays) can pass the first test but not the second. Time the decoder spends
    inside a read with the backlog still queued (descheduled, or its own work) can pass the second but not, on its own,
    the first. On a live input the stalls leave less to wait for, since the audio that arrived meanwhile is already
    queued, and the eighth keeps a live input heard after two reads until the decoder is held for about 17 ms of every
    20 ms read. The first read after the boundary only starts the span. Files and RTL-family streams are not skipped
    (the RTL stream clears its own output at a retune), and the skip only reads: the monitor output plays the backlog as
    before. The tap's own resets act on the read in hand instead: a generation move, a pause or a change of input rate
    since the previous read drops it (after a rate change, samples taken at the old rate are another signal at the new
    one: 1920 Hz at 48 kHz read as 2500 Hz input is a 100 Hz tone), and the hangover expires on it.
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
- `dsd_symbol.c` reads a direct RTL output (the FSK discriminator, CQPSK symbols) through a 512-sample cache labelled
  with the stream generation and the profile its samples came off, and takes a sample only under the labels the
  decoder's work context has. A live read carries no labels, so a batch read across a generation bump is dropped
  whole, and one read across a profile change is dropped from the next refresh on (`rtl_symbol_cache_refill()`).
  Under `--iq-replay` the decoder paces the demod, so every RESET, hunt profile change and CQPSK toggle lands while it
  waits in that read, and the drop would hit the first batch after each one, every time (issue #572). There the read's
  batch tag (`dsd_rtl_stream_metrics_hook_replay_batch()`) labels the cache, and a batch whose labels the work context
  has not seen yet comes back as `RTL_SYMBOL_CACHE_REFRESH`: the caller refreshes the work context, output kind, SPS,
  levels and FSK timing exactly as after a drop, keeps the batch, and takes its first sample next. A monitor-output
  batch, and one the stream flushed after it was published (the pop checks the batch's generation), are still
  dropped. The cache keeps the batch's media span with its samples, and each sample it hands out runs the decode
  clock's media time to that sample's capture time (`rtl_symbol_cache_pop()`), so a decode window reads the same time
  whatever the batch boundaries; a live read's samples move nothing, and the matched-filter seam's hand-backs never
  pass through the pop. Tests: `RTL_SYMBOL_CACHE_GENERATION`, `RTL_SYMBOL_REPLAY_CLOCK`.
- `dsd_symbol.c` owns the open-loop FSK symbol grid. Only the inter-frame sync search moves it, by a whole sample at
  a time, on the first zero crossing latched in the previous symbol — a bang-bang loop on one unfiltered sample
  index, and between frames the only thing tracking the sampling instant across a call. Issue #444 documents how
  sensitive that is; the comment above `symbol_adjust_timing_nxdn()` records the five ways of damping it that were
  A/B'd on real captures and measured worse, so change it only with `tools/replay_ab.sh` evidence
  (`docs/testing.md`). The CQPSK path does not use any of this: it has a real timing loop in `costas.cpp`.
- The analog monitor's audible output leaves `dsd_symbol.c` in `symbol_output_unsynced_analog()`, after the audio
  chain (`symbol_process_unsynced_audio()`, below) and only while the gate is open; for `audio_out_type == 8` it goes
  through `dsd_udp_audio_hook_blast_analog()` with a byte count of int16 mono samples.
- The analog audio chain (issue #518) is `src/dsp/analog_audio.c` (`<dsd-neo/dsp/analog_audio.h>`) over the pure cores
  in `src/dsp/analog_voice.c` (`<dsd-neo/dsp/analog_voice.h>`): the voice band-pass (`use_pbf`, `-v 0x1`; FM: a
  6th-order elliptic high-pass at 300 Hz that takes every CTCSS tone, and all else below 254.1 Hz, 40 dB down (a DCS
  signal shaped below 300 Hz, as transmitters send it, about 32 dB, since its skirt reaches past 254 Hz), AM: a
  Butterworth high-pass at 200 Hz; both a 3400 Hz Butterworth low-pass; `tools/design_voice_filters.py` derives the
  prototype and every bound `DSP_ANALOG_VOICE` holds), the legacy 960 Hz one-pole low-pass and high-pass (`-v 0x2`,
  `0x4`; the chain's own copies at its rate, not `dsd_state`'s `RCFilter`/`HRCFilter`), then the gain stage: `-n N` is
  the source gain x N / 50, `-n 0` (the default) the AGC, a causal per-sample peak-envelope recurrence with no added
  latency whose output depends only on the sample and playing-flag sequence (500 ms hold, 20 dB/s or 3 dB/s release by
  where the input level sits, at most 18 dB over the reference gain, frozen under -36 dB of the reference and while not
  playing, rolled back 250 ms when the gate closes). The source gain takes each source's reference signal to -12 dBFS
  peak: RTL monitor audio after the `vol` trim (0.25 at the default 2: 1 kHz at 3 kHz deviation after the 1/pi output
  scale, or AM at 50%) by 32924, the RTL FSK discriminator output (+/-30000: the -8 source monitor under digital
  decoding, told by `rtl_symbol_cache_output_kind`, and EDACS on RTL, which, like the symbol path, reads it with no
  `vol` trim) by 8231/30000, PCM by 1. Each chain (monitor, EDACS) keeps its band-pass and AGC in
  `DSD_STATE_EXT_DSP_ANALOG_AUDIO`, allocated on first use (the fixed gain alone if that fails), and starts over on
  `DSD_ANALOG_AUDIO_RESET` and on a change of source, rate or band. A block that
  fills across a new reception -- a new RTL stream generation (RTL input), trunk-tuning generation or a boundary
  `dsd_analog_rx_reset()` announces (`dsd_analog_audio_note_reception()`: a scan row commit, a reconnect, the legacy
  `-Y` rigctl retune, which moves neither generation) -- is partly the channel before it: the caller notes when a block
  starts filling (`dsd_analog_audio_block_begin()`; EDACS once for its three blocks), and the chain drops a block whose
  reception moved since, silence out, whichever path collected it (the -8 source monitor's block too, where no tap
  tracks the boundary), and starts over on the next one, which a block collected wholly after the boundary does at once.
  The monitor chain runs the AM band whenever the monitor runs AM, a receiver ahead of PCM input (an AM scan row on
  rigctl) included; EDACS and the FSK output always run FM. `symbol_finalize_unsynced_analog_block()` takes the block's
  gate decision once, after the tap (`symbol_unsynced_audio_allowed()`, which has no side effects), and hands it to the
  chain as the playing flag and to the sink, so the AGC adapts to exactly the audio that plays; a block that straddles a
  retune or reset (`dsd_analog_rx_block_straddles_boundary()`), partly the old channel's and never played, goes in with
  `DSD_ANALOG_AUDIO_DISCARD`: the chain neither filters it nor counts it, turns it into silence, and starts over with
  the next block, so the old channel leaves nothing in the new one's filters. EDACS runs the EDACS chain with the
  talkgroup gate as its playing flag and a reset per call, after the symbol register is read from the raw block. The M17
  encoder keeps its own band-pass, 960 Hz one-poles (`dsd_voice_onepole`) and AGC at 8 kHz in its stream context
  (`m17_voice_chain`) and runs them over the whole codec2 frame, 160 samples at 3200 bit/s and 320 at 1600
  (`m17_voice_chain_process()`; an explicit `-n` keeps `analog_gain()`). Its input reader low-passes at 3400 Hz
  (`DSD_VOICE_BAND_LOWPASS`, at the input rate) before keeping one sample in `m17_rate / 8000`, and scales RTL monitor
  audio by the RTL monitor gain after the `vol` trim (`m17_encoder_read_block()`), so it reaches codec2 at PCM scale.
  The analog_voice and analog_audio sources keep IEEE semantics under fast-math, which would fold away their
  non-finite-sample guards. The published `dsd_state::aout_gainA` is the gain applied, in dB over the `-n 50` gain,
  which the terminal shows as `G: Auto (+x dB)`. Tests: `DSP_ANALOG_VOICE`, `DSP_ANALOG_AUDIO`, `DSP_SYMBOL_REPLAY`
  (`test_chain_playing_follows_the_sink`), `RTL_SYMBOL_CACHE_GENERATION` (source routing), `M17_STATE_DISPATCH`
  (`test_stream_voice_chain_covers_the_whole_frame`, `test_stream_voice_chain_onepoles_run_at_8k`,
  `test_encoder_rtl_input_reaches_pcm_scale`), `EDACS_GRANT_TUNE_MATRIX` (`rtl-unclipped`), the `DECODE_IQ_ANALOG_*`
  level, parity and fixed-gain cases.
- The auto squelch's core (issue #518 follow-up) is `src/dsp/squelch_floor.c` (`<dsd-neo/dsp/squelch_floor.h>`), pure
  and on the channel-filtered I/Q at the channel rate (before any post-decimation). A plan
  (`dsd_squelch_floor_plan_design()`) turns the channel taps and the half-band stage ahead of them into the
  classifier's constants: the coherence lag L from the taps' autocorrelation, and the noise's N_eff and coherence bias
  beta from the noise autocorrelation through both stages, the model `tools/squelch_model.py` fixed the thresholds with
  (`docs/testing.md` "Auto squelch classifier"). Each sample-exact 40 ms window is NOISE, CARRIER or UNDECIDED; the floor
  is learned from NOISE windows only (5 of the last 8), tracked down fast and up at 1 dB/s, and relearned on a sustained
  shift or a collapse, so carrier power never becomes floor and a channel with no noise windows stays learning, its
  carrier windows open. The gate opens on a 20 ms sub-window at floor plus margin and closes 3 dB lower or on a NOISE
  window; each sample's flag (`DSD_SQUELCH_FLAG_CLOSED`) is the gate before that sample's decision, so flags, floor and
  state are bit-identical however the samples are cut into blocks. The per-channel cache keeps floors as a noise density
  (floor over the plan's noise gain) keyed by the values that set the noise (frequency, tuner gain, tuner AGC, bias tee,
  device, channel rate, capture chain), stale after 30 minutes of sample time, which every block counts in every mode
  (`squelch_cache_age()`, level squelch and digital included); a neighbour within 5 MHz seeds a floor provisionally.
  `dsd_demod_reset_filter_state()` (stream open, retunes, family switches, a replay's RESET) makes the next block take
  its context afresh: at the same context it keeps the floor and starts the windows and coherence history over. A
  floor the tracker kept through a spell without running ages the same way (`demod_state::squelch_floor_active_s`):
  past `DSD_SQUELCH_FLOOR_STALE_S` it is learned again, neither kept nor stored for its channel with a fresh stamp. Inside `full_demod()` (`squelch_auto_run()`) the tracker runs on an AUTO setting on the analog
  monitor (`dsd_demod_analog_monitor_active()`), with a plan for the channel filter in force, the half-band stage
  ahead of it (`hb31` for one pass, `hb15` for more) and the channel rate (`rate_out` x `post_downsample`), and nothing
  is zeroed: the level gate and the AM detector's squelched-block branch stand aside, the flags land in
  `result_flags`, and the post-decimator, `low_pass_real()` and the resampler (`resamp_process_block_flags()`) carry
  them, each output taking the flag of its input at or just before the filter's centre (K/2 back, a group's middle
  sample, `low_pass_real()`'s last input; the resampler takes every input into its flag history, a call that makes no
  output included). The AM detector reads them (`am_demod_flagged()`): a closed sample is
  silence and holds the carrier estimate, and the estimate restarts from the open run on reopening after the hold or
  when it is 3 dB off. The tracker takes its context from `demod_state::squelch_context` (the IO layer's) and keeps
  the per-channel cache in `demod_state`. On the decoder side (`dsd_symbol.c`) the monitor reads each RTL sample with
  its flag (`dsd_rtl_stream_io_hook_read_ex()`) into `dsd_state::analog_out_flags`, beside `analog_out_f`. Under the
  auto squelch (`dsd_squelch_dynamic_in_force()`: an AUTO setting on an RTL-family input) the block gate's level
  comparison is off, the audio chain runs each run of heard and unheard samples with its own playing flag
  (`symbol_process_unsynced_audio_runs()`: the AGC adapts to exactly what is heard and rolls back at the close), and
  the sink ramps each sample in over 5 ms and out over 10 ms after every filter (`symbol_apply_sink_gate()`), so a
  closed stretch is exact silence and a block with nothing heard is not written. A block the block gate rejects (a
  muted output, digital sync, a retune still landing, the tone policy) plays nothing, not even a fade, and the next
  open sample ramps in from silence. The receive tap reads the flags too
  (`dsd_analog_rx_tap_flags()`): a read is a carrier when any of its samples is open
  (`DSD_ANALOG_RX_SQUELCH_CARRIER`, with no energy floor), so a silent carrier holds the scan row. Every other squelch
  comparison goes through `<dsd-neo/runtime/squelch.h>` (`dsd_squelch_level_in_force()`, `dsd_squelch_level_open()`,
  `dsd_squelch_gate_open()`): AUTO turns the level comparison off on PCM input, digital decoding (the GFSK sync skip),
  the M17 encoder and EDACS analog voice, which falls back to its no-squelch release watchdog. Tests:
  `DSP_SQUELCH_FLOOR`, `DSP_SQUELCH_AUTO_DEMOD`, `DSP_SYMBOL_REPLAY` (`test_auto_squelch_gates_each_sample`),
  `RUNTIME_SQUELCH`, `FRAME_SYNC_INTERNAL_HELPERS`.
  The block (`dsd_state::analog_out_f`) collects unsynced samples in a digital session too, monitored or not (the
  CQPSK symbol-rate output excepted). `dsd_symbol_analog_block_reset()` (`<dsd-neo/dsp/symbol.h>`, decoder thread)
  drops a part-collected block; app-control and the channel-scan leave call it when the receive family changes. On an
  RTL front end the switch itself lands later, at the demod thread's next block boundary, so `getSymbol()` also
  follows the output it reads: an analog-family decoder does not collect a direct (digital) output's samples while
  the front end has yet to switch, and a move between the monitor output and a direct one drops the part-collected
  block (`symbol_refresh_rtl_profile()`), so the first block the new family plays holds only its own samples.
  On the monitor output it follows the monitor the stream publishes the same way (issue #582): while that is the
  other analog kind (`dsd_rtl_stream_metrics_hook_analog_profile()`, a switch between FM and AM) or, on the analog
  family, no monitor at all (`..._analog_family_active()` with nothing published: a typed digital `-Y` row's channel,
  which a leave back to Analog or AM puts the monitor back from), an analog-family decoder collects nothing and drops
  a part-collected block, whichever path changed the decoder. The demod thread clears the output ring before it
  publishes a new monitor, so the answer read once per `getSymbol()` never admits the old profile's samples. Without
  it a whole block of the old profile's audio, which a backlog the decoder catches up on after a block's synchronous
  playback holds, would play: a block with no boundary in it is one `dsd_analog_rx_block_straddles_boundary()` cannot
  mute. Nothing published (no front-end hooks, a front end off the analog family) leaves collection as it was; of
  those only a session with no decode mode switched to Analog reads another profile until the switch lands (the M17
  encoder is never the analog family). A front end the stream leaves off the configured monitor for good, such as a
  scan leave whose fallback width the rate refuses too (the refusal's toast says so), is silent there, as a digital
  output under an analog decoder is; a refused switch between FM and AM puts the decoder back on the kind the front end
  kept (`ui_revert_analog_entry()`). A digital decoder on the monitor output (a typed row's, the `-8`
  source monitor) collects what it reads. Test: `RTL_SYMBOL_CACHE_GENERATION`.
  `tests/engine/analog_replay.c` (`dsd-neo_test_analog_replay`, the `DECODE_IQ_ANALOG_*` audio cases) captures and
  scores exactly that output through the hook, and times it with a wrapped RTL stream read hook, so changes to the
  monitor chain are measured against what a listener hears; back them with `tools/replay_ab.sh --metric analog` evidence
  (`docs/testing.md`). The host's own options are the `--analog-*` names it lists; other `--analog-*` arguments pass
  through to the CLI parser. After each delivered block it also reads the received-tone publication
  (`dsd_state::analog_rx`, which the tap updated from the same block) into its `tone`, `tone_lock_ms` and
  `tone_lock_pct` fields (`tone=151.4` for CTCSS, `tone=D023N/D047I` for DCS: both spellings in one token), which the
  `DECODE_IQ_ANALOG_REAL_CTCSS_*`, `DECODE_IQ_ANALOG_DCS_023N_HOST` and `DECODE_IQ_ANALOG_DCS_023I_HOST` cases and
  `tools/replay_ab.sh` read.
- The NFM noise squelch's core (issue #518 follow-up) is `src/dsp/nfm_noise_squelch.c`
  (`<dsd-neo/dsp/nfm_noise_squelch.h>`), pure and on the FM discriminator's output at the channel rate. A plan
  (`dsd_noise_squelch_plan_design()`) takes the band from 3.8 kHz to the channel taps' -1 dB point less 800 Hz (under
  0.45 fs; at least 1200 Hz of it, else no valid plan, as there is none for a channel with no channel filter behind the
  half-band cascade at a rate of 16 kHz or less, whose roll-off would truncate a wide signal into the band), cuts it
  into 300 Hz sub-bands (at most fifteen) plus a set staggered half a sub-band between them, order-4 Butterworth
  band-passes it designs in place, and calibrates each band-pass's noise power on 2 s of fixed-seed complex Gaussian
  noise through the half-band cascade ahead of the channel filter (the stages `full_demod_apply_halfband_decimation()`
  runs, passed as `dsd_noise_squelch_stage`s: those before the last as one 63-tap FIR sampled from their composite
  response, then the last), the channel taps and the discriminator (`tools/noise_squelch_model.py` chose all of it:
  `docs/testing.md` "Noise squelch design gate"). Each 40 ms window, taken every 20 ms at sample-exact boundaries, reads
  Q = max(Q_sum, Q_max - 4 dB) of quieting; the gate opens at N and closes under max(N - 3, 1.5) dB, one flag per sample
  as the tracker's, bit-identical whatever the block cuts. Inside `full_demod()` a NOISE setting on the FM monitor with
  a valid plan arms it in `full_demod_update_channel_state()` (`squelch_noise_wanted()`, the level gate standing aside)
  and `squelch_noise_run()` flags each output right after `dsd_fm_demod()`, before the post-decimator, de-emphasis and
  audio filters, which then carry the flags as the tracker's. On an AM channel, or one with no plan (8 and 10 kHz NFM,
  low DSP rates, an unfiltered channel at 16 kHz or less), `squelch_auto_wanted()` runs the tracker instead, N as its
  margin. Its plan follows the same `dsd_demod_squelch_plan_key` as the tracker's, which carries the cascade's pass
  count (designed only when NOISE needs it), and it keeps its own context record (`demod_state::noise_context`), so a
  spell under NOISE never stores the tracker's floor under another channel; a new context or
  `dsd_demod_reset_filter_state()` starts it over, closed. The IO layer's status adds which squelch ran and its quieting
  (`rtl_stream_squelch_status::noise`, `quieting_db`). Tests: `DSP_NFM_NOISE_SQUELCH`, `DSP_NOISE_SQUELCH_DEMOD`,
  `IO_RTL_SQUELCH_PLUMBING`, the `DECODE_IQ_*_SQL_NOISE_*` cases.
- The band-pass bank both noise squelches share is `src/dsp/noise_squelch_bank.{h,c}` (module-private): the order-4
  Butterworth band-pass design (`dsd_noise_squelch_bank_design_band_pass()`), the runners, the per-band ratio and the
  Q = max(Q_sum, Q_max - guard) combine (`dsd_noise_squelch_bank_quieting_db()`); the radio squelch's results are
  bit-identical to before the split.
- The PCM noise squelch's core (issue #628) is `src/dsp/pcm_noise_squelch.c` (`<dsd-neo/dsp/pcm_noise_squelch.h>`), pure
  (no clock, I/O or allocation) and on audio input's own samples at the monitor rate. Its plan
  (`dsd_pcm_noise_squelch_plan_design()`, from the monitor rate and the input's native rate) takes the band from 3.8 kHz
  to the lower of 6.5 kHz and 0.45 times the native rate (at least 1200 Hz, else no room), in 200 Hz sub-bands (at most
  sixteen) with two staggered sets, and a 400-2600 Hz voice band with four parts. With no calibration possible it learns
  the noise reference from the input (`tools/pcm_noise_squelch_model.py` chose all of it: `docs/testing.md` "PCM noise
  squelch design gate"): closed while LEARNING, a reference from the first steady stretch (PROVISIONAL), confirmed or
  raised by a louder transition that reaches it (KNOWN), lowered to noise come back at a lower gain (runs more than 1.5
  dB under it gather evidence of noise's spectrum at their level: the voice band and each of its parts moved as far as
  the sub-bands' median, the sub-bands not tilted, the tilt a line across those within 3 dB of that move; 0.4 s of it in
  three runs of four, 1.5 dB or more on average, steps the reference by the mean move, and any other stretch under it is
  the carrier's modulation or level and leaves it), stepped by a gain-like transition out of noise that holds noise's
  spectrum, a downward step held, with the reference from before the first held one (cached with it), until replaced or
  refuted (eight of ten runs the gate keeps shut against the stepped reference with a voice band clearly louder than
  noise's, or more than 1.5 dB above it without noise's spectrum, bring that reference back: a carrier relaying noise,
  then speech; the stretch then steps down again only on 0.6 s of clean evidence, doubled per refutation), tracked with
  a 1 s time constant, learned again, as a held step, after 5 s open on one stretch with a steady voice band, and
  NO_BAND on 1 s of spectral evidence that nothing is above voice. Up to eight references are cached by
  `dsd_pcm_noise_squelch_key` (source generation and input type, native rate, input volume, rigctl peer passband; an
  unknown passband is never cached). Q = max(Q_sum, Q_max - 6 dB) over the band-passes that take part; the gate opens at
  N and closes under max(N - 3, 1.5) dB, one flag per sample, bit-identical whatever the block cuts. The decoder runs it
  from `symbol_process_unsynced_analog()` for non-RTL input (`dsd_analog_rx_pcm_squelch_sample()`,
  `src/dsp/analog_rx.c`): its state rides `analog_rx_session` (state-ext slot 9) and never creates the session; a
  boundary counter the tap bumps at a reset, a pause, a generation or rate move and an FM/AM switch restarts its
  windows; it holds closed and learns nothing while the tap's backlog skip is armed; it reads the rigctl peer's passband
  through the runtime rigctl query hook (`dsd_rigctl_query_hook_get_passband_hz()`, which the engine installs reading
  `CachedModulation()` under the P25 SM tick guard: busy, keeping the passband last read, when the guard is held;
  unknown, a boundary, after a lost reply); and it writes `dsd_state::squelch_noise_*` and `squelch_auto_gate_open` for
  the views, logging once why it is off (no room, no band, with the scan consequence). Tests: `DSP_PCM_NOISE_SQUELCH`,
  `DSP_PCM_NOISE_SQUELCH_SWEEP`, `DSP_SYMBOL_REPLAY` (`test_pcm_noise_squelch_gates_each_sample`,
  `test_pcm_noise_squelch_holds_the_backlog`), `RUNTIME_RIGCTL_QUERY_HOOKS`, `ENGINE_RIGCTL_QUERY_HOOKS_INSTALL`, and
  the `DECODE_PCM_*` cases (the analog replay host's `--analog-pcm-tap`, which runs a capture's discriminator audio as a
  WAV input in every build).
- AM envelope detector (issue #524, `demod_pipeline.cpp`, declared in `<dsd-neo/dsp/demod_pipeline.h>`):
  `dsd_am_demod()` outputs 0.25 x clamp(|z| / C - 1, +/-2), C being `demod_state::am_carrier`, a one-pole average of
  |z| with a `DSD_AM_CARRIER_TAU_MS` (50 ms) time constant recomputed per block for the detector's rate (`rate_out`:
  AM never runs where an I/Q replay decimates after the demodulator). It writes silence and holds C while
  `channel_squelched`, counting the closed run in `demod_state::am_squelched_samples`; past
  `DSD_AM_CARRIER_HOLD_MS` (100 ms) the next unsquelched block starts C over, since what opens the squelch then is
  another transmission. It warm-starts C from the block's mean magnitude when it is 0, and is silent below a 1e-9
  carrier. `dsd_demod_am_active()` says whether it produces the monitor audio: installed, and the
  monitor on its own channel (`dsd_demod_analog_monitor_active()`). A typed digital scan row's profile on an AM session
  is FM-demodulated instead, as under `-fA` (`full_demod_run_output_demod()`), with the carrier estimate left alone;
  the AM session runs no de-emphasis, so, unlike under `-fA`, the row's discriminator output is not de-emphasized.
  While `dsd_demod_am_active()` the squelch's channel power is the plain mean square of the I/Q floats, the carrier
  measured whole (receiver DC included, as a vector sum), not `mean_power()`'s pooled-mean removal, which reads an
  0 Hz carrier as A²(1 - sin 2φ)/4 and so chopped an AM channel's squelch as the carrier's phase drifted (issue #518
  follow-up, `DSP_SQUELCH`). FM and digital keep `mean_power()`.
  `dsd_demod_iq_dc_block_active()` is the I/Q DC blocker's gate (enabled, and not under AM), which `iq_dc_block()`
  uses; `dsd_demod_iq_balance_active()` is I/Q balance's (enabled, CQPSK off, and not under AM), which
  `full_demod_apply_iq_balance()` uses. `am_carrier` is a float in a scanned header, so `tools/semgrep_float_fields.py` regenerated the semgrep
  `$FLOAT_FIELD` list with it. Tests: `DSP_AM_DEMOD`, `DSP_CHANNEL_FILTERS` (AM widths).
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
  the same design call as WIDE, so its taps are bit-identical wherever WIDE's design succeeds. AM widths (5000..20000
  Hz) use the same design.
  `dsd_channel_lpf_legacy_wide_width_hz()` reports the passband the legacy WIDE plan has at a rate (the 144-tap design,
  its cutoff held to 0.9 x Nyquist, or above ~51.4 kHz the 63-tap fallback prototype, cut at a third of the rate), the
  width published for an unset default that plan runs.
  The plan cache key is (rate_out, profile, width). The SIMD complex FIR kernels size their scratch per call, so the
  288-tap capacity needs no kernel change. A live width edit on the running monitor is seamless (issue #572):
  `rtl_stream_apply_analog_request()` drops only the plan, and the half-band cascade and the channel FIR keep their
  histories and pending counts, so the new width's taps run over the true past and no sample is lost or repeated at
  the edit. The exception is an NFM edit between an explicit width and the unset default when
  `channel_lpf_default_enable` is 0 (`DSD_NEO_CHANNEL_LPF=0`, or `rate_in` below 20 kHz): it turns the channel filter
  off, whose bypass then drops the pending samples, or on, over a zeroed history. A family or FM <-> AM kind switch,
  a rate change and a retune still start the filters over: the controller's retune through its reset
  (`demod_reset_on_retune()`), and an external backend's landing
  (`rtl_stream_apply_pending_retune_profile_for_target()`, which has no finalize) itself when it lands on another
  frequency than the last external landing that took a profile, whatever its width. A landing with no profile queued
  (a DMR or NXDN voice-channel tune) and every finalize (a stream open, a controller retune) forget that landing, so
  the next one starts the filters over even on the same frequency. Tests:
  `DSP_CHANNEL_FILTERS`, `DSP_DEMOD_MISC`, `IO_RTL_ANALOG_FAMILY_SWITCH` (`rtl_stream_test_analog_width_continuity()`),
  `IO_RTL_RETUNE_PREPARE` (`rtl_stream_test_external_landing_filter_state()`).
- Streaming linear front end (issue #572): the half-band cascade and the channel FIR (`simd_hb_decim2_complex()`,
  `simd_hb_decim2_real()` and `simd_fir_complex_apply()` in `<dsd-neo/dsp/simd_fir.h>`, every backend sharing the call
  contracts in `src/dsp/simd_fir_internal.h`) carry their look-ahead, and the half-band its decimation phase, from one
  block to the next, so the front end's output does not depend on where the blocks are cut. The stages after it still
  decide per block, so `full_demod()`'s output as a whole does: channel power and the squelch decision, the squelch
  envelope, I/Q balance, the AM detector's warm start, CQPSK's adaptive Gardner gain, the rounding of a squelched
  block's zero-symbol count, metrics, and the block a profile request is consumed on. Holding the look-ahead is
  latency: a filter emits an output only once it holds the c inputs after it, about 74 samples (1.54 ms) at 48 kHz on
  the default 1.536 Msps RTL chain, about 7 in the five-pass cascade and 67 in the 135-tap FIR, and 67 (1.40 ms) on a
  48 kHz replay, which runs no cascade.
  - Channel FIR, in complex samples, with c = (taps - 1) / 2: a call makes max(0, pending + N - c) outputs, output k
    centred at `hist_len - pending + k` of [history | block], and leaves pending + N - outputs pending with the newest
    `hist_len` inputs in the history. No variant pads the look-ahead past a block's end; an invalid call or a short
    `out_cap` returns -1 with nothing touched. Scalar is bit-exact across splits; the SSE2, AVX2 and NEON kernels (and
    the dispatcher, which sends blocks shorter than the filter to scalar) agree with their own whole-stream output
    within 1e-5.
  - Channel state: `demod_state::channel_lpf_hist_i/q` hold `DSD_CHANNEL_LPF_HIST_LEN` (287) samples whatever the
    taps, and `channel_lpf_pending` the outputs held back. A tap-count change without a reset reconciles through
    pending (up to N + c_old - c_new outputs), so the work buffers the filter writes (`hb_workbuf`, `timing_buf`) are
    `DSD_DEMOD_WORKBUF_LENGTH` floats, a maximum block plus 287 complex.
  - Aligned buffer storage: each `alignas(DSD_DEMOD_BUF_ALIGN)` buffer at the top of `demod_state` is declared as
    `DSD_DEMOD_ALIGNED_FLOATS` of its capacity, rounded up to whole 64-byte blocks, so no compiler pads between them
    (MSVC's C4324 is an error under `/WX`); static asserts below the struct hold this. The capacity constants above,
    not `sizeof`, bound what code uses, and the spare tail is never read.
  - Half-band decimators, with c = (taps - 1) / 2, H = taps - 1 and lt = pending + N (the complex decimator counts
    floats in and out, 2 a sample): a call makes m = (lt - c + 1) / 2 outputs once lt reaches c + 1 and none before,
    output k centred at H - pending + 2k of [history | block], and leaves pending lt - 2m, which is any of 0..c during
    a warm-up and c - 1 or c once outputs flow. N = 0 is valid, and a call makes at most (N + 1) / 2 outputs. No
    variant pads past a block's end; an invalid call (bad taps, a NULL buffer the call needs, a negative or odd float
    count, pending outside 0..c) returns -1 with nothing touched. Every backend's prefix condition, vector base and
    load-footprint guard derives from centre_rel = 2n - pending, and the fixed 15/31-tap kernels read a scalar
    output's centre through the history boundary loader too. Scalar is bit-exact across splits; SSE2, AVX2, NEON and
    the dispatcher agree with their whole-stream output within 1e-5.
  - Half-band state: `demod_state::hb_hist_i/q` and `hb_pending` per stage. `hb_state_passes` is the pass count that
    state belongs to: `full_demod_apply_halfband_decimation()` clears it when `downsample_passes` differs, since the
    rate setup and `restore_capture_rate_settings()` write the count without a reset. The cascade drops a stray odd
    float, as it always did.
  - `dsd_demod_reset_filter_state()` (`<dsd-neo/dsp/demod_pipeline.h>`) is the one filter reset: half-band histories
    and pending counts, channel history, channel pending 0, and the post-demod decimator below on both paths, so every
    filter's next output is centred on its next input and what they held is dropped (the latency above: a RESET or
    loop rewind drops the old dwell's last 74 samples or so, and as nothing flushes them at EOF, a replay never decodes
    a capture's last 1.5 ms). `rtl_demod_clear_filter_histories()` and stream open (`demod_init_common_defaults()`)
    delegate to it, so a retune restarts the post-demod decimator too, as a family switch does.
  - `channel_lpf_apply()` clears the channel state on a plan for another `rate_out` than the last one, and on the first
    block the filter does not run on (filter off, or no plan for the width); a width or profile change at the same rate
    keeps it, and so does the stream layer's live width edit, which keeps the half-band state too (see Channel LPF).
    `full_demod()` gives a block its front end left empty (a filter warm-up) `result_len` 0 and no per-block decision:
    squelch gate, envelope, channel power and CQPSK zero symbols wait for a block with samples. It marks the block
    (`demod_state::front_end_empty`), and the stream's squelch hop count skips it, so a squelched block before it does
    not count twice (`IO_RTL_ANALOG_FAMILY_SWITCH`, `rtl_stream_test_squelch_hop_empty_block()`).
  - The CQPSK Gardner (`op25_gardner_cc()`) runs every block, however short, through the state in `ted_state` (mu,
    omega, the delay line, the last symbol), so its symbols do not depend on the cut either: a block can make none, and
    a symbol the last block owed comes out of the next one. It writes them to the work buffer its input is not in
    (`hb_workbuf` when the channel filter left the block in `timing_buf`, as after an odd half-band pass count), up to
    that buffer's capacity. Only its adaptive gain is decided per block. Test: `DSP_COSTAS` (1-, 2- and 3-pair blocks,
    warm and cold, from each buffer, bit-exact against the whole stream).
  - The post-demod audio decimator (`full_demod_apply_post_audio_decimation()`, only on an I/Q replay whose sidecar
    sets `post_downsample` above 1; every live source and committed fixture runs 1) streams on both paths: the 16-tap
    polyphase decimator (its history and phase) and, when that allocation fails, the fallback, a one-pole low-pass
    and the mean of each M samples, whose one-pole output and part-filled group carry in `demod_state`
    (`post_fallback_*`). A block publishes the outputs its samples complete, 0 included, never its undecimated input.
    A block with another factor, `rate_out` or path than the last one (`post_decim_state_*`) starts the stage over.
    `low_pass_simple()` stays a one-block helper (a trailing part-filled group dropped) over the same boxcar. Test:
    `DSP_AUDIO_DECIM_SEGMENTATION` (both paths, the fallback forced with the DSP test hook
    `dsd_demod_test_fail_post_polydecim_alloc()`: whole stream against a reference, blocks shorter than M bit-exact
    against the whole stream, 0-output blocks, and reset, factor, rate and path changes after a part-filled group).
  - Tests: `DSP_FIR_SEGMENTATION` (every backend and the dispatcher, FIR and half-band, against the count formulas, a
    double-precision reference and their whole-stream output over many splits; tap switches; invalid calls; capacity;
    exact-size buffers at every half-band block size up to a few filter spans; the pipeline rules above, the pass-count
    guard included), `DSP_DEMOD_SEGMENTATION` (`full_demod()` over the 1.536 Msps chain, 12K5 and analog NFM 16 kHz,
    pass-through and FM, with the per-block stages held: one block vs many splits), `DSP_SIMD_FIR`,
    `IO_RTL_REPLAY_DETERMINISM` (its count oracle runs each half-band stage's formula and then the FIR's, cumulatively
    within a RESET epoch).

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
  `rtl_stream_get_analog_setting()` (the kind and width setting that family runs, on the monitor output or off it),
  `rtl_stream_output_rate_for_family()` (the output rate a pending switch will produce),
  `rtl_stream_set_digital_decode_modes()` (the decoder's configured digital modes, which pick the FSK channel profile
  a CQPSK toggle returns to once a live switch has moved the stream onto the digital family),
  `rtl_stream_prepare_retune_analog_profile_for_target()` (the same fields bound to a retune target), and
  `rtl_stream_live_family_request_count()` (the live family requests accepted so far; one accepted after a retune
  profile's family was attached supersedes that family), `rtl_stream_familyless_retune_supersedes()` (the live analog
  requests among them, and the scan leaves; one counted after a retune profile with no family was queued supersedes
  its symbol profile, issue #582) and `rtl_stream_supersede_familyless_retunes()` (the scan leave's count).
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
  - `dsd-neo_io_control` — rigctl/serial control interfaces. `SetModulationKind()` (`rigctl_client.h`) asks a rigctl
    peer for FM (`M NFM <bw>`, then `M FM <bw>`) or AM (`M AM <bw>`) at a passband, caching per socket on the
    demodulator and passband together; FM at 0 (the peer's own passband) is sent only to undo a request made on the
    socket (issue #526). SDR++ and GQRX take a passband of 0 as "unchanged", and SDR++ saves every passband it is sent,
    so the undo sends the peer's own passband explicitly: `SetScanRowModulation()`, a scan row's own request, first asks
    the peer what it runs (`m`, switching a peer on the other demodulator to this one at passband 0 to read that one's),
    and `RestoreScanModulation()` puts back each passband a row changed, the other demodulator's first, before the
    session's request. A peer that cannot answer `m` gets passband 0, best-effort. `Connect()` starts the record empty
    for a connection on the number of a closed socket and leaves it alone for one on another number (the TCP audio
    input's reconnect while the rigctl socket stays open), and `SetModulation()` is the FM call. `RigctlRebindPeer()`
    hands a rigctl reconnect, opened while the socket it replaces is still open, what that socket knew of its peer
    (issue #589), and forgets the frequency `SetFreq()` last sent, since a later connection can get the closed socket's
    number back: the record for the same host and port, so the FM undo and the scan's restore still send the peer's own
    passbands and a failed tune still puts back what the peer last accepted, though no request is taken for one it
    already runs until it accepts one (it may have restarted or been changed meanwhile); for another endpoint while the
    old peer may run AM (an am row's, or either demodulator after a lost reply), a demodulator not known, so FM is sent
    first and a refusal fails a best-effort tune; otherwise, and where no request may have changed the old peer,
    nothing, a peer nothing was asked of as on a first connection, so one that refuses mode requests does not fail every
    tune. No own passband of the old peer reaches another endpoint, and the record names the new socket afterwards,
    never the closed one whose number a later connection may get back. Both rigctl reconnects, the '9' key (the TCP
    input's host at the rigctl port) and the menu's host and port, go through `svc_rigctl_connect()`
    (`src/app_control/rigctl_connect.c`): it connects, then, under the P25 SM tick guard since the watchdog's retunes
    use the socket, rebinds, closes the old socket without sending it anything (the old peer keeps what a scan last set
    on it) and forgets the engine's legacy tune cache; a connect that fails while rigctl is on changes nothing, so the
    connection in use and its record stay. The engine names the record for a run's first connection the same way
    (`RigctlRebindPeer()` with no old socket) before the P25 watchdog starts. The engine's rigctl
    tune leg (`dsd_engine_tune_rigctl_modulation()` in `trunk_tuning.c`) asks a peer that demodulates audio input for
    an AM scan row's AM width and an nfm row's own width through `SetScanRowModulation()`, failing the row's tune when
    the peer refuses, and for `-B` otherwise, best-effort unless the peer refuses it while on an am row's AM or on a
    demodulator not known (`CachedModulationKind()`, no I/O), which fails the tune; on an RTL-family input, where
    DSD-neo demodulates, it always asks for `-B` and no refusal fails the tune. A rigctl tune that fails after its
    modulation request changed what the peer runs puts that back (`CachedModulation()` before the request,
    `RevertModulation()` after), so the row still on air is not heard through the failed row's demodulator or
    passband; where no passband was known before the request (the first row on a fresh socket, or after a lost reply),
    the peer's own passband that the row's request read is what goes back. The leg
    reads a `-Y` row's own width from the row being tuned (`dsd_engine_scan_tuning_row_options()`,
    `scan_analog_internal.h`), since the prepared settings in force cannot tell it from the configured one.
    `dsd_engine_scan_rigctl_restore()` (`trunk_tuning.h`) calls `RestoreScanModulation()` with what the restored
    session runs once `dsd_engine_channel_scan_leave()` has left a `-Y` map or a trunk-scan target list, so neither an
    am row's AM nor a row's passband outlives the scan. After an I/O failure the socket's cache matches no request,
    since what the peer runs is no longer known (a lost reply to a request for the other demodulator leaves which one it
    runs not known either, `DSD_RIGCTL_KIND_UNKNOWN`), and a demodulator whose undo the peer did not accept keeps the
    own passband read for it, so the next undo still sends that rather than reading the row's back as the peer's own.
    The engine's legacy rigctl leg (`no_carrier_tune_rigctl_if_needed()` in `engine.c`: the untyped `-Y` step and the
    direct control-channel return) skips a repeat of the frequency and the `-B` it last sent. That cache holds for one
    connection: `dsd_engine_rigctl_tune_cache_forget()` (`trunk_tuning.h`) empties it when rigctl reconnects (issue
    #589), since the new connection's peer, another one or the same one restarted, was sent neither.

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
    realize, an explicit width with `DSD_NEO_CHANNEL_LPF=0`, or an explicit width on an IQ replay whose sidecar
    decimates after the demodulator (`post_downsample` above 1, `rtl_demod_check_analog_post_decimation()`) fails the
    start with the validator's text; the unset AM default is held to the same rules. The refusal is also recorded, with
    the kind, the configured width and the rate it was held to (`rtl_stream_start_analog_refusal()`, relaxed atomics
    that every `rtl_stream_create()` and stream open clear), since the caller of a failed start has no stream left to
    name the rate the device delivered (issue #578; test: `IO_RTL_STREAM_START_FAILURE`). So is whether the start
    opened the I/Q capture file for writing (`rtl_stream_start_opened_capture()`), which writes it anew before the
    workers and the device's streaming start and can still fail: a rollback that restarts the input that ran cannot
    claim it kept that recording. The record is the writer's own report of that open (`dsd_iq_capture_open_ex()`'s
    `out_data_opened`, set once the data file's descriptor opens for writing, which empties it, from the platform open's
    own report, `dsd_fopen_private_ex()`: a stream that `fdopen()` then cannot make over it, or a later sidecar or
    thread failure, included, and the writer removes the file it emptied), so a writer that fails before it (its
    configuration, an allocation, a data file it cannot open) reports the recording kept, and removes nothing it did
    not open (test: `IO_IQ_CAPTURE_WRITER`, an `fdopen()` failure injected through `--wrap` where GNU ld offers it;
    `PLATFORM_FILE_COMPAT`). The unset NFM default never
    fails: it keeps the `rate_in >= 20000` / `DSD_NEO_CHANNEL_LPF` enable rule and falls back to the legacy WIDE design
    where the rate cannot fit 16 kHz, published as DSP-limited at the width that plan passes.
  - AM (issue #524): an AM open (`rtl_demod_init_for_mode()`) and every switch to the AM kind
    (`rtl_demod_set_analog_kind()`, which also restarts the I/Q DC estimate) install the envelope detector
    (`dsd_am_demod`) with de-emphasis off (its coefficient cleared); FM gets the discriminator and the configured
    de-emphasis back. `rtl_demod_reset_audio_monitor_state()`, which every open, retune and family or kind switch runs,
    returns the detector's carrier estimate to cold (`demod_state::am_carrier` 0 and its closed-squelch run
    `am_squelched_samples` 0; the next block warm-starts it), and `demod_init_common_defaults()` clears both for a fresh
    open. An AM start on a replay whose sidecar decimates after the demodulator is refused with
    `AM needs a capture with post_downsample 1`: no AM width runs there, the default included.
    `demod_write_output_block()` skips the output scale for AM (`dsd_demod_am_active()`), which normalises its own
    level. A configured I/Q DC blocker and I/Q balance are bypassed under AM, each with a one-time note
    (`rtl_demod_note_am_iq_dc_bypass()`, `rtl_demod_note_am_iq_balance_bypass()`, from configuration, a kind switch, a
    switch onto the analog family (`rtl_demod_enter_analog_family()`) and the runtime toggles); a kind switch clears
    both estimates. A live FM <-> AM switch on the running monitor (`rtl_stream_apply_analog_request()`) also resets
    the resampler history and clears the output ring with a generation bump, so the new kind's audio does not follow the
    old detector's, and starts the half-band and channel filters over whatever the width, so it lands where a fresh open
    of the new kind does (their raw I/Q histories are the same under either kind: the I/Q DC blocker and balance run
    after the channel filter); a width-only change keeps all of them. Only a family or kind switch changes the
    detector: a profile that turns CQPSK off (`rtl_stream_disable_cqpsk_mode()`) installs the analog family's own, so
    an AM monitor keeps the envelope detector through a CQPSK-off toggle, a failed tune's restore and a typed digital
    row's profile, which `full_demod()` reads with the discriminator while its channel is not the monitor's. Tests:
    `IO_RTL_DEMOD_CONFIG`, `IO_RTL_ANALOG_FAMILY_SWITCH` (digital <-> AM, an AM start to digital and back to AM on the
    same stream, live FM <-> AM against fresh opens via `rtl_stream_test_analog_kind_switch()`, at the same width too,
    the CQPSK-off profiles via `rtl_stream_test_am_monitor_symbol_profiles()`, the DSP menu's return from CQPSK to the
    FM or AM monitor, taken and refused where it lands, via `rtl_stream_test_monitor_return_from_cqpsk()`, and the live
    output scale through `demod_write_output_block()` via `rtl_stream_test_monitor_output_scale()`: 1/pi x rate / 48000
    for FM at the rate the discriminator runs at (rate_out x post_downsample, which a fixed-grid device forces), so a
    deviation plays at one level whatever the demod rate, none for AM or digital output; IQ replay runs
    the live scale whatever an earlier session left, via
    `rtl_stream_test_replay_output_scale()`), `IO_RTL_RETUNE_PREPARE` (`rtl_stream_test_audio_monitor_retune_kind()`).
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
    toggle, an external backend's retune landing on the decoder's thread). Both gates are one gate with one owner
    (`reconfigure_gate_take()`): a thread that asks for it while another holds it waits until the holder has taken the
    flag (`retune_in_progress`) down and left, so a controller reconfiguration and a landing or toggle never rewrite the
    demodulator or output chain together and neither reopens the gate under the other; a holder that asks again (the
    family switch or CQPSK toggle a retune profile applies) only counts on its own thread. It is taken with no other
    lock held, and its holders wait only for the demod thread, which never asks for it and cuts an output write short
    while the flag is up.
    Receive-family requests are queued and applied by the demod thread between blocks; a retune profile's
    family fields apply with the rest of the retune under the reconfigure gate, as its symbol profile and CQPSK toggle
    always have, and an analog one applies no symbol profile, CQPSK toggle or timing queued for the same target. A live
    family request accepted after a retune profile's family was attached supersedes it (`g_live_family_requests`, read
    by `rtl_stream_live_family_request_count()`, which the `-Y` scanner compares across a row's retune): the
    retune lands on its target with neither that family nor the symbol profile queued with it, so a scanner that leaves
    while its row's retune is still in flight (the configured family put back by live requests) is not switched back
    to the row's family when the device finishes the retune. A retune profile queued with no family (a typed digital
    row on a `-fA` or `-fM` session, whose configured mode is analog, or any retune of a digital-only session) records
    instead the supersedes counted so far (`g_familyless_retune_supersedes`, read by
    `rtl_stream_familyless_retune_supersedes()`, issue #582): every live analog family request, which asks for the
    monitor the profile's CQPSK toggle and symbol profile would take away, and every `-Y` scan leave, before its own
    requests (`rtl_stream_supersede_familyless_retunes()`). One counted after the profile was queued supersedes it, and
    the retune lands its centre only (`rtl_stream_retune_symbols_superseded()`). A scanner leaving while a typed row's
    retune is in flight, and the controller starts that retune late (a PPM correction ahead of it, a starved controller
    thread), therefore keeps the configured decoder's profile its leave put back, the monitor or a digital one, instead
    of landing on the row's channel or CQPSK after the leave had settled, with nothing left to ask again. A plain
    digital request supersedes no such profile: on the digital family it is a no-op a digital session makes around its
    hops. The other way round, such a profile that lands retires a live analog request still queued from before it was
    queued (`rtl_stream_retire_analog_request_before_symbols()`, by the request number it records when queued): a width
    command drained just before the scan advanced to a typed row would otherwise be taken at the demod thread's next
    block boundary and put the monitor back over the row on air. That request reads replaced, and settles once the
    symbol profile has applied; a digital request or a symbol profile queued alone stays, and so does one queued after
    the profile. Both are decided under the gate the landing holds and, for the retire, the request lock (a request
    counts itself before it takes that lock): a request counted after them is taken at the demod thread's next block
    boundary, after the profile. Tests: `IO_RTL_RETUNE_PREPARE` (a typed DMR and a P25 CQPSK row after a later analog
    request, a P25 hop after a later digital or an earlier analog request, and the count),
    `IO_RTL_ANALOG_FAMILY_SWITCH` (`rtl_stream_test_typed_row_retune_across_leave()`: the leave taken and settled
    before the row's retune lands, on the controller and as an external backend's landing, under `-fA` and `-fM`, and a
    leave made before the profile was queued, which the profile still lands over;
    `rtl_stream_test_digital_leave_across_familyless_retune()`: a DMR session's leave, plain or landing, before a P25
    row's late retune; `rtl_stream_test_older_analog_request_across_familyless_retune()`: a width request queued before
    a typed row's retune, with no family or the digital family attached). For a retune with a family, the other
    way round, a family that lands retires the live
    requests still queued from before it was attached (`rtl_stream_retire_requests_before_family()`, by the request
    number it records): a width or mode command drained just before the scan advanced would otherwise be taken at the
    demod thread's next block boundary and put the front end back on the family or width the row just left. An
    analog request retired this way reads replaced (below), never taken; a symbol profile queued after the attach is
    the row's own and still applies. The retire checks
    for a superseding family request again under the request lock (`g_profile_req_m`), which orders the decoder's
    requests against it: one made while the retune lands supersedes the family as one made before does. An
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
    before it settles), and settled by a stream open (which drops the queue) or a request with no pipeline to take the
    queue. An analog request the demod thread never took reads replaced (`RTL_STREAM_RX_REQUEST_REPLACED`, issue #578)
    once it settles, not settled like one the front end ran: a later analog request replaced it in the queue, or a
    retune's family retired it (above: a scan going on to an analog row), and what replaced it decides the front end.
    So do the analog requests it replaced in turn, each queued over the one before it, and a demod profile queued among
    them and dropped with them (`g_rx_req_replaced_run`, the last such run, recorded under the request lock before the
    settlement and forgotten by the next stream open); any other request dropped untaken (a demod profile a later
    request replaced) reads settled. An analog request refused where it landed reads refused,
    with the family, analog width setting and kind the stream kept, and whether it kept the monitor output
    (`dsd_demod_analog_monitor_active()`: a typed digital row's channel profile or CQPSK under the analog family keeps
    the setting without running it), recorded before the settlement (`rtl_stream_receive_request_refusal()`), until the
    next stream open forgets it. The stream publishes the same kind and width setting of the analog family whether or
    not the monitor output runs (`rtl_stream_get_analog_setting()`, beside `rtl_stream_get_analog_profile()`, which
    publishes a kind and width only on the monitor), so a request refused at once is read against what the family runs
    as a refusal where it lands is (issue #578). Each numbered request also notes
    the CQPSK state it leaves the stream on (an analog family request turns it off, a demod profile sets or leaves it),
    which `rtl_stream_requested_cqpsk()` answers with while any request is unsettled.
    That, not the output generation (which the clear for a request moves before the publish, and a retune moves without
    taking a request), is what tells the decoder the published CQPSK state includes its request. Each also notes the
    receive family it leaves the stream on (a family request names it, a demod profile leaves it, and a retune's family
    that retires the requests queued before it names the family it lands), and whether the requests end with a digital
    landing (`rtl_stream_request_digital_family_landing()`, below), which
    `rtl_stream_family_landing_after_pending()` reads (issue #583: the analog family, or such a landing) after the
    controller's in-flight marker
    (`controller_state::retune_in_flight_family`, set when the controller takes a retune and cleared once it has landed
    and published it) and its queued retune, both under `hop_m` and counted for either family they carry
    (`controller_family_retune_outstanding()`), and before the published family, never holding two of the locks; each
    source publishes before it stops counting (a landed retune its family and the output chain the finalize designed),
    so a landing between two reads is not missed. Tests: `IO_RTL_DEMOD_CONFIG` (`rtl_stream_test_rx_request_outcomes()`,
    the requested family included) and `IO_RTL_RETUNE_PREPARE` (`rtl_stream_test_family_landing_after_pending()`,
    and the landing queued, landed and replaced in `rtl_stream_test_live_digital_landing()`). A
    digital family request leaves the analog family whenever the stream
    runs it (`demod_state::analog_family`, published as `rtl_stream_analog_family_active()`), including after a symbol
    profile applied on its own (a typed digital scan row under `-fA`, a CQPSK toggle) has moved the front end off the
    analog monitor: such a profile never leaves the family, and the decoder asks for the digital family only when its
    configured mode is digital. A family request drops any demod profile queued before it (a CQPSK toggle drained in
    the same pass of the command queue), and a digital family request the demod thread finds with no symbol profile
    queued while the stream runs the analog family, or that is a digital landing, stays queued until the profile
    arrives, so a switch to digital requested as two calls (`svc_publish_symbol_profile()`, the channel-scan leave)
    always lands on its own profile.
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
    the output generation; a width-only change redesigns the filter and keeps its histories and the half-band cascade's,
    so the edit is seamless while the channel filter stays on (see Channel LPF). A switch to digital also
    keeps the open's floor of two samples per symbol for the TED, which the symbol-profile setter it applies does not
    (ProVoice at a 12 kHz DSP rate), and times a profile it does not override for the demod rate it lands on, as an
    open does, not for the rate the decoder read when it queued the profile: a retune can settle the device on
    another rate in between, and the CQPSK timing loop takes the SPS it is given (`rtl_stream_landing_ted_sps()`; a
    retune that carries the digital family is timed the same way, not with the TED the decoder queued). The analog
    family never runs
    CQPSK: a `-fA` open demodulates FM whatever `DSD_NEO_CQPSK` or the modulation say, as a switch to analog does. The
    CQPSK family after a switch to digital follows the symbol profile requested with it unless `DSD_NEO_CQPSK` is set,
    which decides it as it does at stream open, the channel filter following the family it lands on and the open's
    enable rule (an FSK landing keeps the WIDE profile while the channel filter is off: `DSD_NEO_CHANNEL_LPF=0`, or
    below a 20 kHz DSP rate by default) (`rtl_demod_open_cqpsk_request()`, `rtl_demod_open_channel_profile()`, also
    behind `rtl_stream_output_rate_for_family()`). The exception is a landing whose digital family says the profile's
    CQPSK state is a trunk-scan target's own choice (issue #583: a P25 target with a `modulation` value, `auto`
    included, or a DMR or NXDN target on FSK): a retune's (`RtlRetuneProfile::cqpsk_explicit`), or a live digital
    landing's (`rtl_stream_request_digital_family_landing()` with its `cqpsk_explicit`, which a republish of a
    trunk-scan row passes by the scope's rule, `dsd_scan_mode_cqpsk_explicit()`). That state stands over the override,
    with the channel filter the profile names under the same enable rule, as on a retune that stays digital
    (`rtl_demod_landing_cqpsk()`, which `rtl_stream_resolve_landing_profile()`, `rtl_stream_leave_analog_family()` and
    `rtl_stream_output_rate_for_family()` resolve through; a plain live family request,
    `rtl_stream_request_analog_profile()`, and a landing whose requester makes no target's choice, such as the
    channel-scan leave, pass 0, and the value the consume resolves resolves to itself again at the switch). The digital
    resampler
    and output rate are decided for that profile when the switch is made (`rtl_demod_enter_digital_family()` takes its
    CQPSK flag and symbol rate), so a forced rate lands where an open of the profile would. A retune that carries the
    digital family lands that way even when the front end already runs the digital family where it lands (an analog
    retune refused there, or a request replaced, left it digital; issue #583,
    `rtl_stream_retune_lands_digital_family()`): the engine attaches the family and times the decoder for the switch's
    landing by one decision per row (`scan_timing_rate_hz()` records it, `dsd_engine_retune_lands_digital_family()`
    spends it), so its symbol profile
    runs the same CQPSK state and channel filter (`rtl_stream_resolve_landing_profile()`), the controller's finalize
    designs the resampler and output rate for them from the same inputs, and the TED is the landing one. Only the switch
    itself is left out there: no loop resets beyond a retune's and a CQPSK change's, and no `rx_family_switch` record,
    so a stream still on the digital family it opened on keeps taking its FSK channel profile from its options. An
    external backend's retune (`rtl_stream_apply_pending_retune_profile_for_target()`: a rigctl peer that tuned an RTL
    input outside `-Y`) has no controller retune and so no finalize: it applies the profile from the decoder's thread,
    its gain first and then the family switch, symbol profile, TED and, for a profile that carries the digital family,
    the output chain a live digital landing designs (`rtl_stream_design_digital_landing_output()`), all under the
    family-switch gate the CQPSK toggle uses (`rtl_stream_enter_demod_family_switch_gate()`), which parks the demod
    thread between blocks, so it lands on the same prediction. That gate is the controller's reconfigure gate with its
    one owner, held for the whole landing: the landing waits while a controller reconfiguration (a PPM correction, a
    hop) runs, and the next one waits for the landing, so the two never redesign the resampler together.
    A retune without a family applies its symbol profile as
    queued, as a digital-only session always has, over the output chain it finds, unless a live analog request or a
    `-Y` scan leave counted after the profile was queued superseded it (above: then it lands its centre only). A live digital landing
    (`rtl_stream_request_digital_family_landing()`, issue #583: a republish, or a channel-scan leave, that timed the
    decoder for the digital family's landing because the analog family runs or outstanding work lands a family) lands
    the same way: from the analog family as the switch a plain digital request
    makes, with the flag it carries saying whether the CQPSK state is a trunk-scan target's own, and on a front end
    already digital as that switch would (`rtl_stream_resolve_landing_profile()`, `rtl_stream_landing_ted_sps()`), with
    the resampler and output rate designed again for the landed CQPSK state by the helper the switch and a retune's
    finalize use (`rtl_stream_design_digital_landing_output()`, `rtl_demod_maybe_update_resampler_after_rate_change()`;
    an output rate that moves clears the ring and moves the generation), and nothing else of the switch: no
    `rx_family_switch` record and no loop resets beyond a CQPSK change's. It is the newer word on the family, so a
    retune that carries one, attached before it, lands its centre only if it lands after the request was made, and one
    that landed first leaves the landing putting the front end on the same prediction again; a later family request
    replaces it in the queue. On the digital family a
    symbol profile request applies the CQPSK state it asks for, `DSD_NEO_CQPSK` or not, and keeps the output chain the
    stream runs: only an open, a family switch, a retune or a live landing designs the resampler again. Tests:
    `IO_RTL_ANALOG_FAMILY_SWITCH` (digital → analog → digital, and a `-fA` start switched to digital, each equal to a
    fresh open, loop state, monitor audio state, I/Q corrections and filter histories included, also under
    `DSD_NEO_CQPSK=0` and `=1` and with the channel filter off (`DSD_NEO_CHANNEL_LPF=0`, a 12 kHz DSP rate), with the
    stream keeping the options snapshot it opened with, for P25
    C4FM/CQPSK, DMR, NXDN48, dPMR and ProVoice (also at a 12 kHz DSP rate) at unforced and forced rates and D-STAR,
    a CQPSK profile and a C4FM one after the switch each running the CQPSK state it asks for at the output rate the
    switch set up, under either override,
    the DSP menu's CQPSK toggle made twice after the switch landing where it lands on a fresh open, including
    from a `-fA` session a CQPSK toggle or a
    typed digital row had moved off the monitor output, or with a CQPSK toggle still queued when the digital mode is
    picked; a typed digital row under `-fA` and on a DMR session switched
    to analog; width-only changes, seamless through the half-band cascade and the channel filter at one tap count and
    onto more and fewer taps; requests with no stream; live requests and
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
    `IO_RTL_DEMOD_CONFIG` (`rtl_demod_landing_cqpsk()`'s truth table) and `IO_RTL_RETUNE_PREPARE` (a trunk-scan target's
    own CQPSK choice landing after an analog target under `DSD_NEO_CQPSK=0` and `=1`, with the predicted rate, and a
    live digital landing on a stream already digital at a forced 78125 Hz, `rtl_stream_test_live_digital_landing()`:
    the output kind, CQPSK state, filter, output rate and TED the prediction names, alone and in every order against a
    retune that carries the digital family, with no family switch recorded, while a plain request keeps the profile
    as queued; and an external backend's landing against a controller reconfiguration on another thread,
    `rtl_stream_test_external_landing_against_reconfigure()`: each waits for the other to leave the gate, and the
    landing still lands where it was timed).
  - `output_state::rate` (`<dsd-neo/runtime/ring.h>`) is atomic: the controller and demod threads write it and the
    decoder and UI threads read it through `dsd_rtl_stream_output_rate()`.
  - The output ring is cleared from the demod thread (family switch, reacquire) and the controller (reconfigure gate)
    while the decoder reads it, and from the decoder thread itself under the family-switch gate (a CQPSK toggle, an
    external backend's retune whose landing moves the output rate). The readers (`ring_read_available()`,
    `ring_read_available_copy()`, `ring_read_batch()`) hold `ready_m` from their tail snapshot to their tail store, and
    `rtl_stream_clear_output_ring()` clears under it, so a read in flight cannot store its old tail over the cleared
    indices (which reads as a ring full of the old stream's samples).
    The clear bumps the output generation before it takes `ready_m`, and again under it once the ring is empty: a
    read that loaded the first bump can still reach the ring before the clear and take samples the clear drops, and
    the second bump keeps the stream from running on the generation that read carried them under.
    Tests: `RUNTIME_RINGS`, `IO_RTL_ANALOG_FAMILY_SWITCH`.
  - The output ring carries a flag byte per sample beside its float (`output_state::flags`): the auto squelch's
    per-sample gate (issue #518 follow-up). The demod writes a block's flags at its samples' positions before the head
    store that publishes both (`demod_copy_output_chunk()`; a block without flags writes them open), and the readers
    copy them under the same cursors. `dsd_rtl_stream_read_ex()` / `rtl_stream_read_ex()` and the runtime hook
    `dsd_rtl_stream_io_hook_read_ex()` hand them to the decoder; a host that installs only `read` gets them all open.
    Tests: `IO_RTL_SQUELCH_PLUMBING`, `ENGINE_IO_HOOKS_INSTALL`.
  - The squelch setting reaches the demod thread through `g_squelch_auto_word` (mode and margin) beside the existing
    `channel_squelch_level`, which the level gate reads under a LEVEL word only. A switch to AUTO stores the word
    alone and leaves the level, so a block that took the old LEVEL word still gates on it; a switch to LEVEL stores the
    level before the word (`rtl_stream_set_channel_squelch_setting()`), so no block runs ungated between them. The
    demod thread copies the word into `demod_state` before each block (`demod_take_squelch_setting()`), with the
    floor's context (applied frequency, tuner gain as applied, tuner autogain, bias tee, device hash, channel rate,
    capture chain; on an Airspy a hash of its own gain controls as applied, at open and by
    `rtl_stream_airspy_controls()`, stands for the tuner gain). After the block it publishes the tracker's status
    (`rtl_stream_get_squelch_status()`).
  - I/Q replay framing (issue #572; `replay_thread_process_block()` in `rtl_device.cpp`, `demod_read_input_block()`
    in `rtl_sdr_fm.cpp`). Each demod block is exactly one capture chunk:
    - The reader reads a chunk whole with `replay_read_exact()`: 64 KiB, or up to the next event or the end, looping
      over short reads. A cf32 read that stopped inside a complex sample would otherwise be skipped, and every read
      after it would stay off the sample grid. A read that fails after part of a chunk hands over that part first.
      A chunk the capture's end or a failure cuts short ends on its last whole complex sample (2 bytes for cu8, 8 for
      cf32): nothing can complete a part of one, and a cf32 chunk holding one could not be converted at all.
    - `replay_submit_whole_chunk()` waits for the chunk's realtime deadline, then (with no deadline) for an empty input
      ring, and commits the whole chunk at once. The reader reads and converts the next chunk while the demod works on
      this one.
    - The chunk's metadata (`struct rtl_replay_chunk_meta` in `rtl_replay_device.h`) travels with it: sequence, submit
      generation, raw input level, and media start and end in ns. Media time counts every capture sample handed over
      or omitted by a MUTE, and a loop does not restart it. The reader writes it into the stream's one chunk slot before
      the commit, and the demod copies it into its `DemodInputSpan` at reserve, before any of the chunk is released
      (the wrapped path releases early).
    - The demod acknowledges its span's own generation, after the block's output is written or when it discards the
      block, never the newest the reader bumped, which can belong to a chunk still outside the ring. It publishes the
      chunk's input level when it starts the block.

    Tests: `IO_RTL_REPLAY_EOF_AND_CF32` (block log per chunk in fast and realtime replay, media time across a loop, a
    chunk held numbered but uncommitted, a block that wraps the ring end and a discarded block, each releasing its
    input before it acknowledges, a multi-chunk EOF, cf32 short reads, a read failure mid-chunk, in cu8 and inside a
    cf32 sample, a cf32 end inside a sample, the input level at the first block).
  - I/Q replay end of stream (issue #572; `replay_thread_fn()` in `rtl_device.cpp`, `rtl_stream_read_replay()` in
    `rtl_sdr_fm.cpp`). The capture's end and a read the capture source refuses end a replay the same way:
    - The source refuses a read that gets no bytes before the capture's end as its open measured it
      (`dsd_iq_replay_read()` returns `DSD_IQ_ERR_IO`): a failed read, or a data file cut short after the open, which
      is not the capture's end. `replay_read_exact()` hands over the whole samples read before it and reports the
      failure on its next call.
    - The reader marks input EOF, waits on the input ring's `space` (50 ms at a time) for the demod to take the rest,
      marks the input drained, then waits for the demod to drain.
    - Under `--iq-loop` the capture's end rewinds only a pass that submitted a chunk (`replay_handle_empty_read()`,
      against the chunk sequence the pass began at). A pass that submitted none would submit none again, and each
      rewind runs the boundary, the reconfigure gate and a full finalize, so such a pass ends the replay as its end
      does without `--iq-loop`, with a warning.
    - A failure is logged and latched as `DSD_INPUT_FAILURE_FILE`. Under `--iq-replay`,
      `dsd_engine_run_with_lifecycle()` then returns 1. The log names what failed: reading the capture, converting a
      chunk of it (with its sample format and capture stage), or handing it on (an input ring that would not take a
      whole chunk, an event boundary the pipeline gave up on).
    - A chunk the converters (`replay_convert_block_to_f32()`) refuse is such a failure, not an empty chunk; a chunk
      that converts to no sample yet (a one-byte cu8 chunk, all carry) is skipped. `dsd_iq_replay_open()` refuses
      every capture they cannot convert, naming its format and stage (`validate_replay_convertible()` in
      `iq_replay.c`, which `dsd_iq_info_print()`'s "Replay compatible" also checks): only cu8, and cf32 at
      `post_driver_cf32_pre_ring`, replay.
    - The drain decision (input drained, demod drained) is made under `replay_eof_m` by the reader and the demod
      alike, so whichever decides second sees the other's store. The demod thread marks itself drained when it
      leaves.
    - The decoder's read never blocks inside a ring read. It loads "drained" before it looks at the ring, copies with
      the non-blocking `ring_read_available_copy()`, which a clear can leave empty-handed, and otherwise waits 10 ms
      at a time on `output.ready`. A reader that left without marking EOF counts as EOF.
    - Only the decoder's read that finds the ring empty with the end known ends the stream: it marks the output
      drained, sets `should_exit` and returns -1. The read that takes the last samples returns them and ends nothing,
      even when the end is already known, so the decoder decodes them with the stream still open. What it asks the
      stream meanwhile (the input level, the decode health, `rtl_stream_is_active()`, all gated on an open stream)
      then cannot depend on whether the reader had reached the capture's end by that read, which a fast replay has
      and a realtime one still pacing has not.
    - A drained demod seen first means the ring already holds all of its output. The demod is reported drained only
      once it has acknowledged the last chunk submitted before EOF, and it acknowledges a chunk only after that
      chunk's output is in the ring. A replay purges its input only at a RESET or loop boundary, on an idle pipeline,
      and the reader commits nothing until the purge is applied.
    - `replay_wake_all()` wakes every replay wait (both rings' `ready` and `space`, the demod's demand condition, and
      the EOF condition). It runs at EOF, on a failure, on a stop and when a start unwinds.

    Tests: `IO_RTL_REPLAY_EOF_AND_CF32` (a cs16 and a cf32-at-the-cu8-stage capture refused at the start, a
    conversion failure mid-replay reported, an `--iq-loop` pass that submits nothing ending the replay, a data file
    cut short under the reader at a chunk's end and inside a cf32 sample), `IO_IQ_METADATA` (the open's and
    `--iq-info`'s format and stage check, the read past a cut, a `data_bytes: 0` capture read to its end),
    `ENGINE_REPLAY_READ_ERROR` and `ENGINE_REPLAY_TRUNCATED_CAPTURE` (exit status 1),
    `DECODE_IQ_RF_CLIP_EOF_DETERMINISM` (a one-chunk clipped capture warns of CLIP fast, realtime and with short
    reads).
  - I/Q replay decoder pacing (issue #572; "Replay decoder pacing" in `rtl_sdr_fm.cpp`). Under `--iq-replay` the
    decoder paces the demod, so the demod's blocks and the decoder's reads and requests interleave the same way fast
    or realtime, however the host is loaded and however the decoder reads:
    - The demod starts a block only once the decoder waits in `rtl_stream_read_replay()` on an empty output ring and
      has acknowledged every batch published (`replay_out_acked == replay_out_written`). The written count starts at
      1, a virtual block 0, so the first block waits for the decoder's first read.
    - The wait (`demod_wait_for_replay_demand()`) sits at the top of the demod loop, outside `demod_processing_active`,
      so a gate waiting for an idle demod does not wait for it. It waits on its own `replay_demand_cond` with
      `output.ready_m` (`output.space` fires on every read) in 10 ms steps, and ends on a stop, a forced stop or the
      global exit, so a start that unwinds before any decoder reads returns. It logs once when the decoder has not
      asked for output in 5 s while a receive request is pending.
    - The demod publishes a block's output as one batch in one critical section under `output.ready_m`: the head
      store, the batch tag, `written++` and the broadcast of `output.ready`. A block with no output publishes nothing,
      and the demod goes on while the decoder still waits.
    - The decoder acknowledges (`acked = written`) and broadcasts the demand under the same lock, when it finds the
      ring empty. Every decoder read reaches that one point (`dsd_symbol.c`'s cache refill and single-sample reads,
      `m17.c`, `edacs-fme.c`, the analog monitor). So the demod never starts a block while the decoder runs, and a
      request, clear or snapshot the decoder makes lands at the start of the next block.
    - The batch tag (`rtl_stream_replay_batch` in `rtl_stream_c.h`: chunk sequence, output generation, the published
      output kind, channel profile, symbol rate and levels, output rate, media start and duration, output count) of
      the batch the last read took samples from, with their place in it, is what `rtl_stream_get_replay_batch()`
      returns, on the decoder thread while the stream is open. The symbol cache labels what it reads from it, through
      the runtime metrics hook (see DSP), so it keeps the first batch after a change, and runs the decode clock on its
      media span, sample by sample (see Runtime).
    - Lock order: `replay_eof_m`, then `output.ready_m`. The locked sections use the unlocked ring helpers.

    Tests: `IO_RTL_REPLAY_DETERMINISM` (greedy fast, slow and realtime readers with the same requests deliver the same
    stream, the oracle's count, and the same media time at fixed delivered positions, which moves by exactly what a
    MUTE omitted at its boundary; a block's output held unpublished stays out of the decoder's reach, and every read's
    tag describes its samples; bounded stops at each wait; zero-output and empty replays; a reader-start failure at
    the first demand wait), `IO_RTL_REPLAY_EOF_AND_CF32`.
  - I/Q replay events on an idle pipeline (issue #572; "Replay events on an idle pipeline" in `rtl_sdr_fm.cpp`,
    `replay_dispatch_pending_events()` in `rtl_device.cpp`). The reader applies every event (RETUNE, MUTE, RESET) and a
    loop rewind only once the pipeline is idle, so it lands between the same two chunks and the same two decoder reads
    however the decoder is scheduled:
    - `rtl_replay_eof_state::wait_event_boundary` (`rtl_replay_wait_event_boundary()`) waits, with no deadline, until
      the input ring is empty, every chunk submitted is acknowledged, the output ring is empty and the decoder has
      acknowledged every batch published. It waits on the demand condition, which the decoder's acknowledgement and the
      demod's (`replay_demod_drain_decide()`) both broadcast, and ends only on a stop, a forced stop or the global exit.
      `replay_event_boundary_drained()` stays the input half, which a replay without a stream behind it waits for alone.
    - An event at offset 0 waits for the decoder's first read (the virtual block 0). A RESET's drain and reconfigure gate
      then find empty rings and only move the output generation; they drop nothing a slow decoder has not read.
    - A RESET or rewind waits for its input purge by `g_ring_purge_done_seq`, which `replay_note_input_purge_consumed()`
      counts after the discard: the reader takes the flag itself when the input ring is empty, and otherwise waits for
      the demod that took it to finish discarding, so the next chunk is never the one discarded. Each wait covers one
      purge request (a RESET's is the finalize's), read against a mark taken before it, with no deadline. A RESET
      checks that its finalize moved the discard generation by exactly one; any other count is logged and counted
      (`replay_event_reset_purge_mismatch_count` in `rtl_stream_test_replay_state`).
    - The purge flag (`g_ring_purge_pending`) is cleared when a stream opens (`stream_open_init_pipeline()`, with the
      input ring's discard generation). A stop between a RESET's or a live retune's request and its take leaves it
      set, and the next stream's demod would otherwise discard its first input: a replay's chunk 1, with every later
      block's media time a chunk late. `g_ring_purge_done_seq` only counts on, so a mark taken before each request
      stays valid across streams.
    - A RETUNE keeps the centre the demod was last reset on, and the RESET after it resets the demod from there, as the
      live retune the pair records does (`demod_retune_reset_plan()`: a hop starts the band-edge FLL fresh, a hop back
      restores the cached seed). A rewind clears it.
    - A replay block's output always fits the empty output ring (a `static_assert` on `resamp_outbuf` and `result`
      against `kOutputRingCapacity`); a shortfall would be counted (`replay_output_truncated`) and logged.

    Tests: `IO_RTL_REPLAY_DETERMINISM` (an eventful capture: RETUNE, MUTE and RESET groups off chunk boundaries, a lone
    MUTE, a MUTE at offset 0 and at the end, identical across the fast, slow and realtime readers with every chunk a
    block and nothing discarded; a decoder stalled at a RESET or a rewind loses nothing; the demod taking the purge
    flag keeps the next chunk; a purge a stop left untaken does not reach the next replay; the reset plans of a hop
    and a hop back; bounded stops at an event boundary and at a rewind).
  - I/Q replay end to end (issue #572). With the pacing and events above, the streaming front end (see DSP) and the
    decode clock on the capture's time (see Runtime), a replay without `-T` or `-Y` prints the same decoder output on
    every run, fast or realtime, however the decoder is scheduled, capture-time timestamps included
    (`docs/iq-capture-replay.md`). The `iq-determinism` CTest cases hold it to that: `dsd-neo_test_replay_jitter`
    (`tests/engine/replay_jitter.c`) runs the real engine with seeded sleeps after the decoder's reads, short reads or a
    stalling asynchronous audio sink, and `tests/iq_determinism_check.cmake` requires every leg to match the first line
    for line (`docs/testing.md`, "Replay determinism"). The supervisory tuner autogain stays off under a replay, which
    has no gain to adjust, so its real-time windows log nothing there (`stream_open_enable_default_autogain()`,
    `demod_autogain_update()`; `IO_RTL_REPLAY_EOF_AND_CF32`).
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
- `src/protocol/provoice/provoice_frame.{c,h}` — the ProVoice frame-pair deinterleaver. It fills each IMBE 7100x4400
  frame with `dsd_vocoder_soft_bit` cells, a hard bit and the confidence in it, and `provoice.c` hands them to
  `processMbeFrameSoft()` (issue #588). The confidence is `dsd_two_level_symbol_reliability()`
  (`<dsd-neo/core/dibit.h>`) of the symbol `getDibitAndSoftSymbol()` returned: its distance from `center` over half
  the spacing of `min` and `max`, which for ProVoice are the class means the sync warm start set and stay fixed
  through the frame (the threshold tracker runs only for `rf_mod` 1 and P25 Phase 1). A legacy symbol capture stores
  only the decided bit and replays it as an ideal four-level amplitude, so its symbols get 255, a hard decision's
  weight; the soft capture format replays the measured symbol. The shared two-level slicer stores no soft metric, so
  `getDibitSoft()` does not apply to ProVoice, EDACS or D-STAR symbols. Cells the
  interleave schedule never reaches are bit 0 at reliability 0, which the decoder never reads.
  `mbe_process_provoice()` hands the decode's `mbe_process_result` to `mbe_processImbe4400Dataf()` unchanged: its
  `MBE_PROCESS_FLAG_PROVOICE` is what gives a muted frame ProVoice comfort noise rather than the P25 level, and
  `CORE_MBE_TRANSFORM_CONTEXT` pins the live path to mbelib's own ProVoice frame API through repeats into muting.
- `src/protocol/dstar/dstar.c` — `processDSTAR()` reads one superframe: 21 AMBE 3600x2400 voice frames and the slow
  data between them. `processDSTAR_HD()` decodes the RF header and then calls it without changing `state->synctype`,
  so the voice frames behind a header reach the vocoder as `DSD_SYNC_DSTAR_HD_*`. `processMbeFrameInternal()` sends
  every `DSD_SYNC_IS_DSTAR()` synctype to `mbe_process_dstar()`; before issue #599 only the voice synctypes went
  there, and a header's superframe was synthesized as DMR AMBE 3600x2450, whose model then carried into the next
  superframe. The two codecs share the ECC, so only the audio shows the difference: `CORE_MBE_TRANSFORM_CONTEXT`
  compares header and voice synctypes with mbelib's own 2400 frame chain. The voice is decoded with soft decisions
  (issue #599): each voice symbol is read with `getDibitAndSoftSymbol()`, its bit is the dibit's low bit as before,
  and its confidence is `dsd_two_level_symbol_reliability()`, on the same scale as ProVoice's, since D-STAR sync
  leaves the same warm-started class means. The slow data keeps hard bits. `processDSTAR()` hands the
  `dsd_vocoder_soft_bit` frame to `processMbeFrameSoft()`; the 24 cells the interleave never reaches are bit 0 at
  reliability 0, which the decoder never reads. `mbe_process_dstar()` passes the soft frame's bits through unchanged,
  so an invalid cell fails as on the hard decode, and hands the decode's `mbe_process_result` to
  `mbe_processAmbe2400Dataf()` unchanged: its C0 error count decides whether a tone frame is played (fewer than two)
  or replaced with comfort noise. `CORE_MBE_TRANSFORM_CONTEXT` pins the live path, hard and soft, to mbelib's own
  D-STAR frame API through a tone frame, repeats and muting, and `DSTAR_PROCESS` pins each cell's bit and
  reliability through the reader. `dstar_confirm` never reads vocoder output, so soft decoding leaves it unchanged.
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
    channel, `(row; default 16 kHz)` while an nfm scan row sets its own width, issue #526, `Analog: AM 8.333 kHz (row;
    default 6 kHz)` for an am row's, and under an analog row on a digital session too). The row's label and prompt read
    the configured width, the one the command edits. Both follow app-control's analog width view (above), and so does
    Qt: `MetricsModel::analogBandwidthHz`/`analogBandwidthDspLimited`/`analogBandwidthReading` (0 and `not used on PCM
    input` on PCM), `analogBandwidthMaxHz` is the widest width the running stream's demod rate filters (with none
    running, the rate an RTL-SDR or rtl_tcp input's DSP bandwidth sets), and `analogBandwidthConfiguredHz` is the value
    `qml/RadioSheet.qml`'s NFM width stepper (`radioAnalogBandwidth`, presets in `Util.NFM_WIDTHS_HZ`, stepping from the
    width in force when the default is set and skipping presets above the max) edits through
    `CommandBridge::setNfmBandwidthHz()`; `radioAnalogBandwidthDefault` sends 0 to return an explicit width to the
    default, and the controls are disabled on PCM input with the reason shown. Under another preset the section stays on
    a radio input while an explicit width is set (`analogWidthOffered`), reading the setting, so a width that blocks a
    switch to NFM can be narrowed first. An nfm or am scan row on air (`analogBandwidthRowActive`, issue #526) keeps the
    section, the width in force and the stepper on any preset (`analogWidthInForce`); a row's own width
    (`analogBandwidthRowOverride`) reads first with a `row` badge and the configured default beside it
    (`radioAnalogBandwidthRowNote`, as `radioSquelchRowNote` does for a row squelch), the value is read aloud as the
    view's full reading, and the stepper steps the configured default of the row's kind (from its unset default when it
    is unset, 16 kHz for an nfm row and 6 kHz for an am row, `analogDefaultWidth`; never from the row's width). The
    `NFM` decode chip (`-fA`) sits in `Util.DECODE_MODES`, so the setup wizard offers it too (it suggests no trunking).
    Tests: `UI_MENU_TREE_AUDIT`, `UI_MENU_ACTIONS`, `UI_MENU_LABELS_RADIO`, `UI_NCURSES_PRINTER_HELPERS`,
    `UI_QT_METRICS_MODEL`, `UI_QT_SESSION_ARGS`, `UI_QT_QML_CALL_LISTS` (`tst_radio_analog.qml`,
    `tst_wizard_decode_chip.qml`).
  - AM (issue #524): the decoder picker lists AM after Analog; on an input that is not an I/Q radio (the snapshot's
    input type) its row reads `AM (needs an I/Q radio input)` and choosing it only repeats the reason in the status
    line. Input > RTL-SDR has `rtl.am_bw` beside `rtl.nfm_bw` (`AM bandwidth... [default]`, `lbl_rtl_am_bw()`,
    `is_am_width_editable()`: the configured AM preset on a radio input, or an explicit AM width under another preset,
    NFM included, as the NFM row stays under AM for an explicit NFM width, or AM's default where the running stream's
    rate cannot filter it but filters a narrower AM width; both rows share one predicate,
    `dsd_app_analog_width_offered()` over the analog width view with the frontend metrics), whose prompt hands the value as typed to
    `DSD_APP_CMD_AM_BANDWIDTH_SET`; the Auto-PPM switch leads the `Auto-PPM & rtl_tcp` submenu, so the RTL menu keeps
    fifteen rows. The status line's `Analog:` field reads `Analog: AM 6 kHz (default);` under AM, from the same view.
    Tests: `UI_MENU_ACTIONS`, `UI_MENU_LABELS_RADIO`, `UI_MENU_TREE_AUDIT`, `UI_NCURSES_PRINTER_HELPERS`.
  - Tone filter (issue #527): the Audio menu's `audio.tone_filter` row (`Tone filter... [allow 100.0 Hz/D023N]`,
    `lbl_tone_filter()`), beside the other analog monitor rows and always shown, reads the rx tone view's policy text
    (`(row; default X)` under a row's own policy; the overflow mark as ASCII `...`; lowercase, as Call Info and other
    shared-view values such as `[default]` read). `act_tone_filter()` opens a picker (`Off`, `Allow list...`,
    `Block list...`, `Off and clear list`) on the configured mode; allow and block prompt for the list filled in with
    the configured one, from `dsd_app_tone_filter_setting_get()` on the snapshot pair (never a row's own policy), and
    the list goes to `dsd_app_command_set_tone_filter()` as typed for the command to check. Off sends the mode alone
    (`dsd_app_command_set_tone_filter_mode()`), so the decoder keeps the list it holds; `Off and clear list` sends off
    with no list. The label and the picker read the same pair: before the first snapshot, the menu's own options with
    no state. Tests: `UI_MENU_ACTIONS`, `UI_MENU_LABELS`, `UI_MENU_TREE_AUDIT`.

Qt Quick frontend (`src/ui/qt`):

- AM (issue #524): `qml/Util.js` `DECODE_MODES` carries an `AM` chip (`-fM`) marked `iqOnly`, which the Radio sheet
  (`metrics.radioInput`, with a `radioDecodeIqNote` saying why) and the add-system wizard (`radioSource`) do not offer
  for audio that arrives demodulated. The wizard drops an `iqOnly` flag back to Auto when its source moves off the radio
  (`dropRadioOnlyDecodeFlag()`) and holds step 1 while one remains, and `session_args_build()` refuses `-fM` on a
  network or file source (`SessionArgsError::AmNeedsRadio`), for a system saved before; the wizard's
  `wizardDecodeIqNote` says why the chip is greyed out. On a USB or rtl_tcp radio the AM default has to fit the DSP
  bandwidth the spec carries (the engine refuses 4 and 6 kHz before the device opens):
  `session_args_am_fits_bandwidth()` is that rule, `session_args_build()` refuses the pair
  (`SessionArgsError::AmBandwidth`), and the wizard holds step 1 with `wizardDecodeAmBandwidthNote` through the
  `sessionArgs.amBandwidthError()` invokable. The Radio sheet's analog section is kind-aware: under the AM preset
  (`sectionAm`, which follows `MetricsModel::analogBandwidthAm` while an analog scan row is on air, since an nfm row
  runs FM over an AM session) its title reads `AM channel width`, it steps over `Util.AM_WIDTHS_HZ`
  (5000/6000/8000/10000/15000/20000) with `Util.nextWidthIn()`, which takes the kind's list, through
  `CommandBridge::setAmBandwidthHz()`, and it reads the same `MetricsModel::analogBandwidth*` properties, which the
  analog width view fills for the configured kind. `MetricsModel::nfmBandwidthConfiguredHz`/ `amBandwidthConfiguredHz`
  publish each kind's setting whichever preset runs, from the configured view (never a row's own width), so a second
  control (`radioAnalogOtherSection`) edits an explicit width of the kind the section does not (AM under `-fA` or a
  digital mode, NFM under `-fM`), which a switch between the kinds is held to. A width request not yet answered belongs
  to its kind, so a change of the section's kind drops both sections' outstanding requests (`onSectionAmChanged`) rather
  than show one kind's request under the other. Tests: `tests/ui/qml/tst_radio_am.qml`, `tst_wizard_decode_chip.qml`,
  `UI_QT_METRICS_MODEL`, `UI_QT_SESSION_ARGS`, `UI_QT_CONTROLLER`.

- After the session's first decoder redraw, `UiController` refreshes live metrics on every timer tick so scan
  countdowns and the sync-loss hold continue aging if input stalls. The countdowns age on the decode clock their
  deadlines were stamped with, so in a replay they freeze while samples stop; the sync hold is real time and expires
  either way. History, network and policy models still refresh on decoder redraws; session lifecycle clears live
  metrics and prevents stale snapshots from restoring them.
- Two clocks: `MetricsModel` ages the call lines, the call-skip count and the scan countdown on the decode clock, and
  publishes its wall-clock now as `decodeNowMs` (read live; `decodeNowMsChanged` fires from `refresh()` when the decode
  second moves). QML compares decoded stamps only against it: `Util.shortAge(when, metrics.decodeNowMs)` for the
  Monitor's recent-call ages. The session views read no clock at all (next bullet).
  The viewer's own moments stay on real time through `realtime_clock.h`, and QML ages a saved system's `lastHeard`
  against `savedSystems.realtimeNowMs()` (`Util.heardText(lastHeard, nowMs)`). The sync-label hold (in the Qt panel and
  the Android notification record, `app_control/notification_status.c`) and the received-tone input-pause check use
  `dsd_realtime_mono_s()`. One sanctioned exception compares a decoded stamp with real time: `day_label()` in
  `call_history_model.cpp` labels history sections "TODAY"/"YESTERDAY" against the viewer's real calendar day, paired
  with the real midnight rollover timer, because the log spans sessions and a replay's calls keep their own dates.
  Tests: `UI_QT_METRICS_MODEL`, `UI_QT_QML_CALL_LISTS` (`tst_monitor_recent_calls.qml`),
  `APP_CONTROL_NOTIFICATION_STATUS`.
- Call history identity, and order across sessions (`call_history_model.{h,cpp}`), never come from the rows'
  stamps. Those are decode time, a replay's are the capture's, and the decode clock may not have moved to the capture
  when a start reads it.
  - Session: `CallHistoryModel::session()`, persisted in `callHistory/session`. `Main.qml` calls `beginSession()` on
    every start, after `UiController::flushHistory()` has logged the previous session's tail. A replay start or an
    input change is a start, and so is the first start after a process start. Every row carries the session that
    logged it, or that last extended it through a merge (`session` role, persisted per row and per seen entry).
  - Views: the monitor's recent calls (`monitorView.historySession`) and the heard talkgroups
    (`TalkgroupListModel::historySession`) select the running session's rows; 0 selects the whole log. A UI
    reattaching to a running session (the service survived an Activity restart) takes the session the history kept,
    so it sees the calls its predecessor logged in that session.
  - Clear (`clearAll()`) names what it wiped by ring position: push_seq per slot as last read (`callHistory/clear/*`).
    Whatever the ring takes in after the clear shows, whatever its stamps. A relaunched UI does not ingest the cleared
    rows again, even if the seen store lost them. The position means something only in its own ring, so the mark also
    names that ring (`Event_History_I::instance`, `callHistory/clear/ring`), and the ring, not the session, is the
    binding. A start usually brings a fresh ring, but an embedding host can reuse the last run's state, ring, rows and
    push count included (the Android service does when a start races `stopSelfLatest()`). So `beginSession()` keeps a
    mark that names its ring, and keeps the last read position, which names the ring it was read in. A clear after a
    start, before the next read, therefore covers what the last read found: a reused ring's cleared rows stay cleared,
    in the new session and across a relaunch. A read of any other ring drops the mark, whatever its position: a fresh
    ring after a start, or a ring replaced without one, can already be past the mark at its first read and shows its own
    first rows. Every ring is read at its first tick, even at the `commit_rev` the last one stopped at, so a quiet
    previous session does not delay that read. Only a freshly constructed model clears before reading the ring, and its
    clear binds to what the first read finds. `beginSession()` drops such a pending mark, and a mark that names no ring,
    since either could bind to a fresh ring and hide its first rows. A mark that names no ring is dropped at the first
    read too. It comes from a model that had read nothing when a start came, which sits at position zero and covers
    nothing, or from a build before marks named their ring, whose ring died with the process an app update replaced. The
    decode-time watermark older builds wrote (`callHistory/clearedThrough`) is removed on load. The rows it cleared had
    already left the store, and the ring it guarded died with the replaced process.
  - Retention: a full log (1000 rows) gives up the oldest session's oldest row, and the seen map and its store keep
    the newest entries in the same (session, start) order. Ranked by stamps alone, a replay's calls would be trimmed
    as they landed and its ring rows logged again as new calls.
  - Calls heard again: a capture replayed in a fresh state pushes the same rows at the same push stamps, with the same
    starts and targets, so the seen key alone cannot tell them from rows already read. Each seen entry keeps the ring it
    was read from (`Event_History_I::instance`, a nonce `initState()` draws for the ring; persisted in the seen store as
    hex), and a new ring is walked whatever its `commit_rev`. A key read from another ring is a call heard again, taken
    in as the live path takes one: it merges into the logged call it overlaps, searched through the whole log rather
    than the newest 32 rows, and that row joins the running session (a notice heard again promotes a field-for-field
    twin no other notice of its ring has taken, so two identical notices promote two rows, and a new ring within the
    session logs none twice; the seen store keeps the twin each such notice took, so a relaunched UI rebuilds what its
    ring has taken). Every in-place update (a seen row that advanced: its end extending, its source learned, its crypto
    verdict) searches the whole log too, newest first. So it reaches a replayed call's row under newer rows, whether the
    call was a first sighting or heard again, after a relaunch as well, and a live update still lands on the row the
    newest rows hold for it. So a second replay shows its calls in the new session's views, logs no new row for a call
    the first replay logged, and a replay after a clear logs them again. A first sighting still merges within the newest
    32 rows only: a call the first replay logged as two rows, because newer rows sat between its fragments, stays two
    rows, and the repeat folds into the newer one. An entry that knows no ring (a row's seed in `load()` when the seen
    store has lost its entry, or an entry from an older store) is taken to be the reading ring's, so a relaunch never
    moves a single-fragment logged row into the running session. The seed covers single-fragment rows only: a merged row
    keeps one fragment's slot and push stamp but the earliest fragment's start, so its seed's key is no ring row's. A
    merged call whose seen entries were evicted or lost is logged again as new rows, one per fragment. A state an
    embedding host reuses for another run keeps its ring and identity, and the rows the earlier run left there stay that
    session's.
  - Tests: `UI_QT_CALL_HISTORY_MODEL`, `UI_QT_TALKGROUP_LIST_MODEL`, `UI_QT_QML_CALL_LISTS`
    (`tst_history_session_identity.qml`), `CORE_INIT_STATE` (the ring identity).
- Received tone or code (issues #522, #523): `MetricsModel` publishes the `rxTone*` group (`rxToneVisible`,
  `rxToneStatus`, `rxToneText`, `rxToneKind`, `rxToneTenthsHz`, `rxToneDcsCode`, `rxToneDcsInverted`,
  `rxToneDcsAliasCode`, `rxToneDcsAliasInverted`, `rxToneCarrier`) with its own `rxToneChanged` signal, filled from
  `app_control/rx_tone_view` in `fillRxToneView()` against real monotonic time, and returned to unknown by
  `clear()` on stop. `rxToneConfiguredText` sits beside it with a signal of its own, `rxToneConfiguredTextChanged`,
  because it is configuration rather than session state: it reads the view's `off` from construction, keeps its value
  across a stop, and no received-tone change announces it. `qml/MonitorScreen.qml` shows it as the `RECEIVED TONE` row
  (`monitorRxTone`) and the policy as the `TONE FILTER` row (`monitorToneFilter`), bound to `rxToneConfiguredText` and
  the `toneFilter*` group (issue #527: `toneFilterVisible`, `toneFilterGate`, `toneFilterStatusText`, translated
  verdict words, with their own `toneFilterChanged` signal, cleared on stop while the policy text stays), so the policy
  can never be mistaken for, or feed, the received tone. The terminal shows the same view as the Call Info `Rx tone:`
  line (`ui_format_rx_tone_line()`) and `Tone filter:` line under it (`ui_format_tone_filter_line()`), compact view
  included. The Android notification is unchanged. The row is also on screen with no policy wherever detection runs
  (`toneFilterEditable`, the view's `policy_editable`), reading `off`, and its `Edit` button
  (`monitorToneFilterEdit`) opens the live editor, `qml/ToneFilterSheet.qml` (issue #527): an Off/Allow/Block
  `SegmentedControl` and a list field, opened on the configured policy (`toneFilterConfiguredMode`,
  `toneFilterConfiguredList`, on their own `toneFilterSettingChanged` signal and kept across a stop as configuration;
  never a row's own), a note while `toneFilterRowOverride` says a row's own policy shadows the edit, and the field's
  inline error from `CommandBridge::toneFilterError()` (the decoder's own `dsd_tone_filter_check()` message), which
  disables Apply; Apply sends `CommandBridge::setToneFilter()`. The row, and so the editor, follows detection on the
  channel on air (`dsd_analog_tone_detection_active()`): the snapshot pair carries neither the channel map's row modes
  nor the trunk-scan targets, so on a scan that mixes `nfm` rows or `nfm-conventional` targets with other modes it is
  reachable while one of those is on air; the terminal row is always shown. Tests: `UI_QT_METRICS_MODEL`,
  `UI_QT_CONTROLLER`, `UI_QT_QML_CALL_LISTS` (`tst_tone_filter_sheet.qml`, `tst_monitor_rx_tone.qml`).
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
  ingestion, merging listed rows with uncovered talkgroups heard this session (history rows of `historySession`).
  `talkgroup_filter_model.{h,cpp}` filters by category and name/ID for `qml/TalkgroupsScreen.qml`, opened by the
  monitor's **TG list** action.
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
  vocoder (`mbe-neo` 2.2+).
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
- `dsd_channel_modes_present()` is true for declared modes and for option-bearing profiles alike, so option-only channel
  maps run the typed scanner. `dsd_channel_mode_hears_tones()` says whether a row runs received-tone detection while it
  is on air: an `nfm` row, or, when the configured decode mode is the FM monitor, one that declares no mode and so runs
  it (issue #527); `dsd_channel_modes_hear_tones()` whether any row the scanner tunes (one with a frequency) does, and
  `dsd_channel_modes_conventional_hear_tones()` whether a session outside a trunk scan does: a `-Y` list with rows by
  its rows, anything else (a `-Y` scan without rows included) by the configured decode mode.
  `dsd_scan_settings_equal()` compares acquisition settings only; the row-scoped options (forcing, CRC, mutes, voice
  gate, group file) are folded through `dsd_scan_mode_resume()` without resetting acquisition. Conventional trunk-scan
  targets take their voice-gate hold/qualify from the row profile. `DSD_SCAN_OPT_MAX_VISIT` is the one **scan-timing**
  row option accepted on trunked types as well, and its `scan_max_visit_ms` travels in the same row-option block,
  outside `dsd_scan_settings_equal()`, so a row cap never restages a tune.
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
  `RTL_SET_SQL_DB` is not a scoped command, nor is `RTL_SET_SQL_SETTING` (the auto squelch's whole setting,
  `svc_rtl_set_sql_setting()` through `dsd_scan_mode_set_configured_squelch_setting()`; `svc_rtl_push_squelch()`
  hands the demod a level through `rtl_stream_set_channel_squelch()` and an AUTO setting whole, and does nothing in a
  build without radio support, where both commands still set the configured default for audio input):
  `svc_rtl_set_sql_db()` edits the configured default through
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
- `DSD_SCAN_OPT_BANDWIDTH` (`--nfm-bandwidth-hz` on nfm rows, `--am-bandwidth-hz` on am rows, issue #526) is an analog
  row option: each spelling's mode mask (`NFM`, `AM`) keeps it off the other kind's rows, refused there as
  `not supported for this mode/target`, and off digital and blank rows, which are told `needs mode nfm` or
  `needs mode am` (`option_mode_allowed()` names the analog classes an analog-only switch serves), while `ANY_MODES`
  (digital plus analog) carries `--squelch-db` and `--scan-max-visit-ms` onto analog rows and every other switch stays
  `DIGITAL_MODES`. The diagnostic names only the classes the switch serves (`needs mode nfm`), not every analog class.
  The value is whole Hz in the range of the kind (`dsd_analog_width_parse()`), recorded in `channel_bw_kind`;
  `dsd_scan_option_width_check()` holds it to a DSP rate with the validator's message for that kind. Unlike squelch it
  is an acquisition setting (see Per-channel decoder modes), so a width change restages a parked row and the configured
  width is never replaced by a row's in the configured view: saves write the configured width, and the width command
  edits it through `dsd_scan_mode_set_configured_analog_width()`, which leaves a row's own width of the same kind in
  force (an nfm row's own width shadows no AM edit, an am row's none of the NFM width). `RTL_SET_BW` stays unscoped, so
  the stream it reopens starts on the width in force, a row's included; `svc_rtl_set_bandwidth()` refuses a bandwidth
  whose DSP rate cannot filter that width, as well as the configured one (on RTL-SDR and rtl_tcp, where the bandwidth
  sets the rate), rather than let the start refuse it and leave no radio input; the toast names the scan row's width
  (`DSP BW 12 kHz cannot filter the scan row's NFM 12.5 kHz (max 9.6 kHz); keep a wider DSP bandwidth`), or gives
  the width's own fix where the row runs the configured width. Input > Switch source > RTL-SDR
  (`svc_check_rtl_input_analog_width()`), unscoped too, holds the same two widths. A config apply is scoped, so it holds
  a row's own width, of its kind, explicitly at a reopen (`cfg_check_scan_row_width()`). On a session whose preset runs
  another kind or none, the configured width of a kind counts as in use for all of these, and for the width command,
  while the scan has an analog row or target of that kind without a width of its own
  (`dsd_engine_scan_runs_configured_nfm_width()`, `_am_width()`; beside the preset's own width,
  `svc_scan_width_beside_preset()`, `cfg_check_scan_widths()`). The option's preview fields, `bandwidth_hz` in
  `dsd_csv_channel_profile` and `dsd_app_scan_csv_target` (-1 when the row inherits), reach the Qt/Android channel-map
  review and target preview as `bandwidthHz` (invalid when the row inherits), which read `NFM bandwidth: 12.5 kHz` or
  `AM bandwidth: 8.333 kHz` by the row's mode or target type, or `inherit` on an analog row without one
  (`Util.analogBandwidthSummary()`).
- `DSD_SCAN_OPT_TONE` (issue #527) is one bit for three spellings, `--tone-allow <list>`, `--tone-block <list>` and the
  explicit row disable `--no-tone-filter`, so combining them is a duplicate; `NFM` only (`needs mode nfm` on digital and
  blank rows; an am row or `am-conventional` target, whose AM monitor hears no tone, refuses it as
  `not supported for this mode/target`). The setter parses with `dsd_tone_set_parse()`, and `option_invalid()` asks it
  again for the entry-numbered reason. The values (`tone_filter`, `tone_set`) land in `dsd_opts::analog_tone_filter` /
  `analog_tone_set` and the leading row block of `dsd_scan_settings`, outside `dsd_scan_settings_equal()` (policy, not
  acquisition). `dsd_scan_mode_configured_tone_policy()` reads the configured one for saves; the rx tone view marks a
  row's own `(row; default X)`. The live editor's command edits the configured policy through
  `dsd_scan_mode_set_configured_tone_policy()`, which leaves a row's own in force and, like the squelch and width
  setters, never suspends the scope, so a tone edit under a P25 row keeps its detected polarity and followed call.
  Scan coupling is the engine's (see Engine): PENDING holds a row while its carrier lasts
  but is no activity (no tail, no hold or dwell restarted), REJECTED releases it at the scanner's next pass, and on an
  am row or target no verdict is in force (`dsd_analog_tone_gate_in_force()`), so the carrier holds it as `Carrier`. The
  CSV splitters end the options cell at an unquoted comma, so both importers (`chan_import_options_split()` in
  `dsd_import.c`, `scan_check_options_split()` in `trunk_scan.c`) ask `dsd_scan_options_tone_list_split()` whether the
  cell ends with a tone list and the field after it goes on with it (any text past the header; in a named column, a
  field that is one space-free run of `/`-separated entries starting with a standard tone or code, alone or followed by
  a row option's switch, the rest of the cell, so a name such as `100 Main St` stays the column's own) and refuse the
  row with `use / between entries, not commas` rather than import it short or blame the column the rest landed in. The
  import previews carry a row's own policy as `tone_filter` (its `dsd_tone_filter_mode`, -1 when the row runs the
  configured one) and `tone_list` (its list as displayed) in `dsd_csv_channel_profile` and `dsd_app_scan_csv_target`,
  both filled by `dsd_scan_option_tone_summary()`; they reach the Qt/Android channel-map review and target preview as
  `toneFilter` (invalid when the row inherits) and `toneList`, which read `Tone filter: allow 100.0 Hz/D023N`, `off`, or
  `inherit` on an nfm row without one (`Util.toneFilterSummary()`). Tests: `RUNTIME_SCAN_OPTIONS`, `RUNTIME_SCAN_MODE`,
  `RUNTIME_CONFIG_USER` (never saved; an edit under a row saves and loads back), `RUNTIME_CONFIG_APPLY` (a loaded
  config under a row's own policy),
  `CORE_CSV_IMPORT` and `APP_CONTROL_TRUNK_SCAN_VALIDATE` (row diagnostics, am rows and targets, previews),
  `ENGINE_NO_CARRIER_RESET` (`-Y`, the legacy untyped list and a list with nowhere else to go, a row refused its width
  included, an am row, rows sharing the frequency on air), `ENGINE_TRUNK_SCAN` (a target refused its width included, an
  am target, a same-frequency target of another type), `UI_QT_IMPORTED_FILES`, `UI_QT_SCAN_LIST_ROUNDTRIP` and
  `UI_QT_QML_CALL_LISTS` (previews).
- Session edits ("this channel", issue #518): while a scan runs, the operator can change the squelch, channel width and
  tone policy a row runs, and a `--trunk-scan` target's tuner gain, for the rest of the session without touching the
  list or the configured defaults. Runtime `scan_row_edit` (`<dsd-neo/runtime/scan_row_edit.h>`, pure data) holds one
  row's edits, each field none, set or inherited (follow the configured default whatever the list says);
  `dsd_scan_row_edit_apply()` lays them over a row's options and `dsd_scan_row_edit_gain()` over a target's list gain,
  and the fields a class takes come from the options grammar (`dsd_scan_options_fields_for_mode()`). The trunk-scan
  coordinator keeps each target's edit in its runtime (`dsd_trunk_scan_target_runtime::edit`) and every reader of a
  target's options goes through `trunk_scan_target_values()` (the profile's values, a live decryption change included,
  with the edit over them) and `trunk_scan_target_gain()`: the scope installed at a switch, a rollback and a decryption
  change, the width checks and skips, the configured-width rule and the tone-rejection alternates. The `-Y` channel scan
  keeps `{row, edit}` pairs in its own extension, which a leave frees (a map change leaves first), and reads rows
  through `channel_scan_row_values()` (`dsd_engine_channel_scan_row_values()` for `trunk_scan.c`'s configured-width
  rule); an edit moves `edits_epoch`, which restages a tune staged before it (`channel_scan_staged_stale()`), and the
  staged row's merged values are what its tune's rigctl leg reads. `dsd_engine_trunk_scan_edit_target()` and
  `dsd_engine_channel_scan_edit_row()` name the row by a scan session number (`dsd_scan_row_edit_new_session()`, new for
  every coordinator and `-Y` scan) and the target id or `-Y` row, never the tune generation, which moves on every
  retune: an edit to the row on air (its scope in force) applies at once and says whether app-control must publish the
  width (the width in force moved, or whether the row sets one of its own did, which is what a rigctl peer's request
  follows: `dsd_scan_row_edit_width_request_differs()`) or reopen the stream for the gain, with the previous edit for
  `..._restore_...()` should that fail, which puts back only the field it names (`dsd_scan_row_edit_take_fields()`) so
  an edit of another field made since stays; any other row runs it from its next visit. A width the published DSP rate
  cannot filter is refused; the trunk change runs under the P25 SM tick guard (try-enter, BUSY), and a width or gain
  edit of the parked target while its retune is in flight is BUSY too (the retune lands the width it queued, and only a
  request app-control makes itself can be rolled back). `dsd_state::scan_row_*` (in the `vertex_ks_count..ui_msg`
  snapshot range) publishes the row on air, the fields it can take, sets in its list and runs an edit of;
  `dsd_scan_mode_options()` stamps `dsd_opts::scan_row_scope_seq` and `dsd_state::scan_row_scope_seq` alike at every
  install, so a frontend reading the two snapshots, published one after the other, can tell whether the options it
  starts an editor from are this row's (`dsd_app_scan_row_view::opts_match`, required by
  `dsd_app_scan_row_view_offers()`, the Qt context and Qt's `scanRowSynced`); a Qt width step on the row starts from
  that snapshot's width setting (`analogBandwidthSettingHz`), never the live front-end reading. App-control's
  `DSD_APP_CMD_SCAN_ROW_EDIT` (`src/app_control/scan_row_edit.c`) applies one edit and its follow-up, and rolls back
  that field when the follow-up fails:
  - the RTL front end's width request, when the width in force moved (`svc_publish_row_analog_width()`, marked
    `svc_monitor_refusal::row_edit`). A refusal where it lands goes through `dsd_app_scan_row_edit_settle_refusal()`,
    never the configured width. The row's width goes back to the edit the front end ran the width it kept under: the
    newest baseline of the run of requests it has not all run (`svc_row_width_request_not_run()`) that ran that width,
    else the oldest, as the configured width's first baseline does. When the edit put back follows a default that
    changed meanwhile, the settle asks the front end for that width once (`reconcile`, never chased again). A later
    width edit of the row that stands without a request of its own (stored for its next visit, applied without
    moving the width in force, or one the front end queued nothing for: `svc_publish_row_analog_width()` returns 1
    only for a request queued) leaves the refusal nothing to put back;
  - a rigctl peer's passband on audio input (`dsd_engine_scan_rigctl_apply_modulation()`, strict where a tune's `-B`
    is best-effort, and reading the scope in force, `dsd_scan_mode_row_options()`, never a `-Y` tune staged since);
  - the stream restart for a gain. One P25 SM tick guard hold covers the reopen and, when it fails, the gain's rollback
    (`dsd_engine_trunk_scan_restore_target_edit_locked()`) and `svc_rtl_restart_recovery_locked()`, which starts the
    input again without reopening an I/Q capture over its recording.

  The trunk restore otherwise waits for the tick guard rather than give up. `<dsd-neo/app_control/scan_row_view.h>` is
  what every frontend asks (row on air, fields, label, payload) and how they word the outcome. The terminal's squelch,
  width, gain and tone rows open a scope chooser through it (`scope_or_default()` in `menu_actions.c`); Qt's Radio and
  Tone filter sheets offer "All channels | This channel" (`MetricsModel::scanRow*`,
  `CommandBridge::scanRowContext()`/`editScanRow()`), holding in "This channel" a control the row takes no edit of, and
  an Airspy's device panel, and taking the choice only once the metrics show the row the context names; rows are told
  apart by `scanRowKey` (scanner, session, row), not by name. Tests: `RUNTIME_SCAN_ROW_EDIT`, `ENGINE_TRUNK_SCAN`
  (`test_target_edit_*`), `ENGINE_CHANNEL_SCAN` (`test_row_session_edits`), `ENGINE_TRUNK_RETUNE_REGRESSION`
  (`test_rigctl_live_edit_apply_is_strict`), `APP_COMMAND_QUEUE` (`test_scan_row_edit_commands_*`),
  `APP_CONTROL_SCAN_ROW_EDIT`, `APP_CONTROL_SCAN_ROW_VIEW`, `UI_MENU_ACTIONS` (`test_scan_row_scope_chooser`),
  `UI_QT_METRICS_MODEL`, `UI_QT_CONTROLLER` and `UI_QT_QML_CALL_LISTS` (`tst_scan_row_edits.qml`).
- Adding a row option: add the `DSD_SCAN_OPT_*` bit (reserved values only), a `dsd_scan_option_values` field and a
  `specifications[]` row with its setter in `runtime/scan_options.c` (use `ANY_MODES` only for options that mean the
  same on every class); add a `scan_option_appliers[]` row in `runtime/scan_mode.c`; if it lands in `dsd_opts`, add
  the field to the leading row block of `dsd_scan_settings` and to `dsd_scan_settings_capture()` (which copies each
  field by name, so a missed field captures a zero baseline that leaving the row would restore),
  `scan_settings_restore_row_opts()` and `scan_settings_copy_row_opts()` (not the equality list, unless it changes
  acquisition). If the value also lives in hardware, push it through a runtime hook from the scope's entry points
  (the `options` call that completes a row, `leave`, and always after `resume`), never from `enter` alone or
  `prepare`. Saves and editors read the configured view, and an editor that must not disturb acquisition edits the
  configured baseline directly, as `dsd_scan_mode_set_configured_squelch()` does, rather than suspending the scope. An
  option the operator should be able to change for "this channel" also needs a `DSD_SCAN_ROW_FIELD_*` field in
  `runtime/scan_row_edit.c` (the value, `dsd_scan_row_edit_apply()`, its check) and a frontend control.
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

### QML garbage collector

Every process that builds a QML engine calls `dsd_qt::applyQmlGcPolicy()` (`src/ui/qt/qml_gc_policy.h`)
before the first engine exists: `android/main.cpp`, the Qt Quick Test `Setup`, and the C++ tests that load
production QML. It sets `QV4_GC_TIMELIMIT=0`, so each collection runs to completion instead of in Qt's
default 5 ms time slices. Qt 6.11.2's incremental collector can free a QML object's function storage while the
object is still being created. A `Connections` element's `function onX()` handler then crashes
`QQmlConnections::connectSignalsToMethods()`. A Qt build with qtdeclarative 42c76d7acd (QTBUG-148459) skips
the handler instead, without any error. On Qt 6.11.2 under load, the session-tools suite crashed 14 runs in 24
with the default slices and none in 24 with the policy. The policy keeps a `QV4_GC_TIMELIMIT` that Qt parses as
an integer.

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

Frequency entries take the protocols `p25`, `dmr`, `nxdn48`, `nxdn`, and the analog `nfm` and `am` (issue #526), which
map to `-fA`/`-fM` and on to `nfm-conventional`/`am-conventional`; saved analog systems map the same way (exactly `-fA`
or `-fM`, `scan_list_entry_analog_kind()`) and are refused when trunked or given a channel map. An analog target
(`Target::analog`) carries no modulation, keys, key files or group file, a saved analog system's included (so none of
those retained paths is checked or has to exist), and `ScanEntryRow.qml` hides its modulation and decryption controls
(`Util.decodeFlagIsAnalog()`, the same exact-flag rule, for a saved system's flags). `scan_list_starter` resolves,
checks and retains no decryption profile for an analog entry, so a choice the editor hides can never block the list. Its
validation also holds the analog targets to the DSP rate an RTL-SDR or rtl_tcp list runs at (the list's bandwidth, else
the app's): an analog target whose width that rate cannot filter (its own, else the configured width of its kind the
app's Extra arguments set, `configuredAnalogWidthHz()`, else an `am` entry's 6 kHz default) is named in a warning beside
the targets ready, as the engine names such a row when a map loads; a SoapySDR or Airspy list is left to the engine's
scan-start check. The Qt/Android channel-map review (`ImportsScreen.qml`) and target preview (`ScanListScreen.qml`) show
no keys or identifiers for an analog row and no modulation for an analog target, which the parser refuses there.

`UI_QT_SCAN_LIST_TARGETS` covers preservation/rejection and option screening, the analog mappings included;
`UI_QT_SCAN_LIST_ROUNDTRIP` exercises persistence and the real facade, with a mixed digital/NFM/AM manual list, a
CSV-backed list whose analog options reach `--trunk-scan` unchanged, analog entries whose hidden decryption is ignored,
and the DSP-rate diagnostic. `ENGINE_TRUNK_SCAN_SCAN_LIST` sends a generated three-target CSV through the real
coordinator, group-policy and key ownership code, replacing only tuning side
effects. It verifies policy/keys on rotation and baseline restoration on shutdown. A generated mixed list (a keyed DMR
system, a keyed saved AM system, nfm and am entries, a target with its own NFM passband and a DMR conventional target)
rotates through the real coordinator with a loopback rigctl peer on audio input, modelled on SDR++ (it refuses `NFM`,
keeps a passband per mode and takes 0 as "unchanged"): the analog targets install no keys or policy, each asks the peer
for its demodulator and passband, the targets after the one with its own passband run the peer's own again, a peer that
refuses AM makes the advance move past the am target, and shutdown returns the peer to FM with its own FM and AM
passbands. The scan-list, Home and Monitor QML cases run in `UI_QT_QML_CALL_LISTS`.
