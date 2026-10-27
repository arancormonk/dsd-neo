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
  - `frontend.c` owns the run's lifecycle hooks: the terminal frontend and/or the control API. With the API but no
    terminal it opens the app-control frontend runtime itself; at stop the API goes before the runtime it submits to.
    It hands the parsed API token to the server and wipes it from `dsd_opts`, and publishes telemetry once right after
    starting it, before the decoder runs: the API feed's first look, from which every event row reaches subscribers.
- Build files: `apps/dsd-cli/CMakeLists.txt` (`DSD_CLI_HAS_API` when `DSD_ENABLE_API` builds `dsd-neo_api`)

## Engine

- Path: `src/engine` (including `src/engine/dispatch`), `include/dsd-neo/engine`
- Targets: `dsd-neo_engine`, `dsd-neo_dispatch`
- Responsibilities:
  - Top-level decode runner and lifecycle (wires core/runtime/IO/protocol state machines). At the end of a run
    `dsd_engine_run_with_lifecycle()` commits the session's last calls (`dsd_engine_finalize_calls()`: end the slots
    as sync loss, commit their rows, retire the held VOICE_END alerts) and publishes telemetry before it calls the
    frontends' stop hooks, so a frontend that records calls gets the final transmission; `dsd_engine_cleanup()`
    repeats the step, which then finds nothing left to do
  - Protocol/frame dispatch glue
  - Signal search (issue #537): `signal_search.c` / `engine/signal_search.h` coordinate
    capture measurement, tagged asynchronous tuning, identification and listening on
    the decoder thread. Its private owner shares the stable trunk-scan extension slot
    through `scan_owner.h`; extension IDs are unchanged. Search excludes other tuner
    owners, suppresses visited frequencies per pass and retains native session avoids.
    Its decoder baseline is the CONFIG-profile Auto preset, which leaves the DMR dispatch
    flags as the session had them; when a mono preset (`-f1`) has turned both off, the
    baseline takes dual-slot dispatch (`dmr_stereo`), since with both off no direct-mode
    voice frame is read, and Stop restores the session's own flags (`ENGINE_SIGNAL_SEARCH`).
    Runtime frame/symbol hooks service cancellation and deadlines inside long protocol
    loops; the in-frame yield check also services live strength, since a continuous
    digital transmission keeps the decoder in its synced-frame loop. A live strength that no
    accepted measurement has refreshed for three intervals and a retry (1.75 s), and every
    ended visit, publishes no live level (`ENGINE_SIGNAL_SEARCH_SCENE`); each accepted
    measurement bumps `live_seq`, so a frontend tells a fresh reading of an unchanged level
    from no reading. The status names the range being searched (`range_name`,
    `range_start_hz`, `range_end_hz`), set from the coordinator's own configuration by
    `search_select_range()` at start, at each span and at a paused restart, never from the
    retained hit, whose range can be an earlier one (`ENGINE_SIGNAL_SEARCH`). A stalled input
    cannot hold a visit: the stall check snapshots the producer's last-sample timestamp before reading the real-time
    clock, and guards the subtraction so a newer sample cannot wrap its age into a false timeout. While a search owns
    the tuner (`rtl_signal_search_owner()`; the orchestrator's read carries no decoder state), a live radio read that finds no
    samples for 100 ms gives up, and every RTL symbol reader (FSK discriminator, monitor,
    CQPSK symbol rate) records that as `dsd_state::input_interrupted` instead of ending
    the session. `getSymbol()` checks a recorded interruption before its every-64-symbol
    yield throttle, so the rest of the frame reads as erasures without waiting again
    until the engine loop consumes it. Whether the yield (a deadline, Skip, Pause or any
    queued command) or a stall cut it, the frame decoders drop that frame as one a
    carrier boundary splits (`dsd_state_frame_read_lost()`; see the carrier count's
    guards under `dsd_engine_carrier_boundary()` in Protocols): nothing of it plays,
    publishes, captures or reports evidence. An analog visit's
    500 ms quiet timing starts only at the monitor's first verdict on the landed channel
    (`dsd_analog_rx_carrier_observed_now()`), bounded at 3 s. `DSD_SEARCH_ACTION_UNAVOID`
    lifts a session avoid by frequency; `DSD_SEARCH_ACTION_AVOID_FREQUENCY` (appended, hit ID
    may be 0) adds one, a duplicate succeeding and the frequency being visited being left as
    `AVOID` leaves it. A full session list refuses either avoid and publishes "The avoid list
    is full" in `status.message` (in the ERROR phase the search error stays instead); any
    action that takes effect, or the next visit's tune (`search_queue_tune()`), clears an
    earlier refusal, and an ERROR phase keeps its message. `dsd_engine_signal_search_target()` validates an action
    against the live coordinator without acting and gives the frequency it acts on;
    `dsd_engine_signal_search_notice()` shows a refusal decided outside the coordinator in
    the running session's status. The coordinator keeps the visit's checked site record
    (`dsd_engine_signal_search_site()`, installed as the runtime site hook): it accepts notes
    only while a visit is receiving and no input interruption stands, clears the record in
    `search_visit_candidate()` at each landing (nothing from the span or an earlier visit
    survives) and when the hit's identified protocol changes to another, and copies its formatted
    text into `hit.site` while the record's protocol is the hit's ("NXDN" for NXDN48/NXDN96; a
    site noted before identification waits for it), publishing the hit when the text changes,
    so a final broadcast before silence reaches the live status and the ended hit. Decoder state
    (`p2_wacn`, `p2_sysid`, `p2_cc`, RFSS/site, `dmr_t3_syscode`) is never cleared or written
    for it (`ENGINE_SIGNAL_SEARCH`, `APP_SIGNAL_SEARCH_PROVENANCE`). The hit's call tuple
    (`target`, `source`, `target_text`, `source_text`, `slot`, `crypt`) is one canonical call
    record's, copied whole by `dsd_engine_signal_search_call()` (installed as the runtime call
    hook) from the slot a call note names, under the same receiving gate: the record must be
    ACTIVE, CRC-clean (`crc_invalid == 0`) and of a protocol the search names (DMR, P25, NXDN,
    dPMR, M17, YSF, D-STAR). The tuple is the most recent call on the hit: a note for the
    tuple's own call (slot and epoch) updates it, a call that began no earlier (`started_m`)
    replaces it whole on either slot, and an older call still running on the other slot leaves
    it, so two DMR slots never mix. `slot` is the record's slot plus one for DMR and P25 Phase
    2, else 0; `crypt` is "ALG xx · KID xxxx" from the record's received bits only
    (`crypto_received`), "Encrypted" when its classification is encrypted, pending or
    decryptable with none received and the call's own checked signalling says encrypted
    (`dsd_call_snapshot::enc_received`, set from `dsd_call_crypto_update::enc_observed` by
    the DMR LC, P25 checked ALG ID and service options, NXDN VCALL/SACCH-2, dPMR and M17
    writers from the checked content itself -- the DMR LC's own privacy bit before the
    classification hysteresis, a dPMR frame's CRC-verified halves, never scrambling learned
    from an earlier frame or a forced or inferred classification), and empty otherwise. Like the site it
    shows once the hit's protocol matches, clears at each landing and on a change to another
    protocol, and stays on the ended hit; evidence IDs fill `target`/`source` only while no
    tuple belongs to the hit. `site`, `slot` and `crypt` are persisted with the rest of the hit
    (journal version 2). Published status carries `owned` (a coordinator holds the tuner) and
    the landed channel's live RF level. Receive profiles, gain and frequency are restored on
    exit: a search Stop tunes back, running the carrier boundary and acquisition reset as each
    visit's tune does, and its retune carries the session's receive family
    (`rtl_stream_prepare_retune_analog_profile_for_target()`), since a retune landing first
    retires the older live family request the restore also makes. The engine loop runs no
    `noCarrier()`
    while a search is paused or failed, so the first tick that finds it PAUSED or ERROR runs
    them too, once per landing (`search_end_reception()`, re-armed by `search_queue_tune()`):
    a call heard on the channel ends at the Pause, the input stall or any other failure rather
    than at the next tune or the Stop. It also publishes the opts/state snapshots, since frame
    sync, which normally publishes them, does not run while the search is paused or failed. A decoder session that ends
    under the search (the Qt Stop) restores the options in
    `dsd_engine_signal_search_shutdown()` before the engine's autosave, so the saved
    configuration never keeps a visited frequency. Protocol details enter only through
    CRC/FEC confirmation or validated activity hooks, never a generic sync or dispatcher
    verdict. See [Signal search](signal-search.md).
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
  - The decode loop's side of an input switch (issue #634, `live_scanner_main_loop()`): a frame-sync hunt read that
    left a silent input's wait for a queued command (`dsd_state::input_interrupted`) ended no reception, so the pass
    pumps the command and skips `noCarrier()`; a replaced stream (`dsd_state::input_boundary`, set by an input switch,
    a radio enable, a config reopen or the fallback below) ends the reception: the synced-frames loop breaks after its
    pump, and the outer loop runs `noCarrier()` and ends every call as a teardown (`DSD_CALL_END_EXPLICIT`), so the new
    source neither inherits digital confirmation nor merges into the old call; the hunt's acquisition proof and slicer
    windows (`dsd_frame_sync_reset_acquisition()`) and the matched filter's history (`init_rrc_filter_memory()`,
    `dsd_symbol_matched_filter_reset()`) go with the old stream. A file or TCP input that ended
    (`dsd_state::input_fallback_pending`, which the symbol reader sets) is replaced by the configured Pulse input here,
    between frames and under the P25 SM tick guard (`dsd_engine_input_fallback()`), unless a command pumped since it
    ended replaced it; the session ends when Pulse does not open. A rigctl peer the switch brings to demodulate the
    input (a symbol replay ended; a WAV or TCP stream was the peer's audio already) is asked for the session's width
    under the same guard (`dsd_engine_rigctl_ask_session_passband()`, issue #621), as app-control's follow asks a peer a
    command's switch brings. Test: `ENGINE_INPUT_BOUNDARY`.
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
target's `dmr_sm_ctx_t`. In single-system sessions the engine ticks admitted DMR recovery between synced frames,
including withheld frames, after startup acquisition has ended too. Data-only or unvalidated syncs need not enter a
voice handler's internal tick and must not starve loss, probe or pending-tune deadlines. Trunk scan ticks its own
parked context. Probes honor avoids and revisit the saved CC anchor between alternate frequencies.
DMR's windows run from the later of their stamp and `dsd_state::dmr_confidence_lock_m`, so validated content (never raw
sync) holds a channel; an `ON_CC` with no stamp (a call abandoned from an unconfirmed machine) gets one on its first
tick, and a hunt step reopens the window it just lost when validated content arrived meanwhile (`cc_window_lost`).
An operator move (`dmr_sm_cold_start_note_retune()`, outside a startup hunt) gives a followed call up, drops the
receiver attribution (`cc_rx_freq_hz` and the per-generation rigctl reading `rx_seen_hz`) and listens for a full
window that starts once a tune it superseded has settled (`cc_tune_superseded`, a settle tune replacing a failure,
as a startup hunt settles): at the move when nothing gated frames, else when the settle released them, whichever tune
it waited for. A replaced rigctl connection drops the attribution too (`dmr_sm_note_receiver_unknown()`); a rigctl
read that failed is not repeated for `DMR_RX_QUERY_RETRY_S` of real time. Under `--trunk-scan`, Return to CC restarts
the parked target's own acquisition on its control channel instead, unless a park the coordinator tracks is still
pending. Each target's `dmr_sm_ctx_t` also holds its per-slot call identities (talkgroup priority and preemption,
below), which the switch's acquisition restarts. A Capacity Plus status follow is the machine's call
(`dmr_sm_note_status_follow()`, with the selected LSN's slot); a
repeated status naming the followed carrier neither retunes nor moves its deadlines (it only seeds a slot whose group
changed), a call followed after an operator move ends that move's listening window, and its release goes to the rest
channel announced last (`dsd_state::dmr_rest_channel`, by a status or a checked voice LC), as the no-carrier return
does. A hangtime edit reaches the single-system machine (`dmr_sm_note_hangtime()`), as the no-carrier return read the option. A followed call's timers run only from its slot's
accepted voice since the follow tune (`dsd_state::dmr_confidence_voice_m`, within 1 s), never from a voice sync alone;
with data calls followed (`trunk_tune_data_calls`), accepted data (`handle_data_sync()`) is slot activity as well, so a
zero hangtime holds while it flows.
A Hytera XPT site (`site_without_cc`: `dmr_sm_note_xpt_site()` from a CRC-valid FID 0x68 status or adjacent-site CSBK
or a completed XPT SLCO, kept across `dmr_sm_begin_cc_acquisition()`, cleared by `dmr_sm_note_system_replaced()` at
an RR apply or a channel-map import) never walks the map: recovery only retunes where it parks, which is where the site
identified itself (`cc_freq_hz` and the shared `trunk_cc_freq` a target revisit returns to). A Capacity Plus return to an announced rest
compares the receiver (`dmr_sm_receiver_on()`), not the shared alias.
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
  snapshots; a rigctl peer's live request reads its row here), `dsd_scan_mode_rigctl_request_is_session()` says whether
  a passband this client sets on a rigctl peer now is the session's rather than a scan row's (issue #621: no trunk scan
  and no scan scope held, a typed `-Y` row's suspended or not; without a state, no `-Y` list configured; the legacy
  untyped list holds no scope, so its step asks for the session's; the scope's answer only: every rigctl leg asks
  `dsd_channel_modes_rigctl_request_is_session()` in core, which refines it), and `dsd_scan_mode_row()` gives the
  row's class while suspended too, which `dsd_scan_mode_active()` reports as INHERIT then (a config apply asks it for
  the analog monitor the resume puts back, issue #578), and `dsd_scan_mode_suspended_effective()` the settings in force
  when the scope was suspended (the row over the baseline, which a rollback restarts the input on, issue #578). Row
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
  the two-slot TDMA verdict (`tdma_carrier`) and slicer windows at committed row boundaries, and at an input boundary
  (issue #634). Conventional rows also discard learned P25 modulation; trunk targets retain their own learned
  modulation. Normal no-carrier protocol confirmation resets remain in use. Unlocked RTL P25 trunk-scan targets try
  CQPSK after their first unproductive 4800-symbol/s dwell, before visiting 6000, so the default three-second target
  visit includes the trial. This changes acquisition scheduling, not symbol timing.
- Frontend snapshots deep-copy only scalar scope metadata. `dsd_app_snapshot_configured_mode()` exposes the baseline;
  `dsd_scan_mode_active()` and `dsd_scan_mode_effective_profile()` distinguish combined P25 from global AUTO.


## Platform

- Path: `src/platform`, `include/dsd-neo/platform`
- Target: `dsd-neo_platform`
- Responsibilities: cross-platform primitives (audio backend, sockets, threading, timing, filesystem/curses
  compatibility, run-time library loading)
  - Directory listing: `dsd_dir_list()` in `include/dsd-neo/platform/file_compat.h`, implemented once per
    platform in `file_compat_posix.c` and `file_compat_win32.c`
  - Large files: `dsd_stat_t` is `struct _stat64` on Windows (`_stat64`/`_fstat64`), so `st_size` holds sizes past
    2 GiB everywhere. `dsd_fseek64()`/`dsd_ftell64()` take and give 64-bit offsets (`_fseeki64`/`_ftelli64` on
    Windows, `fseeko`/`ftello` elsewhere); use them for any offset in a capture file. Test: `PLATFORM_FILE_LARGE`
    (sparse files past 2 GiB and 4 GiB, skipped where the filesystem cannot make one)
  - Private file opens: `dsd_fopen_private()` creates a written file owner-only; `dsd_fopen_private_ex()` also says
    whether it opened the path, which for a write mode creates or empties it before the stream over the descriptor is
    made, so an open whose `fdopen()` fails still reports the file it emptied (issue #578: the I/Q capture writer).
    Open raw bytes with a `b` mode: on Windows a text-mode stream writes every LF as CRLF. An `x` after a write mode
    (`"wbx"`, C11's) creates exclusively: an existing file fails with EEXIST and keeps its bytes (issue #661: MBE
    capture names). Windows uses `CreateFile(CREATE_NEW, FILE_FLAG_OPEN_REPARSE_POINT)` for these opens because the
    CRT's `_O_EXCL` follows dangling symlinks. The final name is claimed without following a link, and
    `out_opened` records creation even if attaching the CRT descriptor or stream fails. Tests:
    `PLATFORM_FILE_COMPAT`, `DMR_SYNTH_OUTPUT_PATHS` (existing and dangling symlinks for both generator outputs)
  - Binary descriptors: `dsd_set_binary_mode()` disables Windows text translation on an existing descriptor (a
    validated no-op on POSIX). Engine setup uses it for `-o -` before emitting audio, so an inherited text-mode
    stdout cannot expand sample bytes containing LF into CRLF. Tests: `ENGINE_RUN_SETUP`, `PLATFORM_FILE_COMPAT`
  - No-replace rename: `dsd_rename_noreplace()` never writes over an existing name (EEXIST, both files kept):
    `renameat2(RENAME_NOREPLACE)` on Linux, `renamex_np(RENAME_EXCL)` on macOS, else a hard link and an unlink (whose
    fallback, `dsd_rename_noreplace_by_link()` in `src/platform/file_compat_internal.h`, tests reach directly), and on
    a filesystem with neither an exclusive create that claims the name, then a rename onto that empty claim
    (`dsd_rename_noreplace_by_reservation()`, also tested directly), which only a process deleting the claim can
    race (never a check then a rename); Windows keeps the CRT's `rename()`, which refuses an existing name, and reports
    that refusal (`_doserrno` exists) as EEXIST (issue #661: per-call WAV publication)
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
  - Output device recovery (issue #659): PulseAudio and PortAudio close a device whose write fails and reopen it
    behind the same `dsd_audio_stream*`, so no caller's handle goes stale and a sound-server restart costs a gap. The
    shared state machine is `src/platform/audio_output_recovery.h` (no clock or lock of its own): the first failure
    after healthy playback retries on the next attempt, each further failure of a reopen or of the first writes after
    one doubles the wait from 250 ms to 4 s, the wait resets only after a reopened device has taken writes for 2 s,
    and an episode counts as recovered only at a write the device took, never one only queued. A failed async device
    drops its queued audio, `dsd_audio_write()` fails without queueing while it is down, and `dsd_audio_drain()`
    fails at once. PulseAudio async streams: the pump owns `handle` after open, frees a failed one and reopens from
    its own thread (waiting interruptibly by stop, answering a drain at once), so the decoder never blocks on a
    reconnect. PortAudio async streams: the pump marks the device down and touches no handle after that; the writer's
    thread, which opened it, closes and reopens it at the next write that finds a reopen due, since host APIs want a
    stream closed where it was made (WASAPI's COM objects). Blocking streams of both reopen on the writer's thread at
    the next write that finds a reopen due; every failure and retry is timed after the blocking call. `dsd_audio_output_health_get()` reports failures, recoveries,
    whether the device is down and the latest reason; AAudio (which recycles a disconnected device itself) and the null
    backend report zeros. The reopen finds the device by the name the stream was opened with; PortAudio does not
    refresh its device list, so it finds the same device again. PortAudio keeps its streams on an unlocked global
    list, so every open, start, stop, abort, close, initialize and terminate takes one process-wide lifecycle lock
    (the pumps stop and restart a stream to drain it). `PLATFORM_AUDIO_PULSE_HELPERS` drives the Pulse recovery over
    a fake `pa_simple` and a fake clock; `PLATFORM_AUDIO_PORTAUDIO_RECOVERY` runs the PortAudio pump thread and locks
    over a fake PortAudio (built wherever `portaudio.h` is found) and checks that every open and close runs on the
    opening thread and that no two overlap
  - Whole-frame descriptor writes (issue #659): `dsd_write_frames()` in `file_compat.h` finishes interrupted and short
    writes, returns `DSD_WRITE_FRAMES_WOULD_BLOCK` at once when a non-blocking descriptor is full at a frame boundary,
    waits (`poll()`, POSIX only, at most 250 ms in all, an interrupted wait going on only for what is left of it) only to
    finish a frame a short count left half written, and treats a write of 0 as EIO. A closed reader is EPIPE, never a
    SIGPIPE: POSIX blocks SIGPIPE in the calling thread around the write and consumes the one the write raised on that
    thread (`sigtimedwait()` with no wait, or `sigwait()` once `sigpending()` shows it where there is no
    `sigtimedwait()`), leaving one that was already pending; Darwin, which raises a write's SIGPIPE on the process,
    sets `F_SETNOSIGPIPE` on the descriptor instead.
    The loop is `src/platform/write_frames.c` behind an ops table (`write_frames_internal.h`), which
    `PLATFORM_WRITE_FRAMES` drives with scripted writers as well as real pipes
  - Socket readiness: `dsd_socket_wait()` in `include/dsd-neo/platform/sockets.h` waits for a socket to become
    readable and/or writable (`poll()` on POSIX, `select()` on Windows), reporting end of stream and errors as ready
    so the next `recv()`/`send()` returns them. Servers poll non-blocking sockets with it rather than receive
    timeouts, which leave a Windows TCP socket in an indeterminate state when they fire
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
  On MSVC, `cmake/MsvcFastMath.cmake` applies `/fp:fast` after all targets are declared, only to sources without an
  explicit source or target `/fp` mode. This preserves the IEEE exceptions without conflicting options and D9025
  warnings; configuration-specific target modes, including PFFFT's Debug `/fp:strict`, take precedence too.
- A hot file keeps fast-math and moves its check into a small IEEE translation unit called off the per-sample path
  (`frames/dsd_dibit_reliability.c`, `io/radio/rtl_finite.cpp`); a by-value argument goes through `dsd_fp_opaque_*()`
  first.
- Per-sample loops validate on bits loaded from memory, which no floating-point assumption reaches: external float
  samples are sanitized where they enter -- SoapySDR CF32 reads and cf32 replays through `bound_cf32_to_f32()` and
  `bound_rotate90_cf32_to_f32_phase()` (`include/dsd-neo/dsp/simd_widen.h`, SIMD with runtime AVX2 dispatch), which
  turn any component that is NaN, infinite or at least 2^60 into 0, and soft symbol files in `src/dsp/dsd_symbol.c` --
  and the Costas and Gardner loops check each sample where they load it (`src/dsp/costas.cpp`).
- Arithmetic whose exact rounding is a contract keeps it explicitly: the decode clock's `ns / 1e9` (`decode_clock.c` is
  IEEE) and a trunk tune's completion stamp, compared with it (`trunk_tuning_hooks.c` is IEEE), the fallback decimator's
  block-split-invariant running sum (`#pragma clang fp reassociate(off)`), and the auto and noise squelches' window
  sums, which must not depend on where a block starts (`squelch_floor.c`, `nfm_noise_squelch.c`, `noise_squelch_bank.c`
  and `pcm_noise_squelch.c` are IEEE).
- Tests that feed NaN or infinity, or check results with `std::isnan()`, compile their own sources with IEEE
  semantics (the block near the top of `tests/CMakeLists.txt`), leaving the code under test on the build's flags.

The `Linux • RelWithDebInfo • ctest (x86-64-v3, fast-math, clang)` check enforces the first point for builtin/constant
forms: with `-Werror`, Clang's `-Wnan-infinity-disabled` makes `isnan()`/`isinf()` builtins and NaN/infinity constants
in fast-math code a build error. Clang can silently fold hand-written bit tests on floats it computed or received
by value; only runtime tests that feed the guard a NaN or infinity catch those dead guards, another reason the
suite runs under this compiler.

## Core

- Path: `src/core`, `include/dsd-neo/core`
- Target: `dsd-neo_core`
- Responsibilities: cross-protocol glue (audio output helpers, vocoder glue, frame helpers, GPS, file import),
  misc/util
- Invariant (MBE capture, `-d`, issue #661; `<dsd-neo/core/file_io.h>`): decoders write capture frames only through
  `dsd_mbe_out_write()`, which creates a slot's capture file (exclusively, `"wbx"`, retrying a taken name with another
  nonce) with the first frame written to it, in the folder whether or not it ends in a separator; there are no
  openers. A capture is owned by `dsd_mbe_out_owner_of()` of the frame's protocol, `dsd_mbe_out_frame_protocol()`: the
  synctype `processFrame()` dispatched (`dsd_state::frame_dispatch_*`, set around `handle_frame`), else
  `state->synctype` (SDRTrunk JSON playback, tests), so YSF and scrambled dPMR frames decoded under a borrowed NXDN or
  P25 Phase 1 synctype stay YSF's and dPMR's. P25 Phase 1 and 2 share an owner (their formats keep them apart), as
  EDACS and ProVoice do. A frame of another owner or `dsd_mbe_out_format` closes the slot's capture, and the other
  slot's when that one differs too, so a TDMA pair of one owner and format stays open side by side. The writer refuses
  a frame read across a carrier boundary during its dispatch (`frame_dispatch_carrier_seq`) and a YSF frame before
  `ysf_fich_confirmed`, which `processYSF()` sets as soon as a FICH verifies. A handle a caller attached (owner 0) is
  written as is. A failed creation is retried once a second (realtime clock) and logged once per run of failures.
  Closes: protocol closers call `dsd_mbe_out_close_protocol()`, which closes only their own protocol's captures (or an
  attached handle): DMR data and finalize (MS data only when trunking does not withhold it), dPMR FS1/3/4, NXDN
  non-voice, X2-TDMA data, P25 Phase 1 header, terminators and control (both slots), the P25 state machine and Phase 2
  XCCH per slot (naming P25 themselves, since the watchdog runs outside any dispatch), EDACS after a control frame
  checked out on an unchanged carrier, a verified YSF header or terminator, a verified D-STAR header; the engine closes
  both at no-carrier, shutdown and in `dsd_engine_forget_carrier_decoding()` (carrier boundary, trunk-scan switch).
  `dsd_mbe_out_enabled()` (folder set or handle attached) gates every capture path before any record gate runs.
- Invariant (voice crypto ownership, issue #656; `<dsd-neo/core/vocoder.h>`): every caller of `processMbeFrame()` /
  `processMbeFrameSoft()` passes the `dsd_voice_protocol` that owns the frame. The codec path and the output stage still
  follow `state->synctype`, which YSF (NXDN and P25 Phase 1 decoders) and scrambled dPMR (NXDN decoder) borrow; the
  owner alone decides which crypto transforms, key selection and encryption verdict apply. DMR: TG key autoload, BP,
  Hytera BP, Vertex, vendor scramblers, `--dmr-force-algid` and every slot-path ALG ID. P25 Phase 2: only P25
  DES/AES/RC4 (0x81, 0x84, 0x89, 0xAA)
  on the shared slot path. P25 Phase 1: multicrypt and RC4, and only it moves `p25vc`/`octet_counter`. NXDN and dPMR:
  the NXDN scrambler (signalled or forced `M == 1`) and cipher 2/3. YSF, D-STAR, X2-TDMA, ProVoice: none. The
  keyloader's ALG ID / KEY ID activation (`mbe_activate_keyloader_slot()`) runs for DMR and P25 only: NXDN voice loads
  its keys from its VCALL, and the others signal none. The manual `-S` keystream stays on the slot path for every owner.
  DMR RC4 (ALG ID 0x21) decrypts a frame whose C0 the vocoder corrected (fewer than 4 C0 errors as mbelib-neo 2.5
  counts them, TIA-102.BABA-1 5.5) and leaves an uncorrectable one as received; its drop position advances either way.
  A keystream or scrambler that changes AMBE 2450 parameter bits leaves the decode's C0 context in place
  (`src/core/mbe_result_context.h`): mbelib-neo repeats such a frame only at 4 C0 errors, as it does clear voice,
  rather than by its no-context fallback (more than 3 errors in all). IMBE transforms still drop the C0/C4 context.
  `.amb` capture records pack the C0 and total error counts behind `DSD_MBE_AMB_ERR_FLAG` (`saveAmbe2450Data()`,
  `<dsd-neo/core/file_io.h>`); `-r` replay restores the C0 count as C0 context and the total for mbelib's error rate
  (`readAmbe2450Data()`, `dsd_state::mbe_file_c0_valid`, `decode_ambe_file_frame()`). A byte without the flag (older
  captures, other tools) is a total and replays with no C0 context, as before. `.imb` and `.dmb` records keep the total;
  a `.dmb` record replays as soft input, as live D-STAR voice decodes, so mbelib does not mute AMBE 2400 on its error
  rate where the live decode played.
  Keystream reads are bounded by what was generated: P25 Phase 1 by ALG ID (DES/TDEA 224 octets, DES-XL 213, AES 15
  blocks = 240; a superframe reads up to octet 228), the slot path by `dmr_voice_stream_apply_frame49()`'s
  `ks_bit_count` and the DES helper (DES and AES 1152 bits, Hytera Enhanced 1080, Kirisun 1008). A frame past the end is
  muted like an undecodable one (silence, no MBE capture, `KEYSTREAM_EXHAUSTED` frame-log event; a stereo slot stages
  silence) and the counter stays put; nothing wraps. An armed `-S` keystream still takes over such a slot frame. P25
  Phase 2 applies AES to every ciphertext word, including patterns DMR reserves for clear silence. NXDN cipher 2/3
  stops after its 32 EHR words (1568 bits) until a fresh IV starts the next encryption session, per NXDN TS 1-D section
  5.4.4.2; it never repeats the final word or uses the unused cipher-block padding. A P25 Phase 1 frame the decoder
  drops before the vocoder (non-standard C0 or the crypto gate) still consumes its keystream position through
  `dsd_mbe_p25p1_skip_voice_frame()`. Raw `.dmb` (D-STAR AMBE 2400) playback never applies `-b`.
  Tests: `CORE_MBE_CRYPTO_SCOPE`, `P25_P1_LDU_HELPERS`.
- Invariant (keyring presence and width, `<dsd-neo/core/keyring.h>`, `src/core/vocoder/keyring.c`): a keyring cell
  is present by its import flag (`rkey_array_loaded`), whatever its value, or by a non-zero value for material stored
  without the flag (`keyring_kid_scalar_present()`), so an imported all-zero DES, RC4 or AES key decrypts as the same
  value entered directly does, and a key ID nothing was imported for stays refused. An AES key's width is its leading
  present segment cells (ID, +0x101, +0x201, +0x301; `keyring_kid_aes_segments()`): a cell after a missing one is
  another ID's key aliased onto it and is never counted. An AES ALG can use a key only at its full width (AES-128 2,
  TDEA 3, AES-256 4; `dsd_key_need_aes_width()`), never "any segment". `keyring_activate_slot_with_kid()` sets
  `scalar_key_present[]` from the scalar cell, `aes_key_segments[]` to the width and `aes_key_loaded[]` when the
  first segment is present; it does not know the ALG, so every consumer that decides decryptability checks the width:
  `p25_crypto_has_complete_key()` (`aes_key_segments[]` and the keyring's first N cells), the DMR voice verdict
  (`dsd_dmr_voice_kid_can_decrypt()` with `dsd_dmr_key_material::aes_segments`), and NXDN through
  `dsd_key_aes_slot_complete()` (`<dsd-neo/core/key_presence.h>`; a legacy writer that set `aes_key_loaded[]` without a
  width keeps the old rule). Classification agrees with activation: `keyring_kid_material()`, `keyring_kid_satisfies_need()`
  and the mapped DMR material use the same cells and width; the Kirisun quartet also requires every word non-zero
  (`keyring_kid_kirisun_complete()`).
  NXDN keys its material by the VCALL key ID, the destination or the frequency hash outside that activation. The VCALL
  and VCALL_IV loaders and the key source the call reports share one choice, `nxdn_select_key()`: the signalled ID
  when it holds the cipher's key (a scalar for the scrambler and DES, all four AES-256 segments for AES), else the
  destination's scalar for the NXDN48 scrambler, else key 0 for AES, which loads (`nxdn_load_aes_key_index()`) only at
  full width. The DMR talkgroup map reports a matched but unapplied row by the same cells: the mapped ID holds a key of
  another kind (incompatible) only when its own cell is present, every key kind occupying it, and is missing otherwise.
  Every other loader that reads `rkey_array` by key ID decides presence the same way, requires the ALG's width for
  AES, and falls back to the direct key only when nothing usable was imported, judging that one by
  `dsd_key_scalar_present()` (R/RR) or `dsd_key_direct_words_complete()` (K1..K4, its recorded word count): the DMR
  Basic Privacy talkgroup autoload in `dsd_mbe.c` (which also sets `basic_key_present`), the DMR data block crypto
  (`dmr_block_crypto.c`), the P25 Phase 1 PDU decrypt (`p25p1_pdu_data.c`) and the SDRTrunk JSON replay keystreams and
  talkgroup-keyed lookup (`dsd_file.c`). Tests: `P25_CRYPTO_STATE`, `NXDN_ELEMENT_BOUNDS` (imported zero keys, the
  narrow AES fallback, VCALL_IV keeping the VCALL's key), `CORE_MBE_TRANSFORM_CONTEXT`, `DMR_BLOCK_CRYPTO`,
  `P25_P1_PDU_ES_EXT`, `CORE_MBE_FILE_IO` (incomplete AES keys refused on the data and replay paths),
  `CORE_DMR_VOICE_ALG_GATE` (DMR voice width), `CORE_DMR_TG_KEY_MAP`.
- Invariant (audio sinks, `<dsd-neo/core/audio_sink.h>`, `src/core/audio/audio_sink.c`, issue #659): every writer of
  decoded or analog audio applies its own gates and picks the output type, then writes the local device streams only
  through `dsd_audio_sink_write_device()` and the `-o -` descriptor only through `dsd_audio_sink_write_fd()`: the
  mixers and P25 Phase 2 playout (`dsd_output_*_block()`), the beeper, `playSynthesizedVoice()`, M17 (decoded voice and
  the encoder's `-8` baseband), EDACS analog voice and the analog monitor sink. The device path reads the stream's
  `dsd_audio_output_health_get()` after each write and logs each failure episode once, at most one failure line a
  minute per stream (`DIGITAL`, `RAW`) with a count of those held back, and one line when the reopened device plays
  again; `closeAudioOutput()` resets it. It never opens a stream the session did not (a NULL stream writes nothing).
  The descriptor path writes whole frames (`dsd_write_frames()`): a failed write logs one error and turns that
  descriptor's audio off for the rest of the session (decoding, recording and the other outputs carry on; the engine
  resets it when it binds `-o -`); a full non-blocking descriptor drops the rest of the block whole, with one warning
  and a count at most once a minute after it; a blocking one (the usual pipe) blocks as any pipe does.
  `dsd_audio_sink_monitor_takes_fd()`: the analog monitor writes `-o -` only in the analog-only modes, where no
  digital writer shares the stream (48 kHz mono s16le); the `-8` source monitor during digital decoding does not, and
  the engine warns once at startup when `-o -` and `-8` meet outside the M17 encoders. The CLI ignores SIGPIPE in the
  branch that installs its SIGINT/SIGTERM handlers (never under `DSD_NEO_NO_SIGNAL_HANDLERS`; nothing in the process
  starts a child that would inherit it), rigctl sends with `MSG_NOSIGNAL` or protects each connected socket with
  `SO_NOSIGPIPE` where supported (refusing a connection if setting it fails), and the UDP blasters report send failures
  once per run, at most a line a minute per socket. A newly opened UDP socket starts fresh reporting; failed setup
  closes and invalidates the socket. Decoder thread only. Tests: `CORE_AUDIO_SINK`,
  `PLATFORM_WRITE_FRAMES`, `PLATFORM_AUDIO_PULSE_HELPERS`, `PLATFORM_AUDIO_PORTAUDIO_RECOVERY`,
  `IO_UDP_SOCKET_BACKENDS`, `DSP_SYMBOL_REPLAY`, and the `DECODE_IQ_STDOUT_*` end-to-end cases.
- `src/core/file/dsd_file.c` reads SDRTrunk JSON MBE exports. Its file-local crypto context owns NXDN SACCH
  position/session tracking and DMR AES late-entry context; playback does not call protocol-layer state-mutating
  IV expansion. Voice bits are FEC-decoded before decryption and then passed to the normal vocoder/output path.
- API note: the high-pass filter in `<dsd-neo/core/audio_filters.h>` is `dsd_hpf()`. It was renamed from
  `hpf()` because codec2 exports a symbol of that name and Android links codec2 statically, which turns the
  duplicate into a link error. It is the only filter in that header that is prefixed: codec2 exports none of
  the others (`lpf`, `lpf_f`, `hpf_f`, `hpf_dL`, `hpf_dR`, `pbf`), so renaming them would break out-of-tree
  callers for no benefit. Out-of-tree callers of `hpf()` need updating
- API note (issue #653): `hpf_dL()`/`hpf_dR()` run the digital voice high-pass of the voice chain on slot 1's and
  slot 2's state (`dsd_state::voice_hpf`, which replaced `HRCFilterL`/`HRCFilterR`) and saturate to int16: a
  2nd-order Butterworth at 300 Hz, no longer the one-pole 960 Hz section that never reached unity. `agf()`, the float
  output's own auto gain, is gone: both formats take their gain from `dsd_voice_chain_run()`.
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
- API note (transmission): `dsd_call_snapshot::transmission_epoch` names the logical transmission an epoch belongs to:
  its own epoch, or for an epoch that reacquired a recoverably-ended one (`call_state_seed_reacquired_snapshot()`),
  that epoch's. `dsd_call_snapshot_transmission()` reads it (falling back to `epoch`). State bound to one call keys on
  it, so a fade the canonical layer healed keeps the binding and a new transmission never inherits it: the DMR trunk
  machine's slot identities and NXDN's held call (`dsd_state::nxdn_call`) below. `dsd_call_snapshot_may_reacquire()`
  says whether an ended epoch may still be healed (a recoverable end of a call that names one, within its reason's own
  window: `DSD_CALL_REACQUIRE_GAP_S` after a sync loss, `DSD_CALL_TERMINATOR_HEAL_GAP_S` after an unverified
  terminator), the rule the store's own reacquisition applies short of what the reopening observation must satisfy; a
  binding renamed meanwhile keeps its transmission only while it says yes.
- API note (received crypto): `dsd_call_snapshot::crypto_received` (`DSD_CALL_CRYPTO_RX_ALG`, `_KID`) with
  `received_algid`/`received_kid` say which of a call's ALG ID and key id were decoded from over-the-air signalling
  that passed its check, beside the live `algid`/`kid` every crypto update rewrites. Only an update whose `received`
  bits name a field sets it (`dsd_call_state_update_crypto()`, `_update_retained_crypto()`), and it stays for the
  epoch: a forced (`--dmr-force-algid`), key-inferred, late-entry, carried or synthesized value, or live values a sync
  loss cleared, change `algid`/`kid` and never the received ones. A new epoch starts with none, an epoch that
  specializes the identity-less voice epoch it opened in keeps them, and a reacquired epoch carries the ending one's
  (`call_state_seed_reacquired_snapshot()`); a trunk-scan context restore copies them with the snapshot. The writers
  that set them: P25 `p25_crypto_resolve()` (HDU, LDU2, ESS, MAC_PTT; not a tuple a clear-conflict quarantine
  deferred, nor an HDU, LDU2 or ESS whose RS decode leaned on soft erasures: `processHDU()`/`processLDU2()` (by
  `p25_p1_soft_rs_ok` moving), the ESS paths and a staged rekey pass that verdict through
  `p25_crypto_resolve_checked()`, `dsd_p25_p2_rekey_state::parity_checked`; the service options of an LDU1 link
  control only a soft-erasure retry recovered, `dsd_state::p25_p1_lcw_soft`, are no encryption indication either), DMR `dmr_pi()` (Kirisun's
  key id is a hash, so ALG only; no RAS or failed-CRC burst, no undefined DMRA
  algorithm) directly or through `dsd_state::dmr_pi_precall_*` into the call the next checked LC opens within
  `DMR_PI_PRECALL_HOLD_S` (only a PI decoded while no call is active is held; one decoded while a call is active is
  that call's), NXDN
  `nxdn_vcall_publish()` once `nxdn_cipher_received_enc()` (the applied encrypted cipher is also what the
  over-the-air-only hysteresis `dsd_state::nxdn_cipher_rx_*` established; a forced scrambler never is) and only when
  that VCALL itself carries the published cipher (a VCALL contradicting a received field withdraws it through
  `dsd_call_crypto_update::contradicted`), and M17 `m17_publish_lsf()` (encryption type only).
  Nothing else does: dPMR's scrambling flag, TETRA, X2-TDMA, EDACS, SCCH and file playback set none. History, events,
  the terminal and Monitor read `algid`/`kid`; only Signal search reads the received ones (`CORE_CALL_STATE`,
  `DMR_SEARCH_CALL_NOTES`, `P25_CRYPTO_STATE`, `P25_P2_RELIABILITY`, `NXDN_ENC_CLASS`, `M17_STATE_DISPATCH`).
- API note (a call's own crypto; decryption state is separate): a call record's `algid`/`kid`/`mi` hold only the
  epoch's own crypto -- what its own signalling or the decoder's forced or key-inferred choice for it published, or
  what a reacquired epoch was seeded with -- never live decoder values a call whose terminator was missed left
  behind. Decryption state is not touched for it: the DMR `payload_*` per slot, the P25 live tuple, MI and LFSR state
  behave exactly as before at every fork (`FORK_DECRYPT_INPUTS`, which records the same values against the
  pre-feature decoder). When a new call's link control forks such a call, the protocol marks the slot's live ALG ID
  and key id as inherited (DMR `dmr_flco_publish_voice()`: `dsd_state::dmr_crypto_inherited*`; conventional P25
  `p25_sm_publish_conventional_voice()`: `p25_crypto_note_inherited()`), and publishes them as
  `dsd_call_crypto_update::carried` while they are unchanged: the record keeps its own values (none on the new
  epoch) and takes only the classification (P25: from the new call's own service options,
  `p25_crypto_carried_classification()`; DMR: its LC's privacy bit). Any PI, P25 resolution or new classification
  clears the mark, and so does a DMR late-entry SB identifier decoded for the new call (even one repeating the marked
  values) and any change of the live values. Live values that are the decoder's own choice for the new call are
  published as its own, never carried: the `--dmr-force-algid` value for an encrypted LC, or, with no forced ALG ID,
  the ALG ID inferred from the slot's loaded key for an encrypted LC (`dmr_le_key_inferred_algid()`), each with the
  0xFF key id placeholder (`dmr_flco_crypto_is_decoders_choice()`). Where the canonical layer keeps the epoch at the
  fork (a BEGIN that names an identity-less active epoch), DMR first calls `dsd_call_state_forget_crypto_values()`,
  so the record drops the ALG ID, key id, MI and received bits the replaced transmission published into it.
  A reacquired epoch's seeded crypto is kept against updates that receive nothing and name no ALG ID or key id
  (`dsd_call_snapshot::crypto_restored`; the live values a sync loss cleared). Because the record no longer holds
  carried values, the DMR heal restores the slot's live ALG ID and key id from `dsd_state::dmr_live_pub_*` (what DMR
  last published into the ending epoch), which is what it always restored; it then binds those values to the healed
  epoch, so a second heal with no LC or PI in between restores them too. The heal also restores the inherited mark
  the terminator reset cleared (stashed with the live values), so the healed call's next LC still publishes them as
  carried. Limits: after a missed terminator, a new
  call whose own PI, HDU, LDU2 or DMR late-entry SB identifier decoded before its link control (header lost, or its
  first embedded LC failing CRC) shows no ALG/KID until a later one of its own, and that signalling is attributed to
  the previous, still-active call (a conventional P25 Phase 1 HDU always
  precedes the LDU1 that forks the call) (`DMR_SEARCH_CALL_NOTES`, `P25_CRYPTO_STATE`).
- API note (CRC provenance, discussion #602): `<dsd-neo/core/crc_provenance.h>` defines the bits that
  `dsd_state::event_crc_invalid` (the decoder-thread scope a dispatcher ORs its bits into and restores),
  `dsd_state::data_header_crc_invalid`, `dsd_call_snapshot::crc_invalid` and `Event_History::crc_invalid` carry:
  `DSD_CRC_FAILED` (1, a check failed; the value P25 and DMR scopes have always ORed in) and `DSD_CRC_RAS_UNVERIFIED`
  (2, DMR content `-F` accepted only as a RAS signature, `dmr_ras_signature()`). The bits accumulate per transmission and are
  never cleared by another writer. Every gate tests `!= 0` (not verified: `-L` rows, strict publication), so RAS stays
  out of location files; only renderers tell the bits apart, through `dsd_crc_marker()` (`[CRC ERR] ` outranks
  `[RAS] `) and `dsd_crc_is_failed()` (the only bit that raises severity to WARNING). `watchdog_event_mark_crc()` swaps
  a row's marker given the bits it was rendered with, so a failure reaching a committed `[RAS]` row through enrichment
  replaces the marker in place and a scan channel labelled `[RAS]` is never stripped; `-J` keeps the marker each line
  was written with. DMR hands the verdict on explicitly: the burst handler classifies each burst once (`crc_flags`,
  RAS or FAILED; the MBC continuation 0x05 has no check of its own), passes it to `dmr_dheader_classified()`
  (`src/protocol/dmr/dmr_crc_provenance.h`; `dmr_dheader()` maps a 0/1 verdict), stores RAS-accepted confirmed blocks
  as `DMR_DATA_BLOCK_CRC_RAS` in `data_block_crc_valid`, and folds them back into bits for the packet
  (`dmr_block_type1_block_crc_flags()`, `dmr_block_type2_crc_flags()` for MBC/UDT). A trunk grant's provenance rides in
  `dmr_sm_ctx_t::vc_crc_flags` to the call `handle_voice_sync()` publishes later, outside the burst's scope.
- API note (DMR UDP services, issue #746): the IPv4 (SAP 4, `decode_ip_pdu()`, shared with P25) and compressed
  UDP/IPv4 (SAP 3, `dmr_udp_comp_pdu()`) paths in `src/protocol/dmr/dmr_pdu.c` decode a datagram through one port
  dispatch, `dmr_udp_service_dispatch()`: the destination port when a decoder claims it, else a source port that names
  its service (4005, the radio's fixed ARS port per the MOTOTRBO ARS specification 4.4.2, and the ETSI radio-network
  defaults 5016/5017; a policy, so an ARS server on an already claimed port still selects that port's decoder).
  `--lrrp-extra-port` ports are part of it. The compressed path passes the data header's LLIDs as the radios and a
  NULL IPv4 header (9361/49198 then print the LLIDs), and appends the decoder's slot string to its notice. TS 102 361-3
  leaves SPID/DPID 95-127 to the manufacturer with a per-radio linkage (table 7.17 note 2), so no index is mapped by
  default: `dsd_opts::dmr_udp_comp_ports` (`--dmr-udp-comp-port`, `mode.dmr_udp_comp_ports`,
  `<dsd-neo/core/udp_comp_ports.h>`) supplies a site's linkage, and an unlinked index is decoded only when its payload
  is exactly one ARS message of that specification's section 3.4 (`dmr_ars_record_is_exact()`; a TMS ack shares the
  0xBF header octet and differs by length; length-value sizes are octets). The SAP length handed to every SAP decoder
  is what the blocks carried (`data_byte_ctr`) less the CRC32 and the pad octets (TS 102 361-1 9.3.22); the decrypt
  window and the AES-ECB block count are bounded the same way. A standard header resets confirmed mode, the MNIS
  P_HEAD flag and the assembler's arming, which only DPF 0-3 with blocks to follow and DD/R_HEAD with appended blocks
  set; `data_block_crc_valid` holds the header plus 127 block verdicts.
- API note (call history rows, issue #575): every `Event_History` row records the frequency its call was heard on
  (`freq_hz`, Hz, 0 unknown) and its access code (`access_code_kind`, a `dsd_access_code_kind`, and `access_code`; zero
  means none, so a memset-zero row reads as unknown). `<dsd-neo/core/access_code.h>` (`src/core/util/access_code.c`,
  outside `state.h` so app-control views can name the kinds) pins the kinds (NONE 0, COLOR_CODE 1, NAC 2, RAN 3, CAN 4;
  the Qt store persists them), maps a protocol to its kind (`dsd_access_code_kind_for_protocol()`) and reads the live
  code from state the protocol set only from checked content (`dsd_access_code_current()`; the header names each check):
  DMR `dmr_color_code` 0..15, P25 Phase 1 `nac` only and Phase 2 `p2_cc` only (0x001..0xFFE; no fallback between them,
  since a Phase 1 NID of 0x000/0xFFF leaves `nac` 0 beside a stale `p2_cc`), and `p2_cc` only while `p2_cc_verified`
  says this carrier proved it (see Protocols), NXDN `nxdn_last_ran` < 64 unless `nxdn_last_ran_stand_in` marks it as a
  value the terminal shows where the RAN goes but that is none (an IDAS area bit or DCR's fixed 7; see
  Protocols), dPMR `dpmr_color_code` 0..63, M17 the service options' low nibble once the call has service metadata,
  TETRA the 6-bit colour code a CRC-checked trunked mode BSCH or a CRC-checked downlink block descrambled with it proved
  on this carrier, from the received identity record (`dsd_state::rx_id`), never `tetra_pub`'s network, which a
  `--tetra-net` preset or another carrier may have set (direct mode has none).
  `watchdog_event_current_load_tuning()` in `dsd_events.c` applies two per-row rules on every voice render, against the
  staged row: the frequency is the call's own `frequency_hz` whenever it has one, else the staged value, else -- only on
  an ACTIVE render outside `--playfiles` -- `dsd_opts_tuned_freq_hz()`, so the tuner is pinned at the epoch's first
  active render as the channel label is (a typed `-Y` row's queued tune writes `rtlsdr_center_freq` before
  `channel_scan_commit()` ends the outgoing calls, and `trunk_scan_switch_to()` restores the next target's state before
  it retunes); the access code is refreshed from a valid live reading while the call is ACTIVE (a sentinel never erases
  a known code), frozen once it has ENDED (a held finalize pass may run after a hop), reused only when its kind is the
  protocol's, and never taken under `--playfiles`. Staged values -- these two, the channel label and the row's start
  stamp -- are reused only when the slot's lifecycle has opened the call's epoch, and every canonical render opens it
  first: `dsd_event_sync_slot()`, `watchdog_event_current()` and `dsd_event_note_current_call()` all run the history
  step before they render, so a render that follows a protocol's unsynced observation no longer draws the new call over
  the outgoing epoch's row, which the next sync committed as the outgoing call's (two rows for the new call, none for a
  staged outgoing one). The window is DMR's: `prepare_dmr_bs_voice_slot()` observes the tuned call through the trunk
  SM's voice sync, a burst can end the superframe before its post-skip sync, and the control pump runs between
  `processFrame()` and the next `getFrameSync()`; D-STAR's `processDSTAR()` syncs after every voice frame. Notes for the
  slot's call (the DMR/NXDN encryption lockout, an operator's slot lockout or skip) go through
  `dsd_event_note_current_call()`, never a raw `Items[0].internal_str` write, and it declines when the slot has no call
  to carry the note (none observed, or one that ended and committed its row). Detail a protocol decodes without naming
  its call -- D-STAR slow-data text and APRS, the NXDN alias and DCR call sign memory -- reaches a row only through
  `dsd_event_set_open_call_detail()`, which writes it only while the slot's call is ACTIVE and its epoch is open, and
  `dsd_event_enrich_*()` declines a row-0 write while the call's epoch is not yet open (the live alias display and the
  call's CRC verdict still follow). DMR embedded talker alias and GPS LCs reach enrichment only when their 5-bit
  checksum passed (`dmr_flco.c`, all modes): a failed one still claims its FLCO, so it is never read as a voice LC, but
  applies nothing, and a failed alias fragment abandons the slot's assembly through `dmr_talker_alias_lc_invalidate()`
  (`<dsd-neo/core/embedded_alias.h>`), which clears the header format and zeroes the alias span of `dmr_pdu_sf`.
  Detail decoded before any call for the transmission was observed used to be
  committed at the next epoch open as a detail-only row with no summary, or to land on a previous call that had ended
  but not yet committed; declined detail lands when it repeats. One case remains: while the previous call is still
  ACTIVE, a direct writer's detail for the next transmission still lands on the previous call's row, since none of these
  messages names its call. The protocols still write their display scratch (`dstar_txt`, `dstar_gps`,
  `generic_talker_alias`) as before; with no orphan commit running `watchdog_event_reset_post_push()` at the next call's
  open, D-STAR text and position decoded before that call was observed now stay on screen through it. A notice for a
  non-canonical call (the P25 lockout's synthetic snapshot, `watchdog_event_emit_noncanonical_notice()`) renders into a
  blank row and restores the canonical staged row byte for byte, as a data notice does: no other call's label,
  frequency, code, start, alias, GPS or text, no WAV rotation or end alert, and the canonical call keeps its row. Data
  notices read both live (the notice's own frequency first). A reacquisition merge only fills an unknown frequency or
  code (kind and value together). The system identity string (`sysid_string`, which also names WAV files and the
  rdio-scanner `short_name`) and the event line print `--` for a DMR colour code, NXDN RAN or dPMR colour code still at
  its sentinel (`DMR_CC_--`, `NXDN_RAN_--`, `DPMR_CC_--`, `CC: --;`, `RAN: --;`; `watchdog_event_code_text()`), and for
  an NXDN stand-in, which is never the row's access code (`dsd_access_code_current()`); the access code's numeric sys id
  (DMR `sys_id2`, P25 and NXDN `sys_id3`) holds the row's code or its "not decoded" value (DMR 16, P25 0, NXDN
  `(unsigned)-1`), since 0 is a DMR and NXDN code, never a Phase 2 seed or a stand-in. The dPMR event line's `CC:` is the row's decoded colour code (its access code), not the
  call's `channel`, which `dpmr_publish_call()` sets to 0 for an undecoded code. In
  `watchdog_event_merge_system_identity()` the sentinel, not 0, is the missing code: a reacquired segment that decoded
  one fills it (0 included), and a decoded code is never replaced. The DMR, NXDN and dPMR strings are then rebuilt from
  the merged row (`watchdog_event_rebuild_code_sysid()`), so a code one segment decoded and a system code or site only
  the other decoded both appear. The DMR colour code, the NXDN RAN, the P25 NAC, the M17 CAN and dPMR's colour code in
  the string and the event line are the row's access code, so a call whose last render runs after the receiver moved
  names the code it was heard with (under `--playfiles`, which takes none, `DMR_CC_--` and `CC: --;` whatever the file
  reader wrote into `dmr_color_code`), set after the tuning load on a live render
  (`watchdog_event_current_apply_code_identity()`) and rebuilt by a merge from the merged code. An unknown NAC prints
  `---` (`P25_---`, `P25_45564006---_10_10`, `NAC: ---;`), three wide like the code, so the long form keeps its field
  positions; an unknown CAN prints `--` (`M17_CAN_--`, `CAN: --;`). The NXDN event line names the call's own `channel`
  and the row's `freq_hz`, never the global last grant (`nxdn_grant_chan`/`_freq`, which left the render env). The
  rdio-scanner sidecar's `freq` is `freq_hz` (clamped to `uint32_t`, still 0 below 1 MHz); it was the channel number
  before. Its `start_time` is the closing segment's `event_start_time`, or the WAV's open stamp when that is later
  (per-call WAV switched on mid-call). Every per-call WAV open sets `opts->wav_out_open_time`/`wav_out_open_timeR`
  from the decode clock, and the close passes the stamp to `dsd_rdio_export_call()`. A per-call WAV's temp name
  carries a per-process random token and a sequence number (`open_wav_file()`), so libsndfile's `SFM_RDWR` never
  appends a second call to a file; the close publishes it with `dsd_rename_noreplace()` (`<dsd-neo/platform/
  file_compat.h>`), drawing another number for a final name another recording holds, and keeps the temp file on any
  other failure (issue #661). Every publication is serialized: the decoder and the watchdog publish under the P25 SM
  tick guard or the event layer's call-state lock, and the WAV app commands (start, stop, toggle, static and raw open)
  take the tick guard in `apply_cmd()`, so one process never publishes two recordings at once.
  `dsd_state_rebase_decode_timestamps()` pulls a stamp that is ahead of now back to now, which covers the `-P` open
  while arguments are parsed, before a replay moves the clock. `event_time` is the row's last activity, the segment's
  end by the time its WAV closes. It is only the fallback for a row with no start (a `--playfiles` row, whose stamp is
  on the file's clock, so the open time is not compared with it), and decode time comes after that. `stop_time` adds
  the WAV length to the start (`RUNTIME_RDIO_EXPORT`, `CORE_MBE_FILE_IO`, `CORE_INIT_STATE`, `UI_MENU_SERVICES`, and
  `CORE_CALL_ALERT_HISTORY` through the real event layer). The WAV name stays on `event_time`. The control API's
  event rows carry `freq_hz`, `access_code_kind` and `access_code`, folded into the row fingerprint. Tests:
  `CORE_ACCESS_CODE`, `CORE_CALL_ALERT_HISTORY`, `RUNTIME_RDIO_EXPORT`, `API_SERVER`.
- History system and site (`dsd_events.c`, `watchdog_event_current_load_system()`): a voice row's system and site slots
  come from the received identity record of the call's protocol family, never from decoder state a seed, a preset, a
  control-channel anchor or another carrier wrote: P25 WACN, SYSID, RFSS and SITE (`sys_id1`, `_2`, `_4`, `_5`), the DMR
  Tier III system code (`sys_id1`), the NXDN location's site and system (`sys_id1`, `_2`; for IDAS the SCCH site code,
  else the Type-D SRV_INFO site, and the Type-D system ID), the EDACS site, area and system (`sys_id1`..`_3`), the TETRA
  MCC and MNC (`sys_id1`, `_2`; a direct mode carrier marks `sys_id3` 1 instead). A part not received takes the value a
  row had before one was decoded (0, so `P25_<NAC>` without a WACN and `EDACS_SITE_000`); dPMR, M17, YSF, D-STAR and
  X2-TDMA rows carry none. The row takes the parts whole from the record of the carrier the call began on (the record's
  epoch is the call's `dsd_call_snapshot::rx_id_epoch`): the record keeps them one consistent observation, and a note
  naming another system, site or access code (an IDAS SCCH site ID that disagrees with the Type-D system ID, another
  EDACS or P25 system, another NAC on the carrier) moves the record's epoch, so the row keeps the identity it was heard
  under, never a mix. While the call is ACTIVE it takes the live record; once it has ENDED, the record the canonical
  layer captured at the end transition (`dsd_call_state_end_ex()` copies `dsd_state::rx_id` into
  `dsd_call_snapshot::ended_rx_id` while its epoch is still the call's, else captures none): identity received after the
  last active render and before the end (a Phase 2 MAC_IDLE handles its broadcast before `p25_sm_emit_idle_at()` ends
  the call) counts, identity received after the end does not. A record of another carrier, or no capture, leaves the row
  what it took last, so a held drop verdict or the finalize pass that runs after a retune, a `-Y` hop or a trunk-scan
  switch never takes the next carrier's. Direct mode empties the record, so the end also notes whether a TETRA call
  ended on a direct mode carrier (`dsd_call_snapshot::ended_tetra_direct`), and the ended row keeps its DMO mark from
  that. The synthetic snapshot a noncanonical P25 encryption lockout notice renders
  (`p25_lockout_snapshot_from_observation()`) takes the current record's epoch, as the grant was heard on this carrier
  (`P25_P1_LOCKOUT_EVENTS`). Test: `CORE_CALL_ALERT_HISTORY` (`test_system_identity_*`).
- API note: text arriving as UTF-16 code units (DMR UDT/SMS, talker aliases) is decoded with
  `<dsd-neo/core/utf16.h>` and printed one scalar value at a time through `dsd_unicode_fput_scalar()` in
  `<dsd-neo/runtime/unicode.h>`. Never pass a code unit to `%lc`: a lone surrogate has no encoding, and the
  Windows CRT turns that failed conversion into an unbounded write of the stack. Semgrep blocks `%lc`/`%ls`
- Invariant (Motorola talker alias reassembly, `src/core/util/dsd_alias.c`, `<dsd-neo/core/embedded_alias.h>`): the
  MFID 0x90 alias (Phase 1 LCW 0x15/0x17, Phase 2 MAC 0x91/0x95) assembles in `dsd_state::p25_apx_alias_rx[slot]`
  (`p25_apx_alias_rx_state_t`), which owns its header copy, block bitmap and record store; it never touches the
  `dmr_pdu_sf` scratch, so an LCW Call Termination or Motorola Talker EOT (which clear that scratch) or the DMR and
  Harris assemblers cannot wipe a record in flight. Phase 2 handlers take the dispatcher's voice slot (`ctx->slot`, the
  SACCH-inverted slot), never `currentslot`. Blocks are stored by number and the record completes once blocks
  1..N are all present, in any order. An assembly is bound to the call epoch (the slot's current or retained ended
  call) and the tuning generation its header arrived in, and survives that epoch's ACTIVE -> ENDED, since Motorola
  sends the alias in hangtime: a header identical in every received bit within the same epoch and generation is a
  repeat and keeps the blocks, any other header restarts, and a block from another epoch or generation, a block whose
  sequence or number the header does not cover, or one past the store's end abandons the record. The engine's carrier
  resets clear the store with the rest of the slot state. The record's length is not the first zero word: scrambled
  alias octets and the CRC can be zero, so the candidate lengths run from the last non-zero 16-bit word to the
  received extent (bounded by the store and the 200-octet decoder scratch) and the first one whose CRC checks wins
  (`apx_alias_record_bits()`). The name the event row, the learned policy row and the group file store is the decoded
  UTF-16BE as UTF-8 (`apx_embedded_alias_dump()`), bounded to the policy name on a scalar boundary: C0, C1 and NUL
  become spaces (`dsd_unicode_scalar_is_control()`), a comma a dot for the CSV, an unpaired surrogate U+FFFD.
  The learned policy row is updated before the event row is enriched (`dsd_event_enrich_alias_ex()`), because the
  alias usually lands after its call's row committed: an alias on the epoch's committed row re-resolves that row's
  source label through `dsd_source_label_lookup()` (an imported source alias, else the group-list exact row, where an
  imported row outranks a learned one), re-renders the row against its `committed_env`, and appends one `Enriched:`
  continuation (the re-rendered line when it changed, and the new `Talker Alias:` line) carrying the row's own stamp,
  with no end effects; a repeat that changes nothing logs nothing. Lines already written stay as written (the log is
  append-only). The event layer caches the label of the epoch's row on the call (`dsd_call_snapshot::source_label`,
  keyed by `source_label_id`) when the row commits and when a late alias renames it, and the shared call view
  (`slot_source_text()`) reads that cache for an ended call whose staged row is gone, so the held Monitor view names
  the talker without a source-table search per render.
  Tests: `P25_MOTOROLA_ALIAS` (real LCW and MAC dispatch),
  `DMR_TALKER_ALIAS_HEADER_LEN` (store bounds and isolation).
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
- State extension slots (`<dsd-neo/core/state_ext.h>`): engine owns 0, 1, 3 and 7; core 2, 4, 5 and 8; runtime 6;
  DSP 9, 10 and 11 (taken from the core range as runtime took 6 from the engine's): 9 is `DSD_STATE_EXT_DSP_ANALOG_RX`,
  the analog receive working state, 10 `DSD_STATE_EXT_DSP_ANALOG_AUDIO`, the analog monitor's audio chains, and 11
  `DSD_STATE_EXT_DSP_ANALOG_SINK`, the analog sinks' converters to the sink rate. `CORE_STATE_EXT` pins all three.
- API note: `dsd_state::analog_rx` (`dsd_analog_rx_publication` in `<dsd-neo/core/state.h>`, issues #522 and #523) is
  the received-tone publication every frontend reads: int-only (`carrier_open`, `tone_kind`, `tone_state`,
  `ctcss_tenths_hz`, `ctcss_off_value` (1 while the locked CTCSS tone rests on transmitter tone error alone, issue #643:
  the tone policy rejects on it only when its window ends), `dcs_code` (the code as its value, 023 octal = 19) and
  `dcs_inverted` for a DCS lock, always the canonical member of the code's alias class, `dcs_candidate` (1 while, with
  nothing locked or only an off-value CTCSS tone, the DCS detector holds a candidate code it has read once and not yet
  confirmed: the tone policy waits past its window for it), `gate`
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
- API note: `dsd_state::tetra_pub` (`dsd_tetra_publication` in `<dsd-neo/core/tetra_publication.h>`, which `state.h`
  includes) is what the TETRA decoder shows every frontend. Its working state lives in its state extension, which no
  snapshot copies; at the end of every burst `tetra_publish()` (`src/protocol/tetra/tetra.c`, from
  `tetra_frame_leave()`, which then ends the burst for the frontends and the event watchdog with that publication,
  `tetra_burst_done()`, whatever burst it was; a carrier boundary, `tetra_forget_carrier()`, clears the published uplink
  air encryption at once) copies into
  the publication the working mode (TMO downlink, uplink or control uplink, DMO
  direct or through a DM-REP, DM-GATE or both), taken only once a block passed its CRC, the network (MCC, MNC, 6-bit
  colour code), the air-interface encryption (on a trunked uplink the encrypted flag of its last MAC-ACCESS or
  MAC-DATA, `tetra_state_t::ul_enc`) and every algorithm signalling named (`ksg_mask`, bit k for KSG k: the
  network's KSG evidence in trunked mode, the transmission's DMAC-SYNC KSG in direct mode), apart from it the
  algorithms radios proposed in clear uplink registrations (`ksg_requested_mask`, trunked mode only), the cell's broadcast
  security (`aie` from the BS service details; `sec_known`, `sec_auth`, `sec_class1`, `sec_class3`, `sec_sckn` from the
  extended services broadcast), its location area, the carrier being received (`tetra_calls_rx_carrier()`) and which
  kind of air key applies there with its TEA (`key_kind`, `key_tea`, from `tetra_air_key_for_carrier()`; never a key),
  in direct mode the transmission's security class and SCKN (`dmo_class`, `dmo_sckn`), where call following stands, the SYSINFO main carrier and its frequency, the channel allocation's carrier, timeslot bitmap (bit 3 is
  timeslot 1, as the Timeslot assigned field carries it) and frequency, the last D-MM STATUS, and the speech decoder's
  status (`dsd_tetra_codec_status`: unknown until a speech frame reached it, ready, decode errors while the last frame
  failed or no decoder could be made) with its frame and error counts. The network, the SYSINFO main carrier, the traffic channel and the D-MM STATUS are a trunked network's, so a
  direct mode carrier publishes none of them, though the TETRA state keeps the network identity for the trunked carrier
  the session goes back to: a retune from a TMO cell to a DMO carrier shows no network of that cell beside the DMO
  mode, as it shows no colour code, cell security, carrier or channel rows. A synchronisation burst that sets the mode or the network publishes at once (`tetra_bsch_parse()`,
  `tetra_dmo_apply_sync()`): the calls and notices the rest of its burst emits read the carrier's colour code from the
  publication (`dsd_access_code_current()`), so they carry the cell or direct mode channel they were decoded on, not the
  last burst's. Integers only, so it rides the `vertex_ks_count..ui_msg` snapshot range, pinned by a `_Static_assert` in
  `ui_snapshot.c`. `dsd_tetra_publication_reset()` clears it and bumps its `generation`: a preset other than TETRA does
  when it commits (`dsd_apply_decode_mode_preset()`, `dsd_decode_mode_commit_tetra()`), and so does `tetra_state_reset()` with the state it drops. TETRA calls, short data
  and status messages reach the frontends as canonical calls and event history notices (TETRA "Calls" below), not
  through the publication. Tests: `TETRA_PUBLICATION`, `APP_CONTROL_SNAPSHOT_EVENT_HISTORY`, `RUNTIME_DECODE_MODE`.
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
- Invariant (learned alias rows, `<dsd-neo/core/talkgroup_policy.h>`): every over-the-air alias publisher (Motorola,
  Harris and Tait in `dsd_alias.c`, the P25 Phase 1 LSD soft ID in `p25p1_ldu1.c`/`p25p1_ldu2.c`) learns its row
  through `dsd_tg_policy_learn_exact()`, always in mode `D` (an encrypted radio is locked out by the encryption ledger,
  never by its alias row): a talkgroup/user row for the ID wins, a learned row is replaced only when its normalised
  name changed, and a repeat changes nothing, so the policy generation (and every frontend's snapshot copy keyed on it)
  stays put while a radio keeps sending the same alias. `DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY` follows the same
  compare. Only a first-learned row is appended to the group file, which is append-only and whose first row for an ID
  is the one a reload keeps: a renamed radio's new alias holds for the session, and after a restart the file's earlier
  name loads as an imported row and wins (documented limit). Tests: `CORE_TALKGROUP_POLICY`, `P25_MOTOROLA_ALIAS`.
- Invariant (alias rows are radios, `talkgroup_policy.c` `tg_policy_entry_is_learned_alias()`): a row with source
  `RUNTIME_ALIAS` or mode `D` (the durable mark a persisted alias keeps) never decides a talkgroup: group evaluation,
  allow-list membership and the followed calls' priority refresh skip it and fall through to a talkgroup row or the
  narrowest range. Private evaluation and labels still see it, behind any talkgroup row for the ID. Edits by bounds
  (`set_fields`, `set_mode`, `remove_bounds`, heard-only selection) address the talkgroup row first and take an
  alias-only ID over in place. Edit staleness (`dsd_tg_policy_edit_version_valid()`) is judged against the structural
  generation, which learned aliases and session avoids do not move; a context created in place by decoder writes
  accepts an edit captured with nothing loaded, and a generation wrap is structural.
- Invariant (preemption bookkeeping, `dsd_tg_policy_should_preempt()`): the active-call records are what a protocol
  describes, not a history. Right before asking, a protocol calls `dsd_tg_policy_describe_followed_call()` for each
  call it holds, with the time it began following that call (dwell runs from it; a matching record keeps its start and
  cooldown; nothing is stamped). A candidate is the held call itself, and never preempts it, only when it matches a
  record's target, channel, slot and group or private class (`tg_policy_active_call_matches_route_slot()`, as a
  describe matches): a talkgroup and a unit may share a number, so a candidate of the other class is another call that
  displaces the record by priority. An accepted takeover commits its cooldown with `dsd_tg_policy_commit_preemption()`
  after the preempting call is acquired, because the release cleared the displaced records. `note_active_call()`
  stamps a cooldown only when it replaces a valid, different record. A list reload or clear carries the records and
  the last cooldown into the new context; `install` (a scan target transition) restarts them.
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
  same gate and protocol-owned encryption verdict as the stereo path. P25 Phase 1's part of that verdict is
  `p25_crypto_audio_output_permitted()` in `<dsd-neo/protocol/p25/p25_crypto.h>`, shared with the record gate.
  Per-call WAV writers use `dsd_audio_record_gate_mono()` (the vocoder's verdict on the slot's frame and policy), or
  `dsd_audio_record_policy_gate_slot()` (policy alone, on the slot the protocol publishes) where the decode path
  settles crypto itself: X2-TDMA, D-STAR, YSF V/D2, EDACS analog and SDRTrunk JSON
  playback (YSF EHR and full-rate frames go through the vocoder's full record gate). Every MBE capture save point (`mbe_capture_frame()` in `src/core/vocoder/dsd_mbe.c`, YSF V/D2 in `ysf.c`, JSON
  playback in `dsd_file.c`) applies the same policy gate; the vocoder's save points also apply the frame's verdict
  (below), which for P25 is the P25 speaker rule. The static WAV writes only recordable slots: the mono paths ask about the slot being played (X2-TDMA may
  play slot 1), and the stereo mixes write silent any channel whose routed source slot, as the output policy
  mirrors it, is record-blocked (`dsd_stereo_wav_channel_mask()`). The vocoder writes each MBE frame's per-call
  WAV block; YSF writes only its V/D2 frames, which decode straight through mbelib. X2-TDMA stages either timeslot
  in the mono (left) buffer: the vocoder writes only its per-call WAV, into the frame's own timeslot's file from that
  buffer (`dsd_write_mono_voice_to_slot_wav()`), and leaves the static WAV to the mono mixer's stereo block (issue
  #654). The mono mixers (SS and MS) read the switch of the timeslot they play (`dsd_mono_source_slot_on()`:
  `slot2_on` for an X2 slot-2 frame, `slot1_on` otherwise) as their last gate, after the talkgroup gate, so a hold
  never plays a switched-off timeslot. Every mono mixer call carries a frame the vocoder just synthesized: X2 mixes
  nothing for a data or idle timeslot, and P25 Phase 1 mixes nothing for a frame `process_IMBE()` did not dispatch (a
  skipped non-standard c0 word, a crypto-muted frame), in every output format.
  `audio_out_idx` counts only the current frame's short samples: the vocoder zeroes it at each frame and
  `dsd_voice_stage_short_left()` counts its own frame (160, or 960 upsampled), so MS plays exactly that frame, or
  nothing (no output, static WAV or stamp) when the frame staged no short samples, as an encrypted NXDN or dPMR
  frame without its key does. FDMA dispatchers (NXDN, YSF, D-STAR, ProVoice, dPMR) set
  `currentslot` to 0, since these paths read it. EDACS analog voice applies the talkgroup gate at its own output.
  M17 is outside talkgroup policy (callsign addresses): its Codec2 voice plays through the FDMA mixers with a settled
  verdict (`dsd_audio_play_decoded_pcm16()`, below). Writers outside the mixers follow the mixers' output rule
  (`dsd_output_*_block()`): the mute (`audio_out` 0, which leaves the output open) silences the local stream, UDP and
  the raw fd alike, and EDACS analog and M17 Codec2 voice also stop while slot 1 is switched off. Every mix applies the
  slot switches last, after the talkgroup gate and a hold, so a held call on a switched-off slot stays silent (SS, SS3
  and the P25 Phase 2 playout, in either output format). The P25 Phase 2 playout reads the slot switches before it
  routes, so a muted companion never mirrors a switched-off slot into both channels. Decoding reads the user's slot
  switches, slot preference, encrypted-audio mute options (`dmr_mute_encL/R`) and DMR late-entry setting (`dmr_le`)
  and never writes them (issue #657): what the user set is what the menu, the API and the next frame see. The slot
  preference is applied as a tie-break in both formats: with both slots audible, the preferred slot fills both
  channels while it carries voice (DMR: `dsd_dmr_preferred_slot_fills_both()`, the slot a hold names standing in for
  the preference; P25 Phase 2: a decoded frame of the preferred slot, never fill).
- Invariant (encrypted-audio verdict, `src/core/audio/voice_crypto.c`, `<dsd-neo/core/audio.h>`, issue #655): each
  vocoder frame gets one verdict after its keystream: `mbe_frame_slot_muted()` switches on the explicit
  `mbe_frame_ctx_t::protocol` (`dsd_voice_protocol`) its caller supplied. The borrowed codec synctype never selects
  its encryption rules. Mixers outside the vocoder resolve the owner through `dsd_voice_frame_owner()` (the
  dispatched protocol, with the helper's fallback outside a dispatch) and ask `dsd_voice_crypto_audible()` for the
  same rule. Live P25, NXDN and dPMR mixers preserve the vocoder's cached mute before consulting call metadata,
  so a still-loaded key cannot revive an exhausted frame or stamp its silence as media. YSF V/D2 and SDRTrunk
  stage independently and do not inherit that cached mute. `mbe_post_audio_and_recording()` stores the verdict per slot in `dsd_state::mbe_slot_muted` before short staging (`mbe_post_{left,right}_stage()` and the mono
  post path), MBE capture (`mbe_capture_frame()`), the per-call WAV (`dsd_audio_record_gate_mono()`) and the shared
  DMR mixer (SS3) consume it. FDMA output uses the same protocol-owned verdict in MS and SS, in both formats.
  DMR is classified from the frame's service options and ALG ID, saved before a transform such as `-S` or a Vertex
  keystream clears them. A loaded key for that ALG ID, a keyed transform applied to the frame (Basic or Hytera
  Privacy, forced with `-4` too, `-S`, a mapped Vertex keystream), or forced clear (`dsd_key_dmr_forced_clear()`)
  settles its decryptability in the frame classification; no mixer applies a forced-privacy unmute override.
  P25 Phase 1 and Phase 2 use `p25_crypto_audio_output_permitted()`; Phase 2 also needs its decode gate. NXDN and
  dPMR use `nxdn_cipher_type` and the key the vocoder applies
  (`dsd_nxdn_voice_crypto_class()`; the dPMR bridge sets the cipher per half). YSF, D-STAR, X2-TDMA and ProVoice
  classify no encryption. The user's class rule (`dsd_voice_crypto_class_audible()`) applies reverse mute (`-q`)
  to the encrypted transmission, whether a key decrypts it or not, and mutes clear frames; otherwise an
  undecryptable frame plays only while the slot's encrypted-audio mute (`dmr_mute_encL/R`, slot 0's for NXDN and
  dPMR) is off. Protocols that classify no encryption and SDRTrunk JSON playback, which checks crypto itself, do
  not take that reverse-mute rule. No key load (CLI, app-control, scan row or trunk-scan target) or decode path
  writes the user's encrypted-audio mute, and reverse mute never rewrites it. A DMR mono slot not playing is muted
  too. Keystream exhaustion (`mbe_frame_ctx_t::crypto_muted`) is a hard mute before the class and user options:
  reverse mute, a loaded key and a talkgroup hold cannot make that frame audible or recordable. Muted slots stage
  silence in `s_l`/`s_r` and retire their fresh-media marks, so a mixer cannot replay an earlier frame. A talkgroup
  hold decides policy and the effective slot preference only: `dsd_audio_group_gate_*()` and the SS3 hold helper
  preserve the incoming mute, the user's slot switches and stored preference. `dsd_audio_group_gate_call()` is the
  same talkgroup gate for a call that is not published yet, judged on the kind and identities it will carry (a private
  call on both parties, a group call on its target and source), for a decoder that chooses between calls before it
  publishes one: TETRA's timeslots share canonical slot 0 (`CORE_AUDIO_GROUP_GATE`). MBE capture saves each frame after
  its own verdict and before staging (issue #661). The vocoder no longer writes `dmr_encL/R`; NXDN retains its own
  classification there for lockout. P25 Phase 2 playout snapshots the P25 rule and actual staged-media provenance
  per frame, so a queued muted frame remains silent.
- Invariant (digital voice output formats, issue #652): integer and float (`-y`) output are one chain, not two. The
  vocoder runs the filter and gain once with `dsd_voice_chain_run()` (the fixed `-g`
  gain, or the auto gain: per slot, peaks toward 30000, `aout_gain`/`aout_gainR` carried from call to call, its peak
  history cleared by the no-carrier reset), and stages each frame's 16-bit samples (`s_l`/`s_r`, and
  `mbe_short_silenced`) the same way in both formats, as do the vocoder bypasses (SDRTrunk JSON playback, YSF V/D2, M17 Codec2).
  Every protocol plays through the same mixers (MS, SS, SS3, the P25 Phase 2 playout), so routing, muting, the digital
  high-pass, the audible-audio stamp and the static and per-call WAVs are the same in both. The one place the format
  shows is `dsd_output_s16_block()` (`dsd_audio2.c`, the writer every mixer and the legacy short output use): under
  `-y` the raw fd (`-o -`) and UDP take each block as f32, s16 / 32768 (exact), in whole frames of at most 1920
  samples per write (one write and one datagram per block for every size a mixer writes), while the local device
  stream is 16-bit in either format and takes the block unchanged. Nothing resets the gain at call boundaries in
  either format. The beeper also uses the shared s16 sink conversion. Outside the chain: EDACS analog voice (no raw-fd output under `-y`). Tests: `DECODE_IQ_FLOAT_OUTPUT_*` (both formats
  replayed and compared), `CORE_AUDIO2_HELPERS`, `CORE_MBE_TRANSFORM_CONTEXT`, `CORE_P25P2_PLAYOUT`.
- API note (decoded PCM voice, `dsd_audio_play_decoded_pcm16()` in `<dsd-neo/core/audio.h>`, `dsd_audio2.c`, issue
  #658): a decoder that synthesizes its own 8 kHz mono int16 voice (M17's Codec2) settles its own gates before calling.
  Each whole 160-sample frame is copied to `audio_out_temp_buf`, runs `dsd_voice_chain_run()` on slot 0 once, then
  `dsd_voice_stage_short_left()`. The per-call WAV takes the same chained frame (`writeSynthesizedVoice()`). MS or SS
  routes it according to `pulse_digi_out_channels`, with a settled verdict that skips talkgroup policy, FDMA crypto
  rules and the static WAV's record policy; the slot 1 switch, output mute and audible-audio stamp still apply.
  Both formats use the shared short staging and sink conversion, so gain, high-pass, stereo level and WAV samples
  agree. Public mixer callers retain their protocol and policy verdicts. A trailing partial frame is dropped, and
  a state without the output history `initState()` allocates plays nothing. The M17 stream encoder (`-fZ`) opens the
  decoded monitor at 8 kHz; explicit `-8` selects its separate 48 kHz baseband monitor whether before or after the
  mode selector. Leaving an analog preset clears monitoring it enabled implicitly. A decoder under talkgroup policy
  that settles the record policy too (TETRA speech, `dsd_audio_record_policy_gate_slot()`) calls
  `dsd_audio_play_decoded_pcm16_record()`: with `record` 0 its frames play but neither WAV takes them. Tests:
  `CORE_AUDIO2_HELPERS`, `M17_STATE_DISPATCH`, `RUNTIME_CLI_PARSE`, `DECODE_IQ_M17_VOICE_OUTPUT`,
  `TETRA_VOICE_PIPELINE`.
- Invariant (P25 Phase 2 voice playout, `<dsd-neo/core/p25p2_playout.h>`, `src/core/audio/p25p2_playout.c`, issue
  #651): both output formats play P25 Phase 2 voice through one staging and one mixer. The frame decoder queues each
  frame its decode gate admits on the slot's queue right after the vocoder (`dsd_p25p2_playout_stage()`): the slot's
  `s_l`/`s_r` as the vocoder staged them, or zeros marked not fresh when `dsd_state::mbe_short_silenced` says the
  vocoder left them out, in either output format. A frame the gate mutes closes the slot's stream (`_note_muted()`).
  After each timeslot the decoder reports what it carried for its slot (`_burst_done()`: 4V, 2V, SACCH, OTHER for a
  FACCH or LCCH in a voice position, LOST), and after slot 2's timeslot of each pair it runs the emission
  (`_pair_done()`).
  - Timing. A physical superframe carries 18 frames per slot, but the 4V/2V order rotates per slot and transmission,
    so the two slots' bursts are out of step by up to two frames. The emission plays as many blocks as every open
    stream has ready (with none open, what the closed ones hold): a slot running ahead keeps at most 2 frames of carry,
    both slots play side by side at the air rate, and a slot END never replays its companion. A queue past 20 entries
    plays out regardless (a safety bound, not part of the timing). A placeholder resolved to 2 frames or repaid fill
    leaves removed entries inside a queue; a push that finds the queue full reclaims them first, so no decoded frame
    loses its place to them. A queue full of live entries (barriers of successive streams and their frames) while the
    companion's placeholder holds every level at zero makes room the same way as a last resort (`make_room()`: both
    slots' placeholders resolve as four and blocks play): a decoded frame is never dropped. Every entry carries the pair it belongs to (`pair_clock`, advanced by each
    `_pair_done()`): a stream that opens while the companion still holds entries of earlier pairs (behind a
    placeholder, or a carry) starts behind a barrier entry, and sits out each block in which the companion plays an
    entry of an earlier pair, so its first frames play beside the companion's frames of its own pair. Two barriers at
    the heads resolve in pair order (the earlier goes, the later still waits), and a slot's level
    (`dsd_p25p2_playout_level()`) walks both queues as the emission pairs them, waits included.
  - Streams. A stream is open only while its slot's canonical call is ACTIVE with the epoch the stream began with
    (checked at stage, `_burst_done()` and `_pair_done()`); an epoch change or an ended call closes it, and its queued
    frames still play. PTT and MAC repeats within one epoch change nothing.
  - Fill. A voice burst an open stream misses (a DUID error, a FACCH in its place, a timeslot an abort or a missed sync
    skipped) queues silence in its place: 2 frames at the slot's 2V pair and 4 elsewhere, once the 2V pair is proven
    (a decoded 2V, or 4V at the other four pairs). Before that, in the stream's first superframe covered from pair 0, a
    missing pair that could be the 2V waits as a placeholder (four entries or none: a queue too full to take four
    leaves the pair out), which holds its slot, and through the emission rule the companion, until a proof or the
    superframe's end settles it; afterwards the pair the first superframe gave 2 is provisional and fills at once. A fill that turns out mis-sized becomes debt (`fill_debt`), repaid only through fill,
    never by dropping decoded audio. Five consecutive missed voice bursts close a stream. A DUID read as SACCH at a
    voice position counts as a lost voice burst. A stream closed after all five voice pairs of a superframe it covered
    from pair 0, before that superframe's SACCH (an END in the companion's SACCH), resolves its placeholders by the
    18-frame rule first.
  - Verdicts. A burst's output verdict (talkgroup block and hold, `p25_crypto_audio_output_permitted()`, static-WAV
    recordability) is fixed when its first frame is queued, from the decision the decode gate took for the burst
    (`dsd_p25p2_decode_audio_allowed_verdict()`), so a tail played after END keeps its own call's verdict. While a
    frame's call is still ACTIVE with the frame's epoch, emission also requires the slot's current talkgroup verdict,
    and after the call ended the last one taken while it was active: a Skip, lockout or policy edit mutes what was
    queued before it, in the release drain too. That verdict decides only whether a queued frame plays; its static-WAV
    recordability stays as queued. The playout evaluates no policy as it plays: the current verdict is the decode
    gate's decision for the call's latest burst (`dsd_p25p2_playout_stage_decided()`, kept with the epoch and key the
    gate judged the call on), or the one taken (`dsd_p25p2_playout_note_policy()`) by every path
    that changes a verdict or ends the calls before the playout drains, and each emission checks, cheaply
    (`dsd_p25p2_playout_policy_key()`), whether anything a waiting call's verdict reads moved since (its source named
    by signaling, a patch change, a policy row learned from an alias, the hold, the allow list, or the decode clock's
    next second as a bound for anything that runs out with time), taking its verdict again if so. The paths that
    note: a user block
    (`apply_user_block_transition_locked()`), the SM release before its return tune (whose engine hook ends the calls),
    the carrier boundary and the no-carrier pass. Every command that can change a call's verdict (a policy-store
    write, a hold change or the allow-list toggle, `apply_cmd()`) makes the change under the tick guard and takes the
    new verdict before it leaves the guard, so the change never lands inside a watchdog release, and a drain the
    no-carrier pass has to leave to its next pass still plays under it. A verdict is taken from one snapshot of the
    call (its epoch and policy evaluation alike): the no-carrier pass may end the calls on the decoder thread while the
    watchdog holds the guard, and a call ended in between keeps the verdict last taken for it. A new call on a slot
    first hands the previous call's verdict to the frames that call still has queued
    (`dsd_p25p2_playout_entry::live`). The slot switches are read at emission and never written: a switched-off slot
    stays silent, a hold included (issue #657).
  - Output. Routing comes from content: two audible slots play as stereo, one plays in both ears; one-channel output
    averages two (`audio_mix_mono_from_slots_s16()`) and plays one alone. With a slot preference set, a block in
    which both slots are audible and the preferred slot's frame is decoded media (`fresh`, never fill) plays that
    slot alone, in both ears; the static WAV follows. The playout filters nothing: the vocoder's voice chain
    high-passed and gained each frame on its slot before it was queued; the static WAV records each played stereo block,
    silent in a channel whose source frame is not recordable, and skips a block with none. At an output rate other
    than 8 kHz nothing is written, but the queues still empty. Every emission ends with the housekeeping SS18 and FS4
    used to do: `dsd_audio_maybe_reset_output_ring_left/right()` and zeroed `audio_out_idx/R`, since
    the short staging (`dsd_voice_stage_short_left/right()`) advances the vocoder's output pointers per frame.
  - When audio plays or goes. `_drain()` plays everything queued and closes both streams: in the release hook
    `dsd_p25p2_flush_partial_audio()` (drain, then reset) once the return-to-CC tune is accepted, in
    `p25_sm_abandon_carrier()`, `p25p2_teardown_call()`, `dsd_engine_forget_carrier_decoding()` (the carrier boundary
    and a trunk-scan target switch) and the no-carrier pass. `_reset()` discards (`_discard()` where an alert may be
    waiting): on manual CC selection, when the user leaves a followed voice channel for the control channel (Return
    to CC, a candidate cycle: `p25_sm_on_external_cc_tune()`), at the conventional idle expiry, and after one of those
    drains (`p25p2_frame_forget_carrier()`, the no-carrier pass). `_close()` (`dsd_p25p2_flush_partial_audio_slot()`:
    slot END, IDLE, HANGTIME, MAC Release, an LCCH that failed its CRC, the SM's slot media closes and a moved target's
    slot) closes one slot's stream and never plays or discards. Crypto purges reset only the vocoder's state.
  - Alerts. `beeper()` for a slot whose queue still holds audio (an END with frames still to play) hands the alert to
    the playout (`dsd_p25p2_playout_defer_alert()`): it sounds right after the block in which the last entry queued
    before it left (played, or discarded), in order per slot, so an end-of-call tone follows the call's last frames.
    A played entry counts as gone only once its block is written, removed entries left at a queue's head are let go
    before the check, and alerts sound only from the playout's own steps (after a block, `_pair_done()`, `_drain()`,
    `_discard()`), never on the raising side; an alert stays queued until its tone is out, so one raised meanwhile
    waits behind it. Past `DSD_P25P2_PLAYOUT_ALERTS` waiting on a slot (far more than a
    superframe's call events), an alert is dropped rather than sounded early. Alerts can be raised outside the tick
    guard (a held VOICE_END from the frame-sync pass), so the entry counts they compare and the alert queues sit behind
    the playout's own lock.
  - Trunking never reads what the output holds: a slot occupies the carrier by its audio gate and MAC recency alone
    (`p25_voice_slot_retains_carrier()`, `frame_sync_p25_slot_activity()`, the VPDU and LCCH occupancy checks), the
    same in either output format. The jitter ring that once stood in for activity, with `--p25-ring-hold`, is gone.
  - Serialization. Every entry point runs on the decoder thread inside `processFrame()`, which holds the P25 SM tick
    guard, or in SM and engine code holding that guard. The no-carrier pass takes the calls' verdicts, drains and resets
    it under the guard, taken when free, before it ends the calls, which a return to the control channel does
    (`no_carrier_drain_playout()`; the return leaves the slot switches as the user set them, issue #657): a pass
    that finds the guard taken leaves the playout to the next pass, and the return to the control channel with it
    while the playout still holds audio (`dsd_p25p2_playout_holds_audio()`, readable from any thread). Frame sync's
    no-sync release (`p25_sm_release_from_frame_sync()`, outside `processFrame()`) takes the guard the same way, or
    leaves the release to the next no-sync pass. Commands that change what the playout reads (a call's verdict, the
    slot switches and the output buffers a switched-off slot rewinds, the slot preference: `command_feeds_playout()`)
    apply under the guard. The playout lives in `dsd_state` outside the ranges UI snapshots copy.
  Tests: `CORE_P25P2_PLAYOUT`, `P25_P2_VOICE_OUTPUT`, `P25_P2_MIXER_GATE`.
- Invariant (audible-audio stamp, `<dsd-neo/core/audio_activity.h>`, `src/core/audio/audio_activity.c`, issue #574): one
  process-wide atomic word says when the decoder last emitted audio the app plays, in real-time monotonic milliseconds
  (`dsd_realtime_mono_ms()`: it drives the Android screen, not a decode decision), 0 for none. Each writer of decoded or
  analog audio notes it (`dsd_audio_activity_note()`) at its own final emit decision: after every gate it applies
  (the protocol-owned crypto verdict, talkgroup policy, slot switches, DMR mono and the mute),
  and only when an output receives the block. Every writer asks the same rule for that,
  `dsd_audio_activity_output_plays(opts, stream, fd_takes_block)`: the output on (`audio_out` 1) and of a type the
  writer serves, the local stream (type 0) while the stream it writes is open (`audio_out_stream` for decoded voice,
  `audio_raw_out` for analog), UDP (type 8), or the raw fd (type 1) where the writer writes that block (the analog
  monitor only in the analog-only modes, `dsd_audio_sink_monitor_takes_fd()`). So the local
  stream, UDP and the raw fd count alike, and the null output (type 9: `-o null`, which keeps that type once unmuted,
  and the M17 UDP frame output) never stamps. It notes provenance, never amplitude: a block counts when it carries
  decoded media, or squelch-open analog reception, from a slot whose gates pass, so valid all-zero decoded PCM counts
  and silence padding or a muted slot never does. The stamp is off until
  `dsd_audio_activity_arm()`, which only `dsd_app_notification_encode()` calls (its one caller is the Android JNI
  accessor `nativeNotificationStatus()`, from the service's status poll and `ScreenSupport`'s synchronous samples, so it
  is the stamp's one reader; see App-Control), and then stays armed for the process. Every writer asks
  `dsd_audio_activity_armed()`, one relaxed load, first, so a desktop, CLI or API session adds no scan, clock read or
  store per block. `dsd_audio_activity_read()` loads the word once (acquire) and ages that same value, -1 when it is 0
  or older than `DSD_AUDIO_ACTIVITY_MAX_AGE_MS` (60 s); `dsd_audio_activity_reset()` clears it and leaves it armed.
  Writers run on the decoder thread; arm, read and reset are safe from any thread. Stamped, in `dsd_audio2.c`
  (the mixers of both output formats): SS3 (by the DMR media marks below), and SS and MS (each decoded frame they play;
  MS only when it loaded one);
  the P25 Phase 2 playout (`p25p2_playout.c`: once per emission, when a played block carries an audible frame the
  vocoder did not silence; its drains reach it too); the
  legacy short output `playSynthesizedVoice()` (`dsd_audio.c`, SDRTrunk JSON playback included, past its delay); M17
  Codec2 voice through SS and MS (`dsd_audio_play_decoded_pcm16()`); EDACS analog voice
  (`edacs_emit_analog_audio()`); and the analog monitor sink (`symbol_write_unsynced_audio()` in `dsd_symbol.c`). Never
  stamped: the beeper's tones, the M17 baseband monitor, the `-6` raw WAV, the static and per-call WAV writers, and the
  output helpers below the writers (`dsd_output_*_block(s)()`, `dsd_audio_write()`, the UDP blasters), which cannot tell
  where a block came from. SS3 runs on a timer of bursts, skipped ones included, and replays each slot's buffers
  whether or not anything new was decoded into them, so the vocoder marks fresh media per slot
  (`dsd_audio_dmr_mix_media_staged()`/`_silenced()`/`_discard()`/`_take()` in `<dsd-neo/core/audio.h>`,
  `src/core/audio/dmr_mix_media.c`; DMR synctypes only, only while armed, decoder thread only): SHORT where the short
  path writes `s_l`/`s_r`, never for the silence staged for a muted slot. That silence replaces the slot's short frame,
  so it retires the slot's SHORT mark: the mark says short media was staged after the slot's last silence, which the
  frames the mix plays still hold, and a muted burst over an unmixed clear one leaves none. Each mix takes both slots'
  marks and stamps only for an unmuted slot holding a mark that a channel it emits carries. A talkgroup hold
  preserves the vocoder's mute, so it never makes a muted slot stamp. Slot purges (`dsd_mbe_purge_slot_audio()`),
  `initState()`, the engine's no-carrier reset and the end of the DMR BS loop (`finalize_dmr_bs()`) discard the marks.
  EDACS analog stamps a triplet only when an output receives it (`edacs_analog_output_writes()`: slot 1 on, then the
  shared rule on `audio_raw_out`, the raw fd taking 16-bit output only) and the squelch the call runs opened on it: the
  dynamic squelch heard a sample (the per-run marking `edacs_process_analog_triplet()` already applies, returned), the
  level squelch's power test (`pwr > call->sql`) passed, or the call runs none; a closed triplet is still written but
  never stamps. The analog monitor stamps analog reception only: the FM or AM monitor
  (`dsd_analog_monitor_tap_active()`), never the `-8` source monitor during digital decoding, with the tap's carrier
  open now and the tone policy passing it (`symbol_unsynced_carrier_active()`), a sink that receives it (the shared rule
  on `audio_raw_out`: the local raw stream while open, UDP, or the raw fd in the analog-only modes), and under the auto
  squelch at least one sample its own gate hears, so the fade-out written after the gate closes does not count and an
  unmodulated carrier does. The block's verdict is taken before the write, but the stamp is noted in the sink's write
  callback (`symbol_monitor_sink_write()`) when it hands the output samples, so a block the sink's converter mutes
  (`DSD_ANALOG_SINK_MUTED`: a rate it cannot take, no memory for it) stamps nothing. Tests: `CORE_AUDIO_ACTIVITY`,
  `CORE_AUDIO2_HELPERS`, `CORE_MBE_TRANSFORM_CONTEXT`, `CORE_AUDIO_GAIN`, `P25_P2_MIXER_GATE`, `M17_STATE_DISPATCH`,
  `EDACS_GRANT_TUNE_MATRIX`, `DSP_SYMBOL_REPLAY`, `DMR_BS_SYNC_TIMES`, `ENGINE_NO_CARRIER_RESET` (see
  `docs/testing.md`).
- Invariant (vocoder PCM scale, `<dsd-neo/core/vocoder.h>`): mbelib-neo's float PCM is int16 / 7
  (`mbe_floattoshort()` multiplies by 7), and from 2.3 it synthesizes speech at that reference level. The voice
  buffers (`audio_out_temp_buf`/`R`) hold int16-scale samples, which the voice chain's fixed and auto gain and the
  WAV writers expect, so every caller of an
  `mbe_process*f` API scales the frame it gets with `dsd_mbe_float_to_pcm16_scale()`: `store_process_result()` in
  `dsd_mbe.c` for the vocoder's own paths, SDRTrunk JSON playback in `dsd_file.c` and YSF V/D2 in `ysf.c`. A new
  call site does the same. Silence from `mbe_synthesizeSilencef()` is zero and needs none.
- Invariant (digital voice chain, `<dsd-neo/core/voice_chain.h>`, issue #653): every decoded 8 kHz voice frame goes
  through its slot's chain exactly once, in place, before anything reads it: the digital high-pass (`use_hpf_d`, `-v
  0x8`; a 2nd-order Butterworth at 300 Hz, unity in the voice band, `dsd_voice_hpf_*()` in `dsd_misc.c`), then the
  slot's gain (the fixed `-g` multiplier `audio_gain`/`audio_gainR`, unity below 0, or the auto peak normalizer on
  `aout_gain`/`aout_max_buf` and their `R` twins), then saturation to the int16 range (`dsd_voice_chain_run()` in
  `dsd_audio.c`). It runs whatever the output format, output type, mute or preset say, before consumer gates, and
  never decides who hears a frame; the gates still do. Producers: the vocoder at the top of its post stage
  (`mbe_post_audio_and_recording()`, on the buffer and slot `processMbeFrameInternal()` names for the decode: the right
  buffer for the slot path's slot 2, the left one otherwise, with X2-TDMA's timeslot as the slot), MBE file playback
  (`play_mbe_output_frame()`), SDRTrunk JSON playback (`sdrtrunk_chain_frame()`, as each frame is synthesized and
  before its decrypt-or-mute decision), YSF V/D2 (`ysf_emit_audio_from_temp()`), and M17 Codec2
  (`dsd_audio_play_decoded_pcm16()`, after M17's CAN and decryptability gates). Consumers take the chained frame
  as it is: the per-call WAV writers and the shared short staging (`dsd_voice_stage_short_left/right()`: `s_l`/`s_r`,
  the output ring and the 48 kHz upsample). No mixer filters or gains. The same staged samples feed every mixer in
  both formats, and float serialization happens only at the sink. The per-call and static WAVs therefore agree
  across output formats. `processAudio()`/`processAudioR()` remain as chain plus
  short staging for out-of-tree callers. Each slot keeps its own filter state (`dsd_state::voice_hpf[2]`), cleared
  when the slot's frames move to a new call (one nonzero epoch to another) or protocol family (the dispatched synctype,
  `frame_dispatch_synctype`, so a borrowed decoder does not count), while the high-pass is off, at each MBE playback
  file, at the carrier boundary and in the no-carrier pass's guarded playout drain, in `dsd_mbe_purge_slot_audio()` and
  in `init_audio_filters()` (startup, rate change, input switch). These resets forget the reception too, so a call
  learned partway through the frames after one does not restart the filter. The gain level carries across calls (the
  no-carrier pass clears only its peak history), so no protocol code writes `aout_gain`; the engine seeds 15 in auto
  mode and clamps `-g` for both formats.
  `beeper()` builds its tone once on the int16 scale and plays the same samples in both formats. Tests:
  `CORE_VOICE_CHAIN`, `CORE_MBE_TRANSFORM_CONTEXT`, `CORE_AUDIO_GAIN`, `DECODE_IQ_VOICE_CHAIN_*`.
- API note (analog receive options, `<dsd-neo/core/opts.h>`): `analog_demod` (`dsd_analog_demod`) is set by the
  decode presets (and restored with the scan-settings snapshot when a typed row is left); no width, CLI or menu code
  writes it directly. The analog preset selects FM, the AM preset (`DSDCFG_MODE_AM`, `-fM`, issue #524) selects AM,
  and every other preset puts it back to FM (the `-f` selectors the CLI handles itself do the same).
  `analog_nfm_bandwidth_hz`/`analog_am_bandwidth_hz` hold the configured channel widths, where 0 means the kind's
  default rather than an explicit request. An explicit width, or the AM default, forces the channel filter on; only
  the unset NFM default keeps the historical enable rule (`rtl_demod_analog_requested_width_hz()`). The presets
  never touch the widths. `dsd_opts_is_analog_family()` names the `-fA` receive family; the M17 encoder shares the analog
  front end but is not part of it. `dsd_opts_runs_m17_encoder()` names the encoder (issue #625). `-fZ` clears
  `analog_only` and leaves the digital frame flags at their defaults, so the RTL front end, the stream's live switches
  and app-control's profile publishes key on it. The encoder's monitor stays FM with de-emphasis on the WIDE channel,
  never CQPSK (`rtl_demod_init_for_mode()`, `demod_apply_cqpsk_defaults()`, `rtl_stream_runs_digital_family()`,
  `rtl_stream_apply_cqpsk_toggle()`, `rtl_stream_set_symbol_profile()`, `svc_publish_symbol_profile()`).
  `dsd_opts_analog_width_hz()` returns the explicit width for the active kind. Beside `dsd_opts_input_is_radio()`,
  `dsd_opts_input_is_pcm_audio()` names the inputs that deliver audio something else demodulated (Pulse, stdin, WAV,
  UDP, TCP; not an RTL-family, symbol-file or null input), `dsd_opts_rigctl_live()` a connected rigctl peer (`-U` on
  with a socket, socket 0 included) and `dsd_opts_rigctl_peer_demodulates()` both together, a peer whose demodulator
  DSD-neo hears (issue #621): the squelch's input kind, the rigctl passband rule, the engine's rigctl legs and
  app-control's analog width view and follow all key on these. `dsd_opts_tuned_freq_hz()` is the frequency the
  receiver is tuned to, as every frontend reads it (the terminal `FRQ:` field, the Qt `centerFreqHz`, the Android
  notification; issue #575): 0 off a radio input, rigctl on an audio input included (`rtlsdr_center_freq` holds a
  made-up 850 MHz until DSD-neo tunes the peer, the legacy rigctl `-Y` scan never writes it, and a change made on the
  peer is invisible), else the I/Q replay centre the decoder last read (`iq_replay_center_freq`) when there is one,
  else `rtlsdr_center_freq`; the replay read paths store that centre through `dsd_opts_note_iq_replay_center()`. The
  decoded readings (SiteSheet CC/VC, `dsd_app_vc_freq()`/`dsd_app_cc_freq()`, the trunk `Frequency:` lines) stay on
  their decoded values. `dsd_opts_input_is_iq_replay()` says whether the input in force is an I/Q replay (an RTL input
  with an `iqreplay` spec), from the input type and device string rather than `iq_replay_active`, which an input switch
  does not clear. `dsd_opts_trunk_vc_followed()` says whether the receiver sits on a voice channel a trunking grant sent
  it to (`trunk_enable` and `trunk_is_tuned` both 1): only then does `trunk_vc_freq[]` name the carrier being decoded,
  and a call observation whose message carries no frequency may stamp it (P25's target-matched `p25_vc_freq[]` differs;
  see Protocols). Tests: `CORE_OPTS_TUNED_FREQ`.
- Session predicates (EDACS and M17): a decoder-enabled flag is a capability, not a session, since Auto enables
  ProVoice and M17 beside every other decoder. `dsd_edacs_session(opts, state)` (`<dsd-neo/core/edacs_session.h>`,
  header-only so the runtime squelch policy reads it without linking core) is a selected EDACS session:
  `frame_provoice` and either an EDACS addressing mode selected (`state->ea_mode >= 0`: `-fh/-fH/-fe/-fE`, `-fh -fa`,
  an EDACS-to-Auto switch, a config's `edacs_ea`, the EA toggle) or ProVoice the one digital decoder (`-fp`, which
  selects no mode). Plain Auto (`ea_mode == -1`, where EDACS only prints the mode-selection hint) is none. Every preset
  but EDACS/ProVoice and Auto sets `ea_mode = -1` and `esk_mask = 0` (`dsd_stage_decode_mode_preset()`), so EDACS, then
  another mode, then Auto is plain Auto; a scan row's preset keeps the configured addressing
  (`scan_scope_apply_row_decoder()`), and a refused switch puts it back: an analog one through its rollback state
  (`ui_revert_analog_entry()`), a config apply through `ui_cfg_mode_extras`. Startup and
  configuration policy keys on it: the raw analog stream (`openAudioOutput(opts, state)`,
  `dsd_audio_ensure_digital_output(opts, state)`, `svc_udp_output_config()`, a same-family `[mode]` apply, a config
  that makes the session an EDACS one with no decode key, `cfg_commit_mode()`), the UDP
  analog socket on port + 2 and the `-8` override under `-T` (engine output setup), the EDACS squelch policy
  (`dsd_squelch_edacs_analog_voice()` and the squelch view, the engine's squelch notices) and the "Pulse Analog Output"
  line. The `-8` override is one helper, `dsd_edacs_session_trunked_monitor_off()` (trunking on, UDP output, an EDACS
  session: `-8` off), which engine output setup applies at start and the EA toggle, `DSD_APP_CMD_DECODE_MODE_SET`
  (`decode_mode_apply_value()`) and `DSD_APP_CMD_CONFIG_APPLY` apply when they make a running session EDACS (false to
  true); the toggle also ensures the raw sink then. TG lockout and Skip skip only an EDACS call (keyed on the call's
  protocol, not the session), never a P25, DMR or NXDN one. The capability flag stays on what lets an Auto session
  select EDACS: the EA/ESK toggles and the EDACS menu rows and toggle lines. `dsd_m17_only_session(opts)` (`opts.h`,
  over `dsd_opts_digital_decoder_count()`) is `frame_m17` with no other digital decoder (`-fz`): only it retries a
  refused TCP audio connect (engine setup) or a dropped TCP input (`dsd_symbol.c`) without end. Tests:
  `RUNTIME_CLI_PARSE`, `CORE_AUDIO_ENSURE_OUTPUT`, `ENGINE_RUN_SETUP`, `APP_COMMAND_QUEUE`, `APP_CONTROL_SQUELCH_VIEW`,
  `RUNTIME_SQUELCH`, `UI_MENU_SERVICES`, `UI_NCURSES_PRINTER_HELPERS`, `DSP_INPUT_INTERRUPT`.
- API note (runtime sink changes, `<dsd-neo/core/audio.h>`): `dsd_audio_ensure_analog_output()` and
  `dsd_audio_ensure_digital_output()` open the sink a new receive family writes to (the raw monitor stream; the digital
  voice stream, plus the raw stream for an EDACS session and `-8`) with the parameters `openAudioOutput()` uses, when
  the session plays to an unmuted local device and the sink is not open. With UDP output the raw sink is the analog
  socket on port + 2 (`udp_sockfdA`), opened through the `connect_analog` member of the runtime UDP audio hook table
  (`<dsd-neo/runtime/udp_audio_hooks.h>`, installed by the engine), muted or not: the mute toggle reopens local
  devices only, so a socket skipped while muted would stay closed. `udp_socket_blasterA()` skips an invalid socket.
  A new UDP target (`svc_udp_output_config()`) closes an open analog socket, which still sends to the old host and
  port, and reopens it for the new target at once; with none open it opens one only when the current mode writes to
  it. Idempotent, decoder thread only; a failure is logged once and leaves that family silent.
  `DSD_APP_CMD_DECODE_MODE_SET` and the RadioReference import (both through `decode_mode_apply_value()`) call them,
  and so does `DSD_APP_CMD_CONFIG_APPLY` when its `[mode]` moves the session between the analog and digital families
  or lands on a digital mode that writes raw audio (an EDACS session, `-8`). Input Monitor turned on
  (`DSD_APP_CMD_INPUT_MONITOR_TOGGLE`) ensures the analog sink itself and stays off, with a toast, when it cannot
  open. Which streams open: `openAudioOutput()` and `dsd_audio_ensure_digital_output()` open the raw stream when it
  has a writer (`dsd_audio_raw_writer()`: `-8` or an EDACS session), while
  `dsd_audio_reconfigure_output_for_input_policy()`, which closes and reopens for a new async policy, reopens the raw
  stream when it was open (or `-8` is on), so it keeps an EDACS session's without reading the state. Tests:
  `CORE_AUDIO_ENSURE_OUTPUT`, `APP_COMMAND_QUEUE`, `APP_CONTROL_ACTIONS`, `APP_CONTROL_RR_APPLY`, `UI_MENU_SERVICES`.
- Runtime input switch (issue #634, `<dsd-neo/core/audio_input_switch.h>`, `src/core/audio/dsd_audio_input_switch.c`):
  `dsd_audio_switch_input()` replaces the running PCM or symbol input with the one a request names (a WAV or headerless
  PCM file, a float symbol stream, a symbol capture, UDP, a connected TCP socket, a Pulse device), on the decoder
  thread. The new input opens before the old one closes: the old file and its `SF_INFO`, symbol file, TCP context and
  socket and Pulse stream are taken out of the options and put back untouched when the open fails
  (`DSD_AUDIO_INPUT_KEPT`). A UDP input cannot be taken out, since its receive thread uses the options, so a switch to
  UDP stops the running one first (it may hold the port) and binds it again when the new one does not
  (`DSD_AUDIO_INPUT_RESTARTED`, a new stream on the old input, or `DSD_AUDIO_INPUT_LOST`). A failure never writes the
  input-failure latch. A switch that opens its input closes every PCM and symbol input the options hold, never a radio
  stream running behind; names the input as `-i` and a saved config spell it (`udp:<bind>:<port>`, `tcp:<host>:<port>`,
  `pulse[:<dev>]`, the path) and keeps a radio spec it replaced in `dsd_opts::radio_in_dev`; opens the next PCM input at
  the raw rate a WAV header replaced (`dsd_opts::wav_header_replaced_rate`, which `openAudioInDevice()` and the config
  file apply keep too); resets the PCM input state, so `pcm_input_generation` moves; and sets the symbol timing for the
  new rate, rescaled between PCM inputs and set afresh from the active hunt profile when the old input was a radio,
  whose timing is in no PCM rate's units. Files must be regular files: a pipe or device would block the decoder thread
  in its open, so only startup opens one. `dsd_audio_path_input_kind()` is `-i`'s extension rule. The switch lives in
  its own object file so a link-time wrap of `openAudioInput()` reaches it. Tests: `CORE_AUDIO_INPUT_SWITCH`,
  `CORE_AUDIO_OPEN_INPUT`.
- Received identity record (`dsd_state::rx_id`, `<dsd-neo/core/received_identity.h>`,
  `src/core/util/received_identity.c`): the system identity the carrier being decoded sent, kept apart from the decoder
  state that decoding needs (the Phase 2 seed, control channel anchors and return hints, a `--tetra-net` preset, values
  another carrier left), so every frontend shows the same identity and none of another carrier's. Protocol code fills it
  through `dsd_rxid_note_*()` only where a message passed its own check, with that message's own values (Protocols,
  "Received identity note points"); content accepted only because `-F` or RAS relaxed a check keeps decoding and
  following but is never noted. Each part has a valid and a fresh bit. Threading: the decoder thread owns the record
  and is its only writer; wherever the P25 watchdog's tick can run beside it, every write is made under the P25 SM tick
  guard (a frame's notes in `processFrame()`, the no-carrier pass's step, the trunk-scan switches and rollbacks, and the
  user-move, DMR reset and decoder settings commands, `ui_rxid_guard_enter()`, unless `apply_cmd()` holds it), because
  the watchdog's tick reads the record under that guard (the calls its release ends keep it as
  `dsd_call_snapshot::ended_rx_id`). The watchdog writes nothing of it: an accepted retune it makes through the trunk
  tuning hooks only requests the access code's drop (`dsd_rxid_request_trunk_tune()`, the guard-held
  `dsd_state::rx_id_trunk_tune_pending`, outside every snapshot copy range), which the decoder thread applies
  (`dsd_rxid_apply_trunk_tune()`). The decoder thread's own retunes apply the drop at once, under the guard: inside a
  frame dispatch (`dsd_state::frame_dispatch_active`, set only under `processFrame()`'s hold, where no watchdog tick
  runs) the request does, and elsewhere the engine's trunk tuning (`dsd_engine_rxid_trunk_tune()`) does on the thread
  `dsd_engine_run_with_lifecycle()` marks as the decoder's (`dsd_engine_set_decoder_thread()`), in the hold it has
  (`p25_sm_tick_guard_held_by_caller()`, a per-thread mark the guard keeps) or one it takes for the drop. On any other
  thread the request only waits. Frontend publication reads the record on the decoder thread without the guard.
  Lifetime, all on the decoder thread:
  `dsd_engine_carrier_boundary()` resets it (every boundary kind); `trunk_scan_switch_to()` resets it on every target
  switch, and no target snapshot holds one (a revisited target is last known, not received), while a rollback after
  every retune of an advance failed puts back the record the advance started with (the coordinator's `rollback_rx_id`,
  taken with the scratch snapshot, again after a forced release); `processFrame()` calls `dsd_rxid_note_sync()` before
  each dispatched frame, so a frame of another protocol family (YSF, D-STAR, X2-TDMA included, which note nothing) drops
  the record; the no-carrier pass's last step (`dsd_engine_no_carrier_identity_stale()`, under the guard; a pass that
  finds it taken leaves the step to the next, as it leaves the playout drain) marks every part stale and keeps the
  values; every accepted trunk retune, a voice channel grant (`dsd_engine_update_vc_tune_state()`) or a control channel
  return or hunt step (`dsd_engine_trunk_tune_to_cc_request()`), requests `dsd_rxid_trunk_tune()`, which drops the
  access code and keeps the system (the protocols and the P25 watchdog reach both through the trunk tuning hooks, so a
  refused tune changes nothing). A retune the decoder thread makes drops the code where it is made: one a frame's
  handler makes (a grant, a Capacity Plus or XPT rest channel a CACH names, a DMR state machine tick in the burst
  loop), so a handler that goes on within the same dispatch, as DMR's burst loop publishes after every burst and
  decodes the bursts the carrier it reached sends, never shows the carrier left's code, and the reached carrier's
  checked code is its first, neither erased by a later drop nor read as another system that drops the one kept; and
  one made between frames (the frame sync's no-sync release, `p25_sm_try_tick()` from the frame sync or the decode
  loop, the startup hunts and TETRA ticks, the no-carrier pass, a command), so no publication after it shows the code.
  The watchdog's requests wait, and the decoder applies them under the guard at the start of `processFrame()` (before
  the frame's first note), after the trunking ticks of the synced-frame dispatch, in the no-carrier step before the
  record goes stale, and before a trunk-scan advance or forced release keeps `rollback_rx_id`; a reset forgets one, and
  a rollback's restore clears it (the switches undone left those carriers). The control channel frequency part
  (`cc_freq_hz`) is the tuner's frequency (`dsd_opts_tuned_freq_hz()`, so none off a radio input, a rigctl peer on audio
  input included) at a checked message that confirms this carrier as the control channel, noted before the message's
  handlers can retune. A note made while a dispatched frame is lost (`frame_dispatch_active` and
  `dsd_state_frame_read_lost()` against `frame_dispatch_carrier_seq`: a carrier boundary moved the count while the frame
  was read, or an input interruption cut it) is refused, so a frame split by an I/Q replay retune never notes into the
  record the boundary reset, whichever decoder's own carrier check comes later. That
  gate starts at dispatch; what a frame reads back from before its sync (DMR's 66 buffered dibits, X2-TDMA's 144, a
  TETRA burst's block 1 in the hunt's history) was read earlier, so the boundary and a trunk-scan target switch also
  mark the symbol clock (`dsd_state_note_carrier_change()`; a change that runs while `getSymbol()` reads a symbol,
  `dsd_state::symbol_read_active`, counts that split symbol as the carrier left's, one between reads none), and each
  protocol's matcher in `getFrameSync()` waits until what its frame reads back (`k_frame_sync_tries`, then the matched
  burst's own, `frame_sync_burst_settling()`: its sync window; DMR 90, X2-TDMA data 90 and voice 144, TETRA control 57,
  synchronisation 79 and normal bursts 126, EDACS 48) is the new carrier's (`dsd_state_carrier_settling()`;
  `FRAME_SYNC_DMR_WAV_RATE`); a protocol that reads nothing before its sync (P25 Phase 1, 24) takes the new carrier's
  first sync at once. Another value of a valid access code on the same carrier is another system sharing it (co-channel
  DMR/dPMR colour codes, P25 NACs, NXDN RANs): the family's other parts and the control channel frequency go (an M17
  CAN, per transmission, drops nothing; a trunk tune drops the code first, so a followed channel's is no change), and a
  note carrying both notes the code first. An IDAS SCCH site ID and a Type-D SRV_INFO that disagree keep the newer;
  another Type-D system drops the SCCH area and site. NXDN Type-C (a location ID) and IDAS Type-D parts exclude each
  other: a checked note of one drops the other kind's parts and the control channel (a RAN is Type-C's: a Type-D RTCH2
  carries SCCH, FACCH1, UDCH2 and FACCH3, NXDN TS 1-E 4.2.3, none of which carries one; so a Type-D note drops a
  location and a RAN, and a RAN drops the IDAS parts). So do the DMR trunking systems (`dsd_rxid_dmr_trunk`: Tier III,
  Capacity Plus, Connect Plus, Hytera XPT), named by a brand, a site text prefix, a rest LSN, a system code or a free
  LCN; the colour code stays. The TETRA cell keeps its colour (`tetra_colour`) apart from the displayed access code, so
  after a trunk tune another colour is still another cell; a SYSINFO of another cell with the same network and colour
  (`tetra_bsch_sysinfo_cell()`) drops the last cell's area and control channel (`dsd_rxid_note_tetra_other_cell()`). A
  note that invalidates parts because it names another system, site, cell or subtype (another access code on the
  carrier, another P25 network or site, a Capacity Plus site change, which also drops the old site's rest LSN and free
  LCN, an IDAS area) moves the record's epoch (`rxid_invalidate()`; another access code moves it even when the record
  held nothing else), so a call's history row, which takes the record only under the epoch it was heard on, keeps that
  identity and is never mixed with the next one. The DMR vendor (`dsd_rxid_note_dmr_vendor()`, the feature set ID of a
  genuinely checked LC or CSBK) is a part of its own, apart from the trunking brand; the terminal header shows its name
  (Hytera, Tait, Motorola). The DMR RAS part (`DSD_RXID_DMR_RAS`, `dsd_rxid_note_dmr_ras()`) says the carrier uses
  Motorola RAS: the DMR decoder notes it once two RAS signature bursts on the carrier confirmed it (`dmr_ras.c`, under
  DMR), and again at every signature after. It is a property of the codewords received, never a decoded field, so the
  rule above that RAS-accepted content is never noted is unchanged, and noting or dropping it moves no epoch. The
  evidence is the DMR decoder's (`dsd_state::dmr_ras_signatures`, outside the record), and one reset,
  `dmr_ras_reset()`, clears it with the part (`dsd_rxid_forget_dmr_ras()`), always where the record may be written:
  `dsd_engine_carrier_boundary()` beside `dsd_rxid_reset()`; every applied trunk tune, which the record leaves to the
  engine (`dsd_rxid_apply_trunk_tune()` returns 1 when it applied a drop, `dsd_rxid_request_trunk_tune()` when it
  applied one at once inside a dispatch): `dsd_engine_rxid_trunk_tune()` on the decoder thread or inside a dispatch,
  `processFrame()`'s apply before the frame, the apply after the synced-frame trunking ticks, and the trunk-scan
  advance, so a watchdog retune resets it when the decoder thread applies the drop, never on the watchdog's thread;
  `trunk_scan_switch_to()` (which also covers the forced release's own apply) and its rollback, which restores the
  record without the evidence and so drops the part; a colour-code change on the carrier (the BS confidence relock,
  the MS/direct slot type and embedded colour code, `dmr_ras_note_color_code()`); the DMR reset command; a user move
  (`ui_user_move_forgets_identity()`); and the guarded no-carrier step (`dsd_engine_no_carrier_identity_stale()`), so
  the next transmission proves RAS again. A frame
  of another family drops the part with the record and keeps the evidence (the same carrier): the next signature notes
  it again. `dsd_engine_forget_carrier_codes()` does not reset it: the voice-channel tune that calls it can run on the
  watchdog's thread, and the trunk tune's apply covers that tune. A tune the user asked for while trunking, which keeps
  the decoder's trunking state and so is no carrier boundary, forgets the record too (D4: any user move), after the
  calls it ended took theirs: a frequency entry that selects a P25 control channel (`ui_cmd_handle_p25_cc_selection()`,
  accepted only), a Return to CC, and a channel or candidate cycle (`ui_user_move_forgets_identity()`, which also marks
  the carrier change for the frame sync). A trunking follow keeps it, a lockout's or skip's return to the control
  channel included (the decoder leaves the call as at its end); every other user retune (a frequency entry on another
  protocol, tap-to-tune, a `-Y` step, a trunk-scan target, a signal-search visit) is a carrier boundary. Commands that
  forget or reinterpret a site forget it in the record too: the DMR reset (`DSD_APP_CMD_DMR_RESET`, terminal `D`) drops
  the DMR trunking parts (keeping the colour code the reset keeps) and the NXDN location, IDAS parts and RAN
  (`dsd_rxid_forget_system()`); a decode mode change, once the calls it ended froze their identity and once a switch
  onto the analog monitor the front end has queued is taken (`ui_commit_mode_identity()`: refused where it lands, the
  session goes back with its identity), an `[edacs]` addressing mode a config apply sets without a `[mode]`, a
  RadioReference import's EDACS variant and the EDACS addressing mode toggle (after its calls ended, which keep their
  site) run `dsd_rxid_note_decoder_settings()`, which drops a record of a protocol the settings no longer decode and an
  EDACS record read under the other addressing mode (`edacs_ea_mode`; an EDACS note under the other mode drops it as
  well). The no-carrier pass, a SIM no-carrier, the ESK toggle, inversion and modulation toggles, the P25 Phase 2 seed,
  the tuner release and the scan resets change no received identity: the carrier and the reading of its messages stay (a
  carrier boundary resets the record anyway). Tests: `CORE_RECEIVED_IDENTITY`, `ENGINE_NO_CARRIER_RESET`,
  `ENGINE_TRUNK_SCAN`, `ENGINE_TRUNK_RETUNE_REGRESSION`, `ENGINE_PROTOCOL_DISPATCH`, `DMR_RAS_DETECT` (the RAS part's
  evidence and its reset points with those five, `DMR_DATA_SYNC`, `DMR_MS_DATA` and `APP_COMMAND_QUEUE`),
  `ENGINE_RXID_WATCHDOG_RACE` (the watchdog's retunes and call ends beside the decoder's notes, no-carrier step and
  reads, race-free under the `tsan-debug` preset, and the decoder thread's grant and frame sync release between frames),
  `DMR_BS_SYNC_TIMES` (a rest channel's retune in one burst, then the reached carrier's bursts in the same loop), and
  end to end `ENGINE_REPLAY_IDENTITY` (a trunked P25 control channel replayed, then a conventional carrier of another
  system).
- Build files: `src/core/CMakeLists.txt`

## Runtime

- Path: `src/runtime`, `include/dsd-neo/runtime`
- Target: `dsd-neo_runtime`
- Responsibilities:
  - Frontend-neutral Signal search records, rational range validation, the aggregate
    configuration check engine and frontends share (`dsd_signal_search_validate_config()`)
    and conservative DC/edge-safe capture planning (`signal_search.h/.c`). `signal_search_hooks.h/.c`
    carries validated protocol evidence, CRC/FEC-checked site notes
    (`dsd_signal_search_note_site()`), call notes (`dsd_signal_search_note_call()`: a slot whose
    canonical call record a protocol just published after its check; the engine reads the record)
    and the decoder-thread active query, with no-op defaults. The checked site record
    (`dsd_signal_search_site`, `dsd_signal_search_site_apply()`,
    `dsd_signal_search_site_format()`) is runtime's: a P25 site note joins only a record with
    no SYSID or the same one, and a network note naming another WACN or SYSID, another protocol's
    note, or a DMR or NXDN note replaces it whole. P25 reports evidence from a control block whose CRC passes
    (`p25_sm_note_cc_activity()`) and from conventional calls whose identity resolves, never
    from a NID or a voice Reed-Solomon word alone, which the list and soft decoders accept on
    a large share of random input (`P25_P1_TSBK_SEQUENCE`; limit in docs/signal-search.md).
    It does not own radio I/O, persistence or Qt state.
  - Readout text (`include/dsd-neo/runtime/number_text.h`, issue #669): `dsd_number_text()` formats with the platform
    printf and drops the sign of a text whose digits are all zero (`-0.0` → `0.0`, any radix; `nan`/`inf` untouched),
    so non-zero readings keep native rounding and the rule holds under fast-math; `DSD_NUMBER_TEXT_SATURATE` caps a
    reading at its width (`>999`, `<-99`). `dsd_count_text()` is a count in six places (`999999`, then `1.0M`...).
    Runtime, core, app-control, the terminal and Qt's C paths print signed readings through it; QML has the same rule
    as `Util.fixed()`. Test: `RUNTIME_NUMBER_TEXT`.
  - Config system (schema, expansion, user config), logging, memory helpers, rings, worker pools, RT scheduling
    - In-memory user configs: `dsd_user_config_init()` resets a `dsdneoUserConfig` to the state the INI loader starts
      every file from (not all zero: several keys default to on), `dsd_user_config_apply_section()` marks a section
      present as a `[section]` header does (an empty `[analog]` acts), and `dsd_user_config_apply_key()` applies one
      `[section] key = value` through the loader's own mapper (the control API's `config_apply`)
  - CLI parsing and interactive/bootstrap helpers (`include/dsd-neo/runtime/cli.h`); the control API options and
    their `DSD_NEO_API_*` fallbacks are resolved at parse time, token file included, so a bad value is an argument
    error. A token can read like an option (`--print-config`), so every pass over the raw argv -- bootstrap's
    pre-parse and inherited-trunk-scan checks, the one-shot check, compaction -- steps over the value of an option
    `dsd_cli_option_takes_free_value()` names; a new raw pass must do the same
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
    analog tap's input-pause deadline and backlog skip, the input-level snapshot's measurement stamp (`updated_ms`,
    real-time monotonic, and the staleness it ages into: `dsd_input_level_is_stale()`),
    RadioReference dates and perf, and in `src/core`, `src/app_control` and `src/ui` the `ui_msg_expire` (the
    input-level warning's toast included) and terminal status toasts, the audible-audio stamp and its age, UI frame and
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
    setup writes its first record (`dsd_engine_decode_clock_enter_replay()`: the sidecar's `capture_started_utc`, or a
    WAV recording's start time from its `auxi` chunk or file name (empty when it records none, which logs an INFO line
    and starts at 2000-01-01T00:00:00Z, issue #701), parsed by `dsd_iq_replay_parse_utc_seconds()`, then
    `dsd_state_rebase_decode_timestamps()` in `<dsd-neo/core/init.h>`, the
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
    restarts runs on SYSTEM, which it logs once at that leave when the input is still the replay; a run-time re-plan's
    stop keeps REPLAY instead, as the replay it reopens resumes the media time from the checkpoint's
    `timeline_samples`, so decode time steps forward over the few ms the resume skips and never back, issue #701), and
    when the input
    moves away from it (`ui_input_left()` in `app_command_queue.c`: an input switch, a stop of playback, a config
    apply that moves the input). Before the switch the leave ends each slot's call (`DSD_CALL_END_EXPLICIT`) and syncs
    the slot. The call's row, and the per-call recording and rdio-scanner sidecar a commit closes, are then stamped on
    the capture's clock (#673), and each WAV reopens empty. A call the run's end already finalized has nothing left to
    end. Decode-mono time goes on from the capture
    time the replay reached (a SYSTEM offset over the platform monotonic clock, set only by leaving REPLAY), so the
    replay's stamps keep ageing; wall time returns to real time. Each replay sample moves media time to its own capture
    time as it reaches symbol processing (`dsd_decode_clock_batch_media_ns()` over the batch tag's span): the symbol
    cache's pop, and the one-sample readers (the analog monitor, M17, EDACS analog) through
    `dsd_rtl_stream_metrics_hook_replay_advance_decode_clock()`, which also reports the centre the batch was captured
    on through its optional `center_hz_out` (issue #575; see IO, "I/Q replay tuned frequency"). Unit tests step
    decode time on the TEST source (`docs/testing.md`, "Decode time in unit tests"). Tests: `RUNTIME_DECODE_CLOCK`
    (the sources, the anchor floor and ceiling, `dsd_decode_clock_batch_media_ns()`, the leave's continuity),
    `RTL_SYMBOL_REPLAY_CLOCK`, `ENGINE_REPLAY_DECODE_CLOCK`, and the leave in `ENGINE_NO_CARRIER_RESET` and
    `APP_COMMAND_QUEUE`.
  - Analog channel contract shared by the CLI, config, app commands, scan rows and the demodulator
    (`include/dsd-neo/runtime/analog_channel.h`, `src/runtime/analog_channel.c`): `dsd_analog_demod` (FM = 0,
    AM = 1), `dsd_rx_family`, per-kind width ranges and defaults (NFM 8000–25000 Hz, default 16000; AM
    5000–20000 Hz, default 6000), the strict whole-Hz parser, `dsd_analog_width_check()` and the kHz formatter. A
    width is accepted only when it is in range, width/2 + 600 Hz stays within 0.45 × the DSP rate, and the Blackman
    tap count fits the 288-tap analog capacity; the refusal names the width, the rate, the largest width that rate
    fits and the RTL DSP bandwidths that would fit (`dsd_analog_width_fitting_rtl_bandwidths()`, which the short
    refusals reuse). It holds the one list of selectable DSP bandwidths (4, 6, 8, 12, 16, 24, 48 and 72 kHz; 72 is
    TETRA's): `dsd_analog_rtl_dsp_bw_is_selectable()`, `_or_default()` (the 48 kHz default,
    `DSD_ANALOG_RTL_DSP_BW_DEFAULT_KHZ`), `_nearest()` and `_list()`, which the input specs, the config schema, apply and
    validation, the services, the terminal menu, the interactive setup and the Qt session read rather than a copy;
    `dsd_analog_airspy_dsp_bw_is_selectable()` caps it at `DSD_ANALOG_AIRSPY_DSP_BW_MAX_KHZ` (48), since no Airspy
    sample rate halves to 72 kHz. `dsd_analog_width_check_at()` words a rate refusal for what sets the rate
    (`dsd_analog_rate_source`, fix text from `dsd_analog_width_rate_fix()`): the RTL DSP bandwidth, a SoapySDR or Airspy
    device (whose capture rate is halved down to the first rate at or above the DSP bandwidth: raise the DSP bandwidth
    or narrow the width) or an I/Q replay's capture, whose rate plan follows the DSP bandwidth (the DSP bandwidths that
    fit, as for the RTL DSP bandwidth, issue #701); where a device's rate filters no width of the kind it never names
    narrowing, and for NFM names leaving the width unset. `dsd_analog_channel_lpf_off_check()` is the
    `DSD_NEO_CHANNEL_LPF=0` rule (an explicit width, or the AM default, cannot run), taking that state as an argument.
    It is pure integer arithmetic so callers need not link the DSP;
    `src/dsp/demod_pipeline.cpp` static-asserts its design constants against the ones here. Tests:
    `RUNTIME_ANALOG_CHANNEL`, `DSP_CHANNEL_FILTERS` (validator and design agree across a width/rate grid; the
    analog design gates on `dsd_analog_width_realizable()` itself).
  - The rigctl passband rule (issues #526, #621; `include/dsd-neo/runtime/rigctl_passband.h`,
    `src/runtime/rigctl_passband.c`): the demodulator and passband a rigctl peer that demodulates audio input is asked
    for, pure and NULL-safe, with liveness the caller's to check. `dsd_rigctl_passband_of()` keys on the analog
    demodulator in force: on PCM audio input (`dsd_opts_input_is_pcm_audio()`) under the analog family it is
    `dsd_rigctl_passband_for_kind()`, the row's own width of that kind (ROW, from the row's options, since the width in
    force cannot tell it from the configured one), else the configured width of that kind in force (CONFIGURED), else
    for AM the 6 kHz default (AM_DEFAULT) and for FM `-B` standing in for an unset NFM width (SETMOD_BW), else the
    peer's own passband at 0 (PEER_OWN); anything else (a digital mode, the M17 encoder, an RTL-family, symbol-file or
    null input) is FOLLOW, FM at `-B`, as every tune asked before the peer demodulated. `dsd_rigctl_passband_is_width()`
    (ROW, CONFIGURED, AM_DEFAULT) is a strict request, whose refusal fails a scan tune;
    `dsd_rigctl_passband_sets_passband()` adds SETMOD_BW, a passband this client sets, and
    `dsd_rigctl_passband_captures()` adds FOLLOW at `-B` on PCM audio input, where a digital mode is heard through the
    peer's FM passband: every request it names goes through `SetScanRowModulation()` so the peer's own passband is read
    first and a later return to it sends the reading rather than `M NFM 0` (FOLLOW on any other input only follows the
    frequency); `dsd_rigctl_passband_equal()` counts the source, so entering the monitor from FOLLOW is a change even
    where the wire request matches. The engine's scan tune, live apply, leave restore, start and reconnect ask,
    legacy `-Y` step and `io_control_set_freq()` (IO, below), and app-control's follow and analog width view, all take
    their request from it. Test: `RUNTIME_RIGCTL_PASSBAND` (also `dsd_scan_mode_rigctl_request_is_session()` over a real
    scope).
  - The configured NFM width (issue #525): `--nfm-bandwidth-hz` (`src/runtime/cli/args.c`; `compact.c` consumes its
    value before getopt; a warning on PCM input, or with rigctl `-U` a notice that it is the FM passband the peer is
    asked for in place of `-B`, issue #621) and the `[analog]` INI section (`has_analog`,
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
    levels, the media span, output count and first sample's index the decode clock runs on, and the channel centre
    the batch was captured on, issue #575, and the stream gaps recorded before it, `boundary_seq`, issue #518; decoder
    thread only, while the stream is open; issue #572), and the stream's discontinuity count (`discontinuity_seq`,
    `rtl_stream_discontinuity_seq()`, issue #518; `dsd_rtl_stream_metrics_hook_read_discontinuity_seq()` gives the count
    the last read's samples carry: the batch's under a replay, the stream's read after the read live); the engine
    installs `rtl_stream_request_analog_profile()`, `rtl_stream_request_digital_family_landing()`,
    `rtl_stream_get_analog_profile()`, `rtl_stream_analog_family_active()`, `rtl_stream_family_landing_after_pending()`,
    `rtl_stream_output_rate_for_family()` and `rtl_stream_get_replay_batch()` behind them. It also hands the
    demodulator a whole squelch setting (`set_channel_squelch_setting`, `rtl_stream_set_channel_squelch_setting()`;
    issue #518 follow-up), which a scan scope's push uses for every setting, levels included; a table without it falls
    back to the level call, with an AUTO setting off. Tests: `RUNTIME_RTL_STREAM_METRICS_HOOKS`,
    `ENGINE_RTL_STREAM_METRICS_HOOKS_INSTALL`.
  - Stream notices and reads with no stream (issue #518): `include/dsd-neo/runtime/stream_notice_hooks.h` carries what
    the input stream got over by itself (`DSD_STREAM_NOTICE_USB_RECOVERED`), posted from the stream's controller with
    `dsd_runtime_stream_notice_post()` into the sink app-control installs at session start
    (`dsd_app_stream_notice_post()`: one slot, which the next `dsd_app_drain_cmds()` shows as a toast; NULL at session
    end); with no sink it is dropped, and nothing is latched in the input failure. A failed notice-mutex initialization
    leaves the sink disabled without accessing an uninitialized lock (`RUNTIME_STREAM_NOTICE_INIT_FAILURE`).
    `rtl_stream_io_hooks.h` gives a live read two results beside a count and -1: `DSD_RTL_READ_YIELD` (no stream and a command queued) and, for a
    reader that asks through `read_ex2` (`want_no_stream`), `DSD_RTL_READ_NO_STREAM` (see IO, "Reads while the device
    has no stream"). Tests: `APP_COMMAND_QUEUE`, `APP_CONTROL_TELEMETRY_HOOKS_INSTALL`, `ENGINE_IO_HOOKS_INSTALL`.
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
  - Auto is the default decoder set: `initOpts()` (`src/core/util/dsd_init.c`) installs exactly what
    `decode_mode_apply_auto()` sets under the CLI profile (every digital decoder but TETRA, C4FM, the stereo layout),
    so a session that names no `-f` runs `-fa`, `dsd_infer_decode_mode_preset_exact()` answers AUTO for it, a config
    without a `decode` key keeps it and autosave writes `decode = "auto"` (`RUNTIME_CLI_PARSE`, `RUNTIME_CONFIG_APPLY`).
    Entry points that name no `-f` inherit it: `-mq` alone (a RadioReference simulcast P25 row) runs Auto under the
    QPSK lock, whose timing-matched SPS hunt never reaches the 2400/9600 profiles (`RUNTIME_CLI_PARSE`,
    `FRAME_SYNC_INTERNAL_HELPERS`); the M17 encoders (`-fZ/-fB/-fP/-fU`) run their own loops, `-fZ` on the fixed WIDE
    front end (`IO_RTL_DEMOD_CONFIG`); signal search applies Auto itself, and scan rows and trunk-scan targets restore
    the set they captured.
  - Decode presets (`include/dsd-neo/runtime/decode_mode.h`, `src/runtime/decode_mode.c`): the `-f` selector map is
    a table (`k_cli_presets`), and so are the presets (`k_decode_mode_presets`): `dsd_apply_decode_mode_preset()` looks
    the mode up before it changes anything, so a mode no preset implements (`DSDCFG_MODE_UNSET`, which a config's
    `[mode]` without a `decode` key and DECODE_MODE_SET 0 pass) changes nothing, a TETRA session's decoder and state
    included. A preset sets its own modulation only when no `-m` lock is in force (`decode_mode_preset_modulation()`;
    the hand-rolled `-fp/-fh/-fH/-fe/-fE` branches in `args.c` through `cli_preset_modulation()`), so a lock holds on
    either side of the `-f` selector (`-mq -f1` is `-f1 -mq`; `RUNTIME_CLI_PARSE`,
    `DECODE_IQ_P25P1_CQPSK_SIMULCAST_CC_LOCK_FIRST`/`_LAST`); the mid-session path releases the lock first, and a
    config's `[mode] demod` applies after the preset, while without one the lock stands (`APP_COMMAND_QUEUE`). A
    transition that can still be put back stages its preset (`dsd_stage_decode_mode_preset()`: every
    setting, `frame_tetra` off for a mode other than TETRA, but the TETRA state extension, `tetra_sync` and `tetra_pub`
    left) and commits once nothing can put it back (`dsd_decode_mode_commit_tetra()`: TETRA is left when the settings
    in force run no TETRA decoder); `dsd_apply_decode_mode_preset()` is both at once. DECODE_MODE_SET commits after a
    switch onto the monitor the front end refused at once was put back, and a runtime config apply
    (`dsd_stage_user_config_to_opts()`) after its reopen and receive-family rollbacks, so a TETRA session they put back
    is whole (`APP_COMMAND_QUEUE`). A switch from TETRA onto the monitor or AM that a running RTL stream queued (under a
    scan row, the one the row's resume makes) commits once it settles (`ui_commit_mode_change()` in
    `app_command_queue.c`): while the switch is armed with the settings it left still in force, a refusal where its
    request lands can still put the session back (`ui_revert_analog_entry()`), so the commit waits for the front end to
    take the request, refuse it, or the switch to be dropped (`ui_analog_entry_settled()`); a command that moves the
    settings on to another mode commits at once. A refusal that puts a TETRA session back puts back as well the follow
    state its trunking state machine shares, which ending the calls cleared (`reset_call_tracking()`: `trunk_is_tuned`
    and the voice channel frequencies, as the first switch found them), while the machine still follows the call
    (`ui_restore_tetra_follow()`; a carrier boundary meanwhile abandoned it), so its next tick does not drop the call as
    one another owner released, the receiver left on its traffic channel. `dsd_apply_decode_mode_symbol_timing()` refuses a mode no preset
    implements the same way (it has no preset timing), and the config paths that call it for a file input
    (`apply_file_input_symbol_timing()` in `config_user.cpp`, `cfg_file_runtime_apply_*()` in `app_command_queue.c`)
    then rescale or keep the session's own timing
    (`RUNTIME_DECODE_MODE`, `RUNTIME_CONFIG_USER`, `APP_COMMAND_QUEUE`). AM (`DSDCFG_MODE_AM` = 16, `-fM`, issue #524) is the last preset: the analog monitor
    with `analog_demod` AM, read back as AM by `dsd_infer_decode_mode_preset()`. AM needs an I/Q radio input, one rule
    for two phases: before the engine opens the input, `dsd_decode_mode_input_spec_is_iq()` reads the spec (an rtl,
    rtltcp, soapy, airspy or iqreplay one, or the RTL input type `--iq-replay` sets while the options are parsed) for
    the CLI (which stops on `-fM` with a PCM input, and falls a config's `decode = am` back to Analog with autosave off
    and a toast saying so, `dsd_decode_mode_keep_saved_am()`, which a runtime config apply of one shares) and the
    setup wizard (which asks it about the input it just configured, and also holds the AM width to the DSP bandwidth
    it just read, as the engine's pre-open check does: `interactive_am_refused()` in
    `src/runtime/bootstrap/interactive.c`); once an input is
    open, `dsd_decode_mode_input_is_iq()` and `dsd_decode_mode_runs_on_input()` go by the input type alone
    (`dsd_opts_input_is_radio()`): an input switch names the input it opens (`tcp:<host>:<port>` and the like, issue
    #634), but a replay session switched to Pulse keeps its replay request. All of them say `DSD_DECODE_MODE_AM_NEEDS_IQ_TEXT`.
    `--am-bandwidth-hz` and `[analog] am_bandwidth_hz` carry the AM width (whole Hz, 5000..20000, 0 = the 6000
    default, saved only when explicit; the section is always saved). Tests: `RUNTIME_DECODE_MODE`,
    `RUNTIME_CLI_PARSE`, `RUNTIME_CONFIG_USER`, `CONFIG_VALIDATION`, `CONFIG_TEMPLATE`,
    `RUNTIME_BOOTSTRAP_INTERACTIVE`.
    TETRA (`DSDCFG_MODE_TETRA` = 17, `-fT`) follows AM and shares its input rule, with one more input it runs on: a
    symbol capture (`.bin`, `.raw` or `.sym`), which carries the symbols an I/Q input's CQPSK demodulator recovered
    (`dsd_decode_mode_runs_on_input()`, `dsd_decode_mode_runs_on_input_spec()`, `DSD_DECODE_MODE_TETRA_NEEDS_IQ_TEXT`
    through `dsd_decode_mode_needs_iq_text()`). The CLI stops on `-fT` with a PCM input, a config's `decode = tetra`
    there keeps the mode with a warning, DECODE_MODE_SET refuses it, and the terminal picker labels it `TETRA (needs an
    I/Q radio input)`. Every preset other than TETRA leaves the TETRA decoder (`dsd_decode_mode_leave_tetra()`:
    `frame_tetra` off, the TETRA state extension, `tetra_sync` and `tetra_pub` dropped), and so do the `-f` selectors
    the CLI handles itself (`-fp`, `-fh`, `-fH`, `-fe`, `-fE`, `-fZ`, `-fB`, `-fP`, `-fU`), so the last `-f` selector
    replaces TETRA rather than running beside it. A typed `-Y` row leaves TETRA when it commits (`dsd_scan_mode_enter()`,
    `dsd_scan_mode_resume()`), not when `dsd_scan_mode_prepare()` stages it for its tune, so a row whose preparation or
    tune fails before the commit goes back to the TETRA session whole (`RUNTIME_SCAN_MODE`, `ENGINE_CHANNEL_SCAN`);
    `dsd_scan_settings` carries `frame_tetra` with the other frame flags, so an inherited row and the end of the scan
    bring the configured TETRA decoder back (`RUNTIME_SCAN_MODE`). TETRA follows calls of a trunked (TMO) network and
    decodes direct mode (DMO), which is not trunked, so every place that offers trunking offers it for TETRA with no
    answer assumed: the setup wizard asks its trunking question for TETRA on an RTL-SDR or rtl_tcp source as for the
    other trunk-capable entries, No by default (`interactive_mode_supports_trunk()`, `RUNTIME_BOOTSTRAP_INTERACTIVE`),
    the Qt wizard's Trunking switch is there for every chip and the TETRA chip does not suggest it (`Util.js`
    `suggestsTrunking()`), and the terminal menu's Trunking row, `[trunking] enabled` and `-T` take it whatever the
    mode. On the RTL front end its 18 ksym/s profile runs CQPSK timing on the WIDE
    channel filter (`dsd_rtl_channel_profile_for()`), and a retune keeps the active profile with its filter
    (`rtl_demod_maybe_refresh_ted_sps_after_rate_change()`): the options a stream opened with in another mode do not
    narrow a live switch to TETRA onto the P25 CQPSK filter (`IO_RTL_DEMOD_CONFIG`). Its rate rule lives in
    `include/dsd-neo/runtime/tetra_rate.h` (`src/runtime/tetra_rate.c`): TETRA runs only at a DSP rate that is a
    multiple of its 18 ksym/s (`dsd_tetra_dsp_rate_runs()`), so `-fT` and a Qt TETRA system without a bandwidth of its
    own take `DSD_TETRA_DSP_BW_KHZ` (72), and `dsd_tetra_dsp_rate_advice()` words the stream-start warning
    (`src/io/radio/rtl_sdr_fm.cpp`) and the mode-change toast for a rate that cannot work, with the fix for what sets
    the rate. Tests: `RUNTIME_DECODE_MODE`, `RUNTIME_CLI_PARSE`, `APP_COMMAND_QUEUE`, `UI_MENU_ACTIONS`,
    `IO_RTL_DEMOD_CONFIG`, `UI_QT_SESSION_ARGS`.
  - rdio-scanner export (`src/runtime/rdio_export.c`, `include/dsd-neo/runtime/rdio_export.h`): DirWatch sidecars and,
    with `USE_CURL`, API uploads on one background worker fed by a bounded queue (`DSD_RDIO_UPLOAD_QUEUE_MAX`).
    `dsd_engine_cleanup()` calls `dsd_rdio_upload_shutdown()` after the final calls close, and it drains the queue, so
    a normal end (replay EOF, input closed, USB detach) and the CLI (SIGINT, the terminal UI's `q`, the API's `quit`)
    still upload the last calls. Only a stop or restart the user starts in the app calls `dsd_rdio_upload_cancel()` (issue #718
    D4): every QML `decoderHost.stop()` caller (Stop listening, switching systems, retry after a failure, leaving
    Signal search or its Stop, Back or closing the window with background listening off, "Dismiss failure and choose a
    site") and the notification's Stop action, as `StopCause.USER` through `nativeStop(userRequested, cause)`: a lock-free flag the worker loop (no further jobs), the retry
    loop (between attempts) and the transfer's xferinfo callback (aborts the transfer in flight) poll. Shutdown still
    owns the join, logs once how many uploads were not sent, and clears the flag; uploads queued after the cancel (the
    final call cleanup closes) are not sent either. Files are deleted only after a successful upload, so abandoned
    calls keep theirs. An embedding host clears a cancel that landed after the last shutdown with
    `dsd_rdio_upload_cancel_reset()` when it configures the next run. Tests: `RUNTIME_RDIO_EXPORT` (stalled endpoint,
    cancel before and during the drain), `ANDROID_ENGINE_STOP_JVM` (the cause each Android caller passes).
  - RadioReference.com import client (`src/runtime/radioreference/`): SOAP envelope builder, expat response parser,
    worker-thread client with cancellation, and the generators that turn fetched systems into the channel-map and
    talkgroup CSVs `src/core/file/dsd_import.c` already parses. UI-agnostic C API in
    `include/dsd-neo/runtime/radioreference.h` and `radioreference_generate.h`; needs `USE_CURL` and `USE_EXPAT`, and
    reports `dsd_rr_available() == 0` without them. Shared curl setup lives in the module-private
    `src/runtime/curl_common.h`, alongside `rdio_export.c`'s use of it.
    `dsd_rr_site::has_position` validates latitude/longitude independently of the optional coverage radius;
    `(0,0)` is a missing-position placeholder. Invalid coverage becomes zero and never erases valid coordinates.
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
    `svc_rtl_restart_recovery_locked()`). Beside the latch, the committed receiver ending
    (`dsd_input_failure_commit_receiver_end()`, `dsd_input_failure_receiver_ended()`; cleared only by
    `dsd_input_failure_clear()`, and not counted by the generation): set only by the radio path that ends a session for
    its USB receiver, when it raises the exit flag. The latch alone is not that fact, since a path that recovers may
    write and restore it (an Airspy reconfigure that fails, rolls back and keeps streaming). Tests:
    `RUNTIME_INPUT_FAILURE`, `IO_RTL_READER_EXIT`, `IO_USB_PROGRESS_MONITOR`.
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

**Multi-consumer observers:** the snapshot accessors are single-consumer, so a second consumer that must read the
published data while a frontend also does (the control API) registers a telemetry observer with app-control instead
(`include/dsd-neo/app_control/telemetry_observers.h`). The two app-control publishers call the observers on the decode
thread with the live `dsd_state`/`dsd_opts`, after releasing their own locks. Callbacks run under the observer
registry lock, so `dsd_app_telemetry_observer_remove()` returns only once no callback of that observer is running; a
callback must not block, submit commands, or add/remove observers. Live event-history rows are read under
`dsd_event_history_transaction_begin()`, as every other cross-thread reader does.

### Control Pump (DSP/Protocol → Runtime ← App-Control)

`include/dsd-neo/runtime/control_pump.h` lets decode loops pump queued commands (`dsd_runtime_pump_controls()`) and ask
whether one waits (`dsd_runtime_controls_pending()`, issue #634), without depending on app-control. App-control
registers both at frontend start (`dsd_app_frontend_runtime_start()`); the pending query is asked only while an input
waits on a silent source.

### Frame Sync Hooks (DSP → Runtime ← Engine/Protocols)

DSP frame-sync code may need to trigger protocol-specific actions (for example, trunking state machine ticks) without
depending directly on protocol headers. The runtime provides a small hook table in
`include/dsd-neo/runtime/frame_sync_hooks.h`; the engine installs the concrete implementations at startup in
`src/engine/frame_sync_hooks_install.c`. `decoder_ticks_due` tells a silent-input wait that decoder-thread trunking
timers are due (issue #703); the engine's answer reads the state machines under the P25 SM tick guard.
`p25_probe_in_flight` tells the SPS hunt that a P25-driven control-channel probe is in flight, so it steps only between
the P25 profiles (see the startup control-channel hunt below); with nothing installed there is no probe.

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
  its park. Pass only identity that has already cleared the protocol's FEC/CRC gate (DMR's `dmr_flco()` reports only
  an LC whose check passed; under `-F` a RAS-accepted voice LC counts, so a scanned RAS repeater holds); the
  coordinator runs it through the talkgroup policy before refreshing the hold, and ignores it unless the parked target
  is of the matching conventional family. P25 reports voice starts only, not PDU data, and treats decryptable calls
  as clear for this policy.
  Phase 2 XCCH reports only after MAC_PTT crypto resolution; Phase 1 late joins use the decoded service encryption
  bit while crypto classification is unknown. `p25_sm_note_conventional_activity()` rejects stale/unidentified calls.
- `dsd_trunk_scan_hook_active_chan_csv()` — the parked target's channel-map path, which `opts->chan_in_file` cannot
  answer while scanning (`src/protocol/nxdn/nxdn_trunk_diag.c`)
- `dsd_trunk_scan_hook_enc_lockout_clear_snapshots()` — scrub the encrypted-target lockout ledger parked in every
  target snapshot, so a user purge is not undone by the next rotation (`src/app_control/actions/actions_trunk.c`)
- `dsd_trunk_scan_hook_control()` — operator scan controls on the parked target list, op-coded (hold toggle, avoid the
  active target, clear avoids, advance now); answers "unavailable" when trunk scan is not installed
  (`src/app_control/app_command_queue.c`)

Recovery ownership lives beside these hooks. `dsd_trunk_p25_recovery_allowed()` / `dsd_trunk_dmr_recovery_allowed()`
admit a validated owner (`dsd_state::trunk_recovery_protocol`), or, with none yet, a frame set that decodes only that
protocol's trunk. `dsd_trunk_p25_cold_start_presumed()` names the protocol that runs the startup control-channel hunt
of a session no protocol has proven yet: P25 for every frame set with P25 (`-ft`, Auto `-fa`,
`-1`, `-2`) except a selected EDACS session (`dsd_edacs_session()`: `-fh -fa`, an EDACS-to-Auto switch), whose
control names no owner the hunt could stop on; it grants no recovery ownership (issue #703,
`RUNTIME_TRUNK_SCAN_HOOKS`). It also keeps a frequency entry an operator move during that hunt
(`manual_frequency_selects_p25_cc()`), keeps the decoder ticks due (`engine_decoder_ticks_due()`), and with
`dsd_trunk_p25_recovery_allowed()` decides the P25 chain of a control-channel tune in `src/engine/trunk_tuning.c`
(`dsd_engine_cc_is_p25()`): a session P25 drives is P25 from that evidence, not from raw sync history or a GFSK
`rf_mod` the frame-sync hunt forced. The presumption counts there only while the P25 machine hunts
(`p25_sm_hunt_probe_in_flight()`, read under the tick guard); another tune of that session (the engine's no-carrier
return, an IDAS busy-repeater follow before NXDN is confirmed) keeps the sync rule.
`dsd_trunk_nxdn_recovery_allowed()` admits the NXDN control-channel hunt: trunking on outside `-Y` and trunk scan,
NXDN48 or NXDN96 decoded (whatever the owner, so a mode change away from NXDN stops it), and an NXDN owner
(`DSD_TRUNK_RECOVERY_NXDN`, named by a CRC-good CAC or a confirmed Type-D SCCH) or, with none yet, a frame set without
P25, DMR or ProVoice (`-fi`, `-fn`). A rest channel DMR left behind (`dmr_rest_channel`) never overrides a P25 or NXDN
owner's control channel at the no-carrier return (`no_carrier_has_mapped_dmr_rest_channel()`).

Retune requests use a sibling table, `include/dsd-neo/runtime/trunk_tuning_hooks.h`, whose implementations the engine
installs from `src/engine/trunk_tuning.c` in `src/engine/trunk_tuning_hooks_install.c`. The correlated request records
(`src/runtime/trunk_tuning_hooks.c`) outlive every owner: an owner that drops what it staged for a request in flight
(the TETRA state machine, whose state a decode-mode change frees) disowns it (`dsd_trunk_tuning_request_disown()`),
which leaves an unanswered request in flight, its success the tuner boundary, and retires its failure at once, since
nothing is left to roll back (`RUNTIME_TRUNK_TUNING_HOOKS`, `ENGINE_TETRA_TRUNK`). Every commit of an input
replacement (`dsd_engine_note_input_replaced()`, under the P25 SM tick guard with the replacement), and every start
of a radio stream on another device (the `DSD_CARRIER_BOUNDARY_SOURCE` carrier boundary), resets the request
records (`dsd_trunk_tuning_requests_reset()`, as the start of a run does): the replaced device's requests can no longer
gate the new input's frames, and a tune recovery gives the new device before the engine reaches the switch's boundary
is kept. When no tune owner is active the engine retires failures through
`dsd_engine_retire_inactive_tune_failures()`, which first reconciles `opts->rtlsdr_center_freq` with the RTL stream's
applied frequency when the newest unresolved request failed, so a frequency an asynchronous retune staged but never
reached is not read as the receiver's. It retires only through the request it inspected
(`dsd_trunk_tuning_retire_failed_requests_through()`): while the newest is in flight only older failures go, and one
that fails after the read waits for the next pass. The synced-frames loop then gates with
`dsd_trunk_tuning_frame_is_current()`, never with `dsd_trunk_tuning_frame_is_dispatchable()`, whose retirement does not
reconcile (issue #703, `ENGINE_COLD_START_HUNT`, `ENGINE_TUNE_FAILURE_RECONCILE`).

## App-Control

- Path: `src/app_control`, `include/dsd-neo/app_control`
- Target: `dsd-neo_app_control`
- Responsibilities:
  - Quit (issue #518 B2): the decoder thread may sit in a read that never returns (a receiver that stopped before its
    silence verdict, a driver call that hangs), so `dsd_app_command_submit()` raises the exit flag for an accepted
    `DSD_APP_CMD_QUIT` itself, after session admission and under the queue lock, and still queues the command (its
    handler stores the same flag). A submission the closed session rejects raises nothing, and an overflow that evicts
    the queued quit cannot take it back. Test: `APP_COMMAND_QUEUE` (`test_quit_takes_effect_at_submit`); tests that
    submit a quit put the flag down after.
  - Signal search commands 712/713 are frontend-only and not public TCP API commands.
    `actions/actions_signal_search.c` runs them on the decoder thread, adds the access code
    and received tone to copied hits, and flushes on stop. The call tuple (IDs, texts, slot,
    crypt) is the coordinator's and passes through untouched: the observer reads no call text
    and carries nothing forward from an earlier publication. The access code is formatted only
    on confirmation callbacks, from the service options of the call the tuple names (its slot,
    the same ACTIVE, protocol and CRC checks); periodic status refreshes cannot promote it from
    a subsequent unconfirmed sync, and keep the access code and tone the hit last had. The
    callback reads the access code from state as it runs, so a protocol reports a confirmation
    only after committing the content that passed the check: M17's LSF, D-STAR's
    slow-data header and YSF's DCH report after publishing their callsigns (and M17's
    CAN), and the dPMR and NXDN confirm modules report a frame's confirmation from
    `*_confirm_end_frame()`, after the frame has published the identity, colour code or
    RAN that confirmation unlocked. One valid frame is then enough for its details to
    reach history.
    `signal_search_store.cpp` owns the mutex-protected snapshots and a native worker for
    the versioned private journal, bounded upserts and atomic compaction/avoids. Each
    journal version has its own file. The store is opened with the version 1 journal's
    path (Qt: `signal-search-history.v1`) and reads and writes `DSS2` records (version 1's
    `DSS1` body followed by the checked `site`, `slot` and `crypt`) in the version 2
    journal beside it (a trailing `.v1` becomes `.v2`, else `.v2` is appended). The avoid
    list keeps its name (`<v1 path>.avoids.json`, the backup's `kAvoidsFileName`). While
    no version 2 journal exists, the version 1 journal's rows load with those details empty
    and the first write after opening (always a compaction) creates the version 2 journal
    from them; once it exists, even empty or damaged, it alone is read. The version 1
    journal is never written, replaced or removed, so an earlier release keeps its own
    history (rows it adds later are not migrated). Each file accepts only its own
    version's magic; another magic, a size that does not match or a bad checksum stops
    the load with an error, keeping the records before it. Loaded texts are cut at their
    last byte and an out-of-range slot loads as 0. Texts are encoded up to their NUL and
    zero-padded, and a republished hit whose encoding is unchanged neither advances the
    history revision nor queues a write, so only new checked content does. Detection
    history (either file) is not part of a backup. History
    and permanent avoids are separate files with separate write state: one whose write
    failed stays owed, with its error, until its own write succeeds, so saving the other
    neither retires it nor clears the error. Each later worker pass, flush and close
    retries it; a history retry compacts every retained record, so nothing is appended
    twice. The worker survives QML teardown; it never reads live decoder state. History
    and avoid revisions let frontends copy history only when it changed. The control
    actions mirror the session's own avoid list (cleared when it stops), only after the
    coordinator accepted the avoid, and with the frequency `dsd_engine_signal_search_target()`
    resolved on the decoder thread, never an older published hit; a stale session or hit
    changes neither list. `dsd_app_signal_search_permanent_avoid()` returns
    `DSD_APP_SEARCH_AVOID_ADDED`/`DUPLICATE`/`FULL`/`INVALID`. `AVOID_PERMANENT` is
    permanent-first: validate, save (a full list refuses with a notice and no skip or session
    avoid), then the session's avoid (the current hit, or by frequency for a history row's
    hit ID 0); a full session list after the save is reported as "Saved as a permanent
    avoid; this session's avoid list is full" (`APP_SIGNAL_SEARCH_PROVENANCE`,
    `APP_SIGNAL_SEARCH_STORE`). A rejected start
    with no coordinator left publishes an unowned error, and
    `dsd_app_frontend_runtime_stop()` retires any status left behind
    (`dsd_app_signal_search_session_ended()`). A rejected restart of a paused coordinator
    keeps that coordinator's phase, session and ownership; only the published message
    changes.
  - Frontend metrics and raw telemetry snapshots used by the terminal renderer
  - Command queue dispatch and menu service helpers. Decoder-owner runtime start/stop opens/closes admission under
    the queue mutex; stop securely cancels pending payloads. Evicted or cancelled talkgroup exports publish failed
    completions. Successful inherited-policy scan exports update the configured persistence path.
    `app_command_queue.c` uses `command_writes_policy_store()` to guard live policy transactions against the P25
    watchdog's release-time audio flush; callees must use `_locked` forms to avoid acquiring the guard again.
  - Input switches (issue #634): Input > Switch source (WAV / raw file, symbol files, UDP, Pulse and a Pulse device),
    Replay last, Stop replay and the TCP connect go through `dsd_audio_switch_input()` by way of the services in
    `menu_services.c` and `tcp_audio_connect.c` (`svc_open_audio_file_in()`, `svc_open_symbol_stream_in()`,
    `svc_open_symbol_in()`, `svc_udp_input_config()`, `svc_set_pulse_input()`, `svc_stop_playback()`, and
    `svc_tcp_connect_audio()`, which connects within a bound through `ConnectBounded()`). Each handler holds the P25 SM
    tick guard around the switch, since the watchdog reads the input type and rate it rewrites, and refuses it first in
    the engine modes that read no switched input (`svc_input_switch_unsupported()`: MBE playback, the M17 IP frame
    receiver and encoders) and while a scan is in force (`ui_scan_goes_on()`, or `--trunk-scan` with targets), whose
    timing snapshots belong to the running input. `ui_cmd_finish_input_switch()` turns the result into the toast and
    status: a switch that ran sets `dsd_state::input_boundary` and runs `ui_input_switched()` (an output that cannot
    follow keeps the new input and fails with its own toast); one that kept the input resets nothing; a UDP restart or
    loss runs `ui_input_left()`. A path payload that does not fit, or that the queue cut short, is refused rather than
    truncated. Stop replay acts on a WAV or symbol playback only and switches to the configured Pulse input. Replay last
    replays `dsd_opts::symbol_capture_last`, which `closeSymbolOutFile()` sets for every capture it closes (a stop, a new
    capture, a rotation), so `SYMCAP_STOP` no longer writes `audio_in_dev`. The RTL-SDR row opens
    `svc_rtl_row_destination()` (the first radio spec that is no Airspy one: the running spec, then the one a PCM switch
    kept; else `rtl`), and is checked against it before anything changes; once the radio runs, the PCM input it
    replaced is closed (`closeAudioInDevice()`), and kept when the radio does not start. A config apply's same-type reopens (TCP, UDP, Pulse) go
    through the switch too, put back the input name the config wrote when they fail, and take the boundary for a
    restarted UDP input (`apply_cfg_note_reopen()`); its WAV reopen refuses a pipe or device. The queue answers the
    runtime's controls-pending query (`dsd_app_commands_pending()`). Tests: `APP_COMMAND_QUEUE`, `RUNTIME_CONFIG_APPLY`,
    `UI_MENU_SERVICES`, `UI_TCP_AUDIO_CONNECT`.
  - Replay tuning (issue #575): an I/Q replay plays the tuning its capture recorded and defers every other retune
    unseen, so while the input in force is a replay (`dsd_opts_input_is_iq_replay()`) `MANUAL_TUNE` (the Spectrum tap),
    `RTL_SET_FREQ` (the terminal menu's frequency row, Qt's frequency entry), `TUNER_RELEASE`
    (`CommandBridge::releaseTuner()`), `RETURN_CC` (refused ahead of its no-op cases, trunking off or no control
    channel yet, so it never reads as done) and `CHANNEL_CYCLE`
    (every leg: LCN, P25 candidate, `-Y` row, trunk-scan target) are refused as failed commands with the toast "An I/Q
    replay cannot retune." (`ui_cmd_refuse_replay_tune()`), where they used to report a tune that never landed or fail
    on the backend's deferred result with no reason given. Scan hold and avoid, skip and lockout are not refused: their
    main effect is on the scan or the decoder. The tap's refusal comes ahead of its tuner-owner gates, whose toasts
    point at a release; every refusal of the tap, the owner gates included, is a failed command with its reason toasted,
    as the frequency entry's are. Qt and the control API learn it from the toast: they see only whether the command was
    queued. Qt's Spectrum screen offers none of them during a replay (`MetricsModel::replayInput`; see UI, Qt, "Tuned
    frequency, call codes and replay tuning on screen"). `svc_rtl_stop_locked()` clears the replay centre the
    tuned-frequency reading follows (see IO, "I/Q replay tuned frequency"). Tests: `APP_COMMAND_QUEUE`,
    `UI_MENU_SERVICES`.
  - Bootstrap retains only positional playback filenames in argv storage, preserving argument indexes with empty
    placeholders. State snapshots exclude argv ownership; teardown securely erases retained strings.
  - Retained results: talkgroup export and decryption completions are kept as the latest result
    (`dsd_app_tg_export_result_get()`, `dsd_app_decryption_result_get()`) and in a history of the last
    `DSD_APP_RESULT_HISTORY` (64) by sequence number (`dsd_app_tg_export_results_since()`,
    `dsd_app_decryption_results_since()`), for a consumer that must see every completion (the control API, which
    reports a `result_gap` when more completed than the history held)
  - Telemetry observers (`telemetry_observers.c`): the multi-consumer alternative to the snapshot accessors; see
    "Telemetry Hooks" for the contract
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
  `DSD_APP_CMD_DECODE_MODE_SET` for the mode already in effect changes nothing (`decode_mode_set_early_verdict()`):
  the lossy `dsd_infer_decode_mode_preset()` reading for a single preset, the exact one for AUTO, so a session already
  on the whole Auto set (`-fa`, the default) keeps its calls and modulation on a repeat Auto while a narrowed set Auto
  only labels still widens (`APP_COMMAND_QUEUE`).
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
  whole config unapplied); a digital-to-digital `[mode]` change keeps its earlier behaviour, except that an EDACS
  session (`dsd_edacs_session()`, or `-8`) also gets the raw sink it writes to, and that an RTL front end is told the
  digital modes it now configures
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
  radio input (PCM input runs no channel filter, so no rate holds the width there; with a rigctl peer it is the
  passband the peer is asked for, below) and to the running stream
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
  or `Failed: <why>`. On an I/Q replay `svc_rtl_set_bandwidth_locked()` stores the request as EXPLICIT and re-plans
  the replay instead of reopening it (`svc_rtl_replay_replan_locked()`, see IO, "Run-time re-plan"): a bandwidth that
  plans the running chain reopens nothing, and a refused re-plan puts the request back with the replay running. These
  are the commands with a rollback: Input > Switch source, RTL_SET_BW, AIRSPY_SET and a
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
  `rtl`/`rtltcp` spec, or any device string on an RTL input that names no SoapySDR, Airspy or replay device (the bare
  `rtl` Input > Switch source > RTL-SDR writes when no radio ran among them), runs at `rtl_dsp_bw_khz`, saturated rather than overflowed for a loaded
  config's out-of-range `rtl_bw_khz`. `svc_check_analog_bandwidth()` holds a width to no rate on a PCM input, which runs
  no channel filter, whatever radio a switch to it replaced (kept in `dsd_opts::radio_in_dev`). A config apply whose reopen of
  the running RTL-family input fails to start (the live Airspy reopen, `svc_airspy_reopen_locked()`, or the hot restart
  for a new spec, `apply_cfg_rtl_hot_restart()`, both from `apply_cfg_radio_input()`) never loses that input either
  (`ui_cfg_settle_reopen()`, issue #578): it takes the reason from `svc_describe_start_failure()`, puts back what the
  apply found once its checks passed (`ui_cfg_rollback`: the `ui_radio_input` above; the configured receive settings,
  meaning `[mode]`, `[demod]` and both `[analog]` widths, restored over the row-scoped options in force by
  `ui_restore_receive_settings()`, which puts back `[analog]`'s tone policy from among those options as well; and what
  `[mode]` sets outside that snapshot that the decoder reads, `ui_cfg_mode_extras`: `ea_mode` and `esk_mask`, which
  `edacs_ea`/`edacs_esk` and the EDACS/ProVoice preset set, the extra LRRP ports and the compressed-UDP port map; the
  audio layout a preset carries
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
- Rigctl passband on audio input (issue #621): where a rigctl peer demodulates PCM audio input
  (`dsd_opts_rigctl_peer_demodulates()`) the analog width is the passband the peer is asked for (runtime's
  `dsd_rigctl_passband_of()`), so a command that changes the request in force asks the peer at once, as the front end is
  asked, rather than at the next retune. `src/app_control/rigctl_passband.c` holds the follow:
  `svc_rigctl_passband_now()` is the request for the options in force and the row whose scope is in force
  (`dsd_scan_mode_row_options()`, as `dsd_engine_scan_rigctl_apply_modulation()` reads it; FOLLOW without such a peer),
  `svc_rigctl_session_now()` pairs it with whether a peer demodulates the input (`svc_rigctl_session`), taken before a
  change, and `svc_rigctl_follow_passband()` hands the engine's strict live apply the request once the change made it
  differ (`dsd_rigctl_passband_equal()`, which counts the source, so entering the monitor from a digital mode asks even
  where the wire request matches; the rules sit in `rigctl_follow_asks()`). It asks nothing without a peer, while a
  scope is suspended (`dsd_scan_mode_updating()`: the scoped dispatcher follows after the resume), or while a `-Y` tune
  or retry is staged (`dsd_engine_channel_scan_waiting()`: the restaged tune asks for its row). Off the monitor the
  request is FOLLOW, FM at `-B` (0: the peer's own passband), which a tune sends best-effort: it is asked only to undo a
  passband this client set on the monitor before the change (`dsd_rigctl_passband_sets_passband()` of the request
  before, with the peer there), so a decode-mode switch or config that leaves the monitor for a digital mode does not
  leave the peer on the monitor's width until another tune; FOLLOW before as well (a `-B` edit on a digital session), or
  the peer's own passband before, asks nothing. A peer the change brought (a first rigctl connect, a switch onto PCM
  input: `peer` 0 before) is asked for a width only (`dsd_rigctl_passband_is_width()`), as the start and reconnect asks
  are, `-B` and the peer's own passband waiting for a tune. The rigctl I/O runs under the P25 SM tick guard, whose
  watchdog retunes use the socket and its peer record; the guard is not re-entrant, so a caller that holds it passes
  `guarded`. It returns 1 when the peer took the request, -1 when it refused or the reply was lost (the peer then runs
  either passband; the engine logs it), and 0 when nothing was asked or the peer went away. `svc_set_analog_bandwidth()`
  (which takes `guarded`, `command_writes_policy_store()` from the handler) follows after storing the width and, on -1,
  stores the previous configured width back, asks again against the refused request, best-effort, and fails with `NFM
  bandwidth -> 20 kHz: the rigctl peer refused the passband`. `svc_apply_rigctl_setmod_bw()` does the same for `-B`
  around `svc_set_rigctl_setmod_bw()` (still clamped to 0..25 kHz), so `DSD_APP_CMD_RIGCTL_SET_MOD_BW` now fails, with
  `Refused: Rigctl setmod BW -> 25000 Hz: the rigctl peer refused the passband`, where it used to only store.
  `apply_cmd_scoped()` takes the session before every command, ahead of the suspend, and follows it after the resume and
  the AM-on-PCM fallback (`apply_cmd_follow_rigctl_passband()`): a refusal leaves any command but a config apply
  standing, with `WARNING: The rigctl peer refused NFM 9 kHz; the setting stands.` and the toast `Rigctl peer refused
  NFM 9 kHz (see log)`, and so does a refused FOLLOW undo, a config apply's included, since a tune's `-B` is
  best-effort; any other refusal of a CONFIG_APPLY whose `[analog]` changed the refused kind's configured width
  (`ui_cmd_widths_before`) stores the old width back, asks again under the guard it holds, logs and fails with `Config
  applied; the rigctl peer refused AM 10 kHz: the AM width stays 8 kHz` (the old setting as
  `dsd_app_analog_width_setting_text()` spells it); one that changed only what runs (its `[mode]`, its `[input]`) logs
  and fails with `Config applied; the rigctl peer refused NFM 12.5 kHz (see log)`, and a config sets no `-B`, so none is
  put back. The width, `-B` and "this channel" commands have asked already, and the IO record skips a request the peer
  is known to run, so their second follow sends nothing. A configured AM width edited under an `am` row without its own
  width therefore reaches the peer at once, where it used to wait for the next retune. `svc_rigctl_connect()` asks a new
  connection for the session's width (`dsd_engine_rigctl_ask_session_passband()`) after the socket swap, under the guard
  it already holds. Tests: `APP_CONTROL_RIGCTL_PASSBAND`, `APP_COMMAND_QUEUE` (a fake peer behind
  `--wrap=SetScanRowModulation` and `--wrap=SetModulationKind`), `UI_RIGCTL_CONNECT`, `UI_MENU_SERVICES`.
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
  hold (issue #526). `dsd_app_scan_timing_phrase_width()` is the longest phrase it can set, phase variants included: the
  room the terminal keeps for the phrase so what follows it holds its column (issue #669). The decoder owns every
  deadline; these views only difference it against the caller's reading of the
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
  `dsd_app_squelch_view_configured_text()` the default as the terminal prompt opens on it (`auto+10`, `-60.0`, `off`;
  a level whose one-place dB would not read back as itself, within 0.05 dB of full scale or below the parser's -200 dB,
  as its linear power, `%.6g`, so accepting the prompt unchanged keeps the level, issue #669);
  `dsd_app_squelch_view_format_ex()` and `dsd_app_squelch_view_dynamic_status_ex()` with
  `DSD_APP_SQUELCH_VIEW_FIXED` give the terminal's fixed-width text: quieting in four places, a floor in six, padded to
  the longest status the setting in force can show, so a terminal field after it holds its column while the gate's
  state or reading changes (Qt keeps the plain text);
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
  `include/dsd-neo/app_control/crc_policy_view.h` and `src/app_control/crc_policy_view.c` report Relaxed CRC (`-F`) as
  one setting: `dsd_app_crc_policy_view()` gives the configured value (`dsd_scan_mode_configured_relaxed_crc()`: the
  scope's configured baseline while a scan scope is live, else `dsd_opts`), the value in force, and `row_override`
  when the row on air carries its own `-F` or `--strict-crc`; `dsd_app_crc_policy_edit_notice()` is the toggle's toast
  under such a row. The pair it reads, `aggressive_framesync` and `dmr_crc_relaxed_default`, is set only through
  `dsd_opts_set_relaxed_crc()` (`core/opts.h`;
  `dsd_opts_relaxed_crc()` reads either field relaxing as on) by `-F`, `--strict-crc`, a row's CRC option and
  `DSD_APP_CMD_AGGR_SYNC_TOGGLE` (no payload flips it, the terminal's key and row; an `int32_t` payload sets it, the
  Qt/Android switch, so a frontend acting on a snapshot one command behind cannot turn it the wrong way); the scan
  scope's capture and restore copy the pair as it is. The terminal's Relaxed CRC row reads the view, and so do Qt's
  `relaxedCrcConfigured`/`relaxedCrcEffective`/`relaxedCrcRowOverride` (Settings → Decoding → Relaxed CRC, an
  `AppPrefs::relaxedCrc` preference `session_args.cpp` turns into `-F` for every app-started session unless the
  Extra decoder arguments name `-F` or `--strict-crc`). Tests: `APP_CONTROL_CRC_POLICY`, `UI_QT_SESSION_ARGS`,
  `UI_QT_QML_CALL_LISTS` (`tst_settings_relaxed_crc.qml`).
  `include/dsd-neo/app_control/dmr_ras_view.h` and `src/app_control/dmr_ras_view.c` are the DMR RAS indicator:
  `dsd_app_dmr_ras_view()` reports `detected` (a DMR record's `DSD_RXID_DMR_RAS`), the Relaxed CRC policy it reads
  through `dsd_app_crc_policy_view()`, and `needs_relaxed` (detected while relaxed CRC is not in force);
  `dsd_app_dmr_ras_label()` is the compact label, `RAS` or `RAS (-F decodes)`, and `RAS (channel sets strict CRC)`
  while `-F` is configured on under a scan row's own `--strict-crc` (pressing F there would turn `-F` off, as Qt's
  `rowKeepsStrict` says). The terminal's DMR BS and MS headers print it after the colour code (`RAS; `,
  `RAS (-F decodes); `), and the site identity view shows the compact `RAS`
  token from the record; a frontend's RAS field and its relaxed-CRC hint read this view (Qt's `dmrRasDetected` and
  `dmrRasNeedsRelaxed`: the site sheet's **Access RAS** field, its hint and **Turn on Relaxed CRC**). Tests:
  `APP_CONTROL_CRC_POLICY`, `UI_NCURSES_PRINTER_HELPERS`, `APP_CONTROL_SITE_IDENTITY_VIEW`, `UI_QT_METRICS_MODEL`,
  `UI_QT_QML_CALL_LISTS` (`tst_monitor_site.qml`).
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
  input without a rigctl peer. The running stream's demod rate, or with none running on an input whose RTL DSP bandwidth
  sets the rate that rate, bounds the widths offered (`max_hz`, on a radio input under any preset); an unset default
  reads as what the monitor runs at that rate, as the next start publishes it: the rate itself where no channel filter
  runs (below 20 kHz, or as `DSD_NEO_CHANNEL_LPF` says), the legacy WIDE plan's passband
  (`dsd_channel_lpf_legacy_wide_width_hz()`) where the filter runs but the rate cannot realize the default, both
  DSP-limited, and so it does at a running stream's demod rate while the front end is off the monitor (a typed digital
  row, CQPSK toggled on under -fA), as the monitor it returns to publishes it. There whether the filter runs is the
  stream's own decision (`rtl_stream_channel_lpf_default()`, carried as `dsd_frontend_metrics::channel_lpf_default`),
  which its configuration made from the rate it started at and keeps whatever rate the device then delivers (a forced
  rate, a replay's capture rate), so the view does not re-derive it from the rate. The configured width comes from the
  scan scope's configured view while one is live. An analog scan row on air (issue #526) shows its width on any session
  (`row_analog`), and a row that sets its own width (`row_override`, `row_hz`) is the width in force, read as `12.5 kHz
  (row; default 16 kHz)` with the configured width of its demodulator, which it overrides (on an AM session its leave
  returns to the AM width instead). On PCM audio input with a rigctl peer that demodulates it (`peer_passband`,
  `dsd_opts_rigctl_peer_demodulates()`, issue #621) the width is the passband the peer is asked for, and no rate bounds
  it (`radio_input`, `max_hz` and `dsp_limited` stay 0). There is a reading whenever the view offers a kind
  (`dsd_app_analog_width_offered()`), so the frontends that show a passband beside the width controls show the one asked
  for now: while the analog family runs the view's kind (`passband_in_force`) the reading is the rule's request,
  `dsd_rigctl_passband_for_kind()` over the row's own width, the configured width and `-B` (`setmod_bw_hz`, 0 off a peer
  session), with its source in `passband_source`; off it (a typed digital row on air under -fA, a digital session with
  an explicit width set) the peer follows at `-B`, or at its own passband without one, and that is the reading, never
  the configured width, which nothing then asks for. `unset_hz` is what an unset setting of the view's
  kind stands for, `dsd_app_analog_width_unset_hz()` (public, for either kind on one snapshot): on a peer session `-B`
  for NFM (0: the peer's own) and the 6 kHz default for AM, elsewhere the kind's default; it is where a stepper starts
  from an unset setting. The view reads only the options and the copied scope, so a snapshot pair reads as the decoder
  thread does. The services that hold a DSP rate to an analog session's width hold the configured preset's own kind and
  width, not the row's. They spell the reading (`12.5 kHz`, `16 kHz (default)`, `12 kHz (DSP-limited)`,
  `not used on PCM input` without a rigctl peer; on a peer session by source, `12.5 kHz`, `6 kHz (default)`,
  `12.5 kHz (-B)`, `peer's own`, and `12.5 kHz (row; default X)` with X the setting the row overrides, the AM unset
  default as `6 kHz`), the width command's notice (`dsd_app_analog_width_edit_notice()`, for the kind the command edits
  whatever kind the configured preset runs: `Applied: NFM bandwidth -> 12.5 kHz`, or `Default NFM bandwidth -> 16 kHz;
  this channel overrides it (12.5 kHz)` under a row width, and on a peer session in force with no row width the passband
  asked for, `Applied: NFM passband -> 12.5 kHz (-B)`) and the configured setting
  (`dsd_app_analog_width_setting_format()`: `12.5 kHz`, `default`; `dsd_app_analog_width_setting_text()` spells it as
  the session reads it, an unset NFM width on a peer session `-B 12.5 kHz` or `peer's own`). The terminal's `Analog:`
  and `Passband:` status fields, the `input.nfm_bw` and `input.am_bw` rows' labels, prompts, chooser titles and
  predicates, the width command's toast, RTL_SET_BW's configured-width check, app-control's config-refusal toast and
  Qt's `analogBandwidth*`, `peerPassband`, `passbandInForce` and `nfm`/`amBandwidthUnsetText`/`UnsetHz` properties (the
  row flags as `analogBandwidthRowActive`/`analogBandwidthRowOverride`) all come from it.
  `dsd_app_analog_width_setting_hz()` reads either kind's width in `dsd_opts` (0 = default) whichever preset runs, for
  every caller that only reads the width in force (`dsd_scan_mode_configured_analog_width()` reads the configured one);
  app-control's `svc_store_analog_width_setting()` is the one writer. `dsd_app_analog_width_offered()` (issue #524) says
  which kind's width the frontends offer for editing on a radio input, and on a peer session (issue #621; never on other
  PCM input): the configured preset's kind whatever an analog scan row on air runs (an nfm row runs FM over an AM
  session), the kind that row runs, the other kind's while an explicit configured width of it is set, and AM's unset
  default where `max_hz` cannot filter it but can filter a narrower AM width (a switch to AM is refused there with word
  to narrow the width; never on a peer session, where no rate is known); it takes the scan state for the configured
  view. The terminal's width rows and Qt's `nfmBandwidthOffered`/`amBandwidthOffered` use it, and Qt's
  `analogBandwidthAm` says which kind the view's width is. Tests: `APP_CONTROL_ANALOG_WIDTH_VIEW` (every peer reading,
  live and on a snapshot pair), `APP_CONTROL_FRONTEND_PUBLIC_BOUNDARY`.
  `include/dsd-neo/app_control/access_code_view.h` and `src/app_control/access_code_view.c` (issue #575) spell a call
  history row's access code, the `dsd_access_code_kind` and value of `<dsd-neo/core/access_code.h>`:
  `dsd_app_access_code_view()` gives the short text a list row shows (`CC 1`, `NAC 293`, `RAN 5`, `CAN 0`) and the long
  one a detail sheet shows (`Color code 1`, `Network access code 293`, `Radio access number 5`, `Channel access number
  0`), a NAC as three uppercase hex digits and every other code in decimal. A kind it does not know, `NONE` included,
  or a value outside its protocol's range (colour code 0..63, dPMR's range, of which DMR's 0..15 is a subset; NAC
  0x001..0xFFE; RAN 0..63; CAN 0..15) is not visible and leaves both texts empty, so a corrupt or newer store never
  reads as a code. `dsd_app_access_code_format()` writes one of the two texts (`DSD_APP_ACCESS_CODE_SHORT` or
  `_LONG`) for a caller that shows one at a time; the view is built on it. The public header names the kinds through
  `core/access_code.h` only, never `state.h`. The Qt call history's `accessCode`/`accessCodeText` roles come from it,
  each formatting only its own text. Test: `APP_CONTROL_ACCESS_CODE_VIEW`.
  `include/dsd-neo/app_control/tetra_view.h` and `src/app_control/tetra_view.c` turn a snapshot of
  `dsd_state::tetra_pub` into the TETRA rows every frontend shows: `dsd_app_tetra_view()` makes them visible while the
  options run the TETRA preset (`frame_tetra`) and the publication has a working mode, and writes the mode line with
  the air encryption and every algorithm signalling named, whatever the last PDU's encryption (`TMO downlink ·
  encrypted (TEA2)`, `TMO downlink · clear · TEA1/TEA2`, `DMO direct · clear`, a proprietary KSG as `proprietary KSG
  9`, as many as fit; an encrypted trunked carrier whose network named none `encrypted (TEA not broadcast)`, since a
  network never broadcasts its algorithm), then the algorithms its radios' clear uplink registrations proposed, never
  as the network's (`TMO control uplink · clear · radios propose TEA1`, with what room the rest left, dropped whole
  when not one name fits), the network (`MCC 262 · MNC 1001 · LA 4097 · CC 25 · carrier 654`: the colour code through
  `dsd_app_access_code_format()`, in decimal as the call history names one, the location area and the carrier being
  received once known, the fields a TEA1 reduced key is bound to), the security row (`class 2 · SCKN 3 ·
  authentication · TEA1 reduced key`, `classes 1 and 3`, `no air encryption`; the class bits only while the BS service
  details offer air encryption, EN 300 392-7 table A.104 note 1; direct mode `DM-2-A · SCKN 5`), where call
  following stands (`control channel`, `traffic channel`, `idle`, only with trunking on and on a TMO downlink), the
  control and traffic channels (`carrier 654 · TS 1,3 · 392.1250 MHz`, a frequency to five decimals with a trailing
  zero dropped, as Qt's `Util.mhzText()` writes one), the MM status (`7 · MS frequency bands request`) and the codec's
  health (`ready · 18 frames · 2 errors`, `missing`, `failed to load`, `decode errors`), with flags for colour
  (`encrypted`, `on_traffic`, `codec_fault`) so no frontend compares words. `DSD_APP_TETRA_ASCII` swaps the ` · `
  separator for ` - ` on a terminal without UTF-8. The network row (MCC, MNC, LA, colour code) reads the received
  identity record (`dsd_state::rx_id`), never `tetra_pub.net_known`: a `--tetra-net` preset or a network the
  descrambler kept names none until a CRC-checked BSCH on this carrier, or a CRC-checked downlink block descrambled
  with it, proves it, and a checked direct mode burst drops it; the carrier number stays the publication's. Tests: `APP_CONTROL_TETRA_VIEW`, `UI_NCURSES_PRINTER_HELPERS`,
  `UI_QT_METRICS_MODEL`.
- Carrier system identity: `include/dsd-neo/app_control/site_identity_view.h` and
  `src/app_control/site_identity_view.c`. `dsd_app_site_identity_view()` turns the received identity record
  (`dsd_state::rx_id`, `core/received_identity.h`) of a snapshot into the display-ready identity every frontend shows:
  the protocol label (`DMR`, `P25`, `NXDN`, `IDAS`, `dPMR`, `M17`, `TETRA`, `EDACS`; none for YSF, D-STAR, ProVoice,
  X2-TDMA), ordered tokens with the access code first, each flagged compact or full-only (the DMR site text is one token
  per ` · ` part: `Net n` and `Site n` are compact, the Tier III model, `Cap+`/`Con+` and `SYS` full-only), `live`
  (synced on the record's family and some part received since the last no-carrier pass), the record's `epoch`, and
  `cc_freq_hz`, the only control-channel frequency a frontend shows. `dsd_app_site_identity_format()` joins the tokens
  with ` · `, compact or full, optionally led by the protocol. It reads nothing but the record (and the EDACS addressing
  flags), so a Phase 2 seed, a preset, a control-channel anchor or another carrier's value never reaches a display. Its
  consumers: Qt `MetricsModel` (`siteLine`, the SiteSheet fields, `ccFreqHz`, the `carrierIdentity*` group), the
  terminal spectrum legend's `System:` line (`spectrum_format_system_line()` in `ncurses_visualizers.c`). The terminal
  Call Info lines (`dsd_ncurses_printer.c`: P25 NAC/WACN/SYS/RFSS/SITE, the `Site:` LRA in `ncurses_p25_display.c`, DMR
  brand/site/XPT free LCN/rest LSN/system code, NXDN location and IDAS fields (which of the two lines shows is the kind
  of site the record names, the decoder's location category only while it names none), the EDACS site header, M17 CAN,
  the DMR and dPMR colour codes and the NXDN RAN, all kept through a sync loss as the record keeps them, `Freq:`) keep
  their own column formats and read the record's parts directly, each only while the record is of that section's family,
  so a section the `ncurses_last_synctype` latch still picks after a boundary prints dashes. A hand-set Phase 2 seed
  prints apart (`Seed: BEE00.348.293 (set)`). Tests: `APP_CONTROL_SITE_IDENTITY_VIEW`, `UI_NCURSES_PRINTER_HELPERS`,
  `UI_NCURSES_VISUALIZERS_HELPERS`, `UI_NCURSES_P25_DISPLAY_HELPERS`, `UI_QT_METRICS_MODEL`,
  `APP_CONTROL_SNAPSHOT_EVENT_HISTORY` (the record rides the snapshot).
- Decode quality: `include/dsd-neo/app_control/p25_metrics.h` and `src/app_control/p25_metrics.c`
  copy FEC ok percentages, populated P25 voice-error averages, and non-P25 last-frame
  errors from the caller's held snapshot. The core vocoder maintains ring counts;
  canonical call starts reset the affected slot. Qt publishes one `qualityChanged`
  group, and the terminal average helpers wrap the same arithmetic. Counts are
  corrected errors per voice frame, not BER; 0/0 FEC ratios are invalid.
- Android notification record: `include/dsd-neo/app_control/notification_status.h` and
  `src/app_control/notification_status.c`. The snapshot publishers (`ui_snapshot.c`, `ui_opts_snapshot.c`) feed it
  through `dsd_app_notification_publish_state()`/`_publish_opts()`, which stay dormant until a reader has called
  `dsd_app_notification_get()` once. `dsd_app_notification_encode()` writes one tab-separated record, version `v3`, 33
  fields: an 11-field header (version, protocol, radio_input, trunking, trunk_tuned, cc_freq_hz, vc_freq_hz,
  center_freq_hz, lead_slot, audible_stamp in unsigned decimal, audible_age_ms in signed decimal with -1 for none), then
  11 fields per slot (state, name, tg_text, src_text, tg_id, enc, algid, kid, elapsed_ms, emergency, priority). Text
  fields have control characters replaced with spaces, and a record that does not fit is dropped (0), never truncated.
  `nativeNotificationStatus()` in `android/dsdneo_jni.cpp` is its one caller, from the Android service's status poll and
  `ScreenSupport`'s synchronous samples, and `DecoderStatus.kt` parses the record; change both together, the version
  included. The audible pair (issue #574) is not stored by the publishers: every `get()` reads it afresh from one
  `dsd_audio_activity_read()`, so the age is that of the stamp beside it, and `encode()` is what arms the stamp (see
  Core), whenever it is given a buffer, so from the first poll, before anything is published; `get()` alone, which the
  API feed uses, never does. `dsd_app_notification_reset()` (from `dsd_app_frontend_runtime_stop()`) clears the
  published status and the stamp, which stays armed. `DecoderStatus.sameDisplay()` compares all but the audible pair, so
  the service re-posts the notification only when what it shows changed, while every running poll tick still feeds the
  screen policy (see "Android screen modes" under Build Targets). Tests: `APP_CONTROL_NOTIFICATION_STATUS` (the v3 field
  order, the pair before any audio and after a note, the unsigned stamp, the reset, only `encode()` arming), and
  `ANDROID_DECODER_STATUS_JVM`, which parses the same golden v3 record.
- Build files: `src/app_control/CMakeLists.txt`

## API

- Path: `src/api`, `include/dsd-neo/api`
- Target: `dsd-neo_api` (built when `DSD_ENABLE_API=ON`, the desktop default; never on Android), linked by the CLI
- Responsibilities: the control and telemetry API, newline-delimited JSON over TCP. Protocol and command reference:
  [docs/api.md](api.md)
  - Public surface: `include/dsd-neo/api/api.h` (`dsd_api_start()`/`dsd_api_stop()` with a `dsd_api_config`). The
    rest is module-private: `src/api/json.h` (the bounded writer/parser), `src/api/api_internal.h`
  - `api_server.c`: accept thread, result-pump thread and one thread per session, all on non-blocking sockets waited
    on with `dsd_socket_wait()`. A session's outbox is appended by the feed and the pump; its thread sends it, keeping
    an unsent tail so no line is ever cut, and reaps itself (the accept thread joins finished sessions and frees their
    slots). Sends never raise SIGPIPE (`MSG_NOSIGNAL`, or `SO_NOSIGPIPE` at accept). A client whose backlog passes
    1 MiB is disconnected, never shown a stream with gaps. An unauthenticated connection is closed at the auth
    deadline, one wrong token or one line that is not JSON ends the connection (the latter keeps a web page's HTTP
    request from driving the port), and the token is compared over its full buffer width
  - `api_feed.c`: a telemetry observer (see "Telemetry Hooks"). On the decode thread it follows the event-history
    rings at every publish (two counters per slot while nothing changes; per-row fingerprints find late enrichment of
    any committed row) and sends the rows pushed or changed since its last look to event subscribers; following them
    whether or not anyone listens is what lets a new subscriber get every row committed after its subscription. An
    event row carries every field a client sees of the row, the call's `freq_hz` and access code included (issue
    #575), and the fingerprint covers the same fields, so a row that learns one in place is sent again. While
    an authenticated client is connected it also encodes the call/status/system/metrics/quality records every 250 ms
    (cached for `get`). The `status` record carries what the stateful
    commands must quote back: the talkgroup policy version, the decryption context and the scan row view. RF metrics
    follow the shared validity rules (app-control behavior note above)
  - `api_commands.c`: one table entry per `DSD_APP_CMD_*` (`API_COMMANDS` reads `commands.h` at run time and fails on a
    command without one), mapping `params` onto the exact payload through the same app-control submit functions the
    frontends use; each entry names its submit function and, for a string command, the longest value its handler
    stores. Values that do not fit their field are refused, never wrapped or cut; 64-bit values convert from
    the JSON literal or a decimal/hex string, never through a double; secret payloads are wiped; `config_apply`
    starts from `dsd_user_config_init()` like the INI loader
  - `json.c` is built with IEEE semantics so its non-finite checks survive fast-math; request lines, parsed strings
    and every buffer are wiped before their storage is released, since requests carry keys and tokens
- Threading: commands run on session threads (the queue is multi-producer); telemetry is encoded on the decode thread;
  `dsd_api_stop()` unregisters the observer (synchronous), stops the result pump and reads the results once more
  (the pump may have slept through the last ones), then lets every session send what is queued (bounded, 1 s; no new
  connections meanwhile) before joining the threads and freeing the server, so the last rows and results reach
  clients and nothing reaches a freed server. The CLI stops it after the frontend runtime, whose close cancels the
  commands still queued, so their cancelled results are among those sent. A connection refused at the client limit
  is told `busy` and lingers (bounded, without blocking the accept thread) so the reply is not lost to a reset
- Tests: `API_JSON`, `API_COMMANDS`, `API_COMMAND_PAYLOADS`, `API_SERVER` (`tests/api/`)
- Build files: `src/api/CMakeLists.txt`

## DSP

- Path: `src/dsp`, `include/dsd-neo/dsp`
- Target: `dsd-neo_dsp`
- Responsibilities: demodulation pipeline, cascaded decimation/resampler, filters, OP25-style CQPSK timing/carrier
  recovery, CQPSK helpers
  (matched/RRC), and SIMD helpers; exposes runtime-tunable parameters consumed by the UI
- Build files: `src/dsp/CMakeLists.txt`
- Sample statistics in `<dsd-neo/dsp/simd_stats.h>` dispatch to AVX2, SSE2, NEON or scalar implementations.
  `dsd_simd_extrema_avg2()` supplies the two-lowest/two-highest averages for the core symbol slicer's window,
  counting duplicate samples. Non-finite values and signed-zero ties use the original ordered comparisons.
  `dsd_simd_iq_abs_stats()` supplies the radio demodulator's mean absolute I/Q sum per complex pair and peak
  component magnitude, with double-precision accumulation. Both accept unaligned windows and exact-sized tails.
  Tests: `DSP_SIMD_STATS` checks each available backend, dispatch, duplicates and exceptional inputs.
- `dsd_simd_extrema_rank5()` (same header) keeps the five smallest (ascending) and five largest (descending)
  samples for `dsd_frame_sync_estimate_window_levels()` (`frame_sync_level.c`), bit-identical to ordered
  scalar insertion: `FLT_MAX`/`-FLT_MAX` seeds fill missing ranks, NaN is never kept, and windows with NaN,
  infinities or -0 run the scalar insertion. Vector backends keep sorted per-lane ranks, then merge lanes with
  a min/max network (high ranks negated so one network serves both). Scalar runs below six samples, AVX2 from
  32 (SSE2 below); the frame-sync caller ranks windows of six or fewer inline and sums ranks as before.
  Tests: `DSP_SIMD_STATS`, `FRAME_SYNC_LEVEL_WINDOW` (full-sort reference, counts 1-48).
- Frame-sync Hamming (`<dsd-neo/dsp/sync_hamming.h>`, `sync_hamming.c`): windows of 8..64 dibits are packed into
  two bit planes (SSE2 16-byte chunks on x86-64, 8-byte words elsewhere) and compared by XOR and popcount; the
  five CQPSK remaps are plane identities (invert `d ^ 2`, xor3 `d ^ 3`, swap, rotate `swap ^ 1`). The single-pattern
  distance uses SSE2 or NEON and otherwise the per-dibit loop, which compilers already vectorize. Other lengths,
  receive bytes outside 0..3 / '0'..'3' and pattern bytes outside '0'..'3' take the per-dibit loops, so every
  result matches the old code. `sync_hamming_internal.h` exposes the scalar and portable backends for tests.
  Tests: `DSP_SYNC_HAMMING` compares each backend with a copy of the old loops on exhaustive and random bytes.
- SNR phase collection (`snr_phase_samples.h`, private to `snr_estimator.cpp`) visits each phase window as
  ordered contiguous spans. It preserves input order, finite-value filtering and the finite-sample cap before
  percentile selection and clustering. IEEE semantics remain enabled for the estimator and its collector tests.
- SNR clustering: percentiles are selected in ascending order, each `nth_element()` over the range above the
  previous pick, which yields the same values as full-range selection. Each k-means pass (`simd_kmeans_internal.h`,
  private to `snr_estimator.cpp`) dispatches to AVX2, SSE2, NEON or scalar double-lane kernels that assign
  samples exactly as the scalar nearest-center rule (strict less-than, lowest index wins) and fold the final
  noise sum into the last pass. Counts match the scalar pass exactly; vector lane sums reorder the double
  additions, so estimates differ from sequential sums by rounding only (~1e-14 dB). The kernels keep IEEE
  semantics. Tests: `DSP_SNR_ESTIMATOR` checks each backend and compares both estimators with the earlier
  scalar algorithm.
- CQPSK short convolutions: `costas.cpp` computes the FLL's four real dot products once and combines them for
  both band edges; filter design keeps the lower taps the exact conjugate of the upper taps. The NCO and feedback
  still advance per sample. `mmse_interp.cpp` applies each interpolated tap directly to both interleaved I/Q
  components, preserving the reversed-tap convention and sample-window bounds. Tests: `DSP_FLL_BAND_EDGE`
  (independent complex convolution over every history position), `DSP_MMSE_INTERP`, and `DSP_COSTAS`.
- Silent inputs and input ends in `dsd_symbol.c` (issue #634). A frame-sync hunt read (`getSymbol()` without sync) waits
  on a UDP input in 100 ms slices (`udp_read_sample_wait` in the network-audio hooks); once the silence has lasted a
  stream pause (`DSD_ANALOG_STREAM_PAUSE_MIN_MS`, the gap the analog receiver already takes for a boundary) and the
  runtime's controls-pending query reports a queued command, or decoder-thread trunking timers are due
  (`dsd_frame_sync_hook_decoder_ticks_due()`: a startup control-channel hunt armed, running or due to arm
  (`p25_sm_cold_start_due()`, `dmr_sm_cold_start_due()`; with trunking turned off too, until the tick that lets it go),
  DMR recovery under way, issue #703, or the NXDN control-channel hunt armed, running or due, `nxdn_cc_hunt_due()`), it
  leaves with no sample and sets `dsd_state::input_interrupted`, which `getFrameSync()` checks after each read and
  returns on at once, with no SPS hunt step and no no-sync hooks. After a carrier boundary or a trunk-scan target switch
  (`dsd_state_note_carrier_change()`) each protocol's matcher waits until the symbols its frame reads back from before
  its sync (`k_frame_sync_tries`, then the matched burst's own before the matcher acts on it,
  `frame_sync_burst_settling()`: the sync window; DMR 90, X2-TDMA data 90 and voice 144, TETRA control 57,
  synchronisation 79 and normal bursts 126 with their buffered pre-sync parts) are all the new carrier's
  (`dsd_state_carrier_settling()`), so no frame decodes the carrier left's buffered dibits on the new carrier, and a
  protocol that reads only forward (P25 Phase 1) loses no frame. A shorter gap, frame decoding, or no queued command
  keeps waiting. The M17 TCP reconnect gives up the same way between attempts, which connect within a bound to the
  address the connection was made to (`tcp_connect` hook, `ConnectBounded()` with no lookup) and back off in slices; "no
  socket" is `DSD_INVALID_SOCKET`. A file or TCP input that ends marks `dsd_state::input_fallback_pending`: frame
  decoding reads zeros until its frame ends (a symbol capture's dibits are erasures then, reliability 0, never its last
  dibit repeated at full confidence), and the hunt's read hands the switch to Pulse to the engine
  (`dsd_engine_input_fallback()`). `dsd_symbol_note_timing_rate()` tells the RTL monitor output's rate rescale which
  rate the timing is in when an input switch returns to the radio. Tests: `DSP_INPUT_INTERRUPT`, `DSP_WAV_INPUT_EOF`,
  `FRAME_SYNC_SPS_HUNT_FALSE_SYNC`. The first crossing a frame records is still pending when the next hunt starts and
  moves the grid on its first symbol; it was measured against the thresholds the warm start set, which are still in
  force, and dropping it was measured worse on faNXDN (issue #729), so it stays.
- Sync anchor (`symbol_anchor.c`, `include/dsd-neo/dsp/symbol_anchor.h`, issue #729). A decoder that reads burst after
  burst without returning to the hunt (DMR BS for a whole repeater call) left the grid open loop, so a TX/RX clock
  offset walked it off the symbols until a sync was lost. `dsd_symbol_note_sync_anchor()` names a sync word the grid
  just read; one symbol later the samples it decided those symbols on are correlated against the word at every
  sub-symbol shift (coarse), refined by a least-squares fit of the word's zero crossings, and the grid moves up to two
  samples (a positive offset consumes extra samples) at the next symbol start, whatever `have_sync` says. State lives in
  state-ext slot 12 (borrowed from the io range), allocated by the first note; samples are recorded only on the live
  multi-sample path and only while `lastsynctype` is DMR. A stream flush, an RTL FSK timing reset, a switch to another
  matched filter (the seam keeps the content position, but the recorded samples were shaped by the old filter), or a
  rewind the seam's history cannot cover resets it, together with a move it queued that the grid took at the start of
  the same symbol; a fractional rate alternating `samplesPerSymbol`, and the group
  delay with it, under the same filter keeps the history (`SYMBOL_MATCHED_FILTER_SEAM` fractional-anchor). Caller: the `dmrBS` loop on every
  exact BS voice or data sync. Tracking range (documented limit): one correction per sync word, so voice on both slots
  (syncs up to ~330 ms apart) tracks +/-50 ppm cleanly; idle or data bursts give a sync every burst. Tests:
  `DSP_SYMBOL_ANCHOR`, `DMR_SYNTH_DRIFT_*`.
- `dsd_state::symbol_clock`/`symbol_clock_epoch` (issue #729): a 64-bit count of every symbol the grid produced that no
  protocol resets (NXDN resets `symbolcnt`), and an epoch DSP bumps on an RTL stream flush or profile change (any reset
  of the RTL symbol cache, also one the profile refresh meets ahead of a take), a symbol-capture replay starting over
  (`-~`), or an input switch (`dsd_opts::pcm_input_generation` moving, one compare a symbol) and the engine bumps on a scan target
  change (`dsd_engine_forget_carrier_decoding()`). Carrier changes are compared on `carrier_seq` by the reader. DMR
  voice crypto measures missed superframes on it.
- `symbol_timing_debug.c`: measures the sub-symbol offset the decoder's symbol grid settled on and reports it once
  per accepted frame sync, behind `DSD_NEO_DEBUG_SYMBOL_TIMING` (see `docs/cli.md`). The sample trace it correlates
  over is filled by `dsd_symbol.c` and owned by decoder-state setup/teardown in `src/core/util/dsd_init.c`.
- Received-tone detection (issues #522 CTCSS, #523 DCS), public entry points in `include/dsd-neo/dsp/analog_rx.h`:
  `dsd_analog_rx_tap()`, `dsd_analog_rx_tap_partial()`, `dsd_analog_rx_block_restart()`, `dsd_analog_rx_reset()`,
  `dsd_analog_rx_carrier_open_now()`, `dsd_analog_rx_carrier_observed_now()` (the tap has judged the channel on air,
  so a closed carrier is a verdict rather than no read yet), `dsd_analog_rx_block_straddles_boundary()`, the monitor
  playback bracket `dsd_analog_rx_playback_begin()` / `dsd_analog_rx_playback_end()` and the live input read bracket
  `dsd_analog_rx_input_wait_begin()` / `dsd_analog_rx_input_wait_end()`. The same header holds the CTCSS timing
  contract in sample time: p95 targets `DSD_ANALOG_CTCSS_LOCK_P95_MS` (400) and `DSD_ANALOG_CTCSS_LOSS_P95_MS` (350) and
  per-event ceilings `DSD_ANALOG_CTCSS_LOCK_CEILING_MS` (700) and `DSD_ANALOG_CTCSS_LOSS_CEILING_MS` (800).
  `DSP_ANALOG_CTCSS` asserts them on every timing row, and a tone policy's acquisition window must exceed the lock
  ceiling by at least 100 ms. Each ceiling sits above the slowest event of the long-run sweeps in `docs/testing.md`
  (1,000,000 starts per condition at 0 dB, 800,000 stops), as the header states; change them only with new sweeps. The
  header also states the measured wrong-tone rates (neighbour locks near 0 dB, talk-off), which a policy acting on the
  first lock has to budget for. A tone off its value by transmitter tone error has its own lock bounds from a carrier's
  start, `DSD_ANALOG_CTCSS_OFF_VALUE_LOCK_P95_MS` and `DSD_ANALOG_CTCSS_OFF_VALUE_LOCK_CEILING_MS` (issue #643), which
  `DSP_ANALOG_CTCSS_TONE_ERROR` asserts on its off-value rows. The DCS timing contract sits beside it, built the same
  way, for the demodulator's DC
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
    moment it does. A CTCSS lock off its value (`dsd_analog_rx_report::off_value`, issue #643) keeps the DCS detector's
    candidate in the merge, so the tone policy's DCS extension runs under it as it would with nothing locked. The front
    end accepts 2400 Hz up to `DSD_ANALOG_RX_MAX_RATE_HZ` (320 kHz, below the ~333 kHz its
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
    (`dsd_analog_ctcss_tables`, built once by `ctcss_tables_build()` in `ctcss_configure()`: the tone's tolerance,
    max(0.8 Hz, 0.5 % of the tone), an encoder's specified accuracy (issue #643), or half the distance to its nearest
    neighbour where that is less -- 0.8 Hz up to 159.8 Hz, 0.5 % from 162.2 Hz up (1.27 Hz at 254.1), 0.7 Hz for 150.0
    and 151.4 Hz, 1.4 Hz apart -- so no estimate is within two gates, and one midway between two snaps to neither; the
    tables also hold each tone's on-value gate, min(0.8 Hz, half that distance), its close neighbour and its noise
    shape, which every hop used to recompute 2,600 times a window, a third of the detector's time),
    rejects aliases (an estimate more than 5 Hz from its bin) and scores rho, the share of the sub-audible band energy
    the tone explains. A tone locks after two consecutive hops qualify it (rho >= 0.35, an estimate within 0.5 Hz of the
    table value, at least 1e-5 (-50 dB) of the raw input's full-band power, which no folded voice-band residue reaches
    and every tone the tests lock exceeds by 24 dB or more, a phase fit whose reduced chi-square against the band's
    own noise stays under 6, estimates within 0.5 Hz of each other, and less than 0.08 of phase-locked second and
    third harmonic power: a voice fundamental has harmonics, a tone does not). A tone of the close 150.0/151.4 Hz pair
    (`ctcss_close_neighbour()`) needs three such hops while the candidate's newest estimate leans more than 0.3 Hz toward
    the other tone (`k_pair_lean_hz`, `DSD_ANALOG_CTCSS_PAIR_LEAN_HOPS`; issue #623): one `ctcss_cand_ready()` test
    for the 250 ms window, the late windows and a lock handed from another tone alike, where noise most often carries an
    estimate of one tone of the pair into the other's acquisition gate. It holds while its own bin's estimate,
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
    from. `DSP_ANALOG_CTCSS` pins tones 0.2 and 0.35 Hz off their table value locking within 400 ms at +10 dB on every
    one of its 200 seeded starts, and 0.2 Hz off at 0 dB within 400 ms on at least 95% of them (all within 500 ms); over
    10,000 starts, 1.55% of 0.2 Hz-off tones at 0 dB take longer than 400 ms (`docs/testing.md`). Transmitter tone error
    beyond that (issue #643: an encoder is specified to 0.5 % of its tone, and the reporter's radios sent 150.6 Hz for
    150.0) gets a second set of rules under one invariant: the on-value rules above -- the detector as it stood before
    -- run first at every step, in their own order, and the second set acts only where they find nothing, so wherever
    it does not fire the output is the old detector's. A hop whose estimate is more than 0.5 Hz off but passes every
    other test qualifies its tone when the window carries the tone throughout (`ctcss_window_stationary()`: every
    sub-block's coherent amplitude at least half the window's median, so an onset, fade or dropout keeps 0.5 Hz) and
    the estimate is within the tone-error gate (`ctcss_tone_error_gate_hz()`): min(0.5 % of the tone, its gate) less
    three standard deviations of the estimate, never under 0.5 Hz. The variance (`ctcss_estimate_variance()`) is the
    weighted fit's slope variance from each sub-block's phase noise, unfloored, the band noise shaped by the
    discriminator's f^2 at the bin, then inflated by the fit's chi-square where that exceeds 1 (against noise floored
    at the tone's own image allowance) and given a 0.01 Hz floor: at 0 dB over 250 ms it is the Cramer-Rao bound,
    0.19 Hz. Both are computed only for such a hop and kept in it (`dsd_analog_ctcss_hop::stationary`, `est_var_hz2`):
    nothing is added per sample. `ctcss_measure()` ranks on-value qualifiers first, then tone-error ones, then the
    rest, and the late windows run the on-value rules over both spans before tone error over either. A candidate counts
    `narrow_run`, the part of its run the on-value rules qualified (`ctcss_cand_main_ready()`); one that is not
    main-ready locks only once the late windows exist, with no on-value candidate of another tone in either acquisition
    window and no on-value rival in the late windows (`ctcss_lock_allowed()`), so a tone the on-value rules are still
    acquiring is never pre-empted. Such a lock is not `main_confirmed` and publishes `ctcss_off_value` until a
    main-ready run of the same tone confirms it; while unconfirmed, `ctcss_step_late()` keeps listening with the
    on-value rules and hands the lock to any tone they confirm. A lock is only ever handed to a main-ready candidate,
    and
    a rival fails the hold only when the on-value rules qualified it this hop. The hold is min(gate, max(0.8 Hz,
    `lock_offset_hz` + 0.3 Hz)), so a tone that locked on its value keeps 0.8 Hz exactly, or the same tone under the
    tone-error gate from this window, or, while the window is one steady tone that misses on frequency alone, the late
    same-tone hold (`ctcss_late_same_tone()`: a late window over the locked bin passes every late acquisition test).
    `main_fail_run` counts the held hops the on-value hold would fail; at `DSD_ANALOG_CTCSS_LOSE_HOPS` the lock is
    unconfirmed again (`main_lost`). A lock the on-value rules do not hold is none for them: on every hop they run
    first as from no lock and lock what they confirm, and its coming and going leaves what they keep alone -- their
    late windows count from their own losses (`main_fresh`), their late run outlives it (`ctcss_cand_narrow()`: the
    agreeing on-value hops at its end), its reverse burst holds off only the tone-error rules
    (`wide_holdoff`), and its loss returns to their own verdict (`main_none`): NONE once they lost a lock or read no
    tone for 500 ms of carrier, ACQUIRING before. A hop only the wider hold keeps
    leaves `locked_hz`, the burst and presence reference, where the on-value rules left it.
    `dsd_analog_ctcss_measure_span()` exposes the precision to `DSP_ANALOG_CTCSS_TONE_ERROR`, which
    pins 150.6 Hz naming 150.0, every tone 0.4 % off, the midpoint naming neither, the precision against what it claims,
    and the cases where an on-value tone must not be pre-empted, nor anything the on-value rules keep be disturbed by
    an off-value lock coming and going. A carrier with no lock after 500 ms of evaluation reads
    `NONE`, on the first hop after it however the input
    is blocked (carrier time is counted per sample), and a tone that starts later still locks. Late acquisition keeps
    lock time in noise inside the lock ceiling: the ring holds 12 sub-blocks, and while nothing is locked each hop also
    measures its newest 600 and 400 ms (`ctcss_step_late()`), over no more than has closed since the last reset or
    loss (`fresh`), so a lost tone is never in them and neither runs before 400 ms have. They apply the same tests with
    rho down to 0.25 (noise alone puts about 1/116 of the band into a bin over 400 ms) and one more: the newest 250 ms
    must still carry the tone at that rho, within its on-value gate (its snap gate for what tone error qualified), which
    keeps a voice that held a pitch near a table tone for most of a longer window and then moved on from locking. Two
    agreeing hops lock, as for the 250 ms window.
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
    `dcs_candidate` stands. An off-value CTCSS tone (`ctcss_off_value`, issue #643) the list does not pass is judged
    only when the window ends (`policy_off_value_reject()`), since the detector may still confirm another value on its
    own rules, and under ALLOWED it starts a fresh window, as a loss does; one the list passes is judged at once.
    `_Static_assert`s hold both windows to the CTCSS and DCS lock ceilings plus 100 ms. After a
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
    moves a rigctl radio without advancing the trunk-tuning generation), on every input switch that ran
    (`ui_input_switched()`, a stop of playback included, a UDP restart through `ui_input_left()`, and the config apply's
    input comparison in `app_command_queue.c`), on
    a config apply that changes the decode mode (the same comparison: out of the analog monitor no monitor block need
    arrive to forget the tone before the row comes back), when the engine falls back to Pulse at the end of a WAV or
    symbol file or after a lost TCP connection (`dsd_engine_input_fallback()` in `engine.c`), whenever
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
  history into a filter without reading an output. Each history sample is stored twice, one tap-count apart,
  so the oldest-to-newest dot product reads contiguous lanes. Apply and prime share the same push; reset and SPS
  redesign clear both copies. `MATCHED_FILTER_SEAM` pins the timing promise and compares wrapped histories with
  independent linear convolution.
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
  pass through the pop. The cache keeps the batch's capture centre too (`rtl_symbol_cache_center_hz`, 0 after a live
  read), which its readers publish into `opts->iq_replay_center_freq` after each sample they take (issue #575; see IO,
  "I/Q replay tuned frequency"). Tests: `RTL_SYMBOL_CACHE_GENERATION`, `RTL_SYMBOL_REPLAY_CLOCK`.
- Stream gaps (issue #518 D5). `dsd_state::rtl_stream_gap_seq` is the stream discontinuity count the decoder has taken
  up to; the engine sets it to the stream's at every stream start (`dsd_engine_note_stream_source()`). A cache fill
  keeps the count its samples carry (`rtl_symbol_cache_boundary_seq`: a replay batch's own, or the stream's read after
  a live read), and the pop answers `RTL_SYMBOL_CACHE_GAP` while it differs, before the first sample of that fill;
  the one-sample reader (`symbol_rtl_read_one()`) checks after every read and holds back the sample that showed the
  gap (`rtl_gap_held*`, dropped if the stream generation moves meanwhile). Either way the decoder takes the count up
  and raises `DSD_INTERRUPT_STREAM_GAP` (`symbol_rtl_stream_gap()`), and `getSymbol()` stands unusable zeros in for
  every read until the engine loop consumes it (`dsd_state::input_interrupt_reason`: a stream gap or a yield is sticky
  until consumed, a Signal search's while the search runs; `dsd_state_raise_input_interrupt()` never lowers a
  reason). The held or cached samples come next. The EDACS analog call loop and the M17 encoder read the stream
  themselves and take the gap up there (the triplet it cut plays as silence and the chain starts over; the block it cut
  is dropped with the input filter's memory), so the symbol reader never raises it again. Tests:
  `DSP_SYMBOL_STREAM_GAP`, `M17_STATE_DISPATCH`, `EDACS_GRANT_TUNE_MATRIX`; bench: `dsd-neo_bench_rtl_consumer` (both
  cached readers and the one-sample reader over a live rtl_tcp stream).
- Reads while the radio has no stream (issue #518 D4). A live read that found nothing while the device has no stream
  and a command is queued returns `DSD_RTL_READ_YIELD` (`<dsd-neo/runtime/rtl_stream_io_hooks.h>`; see IO, "Reads
  while the device has no stream"), its own status beside -1 (a failure) and an empty read: the cache refill answers
  `RTL_SYMBOL_CACHE_YIELD` (both cached readers) and the one-sample reader sees it directly, and either raises
  `DSD_INTERRUPT_YIELD` (`symbol_rtl_read_yielded()`), sticky like a stream gap, so the frame being decoded finishes on
  erasures; nothing asks for shutdown and nothing of the stream is lost. The engine loop consumes it at its next pass
  and its control pump applies the command, with no carrier boundary and no `noCarrier()` (the gap the stream's return
  brings resets the decoding). A search's failed read and an idle NFM monitor's empty read keep their contracts. The
  M17 encoder takes a yield as `M17_STR_READ_AGAIN` (the block read so far dropped; its loop pumps the command). The
  EDACS analog call loop, which holds the decoder until its call ends, asks for `DSD_RTL_READ_NO_STREAM` instead
  (`dsd_rtl_stream_io_hook_read_ex2()`, `want_no_stream`) and never yields: each one pays the realtime since the outage
  began in silent samples at the stream's rate with the squelch flag closed (`edacs_no_stream_pay()`,
  `dsd_state::edacs_no_stream_*`), and a triplet that holds any has no RF power (`edacs_rtl_triplet_pwr()`), not the
  stream's last measurement, so the level squelch's count and the gate's closed run end the call on its own timers,
  plain or rate-converted; it asks for no shutdown. Tests: `DSP_SYMBOL_RTL_YIELD`, `ENGINE_STREAM_GAP` (the engine
  applies the command), `M17_STATE_DISPATCH`, `EDACS_GRANT_TUNE_MATRIX`.
- `dsd_symbol.c` owns the open-loop FSK symbol grid. Only the inter-frame sync search moves it, by a whole sample at
  a time, on the first zero crossing latched in the previous symbol — a bang-bang loop on one unfiltered sample
  index, and between frames the only thing tracking the sampling instant across a call. Issue #444 documents how
  sensitive that is; the comment above `symbol_adjust_timing_nxdn()` records the five ways of damping it that were
  A/B'd on real captures and measured worse, so change it only with `tools/replay_ab.sh` evidence
  (`docs/testing.md`). The CQPSK path does not use any of this: it has a real timing loop in `costas.cpp`.
- The analog monitor's audible output leaves `dsd_symbol.c` in `symbol_output_unsynced_analog()`, after the audio
  chain (`symbol_process_unsynced_audio()`, below) and only while the gate is open; for `audio_out_type == 8` it goes
  through `dsd_udp_audio_hook_blast_analog()` with a byte count of int16 mono samples.
- Analog output at the sink rate (issue #633). Every analog sink runs at `dsd_opts_analog_sink_rate_hz()` (48 kHz):
  the local raw stream, the UDP analog socket and the `-6` raw WAV (`openWavOutFileRaw()`). The monitor runs at its own
  rate (`symbol_analog_audio_rate_hz()`: the RTL output rate, or the effective PCM rate, which only the 8-24 kHz
  divisors of 48 kHz are staged up to), so `symbol_write_unsynced_audio()` and `symbol_write_raw_wav_block()` hand a
  block at another rate to `src/dsp/analog_sink.c` (`<dsd-neo/dsp/analog_sink.h>`). It converts the block with that
  sink's own `dsd_rate_converter` and writes it in chunks of at most 960 samples, one UDP datagram each. A block
  already at 48 kHz is written exactly as before, and nothing is allocated (`DSD_STATE_EXT_DSP_ANALOG_SINK`) until a
  block needs converting. A converter starts over from silence when its sink's previous block was not written
  (`dsd_analog_sink_block_begin()`), where samples were dropped (`dsd_analog_sink_break()`: an abandoned block, a
  receive-family switch, the RTL early returns, a mixed-rate block), when the RTL stream, trunk-tuning or PCM input
  generation moved since its last write, and, for the monitor only, at an announced reception boundary
  (`dsd_analog_sink_note_reception()` from `dsd_analog_rx_reset()`). The `-6` WAV runs on across such a boundary, which
  drops no input, so an acquisition reset in a digital session keeps every sample. A rate the converter cannot take
  writes nothing and logs once. `analog_sink.c` stays apart from `analog_audio.c`, which `DSP_SYMBOL_REPLAY` replaces
  with its own copy. The `-6` WAV is written at int16 scale (issue #643): `symbol_write_raw_wav_block()` scales the
  native and the converted path alike (`dsd_analog_sink_write()`'s `gain`, which the monitor passes as 1.0) by
  `dsd_analog_audio_int16_scale()` of the block's source, 32924 for RTL monitor audio at its 1/pi output scale and 1.0
  for the FSK discriminator output and PCM, and leaves `analog_out_f` to the tap and the monitor; that scale is inline
  in `<dsd-neo/dsp/analog_audio.h>` for the same reason, since the tests that replace `analog_audio.c` keep the writer
  (tests: `DSP_ANALOG_AUDIO`, `RTL_SYMBOL_CACHE_GENERATION`, `DECODE_IQ_ANALOG_RAW_WAV_*`).
  - The converter (`src/dsp/rate_converter.c`, `<dsd-neo/dsp/rate_converter.h>`): the exact L/M where both reduced
    terms are at most 4096, else the closest ratio with terms in that bound (continued-fraction convergents and
    semiconvergents, compared in 64-bit), which is within 250 ppm for every supported rate. Equal rates, or a best ratio
    of 1/1, pass through bit for bit; inputs below 1 kHz, or above 16 times the output, are refused. The taps per phase
    scale with the decimation (`dsd_resampler_design_taps()`: 16 when upsampling, 32 at 96 kHz, up to 256 at 768 kHz),
    so the transition band stays fixed relative to the output. `dsd_rate_converter_process()` refuses an output buffer
    smaller than one input's outputs rather than stall; `dsd_rate_converter_fill()` pulls exactly the outputs asked for
    and keeps the rest, and can tag each output with the tag of the newest input it was made from (EDACS carries the
    squelch's per-sample flags through it).
  - Mixed-rate blocks: `dsd_opts_apply_input_sample_rate()` bumps `dsd_opts::input_rate_generation` at each PCM rate
    change, provisional ones a transaction restores included. A block records the rate it starts at and the PCM rate,
    PCM stream and RTL stream generations (`symbol_analog_block_begin()`); every collected sample compares those three
    fields and, only on a mismatch, the rate (`symbol_analog_block_track_rate()`). A sample at another rate marks the
    block mixed (`dsd_state::analog_block_mixed`). A mixed block, or one whose finishing rate differs from its start, is
    handled as a boundary-straddling block: neither played nor written to `-6`. A rate restored before the next sample
    is read marks nothing.
  - The rational resampler refuses a ratio whose tap count, its byte size or the phase step overflow
    (`dsd_resampler_ratio_designable()`). RTL stream open, retunes and `rtl_demod_monitor_output_rate_for()` decide the
    output resampler through `rtl_demod_resampler_ratio()`, in 64 bits, and a design that fails leaves the resampler off
    with the demod rate published, the rate the samples have.
  - Tests: `DSP_RATE_CONVERTER`, `DSP_RESAMPLER`, `IO_RTL_DEMOD_CONFIG`, `DSP_SYMBOL_REPLAY` (the issue #633 cases),
    `DECODE_PCM_ANALOG_TAP_44K`, `DECODE_PCM_ANALOG_SQL_NOISE_TAP_BURST_44K`.
- Complex I/Q resampler for replays (issue #701): `src/dsp/iq_resampler.c`, `<dsd-neo/dsp/iq_resampler.h>`. It
  resamples interleaved I/Q ahead of the half-band chain when a replay's rate plan cannot reach the mode's DSP rate D by
  decimation alone (IO, "Replay rate plan"). L/M is `dsd_rate_converter_ratio()`'s, as is (exact where both reduced
  terms fit 4096, else within 250 ppm; `DSD_IQ_RESAMPLER_MAX_PPM`). The filter is a Kaiser-windowed sinc polyphase bank
  designed in the file from explicit edges: the protected band is 0.45 x D (the caller's `protect_hz`), the passband
  edge that band or 0.45 of the input rate, whichever is lower, and the stopband starts where input content would fold
  into the protected band at the output rate (or at the input's own first image, upsampling), at least 60 dB down with
  at most 0.1 dB of ripple; taps per phase from the Kaiser estimate rounded up to a multiple of 4, refused past 256 per
  phase or 4096 x 256 in the bank. I and Q share one phase and one set of taps; the output depends only on the samples
  fed since the last `dsd_iq_resampler_clear()`, never on how calls cut them. `dsd_iq_resampler_group_delay_seconds()`
  reports the delay, which replay does not compensate (0.41 ms for 48,254 Hz to 72 kHz, about 2.5 ms for 8 to 72 kHz).
  Only the replay reader thread touches it (`rtl_device.cpp`: configured before the thread starts, cleared at a RETUNE,
  RESET, MUTE and loop rewind, freed after the join); live paths never run it. Test: `DSP_IQ_RESAMPLER`.
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
  talkgroup gate as its playing flag and a reset per call, after the symbol register is read from the raw block; under
  a dynamic squelch it runs the chain per run of heard and unheard samples and silences the closed ones
  (`edacs_process_analog_block_runs()`, issue #625). The M17
  encoder keeps its own band-pass, 960 Hz one-poles (`dsd_voice_onepole`) and AGC at 8 kHz in its stream context
  (`m17_voice_chain`) and runs them over the whole codec2 frame, 160 samples at 3200 bit/s and 320 at 1600
  (`m17_voice_chain_process()`; an explicit `-n` keeps `analog_gain()`). Its input reader low-passes at 3400 Hz
  (`DSD_VOICE_BAND_LOWPASS`, at the input rate) before keeping one sample in `m17_rate / 8000`, and scales RTL monitor
  audio by the RTL monitor gain after the `vol` trim (`m17_encoder_read_block()`), so it reaches codec2 at PCM scale.
  On RTL it reads each sample's squelch flag (`read_ex`); while the stream's gate runs
  (`dsd_squelch_stream_gate_running()`, asked once a block's first sample is in) a closed sample fades to silence before
  the low-pass, and the read records whether any sample was heard (`m17_encoder_input::squelch_heard`). A stream's end
  (`m17_str_end_stream()`, and the packet encoder's) drains a local output after the EOT marker, before its dead air,
  and `m17_str_finalize()` drains again at shutdown, whatever ended the last stream: an asynchronous output drops its
  oldest samples when full, and the engine closes it without a drain. A stream that ends while keyed (a shutdown, or a
  manual unkey while VOX hears a carrier) resets as the idle path's end does, so the next key-up sends a new LSF; a
  monitored stream (`-8`) ends its call there, so the next one begins its own.
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
  past `DSD_SQUELCH_FLOOR_STALE_S` it is learned again, neither kept nor stored for its channel with a fresh stamp. Inside `full_demod()` (`squelch_auto_run()`) the tracker runs on an AUTO setting on a path
  `dsd_demod_dynamic_squelch_path()` names:
  - the analog monitor (`dsd_demod_analog_monitor_active()`);
  - the M17 encoder's monitor (`dsd_demod_encoder_monitor_active()`: `demod_state::m17_encoder_monitor`, set at open,
    FM audio on the WIDE channel);
  - the FSK discriminator at 9600 bit/s, two levels, that EDACS analog voice reads (`dsd_demod_fsk9600_active()`,
    keyed on the symbol profile, so a channel LPF that is off and leaves the WIDE label still counts).

  It runs with a plan for the channel filter in force, the half-band stage
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
  `dsd_squelch_gate_open()`): AUTO turns the level comparison off on PCM input and in digital decoding (the GFSK sync
  skip). Two readers of their own take the gate instead (issue #625), when `dsd_squelch_stream_gate_running()` says the
  stream's status ran a dynamic squelch with a valid plan; elsewhere they fall back to what they do with no squelch.
  - The M17 encoder's VOX counts a 40 ms read heard when any sample was (`m17_encoder_squelch_heard()`,
    `m17_encoder_vox_keyed()`).
  - EDACS analog voice (`edacs_analog()`) reads each triplet's flags (`edacs_collect_analog_triplet_flags()`,
    `edacs_analog_sql_kind()`). It ends the call once the run of closed samples reaches four triplets less
    `DSD_SQUELCH_CLOSE_DELAY_MS` at the chain's rate (`edacs_gate_hold_samples()`), never under
    `EDACS_ANALOG_GATE_MIN_HOLD_MS` (two windows: a call's first decision or one window read closed cannot end it).
    EDACS converts every input rate to 48 kHz (issue #633), and a converted sample takes the flag of the newest input
    sample it was made from (`dsd_rate_converter_fill()`'s tags), so at every rate that ends a dropped carrier no later
    than the level squelch's fifth closed reading. The fallback watchdog runs only when nothing decides the call.

  The gate closes within that delay of a drop: `<dsd-neo/core/power.h>`; `tools/squelch_paths_model.py` measures it,
  and `DSP_SQUELCH_AUTO_DEMOD` pins it. `dsd_state::squelch_edacs_call` marks an EDACS call for the readout. Tests:
  `DSP_SQUELCH_FLOOR`, `DSP_SQUELCH_AUTO_DEMOD`, `DSP_SYMBOL_REPLAY` (`test_auto_squelch_gates_each_sample`),
  `RUNTIME_SQUELCH`, `FRAME_SYNC_INTERNAL_HELPERS`, `EDACS_GRANT_TUNE_MATRIX` (the analog-call cases),
  `M17_STATE_DISPATCH` (the VOX and end-of-stream cases), `IO_RTL_DEMOD_CONFIG`, `IO_RTL_ANALOG_OPEN` (the `-fZ` front
  end) and `DECODE_IQ_M17_ENCODER_VOX_SQL_*`.
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
  as the tracker's, bit-identical whatever the block cuts. Inside `full_demod()` a NOISE setting on the FM monitor (or
  the M17 encoder's, issue #625) with a valid plan arms it in `full_demod_update_channel_state()`
  (`squelch_noise_wanted()`, the level gate standing aside)
  and `squelch_noise_run()` flags each output right after `dsd_fm_demod()`, before the post-decimator, de-emphasis and
  audio filters, which then carry the flags as the tracker's. On an AM channel, one with no plan (8 and 10 kHz NFM,
  low DSP rates, an unfiltered channel at 16 kHz or less), or the 9600 bit/s FSK path EDACS reads (its discriminator's
  output is peak-normalised, not radians), `squelch_auto_wanted()` runs the tracker instead, N as its margin. Its plan follows the same `dsd_demod_squelch_plan_key` as the tracker's, which carries the cascade's pass
  count (designed only when NOISE needs it), and it keeps its own context record (`demod_state::noise_context`), so a
  spell under NOISE never stores the tracker's floor under another channel; a new context or
  `dsd_demod_reset_filter_state()` starts it over, closed. The IO layer's status adds which squelch ran and its quieting
  (`rtl_stream_squelch_status::noise`, `quieting_db`). Tests: `DSP_NFM_NOISE_SQUELCH`, `DSP_NOISE_SQUELCH_DEMOD`,
  `IO_RTL_SQUELCH_PLUMBING`, the `DECODE_IQ_*_SQL_NOISE_*` cases.
- The band-pass bank both noise squelches share is `src/dsp/noise_squelch_bank.{h,c}` (module-private): the order-4
  Butterworth band-pass design (`dsd_noise_squelch_bank_design_band_pass()`), the runners, the per-band ratio and the
  Q = max(Q_sum, Q_max - guard) combine (`dsd_noise_squelch_bank_quieting_db()`); the radio squelch's results are
  bit-identical to before the split. The runtime bank caches plan coefficients alongside transposed direct-form-II
  state in groups of four independent bands (`dsd_noise_squelch_band_group`, owned by each squelch's `bank`). Reset
  clears all lanes and repacks the active plan. Calibration uses the same grouped runner. SIMD spans bands only:
  each cascade, per-sample recurrence, energy accumulation and half-window decision retains its original order.
  `DSP_NOISE_SQUELCH_BANK` compares all bank sizes and partial groups with scalar filters.
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
  refuted (eight of ten runs less than N dB under the stepped reference with a voice band clearly louder than noise's,
  or more than 1.5 dB above it without noise's spectrum, two of them with the voice band moving against the band above
  it, bring that reference back: a carrier relaying noise, then speech, or weak traffic on noise genuinely turned down;
  the stretch then steps down again only on 0.6 s of clean evidence, doubled per refutation), the refuted step kept too
  (cached) until 0.8 s of noise with its spectrum since the last modulation against it takes it back (moved by their
  recent step), a step held over a reference a refutation brought back contested (a voice band louder than noise's by
  the shape tolerances alone, moving against the band above it, refutes it), tracked with a 1 s time constant, learned
  again, as a held step, after 5 s open on one stretch with a steady voice band, and NO_BAND on 1 s of spectral evidence
  that nothing is above voice. Up to eight references are cached by
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
- dPMR FS2 acceptance (`frame_sync_try_dpmr()` in `dsd_frame_sync.c`), a decided cost. FS2 (ETSI TS 102 658 6.1.2,
  5FF77D) is matched as one exact window of 12 sign-sliced symbols, which about one window in 4096 of any carrier's
  payload or of receiver noise passes: about 0.6-0.8 false FS2s a second on noise and on NXDN48 data alike (8 in the 10
  s noise_floor fixture under -fm). Each one reads a 372-dibit dPMR frame, about 155 ms, and stands the NXDN48 matcher
  down for it (`dsd_frame_sync_suppress_nxdn48_sync()`, #374); nothing is published, since the CCH CRC gate (#407) holds
  every call. A four-level read of FS2 (all twelve symbols outer, within two bit errors) would drop those, but is not
  used: the closed-eye off-air `dpmr` fixture reads 5-8 bit errors on its FS2s, the same as noise (5-7), so no threshold
  keeps its syncs (`DECODE_IQ_DPMR_MARGINAL`) and drops noise, and on the scan captures the shorter 2400/4 dwells it
  leaves move DMR and NXDN48 by as much as a 300-symbol dwell change does.
- M17 sync acceptance (`frame_sync_try_m17()` in `dsd_frame_sync.c`). The windows are sign-sliced, so the eight-symbol
  words and the alternating preamble are cheap for receiver noise to reproduce. A preamble candidate needs
  `DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS` (32) consecutive windows within one error, then the LSF or BERT word within one
  error (`frame_sync_try_m17_lsf_or_bert()`); a stream or packet frame after an LSF, and a chain with evidence, are
  taken as before. A transmission joined after its preamble opens on the same exact sync word three frames
  (`DSD_FRAME_SYNC_M17_FRAME_SYMBOLS`, 192) running (`frame_sync_try_m17_join()`, `dsd_state::m17_join_hits`, cleared
  with the preamble candidate and with the NXDN96 sync; every exact word is kept, whichever path took its window,
  since the payload matches some word a few times a frame): the stream word is a stream, the LSF word repeated an
  inverted stream (a transmission sends one LSF), and the BERT word, or the packet word under an inverted session,
  BERT. A packet is never joined (its CRC covers every packet frame; the packet word sits a frame apart in NXDN96 data
  frames' LICH). A join opens one frame and clears the history, so the next frame continues only on M17's evidence
  (`frame_sync_m17_chain_has_evidence()`, a BERT PRBS lock) or waits for three fresh hits. Tests: `FRAME_SYNC_INTERNAL_HELPERS`
  (`test_m17_joins_on_a_sync_word_three_frames_running`, `test_an_nxdn96_data_call_stays_nxdn96_under_auto`),
  `DECODE_IQ_NOISE_FLOOR_M17_NO_SYNC`, `DECODE_IQ_NOISE_FLOOR_AUTO_NO_M17_NXDN96_SYNC`, `DECODE_IQ_M17_MID_STREAM_JOIN`
  (`m17_after_retune`: noise, then a stream landed in mid-frame) and its `_AUTO` twin, `DECODE_IQ_M17`.
- NXDN96 sync acceptance (`frame_sync_try_nxdn()`, `frame_sync_nxdn96_accepts()` in `dsd_frame_sync.c`), after the
  receiver rules of NXDN TS 1-A 5.3 and Table 5.9-3, on the NXDN96 window (`frame_sync_nxdn96_window()`: 4800/4, or a
  symbol capture with NXDN96 alone on). The sign-sliced FSW match (ten symbols, five variants per polarity) passes about
  one arbitrary window in a hundred, so acquisition also reads the window's last ten levels as four-level dibits and
  needs the FSW's 20 bits within two bit errors (`frame_sync_nxdn96_fsw_bit_errors()`, a "clear" hit,
  `DSD_FRAME_SYNC_NXDN96_CLEAR_HIT`). A clear hit acquires with a clear hit of its polarity one frame
  (`DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS`, 192) before it, within `DSD_FRAME_SYNC_NXDN96_TIMING_TOLERANCE` (1) symbol (N1 =
  N5 = 2). N6 (one FSW alone behind the preamble or RCCH post-field tail) is not used: acquiring on the FSW right after
  a preamble left weak calls (`nxdn96_weak`, nxdn96 at 8 dB wideband SNR) reading one FSW symbol wrong on every frame
  and failing every SACCH for the rest of the call. So a transmission's first frame is lost, a trunked voice channel's
  header included (N2 = 1, which reuses the CC's timing, is not implemented). An accepted FSW is pending
  (`dsd_state::nxdn96_sync`, `dsd_nxdn96_sync`) until its frame's LICH is read (`dsd_nxdn96_sync_note_lich()` from
  `nxdn_frame()`): a good LICH makes it the lock, which accepts the FSWs that follow at whole frames from it, the
  tolerance either way, for up to `DSD_FRAME_SYNC_NXDN96_FLYWHEEL_FRAMES` (6) frames -- five missed in a row (N7) -- the
  next frame's on its signs and later ones only clear (N7's two-bit allowance). As TS 1-A 5.3 has it, a LICH whose
  parity fails says nothing: the lock stays on the last good frame, so a call rides out the error and an accept on noise
  never refreshes it. `DSD_NXDN96_SYNC_LICH_WRONG_S1` (S1 = 5) good-parity LICHs in a row that `nxdn_frame()`
  rejects -- a value no NXDN frame uses, or an inbound LICH on a trunked session -- clear the lock and the hits.
  Decided limit: the next frame's FSW is taken on its signs, and the sign variants tolerate a misread symbol at 1, 5 or
  8, so a lock can ride a slicer whose four-level reading is biased for a whole call. Dropping the lock after 2, 3 or
  4 locked FSWs in a row that read more than two bits on four levels was measured on 72 weak nxdn96 realizations
  (0-10 dB): mean RAN frames 28.92, 29.14 and 29.18 against 29.24 keeping it, no run rescued, so the lock is kept.
  Both are also cleared with the M17 join history (`dsd_state_forget_sync_join_histories()`), at an acquisition reset
  and any carrier change (`dsd_state_note_carrier_change()`, so a tune, a probe or a trunk-scan switch), at every
  accepted trunk retune of any protocol (`dsd_engine_trunk_tune_to_freq_request()`, `_to_cc_request()`: an NXDN DISC's
  return included; one the P25 watchdog makes off the decoder thread is applied by the frame sync's next step,
  `dsd_frame_sync_apply_off_thread_tunes()`), at a carrier boundary inside an NXDN frame and at the CAC reset. The
  four-level reading takes the window's level ring, which a symbol capture fills with its dibits' levels
  (`dsd_symbol_level_from_dibit()`), so it applies there too; only a window holding fewer than ten levels (the unit
  tests' sign-only windows) skips it, and the sign match then counts as clear. A LICH failure prints its sync under
  `-Z`. NXDN48 on 2400/4 keeps two hits at any distance. Tests: `FRAME_SYNC_INTERNAL_HELPERS`
  (`test_nxdn96_follows_the_receiver_sync_rules`, `test_nxdn96_lock_follows_the_lich`,
  `test_nxdn96_acquires_on_the_four_level_fsw`), `FRAME_SYNC_DMR_WAV_RATE`, `DECODE_IQ_NOISE_FLOOR_NXDN96_NO_SYNC`,
  `DECODE_IQ_NOISE_FLOOR_AUTO_NO_M17_NXDN96_SYNC`, `DECODE_IQ_NXDN96_MID_FRAME_JOIN` (`nxdn96_after_retune`) and its
  `_AUTO` twin, `DECODE_IQ_NXDN96_WEAK`, `DECODE_IQ_NXDN96_WEAK_0DB`, `DECODE_IQ_NXDN96`.
- P25p1 chain choice on 4800/4 (`dsd_frame_sync.c`, unlocked RTL sessions with P25p1 on). The votes cannot move a P25p1
  signal onto CQPSK, so alternate 4800/4 dwells that expired with nothing decoded try the CQPSK chain
  (`frame_sync_maybe_probe_p25p1_cqpsk()`, `p25_p1_mod_probe_next_qpsk`; the trial's dwell is kept off the votes by
  `p25_p1_mod_probe_active`). The CQPSK chain cannot read DMR, NXDN96, YSF or M17, so when any of them is enabled a
  trial that has validated nothing by half its dwell -- the hunt counter, symbols the search spent, at half the dwell,
  which the budget path checks on every symbol once a pass has run (2700 of 5400; handler-read frames do not count) --
  hands the rest of it back to C4FM with the votes free (`frame_sync_p25p1_cqpsk_trial_half_dwell()`,
  `FRAME_SYNC_HUNT_CHAIN_HANDBACK` from `frame_sync_no_sync_sps_hunt()`: `getFrameSync()` rebuilds its window in place
  and keeps hunting, so neither its no-sync hooks nor the engine's `noCarrier()` after a no-sync return run, since the
  dwell goes on, from the budget path and from the matchless-pass timeout alike, which can reach the point on the same
  symbol; a modulation lock applied live ends a probe or a comparison in flight at the next `getFrameSync()` entry,
  `frame_sync_apply_cli_mod_lock()`, which moves the front end with the decoder, and the handback refuses under a lock
  and on a trunked voice channel the engine tuned, as a full dwell expiry holds the profile there; an accepted tune, on
  either thread, retires a probe in flight, and one that fails keeps it; a lock always wins, so reconciling a watchdog
  tune's notice puts neither the decoder nor the front end on the tune's chain against one); P25p1 alone and a
  trunk-scan target's trial (`frame_sync_trunk_scan_p25p1_trial()`) keep the whole dwell, and the rotation and dwell
  lengths are unchanged. A decoded NID (`dsd_dispatch_handle_p25p1()`) records the chain that carried it
  (`p25_p1_validated_rf_mod`, the learned modulation the hold, the hunt's restore and trunk retunes read). The first
  NID-decoded frame of an acquisition starts one chain comparison (`frame_sync_p25p1_mod_compare_note()`,
  `dsd_state::p25_p1_mod_compare`, run on `getFrameSync()` entry): the symbols that chain's validated frames read over
  one search pass (`DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS`) are added up. P25p1 frames come back to back, so a search
  after a validated frame at least as long as that frame, before the next one or the pass's end, is a frame read by
  nobody (a one-block TSDU reads 134 of its 180-symbol slot, and the search to the next is 46); a pass without one keeps
  the chain at once, a pass with one puts the other chain on for a pass of its own, and the chain that read strictly
  more keeps the profile and becomes the learned modulation, with a verdict that stops a frame on that chain opening
  another comparison. The other chain's pass is refunded to the dwell (the counter goes back to its value when that pass
  began).
  - The other chain's pass starts at a `getFrameSync()` entry and is one search pass long, so on a carrier that chain
    cannot sync it runs out on the symbol the matchless-pass timeout fires on. It ends there instead, on whichever runs
    out first, its symbols read or the call's symbols searched (a read that commits no symbol counts only in the
    second) (`frame_sync_p25p1_mod_compare_trial_pass_ended()`): `getFrameSync()` rebuilds its window in place and keeps
    hunting on the chain the comparison chose, as after a CQPSK trial's handback, so neither its no-sync hooks nor the
    engine's `noCarrier()` run on a carrier the incumbent still decodes (an LSM carrier the C4FM chain is deaf to).
  - A verdict needs the winner to have validated a frame in its own pass: an incumbent whose only frame opened the
    comparison (the transmission ended, an isolated NID) ends it with no verdict and nothing learned. A verdict is
    dropped with the learned modulation (a CQPSK dwell that decodes nothing, an acquisition reset that forgets it), by
    a 4800/4 dwell on its own chain that decodes nothing (so a C4FM verdict cannot outlive its carrier), and is kept per
    trunk-scan target (the snapshot records the chain leading so far, `dsd_frame_sync_p25p1_learned_rf_mod()`). A
    watchdog tune's notice still pending at a switch is reconciled into the target being left before its snapshot is
    saved (and before the rollback snapshot and the targets' build), so no target's learned modulation takes
    another's tune.
  - A comparison ends without a verdict at a carrier boundary (`carrier_seq`; the leading chain is put back), a
    profile change, a modulation lock or a trunked voice tune (the demodulator is theirs; the learned modulation goes
    back to the leading chain), a dwell expiry that overtakes it (put back on the leading chain before the hunt moves;
    the expiry still judges the chain the dwell ran on), and `dsd_frame_sync_reset_acquisition()` (a trunk-scan switch,
    a row or input boundary; the caller owns the demodulator and the learned modulation). The leading chain is the
    incumbent unless the other chain has read more. Each of those boundaries, and a tune on either thread, also
    consumes the pending NID stamp (`seen_symbolcnt`) with no comparison running, and a stamp read across a
    `carrier_seq` change or with no frame read since the mark opens none (`seen_carrier_seq`): no comparison opens
    on the carrier reached from the carrier left's last frame.
  - Tunes: `dsd_engine_trunk_tune_to_freq_request()` and `dsd_engine_trunk_tune_to_cc_request()` (the latter also
    under `dsd_engine_return_to_cc_request()`, when it retunes; one that retunes nothing leaves the comparison going
    and does not apply the control channel's timing under it) end a comparison in flight on the leading chain before
    they read the modulation (`dsd_frame_sync_p25p1_mod_compare_end_for_tune()`, `frame_sync_p25p1_compare.c`, which
    the engine tests link), so an FDMA grant, which inherits `rf_mod`, and a control-channel tune, which reads the
    learned modulation, never land on a chain only because its pass was running. A Phase 2 grant has already put
    `rf_mod` on CQPSK and named its slot (`p25_p2_active_slot`), and that modulation stays the tune's; only the learned
    modulation goes back. A tune that fails or is deferred puts the comparison back as it was, with the front end's
    profile captured before the comparison ended, so the decoder and the front end stay on the same chain.
  - The comparison is the decoder thread's: it runs in the hunt, outside the P25 SM tick guard, and a tune the P25
    watchdog's tick makes on its own thread never touches it. The invariant: an off-thread tune makes one coherent read
    of the decision the decoder publishes (`dsd_frame_sync_p25p1_read_decision()`: one atomic value, whether a
    comparison runs and the learned modulation, the leading chain while one runs, published at every comparison step and
    every write of the learned modulation by `dsd_frame_sync_p25p1_mod_compare_publish()`), taken when its outermost
    tune request begins (`dsd_engine_tune_decision_begin()`), and uses that one read both for the chain it tunes
    (`rf_mod`, the control channel's chain) and for the notice it posts (`dsd_frame_sync_note_off_thread_tune()`, which
    carries the read with the count, in one atomic value). The decoder thread reconciles every notice, whatever the
    comparison did since (`dsd_frame_sync_apply_off_thread_tunes()`, at `getFrameSync()` entry, in the hunt while a
    comparison runs, before the NXDN96 and M17 matchers read their histories, and before a decoder-thread tune decides):
    the learned modulation becomes the chain the latest tune set, a comparison still running (or opened since) ends
    there without a verdict, a verdict reached since on a comparison the tune captured is dropped (it measured the
    carrier left, and rewriting it to the tune's chain would claim a measurement no comparison made and stop the new
    carrier being compared), and the sync join histories go. A run boundary seeds the decision from the new session's
    state and drops the notices posted so far (`dsd_frame_sync_off_thread_tunes_reset()`, at the run's start before the
    watchdog starts, and at its end after it stops). The serialization rule: the watchdog tunes under the P25 SM tick
    guard, and the decoder takes the same guard (`dsd_frame_sync_hook_tick_guard_enter()`, re-entrant, installed by the
    engine) around every step that changes the comparison or the chain -- opening it, a pass's end and its chain switch,
    a settle or an abort, a CQPSK trial's handback, a dwell expiry's probe, an acquisition reset, and reconciling a
    notice -- and reconciles any notice still pending inside the guard before it acts
    (`frame_sync_p25p1_chain_step_begin()`), standing down when one was. So no tune lands between a step's decision and
    the chain it installs, and a reconciled notice puts `rf_mod` and the front end back on the tuned chain
    (`frame_sync_p25p1_set_chain()`), which repairs a state already wrong. The guard is taken only on those steps, which
    run while a comparison or probe is in flight, at a dwell expiry, or at a reset; the per-call checks are an atomic
    load and a phase read. The publication stays: not every writer of the learned modulation runs under the guard (a
    trunk-scan snapshot restore, a decoder-thread tune), so the one coherent read stays a read of the published value.
  - Decided limit: "read" counts a frame once its NID decodes, even when its TSBK trellis or CRC then fails, and a
    gap is judged against the frame before it, so a one-block TSDU missed after a three-block one (a 226-symbol search
    against 330 read) does not count as missed, and a conventional call is judged at about one LDU of resolution. The
    TSBK/MPDU success count (`p25_p1_fec_ok`) would be finer for control channels, but it says nothing about voice
    frames (HDU, LDU1/2, TDU), so the NID stays the one check that covers every DUID.
  Without the comparison, a chain that reads part of an LSM simulcast carrier (C4FM: about half the 769 MHz control
  channel's TSBKs) kept the profile for as long as the carrier stayed up. Tests: `FRAME_SYNC_INTERNAL_HELPERS`
  (`test_p25p1_partial_chain_loses_the_comparison`, `test_p25p1_comparison_ends_cleanly`,
  `test_p25p1_cqpsk_trial_leaves_half_the_dwell_to_fsk_modes`),
  `ENGINE_TRUNK_RETUNE_REGRESSION` (`test_chain_comparison_in_flight_tunes_on_the_leading_chain`), `ENGINE_TRUNK_SCAN`,
  `DECODE_IQ_P25P1_CQPSK_SIMULCAST_CC_AUTO`.
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
  block's zero-symbol count, metrics, and the block a profile request is consumed on. A replay's block is therefore
  sized like a live one (I/Q replay framing below): one live transfer at most, so those decisions run at a receiver's
  cadence in a replay too (issue #626). Holding the look-ahead is
  latency: a filter emits an output only once it holds the c inputs after it, about 74 samples (1.54 ms) at 48 kHz on
  the default 1.536 Msps RTL chain, about 7 in the five-pass cascade and 67 in the 135-tap FIR, and 67 (1.40 ms) on a
  48 kHz replay, which runs no cascade.
  - Channel FIR, in complex samples, with c = (taps - 1) / 2: a call makes max(0, pending + N - c) outputs, output k
    centred at `hist_len - pending + k` of [history | block], and leaves pending + N - outputs pending with the newest
    `hist_len` inputs in the history. No variant pads the look-ahead past a block's end; an invalid call or a short
    `out_cap` returns -1 with nothing touched. Scalar is bit-exact across splits; the SSE2, AVX2 and NEON kernels (and
    the dispatcher, which keeps blocks shorter than both 16 complex samples and the filter on scalar) agree with
    their own whole-stream output within 1e-5.
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
- Signal discovery (`radio/signal_measure.cpp`, `io/signal_measure.h`) uses an independent
  PFFFT/Hann cache at the capture-rate I/Q boundary. Linear channel power and neighboring
  noise are energy-normalized over two sample-count windows (local noise: the mean of the
  sideband blocks within 6 dB of the third lowest of eight). Session/request IDs and
  applied generation accompany every report; gain, rate, drops, tuning and input changes
  invalidate partial and ready reports. Leakage rejection runs once per request on both
  windows' power: transmitters are located in the spectrum (not just the requested
  channels), each from the anchor its channels' windows climb to, centred where its
  segment best overlaps its own mirror image, with segments cut and merged at valleys and
  weak centres by comparing one source with two (refined on the FFT bins of the whole body
  with aggregated bins); transmitters beside each are found by a valley rise above every
  skirt ridge passed, 10 dB above the mirrored far side (rising from a dip, up to the first
  other transmitter) or, within its width, an unmirrored rise beyond a 20 dB valley, and
  located once (as a source of their own where their segment allows). A channel keeps a
  raw detection only for a transmitter centred within its half-open interval on the
  range's rational grid that holds its own power in both windows (within 10 dB).
  `docs/signal-search.md` lists the measured limits. The tap's `feed_mutex`
  serialises the detector (feeding and analysis, request, cancel); `tap_mutex` guards only
  the published capture, readiness and report, so polls never wait for analysis; locks
  are taken feed first. The tap describes only the stream feeding it: once the demod
  thread has joined, the stream's soft stop resets it (`rtl_signal_measure_reset()`), so
  no capture, latest-sample time or request outlives the stream. A search started on the
  next stream before its first block then waits for that block under the startup
  deadline instead of reading the stopped stream's age as a stall. Search bypass
  preserves capture/control health while skipping demodulation, protocol/audio work and
  display FFTs. Decoder-dependent
  gain/PPM supervision is suspended during discovery. `rtl_stream_tune_async()` and
  `rtl_stream_tune_poll()` reuse the controller's completion ledger and settling gates;
  the search owner reserves tagged manual tunes, including against unsolicited UDP tunes.
- View rings and SNR fallbacks (`radio/rtl_view_rings.{h,cpp}`): the constellation and eye rings (relaxed-atomic
  samples, demod-thread appends, retune clears) and the fallback estimates over them behind
  `rtl_stream_estimate_snr_{c4fm_eye,gfsk_eye,qpsk_const}()`. Every append and clear is one write batch (a writer
  count raised for it, a batch count bumped when it ends). Each estimate caches per thread, keyed by batch count, TED
  SPS, output rate and channel profile, and returns exactly a fresh estimate's answer; a read a batch raced is answered
  (relaxed, as before) but never cached. The decoder polls them per symbol, the rings change per block.
  `IO_RTL_VIEW_RINGS` checks cached against fresh answers.
- CF32 input levels (`runtime/input_level.c`): finite checks, peak and clipping stay in the input's float precision;
  `0.98f` is the first float at or above the double clipping threshold. Ordered double sums retain the original
  variance calculation. `IO_INPUT_LEVEL_METRICS` checks exact output bits against a scalar double reference across
  threshold neighbors, non-finite inputs, DC and mixed-exponent blocks.
- Automatic PPM (`radio/rtl_sdr_fm.cpp`, `radio/rtl_auto_ppm.cpp`): each live read synchronizes the applied PPM and
  frequency. While disabled, the controller only clears training and publishes its status; the reader skips the
  unused clock, signal estimates, configuration and request-lock snapshots. Enabled updates gather the full inputs.
  `IO_RTL_AUTO_PPM` compares every output field through disable, repeated disabled updates and re-enable with full
  versus minimal disabled inputs; `dsd-neo_bench_rtl_consumer` covers the per-sample monitor cost.
- Source kind per read: a prepared stream records whether its private `opts` copy names an I/Q replay
  (`RtlSdrInternals::source_replay`), so `stream_is_replay_active()` (every decoder read and replay clock step) no
  longer parses `audio_in_dev`.
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
    socket (issue #526), and nothing is sent where the peer was never asked anything or last accepted FM at its own
    passband, a reconnect since or not (the unconfirmed cache re-sends a passband, but a return has nothing to undo).
    SDR++ and GQRX take a passband of 0 as "unchanged", and SDR++ saves every passband it is sent, so the undo sends the
    peer's own passband explicitly: `SetScanRowModulation()`, a passband this client sets (a scan row's own request, or
    since issue #621 the session's: its configured width, the AM default, or `-B` standing in for an unset NFM width)
    or the `-B` a digital mode is heard through on audio input (`dsd_rigctl_passband_captures()`), first asks the peer
    what it runs (`m`, switching a peer on the other demodulator to this one at passband 0 to read that one's, and
    reading that other demodulator's passband in passing), and `RestoreScanModulation()` puts back each passband a row
    changed, the other demodulator's first, before the session's request; with `session_passband` (a request
    `dsd_rigctl_passband_captures()` names) that request goes through `SetScanRowModulation()` too and the reading of
    the session's demodulator stays, whatever the answer. A peer that cannot answer `m` gets passband 0, best-effort.
    The record (`rigctl_peer`; its state model is the comment above the struct) keeps per demodulator the peer's own
    passband as read (`own_hz`, 0 when not known), who that reading serves (`reading`: none, a scan's, or the
    session's) and whether a passband this client asked for stands on it (`changed`). A scan's reading lasts the scan,
    whose restore resets it, except for a demodulator whose undo the peer refused or lost. A reading that serves the
    session's passband (kept by a restore with `session_passband`, or asked off a scan and marked by
    `RigctlMarkSessionPassband()`, no I/O) is retired once the peer is known to run its own passband
    (`rigctl_retire_session_reading()`): it accepts that reading back (`SetModulationKind()` at 0,
    `RevertModulation()`, the restore's undo), or it refused the session request while on its own
    (`RigctlMarkSessionPassband()` after a refusal); the other demodulator's reading the same lookup took in passing,
    which nothing of this client's holds, is retired with it. "The peer runs its own passband" is one rule,
    `rigctl_runs_own_passband()`: the last request it accepted left it on that demodulator at 0
    (`rigctl_peer_left_on_own()`, which also says when FM at 0 has nothing to undo), with a reading behind the 0, a
    reconnect since or not. A retired reading is never sent again as a return: the next change reads `m` again, since
    the operator may have changed the peer's passband meanwhile, and a return before that sends 0 where the peer runs
    the other demodulator (a read that fails leaves the passband not known the same way). One exception undoes a
    retire: a tune that fails after its request returned the peer to its own passband puts back the passband this
    client had set (`RevertModulation()`), which undoes that return, so the reading it retired (`retired_hz`) is
    reinstated for the session, the other demodulator's included; a revert whose reply was lost reinstates too, one the
    peer refused leaves the return in force. A return's holds live only until the next request sent on the socket,
    for either demodulator and whatever the answer, so a revert brings back only what the request it undoes (the tune's
    last) retired, and it consumes them whichever way it goes; a reading retired on a refused session request (the
    mark) is not held at all. A scan row's taken request makes a session reading the scan's, a same-endpoint rebind
    keeps everything, and a return sent as 0 retires nothing. `Connect()` starts the record empty for a connection on
    the number of a closed socket and leaves it alone for one on another number (the TCP audio input's reconnect while
    the rigctl socket stays open), and
    `SetModulation()` is the FM call. `RigctlRebindPeer()` hands a rigctl reconnect, opened while the socket it replaces
    is still open, what that socket knew of its peer (issue #589), and forgets the frequency `SetFreq()` last sent,
    since a later connection can get the closed socket's number back: the record for the same host and port, so the FM
    undo and the scan's restore still send the peer's own passbands and a failed tune still puts back what the peer last
    accepted, though no request is taken for one it already runs until it accepts one (it may have restarted or been
    changed meanwhile); for another endpoint while the old peer may run AM (an am row's, or either demodulator after a
    lost reply), a demodulator not known, so FM is sent first and a refusal fails a best-effort tune; otherwise, and
    where no request may have changed the old peer, nothing, a peer nothing was asked of as on a first connection, so
    one that refuses mode requests does not fail every tune. No own passband of the old peer reaches another endpoint,
    and the record names the new socket afterwards, never the closed one whose number a later connection may get back.
    Both rigctl reconnects, the '9' key (the TCP input's host at the rigctl port) and the menu's host and port, go
    through `svc_rigctl_connect()` (`src/app_control/rigctl_connect.c`): it connects, then, under the P25 SM tick guard
    since the watchdog's retunes use the socket, rebinds, closes the old socket without sending it anything (the old
    peer keeps what a scan last set on it) and forgets the engine's legacy tune cache; a connect that fails while rigctl
    is on changes nothing, so the connection in use and its record stay. The connect then asks the new peer for the
    session's width (`dsd_engine_rigctl_ask_session_passband()`, under the same hold). The engine names the record for a
    run's first connection the same way (`RigctlRebindPeer()` with no old socket) before the P25 watchdog starts. The
    engine's rigctl legs ask for what `dsd_rigctl_passband_of()` (runtime) gives: a request
    `dsd_rigctl_passband_captures()` names (a passband this client sets, or `-B` on a digital mode of audio input)
    through `SetScanRowModulation()`, anything else (`-B` on an input the peer does not demodulate, the peer's own
    passband) as before (`dsd_engine_rigctl_send_passband()` in `trunk_tuning.c` for the scan legs). Which requests are
    the session's, `dsd_channel_modes_rigctl_request_is_session()` (core) decides for every leg, the engine's and the
    manual tune's alike: what the scan scope says (`dsd_scan_mode_rigctl_request_is_session()`, given the state
    wherever the caller has one), except that a typed `-Y` list configured before its first row's scope is entered
    counts as a scan (`dsd_channel_modes_present()`, which runtime cannot see), while an untyped list's requests are the
    session's. The tune leg (`dsd_engine_tune_rigctl_modulation()`) asks a peer that demodulates audio input
    (`dsd_opts_rigctl_peer_demodulates()`) for the row being tuned: a width (the row's own, the configured width of its
    kind, the AM default) fails the row's tune when the peer refuses it, and anything else, `-B` on the FM monitor
    included, is best-effort unless the peer refuses it while on an am row's AM or on a demodulator not known
    (`CachedModulationKind()`, no I/O), which fails the tune; on an RTL-family input, where DSD-neo demodulates, it
    always asks for `-B` and no refusal fails the tune. This leg marks nothing for a tune that sets a passband, a scan
    row's (a trunk grant runs a digital mode); a tune that follows with `-B` on audio input is captured too, and is
    the session's or a row's as `dsd_channel_modes_rigctl_request_is_session()` says (a trunk grant on a plain session
    is marked; a typed `-Y` list's first row, tuned before its scope is entered, is a row's). The live apply
    (`dsd_engine_scan_rigctl_apply_modulation()`, a "this channel" edit and
    app-control's follow) asks for the row whose scope is in force, strictly whatever the request, returns -1 unless the
    peer demodulates (symbol and null inputs are not peer inputs), and marks the request the session's when
    `dsd_channel_modes_rigctl_request_is_session()` says so. `dsd_engine_rigctl_ask_session_passband()` asks for a width
    only (`dsd_rigctl_passband_is_width()`; `-B` and the peer's own passband wait for a tune), best-effort, marking it
    the session's off a trunk scan or a typed `-Y` list (an untyped list's is the session's): once the input is open at
    start (`liveScanner()`, before the lifecycle starts the frontends and before the P25 watchdog starts), from
    `svc_rigctl_connect()`, and from the engine's fallback to Pulse when it brings a peer to demodulate the input
    (`dsd_engine_input_fallback()`, under its tick guard), each of which passes the decoder state. A rigctl tune that
    fails after its modulation request changed what the peer runs puts that back (`CachedModulation()` before the
    request, `RevertModulation()` after), so the row still on air is not heard through the failed row's demodulator or
    passband; where no passband was known before the request (the first row on a fresh socket, or after a lost reply),
    the peer's own passband that the row's request read is what goes back. The leg reads a `-Y` row's own width from the
    row being tuned (`dsd_engine_scan_tuning_row_options()`, `scan_analog_internal.h`), since the prepared settings in
    force cannot tell it from the configured one. `dsd_engine_scan_rigctl_restore()` (`trunk_tuning.h`) calls
    `RestoreScanModulation()` with what the restored session runs, `session_passband` when
    `dsd_rigctl_passband_captures()` names it (a passband this client sets, or `-B` on a digital mode of audio input),
    once `dsd_engine_channel_scan_leave()` has left a `-Y` map or a trunk-scan target list, so neither an
    am row's AM nor a row's passband outlives the scan. After an I/O failure the socket's cache matches no request,
    since what the peer runs is no longer known (a lost reply to a request for the other demodulator leaves which one it
    runs not known either, `DSD_RIGCTL_KIND_UNKNOWN`), and a demodulator whose undo the peer did not accept keeps the
    own passband read for it, so the next undo still sends that rather than reading the row's back as the peer's own.
    The engine's legacy rigctl leg (`no_carrier_tune_rigctl_if_needed()` in `engine.c`: the untyped `-Y` step and the
    direct control-channel return) skips a repeat of the frequency it last sent, and so does its RTL twin
    (`no_carrier_tune_rtl_if_needed()`). Both caches hold only until the trunk tuning hooks issue another request
    (`dsd_trunk_tuning_last_request()`): a grant or a control-channel hunt moves the radio without writing them, and a
    return to the cached frequency after it must be sent (`ENGINE_NO_CARRIER_RESET`). An accepted direct return records
    the control channel in `opts->rtlsdr_center_freq`, as a hook tune does, so a control message decoded next is not
    attributed to the voice channel. The rigctl cache holds for one connection:
    `dsd_engine_rigctl_tune_cache_forget()` (`trunk_tuning.h`) empties it when rigctl reconnects (issue #589), since the
    new connection's peer, another one or the same one restarted, was sent nothing. The leg keeps no copy of the `-B` it
    sent (issue #621): it asks for `-B` at every step, strictly as always (through the capturing call on audio input,
    where the peer's FM passband is what a digital mode is heard through), and the client's peer record skips a repeat
    (`SetModulationKind()`), the record every writer updates (a live width, a scan's restore, a manual tune) and
    `RigctlRebindPeer()` hands a reconnect, where a private copy would go stale. On the analog monitor of audio input
    (only the `-Y` step runs there) it asks for the session's passband through `SetScanRowModulation()` instead, marked
    the session's (the untyped list holds no scope and has no leave restore), and a refusal abandons the step as a
    refused `-B` always did. It marks as every leg does (`dsd_channel_modes_rigctl_request_is_session()`), so a
    direct control-channel return under a typed `-Y` list configured before its first row's scope is a scan's. Where
    the session runs the peer's own passband (`-B` 0, or the FM monitor with neither a width nor `-B`) it asks for FM
    at 0, best-effort: that undoes a passband this client set (a session width before a switch to a digital mode, say)
    with the passband read before it, and is a no-op for a peer this client never changed or one back on its own. A
    manual tune (`io_control_set_freq()`, which takes a `const dsd_state*`, as does `set_rtl_frequency()`, and whose
    rigctl leg `set_rigctl_frequency()` reads it to tell a scan's request from the session's, through
    `dsd_channel_modes_rigctl_request_is_session()` as every leg) asks the same way before the frequency: the session's
    passband, or `-B` on a digital mode of audio input, each captured, a refusal failing the tune before the frequency
    moves; `-B` as before, strictly, on an input the peer does not demodulate; and FM at 0, best-effort, where the
    session runs the peer's own. Tests:
    `IO_RIGCTL_CONTROL`, `ENGINE_TRUNK_RETUNE_REGRESSION`, `ENGINE_NO_CARRIER_RESET`, `ENGINE_RUN_SETUP` (the start
    ask), `ENGINE_TRUNK_SCAN_SCAN_LIST` (a mixed scan's wire sequence: a scan's reading lasts the scan).

Key public headers:

- RTL stream C API: `include/dsd-neo/io/rtl_stream_c.h`
- RTL C++ orchestrator: `include/dsd-neo/io/rtl_stream.h` (class `RtlSdrOrchestrator`)
- Native Airspy adapter: `src/io/radio/airspy_source.cpp` owns SDK calls and USB lifecycle;
  `rtl_device.cpp` connects its CF32 callbacks to the shared generation-checked input ring.
  Value-only settings and identity/rate snapshots use `core/airspy_config.h`.
  The vendored Android libairspy's stop (0001-libairspy) waits until libusb has called back every cancelled transfer,
  so `airspy_close()` and the restart after a retune never free or resubmit one still in flight; system libairspy
  keeps upstream's single poll. Test: `VENDORED_LIBAIRSPY_TRANSFERS`.
- RTL device/config/metrics: `include/dsd-neo/io/rtl_device.h`, `include/dsd-neo/io/rtl_demod_config.h`,
  `include/dsd-neo/io/rtl_metrics.h`
  - USB reader exit (`dongle_thread_fn()` in `rtl_device.cpp`): any `rtlsdr_read_async()` return with no stop in
    flight (the device's `run` flag, which a stop clears before it cancels the reader, and the exit flag) is a device
    failure, whatever its code: -1 for a lost device (unplugged, or librtlsdr gave up after repeated transfer errors;
    the vendored Android librtlsdr also once no transfer is left, since its patch 0003), a failed submission or event
    poll, even 0. It goes through `rtl_device_end_for_receiver()`, the one path that ends
    a session for its receiver: after the unplug-owner grace (`rtl_device_set_unplug_owner_grace_ms()`, process-wide,
    default 0; the Android host sets 2000 ms so its USB detach broadcast can own an unplug, and a stop or exit request
    inside the grace keeps the failure silent), it latches `DSD_INPUT_FAILURE_DEVICE` with that code, commits the
    receiver ending (`dsd_input_failure_receiver_ended()`) and ends the session (exit flag, input ring waiters woken).
    `-ENOMEM` (librtlsdr could not allocate its USB transfers and submitted none; the vendored Android librtlsdr
    returns it since `android/third_party/patches/0002-librtlsdr-check-async-buffer-allocation.patch`) is the host's:
    reported at once, without the grace or the committed ending. Stop handshake (`rtl_usb_reader_stop()`, shared by
    `rtl_device_stop_async()` and `rtl_device_destroy()`): clear `run`, then retry `rtlsdr_cancel_async()` every
    1 ms until the reader published `reader_exited`, then join. librtlsdr refuses a cancel until the reader streams,
    so a single cancel lost a stop that landed while the reader was starting and the join hung; the reader checks
    `run` after publishing `reader_entered` and before it calls `rtlsdr_read_async()`. The vendored librtlsdr's
    teardown (0003) frees transfers only once libusb has called each back; system librtlsdr keeps upstream's, so
    desktop builds get this exit rule, the handshake and the progress monitor but not the safe teardown. A stop that
    lands between a verdict's last check and its report is still reported and committed; on Android the stop's cause,
    latched first, decides what the UI shows. Tests:
    `IO_RTL_READER_EXIT`, and `VENDORED_LIBRTLSDR_ASYNC_ALLOC` and `VENDORED_LIBRTLSDR_TRANSFERS` for the vendored
    allocation path and transfer accounting.
  - USB sample-progress monitor (RTL-SDR and Airspy; SoapySDR's is below; replay and rtl_tcp have none): a receiver can
    stay enumerated and stop delivering with nothing returning (system librtlsdr drops a transfer for good on each error
    status and waits forever once none is left, where the vendored copy returns -1; a device that stops completing
    transfers, which have no timeout, leaves either SDK waiting). `rtlsdr_callback()` counts each callback that carries
    bytes (a stalled transfer can still complete with none, which is no progress) and the Airspy source's receive
    callback each callback, as their first statement (one relaxed increment, no clock read: `rtl_device::progress`,
    `airspy_source_progress()`). A monitor samples it every 100 ms on `dsd_realtime_mono_ms()`: for the RTL-SDR its own
    thread, started by `rtl_device_start_async()` after the reader and stopped and joined in `rtl_usb_reader_stop()`
    before the cancel loop; for the Airspy the device thread, `airspy_monitor()`. Silence accrues by the time between
    samples while the stream is up (the RTL reader inside `rtlsdr_read_async()`; the Airspy not inside a retune or rate
    change, `airspy_source_reconfiguring()`, and the time a sample waits on the source's lock is not counted), does not
    reset when the stream restarts (`airspy_source_generation()` counts restarts, for the log only), and only observed
    progress ends it. 5000 ms of it (`RTL_PROGRESS_SILENCE_LIMIT_MS`; the 100 ms period is `RTL_PROGRESS_PERIOD_MS`;
    both are constants that only internal test hooks shorten) ends the session with `-ETIMEDOUT` through
    `rtl_device_end_for_receiver()`, so the grace applies, and cancels the RTL reader; an RTL-SDR eligible for stall
    recovery is handed to its stream's controller instead, within its budget (see "USB stall recovery" below). The
    Airspy's "stream stopped" verdict (`-1`) takes the same path. Every verdict carries the progress count it was made
    at; during the grace only progress since then drops it, never a restart or an SDK that says it streams, and the
    deadline never restarts. Of verdicts that race (a reader exit and the monitor), the first reports
    (`receiver_end_claimed`). Tests: `IO_USB_PROGRESS_MONITOR`; bench: `dsd-neo_bench_usb_ingest`.
  - SoapySDR stream loss: the reader (`soapy_read_loop()`) counts every read with samples in `rtl_device::progress`
    before a retune mute or a reconfigure hold can drop them (timeouts, overflows and empty reads do not count), and
    publishes `reader_entered` once its stream is active and `reader_exited` once it left its loop. Its own monitor
    thread (`soapy_progress_monitor()`), started by `rtl_device_start_async()` before the reader (a monitor that cannot
    start fails the start) and stopped and joined by `rtl_device_stop_async()` and `rtl_device_destroy()` before the
    reader, applies the rule above: silence accrues while the reader streams and no control or setup transaction holds
    `soapy_lock` (`soapy_control_hold`, set once the lock is held by `soapy_call_locked()`, the stream setup and its
    cleanup, never by a read, so a read that hangs or keeps timing out accrues), and the verdict waits out the grace on
    the monitor while the reader keeps reading. An end of the reader nobody asked for reports once
    (`soapy_reader_finished()`): a failed setup or buffer allocation at once as a device failure without the grace or
    the committed ending (as `-ENOMEM` above); internal read-buffer, mute or lock failures take the same immediate
    path. Only a driver read that threw (`-EIO`) or returned a fatal code goes through
    `rtl_device_end_for_receiver()`; either raises the exit flag and wakes the demod, and a stop is silent. A driver
    call that hangs cannot be cancelled: a hung read ends the session, and both controls and stop wait for the driver
    to release `soapy_lock`; a hung control or setup call holds the same lock, which pauses the silence count and
    blocks the reader and the stop alike until the driver returns. Tests: `IO_SOAPY_STREAM_LOSS` (the reader runs against a stand-in driver,
    `rtl_device_test_soapy_device()`; internal failures, restart counters, refused reader creation and competing
    grace verdicts included). Timeout, overflow and diagnostic counters reset before every reader start. Bench:
    `dsd-neo_bench_soapy_ingest`.
  - USB gain controls (`rtl_device.cpp`): demod register 0x19 holds test mode and the digital AGC both, and librtlsdr's
    setter for each writes it whole, so `rtl_usb_apply_register19()` resolves it from the record (test mode while on,
    else `digital_agc_wanted`: on only with automatic gain, per `DSD_NEO_RTL_AGC`) at every gain, AGC and test-mode
    change; asserting test mode off restores the digital AGC rather than writing it off. A gain change
    (`rtl_usb_change_gain()`: manual = digital AGC off + manual mode + gain, automatic = automatic mode + digital AGC)
    is one transaction: a partial failure re-applies the state the controls last took, and when that fails too (or a
    first change has none) `gain_state_unknown` makes the gain getters report no mode and no reading until a change
    takes. The nearest-gain lookup returns a status apart from the value (gains reach below zero), and an unreadable
    table is an error. The stream's gain callers (retune profile, autogain supervisor) note the gain that took and
    otherwise republish what the device reports. The supervisor keeps its prior target on failure and commits the
    selected gain on success. SoapySDR gain callers read back aggregate gain, including a preserved named-stage
    override. Test: `IO_RTL_RETUNE_PREPARE` (an effective-state fake dongle,
    `rtl_device_test_usb_gain_device()`).
  - USB offset tuning (`rtl_sdr_fm.cpp` `controller_program_initial_capture_settings()`): librtlsdr computes the LO
    offset (`offs_freq = rate / 2 * 1.7`) from the rate the dongle holds when offset tuning is enabled, so the stream
    start sets the frequency (Fs/4-shifted centre), then the rate (read back), then offset tuning, then recomputes the
    capture centre and the Fs/4 rotation from whether it took, the tuner bandwidth, and the frequency again if the
    centre moved. Even when an offset setter reports failure, its readback records any LO change that took before
    that failure. The device records `offset_took` only when librtlsdr reports an offset
    (`rtl_device_get_offset_tuning()`, `rtl_device_get_offset_tuning_hz()`); R82xx tuners (-2) and direct sampling (-3)
    refuse it and keep the Fs/4 shift. Every tuner-bandwidth call (start, retune, refused-retune restore) goes through
    `rtl_offset_aware_bandwidth_hz()`: while offset tuning is on the bandwidth is at least `2 * offs_freq`, the filter
    librtlsdr opens on enable (an explicit `DSD_NEO_TUNER_BW_HZ` above it is kept, below it is raised and logged once).
    The device caches the bandwidth it last took (`tuner_bw_hz`). Test: `IO_RTL_OFFSET_TUNING` (an effective-state fake
    of librtlsdr's tuning controls, `rtl_device_test_usb_tuner_device()`).
  - Frontend telemetry is value-only: frontends poll the metrics getter (`src/app_control/frontend.c`) from their own
    threads while the decoder thread soft-stops the stream or replaces the input, which frees the device, the stream
    internals (`g_stream`) and the stream context a UI snapshot copied the pointer of. So every getter on that path
    reads a process-lifetime atomic or a copy the stream published: the tuner gain (read back from the device by
    the thread that applied it, `dongle_note_gain()`, and published as no reading before the device goes), the stream
    activity (`stream_set_lifecycle_flag()`), the input level (a multi-writer seqlock: reader, demod, controller and
    decoder all write it; dropped at stream start and stop), the decode health, the IQ controls and TED gain, the
    requested PPM (published under its mutex), the narrow spectrum (`rtl_metrics.cpp`: the demod folds each frame out of
    place into a buffer no reader holds and makes it current, and a reader holds the current buffer while it copies it,
    so nothing is copied to publish) and the output rate (the metrics hook's; the snapshot's `rtl_ctx` is never
    dereferenced). `rtl_device_handle` and `g_stream` are cleared before the objects they point at are destroyed. Test:
    `IO_RTL_FRONTEND_TELEMETRY` (ASan/TSan). The spectrum fold walks contiguous halves under strict floating-point
    rules; fast-math builds keep the original vector-libm loop to preserve its rounding. The published index also marks
    readiness; the demod alone owns the averaging seed. A reader keeps raced candidate frames pinned until it
    acquires a stable publication, bounded by the four shared buffers and private writer scratch. This prevents an
    empty-frame flash under repeated publication. The getter returns the published frame's bin count and rate,
    clamped to the caller's capacity; a size change affects the next published frame. `IO_RTL_SPECTRUM_FRAMES`
    compares both in-place and out-of-place folds bit for bit with the original walk, and pins the order and bits of
    the two noise-bin spans copied around the excluded peak before the median search.
  - Input level freshness (B6): the producer stamps every level it measures on the real-time monotonic clock
    (`updated_ms`, in place of the wall read moments finalization used to take, so no new clock read on the sample
    path). `dsd_input_level_is_stale()` (`runtime/input_level.c`, `DSD_INPUT_LEVEL_STALE_MS` = 2 s) is the one rule:
    frontends show a radio input's level through `dsd_app_frontend_input_level()` (`app_control/frontend.c`), built from
    the stream's seqlock copy (`rtl_stream_get_input_level()`), never the decoder's `state->input_level`, which
    refreshes only every 64 decoded symbols, and classified with `opts->input_warn_db` by `dsd_input_level_classify()`;
    a stale or missing level reads `No input` with no readings. When the live decoder is behind,
    `dsd_input_level_publish()` replaces its stale RF level with a fresh producer snapshot before classifying
    warnings; with no fresh measurement it keeps the old level and warns about nothing. An I/Q replay
    (`opts->iq_replay_active`) keeps every warning its capture calls for, so a paused replay prints the same advisories in every run. PCM inputs keep the decoder's level. Tests:
    `IO_INPUT_LEVEL_METRICS`, `APP_CONTROL_FRONTEND_METRICS`, `UI_NCURSES_PRINTER_HELPERS{,_RADIO}`. CU8 moments keep
    64-bit totals while accumulating at most 65536 samples per 32-bit reduction (`65536 * 255^2 < 2^32`); x86 widening
    reduces its byte extrema in registers and maps the four clipping-edge values to one unsigned interval. These
    integer reductions leave both the sample values and the statistics exact (`DSP_SIMD_WIDEN`,
    `IO_INPUT_LEVEL_METRICS`).
  - USB identity and reset (`radio/rtl_usb_identity.{h,cpp}`, module-private, the only includer of `libusb.h`, which
    `cmake/arch_rules.cmake` enforces): which USB device a stalled dongle is, so a USB port reset reaches it and never a
    second dongle another program streams from. `rtl_usb_identity_resolve()` runs before `rtlsdr_open()`: it takes the
    (manufacturer, product, serial) triple librtlsdr reads for the index, and is valid only when exactly one device with
    a vendor-specific (0xff) device or interface class carries it, whatever its VID:PID, none of those devices was
    unreadable (an unreadable one might be the dongle beside a readable twin), the serial is not empty, and librtlsdr
    has exactly one index with it; it records bus, port path and VID:PID. A device whose open is refused for access or
    lack of a usable driver (`LIBUSB_ERROR_ACCESS`, `LIBUSB_ERROR_NOT_SUPPORTED`) is not unreadable here (nor one whose
    configuration descriptor read is refused that way): librtlsdr opens the dongle with the same permissions right
    after, so it cannot be the dongle; every other failure (I/O, pipe, timeout, descriptors unreadable on an opened
    device) refuses. Accepted limit, of the same class as the open-time hotplug window: if the dongle's own open is
    refused in that instant while a readable device of another VID:PID carries the same triple, that device's port is
    recorded (a reset still requires the triple there). `rtl_usb_identity_verify_handle()` checks the opened handle's
    strings. `rtl_usb_identity_reset()` opens the device at the recorded bus + port path + VID:PID, re-reads its strings
    on the handle the reset goes through and resets only on the triple (a replacement on that port gets
    `RTL_USB_ID_MISMATCH` and no reset); a device that re-enumerates is found again by its triple within 5000 ms
    (cancellable, twins refused, unreadable candidates waited out; refused devices of another VID:PID are left out, a
    refused one with the recorded VID:PID, the dongle before udev has applied its permissions, is waited for).
    `rtl_usb_identity_index()` is the librtlsdr index by triple, -1 unless unique. `rtl_usb_identity_off_reason()` gives
    the "stall recovery is off" text. Every libusb/librtlsdr call goes through `struct rtl_usb_driver_ops`
    (`rtl_usb_identity_set_driver_ops()`, test hooks only). CMake defines `DSD_HAVE_LIBUSB` (`cmake/FindLibusb.cmake`:
    header, library and a `libusb_reset_device` link check) privately on `dsd-neo_io_radio` and its test mirror, only
    with a system librtlsdr and never on Android; otherwise every call returns `RTL_USB_ID_NO_LIBUSB`. Tests:
    `IO_RTL_USB_IDENTITY` (fake device list), `IO_RTL_USB_IDENTITY_STUB`.
  - USB handle swap (`rtl_device.cpp`): `rtl_device_create()` resolves the identity before the open and verifies the
    handle after; any other result (or an Android descriptor) logs `RTL-SDR: stall recovery is off for this receiver:
    <reason>` once (info level for Android, a warning otherwise) and leaves `rtl_device_usb_recovery_eligible()` 0. Two
    locks, taken `usb_lifecycle_lock` then `usb_ctrl_lock`, never the reverse, neither on the sample path: every use of
    the librtlsdr handle (setters, getters, `rtlsdr_cancel_async()`) and of the controls' record is under
    `usb_ctrl_lock`, so the demod never reads a gain change in flight; the handle is published or retired only with both
    held; the reader reads it unlocked, as it starts after publication and is joined before retirement. `thread_started`
    changes only under `usb_lifecycle_lock`. Each USB setter records in `usb_eff` (`struct rtl_usb_effective`) what the
    dongle took, with a took flag per control, only on success. `rtl_device_usb_recover_handle()` holds
    `usb_lifecycle_lock` throughout: sets `recovering` (setters then return `RTL_DEVICE_EBUSY` and change nothing,
    getters still lock), cancels and joins the reader (its return is silent while `recovering`), unpublishes the handle,
    attempts its FIFO reset, closes it, then calls `rtl_usb_identity_reset()`. The FIFO clear is best effort, as in normal
    destruction: resetting an RTL2832U with its endpoint FIFO held in reset can leave it repeating stale samples after
    reopening, but a wedged handle's refusal must not prevent the USB reset. Recovery re-resolves and reopens by
    `rtl_usb_identity_index()`, verifies, and replays
    `usb_eff` onto the unpublished handle (crystal, direct sampling, frequency, rate, offset tuning only if it took,
    bandwidth through `rtl_offset_aware_bandwidth_hz()`, PPM if nonzero, frequency, gain mode + gain if a gain took,
    register 0x19, IF gains, bias tee, FIFO reset; the frequency precedes the rate so an R82xx never tunes to 0 Hz). A
    control that took and does not take again fails the recovery with the handle closed and nothing published; success
    publishes the handle and clears `recovering`. Every handle the swap closes (the old one, or a new one it gives up
    on) first gets one best-effort bias-tee-off write when the bias tee took on: librtlsdr's close and a USB reset leave
    it powered. It never starts the reader: `rtl_device_usb_resume_reader()` does, under `usb_lifecycle_lock`, only
    while `stop_requested` is clear, after recording `progress_baseline` and bumping `watch_rearm_seq`. Stop and destroy
    set `stop_requested` first, whatever runs, then take `usb_lifecycle_lock`; the swap checks the latch between steps
    (the reset polls it) and returns `-ECANCELED`, so either the reader restarted before the stop (which joins it) or it
    never starts. A dongle found on another port is recorded there; a triple that after the reset no longer leads to
    exactly one device (index or resolution) fails the recovery before any open and turns recovery off from then on.
    Test: `IO_RTL_USB_SWAP` (a whole fake dongle, `rtl_device_test_usb_dongle_install()`, over a fake device list;
    ASan/TSan). Swap/replay cases require libusb; builds without it exercise the ineligible-device paths.
  - USB stall recovery (issue #518 B1, D1, D2). The stream registers `rtl_device_set_recovery_callback()` before the USB
    reader starts and clears it once `rtl_device_stop_async()` joined the progress monitor. At the 5000 ms silence limit
    the monitor of an eligible receiver with a callback posts a request (`recovery_request` REQUESTED, and
    `recovery_epoch` moves) unless its budget refuses: one per stall (a restarted reader with no nonempty callback since
    its `progress_baseline` is the same stall) and none within 60 000 ms of the previous request; a refusal, an
    ineligible receiver or one without a callback ends the session as #774 does. While a request is pending or taken the
    monitor accrues nothing; one the controller has not taken (`rtl_device_usb_recovery_take()`, which logs `RTL-SDR:
    the receiver sent no samples for <n> ms; resetting the USB device`) within 2000 ms is withdrawn and ends the
    session. Taking a request checks teardown both before and after claiming it, so a quit or receiver ending cannot
    begin a reset. A changed `watch_rearm_seq` restarts the watch from `progress_baseline` with no silence, so callbacks
    before the reader thread started count. `rtl_device_end_for_receiver()` drops a verdict while a request is pending
    or taken, or when one was posted since it was made (a reader-exit verdict waiting out the unplug-owner grace);
    `rtl_device_usb_recovery_finish()` reports the recovery's own result: 0 resumes, a stop is silent, anything else
    ends the session for the receiver with the failed step in the cause. The controller side (`rtl_sdr_fm.cpp`): the
    callback queues `usb_recovery_pending` on `hop_m`; `controller_wait_for_retune_work()` takes it alone, ahead of a
    queued retune or PPM change, and `controller_process_usb_recovery()` runs `controller_stream_boundary(s, reason,
    device_op, ctx)` (`reason`: the gap's, `kStreamGap*`). An unset channel center fails before any device or gate
    operation (`IO_RTL_CONTROL_EDGES`). Otherwise: `controller_begin_reconfigure()` (gate, capture hold, input
    purge, pre mute), `device_op` (USB: `rtl_device_usb_recover_handle()`, then the published tuner gain), then on every
    exit the retune tail (post mute, `controller_finalize_reconfigure()` with `DemodRetuneResetReason::StreamGap`, which
    keeps the FLL and records no capture RESET of its own, post-reset mute, capture hold released); only after a
    successful `device_op` the output ring is emptied with the generation moved, `rtl_stream_note_discontinuity()`
    records the gap's RETUNE+RESET pair and moves the count, and `rtl_device_resume_stream()` restarts the producer; the
    gate ends last. `retune_reset_reason_from_name()` maps the gap reasons to `StreamGap` for a replay. A success posts
    `DSD_STREAM_NOTICE_USB_RECOVERED` (`<dsd-neo/runtime/stream_notice_hooks.h>`); app-control installs the sink at
    session start, `dsd_app_drain_cmds()` shows it as the toast `RTL-SDR stopped sending samples; reset and reopened`
    (no failure latched), and session end clears it. Tests: `IO_RTL_USB_RECOVERY` (device level with the test as the
    controller: budget, take bound, rearm, grace verdict; stream level over the real controller: step order with a
    probe, failure and stop, capture events; ASan/TSan), `IO_USB_PROGRESS_MONITOR` (ineligible with a callback),
    `IO_RTL_REPLAY_EOF_AND_CF32` (gap reasons replay as `StreamGap`), `APP_COMMAND_QUEUE` (the toast),
    `APP_CONTROL_TELEMETRY_HOOKS_INSTALL`.
  - rtl_tcp connections and reconnects (issue #518 B3, B4, B13, B16, B17; `rtl_device.cpp`). The server sends exactly a
    12-byte header ("RTL0", tuner type, gain count, big-endian) and then u8 I/Q: `rtl_tcp_read_header()` reads those
    twelve bytes and nothing more (no gain table), across receive timeouts, with explicit results (SUCCESS, EOF,
    INVALID, CANCELLED, TIMEOUT after 10 s); an open whose header fails fails. Each connection is a counted
    `rtl_tcp_conn` (socket, `generation`, its own `tx_lock`, `lost`); the last reference closes the socket. The
    cancellation token is the stop latch (`stop_requested`, set first by stop and destroy) or the exit flag, never
    `run`, so the session's first connect and header read (before any reader) are cancellable too. A command (or a gain
    change's commands, as one unit) takes a reference to the published connection under `tcp_lock`, sends under its
    `tx_lock` with `rtl_tcp_send_all()` (waits for room a 50 ms slice at a time, checking cancellation between slices,
    then sends what fits without blocking (`MSG_DONTWAIT`), partial writes continued, 2 s in all; where a send cannot be
    asked not to block (Winsock) the socket's 2 s send timeout only bounds one that blocks anyway, and any send that
    fails or times out ends the connection, since Winsock leaves a timed-out socket indeterminate) and, still under
    `tx_lock`, records what took in the effective record (D6: the device's values plus `tcp_took`) under `tcp_lock` only
    while that connection is still the published one; `tcp_lock` is never held while sending. With no published
    connection, or when the connection was lost or replaced meanwhile, a setter returns `RTL_DEVICE_ENOTCONN` and
    records nothing; a failed send takes down only the connection it used (unpublished, `lost` set, socket shut down, so
    its reader stops reading it even while the server still sends). Gain follows the USB register-0x19 policy in
    commands: manual = 0x08 off (left out under test mode) + 0x03 manual + 0x04 gain; automatic = 0x03 auto + 0x08 as
    configured; test mode (0x07) is asserted after either and wins; turning test mode off sends 0x08 as the gain state
    wants. The nearest-gain setter is a manual gain change (B16). Before any gain takes the record holds the session's
    default (automatic, digital AGC as configured); a gain or test-mode change that failed part way, or whose
    connection was replaced before commitment, marks it (`gain_unknown`, `testmode_unknown`), and the replay then
    asserts the record's state even if none ever took (as a
    USB recovery does). The reader (`tcp_thread_fn()`, `rtl_tcp_reader_recv()`; on Winsock it waits for data a slice at
    a time rather than leaning on a receive timeout) treats an error, end of stream, `DSD_NEO_TCP_MAX_TIMEOUTS` receive
    timeouts in a row, the throughput watchdog or a connection taken down as a loss, and logs which: it unpublishes it
    (no stream from here, `rtl_device_has_stream()`), then `rtl_tcp_reconnect()` connects, reads the header and hands
    the new connection over (`rtl_tcp_handoff_and_wait()`): QUEUED with a new generation, posted through the recovery
    callback, which the stream sets to `controller_post_tcp_handoff()` before the reader starts. While it waits the
    reader drains the connection into a discard path, counting the bytes modulo the I/Q pair, so the server's I/Q from
    before its settings are restored builds no backlog. The controller takes it (`rtl_device_tcp_handoff_take()`,
    CLAIMED; first and alone, like a USB recovery) and runs `controller_stream_boundary(s, kStreamGapTcpReconnect, ...)`
    with `rtl_device_tcp_publish_handoff()` as its device step: the effective record replayed as one unit (crystals,
    direct sampling, frequency, rate, offset tuning, PPM, gain policy, IF gains, bias tee; every control that took, as
    last taken, so a server restarted with other defaults ends with the session's settings), then published, or closed
    (FAILED, the reader reconnects again; CANCELLED on a stop). The boundary's `rtl_device_resume_stream()` releases the
    reader (RELEASED) and waits until it acknowledged after any receive in flight: it drops the odd byte the discard
    left (a Q byte), clears its carry and partial slice (`rtl_reset_capture_state_on_stream_boundary()`), and from there
    its bytes meet the post-reset mute and the capture. Stop and destroy run `rtl_tcp_stop()` first: latch, admission
    closed for good, a QUEUED hand-off cancelled (the reader, its waiter, closes the connection it still holds; a
    CLAIMED one is the controller's, which gives it up at its next step), the published socket shut down (waking the
    reader and any blocked sender); only then is the reader joined. Reconnect back-off sleeps in 10 ms slices. The
    reader passes whole I/Q pairs only into its pipeline (mute, capture hold, capture, ring; `rtl_tcp_process_pairs()`),
    as a USB reader's buffers are, holding an odd last byte at the front of its buffer for its Q: TCP chunks have any
    length, and the byte accounting the pipeline shares with the controller (a mute armed while a chunk is being muted,
    the capture writer's half pair at an event) slips a pair on an odd chunk. After a frequency command the reader drops
    what it buffered for the old frequency on the way to the ring itself (`tcp_restart_pending`,
    `rtl_tcp_drop_buffered_for_retune()`): the stream goes on. The rtl_tcp pipeline start fails when its controller or
    demod thread cannot start (B17), as every other pipeline's does. Frontends: `rtl_device_control_unavailable()` /
    `rtl_stream_control_unavailable()` say why the receiver cannot take a control (`RTL_DEVICE_EBUSY` during a USB
    reset, `RTL_DEVICE_ENOTCONN` without a server), and app-control's `svc_radio_control_unavailable_text()` turns
    either (or the control's own result) into the reason of the "Failed:" toast for a frequency, gain, bias tee or PPM change
    ("the RTL-SDR is being reset; try again in a moment", "the rtl_tcp server is not connected"); a PPM change, which
    applies later on the controller, is refused up front, including PPM-delta actions. A refused gain or bias-tee
    edit leaves the configuration unchanged. Tests: `IO_RTL_TCP_RECONNECT` (a scripted fake rtl_tcp server
    on loopback; device level with the test as the controller and stream level through the real controller; ASan/TSan),
    `DSP_SYMBOL_RTL_YIELD` (`sticky_yield_across_reconnect`), `APP_COMMAND_QUEUE` (the toasts).
- Rig/control: `include/dsd-neo/io/control.h`, `include/dsd-neo/io/rigctl_client.h`,
  `include/dsd-neo/io/m17_udp.h`
- UDP control API: `include/dsd-neo/io/udp_control.h`
- UDP audio output: `include/dsd-neo/io/udp_audio.h` (implemented in `src/io/audio_backends/udp_audio.c`)
- UDP/TCP PCM input: `include/dsd-neo/io/udp_input.h`, `include/dsd-neo/io/tcp_input.h`
- I/Q capture/replay: `include/dsd-neo/io/iq_capture.h`, `include/dsd-neo/io/iq_replay.h`,
  `include/dsd-neo/io/iq_types.h`, `include/dsd-neo/io/iq_wav.h`
  - Tuner gain (issue #669): `rtl_device_get_tuner_gain(dev, &tenth_db)` returns a status separate from the value,
    since librtlsdr's gains reach below zero (E4K -1.0 dB, FC0012/FC0013 -9.9 dB); its callers (`rtl_stream_get_gain()`,
    the autogain latch, the capture metadata, the retune note) all read it that way. A capture whose tuner reports no
    gain writes `"tuner_gain_known": false` with a gain of 0 (`dsd_iq_capture_config::tuner_gain_unknown`); replay
    reads it, and a legacy Airspy sidecar without it, as `dsd_iq_replay_config::tuner_gain_unknown`, and then reports
    no gain. Test: `IO_RTL_REPLAY_EOF_AND_CF32`.
  - WAV I/Q recordings (issue #701, `src/io/iq/iq_wav.c`): 2-channel RIFF/RF64/BW64 WAVs of u8, s16, s24, s32, f32
    or f64 (plain or EXTENSIBLE fmt). `dsd_iq_replay_read_metadata()` sniffs a non-`.json` path before it looks for a
    sidecar, and pre-reads only `data_file` from a sidecar, so a WAV is recognised before any sidecar field is
    validated; a sidecar beside a WAV is ignored and reported in `dsd_iq_replay_config::open_notice`. The config is a
    virtual cf32 capture (`container == DSD_IQ_REPLAY_CONTAINER_WAV`, stage `post_driver_cf32_pre_ring`, base and
    post 1, centre from `auxi` or the file name, 0 when unknown): `data_bytes`, seeks and offsets count 8 bytes per
    frame, and the replay reader converts frames as it reads them, keeping a short read's partial frame and the rest
    of a frame a read smaller than 8 bytes split for the next read. `dsd_iq_replay_measure()` is the effective-size
    rule for both containers (used by the open, `--iq-info` and the CLI); `dsd_iq_replay_seek()` moves either kind of
    source to a whole complex sample. `dsd_iq_wav_read_info()` decides the container's extent before it walks the
    chunks (RF64/BW64 from `ds64`, which must come first; a RIFF whose size is 0, 0xFFFFFFFF or past the file runs to
    the file's end, and a `data` chunk sized 0 or 0xFFFFFFFF as its last chunk is then unfinalized: whole frames to
    EOF; otherwise the data stops at `min(declared, container end, file)`, and a container that ends inside the
    declared data sets `container_clamped`, a warning in `--iq-info` and the CLI summary), tries a chunk's pad byte
    first and the unpadded position only where the padded one is no chunk header, and caps the walk at
    `DSD_IQ_WAV_MAX_CHUNKS`. Centre: the `auxi` chunk's, else `dsd_iq_wav_center_from_name()`, else 0;
    start: the `auxi` SYSTEMTIME taken as UTC (a year below 1601 is none), else `dsd_iq_wav_start_from_name()`.
    `dsd_iq_wav_frames_to_cf32()` scales u8 as cu8 widening does ((x - 127.5) / 127.5) and classifies f64 before it
    narrows it (NaN to 0, infinities and magnitudes past `DSD_IQ_WAV_F64_CLAMP` clamped). `--iq-info` has a WAV branch
    (`dsd_iq_info_print_wav()`: provenance, the plan at 48 and 72 kHz, truncated/unfinalized/partial-frame/ignored
    sidecar warnings); `args.c` logs the open notice and a one-line summary (`cli_log_iq_replay_wav_summary()`). Tests:
    `IO_IQ_WAV` (with an unfinalized WAV read near its end past 4 GiB, on a sparse file), `IQ_INFO_WAV_SCBS`,
    `FUZZ_IO_IQ_WAV`
  - Replay rate plan (issue #701, user rules in `docs/iq-capture-replay.md` "Rate Chain And Resampling"):
    `dsd_iq_replay_plan_rate()` picks the chain a replay runs for the mode's DSP rate
    and `dsd_iq_replay_apply_rate_plan()` writes it into the config (`dsp_input_rate_hz`, base, post, demod;
    `sample_rate_hz` stays the capture's rate for pacing, media time, chunking and events). A sidecar with no explicit
    bandwidth request keeps its recorded chain unless TETRA's 18 kHz requirement fails on its CQPSK rate (dsp input /
    base, post 1); otherwise the capture decimates by 2^k when it is D x 2^k, else it is resampled to the largest
    D x 2^k at or below its rate (or up to D). The plan refuses a capture below 8 kHz, an upsample past 9x and a capture
    at 2 x 2^10 x D or faster. Stream open (`stream_open_plan_replay_rate()`) plans at every start for the target
    `stream_open_replay_target_hz()` picks (an EXPLICIT request, else the recorded bandwidth, else the request in force;
    under TETRA 72 kHz for a target that is no multiple of 18 kHz) and refuses a start whose plan fails; the stream runs
    the planned bandwidth while opts keeps the request, and the plan in force is kept for
    `rtl_stream_replay_replan_check()`; the replay device resamples with `dsd_iq_resampler` (protected band 0.45 x D) in
    the reader thread only when `dsp_input_rate_hz` differs from the capture's, clears it at a RETUNE, RESET, MUTE and
    loop rewind, hands event callbacks the DSP input rate, and counts pacing and media time in capture samples
    (`dsd_iq_replay_chunk_bytes_for()` keeps a resampled chunk within one live transfer). A replay with no known centre
    (a WAV with none) opens and loops: stream open skips the frequency check for a replay, and
    `controller_finalize_dsp_chain()` runs the DSP half of `controller_finalize_rate_chain()` without a centre.
    Tests: `IO_IQ_REPLAY_RATE_PLAN`, `IO_RTL_REPLAY_RESAMPLE`, the `DECODE_IQ_TETRA_WAV_*` and
    `DECODE_IQ_TETRA_REAL_AIR_SCBS_50K` cases (WAVs made in the build tree by `dsd-neo_test_iq_wav_fixture_tool`)
  - Requested bandwidth provenance: `dsd_opts::rtl_dsp_bw_khz` is the requested DSP bandwidth and
    `rtl_dsp_bw_source` says where it came from (`DSD_RTL_DSP_BW_SOURCE_DEFAULT`, `_EXPLICIT`, `_PRESET`). Writers set
    both (`dsd_opts_set_rtl_dsp_bw()`): mode presets in `args.c` (PRESET); input spec bandwidth fields
    (`input_spec.c`, `airspy_config.c`), the INI `[input]` `rtl_bw_khz` of an RTL-family source at startup
    (`config_user.cpp`) and on a runtime config apply, the interactive bootstrap, and `svc_rtl_set_bandwidth_locked()`
    (EXPLICIT). Snapshots put both back (`ui_capture_radio_input()`/`ui_restore_radio_input()`, the config rollback's
    live-gain path, `svc_airspy_tuning`). A replay no longer writes its recorded bandwidth into opts: only an EXPLICIT
    request beats `recorded_dsp_bw_khz` as the plan's target, so a mode preset (`-fT`) keeps a sidecar's recorded chain.
    An RTL-SDR or rtl_tcp `[input]` `rtl_bw_khz` is an EXPLICIT request of its own (`apply_input_rtl_bw_request()`), so
    it outlives the spec `--iq-replay` replaces at startup.
  - Run-time re-plan with approximate resume (issue #701): the replay reader records, after every chunk it hands the
    ring, a checkpoint (`dsd_iq_replay_checkpoint` in `<dsd-neo/core/iq_replay_checkpoint.h>`: data offset, next event
    index, media timeline and pass samples, loop pass, fs/4 phase; `rtl_device_replay_checkpoint()`), which soft stop
    keeps once the reader is joined (`rtl_stream_replay_checkpoint()`). A start with `dsd_opts::iq_replay_resume_pending`
    seeks there (`dsd_iq_replay_seek()`), opens on the centre the last RETUNE before the event index set
    (`dsd_iq_replay_resume_check()`), and goes on with the event cursor, media time, loop count and fs/4 phase;
    realtime pacing is rebased to the resume, the resampler starts fresh, and what was in the ring or the decoder's
    cache is dropped (a few ms). Test: `IO_RTL_REPLAY_RESUME`.
  - App-control re-plan (issue #701): `svc_rtl_replay_replan_locked()` (decoder thread, P25 SM tick guard held) asks
    `rtl_stream_replay_replan_check()` whether the options plan another chain, stops with the decode clock kept on the
    capture (`svc_rtl_stop_ex_locked()`), and starts at the checkpoint, taking no source carrier boundary (the replay
    stays on its carrier); a refused plan, or a failed start, puts the caller's receive profile back through its restore
    callback and resumes the previous chain from the same checkpoint, and a recovery that fails too takes the caller's
    start-failure path. Callers: `RTL_SET_BW` on a replay, `decode_mode_apply_value()` (`decode_mode_replan_replay()`,
    TETRA on and off) and a config apply (`apply_cfg_replay_replan()`: `[mode]`, and an RTL-family `[input]`
    `rtl_bw_khz` for the replay it leaves running). A refused or failed mode switch's restore puts back everything the
    staged preset wrote (`decode_mode_restore_profile()`: the receive settings, the audio layout and the `[mode]`
    extras). A config's gain restarts no replay (`apply_cfg_rtl_live_gain()`), so a config's one reopen is the
    re-plan's. Checks made before a change hold an analog width to the rate the replay will run at
    (`svc_replay_planned_rate_hz()` over `rtl_stream_replay_replan_check_request()`, then
    `svc_check_analog_bandwidth_at_rate()`): a config's preflight (`CFG_REOPEN_AT_REPLAY_RATE`), `RTL_SET_BW`
    (`svc_rtl_bandwidth_fits_analog_width()`) and a switch to Analog or AM (`ui_check_mode_receive_profile()`). A
    replay's analog-width refusals name the DSP bandwidths that fit, and TETRA's capture advice only a refused re-plan.
    Tests: `APP_COMMAND_QUEUE` (`test_replay_*`), `RUNTIME_CONFIG_APPLY`.

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
    open. An AM start on a replay whose kept sidecar chain decimates after the demodulator is refused with
    `AM needs a capture with post_downsample 1`: no AM width runs there, the default included (a planned chain always
    has `post_downsample` 1).
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
    takes from the replay's rate plan (`dsd_iq_replay_dsp_input_rate_hz()` / `base_decimation`), so only the rational
    resampler converts `rate_out` to the output rate. Test:
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
    - A chunk is at most one live transfer (`dsd_iq_replay_chunk_bytes()`, `<dsd-neo/io/iq_replay.h>`, issue #626):
      min(8192, rate x 8192 / 1,536,000) complex samples, at least one, in the format's bytes. That is never more than
      the 16384 cu8 bytes a live RTL transfer holds (`ACTUAL_BUF_LENGTH`; a `static_assert` in `rtl_sdr_fm.cpp` ties
      the two), and never longer than such a transfer lasts at the default 1.536 Msps, 5.33 ms. A 48 kHz capture
      (every committed fixture) is replayed 256 samples a block, the 256 channel samples a live default block
      decimates to. The rule depends on the capture's format and rate alone, so the chunking never follows a runtime
      setting. With the old fixed 64 KiB chunk a 48 kHz block was 683 ms, and every per-block decision (the level
      squelch, its envelope, the published channel power, adaptive loop gains, metrics, how soon a decoder request
      lands) ran on that grid. Below 1.536 Msps a chunk is shorter than a live transfer at that rate, never longer.
    - The reader reads a chunk whole with `replay_read_exact()`: one chunk, or up to the next event or the end, looping
      over short reads. A cf32 read that stopped inside a complex sample would otherwise be skipped, and every read
      after it would stay off the sample grid. A read that fails after part of a chunk hands over that part first. A
      chunk the capture's end or a failure cuts short ends on its last whole complex sample (2 bytes for cu8, 8 for
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
      chunk's input level when it starts the block, so a replay's level covers one live transfer's time, as a live
      stream's covers each transfer.

    Tests: `IO_IQ_METADATA` (the chunk rule against literal sizes), `IO_RTL_REPLAY_CHUNKING` (48 kHz cu8 and cf32
    blocks of 256 samples, 1.536 Msps cu8 blocks of 16384 bytes, a MUTE cutting a chunk and moving the media time by
    what it omits, and a level squelch on `nfm_burst_synth` opening on one run of blocks within one block of the
    carrier's key and unkey), `IO_RTL_REPLAY_EOF_AND_CF32` (block log per chunk in fast and realtime replay, media time
    across a loop, a chunk held numbered but uncommitted, a block that wraps the ring end and a discarded block, each
    releasing its input before it acknowledges, a multi-chunk EOF, cf32 short reads, a read failure mid-chunk, in cu8
    and inside a cf32 sample, a cf32 end inside a sample, the input level at the first block).
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
    `DECODE_IQ_RF_CLIP_EOF_DETERMINISM` (a capture clipped in its last chunk only warns of CLIP fast, realtime and with
    short reads, from the level poll made after the read that took its last samples).
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
      output kind, channel profile, symbol rate and levels, output rate, media start and duration, output count, and
      the channel centre the chunk was captured on) of the batch the last read took samples from, with their place in
      it, is what `rtl_stream_get_replay_batch()` returns, on the decoder thread while the stream is open. The symbol
      cache labels what it reads from it, through the runtime metrics hook (see DSP), so it keeps the first batch after
      a change, and runs the decode clock on its media span, sample by sample (see Runtime).
    - Lock order: `replay_eof_m`, then `output.ready_m`. The locked sections use the unlocked ring helpers.

    Tests: `IO_RTL_REPLAY_DETERMINISM` (greedy fast, slow and realtime readers with the same requests deliver the same
    stream, the oracle's count, and the same media time at fixed delivered positions, which moves by exactly what a
    MUTE omitted at its boundary; a block's output held unpublished stays out of the decoder's reach, and every read's
    tag describes its samples; bounded stops at each wait; zero-output and empty replays; a reader-start failure at
    the first demand wait), `IO_RTL_REPLAY_EOF_AND_CF32`.
  - Stream gaps on the same carrier (issue #518 D5: a USB recovery, an rtl_tcp reconnect). The stream counts them in a
    process-wide, value-only `g_rtl_discontinuity_seq` (`rtl_stream_discontinuity_seq()`), which never goes back.
    `rtl_stream_note_discontinuity(reason)`, which the controller runs inside its boundary transaction after the purge
    and before the producer resumes, records a RETUNE and a RESET with the reason (`RTL_STREAM_GAP_REASON_*`) on the
    centre in force in the capture, then moves the count: no sample past the gap can be read or captured before either.
    Under a replay, a RESET with one of those reasons moves the count once it is applied on the idle pipeline
    (`rtl_replay_on_reset_event()`), and `demod_replay_batch_tag()` tags every batch with the count
    (`rtl_stream_replay_batch::boundary_seq`): the batches of chunks before the gap carry the old one, those after it
    the new. `dsd_rtl_stream_metrics_hook_read_discontinuity_seq()` checks the live count first; only a moved count
    reaches the separate replay-tag path, so ordinary one-sample reads need no tag buffer or its stack frame.
    `RUNTIME_RTL_STREAM_METRICS_HOOKS` pins both paths and the position of a gap behind an earlier replay batch.
    The decoder side is in DSP (`dsd_symbol.c`, stream gaps) and Engine (`DSD_CARRIER_BOUNDARY_STREAM_GAP`).
    Tests: `IO_RTL_REPLAY_DETERMINISM` (recovery and reconnect events mark exactly the batches after them, a frequency
    RESET marks none, every batch read whole in order; a gap noted while a capture is written replays as the same gap).
  - Reads while the device has no stream (issue #518 D4, B3). `rtl_device_has_stream()` says whether the device has a
    stream at all, as against one that is only slow: a USB receiver has none from its progress monitor's recovery
    request until the recovery finished (`recovery_request` not IDLE, or `recovering`); rtl_tcp has none from the loss
    of its connection until a reconnected one is published (`tcp_connected`); the other backends report one.
    `rtl_stream_read_live_samples()` asks it only after an empty poll (`rtl_live_read_no_stream()`), so a read that
    finds samples asks nothing. With no stream, a reader that asks for it (`want_no_stream`, through
    `dsd_rtl_stream_read_ex()`, `RtlSdrOrchestrator::read()`, `rtl_stream_read_ex2()` and the runtime hook's `read_ex2`)
    gets `DSD_RTL_READ_NO_STREAM` once a 10 ms wait slice passed; `rtl_stream_no_stream_since_ms()` publishes that
    slice's start on the reader thread, through the runtime `no_stream_since_ms` hook. EDACS counts the first
    interval too, using elapsed real monotonic time, without a clock read on successful sample reads; any other read
    returns `DSD_RTL_READ_YIELD` while
    `dsd_runtime_controls_pending()`, else waits on. A slow stream's read never yields, whatever is queued (the 5 s
    silence verdict bounds that wait). The search's 100 ms failure and the idle NFM monitor's zero read are checked
    first and are unchanged; with no stream and a command queued, a search visit's read yields at once like any other
    (the yield outranks the search's interruption, and the engine loop runs the visit's deadlines on the same pass). A
    replay never asks. The orchestrator carries both results as they are (its own no-options error is -1, not -2).
    Tests: `IO_RTL_RETUNE_PREPARE` (the live read over a device with and without a stream), `IO_RTL_USB_RECOVERY` (a
    read yields while a real recovery runs and the samples after it arrive; a slow stream with a command queued never
    yields and decodes exactly as without one), `IO_RTL_STREAM_ORCHESTRATOR`, `ENGINE_IO_HOOKS_INSTALL`.
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
  - I/Q replay tuned frequency (issue #575). A replayed RETUNE moves the demod, but `opts->rtlsdr_center_freq` keeps
    the capture's opening centre: a live retune is deferred during a replay before it writes the field, and the DMR,
    NXDN, EDACS and P25/DMR trunking paths read it as the control or current channel, so moving it would change how a
    replay decodes. The reading follows the capture instead:
    - `demod_replay_batch_tag()` tags each batch with `controller.last_applied_freq_hz`, which the opening settings
      and a loop rewind set (`controller_apply_replay_settings()`) and a replayed RETUNE moves
      (`rtl_replay_on_retune_event()`). Each is applied on an idle pipeline (above), so the centre in force when the
      demod tags a block is the one its chunk was captured on. The engine's metrics adapter copies it into
      `dsd_rtl_stream_replay_batch::center_frequency_hz`.
    - The decoder thread writes it into `dsd_opts::iq_replay_center_freq` wherever a replay sample runs the decode
      clock: the symbol cache keeps the batch centre beside its media span (`rtl_symbol_cache_center_hz`) and its
      readers (the FSK sample read and the CQPSK fast path) publish it after each sample they take; the one-sample
      readers (the analog monitor, M17, EDACS analog) get it from
      `dsd_rtl_stream_metrics_hook_replay_advance_decode_clock()`. Each notes it through
      `dsd_opts_note_iq_replay_center()`, which stores only a nonzero centre that differs from the one noted; a live
      read writes nothing and costs nothing new, and a live fill drops the centre a replay fill left in the cache.
    - A move from one nonzero centre to another is a retune the capture recorded. The read paths note each centre
      through `dsd_frame_sync_note_replay_center()` (runtime), which returns from `dsd_opts_note_iq_replay_center()`
      whether the decoder adopted a new centre over another and, when it did, runs the frame-sync hook `replay_retune`
      there, before the read returns the first sample the new carrier carries: a retune can land inside a frame, whose
      decoding goes on publishing calls before the next frame-sync return. The engine installs
      `dsd_engine_leave_replay_carrier()` for it. With trunking off it is another conventional carrier, and it runs the
      carrier boundary an accepted live retune runs (`dsd_engine_carrier_boundary()`, see Protocols, "Call frequency and
      access-code provenance"): the outgoing calls end and commit as a hop, the codes and the untrunked state go, and
      `carrier_seq` moves, so the frame the retune landed in is dropped by the decoder reading it; a call decoded after
      the adoption takes the new centre and code, and no later pass ends it. With trunking on it is the system
      following itself, and under trunk scan a target switch with its own snapshots: nothing changes. It runs inside a
      sample read on the decoder thread, where no call-state lock is held: that lock is taken only inside
      `call_state.c` and the event layer, which never read samples, and `dsd_call_state_end_ex()` and
      `dsd_event_sync_slot()` take and release it themselves, as a protocol publishing a call mid-frame does. The P25
      SM tick guard the frame may hold is not taken. A stop of the stream clears the centre
      (`dsd_opts_forget_iq_replay_center()`). Test: `ENGINE_INPUT_BOUNDARY` (a retune adopted inside a frame); the one
      `iq-decode` fixture with a conventional recorded retune (`nxdn48_after_retune`, `-fi` and `-fa`) decodes byte for
      byte as before.
    - It lives in the caller's options, not the RTL orchestrator's copy, and does not key on `iq_replay_active`. Every
      stop of the stream clears it (`svc_rtl_stop_locked()`, which each app-control stop, restart, input switch and
      rollback runs; the engine's own open and close in `engine.c`), so a live radio that replaces a replay reads its
      own centre again.
    - Readers go through `dsd_opts_tuned_freq_hz()` (see Core). Tests: `IO_RTL_REPLAY_DETERMINISM` (every leg's batch
      centres follow the capture layout's RETUNEs; a looping capture's opening centre, the RETUNE's, and the opening
      one again after the rewind), `RUNTIME_RTL_STREAM_METRICS_HOOKS`, `ENGINE_RTL_STREAM_METRICS_HOOKS_INSTALL`,
      `RTL_SYMBOL_REPLAY_CLOCK`, `UI_MENU_SERVICES`, `ENGINE_RUN_SETUP`.
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
  `m17_udp.c`, `udp_bind.c`). `udp_input_read_sample_wait()` gives up after a timeout with no sample, so the decoder can
  apply a queued command while the input is silent (issue #634); `ConnectBounded()` (`dsd_rigctl.c`) connects within
  `DSD_CONNECT_BOUNDED_TIMEOUT_MS`, cancellably; with resolve 0 (the TCP audio reconnect) it goes back to the address
  the running input's connection went to, which `ConnectKeepTcpAudioAddress()` keeps once that input is installed (the
  startup `Connect()`, a switch that took the socket), so a failed switch elsewhere never displaces it. The engine
  installs both in the network-audio hook table (`udp_read_sample_wait`, `tcp_connect`).
- M17 protocol frame packing/parsing lives in `src/protocol/m17/m17.c`; M17 UDP socket helpers are exposed via
  `include/dsd-neo/io/m17_udp.h`.

Build files: `src/io/CMakeLists.txt` (defines radio/audio/control subtargets)

## FEC

- Path: `src/fec`, `include/dsd-neo/fec`
- Target: `dsd-neo_fec`
- Responsibilities: BCH, Golay, Hamming, RS, and BPTC helpers. Protocol-specific CRC/FCS helpers live with the
  corresponding protocol modules under `src/protocol/...`.
- `bptc.c` / `<dsd-neo/fec/bptc.h>`: `BPTC_196x96_Extract_Data()` is the best-effort DMR payload extractor; a column
  whose Hamming(13,9) decode fails keeps its received bits (the Hamming(15,11) rows correct every syndrome). It copies
  back data bits only, so a parity-bit error can still fail its second pass; then it takes the full decoder's codeword
  if that lies within `DSD_BPTC_196x96_UNIQUE_RADIUS` (4) and reports no irrecoverable error.
  `BPTC_196x96_Decode_Codeword()` instead decodes a copy with full row and column copy-back (parity included, two
  passes) and succeeds only on a valid product codeword, returning that codeword's payload, R bits and its distance to
  the received bits. The decode is iterative, not nearest-codeword (some four-error patterns reach a different
  codeword at distance 5), so callers gate on the distance.
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
- `tetra_tea.c` / `<dsd-neo/crypto/tetra_tea.h>` wrap the pinned TETRA_crypto TEA1/2/3 keystream generators
  (`dsd-neo_tetra_crypto`, see Third-Party) and pack the EN 300 392-7 clause 6.3.2.1 frame-number IV themselves,
  returning 0 for an out-of-range slot, frame, multiframe or direction. `tetra_tea_derive_eck()` is TB5 (clauses
  4.2.7 and 6.3.2.2): the encryption cipher key the generators take, from a cipher key (SCK or CCK) and the carrier
  number, location area and colour code, pinned by TETRA_crypto's four TB5 vectors (`TETRA_CRYPTO`).
  `tetra_tea1_keystream_reduced()` runs TEA1 from its 32-bit key register (the reduced ECK) instead of an ECK: the
  register TEA1 compresses the ECK into before it generates anything, which belongs to the one channel TB5 made that ECK
  for; `tetra_tea1_reduce_key()` gives that register of an ECK. It is the only file that includes the upstream headers.
- `tetra_ta61.c` (declared in `<dsd-neo/crypto/tetra_tea.h>`) is TA61, the encrypted short identity of EN 300 392-7
  clause 4.2.6 (`tetra_ta61_encrypt()` / `_decrypt()`: a 24-bit SSI and its ESI under an 80-bit SCK or CCK), with the
  HURDLE block encryption it runs on, ported from the pinned TETRA_crypto `taa1.c` / `hurdle.c` (Apache-2.0, see
  THIRD_PARTY.md) byte by byte: upstream reads words through byte pointers and picks a bit order by host byte order, so
  it is not compiled. `TETRA_CRYPTO` pins both directions to upstream's vectors.
- `backup_seal.c` / `<dsd-neo/crypto/backup_seal.h>` seal and open passphrase-protected backups (scrypt + AES-256-GCM,
  issue #580). The 64-byte header is the GCM AAD. Seal and open both accept only log2(N) 14..17, r 8 and p 1..4, so an
  untrusted file cannot demand more scrypt memory than that. `dsd_backup_open()` wipes its output on any failure:
  plaintext leaves only after the tag verifies. New backups use `dsd_backup_kdf_params_default()` (log2(N) 16, r 8, p 2,
  about 64 MiB). `dsd_backup_derive_key()` is a known-answer helper without that policy; never pass it untrusted
  parameters. The Qt archive inside the envelope is under UI, Qt, "Configuration backup". Tests: `BACKUP_SEAL`,
  `FUZZ_CRYPTO_BACKUP_HEADER` (`dsd_backup_parse_header()`, the only parser that runs before authentication),
  `HEADERS_PUBLIC_CRYPTO_BACKUP_SEAL` and `HEADERS_PUBLIC_CRYPTO_BACKUP_SEAL_CXX`.
- Build files: `src/crypto/CMakeLists.txt`

## Protocols

- Path: `src/protocol`, `include/dsd-neo/protocol`
- Targets (one per protocol):
  - `dsd-neo_proto_dmr`, `dsd-neo_proto_dpmr`, `dsd-neo_proto_dstar`, `dsd-neo_proto_nxdn`, `dsd-neo_proto_p25`
    (phase1/phase2), `dsd-neo_proto_m17`, `dsd-neo_proto_x2tdma`, `dsd-neo_proto_edacs`, `dsd-neo_proto_provoice`,
    `dsd-neo_proto_ysf`, `dsd-neo_proto_tetra`

Notes:

- P25 Phase 1 list trellis decoding (`p25_12.c`) merges four sorted predecessor lists for each destination state.
  Equal metrics retain predecessor-state order, then survivor rank, matching stable insertion; traceback and final
  candidate deduplication keep that order. Invalid metrics remain `UINT32_MAX` and never participate in addition.
  `P25_12_LIST` compares complete candidate bytes, metrics and order with an independent full-path reference,
  including zero and partial metric ties, extreme LLRs and every output limit.

- Optional codec integrations are expressed via feature interface targets:
  - `dsd-neo_feature_codec2` → `USE_CODEC2` (used by M17 when available). M17's decoded Codec2 voice plays through
    the digital voice output every other mode uses (issue #658): `m17_play_codec2_voice()` in
    `src/protocol/m17/m17.c` settles M17's own gates (a clear or decrypted payload, a CAN the CAN filter allows; M17
    is outside talkgroup policy) and hands the decoder's real output, 160 samples per 3200 frame and 320 per 1600
    frame, to `dsd_audio_play_decoded_pcm16()` (see the Core audio API note). So the mute, the slot 1 switch, the
    digital gain (`-g` and its live controls), the high-pass, the stream's channel count and sample format (`-y`),
    the WAVs and the audible-audio stamp follow the shared chain and mixers. Tests: `M17_STATE_DISPATCH`, `CORE_AUDIO2_HELPERS`,
    `DECODE_IQ_M17_VOICE_OUTPUT`.

- M17 IP dispatch validates the received length before reading a stream or control frame or changing its state.
  MPKT header conversion reads only its 34-byte prefix, since a valid packet can be shorter than a 54-byte stream
  frame. The application length excludes only the two CRC bytes, preserving the final application byte emitted by
  the sender. `M17_STATE_DISPATCH` exercises every truncated prefix, an exact-sized short valid MPKT and a complete
  text-metadata payload under ASan.

EDACS analog voice (`edacs_analog()` in `src/protocol/edacs/edacs-fme.c`) runs at 48 kHz (`EDACS_ANALOG_RATE_HZ`,
issue #633): the release register reads every 5th sample as a 9600-baud symbol, the per-call WAV opens at it and the
static WAV takes every 6th sample. TCP and UDP voice is read off the socket at the raw input rate, past the staging, RTL
at the stream's output rate and Pulse at 48 kHz (`edacs_analog_input_rate_hz()`); at any other rate a call-local
`dsd_rate_converter` converts each triplet as it is collected, so the register, the audio chain, the sinks and both
WAVs see 48 kHz, and at 48 kHz the collection is unchanged. A triplet a retune cut through (the trunk-tuning, RTL
stream or PCM stream generation moved while it was read), or read while the call's tune or any other retune is
unresolved, is discarded: no audio, recording, release check, squelch count or media update. A tune that failed leaves
the call, since the radio reads the previous channel again. The converter starts over at every retune, during a
triplet or between two, and the PCM staging held from before the call is dropped when it ends. A triplet plays
(`edacs_emit_analog_audio()`) only past the talkgroup gate, and only while the output is unmuted (`audio_out` 1) and
slot 1 is on, on the local stream, UDP and the raw fd alike (`edacs_analog_output_enabled()`, the mixers' rule); it
notes the audible-audio stamp only when one of them receives it (the shared output rule: the raw fd takes 16-bit output
only, and the null output or a local stream that is not open receives nothing) and the squelch the call runs opened on
it (see Core). Tests:
`EDACS_GRANT_TUNE_MATRIX` (the `analog-633` cases, with golden hashes of a 48 kHz call's audio, WAVs and stdout; the
emitter's outputs and stamp under each gate, squelch, output type and sample format).

P25 manual control-channel selection lives in `src/protocol/p25/p25_cc_selection.c`. The Frequency command routes
active single-system P25 sessions here; the module holds the watchdog guard through the runtime CC tuning hook,
call teardown, and acquisition restart, which starts the CC acquisition and its grace window at the tune request's
completion stamp (`dsd_trunk_tuning_request_status()`), as the engine's no-carrier return and the trunk-scan retune
do, also when the request completed inside the call. A learned CC type identifies quiet P25 sessions in mixed modes;
`noCarrier()` clears that evidence after another trunking protocol takes over. Extension ID 26
(`DSD_STATE_EXT_PROTO_P25_CC_SELECTION`) retains the site-specific cache requirement across no-carrier resets,
while network band plans and user settings survive.

Startup control-channel hunt (issue #703). A single-system trunked session whose start frequency yields no validated
control within the CC grace walks its channel map with the ordinary hunts.
- Eligibility: `dsd_state_trunk_cold_start_eligible()` (core): trunking on outside `-Y` and `--trunk-scan`, not on a
  granted voice channel, no input switch pending (`input_boundary`), not an I/Q replay
  (`dsd_opts_input_is_iq_replay()`), a tuner the trunking hooks move (RTL family or rigctl), a user channel map with a
  nonzero row, and no owner yet. P25 runs it when recovery is admitted or `dsd_trunk_p25_cold_start_presumed()`
  holds; DMR when its recovery is admitted (`dmr_sm_hunt_eligible()`), with no owner or with a DMR owner whose machine
  has confirmed no control channel (`dsd_state_trunk_dmr_hunt_eligible()`), and never on a Hytera XPT site.
- Thread: decoder thread only, under the P25 SM tick guard: `p25_sm_cold_start_tick_locked()` and DMR's own tick from
  the engine's outer loop and, while the owner is unknown, a machine is armed or hunting, or DMR recovery is admitted,
  between frames in `live_scanner_process_synced_frames()`. The watchdog never ticks a P25 machine whose `cold_start_hunt` is set; the
  decoder's next startup tick ends one whose owner validated control has named. The end of an input switch
  (`dsd_engine_end_input_boundary()`) runs that tick too, so the new input's windows are armed before its first read,
  and a silent UDP input's wait gives way while a machine is due to arm (a session that became eligible after its
  pass's tick, or whose tick found the guard busy). Arming never happens in `p25_sm_tick_ctx()`.
- Window: armed in `IDLE` (a machine that leaves `IDLE` other than for its startup hunt drops it), it runs from the
  later of its opening and `dsd_state::dmr_confidence_lock_m` (validated DMR content), and for P25 also
  `dsd_state::nxdn_confirm_lock_m` (an NXDN frame whose own CRCs confirmed it, so Auto never leaves NXDN traffic); raw
  sync never holds it, and neither names an owner. The hunt starts when it exceeds the CC grace (P25
  `--p25-cc-grace`, DMR 6 s).
- Hunt: `try_next_cc()` (P25) or `dmr_cc_hunt()` (DMR) unchanged, 2 s probes included (too short for the SPS and
  modulation hunt to reach a CQPSK trial: a simulcast system whose C4FM chain decodes nothing needs `-mq`, as a
  RadioReference import writes; one the C4FM chain reads in part reaches CQPSK through the chain comparison); a P25
  startup probe restores `trunk_cc_freq` after each probe tune, as DMR probes do, and records `cold_start_probe_hz`.
  A P25 probe tune takes the P25 chain from the session (`dsd_engine_cc_is_p25()`, above), and while a P25-driven
  probe is in flight (`p25_sm_hunt_probe_in_flight()`: `HUNTING`, or `ON_CC` on a hunt probe not yet decoded, under a
  P25 or unknown owner) the frame-sync SPS hunt steps only between 4800/4 and 6000/4
  (`dsd_frame_sync_hook_p25_probe_in_flight()`, installed in `frame_sync_hooks_install.c` and asked once per step
  on the decoder thread, holding the tick guard or not: the engine reads the machine under the guard, taking it when
  free, and the last answer stands while the watchdog holds it; `frame_sync_sps_hunt_p25_probe_next_index()`), the
  sequence a P25-only rotation runs: from 4800/4 it steps to 6000/4, never to 2400/4 or 9600/2. The trunked 4800/4
  dwell (1.875 s) ends just inside the 2 s probe, so 6000/4 gets the probe's last ~125 ms, as under a P25-only set.
  The full rotation resumes when the hunt ends. DMR and NXDN96 control live on 4800/4; NXDN48 control (2400/4) is not
  found by a P25-driven probe (a decided limit: a dead-start NXDN48 system names `-fi`). With no other eligible row,
  DMR probes the row it last probed again when that tune never completed (a failed or expired probe: only a success
  retires the failure that gates every frame), and listens on one the radio is on (`dmr_cc_next_mapped_candidate()`).
- Ending: validated control naming another protocol's owner (DMR control, an NXDN CAC through
  `nxdn_cc_hunt_note_control()`, a confirmed Type-D SCCH through `nxdn_cc_hunt_note_type_d()`, after which nothing
  hunts), or a Hytera XPT site identifying itself, ends the hunt in place (the owner's ordinary recovery follows; a grant either machine followed meanwhile stands, and only the
  startup bookkeeping ends). DMR content that names DMR without confirming a control channel (a data preamble, a
  Capacity Plus status naming a rest LSN the map lacks, a traffic channel) does not end DMR's hunt, which revisits
  only a confirmed anchor, never the alias such content staged (`dmr_cspdu_init_cc_anchor()`); a call followed
  meanwhile ends only the bookkeeping. Trunking off or an input switch parks it at once. Lost eligibility or
  admission, an operator move (`p25_sm_cold_start_note_retune()` / `dmr_sm_cold_start_note_retune()`, from the
  carrier boundary's guard-holding kinds and the trunked cycles) or an unexpired window at a decision point parks it
  only once `dsd_trunk_tuning_pending_request()` is 0: a probe in flight is resolved first, any other tune in flight is
  waited for, and only a failure is replaced, by a settle tune to the frequency the decoder's tuner options name,
  tracked like a probe (DMR's through `cc_tune_request_id`, so its 5 s deadline fails one the backend never answers, as
  it fails a probe). A settle tune carries no symbol profile (`ted_sps` 0): it keeps the receive profile in force, which
  a mode change may have chosen. The hunt never disowns or completes a request. Before an owner is proven, the
  frequency entry of a set that presumes P25 (`-ft`, Auto) stays an operator move even after a raw P25 sync: `manual_frequency_selects_p25_cc()` does not route
  it to `p25_sm_select_control_channel()`, whose acquisition no admitted recovery would time out and whose control
  channel nothing has validated. Under `-1`/`-2`, whose P25 recovery is admitted, the entry stays the P25 manual
  control-channel selection: it supersedes the startup hunt, and the ordinary recovery hunts the map if it is dead.
- Provenance: `p25_sm_note_validated_cc()` (TSBK FEC counters, outbound MBT, LCCH MAC) notes the activity and, with
  trunking on, establishes `p25_cc_freq` from the tuner at the decode boundary; when that tuner query fails, the same
  boundary uses the probe the tuner completed (`cold_start_probe_hz`), with the type the sync names
  (`p25_sm_seed_cc_at()`, as the tuner seed), before a handler of the same block (a grant) can end the probe's
  acquisition. Ending a hunt in place on a P25 owner does the same if the probe's tune was still
  unresolved there.
- Tests: `CORE_TRUNK_COLD_START`, `RUNTIME_TRUNK_SCAN_HOOKS`, `P25_SM_COLD_START`, `P25_MBT_DECODE`,
  `ENGINE_DMR_CC_RECOVERY`, `ENGINE_COLD_START_HUNT`, `APP_COMMAND_QUEUE`, `DMR_CONFIDENCE_GATE`,
  `ENGINE_TRUNK_RETUNE_REGRESSION`, `RUNTIME_FRAME_SYNC_HOOKS`, `FRAME_SYNC_POLICY`, `FRAME_SYNC_INTERNAL_HELPERS`.
- NXDN (`src/protocol/nxdn/nxdn_cc_hunt.c`, `<dsd-neo/protocol/nxdn/nxdn_cc_hunt.h>`, context in state-ext slot 28
  `DSD_STATE_EXT_PROTO_NXDN_CC_HUNT`): NXDN has no trunk state machine and no other control-channel hunt, so this one
  runs at session start and after a control channel is lost. Eligibility is
  `dsd_state_trunk_nxdn_hunt_eligible()` (the startup conditions with an NXDN owner allowed) under
  `dsd_trunk_nxdn_recovery_allowed()`; with an NXDN owner only after a CAC confirmed a control channel. Ticked like the
  others (`nxdn_cc_hunt_tick_locked()` in `dsd_engine_cold_start_tick_locked()`, between frames while
  `nxdn_cc_hunt_due()`, so a window a followed call dropped re-arms after the return, and in the decoder-ticks hook);
  notes on operator moves from the same carrier boundary and cycle sites (`nxdn_cc_hunt_note_retune()`, which also
  resets the per-carrier NXDN evidence: a trunked cycle crosses no carrier boundary; every accepted manual tune of
  app-control's `request_manual_tune()`, a return to the control channel included, notes it too). The window (6 s) runs from the later of its arming
  and `dsd_state::nxdn_confirm_lock_m` (stamped by `nxdn_confirm_end_frame()` when a frame's own CRCs confirmed the
  transmission, never cleared); it is dropped on a voice channel and restarted while a tune the hunt does not own is in
  flight, so a DISC return still pending when `trunk_is_tuned` drops starts it at that tune's completion stamp, even
  when the decoder could not tick while the stream waited for it. A context created while NXDN is named the owner
  belongs to a reused state's next run (the engine's cleanup frees the context): the owner goes back to unknown. Probes, settle tunes,
  deadlines and parking follow the DMR startup hunt's rules (a probe's 2 s dwell runs from its tune's completion); each
  hunt tune also resets the per-carrier NXDN evidence
  (`nxdn_confirm_reset()`, the SACCH superframe and alias assemblies, and the data call context through
  `nxdn_reset_data_call_state()` in `nxdn_data_call.c`), so a weak streak, a sticky confirmation, a half-assembled
  alias or a data call's header never crosses carriers (the RAN
  stays, as at every trunked move within the system's map, issue #575), and
  restores both control-channel aliases. `nxdn_handle_cac()` calls `nxdn_cc_hunt_note_control()` on every CRC-good CAC,
  before element decode: an unresolved probe is not credited; otherwise the frequency (the completed probe, else one
  tuner query per tuning generation, a failed one included, forgotten at an operator move and at a rigctl reconnect,
  `nxdn_cc_hunt_forget_attribution()` from `svc_rigctl_connect()`, and replaced by a site message's own tuner query,
  `nxdn_cc_hunt_note_tuner_frequency()` from `nxdn_anchor_control_channel_from_current_tuner()`) becomes `p25_cc_freq`/`trunk_cc_freq`, NXDN is
  named the owner and the post-loss window re-armed; a CAC whose frequency no tuner names still proves a Type-C control
  channel, and its loss hunts the map without an anchor. A post-loss hunt revisits that control channel between other rows, judged by the last attempt, so one the tuner
  refuses does not hold the hunt.
  The confirmed branch of `nxdn_scch_prepare_type_d()` calls `nxdn_cc_hunt_note_type_d()`, which counts only SCCHs in
  `NXDN_CONFIRM_WEAK_OBSERVES` frames running (`nxdn_confirm_scch_confirmed()`: the gate's own SCCH streak, which other
  channels neither extend nor break, and a frame rejected at its LICH or a completed tune (a new trunk tuning
  generation) ends, `nxdn_confirm_frame_rejected()`; not a confirmation left by a CAC, a SACCH or another carrier), on a control or a
  followed voice channel: owner named, hunt ended (a RadioReference apply's `rr_apply_reacquire()` calls
  `nxdn_cc_hunt_note_system_replaced()`, which forgets that and an NXDN owner for the new system; the apply also drops a
  P25 or DMR owner its decode mode no longer decodes),
  any CAC-confirmed control channel forgotten, so a Type-D site never hunts afterwards (an idle, silent Type-D site at
  start is walked like a dead one until confirmed traffic holds the window). A hunt tune, like every trunk hook tune
  (grant follows, the P25/DMR probes), is not a carrier boundary: beyond what the hunt resets itself, a call, its voice
  crypto, the displayed alias and an MBE capture are ended by the no-carrier pass, which continuous syncs can defer. Tests: `NXDN_CC_HUNT`,
  `ENGINE_COLD_START_HUNT`, `NXDN_CONFIRM`, `CORE_TRUNK_COLD_START`, `RUNTIME_TRUNK_SCAN_HOOKS`.

P25 Phase 2 voice dispatch (`src/protocol/p25/phase2/p25p2_frame.c`, issue #651). A `processP2()` window is four
timeslots, 700 dibits after a 20-dibit sync, 180 dibits each. `p2_scramble_offset` is the superframe timeslot (mod 12)
of window timeslot 0 and `ts_counter` (0-3) indexes the window, so a timeslot's pair is
`((p2_scramble_offset + ts_counter) % 12) / 2` (`p25p2_pair_index()`): pairs 0-4 are voice positions, pair 5 is the
SACCH, and the slot is the timeslot's parity. A FACCH MAC belongs to its burst's slot, a SACCH MAC to the other slot.

- Window position (`p25p2_window_position()`). Each window is measured against the previous one on the same carrier and
  retune token by `state->symbolcnt`: a distance of exactly the sync is contiguous (start = previous + 4); the sync
  plus whole timeslots is a missed sync that skipped them (the start advances and the skipped timeslots retire as lost
  through `p25p2_retire_timeslots()`; 12 or more close the voice streams instead). A decoded channel-1 I-ISCH wins over
  continuity, and a disagreement closes the streams. Without one, continuity sets the descrambler offset, which a
  window with no I-ISCH used to keep from the previous window, 4 timeslots stale. A window that closes the streams
  this way (unproven windows included) breaks the playout's timeline (`dsd_p25p2_playout_break()`: tails still queued
  play, and what follows starts a later pair, so a stream opening beside such a tail waits behind it), and promotes a
  deferred rekey before its bursts decode: the pair it waited for will not complete.
- A window nothing locates (no I-ISCH and no continuity, the first after a tune included) is unproven: its 4V/2V,
  FACCH and SACCH bursts are not dispatched, since a CRC proves a MAC payload, not which slot it belongs to. LCCH
  (DUID 13/4) still is, but both slots' pending MAC assemblies are discarded and the VPDU drops the window's fragment
  segments (`p25p2_window_slot_unproven()`).
- Retune token (`p25p2_retune.c`): `dsd_trunk_tuning_generation()`, `dsd_trunk_tuning_pending_request()` and the
  frame-reset generation `p25_p2_frame_reset()` bumps. `p25p2_process_duid()` snapshots it per window and stops as
  soon as a dispatch, a post-timeslot (whose rekey commit can lock out and return to the CC) or a retirement changes
  it, and `processP2()` dispatches nothing of a window whose missed-sync retirement changed it; the XCCH MAC handlers return right after a `process_MAC_VPDU()` that changed it, and the VPDU segment loop stops
  after such a segment. So nothing of the old channel's PDU acts on the new assignment, and `p25_p2_frame_reset()`
  zeroing the dispatch's `ts_counter` can no longer replay timeslots as phantom 4V bursts. The DUID-13 LCCH timeout
  re-evaluates its release verdict after the burst's MAC PDU (`p25p2_duid_lcch_release_due()`).
- MAC_IDLE carrying a grant. The SACCH and FACCH MAC_IDLE handlers observe the IDLE, then dispatch its payload, then
  emit it. A grant the payload carries for the IDLE's slot is newer than the IDLE and keeps its policy route, service
  options and crypto state. The SM numbers each slot grant stamp from one counter (`p25_sm_slot_ctx_t.grant_seq`), and
  the handlers capture `p25_sm_slot_grant_seq()` before the payload and pass it to `p25_sm_emit_idle_at()` and
  `p25_sm_slot_grant_since()`. Time cannot order them: the decode clock holds still across a window's dispatch during
  an I/Q replay, and a coarse system clock can do the same, so the grant shares the IDLE's reading (issue #747).
- Voice. Each processed voice burst gets a serial, and the decode gate's decision for it (`p25p2_prepare_voice_crypto()`,
  the ESS enable) becomes the playout's burst verdict (see Core). After slot 2's timeslot of every pair the pair plays,
  then deferred rekeys commit, whatever the output format or rate. A pair that retirement completes (a missed sync's
  skipped timeslots, or an abort's) plays and commits the same way, before the next voice burst decodes. An abort
  retires the aborting timeslot and the rest of the window as lost, then commits pending rekeys.
- Timeouts. The LCCH timeout and the no-voice fallback release first and tear the call down
  (`p25p2_teardown_call()`) only once the receiver has left the voice channel (`p25p2_release_then_teardown()`): a
  return the tuner refuses or defers keeps the channel with its gates, crypto and voice streams.
- Flush entry points. The release hook `dsd_p25p2_flush_partial_audio()` runs from
  `p25_release_return_to_cc_accepted()` only once the return-to-CC tune is accepted (OK or PENDING), so a FAILED or
  DEFERRED attempt that keeps the voice channel never splits a pair, and from `p25_sm_abandon_carrier()`.
  `dsd_p25p2_flush_partial_audio_slot()` closes one slot's stream. Manual CC selection (`p25_cc_selection.c`)
  discards the playout before `p25_p2_frame_reset()`.

Tests: `P25_P2_VOICE_OUTPUT` (end to end through the DUID dispatch, against a tagging stand-in vocoder),
`P25_P2_RELIABILITY` (window position, retirement, unproven windows, the retune token), `P25_P2_VPDU_GRANTS`,
`P25_P2_XCCH_HELPERS`, `APP_P25_CC_SELECTION`.

Call frequency and access-code provenance (issue #575). A call's canonical `frequency_hz` and the live access codes
(`dmr_color_code`, `dpmr_color_code`, `nxdn_last_ran`) are read as describing the carrier the call is decoded on, so:

- A call observation's `frequency_hz` is either a frequency its own message carries (a P25 TSBK/LCW/MBT/VPDU grant,
  a DMR CSBK grant's activity, an EDACS grant) or the frequency of the carrier the call is decoded on. Messages that
  carry none read a global only when it names that carrier:
  - `trunk_vc_freq[]` only while `dsd_opts_trunk_vc_followed()` (the DMR voice LC `dmr_flco_publish_voice()`, the NXDN
    VCALL `nxdn_vcall_publish()`, the P25 Phase 1 ESS epoch `p25_crypto_ensure_phase1_call()`); with trunking off every
    DMR grant in `dmr_csbk.c` writes it for display while the receiver stays put, so it names another channel. The
    P25 conventional voice publication (`p25_sm_conventional_frequency()`), which runs only with trunking off, never
    reads it.
  - P25 `p25_vc_freq[]` while trunking (the ESS epoch): followed, it is the voice channel. With trunking off a grant's
    frequency belongs to the call whose target it named, and to no other: every trunking-off writer is target-matched,
    writing only for a grant or grant update that names the target of a call active on this carrier
    (`p25p2_vpdu_update_playback_if_match()`, used by both MFID90 regroup grants too, the VPDU telephone and SNDCP
    data grants, and `p25_telephone_update_nontrunk_vc_freq()` in `p25p1_pdu_trunking.c`), and each keeps the
    frequency with that target, per slot whose active call has it (`p25_conventional_grant_note()`:
    `p25_conventional_grant_freq[]`/`_target[]`). The Phase 2 writer fills both slots' `p25_vc_freq[]`, so the
    conventional publication (`p25_sm_conventional_frequency()`) and the ESS epoch read the pair instead
    (`p25_conventional_grant_frequency()`), and a call takes it only when it has that target: a second talkgroup on
    the carrier, or an ESS epoch that names none, takes none and its row falls back to the tuned frequency.
    `noCarrier()` and the carrier boundary clear both with trunking off. Trunk scan keeps the pair in each target's
    snapshot beside `p25_vc_freq[]`, and a fresh target starts without one: a rotation between conventional P25
    targets runs neither, so the incoming carrier's call with the same target would otherwise take the other's grant.
  - The DMR trunk SM's voice-sync publication stamps its own tuned `vc_freq_hz`. The NXDN VCALL names the channel of
    the followed route (`dsd_state::nxdn_follow`: written after an accepted tune, cleared by DISC's return and a Type-D
    termination, kept in each trunk scan target's snapshot, and valid only while the receiver is still tuned to its
    frequency, `nxdn_follow_valid()`), never the last grant's (`nxdn_grant_chan`), since a duplicate assignment decoded
    while tuned moves that without moving the receiver.
  - The P25 encryption lockout's notice for a call the slot has not published (the synthetic snapshot of
    `p25_lockout_snapshot_from_observation()`) carries the frequency of the grant the SM tuned the slot to when that
    grant names the locked-out target (`p25_lockout_grant_frequency()`), as `p25_call_publish_observation()` stamps
    the SM's own calls. The notice renders before the release returns the tuner to the control channel, so on a
    radio input the live tuner reads the same channel; off one (an audio input with a rigctl peer), where the tuner
    reads nothing, the grant is the only source.
- `dmr_color_code` goes back to 16 ("not decoded") at the carrier boundary, with `dmr_confidence_reset()`, in
  `dsd_engine_forget_carrier_codes()`, which `no_carrier_reset_decode_state()` runs in `noCarrier()` and in
  `dsd_engine_reset_no_carrier_state()`, and the carrier boundary runs (`dsd_engine_carrier_boundary()`, below). BS
  mode rewrites it at the confidence relock before any burst is dispatched; DMR MS mode has no lock, so without the
  reset a call opening before its embedded code decodes, or after a failed QR(16,7,6), took the previous carrier's
  code. It is reset there rather than inside `dmr_confidence_reset()`, whose
  BS burst-error callers in `dmr_bs.c` stay on the same carrier.
- `nxdn_last_ran` goes back to `(unsigned)-1` at the same boundary (RAN 0 is a legal code). The SACCH writes it only
  once the transmission is confirmed, and `nxdn_confirm_reset()` restarts confirmation at every no-carrier pass, so a
  call opening on its FACCH1 VCALL read the previous transmission's RAN; on a control channel the CRC-gated CAC sets
  it again at once. The `LIMAZULUTWEAKS` reset in `noCarrier()` and the legacy `-Y` step's reset remain.
- `nxdn_last_ran` also holds values that are not a RAN, which the terminal shows in its place: the IDAS (Type-D) SCCH's
  area bit (`nxdn_scch_prepare_type_d()`, confirmed transmissions only) and the fixed 7 the DCR SACCH2 gives a confirmed
  transmission (`nxdn_update_sacch2_identity_state()`). Every write of a value sets `nxdn_last_ran_stand_in` beside it
  (the `(unsigned)-1` resets need not, since no reader takes -1): 1 for those two, 0 for the RAN fields (CAC,
  FACCH2/UDCH, SACCH, and the site information message's site code), so `dsd_access_code_current()` gives an IDAS or DCR
  call no access code, and a RAN decoded afterwards reads again. The flag marks the value rather than the carrier:
  `nxdn_location_category` "Type-D", which labels the terminal's "IDAS - Area", is set by any SCCH that passes its CRC,
  noise included, and kept across carriers with trunking on, so it could hide a real RAN; and no state marks a DCR
  carrier (`nxdn_dcr_sf_message_type` drops to 0xFF on every SACCH2 CRC failure and survives the carrier boundary). The
  live displays follow the mark too: outside IDAS, whose area keeps its own label, the terminal's NXDN line prints
  `RAN: --` and the Qt site sheet shows no RAN for a stand-in, which there is DCR's 7 (`ui_render_nxdn_site_line()`,
  `MetricsModel::fillNxdnSite()`).
- NXDN site identity (`nxdn_element.c`). `NXDN_Elements_Content_decode()` is handed buffers of every channel's size,
  down to the 26 elements of a non-superframe SACCH whose 6-bit CRC passes noise one time in 64, so each parser checks
  the length it was given before reading (`nxdn_element_has_bits()`: SRV_INFO 72, 80 on Type-D; VCALL/TX_REL/DISC 64;
  assignments 72, 80 for the DFA; VCALL_IV 72, 30 on Type-D; the ARIB VCALL and alias and the standard alias 72).
  The Type-C location ID (`nxdn_location_id_handler()`, NXDN TS 1-A 6.5.2: Global 10-bit system and 12-bit site,
  Regional 14/8, Local 17/5, category 3 reserved) is printed whenever it decodes but written to
  `nxdn_location_category`/`_sys_code`/`_site_code` and `nxdn_last_ran` (the site code's low six bits) only once the
  transmission is confirmed, as the SACCH's own RAN is, and never for the reserved category; a zero system or site
  code leaves the stored one. A Type-D (IDAS) site's identity has fields of its own, `nxdn_idas_system_id` (19 bits,
  TS 1-E §6.6.2) and `nxdn_idas_site_code` (1-250, §6.6.5), 0 for not received, reset and snapshotted with the
  location: a Type-D SRV_INFO (§6.4.3.1, spare 5 + System ID 19 + Site Code 8) writes both once confirmed, and the
  SCCH site ID message (§6.5.3.1 Fig 6.5-8: Site Type 2 bits, Site Code 8) the code once
  `nxdn_confirm_scch_confirmed()`; neither writes a RAN or the Type-C fields. Which SRV_INFO layout applies, and
  whether it places the control channel here, comes from the frame's own LICH, which `nxdn_frame()` holds in
  `dsd_state::nxdn_frame_channel` while the frame's channels decode (`nxdn_frame_channel` in
  `<dsd-neo/protocol/nxdn/nxdn.h>`: a CAC is the control channel, an RTCH2 profile Type-D, anything else traffic), never
  from the retained "Type-D" category: only a control channel's SRV_INFO refreshes the control-channel sync and anchors
  the CC to the tuner. An outbound SCCH INFO2 has the INFO4 format (§6.5.3.3), so its idle, halt, free repeater and site
  ID messages (IDs 2046, 2045, 2044, 2041) go through the INFO4 handling (`nxdn_scch_handle_repeater_status()`) and never
  become the call's target; its busy repeater message keeps the target handling of `nxdn_scch_handle_info2()`. The
  terminal's IDAS line and the Qt site view show the IDAS fields, the NXDN ones the Type-C location.
- NXDN trunking admission and preemption (`nxdn_element.c`; NXDN has no trunk state machine). Two records in
  `dsd_state`, both saved with a trunk scan target's snapshot (and so restored by a failed switch's rollback):
  - `nxdn_follow` (`dsd_nxdn_follow_route`): the route the receiver last followed a grant or busy repeater message to
    (frequency, channel, Type-D repeater), written only after an accepted tune. It holds only while the receiver is
    still tuned there (`nxdn_follow_valid()`: `dsd_opts_trunk_vc_followed()` with `trunk_vc_freq[0]` equal), so an
    engine-level release (the hangtime return, a scan or operator move) invalidates it without a word to NXDN. It names
    where the receiver went, never which call is on it; the VCALL's channel comes from it.
  - `nxdn_call` (`dsd_nxdn_call_identity`): the held call as preemption judges it. The followed grant's identity, then
    reconciled from the canonical call after a verified VCALL publication and after an SCCH identity that belongs to
    the receiver's repeater (`nxdn_call_identity_reconcile()`), so a new call on the followed carrier is what a
    candidate is compared against. Its private/group class is the naming message's own (a VCALL's call type, 4 =
    private, as admission judges it; a busy message's G/U bit), else the canonical call's group or private kind, else,
    for a generic voice call (late entry, an INFO2 target message), the held call's when it names that target; a generic
    voice call is never read as group, and one of no known class leaves the held call unknown (only Hold displaces it).
    Its `follow_m` moves only when the identity changes (target, kind, or frequency
    when both are known); one heard on another frequency than the receiver's is not known here. A follow replaces the
    previous record outright unless it re-follows the call the receiver still holds (`NXDN_FOLLOW_HELD`, a Type-D busy
    message repeated past the voice window): a record the engine's hangtime return left behind never lends the
    followed call its follow time or binding, even for the same target on the same channel. It is bound to the
    canonical call it names (`call_epoch`: that call's `dsd_call_snapshot_transmission()`; a VCALL's or SCCH's identity
    binds to the call on the air, and so does a follow's that found the announced call in place
    (`nxdn_on_air_call_is()`: its target, or none yet, as a late-entry call before its VCALL, never another target a
    message named; and its kind when it has one; a follow that left the carrier ended the call
    there, so an active one is on the followed carrier), whose release then ends the held call, so the next
    transmission never inherits its identity; any other follow's binds to the first call to begin after the tune, except
    the same call named again while its call is faded but may still be reacquired, `dsd_call_snapshot_may_reacquire()`,
    which keeps its binding) and never outlives it
    (`nxdn_call_identity_refresh()`, before every read): one still awaiting its call holds only on the route it was
    followed to (`nxdn_held_call_on_followed_route()`), so once that route is abandoned (the engine's hangtime or
    no-carrier return, a release, a retune elsewhere) it goes, and a call heard there later, without another grant, never takes
    it; its call's end, a sync loss included, clears it except on the
    followed route within the hangtime (a conversation is held in its gaps); another call clears it, and on the followed
    route one that began within the hangtime of the named call leaves a gap record (unknown, `valid` 0) whose renaming
    as the same call (target and kind, binding that call) keeps `follow_m`. A new call never inherits the identity, its
    priority or its dwell. A release or Type-D termination on the followed route updates the binding from the ended
    call at once (`nxdn_call_identity_end()` refreshes it: the call's own end time), and so does a sync loss, which no
    NXDN message reports: the engine's no-carrier pass calls `nxdn_held_call_note_call_end()` (`nxdn_held_call.c`,
    which only stamps the bound call's end and writes no ledger; a held call still awaiting its call, a follow whose
    voice no message named, binds there to the ended call when that is the first to begin after it was named and only
    while its followed route holds, `nxdn_held_call_on_followed_route()`, so the next transmission never takes it and a
    follow abandoned before its call began never names a call heard there later) right after it ends the calls. A reply's gap is so
    measured from the end, however long ago the identity was last read. A held call known only from the call heard there (no
    valid route: a resident call, no grant tune) ends with that call: a release, DISC or Type-D termination clears it
    (`nxdn_call_identity_end()`), as its canonical end does, so the next call there is unknown until a message names
    it. A Type-D held call (`type_d`: followed from a busy message, named by an SCCH, or by a VCALL on an RTCH2 or the
    followed Type-D route) is judged as Type-D admission judges it, by its target alone (`nxdn_held_policy_source()`):
    no source fallback for a group call and no source for a private one, so an INFO3 source, which may misname the
    talker, never reaches policy.
  - The receiver's frequency (`nxdn_receiver_freq()`): the valid route, else the tuned voice channel, else the
    tuner's own (`dsd_opts_tuned_freq_hz()`), else, rigctl on an audio input, the peer's, asked through the
    hunt's per-tuning-generation attribution (`nxdn_cc_hunt_receiver_frequency()`: one rigctl round trip per
    generation, a failed one included; a VFO moved outside the decoder is not seen within a generation).
  - Type-C (`nxdn_vcall_assgn_admit()`): a duplicate assignment (0x05) heard while tuned reaches admission with the
    assignment's call-type gates (one heard off a voice channel still is not followed), and never untunes the receiver:
    one past the call's hangtime is converted and marked `dup_after_hangtime`, and a refused one leaves
    `trunk_is_tuned` set for DISC or the hangtime return (and the voice channel is never anchored as the control
    channel). Order: policy, control-channel or frequency refusal; untuned: follow; on the channel the receiver is on,
    a repeat of the held call known there (its target and class): nothing to do (with none known, as after its call
    lapsed past the hangtime, the assignment goes on through admission and a follow in place makes it the held call,
    its key loaded); the held talkgroup over a held call that is not of it (unknown included): follow; a duplicate past
    the hangtime: follow; within the hangtime, an outranking preempting candidate (`nxdn_preempts()`): follow; else
    refuse.
  - Type-D association (`nxdn_idas_assoc()`): a busy repeater message's or INFO2 target message's go-to repeater is
    MATCH (its mapped frequency is the receiver's), OTHER, or UNKNOWN (no repeater 1-30, unmapped, or the receiver's
    frequency unknown). The live call takes the message's target (`nxdn_scch_enrich_identity()`) only on MATCH, and a
    busy message's G/U bit sets the canonical call's group or private kind, so a later INFO2 for that target never reads
    a previous call's class; an INFO2 naming another target than the live call's names another call, which takes no
    class from it (`nxdn_scch_named_kind()`: a generic voice call, so the held call is unknown and yields only to Hold
    until a classified message names it); UNKNOWN never relabels. While voice is live a busy message for OTHER takes the receiver for
    the held talkgroup over a call not of it, else by preemption; MATCH and UNKNOWN never move it; with no voice for a
    second the message is followed as before. INFO3 names no go-to repeater (`rep1` is a free repeater), so its source
    enrichment is unchanged and can misname a talker (documented limit); it never changes the target or the policy. A
    source other than the live call's makes the canonical layer open another epoch of the same target and class, which
    the held call follows there (`nxdn_call_identity_follow_source_update()`), its follow time kept and its source left
    alone.
  - Ledger: `nxdn_preempts()` describes the held call (`dsd_tg_policy_describe_followed_call()`, start `follow_m`; a
    Type-D one by its target alone) before `dsd_tg_policy_should_preempt()`, with the candidate's route
    `requires_tuner_retune`; a held call whose identity is not known is never preempted, only displaced by Hold. An
    accepted tune notes the followed call (`nxdn_note_follow()`): a follow that was no preemption first clears the
    ledger, so a call left there (the engine's hangtime return) does not read as a takeover; a preemption commits its
    cooldown (`dsd_tg_policy_commit_preemption()`); a Type-D busy message repeated past the voice window for the call
    the receiver still holds (`nxdn_follow_holds_call()`, read before the tune) describes it from its retained
    `follow_m`, so the repeat never restarts the dwell. An accepted Type-C or Type-D follow that leaves the carrier the
    receiver was on (`nxdn_tune_leaves_carrier()`, the receiver's frequency read before the tune; unknown counts as
    leaving) ends slot 0's canonical call (EXPLICIT, synchronized, `nxdn_end_left_call()`, as DMR's
    `dmr_sm_end_left_calls()`) before the follow is noted, so the followed call's voice, before its VCALL names it,
    opens an epoch of its own that the held call binds to, never continuing the displaced call under its identity and
    policy, and no sync loss reacquires the displaced call there; a follow on the same carrier (the in-place repeat of
    the held call, or a resident call's announcement) leaves the call on the air alone, and the held call binds to it
    when it is the announced call or one nothing has named yet. DISC's return and a Type-D termination (repeater 31) tune clear
    the route, the held call and the ledger (`nxdn_follow_release()`). A frame channel whose message retunes the
    receiver (a follow, a preemption, a termination's return) ends that frame's decoding (`nxdn_frame_left_carrier()` in
    `nxdn_frame.c`: the trunk-tuning generation or the newest unresolved tune request moved, as P25 Phase 2's retune
    token, a pending tune included): its later channels and voice were read on the carrier left, so a VCALL there never
    reopens the displaced call on the new carrier nor plays its voice as the new carrier's. All of it runs in frame
    decoding, under the P25
    SM tick guard the dispatch holds. Tests: `NXDN_GRANT_TUNE_MATRIX` (also built on the real canonical call state as
    `NXDN_GRANT_TUNE_MATRIX_REAL_CALL_STATE`, which adds the cases that need its own start and end times),
    `NXDN_ELEMENT_BOUNDS`, `NXDN_FRAME_ROUTING`, `ENGINE_TRUNK_SCAN`, `ENGINE_NO_CARRIER_RESET`.
- `p2_cc` is the Phase 2 descrambler's seed, which `-X`, a Phase 1 NID or another carrier may have left. It is a call's
  received NAC only while `p2_cc_verified` is set: a burst descrambled with it passed its Reed-Solomon check (a
  scrambled FACCH or SACCH, `process_FACCHs()`/`process_SACCHs()`, or an ESS, `p25p2_process_ess()`, all through
  `p25p2_note_seed_proven()`; it proves the seed `p25p2_process_frame_scramble()` descrambled the buffered superframe
  with, so the seed in force is proven only while it is still that one: a broadcast decoded from an earlier burst of the
  same buffer may have replaced it; when the proof is first set it renders both slots before the burst's MAC PDU is
  dispatched, since that burst may carry the MAC_END_PTT that ends its call and an ended call no longer reads the live
  NAC), or a network status broadcast that passed its CRC named it
  (`p25p2_vpdu_apply_nsb_identity()`). Under `-F` an XCCH MAC_SIGNAL that failed its CRC still reaches the VPDU decoder,
  which `p25p2_xcch_validate_sacch_crc()` allows, so the signal handler passes the verdict through
  (`process_MAC_VPDU_crc()`): its grants and voice still act, but its system broadcasts change no system state
  (`p25p2_vpdu_system_tables_writable()`): the network status's WACN/SYSID/NAC and control channel anchor, the RFSS
  status's site, LRA and IDEN trust promotion, the IDEN updates (0x73/0x74/0x7D/0xF3) with their trust and the
  pending announcements they resolve, the secondary control channel broadcasts (0xE9/0x79/0x69: site fill-in,
  candidates, pending announcements, scan-list fallback), the adjacent-status neighbours, the system service broadcast
  (0x78), the time and date announcement (0x75) and the protection parameters (bridged 0x7F); so it proves no NAC
  either. The TSBK path (`tsbk_dispatch_message()`) and the MBT trunking path (`p25_mpdu_handle_trunking()`) need
  their CRCs under `-F` too. A seed set by hand (`-X`, or `svc_set_p2_params()` for the menu, Qt and API) with all
  three parts nonzero sets `p2_hardset`: no NID or network status rewrites it, and neither a CC selection
  (`p25_sm_forget_selected_site()`) nor the no-carrier cleanup after generic trunk activity
  (`no_carrier_clear_stale_p25_return_hints_after_generic_activity()`) clears it; both still drop its proof. Every other
  proof needs a Reed-Solomon decode that succeeded with its parity check intact, which no option relaxes: a FACCH or
  SACCH that needed the soft-erasure retry, or an ESS that needed `p25p2_ess_decode_with_soft_erasures()`'s erasures,
  still decodes for its content but proves nothing, since erasures spend the code's check (the shortened RS(63,35)
  ESS with all 28 parity symbols erased decodes any payload). And a slot's ESS_B that the carrier left
  (`p25_p2_ess_b_stale`, set when the carrier's decoding is forgotten, cleared by the next 4V burst's fragment) is not
  decoded at all: a 2V burst ahead of any 4V burst would read the old carrier's ALG, KID and MI from it, and
  prove the new seed with it. An unscrambled FACCH/SACCH (DUID 15, 12, 13) never tests the seed, so a call carried only
  by those records no NAC. The mark goes at the carrier boundary with
  the codes above, and whenever a Phase 1 NID, a hand-set seed or a site reset changes `p2_cc`; `p2_cc` itself is the
  descrambling key and is never reset for it.
- Trunk scan skips both resets (`preserve_scan_state`): the per-target snapshot saves and restores the DMR colour code
  with its confidence lock, the RAN with its stand-in mark, `p2_cc` with its proof, and the conventional grant pair.
  What the decoders gathered on the carrier is in no snapshot: a target switch (`trunk_scan_switch_to()`) forgets it
  between saving the outgoing target and restoring the incoming one (`dsd_engine_forget_carrier_decoding()`, the
  evidence and assemblies of the boundary's step 3). The decoder-wide values stay global: `carrier_seq` only counts
  boundaries, and a switch is none; `carrier_source_key` and the RTL symbol cache's centre belong to the one tuner's
  stream; the ESS_B stale mark is set by that forget.
- `dsd_engine_carrier_boundary()` (`<dsd-neo/engine/frame_processing.h>`) is the one carrier boundary: every place the
  receiver leaves a carrier for another runs it, and no caller open-codes any part of it. A tune, a source change or an
  input switch can run beside the watchdog's ticks, so the boundary holds the P25 SM tick guard from its first
  inspection of the state machines through its last step, taking it when the caller does not hold it (`guard_held`
  must be exact: the guard is not re-entrant); a scan step and a replay retune run where no recovery tick runs,
  inspect no state machine and take none. Its order is fixed:
  1. The voice channel a trunking state machine followed, if one is held, is released while its calls are still active
     (`carrier_boundary_release_followed()`): the P25 or DMR state machine comes to rest on its control channel without
     tuning, as trunk scan hands a carrier back (the P25 release drains the Phase 2 playout: each queued frame plays
     with the verdict it was decoded with, and, while its call is active, the talkgroup policy's current one), and the
     shared
     `dsd_engine_release_tuned_call_state()` drops `trunk_is_tuned` and the voice channel frequencies, so
     `dsd_opts_trunk_vc_followed()` stamps no frequency of the assignment left behind. A scan step and a replay retune
     release no state machine: conventional scanning and trunking off exclude both recovery ticks, so none follows a
     voice channel, and a replay retune runs inside a sample read, where the guard's holder is not known.
  2. The outgoing calls end, as a hop, and commit (`no_carrier_finalize_canonical_calls()`), while the live codes are
     still that carrier's.
  3. `dsd_engine_forget_carrier_codes()` forgets the codes: the DMR colour code with its confidence lock, the RAN with
     its stand-in mark, the dPMR colour code, the Phase 1 NAC, and the seed's proof (never `p2_cc`). The NAC goes
     because a NID whose BCH-decoded NAC is the reserved 000 or FFF leaves `state->nac` as it was
     (`p25p1_apply_nac_update()`) while the frame still dispatches; `noCarrier()` resets it too. With them goes the
     evidence that vouched for the carrier's transmissions (`carrier_boundary_forget_evidence()`): the NXDN, dPMR (with
     its CCH evidence), D-STAR, M17 and ProVoice confirmation gates restart, the YSF FICH verdict, the Phase 1 NID's
     evidence and the two-slot TDMA verdict (`tdma_carrier`, see UI, Qt, "Second-slot strip") go, as the no-carrier pass
     restarts them, so the next carrier proves itself again. And every multi-frame assembly that can publish an identity
     or a code goes too (`carrier_boundary_forget_assemblies()`), as the no-carrier pass drops them, since the next
     carrier's next piece could complete one with the carrier left's pieces: the NXDN SACCH superframe (segments, CRC
     marks, part) and alias blocks; the DMR data blocks, short LC fragments and Capacity Plus blocks
     (`dmr_reset_blocks()`), embedded LC, late-entry MI and talker alias, with the alias shown; the P25 MAC fragments
     and Phase 1 talker aliases; the Phase 2 slots' ESS_B with its reliabilities (marked stale, see `p2_cc` above),
     Phase 2 voice streams (the playout drained first, then reset) and staged rekey (`p25p2_frame_forget_carrier()`);
     the ended calls' P25 crypto (`p25_crypto_reset_slot()`: ALG, KID, MI and the LFSR state that steps it); the dPMR
     superframe part; the M17 LSF chunks, packet and signature; the YSF text. The evidence and assemblies together are
     `dsd_engine_forget_carrier_decoding()`, which a trunk-scan target switch runs as well. D-STAR slow data, X2-TDMA
     signalling and P25 link control live within one superframe or frame, which the carrier count covers; the Phase 2
     ESS_A is collected fresh from each 2V burst.
  4. With trunking off, `dsd_engine_forget_untrunked_carrier_state()` forgets what the carrier left beyond its codes:
     what the trunking-off no-carrier pass forgets (`no_carrier_reset_non_trunk_fields_if_needed()`: the P25 voice
     frequencies a grant update wrote, which `p25_sm_conventional_frequency()` gives the call it named over the
     tuner's, the DMR
     rest channel and branding, the NXDN site and channel plan) and the DMR grants' `trunk_vc_freq[]`.
  5. `state->carrier_seq` moves, so a decoder that buffered the left carrier's bursts drops them (see below).

  A stream gap (`DSD_CARRIER_BOUNDARY_STREAM_GAP`, issue #518 D5: a USB recovery, an rtl_tcp reconnect, or one a
  capture recorded) leaves no carrier and runs only what the gap cut (`carrier_boundary_stream_gap()`): the Phase 2
  playout takes the calls' verdicts, the calls end as on a sync loss, the DMR confirmation restarts
  (`dmr_confidence_reset()`: candidate, lock, voice gate, without the colour code
  `dsd_engine_forget_carrier_codes()` would take with it), `dsd_engine_forget_carrier_decoding()` runs, and the
  carrier count moves with the carrier change noted. The codes, the received identity, the followed voice channel and
  trunk-scan ownership stay; the hangtime and visit timers decide as after a fade. It takes the tick guard as a tune
  does. A cut frame unwinds before the boundary runs, outside its tick guard and before any scan or trunk tick; a
  gap found in the hunt is consumed first in the next outer pass. A pending scan tune (which makes `noCarrier()`
  return early) never skips it. An older symbol cache is dropped when a direct reader has already acknowledged its
  gap, without moving the consumed count backwards. Tests:
  `ENGINE_STREAM_GAP`.

  The carrier count guards every decoder that reads more of the air after a unit is whole and before it decodes or
  publishes that unit: a replay read that adopts a recorded retune runs the boundary inside such a read, and the unit
  read whole before it would decode after the boundary ended its calls. Each notes `carrier_seq` where it starts reading
  and, if it moved before the decode, drops what it collected, as on a sync loss, without reporting a decode error.
  Nothing after the drop confirms or publishes for that frame: YSF does not set its FICH verdict from the carrier left's
  FICH, and `processFrame()` reports any frame whose handler saw the count move as `DSD_FRAME_VERDICT_UNPRODUCTIVE`,
  whatever the handler made of it, so the SPS hunt is not told a dropped frame proved anything. A unit whose own symbols
  straddle the move is mixed, and its FEC or CRC decides, as for any noise, unless the unit is a whole frame guarded
  below. Guarded:
  - P25 Phase 2 `processP2()`: the four-burst buffer (`p2_dibit_buffer()`); a boundary between buffers drops the slots'
    ESS fragments, voice streams and staged rekey (`p25p2_frame_forget_carrier()`).
  - P25 Phase 1: `processLDU1()` (link control, read by the seventh voice frame), `processLDU2()` (encryption sync),
    `processHDU()` and `processTDULC()` (decoded before their trailing symbols, published after), `processMPDU()` (the
    header, read before its data blocks).
  - DMR: the BS burst loop (`process_dmr_bs_iteration()`; the loop ends and `dmr_reset_blocks()` drops the data blocks
    and CACH fragments a short LC gathers) and its bootstrap, the BS data burst's live half (`dmr_data_sync()`), the MS
    voice superframe (`dmrMS()`, whose sixth burst assembles the embedded LC), its bootstrap, and `dmrMSData()`.
  - NXDN `nxdn_frame()` (everything after the LICH decodes after the 174-dibit read; the SACCH segments go too).
  - dPMR `processdPMRvoice()` (two frames), D-STAR `processDSTAR()` (slow data decoded after the twenty-first voice
    frame), X2-TDMA `processX2TDMAvoice()` (the six-slot signalling published as the call's encryption), YSF
    `processYSF()` (the V/D type 1 voice and data channels, the full-rate data channels), and the EDACS control frame
    `edacs()` (two of three voted copies can be read before the move).
  - Frames that are one unit, interleaved or convolutionally coded over their whole length and decoded once read
    whole: a boundary late in one leaves it mostly the carrier left's, which the FEC corrects into that carrier's
    content after the boundary ended its call. The D-STAR header (`processDSTAR_HD()`; the superframe behind it is not
    read), the YSF FICH (no evidence, and no call observed on the fallback type; `processYSF()` drops the frame), and
    every M17 frame read over the air: the LSF (`processM17LSF()`), the stream frame with its LICH chunk
    (`processM17STR()`: no frame number, chunk, voice, evidence or terminator), the packet frame (`processM17PKT()`:
    no chunk, and no packet ended, decoded or confirmed) and the BERT frame (`processM17BRT()`).

  Not guarded, each because no unit is whole before its frame's last read: the P25 Phase 1 TSBK (each block decodes as
  read), ProVoice (the two IMBE frames of a pair are interleaved together), the X2-TDMA data burst (read from the frame
  sync's buffer) and EDACS analog voice.

  An input interruption drops the same units: each guard is `dsd_state_frame_read_lost()`, true when the count moved or
  `dsd_state::input_interrupted` is set. A Signal search sets the flag inside a frame when it leaves it (a deadline,
  Skip, Pause or any queued command) or a live read gives up on a stalled input, and so does a gap in the radio stream
  (`DSD_INTERRUPT_STREAM_GAP`, see DSP, "Stream gaps") or a live read that gives way to a queued command while the radio
  has no stream (`DSD_INTERRUPT_YIELD`, see DSP, "Reads while the radio has no stream"), and `getSymbol()` then stands
  erasures in for every symbol until the engine loop consumes it, so the flag stays set wherever a guard sits, also for
  a frame a decoder starts after the cut. The stood-in zeros slice to one constant dibit, which the BCH, Golay, Hamming,
  QR and Reed-Solomon codes can take for their all-zero codeword, so units the count leaves unguarded check the flag
  too, keeping the boundary's behaviour as it was: the P25 Phase 1 NID (the dispatcher decodes nothing and withholds the
  verdict), each TSBK block, each IMBE frame (`process_IMBE()` hands nothing to the vocoder, and the LDUs raise no voice
  activity on it), the ProVoice IMBE pairs, the DMR RC burst, and the DMR data burst's slot type and the MS EMB (decoded
  as soon as read). Descrambled, stood-in M17 symbols can still clear every Golay block of a LICH and Viterbi-decode to
  a stream frame number with its end flag set, which would end the call as a terminator does; the M17 frame guards above
  drop such a frame. A P25 Phase 2 window an interruption cuts is dropped without `p25p2_frame_forget_carrier()`: the
  receiver is still on the carrier, so the slots keep what they gathered and the playout keeps its frames. Behind the
  guards, `processFrame()` withholds a cut frame's SPS-hunt verdict (`DSD_FRAME_VERDICT_WITHHELD`: the cut was the
  engine's), the MBE capture refuses its frames, `dsd_audio_play_decoded_pcm16()` (M17's Codec2 voice) plays nothing,
  the search ignores evidence noted while the flag stands, and `dsd_two_level_symbol_reliability()` gives the stood-in
  symbols no confidence. Nothing sets the flag inside a frame outside a search, a stream gap or a yield, so none of this
  changes decoding there or symbol-file replay.

  It runs at the lowest-level primitives where the carrier changes, so a new command that reaches one is covered:
  - An accepted tune the user asked for: `svc_rtl_set_freq()`/`svc_rtl_set_freq_locked()` (`svc_rtl_tune()`) run it
    at their success point (applied, or pending on a tuner that timed out). That covers `RTL_SET_FREQ` when it does not
    pick a P25 control channel, `MANUAL_TUNE`, and the live Airspy retune of an Airspy or config apply. A refused,
    deferred or failed tune keeps everything, since the receiver stayed.
  - A stream start on another source: every radio stream start (`svc_rtl_start_locked()`, which `svc_rtl_restart()`,
    `RTL_SET_DEV`, the source reopens of a config apply, an input switch and a replay open reach, and the engine's own
    start, `live_scanner_start_rtl_if_needed()`) runs `dsd_engine_note_stream_source()`, which keys the source by its
    kind of device and which one (an RTL-SDR index, an rtl_tcp host and port, an Airspy serial, a SoapySDR device, a
    capture), never its settings or tuning, and runs the boundary when the key moved, or for any replay start, which
    plays its capture again from its start. The first start of a session, and the same source reopened for its gain,
    PPM, bandwidth, squelch or volume, keep the carrier.
  - A config apply that leaves the same radio source on another frequency as `dsd_opts_tuned_freq_hz()` reads it
    (`apply_cfg_radio_input()`), when no start or retune inside it ran the boundary already (`carrier_seq` unmoved).
  - A channel cycle with trunking off: its LCN list and its P25 candidate list name no system the receiver follows.
  - A RadioReference import that tunes its channel on a session that owns a tuner: it runs the boundary first, before
    `decode_mode_apply_value()` and `rr_apply_reacquire()` end the calls with `reset_call_tracking()`, so the followed
    channel's buffered audio still plays under its talkgroup.
  - An input switch (`state->input_boundary`), trunking on or off: the engine loop runs it before `noCarrier()` and
    `dsd_engine_end_input_boundary()`.
  - A `-Y` row commit (`channel_scan_commit()`, then `channel_scan_end_calls()` and the shared reset) and the untyped
    `-Y` step (in `no_carrier_run()`, before the finalize and the shared reset).
  - An I/Q replay's retune the capture recorded, with trunking off, where the read path adopts the new centre
    (`dsd_engine_leave_replay_carrier()`; see IO, "I/Q replay tuned frequency"); with trunking on it is exempt.

  Exempt, and never routed there: a retune within a system under trunking (the return to the control channel, the
  skip and lockout returns, the P25 control channel pick `ui_cmd_handle_p25_cc_selection()` and candidate cycle, and a
  channel cycle with trunking on, whose list is the system's own channels), whose codes stay valid as under automatic
  trunk following, and where the DMR decode gate keeps dispatching the control channel's bursts: forgetting it made the
  first CSBK after a return pending, and `dmr_data_dispatch_burst()` dropped it. The skip and lockout returns under
  trunking still run the no-carrier pass they always ran (`dsd_engine_no_carrier_locked()`). Nor do a trunk-scan target
  switch, whose snapshots carry the codes and which forgets only what the decoders gathered (above); an external controller's retune over the RTL UDP port, which follows a system
  for the decoder as the trunking state machines do and reaches the tuner on the IO thread; or the tuner release,
  trunking toggles and rigctl reconnect, which retune nothing.
- `dpmr_color_code` is set only in `dpmr_publish_call()`, after `dpmr_confirm_is_confirmed()`, from a decoded
  `ColorCode[0]`, with or without a caller identity; the ID printer only prints.
  `no_carrier_reset_call_strings_and_dpmr()` resets it to -1 with the confirmation evidence.
- Between transmissions the terminal's `DMR BS`/`DMR MS - DCC:` and dPMR `DCC:` fields print `--` for these
  sentinels (16, -1; `ui_print_dmr_dcc()`, `ui_render_call_info_dpmr()`), as the NXDN `RAN:` line does, and the Qt
  site sheet leaves an unknown DMR colour code or NXDN RAN out.

Tests: `DMR_FLCO_PRIVACY_MODES`, `DMR_MS_DATA`, `NXDN_ELEMENT_BOUNDS`, `NXDN_DEPERM_PRIMITIVES`, `P25_SM_UNIFIED_CORE`,
`P25_CRYPTO_STATE`, `P25_P1_LOCKOUT_EVENTS`, `DPMR_VOICE_BRIDGE`, `ENGINE_NO_CARRIER_RESET`, `ENGINE_TRUNK_SCAN`,
`CORE_ACCESS_CODE`, `UI_NCURSES_PRINTER_HELPERS`, `UI_QT_METRICS_MODEL`, `APP_COMMAND_QUEUE`, `APP_P25_CC_SELECTION`,
`APP_CONTROL_RR_APPLY`, `RUNTIME_CONFIG_APPLY`, `UI_MENU_AIRSPY_CONFIG_FAILURE`, `UI_MENU_AIRSPY_CONFIG_TUNING`,
`P25_P2_VPDU_GRANTS`, `P25_P2_XCCH_HELPERS`, `P25_P2_RELIABILITY`, `P25_P2_ESS_CARRIER`, `ENGINE_CHANNEL_SCAN`,
`ENGINE_INPUT_BOUNDARY`, `UI_MENU_SERVICES`; the decoders' carrier-count guards: `P25_P2_RELIABILITY`, `P25_P1_LDU1_HELPERS`,
`P25_P1_LDU2_HELPERS`, `P25_P1_HDU_HELPERS`, `P25_P1_TDULC`, `P25_P1_MDPU_HELPERS`, `DMR_BS_SYNC_TIMES`,
`DMR_DATA_SYNC`, `DMR_MS_DATA`, `NXDN_FRAME_ROUTING`, `DPMR_VOICE_BRIDGE`, `DSTAR_PROCESS`, `X2TDMA_VOICE_HELPERS`,
`YSF_DCH_DECODE`, `EDACS_FRAME_VERDICT`, `M17_FRAME_CUT`, `ENGINE_PROTOCOL_DISPATCH` (a split frame's verdict).

Received identity note points (`dsd_state::rx_id`; see Core). Each protocol notes the identity a message carries where
the message passed its own check, with that message's values, an unchanged repeat included (the note precedes any
"value changed" test), and never a seed, preset, cache, anchor, voice LCW, fill-in or soft-recovered word:

- P25: the signal search's site note points, now in `p25_search_site.h` for both: `p25_search_note_network()` (TSBK
  network status, MBT network status, Phase 2 network status MAC) and `p25_search_note_rfss()` (TSBK RFSS status through
  the vPDU bridge, MBT RFSS status, Phase 2 RFSS status MAC) note the network or the site, bound to the SYSID the message
  carries as the search binds it, and the location registration area; a Phase 2 network status also notes the NAC it
  carries (`p25_search_note_network_nac()`), so an unscrambled SACCH's records its own NAC whatever seed is set. Phase 2
  MACs note only with `ctx->crc_ok` (`p25p2_vpdu_system_tables_writable()`). The Phase 1 NID's NAC at the BCH accept
  (`p25p1_apply_nac_update()`, reserved $000/$FFF excluded); the Phase 2 seed's WACN, SYSID and NAC once a burst
  descrambled with the whole seed passed RS with its parity kept (`p25p2_note_seed_proven()`). The control channel at
  `p25_sm_note_validated_cc()` (a CRC-passed TSBK, outbound MBT or LCCH MAC), once the record names a SYSID.
- DMR: the colour code at the BS confidence lock and relock (`dmr_confidence_note_evidence()`, before the burst is
  dispatched) and the MS slot type's Golay(20,8) / embedded QR(16,7,6) accepts (`dmr_data.c`, `dmr_ms.c`); the Tier III
  system code and site text at `dmr_tiii_note_search_site()` (C_ALOHA with a genuine CRC, C/P_SYS_PARMS short LCs),
  which notes the record whether or not a search runs; the control channel at C_ALOHA and C_SYS_PARMS. Connect Plus site
  text and brand from its control (0xA, with the control channel) and traffic (0x9) short LCs and the brand from its
  CSBKs; Capacity Plus rest LSN and brand from the channel status CSBK 0x3E and the voice LC, and site text, rest LSN
  and brand from its short LC; the Hytera XPT brand from its CSBKs, LC and short LC (XPT sends no site number of its
  own), and the site's free channel (`dsd_rxid_note_dmr_xpt_free_lcn()`, the view's `Free LCN n` and the terminal's
  `Free LCN: n;`) from its site status CSBK 0x0A, call alert LC 0x09 and short LC 0x8. The site text is its parts
  joined by ` · ` ("TIII Small · Net 12 · Site 3.1 · SYS 0A3F", "Cap+ · Site 2", "Con+ · Net 3 · Site 5"). The colour
  code a slot type or EMB split by a carrier boundary would carry is neither noted nor fed to the confidence lock
  (`dmr_data_carrier_left()`, `dsd_state_frame_read_lost()` before the decode). A CSBK or full LC counts as checked only when the slot's `event_crc_invalid` is clear, so one RAS accepted
  under `-F` (and an MBC whose header CRC failed) is decoded and followed but names nothing (`dmr_csbk_crc_checked()`,
  `dmr_flco_crc_checked()`); a short LC reaches its handler only after Hamming(17,12,3) and its CRC-8. Nor does the
  P_CLEAR the encryption lockout synthesizes to release the channel (`dmr_cspdu_release_channel()`, FID 0xFF, handled as
  a checked CSBK): it releases as a received one would, and `dmr_csbk_crc_checked()` refuses every note while it runs.
  NXDN's synthesized lockout DISC reaches no note point (its handler notes nothing), and P25 synthesizes no message.
- NXDN: the RAN of a CRC-passed CAC (with the control channel, before its elements can retune), FACCH2/UDCH, and a SACCH
  once the transmission is confirmed (`nxdn_deperm.c`), never the RAN a site code implies; the Type-C location at the
  location handler's own gate (confirmed, category not reserved); the IDAS area bit and site ID under
  `nxdn_confirm_scch_confirmed()`; a Type-D SRV_INFO's System ID and site code once confirmed.
- dPMR: the colour code in `dpmr_publish_call()` after `dpmr_confirm_is_confirmed()`. M17: the CAN in
  `M17decodeLSF()` only with `publish_canonical` (the LSF's CRC passed), never while the encoder monitors what this
  station transmits (`s_m17_monitoring_local_tx`).
- EDACS (`edacs_process_valid_frame()`, both messages' BCH re-derived): a standard site ID notes the site and, unless
  auxiliary, the control channel LCN and the control channel; an extended system information message the system and
  LCN and the control channel; an extended site ID the site and the area.
- TETRA: a CRC-passed BSCH the network and colour code (`tetra_bsch_parse()`), and so does a trunked mode block,
  downlink or uplink (a NUB's MAC block, a control uplink burst's SCH/HU), whose CRC passed once descrambled with the
  network's extended colour code (`tetra_note_proven_cell()` in `tetra.c`, before the MAC acts on it): every carrier of
  a cell, uplink included, scrambles with its MCC, MNC and colour code, so such a block proves all three on this
  carrier, and an uplink carrier, which has no BSCH, names its cell this way only. A followed traffic carrier, whose
  colour code the trunk tune dropped, names it again with its next checked block. A SYSINFO notes the location area,
  and the control channel only when the tuner is on the main carrier it names; a checked direct mode burst
  (`tetra_dmo_apply_sync()`) drops a TETRA record. A `--tetra-net` preset notes nothing by itself: it becomes received
  identity only once a block descrambled with it passed its CRC on this carrier (one that never descrambles anything
  stays configuration).

Tests: `P25_SEARCH_SITE_NOTES`, `P25_P1_SYNC_DISPATCH`, `P25_P2_ESS_CARRIER`, `DMR_SEARCH_SITE_NOTES`,
`DMR_CONFIDENCE_GATE`, `DMR_DATA_SYNC`, `NXDN_SEARCH_SITE_NOTES`, `NXDN_ELEMENT_BOUNDS`, `DPMR_VOICE_BRIDGE`,
`M17_STATE_DISPATCH`, `EDACS_GRANT_TUNE_MATRIX`, `TETRA_MAC_STATE`, `TETRA_PUBLICATION`, `TETRA_BURST_VERDICT`.

Key public headers (selection):

- DMR: `<dsd-neo/protocol/dmr/dmr_utils_api.h>`, `<dsd-neo/protocol/dmr/dmr_trunk_sm.h>`
- P25: `<dsd-neo/protocol/p25/p25p1_const.h>`, `<dsd-neo/protocol/p25/p25_trunk_sm.h>`,
  `<dsd-neo/protocol/p25/p25_sm_watchdog.h>`
- NXDN: `<dsd-neo/protocol/nxdn/nxdn_const.h>`, `<dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>`
  (the movable missing-channel ledger and its exit summary, so trunk scan can park one per target)
- D‑STAR: `<dsd-neo/protocol/dstar/dstar_const.h>`, `<dsd-neo/protocol/dstar/dstar_header.h>`
- ProVoice/EDACS: `<dsd-neo/protocol/provoice/provoice_const.h>`

Private per-protocol modules worth knowing about:

- RAS burst signature (`dmr_dburst.c`, `dmr_ras_signature()` in `src/protocol/dmr/dmr_crc_provenance.h`). Motorola
  RAS computes a burst's CRC (RS(12,9) parity for a voice LC) over the payload and a secret, and sets the BPTC
  reserved bits R(2..0), which ETSI fixes at zero. A BPTC burst whose extractor R is nonzero and whose standard
  check failed is a candidate: only then does the handler run `BPTC_196x96_Decode_Codeword()`, and when that reaches a
  codeword within `DSD_BPTC_196x96_UNIQUE_RADIUS` (4, the guaranteed radius of the minimum-distance-9 product code,
  so the unique nearest codeword) the codeword's payload, R and check replace the extractor's and are dispatched.
  The signature is that codeword's R nonzero and check failed, on an eligible type (PI with FID 0x10 only, voice LC
  header and terminator, CSBK, MBC header, data header, confirmed rate-1/2 block, USBD; never the MBC continuation,
  rate 3/4, rate 1 or idle) and not Hytera (byte 1 0x68 where it is a FID or MFID: CSBK, MBC header, voice LC
  header and terminator, proprietary DPF 15 data header; never USBD or an ordinary data header). The context's
  `ras_signature` is computed with or without `-F`; `is_ras` (accept, `crc_correct = 1`, `-RAS`,
  `DSD_CRC_RAS_UNVERIFIED`, `DMR_DATA_BLOCK_CRC_RAS`) is a signature under `-F`. A voice LC with R nonzero only gets
  the syndrome check `CheckFullLinkControlCrc()` and is never RS-corrected; R == 0 bursts keep the extractor and
  ordinary RS paths and pay nothing. The adopted codeword's check decides even when it is not a signature: one that
  passes is an ordinary checked burst (dispatched, unmarked, may prove TDMA, with or without `-F`), and one whose R is
  0 is handled as an R == 0 burst (a voice LC goes through ordinary RS(12,9) correction; a failed check is a plain
  failure, never RAS). Decided limit: the decoder is iterative, correct-or-reject; a RAS burst with
  more than 4 errors, or one of the few-error patterns it carries beyond 4, is a failure. Tests: `DMR_RAS_SIGNATURE`,
  `DMR_DBURST_PROFILE`, `DMR_EVENT_CRC`, `DECODE_IQ_DMR_T3_RAS_CC`.
- Voice crypto continuity (`dmr_vcrypt.c`, issue #729). Every DMR voice cipher keys a frame by its superframe's IV/MI
  and its position 0..17. Per slot, `dmr_vc_ref_*` holds the symbol-clock position of the voice sync of the superframe
  whose IV is current: a checked PI header or voice-header Hytera LC makes it FIRST (the next voice sync within four
  burst pairs is superframe 0); every VC1 then counts superframes since it, signed and rounded to the nearest, and
  advances the cipher's MI that many times (AES `dmr_aes_expand_iv`, DES `dmr_des_expand_iv`, RC4
  `dmr_mi_advance32`, Kirisun `kirisun_lfsr`, Hytera Enhanced its LFSR), idempotent across repeated re-entry; a
  processed VC6 moves it one superframe on. A different epoch or carrier, a gap over 10000 superframes, or a FIRST too
  far before its voice sync loses the chain, and with it the identifiers a PI checked: encrypted
  voice on that slot is muted (`FRAME EVENT ... type=MI_UNKNOWN`), whatever ALG ID the voice sync read (a
  `--dmr-force-algid` installs one only as frames decode), until a PI, a complete late-entry MI or a Hytera LC re-keys
  it. A PI trusts the slot at once; a late-entry MI or Hytera LC names the next superframe's MI, so the slot is trusted
  from the next voice sync and the frames still to decode in this one stay muted. The chain ends at a call boundary (a voice header
  that opens a transmission, unless a PI already prepared it; the terminator's crypto reset) and comes back with the
  missed-terminator heal, and also at an LC naming another call that replaced the active one unannounced (an epoch
  fork) and at a carrier boundary, which clears the checked identifiers with the ALG ID and key id. A PI of the call
  now starting is kept: a FIRST whose voice sync is still due on this timeline. Once that PI's voice runs, an epoch
  fork cannot tell it from the replaced call's, so the chain goes and late entry re-keys the slot (decided limit). Only
  a PI whose CRC passed (or a Hytera PI whose checksum did) trusts the slot: one `-F` accepted as RAS sets its fields
  but checks nothing, so it loses the chain and any earlier PI's trust, as do a PI naming no cipher the decoder knows
  and an SB identifier naming another cipher or key. A forgotten or lost chain mutes the slot's MI-keyed frames at once (the VC6 an LC was read ahead of), not
  from the next voice sync. A failed trunk-scan switch
  that rolls back to the original carrier does not restore the chain (decided limit): the switch already cleared the
  carrier's ALG ID, key id and confirmation evidence, and the slot re-keys as the original carrier proves itself
  again. Hytera
  Enhanced's BS reader steps its MI before VC6 decodes; the superframe's MI is kept aside (`dmr_vc6_seed`) until VC6
  is done, so a keystream generated again for VC6 uses it. The voice readers (`dmr_bs.c` loop and bootstrap, `dmr_ms.c`) publish each frame's position
  (`dmr_voice_ks_pos`, stored one up so a zeroed state is unpositioned) and the vocoder reads every positional
  keystream from it (AES/DES/Kirisun 56-bit steps, Hytera Enhanced 49, RC4 drop 256 + 7/frame, Hytera BP frame index,
  Static/Vertex frame or 49-bit steps), regenerating when the cipher or key fingerprint changes (a one-way AES-256 Matyas-Meyer-Oseas tag: the scalar key
  starts the chain and the AES words, then the K words, each key one block, so material loaded twice cannot cancel;
  taken once a burst and never for a clear slot; the key-bearing scratch is erased). The old error-time
  refresh (one guessed step, RC4/Hytera/Kirisun only) is gone. Late entry decodes only a complete VC1..VC6 fragment
  set of one superframe; against a trusted chain a disagreeing MI waits for the next superframe's to confirm it (its
  successor under the cipher's own step: DES, AES, RC4, Kirisun); a held MI expires after that one superframe and is
  dropped when a PI re-keys the slot or superframes go by unseen. An SB
  identifier (FEC only) never replaces the ALG ID and key id a checked PI gave the current call. Tests:
  `DMR_VCRYPT_CONTINUITY`, `DMR_LE_INFER_ALGID`, `DMR_BS_ALG_REFRESH_ERROR`, `CORE_MBE_CRYPTO_SCOPE`, `DMR_SYNTH_*`.
- Slot and polarity evidence in the BS loop (`dmr_bs.c`, `dmr_data.c`, `dmr_ms.c`, issue #729). Bursts alternate
  slots, so the slot follows alternation from the bootstrap's burst, not the Hamming(7,4) TACT bit (a perfect code
  that miscorrects silently); until two TACTs agree with it, two in a row against it re-anchor it (a wrong start costs
  one burst), and once confirmed no TACT moves it.
  A BS voice sync is an inverted data sync: voice syncs on four adjacent bursts (one burst apart on the symbol clock,
  the bootstrap's included; a lost burst breaks the run) with no voice gate open (data at the wrong polarity), two failed data bursts whose first AMBE frame passes FEC at the other polarity (a
  call at the wrong polarity), or two MS voice syncs after burst A each take the other polarity for the session
  (`opts->inverted_dmr`, as `-xr` and the menu toggle set it). On a slot whose voice gate is open, one EMB naming
  another colour code counts as an EMB miss (the burst's voice still decodes) rather than ending the loop: it is far
  likelier a QR(16,7,6) miscorrection; a second miss in a row ends it as before, and only an EMB the colour-code check
  accepted ends a miss streak. Tests: `DMR_SYNTH_SLOT_FLIP`,
  `DMR_SYNTH_INVERTED_*`, `DMR_BS_SYNC_TIMES`, `DECODE_IQ_DMR_T3_CC`, `DECODE_IQ_DMR_POLARITY_AUTO`.
- Capacity Plus identity (`dmr_flco.c`, `dmr_csbk.c`, `dmr_trunk_sm.c`). Cap+ talkgroups are 8-bit (1..255) and
  radio IDs 16-bit. The FID 0x10 FLCO 0x04 group voice LC carries the talkgroup at bits 40..47, a 5-bit rest LSN at
  51..55 and the radio at 56..71 in every LC type (header, embedded, terminator); bits 24..39 are not part of the
  talkgroup. FLCO 0x07, the private call header, has no published layout and keeps its standard 24-bit target, with
  the 16-bit source and 4-bit rest LSN read from the header only. A rest LSN outside 1..16 announces none, and an LC
  announces one only when its own check passed (`CRCCorrect`: the RS check of a header or terminator, the 5-bit
  checksum of an embedded LC); clean FEC alone does not. Under `-F` a RAS-accepted LC counts as checked, as a
  RAS-accepted 0x3E status does. The LC's vendor ID (`dmr_mfid`, shown as the branding), the Hytera XPT branding and
  an XPT alert's free channel take the same gate. The 0x3E
  channel status reads its rest LSN as 5 bits (19..23) and decodes each active LSN's ID in the parser, one byte per
  active group LSN and one 16-bit word per active private LSN, so which LSNs the display window shows never moves an
  offset. Cap+ follows a call already in progress on that status alone, outside the DMR SM grant path, so the voice
  LC header has usually gone by. When it tunes, `dmr_cspdu_cap_plus_3e_seed_carrier()` hands the SM the group each
  slot of the followed carrier is carrying (`dmr_sm_capplus_seed_arm()`, with the fragments' accumulated CRC flags in
  `cap_plus_csbk_crc_invalid[]`). At voice sync the seed names the slot's call only when that call is the vocoder's
  identity-less epoch, opened after the status (on a carrier the status follow adopted in place, the receiver already
  being there, any identity-less call on the air: none is from another carrier, `dmr_sm_capplus_seed_since()`). It
  never opens, forks or reopens an epoch, so an LC always wins. It
  is used once, and dropped by a terminator on the slot, by a later status showing the LSN idle or on another group,
  by a later tune (the tuning generation, read before the follow tune: the seed belongs to the next completion), by
  a receiver frequency other than the one the follow tune left (an operator retune moves no generation), after
  `DMR_CAPPLUS_SEED_WINDOW_S`, and under single-slot decoding. Group calls only: a bank-two private ID's layout is
  inferred from bank one's, not published. The seed also names the slot's identity record (`dmr_sm_slot_ctx_t`,
  provenance SEED, below) unless a checked LC already named the call there; a later status naming another group on
  the slot's LSN replaces a seed's identity (`dmr_sm_status_identity_reconcile()`), never an LC's, and an idle LSN
  leaves it. The slot's LSN is the followed carrier's (LSN n is slot (n - 1) & 1 of the channel-map frequency,
  `dmr_cspdu_cap_plus_slot_lsn()`), not the record's, so a slot whose identity expired with its call while the
  companion kept the carrier busy (no record left) takes the group a status names for its next call. Tests:
  `DMR_FLCO_PRIVACY_MODES`, `DMR_GRANT_POLICY`.
- DMR talkgroup priority and preemption (`dmr_trunk_sm.c`, `dmr_csbk.c`, `dmr_csbk_parse.c`, `dmr_flco.c`). Policy
  stays at the CSBK/status layer, which alone knows each grant's classification (Tier III data grants rewritten to
  group, Connect Plus private refused with group tuning off, Cap+ private LSNs, XPT kinds); its decision, call kind,
  identity and origin travel on `dmr_sm_event_t` to `dmr_sm_submit_grant()` (FOLLOWED / IN_PLACE / REFUSED / BUSY /
  DEFERRED), which never re-evaluates a grant. The `dmr_sm_emit_*_grant*` wrappers carry no decision and are never a
  Hold or preemption candidate.
  - Busy view (`dmr_sm_describe_held_calls()`). TUNED: the tick's own retention rule, factored out of `tick_tuned()`
    without changing the tick (`dmr_sm_tuned_retention()`): live voice on an admitted slot (0.2 s stale) whatever the
    hangtime, the hangtime tail (a tail no admitted slot is in holds nothing), acquisition before the first voice (the
    grant timeout; a carrier a channel status selected, Cap+ or XPT, keeps `DMR_CAPPLUS_SEED_WINDOW_S`), and a receive
    check still pending on a carrier no slot of which the policy admits yet (not once its verified code is rejected).
    Before the first voice the held slots are the grant's slot; for a slotless status follow (Cap+), each slot the
    status selected (`dmr_sm_note_status_follow()`) or seeded (`dmr_sm_capplus_seed_arm()`), tracked apart from identity
    knowledge (`vc_acquire_slots`), so a private LSN or a seed a later status left unknown is a held call of unknown
    identity, and each slot named on the carrier, each only on a slot the receive slot filter admits
    (`dsd_receive_grant_allows()`, the slot alone: a seed on a rejected slot holds nothing, while a slot whose colour
    code is still being checked is held), and an acquisition whose every slot it rejects (a receive edit after the
    follow rejected the grant's own slot) is not held at all, so the grant's identity never stands for the carrier and
    an ordinary grant elsewhere is followed; once there is voice, each slot the receive policy admits whose voice
    is live or in its hangtime tail (a slot a receive edit rejected holds nothing, its tail included). ON_CC: calls
    heard on the carrier the receiver listens on (composite TSCC payload, XPT parking, late entry) whose identity is
    known there and admitted by the policy, on a slot the receive policy admits (`dsd_receive_allows()`, as for the
    receive hold: rejected media never holds the receiver); an unknown, blocked or receive-rejected resident call never
    makes it busy. Held slots are described into the policy's followed-call ledger
    (`dsd_tg_policy_describe_followed_call()`, start = the slot's follow time) and every other ledger slot is cleared; a
    held carrier whose slots name nothing yet is described by the grant's identity (slot -1).
  - Gate. A candidate on another carrier while held (or while the caller's own legacy window says busy: `caller_busy`)
    is followed only by Hold over a non-held call (`tg_hold_match` and no held slot on the Hold group) or by preemption
    (`dmr_sm_may_displace()`: a decision the caller evaluated for a known identity, no held call of unknown identity,
    and `dsd_tg_policy_should_preempt()` over every held slot); otherwise `grant-busy`. A candidate on the held carrier
    is followed as before (IN_PLACE), and names its slot's call (`dmr_sm_grant_reconcile_in_place()`): another call
    there takes the slot's record with its follow time now (on the followed slot, the grant identity too, with this
    announcement's CRC provenance, `vc_crc_flags`, which the first voice publishes it with, never the replaced
    grant's: a RAS-accepted reassignment under `-F` reads unverified, a verified one verified); a repeat of
    the call the slot names, or of the grant followed there, keeps record and follow time. A status's in-place entry
    (origin STATUS) replaces only a seed or grant identity, never a checked LC's on that carrier.
  - Slot identities (`dmr_sm_slot_ctx_t`: target, source, kind, provenance SEED/GRANT/LC, policy class, channel,
    carrier, follow time, and `crc_flags`, the DSD_CRC_* bits of what named it: the grant's or status's burst, a Cap+
    status's every fragment, a seed's, the LC's scope) come from the followed grant (a slotless grant's at its first
    accepted voice), an in-place grant, Cap+ seeds and status reconciliation (a seeded LSN the status shows carrying a
    private or data call keeps the slot's activity with an unknown identity, and a slot with no identity takes that
    unknown record on the followed
    carrier, so a later group status names it; an idle one changes nothing), XPT status reconciliation
    (`dmr_sm_status_slot_reconcile()`, from each site status before its busy view and ranking: on the followed carrier
    a group entry names its slot's call (on the followed slot the grant identity too, with the status's own CRC
    provenance, `dmr_sm_vc_identity_group()`), a private one leaves it unknown and, on the followed slot, withdraws
    the grant identity's fallback and publication; an LC's identity, idle and unknown entries are left alone), and a
    voice LC only at its checked
    publication (`dmr_sm_note_lc_identity()` from `dmr_flco_publish_voice()`). The follow time moves only when the call
    (policy class, target, carrier) changes, so a candidate's dwell runs from the conversation's first follow. They live
    in each trunk-scan target's `dmr_sm_ctx_t` and are dropped by a release, an abandoned carrier, an operator move and
    the acquisition a target switch starts.
  - Call binding. Each record is bound to the canonical call it names (`call_epoch`: that slot's
    `dsd_call_snapshot_transmission()`), so it never outlives it (`dmr_sm_slot_identity_refresh()`, run by the busy
    view, grant submission, the status reconciliations, voice sync, an LC and the tick). An LC binds to the call it was
    published on; a grant, seed or status to the slot's active call when that names its target or nothing yet, and
    never one of the other class (`dmr_sm_call_names_other()`: a talkgroup and a unit may share a number) (on the
    followed carrier only one begun since the receiver arrived there, `t_carrier_m`: the follow tune, or for a carrier
    adopted in place any call), else to the first call to begin on the slot after it was named; the same call named
    again while its call is faded but may still be reacquired (`dmr_sm_slot_call_may_heal()`:
    `dsd_call_snapshot_may_reacquire()`, its end reason's own window) keeps that binding, so the reacquired call, which keeps its transmission, stays the
    record's and the next call never inherits it. The call's end leaves
    the record only on the followed carrier within the hangtime (a conversation is held in its gaps); a resident call's
    identity ends with it. Another call on the slot clears the record, and on the followed carrier one that began
    within the hangtime of the named call leaves a gap record (unknown) whose renaming as the same call (target, class,
    carrier, binding that call) keeps the follow time; on the followed slot the grant's identity then stands for
    nothing (no fallback, no publication). A new call never inherits an identity, and LC precedence over seeds, grants
    and statuses holds only while the LC's call lasts (a Cap+ status may name a gap record's LSN, or a slot whose
    identity expired).
  - Leaving a carrier. An accepted follow that leaves the carrier the receiver was on (a grant, Hold or preemption
    takeover, a matched C_MOVE, Connect Plus, XPT, or a Cap+ status follow, `dmr_sm_note_status_follow()`; the
    receiver's frequency read before the tune, `dmr_sm_receiver_frequency()`: the followed carrier, else the one it
    listens on; unknown counts as leaving) ends every slot's canonical call (EXPLICIT, synchronized,
    `dmr_sm_end_left_calls()`, as a release without a control channel does), so a displaced call never stays active
    beside the followed one. A follow of the carrier the receiver is already on (a grant for the composite control
    channel's own payload, a Cap+ status naming the rest channel it parks on, an XPT entry where it parks) adopts it in
    place: no call there ends, the slots keep that carrier's records (identity, binding, follow time) and voice timers,
    the grant names its slot only when the slot's record does not already name its call, its identity is not
    published into a call on the air that already names one, and no block state is reset.
  - Ledger. A follow clears the ledger and notes the followed call (`dmr_sm_policy_note_follow()`); a preemption
    commits its cooldown (`dsd_tg_policy_commit_preemption()`) once the new carrier's tune was accepted, a Hold
    takeover commits none, and a deferred or failed tune changes nothing; every release clears the ledger
    (`dmr_sm_clear_released_carrier()`). Writers run under the P25 SM tick guard (frame dispatch, the DMR tick, the
    carrier boundary, the trunk-scan switch); an operator move from a command takes it unless held
    (`dmr_sm_note_operator_move()` in `app_command_queue.c`).
  - C_MOVE. A move of the followed call (its talkgroup or either party of its private call, as the followed slot's
    identity names it; the grant's only while that slot names none, so a talkgroup a checked LC renamed is another
    call's), whatever slot it names, is judged with that call's own classification: refused only when tuning is denied
    for a reason other than a Hold mismatch (media-only restrictions never stop following it); same carrier -> IN_PLACE
    on the new slot: the call's canonical epoch on the vacated slot ends (EXPLICIT, synchronized) and its first voice on
    the new slot publishes it there again, so one call never shows ACTIVE on both slots; another carrier -> a new
    acquisition that ends the calls of the carrier left (as every follow does) before naming the moved call, so its
    first voice there publishes it in an epoch of its own, which the moved identity binds to (it never waits behind
    the old carrier's epoch to bind the next, anonymous call); both keep the follow time. Both republish the moved
    call's identity as the followed slot's record names it (with the record's `crc_flags` OR'ed with the move burst's:
    the move confirms the target, not a source a RAS-accepted grant or LC under `-F` named), which makes the grant's
    identity known again even when the end of the call it first named had withdrawn it (a checked LC then named the
    moved call). A deferred or failed move tune changes nothing, the followed grant's identity included (it is rewritten
    from the slot's record only in place
    or once the tune is accepted), so a repeat of the original grant still matches it. A move of any other call is an
    ordinary grant under its group decision and the gate. Its burst indicators and companion-row clear apply only once
    accepted (as before when it is not submitted: trunking off or not on a voice channel).
  - Status selection (Cap+ 0x3E, XPT site status). One tune per status: while held (busy view or the status's own 2 s
    voice window, which an allow-list or Hold no longer zeroes) an entry on another carrier is eligible only by Hold or
    preemption and one on the held carrier never displaces; among the eligible Hold, then priority, then channel order
    decides (with no priorities, channel order as before). Inside the voice window an entry on the carrier the receiver
    holds or is parked on is not submitted (Cap+ follows it only outside the window, when nothing else is eligible), so
    a resident call is never retuned, block-reset or cleared from the control channel. An XPT entry is submitted with
    its LSN as the grant's channel, the route its ranking judged. XPT entries keep their status kind: 3 (group, 8-bit
    ID) is a group candidate; 2 (private, the value a hash or placeholder) only with private tuning on, as an unknown
    identity whose decision is built without any endpoint lookup (allow-list, Hold and private-off block it; priority 0,
    no preemption) and published by the voice LC only; 1 and 0 are not followed; private needs no group tuning.
  - Connect Plus evaluates policy before its 2/4 s window: an ordinary grant inside it is refused as before, a Hold or
    preempt grant goes to the gate with the window as `caller_busy`, and `is_con_plus` is set only once taken.
  - Decided limits: a candidate preempts only on a repeat after the dwell of every held slot and outside the cooldown;
    a held call of unknown identity (a Cap+ private LSN, an XPT private call, late entry before the LC) yields only to
    Hold; a Connect Plus data channel the CSBK path tuned itself has nothing to describe, so only Hold leaves it inside
    its window; the ranking covers one status message (one XPT bank). Tests: `DMR_PREEMPT`, `DMR_GRANT_POLICY`,
    `DMR_T3_CMOVE`, `DMR_T3_MOVE_RELEASE`, `DMR_CSBK_HELPERS`, `DMR_FLCO_PRIVACY_MODES`, `APP_COMMAND_QUEUE`.
- P25 preemption (`p25_trunk_sm.c`, `p25p2_vpdu.c`, `p25_lcw.c`). While the SM holds a voice channel, grants and
  updates for another carrier that the voice channel carries (Phase 2 FACCH/SACCH MAC grants, Phase 1 LC `0x42`,
  `0x44` with `-j` off, Motorola LCGRGU) reach it with `p25_sm_event_t.preempt_only`. `handle_grant` drops such an
  event unless the SM is tuned, the frequency differs, and the read-only prospective decision
  (`p25_grant_prospective_decision()`, also behind `p25_sm_grant_candidate_rank()`) is a tunable preempt talkgroup, so
  ordinary announcements never touch SM timers. Calls announced together, on Phase 2 and in LC `0x42`, are ranked
  (audible before probe as `p25_sm_grant_is_silent_probe()` classifies them, admitted, hold, priority, listed order)
  and the first carrier change ends the update. Before `should_preempt` the SM
  describes every assigned slot with `followed_since_m` (kept across repeats and re-grants of the same call; reset by
  `p25_sm_clear_followed_history()`; restarted when a TDMA slot's conversation lapsed, its last stop a whole hangtime
  old with no voice awaited since). A TDMA slot counts as held while its voice is live, within the hangtime after it,
  or while its voice is awaited (`p25_slot_awaiting_voice()`) for at most the grant timeout from `acquire_since_m`.
  The wait starts at the first grant after a new follow or a voice start or stop (`acquire_ended`, a flag because a
  grant, its voice and its end can share one clock reading); repeats of the grant, as updates or as assignments
  replaced through a slot clear, do not renew it. A data assignment counts from its last grant instead. Duplicates,
  voice starts and accepted grants describe; only a preemption commits a cooldown, after the candidate is tuned. A
  preemption whose CC return fails or defers restores the force-release latch and keeps the held call. A clear-bit
  encryption-lockout release (`p25_grant_commit_enc_release()`) and the stale-regrant guard's probe are spent only on
  acceptance. Tests: `P25_PREEMPT`, `P25_GRANT_POLICY`, `P25_P2_VPDU_GRANTS`.
- P25 followed-call identity across a fade (`p25_trunk_sm.c`, `p25_voice_canonical_lacks_identity()`). The engine's
  no-carrier pass ends the canonical calls as a sync loss and resets the P25 crypto without telling the P25 SM, which
  keeps following the assignment. Past the reacquire window a Phase 1 call comes back through the clear ESS with the
  assignment's target only (`p25_crypto_ensure_phase1_call()`), or stays ended until it, and the LCs after it
  re-describe what the SM already holds. So a decoded identity-bearing voice event (LCW/MAC) that decoded its own
  subscriber source, on the call the SM follows, republishes when the canonical slot lacks it: a sync-loss end, an
  identity-less P25 epoch, or an epoch of the same target without a source. The publish is a CONTINUE, so an ACTIVE
  epoch is filled in place, and it reclassifies nothing (not even under a regroup KEY=0 clear override): the ESS
  that reopened the call owns its crypto. A source inherited from the assignment never triggers it (it may be the talker before the
  fade), and terminator, unverified-terminator and explicit ends are not reopened by it; the existing
  new-transmission handling (the identity-pending wait, Phase 2 post-END retention) is unchanged. Phase 2 needs none
  of it: the MAC_ACTIVE's own observation reopens and classifies the call before the SM sees it. Test:
  `P25_VOICE_RESUME_IDENTITY`.
- Documented limit (P25 Phase 2 fast release and late aliases, `p25_trunk_sm.c` `handle_facch_voice_end()`): while
  trunking, two FACCH ENDs for the same call within 1 s, with no newer grant and both slots idle, release the voice
  channel at once (`facch-double-end`) instead of waiting out hangtime. A Motorola talker alias sent in hangtime after
  that release is never received, so that call keeps whatever name its row had. The fast release is kept (maintainer
  decision): it returns the tuner to the control channel early, and an alias sent before the second END, or on a
  channel the fast release does not close, still names the call (see the Motorola alias invariant under Core).
- `src/protocol/dmr/dmr_confidence.{c,h}` — colour-code and voice-burst confidence, so a burst has to be corroborated
  before DMR decodes or unmutes it. Every evidence point also stamps `dsd_state::dmr_confidence_lock_m` with the
  decode time (never cleared, not even by `dmr_confidence_reset()`), which a startup control-channel hunt reads as
  validated content (issue #703). A relock onto another colour code resets the RAS evidence (`dmr_ras_reset()`).
- `src/protocol/dmr/dmr_ras.c` — RAS on the carrier (`<dsd-neo/protocol/dmr/dmr.h>`). The burst handler notes every
  burst with the RAS signature, with or without `-F`, before it dispatches (`dmr_ras_note_signature()`; a retune its
  handler makes then resets the carrier left's evidence); a frame read across a carrier boundary or cut by an input
  interruption counts for nothing. Two signature bursts on the carrier (`dsd_state::dmr_ras_signatures`, saturating)
  confirm RAS (`dmr_ras_confirmed()`) and note `DSD_RXID_DMR_RAS`; the burst that confirms it prints, while relaxed CRC
  is off (`dsd_opts_relaxed_crc()`), the console notice `DMR RAS (Restricted Access to System) detected: control and
  data fail the standard CRC; -F decodes them` once per confirmation, with `, but this channel sets strict CRC itself`
  added while `-F` is configured on (`dsd_scan_mode_configured_relaxed_crc()`) under a row's own `--strict-crc`.
  `dmr_ras_reset()` is the one reset; its call points are listed under the received identity record (Core). Tests:
  `DMR_RAS_DETECT`, `DECODE_IQ_DMR_T3_RAS_CC_DETECT` (no `-F`: the notice, nothing accepted), and `DECODE_IQ_DMR_T3_CC`
  and `DECODE_IQ_DMR_POLARITY_AUTO`, whose carriers must never print it.
- `src/protocol/dmr/dmr_enc_class.c` — per-slot privacy-bit hysteresis, and each slot's vendor late-entry mode
  (`dsd_state::dmr_le_vendor[]`, `DMR_LE_VENDOR_*` in `<dsd-neo/protocol/dmr/dmr.h>`, issue #657). A CRC-verified
  Kirisun PI or LC, or a checksum-verified Hytera Enhanced PI or LC, sets the slot's mode for its call; it is cleared
  with the slot's crypto at the terminator (stashed and restored by the unverified-terminator heal), by a verified DMRA
  PI or a data header, at the carrier boundary, on a retune and in the no-carrier reset. A DMRA PI supersedes vendor
  evidence even when a missed terminator or voice header hid the call boundary. Trunk-scan snapshots carry the mode
  for failed-switch rollback, but each new target visit clears it instead of restoring a previous call's mode.
  `dmr_flco_publish_voice()` also clears it when a new transmission replaces the slot's active call: a voice LC header
  after that call carried voice (the BEGIN `dmr_flco_voice_boundary()` returns), which may specialize the identity-less
  epoch a Kirisun or Hytera Enhanced call runs on (their own LCs publish no identity) rather than open a new one, or an
  LC that forks the epoch. Header repeats, the same call's embedded LCs and a reacquired epoch keep it, and so does
  evidence decoded before any call was active. `dmr_le.c` combines it with the user's `-3` setting (`opts->dmr_le`, 0 or
  1, never written by decoding): Kirisun's SB identifier is read for an encrypted Kirisun call whatever `-3` says;
  standard SB/RC parsing needs the user's setting on and no vendor mode on the slot; `dmr_late_entry_mi_fragment()`
  skips a Hytera Enhanced call's slot.
- `src/protocol/nxdn/nxdn_confirm.{c,h}` — its NXDN counterpart, and for the same reason: the 10-symbol sync word and
  one-parity-bit LICH ahead of it are weak enough that receiver noise clears both. Channel decoders report their CRC
  verdicts to it, and `nxdn_frame.c` consults it before refreshing the scan hold or synthesizing voice, as
  `nxdn_deperm.c`/`nxdn_element.c` do before publishing a RAN or a call. The engine clears it with the carrier through
  `nxdn_confirm_reset()`, the one entry point exported in `<dsd-neo/protocol/nxdn/nxdn.h>`.
  `NXDN_Elements_Content_decode()` carries no CRC verdict of its own: the channel decoders in `nxdn_deperm.c` and
  `NXDN_SACCH_Full_decode()` hand it CRC-verified content only, so the gate lives in those callers, with
  `nxdn_confirm_is_confirmed()` as the frame-level check at the sites that publish a call or refresh a scan hold.
  Evidence confirms at once, before the RAN and call it unlocks are published; a signal search hears a frame's
  confirmation once, from `nxdn_confirm_end_frame()`, after they are (`NXDN_CONFIRM`, `NXDN_DEPERM_PRIMITIVES`), which
  also stamps `dsd_state::nxdn_confirm_lock_m` for the NXDN control-channel hunt.
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
  16384 and confirms outright. Evidence confirms before the frame publishes its identity, service and colour code; a
  signal search hears the frame's confirmation from `dpmr_confirm_end_frame()`, after the publication (`DPMR_CONFIRM`,
  `DPMR_VOICE_BRIDGE`). `processdPMRvoice()` returns how many halves passed, `dsd_dispatch_handle_dpmr()`
  maps that to `DSD_FRAME_VERDICT_PROFILE_PROVEN` (for two seconds after the last decode) or `UNPRODUCTIVE`, and the
  engine clears both through the exported `dpmr_confirm_reset()` in `<dsd-neo/protocol/dpmr/dpmr.h>`.
  `tests/protocol/dpmr/fixtures` holds the CCH reference vectors that prove the pipeline decodes correct dPMR, which
  the off-air `dpmr` capture never established. The same CRC decides scrambling (issue #655):
  `dpmr_resolve_half_scrambling()` reads a half's Version only when its CRC-7 passed, keeps it as the transmission's
  `dsd_state::dpmr_scrambled`, and gives a failed half that value (or, with none yet, this frame's verified half);
  `dPMRVoiceFS2Frame.Scrambled[]` holds the result, which the bridge (per half: NXDN scrambler and `nxdn_cipher_type`
  1, or clear), the call's crypto classification and the terminal read; `Version[]` stays the raw field.
  `dpmr_confirm_reset()`, initState() and the FS1/FS3/FS4 boundaries (`dpmr_scrambling_reset()`) forget it. A boundary
  leaves confirmation standing, so a half no verified half speaks for since it (`DPMR_HALF_UNVERIFIED`) plays nothing
  and leaves the call's classification alone, rather than playing a scrambled call's first undecodable halves as
  clear.
- Signal search site notes (`dsd_signal_search_note_site()` in `<dsd-neo/runtime/signal_search_hooks.h>`): each writer
  passes the identity of one message that passed its check, after handling it, never decoder state; a SYSID of 0 names
  no system. P25 (`src/protocol/p25/p25_search_site.h`): a TSBK network status (`tsbk_handle_network_status()`,
  dispatched only after the block's CRC-16, and for opcode 0x3B only under MFID $00/$01: as for every standard opcode, a
  manufacturer's 0x3B is that manufacturer's message and changes no control channel or system), an MBT network or RFSS
  status (`p25_decode_pdu_trunking()`, reached only after the header CRC-16 and CRC-32), and the vPDU network status
  (0x7B/0xFB) and RFSS status (0x7A/0xFA) blocks only when `p25p2_vpdu_ctx::crc_ok` holds, which covers a bridged TSBK
  and a Phase 2 MAC but not a MAC_SIGNAL let through with a failed CRC under `-F` (which the decoder does not apply
  either). An RFSS status notes the SYSID it advertises, even one the decoder's `p2_sysid` check ignores. Not notes:
  the voice LCW network/RFSS formats (Reed-Solomon only), the secondary control channel fill-in
  (`p25_note_sccb_site()`), adjacent-status broadcasts, CLI/menu hard-set values and trunk-scan snapshot restores
  (`P25_SEARCH_SITE_NOTES`, `P25_P1_TSBK_SEQUENCE`). DMR
  (`dmr_tiii_note_search_site()` in `src/protocol/dmr/dmr_tiii_site.h`, which notes the search only while one runs and
  the received identity record always):
  C_ALOHA inside `dmr_cspdu()`'s CRC gate when the slot's `event_crc_invalid` is clear, since that gate also passes a
  burst `-F` accepts as RAS (`DSD_CRC_RAS_UNVERIFIED`) and an MBC whose header CRC failed (`DSD_CRC_FAILED`), and the
  C_SYS_PARMS / P_SYS_PARMS short LC that `dmr_cach()` assembles behind Hamming(17,12,3) and CRC-8 (no RAS path); a
  C_BCAST vote/adjacent syscode names another site (`DMR_SEARCH_SITE_NOTES`). NXDN: the current-site location ID in
  `nxdn_location_id_handler()` (SITE_INFO, SRV_INFO, CCH_INFO, all CRC-gated by their channel decoders) once
  `nxdn_confirm_is_confirmed()`, so a single short SACCH CRC does not show a site; adjacent sites, the reserved
  category and the Type-D site identity (`nxdn_idas_*`) are not notes (`NXDN_SEARCH_SITE_NOTES`).
- Signal search call notes (`dsd_signal_search_note_call()` in `<dsd-neo/runtime/signal_search_hooks.h>`): each
  writer passes the slot of a canonical call record it just published after its check, and the engine reads that
  record (see Signal search under Engine). DMR: `dmr_flco_publish_voice()` (a CRC-checked voice LC, after the call and
  its crypto are published) and `dmr_pi_publish_crypto()` (a checked PI header's crypto-only update of the slot's
  call; with no call active the PI is held per slot in `dsd_state::dmr_pi_precall_*` for the call the next checked LC
  opens, and dropped at that slot's terminator, `dmr_pi_precall_reset()` in the no-carrier pass, a carrier boundary, a
  retune and a trunk-scan visit). P25: the trunk state machine's canonical publish (`p25_call_publish_observation()`,
  `p25_sm_publish_conventional_voice()`), the LCW source update, the Phase 2 vPDU voice observation and every
  `p25_crypto_resolve()` of a checked tuple (HDU, LDU2, ESS, MAC_PTT after its identity). NXDN, dPMR, M17, YSF and
  D-STAR: beside each search evidence report of their confirm points, after the frame published what its
  confirmation unlocked (`NXDN_CONFIRM`, `DPMR_CONFIRM`, `M17_STATE_DISPATCH`, `YSF_DCH_DECODE`,
  `DSTAR_HEADER_UTILS`, `P25_P2_ACTIVE_PTT_EPOCH`, `DMR_SEARCH_CALL_NOTES`). Evidence IDs: the runtime
  `dsd_trunk_scan_hook_*_conventional_activity()` wrappers report every confirmation to the search but pass its target
  and source only while the reporting slot's `event_crc_invalid` is clear, so the RAS acceptance the scan hold
  deliberately honours names no detection (`DMR_SEARCH_CALL_NOTES`).

- `src/protocol/tetra/tetra_confirm.{c,h}` — the same module for TETRA, whose 22-bit normal-burst training sequence
  receiver noise matches under the frame sync's eight hypotheses. A burst proves itself with a block whose CRC passed:
  a BSCH, SCH/HD, SCH/F, STCH or BNCH block (the CRC-16 of EN 300 392-2 clause 8.2.3.3), direct-mode or uplink
  signalling, or a speech frame that passes its CRC and re-encodes like speech. `processTetraFrame()` and
  `processTetraSBFrame()` return it and `dsd_dispatch_handle_tetra()` maps it to `PROFILE_PROVEN` or `UNPRODUCTIVE`.
  The carrier's evidence (CRC-passed BSCH and MAC blocks, and the followed call's signalling) gates trunking and
  speech; see TETRA below. The engine clears it with the carrier through `tetra_confirm_reset()`, exported in
  `<dsd-neo/protocol/tetra/tetra.h>`.

### TETRA

The signalling and speech Viterbi decoders in `tetra_fec.c` precompute each destination's incoming coded word.
The higher predecessor complements every coded lane, so one branch cost serves both edges. Independent
add-compare-select operations use 32-bit metrics (a 512-bit path costs at most 134215680), clamp unreachable paths,
and prefer the lower predecessor on ties. `conv_acs_step()` is the scalar reference; on SSE2 (x86-64 baseline) and
NEON targets `conv_acs_run()` keeps the 16 survivors in four 128-bit vectors (predecessor gathers are fixed
shuffles; metrics stay below 2^31, so SSE2's signed compares are exact) and drops the INF clamp once no survivor can
reach INF in the remaining steps. Other targets run the scalar steps. `TETRA_VITERBI` checks every survivor and
traceback against a 64-bit polynomial oracle, including maximum-length, erased and tied inputs, and the vector kernel
against `conv_acs_step()` bit for bit on arbitrary metrics (near-INF, forced ties, unreachable) and whole runs.
`dsd-neo_bench_tetra_viterbi` times both decoders.

#### Burst sync

Training matching (`<dsd-neo/dsp/tetra_training.h>`) uses the first dibit to select possible rotations before comparing
the full word; lower rotations still win, with the normal word preferred over its inverse at the same rotation.
Frame-sync history windows copy at most two contiguous spans of the 128-symbol ring and terminate only complete
windows. `FRAME_SYNC_INTERNAL_HELPERS` covers every ring head and compares training decisions with a scalar oracle.

The frame sync (`frame_sync_try_tetra()` in `src/dsp/dsd_frame_sync.c`) locks a TETRA burst at the end of its training
sequence and captures block 1 from its history into `dsd_state::tetra_sync`, on the TMO downlink with AACH half 1, the
7 dibits right before the training sequence (`aach1_dibuf`; the decoder reads half 2 after it). On the uplink (`--tetra-uplink`) normal
training sequence 1 is also dibits 2..12 of the control burst's extended training sequence (EN 300 392-2 clause
9.4.4.3.3), so there the control burst is tried first and a normal burst's training is matched two symbols back, once
those two symbols showed it ends no control burst; they are block 2's first two dibits, which the decoder takes before
it reads on (`b2_lead_count`, `tetra_read_block2()` in `tetra.c`). Off the uplink the normal burst locks at its training
as before. Every downlink burst runs the TDMA clock to its own slot on the symbol count before its blocks are decoded: a
normal burst at its training sequence (`tetra_tdma_advance_ndb()`), a synchronisation burst at its BSCH, which sets the
clock when its CRC passes and otherwise only runs it (`tetra_sb_bsch()`), so block 2 of a burst whose BSCH failed is not
decoded as the last burst's timeslot. Normal bursts cannot tell the signal's polarity (NTS2 is NTS1 inverted), so the
carrier's polarity (`tetra_sync.polarity`) comes from a synchronisation burst's training, which the frame sync latches,
and on the uplink from a control burst: its extended training sequence is no other training sequence inverted, so its
synctype's sign is its polarity, its SCH/HU is decoded with it, and once that CRC passed the decoder makes it the
carrier's (`tetra_uplink_process_cb()`); one that fails changes nothing, since a normal-polarity NTS2 burst can match
the inverted extended training sequence. That SCH/HU also proves a trunked mode uplink, which no BSCH comes to say there:
like a BSCH of a trunked system code on the downlink, it ends direct mode (`tetra_dmo_leave()`: the mode, the DM colour
code, its protection and the direct mode call with its canonical call), so the normal bursts after it are NUBs again
and not DNBs descrambled with a DM colour code. The frame-sync slicer cuts TETRA's symbols at fixed thresholds of the CQPSK
symbol domain (0 and ±2, `frame_sync_slice_cqpsk_dibit()`), where the RTL front end and a soft symbol capture give them;
a float symbol file (`.raw`, `.sym`) holds them there too, so a TETRA session reads it as it is
(`symbol_file_flt_scale()` in `dsd_symbol.c`), while the other modes' level-tracking slicers read one scaled by 10000
into the PCM sample domain; scaled, both inner levels would slice as outer ones. Tests: `FRAME_SYNC_INTERNAL_HELPERS`,
`TETRA_BURST_VERDICT`, `DSP_SYMBOL_REPLAY`, `DECODE_IQ_TETRA_FLOAT_SYMBOL_REPLAY`.

#### Signalling decoders

The uplink is the radios' side, and its PDUs are not the downlink's (EN 300 392-2 clause 21.4.2; uplink MM PDU type 2
is U-LOCATION UPDATE DEMAND where the downlink's is D-CK CHANGE DEMAND), so no uplink block reaches the downlink
dispatchers. `src/protocol/tetra/tetra_ul.c` (`tetra_ul.h`) is entered only from a control uplink burst's CRC-passed
SCH/HU (`tetra_uplink_process_cb()`) and a normal uplink burst's CRC-passed SCH/F (`tetra_nb_signalling()` with the
NUB's `tetra_nb_ctx::uplink`; its stealing channel half slots and a synchronisation burst's block 2 still go to
`tetra_mac_parse_block()`, which logs them unread). `tetra_ul_read()` reads the block's first MAC PDU only, a
MAC-ACCESS (table 21.41) or MAC-DATA (table 21.46), its end the lesser of its length indication and the block (clause
23.4.3.3) and its fill bits off (`tetra_mac_content_end()`, shared with the downlink); of a clear TM-SDU that is whole
or a fragment start, the basic link LLC header (a whole FCS variant's FCS checked), the MLE discriminator, an MM PDU's
type, and of a U-LOCATION UPDATE DEMAND (table 16.18) its leading type 1 elements up to the ciphering parameters.
`tetra_ul_block()` sets the uplink's air encryption (`ul_enc`, Null PDUs included, clause 21.4.2.1 note 1), prints the
summary lines (an encrypted PDU's SSI as `esi=`, EN 300 392-7 clause 4.2.6), and notes the KSG a clear demand proposes for the serving network (`tetra_air_ksg_request()`, never the
address extension's network, the MS's own MNI), apart from the network's KSG evidence: a proposal the SwMI may reject
(EN 300 392-7 clause 6.6.2.1) never chooses a key's algorithm. Nothing else of the uplink is read: no associated PDU,
later fragment, MAC-U-BLCK, encrypted TM-SDU or other PDU. Test: `TETRA_UL`.

Each downlink signalling PDU family has a pure decoder that reads one PDU into a struct the caller provides and returns
1 only when the whole PDU, optional elements included, was there and well formed; it keeps nothing and acts on nothing.
All are module-private: `tetra_mac_resource_decode()` and `tetra_mac_broadcast_decode()` (the MAC-RESOURCE header with
its address, SYSINFO with its D-MLE-SYSINFO, ACCESS-DEFINE; `src/protocol/tetra/tetra_mac_pdu.h`, with
`tetra_access_assign_dl_usage()`, the downlink usage an AACH's ACCESS-ASSIGN gives its slot), `tetra_mle_decode()`
(the MLE protocol PDUs, the D-CALL RESTORE of a D-RESTORE-ACK included), `tetra_sds_decode()` (D-STATUS and D-SDS-DATA:
the SDS-TL header, the text, a concatenation part's control fields but not its reassembly) and `tetra_sndcp_decode()`
(the SN-PDU header; all three in `tetra_mle_pdu.h`), and `tetra_mm_decode()` (the MM PDUs of EN 300 392-2 clause 16 and
EN 300 392-7 annex A; `tetra_mm_pdu.h`). The dispatchers (`tetra_mac_parse_schd()`, `tetra_mle_dispatch()`,
`tetra_mm_dispatch()`) decode through the same readers and act only on a complete PDU: the summary line, the call event,
the notice, the SDS reassembly, the KSG an SCK Provide of a trunked mode SCK (SCK use 0, EN 300 392-7 table A.101;
SCKs for direct mode alone name no trunked algorithm) or a GCK Provide names, and the KSG the ciphering parameters of a
D-LOCATION UPDATE COMMAND or REJECT name for the serving network (not the MS's network of its address extension). Of
signalling, `tetra_state_t` keeps only what the decoder reads back: the cell's main carrier, band, offset and location
area with its broadcast security (the extended services broadcast's security information, EN 300 392-7 table A.104, and
the BS service details' air interface encryption bit, each kept until the next SYSINFO that carries it), the KSG
evidence (`air_ksg`: per network, the set of KSG numbers its signalling named; `tetra_air.c`), the hyperframe number,
the encryption mode
of the last addressed MAC-RESOURCE, the reassembly contexts (one message each, keyed by its reference, its transport
and its sender's TETRA subscriber identity: the SSI with the network a calling party of type 2 names in its extension
and one of type 1 leaves to the current network, EN 300 392-2 clause 14.2.3 and tables 14.43, 14.44) and the D-MM
STATUS the frontends show. What a BSCH of
another cell or network forgets is decided in one place (`bsch_identity_change()` in `tetra_bsch.c`, before the new
identity is taken, so what ends carries the network it was decoded on). The SYSINFO part belongs to the cell: a BSCH
naming another MCC, MNC or colour code (`bsch_forget_cell()`) drops it with the TDMA clock, the partial TM-SDUs (a
TM-SDU is rebuilt on one cell's control channel, whatever the colour code of a later fragment), the channels the last
cell allocated to the calls and the event labels it assigned (`tetra_calls_forget_cell()`; the records stay), and `trunk_cc_freq`
when that SYSINFO set it, so an allocation decoded before the new cell's SYSINFO resolves to no frequency and nothing
is followed until it names its control channel; `tetra_forget_carrier()` keeps it, since another carrier of the same
cell shares it. The cells of a network may share a colour code, so a SYSINFO naming another main carrier (the cell's,
clause 21.4.4.1) or another location area than the cell's known one is another cell too, which forgets the same before
the SYSINFO is taken (`tetra_bsch_sysinfo_cell()`, from `apply_sysinfo()` in `tetra_mac.c`). Another cell is also another carrier, whatever the engine saw (a radio retuned outside the decoder),
so what the last carrier's speech left goes as at a carrier boundary: its timeslots' signalling encryption, the heard
timeslot with the heard or anonymous canonical call (`tetra_calls_forget_trunked_carrier()`), the speech in progress,
the followed call's proof on the carrier, and the encryption mode the last cell's signalling showed. A DSB that puts a
trunked carrier in direct mode (`tetra_enter_dmo()` in `tetra.c`) forgets the same of the trunked carrier, its partial
TM-SDUs too, and stops the TDMA clock; a BSCH of a trunked system code or an uplink SCH/HU that ends direct mode
forgets the direct mode channel's (`tetra_dmo_leave()`). A BSCH naming another network (MCC or MNC) also forgets what that network's SwMI numbers: a downlink
SSI is a party of the current network and the call identifiers are the SwMI's (EN 300 392-2 clause 14.2.3), so the
call records end, with the state machine's follow of their identifiers, the heard call and the speech in progress
(`tetra_calls_forget_calls()`), and so do the concatenated short data being reassembled
(`tetra_mle_forget_reassembly()`) and the D-MM STATUS the frontends show. Another cell of the same network keeps them; the call records end there only when
its SYSINFO names another location area, whose call identifiers may differ (clause 14.2.3,
`tetra_calls_note_location_area()`). Retunes within the cell, to a traffic carrier and back, keep all of it. Nothing
here tunes: a followed call of the last network is followed on as a channel without a call record until the grant
timeout or the hangtime ends it. A fragmented TM-SDU (clause 23.4.3.1.1) is rebuilt per
timeslot, the one the TDMA clock gives its first fragment: before the clock is set (a new carrier before its BSCH) a
first fragment is not kept, since a continuation of any timeslot could join it (`start_fragment()`). It is dropped
after N.203, four slots of its timeslot without a fragment, counted on the TDMA clock: the slots of
the timeslot that went by undecoded count too (`catch_up_fragment()` in `tetra_mac.c`), so a late continuation cannot
join an expired prefix. Tests: `TETRA_MAC_PDUS`, `TETRA_MLE_PDUS`, `TETRA_SDS_PDUS`, `TETRA_CMCE_PDUS`,
`TETRA_MM_DISPATCH`, `TETRA_MLE_CMCE` (the SDS-TL, text and reassembly cases, the sender's identity, the reassembly
a network change drops),
`TETRA_MAC_STATE`, `TETRA_MAC_FRAGMENTS` (the TM-SDU a network change drops, none without a timeslot),
`TETRA_TDMA_CLOCK`, `TETRA_CALLS` (the
network, cell and location area boundaries), `TETRA_VOICE_PIPELINE` (the speech a cell or network change starts
over).

#### Air encrypted signalling

`tetra_air_sig.{c,h}` decrypt the air encrypted downlink signalling with a loaded `--tetra-key` (issue #750). EN 300
392-7 clause 6.7.1.2: everything after the channel allocation flag of a MAC-RESOURCE and of a downlink MAC-END is
ciphertext, the allocation element and the TM-SDU; a MAC-FRAG's fragment and a MAC-D-BLCK's TM-SDU are too; the MAC
header and the fill bits stay clear (clause 6.4.0), so `content_end()` on the received bits finds the plaintext's end.
Each PDU restarts its slot's key stream segment (clause 6.4.2.2, table 6.4): KSS(0) in a full slot and a first half
slot, KSS(216) in a second half slot (STCH-2, an SB's block 2), which `tetra_nb_signalling()` hands the MAC
(`tetra_mac_parse_block()`, `mac_block::ks_offset`). The key stream is the voice path's: the slot's initial value from
the TDMA clock and hyperframe, TB5 of the carrier being received (`tetra_air_sig_slot()`). A fragmented TM-SDU keeps
its ciphertext pieces (`tetra_mac_fragment::piece`, each with its slot's initial value, carrier and offset) and is
decrypted piece by piece at its MAC-END, whose piece is the allocation then the last fragment (`fragment_open()`); an
encrypted first fragment with an allocation, or a piece that cannot be kept (more than `TETRA_MAC_FRAG_PIECES`, 128:
an N.202 TM-SDU in fragments averaging 21 bits, which associated PDUs ahead of each MAC-FRAG can leave), leaves the
TM-SDU undecrypted (`cipher_lost`).

Basic link signalling carries no check of its plaintext, so a decrypted TM-SDU is acted on only when its own plaintext
proved itself (`tetra_air_sig_open()`): every candidate (the network's 80-bit key with its named TEA, or each of
TEA1..TEA3; a TEA1 reduced key of exactly this channel, after every 80-bit key whatever the key slots, so a proof that
both give names identities) decrypts it, the digest of each plaintext is kept as evidence
(`tetra_state_t::air_evidence`: 512 entries, `TETRA_AIR_EVIDENCE_S` = 1 s and, for a fragmented TM-SDU, as long again
as it took to arrive (`tetra_air_sig_input::span_s`, at most `TETRA_AIR_EVIDENCE_SPAN_S`), at most 16 of one clear
header, cleared at a carrier boundary and another cell), and an earlier TM-SDU of the same clear header (PDU kind, address type and value,
label, usage marker, encryption mode, allocation flag), length, carrier, location area and colour code, taken under
another initial value, that decrypted to the same plaintext under a candidate proves that candidate. Only 48 or more
encrypted bits count; a wrong key, wrong TEA, or a PDU under a key the user lacks (DCK, MGCK, a second TEA, another key
version) matches with a chance of about 2^-L + 2^-64 per comparison, at most 64 comparisons a PDU. Nothing reads the
cell's broadcast security class. A first copy, a TM-SDU sent once or shorter than 48 bits is handled as before
(`resource_encrypted()`, `deliver_encrypted()`); a proved one is parsed as a clear one (`resource_parse()`,
`complete_fragment()`), carrying its encryption mode and key to the call records, which take them before anything
judges the call (`tetra_calls_on_event()`; the MAC reports no unread allocation for it to `tetra_calls_on_addressed()`).
A rebuilt TM-SDU's digest puts its MAC-END's allocation first (`tetra_air_sig_input::alloc_at`, `alloc_bits`), so two
copies fragmented apart prove each other; until it is decrypted the fragment buffer holds that allocation beside the
TM-SDU (`TETRA_MAC_FRAG_BITS`), and the rebuilt TM-SDU is held to N.202. A proof notes its TEA as the
network's KSG evidence. A proved PDU's encrypted short identity is decrypted with TA61 under the 80-bit key that proved
it (`tetra_air_sig_identity()`, `tetra_mac_address::esi_decrypted`; the `-Z` line shows `SSI(n from ESI m)`); a reduced
key leaves it, so its D-SETUPs name no called party. The cell key epoch (`air_epoch`) moves when a SYSINFO announces
another SCK-VN or CCK-id (CCK flag set), SCKN or GCK support (on a cell that offers air encryption), when the
encryption mode's parity settles on the other value (three encrypted headers in a row, `tetra_air_sig_note_parity()`,
or a TM-SDU proved under it, which settles it before its key is stamped, so a key proved during a key version change
is current at once), and at another cell; a call's key applies only under the epoch and parity it proved under and while the same key (and
named TEA) is loaded, while the call's latest allocation names the key version parity it proved under
(`tetra_calls_air_key_applies()`), and a reduced key only while the call is on its channel. No key, ECK, register or key stream bit is printed. Tests: `TETRA_AIR_SIGNALLING` (TETRA_crypto's
own TB5 and TEA2 key streams, frozen; wrong keys and TEAs; two TEAs under one key; a DCK stand-in; PDU association and
the second half slot; a fragmented TM-SDU; a reduced key; the key epoch; the repeat rules and a busy carrier),
`TETRA_CRYPTO` (TA61).

#### Trunking

`src/protocol/tetra/tetra_trunk_sm.c` behind `include/dsd-neo/protocol/tetra/tetra_trunk_sm.h` follows at most one
traffic channel off a TETRA control channel (the MCCH, EN 300 392-2 clause 9.4.2.1), with three states: IDLE (not
trunking, or no control channel known), ON_CC and TUNED (on the followed channel, a tune in flight included). Its state
is `tetra_state_t::sm` in the session's TETRA state extension (`DSD_STATE_EXT_PROTO_TETRA`), so two sessions never share
it, and nothing in it is global. TETRA is not a trunk-scan target type; trunk scan never runs it.

- Grants. The call records decide what to follow (Calls below: talkgroup policy, hold, preemption, the encryption
  lockout) and hand the state machine a `tetra_sm_grant`: downlink frequency, carrier number, timeslot bitmap (table
  21.80: bit 3 is timeslot 1) and CMCE call identifier (0 when the allocation carried none). `tetra_sm_on_grant()`
  follows it only with trunking on, a control channel known (`state->trunk_cc_freq`, which a SYSINFO with a valid main
  carrier sets before calling `tetra_sm_on_cc_sync()`), a frequency, no tune in flight, and a CRC-passed downlink BSCH
  or MAC block decoded on the carrier the receiver is on before the block that carried the grant
  (`tetra_confirm_downlink_proven()`: `tetra.c` notes a block only after the MAC acted on it; the uplink's SCH/HU and
  MAC blocks prove its carrier for publication, `tetra_confirm_carrier_proven()`, but no control channel, and carry no
  grant). A grant for the followed
  call is ignored, a call identifier arriving after a header-only allocation is adopted, and a grant on the carrier the
  receiver is on (`tetra_sm_receiver_freq()`: the followed channel while TUNED, else the tuned frequency,
  `dsd_opts_tuned_freq_hz()`, none while a request is in flight; not the SYSINFO main carrier `trunk_cc_freq`, which a
  secondary carrier broadcasts too, so a session started there tunes to a grant on the main carrier; the calls module
  reads the carrier being received by the same rule, `tetra_calls_carrier()`) is followed without a retune; the grant
  itself then
  proves the call on that carrier (`tetra_confirm_note_followed_call()`). Every other grant tunes through
  `dsd_trunk_tuning_hook_tune_to_freq()` with `ted_sps` 0: a TETRA traffic carrier keeps the control channel's symbol
  rate, and `dsd_engine_cc_is_p25()` (`trunk_tuning.c`) answers 0 for TETRA syncs, before looking at `rf_mod`, whose
  QPSK value would otherwise read as P25 CQPSK.
- Releases. `tetra_sm_on_release()` releases only the followed call: its call identifier must match (a release of
  the followed call, a "go to the MCCH" allocation to it, its channel allocated to a call the state machine does not
  follow, or its turning undecryptable; see Calls). A traffic timeslot of the control
  channel's carrier, or a release with no control channel to go to, stops following without a tune; anything else
  returns through `dsd_trunk_tuning_hook_return_to_cc()` with `p25_cc_freq` cleared, so no P25 alias or its symbol
  profile comes along. A return the backend refuses or defers keeps the call, and the next tick tries again. With
  trunking off nothing returns (`sm_release()`): the engine's return tunes nothing without trunking
  (`dsd_engine_return_to_cc_request()`) yet answers OK, so a return asked for then would report the receiver back on
  the control channel while it stays on the traffic channel.
- Timers. Liveness never depends on CMCE call state, which one missed release would leave stuck. Speech of the
  followed call (`tetra_calls_on_speech()`, for each burst the follow gate let through with a speech frame whose CRC
  passed) calls `tetra_sm_note_voice()` with the timeslot the TDMA clock gives it: that stamps
  `last_vc_sync_time(_m)`, and while TUNED it restarts the hangtime when the timeslot is a followed one (not 0, a
  timeslot the decoder cannot place, which may be any timeslot's). Other heard speech stamps `last_vc_sync_time(_m)` only. The tick returns `opts->trunk_hangtime` seconds after
  speech ends (the first gap of `TETRA_SM_SPEECH_GAP_S`, longer than a frame 18 gap), and after
  `TETRA_SM_GRANT_TIMEOUT_S` (4 s, DMR's default; no option) when no speech followed the tune, counted from the later
  of the tune and the last maintenance event before the first speech (`tetra_sm_note_maintenance()`, `t_maint_m`,
  rolled back with `t_tune_m`): an ACCESS-ASSIGN PDU of a followed timeslot carrying the call's usage marker, or a
  CRC-passed PDU there addressed to a party of the call, a MAC-D-BLCK to its event label included whether or not its
  TM-SDU can be read (`tetra_calls_on_label_pdu()`; the label is clear), never to all (the broadcast SSI or the all-ones event label; `address_maintains()`;
  on the record's own channel, carrier, frequency and timeslot, which an allocation that moved the call without the
  receiver leaves; the T.209 events of EN 300 392-2 clause 23.5.6.1.2; the 4 s
  and the before-first-speech scope are the scanner's, not T.209's 18 multiframes), so a call set up and silent is
  not left and re-entered every 4 s while its channel is kept for it. It drops the call
  without a tune when another owner released the shared tuning state (`opts->trunk_is_tuned` cleared: a tuner
  hand-back, the stale-follow cleanup), and drops ON_CC to IDLE once trunking is off or no control channel is known.
  Trunking turned off while tuned (the trunk toggle clears `trunk_enable` alone) leaves the receiver on the followed
  channel, as DMR's and P25's machines leave it, which do not tick then (`dsd_trunk_*_recovery_allowed()`): the machine
  keeps the channel, its shared tuning state and its timers, and lets go of the call (`sm_hold_untrunked()`, no call
  record, as `tetra_sm_retire_call()` leaves it), so what the receiver decodes there is decoded as on any carrier with
  trunking off: the record ages, its signalling publishes it only as the heard call, and the follow gate's proof goes.
  With trunking on again the hangtime or the grant timeout returns from the channel.
- Tunes in flight. A request the backend reports PENDING stages the new state with everything a failure puts back
  (`tetra_sm_rollback`: the shared tuning state, the followed grant, the timers, the state, and the followed call's
  proof on the carrier, which comes back only while no carrier reset came between: `tetra_confirm_carrier_epoch()`).
  Until the backend answers, the request is the backend's, however long that takes: nothing times it out locally (as
  with P25's tunes), so the frame gate stays closed and the backend's completion is the tuner boundary. It is settled
  once it is answered -- a completion commits, a failure restores the rollback first and completes the request after,
  so the frame gate opens onto restored state (`trunk_tuning_hooks.h`) -- at one point per burst: the burst's entry,
  before any of its blocks (`tetra_burst_enter()` in `tetra.c`, `tetra_sm_settle()`, for every burst `processTetraFrame()`
  and `processTetraSBFrame()` take: the trunked downlink's normal and synchronisation bursts with both their blocks,
  the uplink's normal and control bursts, direct mode's), and the tick between bursts. Nothing inside a burst settles:
  the state machine's entry points the burst's PDUs and speech reach (`tetra_sm_on_grant()`, `tetra_sm_on_release()`,
  `tetra_sm_on_cc_sync()`) take a request answered during the burst as still in flight (`sm_settle_at_entry()`), and
  so do the call records' (`tetra_calls_on_addressed()`, `tetra_calls_on_event()`, `tetra_calls_on_allocation()`, the
  fragments a MAC-END completes, the voice gate), so every PDU and speech frame of a burst is judged against one
  state machine, the one its entry left, and the answer is taken at the next burst. A request a PDU of the burst
  itself starts is in flight for the rest of the burst too, and no grant is handed over while one is
  (`call_consider()`). Outside a burst (the engine, the frontends' commands) an entry point settles as the tick does.
  A known limit: a completion the backend publishes after the engine's last tick (`dsd_engine_tetra_trunk_tick_locked()`
  in `live_scanner_process_synced_frames()`) but before the next burst's capture lets that burst, captured on the new
  carrier, through the engine's tune-generation gate (`frame_tune_generation`) while the state machine has not yet
  taken the completion. The burst's entry settle then commits the tune, moves the carrier evidence epoch, and drops the
  burst as one captured on the carrier left: the first burst on the new carrier is lost, one burst, and decoding
  resumes with the next. This is accepted because every burst is decoded against the state machine of one carrier, and
  telling a burst captured on the new carrier from one captured on the old needs the tune generation the burst was
  captured under, which the burst decoder is not given.
  `tetra_sm_init()`, the external return, the carrier abandon and the TETRA state's own cleanup (a decode-mode change
  drops the extension) let go of a request they find (`tetra_sm_disown_pending()`), and hand its terminal cleanup to
  the runtime trunk-tuning layer, which outlives the TETRA state (`dsd_trunk_tuning_request_disown()`): one the
  backend has not answered stays the backend's, so the frame gate waits for it and its completion still publishes the
  tuner boundary, and its failure, answered before or after, is retired at once with nothing to put back, so it does
  not hold the frame gate shut while trunking stays on, whether the session is still on TETRA, in another mode or back
  on TETRA with a new state machine that knows nothing of it.
- The carrier. The receiver's carrier changes when a tune completes, not when it is asked for: then
  `tetra_forget_carrier()` (`tetra.c`) drops the carrier's CRC evidence, the partial TM-SDUs
  (`tetra_mac_forget_fragments()`), the TDMA clock (`tetra_tdma_stop()`), which counts symbols a retune may have
  dropped, the speech in progress (`tetra_voice_reset()`), and the direct mode call on the channel with its DM colour
  code, seed and protection (`tetra_dmo_forget_carrier()`, `tetra_calls_forget_carrier()`), so the next carrier's normal
  bursts are not descrambled with them and its calls and speech are not classified by them, and direct mode itself: the
  next carrier is trunked until its own DSB says otherwise, since no DNB decodes before that DSB gives its colour code
  anyway, and an uplink session, whose carriers have no BSCH, decodes its NUBs at once; the frame sync is told to look
  for block 1 behind AACH half 1 again (`tetra_sync.block1_follows_training`). A tune or return completed when a burst
  begins (answered since the last burst or tick, and settled at the burst's entry) leaves that burst, captured on the
  carrier left, to that carrier: it is dropped whole, its symbols read and none of its blocks decoded
  (`tetra_confirm_burst_begin_captured()`: stale from its start; `tetra_nb_decode()`, `tetra_uplink_process_cb()`,
  `tetra_process_sb()`). A tune completed inside a burst (a grant in its first block) leaves the rest of that
  burst on the carrier left: `tetra_confirm_burst_current()` turns 0, the MAC stops parsing the block and the
  normal-burst decoder skips the second half, and nothing the burst decoded counts for the new carrier, though the
  burst's verdict still does. Nothing here frees or replaces the state extension: the MAC holds its `tetra_state_t`
  pointer across these calls, so every reset clears fields.
- Leaving a call (`sm_unfollow()`) ends its canonical call as a teardown and clears its talkgroup-policy route
  (`tetra_calls_unfollowed()`; with NULL options after the engine ended the canonical calls itself). Its call record
  stays until its release or its ageing; the follow gate reads only the state machine, so no stale record gates a
  later timeslot.
- Engine. `dsd_engine_tetra_trunk_tick_locked()` (`engine.c`) runs `tetra_sm_tick()` in a TETRA session (`-fT`)
  outside trunk scan, from `live_scanner_main_loop()` under the P25 SM tick guard, at every pass and after every frame,
  withheld ones included, since a TETRA carrier syncs on every burst and the synced-frames loop may never hand back.
  The decoders never tick it. `tetra_sm_init()` runs at engine start. `carrier_boundary_release_followed()` abandons
  the machine without tuning (`tetra_sm_abandon_carrier()`) whenever it owns the return (`tetra_sm_owns_return()`), not
  only while TUNED: a return of its own in flight rests it on the control channel with `trunk_is_tuned` already
  released, and that request failing later would put the call left back onto the operator's carrier;
  `carrier_boundary_forget_evidence()` clears the
  evidence (`tetra_confirm_reset()`), and `dsd_engine_forget_carrier_decoding()` and the no-carrier reset
  (`no_carrier_reset_decode_state()`) forget the carrier (`tetra_forget_carrier()`). While the machine owns the return
  (`tetra_sm_owns_return()`: TUNED, a tune in flight and a grant still waiting for its first speech included, or a
  return of its own in flight), the no-carrier pass ticks it instead of running the generic recovery
  (`no_carrier_tick_tetra_owner()`, as `no_carrier_tick_dmr_owner()` does for an active DMR machine): the generic
  return reads `last_vc_sync_time` against the hangtime alone, so with `-t 1` it would end a grant its four-second
  timeout still keeps, or drop the follow where no control channel is known. Otherwise `no_carrier_finish_cc_return()`
  reconciles an accepted return (`tetra_sm_on_external_cc_return()`: ON_CC, no tune); when it cleared the voice state
  without a return (the P25 helper failed, or no control channel) a TUNED machine drops its call where the receiver
  is; a return that failed or was deferred never gets there and leaves the call. Every manual tune in
  `app_command_queue.c` that moves the receiver under trunking (Return to CC, a skip or lockout returning to the
  control channel, a control channel candidate or channel cycle) hands the machine over the same way once the calls
  are ended (`hand_tetra_sm_over()`), a tune or return of its own in flight included, so the request failing later
  puts nothing back.
- Synctypes. `DSD_SYNC_IS_TRUNKABLE` includes TETRA, which offers the Qt "Follow this system" control on a TETRA
  carrier; direct mode shares the sync words, and there the state machine finds no control channel and follows
  nothing. `DSD_SYNC_IS_INVERTED` includes the negative TETRA syncs.

Tests: `TETRA_TRUNK_SM` (the state machine per session against stand-in tuning hooks), `ENGINE_TETRA_TRUNK` (the
engine tick, the carrier boundary, a return in flight included, a decode-mode change with a request in flight, trunking
turned off while tuned against the engine's own return, and the no-carrier pass with the real machine, which returns
at its own deadlines and settles its own return in flight), `ENGINE_GENERIC_TRUNK_RETURN_MATRIX` (the TETRA rows: the no-carrier pass leaves the
real machine's follow to it, with and without a control channel),
`ENGINE_TRUNK_RETUNE_REGRESSION` (no P25 timing for TETRA), `APP_COMMAND_QUEUE` (manual Return to CC and the other
manual tunes, a return in flight included, decode-mode change, a refused switch off TETRA that keeps the follow), `TETRA_BURST_VERDICT` (whole bursts: the verdict, the synchronisation burst's BNCH, the MAC told of failed
CRCs and given the colour code, a retune inside a burst, the settle at a burst's entry and the burst it drops whole;
and, at compile time, the TETRA syncs in the two synctype
macros), `TETRA_MAC_STATE`, `TETRA_CALLS` (grants through the call records against stand-in tuning hooks).

#### Calls

`src/protocol/tetra/tetra_calls.{c,h}` (module-private) hold the CMCE calls the downlink signals, decide which one to
follow, and publish the canonical call; `tetra_voice_gate.{c,h}` decide which speech is heard and which call it
belongs to. The MCCH signals the calls of every carrier and timeslot of a cell, several at once, so nothing here is a
single "current call".

- Records. Up to `TETRA_CALLS_MAX` (8) `tetra_call_t` in `tetra_state_t::calls`, keyed by the 14-bit call identifier
  every call PDU of EN 300 392-2 clause 14.7.1 carries. The MLE turns each CRC-passed call PDU into a `tetra_call_event`
  (`tetra_calls_on_event()`): D-SETUP is the only PDU that makes a record (its MAC address is the called party, its
  calling party and basic service the rest); every other PDU updates the record it names and nothing when there is none,
  and D-RELEASE / D-DISCONNECT end it. A D-SETUP naming another called party, another calling party or another kind of
  call under a known identifier, or a D-INFO or D-CALL RESTORE renaming another call to it, is a new call taking the
  identifier of one whose release went unheard: a call keeps its parties, which a late-entry D-SETUP repeats. A calling
  party is its SSI in its network (`setup_names_another_caller()`): one of type 2 names its network in its extension,
  one of type 1, or the D-CONNECT address a record takes for one, is of the current network (EN 300 392-2 clause
  14.2.3, tables 14.43 and 14.44), so the same SSI of another network is another party, and a party not known on
  either side is no change. Both end a call the same way (`call_end()`): its canonical call ends, its record goes and,
  when it was the followed call, the state machine lets go of it (`tetra_sm_on_release()`), so a new call under the
  identifier meets the talkgroup policy like any other and is named by its own parties, never the old call's talker.
  Every record that leaves the table, a release, an identifier taken, ageing or eviction, ends its canonical call and
  lets go of its identifier (`calls_drop()`), the heard speech's included (`calls_forget_heard_call()`: not trunking, a
  call given the identifier is the heard call only once its own speech is heard): while the state machine cannot let
  go (the return refused or
  deferred, a tune in flight, or a return in flight that would put the call back if it failed) it follows the channel
  but no call record (`tetra_sm_retire_call()`, the rollback included): `tetra_sm_followed_call_id()` reports none, the
  followed call's proof goes, and no PDU, rename, allocation or speech of a call given the identifier counts as the
  followed call's or takes the followed call's grant path, until the grant timeout or hangtime gives up following or a
  grant the policy allows follows the new call as a call of its own. D-INFO and D-CALL RESTORE may rename a call: the
  state machine takes the followed one's new identifier as it is (`tetra_sm_on_call_renamed()`), in what a tune in
  flight would put back too, not as a grant to admit, since the first block decoded on a new traffic carrier can
  rename it before anything proved that carrier, and no timer starts over; it stays one canonical call. D-TX GRANTED / D-TX INTERRUPT give
  the transmitting party (or the addressed one when granted) and their encryption control the end-to-end flag; D-TX
  CEASED ends the transmission. The records belong to the network and location area they were made in: a BSCH of
  another network or a SYSINFO of another location area ends them all (`tetra_calls_forget_calls()`, Signalling
  decoders above). What a record holds of a cell goes with the cell: a channel allocation names a carrier number and
  timeslots of the cell that made it (clause 21.5.2), and an event label or usage marker is valid on one channel of one
  cell (clauses 23.4.1.2.3, 23.5.5), so another cell of the network keeps the records but not their channels and labels
  (`tetra_calls_forget_cell()`): no speech on a channel the last cell allocated is a record's, and no PDU addressed by a
  label it assigned attaches to one, until the new cell allocates the call a channel of its own. A full table evicts the least recently heard record that is not followed;
  `tetra_calls_tick()` (from `tetra_sm_tick()`) drops records nothing named for `TETRA_CALL_IDLE_S` (30 s) on the decode
  clock. The followed record never ages: the state machine's hangtime ends it. A record's air encryption is the
  encryption mode of the MAC-RESOURCE that allocated its channel, learnt from the clear address (a USSI, SMI or event
  label: an encrypted PDU's SSI is an encrypted short identity, clause 4.2.6) when that allocation is ciphertext (EN 300
  392-7 clause 6.5.1), in the MAC-RESOURCE or in the MAC-END of a TM-SDU encrypted from its first fragment, which the
  call's canonical call takes at once (`calls_sync_published()`); end-to-end encryption
  never overwrites it, and the cell-wide `enc_mode` is for display only. An allocation in signalling a loaded key
  decrypted (Air encrypted signalling below) sets the record's air encryption to the mode it was sent in and its key
  to the one that proved it (`tetra_call_t::air_key`, `tetra_mac_sdu::enc_mode` / `air_key`, `call_attach()`): one
  verdict (`call_air_decryptable()`) then says whether the call is decryptable for the follow policy and lockout, its
  canonical call's crypto class and its speech, which that key and TEA decrypt while it applies
  (`tetra_air_sig_key_current()`) and nothing decrypts after; a record whose allocation was clear or unread keeps the
  key of its carrier (`tetra_air_key_usable_on()`), as anonymous speech does.
- Allocations. The MAC hands the MLE a `tetra_mac_sdu` with the PDU's address and channel allocation. A call PDU in
  the TM-SDU takes the allocation (`call_attach()`); one no call PDU took attaches through the addressed party
  (`tetra_calls_on_allocation()`: the called, calling or transmitting SSI of a record, or the event label assigned to
  one of them on this channel) and never tunes. A usage marker assignment (address type 110) applies with the
  allocation of its own PDU or MAC-END, to the directions it assigns, and is ignored without one (EN 300 392-2 clause
  23.5.5): the record the readable allocation went to (`tetra_mac_sdu::alloc_taker`) takes it as its downlink traffic
  usage marker when the allocation assigns the downlink (`call_bind_usage_marker()`); any allocation that moves the
  record ends the last one, and losing the channel (another call's allocation of it, "go to the MCCH", another
  cell) ends it. An allocation of the uplink alone of the record's own channel (carrier, frequency where both are
  known, timeslots) leaves a record that holds a downlink allocation as it was, its usage marker included, now an
  allocation of the downlink alone (`call_attach()`, `alloc_keeps_downlink()`): the uplink and the downlink of a
  channel are allocated apart (clauses 23.3.4, 23.5.5). One of another channel replaces the record's channel (clause
  23.5.4.2.1), whose downlink an MS then receives by the normal procedures (clause 23.5.4.3.3) with no downlink usage
  marker of the call there. A clear or decrypted allocation that comes with a call PDU sets the call's
  encryption before the PDU is judged (`call_take_encryption()` in `tetra_calls_on_event()`); a followed call that a
  PDU's downlink allocation moves is judged for its encryption once the allocation is attached, on the channel it
  allocates (a reduced key decrypts its own channel only). A "go to the MCCH" allocation (timeslots 0000,
  table 21.87) of the followed call releases it, and retires the call either way (`tetra_sm_retire_call()`, the
  rollback of a tune or return in flight included): where the release cannot be made (a tune in flight for the rest
  of the burst, the call's own reallocation's included, or the return refused or deferred) the state machine follows
  the channel without the call, and a request in flight that fails puts the channel back without it, so nothing of
  the channel the call left plays as its; an allocation to a party no record holds goes nowhere.
- Channel exclusivity. The downlink of a traffic channel carries one call's traffic at a time, under one downlink
  traffic usage marker, and an MS whose call's downlink carries another's leaves the channel (EN 300 392-2 clauses
  23.5.5, 23.5.6.1.1 c)). So an allocation takes its channel from every other record, in the one place a record's
  allocation is set (`call_attach()`, whatever carried it: D-SETUP, D-CONNECT, D-INFO, D-CALL RESTORE, D-TX GRANTED or
  an addressed MAC-RESOURCE; `calls_take_channel()`): a record whose allocation assigns the downlink of a timeslot of
  the same carrier (of the same frequency where both are known) holds no channel from there, whether the new call is
  followed, allowed or blocked, its canonical call ends, it is not the heard call, and the state machine follows its
  identifier no more (`tetra_sm_retire_call()`, a tune in flight's rollback included). The uplink and downlink of a
  timeslot may be allocated to two calls (clause 23.5.5), so an allocation of the uplink alone takes nothing, and
  neither does one the decoder cannot place ("go to the MCCH"; a ciphertext one attaches to no record). When the
  followed channel is taken (the followed call's record lost it, or the allocation names the followed channel itself,
  which a record moved elsewhere or a follow without a call record still holds), the call followed there is over: the
  state machine follows the new call there without a retune when the policy follows it, as no call holds the channel
  to take over, and otherwise lets go of the channel at once as for a "go to the MCCH" allocation, returning to the
  control channel, as P25's machine leaves a voice channel whose new call the policy rejects; where it cannot (the
  return refused or deferred, a tune in flight) it follows the channel without a call, where no speech plays as the
  followed call's or renews the hangtime, until the hangtime or the grant timeout gives up (`calls_leave_taken_channel()`).
- Event labels. An event label stands for one party in place of its address on one MAC channel of one cell (EN 300
  392-2 clause 23.4.1.2.3); a record holds the label of a party of its call and the channel it is valid on
  (`tetra_label_channel`: an allocated channel by its carrier number and timeslots, which names that channel of the
  cell wherever the receiver is, or the channel the assignment came on, by the carrier the receiver was on then,
  `tetra_confirm_carrier_epoch()`, and the timeslot the TDMA clock gave). Its whole lifecycle is one section of
  `tetra_calls.c`, run once per delivered PDU (`calls_label_delivered()`), with the cell and the record:
  - Assignment. An SSI or SMI + event label address (types 101, 111) gives its party the label when its PDU is
    delivered (clause 23.4.1.2.3.1): a fragmented TM-SDU at its MAC-END, never at its first fragment, and an encrypted
    PDU as a clear one, the MAC delivering its clear address (`deliver_encrypted()`), in which an SSI is an encrypted
    short identity that names no party (EN 300 392-7 clause 4.2.6). It is valid on the channel the PDU allocates, else
    the one it came on. A PDU addressed by the label (type 010) is the holder's only on that channel
    (`calls_label_here()`): the carrier being received (`tetra_calls_carrier()`) for an allocated channel, the same
    carrier epoch for the other, and an overlapping timeslot when both are known.
  - Reuse. On one channel a label stands for one party, so an assignment takes the number from every record of
    another party whose label is bound to a channel that meets the new one (`label_bind()`, `label_channels_meet()`:
    the same carrier, overlapping timeslots), whether or not a record holds the party it went to; the same number held
    on another carrier or timeslot is another label and stays.
  - KEEP (all ones). The party's current label stays valid after the channel change its PDU makes (clause
    23.4.1.2.3.0): with an allocation its binding moves to the allocated channel, the value kept, taking the number
    there from others as an assignment does; without one nothing changes.
  - Withdrawal (all zeros). The party's label goes.
  - A channel the decoder cannot tell: a "go to the control channel" allocation (the MCCH or a common SCCH) or a
    ciphertext one, and a received carrier whose number is not known. An assignment or KEEP to it binds none, the
    party's label goes, and so does the number of every other party, since it may be any of their channels.
  - Channel reallocation without KEEP. An allocation to the party (by its SSI, or by the label itself) moves its call,
    not its label: the label is valid on the channel left and no other, so its binding stays there and no PDU of the
    new channel addressed by it is the party's. A stale binding left on a channel the party left can only matter if
    the SwMI gives that number there to another party without the decoder hearing the assignment, which takes it.
  - Expiry (T.201) is not modelled: the decoder could only time it from the PDUs it happened to decode, and the SwMI
    uses an expired number again only by assigning it, which takes it from the old holder. The label lives as long as
    its record, which ends at its release, at an identifier taken, at eviction, or after `TETRA_CALL_IDLE_S` with no
    PDU or speech naming it, taking the label with it.
  - Cell change. Another cell (a BSCH or SYSINFO, `tetra_calls_forget_cell()`) drops every label.
  - Carrier boundary or retune. A label bound to the carrier the receiver was on applies there no more (the carrier
    epoch moved, and no reassignment there was heard while away); one bound to an allocated carrier number still names
    that channel of the cell, so the call followed onto it keeps it.
- Following. Trunking on, an allocation that came with a call PDU, of a call whose D-SETUP named its called party, on
  a downlink phase-modulation channel with a frequency, is judged: encrypted calls not followed lock out
  (`dsd_enc_lockout_note()`) a voice call the listener cannot decrypt (end-to-end encrypted, or air encrypted without
  a key for the carrier it is on, `tetra_air_key_usable_on()`: its allocated carrier, else the carrier being received;
  an 80-bit key covers every carrier, a TEA1 reduced key its own) and release a call it can;
  `dsd_tg_policy_evaluate_group_call()` / `_private_call()` decide `tune_allowed`; while another call is followed, a
  held talkgroup takes over a call of another one, else `dsd_tg_policy_should_preempt()` (route: carrier, any
  timeslot), asked after the followed call is described to the preemption ledger
  (`dsd_tg_policy_describe_followed_call()`) from when it was followed (`tetra_call_t::followed_m`, stamped when
  `tetra_sm_on_grant()` follows it), so a ledger cleared or restarted meanwhile neither restarts its dwell nor reads
  as a takeover. `tetra_sm_on_grant()` then follows it and the route is noted active; a preemption it accepted is
  committed (`dsd_tg_policy_commit_preemption()`, the cooldown), and leaving the followed call clears the ledger
  (`tetra_calls_unfollowed()`). A header-only allocation, one to an
  unknown party or one of a call without a called party never tunes. A followed call that turns undecryptable (its
  encryption control, an encrypted allocation to its party, its basic service, or its speech) while encrypted calls
  are not followed is locked out and left (`call_followed_excluded()`), and its own reallocation is not followed: a
  return refused or deferred keeps the receiver where it is, never on the call's new channel. CRC-passed
  signalling of the followed call, or an addressed MAC-RESOURCE to its party, proves it on the carrier
  (`tetra_confirm_note_followed_call()`).
- Speech. Frame 18 is the control frame of a trunked mode carrier, signalling only (EN 300 392-2 clause 19.4.3.1: traffic
  takes frames 1 to 17, and 17 speech bursts of 60 ms fill the 1.02 s multiframe), so a burst the TDMA clock places in
  frame 18 is never speech (`tetra_tdma_control_frame()`): its signalling block failing its CRC, the burst decoder does
  not try it as full-slot or half-slot speech (`tetra_nb_nts1()`, `tetra_nb_nts2()`), so no chance speech CRC proves the
  burst, and the voice gate hears nothing of it (`tetra_voice_gate()`), so no frame 18 is concealed into 60 ms of audio
  the air never sent, and the speech in progress goes on in frame 1. Direct mode and the uplink run no such clock, and
  a clock left from an earlier BSCH does not place their bursts either. Nor is speech tried in a downlink slot whose
  AACH says it carries no traffic: TCH and STCH are sent under a traffic usage marker in frames 1 to 17 (clause
  23.5.6), so a block that fails its CRC in a slot the ACCESS-ASSIGN PDU gives to control or leaves unallocated
  (`tetra_access_assign_dl_usage()`, table 21.82) is signalling that failed. Between a call's talkers its timeslot
  sends SCH/F blocks of nearly all zero bits, which pass the speech CRC once noise breaks theirs (all zero bits are a
  codeword of every linear code). The AACH is the shortened (30,14) Reed-Muller code (`tetra_rm3014_decode()`, up to 3
  bits corrected, scrambled with the colour code); one that cannot be read says nothing, and the block is tried as
  speech as before. The `-Z` NDB line shows the PDU read, header/field 1/field 2 (`AACH 3/51/51`), or `AACH ?`. Trunking on, only the followed call's timeslots play, once its tune settled and its signalling proved it on
  the carrier, while its record does not place it elsewhere (an allocation no call PDU took, which never tunes, or a
  reallocation whose tune failed moves it off the channel the receiver stays on, which the SwMI may give any call next;
  `voice_record_on()`) and no other call was allocated its channel (Channel exclusivity above: the state machine let
  go of it then), and only as the TDMA clock places the burst: a retune stops the clock until the carrier's next BSCH, and
  a burst it cannot place may be any timeslot's, another call's included, so none plays even when addressed signalling
  proved the call sooner. A followed record that holds an allocation plays only with a downlink usage marker valid on
  the followed channel and receive permission there (EN 300 392-2 clause 23.8.2.3.2; `tetra_calls_followed_rx()`,
  `tetra_confirm_t::rx_perm`): the call's U-plane switching gives it (D-SETUP "granted to another user"; D-CONNECT,
  D-CONNECT ACKNOWLEDGE, D-TX GRANTED and D-CALL RESTORE "granted" or "granted to another user"; D-TX INTERRUPT
  "granted to another user"; D-TX CONTINUE "continue": a receiver that listens hears the downlink whichever party the
  grant names) and takes it back (D-TX CEASED, D-TX WAIT, another D-TX INTERRUPT, for the rest of the burst too), and
  so do the channel's ACCESS-ASSIGN PDUs (`tetra_calls_on_aach()`, from the NDB before its blocks): N.213 (3) of them
  in successive slots of the channel (frame 18 being none) in frames 1 to 17 with header other than 00 and the call's
  marker give it and prove the call on the carrier, which a call followed onto another carrier needs, since its
  traffic slot carries speech and no signalling of it; N.212 (3) received without the marker or T.212 (18 TDMA frames)
  without one (at 72 slots) take it back; an AACH of frame 18 that could not be read breaks no run, another usage
  marker bound to the record starts the run over (the same one assigned again does not), and of the follow's timeslots
  only those the record still holds count while the record is on the followed carrier and frequency, its usage marker
  valid or not.
  A "Replace" (or "Replace + CSS channel") of the followed call's channel to an assigned channel of its downlink, with a
  usage marker assignment, keeps the permission the call held when it is followed without leaving the carrier
  (`alloc_keeps_reception()`, `calls_keep_reception()`; the exception to case d) of clause 23.8.2.3.2); any other
  allocation of the call, "Quit", an additional channel, one with no marker or of the uplink alone of another channel,
  ends a permission held (`call_attach()`, `call_end_reception()`), followed or not, the one a tune in flight's
  rollback would put back included, and leaves an N.213 run alone while none is held; a
  withdrawal in the same PDU applies after; and one the receiver retunes for regains it on the new carrier as every
  follow onto another carrier does.
  A CC withdrawal ends the permission and the run but not the N.208 count; one for the call a tune in flight left is
  what that tune puts back if it fails (`tetra_sm_rollback::followed_rx`). Addressed signalling and short data prove the call but give no permission. N.208 (3) of
  them saying the channel is no longer the call's (header 00, UMx, a downlink marker neither UMa nor the call's, and for
  an allocation of both directions an uplink given to other traffic; clause 23.5.6.1.1) end the follow as "go to the
  MCCH" does, whatever maintains it; the record's allocation goes with it only while it still describes the channel
  lost, so one that moved the call elsewhere since stays. A follow with no record, or a record that holds no allocation, keeps the gate
  above. Not trunking, one timeslot is heard at a time, the first with speech that plays, until that
  speech stops for `TETRA_CALL_SPEECH_END_S` (`tetra_voice_hold()`, `tetra_calls_t::held`), and on the downlink only a
  timeslot the clock places: a burst it cannot place could be the held timeslot's or any other's (the uplink has no
  BSCH, so nothing places its bursts and its speech is heard as it comes). Whether a burst plays is judged before its
  call is published (`tetra_voice_audible()`): its encryption class under the user's encrypted-audio and reverse mutes,
  and the talkgroup policy on the identities its canonical call will carry (`tetra_calls_speech_muted()`, through
  `dsd_audio_group_gate_call()`). Speech that does not play holds nothing and renews no hold, so a blocked or
  undecryptable call never keeps one that plays off the receiver: it shows its call on canonical slot 0 while no other
  timeslot's speech is shown, so two muted calls do not take the slot from each other at every burst, gives way to
  speech that plays, and a held timeslot whose call turns muted holds the receiver only until
  `TETRA_CALL_SPEECH_END_S` after its last speech that played. Its speech belongs to the record whose traffic channel
  is that timeslot of the frequency the receiver is on (`tetra_calls_on_timeslot()`, by `tetra_sm_receiver_freq()`: the
  channel the state machine holds once trunking was turned off while following, whatever the input reports as tuned),
  else to no record, whose air encryption is
  then the last addressed MAC-RESOURCE's on its timeslot or carrier. Direct mode is never held by these gates. The call
  gives the talkgroup (its canonical call's kind, target and source: `tetra_calls_target()`, or the direct mode
  destination) and the encryption class: end-to-
  end and direct mode encryption are never decrypted; air encryption is when `tetra_air_decrypt_voice()` applies a key
  stream. Which key (`tetra_air_key_for_carrier()`, `tetra_air.c`): a TEA1 reduced key of exactly this channel (MCC,
  MNC, location area, colour code, carrier), which runs TEA1 from its register with no TB5; else the network's 80-bit
  key with the algorithm it names, else with the one TEA1..TEA3 the network's signalling named (none while it named two
  or more: a SwMI may support several, EN 300 392-7 clause 6.3.1), from the encryption cipher key TB5 derives with the
  carrier being received (`tetra_calls_carrier()`: the
  followed call's carrier, else the carrier whose frequency is exactly the one the receiver is on,
  `tetra_sm_receiver_freq()`, on the control channel too, which may be a secondary carrier that broadcasts the main
  carrier's SYSINFO), the SYSINFO location area and the BSCH colour code. The encryption sits above the channel coding (EN
  300 392-7 clause 6.1) and the sensitivity-class order is part of that coding (EN 300 395-2 clause 5.2), so the key
  stream applies after `tetra_voice_reorder_full()` / `_half()`, to the speech frame bits in codec order (table 6.4: a
  full slot's frame A takes KSS(0..136) and frame B KSS(137..273), a half slot's frame KSS(216..352)). Only the
  encrypted values (2 and 3) of the encryption mode element are encrypted; 1 is reserved. The speech in progress (the
  speech decoder's state, the speech's integrity and its unplayed samples) belongs to one transmission: a burst of speech
  of another kind, call identifier or target, on another timeslot (each a traffic channel of its own, EN 300 392-2
  clause 9.4.2.2; speech no record names has no other identity), of another transmitting party (the call's talker, which D-TX GRANTED and
  D-TX INTERRUPT name, or the direct mode source; a party not known on either side is no change, as for the canonical
  call), or the first after a TETRA canonical call ended (`tetra_calls_t::ends`: a release, D-TX CEASED, a grant that
  names no talker where one was known, the state machine leaving the call, another call published, the call records
  forgotten), or the first after the heard speech
  stopped for `TETRA_CALL_SPEECH_END_S`, published or not (`calls_tick_heard()`), or the first that comes
  `TETRA_CALL_SPEECH_END_S` or more after the last burst of the speech with a good frame (`tetra_voice_t::last_good_m`),
  in every mode, the followed call's and direct mode's included, whose call goes on across the pause (the D-TX
  CEASED, D-TX GRANTED or DMAC-SYNC between two transmissions may go unheard), or the first after a grant that
  follows D-TX CEASED (`tetra_voice_transmission_started()`: the trail of speech after D-TX CEASED, whose talker is
  no longer known, is the last transmission's), starts over (`voice_bind()` in
  `tetra_voice.c`), so nothing of one transmission plays under the next one's record policy and no bad opening frame is
  concealed with another call's, timeslot's or talker's codec state; the call identifier and the timeslot tell two
  transmissions apart at the second's first burst, which publishes nothing when its CRC fails. The kind tells direct
  mode from trunked mode; downlink or uplink is the session's (`--tetra-uplink`). A D-INFO or D-CALL RESTORE renaming the call keeps
  its speech (`tetra_voice_call_renamed()`).
- Speech decoding. mbelib-neo's TETRA decoder (`mbe_tetraDecodeFrame()`, mbe-neo 2.6+) turns each 137-bit speech frame
  into 240 samples at 8 kHz; a CRC-failed or stolen frame goes to it as a bad frame, which it conceals. `tetra_voice.c`
  makes one decoder (`tetra_voice_t::codec`) at the first frame that plays, resets it (`mbe_tetraDecoderReset()`) when
  the speech in progress starts over and frees it with the TETRA state. A decoder that cannot be made or a decode that
  fails drops that frame with one logged error per session; decoding carries on. Tests swap in a stand-in decoder
  through `tetra_voice_set_codec_for_test()` (`tetra_voice_state.h`).
- Canonical calls. Slot 0, the TETRA synctype as protocol. The followed call publishes from its CRC-passed signalling
  once the carrier is proven and its tune settled, until its transmission ceased: a group call's called group as
  target and its talker as source; an individual call's talker (the calling party while none is named) as source and
  its other party as target (`tetra_calls_target()`: the calling party while the called one talks), as DMR and P25 name
  a unit-to-unit transmission, so the talkgroup policy's private-call audio and record gates judge both parties
  whoever transmits; carrier and frequency, basic service as service options, priority, and its crypto class. Not trunking, the
  heard call publishes from its speech and then its signalling, and speech no record names publishes an anonymous call
  on a proven carrier (a CRC-16 signalling block of it, `tetra_confirm_carrier_proven()`: on the uplink, which has no
  BSCH, its SCH/HU or MAC blocks), one call per timeslot (`speech_anonymous()`: each timeslot is a traffic channel of its own, EN
  300 392-2 clause 9.4.2.2), so when the heard timeslot moves, the speech of the new one ends the last one's anonymous
  call and publishes its own, with its own epoch, media and crypto class. A grant is the SwMI's floor decision, never sent unsolicited (EN 300 392-2 clause 14.5.1.2.1),
  so one that names no talker where the last named one is the next transmission with its D-TX CEASED unheard: the
  last talker's canonical call ends and the next one publishes without a talker. Once published, a call follows its record: whatever a CRC-passed PDU, its allocation, an
  allocation addressed to the call's party or a MAC header changes of it (its parties, basic service, traffic channel or
  encryption) is observed at once (`calls_sync_published()`), on a carrier no block proved yet too, since the call was
  published before and the PDU passed its CRC; only publication itself waits for the proof. A reallocation of the
  followed call to another carrier moves its canonical call there when its tune completes
  (`tetra_calls_followed_moved()`), and not while it is in flight; a grant of another call that took the receiver from
  the followed one ends the call left as a teardown when its tune completes, and the new call publishes once it proves
  itself on its carrier. What changes a carried call's crypto class without
  any PDU of it (a cipher key loaded, the network's KSG learnt, or the timeslot's signalling encryption that classifies
  anonymous speech) reaches it with its next speech (`calls_reclassify()`). Speech marks the media
  (`dsd_call_state_update_media()`). D-TX CEASED and releases end the call
  as terminators, the state machine leaving it as a teardown, the heard speech stopping as a sync loss. Direct mode
  (`tetra_calls_dmo_pdu()`, EN 300 396-3) publishes the call on the channel from DM-SETUP, DM-SETUP PRES, DM-CONNECT
  ACK, DM-OCCUPIED and DM-GSETUP with what its security class leaves clear (DM-2-B hides the destination, DM-2-C every
  identity), ends it at DM-TX CEASED, DM-RELEASE, DM-DISCONNECT and DM-GRELEASE, after `TETRA_CALL_DMO_IDLE_S`
  without a PDU or speech of it, and when the channel's traffic is another transmission's (`dmo_tx_seq`,
  `dmo_call_of_transmission()`): a transmission keeps its source and class, so one of another class is another call
  even where neither names the other's parties (a clear call after an encrypted DM-2-C one, or the other way, and a
  DM-2-C one after a DM-2-A or DM-2-B one, which encrypt the air interface too). A call
  its set-up announced before any transmission began (DM-SETUP PRES, whose calling party's DM-CONNECT ACK starts the
  traffic, clause 6.2; DM-GSETUP) takes the first transmission of its source as its own. The event history names the network (`TETRA_<mcc>_<mnc>_CC_<cc>` or `TETRA_DMO`,
  `dsd_events.c`), the access code is the colour code (`access_code.c`), and `call_state.c` gives TETRA its own call
  family.
- Direct mode protection (`tetra_dmo.c`, `tetra_state_t::dmo_crypt`, `tetra_dmo_call_crypt()`) is that of the traffic
  on the channel, which the DMAC-SYNC carrying a DM-SETUP, DM-CONNECT ACK or DM-OCCUPIED sets (EN 300 396-3 clause
  8.5.3); any other message's security class is its own. There is no direct mode decryption, so only CLEAR speech is
  audible, and protection fails closed: a DMAC-SYNC whose message went unread (its SCH/H failed its CRC or was
  skipped, or DM-2-C hides the message type) may be the message that sets it, so an encrypted SCH/S security class
  seals the traffic (`dmo_apply_unread()`; DM-2-C drops the colour code too, which it derives from the encrypted short
  identity, clause 8.2.4), whatever an earlier clear call left, and a DM-1 one opens nothing. A transmission keeps the
  class its DM-SETUP or DM-CONNECT ACK gave (`dmo_tx_crypt`, and the SCH/S security class itself, `dmo_tx_aie`: DM-2-A,
  DM-2-B and DM-2-C all encrypt the air interface, but are three classes), which its DM-OCCUPIED shall repeat (clause
  8.5.3): one that agrees confirms it, opening what an unread DMAC-SYNC sealed, one that contradicts it leaves the more
  closed of the two (`dmo_apply_setup()`), and a set-up from another source or of another class, like a DM-2-C
  DMAC-SYNC after a transmission of any other class, DM-2-A and DM-2-B included, starts the next transmission
  (`dmo_tx_seq`), so a DM-2-C one, which names no one, never goes on with the last one's source and destination. An
  unread DM-2-A or DM-2-B DMAC-SYNC only seals: its transmission's next DMAC-SYNC, whose addresses those classes leave
  clear, says whose it is. The canonical direct mode
  call takes a protection that no call PDU republishes at once (`tetra_calls_dmo_protection()`). The protection and the
  DM colour code belong to one traffic transmission (clauses 8.2.4, 8.5.3) and end with it
  (`tetra_dmo_transmission_over()`): at DM-TX CEASED, DM-RELEASE or DM-DISCONNECT, in a DMAC-SYNC or on the STCH in a
  DMAC-DATA (one under DM-2-C, whose message type is unread, ends nothing: a DMAC-DATA's security class is its own
  message's), and when the call is idle for `TETRA_CALL_DMO_IDLE_S`, so a next transmission is never descrambled or
  classified by the last one's.
- Notices. D-SDS-DATA becomes a data notice with its text as the message, D-STATUS and MM D-ENABLE / D-DISABLE
  control notices, once the carrier is proven (`tetra_calls_notice()`); direct mode short data becomes a data notice,
  a repeat within 10 s not again: a repeat is the same message (`tetra_dmo_sds_key`: message type, source,
  destination, short data type, user data length, and the whole DM-SDU, its first 64 bits of user data and the digest
  of every element the parser read but an FCS that ends them, `tetra_dmo_sync_h_t::sdu_digest`: over that FCS too the
  CRC would be its residue, the same for every message whose FCS is right), so a pre-coded status and user data of the
  same value, or two long messages that share their first 64 bits, with an FCS or without, are two notices. Its DM-SDU's FCS, when the FCS flag says it has one, is tested first (EN 300 396-3
  clauses 6.3.4 and 9.7.10, annex B: the CRC of `tetra_llc_fcs32()` over the DM-SDU elements before it; the parser
  records the verdict, `tetra_dmo_sync_h_t::fcs_state`): short data whose FCS fails, or whose DM-SDU the block ends in
  before its FCS, is discarded (clause 6.3.2), neither noted nor kept as the last one noted, since the channel CRC does
  not stand in for it. Cell reselection is not noted: it concerns one MS at a time.

Tests: `TETRA_CALLS` (records, attachment, policy, hold, preemption, lockout, renaming, the followed call's in the
first block on its carrier included, identifier reuse, another calling party or its network included, a released,
renamed-over or aged followed identifier, a released, reused or evicted heard identifier, ageing,
canonical calls and notices in trunked and direct mode, direct mode short data whose FCS fails and what repeats it,
an individual call's two parties, speech publication, the receiving carrier (on a secondary carrier's control
channel too), the speech of a channel held with trunking off, what a record change, a MAC header's
encryption and a reallocation do to the published call, a direct mode call idling out, direct mode transmissions of
another security class, an anonymous call per timeslot, the last cell's channels and event labels a BSCH or SYSINFO
of another cell drops, an event label's party and channel, an assignment at its PDU's delivery and in an encrypted
PDU, a label number reused on another channel, a label kept across a channel change, one left by a reallocation and
by its record's ageing, the call trunking turned off lets go of, a channel allocated to another call: blocked or
allowed, taken from the followed call, from a followed record moved elsewhere, from a failing tune's rollback and
from the heard call, by every kind of allocation, and none taken by another timeslot, carrier or direction or by "go
to the control channel"; an allocation, a "go to the MCCH" and an encrypted reallocation judged after a failed tune
of the same block; a tune answered during a burst, which no PDU of it takes and which no label of the carrier left
resolves to the call on the new one; one burst, one state machine; "go to the MCCH" during the call's own tune),
`TETRA_DMO` (the
DM-SDU's FCS and digest, an unread
DMAC-SYNC's protection, a DMAC-DATA that ends the transmission, the security class a transmission keeps),
`TETRA_PUBLICATION` (no trunked network on a direct mode carrier), `TETRA_VOICE_GATE`,
`TETRA_VOICE_PIPELINE` (direct mode with trunking on, the followed timeslot, end-to-end encryption, talkgroups, an
individual call's speech judged by both parties, muted speech that holds no timeslot, the
derived key stream, the speech starting over at call boundaries, another call identifier, another timeslot, another
cell or network, a stop of the heard speech and a pause in the followed or direct mode call's, and going on across a
rename, the crypto class speech gives the published call, frame 18 that plays nothing), `TETRA_BURST_VERDICT` (the modes a BSCH, an SCH/HU, a DSB and a carrier
boundary leave and enter, an unread DMAC-SYNC that seals the traffic, a frame 18 burst not tried as speech, nor one
whose AACH says control or unallocated, the uplink signalling that proves its carrier for publication but admits no
grant), `TETRA_AACH` (the Reed-Muller code and table 21.82 against AACH words received off air),
`DECODE_IQ_TETRA_REAL_AIR_CALL_AACH`, `DECODE_IQ_TETRA_REAL_AIR_GAP_AACH_GATE`, `TETRA_AIR`, `TETRA_CRYPTO` (TB5),
`TETRA_MLE_CMCE`, `CORE_ACCESS_CODE`.

#### State and its boundaries

What each part of the TETRA decoder state belongs to, what sets it, and what forgets it. A boundary the engine sees (a
manual tune, a source or input switch, a scan step, the no-carrier pass) and one the state machine makes (a tune or
return completed, a hand-over) forget the carrier (`tetra_forget_carrier()`); a boundary only the air tells (a BSCH of
another cell or network, a DSB on a trunked carrier, a trunked burst on a direct mode one) is handled where it is
decoded, so the decoder forgets the same whatever moved the receiver.

| State (in `tetra_state_t` unless named) | Belongs to | Set by | Forgotten at |
|---|---|---|---|
| Direct mode (`dmo`, `dmo_comm_type`) | the carrier | a DSB (`tetra_dmo_apply_sync()`) | a CRC-passed BSCH of a trunked system code or uplink SCH/HU (`tetra_dmo_leave()`), a carrier boundary |
| DM colour code (`dmo_dcc_valid`, `dmo_dcc_seed`, `dmo_since_sync`) | one direct mode transmission | the DMAC-SYNC of its DM-SETUP, DM-CONNECT ACK or DM-OCCUPIED, or of a fragmented DM-SDS | its end (`tetra_dmo_transmission_over()`: DM-TX CEASED, DM-RELEASE or DM-DISCONNECT in a DMAC-SYNC or DMAC-DATA, the call idle `TETRA_CALL_DMO_IDLE_S`), DM-2-C, a DMAC-SYNC with no source address, 200 DNBs without a DMAC-SYNC, leaving direct mode, a carrier boundary |
| Direct mode protection (`dmo_crypt`; the set-up's `dmo_tx_crypt`, `dmo_tx_aie`, `dmo_tx_seed`, and the KSG and key number of the DMAC-SYNC that started it, `dmo_tx_ksg`, `dmo_tx_key_number`, which the call transaction keeps, EN 300 396-6 clause 5.2) and the transmission (`dmo_tx_seq`) | one direct mode transmission | the DMAC-SYNC of its DM-SETUP or DM-CONNECT ACK, or of a DM-OCCUPIED when the set-up went unheard; a set-up of another source or security class, and a DM-2-C DMAC-SYNC in a transmission of another class, start the next; a DM-OCCUPIED that contradicts the set-up and an unread DM-2-A or DM-2-B DMAC-SYNC only seal it | as the colour code (UNKNOWN, sealed) |
| Direct mode call (`calls.dmo`) and its canonical call | the channel's call | DM-SETUP, DM-SETUP PRES, DM-CONNECT ACK, DM-OCCUPIED, DM-GSETUP, a DM-2-C DMAC-SYNC | its end PDUs, idling out, leaving direct mode, a carrier boundary (the engine ends the canonical call) |
| Network (`net_known`, `mcc`, `mnc`, `colour`, `lfsr_seed`) | the session; published only on a trunked carrier (`tetra_pub_network()`) | a BSCH, `--tetra-net` | replaced by a BSCH of another identity |
| KSG evidence (`air_ksg`: per network, the KSG numbers its signalling named, `mask`, and apart from them those clear uplink registrations proposed, `requested`) | the session, per network | `mask`: a complete D-OTAR SCK Provide of a trunked mode SCK or GCK Provide, D-LOCATION UPDATE COMMAND or REJECT with cipher control on, a repeat proof; `requested`: a clear U-LOCATION UPDATE DEMAND with cipher control on (`tetra_ul.c`) | a full cache (20 networks) replacing the oldest network no air key is loaded for; never a keyed network's |
| Uplink air encryption (`ul_enc`) | the carrier | a CRC-passed uplink MAC-ACCESS or MAC-DATA, Null PDUs included (`tetra_ul_block()`) | a carrier boundary (`tetra_forget_carrier()`, which clears its published value at once, the rest of the publication waiting for the next burst), another cell (`bsch_forget_cell()`), entering direct mode (`tetra_enter_dmo()`) |
| Tuned frequency (`rx_tuned_hz`), which `tetra_calls_rx_carrier()` reads the carrier being received from without options | the receiver | every burst (`tetra_frame_enter()`) | a carrier boundary |
| Encrypted signalling evidence (`air_evidence`) | the carrier | encrypted TM-SDUs a key could decrypt | `TETRA_AIR_EVIDENCE_S`, eviction, a carrier boundary, another cell or network |
| Cell key epoch (`air_epoch`, the SCK-VN / CCK-id, SCKN, GCK support and parity seen) | the cell | SYSINFO, encrypted MAC headers | moves at another key announced or a parity switch; another cell, location area or network |
| A record's key (`tetra_call_t::air_key`, a key copy) | the call record | an allocation in signalling it decrypted | the record ending, a clear allocation; applies only under its epoch and parity, the key still loaded |
| Cell (SYSINFO: main carrier, band, offset, location area, `sysinfo_known`, security `sec`/`sec_valid` and `aie_known`/`aie_available`; `trunk_cc_freq` it set; `enc_mode`, `frames_resource`) | the cell; its control channel and security published only on a trunked carrier | SYSINFO (the security from one with the extended services broadcast, the AIE bit from one with its D-MLE-SYSINFO), addressed MAC-RESOURCEs | a BSCH of another cell or network (`bsch_forget_cell()`), a SYSINFO of another main carrier or location area (`tetra_bsch_sysinfo_cell()`) |
| TDMA clock (`tdma_valid`, `tn`, `fn`, `mn`, `hyperframe_valid`); a burst it places in frame 18 is no speech | the carrier | a CRC-passed BSCH, run by every downlink burst | a DSB, another cell, a gap longer than a multiframe, a carrier boundary |
| Partial TM-SDUs (`mac_frag`) | a timeslot of one cell's control channel | a first fragment on a timeslot the clock places | N.203, a failed block of the timeslot, another cell, entering direct mode, a carrier boundary |
| Call records, the follow of their identifiers, SDS reassembly | the network (records: and location area) | D-SETUP; reassembly parts | release, reuse, ageing, eviction; a BSCH of another network; a SYSINFO of another location area (records) |
| A record's channel allocation and event label (`alloc`, `alloc_m`, `event_label`, `label_chan`, `usage_marker`) | the cell (a label: one channel of it, the allocated one or the one its assignment came on; Calls, "Event labels") | an allocation (`call_attach()`), an SSI + event label assignment at its PDU's delivery, a usage marker assignment with the downlink allocation of its own PDU; a label's channel also a KEEP with an allocation | a "go to the MCCH" allocation, the record ending, another cell (`tetra_calls_forget_cell()`); the usage marker also any other allocation of the downlink; the allocation also another call's allocation of a timeslot of its downlink (`calls_take_channel()`: one call per traffic channel, EN 300 392-2 clauses 23.5.5, 23.5.6.1.1); a label also its assignment to another party on a channel that meets its own, its withdrawal (label 0), an assignment or KEEP to a channel the decoder cannot tell, and, assigned without an allocation, a carrier boundary or retune (it applies there no more) |
| D-MM STATUS shown (`mm_status_valid`, `mm_status_code`) | the network; published only on a trunked carrier | D-MM STATUS | a BSCH of another network |
| Timeslot speech context (`calls.slot_enc`, the heard timeslot) and the heard or anonymous canonical call | the carrier (the anonymous call: one timeslot of it) | addressed MAC-RESOURCEs; heard speech | the heard speech stopping, another cell, entering direct mode, a carrier boundary, another network; the anonymous call also speech of another timeslot or of a call record |
| Carrier evidence (`confirm`: `carrier_blocks` for publication, `signalling_blocks`, downlink only, for grants) | the carrier | CRC-passed blocks (an uplink SCH/HU or MAC block counts for publication only) | a carrier boundary and a trunking retune (`tetra_confirm_reset()`); the followed call's proof also at another cell, entering direct mode and every change of follow |
| The followed call's receive permission and AACH counts (`confirm.rx_perm`, `rx_run`, `rx_adverse`, `rx_chan_bad`, `rx_qual_start`) | the followed call on the carrier | its U-plane switching, N.213 qualifying ACCESS-ASSIGN PDUs | its U-plane switching off, N.212, T.212, with its proof (carrier boundary, follow change, retirement); a failed tune puts the permission back, the counts start over |
| Follow (`sm`) | the session | a grant | release, hangtime, grant timeout, abandon at a carrier boundary, an external return, its channel allocated to a call it does not follow (it moves to one it does); a decode-mode change disowns its tune in flight; trunking off lets go of its call but keeps the channel and asks for no return |
| Speech in progress (`voice`) | one transmission | its first burst | speech of another kind, call identifier, timeslot, target or talker; a canonical call ended; a grant after D-TX CEASED; the heard speech stopping; a pause of `TETRA_CALL_SPEECH_END_S` in it, in every mode; another cell; a carrier boundary |
| Polarity (`dsd_state::tetra_sync.polarity`) | the receiver chain | a synchronisation burst's training, a CRC-passed SCH/HU | the next of them |

What each decoded event does:

| Event | Leaves | Enters / sets |
|---|---|---|
| CRC-passed BSCH, same cell | direct mode | the TDMA clock |
| CRC-passed BSCH, another cell | direct mode; the cell, the clock, the partial TM-SDUs, the records' channel allocations and event labels, the timeslot speech context, the speech in progress, the followed call's proof | the new cell's identity and clock |
| CRC-passed BSCH, another network | as another cell, and the call records with the follow of their identifiers, the SDS reassembly, the D-MM STATUS shown | the new network |
| BSCH that fails | nothing | runs the clock to its slot |
| CRC-passed SCH/HU (uplink) | direct mode | the carrier's polarity, its evidence for publication |
| DSB (CRC-passed SCH/S, DMO system code) | the trunked carrier's TM-SDUs, timeslot speech context and followed call's proof (`tetra_enter_dmo()`), the TDMA clock | direct mode; with its SCH/H, the colour code, protection and call its message sets; without it, an encrypted SCH/S seals the traffic; a presence signal or a SYNC PDU type not read (managed direct mode, reserved types) only direct mode |
| DMAC-DATA (CRC-passed DNB block) | at DM-TX CEASED, DM-RELEASE, DM-DISCONNECT the transmission's colour code and protection | the call it names; its own security class is its message's only |
| SYSINFO of another main carrier (a cell sharing the colour code) | as a BSCH of another cell | the new cell's SYSINFO |
| SYSINFO of another location area | as a BSCH of another cell, and the call records | the new area |
| Carrier boundary (`tetra_forget_carrier()`) | the carrier evidence, the partial TM-SDUs, the clock, the speech in progress, the timeslot speech context, direct mode with its colour code, protection and call, and with the evidence's epoch the event labels assigned on the carrier | trunked mode, block 1 behind AACH half 1 |
| Decode-mode change off TETRA (committed) | the whole TETRA state with its tune in flight disowned, the frame sync capture and the publication (`dsd_decode_mode_leave_tetra()`) | nothing |

A DSB whose SCH/S fails is not told from noise on its training sequence and changes nothing; sealing at every such
burst would mute clear direct mode calls at every false sync. The uplink has no TDMA clock, so its speech has no
timeslot: one transmission's speech ends where the heard speech stops (`TETRA_CALL_SPEECH_END_S`). Direct mode is
decoded as one channel: in frequency efficient mode, two calls on channels A and B of one carrier (EN 300 396-2 clause
3.1; the SCH/S channel usage), their DSBs take the channel from each other, one colour code and protection at a time.

Build files: `src/protocol/CMakeLists.txt` and per‑protocol `src/protocol/<name>/CMakeLists.txt`

## Third‑Party

- Paths:
  - `src/third_party/ezpwd` — Target: `dsd-neo_ezpwd` (INTERFACE; headers included via `src/third_party` path)
  - `cmake/Pffft.cmake` — Target: `dsd-neo_pffft` (STATIC; pinned fork sources for spectrum/diagnostics; accessed only through `src/io/radio/rtl_fft_cache.h`)
  - `cmake/TetraCrypto.cmake` — Target: `dsd-neo_tetra_crypto` (STATIC; pinned TETRA_crypto TEA1/2/3 keystream generators, linked privately into `dsd-neo_crypto`; accessed only through `src/crypto/tetra_tea.c`)
- Build files: `src/third_party/CMakeLists.txt` and subdirectory `CMakeLists.txt` files

## UI

- Qt Signal search uses `SignalSearchModel`, `SignalSearchScreen.qml` and
  `SignalSearchRangeEditor.qml`, sharing Theme/Plex controls, source startup and the
  save-found-system wizard. The range editor's discard fingerprint and Save read the
  Airspy gain fields' typing before it commits (`AirspyControls.pendingText()`,
  `pendingSettings()`; the fields commit on `editingFinished`), and opening it resets them
  (`resetText()`). The system wizard and the scan-list editor do the same: their discard
  fingerprints include the Airspy fields' pending text, every load resets them, and the
  wizard saves the pending settings. While a session runs the screen reads status line, Now panel,
  five recent detections (Show more adds 25), then the ranges behind an expandable
  `DisclosureRow` (`expandable`/`expanded`; forced open while paused or idle; a manual
  expansion and the history limit reset on close and on a new session). The Now panel's
  height depends on width and font scale only: each line is a fixed-height `Text` sized
  from `FontMetrics`, and its action grid takes three equal columns when the widest label
  any cell can show fits, else two when every word of every label fits half a row, else
  one, with as many label lines as the longest label wraps to at the cell width
  (`wrappedLines()`), for any cell count. The status line keeps `statusLines` lines (two,
  or three where the widest pass, bounds and channel count would not fit two at this width
  and scale) and shortens only the range name, so the pass and channel count always show
  whole. Its sixth cell, More, opens the
  detection menu (`tst_signal_search.qml`). `hitMap()` passes the hit's checked `site`,
  `slot` and `crypt` through; the panel's two-line detail (`detailText()`) reads site,
  access, tone, `Slot N`, target, source, then crypt, eliding within the two lines with
  the whole text as its accessible name and description, and a history row shows site and
  crypt under its verdict. `keepValues()` makes every space but the one after a `·`
  non-breaking, so wrapped details break only between values. Its
  liveness cue marks each new `liveSeq`. Each Recent detections row has an `IconButton`
  opening one `ActionMenu` (`searchDetectionMenu`, acting on the snapshot it opened with):
  Explore here, Start for each distinct saved system its `matches[].systemUid` resolves to
  (an avoided site's row disabled with its reason), Save as system, Avoid this session
  while a search owns the tuner (`SignalSearchModel::avoidFrequency()`), and Avoid
  permanently or Remove permanent avoid (`avoidPermanently()`: through the decoder thread
  while a search runs, the store directly when none does). The verdicts arrive as the
  status message or the model's error. That message (`searchMessage`, an `AlertMessage`) is
  reparented by `sessionActive`: under the Now panel while a session runs, so it never moves
  the panel, and above the setup while idle (`test_message_never_moves_the_now_panel`).
  Leaving for another session is one Main operation,
  `leaveSearchFor(request)` (`{kind: "explore", system, remember: false}` or `{kind:
  "saved", uid}`): it abandons the search's start and runs the request through
  `pendingRestart` (marked `leaveSearch`) once the host is Idle, stopping it from
  Starting, Running or Failed. Until then `leavingSearch` keeps the search screen the root
  (its `leaving` state reads "Stopping the search…", disables its controls and holds focus
  so Escape/Back reach Main). Back cancels the handoff (`requestBack()` clears it and
  returns), so a stop that hangs past the engine's restore timeouts cannot lock the
  screen; the site chooser's any-input cancel is ignored while it runs, and
  `startSystem()`/`restartSite()` refuse by state before cancelling, so a USB
  attachment's start cannot drop it. `leaveSearchFor()` pins `signalSearchOpen`
  true, as Stop does, so a screen reopened after Activity recreation (bound to `status.owned`, which
  the search clears before the decoder is idle) stays through the stop and after a Back cancel.
  `finishPendingRestart()` closes the screen just
  before starting the request, and a stop ending in Failed closes it and clears the flag.
  A saved request is checked (`savedStartRefusal()`) before the stop, the refusal shown in
  the screen's `localFailure`, and again in `finishPendingRestart()`, which shows the
  refusal (an avoided site no longer declines silently) and records the system as
  `attemptedSource` so Home's Retry asks again and Edit opens it. Explore here
  (`exploreHereSystem()`) is `systemForHit()` renamed Exploring, untrunked, on the
  `effectiveSource()` kind through `onAttachedReceiver()`/`onReceiver()` (an Airspy map only
  stays on an Airspy; a switch to an Airspy drops the RTL `ppm`, `biasTee` and gain); `startWithMap(sys,
  row, scan, options)` records `options.remember` in `attemptedSource` and
  `pendingStartRemember` (through a USB wait), and only a remembered explore session
  writes `prefs.exploreFreqMhz` when it ends. `retrySource()` carries the attempted system:
  a failed search is retried by reopening the screen and running its `startSearch()` (arm,
  then start), and edited by reopening it (`openSignalSearchOn()`, which resolves the
  `effectiveSource()` kind). A failed Explore here is retried on `onAttachedReceiver()` (the
  attached kind; an Airspy map, serial included, only on an Airspy) and edited through
  `openExploreSetupOn()`: Explore setup on that attempt's resolved receiver and frequency,
  held in `exploreSetupBase`, whose start keeps the rest of the map and `remember: false`
  and writes no `explore/*` prefs (`tst_signal_search_handoff.qml`). Native status
  is copied on a 250 ms timer; history and the avoid lists only when their store revisions
  move. While a search session is active, Escape and Back from any control go Back, which
  delegates to Main's session policy; a search a coordinator owns (`status.owned`) is a
  navigation root so its controls remain accessible after Activity recreation. Range edits
  apply on paused restart. Home's `onSignalSearch` seeds the source fields
  (`Main.seedSignalSearch()`) only when it opens the screen: `AppPrefs`
  `search/sourceType`/`search/host`/`search/port` (written by
  **Start search** before the start, portable in the backup), else the `explore/*` answer,
  else `usb`. Every Explore and search entry (Home's direct Explore start, Explore setup's
  `reset()` before it captures its dirty baseline, the unconfigured first run, failure edit,
  search seeding, and the Home Explore card's label) resolves the remembered kind through
  `Main.effectiveSource()` over `Util.effectiveSource()`, the one place the rule lives:
  rtl_tcp is kept; a remembered USB kind not attached while exactly the other kind is
  (`DecoderHost::attachedDeviceSource`: `usb`, `airspy`, `both` or empty; empty on desktop)
  becomes the attached kind; `both` or nothing keeps it. Android polls
  `UsbSourceManager.attachedSource()` (permission-free `UsbManager.deviceList`, classified
  by the pure `UsbReceiverKind`, whose id table `isKnown()`/`sourceOf()` share, so hubs and
  other devices never count) with the USB record. A screen
  reopened over an owned search never had them seeded: while an owned search runs, its
  `sourceType`/`host`/`port` follow `MetricsModel::radioSource`, the receiver in the
  decoder's options snapshot named as the Qt start path names it (`usb`, `airspy`, or
  `rtltcp` with the server the engine parsed; empty for audio, SoapySDR and I/Q replay
  input). Stop from the screen keeps it open, reattached or opened from Home, so a restart
  runs on that receiver. Leaving the search abandons the start it asked for
  (`abandonSignalSearchStart()` in Main): closing the screen, a Start it refuses
  (`startRefused`) and Stop cancel the model's arm and drop a USB-access start the search
  left pending (`pendingStart.signalSearch`), so a later grant cannot start a plain decoder
  session behind Home; a start pending from Home keeps waiting, and when its grant lands
  while the search screen is open the screen closes, so that session is not taken for a
  search (`tst_main_session_tools.qml`). A refused `arm()` leaves
  nothing armed (`tst_main_session_tools.qml`, `tst_signal_search.qml`,
  `UI_QT_METRICS_MODEL`, `UI_QT_SIGNAL_SEARCH`).
  The armed start goes in when the decoder's command admission takes it, not merely on
  Running: Android's engine thread raises `g_running` a moment before its
  `dsd_app_frontend_runtime_start()` opens admission, and a session winding down closes
  admission before Running drops. A refused submission queues nothing, so the model keeps the
  arm and submits it again on `DecoderHost::sessionInitialized()` (which follows the
  admission edge, and can arrive while the host still reads Starting) and on each 250 ms
  tick while Running; an accepted one ends the arm, so the search starts once. An arm
  belongs to one session, the one up when it was made (a paused search's Resume) or else
  the next to come up (a USB grant can hold that start while the host stays Idle), and
  that session's Stopping, Failed or return to Idle ends it, so it never starts a search
  in a later, ordinary session (`UI_QT_SIGNAL_SEARCH`). `arm()` keeps a paused search's
  session ID (its hits keep numbering on) only when the current native status, not the
  copy a tick old, shows that search paused and holding the tuner in a Running decoder
  session; a start after a Stop is always a new session, so its hits never overwrite the
  old session's history rows (`UI_QT_SIGNAL_SEARCH`).
  Main binds the screen's `failure` to its own failure text, and the screen never assigns
  it: an assignment would drop the binding, and a connection, USB-access or configuration
  failure after Start or Resume would never reach the screen that covers Home's banner.
  The screen's own refusals (`arm()`, a refused handoff) live in `localFailure` until the
  next attempt or Back, shown first and never hiding what follows it: the host's failure,
  else the search's message or the model's error (`tst_signal_search.qml`,
  `tst_main_session_tools.qml`, `tst_signal_search_handoff.qml`).
  `KnownFrequencyIndex` caches saved frequencies and referenced CSVs, rebuilding on model
  or file changes; Signal search, Spectrum and Live Exploring look up labels without
  reading files during rendering. Exact matches win, nearest matches use half the smaller
  applicable channel width/spacing, and ties remain ambiguous. A channel-map row that names
  no mode or bandwidth of its own takes the width/spacing of the saved system or target row
  that references the map, so an NXDN48 system's plain `Channel,Frequency` map keeps its
  6.25 kHz channels (`UI_QT_SIGNAL_SEARCH`). NXDN48, dPMR and D-STAR are the 6.25 kHz modes,
  as the receiver's channel filter has them; decode flags compare exactly (`-fm` is dPMR,
  `-fM` AM) and mode or type names without case. Only the width option of the channel's own
  analog kind applies (`--am-bandwidth-hz` for AM, `--nfm-bandwidth-hz` for NFM, either as
  `--name value` or `--name=value`), so an NFM system that also sets an AM width keeps its
  NFM channels' width, and its mode-less map rows inherit that. A saved system's mode is its decode
  flag, as the session builder and the rest of the app read it (`decode_flag_names_am()`); a
  decoder chosen only through its extra arguments is not read, a documented limit, so such a
  system is labeled as its decode flag's channel. A scan-list entry's mode is its protocol. The Monitor and Spectrum labels name the
  tuned channel through `tunedNames()`, at the width the receiver filters
  (`MetricsModel::channelBandwidthHz`, 12.5 kHz when the input publishes none). Saving uses an immutable
  hit snapshot, including its original source settings and unresolved identity.
  `SignalSearchModel::systemForHit()` shapes the captured receiver settings as saved-system
  fields, each source only its own: an Airspy's native `airspy` map, or an RTL's `ppm`
  text and explicit `biasTee` (0 or 1, never the follow-the-app -1). The detection's tuner
  gain (`gain` dB, `gainAuto`) becomes the whole-dB `gainDb` a saved session starts with,
  the `-i` spec's gain field, where 0 is the tuner's AGC: AGC stays 0, and a manual gain
  rounds to the nearest whole dB from 1 to 49, so a manual 0–1.4 dB detection stays manual
  at 1 dB instead of becoming AGC. An Airspy's `gainDb` is -1, its gains being in its map.
  The start path has no tenth-dB or manual-0 gain; the tuner runs its nearest step.
  `WizardScreen.openForFound()` copies them into the draft only for a detection; the
  Spectrum's save passes none and keeps the app defaults its session ran on
  (`tst_wizard_search_save.qml`, `UI_QT_SIGNAL_SEARCH`).

- Path: `src/ui`
- Targets: `dsd-neo_ui_terminal` (option `DSD_ENABLE_TERMINAL_UI`, default ON), `dsd-neo_ui_qt`
  (option `DSD_ENABLE_QT_UI`, default OFF)
- Legal resources: `DecoderHost::licenseNotices()` reads the retained project and
  dependency notices, including `legal/status.md` and the selected mbelib installation's
  unresolved disclosures. `cmake/MbelibNotices.cmake` requires those dependency records;
  the root CMake `legal` install component and `cmake/stage_android_package.cmake` carry
  them into portable packages and Android assets. `tools/legal_inventory.py` checks
  retained notice hashes and supplements release SBOMs without asserting legal clearance.
- Responsibilities:
  - Terminal frontend implementation (panels, logging, protocol displays, visualizers)
  - Status rows that never wrap (issue #669): `ui_row.{h,c}` builds a row from atomic fields and draws it whole, or cut
    after the last whole field that fits (cols - 1 - start column: the last column wraps) with `…` (`...` without UTF-8
    or under `DSD_FORCE_ASCII`), trailing blanks dropped, the caller's attributes put back before the marker and the one
    newline. Widths follow the curses library's model (`wcwidth()` on ncurses, one cell per BMP code point on
    PDCurses); controls, invalid UTF-8 and undrawable code points print as `?`. The Input Output section's live rows go
    through it — each input's source row (receiver readings at fixed widths, `FRQ:` last) and channel row (`SQL:` and
    its reading first, then `Analog:`/`Passband:`), the Airspy identity row, Auto PPM, input level, outputs, M17
    encoder, Scan Mode, Trunk Scan, Scan Timing, and every DSP panel line — with readings through `dsd_number_text()`
    at fixed widths and status words in rooms that stay; configuration-only rows (toggles, keys, paths, External RTL
    Tuning, M17 UDP, Reverse Mute) keep wrapping. Tests: `UI_ROW` (real `newterm`), `UI_NCURSES_PRINTER_HELPERS`
    (+ `_RADIO`), `UI_DSP_DISPLAY_RENDER`.
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
  - Qt/Android Radio defaults: `qml/RadioSettingsPersistence.qml` associates explicit squelch, modulation and
    decoder requests with the active saved-system UID. It saves only matching configured snapshots through
    `SavedSystemsModel::updateRadioSettings()`. Changing the decoder clears the previous manual modulation override,
    as the live command does; reselecting a named preset keeps it. Auto resumes modulation hunting instead of locking
    its initial reading. A later modulation request waits for the pending decoder choice to be confirmed. Pending
    requests survive closing Radio but end with the session; Explore, signal search, scan-list sessions and
    per-channel edits do not write a saved system. The typed `radioSettings` map is validated by `radio_settings.h`
    and appended after advanced arguments in `session_args.cpp`, so a confirmed choice wins on restart.
    The saved decoder flag also updates wizard/list metadata; a later wizard preset replaces its saved live
    decoder/modulation override. Manual scan lists inherit saved squelch and modulation (an entry's explicit
    modulation wins). `UI_QT_PERSISTENCE`, `UI_QT_SESSION_ARGS`, `UI_QT_SCAN_LIST_TARGETS` and the Radio persistence
    QML test cover disk reload, restart, validation and temporary scopes.
  - Analog channel width (issue #525): the Input menu's `input.nfm_bw` row, right after `input.sql` (`NFM bandwidth...
    [12.5 kHz]`, or `[default]`, on a radio input while the configured -fA preset runs FM or an explicit width is set
    under another preset, `is_nfm_width_editable()`), prompts for Hz and submits `DSD_APP_CMD_NFM_BANDWIDTH_SET` as
    typed; the RTL-SDR submenu's DSP rate row reads `DSP bandwidth...`, and the rtl_tcp adaptive buffering toggle lives
    in `Auto-PPM & rtl_tcp`. The input's channel row prints `Analog: NFM 16 kHz (default);` after the squelch
    (`(DSP-limited)` when the rate bounds the channel, `(row; default 16 kHz)` while an nfm scan row sets its own width,
    issue #526, `Analog: AM 8.333 kHz (row; default 6 kHz)` for an am row's, and under an analog row on a digital
    session too). The row's label and prompt read the configured width, the one the command edits. Both follow
    app-control's analog width view (above), and so does Qt:
    `MetricsModel::analogBandwidthHz`/`analogBandwidthDspLimited`/`analogBandwidthReading` (0 and
    `not used on PCM input` on PCM input without a rigctl peer; on a peer session the passband the peer is asked for),
    `analogBandwidthMaxHz` is the widest width the running stream's demod rate filters (with none running, the rate an
    RTL-SDR or rtl_tcp input's DSP bandwidth sets), and `analogBandwidthConfiguredHz` is the value
    `qml/RadioSheet.qml`'s NFM width stepper (`radioAnalogBandwidth`, presets in `Util.NFM_WIDTHS_HZ`, stepping from the
    width in force when the default is set and skipping presets above the max) edits through
    `CommandBridge::setNfmBandwidthHz()`; `radioAnalogBandwidthDefault` sends 0 to return an explicit width to the
    default, and the controls are disabled on PCM input with the reason shown (`radioAnalogBandwidthNote`, which on a
    rigctl peer session points to the monitor's Passband row instead, issue #621). Under another preset the section
    stays on a radio input while an explicit width is set (`analogWidthOffered`), reading the setting, so a width that
    blocks a switch to NFM can be narrowed first. An nfm or am scan row on air (`analogBandwidthRowActive`, issue #526)
    keeps the section, the width in force and the stepper on any preset (`analogWidthInForce`); a row's own width
    (`analogBandwidthRowOverride`) reads first with a `row` badge and the configured default beside it
    (`radioAnalogBandwidthRowNote`, as `radioSquelchRowNote` does for a row squelch), the value is read aloud as the
    view's full reading, and the stepper steps the configured default of the row's kind (from its unset default when it
    is unset, 16 kHz for an nfm row and 6 kHz for an am row, `analogDefaultWidth`; never from the row's width). The
    `NFM` decode chip (`-fA`) sits in `Util.DECODE_MODES`, so the setup wizard offers it too (it suggests no trunking).
    Tests: `UI_MENU_TREE_AUDIT`, `UI_MENU_ACTIONS`, `UI_MENU_LABELS`, `UI_NCURSES_PRINTER_HELPERS`,
    `UI_QT_METRICS_MODEL`, `UI_QT_SESSION_ARGS`, `UI_QT_QML_CALL_LISTS` (`tst_radio_analog.qml`,
    `tst_wizard_decode_chip.qml`).
  - AM (issue #524): the decoder picker lists AM after Analog; on an input that is not an I/Q radio (the snapshot's
    input type) its row reads `AM (needs an I/Q radio input)` and choosing it only repeats the reason in the status
    line. Input has `input.am_bw` right after `input.nfm_bw` (`AM bandwidth... [default]`, `lbl_input_am_bw()`,
    `is_am_width_editable()`: the configured AM preset on a radio input, or an explicit AM width under another preset,
    NFM included, as the NFM row stays under AM for an explicit NFM width, or AM's default where the running stream's
    rate cannot filter it but filters a narrower AM width; both rows share one predicate,
    `dsd_app_analog_width_offered()` over the analog width view with the frontend metrics), whose prompt hands the value as typed to
    `DSD_APP_CMD_AM_BANDWIDTH_SET`. The status line's `Analog:` field reads `Analog: AM 6 kHz (default);` under AM,
    from the same view. Tests: `UI_MENU_ACTIONS`, `UI_MENU_LABELS`, `UI_MENU_TREE_AUDIT`, `UI_NCURSES_PRINTER_HELPERS`.
  - Rigctl passband (issue #621): the two width rows sit in Input (`INPUT_MENU_ITEMS`) outside any `#ifdef USE_RADIO`,
    since the RTL-SDR submenu that held them is hidden on audio input; their predicates (`is_nfm_width_editable()`,
    `is_am_width_editable()`, over `dsd_app_analog_width_offered()`), labels (`lbl_input_nfm_bw()`, `lbl_input_am_bw()`,
    sharing `lbl_input_width()`) and actions (`act_set_nfm_bw()`, `act_set_am_bw()`) are in every build, and the RTL-SDR
    submenu is down to 12 rows. On audio input with a rigctl peer (the view's `peer_passband`) the rows edit the
    passband the peer is asked for: the labels read `NFM passband... [-B 12.5 kHz]` (the setting as
    `dsd_app_analog_width_setting_text()` spells it: `[peer's own]` without `-B`, `[12.5 kHz]` for a width) and `AM
    passband... [default]`. The default prompts, the scope chooser and its this-channel prompt take their titles from
    one table, `k_width_titles[kind][peer]` in `menu_actions.c` (`width_titles_of()`): `NFM passband`, `NFM passband Hz
    on this channel (8000..25000)`, `AM passband Hz (5000..20000; 0 = default 6000)`, and for the configured NFM
    passband `WIDTH_NFM_PASSBAND_PROMPT_FMT` filled with what an unset width asks for (`NFM passband Hz (8000..25000; 0
    = -B 12.5 kHz)`). The chooser's title is decided from the snapshot pair it captures, and `scope_capture()` keeps the
    passband the row ran (`peer_width_hz`, for the chooser's kind only, so an AM passband never opens an NFM prompt), so
    `scope_open_width()` opens on the row's own width, else the width in force, else that passband (`-B` included), else
    the kind's default. The audio inputs' channel rows (Pulse, TCP, UDP, WAV, stdin) print
    `ui_row_analog_channel_field()` after the SQL field, ` Passband: NFM 12.5 kHz (-B);` on a peer session whenever the
    view has a reading (a width offered) and nothing without one, fetching the frontend metrics only on a radio input;
    `ui_row_rigctl_field()` prints `RIG: <rigctlhostname>:<rigctlportno>;` last on the Pulse, TCP and UDP source rows
    (the Pulse and TCP lines printed the TCP audio host, which a menu reconnect of rigctl or of TCP audio sets apart). Tests: `UI_MENU_TREE_AUDIT` and
    `UI_MENU_TREE_AUDIT_NO_RADIO` (the two rows right after `input.sql`, and no width row anywhere else),
    `UI_MENU_ACTIONS`, `UI_MENU_LABELS` (built without `USE_RADIO`: the width label goldens, radio and peer),
    `UI_NCURSES_PRINTER_HELPERS`.
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

- Listen creation actions (issue #709): `qml/HomeScreen.qml` places Add a system and Add a scan list after the status
  cards and before either saved collection, inside the same modal-guarded scroll body. Two equal columns fit only
  when both can hold the wider button's complete label plus padding (`DashedActionButton.implicitWidth`); otherwise
  they stack in focus order, keeping the source hint beneath Add a system. The available content width, including
  the supporting pane, and the current font determine reflow. Empty collection headings are hidden. Tests:
  `UI_QT_QML_CALL_LISTS` (`tst_home_add_actions.qml`, plus the existing Home and Back/modal cases).
- Receiver status, receiver notice and Reconnect (issue #647). Every rule here holds only where the host brokers USB
  access (`DecoderHost::localDeviceBrokered`, Android); the desktop opens its receiver itself and keeps its Listen
  layout, Onboarding wording and Monitor geometry. A desktop RTL-SDR eligible for stall recovery is reset and reopened
  once per stall before its session would end (IO, "USB stall recovery"); a brokered receiver is opened from a USB
  descriptor and never recovered in place (stall recovery is off by design, logged at info level), so a stall ends its
  session as described here.
  - `qml/DeviceStatus.qml` (a non-visual `QtObject`, in `qmldir` and the `qt_add_resources` list) is the one reading
    of the brokered receiver that Listen, Onboarding and the Monitor share, so one receiver is never named or worded two
    ways. `kind`: the connected source when ready; else the receiver of the session that just ended
    (`receiverHealth.source`, so a receiver lost with no request on record is still named); else the requested one
    (`localDeviceRequestedSource`, which Main overrides with the source a start waits on); else the only kind on the bus
    (`attachedDeviceSource`); else none, named "receiver". The requested kind outranks the bus: after an RTL-SDR
    request with only an Airspy plugged in the pill reads `NO RTL-SDR` and Retry answers "No RTL-SDR attached" until
    an Airspy system is started. `state`, first match: `lost`/`fault` (`receiverHealth` lost
    or failed for this kind), `failed` (failure kind busy, open failed or permission), `ready`, `pending`
    (`localDeviceRequestPending`), `missing` (detached or not found, or the kind not on the bus, even with the other
    kind attached), `available`, `absent`. Pill copy, mono caps: `RTL-SDR READY`, `CONNECT RTL-SDR` (`CONNECT
    RECEIVER` with no kind), `RTL-SDR WAITING`, `NO RTL-SDR` (`NO RECEIVER`), `RTL-SDR NO ACCESS` (permission),
    `RTL-SDR ERROR`, `RTL-SDR LOST` (lost and fault alike), `AIRSPY` for an Airspy; the dot is cyan when ready,
    `Theme.alert` for failed, fault and lost, subdued otherwise. The action is Retry for failed, fault, lost and
    missing, Connect for available and absent, and none while ready or pending (Connect returns once the request
    expires); it only asks for access (`requestLocalDeviceAccess()`), never starts, and is offered only with no session
    active or in transition (`canAct`). It also gives the accessible name, the detail line (the host record's text where
    it has one; a receiver on the bus reads "plugged in. Tap Connect", not "Plug in") and Onboarding's card title
    (`headline`, e.g. "Airspy detected"), so Onboarding names the receiver it found.
  - Listen (`qml/HomeScreen.qml`): the title row is a fixed `listenHeader` above the scroll body, on the content
    column's x and width (`columnX`/`columnWidth`; the supporting pane is untouched; safe areas come from the parent),
    so the pill (`dongleStatusPill`, label `dongleStatusLabel`) never scrolls away. The body is clipped 6 px under the
    header, and a 1 px `Theme.divider` shows at that edge only while the body is scrolled. Title and pill share a row
    while `title.implicitWidth + gap + pill.implicitWidth` fits the column; otherwise the pill drops under the title,
    left-aligned and at most the column wide, its label wrapping at spaces (never elided) and its height following the
    label (at least `max(48, 30 * fontScale)`). Header and details-panel heights are keyed on the host, not on
    `visible`, so hiding Home on another tab keeps its scroll range and position. One guarded `activate()` serves tap,
    Return/Enter/Space and `Accessible.onPressAction`, under `canAct`, `Navigation.allows()` and no open management
    sheet; the role is Button while actionable, else StaticText, and the pill stays a tab stop while shown, so focus
    stays put when Connect turns into WAITING. The pill stays up whatever source a failure on screen is about;
    `localDeviceRelevant` (`usb` and `airspy`) gates only the details panel (`dongleDetailsPanel`), which gives way to a
    failure card. `revealFailure()` calls `homeScreen.revealTop()`, so a failure card or the panel is in view when a
    session ends; desktop scroll restoration is unchanged.
  - Receiver notice (Main.qml): when `receiverHealth` reads `lost` or `failed` and no session is active,
    `receiverNotice()` words the ending as Home's failure, ranked after a start error and live USB-access text and
    before the host's failure: "RTL-SDR was unplugged, so <name> stopped. Plug it back in, then tap Reconnect." or
    "RTL-SDR stopped sending samples, so <name> stopped. Check its USB connection, then tap Reconnect.", naming the
    attempt's system, scan list, Explore or Signal search. The `FailureCard` (`homeFailureCard`) button reads
    `retryText` "Reconnect" while the notice shows (`receiverNoticeShown`), and the details panel is hidden, so two
    Retry actions never compete. Reconnect is `retrySource()`: the usual USB gate (a permission prompt if needed), then
    the same session. After an Activity recreation the UI has no verified link from the service's run to a saved
    system or list (`attemptedSource` is empty), so Reconnect only asks for access to the lost kind, starts nothing,
    and the copy ends "Plug it back in, tap Reconnect, then start it again." (a stream failure: "Check its USB
    connection, tap Reconnect, then start it again."). With Signal search open, its screen says "… plug it back in,
    then start the search again." (or "Check its USB connection, then start the search again."). Dismiss clears the
    notice and any restart queued behind a Reconnect or Retry (`cancelPendingRestart()`); Back does not cancel a
    pending Reconnect (Android's permission dialog takes Back as a denial). While a start waits on USB access the live
    access text ("USB permission denied") outranks the input-failure texts. During a session the notice never shows;
    the Monitor carries the loss (Monitor receiver pill below).
  - Auto-start on attachment (Settings "Start when a dongle is attached", "Resume the last USB-receiver system or scan
    list"): saved systems and scan lists on `usb` or `airspy` are eligible, and an attachment starts one only when the
    kind of the receiver that attached equals the target's source (`autoStartKindMatches()` in
    `auto_start_attachment.h`; `DecoderHost::localDeviceAttached` carries `"<kind>:<device name>"`, read by
    `attachedReceiverKind()` in `qt_ui.cpp`'s `wire_attachment()` and matched by `UiController::requestAutoStart()`),
    never because of what else is on
    the bus; a suppressed attachment is still consumed. An unanswered permission request
    (`localDeviceRequestPending`) joins `autoStartBlocked`; its expiry lifts the block without retrying the old
    attachment.
  - Tests: `UI_QT_QML_DEVICE_STATUS` (`tst_device_status.qml`, every state), `tst_home_dongle_status.qml` (pinned
    header, reflow at 320/360/411 px by 1.0/1.6/2.0 and the 900 px supporting pane, activation paths and the modal
    guard, focus kept when the prompt opens, scroll kept across tabs, desktop layout unchanged, Onboarding),
    `UI_QT_QML_MAIN_RECEIVER_NOTICE` (`tst_main_receiver_notice.qml`: copy, Reconnect resuming the same session, a
    Failed session's one card, permission denial, Dismiss before a deferred restart, recreation, Signal search, the pill
    through a refused Airspy start, the reveal, auto-start blocking and kind), `UI_QT_CONTROLLER`
    (`test_auto_start_follows_the_attached_kind`, `test_attachment_kind`), `UI_QT_AUTO_START_POLICY`.
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
- Rigctl passband (issue #621): on audio input with a rigctl peer that demodulates it, `MetricsModel::peerPassband`
  (the analog width view's `peer_passband`, on `tunerChanged` with `passbandInForce`, its `passband_in_force`) shows
  the monitor's `PASSBAND` row (`monitorPassband` in `qml/MonitorScreen.qml`, right after the squelch row) while either
  width is offered (`nfmBandwidthOffered`/`amBandwidthOffered`). The row reads `qml/PassbandSheet.qml`'s `summary`, as
  the terminal's Passband field spells it: the passband asked for now, which the view reads whenever a width is offered
  (`NFM 12.5 kHz (-B)` on a digital session with an explicit width too), and never a setting the peer is not asked for
  (empty with no reading). Its Edit (`monitorPassbandEdit`) opens the sheet. The sheet scopes `All channels | This
  channel` as `SquelchSheet.qml` does (a this-channel step through `CommandBridge::editScanRow()`, `Use default` and
  `List value` row actions, back to All channels when another row comes on air) and steps the NFM and AM widths over
  `Util.NFM_WIDTHS_HZ`/`AM_WIDTHS_HZ` with no DSP bound (`Util.nextWidthIn()` with a max of 0, so a stale rate from a
  radio session bounds nothing) through `setNfmBandwidthHz()`/`setAmBandwidthHz()`; *Use the default width* sends 0. An
  unset width reads as, and steps from, what stands in for it: `nfmBandwidthUnsetText`/`amBandwidthUnsetText`
  (`dsd_app_analog_width_setting_text()`: `-B 12.5 kHz`, `peer's own`, `default`) and
  `nfmBandwidthUnsetHz`/`amBandwidthUnsetHz` (`dsd_app_analog_width_unset_hz()`, the kind's default where that is 0),
  on `controlChanged`, compared in `analogSettingEquals()`. All channels shows the reading only while `passbandInForce`
  says the kind's monitor runs with no row width of its own, and the setting otherwise, so a typed digital row keeping
  the peer at `-B` does not read as the setting the steps move. The sheet closes when the session stops, the peer goes
  away, or no kind is offered or pending. `qml/RadioSheet.qml` stays radio-only: on a peer session its width note points
  to the monitor's Passband row. Tests: `UI_QT_METRICS_MODEL` (`test_rigctl_audio_passband()`), `UI_QT_QML_CALL_LISTS`
  (`tst_monitor_passband.qml`, `tst_radio_analog.qml`).

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
  `dsd_realtime_mono_s()`, and the audible-audio stamp that record carries (`src/core/audio/audio_activity.c`) and its
  age use `dsd_realtime_mono_ms()`. One sanctioned exception compares a decoded stamp with real time: `day_label()` in
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
    It removes the rows (one `rowsRemoved`) rather than resetting the model: a list building a row in its cache buffer
    cancels it by index on a reset, after the model is already empty.
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
  - Persistence: `call_history.json` (rows) and `call_history_seen.json` (seen map, newest 2048 in retention order).
    Both are rewritten whole, so the debounce follows their size (issue #745):
    - **Debounce:** after a save, the next waits the longer of 3 s and the time the last save's bytes take at
      `kSaveBudgetBytesPerSec` (16 KiB/s). A small log still saves 3 s after a change; a full one, ~860 KB, saves about
      once a minute instead of every 3 s, which was ~1 GB/hour of flash on a busy system.
    - **Worker:** the UI thread hands the single save worker (`m_savePool`, one thread) copies of `m_rows` and `m_seen`.
      The worker builds the JSON and writes both files, reading nothing live (issue #718: the build alone cost ~62 ms
      per save at a full log). It reports the bytes written to `onSaveFinished()`.
    - **Ordinary saves:** the debounce and `clearAll()` never queue a second save while one is in flight
      (`m_savesInFlight`); a change during it marks the stores dirty and re-arms the debounce when the last one finishes.
    - **Forced saves:** `saveNow()`, when something is owed, copies the stores at once and queues the save straight onto
      the pool. It runs right after any save in flight, without waiting for the GUI event loop, which Qt pauses on
      Android in the background. It runs:
      - when a session ends, from `UiController::tick()` after that tick's final history ingest;
      - when the application turns Suspended or Hidden;
      - at `aboutToQuit`.
    - **Destructor:** waits for the worker, then writes any owed save synchronously (`saveSync()`).
  - Tests: `UI_QT_CALL_HISTORY_MODEL`, `UI_QT_TALKGROUP_LIST_MODEL`, `UI_QT_QML_CALL_LISTS`
    (`tst_history_session_identity.qml`), `CORE_INIT_STATE` (the ring identity).
- Call history frequency and access code (issue #575; `call_history_model.{h,cpp}`, `call_history_merge.h`). Every row
  carries the frequency its call was heard on (`Row::freqHz`, Hz, 0 unknown) and its access code as a
  `dsd_access_code_kind` and value (`Row::codeKind`, `Row::code`), read from `Event_History::freq_hz`,
  `access_code_kind` and `access_code`. The roles are `freqHz` (qint64), `accessCode` and `accessCodeText`; the two
  texts are spelled by `app_control/access_code_view` at read time, and only the kind and value are stored.
  - Per-field fold: the source label, the frequency and the code pair each keep the provenance of the fragment they
    came from (start, push stamp, slot; `CallHistoryProvenance`, `call_history_provenance_compare()`). A merge
    (`tryMerge()`) adopts a fragment's value by `call_history_fold_adopts()`: the newest fragment with a known value
    wins, unknown never erases known, and equal provenance adopts, since that is the same fragment filling itself in.
    So fragments folded in any arrival order give the same row. `merge_source_label()` runs on the same rule, with its
    behaviour unchanged. `rows_mergeable()` never looks at the frequency: a trunked conversation lands on several voice
    channels, and splitting on frequency would undo the fold, so the merged row shows the newest fragment's frequency.
  - Seen ratchet: `SeenState` keeps the `freqHz`, `acKind` and `ac` last read. `call_history_seen_absorb_fill()`
    counts unknown -> known on a re-read ring row as an advance, so a committed row whose frequency or code the core
    filled in place re-ingests as an update while its end and source stay. A change from one known value to another
    is ignored, because the core only ever fills a committed row (a reacquisition merge).
  - Repeated notices: a notice heard again (`absorbRepeatedNotice()`, matched by the unchanged `findRepeatedNotice()`)
    fills the frequency and code the logged notice lacks, keeps the ones it knows (push stamps say nothing across
    rings), and signals them with `session`. So a notice an older build logged gains them when a fresh replay ring
    delivers it again.
  - JSON: rows store `freqHz`, `freqWhen`, `freqSeq`, `freqSlot`, `acKind`, `ac`, `acWhen`, `acSeq` and `acSlot`; the
    seen store `freqHz`, `acKind` and `ac`. Every key of an unknown field is omitted, as `detail` and `channel` are. On
    load a missing key reads as unknown, and a known value stored without its provenance takes the row's own
    (start, push stamp, slot), as `srcNameWhen` does. A kind or value that does not fit the ring's `uint8_t` and
    `uint16_t` loads as unknown. Older builds ignore the keys.
  - Tests: `UI_QT_CALL_HISTORY_MODEL`, `UI_QT_CALL_HISTORY_MERGE`, `APP_CONTROL_ACCESS_CODE_VIEW`.
- Tuned frequency, call codes and replay tuning on screen (issue #575; `qml/MonitorScreen.qml`,
  `qml/HistoryScreen.qml`, `qml/HistoryDetailSheet.qml`, `qml/HomeScreen.qml`, `qml/SpectrumScreen.qml`).
  - Signal strip: its first reading is `FREQ` (`monitorFreq`), `Util.fmtMhz(metrics.centerFreqHz)` while that is above
    0, in every mode: the tuned frequency, a scan target's park frequency, or the voice channel a trunked target
    follows. The scan header (`scanTargetHeader`) is unchanged, because on a phone it would elide the target's name.
  - Rows: a voice row's meta line puts the `accessCode` role right after the ids and `Util.fmtMhz(freqHz)` last, after
    the channel, so the elide takes the frequency first. History notice rows place them the same way around their
    detail; the monitor's notice rows keep their detail (or "data message") first, then the code, then the frequency.
    The exception is a History row its search found by frequency (next bullet), which leads with it. `CallRow` renders
    its name and meta line as plain text: aliases and SMS bodies are what the air delivered, and Qt's automatic
    rich-text detection would draw a tag in them as markup.
  - History search (`CallHistoryFilterModel`, `frequency_query.{h,cpp}`): the trimmed text matches the name, the
    talkgroup and unit when above 0 (0 is "none decoded", which no row prints), the source label, a notice's detail,
    the channel, and the frequency. The frequency is searched only when the text has a decimal point or the MHz unit
    (`digits "." digits* [MHz]` or `digits MHz`, at most 5 MHz digits and 6 decimals): a bare number stays an ID search,
    since three- and four-digit talkgroups collide with MHz values. A frequency query finds a row when its digits begin
    the row's frequency in either spelling the app prints: rounded half up to 10 Hz, or to the Hz (the Site and
    Network sheets' six decimals). `Util.mhzText` formats the inexact double `hz / 1e6` with `toFixed(5)`, which lands
    on the floor or the ceiling of the exact value (ceiling only when the sixth digit is 5 or more); the floor is a
    prefix of the six-decimal spelling and the ceiling is the rounded one, so typing what a row shows always finds it.
    `UI_QT_FREQUENCY_QUERY` holds that contract against the real `Util.js`. The view reads `frequencyQuery` and
    `frequencyMatches(hz)`: a row found by its frequency leads its meta line with it (ahead of EMERGENCY, which the badge
    still shows) and adds it to its accessible name, while a row the same text matched by its channel keeps its order.
    `filterText` notifies on its own `filterTextChanged`, because the search field mirrors it and a pill change must not
    write the applied text over pending typing; `filterChanged` still fires for every filter, for the scroll-to-top.
    "Clear filters" (`HistoryScreen.clearFilters()`) also stops the debounce and empties the field. Limit: at very
    large text (about 1.6x and up on a 360 px phone with a badge) the meta line, which shares its row with the badge and
    time, can still elide the leading frequency; the accessible name and the details sheet keep it. Tests:
    `UI_QT_FREQUENCY_QUERY`, `UI_QT_CALL_HISTORY_MODEL`, `UI_QT_QML_CALL_LISTS` (`tst_call_log_follow.qml`).
  - Frequencies written to be tuned or saved use `Util.mhzValueText` (to the Hz; a whole 10 Hz keeps `mhzText`'s
    spelling): the remembered Explore frequency, Save as a system, Spectrum "Go to" and the band buttons. `mhzText`
    rounds to 10 Hz for reading only. The rtl and rtltcp input specs round the MHz field to the nearest Hz
    (`input_spec.c`), since a decimal like `128.155625M` multiplies out just under its integer.
    The record every details sheet opens with (monitor, History, Home's recent activity) carries `freqHz`, `accessCode`
    and `accessCodeText`, and the sheet adds the long code text and "Frequency …" after the radio ID, which keeps the
    indexes of the lines before it.
  - Replay: `MetricsModel::replayInput` (`controlChanged`, beside `tunerControlled`) is `dsd_opts_input_is_iq_replay()`
    on the snapshot `radioInput` reads, the predicate app-control refuses tunes on. The Spectrum screen is view-only
    while it holds, whatever the session's intent, so `tuneTo()`, the tap, the rail, stepping and "Go to" do nothing.
    It hides "Explore from here", closes that button's confirm and an open "Go to" sheet when a replay begins, and
    shows "An I/Q replay cannot retune." in the button's place, the refusal's own words.
  - Carrier identity (Spectrum strip): `SpectrumScreen.qml`'s status strip is two lines bounded by the Radio button
    (the strip edge when it is hidden). Line 1 (`spectrumSyncLabel`, SNR, `spectrumCfoLabel`, `spectrumCallLabel`)
    gives way talkgroup first (elided), then the offset (hidden); line 2 (`spectrumIdentityLabel`) is
    `Util.fitTokens()` over `carrierIdentityTokens`, whole tokens only with " …" when any were dropped, led by
    `carrierIdentityProtocol` unless `syncedHere && carrierIdentityLive`. Line 2 keeps a line's height when empty so
    line 1 never moves; the strip is `max(Radio button, both lines)`. The header moves its status pill under the
    readout, which is never elided, when the two do not fit side by side.
  - Tests: `UI_QT_METRICS_MODEL` (`test_replay_input()`), `UI_QT_QML_CALL_LISTS` (`tst_monitor_scan_target.qml`,
    `tst_monitor_recent_calls.qml`, `tst_call_log_follow.qml`, `tst_spectrum_screen.qml`, `tst_back_modals.qml`).
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
- Site identity (Monitor and Spectrum): `MetricsModel::fillSiteView()` builds the `siteChanged` group from
  `app_control/site_identity_view` over the snapshot: `siteProtocol`, `siteLine` (the full form with its protocol),
  `siteConfirmed` (the view's `live`), the SiteSheet fields (P25 NAC/WACN/SYS/RFSS/SITE/LRA, DMR colour code/site
  text/rest LSN/XPT free LCN (`dmrXptFreeLcn`, -1 before one)/trunking brand (`dmrBrand`)/Tier III system code with no
  site text (`dmrT3Syscode`), NXDN RAN or IDAS area and location codes, EDACS site/system/area/CC LCN
  (`edacsSystem`, `edacsArea`, `edacsCcLcn`), the dPMR colour code or M17 CAN (`siteAccessCode`), TETRA MCC, MNC,
  colour code and LA (`tetraSiteMcc`, `tetraSiteMnc`, `siteAccessCode`, `tetraSiteLa`); each int is -1 until received),
  so every protocol whose site row opens the sheet has its rows there,
  each only once received, and `ccFreqHz` from the view's `cc_freq_hz` only. There is no Qt latch: the record keeps the
  last heard values on its carrier and is reset at every carrier boundary and trunk-scan target switch, so no visit
  generation is needed. The Spectrum-facing
  `carrierIdentityTokens` (compact tokens), `carrierIdentityProtocol`, `carrierIdentityText` (full form, for
  accessibility) and `carrierIdentityLive` notify on the same signal, a liveness-only change included.
  `p25Phase2ParamsReady`, `p25Phase2Seed` (`BEE00.348.293`) and `p25Phase2SeedSet` (`p2_hardset`) read the Phase 2 seed,
  which is configuration; `qml/SiteSheet.qml` shows it as its **Phase 2 seed** row, "(set)" when hand-set. `vcFreqHz`
  is the voice channel being followed. Tests: `UI_QT_METRICS_MODEL` (`test_site()`), `UI_QT_CONTROLLER` (stop clears),
  `UI_QT_QML_CALL_LISTS` (`tst_monitor_site.qml`).
- TETRA: `MetricsModel` publishes the `tetra*` group (`tetraVisible`, `tetraModeText`, `tetraEncrypted`,
  `tetraNetworkText`, `tetraTrunkText`, `tetraOnTraffic`, `tetraControlText`, `tetraTrafficText`, `tetraMmStatusText`,
  `tetraCodecText`, `tetraCodecFault`) on its own `tetraChanged` signal, filled in `fillTetraView()` from
  `app_control/tetra_view` over the snapshot `refresh()` is handed, so no Qt source includes a protocol header.
  `qml/MonitorScreen.qml` shows the rows (`monitorTetra`, `monitorTetraChannels`, `monitorTetraMmStatus`,
  `monitorTetraCodec`) only while `tetraVisible`, each part only while its text is set, coloured from the flags
  (magenta for encryption, cyan on a traffic channel, the alert colour for a codec fault). The terminal shows the same
  view as Call Info lines (`TETRA:`, `Network:`, `Trunk:`, `Control:`, `Traffic:`, `MM status:`, `Codec:`,
  `ui_format_tetra_lines()`), with no protocol call and no environment variable. The TETRA decode chip (`-fT`, `qml/Util.js`) carries no `trunked` suggestion: it covers
  direct mode too, whose speech call following would mute. Tests: `UI_QT_METRICS_MODEL` (`test_tetra_rows()`),
  `UI_QT_QML_CALL_LISTS` (`tst_monitor_tetra.qml`, `tst_wizard_decode_chip.qml`), `UI_NCURSES_PRINTER_HELPERS`.
- `dsd_app_lead_slot()` in `app_control/call_view.c` selects the earliest exact start among identified active
  calls for the Monitor hero, its quality row and the Android notification. Later calls on the other slot do not
  displace it. Exact ties prefer the lower slot; when no call is active, the lowest ended slot wins. An earlier
  call gaining identity late takes the headline using its original start.
- Second-slot strip (`otherSlotPanel` in `qml/MonitorScreen.qml`): shown while `MetricsModel::twoSlotCarrier` holds or
  the non-hero slot has an active call, so on a two-slot carrier the other slot's calls never move what is below it.
  Idle, it reads "SLOT N · idle" (slot 2 with no call or slot 1 leading, slot 1 with slot 2 leading), and Skip and More
  are hidden in their places and `Accessible.ignored`. `twoSlotCarrier` mirrors `dsd_state::tdma_carrier`, the decoder's
  own verdict, from each snapshot with no hold of its own. The decoder sets it on content that passed its check, never a
  bare sync: the DMR colour-code lock (`dmr_confidence.c`) or a DMR burst whose own 16- or 24-bit check passed with no
  RAS (`dmr_dburst_proves_tdma_carrier()`, which is how mobile and direct-mode carriers prove it), a P25 Phase 2 FACCH,
  SACCH or ESS that decoded with its parity check kept (`p25p2_note_tdma_carrier()`, no soft erasures), or a whole
  X2-TDMA voice superframe on one carrier with a further X2-TDMA sync word in its places, or one that follows another
  whole superframe on that carrier with no hunt gap (its sync word ends exactly 24 symbols after the last one ended,
  `dsd_state::x2tdma_superframe_*`, dropped by an acquisition reset), which is how direct mode, whose other timeslot
  carries no sync, proves it (X2-TDMA has no FEC to check). A sync loss (`noCarrier()`) keeps it.
  `dsd_frame_sync_reset_acquisition()`, the carrier boundary and a trunk-scan target switch
  (`carrier_boundary_forget_evidence()`), a decode-mode change and the start and end of an engine run clear it, and so
  does a decoded P25 Phase 1 NID (`dsd_dispatch_handle_p25p1()`), since a trunked return from a Phase 2 voice channel to
  a Phase 1 control channel is no carrier boundary. Another FDMA protocol heard on the same carrier with no reset
  between leaves it standing until the next reset or boundary. The Skip button widens with the text scale instead of
  cutting its label short. Tests: `FRAME_SYNC_INTERNAL_HELPERS`, `ENGINE_NO_CARRIER_RESET`, `APP_COMMAND_QUEUE`,
  `DMR_CONFIDENCE_GATE`, `DMR_LC_RS_VERDICT`, `P25_P2_RELIABILITY`, `P25_P1_SYNC_DISPATCH`, `X2TDMA_VOICE_HELPERS`,
  `APP_CONTROL_SNAPSHOT_EVENT_HISTORY`, `UI_QT_METRICS_MODEL` (`test_two_slot_carrier_mirrors_the_decoder()`),
  `UI_QT_QML_CALL_LISTS` (`tst_monitor_other_slot.qml`).
- Monitor layout (issue #718, `qml/MonitorScreen.qml`): calls first. Top to bottom: header, hero, live strip
  (`monitorLiveStrip`: the second-slot strip, then the scan controls, each only when the session has them), one
  `ListView` (`recentCallsList`), the call actions and Stop. The list is the only scrolling surface: its header
  (`monitorListHeader`, declared outside the view and parented into `headerItem` so the screen reaches its rows by id)
  holds the "Signal & site" disclosure (`monitorDetailsRow`, a `DisclosureRow` in `monitorDetailsCard`), the readings it
  folds away (`monitorDetails`: site, decryption, signal strip, quality, TETRA, ENC LOCKOUT, received tone, tone filter,
  squelch, passband and scan avoids, in that order with their own objectNames) and the `RECENT CALLS` label; the rows
  are `CallRow`s over one `recentCallsPanel` card in the view's `contentItem`. The engine's last answer (`uiMessage`) is
  the line under the disclosure. The disclosure is closed by default and remembered in the device-local
  `AppPrefs::monitorDetailsExpanded` (`monitor/detailsExpanded`, kept out of backups and kept on restore); its summary
  is built only from readings the rows show (SNR on radio input, the quality row's lead reading with a TETRA codec fault
  ahead of it in `Theme.alert`, the decryption state, an ENC LOCKOUT count in `Theme.magenta`, `Util.siteShortForm()`),
  one elided StyledText line; a fault never opens the rows. `tier` is chosen from measured geometry, never call
  content: the first of 0 (stacked header, full hero, live strip fixed; on brokered hosts stacked only when the header
  actions fit, see the width rule under Monitor receiver pill), 1 (header actions on the title line, Sites and
  Spectrum left to the session menu, which carries both on the same conditions; at 500 dp and wider they stay), 2
  (compact hero, duration on the name line), 3 (live strip in the list header above the disclosure), 4 (no label)
  whose `callSpaceFor()` (viewport less the closed list header) holds two `callRowHeight`s; 4 when no tier does but 4
  leaves a row, else 5, where only Stop (and, for a lost receiver, the line above it) stays put and header, hero, strip
  and actions move into slots at the top of the list header. Parts move by `parent` bindings between fixed hosts
  (`topHost`, `stripHost`, `actionsHost`) and those slots; slots and hosts size from the budget's own numbers
  (`stripHeight`), since an Item hides its children. The hero's height is reserved from its fixed lines
  (`heroFullHeight`, `heroCompactHeight`) so waiting, a call and its end share
  one box. The list has no current item (`currentIndex: -1`): a ListView is a focus scope that would hand focus to it
  as rows land. Keyboard focus inside the list is revealed through `Theme.revealFocus(list, list.contentItem, item)`,
  whose bounds follow `originY` when passed a view's `contentItem` (plain Flickables keep 0). `FollowLatest` measures
  against the header's end (`latestY`): with any header in view a landing call moves nothing (ListView puts it under the
  header), within a row of the calls' top the newest row is kept there, below that the reading position is kept; and on
  a header resize while the header is in view it moves `contentY` so `headerAnchor` (the disclosure card) keeps its
  place on screen, keeping the list's start at the start, deferring to the release under a held finger, and measuring
  from the offset noted between resizes because ListView may clamp `contentY` before the header announces its height.
  The header's end is read off the first row; the footer stands in only for an empty list, and with the first row
  recycled far down a long list the header's own geometry is used while it is in view, else the view is left alone.
  Tests: `tst_monitor_layout_budget.qml` (tiers at 360x690, 411x780, 411x914, 640x360 by scale and live strip,
  disclosure, summary, follow and focus), `tst_monitor_recent_calls.qml`, `tst_monitor_compact.qml`,
  `tst_main_safe_area.qml`, `UI_QT_PERSISTENCE` (`test_prefs_key_table`).
- Monitor receiver pill (issue #647; brokered hosts only, `qml/MonitorScreen.qml`): the live pill (`monitorLiveStatus`,
  label `monitorLiveLabel`, dot `monitorLiveDot`) reads `LOST` with a `Theme.alert` dot (no halo) as soon as the host's
  `receiverHealth` reports the session's receiver lost or failed (`receiverLost`, read through a `DeviceStatus`), while
  the engine still saves and uploads its last calls, instead of `LIVE` until teardown ends; Stop reads "Stopping…" and
  is disabled meanwhile. For a session on a local receiver (`sessionReceiver`: the system's `usb` or `airspy` source,
  or the lost receiver's kind) the pill is a button (accessible name "RTL-SDR lost" while lost) that opens
  `monitorReceiverSheet`, a `ConfirmDialog`: healthy, the receiver's headline and record text with OK; lost, what
  happened and that the session is finishing, with Cancel and **Stop now** (`decoderHost.stop()`, the user's Stop,
  which abandons the upload backlog). The sheet fixes its view when it opens, closes if `receiverLost` changes under it
  (OK never turns into Stop now under a finger) and closes with the screen. rtl_tcp, network and file sessions, and
  every desktop session, keep an inert pill (StaticText, no tab stop); so does a healthy session the Monitor reattaches
  to after an Activity recreation, which has no system map, while LOST still shows there from `receiverHealth`. In tier
  5, where the header scrolls with the list, a lost receiver also shows as one fixed line above Stop
  (`monitorReceiverLostLine` in `lostLineHost`: alert dot, `RTL-SDR LOST`, "Session finishing", ›) that opens the same
  sheet; hidden it has no height, and it sits outside the budget (it can appear only where the budget already fixes
  only Stop), so LOST never changes the tier. Width rule: `headerActionsWidth` sums the actions the session would show
  (Sites, session options, Spectrum, and the pill at the widest label it can show, `LIVE`, `LOST`, `STARTING…`,
  `STOPPING…` or the current one, measured by a hidden probe plus constant padding), and `headerActionsFit` requires it
  to fit the row (less the title's 12 px at 500 dp and wider). `wideHeaderActions`, `headerStacked` and the budget's
  `headerBarHeight()` all require it, so when it does not fit Sites and Spectrum go to the session menu as in tier 1
  and tier 0 keeps the title-line header; a label change never changes which actions show. A 360 dp phone at 1.0 with
  Sites and Spectrum therefore keeps both in the session menu even under LIVE (`STOPPING…` would overflow); 411 dp at
  1.0 keeps all four (at 1.6 `STOPPING…` overflows there too). Every gate is `!localDeviceBrokered ||`, so desktop
  geometry is unchanged; the one desktop change is the LIVE pill's accessible name, which now names the receiver's
  state there too. Tests:
  `UI_QT_QML_MONITOR_RECEIVER` (`tst_monitor_receiver.qml`), `tst_monitor_scan_target.qml`
  (`test_header_actions_fit_a_tall_phone`, `test_header_geometry_unchanged_off_android`),
  `tst_monitor_layout_budget.qml` (`test_lost_line`, the `+brokered` tier rows).
- Monitor call actions (`monitorLiveActions` in `qml/MonitorScreen.qml`): Mute · Hold TG · Skip · Avoid at every size
  (the talkgroup list is the session menu's Talkgroups). The toggles keep one label each and show their state as
  checked (`OutlineButton.checkable`/`checked`: `Theme.cyan` border and the pressed tint, `Accessible.checked`), as does
  the scan controls' Hold scan. Four columns where a quarter of the row, less the 12 px either side of a label, fits the
  longest word of any label (Mute, Hold TG, Skip, Avoid TG, Lock out) at the font scale in force, else two (a 360 px
  phone takes four at 1.0, two at 1.6); one label line count and one height for every cell, at least
  `Theme.minimumTouchSize`; labels wrap only at spaces (`Text.WordWrap`). Cells are whole pixels, centred, so a cell's
  border is never lost to the list's clip in tier 5. Tests: `tst_monitor_call_actions.qml` (`test_phone_grid`,
  `test_toggles_show_checked`), `tst_monitor_compact.qml`.
- Typed input and sheet dismissal (issue #718): `qml/ModalSheet.qml` dismisses through `requestDismiss()` on Back,
  Escape and a real tap outside its panel: press and release both off the panel with no more movement than
  `Qt.styleHints.startDragDistance` (a press on the panel's padding that slides off, or a drag in the 18 px phone
  gutter, is not a tap). Its `PointerBarrier` accepts every press, so nothing behind a sheet receives presses, drags
  or releases. Every open scrolls the panel back to its top before focus moves, so a reused sheet never reopens where
  its last visit was scrolled to and the focus reveal still runs. Sheets fall in two classes by what they hold:
  - Form sheets (typed input or an in-progress choice) set `closesOnOutsideTap: false`, keep a visible Cancel/close
    control, and leave only through it, Back or Escape, every one of which goes through the sheet's discard check:
    `BackupFlow.qml`'s passphrase and unlock sheets (Cancel runs `dismissExport()`/`dismissUnlock()`, as dismissal
    does; the busy steps still cancel at once), `SignalSearchRangeEditor.qml`, `CsvImportFlow.qml`'s companion sheet
    (asks once a companion was chosen; "Choose a file…" keeps the current companion until a replacement is
    accepted), `TalkgroupEditSheet.qml`, `KeySheet.qml`, `ToneFilterSheet.qml`, `DecryptionProfileEditor.qml` with its
    key and mapping dialogs, Spectrum's Go to, the wizard's CSV picker (its pending Decimal/Hex choice resets on the
    next open, so it asks when the choice changed since opening with no key file assigned to apply it to) and RadioReference's review-import sheet.
  - Menus (`ActionMenu`), `ConfirmDialog`, `ChoiceSheet`, `DiscardDialog` (outside tap = keep editing), read-only
    sheets (history details, network, site, failure and notification sheets, replay details), the live-apply
    `RadioSheet`/`SquelchSheet`/`PassbandSheet` (their values are in-flight engine requests) and the Imports, scan
    list and site-chooser sheets keep tap-outside-to-close. Busy/saving/closing sheets stay non-dismissable through
    an empty `dismissHandler`.
  A form's `requestClose()` compares the live field text (never state committed on `editingFinished`, which Back
  beats) with a baseline captured when its editing scope opened or was replaced, and asks through
  `qml/DiscardDialog.qml` (parented to the sheet) when they differ: the Signal search range editor (`openedForm`),
  the idle Signal search screen (`openedSource`, set by `seed()` from Main's `seedSignalSearch()` and when a session
  stops), the backup sheets (only while editable and typed), the profile editor and its dialogs (`openedForm`,
  re-captured after `openMatching()`/`openTalkgroup()` prefill, plus the pending fallback algorithm), `KeySheet.qml`
  (an unapplied direct-key draft asks the discard question; another saved profile or scope than at open, not
  applied yet, asks "Close without applying the profile …?" through `DiscardDialog.askWith()`), `ToneFilterSheet.qml`
  (`openedPolicy`, re-captured by `chooseScope()`), Spectrum's Go to, `ScanListScreen.qml` (`fingerprint()` includes
  the unadded frequency fields), plus the wizard, Explore setup and talkgroup sheets. `RadioReferenceScreen.qml`
  holds its site selection and import options on the screen: the header's back button, Android Back (its
  `NavigationLayer` calls `requestClose()`) and the "All systems"/"Search" row ask once `selectionState()` differs
  from `openedSelection` (captured when a system's sites land, so a single site the screen picked is not an edit). A
  `DiscardDialog` closes with the surface under it, so it never returns when that reopens.
  `RadioReferenceScreen.reset()` keeps the typed zip code and system ID (and their tab) across visits instead;
  `doImport()` clears them.
- Composing (preedit) text (issue #718): an Android keyboard holds the word being typed as preedit, which
  `TextInput.text` leaves out, so forms would save a name without its last word, judge an edit unchanged or reject a
  field as empty. `input_commit.{h,cpp}` (`InputCommit`, the `inputCommit` context object, wrapped by
  `Navigation.commitComposing()`/`Navigation.composing`) commits it at action boundaries rather than from a
  window-wide press filter, which would fire before Qt routes a tap inside the composing text to the keyboard and
  break cursor taps, selection handles and drags. It calls `QInputMethod::commit()` (Android finishes composing and
  delivers the word); when the focus item still reports `inputMethodComposing` (offscreen/test platforms) it sends
  the item its `preeditText` as a commit and resets the input method. Callers: `activate()` of `OutlineButton`,
  `GradientButton`, `IconButton`, `DashedActionButton` and `BottomNav` (commit and finish editing: focus leaves the
  text field so `editingFinished` drafts, such as the profile editor's fallback algorithm, update before the action
  reads them) and of `DecodeChip`, `FilterPill`, `SegmentedControl` segments and `DisclosureRow` (commit only);
  `ModalSheet.requestDismiss()`; `Navigation.back()` before its focus clear. A button gated on a field's committed
  text puts that term in `inputValid` (visuals dim on it) rather than `enabled`: its default `enabled` is `inputValid
  || Navigation.composing`, so it is live while a word is composing, and a tap commits first and calls `clicked()`
  only if `inputValid` then holds; otherwise it is disabled as before (accessibility and keyboard included), and a
  real guard (busy, permission) stays in `enabled`. Callers: the profile editor's "Use this key" and "Use override",
  `KeySheet`'s apply, Scan list "Add frequency", RadioReference Find/Open, the wizard's Continue/Save, Tone filter
  Apply and Explore's Start.
  Tests: `UI_QT_INPUT_COMMIT`, `UI_QT_QML_CALL_LISTS` (`tst_input_commit.qml`, `tst_modal_sheet_outside_tap.qml`,
  `tst_signal_search.qml`, `tst_settings_backup.qml`, `tst_csv_import_flow.qml`, `tst_talkgroup_edit_sheet.qml`,
  `tst_modal_input.qml`, `tst_decryption_profile_editor.qml`, `tst_key_sheet.qml`, `tst_tone_filter_sheet.qml`,
  `tst_scan_row_edits.qml`, `tst_spectrum_screen.qml`, `tst_scan_list_screen.qml`, `tst_radioreference_screen.qml`,
  `tst_back_modals.qml`, `tst_wizard_encryption.qml`, `tst_main_overlay_layering.qml`, `tst_receiver_choice.qml`).
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
  `DecoderHost::requestSiteLocation` shares request ownership and cancellation with nearby import but asks Android
  for coordinates only: fixes must be at most 30 seconds old, a cache must also be within 100 m to finish immediately,
  and current providers compete for accuracy up to the 20-second deadline. GPS participates with fine foreground
  permission; approximate permission remains usable. The new JNI entry point is retained in `proguard-rules.pro`.
  `SiteChooserSheet` orders eligible saved siblings by distance, names the nearest site, exposes missing positions
  and saved-fix/accuracy limitations, and gates the nearest action during a lookup. While Running that action emits
  the same saved UID restart as a manual site switch; Main waits for Idle and resolves the UID again. Location
  completion itself never starts a session. Tests: `ANDROID_LOCATION_JVM`, `RUNTIME_RR_SOAP`,
  `UI_QT_RADIO_REFERENCE`, `UI_QT_QML_CALL_LISTS` (`tst_site_chooser_sheet.qml`, `tst_site_restart.qml`).
- `talkgroup_list_model.{h,cpp}` polls the effective core policy's source-context/generation pair after call-history
  ingestion, merging listed rows with uncovered talkgroups heard this session (history rows of `historySession`).
  Every history change rescans the whole history (a merge can move a row to another session, so contributions are
  not tracked incrementally). `CallHistoryModel` hands the heard ids over from its row storage through the
  `HeardTalkgroupSource` Qt interface (`heard_talkgroup_source.h`), not one QVariant `data()` lookup per role and row;
  any other history model is read by role name. Coverage is a binary search over the listed rows sorted by start
  with a running maximum end, so overlapping ranges cover correctly (issue #718).
  `talkgroup_filter_model.{h,cpp}` filters by category and name/ID for `qml/TalkgroupsScreen.qml`, opened by the
  monitor session menu's **Talkgroups** entry.
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
  The model's `active` property is bound by `Main.qml` to the diagnostics screen's
  visibility (issue #718): while inactive a tick consumes new lines (the pause and
  pending-count rules are unchanged) without resetting the model, and becoming active
  performs one reset, from a fresh snapshot or, while paused, from the lines held at
  the pause.
  Runtime's install-once log tap and Android's stderr/host paths feed `submit()`;
  platform sharing remains behind `DecoderHost` in Android's diagnostics provider.
  The persistent tail (issue #745) is two append-only segments, `tail.prev.log` then `tail.log`, each at most
  `DiagnosticsLog::kSegmentLimit` (128 KiB). The writer appends whole records and never rewrites a file. It drains
  every 250 ms and syncs at most once a second, except inside the startup window. `flush()` returns whether everything
  before it is appended and synced, with an optional bound. A segment is synced before it rotates. Loading:
  - drops a crash's unterminated last record;
  - deletes orphaned QSaveFile temporaries of `tail.log` and `tail.prev.log`;
  - expires each segment after seven days;
  - splits an older build's single tail at line starts.

  `installTap()` records the run's `process:` line (pid, OS, app version, plus Android's boot count, device and recent
  `exit:` lines from the platform facts provider). Each new segment starts with that line and the current
  `session starting:` line, including the first segment after Clear (which forgets the cleared session's line). The
  ring keeps the previous run's divider and identity and this run's process line and current session line pinned at
  its front through eviction (`appendRingLocked()`); a session line a newer one replaces is released and evicted first.
  Pins name ring lines by id (`m_ringIds`), not text, so identical session lines from two runs stay apart, and the
  previous run's session line is the one after its last process line.
  Copy and Share therefore always carry them. The only derived line is
  "previous run: Android has restarted since (boot a -> b)", from the previous run's last `process:` line.
  Session attempts are recorded where a start is about to touch a device:
  - `DecoderHost::noteStartAttempt()`, called by `SessionArgsBuilder::start` and `ScanListStarter::start`, and Android's
    USB access request (stage `UsbAccess`) call `noteSessionAttempt()`. That records the start's marker (a USB access
    request records a `usb access requested:` line instead, so a request whose start never comes merges with nothing),
    arms the 10 s startup window, in which every append is drained at once and synced, and waits up to 200 ms for it to
    be durable (best effort, never blocking listening).
  - `UiController`'s Starting transition calls it with no context. It records the plain marker only when no start note
    just described the attempt.
  - Android re-arms the window at `UsbManager.openDevice` and the service's `startDecoding` (`armStartupWindow()`).
  - `aboutToQuit` and the Activity teardown make the tail durable with a bounded flush.

  A failed sync is retried after `kSyncIntervalMs`, never in a loop, and a file given up on (`closeCurrent()`) drops its
  unsynced count, so a lost segment never leaves the writer due. A failed append, sync, open or Clear makes the next
  `flush()` report false. On Windows `QFile::handle()` is a C runtime descriptor, so `dsd_fsync()` (`_commit`) is a
  real flush there too. A current segment over the cap is an older build's unsplit tail and alone is authoritative,
  so a split interrupted by a crash is redone without duplicating records. The split rewrites it only after the
  previous segment holds its prefix, so a failed write of that segment loses nothing.

  `dsd_fsync_parent_dir()` (platform `file_compat`) makes a created, renamed or removed entry durable on POSIX.
- Configuration backup (issue #580; user rules in `android/README.md` "Backup and restore"). The sealed envelope is
  crypto's `backup_seal`; everything inside it is Qt code.
  - Archive (`backup_archive.{h,cpp}` public surface, `backup_archive_build.cpp` export, `backup_archive_parse.cpp`
    restore validation, module-private `backup_archive_rules.{h,cpp}` in `dsd_qt::backup::detail`): `DNBA`, a
    big-endian u32 manifest length, the manifest JSON (format, version 1, app tag, creation time, source roots, the
    portable settings, the five stores, the permanent signal-search avoids, `notIncluded`, and a path/size/sha256
    table), then the carried files' bytes. One table, `typed_path_fields()` (store, field, `File` | `Recording` |
    `BundleRoot`), drives export sanitizing, restore validation and the apply's remapping: a new store field that names
    a path must join it. The export (worker-safe; only `backup_take_snapshot()` reads app state, on the UI thread)
    repairs rows the way the loaders would, clears every `Recording`, strips each profile's `compiled`, `runtimeRef`
    and `pastRuntimeRefs`, carries only regular non-symlink files under the data root's `imports/` (an owned bundle
    whole), clears any other reference and lists it in `notIncluded`, then parses its own output as a self-check. A
    carried file that changes between the export's two reads (a session appends to its talkgroup lists) fails it with
    `BackupError::SourceChanged`, not `Io`.
    `backup_parse_archive()` is pure and treats the archive as untrusted: size caps (64 MiB sealed, 4096 files, 32 MiB
    a file, 4096 rows a store, 4 MiB manifest), a sha256 per file, archive paths under `imports/` with case- and
    NFC-folded duplicates refused, store shapes and identities, settings filtered to `AppPrefs::portableKeys()`; it
    rebases every typed value from the source root onto the target root. Every carried file must be named exactly by a
    `File` value or lie inside a claimed bundle root. A bundle root is an owned `imports/bundles/<id>` holding a carried
    file and claimed by one row only; a row's file under `imports/bundles/` lies in its own root, and no row's file lies
    in another row's root. A plain channel map updated in place with companions keeps its file in `imports/` beside its
    bundle, so a root need not hold its row's file. `Summary::systemsWithExtraArgs` counts systems with extra
    arguments or a decode flag with a token `decode_flag_is_standard()` (`decode_mode_flag.h`) does not list. That
    allowlist is every token the app's producers write (wizard chips, scan-list entries, signal search, the
    RadioReference generator), so a new producer token must join it. Passphrases are NFC UTF-8 of at least 8 code
    points (`backup_passphrase_ok()`); the byte and normalized copies the backup code makes are wiped, the strings
    QML hands it cannot be.
  - Staging and apply (`backup_apply.h`, `backup_stage.cpp`, `backup_apply.cpp`, module-private
    `backup_apply_fs.{h,cpp}`): `stage_restore()` takes the data root its caller read (the model passes the UI thread's
    reading to its worker) and writes `restore-staging-<job>/` (files, stores, settings, avoids and a manifest, each
    file 0600). It dry-run validates each library file with `ImportedFilesModel::validate()`; a row that fails (a
    channel map whose companions this phone refuses, say) is left out, not the whole backup: its file and its bundle's
    files are not staged, every typed reference to them is cleared, and the staged summary counts it out and lists
    "<name>: couldn't be read on this phone" in `notIncluded`. Only structural problems refuse a staging. The dry runs
    stop as soon as the job is cancelled (`stage_restore()`'s `proceed`), and are bounded: rows the app writes never
    name each other's files, so a library still losing rows on its third pass (a crafted chain) is refused as `Damaged`,
    at most three dry runs a row. `promote_staging()` renames it to `restore-pending/`, and `commit_pending()` writes
    `COMMITTED` (the manifest's sha256); every check reads only a regular `COMMITTED` file as the marker, as the apply
    does. `ui_load()` calls `apply_pending_restore()` as its first statement, before any model reads a store, so
    restored stores go through the loaders and migrations like any start's; its `engineBusy` is
    `DecoderHost::engineBusy()`. The apply writes an undo journal (`restore-pending/journal/`, `COMPLETE` last) of the
    old managed set: the five stores and the avoid file, the files under `imports/` a store's `File` value names (never
    one a `Recording` names), the owned bundle directories library rows claim (the first claim of a folded name), and
    the files the app generates under `decryption/` (`<uid>/<uuid>.(hex|dec|map).csv`) and `scan_lists/` (`<uid>.csv`);
    anything else there is kept, with the directories it is in. Only those are deleted. An incoming file whose
    destination holds anything else takes a free name (`name (n).ext`, a bundle `<id>-<n>`, a blocked directory `dir
    (n)`) and the stores' references follow. File contents are never rewritten, so a bundle whose row keeps its file
    outside it (a plain channel map beside its bundle names companions `bundles/<id>/…`) never moves: when an entry the
    old setup does not own holds its place, the apply fails before any change ("A folder left by an earlier import is in
    the way of a restored channel map…"). A crash re-runs to the same end state, and an I/O failure rolls back byte for
    byte (pruned empty directories excepted). Outcomes are `Applied`, `Failed` (nothing changed), `Deferred` (engine
    busy or storage short; kept) and `Stuck` (neither finishing nor undoing worked; kept); `backup-restore-result.json`
    records the applied, failed and stuck ones. A commit marker that cannot be removed is Stuck without a rollback, on
    purpose: a rollback ends by removing that same marker, so it could not complete either; the next start repeats the
    idempotent roll forward and removes the marker then. A journal whose `prefs.json` does not match the hash its
    manifest records is unreadable (Stuck) rather than a rollback that would clear the settings. `discard_pending()`
    refuses once the journal is complete (`pending_apply_started()`). `detail::DataRoot` is the only writer, deleter and
    renamer: it refuses a path outside the data root or one with a symlink or junction among its existing components,
    and removes trees without following links. Documented limits: a resumed apply (journal complete) that finds the
    engine busy is Deferred, and that session runs on the half-applied configuration, whose edits the next roll forward
    can overwrite; a corrupt complete journal is Stuck at every start and cannot be discarded; a store file that is a
    symlink fails every restore (`QSaveFile` writes through a link, so the models' own saves never replace it).
    `ApplyEnv` injects the engine check, free space and a fault per named step.
  - `backup_model.{h,cpp}`: `BackupModel`, context property `backup`. One low-priority worker thread (a `QThreadPool` of
    one) runs each job; work and replies cross through two queues under one `std::mutex`, and a job's files are its own
    (`<CacheLocation>/backup/<job>.dsdbak` or `.in`, `restore-staging-<job>/`), so a cancelled job removes only them
    (`discard_staging()` for its staging directory). Construction sweeps both (`sweep_stale_staging()`).
    `prepareExport()` snapshots on the UI thread, then builds and seals on the worker; `saveExport()` calls
    `DecoderHost::copyFileToDocument()`; `inspectRestore()` copies the document with `copyDocumentToFile()` (capped at
    64 MiB), opens, parses, checks free space (twice the archive plus 1 MiB) and stages, and the UI thread promotes.
    `commitRestore()` refuses while a job runs, a session is on screen or `engineBusy()`, commits, and asks `quitApp()`
    once; a refused commit leaves the stage as it was. `restorePending` follows the committed stage on disk, and
    `restoreCancellable` (pending, no complete journal) with it; `discardRestore()` reports `RestoreInProgress` once the
    apply has started. The inspection reports the staged summary, so the confirmation sees left-out rows; its `Io`
    means only that the picked document could not be read (`DocumentFailed`), while a failure on this phone is
    `StagingFailed` (the screen offers the same file again, not another one): no cache or staging place, a copy it
    could not write (`DecoderHost::DocumentLocalFailed`) or read back, an argument or OpenSSL failure once the header
    has parsed (`backup_error_from_open_status()`), a staging or promotion that failed.
    `startupOutcome` is this start's `ApplyResult`, else the result file until acknowledged, and carries the restored
    backup's `notIncluded` lines. The destructor calls `abortDocumentTransfers()` while a job may be on the worker, then
    waits for it. Production seals with `dsd_backup_kdf_params_default()`.
  - QML: one `BackupFlow.qml` in `Main.qml` (z 22, above the scan layers) serves both Settings instances through
    `exportBackupRequested()`, `restoreBackupRequested()` and `cancelRestoreRequested()`. `backup.flowActive` (set for
    the flow's whole length, kept while the app closes) joins `autoStartBlocked`, a USB grant that lands while it is set
    starts nothing, and entering a restore drops pending starts (`dropPendingStarts()`). An unlock error about the file
    (not a backup, newer, too large, damaged, unreadable) swaps Unlock for "Choose another file", which reopens the
    picker inside the same flow. A commit refused as `SessionActive` or `Busy` keeps the stage and the confirmation
    (`ConfirmDialog.closeOnConfirm: false`, its `error` line) for a retry; only Cancel discards. Settings' Backup card
    gates the Restore row on `restoreAllowed` and `!restorePending`, and it and the Home card offer "Cancel
    restore"/"Discard restore" only while `restoreCancellable`, else `Util.restorePartWayText()`. The confirmation, the
    export result and the Home card list `notIncluded` through `Util.notIncludedText()`. `BackupResultCard.qml` on Home
    shows `startupOutcome` and matches the `BackupApply` reason strings of `backup_apply.cpp` through `qsTranslate()`,
    so a change to them changes the card. Text from a backup (app version, `notIncluded`) renders as `Text.PlainText`.
  - Supporting changes: `AppPrefs` names every key once in a table (`keyTable()`) that marks it portable or device-local
    (onboarding, the notification explanation and the last location fix are device-local), with
    `readPortable()`/`replacePortable()`; a new key must be classed there. `DecryptionProfilesModel` compiles at load
    any row without a compiled form, which every restored row is, and persists once; a row that cannot compile is kept,
    and `configuration()` refuses it ("Open this decryption profile and save it again."). Its key files are written only
    below `<root>/decryption` reached through no symlink or junction (`privateWrite()`), so a link left under
    `decryption/` refuses the compile, at load or on save, instead of placing keys elsewhere. `-fp` (ProVoice) reads as
    `mixed` in `Util.js` `decryptionProtocol()`, `session_args_profile_compatible()` and the legacy key migration.
    `json_store_root()` is the data root every store path builds on. A file-source system without its recording, as
    every restored replay system arrives, is refused before it starts (`SessionArgsError::RecordingMissing`, start
    category `recording-missing`), checked after AM's source rule (AM needs a radio whatever the file) and before
    TETRA's, so it never runs `-i ""` and is never told to choose a radio.
  - Tests: `UI_QT_BACKUP_ARCHIVE`, `UI_QT_BACKUP_APPLY` (every step crashed in a clean apply and in a rollback; two
    restores in a row), `UI_QT_BACKUP_MODEL` (with `test_recorder_parity()` against the QML fixture's
    `tests/ui/qml_backup_recorder.h`), `UI_QT_QML_CALL_LISTS` (`tst_settings_backup.qml`,
    `tst_util_decryption_protocol.qml`), `UI_QT_DECRYPTION_PROFILES`, `UI_QT_PERSISTENCE` (`test_prefs_key_table()`),
    `UI_QT_ANDROID_HOST`, `ANDROID_DOCUMENT_TRANSFER_JVM`, `BACKUP_SEAL`; a replay system without its recording in
    `UI_QT_SESSION_ARGS` and `tst_session_initialized.qml`.
- Decryption profile key material (`decryption_profiles_model.{h,cpp}`, `session_args.{h,cpp}`):
  - One hex contract: `session_args_key_hex_normalize()` drops every `QChar::isSpace()` character, `-` and `:` and a
    leading `0x`/`0X`, for the hex direct types (`session_args_key_type_hex()`: hex, rc4, m17Scrambler, m17Aes) and the
    managed hex formats only. `session_args_key_value_normalize()` is the per-type form every argv and live-apply path
    uses (decimal types trimmed). Vendor grammars keep their own (`vendorHexNormalize()`: ASCII whitespace and `0x`).
    `configuration()` returns a direct profile's value normalised, the stored row keeps what was typed, so
    `CommandBridge::applyDecryptionProfile()` never submits a grouped key past the 72-byte command field.
  - `session_args_key_protocol_error()` is the profile protocol's direct-key rule (P25 hex never 10 digits, NXDN hex
    64), used by `ProfileCompiler::direct()` and by `sessionArgs.keyError(type, value, csv, force, protocol)`; the
    four-argument QML call keeps working.
  - `keyKinds(protocol)` lists the managed formats the protocol takes with a width label; ids are unchanged.
    `keyEntryError(profileUid, draftKeys, index, entry)` runs compile's own per-key helpers (`managedSlotError()`,
    `managedMaterialError()`, `legacyDestinationError()`, the collection's cells and references) for the edited row
    against the other draft rows, and judges blank material on what the key keeps (an unsaved row's own material, else
    the stored key's, by key UID) without returning it. `mergeProfileKeys()` refills blank material only under an
    unchanged format; save-time key errors name the key (`Key "Dispatch" (ID 1): ...`).
  - `-mq` (P25 Simulcast) reads as P25 in `Util.decryptionProtocol()`, `session_args_profile_compatible()` and the
    legacy migration's `protocolFor()`.
  - Tests: `UI_QT_DECRYPTION_PROFILES` (entry table, format change, named errors, normalisation, AES-256 through the
    hex importer, restore recompile, `-mq`), `UI_QT_CONTROLLER` (`test_spaced_direct_profile_applies_live()`).
  - QML: `DecryptionProfileEditor.qml`'s key dialog lists `keyKinds()` (the choice kept by id, `kindId`) and gates
    "Use this key" on `keyEntryError()` (`inputValid`, so the #718 composing gate holds). It places the message with
    two narrower calls: the ID alone as a one-digit scalar with no other keys (`idError`) and the key without the other
    draft rows (`ownError`); what remains is a shared-cell clash, shown at the key ID. Material is mono, masked, with
    `RevealButton.qml` (shared with `EncryptionEditor.qml`). `EncryptionEditor.qml` checks only the format list itself
    and leaves shape and protocol width to `sessionArgs.keyError(..., protocol)`. Sheet errors scroll into view
    through `ModalSheet.reveal()`.
  - `KeySheet.qml`: Main's `sessionSavedSystemUid` (also the radio persistence target: empty for scan lists, Explore,
    signal search and wizard tests) reaches it through `MonitorScreen.sessionSystemUid`. "Use for <system> on future
    starts" resolves the system again by UID at the tap, asks through `ConfirmDialog` before replacing another
    profile, refuses a removed system, and writes `decryptionProfileUid` with `update()`; Main then refreshes
    `sessionSystem`. "Save as profile…" opens the editor's `openDirect()` with the typed draft (QML fields only until
    Save profile) and selects the saved profile. Tests: `tst_decryption_profile_editor.qml`, `tst_key_sheet.qml`,
    `tst_history_session_identity.qml`.
- Platform-free by rule: it may include Qt and the public `app_control`, `core`, `runtime` and `crypto` headers (the
  backup seals through `<dsd-neo/crypto/backup_seal.h>`), never engine/io/protocol internals, and never platform APIs
  (`QJniObject`, `jni.h`, `<android/*.h>`). Platform specifics live behind the `DecoderHost` interface, implemented per
  host (`android/decoder_host_android.cpp` today).
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
  `UI_MENU_TREE_AUDIT_NO_RADIO` builds the same audit without `USE_RADIO`, so the tree a radio-off build has is held to
  the same rules in every configuration; its stubs split into common and `*_RADIO` lists in
  `tests/ui/test_ui_menu_tree_audit_stubs.h`, so a row whose handler, label or predicate is radio-only goes in the
  `*_RADIO` list.
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
- API: `<dsd-neo/api/api.h>`

Additional includes of interest:

- Engine: `<dsd-neo/engine/engine.h>`, `<dsd-neo/engine/frame_processing.h>`,
  `<dsd-neo/engine/protocol_dispatch.h>`, `<dsd-neo/engine/trunk_scan.h>`, `<dsd-neo/engine/trunk_tuning.h>`
- Runtime: `<dsd-neo/runtime/cli.h>`, `<dsd-neo/runtime/frame_sync_hooks.h>`, `<dsd-neo/runtime/telemetry.h>`,
  `<dsd-neo/runtime/trunk_scan_hooks.h>`, `<dsd-neo/runtime/trunk_tuning_hooks.h>`,
  `<dsd-neo/runtime/radioreference.h>`, `<dsd-neo/runtime/radioreference_generate.h>`,
  `<dsd-neo/runtime/radioreference_import.h>`, `<dsd-neo/runtime/rigctl_passband.h>`
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

Native CPU tuning (`cmake/NativeArch.cmake`, `DSD_ENABLE_NATIVE`, default ON):

- `dsd_native_arch_apply()` adds the host-tuning flag as a directory-scoped compile option, so every module and the
  embedded PFFFT get it, wrapped in `DSD_NATIVE_ARCH_CONFIGS` (`$<CONFIG:Release,RelWithDebInfo,MinSizeRel>`): Debug,
  sanitizer and coverage builds stay on the compiler's baseline target, and multi-config generators tune per config.
- GCC/Clang: `-march=native` on x86 (it implies `-mtune=native`), `-mcpu=native` on ARM/AArch64, each accepted only if
  `check_c_compiler_flag`/`check_cxx_compiler_flag` pass. MSVC and clang-cl on x86/x64: `try_run` of
  `cmake/native_arch_probe.c` (CPUID + XGETBV, the checks of `src/dsp/simd_x86_cpu.h` extended to every feature
  `/arch:AVX`, `/arch:AVX2` (x86-64-v3) and `/arch:AVX512` (F/CD/BW/DQ/VL, opmask/ZMM state) may emit), cached as
  `DSD_NATIVE_ARCH_MSVC_PROBE`.
- `dsd_native_arch_skip_reason()` turns tuning off when cross-compiling, for Android/iOS/Emscripten, for a universal or
  non-host `CMAKE_OSX_ARCHITECTURES`, for `CMAKE_<LANG>_COMPILER_TARGET`, or when `CMAKE_<LANG>_FLAGS` or a
  release-like `CMAKE_<LANG>_FLAGS_<CONFIG>` already selects a target ISA (`-march=`, `-mcpu=`, `/arch:`, `-arch`).
  The configure output names the flag or the reason.
- Runtime AVX2 dispatch in `src/dsp` is unchanged: its per-file `-mavx2 -mfma` only adds features. MSVC accepts one
  `/arch:` per compile, so those files drop their `/arch:AVX2` in tuned configs (MSVC compiles AVX2 intrinsics under
  any `/arch:` level and the dispatcher runs them only on AVX2 hosts).
- Shipped binaries are portable: the AppImage, macOS, Windows and Android builds pass `-DDSD_ENABLE_NATIVE=OFF` (the
  Android presets set it), CI's mbelib-neo dependency builds and the vcpkg `mbe-neo` port pass
  `-DMBELIB_ENABLE_NATIVE=OFF`, and `perf-bench` keeps it off. The AUR PKGBUILDs pass nothing: makepkg's `CFLAGS`
  `-march=` keeps the user's `makepkg.conf` choice. `TOOLS_NATIVE_ARCH` covers the rules.

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
  vocoder (`mbe-neo` 2.6+).
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
  `dsd_channel_modes_rigctl_request_is_session()` (`src/core/util/channel_modes_rigctl.c`, issue #621) says whether a
  passband asked of a rigctl peer now is the session's or a scan row's, for every rigctl leg (the engine's tunes, live
  apply, start and reconnect ask and legacy leg, and the IO manual tune): the scan scope's answer
  (`dsd_scan_mode_rigctl_request_is_session()`), except that a typed `-Y` list configured on the state is a scan's
  before its first row's scope is entered, which the scope cannot tell and runtime cannot see. Core is the lowest layer
  that sees both the scope (runtime) and the row modes, and both the engine and `dsd-neo_io_control` link it. Tests:
  `CORE_CHANNEL_MODES`, and the legs' own (`ENGINE_TRUNK_RETUNE_REGRESSION`, `IO_RIGCTL_CONTROL`, which build the file
  in, and `ENGINE_NO_CARRIER_RESET`).
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
    is best-effort, and reading the scope in force, `dsd_scan_mode_row_options()`, never a `-Y` tune staged since): the
    row's own width, or for a row that goes back to the default the configured width of its kind, else the AM default,
    or for NFM `-B` or the peer's own passband (issue #621);
  - the stream restart for a gain. One P25 SM tick guard hold covers the reopen and, when it fails, the gain's rollback
    (`dsd_engine_trunk_scan_restore_target_edit_locked()`) and `svc_rtl_restart_recovery_locked()`, which starts the
    input again without reopening an I/Q capture over its recording.

  The trunk restore otherwise waits for the tick guard rather than give up. `<dsd-neo/app_control/scan_row_view.h>` is
  what every frontend asks (row on air, fields, label, payload) and how they word the outcome. The terminal's squelch,
  width, gain and tone rows open a scope chooser through it (`scope_or_default()` in `menu_actions.c`, which the width
  rows hand their passband titles on a rigctl peer session); Qt's Radio and Tone filter sheets, and on audio input the
  monitor's Squelch and Passband sheets, offer "All channels | This channel" (`MetricsModel::scanRow*`,
  `CommandBridge::scanRowContext()`/`editScanRow()`), holding in "This channel" a control the row takes no edit of, and
  an Airspy's device panel, and taking the choice only once the metrics show the row the context names; rows are told
  apart by `scanRowKey` (scanner, session, row), not by name. Tests: `RUNTIME_SCAN_ROW_EDIT`, `ENGINE_TRUNK_SCAN`
  (`test_target_edit_*`), `ENGINE_CHANNEL_SCAN` (`test_row_session_edits`), `ENGINE_TRUNK_RETUNE_REGRESSION`
  (`test_rigctl_live_edit_apply_is_strict`), `APP_COMMAND_QUEUE` (`test_scan_row_edit_commands_*`),
  `APP_CONTROL_SCAN_ROW_EDIT`, `APP_CONTROL_SCAN_ROW_VIEW`, `UI_MENU_ACTIONS` (`test_scan_row_scope_chooser`),
  `UI_QT_METRICS_MODEL`, `UI_QT_CONTROLLER` and `UI_QT_QML_CALL_LISTS` (`tst_scan_row_edits.qml`,
  `tst_monitor_passband.qml`).
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

Release APK/AAB builds enable R8 in `android/package/build.gradle`.
`proguard-rules.pro` owns the named C++ → Kotlin interface contract and `DsdNative`
bindings; update it when adding a `QJniObject` upcall. `qt-rules.pro` owns the pinned
Qt kit's JNI keep rules, including unannotated native registration/teardown paths and
`QtApkFileEngine.JFileInfo`'s `relativePath`, `size` and `isDir` fields, which native plugin discovery reads from
Play's APK splits. The artifact check validates these fields' names, descriptors and instance storage;
re-audit on a Qt upgrade. `res/raw/dsdneo_keep.xml` preserves Qt's named resource
lookups. `tools/check_android_r8.py` validates shipped DEX definitions and bundle
metadata in Android CI; `ANDROID_R8` tests its failure cases. Device testing of
optimized releases and mapping retention are described in `android/README.md`.

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

Stop feedback (issue #718): `DecoderHostAndroid::stop()` (`decoder_host_android_lifecycle.cpp`) publishes Stopping
through `setSessionPhase()` before it dispatches the stop intent, when the tracker's phase is Starting or Running, so
`sessionState`, `transitioning` and Monitor's Stop button ("Stopping…") change on the tap instead of a later poll.
`SessionPhaseTracker::note_stop_requested()` (`session_state_map.h`) binds the request to the start attempt being
followed, not to a session id: the previous session's IDLE record, the new id's STARTING, RUNNING and STOPPING all
publish Stopping until the attempt's terminal record (Idle, or a failure, published as without a request) or a later
start attempt. `DecoderService` handles intents in order and runs a whole start inside one `onStartCommand`, so a
stop queued during configure is processed after it. Successful native cancellation turns RUNNING into STOPPING;
RUNNING records for `kStopGraceMs` (2 s) after the request, with no STOPPING, drop it, republish Running and log one
host diagnostics line. Times are `dsd_realtime_mono_ms()` read through `DecoderHostAndroid::readRealtimeMs()`, which
`UI_QT_ANDROID_HOST` replaces. The request lives in the host; a recreated
Activity maps the service state as before. Tests: `UI_QT_SESSION_STATE`, `UI_QT_ANDROID_HOST`,
`tst_monitor_call_actions.qml`.

Receiver endings (issue #647): `RunStatus` (`android/run_status.h`) keeps, per run, why it ended (`end_cause`: user,
detached, receiver failed, lifecycle) and the local receiver it was configured on (`receiver_source`, from the last
`-i` at configure by `receiver_source_for_input()`: `rtl`/`rtl:` and `airspy`/`airspy:`, never rtl_tcp).
`nativeStop(userRequested, cause)` latches its cause and raises its stop flag together under the lifecycle-status
lock, and `nativeConfigure` clears that flag, the input-failure latch and the run record in the same block, so a stop
configure discards leaves no cause. Each `nativeLifecycleStatus` poll, and the run's collection, takes the radio
path's committed receiver ending (`dsd_input_failure_receiver_ended()`; never the raw failure latch, which an Airspy
reconfigure writes, rolls back and keeps streaming) as "receiver failed". The first cause accepted is kept: a user Stop
then an unplug during cleanup stays a user stop (no notice); an unplug then Stop now stays an unplug.
`nativeSessionUsesLocalReceiver()` answers from the same record; configure and the USB detach broadcast both run on the
main thread, so a detach during a start is answered by it once configure returned. `nativeInit` installs the 2000 ms
unplug-owner grace (`install_receiver_policy()`). `DecoderHostAndroid` turns the record's `endCause`/`receiverSource`
into `receiverHealth` (`{state: ok|lost|failed, source: usb|airspy, session}`; the base host always answers ok, so
desktop UIs never see an ending): a detach reads lost, a committed ending failed, an ending with no local receiver ok.
Such an ending publishes Stopping at once, and `SessionPhaseTracker::note_receiver_end()` holds it as an acknowledged
stop until that session's terminal record, with no `kStopGraceMs` expiry, however long the upload drain runs. An
accepted `start()` clears the health; an access request clears it only once no session is active (during the hold the
Monitor's LOST stays) and, for a session that ended Failed by that same ending, acknowledges the failure first, so
Listen shows one card. Each acknowledgement is also handed to `DecoderService.acknowledgeReceiverEnding()`, and a host
takes the service's `endingAcknowledged` mark from every record, so a recreated Activity's host starts from it. A native
claim error is kept with the receiver of the session it came from (source, and the ready connection's serial when the
error is first seen) and shown only while that receiver is the ready one. The USB record also carries the broker's
request (`pending`, `requestedSource`, `requestedSerial`, empty before the first request; `LocalDeviceState`), published
as `localDeviceRequestPending`/`localDeviceRequestedSource`; kind `refused` (the switch refusal) maps to no device
failure. The broker itself (`UsbSourceManager.kt`, `UsbRequestState.kt`: permission request generations and their 60 s
expiry, scoped attachment diagnostics, detach ownership) is described in `android/README.md` "USB-OTG". Known limits:
`receiverHealth` carries no Airspy serial, so the notice names only the kind and a recreated Reconnect asks for access
by kind (with two Airspys it may prompt for the other); a claim error's serial is the ready connection's when the error
is first seen, a proxy, so after a swap between two Airspys it can be kept with the wrong unit. Tests:
`UI_QT_SESSION_STATE` (`test_receiver_end_holds_stopping_through_long_running`), `IO_RTL_RUN_RESULT` (causes, sources,
the grace), `UI_QT_ANDROID_HOST` (`check_receiver_health`, `check_claim_error_scope`, `check_request_record`),
`UI_QT_CONTROLLER`, `ANDROID_ENGINE_STOP_JVM`, `ANDROID_USB_BROKER_JVM`.

Stop delivery (PR #723 follow-up): `DecoderService.stopDecoder()` also calls `requestEngineStop()` synchronously
under the service lock, before posting the intent, for RUNNING and STOPPING. The JNI stop only raises atomics; the
lock keeps another run from starting between choosing the session and signalling it. RUNNING is published after
`nativeConfigure()` has reset the flags; earlier requests still follow ordered intent delivery, and JNI's existing
startup latch covers the gap before the decode loop. The deferred intent carries `EXTRA_STOP_SESSION` when a session
already exists; a stale stop cannot cancel a subsequent run. Missing context and refused dispatch are diagnosed and
do not undo native cancellation. `onStartCommand()` signals notification stops before notification binder work;
`onTaskRemoved()` requests a user stop even without Activity destruction. Repeated stops reach native code, allowing
a user Stop to abandon a lifecycle drain. A START publishes `lastStartId` with its locked start/reject decision; the
worker captures it under the lock publishing IDLE and passes that value to `stopSelf()`, so completion cannot read
and stop a newer start. `ANDROID_ENGINE_STOP_JVM` now runs the actual service against a deferred-intent transport,
a controlled native worker and platform stubs.

Configuration backup (issue #580; the Qt side is under UI, Qt, "Configuration backup") adds five `DecoderHost`
methods. `quitApp()` and `engineBusy()` are called on the Qt main thread; the document copies are called from a worker
thread, which the rest of the interface never is.

- Startup apply: `ui_load()` runs `apply_pending_restore()` first, before any model reads a store (the native
  signal-search store opens later, from `SignalSearchModel`), and asks `engineBusy()` there.
  `DecoderHostAndroid::engineBusy()` (`decoder_host_android_lifecycle.cpp`) reads the engine atomic, then
  `DecoderService.stateName()`, and returns `engine_busy()` from `session_state_map.h`: busy unless the service is IDLE
  with the engine off, and busy when the state cannot be read. It never refreshes the host, because a refresh can
  deliver a USB attachment that starts a session. `commitRestore()` asks it too.
- `quitApp()` returns false without an Activity context; otherwise it posts `AppSupport.finishApp()`
  (`finishAndRemoveTask()`) to the Android main thread without waiting, and `DsdNeoActivity.onDestroy` stops the
  engine and the service and ends the process as for any finish. The base host answers false, and the model then
  leaves the confirmed restore waiting for the next start.
- Document copies run on `BackupModel`'s worker thread, never the Qt main thread (`decoder_host_android_documents.cpp`).
  A `content://` reference goes over JNI to `DocumentCopies.kt` through the application `ContentResolver` the
  constructor holds; a file URL or plain path takes the base host's copy. The read side refuses a provider-declared size
  over the cap before opening, counts the cap while copying (`DocumentTransfer.kt`), writes a temp file beside the
  destination and renames it; a stale destination is removed first, and `DocumentCopied` counts only when the file is
  there. A failure on this device's side (no held `ContentResolver`, a stale destination that stays, the temp file, a
  write to it through `DocumentTransfer.LocalOutput`, the rename, a copy reported without its file because the JNI call
  failed) is `DocumentLocalFailed` (`LOCAL_FAILED`), not the document's `DocumentFailed`; the codes cross JNI as plain
  ints, and the `document_transfer` suite checks that both sides name the same values. A save opens the document `wt`,
  else `w`; a file (not a provider's pipe) is cut back to the copy's length and synced. A save that fails once the
  document is open closes it with `closeWithError()`, so a provider reading a pipe or socket sees the failure rather
  than a short document, and deletes it; only a whole save closes normally.
- `abortDocumentTransfers()` (only `~BackupModel`) cancels the `CancellationSignal` of every provider call in flight,
  raises `DocumentTransfer`'s sticky flag, which every chunk checks, and then the base host's. The delete is skipped
  after an abort, so a save stopped partway leaves a partial document, which never opens as a backup.
- Tests: `UI_QT_ANDROID_HOST` (engine-busy decision, document dispatch), `ANDROID_DOCUMENT_TRANSFER_JVM`
  (`DocumentTransfer.kt` with no stubs, `DocumentCopies.kt` against provider stubs). Real providers, JNI and
  `finishAndRemoveTask()` need a device or emulator.

### Android screen modes

The Settings → Listening → Screen modes (issue #574; user-facing rules in `android/README.md` "Screen") are decided in
Kotlin, not QML: Qt for Android stops its event loop while the activity is paused or stopped, which is exactly when a
call must wake the screen. Every class below (`android/package/src/io/github/arancormonk/dsdneo/`) runs on the Android
main thread only. The decision is pure Kotlin with no `android.*` import, run on the JVM by `ANDROID_SCREEN_POLICY_JVM`:

- `ScreenPolicy.kt`: the state machine. Each input (the settings, session start and end, a status sample, focus,
  top-resumed, pause, stop, start, destroy, multi-window, a touch or other interaction, screen on and off, user present,
  a refused wake, a tick) runs one step: the change, then the snooze housekeeping, then the outputs `keepScreenOn` (the
  window flag), `dimmed` (the brightness override) and `holdLease`, and the next `deadline` at which an output changes
  by time alone. Time is an injected millisecond clock; nothing in it reads a system clock. `Mode` codes 0-3 are a
  contract with Qt's `AppPrefs::ScreenMode` and are persisted, so never renumber them, and `DELAY_CHOICES_SECONDS`
  matches `kScreenDelayChoices` in `app_prefs.cpp` (default 30). The policy is the only keeper of the last audible-audio
  stamp. The first readable sample of a session takes its stamp as new: `dsd_app_notification_reset()` clears the native
  stamp at every session end, so audio it reports (age 0 or more) was played in this session. That audio counts for
  engagement and the snooze, so Power pressed during a call before the first readable poll still snoozes, but it never
  wakes. A changed stamp with age -1 records the stamp and nothing else, and a sample with no stamp (no readable record)
  changes nothing.
  `wakeVerdictNeeded` (Off between calls, a session, the activity not visible, armed, the screen off by the broadcasts)
  is the predicate every wake is judged through. Android's verdict reaches the policy as a supplier, which it asks last,
  at most once, and only for a sample that would otherwise wake (new audio no more than 2 s old, not snoozed,
  `wakeVerdictNeeded`): no read for a sample with no record, an unchanged stamp, stale audio or a snooze. Audio that
  would wake but for an event of the same off still to come is held: the stop (armed with the screen off while DSD-neo
  is still visible), the first loss callback (after a screen-off broadcast that came first), or the screen-off broadcast
  (after an arming loss that saw the screen off). `stopped()` and `screenOff()` judge it the same way once nothing is
  pending, at most 2 s old by then, so the last of those events wakes; a change of arming (except the arming loss after
  a broadcast that came first, which keeps it), a screen-on and a session change drop it. Arming (DSD-neo was in front
  when the display went off) is decided only by the first foreground-loss callback of a sequence (focus, top-resumed,
  pause or stop, whichever Android delivers first), from whether the activity was top-resumed and focused just before it
  and the screen already off; regaining the foreground ends the sequence and clears the arming and any snooze. The
  policy's own screen state follows only the screen broadcasts and a refused wake (a loss callback judges arming from
  the live `isInteractive` the glue passes with it), so the sample taken just before a screen-off cannot wake a screen
  the user has just turned off (it can only be held, for the broadcast to judge); a screen-on DSD-neo did not cause, or
  `USER_PRESENT`, disarms at once. After DSD-neo's own wake the arming it carried survives the next screen-off only if
  that off comes no later than `max(leaseReleasedAt, wakeAt + screen-off timeout) + OWN_WAKE_GRACE_MS` (5 s), a lease
  still held at that off counting as released by it. That window judges only an arming the wake carried: an own
  screen-on records `wakeAt` only while armed, and the screen-off, every disarm and every fresh arming (an unlock, a
  regain, the first loss of a new sequence) clear it, so a later sleep in front after an unlock, or on a phone with no
  lock screen whose regain can land on either side of the wake's broadcast, arms afresh. In a between-calls mode with a
  session, a screen-off while audio is engaged (within the delay of the last audio) snoozes wakes until a full delay
  passes without audio. Each off is judged once, as of the moment the display went off, by whichever comes first: an
  arming loss that sees the screen already off, or the screen-off broadcast. The other one for the same off neither
  judges again nor adds a snooze (`offJudgedAtLoss` carries a loss's judgment to its late broadcast), so a call that
  begins after the first is never that off's snooze, and is held for a wake. Where no arming loss comes for an off
  (DSD-neo not in front, such as on the lock screen after its own wake, when Power is pressed during a call) the
  broadcast alone judges it. `wakeAllowed(audioMode, interruptionFilter)` allows a wake only in `MODE_NORMAL` with the
  filter `ALL` (or `UNKNOWN`); any other mode, a newer one included, blocks it. The two platform limits the README lists
  are documented in its KDoc.
- `ScreenController.kt`: runs the policy and carries its outputs out through `Effects`, only on a change and
  make-before-break (a keep-on flag being set, then a lease being taken, then a flag being cleared, the dimming, a lease
  being released), renews a held lease on every sample, asks for one pulse per `wakeSerial` step and turns a pulse that
  left the screen off into `wakeRefused()`, and keeps one scheduled tick in step with the deadline. Its `stopped()` and
  `screenOff()` take the verdict supplier as `sample()` does, since either can judge held audio. `Effects.pulseWake()`
  answers with a `ScreenWake` (`ScreenWake.kt`, plain Kotlin): `WOKE`, `ALREADY_ON` or `REFUSED`. It decides which
  screen-on is DSD-neo's own wake: the first after an outstanding pulse that answered `WOKE`, with no delivery-time
  cutoff. Android can delay `SCREEN_ON` while the lock screen or main looper is busy; the former 2 s cutoff incorrectly
  disarmed repeat wakes after a successful wake (issue #574 follow-up). A screen-off, unlock, Activity destruction or
  session change cancels the pending claim. Screen-off clears it before applying the policy, so a new pulse for held
  audio at that off keeps its own claim. A refused pulse leaves none, so the next screen-on is someone else's and
  disarms. `ALREADY_ON` is the screen turned on
  before its broadcast reached DSD-neo, as when the user or a notification turns it on and a status tick with a call
  runs before the queued `SCREEN_ON`: the policy, which counts the screen on only by that broadcast, still asks for a
  wake. Nothing was pulsed, so the controller claims nothing and tells the policy nothing; the broadcast then disarms
  as any foreign screen-on does (D7). An own pulse whose screen-on is still to come keeps its claim across such an
  `ALREADY_ON` result. `state` is a read-only view of the policy, so nothing bypasses the effects.
- `StatusFeed.kt`: the service's status poll as the policy sees it. Every running tick samples, even an unchanged
  record (a held lease is renewed per sample); a new session id starts a session; every path that stops the poll ends
  it, once. It keeps no stamp, so a synchronous sample the glue sends straight to the controller is never undone by a
  later tick.
- `ActivitySlot.kt`: the live activity by identity. Android can create the new activity before the old one's
  `onDestroy`, so a detach of anything but the current instance is ignored.

The Android glue only delegates:

- `ScreenSupport.kt` (an `object`) feeds one `ScreenController` and is its `Effects`. An entry point called off the main
  thread is logged and ignored. Its inputs: `configure(Context, Int, Int)` (`@JvmStatic`, JNI signature
  `(Landroid/content/Context;II)V`), which `DecoderHostAndroid::setScreenPolicy()` calls through
  `runOnAndroidMainThread`; `DsdNeoActivity` (attach in `onCreate`, detach in `onDestroy`; start, stop unless it is part
  of a configuration change, pause, top-resumed, focus and multi-window; `dispatchTouchEvent` swallows the events
  `touch()` names; a key's `ACTION_DOWN`, generic motion and Back count as interaction and are never swallowed);
  `DecoderService`'s 1 s status poll (`statusTick()` on every running tick, `statusStopped()` from the poll's two ending
  tails and `stopStatusPolling()`, which every teardown runs); and a receiver for `SCREEN_ON`, `SCREEN_OFF` and
  `USER_PRESENT`, registered exported. Exporting is needed because `USER_PRESENT` comes from SystemUI, not the system
  uid, so a not-exported receiver never gets it and the unlock would never disarm; it is safe because all three actions
  are protected broadcasts that only system-side senders can send. A failed registration is retried on the next
  configure, attach or tick. Before the screen-off broadcast and each callback that can lose the foreground it takes a
  synchronous sample (`DsdNative.nativeNotificationStatus()`), only for the session the poll is feeding
  (`DecoderService.runningSessionId()`). It hands every sample, the stop and the screen-off the supplier that reads the
  audio mode and interruption filter for `wakeAllowed()`, both binder calls on the main thread, so they run only when
  the policy asks. Each screen-on broadcast goes to the controller with the phone's timeout
  (`Settings.System.SCREEN_OFF_TIMEOUT`, 30 s when unreadable). The controller's clock is `SystemClock.uptimeMillis`,
  the base `Handler.postAtTime()` schedules the tick
  on. Effects land on the attached activity's window (`FLAG_KEEP_SCREEN_ON`; `screenBrightness` 0.01 or
  `BRIGHTNESS_OVERRIDE_NONE`), written only where they differ from the live attributes and re-applied after each
  activity callback, poll tick and synchronous sample, since Qt may replace them. A refused wake logs "Screen wake
  refused by Android; calls cannot turn the screen on" once per session, to logcat and as a host diagnostic.
- `ScreenLocks.kt`: the two screen wake locks, created once and not reference-counted, so one release undoes any
  number of renewals. The lease `dsd-neo:screen` (`SCREEN_BRIGHT_WAKE_LOCK`, acquired for 10 s and renewed by each
  sample while wanted) holds the screen on in Off between calls; it has no `ON_AFTER_RELEASE`, so on release the
  screen goes off on the phone's timer. The window flag cannot do that: the window manager's own lock for it carries
  `ON_AFTER_RELEASE`, so clearing the flag counts as user activity. The wake pulse `dsd-neo:screen-wake`
  (`SCREEN_BRIGHT_WAKE_LOCK | ACQUIRE_CAUSES_WAKEUP`, 1 s) turns the screen on; Android offers no way to turn it off.
  `pulseWake()` reads `isInteractive` before the pulse: a screen already on is not pulsed (`ALREADY_ON`); otherwise
  it pulses and answers `WOKE` or `REFUSED` by `isInteractive` after. This is the one file that names the deprecated
  levels.

Qt side: `AppPrefs` stores `screenMode` (`listen/screenMode`) and `screenDelaySec` (`listen/screenDelaySec`) under
one `screenPolicyChanged` signal, with the offered delays as the CONSTANT `screenDelayChoices`. A stored value off
either list reads as System default or 30 s, and with no stored mode the replaced `listen/keepAwake` switch reads as
Always on (on) or System default (off); neither read writes, and the legacy key is never written, so a downgrade finds
it intact. `DecoderHost::setScreenPolicy(int, int)` does nothing and `screenPolicySupported` is false by default; the
Android host overrides both. `qt_ui.cpp` applies the pair at startup and on every change. `SettingsScreen.qml` shows
`screenModeRow` and, for the two between-calls modes only, `screenDelayRow` ("Dim after", or "Turn off after" in Off
between calls), both through the `screenChoices` `ChoiceSheet` and hidden unless `screenPolicySupported`. Tests:
`ANDROID_SCREEN_POLICY_JVM` (`ScreenPolicyTest.kt`, `ScreenControllerTest.kt`, `ScreenTouchTest.kt`,
`StatusFeedTest.kt`, `ActivitySlotTest.kt`; `ScreenLocksTest.kt` against a `PowerManager` stub, with `ScreenWake.kt`),
`UI_QT_PERSISTENCE` (`test_screen_policy_prefs()`), `UI_QT_QML_CALL_LISTS` (`tst_settings_screen_policy.qml`) and
`UI_QT_ANDROID_HOST`.

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
A binding that mixes an item's own state (`visible`, `enabled`) with `Navigation.allows()` reads the gate first
(`Accessible.ignored: !navigationAllowed || !visible`): the gate's walk up the parents can settle an ancestor's pending
`parent:` binding (MonitorScreen's tier groups), and state read before it would change under the running binding, a
binding loop. A size bound to a `Text` reads its `height`, not `implicitHeight`, whose getter lays out pending text and
changes itself under the binding reading it. Deferred work in a component that can be torn down with a call in flight
runs from a `Timer` it owns, not `Qt.callLater`, whose queued call outlives the object (`FollowLatest.qml`). The QML
tests pin these with `failOnWarning`.

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

### Receive admission and service identity (#637)

- `core/receive_types.h` owns bounded filter bitsets, naming rules and received identities. Runtime's
  `receive_policy.h` parses/validates CLI, INI, CSV and command edits; the frame path only matches pre-parsed data.
  `core/receive.h` supplies explicit protocol/slot admission from validated DCC/RAN/NAC/tone evidence. A P25 Phase 2
  seed without proof and NXDN stand-in values stay unknown. Shared-vocoder invocations carry their owning protocol
  through `dsd_state::receive_context_*`, since YSF/dPMR temporarily borrow another protocol's decoder.
- Core audio, recording and event paths combine receive admission with existing policy/provenance gates. Internal
  acquisition continues while output is Checking/rejected. Scanner and trunking activity cannot turn rejected media into
  a hold. Restoring a scan target preserves acquisition hints but clears its current received-code proof. The P25 and
  DMR trunk state machines keep `dsd_receive_carriers` (the code last verified on each voice frequency, 16 entries) and
  skip a grant whose remembered code the current policy rejects; a channel never verified still gets the acquisition
  window. Receive admission gates only holds and output: decode-progress evidence (the P25 reacquire watchdog, CQPSK
  timing memory, the CQPSK retry) runs whatever the verdict. The P25 conventional-activity hook carries its call's
  protocol and slot. `RECEIVE_SET` takes the P25 SM tick guard (its boundary ends calls and rotates WAVs). The
  single-slot DMR decoder (`dmrMS()`, `-fr`/`-nm` conventional on a repeater) decodes every burst on lane 0, so
  `dsd_state::dmr_mono_rx_slot` keeps the over-the-air slot its CACH TACT proved; receive identity uses it for that lane
  (Checking while unproven) and two-slot decoding, direct mode and a carrier boundary clear it. CSBK-tuned DMR carriers
  (Cap+, Connect Plus) get the same remembered code and bounded receive window as followed grants, and DMR BS voice, PI
  and P_Clear stamp `last_vc_sync_time` only for admitted slots, so rejected traffic never holds a channel. A trunk
  voice-channel tune forgets the carrier codes, so the control channel's code neither admits, names or splits the voice
  channel's calls nor is remembered as its code: while any receive filter is enabled with their confirmation gates
  (`dsd_engine_forget_carrier_codes()`), otherwise only the published values
  (`dsd_engine_forget_carrier_code_values()`), keeping the DMR confidence lock so call start is not delayed. YSF and
  scrambled dPMR play through NXDN's decoder under its synctype; they judge playback as their own through
  `dsd_receive_context_begin()/end()`. A call first seen while Checking withholds VOICE_START until it is admitted
  (`dsd_call_event_lifecycle::start_alert_pending`). The terminal hides a Checking/rejected call's identity, alias and
  location like the shared call view (the NXDN line's decoder alias too, by the live verdict when no call is held), and
  names NXDN and NFM calls by their service label on the NXDN and Rx tone lines. Two identities differ when their
  verified codes or their proven over-the-air slots differ (`dsd_receive_identity_changed()`), so a single-slot DMR move
  from TS1 to TS2 ends the call; a call keeps its proven slot while the next one is pending
  (`dsd_receive_identity_merge()`). A vocoder frame the policy has not admitted opens no call and marks no media, but
  the call already held still selects its key (`mark_vocoder_call_media()`), and a policy edit closes only the ended
  slots' MBE captures. A tuned release (`dsd_engine_release_tuned_call_state()`) ends the NFM call first, so its WAV is
  named against its own row. The trunk state machines remember a voice channel's code only when its tune forgot the
  control channel's (`receive_codes_fresh`, set by every tune through `dsd_engine_update_vc_tune_state()`). A P25 voice
  start while Checking is accepted for its PTT crypto fields but marks no activity; a followed TDMA slot the policy
  rejects is retired (grant, Phase 2 audio permission) and starts the hangtime. A direct-mode DMR sync word names its
  slot (`dsd_state::dmr_dm_sync_slot`); other MS-sourced bursts stay Checking under a slot filter. Legacy `-Y` judges a
  two-slot carrier on both lanes, and `dsd_engine_trunk_scan_receive_expired()` lets the frame loop yield on a
  conventional trunk-scan target the policy rejects. A standalone DMR data burst is judged by its own CACH slot
  (`dmr_data_sync()` sets the mono proof aside for it), an MS-mode data burst takes its lane's slot as its voice does,
  the LC path holds the voice channel only for admitted slots, and enrichment (`dsd_event_enrich_epoch()`) skips a
  Checking/rejected call. Anonymous history rows (no target or source) heard on different frequencies never merge;
  analog rows (`Row::analog`) are not told apart by their label, so a confirmed tone folds into its fragment. Code
  freshness is decided at the tune (`dsd_state::receive_codes_fresh_freq`), a mono-slot proof survives a standalone data
  burst only on the same carrier, an interrupted CACH proves no slot, and a trunk-scan conventional target whose
  admitted activity hold (data included) still runs is never released by the receive check. The receive policy boundary
  runs after the new policy is in force: it ends only calls it rejects, relabels the rest, and forgets codes a voice
  channel inherited when filters come on. It drains no Phase 2 playout: a rejected slot's queue is discarded with its
  call, and an admitted slot keeps its queue and 4V/2V pairing. SDRTrunk JSON playback carries no colour code
  (`dmr_color_code` 16), and the Qt call history compares a proven over-the-air slot apart from the access code
  (`receiveSlot`, persisted on its own; a pending slot's fragment joins the one that proved it), as the core history
  merge does (`dsd_receive_identity_merge()`). `-Y` carrier-wide admission (`dsd_receive_carrier_allows()`) judges both
  over-the-air slots whatever the single-slot decoder proved for its lane. The RTL live read bounds an idle NFM
  monitor's read by the decoder's live options (`dsd_rtl_stream_read_ex()` `live_opts`), not the orchestrator's
  start-time copy, so a live AM/NFM switch takes effect. A call reacquired after a sync loss keeps its confirmed receive
  identity (`dsd_call_state_observe()`), so a different code decoded next ends it. P25 Phase 2 voice the policy is still
  checking keeps decoding, muted and unqueued (`dsd_p25p2_decode_policy_verdict()`), so cipher positions stay in step
  for the frames admitted once the NAC is proven. A `-Y` scan is held by an admitted call only while it is still heard
  (its latest observation within the voice hold). A decode-mode change ends an NFM call through its own lifecycle
  (`ui_end_calls()`), so its recording keeps its name. Hytera XPT and Capacity Plus status selection skip candidates the
  receive policy rejects.
  The API's empty-row fingerprint has receive kind "none"; the Qt seen map tracks receive evidence and the
  service name, and a tg-0 row's name that is only a label (service, channel) is no destination of its own
  (`row_textual_destination()`). Holds from an admitted call (`dsd_call_state_admitted_call_active()`, the `-Y` receive
  leave) need it still heard within the hangtime or voice hold; followed DMR data's monotonic stamp counts too. A
  retained `-Y` call is judged by the identity it was received with (`dsd_receive_identity_admitted()`), so a sync loss
  that forgets the live code keeps its hold. Signal search judges P25 evidence by the reporting synctype and voice slot
  (`dsd_signal_search_note_evidence_at()`). P25 Phase 1 HDU/LDU/TDULC and Phase 2 voice stamp `last_vc_sync_time` only
  for admitted traffic. Trunk scan's no-carrier pass keeps a target's DMR confidence lock but forgets the published
  codes (`dsd_engine_forget_carrier_code_values()`), so the next transmission proves its own. X2-TDMA MBE capture judges
  each frame by its received slot (`dsd_mbe_out_write_rx()`). Under `-nm` a standalone burst the single-slot output
  folds onto lane 0 is judged by its own CACH slot (`dmr_data_collect_cach()`), and a standalone burst the policy does
  not admit leaves nothing in its lane's position cache (`dmr_lrrp_gps`). `dsd_engine_trunk_receive_release_due()` lets
  the frame loops hand a trunk voice channel only rejected traffic holds to noCarrier()'s control-channel return (it
  needs a REJECTED verdict: traffic still Checking is left to the P25/DMR trunk state machine's acquisition window, or
  on other trunked protocols (NXDN) gets a 4 s window from the tune, and admitted traffic or one no filter judges keeps
  the channel); an admitted call in progress (`dsd_call_state_admitted_call_active()`, data included) keeps it, as it
  keeps a `-Y` channel with voice-only off. The single-slot DMR lane is re-proven when a burst names the other slot. A
  voice LC header the single-slot output folds onto lane 0 applies that rule once the lane's call took it (its epoch or
  revision moved during the dispatch, on the header's slot; `dmr_data_header_opened_lane_call()`), instead of the voice
  lane's restored proof, so the call it opens on the other slot is not ended by the previous call's proof; its `-J` line
  names the row's proven over-the-air slot (`watchdog_event_log_slot_number()`), the lane only while none is proven. A
  rejected Phase 2 call's queued audio never plays under a talkgroup hold. A P25 start while
  Checking keeps its identity and epoch (its PTT crypto fields survive the NAC proof). A segment staged while admitted
  commits when a live edit rejects its call. A suspended scan scope (CONFIG_APPLY) defers the boundary to
  `dsd_scan_mode_resume()`. Signal search ignores rejected or checking traffic, and the terminal names an admitted call
  by its service.
- Canonical calls carry a separate service label, received identity and admission verdict. A confirmed identity
  change starts another epoch; completed event/history rows retain their labels. Frontend/API history merging checks
  confirmed service identities. Channel labels, numeric IDs and imported files keep their existing meaning.
- Trunk-scan departures end the outgoing target's calls before its snapshot is saved, so rows and per-call recordings
  (the NFM reception's included, which lives outside the snapshot) close under its own label and names: the visit cap
  and an operator's advance or avoid release the carrier (`trunk_scan_advance_releasing()`; an operator's departure from
  a park still pending, or one that never landed, is an ordinary one), and every other departure ends the calls in
  `trunk_scan_switch_to()` and re-saves the rollback snapshot. `dmr_sm_abandon_carrier()` keeps the destination an
  ordinary release would return to (a Cap+ rest channel a voice LC named). `runtime/control_pump.h`
  `dsd_runtime_controls_need_frame_loop()` (installed from `dsd_app_commands_need_frame_loop()`: any queued command but
  those on `k_deferred_commands` (display, audio, keys and decryption, recording, front-end slider tweaks), with
  `DSP_OP` and `AIRSPY_SET` judged by their payload) lets `dmrBS()`, which holds the decoder for a whole repeater
  carrier, return to the frame loop at a burst boundary; other commands wait for the carrier to end. EDACS analog voice
  (`edacs_analog_call()`) keeps every command until its call ends: leaving the loop early ends the call, which cannot be
  re-entered before the next grant. SIGHUP shuts down as SIGINT does unless inherited as ignored, and the runtime
  config's snapshot comparison includes `no_signal_handlers_*`.
- `engine/analog_call.c` owns conventional NFM call/recording lifecycle in the engine scan-owner extension. Runtime's
  frame-sync hooks accept DSP monitor blocks without a backend-to-engine dependency. Speaker output is independent
  of reception and recording. The existing analog sink provides a separate 48 kHz call-WAV conversion lane;
  `dsd_analog_sink_end_call()` clears that lane at a service boundary without changing diagnostic raw capture.
  An NFM recording is published as `<date>_<time>_<nnnnn>_NFM[_SVC_<name>].wav` (no talkgroup, source or call type;
  `wav_rename_build_final_filename()`), chosen by the row's `systype`; a teardown that no longer has the row publishes
  it through `close_and_rename_analog_wav_file()` (`<dsd-neo/core/file_io.h>`, no service, no rdio export), so it
  is never left under its temp name nor given the digital shape.
  A live radio NFM read yields with zero samples after 100 ms without output; DSP marks that as an interruption,
  and the engine ticks the analog lifecycle even when the interrupted pass skips `noCarrier()`. The existing
  received-tone freshness deadline closes stalled calls and WAVs on the decoder thread. Replay reads keep their
  sample-driven behavior. `IO_RTL_RETUNE_PREPARE`, `DSP_INPUT_INTERRUPT` and `ENGINE_INPUT_BOUNDARY` cover the handoff.
- App-control receive commands validate complete edits before application. Row commands retain scanner session and
  row/target identity. `receive_view` shares checking/rejection text with Qt, the terminal and the API `status.receive`.
  The Qt ReceivePolicySheet uses existing modal/keyboard/discard behavior; terminal menu_receive uses one captured
  destination per prompt chain. Global defaults live in `[receive]`; `scan_mode` restores them after row overrides.
  Recording publication sanitizes service-name filename components; Rdio uses its supported talkgroup label without
  changing the numeric target.
- A service name only rejected traffic could carry (`dsd_receive_names_unreachable()`: an enabled filter of its kind, or
  the analog tone filter for a CTCSS/DCS name, rejects its identifier) is kept and reported, never refused, so a service
  can be blocked for a while. `dsd_scan_option_unreachable_names()` judges a row's options over the configured policies
  (`dsd_scan_option_baseline`, from `dsd_scan_option_baseline_configured()`, never the live `opts->receive` a row may be
  overriding), as the row's options apply. Reported at start (bootstrap), in composed config validation (a WARNING), per
  target or channel-map entry when a scan list loads (`csvChanWarnUnreachableNames()`, called once the map and its
  policies are final: after the command line, after the engine's or the interactive import, after a live reimport is
  adopted, never on an import's scratch state), in import previews (`dsd_scan_option_receive_summary_against()` through
  the inspectors' explicit baseline: `dsd_app_scan_csv_callbacks` `baseline`,
  `dsd_csv_inspect_channel_profiles_against()`), and in the decoder-thread notice of a receive, tone-filter or row edit
  (`dsd_app_receive_note_unreachable()` in `app_control/receive_note.c`; the note lists whole identifiers and the notice
  or summary before it gives way, `dsd_receive_note_append()`; a row edit's `dsd_scan_row_edit_result::options` are the
  options the row runs with the edit in, on air or from its next visit).
- An `options` cell a spreadsheet quoted (wrapped in double quotes, each quote inside doubled) reads as its content when
  it does not parse as written for any row class (`dsd_scan_options_parse_cell()`,
  `dsd_scan_options_cell_csv_quoted()`), so the reading never depends on the row's mode and every cell that loaded
  before keeps its meaning. The trunk-scan loader, the channel-map loader, the scan document's companion visitor
  (`dsd_scan_options_visit_cell_files()`, offsets into the cell's content) and the Qt channel-map bundle rewrite read it
  the same way; a rewritten companion path is bare or grouped by doubled quotes.
- `dsd_engine_end_calls()` (`engine/end_calls.c`) ends every call the way every end-all path must: the analog call
  through its own lifecycle first, so its per-call recording closes under the row it was heard as, then each slot's
  call, committing it. The UI end-all and ProVoice toggle, the `-Y` leave and step, the trunk release, the replay leave
  and the voice-tune clear use it.
