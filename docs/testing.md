# Testing Policy

DSD-neo uses automated tests, static analysis, sanitizers, and fuzz smoke tests
to reduce regression and security risk.

## Test Suites

The CTest suite includes focused tests for:

- runtime configuration, CLI parsing, rings, hooks, shutdown, and telemetry
- platform audio concealment, atomics, files, and timing primitives
- DSP filters, demodulators, CQPSK timing/carrier recovery, resamplers, SIMD helpers, and symbol paths
- IO capture/replay metadata, UDP/TCP metrics, RTL/Soapy controls, and retune
  behavior
- protocol behavior for DMR, M17, NXDN, P25 Phase 1/2, and trunking state
  machines
- FEC block-code helpers
- crypto helpers such as AES OFB
- core audio, CSV import, key handling gates, frame logs, and state init
- terminal UI menus, hotkeys, prompts, history, and meters

Run the default test suite with:

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug -j
ctest --preset dev-debug --output-on-failure
```

### Full-chain modulation decode tests

The `DECODE_IQ_*` cases (CTest label `iq-decode`) are end-to-end regression
tests for every supported modulation. Each replays a short I/Q fixture from
`tests/fixtures/iq` through `--iq-replay`, which drives the complete chain:
decimation, filtering, discriminator/CQPSK timing recovery, symbol slicing,
frame sync, and the protocol layer. Unlike `.bin` symbol-capture replay, which
begins after the demodulator, this covers the DSP front end as well.

```sh
ctest --preset dev-debug -L iq-decode --output-on-failure
```

Each decode case asserts on a decoded payload field (NAC, WACN/SYS, colour code,
RAN, callsign, site ID) rather than a sync count, so a silent framing or protocol
regression fails the test instead of merely moving a counter. The
`DECODE_IQ_*_AUTO_HUNT` cases are the documented exception, described below. The
whole set runs in about 7 seconds because I/Q replay defaults to
`--iq-replay-rate fast`.

Covered: P25 Phase 1 C4FM (control and voice), P25 Phase 1 CQPSK/LSM (control and
voice, plus a two-ray simulcast-impaired control channel), P25 Phase 2, DMR
voice, DMR Tier III control (including a CSBK-only RAS control channel replayed
with `-F`, colour code 0 — regression coverage for issue #348), NXDN48, NXDN96,
dPMR, D-STAR, YSF, EDACS, and M17.

The simulcast case (`DECODE_IQ_P25P1_CQPSK_SIMULCAST_CC`) is derived from the
clean CQPSK control-channel capture by summing a delayed (62.5 µs), attenuated
(-4.4 dB), frequency-offset (+1.5 Hz) second ray, so the composite sweeps
through the beat fading and frequency-selective ISI seen on real LSM simulcast
systems. Derived fixtures regenerate offline from the committed sources:
`python3 tools/build_iq_fixtures.py --derived-only` (no network or ffmpeg).

Fixture provenance and regeneration live in `tools/build_iq_fixtures.py`; see
`THIRD_PARTY.md` for sample attribution. After regenerating a fixture, re-verify
its decode margin (the original set still passed with ±45 counts of added
noise) and that a mismatched mode flag produces no match, so the assertions
stay robust rather than borderline. Sources that exist only as FM
discriminator audio are integrated back into complex baseband (FM demodulation
is invertible), so those fixtures exercise the same code path but carry none of
the original RF impairments. Where a genuine off-air I/Q recording exists (P25
C4FM/CQPSK, NXDN48/96, dPMR, TETRA) it is used directly. The TETRA SCBS
fixture preserves the Apache-2.0 upstream 50 kHz CS16 capture byte-for-byte and
also commits a deterministic 54 kHz derivative because the current CQPSK path
requires an integer samples-per-symbol rate. `DECODE_IQ_TETRA_REAL_AIR_SCBS`
asserts the independently reported MCC 250, MNC 13 and colour code `0x2C`.
`DECODE_IQ_TETRA_REAL_AIR_MARGINAL_CC` replays a second unmodified Apache-2.0
CS16 capture recorded during control-channel reacquisition at about 10 dB
in-channel SNR. It requires at least 15 clean MCC 250/MNC 13/colour `0x2C`
BSCH results from the two-second recording; the current run recovered 26 from
30 synchronized bursts. A deterministic complex conjugate of this capture
retains its noise, fading and ISI while reversing all differential phase steps;
`DECODE_IQ_TETRA_REAL_AIR_MARGINAL_CC_INVERTED` also recovers 26 identities and
requires at least 15. `tools/tetra/prepare_real_air_fixture.py` verifies the raw
SCBS, resampled, marginal, and inverted hashes; the adjacent provenance file
records the source commits, transforms, and parameters.

`dpmr_synth` is the exception to all of that: it is modulated from the CCH
reference vectors in `tests/protocol/dpmr/fixtures`, the same vectors
`DPMR_REFERENCE_VECTORS` decodes, as continuous-phase 4FSK at 2400 baud with
dPMR's ±1050/±350 Hz deviations. It exists because the off-air `dpmr` recording
carries no recoverable CCH — its Hamming syndromes sit at the random rate and its
4FSK eye is closed — so nothing decoded from it is a decode, and no integrity
check could be validated against it (issue #407). `dpmr_synth` is correct by
construction: every CCH CRC-7 in it passes, and `DECODE_IQ_DPMR_SYNTH` asserts
the identity the vectors encode. Its voice payload is deterministic filler, so
the run reports vocoder errors; the CCH is what it pins.

`DECODE_IQ_DSTAR_AUTO_HUNT` and `DECODE_IQ_P25P2_CC_AUTO_HUNT` assert on an `SPS hunt:` rotation line rather than a
decoded payload. The `dstar` (4 s) and `p25p2_cc` (2 s) fixtures are shorter than one full hunt rotation (~6 s at
48 kHz), so under `-fa` they cannot be expected to decode; what they can prove is that the hunt is not pinned on its
starting profile, which is the defect those cases cover.

`DECODE_IQ_YSF_AUTO_HUNT` covers the opposite direction, the one issue #391 names: a handler that reports its frames
validated nothing while decoding them rotates the hunt off live traffic. Every other fixture that exercises a
protocol which reports a verdict pins its frame type (`-fy`, `-fh`, `-fn`, `-fi`, `-fz`), so the hunt is inactive in
it. The `ysf` capture is 6 s — one full rotation — and decodes on the hunt's starting profile, so it can assert both
halves: the payload still decodes under `-fa`, and no `SPS hunt: trying` line appears at all. Reporting every YSF
frame unproductive drops the payload from 23 hits to 13 and steps the hunt to 20 sps partway through the capture.
`dsd_neo_add_iq_decode_test` takes the "must be absent" regex as an optional fifth argument.

`DECODE_IQ_P25P1_CQPSK_VOICE_AUTO_HUNT` covers issue #400, the same rule as the YSF case read the other way. A
handler's credit is bounded by what it read, and P25p1 reads 33 symbols when the NID fails against 134 of a
~180-symbol slot when a one-block TSDU decodes, so a channel that was decoding still lost the profile to the failures
between its frames. The `p25p1_cqpsk_vc` capture is already registered under `-f1`; under `-fa` it steps to 20 sps
partway through on `main` and finishes in dPMR, so holding the profile is what the negative half of this case pins.
Both halves were confirmed stable over repeated runs on `dev-debug`, `asan-ubsan-debug` and `tsan-debug`.

Its payload assertion changed in issue #388, and the reason is worth recording: it had been
`ALG ID: 0xC0 KEY ID: 0x3900`, which is not in this capture. The `-f1` preset emits no `0xC0` anywhere in it, reading
three clear-voice headers (`ALG ID: 0x80 KEY ID: 0x0000`) and six `Group Voice Channel User` grants. What `-fa`
produced was a corrupted read of one of those clear headers — the run decoded zero grants, two headers and 127 header
errors — and the case had been asserting that artifact. Holding the 4800/4 co-tenants off the frame brings the `-fa`
run in line with the native one (seven grants, four clear headers, irrecoverable header errors 3 → 0), so the
assertion is now the same real payload `DECODE_IQ_P25P1_CQPSK_VOICE` asserts under `-f1`. The lesson generalizes: an
AUTO case should assert a payload the native preset also produces, or it can end up pinning the corruption it was
meant to catch.

`DECODE_IQ_P25P1_C4FM_CC_AUTO_HUNT` covers issue #388 proper. P25 Phase 1 C4FM's own profile is where the hunt
starts, so the `p25p1_c4fm_cc` capture needs no rotation and gets none — the failure was entirely the company 4800/4
keeps. NXDN96's ten-symbol sync word and M17's alternating-run preamble both fire on P25 payload, and each false
frame consumes symbols the next sync needed, warm-starts the slicer from that payload, and takes the `lastsynctype`
that keeps the C4FM threshold tracker engaged between frames: 26 decoded NACs under `-f1` became none under `-fa`,
every P25p1 sync reading `NAC: 000 duid:EE`. With the span guard the capture decodes 23 of those 26 under `-fa`. The
three still missing are the frames ahead of the first accepted P25p1 sync, which is the earliest point anything can
know P25p1 is on the channel — an evidence-based guard cannot cover its own bootstrap. The case asserts presence
rather than a count, per the policy above, plus the absence of any hunt rotation.

`DECODE_IQ_M17_AUTO` covers issue #399. M17's own profile is where the hunt starts, so the `m17` capture needs no
rotation and gets none: it must publish the same identity under `-fa` that `DECODE_IQ_M17` asserts under `-fz`.
Before the fix it published nothing, because AUTO demanded an exact repeated preamble marker that real M17 never
presents, and because `-fz` switches the matched filter off through the global `use_cosine_filter` while AUTO left it
on over every M17 payload. A BERT transmission has no fixture; under `-fa` its first frames report unproductive to
the SPS hunt until PRBS9 locks, which is the price of a frame type that carries no CRC of its own.

The other half of #399 — that a capture containing no M17 does not decode any — is pinned in
`FRAME_SYNC_INTERNAL_HELPERS` (`test_m17_alternating_runs_alone_are_never_a_sync`) rather than as a reject case on
the `dstar` fixture. A reject case there was tried and removed: how far the hunt gets through a 4 s capture under
`-fa` depends on front-end and thread scheduling, so the run varies between builds and between presets, and the
assertion failed under `tsan-debug` while passing repeatedly under `dev-debug`. Only assertions that hold for every
schedule belong in an `-fa` decode case, which is why the AUTO cases here assert either a payload the capture always
reaches or a hunt line that always prints.

Reject cases assert the opposite: that a fixture produces *no* decode. `dsd_neo_add_iq_reject_test` takes a
`NOT_EXPECTED` regex alongside the usual `EXPECTED` one, so a run that printed nothing at all cannot pass by
default. `noise_floor` is 10 s of synthetic complex Gaussian receiver noise (seed 398, sigma 16 LSB, regenerated by
`python3 tools/build_iq_fixtures.py --derived-only`) with no signal in it whatsoever. The `DECODE_IQ_NOISE_FLOOR_*`
cases assert `Total audio errors: 0`, which is the count of vocoder frames the decoder synthesized: before issue
\#398's confirmation gate this fixture produced 89 of them under `-fn` and 49 under `-fi`, decoded from nothing.
They deliberately do not cover `-fa`, where how far the hunt gets is schedule-dependent.

There is no `-fa` hunt case on `noise_floor`, and the measurement behind that is worth recording. Issue #391's
remaining set — DMR, P25 Phase 2 and X2-TDMA, the handlers that report no verdict — is bounded by arithmetic
rather than by a verdict, so a replay assertion cannot see it. Under `-fa` the fixture completes six rotations in its
10 s; defeating the verdict gate in `frame_sync_sps_hunt_note_handler_consumption()` gives five, and every individual
`SPS hunt: trying` line still prints in both. Only the rotation *count* separates them, and how far the hunt gets is
exactly the schedule-dependent quantity that got the `dstar` reject case removed above. So the property is pinned
where it can be stated exactly, in `FRAME_SYNC_SPS_HUNT_FALSE_SYNC`
(`test_no_verdict_handlers_still_rotate_at_the_noise_cadence`): a handler consuming a dPMR FS2 frame's 372 symbols on
the default productive verdict still reaches its dwell at the cadence that matcher reaches on noise. Both bounds are
mutation-checked — raising the consumption past half the period, or tightening the period below twice the
consumption, pins the profile and fails the case. dPMR itself has since left that set, and the two cases beside it
cover what it does now: at the real 384-symbol frame cadence, which is inside the arithmetic's reach, a carrier whose
CCH decodes nothing still rotates, and one whose CCH decodes on three frames in four holds.

There is no `-fa` case on `nxdn48` for issue #445 either, and for the same reason plus one of its own. The guard
that issue added withholds the NXDN96 and M17 matchers on 4800/4 while a 2400/4 transmission has recently proved
itself, so seeing it work in a replay needs the hunt to complete a rotation *after* a proof — and the `nxdn48`
fixture is 6 s, about one full rotation, which is precisely the schedule-dependent regime that got the `dstar`
reject case removed above. The change also leaves the hunt's own accounting untouched, so an `-fa` case would mostly
re-assert rotation behaviour that did not move. The property is pinned exactly instead, in
`FRAME_SYNC_INTERNAL_HELPERS` (the matchers stand down inside the span and are live again past it, the level blend
still runs while the sync is withheld, the guard is scoped to its arming protocols and to the profile proved, and it
survives the `symbolcnt` wrap) and in `FRAME_SYNC_POLICY` (the predicate's truth table and both span boundaries).
`DECODE_IQ_NXDN48`, `DECODE_IQ_NXDN96`, `DECODE_IQ_M17` and `DECODE_IQ_M17_AUTO` are the guard that real acquisition
still happens; none of them carries a 2400/4 proof, so they exercise the un-armed path.

What that issue *did* measure is worth recording, because it says something about this suite's limits. The obvious
root-cause fix — letting a passing NXDN CRC report `DSD_FRAME_VERDICT_PROFILE_PROVEN` so a live call holds its rate
against rotation — makes the decoder worse, and no test here would have caught it. Restarting the dwell keeps
`dsd_state::sps_hunt_counter` away from the budget exit in `getFrameSync()`, and that exit is what runs the no-sync
hooks that end a call and clear the state the next one is assembled from. On the uncommitted 35 s four-channel
NXDN48 capture from issue #373, ten rotated replays per build (`tools/replay_ab.sh`) scored 75 NXDN48 syncs and 11
voice calls per run on `main` against 66 and 9 with the verdict change, every paired repeat worse. Single replays
cannot see this: the same build scores anywhere from 57 to 94 NXDN48 syncs run to run under `-fa`, which is wider
than the effect. Decode *volume* under the hunt is not covered by the `iq-decode` suite, which asks only whether a
build still decodes.

Known gaps and caveats:

- **TETRA** has focused CTest coverage for FEC/interleaving, MAC, MLE/MM/CMCE,
  SDS, channel information, TCH slot gating, and trunking state helpers (run
  with `ctest --preset dev-debug -L tetra --output-on-failure`). A deterministic
  trunking result matrix correlates asynchronous VC and CC-return requests,
  blocks an overlapping transition, commits successful completion, and rolls
  failed completion back to the prior channel state while retiring its frame
  gate. It also verifies that a failed VC reassignment or CC return preserves
  the previous VC identity, generic frequency caches, activity timestamps, and
  shared tuned flag, and that disabling trunking finishes in `IDLE`. The engine
  polls correlated TETRA completion before its frame-dispatch gate and while
  unsynchronized, so a failed request can roll back even when no TETRA frame is
  allowed to reach the protocol tick.
  A deterministic Synchronisation Burst fixture crosses the full 18 ksym/s
  pi/4-DQPSK chain. Its transmitter applies convolutional coding, RCPC
  puncturing, block interleaving, and then scrambling; the receiver reverses
  that order by descrambling before deinterleaving. This ordering matches the
  air-interface chain and is pinned by fixture reproducibility plus the IQ
  decode assertions. `TETRA_AIR_CHAIN_VECTOR` additionally decodes fixed
  120-bit BSCH and 216-bit SYSINFO type-5 literals through descrambling,
  deinterleaving, depuncturing, Viterbi, CRC, and field assertions,
  independently of fixture regeneration. The SYSINFO case uses the network
  scrambling seed recovered from BSCH and verifies the K216/a101 SCH-HD path;
  the BSCH case also proves that the former deinterleave-before-descramble order
  fails CRC. The fixture continues
  through a CRC-valid BSCH, NDB SCH-HD blocks, a standards-shaped MAC-RESOURCE
  Channel Allocation IE, and a known TCH/FS codeword. It verifies network
  identity, SYSINFO, assigned-carrier parsing, and recovery of all 292 TCH/FS
  type-2 bits. The sequence advances the BSCH timestamp across its NDB bursts
  and verifies that the allocated TN2 bitmap admits the TCH/FS burst; focused
  slot-gate cases cover all four TN/bitmap mappings and released allocations.
  Frame synchronization preserves the analog soft symbols preceding the NTS
  and SSB markers, so BSCH and NDB block 1 enter Viterbi decoding with the same
  soft-decision quality as the live second block. Symbol replay retains a
  hard-decision fallback when no analog history is available. A
  complex-conjugated fixture verifies the negative-polarity NTS/SSB patterns
  and normalizes the dibit MSB in captured/live blocks, control bits, and soft
  metrics before protected BSCH, SCH-HD, and TCH/FS decoding.
  A paired bad-CRC burst verifies rejection. The same accept and
  reject paths are replayed with deterministic 300 Hz carrier offset and 14 dB
  complex-AWGN SNR, covering carrier/timing recovery under a repeatable channel
  impairment. Two additional fixtures retain that 300 Hz offset and fixed noise
  realization to bracket protected-control decoding: at 6 dB SNR the BSCH,
  SYSINFO, and Channel Allocation assertions must all succeed, while at 4 dB
  synchronization may occur but none of those CRC-protected messages may be
  published. This is a deterministic synthetic regression boundary, not a
  calibrated receiver-sensitivity claim. A deterministic two-ray variant adds a second path delayed by one
  54 kHz sample (18.5 us), attenuated to 0.45 (-6.9 dB), offset by 3 Hz, and
  shifted by 2.2 radians. Its BSCH, SYSINFO, Channel Allocation, and TCH/FS
  replay cases cover frequency-selective fading and intersymbol interference.
  Two separate fixtures resample the waveform with +100 ppm and -100 ppm clock
  mismatches while retaining the nominal 54 kHz replay rate. The same four
  assertions then verify continuous symbol-timing tracking as the sampling
  phase drifts in either direction.
  `TETRA_IQ_FIXTURE_REPRODUCIBILITY` regenerates all ten deterministic TETRA
  waveforms and their replay metadata in a temporary directory, then requires
  byte-for-byte identity with the checked-in fixtures. This makes a generator
  change or a missing derived fixture fail CTest before decode expectations can
  silently continue against stale data.
  The CMCE/SDS unit cases also require declared optional information elements
  and SDS payloads to be complete before any call, floor-control, or short-data
  state is published. MM identity, authentication, location-update, and status
  fields follow the same atomic rule, including preservation of the last
  valid state when a replacement PDU is truncated. MAC fragment tests require
  an explicit RESOURCE start, a consistent colour code, and an in-bounds final
  payload before the reassembled TM-SDU passes through basic-link LLC and
  reaches MLE. The FCS-bearing fragmentation case validates the Annex C
  32-bit FCS and proves a corrupted replacement cannot publish a call or tune.
  `TETRA_MLE_CMCE` also covers SDS-TL ACK, REPORT, and TRANSFER references,
  conditional forward-address lengths, short reports carried by D-STATUS, and
  concatenated SDS both with and without SDS-TL. Concatenation cases exercise
  out-of-order delivery, extended references, duplicate suppression, conflicting
  duplicate rejection, invalid sequence values, and publish only after every
  part has arrived.
  It also decodes a frozen 88-bit CMCE/MLE vector containing the published
  `tetra-multiframe-sds` example's 64-bit PID-130 SDS-TRANSFER field, and
  independently checks message reference 32 and Latin-1 text `Ahoj` without
  using the local bit-field constructor.
  `TETRA_SDS_REFERENCE_MAC` starts from a separate published 272-bit MAC PDU
  from `smarek/kaitai-tetra-sds`. It verifies the MAC length boundary,
  BL-UDATA LLC removal, MLE/CMCE dispatch, message reference 32, and the same
  `Ahoj` text without a locally constructed protocol envelope. The source
  publishes no reproducible off-air capture provenance, so this is an
  interoperability regression rather than the remaining field-capture gate.
  The table 14.15 D-SETUP vector covers notification, temporary address, calling
  SSI, and calling-party extension in their standardized type-2 order. A later
  setup without those elements must clear their validity and the previous
  calling identity instead of leaking stale call metadata.
  The implemented CMCE downlink family requires the Annex E O-bit, including
  setup/release, call-response, status, information, restoration, facility,
  short-data, function-not-supported, and floor-control PDUs. When it is set,
  tests require every table-defined P-bit/value and a complete terminating
  M/type-3/4 chain before publishing state. The SDS case also exercises one
  unknown, bounded Type 3 element and rejects an incomplete replacement.
  D-FACILITY vectors follow the official Annex E.2 and E.3 layouts for two
  independently length-delimited SS-PDUs and the 12-bit "SS not supported"
  acknowledgement. They require the outer O-bit and publish no partial
  facility state when a later SS-PDU is truncated.
  The official Annex E.8 D-CONNECT vector adds the complete 93-bit CMCE body
  with a 42-bit SS-AL INVOKE1 ACK Facility Type 3 element. It verifies the
  standardized identifier and length envelope, the terminal M-bit, and atomic
  rejection when either the Facility value or terminal bit is truncated.
  Tables 14.18 and 14.19 vectors likewise cover the transmitting-party type,
  SSI, and extension carried by D-TX-GRANTED and D-TX-INTERRUPT. Missing
  optionals clear old floor identity, while a truncated conditional extension
  preserves the last complete event. Release and control-channel return clear
  all associated floor identities.
  Text cases cover Latin-1 to UTF-8 and UTF-16BE surrogate pairs, with malformed
  input preserving the previous message. GSM packed 7-bit text includes the
  default alphabet and escape table, with a cross-octet euro-symbol test.
  Other legacy code pages remain unsupported. The published decoder trace adds
  an independent SDS application result, but its upstream example says real
  data was removed and does not provide a raw capture with reproducible
  provenance, so the full off-air interoperability gate remains open.
  `TETRA_FIELD_CAPTURE_VERIFIER` tests the stage-5 evidence checker with a
  complete CC-to-VC-to-CC bundle and rejection cases for missing return,
  capture drops, and out-of-order decode milestones. The checker itself still
  requires a real v2 capture, provenance document, and realtime replay log
  before it reports an external bundle as accepted.
  `TETRA_ACELP_PIPELINE` additionally launches the persistent Python vocoder
  stub, exchanges both
  137-bit codec frames, and verifies that 480 PCM samples reach a WAV sink.
  `TETRA_ACELP_SHORT_OUTPUT` makes the subprocess return only 80 of the required
  240 samples and verifies that the decoder resets it without routing a partial
  frame. `TETRA_ACELP_TIMEOUT` holds the subprocess output open without returning
  PCM and verifies that the decoder leaves the blocking read within two seconds;
  Windows additionally terminates the complete shell/codec process tree. All
  three cases run through the Windows and POSIX subprocess implementations.
  `TETRA_ACELP_REFERENCE` decodes the GPL-3.0 telive “Hello Tetra” hardware-test
  sample through the speech 24x18 deinterleaver and class-specific RCPC path.
  It compares all 8,220 bits in the resulting 60 ACELP frames with output from
  the ETSI reference decoder. The adjacent 8 kHz mono PCM16 WAV contains the
  independently decoded 14,400-sample reference; source commits, package
  hashes, licenses, and artifact hashes are recorded in its `PROVENANCE.md`.
  `TETRA_REAL_AIR_BIT_TRACE` anchors BSCH and SYSINFO parsing to a public
  [Osmocom real-air decode trace](https://lists.osmocom.org/hyperkitty/list/tetra@lists.osmocom.org/thread/6DN55M2TG23RWBDOLGJYJPNQB337C4H6/).
  It verifies MCC 293, MNC 7, colour code 3, the frame timestamp, service detail
  `0x0D77`, and the standards-based 393.7125 MHz downlink carrier calculation.
  `TETRA_AIR_CHAIN_VECTOR` also checks the scrambler against a direct ETSI
  recurrence and CRC-decodes a captured non-zero-colour BNCH block. The
  redistributable raw SCBS IQ pair and marginal control-channel capture then
  exercise CS16 replay, modulation, synchronization, BSCH FEC/CRC and identity
  parsing end to end. A hardware retune capture remains outstanding.
  `TETRA_MM_DISPATCH` includes frozen hexadecimal PDUs for EN 300 392-7 tables
  A.4, A.26, A.30a, and A.31. They bypass the test-side bit-field constructor
  and independently pin the D-AUTHENTICATION, D-CK CHANGE, D-OTAR, and
  D-DISABLE field offsets.
  MM dispatch and mandatory-field vectors use the 4-bit downlink PDU type map
  from ETSI EN 300 392-2 V3.8.1, Table 16.75. The focused vectors cover
  location-update accept, command, reject and proceeding, group identity and
  acknowledgement, D-MM-STATUS, and function-not-supported; conditional cipher
  parameters must be complete before state is published. D-LOCATION UPDATE
  COMMAND also decodes its optional Address Extension MCC/MNC and validates the
  terminating Annex E type-3/4 chain before publishing the command.
  D-LOCATION UPDATE ACCEPT applies the same rule to its SSI, Address Extension,
  Subscriber Class, Energy Saving, and SCCH Type 2 elements.
  Official Annex E.10 and E.11 vectors cover the minimal LU Accept and the
  159-bit form containing Subscriber Class plus consecutive New Registered Area
  and Group Identity Location Accept Type 4 elements. The E.11 case requires
  both nested and outer M-bit termination before replacing the previous state.
  The official Annex E.9 Group Identity Downlink vector retains both attachment
  and detachment entries. Additional table 16.54 cases cover GTSI, visitor GSSI,
  and combined GTSI/visitor-GSSI conditional address layouts. The terminal
  status summary exposes the accepted group-entry count.
  Official Annex E.16-E.18 MLE vectors cover D-NWRK-BROADCAST with no optional
  data, an explicit zero-neighbour indication, and two CA neighbours. All
  mandatory fields for up to seven neighbours are retained; every neighbour's
  ten optional P-bits and declared values must be complete before the network
  broadcast snapshot is replaced.
  Official Annex E.19-E.24 MLE vectors cover D-NEW-CELL and D-PREPARE-FAIL
  both with and without their MM SDUs, plus D-RESTORE-ACK carrying the required
  CMCE D-CALL-RESTORE and D-RESTORE-FAIL. The mandatory outer O-bit is consumed
  before the SDU, malformed nested restore data cannot publish an ACK, and the
  table 18.10 D-CHANNEL RESPONSE fields are retained only for a conforming
  message with its reserved optionals absent.
  Table 18.4 D-NWRK-BROADCAST EXTENSION coverage retains serving-cell and CA
  neighbour channel classes plus regular and extended-carrier irregular
  channel details. Its six ordered Type 2 presence bits, repeated-entry
  counts, conditional ten-bit carrier extension, and forbidden reserved
  fields must all validate before the previous extension snapshot is replaced.
  Extended-PDU type 1 coverage decodes table 18.5 D-NWRK-BROADCAST REMOVE,
  retaining complete CA and DA whole-cell or selective class/channel removal
  records plus the serving-cell removal lists. Truncated nested records and
  any asserted reserved Type 2 field preserve the previous valid snapshot.
  Extended-PDU type 0 coverage decodes the complete table 18.3
  D-NWRK-BROADCAST-DA layout: serving DA identity/reselection/load/time, local
  CA cell, DA neighbours and their standardized optionals, channel classes,
  and irregular channels. Conditional carrier extensions, all nested counts,
  and forbidden inner and outer reserved fields validate before the previous
  DA snapshot is replaced.
  EN 300 392-7 tables A.31/A.32 D-DISABLE and D-ENABLE coverage requires the
  complete conditional 60-bit TEI, subscription identity, optional 160-bit
  authentication challenge, and terminating proprietary-element chain before
  changing enabled state. Legacy tests and display text that treated SDS
  length forms as separate CMCE PDU types have been removed.
  Legacy fields and dead tests derived from a conflicting five-bit MM PDU
  table have also been removed; downlink MM dispatch and fixtures now use the
  four-bit table 16.75 exclusively.
  EN 300 392-7 table A.26 D-CK CHANGE coverage validates SCK lists and DMO
  subsets, GCK lists, combined CCK/GCK activation, every time representation,
  reserved key types, and atomic rejection of incomplete demands.
  Tables A.1-A.4 D-AUTHENTICATION coverage retains complete 80-bit random
  challenges and seeds for demand/response, conditional mutual-authentication
  responses, result/reject fields, and bounded proprietary Type 3 tails.
  Table A.9 D-OTAR CCK Provide coverage retains the complete 120-bit sealed
  current and conditional future CCK values. It validates all-location, list,
  mask/selector and ordered-range location-area forms, rejects forbidden future
  key combinations and truncated fields atomically, and uses the four-bit OTAR
  subtype mapping from table A.85.
  Tables A.12 and A.15 cover GCK Provide/Reject, including individual/group
  session selection, complete repeated 152-bit sealed-key descriptors,
  GCKN/GSSI rejection references, group-association consistency, retry timing,
  and optional address extension. Tables A.16 and A.19 cover SCK Provide/Reject, including individual/group
  session selection, complete repeated 143-bit sealed-key descriptors, per-key
  rejection reasons, KSG reserved values, retry timing, and optional address
  extension. Tables A.20 and A.23 retain complete GSKO seed, version, sealed key,
  GSSI and reject fields. Reserved bits, reserved reject reasons, and incomplete
  O/P/M tails preserve the previous complete OTAR snapshot.
  Table A.24 covers SCK/GCK key association and disassociation, including GSSI
  lists and ranges, optional foreign-network addressing, reserved selection and
  grouping values, invalid response controls, and atomic malformed-PDU rejection.
  Table A.27f covers individual SCK, KAG member, SCK subset, all-SCK,
  individual GCK, all-GCK and GSKO deletion demands, including repeated key
  identifiers, subset bounds, optional addressing and invalid extension rejection.
  Table A.27h covers individual/subset/all SCK, individual/all GCK and GSKO
  key-status demands with validated subset bounds and optional addressing.
  Table A.27d covers DMO SCK subset and repeated individual-key activation,
  all four time forms, mandatory DMO-network MNI, and atomic truncation handling.
  Table A.30 covers NEWCELL DCK-forwarding results and complete conditional CCK
  information using the same validated table A.42/A.43 decoder as CCK Provide.
  Table A.30a covers CMG GTSI provision with optional foreign-network address.
  D-LOCATION UPDATE REJECT validates its O/P/M structure and publishes the
  optional Address Extension only after the full MCC/MNC and tail are present.
  Location-update proceeding, group identity, and group acknowledgement also
  require their O-bit and a complete Type 3/4 chain when optional data follows.
  A separate validity flag prevents the default zero Status Downlink value from
  appearing as received data; valid status is shown with its Table 16.92 label
  in the terminal channel summary.
- **ProVoice** and **X2-TDMA** have no usable public sample and are untested here.
- **dPMR** decodes only what its CCH CRC-7 verifies (issue #407), so the `dpmr` off-air capture publishes nothing:
  it carries no recoverable CCH, which is why the CRC was thought to be broken. `DECODE_IQ_DPMR_MARGINAL` pins that
  it still syncs and still publishes no identity; `DECODE_IQ_DPMR_SYNTH` is the accept case.
- **P25 Phase 2** asserts SACCH framing only. Full payload decode needs the
  system WACN/SYSID/CC via `-X`, which the public sample does not identify.
- Fixtures are timing-insensitive by construction. Do not add assertions that
  depend on wall-clock call-state timers, because `fast` replay compresses them;
  use `--iq-replay-rate realtime` for that.

### Qt frontend and QML screen tests

The `UI_QT_*` cases register only under `-DDSD_ENABLE_QT_UI=ON`. Most of them
link `Qt6::Core` alone and run anywhere Qt 6 is installed. `UI_QT_QML_CALL_LISTS`
(CTest label `qml`) is the exception: it drives the real `.qml` screens through Qt
Quick Test, so it also needs the **QuickTest** module — both its CMake package
and its QML plugin, which Debian and Ubuntu package separately as
`qml6-module-qttest`. Both are probed, and the test is left unregistered with a
configure-time `STATUS` message when either is missing, rather than failing the
whole project's configure step.

```sh
cmake --preset dev-debug -DDSD_ENABLE_QT_UI=ON
cmake --build --preset dev-debug -j --target dsd-neo_test_ui_qt_qml
ctest --preset dev-debug -L qml --output-on-failure
```

It carries its own headless environment (offscreen platform, software renderer)
in the CTest registration, so it needs no window server and no GPU.
On Windows, `uiDir` is published to QML as a `file:///` URL rather than a raw
drive-qualified path; otherwise `Loader.source` treats the drive letter as an
unknown network scheme. The Qt 6.10.3 MSVC validation built the complete
`dsd-neo_ui_qt` target and passed all 12 registered `UI_QT_*` tests, including
the real QML screen test. When Qt is not deployed beside the test executables,
prepend the selected Qt kit's `bin` directory to `PATH` before invoking CTest.

