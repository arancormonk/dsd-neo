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

### Data-event CRC integrity

`DMR_EVENT_CRC` exercises real header, assembler, application decoder, history and event-file paths:
strict rejection, marked relaxed recovery, header-chain failures, encrypted notices, UDT text detail
lines, a BPTC-encoded RAS-shaped header, slot isolation and clean subsequent packets. Its unchanged
MNIS LRRP vector comes from [DSD-FME issue 283](https://github.com/lwvmobile/dsd-fme/issues/283).
The 46 received octets end in CRC `0FE81D14`; the two extra zero octets in that issue's dump are buffer
fill. The former CRC span computes `FF38D9C7`, while the corrected span matches the transmitted value.
A one-bit payload mutation must not publish in strict mode. With `-F` and `-L`, the clean captured MNIS
packet must append exactly one location row, while the CRC32-failed packet must append none.

`CORE_EVENTS_STAGED_PAYLOAD` checks that data-notice staging is independent of active-call enrichment.
`CORE_GPS_LRRP_CRC_GATING` checks that the real GPS decoders retain their state strings while suppressing
CRC-failed location rows, with slot isolation and both file date formats; the LIP vectors follow
TS 102 361-4 Table 6.79 and TS 100 392-18-1 Table 6.1. `DMR_USBD_LIP_LRRP_CRC` runs
BPTC-encoded USBD LIP bursts through the real burst handler in strict and relaxed/RAS modes, checking
decoded coordinates, file rows, unchanged state strings and CRC-flag restoration. `DMR_FLCO_MONO_CRC_SCOPE`
covers the mono-mode CRC carry, and `DMR_LIP_CARRIERS` covers UDT and UDP-carried LIP.
`DMR_LRRP_EVENT_CRC_GATING` checks LOCN (including the short-data entry point) and UDP ports 4001/49198
against the event CRC verdict, including the LRRP suppression summary and clean rows.
`DMR_LRRP_CRC_GATING` remains the token-level counterpart over `dmr_pdu.c`. `FEC_BPTC_RS` and
`DMR_LC_RS_VERDICT` pin RS(12,9) rejection of root-less, multiple-root and residual-syndrome words.

The MNIS coverage follows the independent
[node-dmr-lib assembler](https://github.com/rick51231/node-dmr-lib/blob/2a6579e3d3af8f0fde529028962d8fa0e89d2d1d/src/DMR/Util/DataBlock.js):
omit the first three proprietary-header octets and the octet immediately before CRC32; the stored
header already excludes CRC16. This is proprietary-format evidence, not an ETSI definition of MNIS.
Ordinary DMR CRC32 and the separate UDT header/message CRC16 domains follow
[ETSI TS 102 361-1 V2.6.1](https://www.etsi.org/deliver/etsi_ts/102300_102399/10236101/02.06.01_60/ts_10236101v020601p.pdf),
§§8.2.2 and B.3.8–B.3.9, corroborated by
[node-dmr-lib CRC32](https://github.com/rick51231/node-dmr-lib/blob/master/src/Encoders/CRC32.js)
and [MMDVMHost CRC-CCITT](https://github.com/g4klx/MMDVMHost/blob/master/CRC.cpp).

`P25_P1_MDPU_HELPERS` covers both packet rates, header versus packet failures, and CRC9-only failures
with a passing packet CRC32. Those domains are distinct in
[TIA-102.BAAA-A](https://qsl.net/kb9mwr/projects/dv/apco25/TIA-102-BAAA-A-Project_25-FDMA-Common_Air_Interface.pdf),
§§6.2–6.4, and independently in
[SDRTrunk's confirmed blocks](https://github.com/DSheirer/sdrtrunk/blob/master/src/main/java/io/github/dsheirer/module/decode/p25/phase1/message/pdu/block/ConfirmedDataBlock.java).
`CORE_CALL_ALERT_HISTORY` covers marker placement, warning severity, long notices, detail lines and
delayed/reacquired voice history. Strict suppression is decoder policy, not a specification requirement
for passive-receiver user interfaces. A failed standard CRC can also be intentional:
[Motorola's private-CRC patent](https://patents.google.com/patent/US8914699B2/en) and
[independent RAS documentation](https://cwh050.mywikis.wiki/wiki/Restricted_Access_to_System) explain why
suspected RAS is marked as a failed check rather than asserted to be corrupted or authenticated.

### Known-key MBE playback

`CORE_KEY_DIRECT` also checks recovered AMBE plaintext for all-zero RC4, AES-128 and AES-256 keys through CLI,
typed live commands and legacy live commands, in both slots. `UI_KEY_CLEANUP` observes production volatile erasure
while handler and TYT buffers are still alive.

`UI_QT_ANDROID_HOST` runs Android host lifecycle publication with a desktop transport fixture. `ANDROID_LOCATION_JVM`
runs the production Kotlin location broker and geocoder queue with deterministic platform stubs, without an Android
build or emulator. It uses `kotlinc` or a cached Gradle Kotlin compiler plus Java; missing tools skip the CTest entry.
Android CI runs `python3 tests/android/run_location_tests.py --require-tools` after its APK build, when the compiler
is cached. Qt persistence tests use disposable directories and do not require a writable home on Linux.

`CORE_MBE_FILE_IO` checks decrypted AMBE payloads, not merely output-file existence. Its NXDN vectors come
from [NXDN TS 1-D v1.3](https://www.qsl.net/kb9mwr/projects/dv/nxdn/NXDN-TS-1-D_v0103.pdf),
§§7.2.1.1–7.2.1.3: every EHR frame decrypts to the published 1031 Hz tone. Scrambler repetitions cover the
16-frame reset; DES/AES cover the 32-frame session. Missing-IV, changed-to-unloaded-key, manual-key versus
stale-CSV, and skipped-SACCH cases defend against stale crypto context and incorrect positioning.

The DMR AES-128/256 and RC4 excerpts come from the Baofeng DM-32 recordings in
[known-key-mbe-samples](https://github.com/tylerwatt12/known-key-mbe-samples/tree/2e15b86e305b7a3c71d52873518e2908747f6e09).
The tests compare exact plaintext voice bits independently checked with OpenSSL-backed AES/OFB and an
[RFC 6229](https://www.rfc-editor.org/rfc/rfc6229)-checked RC4 reference, both with serialized MI metadata and
with MI recovered from the preceding superframe's Golay/CRC-protected fragments. A late-arriving AES algorithm
without an IV must not corrupt the keyring or the plaintext of subsequent valid frames. The short vectors
are embedded in the test; running it requires neither a network nor a radio.

For full-capture verification, replay the repository's `encrypted.mbe` and `encrypted_legacy.mbe` files with
their supplied keys using the CLI examples in [cli.md](cli.md). Compare decrypted frame logs as well as audio:
PCM need not be byte-identical across vocoder versions or synthesis histories. These exports validate playback
and crypto, not RF reception or demodulator sensitivity.

### Full-chain modulation decode tests

The `DECODE_IQ_*` cases (CTest label `iq-decode`) are end-to-end regression
tests for every supported modulation. Each replays a short cu8 I/Q fixture from
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
dPMR, D-STAR, YSF, EDACS, and M17, plus the received CTCSS tone on synthetic analog NFM
under `-fA` (see "Received tone (CTCSS) on the analog monitor" below). The analog FM
monitor's audio has its own cases, which score the audio rather than a log line; see
[Analog monitor audio checks](#analog-monitor-audio-checks).

The simulcast case (`DECODE_IQ_P25P1_CQPSK_SIMULCAST_CC`) is derived from the
clean CQPSK control-channel capture by summing a delayed (62.5 µs), attenuated
(-4.4 dB), frequency-offset (+1.5 Hz) second ray, so the composite sweeps
through the beat fading and frequency-selective ISI seen on real LSM simulcast
systems. Derived fixtures regenerate offline from the committed sources:
`python3 tools/build_iq_fixtures.py --derived-only` (no network or ffmpeg).

`nxdn48_attenuated` is `nxdn48` replayed 20 dB down, for the per-row squelch cases
(`DECODE_IQ_SCAN_NXDN48_SQUELCH_*`, issue #521). The source is normalized near full scale, so
its channel power leaves no room for a closed threshold inside the `--squelch-db` range; 20 dB
down, the signal-bearing replay block measures -25.8 dB post-filter and the cases pin
thresholds at least 10 dB either side of it. The measurement is recorded beside the test
registration; re-measure it there if the fixture or the channel filter changes. The
`scan_mode_replay` host installs row zero's options and takes a configured squelch default from
`DSD_NEO_SCAN_REPLAY_DEFAULT_SQL_DB`, because I/Q replay has no other way to set one.

Fixture provenance and regeneration live in `tools/build_iq_fixtures.py`; see
`THIRD_PARTY.md` for sample attribution. After regenerating a fixture, re-verify
its decode margin (the original set still passed with ±45 counts of added
noise) and that a mismatched mode flag produces no match, so the assertions
stay robust rather than borderline. Sources that exist only as FM
discriminator audio are integrated back into complex baseband (FM demodulation
is invertible), so those fixtures exercise the same code path but carry none of
the original RF impairments. Where a genuine off-air I/Q recording exists (P25
C4FM/CQPSK, NXDN48/96, dPMR) it is used directly.

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

#### Received tone (CTCSS) on the analog monitor

`DECODE_IQ_ANALOG_CTCSS_1000`, `_670`, `_DROP` and `_NOTONE` (issue #522) replay synthetic NFM under `-fA -o null`, so
they also show detection needs neither audio output nor a tone policy. The fixtures (`nfm_ctcss_synth_1000`,
`nfm_ctcss_synth_670`, `nfm_ctcss_synth_drop`, `nfm_notone_synth`) are built offline by
`python3 tools/build_iq_fixtures.py --derived-only`: voice-band audio (noise limited to 300-3000 Hz, gated into
syllables) plus a 600 Hz-deviation sine at the named tone, through the existing `remodulate()`, with receiver noise
added at baseband. Each asserts the `Received tone:` log line and, as its negative half, that no other tone was ever
reported; CMake regexes have no negative lookahead, so "any other tone" is spelled out as every value that is not the
expected one. The drop fixture stops its tone at 1.2 s under a live carrier and must log `CTCSS 100.0 Hz` then `none`.
Each case's expected line appears only once the replay has run long enough to reach its verdict, so a replay that
ends early fails rather than passing. Replays that opened on quiet monitor audio used to do exactly that: under `-fA`
the sync hunt's modulation vote could pick QPSK and send the RTL front end a CQPSK symbol profile, after which the
stream carried symbols instead of monitor audio, the unsynced analog path (and the tap) never ran, and the replay
drained with nothing decoded. The analog family now stands the vote down, and the hunt sends the RTL front end no
symbol profile at all in analog-only mode, as the app-control modulation path does not; `FRAME_SYNC_INTERNAL_HELPERS`
pins both, with a digital session as the control.

The detector's own bounds are pinned in sample time by `DSP_ANALOG_CTCSS`, through the pure receive core
(`src/dsp/analog_rx_internal.h`) and seeded generators in `tests/dsp/analog_tone_synth.h`. Every figure it asserts
holds for its fixed seeds, and each row prints its measured share and p50/p95/worst for the PR evidence.

Every timing row also checks the CTCSS timing contract in `<dsd-neo/dsp/analog_rx.h>`: the lock rows (all tones at both
levels, the off-nominal rows, the tone under speech) keep their p95 within `DSD_ANALOG_CTCSS_LOCK_P95_MS` (400 ms) and
every start within `DSD_ANALOG_CTCSS_LOCK_CEILING_MS` (700 ms); the loss rows (both levels, and the reverse bursts
caught at +10 and 0 dB, which end a lock as a stop does) keep their p95 within `DSD_ANALOG_CTCSS_LOSS_P95_MS` (350 ms)
and every stop within `DSD_ANALOG_CTCSS_LOSS_CEILING_MS` (800 ms). Static asserts keep the fixed pins below (the 400 and
500 ms lock bounds, the 350, 150 and 450 ms loss bounds) inside the ceilings, so the checks that assert only a pin sit
inside the contract too. The loss ceiling sits above the slowest of the 800,000 stops in the ceiling-tail sweeps below
(738 ms). The lock ceiling may not exceed 700 ms, because the tone policy's 800 ms acquisition window (issue #527) has
to leave two hops beyond it; it sits above the slowest of the 2,000,000 starts at 0 dB in those sweeps (651 ms). In
noise a start can take longer than any fixed bound: the 250 ms window alone passed 700 ms on about one start in 125,000
at 0 dB, and the late acquisition windows (`ctcss_step_late()` in `src/dsp/analog_ctcss.c`) are what keep those inside
the ceiling. The p95 targets, the ceilings and those rates are what the
user guide states; the per-row pins are tighter and record what these seeds measure:

- Lock: all 50 tones at four input rates, at +10 and 0 dB in-band tone-to-noise, lock within 400 ms of an onset placed
  anywhere inside a hop. Tones off their table value by transmitter encoder error lock on that value: 0.2 and 0.35 Hz
  off within 400 ms at +10 dB, and the exact tone at 0 dB on a second seed set within 400 ms too; 0.2 Hz off at 0 dB,
  99% of the starts lock within 400 ms and the slowest at 468 ms (the row asserts at least 95% and 500 ms). The lock
  gate is 0.5 Hz, so encoder error trades against lock time. The 32 slowest of the 2,000,000 long-run starts at 0 dB
  as the 250 ms window alone locked them (8 exact tones and 24 tones 0.2 Hz off, from 701 ms to 1,128 ms), rebuilt from
  the sweep's own seeds, each lock within the 700 ms ceiling (the slowest at 509 ms). Under transmitter-filtered speech 10 dB above the tone
  every tone locks within 500 ms and holds that one lock to the end of the run; a tone held 15 s at 0 dB, at each rate,
  locks once and is never lost.
- Rejection: 67.0/69.3/71.9 Hz are each identified; off-table tones lock nothing over 3 s runs down to 0 dB in-band
  (150.0 Hz, and 68.2, 161.0 and 166.7 Hz between two table tones), nor do 100.85 and 100.9 Hz, just outside the snap
  gate of 100.0, at +10 and +20 dB; a locked tone that moves off the table is dropped within 450 ms (one window plus
  four failing hops) and nothing locks in its place; no DCS code locks -- every rotation class of the Golay (23,12)
  code, forward and bit-reversed, which is every periodic DCS waveform in either polarity, plus the nearest words at
  every rate, clean, at +10 dB and at 0 dB; and a minute each of unfiltered speech, transmitter-filtered speech and
  noise never locks and reaches the `none` verdict, which the speech runs hold for 87% of their carrier time (the rows
  assert 80%; each pause that closes the carrier starts the verdict again). Two more minutes of speech, one filtered and
  one not, in which a high voice holds a pitch near 225.7 or 229.1 Hz for most of a late acquisition window and then
  moves on, never lock: the late windows qualify that pitch, and only the check that the newest 250 ms still carry it
  keeps it from locking. The speech model is source-filter speech
  with a jittering, drifting, wobbling fundamental in 85-255 Hz. A steady voice-band tone alone on a clean carrier
  never locks and reads `none`, though decimation folds a residue of it onto the sub-audible band: every table tone is
  hit that way at 6, 8, 44.1, 48 and 78.125 kHz from either side of the first multiple of the decimated rate (the lower
  side only at 6 kHz, where the upper one is past half the input rate), and a spread of tones from the next three
  multiples wherever they lie below it (2300 Hz at 48 kHz lands on 100.0 Hz, 2333 Hz on 67.0 Hz). The other side of
  that check: a tone at +10 dB beside a voice-band tone 30 dB louder whose residue lands on the tone's own bin locks as
  itself within 400 ms, and when the tone stops the residue alone does not hold the lock (dropped within 350 ms).
- Loss and verdicts: every tone at every rate is dropped after it stops under a live carrier, at +10 dB 99% of the stops
  within 350 ms and all within 386 ms, at 0 dB 98% within 350 ms and all within 476 ms (the rows assert 98% and 400 ms,
  97% and 500 ms), and nothing locks again from what the detector still holds of the stopped tone; every tone at every rate is dropped within 150 ms of a reverse burst at +10 dB, a 180 degree one and
  a 120 and a 240 degree one alike, the flip landing anywhere inside a sub-block; at 0 dB, with a 180 degree flip held
  400 ms before the carrier drops, 91% of the bursts are caught within 150 ms and every caught one within the loss
  contract, and the 4 of 200 that are missed end with the carrier drop, within its 200 ms hangover (the row asserts 89%
  and at most 5 missed); a 120 or 240 degree burst inside the sub-block a tone locks on -- driven through the detector
  alone at +10 dB, every tone on its table value and 0.15 Hz either side, the step at every eighth sample of that
  sub-block -- ends the lock within 150 ms after every one of the 2,882 steps that still lock the tone on that hop, and
  after all but 15 of 2,852 when the tone has just moved there from another table tone it was locked on (248 of those
  300 tones lock in place of the old tone's lock, the rest just after losing it); the rows assert at most 2 and 20, and
  measured against the lock hop's own estimate 115 and 143 were missed; a tone that stops under a carrier that keeps
  dropping out (up for 20 ms of every 40 to 180 ms,
  a voice-band tone and noise between stretches of digital silence, so the carrier never expires) is dropped at every
  rate, period and block size, 95% of the 100 stops within 427 ms and all within 457 ms, and nothing locks again (the
  row asserts the 800 ms loss ceiling only, since the p95 target is for a live carrier); a carrier with no tone reads
  `detecting` until 500 ms of it and `none` by the next hop, and a tone that starts after that verdict still locks
  within the lock bound; the verdict, the no-tone one included, is identical at the three input scales and for any
  block size; the front end runs from 2400 Hz up to its 320 kHz limit and reports itself unavailable outside it.

Fixed seeds say what the detector does on those seeds, not how often it misses in the long run. The long-run figures
below come from wider seed sweeps run offline over the same generators and the same core (x86-64 at -O0 and -O2 give
identical results), and they are the numbers the user guide quotes:

- Lock, 10,000 starts each (every tone at every rate, onset anywhere in a hop): at +10 dB, p95 255 ms and none beyond
  400 ms (the slowest 331 ms); at 0 dB, p95 341 ms and 0.30% beyond 400 ms (447 ms); 0.2 Hz off at +10 dB, none
  (331 ms); 0.35 Hz off at +10 dB, 1 start (424 ms); 0.2 Hz off at 0 dB, p95 362 ms and 1.55% (553 ms).
- Loss, 4,000 stops each: 2.0% beyond 350 ms at +10 dB (the slowest 515 ms) and 1.25% at 0 dB (494 ms), and about 2%
  at +60 dB too: what is left after the stop is noise, and at the locked bin noise alone clears the 0.15 hold threshold
  on about one hop in eighty, whatever its level, which restarts the four-hop count. Making the hold stricter once a hop
  has failed cuts that tail to about 0.4%, but a held tone at -3 dB then drops 7 to 45 times as often, so the hold
  stays as it is. Reverse burst, 40,000 each: none beyond 150 ms at +10 dB (123 ms); at 0 dB 6.1% beyond 150 ms (the
  slowest 344 ms) and 1.1% missed, where the carrier drop that follows ends the lock. The 120 and 240 degree variants,
  40,000 each: at +10 dB 12 and 13 missed and 4 and 1 beyond 150 ms (the slowest 244 ms); at 0 dB 8.9% beyond 150 ms
  (the slowest 408 ms) and 8.0% missed.
- Loss under a carrier that keeps dropping out, 11,264 stops (openings of 1, 5, 20 and 60 ms between dropouts of 10 to
  199 ms, the stop anywhere inside a hop, at 0 and +10 dB, with and without a voice-band tone in the openings): none was
  held, p95 465 ms, 30% beyond 350 ms and the slowest 693 ms, inside the loss ceiling. A hop changes the verdict only
  when its newest 100 ms heard the carrier, so each opening makes the two hops that read it count, and a dropout the
  hangover allows leaves at most three hops in a row that do not; if every hop that closed inside a dropout kept the
  verdict instead, openings that missed every hop's end would keep the stopped tone for good.
- Ceiling tail, sweeps up to a hundred times larger: over 100,000 starts each, the exact tone at +10 dB and 0.2 and
  0.35 Hz off at +10 dB stayed within 400 ms but for 11 starts 0.35 Hz off (334, 334 and 452 ms at the slowest); over
  1,000,000 starts each at 0 dB, none of the exact tones or of those 0.2 Hz off passed the 700 ms lock ceiling (651 ms
  at the slowest for both, on the same seed, whose noise kept the harmonic test failing on nearly every window for 250 ms;
  the next slowest 583 and 596 ms), and 0.33% and 1.43% took longer than 400 ms. Before the late acquisition windows the 250 ms window
  alone passed the ceiling on 8 of those exact starts (the slowest at 1,128 ms) and 24 of those 0.2 Hz off (843 ms), and
  took longer than 400 ms on 0.93% and 3.06%. Over 400,000 stops each at 0 and +10 dB, none passed the 800 ms loss
  ceiling (738 ms at the slowest at both), though 24 and 31 took longer than 550 ms. The p95s did not move with sweep
  size (lock 342 and 360 ms at 0 dB, loss 299 and 312 ms).
- Lock under transmitter-filtered speech 10 dB above the tone, 50,000 starts at 48 kHz (voice from the carrier's start,
  the tone 200-250 ms later): p95 301 ms, 1.4% beyond 400 ms, 0.16% beyond the 700 ms lock ceiling and the slowest at
  1,314 ms. Twice a voice holding 254.1 Hz locked that tone for 150-250 ms, once before the real tone locked and once
  in its place (the detector hands a lock to a steadier tone). The contract covers tones in noise, so the speech row
  holds only its fixed seeds to it.
- Off-table tones, two hours of continuous carrier at 0 dB each, at 48 kHz and 8 kHz: 150.0 Hz never locked; 68.2 Hz
  locked a neighbour 20 and 25 times, 161.0 Hz 5 and 5, 166.7 Hz 6 and 4, each for 200-260 ms; at +3, +6 and +10 dB,
  68.2 and 161.0 Hz never locked in two hours.
- Holds, 200 runs of 20 s: at 0 and +3 dB a held tone never dropped. Under transmitter-filtered speech 10 dB above the
  tone, 300 runs of 20 s: 4 drops, all on 250.3 and 254.1 Hz, each reading `none` for 340-740 ms before the same tone
  locked again.
- Talk-off: over two hours of seeds the unfiltered speech model locked a tone three times (at 233.6, 241.8 and
  254.1 Hz, where a high voice with weak harmonics holds near a table tone), and the transmitter-filtered model once
  (at 254.1 Hz). The 250 ms window alone locked the 233.6 Hz and the filtered 254.1 Hz voice too; without the check
  that the newest 250 ms still carry the tone, the late windows would lock another five unfiltered and one filtered.

Retune clearing is covered where each path lives: `DSP_SYMBOL_REPLAY` (the tap reads the raw block before the voice
high-pass removes the tone, leaves the audio byte-identical, clears on an RTL stream-generation or
trunk-tuning-generation move, on a change of the analog profile the RTL stream publishes with neither moved (a width
alone, then the channel filter alone, while the same profile published block after block keeps the lock and its
generation) and on a change between two usable input rates, dropping the read the change falls in (also
sample by sample, where after a drop from 48 kHz to 2500 Hz the old reception ends within one 20 ms read at the new
rate, not 384 ms later at the end of the block, and where the 958 samples of a 1920 Hz tone waiting in the block at 48
kHz, which read as 2500 Hz input are a 100 Hz tone, are dropped rather than locking 100.0 Hz, and a 131.8 Hz tone at the
new rate then locks), publishes an unusable input rate as unavailable without resetting block after block and warns
about it once for each stretch of input at it (a 384 kHz, 48 kHz, 384 kHz sequence warns twice), logs `Received tone:`
once per change of verdict -- not per block, nor for a fade inside the hangover -- and again for a new reception after
the hangover, a reset or a change of input rate, and on a UDP stream whose producer pauses -- driven on an injected
clock -- stamps the deadline the frontends age the row against and starts a new reception after the pause, dropping the
read that spans it and never showing the previous channel's tone, also when the pause falls part-way through a block
(driven sample by sample through the unsynced analog path at 8192 Hz, where the 958 samples waiting in the block locked
the old tone again over a quiet carrier), and on a live radio stream that stops delivering (an `rtl_tcp` outage) the
same way, while IQ replay keeps no deadline and never resets on a gap, and starts a new reception the moment a TCP audio
connection drops when it reconnects inside the read, whose 300 ms backoff is shorter than the pause deadline; skips, on
Pulse, stdin, UDP and TCP input after a reset or a trunk-tuning generation move, the half second of the old channel's
100 Hz tone the input had queued -- read with no time passing on the injected clock, as a decoder drains a backlog,
which heard locked 100.0 Hz again on the new channel -- whether the new channel then arrives in 20 ms reads or in 100 ms
bursts, also when the reset came in a digital mode before detection started, and on stdin input whose monitor playback
holds the decoder for each block's 20 ms (the time spent playing is not counted as waiting for input), and its own
131.8 Hz tone locks within the p95 target plus one read, while a WAV file is heard at once after a reset, a UDP stream
with nothing queued from the third read after a reset or a generation move, and stdin fed faster than real time again
after 2 s; restarts the tap at the first sample of the next block when the symbol path drops a part-collected block
(`dsd_symbol_analog_block_reset()` on a receive-family change, or a family switch landing on an RTL front end), where
read on from its place in the dropped block it missed the new block's opening samples; and --
driven through `getSymbol()` on 2500 Hz WAVs, where one 960-sample block is 384 ms -- reads the block as it fills, so a
tone starting mid-block locks within the 400 ms p95 target of its start and a carrier drop is forgotten within the
hangover and two 20 ms reads (read only at block ends they took 576 and 544 ms), and sets aside the part of the monitor
block already assembled at a reset, so the tap reads none of the old channel's samples and the new channel does not lock
its tone again, while the raw WAV keeps every sample of that block -- also in a digital mode, where detection that
starts part-way through the block, after a reset or with none announced, reads from the sample it started on, also
when 959 old samples wait and that sample completes the block),
`FRAME_SYNC_INTERNAL_HELPERS` (the acquisition reset), `ENGINE_NO_CARRIER_RESET` (survives `noCarrier()`,
cleared by the legacy `-Y` step -- on RTL, by rigctl on PCM input in radio-off builds too, and by a failed step whose
rigctl leg already moved the radio -- and kept by a refused one), `ENGINE_CLEANUP_AUDIO` (engine stop frees the detector
and moves the generation on), `ENGINE_CHANNEL_SCAN`/`ENGINE_TRUNK_SCAN` (row commit and target switch),
`DSP_WAV_INPUT_EOF` (the switch to live Pulse input when a WAV file ends), `APP_CONTROL_RX_TONE_VIEW`,
`UI_NCURSES_PRINTER_HELPERS` and `UI_QT_METRICS_MODEL` (a paused stream's publication reads no carrier past its deadline
on the caller's clock) and `APP_COMMAND_QUEUE` (decode-mode change, `RTL_SET_FREQ`, `MANUAL_TUNE`, a manual channel
cycle and scan avoid by rigctl on PCM input, accepted, pending or refused; switching to WAV, Pulse, a named Pulse
source, UDP or symbol-stream input, replay and stop-playback, including a stop whose Pulse open fails; TCP connect and
reconnect, accepted or refused; and a config apply, which clears the tone when it moves the input or changes the decode
mode -- out of the analog monitor and straight back with no monitor block read in between, the row returns with no
carrier, not the old tone -- and keeps it, generation included, when it changes an unrelated setting or restates the
mode the session is in). The `APP_COMMAND_QUEUE` cases for `RTL_SET_FREQ`, `MANUAL_TUNE`, the manual channel cycle, scan
avoid, the TCP connect and the failed Pulse open, and the `ENGINE_NO_CARRIER_RESET` `-Y` step cases (the rigctl step,
the legacy RTL step and the failed partial hop), stub what they drive through the linker's `--wrap` seam, so they run
only where that seam exists: GCC or Clang builds off macOS, and for `APP_COMMAND_QUEUE` off Windows too. The RTL cases
also need a radio build. All of these bounds come from synthetic signals.

The off-air excerpts are pinned against their oracle labels (see [Tone and code labels](#tone-and-code-labels)) by
cases in the analog block, run through the analog replay host, which reads the received tone from the decoder's
publication into its `tone`, `tone_lock_ms` and `tone_lock_pct` fields:

- `DECODE_IQ_ANALOG_REAL_CTCSS_1514` (`nfm_ctcss_real`): CTCSS 151.4 Hz, locked within the 400 ms bound
  (`--analog-max-tone-lock-ms 400`; 300 ms measured) and no other tone ever reported, the 173.8 Hz neighbour 12.5 kHz
  away included. The excerpt also holds what a detector must drop the tone for: at 3.73 s the tone's phase reverses by
  180 degrees for about 180 ms (a reverse burst), then the tone is absent for about 60 ms under an unbroken carrier
  and comes back at a new phase. The detector drops the tone at 3.86 s, about 130 ms after the reversal, logs `none`,
  and locks 151.4 Hz again at 4.26 s, so the tone reads locked for 88.67% of the excerpt; the case allows that `none`
  rather than requiring it, since only the tone value is a confirmed label.
- `DECODE_IQ_ANALOG_REAL_CTCSS_NOFALSE_SQUELCH_A`, `_SQUELCH_B` and `_AM` (`nfm_squelch_real_a`, `nfm_squelch_real_b`,
  and `am_airband_real` through the FM monitor): each must log the `none` verdict, which shows detection ran, report
  `tone=NA` with a 0.00% lock share, and never log a tone. The squelch excerpts carry 150 bit/s data below 300 Hz and
  speech, the AM one voice on a steady carrier.

The neighbour's 173.8 Hz has no case: `--iq-replay` plays a capture at its recorded centre, with no way to tune
12.5 kHz off it, and a shifted copy would be another committed fixture, which neither the #518 fixture reservations
nor the budget below plan for. The copy is rebuilt outside the tree, byte for byte, by mixing `nfm_ctcss_real.iq` up
by 12.5 kHz with the fixture builder's own cu8 helpers, so the neighbour sits at 0 Hz:

```sh
mkdir -p /tmp/neighbour
python3 - /tmp/neighbour <<'EOF'
import json, math, os, sys
import numpy as np
sys.path.insert(0, "tools")
from build_iq_fixtures import load_cu8_fixture, to_cu8
src, out = "tests/fixtures/iq/nfm_ctcss_real.iq", sys.argv[1]
iq = load_cu8_fixture(src)
iq *= np.exp(2j * math.pi * 12500 * np.arange(len(iq)) / 48000)
to_cu8(iq, headroom=1.0, normalize=False).tofile(os.path.join(out, "nfm_ctcss_real_neighbour.iq"))
with open(src + ".json", encoding="utf-8") as handle:
    meta = json.load(handle)
meta["data_file"] = "nfm_ctcss_real_neighbour.iq"
meta["center_frequency_hz"] -= 12500
meta["capture_center_frequency_hz"] -= 12500
with open(os.path.join(out, "nfm_ctcss_real_neighbour.iq.json"), "w", encoding="utf-8") as handle:
    json.dump(meta, handle, indent=2)
EOF
tools/replay_ab.sh --metric analog --capture /tmp/neighbour/nfm_ctcss_real_neighbour.iq.json \
    --mode -fA --reps 12 --out /tmp/ab/neighbour /tmp/ab/analog_replay.main /tmp/ab/analog_replay.branch
```

The hosts are set up as under [Analog A/B](#analog-ab). Over those 12 realtime repeats the detector locked 173.8 Hz at
300 ms on every one, reported no other tone, and dropped and re-locked it once around a similar reversal and gap at
2.44 s, 90.33% locked in all.

Known gaps and caveats:

- **ProVoice** and **X2-TDMA** have no usable public sample and are untested here.
- **dPMR** decodes only what its CCH CRC-7 verifies (issue #407), so the `dpmr` off-air capture publishes nothing:
  it carries no recoverable CCH, which is why the CRC was thought to be broken. `DECODE_IQ_DPMR_MARGINAL` pins that
  it still syncs and still publishes no identity; `DECODE_IQ_DPMR_SYNTH` is the accept case.
- **P25 Phase 2** asserts SACCH framing only. Full payload decode needs the
  system WACN/SYSID/CC via `-X`, which the public sample does not identify.
- Fixtures are timing-insensitive by construction. Do not add assertions that
  depend on wall-clock call-state timers, because `fast` replay compresses them;
  use `--iq-replay-rate realtime` for that.
- **AM** has a fixture (`am_airband_real`) but no AM case until native AM reception (#524) adds `-fM`. Under `-fA`
  the FM monitor demodulates it, which measures an FM discriminator on an AM signal, so `-fA` cannot stand in for an
  AM case (`DECODE_IQ_ANALOG_REAL_CTCSS_NOFALSE_AM` uses it under `-fA` only as no-false-lock material for the tone
  detector). The excerpt serves `-fA` for another reason: its carrier sits within a few hertz of 0 Hz, where the
  modulation auto-switch (`frame_sync_maybe_auto_switch_modulation()` in `src/dsp/dsd_frame_sync.c`) votes for CQPSK.
  The switch used to run in analog-only mode too, because `-fA` does not set `opts->mod_cli_lock`: it applied the
  P25 CQPSK demod profile to the RTL front end, which then delivered CQPSK symbols instead of monitor audio, after 0
  or 20 ms of monitor audio in `fast` replay and 680 ms in `realtime` (the switch's dwell is timed by the wall clock).
  The analog family (`dsd_opts_is_analog_family()`) now stands the switch down, so the front end stays on the monitor
  path whatever the carrier offset, and replay of the excerpt is sample-deterministic. Two tests pin it:
  `FRAME_SYNC_INTERNAL_HELPERS` feeds the switch CQPSK-favouring metrics under the analog preset and requires no vote
  and no demod profile, with no clock involved, and `DECODE_IQ_ANALOG_NO_MOD_AUTO_SWITCH` replays the excerpt under
  `-fA` and requires all 8000 ms on the monitor path. The vote is not the hunt's only request: every symbol profile the
  sync hunt asks for goes through `rtl_maybe_apply_demod_profile()`, which sends the RTL front end nothing in
  analog-only mode, so a two-level profile the hunt re-normalises when its dwell runs out (a live switch to `-fA` from
  D-STAR leaves the hunt on 4800/2) cannot narrow the monitor to a digital channel either; `FRAME_SYNC_INTERNAL_HELPERS`
  pins that guard on both paths. The analog replay host still warns whenever the front end
  delivers CQPSK symbols, since symbols are not samples at the output rate and its stream clock then does not measure
  stream time, and every registered `-fA` case fails on that warning (`NOT_EXPECTED`); `tools/replay_ab.sh` leaves
  such repeats out (its `off_path` column). An AM case on this excerpt (`-fM`) carries the same guard.

### Analog monitor audio checks

The `DECODE_IQ_ANALOG_*` audio cases (CTest labels `iq-decode` and `analog`, radio builds only) check what the analog
FM monitor lets a listener hear; the received-tone cases `DECODE_IQ_ANALOG_CTCSS_*` carry the same labels but match a
log line, and `DECODE_IQ_ANALOG_REAL_CTCSS_*` read the received tone through this host (see
[Received tone (CTCSS) on the analog monitor](#received-tone-ctcss-on-the-analog-monitor)).
`iq_decode_check.cmake` can only match log lines, and the `-6` WAV is taken before the monitor's filters, gain stage
and squelch gate, so neither can say whether the monitor produced the right audio. The
cases therefore run `dsd-neo_test_analog_replay` (`tests/engine/analog_replay.c`) as `DSD_BIN` through the unchanged
checker (the negative controls below through their own). The host runs the real engine on the arguments `dsd-neo` would
get. From its lifecycle start hook, which runs after the engine has installed its hooks and opened (no) audio output, it
switches the monitor's UDP output on and replaces the UDP analog hook with a capture. It also wraps the RTL stream read
hook, so it knows how much of the stream the decoder has consumed: muted or squelched blocks never reach the audio hook,
so the stream position is the only clock that says when audio started, or that a run with no audio ran at all. Each read
is converted to milliseconds at the output rate in force when it was made. No product code is involved.

```sh
ctest --preset dev-debug -L analog --output-on-failure
build/dev-debug/tests/dsd-neo_test_analog_replay --frontend none -fA --iq-replay \
    tests/fixtures/iq/nfm_tone_synth.iq.json -o null --analog-expect-tone-hz 1000 --analog-probe-hz 12500
```

The host removes its own `--analog-*` options (either `--opt VALUE` or `--opt=VALUE`) before the rest reach the CLI
parser; any other `--analog-*` argument goes to `dsd-neo` unchanged, so the prefix stays free for real options. Each
audio block is scored as delivered, 20 ms at the monitor's rate, with one gain for the whole block: the default
fixed gain, or with `-n 0` the per-block AGC.

| Option | Measures |
| --- | --- |
| `--analog-expect-tone-hz HZ` | Fits a sinusoid at `HZ` to every block: `tone_dbfs`, and `tone_snr_db` as the fitted energy over everything else. It is also the reference for `dbc`. |
| `--analog-min-snr-db DB` | Lower bound on `tone_snr_db`. |
| `--analog-min-total-ms MS`, `--analog-max-total-ms MS` | Stream time the decoder consumed (`total_ms`), whether or not any audio came out. A stalled or empty replay fails the lower bound. |
| `--analog-min-captured-ms MS` | Audio the monitor delivered at all, i.e. with the gate open. A stalled or shortened replay fails it. |
| `--analog-min-audible-ms MS`, `--analog-max-audible-ms MS` | Blocks whose RMS is at or above the audible level (`--analog-audible-dbfs`, default -50 dBFS). |
| `--analog-min-first-audible-ms MS`, `--analog-max-first-audible-ms MS` | Stream time where the first audible block starts. Fails when nothing is audible. |
| `--analog-min-inband-db DB` | 300-3000 Hz energy over 3400-6000 Hz energy, Hann-windowed per block. |
| `--analog-min-rms-dbfs DB`, `--analog-max-rms-dbfs DB` | RMS level of all delivered audio (`rms_dbfs`). |
| `--analog-max-peak-dbfs DB` | Largest delivered sample (`peak_dbfs`). |
| `--analog-max-clip N` | Samples at int16 full scale. |
| `--analog-probe-hz HZ` | Level at `HZ` (Hann-windowed Goertzel), repeatable up to 8 frequencies: `dbfs`, and `dbc` against the expected tone. |
| `--analog-probe-{min,max}-{dbc,dbfs} HZ:DB` | Bounds on a probe's level; each also adds the probe. |
| `--analog-max-tone-lock-ms MS` | Upper bound on `tone_lock_ms`, the stream time of the first received-tone lock. Fails as not measured when no tone locked. |

A case that expects silence (a muted or rejected transmission) pairs `--analog-max-audible-ms 0` with
`--analog-min-total-ms`: silence alone is also what a replay that stalled or never started produces.

The tone fit and the probes work on one 20 ms block at a time: 960 samples at 48 kHz, so 50 Hz bins, a Hann
equivalent noise bandwidth of about 75 Hz and a main lobe of +/-100 Hz. Frequencies closer than about 100 Hz are not
resolved, and the host warns when a probe sits that close to the expected tone or to another probe (a probe exactly
on the tone is a deliberate cross-check and passes quietly). A probe reads the energy within that bandwidth, not a
single line: on noise it measures the noise density there, and on an FM-modulated beat mainly its carrier line, not
the beat's total power. On `nfm_ctcss_real` under `-v 0`, a 173.8 Hz probe reads -1.1 dBc against the 151.4 Hz tone,
and all of it is leakage of that tone: the 173.8 Hz neighbour sits 12.5 kHz away, where the channel filter rejects
it. Sub-audible tones 20-30 Hz apart need a longer, phase-continuous window than this host has.

When live processing ends the host prints `ANALOG METRIC:` (`rate_hz`, `total_ms`, `captured_ms`, `audible_ms`,
`first_audible_ms`, `rms_dbfs`, `peak_dbfs`, `clip`, `inband_db`, `tone_hz`, `tone_dbfs`, `tone_snr_db`, and the
received-tone fields `tone`, `tone_lock_ms` and `tone_lock_pct` described under [Analog A/B](#analog-ab); `NA` where
a value was not measured), one `ANALOG PROBE:` line per probe, and then `ANALOG AUDIO OK`, or one
`ANALOG AUDIO FAIL:` line per missed bound and exit status 1. Without bounds it only reports, which is how
`tools/replay_ab.sh --metric analog` uses it. Every bound is absolute and per case, so "narrower is cleaner" becomes
two cases with their own limits, not a comparison between runs. Measured on the commit that added them:

| Case | Fixture | Measured (default monitor filters) | Bounds |
| --- | --- | --- | --- |
| `DECODE_IQ_ANALOG_NFM_TONE` | `nfm_tone_synth` | tone SNR 25.9 dB, in-band 30.5 dB, RMS -36.1 dBFS, audible 1500 of 1500 ms from 0 ms, no clipping; a 1 kHz probe reads 0.00 dBc | SNR ≥ 20, captured and audible ≥ 1400 ms, first audible ≤ 100 ms, in-band ≥ 24, RMS -42 to -30 dBFS, clip 0, 1 kHz probe within ±1 dBc |
| `DECODE_IQ_ANALOG_NFM_ADJACENT` | `nfm_adjacent_synth` | tone SNR 25.8 dB; 12.5 kHz probe -64.9 dBc | SNR ≥ 20, captured ≥ 1400 ms, 12.5 kHz ≤ -40 dBc |
| `DECODE_IQ_ANALOG_SILENT_STREAM_TIME` | `nfm_tone_synth` under `-fi` (monitor off) | total 1500 ms, no audio at all | audible 0 ms, total 1400 to 1600 ms |
| `DECODE_IQ_ANALOG_NFM_REAL_CTCSS_SMOKE` | `nfm_ctcss_real` | captured and audible 6000 ms, in-band -1.0 dB, RMS -44.6 dBFS | captured ≥ 5800, audible ≥ 4500, in-band ≥ -4, RMS ≤ -34 dBFS |
| `DECODE_IQ_ANALOG_NFM_REAL_SQUELCH_A_SMOKE` | `nfm_squelch_real_a` | captured 4000 ms, audible 1460 ms from 460 ms, in-band 8.0 dB | captured ≥ 3900, audible ≥ 900, first audible 250 to 1000 ms, in-band ≥ 4 |
| `DECODE_IQ_ANALOG_NFM_REAL_SQUELCH_B_SMOKE` | `nfm_squelch_real_b` | captured 4000 ms, audible 2980 ms, in-band 9.6 dB | captured ≥ 3900, audible ≥ 2000, in-band ≥ 5 |
| `DECODE_IQ_ANALOG_NO_MOD_AUTO_SWITCH` | `am_airband_real` under `-fA` (monitor path only, not AM reception) | total and captured 8000 ms, no CQPSK symbols (before the fix: total 751 ms, captured 0 to 20 ms, CQPSK warning) | captured ≥ 7800 ms, total 7800 to 8200 ms |
| `DECODE_IQ_ANALOG_REAL_CTCSS_1514` | `nfm_ctcss_real` | tone 151.4, first lock at 300 ms, locked 88.67% | tone lock ≤ 400 ms; tone 151.4 and no other tone logged |
| `DECODE_IQ_ANALOG_REAL_CTCSS_NOFALSE_SQUELCH_A`, `_B`, `_AM` | `nfm_squelch_real_a`, `nfm_squelch_real_b`, `am_airband_real` | `none` verdict, no tone, locked 0.00% | `none` logged, `tone=NA`, 0.00% locked, no tone logged |

Every `-fA` case in the table also fails if the host warns that the front end delivered CQPSK symbols instead of
monitor audio (see the `am_airband_real` note above).

The cases above can only show that bounds hold. The `DECODE_IQ_ANALOG_NEG_*` negative controls show that a missed
bound fails: they run the host through `tests/analog_replay_fail_check.cmake`, which requires its exit status (1 for
a missed bound), the named `ANALOG AUDIO FAIL:` line, an `ANALOG METRIC:` line (so the replay was scored) and no
`ANALOG AUDIO OK`, and can name a bound that must keep holding.

| Case | Run | Must fail on |
| --- | --- | --- |
| `DECODE_IQ_ANALOG_NEG_ADJACENT_UNFILTERED` | `DECODE_IQ_ANALOG_NFM_ADJACENT`'s 12.5 kHz bound with `DSD_NEO_CHANNEL_LPF=0`, options spelled `--opt=VALUE` | the probe's measured level: about -8 dBc against -40 (-64.9 with the channel filter); "not measured" does not count |
| `DECODE_IQ_ANALOG_NEG_AUDIBLE_NOT_SILENT` | the silence pattern (`--analog-max-audible-ms 0`, `--analog-min-total-ms 1400`) on `nfm_tone_synth` | audible ms, while the stream-time bound holds |
| `DECODE_IQ_ANALOG_NEG_TONE_SNR` | a 60 dB tone SNR floor on `nfm_tone_synth` (25.9 dB) | tone SNR |
| `DECODE_IQ_ANALOG_NEG_TONE_NOT_MEASURED` | an SNR bound without `--analog-expect-tone-hz` | tone SNR "not measured" |
| `DECODE_IQ_ANALOG_NEG_BAD_BOUND` | a malformed bound value | exit status 2 before the replay starts |
| `DECODE_IQ_ANALOG_NEG_MISSING_VALUE` | a host option as the last argument, with no value | exit status 2 and "needs a value" before the replay starts |
| `DECODE_IQ_ANALOG_NEG_TONE_LOCK_MS` | a 50 ms tone-lock bound on `nfm_ctcss_synth_1000` (300 ms) | tone lock ms, on the measured value |
| `DECODE_IQ_ANALOG_NEG_TONE_LOCK_NOT_MEASURED` | a 400 ms tone-lock bound on `nfm_notone_synth`, where no tone locks | tone lock ms "not measured" |

When a later change adds a bound or a new kind of check to the host, add a negative control beside it.

A few things about these numbers. The default monitor chain puts a first-order 8 kHz high-pass (`pbf_f`) and a
960 Hz high-pass after the discriminator, then a fixed gain: at the default `-n 50`, `analog_gain_f()` multiplies by
12000 (4800 x 50/100 x 5). The per-block AGC (`agsm_f()`, which aims each block's peak at 4800 and caps its gain at
6000x) runs only with `-n 0`. The default chain's audio therefore sits near -36 dBFS; with `-n 0` the same tone comes
out at -42.1 dBFS, 6 dB lower, because the cap is reached. The fixed gain is also why clipping can happen at all, and
why FM quieting shows as level: receiver noise (`noise_floor` under `-fA`) measures -23.3 dBFS RMS against -44.6 dBFS
for `nfm_ctcss_real`. The high-passes bring 1 kHz out about 18 dB lower, relative to 8 kHz and up, than it went in,
so the default chain's in-band ratio says more about those filters than about the demodulator: receiver noise
measures -0.6 dB, no better than `nfm_ctcss_real`, whose speech deviation is small (0.2-0.6 kHz RMS) against a
channel CNR of roughly 20 dB. That is why its smoke case bounds the level instead. With the filters off (`-v 0`) the
same fixtures measure a tone SNR of 36.8 dB and an in-band ratio of 42.7 dB on `nfm_tone_synth`, 8.7, 15.8 and
17.8 dB in-band on the three real excerpts, and 6.9 dB on receiver noise. Use `-v 0` when the question is what the
demodulator did.

The 12.5 kHz probe on `nfm_adjacent_synth` reads -64.9 dBc, against -66.7 dBc on `nfm_tone_synth`, which has no
interferer: at the default channel width it measures the floor, so its bound catches a channel filter that lets the
neighbour through, not finer changes. The probe itself reads a real line at its true level: in
`DECODE_IQ_ANALOG_NFM_TONE` a probe on the 1 kHz tone agrees with the least-squares tone fit to 0.00 dB, and a -6 dB
carrier inside the passband (5 kHz up, a variant fixture that is not committed) measured -5.9 dBc at 5 kHz.

#### Analog fixtures

| Fixture | Source | Content |
| --- | --- | --- |
| `nfm_tone_synth` | synthetic, seed 5181 | 1.5 s: 1 kHz at 3 kHz peak deviation, complex noise 30 dB under the carrier across 48 kHz |
| `nfm_adjacent_synth` | synthetic, seed 5182 | the same plus an unmodulated carrier 12.5 kHz up at -6 dB |
| `nfm_ctcss_real` | sigidwiki `IQ_CTCSS_example_482768kHz_IQ.zip` (s16, 156.25 kHz) | 6 s from 10 s: the channel 12.255 kHz above the recording centre, speech throughout; its neighbour (CTCSS 173.8 Hz) is kept 12.5 kHz below |
| `nfm_squelch_real_a` | sigidwiki `Unknown_NFM_squelch_IQ.zip`, `855111kHz_IQ.wav` (s16, 39.0625 kHz) | 4 s from 0.3 s: speech, then carrier only; 150 bit/s sub-audible data, not CTCSS or DCS |
| `nfm_squelch_real_b` | the same zip, `855361kHz_IQ.wav` | 4 s from 0.5 s: speech, then carrier only; the same data signalling |
| `am_airband_real` | sigidwiki `AM_IQ.zip` (u8, 64 kHz) | 8 s from 22 s: AM airband voice on a continuous carrier |
| `nfm_ctcss_synth_1000` | synthetic, seed 5221000 | 2 s: voice-band audio at up to 4 kHz deviation plus CTCSS 100.0 Hz at 600 Hz deviation, receiver noise at baseband (#522) |
| `nfm_ctcss_synth_670` | synthetic, seed 5220670 | the same with CTCSS 67.0 Hz |
| `nfm_ctcss_synth_drop` | synthetic, seed 5221001 | 2.5 s: CTCSS 100.0 Hz that stops at 1.2 s while the carrier and voice carry on |
| `nfm_notone_synth` | synthetic, seed 5220000 | 2 s: the same voice and noise with no tone |

`tools/build_iq_fixtures.py` pins each zip's SHA-256, reads the WAV members sample-exact (u8 centred on 127.5),
shifts each wanted carrier to 0 Hz by an offset measured over the excerpt (`ANALOG_EXCERPTS` documents each), and
resamples to 48 kHz in the frequency domain. The synthetics regenerate offline and byte for byte with
`python3 tools/build_iq_fixtures.py --derived-only`; the excerpts need the network:
`python3 tools/build_iq_fixtures.py --only nfm_ctcss_real` (and so on).

The whole analog effort (issue #518) stays within 5 MB of new fixture bytes. The six #518 fixtures take 2.4 MB, the
`nxdn48_attenuated` replay that #521 committed 0.58 MB and the four received-tone synthetics of #522 0.82 MB
(`nfm_ctcss_synth_drop` runs 2.5 s: after its tone stops at 1.2 s it still has to lose the tone and reach the no-tone
verdict), 3.8 MB in all, which leaves about 1.2 MB. A 48 kHz cu8 fixture takes 96 kB a second, so keep each later
synthetic fixture to 2 s (192 kB) or less: the ones still reserved (three for #523, two for #524) then take at most
0.96 MB. A pull request that adds analog fixtures states the running total.

#### Tone and code labels

The real excerpts come unlabelled, and a C detector must not be graded against its own output. `tools/analog_oracle.py`
labels them independently (numpy only; not run by CI): one zero-padded FFT of the whole excerpt's sub-audible band for
CTCSS, and a brute-force slicer at every bit phase and both polarities with its own Golay (23,12) check for DCS.
`python3 tools/analog_oracle.py --self-test` checks its DCS word layout against a published codeword and aliases and
labels synthetic CTCSS, DCS and no-tone signals. On the committed fixtures:

| Fixture | Oracle label | Evidence |
| --- | --- | --- |
| `nfm_ctcss_real` | CTCSS 151.4 Hz | line at 151.34 Hz, 20.8 dB over the next line in 60-260 Hz, about 360 Hz deviation; no DCS word |
| `nfm_ctcss_real`, neighbour (`--offset-hz -12500`) | CTCSS 173.8 Hz | line at 173.96 Hz, 21.7 dB over the next |
| `nfm_squelch_real_a` | none | no CTCSS line and no repeating DCS word; the sub-audible band carries NRZ data at 150.0 bit/s that repeats every 21 bits |
| `nfm_squelch_real_b` | none | the same 150.0 bit/s, 21-bit pattern |

So the two "unknown squelch" captures do not carry DCS, which is 134.4 bit/s in 23-bit words: they are no-false-lock
material for CTCSS and DCS detectors, not accept cases. Issue #518's validation plan counted this recording as its
NFM+DCS source, and early plans named the excerpts `nfm_dcs_real_a/_b`; they are named after their source instead,
because they hold no DCS, and the corpus has no real DCS recording yet (`build_iq_fixtures.py --only` answers the old
names with the new ones and refuses any name it does not build). A real DCS accept case needs another source.

Every DCS waveform has two spellings, because inverting a DCS word gives another valid word: the oracle prints both
(`D023N = D047I`, and `D023I = D047N` for the inverted waveform). A detector reports one canonical label per waveform,
and the DCS detector (#523) defines which; compare an oracle label with a detector's by waveform, not by spelling. The wiki's CTCSS page, which links the I/Q recording, carries audio samples at
151.4, 173.8 and 186.2 Hz without saying which tones the recording holds; the oracle finds the first two. A
maintainer has confirmed the CTCSS labels in the table, "none" for both squelch captures included, and the received-tone
cases pin them: `DECODE_IQ_ANALOG_REAL_CTCSS_1514` the 151.4 Hz of `nfm_ctcss_real`, and the
`DECODE_IQ_ANALOG_REAL_CTCSS_NOFALSE_*` cases the absence of any tone on both squelch captures (see
[Received tone (CTCSS) on the analog monitor](#received-tone-ctcss-on-the-analog-monitor), which also says why the
neighbour's 173.8 Hz has no case). The DCS labels stay unpinned until a DCS detector exists.

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
  -DDSD_ENABLE_SOAPYSDR=OFF -DDSD_REQUIRE_SOAPYSDR=OFF \
  -DDSD_ENABLE_AIRSPY=OFF -DDSD_REQUIRE_AIRSPY=OFF
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

For a declared channel mode, build `dsd-neo_test_scan_mode_replay` and pass it as the comparison binary.
This test-only host parses the real `-C` map, prepares and enters row zero through the production scoped-mode
API, and asserts the override before starting the real engine. It leaves scanner retuning disabled because the
I/Q replay API rejects live retunes. For example, compare native `-fi` with a map whose first row declares
`nxdn48`, using `--mode "-fi -Z -C /path/to/map.csv"` for both binaries. The baseline ignores the optional mode
column. `DECODE_IQ_SCAN_*` additionally tests overrides from global presets that exclude the declared class.

Reading it:

- **Errors per decoded voice frame**, never the raw error total. A build that
  loses sync decodes fewer frames and accrues fewer errors without being better,
  so watch the `voice` column alongside the error rate.
- **Paired per repeat.** Builds run round-robin with the order rotated each
  repeat, because a fixed order credits the better slot to whichever build holds
  it. The report compares within a repeat for the same reason.
- **Run the baseline against itself first**, as a copy under another name
  (replay_ab.sh refuses two builds with the same basename). That control
  establishes the noise floor for the machine and the capture; a difference
  smaller than it has not been measured. On the captures behind issue #444 the
  floor was about 0.1 errors per voice frame over 12 repeats.
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

### Analog A/B

Analog DSP changes (channel width, AM demodulation, de-emphasis, tone detection) are judged the same way, on the real
excerpts in `tests/fixtures/iq` and on any longer real capture, with `--metric analog`. The builds are analog replay
hosts rather than `dsd-neo`, and `summary.tsv` gains the analog columns: `tone_snr_db`, `inband_db`, `clip`,
`audible_ms`, `first_audible_ms`, `rms_dbfs`, the first probe given as `probe_hz`, `probe_dbfs` and `probe_dbc`, `tone`,
`tone_lock_ms` and `tone_lock_pct`, and last the run's exit status `rc` and `off_path`, 1 when the host warned that the
front end delivered CQPSK symbols instead of monitor samples. `probe_dbc` needs `--analog-expect-tone-hz`, so on a real
capture, which has no test tone, it is `NA` and `probe_dbfs` is the probe's level. The report pairs each column per
repeat, pairs probe levels only between builds that probed the same frequency (a wrapper that puts its own
`--analog-probe-hz` first changes which probe comes first), and gives the tone label each build settled on. A repeat
that exited non-zero or ran off the monitor path is left out of every column and counted in a per-build warning, even
though the host prints its metrics before it exits: it measured a crash, a timeout or the modulation auto-switch, not
the build. So give the host no `--analog-*` bounds in `--mode`, since a missed bound exits 1. Its `n` column counts the
repeats in which a build measured that column. The paired interval needs at least two paired repeats and reads `n/a`
with one (`--reps 1`, or every other repeat left out), in the digital report too, since one pair says nothing about the
spread. A build missing a column that another build measured gets an explicit `NA` row, and a build with no usable
repeat (it crashed, timed out, left the monitor path, or is not an analog replay host) makes the report warn and exit 1,
rather than leave the other builds' rows looking like a clean result. `tests/tools/test_replay_ab_report.py` (stdlib
only) covers the per-repeat pairing, the A-vs-A control, probe frequency keying, the coverage reporting, crashed and
off-path repeats, the single-pair interval, the received-tone columns and the duplicate-name refusal. CTest runs it as
two tests: `TOOLS_REPLAY_AB_REPORT` scores canned summaries and runs wherever Python does, and
`TOOLS_REPLAY_AB_ANALOG_METRIC` drives the real `replay_ab.sh` with a fake host, so it is registered only outside
Windows where bash and coreutils `timeout` are found.

`tone`, `tone_lock_ms` and `tone_lock_pct` come from the host's `ANALOG METRIC:` line, which reads them from the
decoder's received-tone publication (`dsd_state::analog_rx`) after every block the monitor delivers; a host built
before a field existed reads `NA` for it. The host defines the fields and replay_ab.sh and the report pair them; the
CTCSS detector (#522) fills them for CTCSS, the DCS detector (#523) adds its label, and a detector that needs another
statistic adds its own field and column. The contract (also in the file comment of `tests/engine/analog_replay.c`),
which matters because replay_ab.sh splits the line on spaces:

- `tone=<label>`: the received tone or code with no whitespace, `151.4` (Hz, one decimal) for CTCSS and the DCS
  detector's canonical label, such as `D023N`, for DCS (see [Tone and code labels](#tone-and-code-labels)); `NA` when
  none was confirmed. When the label changes during a run, the last one confirmed.
- `tone_lock_ms=<ms>`: stream time of the first confirmed lock, on the same clock as `first_audible_ms`: the end of
  the block after which the publication first read locked, with two decimals; `NA` when nothing locked.
- `tone_lock_pct=<pct>`: the share of the delivered audio (`captured_ms`) in blocks after which the publication read
  locked, 0 to 100 with two decimals: `0.00` when nothing locked, `NA` when no audio came out. On a capture whose tone
  is continuous it says how steadily the tone was held; on one with no tone it is the false-lock share and should read
  `0.00`.

replay_ab.sh names each build by its basename and refuses two with the same one, so copy each tree's host to its own
name. Per-variant flags or settings within one build go in a wrapper script per variant; its name is the variant's
name. The example compares main with a branch and, on the branch, the land-mobile de-emphasis (`DSD_NEO_DEEMPH=nfm`)
with the default. A channel-width variant is the same kind of wrapper passing `--nfm-bandwidth-hz`, once #525 adds
that option; an AGC variant passes `-n 0`, since the default gain is fixed.

```sh
mkdir -p /tmp/ab
cmake --build --preset dev-debug -j --target dsd-neo_test_analog_replay
cp build/dev-debug/tests/dsd-neo_test_analog_replay /tmp/ab/analog_replay.branch   # and analog_replay.main
printf '#!/bin/sh\nDSD_NEO_DEEMPH=nfm exec /tmp/ab/analog_replay.branch "$@"\n' > /tmp/ab/deemph_nfm
chmod +x /tmp/ab/deemph_nfm

tools/replay_ab.sh --metric analog --capture tests/fixtures/iq/nfm_ctcss_real.iq.json \
    --mode "-fA -v 0 --analog-probe-hz 12500" --reps 12 --out /tmp/ab/ctcss \
    /tmp/ab/analog_replay.main /tmp/ab/analog_replay.branch /tmp/ab/deemph_nfm
tools/replay_ab_report.py /tmp/ab/ctcss/summary.tsv --baseline analog_replay.main
```

`-v 0` takes the monitor's fixed voice filters out of the measurement (see
[Analog monitor audio checks](#analog-monitor-audio-checks)); leave it out when the question is what a listener gets.
The 12.5 kHz probe there reads the neighbour channel's leakage as `probe_dbfs`, over about 75 Hz around 12.5 kHz in
each 20 ms block, no finer.
Run the control first, one host against a copy of itself. While the front end stays on the monitor path, I/Q replay
is sample-deterministic, so the control must read `+0.00 +/- 0.00` with no differing repeats: 12 realtime repeats on
`nfm_ctcss_real` and on `nfm_tone_synth` did, for every column. A run whose log carries the host's CQPSK-symbols
warning left the monitor path and is not deterministic: a build from before the analog family stood the modulation
auto-switch down does that on `am_airband_real` (see the `am_airband_real` note under
[Full-chain modulation decode tests](#full-chain-modulation-decode-tests)), and such a run measures the auto-switch,
not the change; the report leaves it out as `off_path` and warns, and a build with no other repeats fails the report. A
difference that shows up in a monitor-path control is the harness's, not the change's. Attach the report to the pull request with the capture, flags and repeat count.

### Analog listen-test sign-off

Measured metrics do not replace listening. Before an analog DSP change merges, a maintainer listens to the paired
builds on the real excerpts, at `--iq-replay-rate realtime` with audio output on, and records the result in the pull
request:

- [ ] NFM at the default width and at 12.5 and 25 kHz on `nfm_ctcss_real`, `nfm_squelch_real_a` and
  `nfm_squelch_real_b`: speech intelligible, no new distortion, hiss or clicks; the neighbour channel of
  `nfm_ctcss_real` audible only at the widest setting (#525).
- [ ] AM on `am_airband_real` at several widths and with the AGC: speech intelligible, level steady across the
  excerpt, no pumping or clipping (#524).
- [ ] Tone filtering: allowed traffic opens within the detection window, rejected and untoned traffic stays silent,
  and nothing leaks at the start of a rejected transmission (#527).
- [ ] The A/B report agrees with what was heard; where it does not, say which one the pull request relies on.

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

Scoped scanning options are covered by `RUNTIME_SCAN_OPTIONS` and `CORE_SCAN_PROFILE`, plus conventional/trunk tune
boundary regressions (option-only rows, policy edits beneath a staged tune, keyring changes in the tune window, a
failed key preparation mid-rotation, per-target voice-gate intervals), CLI conflict/off-switch checks, and terminal
loaded-key/redaction tests. The production MBE
regression exercises clear and Motorola BP frames on both slots, with normal signalling and explicit forcing.
`CORE_SCAN_PROFILE` tests actual group-store ownership; scanner hosts that stub group policy use `scan_group_stubs.c`.