#### Build-time constants that gate a branch

`DSD_RR_APP_KEY` bakes the RadioReference application key into a generated C
source at configure time, so `dsd_rr_builtin_app_key()` is a compile-time
constant and a test can only ever see the configuration it was built in. A
keyless build — every developer machine and every CI job but one — cannot reach
the shipped behaviour where the baked key is authoritative and a stored override
is ignored.

Two things close that, and both are needed:

- `RadioReferenceModel::chooseAppKey(builtin, override)` is a static, public pure
  function taking both candidates as arguments, so every combination is asserted
  in any build. This is what catches a regression on a developer's machine.
- The `qt-ui-tests` job reconfigures the same tree with a dummy
  `DSD_RR_APP_KEY` and re-runs `UI_QT_RADIO_REFERENCE`. Only the generated
  one-line key file recompiles, so it costs a relink. This is what proves the
  chosen key reaches the SOAP envelope and the ignored override does not.

The pattern generalises: when a build-time constant selects a branch, take the
constant as a parameter somewhere testable rather than reading it in the code
under test, and reconfigure in CI to prove the wiring. A case that reads the
constant directly must assert *against* it (`hasAppKey() == baked`) rather than
assume a configuration, or it passes only in the one it was written in.

```sh
DSD_RR_APP_KEY=CI_DUMMY_KEY_not_a_real_credential cmake --preset dev-debug -DDSD_ENABLE_QT_UI=ON
cmake --build --preset dev-debug -j --target dsd-neo_test_ui_qt_radio_reference_model
ctest --preset dev-debug -R '^UI_QT_RADIO_REFERENCE$' --output-on-failure
```

#### Guards that gate which code compiles

`USE_RADIO` is defined only when the radio pipeline is available — an SDR
backend was found, or `DSD_FORCE_RADIO_PIPELINE=ON` — and it guards
declarations, not only call sites. A test that calls a `USE_RADIO`-only helper
from outside the guard therefore compiles wherever a backend is present and
fails only where the symbol is absent:

```
error: implicit declaration of function 'test_...' [-Werror=implicit-function-declaration]
```

The run-time form of the same mistake is quieter. A case that asserts on a
handler existing only under `USE_RADIO` — a queued `DSD_APP_CMD_MANUAL_TUNE`,
say — watches the command drain with no handler in a radio-off build, so the
expectation stops holding without anything failing to compile.
`tests/ui/test_ui_cmd_queue.c` shows the split: validation cases that hold in
any build stay outside the guard, cases observing a radio handler sit inside it.

`backend-matrix (neither)` builds and runs ctest with both SDR backends off, on
pull requests as well as pushes, so either mistake fails the pull request rather
than the default branch. Reproduce that configuration locally when a change
touches a `USE_RADIO` guard:

```sh
cmake --preset dev-debug -DDSD_ENABLE_RTLSDR=OFF -DDSD_REQUIRE_RTLSDR=OFF \
  -DDSD_ENABLE_SOAPYSDR=OFF -DDSD_REQUIRE_SOAPYSDR=OFF
cmake --build --preset dev-debug -j
ctest --preset dev-debug --output-on-failure
```

Clear the `DSD_REQUIRE_*` flags alongside the `DSD_ENABLE_*` ones. Against a
cached build tree, configure otherwise stops with
`DSD_REQUIRE_RTLSDR=ON requires DSD_ENABLE_RTLSDR=ON.`

## Continuous Integration

GitHub Actions runs tests and quality checks on pull requests, primary-branch
pushes, tags, schedules, and manual dispatches. Coverage varies by event:
cross-platform builds, sanitizer tests, static analysis, workflow linting,
secret scanning, OSV scanning, repository guardrails for secret redaction and
workflow source/download pinning, fuzz smoke tests, and install/package
validation run where their workflows declare those events. Dependency review is
PR-only, release tag validation is tag-only, and extended fuzzing is scheduled
or manually dispatched. The backend matrix builds and tests all four
SDR-backend combinations — `both`, `rtl_only`, `soapy_only` and `neither` — on
pull requests as well as pushes, so the radio-off build is proven before a merge
rather than after one.

## Decode-Quality A/B on Real Captures

The `iq-decode` suite answers whether a change still decodes; it does not answer
whether it decodes *as well*. Symbol-timing, slicer and demodulator changes need
the second question answered, and one replay cannot answer it: decoding runs on a
threaded pipeline, so how much of a capture gets decoded varies run to run by more
than the effect being looked for.

`tools/replay_ab.sh` replays one I/Q capture through two or more builds and
`tools/replay_ab_report.py` reports the result:

```sh
cmake --build --preset dev-debug -j --target dsd-neo
cp build/dev-debug/apps/dsd-cli/dsd-neo /tmp/dsd-neo.after   # and one for 'before'

tools/replay_ab.sh --capture ~/captures/nxdn.json --mode -fi --reps 12 \
    --out /tmp/ab /tmp/dsd-neo.before /tmp/dsd-neo.after
tools/replay_ab_report.py /tmp/ab/summary.tsv
```

Reading it:

- **Errors per decoded voice frame**, never the raw error total. A build that
  loses sync decodes fewer frames and accrues fewer errors without being better,
  so watch the `voice` column alongside the error rate.
- **Paired per repeat.** Builds run round-robin with the order rotated each
  repeat, because a fixed order credits the better slot to whichever build holds
  it. The report compares within a repeat for the same reason.
- **Run the baseline against itself first.** That control establishes the noise
  floor for the machine and the capture; a difference smaller than it has not
  been measured. On the captures behind issue #444 the floor was about
  0.1 errors per voice frame over 12 repeats.
- Keep the machine otherwise idle: `--rate realtime` is wall-clock paced, so a
  build competing with a compile is measured under different conditions.

- **When the frame count moves, match payloads before calling it quality.**
  Paying off the matched filter's group delay at every switch took errors per
  voice frame from 3.63 to 2.34 on `fiNXDN` (12 repeats, 12/12), with total
  errors down 41% and 9% fewer voice frames a run. That ratio alone does not say
  which frames changed. Matching the AMBE payloads in `-Z` logs of the two
  builds did: every subframe both builds decoded carried the same errors, and
  the errors that went away sat in frames only `main` produced -- the first
  frame after each switch-on, read from rewound samples, at four errors per
  subframe against under one for the rest. The frame count fell because those
  frames were never real decodes. Report both numbers, and say where the
  difference sits. The same run is the one quoted in PR #454, so the two do not
  drift apart.

- **A unit test cannot see a call-order bug in its caller.** The first cut of
  that seam recorded the sample in hand before priming from the history and then
  fed it again, so every switch-on read one sample twice for a window; the
  filter test passed because it primed in the right order itself. The property
  has to be checked where the order is decided, which is why
  `SYMBOL_MATCHED_FILTER_SEAM` drives `getSymbol()` rather than the filter
  module.

Issue #444 is the worked example, and the comment above `symbol_adjust_timing_nxdn()`
in `src/dsp/dsd_symbol.c` records what it ruled out. A closed timing loop was
built and measured against that slip as part of the same issue and is not in the
tree: on `fiNXDN` it was worth about 0.18 errors per voice frame, but the anchor
it measures its phase against had to differ per protocol to work at all -- the
window centroid for NXDN48, the span edge for DMR, each breaking the other -- so
it replaced one fragile constant with two.

## Regression Test Requirement

At least 50% of bugs fixed in the last six months should include regression
tests. A pull request that fixes a bug should add a regression test unless:

- the behavior cannot be reproduced reliably in automation
- the fix is entirely documentation or packaging metadata
- a better guardrail exists, such as a static-analysis rule or workflow check

When no regression test is added for a bug fix, the pull request must explain
why.

## Major Functionality Test Requirement

Major new functionality must add or update automated tests. Major functionality
includes:

- public API or CLI changes
- protocol, FEC, crypto, or DSP behavior changes
- external file, network, radio, or capture input handling changes
- dependency changes that affect compiled code
- security-sensitive workflow or release changes
- installation and packaging behavior changes

## Coverage

Coverage can be generated with:

```sh
tools/coverage.sh
```

Do not claim a numeric coverage percentage without a current coverage artifact.
Vendored code under `src/third_party/` is excluded from project-owned coverage
accounting.

## Dynamic Analysis

For memory-safety-sensitive C/C++ changes, run sanitizer tests:

```sh
cmake --preset asan-ubsan-debug
cmake --build --preset asan-ubsan-debug -j
ctest --preset asan-ubsan-debug --output-on-failure
```

Threading-sensitive changes can use the separate TSan preset:

```sh
cmake --preset tsan-debug
cmake --build --preset tsan-debug -j
ctest --preset tsan-debug --output-on-failure
```

Fuzz-facing changes should run bounded libFuzzer smoke passes:

```sh
tools/fuzz_smoke.sh
```
