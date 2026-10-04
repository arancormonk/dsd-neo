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
`DECODE_IQ_*_AUTO_HUNT` cases are the documented exception, described below. I/Q
replay defaults to `--iq-replay-rate fast`, so run one after another in
`dev-debug` the 40 digital cases take about 27 seconds and the analog ones (see
[Analog monitor audio checks](#analog-monitor-audio-checks)) about 22. The five
`iq-determinism` cases (see [Replay determinism](#replay-determinism-issue-572))
add 46 seconds, two realtime cases of 15.6 seconds among them; under
`ctest -j 16` they run beside the suite's longest test, and the full suite
still takes about 55 seconds.

Covered: P25 Phase 1 C4FM (control and voice), P25 Phase 1 CQPSK/LSM (control and
voice, plus a two-ray simulcast-impaired control channel), P25 Phase 2, DMR
voice, DMR Tier III control (including a CSBK-only RAS control channel replayed
with `-F`, colour code 0 — regression coverage for issue #348), NXDN48, NXDN96,
dPMR, D-STAR, YSF, EDACS, ProVoice, and M17, plus the received CTCSS tone and DCS code on synthetic
analog NFM under `-fA` (see "Received tone (CTCSS) on the analog monitor" and "Received code
(DCS) on the analog monitor" below). The analog FM
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

`provoice` is the sigidwiki ProVoice clip, one 3.5 s transmission (issue #588). Unlike the other audio sources it was
recorded from a receiver's de-emphasized audio rather than from its discriminator: its 9600 baud clock line is there,
but the roll-off smears each transition across its neighbours and, as recorded, it gives no frame sync at all. The
builder undoes a single-pole 225 us de-emphasis (`AUDIO_DEEMPHASIS_US`) before remodulating it; swept over 150-500 us
with the hard-decision decoder of the time, that value decoded the most frames at under one correction per voice
frame. Under `-fp` and `-fh` the fixture decodes 40 ProVoice frames, 160 IMBE 7100x4400 voice frames, 86 of them with
nothing to correct. Nothing in a ProVoice frame can fail a
check: its header carries no CRC, BCH or parity, and the Golay(23,12) and Hamming(15,11) codes that protect the voice
are perfect codes, which decode any input to some codeword. So `DECODE_IQ_PROVOICE` and `DECODE_IQ_PROVOICE_EDACS`
assert on what `-Z` prints: the 64-bit header field after the LID, the same in 30 of the 40 frames (the rest are a bit
or two off, since nothing protects it). `DECODE_IQ_PROVOICE_IMBE` asserts a voice frame that decoded with no correction. With ±45 counts of added
noise the header still matches 21-25 times, and no other preset prints a ProVoice line from it.

`dstar` is the sigidwiki D-STAR clip, 4 s of one transmission. Under `-fd` it decodes six superframes, 126 AMBE
3600x2400 voice frames: first a header matched in the noise before the carrier, whose 21 frames are mostly corrupt,
then the transmission's own header and four voice superframes, 99 of whose 105 frames need no correction.
`DECODE_IQ_DSTAR` asserts the source callsign from the slow data, and `DECODE_IQ_DSTAR_AMBE` (issue #599) asserts what
`-Z` prints for the first frame of the first voice superframe. The hard and soft decoders give the same payload for
every frame of the transmission; they differ only in the superframe behind the header matched in noise.

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
between its frames. The `p25p1_cqpsk_vc` capture is already registered under `-f1 -mq`; under `-fa` it steps to 20
sps partway through on `main` and finishes in dPMR, so holding the profile is what the negative half of this case pins.
Both halves were confirmed stable over repeated runs on `dev-debug`, `asan-ubsan-debug` and `tsan-debug`. Under `-fa`
the modulation vote never picks CQPSK on this capture, so that hunt runs the FSK discriminator path; it is
`DECODE_IQ_P25P1_CQPSK_VOICE_AUTO_HUNT_UNLOCKED` now, and `DECODE_IQ_P25P1_CQPSK_VOICE_AUTO_HUNT` runs `-fa -mq`, the
same protocols on the CQPSK chain. The `-mq` lock lets the hunt rotate only among profiles with the same timing, which
prints no `SPS hunt: trying` line, so there the negative half guards against a timing change.

The P25 CQPSK fixtures run the CQPSK chain only with `-mq` after the preset. `-f1` sets C4FM modulation and the last of
the two options wins, so `-f1` alone (how `DECODE_IQ_P25P1_CQPSK_CC`, `_VOICE` and `_SIMULCAST_CC` were first
registered) and `-mq -f1` both demodulate them on the FSK path; the replay jitter host's `REPLAY STREAM` line shows it
(`cqpsk_symbols=0`). They are registered under `-f1 -mq` now, and the same goes for an A/B of a CQPSK change.

Its payload assertion changed in issue #388, and the reason is worth recording: it had been
`ALG ID: 0xC0 KEY ID: 0x3900`, which is not in this capture. The `-f1` preset emits no `0xC0` anywhere in it, reading
three clear-voice headers (`ALG ID: 0x80 KEY ID: 0x0000`) and six `Group Voice Channel User` grants. What `-fa`
produced was a corrupted read of one of those clear headers — the run decoded zero grants, two headers and 127 header
errors — and the case had been asserting that artifact. Holding the 4800/4 co-tenants off the frame brings the `-fa`
run in line with the native one (seven grants, four clear headers, irrecoverable header errors 3 → 0), so the
assertion is now the same real payload `DECODE_IQ_P25P1_CQPSK_VOICE` asserts. The lesson generalizes: an
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
the `dstar` fixture. A reject case there was tried and removed: at the time, how far the hunt got through a 4 s
capture under `-fa` depended on front-end and thread scheduling, so the run varied between builds and between
presets, and the assertion failed under `tsan-debug` while passing repeatedly under `dev-debug`. Replay no longer
depends on scheduling (issue #572), but how far the hunt gets still depends on its phase: anything that shifts its
timeline by a few samples, the front end's latency or an event's offset, can move it, so it still differs between
builds (see [Replay determinism](#replay-determinism-issue-572)). Only assertions that hold whatever that phase
belong in an `-fa` decode case, which is why the AUTO cases here assert either a payload the capture always reaches
or a hunt line that always prints.

Reject cases assert the opposite: that a fixture produces *no* decode. `dsd_neo_add_iq_reject_test` takes a
`NOT_EXPECTED` regex alongside the usual `EXPECTED` one, so a run that printed nothing at all cannot pass by
default. `noise_floor` is 10 s of synthetic complex Gaussian receiver noise (seed 398, sigma 16 LSB, regenerated by
`python3 tools/build_iq_fixtures.py --derived-only`) with no signal in it whatsoever. The `DECODE_IQ_NOISE_FLOOR_*`
cases assert `Total audio errors: 0`, which is the count of vocoder frames the decoder synthesized: before issue
\#398's confirmation gate this fixture produced 89 of them under `-fn` and 49 under `-fi`, decoded from nothing.
They deliberately do not cover `-fa`, where how far the hunt gets moves with its phase.

There is no `-fa` hunt case on `noise_floor`, and the measurement behind that is worth recording. Issue #391's
remaining set — DMR, P25 Phase 2 and X2-TDMA, the handlers that report no verdict — is bounded by arithmetic
rather than by a verdict, so a replay assertion cannot see it. Under `-fa` the fixture completes six rotations in its
10 s; defeating the verdict gate in `frame_sync_sps_hunt_note_handler_consumption()` gives five, and every individual
`SPS hunt: trying` line still prints in both. Only the rotation *count* separates them, and how far the hunt gets is
exactly the phase-dependent quantity that keeps a reject case off the `dstar` fixture above. So the property is pinned
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
fixture is 6 s, about one full rotation, which is precisely the phase-dependent regime that keeps a reject case off
the `dstar` fixture above. The change also leaves the hunt's own accounting untouched, so an `-fa` case would mostly
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
could not see this then: the same build scored anywhere from 57 to 94 NXDN48 syncs run to run under `-fa`, which is
wider than the effect. Replay is repeatable now, so a build scores the same on every replay, but that score is one
draw of the hunt's phase against the calls, which a change to the hunt's timeline can move by as much;
[Replay determinism](#replay-determinism-issue-572) says how to judge such a change across captures and their
realizations instead. Decode *volume* under the hunt is not covered by the `iq-decode` suite, which asks only whether a
build still decodes.

#### Replay determinism (issue #572)

Under `--iq-replay` the decoder paces the front end a capture chunk at a time, every event and loop rewind lands on an
idle pipeline, the front end's filters carry their state across blocks, and the decode clock runs on the capture's time
(`docs/iq-capture-replay.md` says what that means for a user, and the IO, DSP and Runtime sections of `docs/code_map.md`
how it works). For one build, configuration and machine, a replay without `-T` or `-Y` therefore prints the same decoder
output on every run, fast or realtime, idle or loaded, capture-time timestamps included. The cases under the CTest label
`iq-determinism` (also in `iq-decode`, radio builds only) hold it to that:

```sh
ctest --preset dev-debug -L iq-determinism --output-on-failure
```

The host is `dsd-neo_test_replay_jitter` (`tests/engine/replay_jitter.c`). It runs the real engine on the arguments
`dsd-neo` would get and perturbs only the decoder's side of the replay, through the stream read hook: with
`--replay-jitter-seed N` it sleeps a seeded U(0, `--replay-jitter-max-ms`) ms (120 by default) after about one read in
`--replay-jitter-every` (64), within `--replay-jitter-budget-ms` (1.5 s, which cannot be raised); with
`--replay-short-reads N` it caps each read at a seeded 1 to count samples. Where GNU ld can replace the audio device
layer (`--wrap=dsd_audio_*`, the seam `CORE_AUDIO_GAIN` uses; not on Apple or Windows), `--replay-sink free|stalled`
gives the engine a test sink with the real backends' asynchronous contract (a 1 s ring, writes that never wait, a
pump thread) whose device takes every chunk or stalls after the first. The host fails a leg whose stalled sink never
backed up and dropped audio, whose decoder drained a stalled sink, or whose engine opened a synchronous one. At the end
it prints `REPLAY STREAM: fsk_samples=… cqpsk_symbols=… monitor_samples=… generation_changes=… media_ms=…`, counted
from the batch tags the decoder read, each output kind in its own unit, and a `REPLAY SINK:` line per stream with an
FNV-1a of every sample the decoder handed it. A sample read without a tag, or with an unknown kind, fails the host.
When the engine returns it prints `REPLAY WALL: wall_ms=…`, the real time the engine run took, timed around
`dsd_engine_run_with_lifecycle()` on the real-time monotonic clock (`dsd_realtime_mono_ns()`).

The runner is `tests/iq_determinism_check.cmake`, registered through
`dsd_neo_add_iq_determinism_test(name fixture mode runs expected min_fsk min_cqpsk min_total [NOT_EXPECTED regex])`.
RUNS is a `;` list of at least two legs, each joining parts with `+`: `fast` or `realtime`, `jitter:SEED:MAX_MS`,
`short:SEED`, and `sink:free` or `sink:stalled` (which plays to `-o pulse` in place of `-o null`). Each leg is one
process, held to `iq_decode_check.cmake`'s exit-status and sanitizer checks, to EXPECTED and, when given, to
NOT_EXPECTED. A leg that prints `Retune ignored during IQ replay` fails as misconfigured: only `-T` and `-Y` print it,
and they are outside the guarantee. A leg must also show that its perturbation happened: the host's `REPLAY JITTER`
line has to report `sleeps` above 0 on a leg with a jitter part and `shortened_reads` above 0 on one with a short part,
and a realtime leg has to take at least 90 % of its `REPLAY STREAM` `media_ms` in wall time, so an inert option or a
replay rate the host ignores fails the case instead of comparing two identical fast runs. The wall time is the host's
`REPLAY WALL` `wall_ms`, never a clock the runner reads: CMake's own timestamps read whole seconds before 3.23 (and
follow `SOURCE_DATE_EPOCH`), which would time the 85 ms `rf_clip` capture's realtime leg at 0 ms. A leg without exactly
one `REPLAY WALL` line carrying `wall_ms` fails, whatever its rate. `IQ_DETERMINISM_CHECK_WALL`
(`tests/cmake/IqDeterminismCheckWall.cmake`) holds the runner to that against a stand-in host: a missing line or one
without `wall_ms` fails, and so does 899 ms for 1000 ms of air time, while 900 ms passes. Every leg's stdout and stderr
are then compared with the first leg's, line for line, after a normalization kept as small as the measurement allows:

- ANSI colour sequences are removed.
- Four kinds of line are dropped: `NOTICE: Runtime:` (the decode loop's real-time duration), `REPLAY JITTER:` (what
  the host injected), `REPLAY WALL:` (the real time the host's run took) and the audio-sink diagnostics
  (`PulseAudio output stats:` and the other backends', and the host's `Replay sink output stats:`, the device side a
  stall changes on purpose). The input-level advisories (`WARNING: …` to raise or lower the RF gain or the source
  volume) are the decoder's own lines and are compared like the rest: their 10 s cooldown runs on decode time
  (`dsd_input_level_publish()`), so a replay prints each at the same capture time however fast it runs, even where one
  lands in the middle of a decoder line.
- Lines the reader, demod or controller threads print would be compared as a sorted set, since where they fall among
  the decoder's lines follows thread timing. The pattern list is empty, by measurement: traced per thread
  (`strace -f -e trace=write`) under every leg kind, everything these legs print comes from the decoder thread.
- CMake itself drops two differences before any of that: `execute_process` turns CRLF line ends into LF, so a CR just
  before a line break is never compared (one anywhere else in a line is), and a CMake list keeps no empty element at
  its front, so blank lines before a stream's first text line are not compared either.

Everything else is compared verbatim and in order: the decoder's lines with their `HH:MM:SS` capture-time stamps, the
`REPLAY STREAM` and `REPLAY SINK` lines and the end-of-run totals. A mismatch prints both legs' specs and the first
differing line with context. A new difference is a defect to fix, not a pattern to add to the list.

Every leg's `REPLAY STREAM` line must also count at least MIN_FSK discriminator samples and MIN_CQPSK CQPSK symbols.
The verbatim comparison already catches one leg reading less than the others; the floors catch every leg doing so,
and keep the case on the path it was written for. On `nxdn48_after_retune` under `-fa` every leg measures 582,330 and
8,189: the hunt spends about 1.37 s on the CQPSK path (6000 symbols a second, 8 samples each), so the legs cross output
kinds and generations, and the two counts cover all but 158 of the capture's 648,000 samples
(FSK = 648,000 - 158 - 8 x CQPSK). MIN_CQPSK is 5,000, just under one CQPSK dwell, a fixed 3 passes of 1,800 symbols
(`DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS`), 5,400; the other 2.8k or so of the 8,189 come from the hunt's visit and
credit accounting, which may legitimately change. MIN_FSK is 535,000, the measured count less one more full dwell
(5,400 x 8 = 43,200 samples, leaving 539,130) and a little more. MIN_TOTAL floors the two together, FSK + 8 x CQPSK,
which no split of the hunt's time between the paths moves: 647,842 measured, so 647,000. The comment beside the
registration records the measurement; re-derive all three from it when the hunt or the fixture changes.

The fixture, `nxdn48_after_retune` (1.3 MB, `DERIVED_RETUNE` in `tools/build_iq_fixtures.py`), is a call after a scan
retune: the first 7.5 s of `noise_floor` on one channel, then the four events a real scan retune records, all at byte
720,000 (a `RETUNE` to 467.75625 MHz, a 154.7 ms `retune_mute` `MUTE`, a `RESET` and a 25 ms `MUTE`, the reasons and
lengths of the first retunes in a real 1.536 Msps NXDN scan capture divided by its decimation of 32), then all of
`nxdn48`, byte for byte. Its version 2 sidecar starts the capture at 2026-01-01T00:00:00Z.
`DECODE_IQ_NXDN48_AFTER_RETUNE` (`-fi`) is the control that the eventful path decodes the call,
`IQ_INFO_NXDN48_AFTER_RETUNE` reads the event timeline back, and `DECODE_IQ_NXDN48_AFTER_RETUNE_AUTO` decodes the call
under `-fa`, which needs the hunt to land on 20 sps after the `RESET` by itself. The lead-in was chosen by sweeping it,
three fast `-fa` replays per point: 1.5-9.5 s in 0.5 s steps, then 6.0-9.0 s in 0.1 s steps (the grid the comment above
`DERIVED_RETUNE` records), 48 sweep points over 41 distinct lead-ins, since the fine grid repeats seven of the coarse
ones. The three replays were byte-identical at every point, and `-fi` decoded the call at all of them:

| Lead-in (s) | `-fa` decodes `Src=901` | Hunt steps before the `RESET` (sps) | First hunt step after it | `Src=901` lines / audio errors |
| --- | --- | --- | --- | --- |
| 1.5 | yes | 1 (20) | none | 4 / 76 |
| 2.0, 2.5 | no | 1 (20) | 5 | 0 / 8, 0 / 30 |
| 3.0-4.0 | yes | 1 (20) | 5 | 1-2 / 19-72 |
| 4.5 | yes | 2 (20, 5) | 8 | 3 / 30 |
| 5.0, 5.5 | yes | 3 (20, 5, 8) | 10 | 3 / 45, 3 / 55 |
| 6.0-6.6 | yes | 4 (20, 5, 8, 10) | 10 | 4 / 76 (6 / 86 at 6.3 and 6.4) |
| **6.7-8.1** | **yes** | **5 (20, 5, 8, 10, 10)** | **20** | **4 / 76** |
| 8.2, 8.3 | yes | 6 | none | 4 / 76, 6 / 64 |
| 8.4-8.8, 9.0 | no | 6 | 5 | 0 / 0 |
| 8.9 | yes | 6 | none | 6 / 64 |
| 9.5 | no | 6 | 5 | 0 / 8 |

The lead-in had to put at least a whole hunt rotation (five steps) before the `RESET`, with its neighbours 0.5 s either
side decoding under `-fa` too. 7.5 s sits mid-plateau: every point from 6.7 to 8.1 s reaches the `RESET` after the same
five steps and decodes identically, 0.8 s above the four-step edge (which still decodes) and about 0.85 s below the
cliff at 8.35-8.4 s, where the hunt reaches 20 sps just before the `RESET` and its dwell runs out early in the call. A
change to the hunt's dwell or to the front end's latency can move those edges. When `DECODE_IQ_NXDN48_AFTER_RETUNE_AUTO`
fails after one, run the sweep again (`build_retune_fixture()` builds a fixture at any lead-in) before moving the
lead-in.

| Case | Legs |
| --- | --- |
| `DECODE_IQ_NXDN48_AFTER_RETUNE_AUTO_DETERMINISM` | `fast;fast;jitter:7:120;jitter:11:120;short:13` |
| `DECODE_IQ_NXDN48_AFTER_RETUNE_AUTO_DETERMINISM_REALTIME` | `fast;realtime` |
| `DECODE_IQ_NXDN48_AFTER_RETUNE_AUTO_DETERMINISM_REALTIME_JITTER` | `fast;realtime+jitter:17:120` |
| `DECODE_IQ_NXDN48_AFTER_RETUNE_AUTO_DETERMINISM_STALLED_SINK` | `sink:free;sink:stalled`, only where `--wrap` links |
| `DECODE_IQ_P25P2_CC_DETERMINISM` (`p25p2_cc`, `-f2`) | `fast;short:19;jitter:23:120` |
| `DECODE_IQ_NXDN48_GAP_ADVISORY_DETERMINISM` (`nxdn48_gap`, `-fi`) | `fast;realtime;jitter:29:120` |
| `DECODE_IQ_RF_CLIP_EOF_DETERMINISM` (`rf_clip`, `-f1 -mq`) | `fast;realtime;short:37` |

`DECODE_IQ_P25P2_CC_DETERMINISM` holds the CQPSK path decoding real frames to the same rule: under `-f2` the whole
2 s capture runs the CQPSK chain (`fsk_samples=0 cqpsk_symbols=11992`, 95,936 of its 96,000 samples), and every leg
must decode its `P25p2 SACCH` lines alike; MIN_CQPSK 11,900 and MIN_TOTAL 95,000 sit just under the measurement.

`DECODE_IQ_NXDN48_GAP_ADVISORY_DETERMINISM` holds the input-level advisories to it. `nxdn48_gap` (`DERIVED_GAP` in
`tools/build_iq_fixtures.py`) is a version 2 sidecar alone: its `data_file` names `nxdn48.iq` (replay resolves it beside
the sidecar), and it adds one 12 s `MUTE` (a `driver_overflow` gap) 4 s in, so the capture spans 18 s. `nxdn48` sits
near full scale, RF HOT throughout: every leg warns 3 s into the capture and again 17 s in, the cooldown having run out
at 13 s, and decodes the call between the two (EXPECTED `RF Level HOT.*Src=901.*RF Level HOT`; it decodes `Src=901`
before the first warning too). Each leg measured
`fsk_samples=287933 cqpsk_symbols=0 media_ms=18000`; MIN_FSK and MIN_TOTAL 287,000 sit just under it. With the
cooldown on real time, before the fix, the fast and jitter legs finished before it ran out and printed only the first
warning, and the realtime leg printed both, so the case failed at the second warning's line. Four `nxdn48` captures
played back to back show the same split (one warning fast, two realtime) in 2.3 MB of data where the gap adds none.

`DECODE_IQ_RF_CLIP_EOF_DETERMINISM` holds the end of the stream to it. `rf_clip` (`DERIVED_CLIP` in
`tools/build_iq_fixtures.py`) is 4,096 I/Q pairs on the cu8 rails (85 ms, 8 KiB), one replay chunk, so the decoder takes
the front end's whole output in one read and polls the input level only while it decodes that read's samples; its next
read ends the stream. Every leg must print `RF Level CLIP 100.0%`. The replay used to end the stream on the read that
took the last samples whenever the reader had already reached the capture's end, which cleared the input level under
those polls: the fast leg, whose reader is there by then, printed no warning, and the realtime leg, whose reader is
still pacing, and the short-read leg, whose last read comes later, did. A stream now ends only on the read that finds the
ring empty (`rtl_stream_read_replay()`). With two reads in a run, a jitter leg, which sleeps after about one read in 64,
would never sleep. Each leg measured `fsk_samples=0 cqpsk_symbols=402 media_ms=85.333333`; MIN_CQPSK 400 and MIN_TOTAL
3,200 sit just under it.

On `main` before issue #572 the cases fail. The host needs the replay batch tag, which `main` lacks, so its own
`dsd-neo` ran the fast and realtime legs through the runner: `fast;fast` and `fast;realtime` both stop at the first
differing decoder line. Even with the timestamps stripped as well, three fast replays decoded 9, 9 and 13 voice frames
of the call and one `Src=901` line each, two distinct outputs in three runs, and a realtime one 50 frames and four
`Src=901` lines. Every leg on the fixed tree decodes the whole call as that realtime run did, four `Src=901` lines and
76 audio errors. Two mutations show the cases are live: leaving the decode clock on the system clock under replay
fails `_REALTIME` on a timestamp alone (`03:15:34` against `03:15:36`), and making replay audio output synchronous
fails `_STALLED_SINK`.

The eight cases under the label take 69 s in `dev-debug` run one after another, the two `nxdn48_after_retune`
realtime ones 15.6 s each, since a realtime leg takes the capture's 13.7 s of air time,
`DECODE_IQ_NXDN48_GAP_ADVISORY_DETERMINISM` 20.5 s (its realtime leg takes 18 s), `DECODE_IQ_P25P2_CC_DETERMINISM`
1.3 s and `DECODE_IQ_RF_CLIP_EOF_DETERMINISM` 1.2 s. Under `ctest -j 16` they run beside the suite's longest test, and the full
suite's wall time does not move (about 55 s). Under `tsan-debug` and `asan-ubsan-debug` they take up to 37 s each.

Repeatable is not the same as representative. Which frames a replay yields depends on where the decoder stands when each
transmission arrives, the `-fa` hunt's rotation above all but a pinned mode's symbol timing too, and a capture replayed
as recorded is one fixed draw of that: anything that moves the front end's output against the signal, an event's offset,
the filters' latency or a dwell, can win or lose a call or a frame, in either direction, with nothing decoding better or
worse. The sweep above shows it on one fixture. The uncommitted 94 s NXDN48 scan capture behind this issue
(`faNXDNScan`, 88 retunes at 1.536 Msps) shows it on a real one: the streaming filters, which hold back 1.5 ms of
look-ahead and drop it at each `RESET`, took its `-fa` replay from 144 voice frames and a caught `Src=102` call to 82
frames and no `Src=102`, as repeatably as before. Repeating a replay repeats its draw, so two builds paired over
identical repeats read a precision the capture does not have: six realtime replays of the uncommitted 35 s `fiNXDN`
capture (30 retunes) under `-fi` put the branch +4.09 +/- 0.04 errors per voice frame from the build before the
streaming filters.

A draw is a realization of the capture: a copy with every event shifted by k input samples. Over 32 of them,
k = 20i + (7i mod 20) for i = 0 to 31, spread over one 2400-baud symbol (640 samples at 1.536 Msps), the build before
the streaming filters and the branch compare as below (fast replays of `dev-release` builds, paired per realization, 95%
intervals):

- `fiNXDN`, `-fi`: SACCH CRC passes 89.3% against 87.8% (paired -1.6 +/- 3.4 points) and errors per AMBE frame 0.75
  against 0.86 (+0.12 +/- 0.16). Neither difference is resolved, and the build before the filters alone passes anywhere
  from 62% to 100% of SACCH across the realizations.
- `faNXDNScan`, `-fa`: `Src=102` caught at 25 of the 32 realizations against 17 (13 caught only before the change, 5
  only after it: McNemar p about 0.10, not settled), 123.0 voice frames against 114.7 (paired -8.3 +/- 9.1) and 4.19
  errors per voice frame against 4.14.
- `faNXDNScan`, `-fi`: `Src=102` caught at all 32 on both builds, 5.26 errors per voice frame against 4.90
  (paired -0.36 +/- 0.27, better) and SACCH 61.3% against 61.7%.

A capture of a local P25 Phase 1/2 system (593 s, 36 retunes between the control channel and Phase 2 voice channels),
replayed with `-f2 -mq -X` and its WACN, system and control channel, without `-T`, over 16 realizations, puts the branch
at -3.88 +/- 0.18 of 677 Phase 2 syncs, -2.44 +/- 0.75 of 1040 voice frames and +0.19 +/- 0.40 audio errors (equal)
against the build before the streaming filters. That loss is small and repeatable across the realizations, and all of it
comes from the channel filter's streaming change. A frame that straddles a dwell's closing `RESET` is no longer made
from the dropped tail and the new channel's first samples (10 error-type frames against 18), and about one
call-opening `MAC_ACTIVE` a run is missed at a dwell's start (paired -1.9 +/- 0.2). Against `main`, replayed realtime,
the branch is level or ahead on every count except audio errors, 25 against 24 (`docs/iq-capture-replay.md`, "Replay
Pacing And The Decode Clock").

A shift must not be a whole multiple of the front end's total decimation (the capture rate over its `demod_rate_hz`, 32
at 1.536 Msps). Replay anchors the chunk grid and restarts every filter at each event, so a shift of m whole decimations
hands every dwell the same output samples, m outputs later: it changes nothing inside a dwell, and copies shifted that
way are not independent draws of the signal. The earlier sweeps of this kind shifted mostly by multiples of 32 samples
at 1.536 Msps, so their copies were not independent draws either. A capture made at its demod rate, decimation 1, has no
other kind of shift, so its realizations vary only where each dwell starts against the decoder's own timeline.

So judge a hunt, timing or front-end change across captures and across realizations of each, with a pinned mode beside
`-fa`, never from one replay or from identical repeats of it. `tools/replay_ab.sh` replays each repeat as its own
realization (see [Decode-Quality A/B on Real Captures](#decode-quality-ab-on-real-captures)). A shifted copy by hand
needs only a new sidecar: point its `data_file` at the original data and add the same whole number of samples (2 bytes
each for cu8, 8 for cf32) to every event's `byte_offset`, keeping each within the bytes replay reads. Those are
`data_bytes`, or the data file's size when that is smaller or `data_bytes` is 0, rounded down to whole samples. The
number of samples must not be a multiple of the front end's total decimation; spread the copies across one symbol at the
capture's rate, as `replay_ab.sh` does (copy i of n shifted floor(i P / n) + i samples, P the samples in one 2400-baud
symbol, and one sample more where that lands on a multiple of the decimation).

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

The detector listens behind the NFM channel filter, so `DECODE_IQ_ANALOG_CTCSS_1000_NFM_8K` and `_NFM_25K`
(issue #525) replay `nfm_ctcss_synth_1000` through the narrowest and the widest channel (`--nfm-bandwidth-hz 8000`
and `25000`) and hold it to `DECODE_IQ_ANALOG_CTCSS_1000`'s assertions: a width must not cost the tone.

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

- Lock: the 50 tones of the table these rows were pinned on, iterated by their own indices (`analog_tone_synth.h`) so
  the seeds and pins reproduce, at four input rates, at +10 and 0 dB in-band tone-to-noise, lock within 400 ms of an
  onset placed anywhere inside a hop. 150.0 Hz, which joined the table later (issue #518 follow-up), has rows of its
  own: at every rate at +10, +3 and 0 dB it locks within the lock contract on 20 seeds each, and with 151.4 Hz, each on
  its value and 0.2 or 0.35 Hz either side, it never reads as the other on its seeds. Tones off their table value by
  transmitter encoder error lock on that value: 0.2 and 0.35 Hz
  off within 400 ms at +10 dB, and the exact tone at 0 dB on a second seed set within 400 ms too; 0.2 Hz off at 0 dB,
  99% of the starts lock within 400 ms and the slowest at 468 ms (the row asserts at least 95% and 500 ms). The lock
  gate is 0.5 Hz, so encoder error trades against lock time. The 32 slowest of the 2,000,000 long-run starts at 0 dB
  as the 250 ms window alone locked them (8 exact tones and 24 tones 0.2 Hz off, from 701 ms to 1,128 ms), rebuilt from
  the sweep's own seeds, each lock within the 700 ms ceiling (the slowest at 509 ms). Under transmitter-filtered speech 10 dB above the tone
  every tone locks within 500 ms and holds that one lock to the end of the run; a tone held 15 s at 0 dB, at each rate,
  locks once and is never lost.
- Rejection: 67.0/69.3/71.9 Hz are each identified; off-table tones lock nothing over 3 s runs down to 0 dB in-band
  (152.6 Hz, 1.2 Hz above 151.4, and 68.2, 161.0 and 166.7 Hz between two table tones), nor do 100.85 and 100.9 Hz,
  just outside the snap
  gate of 100.0, at +10 and +20 dB; a locked tone that moves off the table is dropped within 450 ms (one window plus
  four failing hops) and nothing locks in its place; no DCS waveform locks a CTCSS tone, in the publication or in the
  CTCSS detector's own state, where the DCS lock that outranks it would hide one -- every rotation class of the
  Golay (23,12) code, forward and bit-reversed, which is every periodic DCS waveform in either polarity (what
  the DCS detector makes of each is under [Received code (DCS)](#received-code-dcs-on-the-analog-monitor)),
  plus the words that come nearest a table tone, which lock nothing at all at every rate, clean, at +10 dB
  and at 0 dB; and a minute each of unfiltered speech, transmitter-filtered speech and noise never locks and
  reaches the `none` verdict, which the speech runs hold for 87% of their carrier time (the rows assert 80%;
  each pause that closes the carrier starts the verdict again). Two more minutes of speech, one filtered and
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
- Loss and verdicts (the 50 pinned tones, as for lock; 150.0 Hz has rows of its own, below): every tone at every
  rate is dropped after it stops under a live carrier, at +10 dB 99% of the stops
  within 350 ms and all within 386 ms, at 0 dB 98% within 350 ms and all within 476 ms (the rows assert 98% and 400 ms,
  97% and 500 ms), and nothing locks again from what the detector still holds of the stopped tone; every tone at every rate is dropped within 150 ms of a reverse burst at +10 dB, a 180 degree one and
  a 120 and a 240 degree one alike, the flip landing anywhere inside a sub-block; at 0 dB, with a 180 degree flip held
  400 ms before the carrier drops, 91% of the bursts are caught within 150 ms and every caught one within the loss
  contract, and the 4 of 200 that are missed end with the carrier drop, within its 200 ms hangover (the row asserts 89%
  and at most 5 missed); a 120 or 240 degree burst inside the sub-block a tone locks on -- driven through the detector
  alone at +10 dB, every tone on its table value and 0.15 Hz either side, the step at every eighth sample of that
  sub-block -- ends the lock within 150 ms after every one of the 2,887 steps that still lock the tone on that hop, and
  after all but 15 of 2,859 when the tone has just moved there from another table tone it was locked on (2,882 and
  2,852 before 150.0 Hz's bin joined the detector; 248 of those
  300 tones lock in place of the old tone's lock, the rest just after losing it); the rows assert at most 2 and 20, and
  measured against the lock hop's own estimate 115 and 143 were missed; a tone that stops under a carrier that keeps
  dropping out (up for 20 ms of every 40 to 180 ms,
  a voice-band tone and noise between stretches of digital silence, so the carrier never expires) is dropped at every
  rate, period and block size, 95% of the 100 stops within 427 ms and all within 457 ms, and nothing locks again (the
  row asserts the 800 ms loss ceiling only, since the p95 target is for a live carrier); a carrier with no tone reads
  `detecting` until 500 ms of it and `none` by the next hop, and a tone that starts after that verdict still locks
  within the lock bound; the verdict, the no-tone one included, is identical at the three input scales and for any
  block size; the front end runs from 2400 Hz up to its 320 kHz limit and reports itself unavailable outside it.
- 150.0 Hz loss: at every rate, on seeds of its own, it is dropped after it stops under a live carrier, 40 stops each
  at +10 and 0 dB (p95 312 and 289 ms, all within 318 ms), nothing locks again, and a 180 degree reverse burst at
  +10 dB ends the lock within 121 ms on all 40.

Fixed seeds say what the detector does on those seeds, not how often it misses in the long run. The long-run figures
below come from wider seed sweeps run offline over the same generators and the same core (x86-64 at -O0 and -O2 give
identical results), and they are the numbers the user guide quotes:

- Lock, 10,000 starts each (every tone of the 50-tone table at every rate, onset anywhere in a hop, measured before
  150.0 Hz joined the table; the 150.0/151.4 Hz pair's own sweeps are below): at +10 dB, p95 255 ms and none beyond
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
- Off-table tones, two hours of continuous carrier at 0 dB each, at 48 kHz and 8 kHz: 68.2 Hz locked a neighbour 20
  and 25 times, 161.0 Hz 5 and 5, 166.7 Hz 6 and 4, each for 200-260 ms; at +3, +6 and +10 dB, 68.2 and 161.0 Hz never
  locked in two hours. Before 150.0 Hz joined the table it never locked in two hours either; with it, 152.6 Hz (1.2 Hz
  above 151.4, whose gate is now 0.7 Hz) locked 151.4 Hz 2 and 3 times, each for 200 ms.
- 150.0 and 151.4 Hz (issue #518 follow-up), 1.4 Hz apart with a 0.7 Hz gate each, 10,000 starts per row (every rate,
  onset anywhere in a hop, the offset alternately toward and away from the other tone): on the table value at +10 dB,
  p95 257 and 260 ms and never the other tone; at 0 dB, p95 343 ms for both, 0.32% and 0.37% beyond 400 ms, and the
  other tone named on 3 and 1 starts; 0.2 Hz off at 0 dB, p95 358 and 362 ms, 1.53% and 1.68% beyond 400 ms, the other
  tone on 4 and 3; 0.35 Hz off at +10 dB, p95 283 and 292 ms, the other tone on 0 and 12; 0.35 Hz off at 0 dB, p95 426
  and 430 ms, the other tone on 50 and 61. Every such start was toward the other tone, named it for 9-202 ms (mostly
  100-150) while the window still held the noise before the onset, which pulls the estimate toward the nearer bin, and
  then locked its own value. Two hours of continuous carrier at 0 dB, at 48 and 8 kHz: on their values, never the other
  tone and never a drop; 0.35 Hz toward each other, the other tone 55 and 38 times (150.35 Hz) and 51 and 57 times
  (151.05 Hz), each for 200 ms. The ad-hoc driver links the same generators and core at -O2.
- Holds, 200 runs of 20 s: at 0 and +3 dB a held tone never dropped. Under transmitter-filtered speech 10 dB above the
  tone, 300 runs of 20 s: 4 drops, all on 250.3 and 254.1 Hz, each reading `none` for 340-740 ms before the same tone
  locked again.
- Talk-off: over two hours of seeds the unfiltered speech model locked a tone three times (at 233.6, 241.8 and
  254.1 Hz, where a high voice with weak harmonics holds near a table tone), and the transmitter-filtered model once
  (at 254.1 Hz). The 250 ms window alone locked the 233.6 Hz and the filtered 254.1 Hz voice too; without the check
  that the newest 250 ms still carry the tone, the late windows would lock another five unfiltered and one filtered.

Detection runs on the FM monitor only (issue #524), DCS (issue #523) as well as CTCSS. `DSP_SYMBOL_REPLAY` feeds the
same CTCSS-bearing monitor blocks, and the same D023N-bearing ones, through the tap on the FM monitor, where they lock,
and on the AM monitor, where nothing is published or logged and a live switch to AM forgets the FM lock;
`APP_COMMAND_QUEUE` checks that the decode-mode switch from Analog to AM clears a received tone or code, and
`RUNTIME_ANALOG_TONES` pins the predicate every frontend's row asks. The `Received tone:` forbid on
`DECODE_IQ_ANALOG_AM_REAL` only guards against a false report: that excerpt carries no sub-audible tone or code.

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
bursts, also when the reset came in a digital mode before detection started, on stdin input whose monitor playback holds
the decoder for each block's 20 ms, and with a raw WAV whose per-block sync to disk holds it 12 or 15 ms (issue #576; a
test hook stands in for the sync, so no test waits on the disk), which makes each 20 ms of the backlog take more than 10
ms to read but is no time spent waiting inside the input read, and while the decoder is descheduled 3 or 9 ms inside
each backlog read, which is time in the read but leaves the span short of half its length, also with stdin's synchronous
playback holding it for each block's 20 ms on top, which only leaving the time spent playing out keeps short of half --
and its own 131.8 Hz tone locks within the p95 target plus one read, while a WAV file is heard at once after a reset, a
UDP stream with nothing queued from the third read after a reset or a generation move, a new channel arriving live after
the backlog on a decoder a 15 or 17 ms sync holds, which leaves it only 5 or 3 ms of each 20 ms read to wait, from its
second read (an 18 ms sync, 2 ms, is below the eighth of a read that shows the input ran dry, and is heard only 2 s
after the retune), and stdin fed faster than real time again after 2 s; driven through `getSymbol()` on UDP input, where
the symbol path brackets the live read and a fake producer delivers each sample at its real-time arrival on the injected
clock, keeps the same skip across a 400 ms hold with a 12 ms sync per block, both after a reset and after a generation
move: the old tone never returns, nothing is heard until a read has had to wait for the producer, well short of the 2 s
cap, and 131.8 Hz then locks within the
p95 target plus one read; restarts the tap at the first sample of the next block when the symbol path drops a
part-collected block (`dsd_symbol_analog_block_reset()` on a receive-family change, or a family switch landing on an
RTL front end), where read on from its place in the dropped block it missed the new block's opening samples; and --
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
    --mode -fA --reps 3 --out /tmp/ab/neighbour /tmp/ab/analog_replay.main /tmp/ab/analog_replay.branch
```

The hosts are set up as under [Analog A/B](#analog-ab), which says why three repeats are enough. Over the 12 realtime
repeats measured when this case was added, the detector locked 173.8 Hz at 300 ms on every one, reported no other
tone, and dropped and re-locked it once around a similar reversal and gap at 2.44 s, 90.33% locked in all.

#### Received code (DCS) on the analog monitor

`DECODE_IQ_ANALOG_DCS_023N`, `_023I`, `_NOISY`, `_DROP` and `_NOCODE` (issue #523) replay synthetic NFM under
`-fA -o null` like the CTCSS cases. The fixtures (`nfm_dcs_synth_023n`, `nfm_dcs_synth_023i`, `nfm_dcs_synth_noisy`,
`nfm_dcs_synth_drop`) are built by `python3 tools/build_iq_fixtures.py --derived-only`: the same voice-band audio and
receiver noise with a DCS word in place of the tone, repeated at 134.4 bit/s, bit 0 first, as NRZ at 600 Hz deviation
(a one as positive deviation in normal polarity, the complement in inverted polarity), low-passed below 300 Hz. The
words come from the builder's own Golay encoder, whose layout `RUNTIME_ANALOG_TONES` and `DSP_ANALOG_DCS_GOLAY_XCHECK`
pin against the published words, so the fixtures are correct by construction. No public off-air DCS recording exists
(see [Tone and code labels](#tone-and-code-labels)); a real accept fixture waits for a maintainer recording.

- `_023N`: must log `Received tone: DCS D023N / D047I` (both spellings of the signal, canonical first), and never
  `none`, a tone or another code.
- `_023I`: D023 sent in inverted polarity is the signal of D047N and must log `Received tone: DCS D047N / D023I`, never
  `DCS D023N / D047I`, which a detector that ignored polarity would report. Together the two cases pin the polarity
  convention end to end, through the RTL replay chain; `DSP_FM_DEMOD_REF` pins the discriminator's sign on its own (a
  carrier above the tuned frequency demodulates positive, and D023N sent as +/-600 Hz NRZ reads back as D023N).
- `_NOISY`: D023N's word sent two bits wrong, the same two bits in every repetition, which is no code (two bits from
  D023N and, the code words being at least 7 bits apart, at least 5 from every other); it must log `none` and never a
  code or a tone.
- `_DROP`: the code stops at 1.2 s with no turn-off tone while the carrier and voice carry on: `DCS D023N / D047I`, then
  `none`.
- `_NOCODE`: `nfm_notone_synth` through the narrowest NFM channel (`--nfm-bandwidth-hz 8000`), voice with no code:
  `none`, never a code. `DECODE_IQ_ANALOG_CTCSS_NOTONE`, which fails on a code too, replays the same voice at the
  default width.

The CTCSS cases and the off-air cases fail on any DCS code too, so none of the tone fixtures, the squelch captures'
150 bit/s data or AM airband voice reads as a code. `DECODE_IQ_ANALOG_DCS_023N_HOST` and `_023I_HOST` replay the two
accept fixtures through the analog replay host and match its `tone=` field, which reads the publication's code as both
spellings of its signal, canonical first: `tone=D023N/D047I` and `tone=D047N/D023I` (never `D023N/D047I` for the
inverted word), locked within the 520 ms bound (360 ms measured, 83% of each run locked). Over 12 realtime
`replay_ab.sh --metric analog` repeats against `main` (see [Analog A/B](#analog-ab)), this build read `D023N/D047I`,
`D047N/D023I` and `D023N/D047I` on `_023n`, `_023i` and `_drop` on every repeat (first lock 360 ms on each; 83, 83 and
57% locked), where `main`, which has no DCS detector, reads `NA`; `_noisy`, both squelch captures and `nfm_ctcss_real`
(151.4 Hz at 300 ms, 88.67% locked) read the same in both builds, with every audio column unchanged.

The detector's own bounds are pinned in sample time by `DSP_ANALOG_DCS`, through the pure receive core. Its signals are
built the way a receiver hears them: the transmitter's NRZ word through the receiver's de-emphasis (75 us, or the
750 us land-mobile option) and the demodulator's DC block (`dc += (x - dc) / 2^11`, as `demod_pipeline.cpp` runs it),
plus white noise at an in-band (0-290 Hz) signal-to-noise ratio against the NRZ's power, all from seeded generators.
Its pins are stronger than the per-event ceilings in `<dsd-neo/dsp/analog_rx.h>`: every stop meets the loss p95
target on its own, and at 3 dB every row through the demodulator's DC block meets the lock p95 target and every
fixed-seed start there locks within 700 ms.

- Lock: every code in both polarities at 48 kHz, with both de-emphasis settings, within 520 ms of its onset at 10 dB and
  700 ms at 3 dB (p50 347-356 ms; the slowest 400 ms at 10 dB and 529 ms at 3 dB), each 3 dB row within the 450 ms p95
  target; at 8, 44.1, 48 and 78.125 kHz, every eighth code in both polarities within 520 ms at 10 dB and every second
  code, 104 starts a row, within 700 ms at 3 dB, each 3 dB row within the p95 target (p95 370, 379, 393 and 415 ms;
  worst 529 ms at 78.125 kHz); PCM input through a 10 Hz sound card coupling in place of the demodulator's DC block, at
  48 kHz with 75 us, every second code in both polarities within 520 ms at 10 dB (worst 376 ms), and at 3 dB, which the
  timing contract does not cover through a coupling, every code in both polarities within the row's own pins, p95 550 ms
  and 1,000 ms a start (p95 436 ms, worst 683 ms); a DC step at the onset, as a carrier off frequency puts under the
  code: through a DC-coupled PCM input at 48 kHz, every second code in both polarities with a step of 1, 2 and 4 times
  its level, up or down, within 520 ms at 10 dB (worst 413 ms), and every code in both polarities with a 4x step within
  700 ms at 3 dB and the 450 ms p95 target (p95 390 ms, worst 473 ms); through the demodulator's DC block at 8 kHz,
  where it removes a step slowest, every second code with a 4x step within 520 ms at 10 dB (worst 422 ms) -- the
  detector without its balance slicer fails the first row (some starts not locked within 670 ms, p95 639 ms); a code
  under transmitter-filtered speech 10 dB above it within 700 ms, and held from then on for 20 s; a transmitter at 134.3
  or 134.5 bit/s locks and holds for 8 s; the alias pins (D023I is published as D047N, D047I as D023N, D754I as D116N
  and others) through the detector.
- Acquisition: a code whose every other word is damaged, so that no two windows 23 bits apart match, started at the top
  of a clean word once random bits have settled the receiver, at 8 kHz: one bit off, the bit spread over the word, locks
  every code in both polarities within 520 ms of its start, and bits 3 and 12 off lock none. A rule asking for both
  windows exactly fails the first, and one allowing two bits the second.
- Hold and loss: one wrong bit in every word, in the same bit or moving through the word, holds for 20 s; two wrong bits
  in every word lose the lock within 522 ms of the damage (a window holds both errors only once it lies wholly after the
  damage started, up to a word later), and nothing locks in its place; a code that stops under a live carrier is lost
  within 350 ms (p50 293 ms), and the 134.4 Hz turn-off tone within 150 ms (p50 76 ms, worst 99 ms); a steady 130, 134.4
  or 140 Hz component at the code's power or 3 dB above it, starting under a held code, never ends the lock (the
  turn-off rule waits for the code to go too); a carrier drop is forgotten within the 200 ms hangover, and a 150 ms
  dropout inside a transmission keeps the lock; that hangover running out, and the core's reset under a running code,
  unlock the detector at once: a carrier back with no code never locks, one back with another code never shows the old
  one, and the same code shows again only once read twice from scratch, not within 330 ms of the reset; a code that
  stops under a carrier up for 20 ms of every 40-180 ms is lost within the 64-bit span and a word of the stop (649 ms;
  worst 577 ms over 100 cases at every rate, 1 and 20 ms blocks), with the carrier open throughout; the same code
  starting over 1 to 22 bits and a share of a bit further on in its word, on a continuous carrier and after 120 and
  190 ms gaps, keeps one lock and never reads `none`.
- Publication: a code outranks a tone. A CTCSS tone that locks under a held code never shows while the code holds, and a
  code that locks under a held tone replaces it at once, within the 10 dB bound of the code's start; both detectors are
  locked meanwhile, so the rule, not a missed detection, decides the display. The two long-run starts at 10 dB in which
  a code's own waveform read as a CTCSS tone before the code locked (D274N as 67.0 Hz at 8 kHz, D122N as 77.0 Hz at
  78.125 kHz), replayed exactly, show the code 346 and 357 ms after its onset; a rule that let the first lock keep the
  publication showed D122N only after 535 ms, when the tone was lost.
- Rejection: ten minutes of random bits at 134.4 bit/s; the 74 rotation classes of the Golay (23,12) code that carry
  no standard code, in both polarities; every standard word sent bit-reversed that is more than one bit from every
  standard word; every CTCSS tone at 8 and 48 kHz, clean and at 10 dB, never locking even for a moment; and two
  minutes each of unfiltered and transmitter-filtered speech: none locks. A carrier with no code reads `detecting`
  until 500 ms, then `none`, and a code that starts later still locks within its bound. A rate beyond the detector's
  sample ring (none the front end delivers) leaves it inert, reporting no code.
- Invariance: the RTL live, replay and int16 PCM scales lock in the same block, and block size never moves the lock.

`DSP_ANALOG_CTCSS` also checks that every supported code's waveform locks once as DCS, under its canonical name, and
never as a CTCSS tone, reading the CTCSS detector's own state, where the code's lock would hide one from the
publication; that every other periodic waveform locks nothing, not even for a moment, unless it is one bit from a
supported word, when it locks once as that code; and that the words that come nearest to a table tone, at least three
bits from every supported word, lock nothing at every rate and SNR. `DSP_SYMBOL_REPLAY` runs D023N and the inverted word
through the real tap and checks the `Received tone: DCS` line, and the resets that clear a tone are shown to clear a
code as well: a trunk-tuning or RTL stream generation move and an announced reset in `DSP_SYMBOL_REPLAY`,
`RTL_SET_FREQ`, `MANUAL_TUNE` and a decode-mode change in `APP_COMMAND_QUEUE` (each named for the tone and the code
pass), a `-Y` row commit in `ENGINE_CHANNEL_SCAN`, and an `nfm-conventional` target switch in `ENGINE_TRUNK_SCAN` (the
visit cap moving on under the carrier that carries a code, then an operator advance back). The two scan tests clear the
code through their own stub of the acquisition reset each switch calls, so they show that the switch calls it and that
no parked state brings the code back; `FRAME_SYNC_INTERNAL_HELPERS` checks that the real reset clears a code, called as
a trunk-scan target switch calls it. That a reset clears the detector itself, not only the publication, is shown twice:
in `DSP_SYMBOL_REPLAY` the same code running on through a generation move is neither shown nor logged for 300 ms after
it, less than the 342 ms a lock read from scratch needs, and after the announced reset the inverted word locks as D047N
with D023N never shown again; `DSP_ANALOG_DCS` runs the core's own reset (the carrier hangover running out, and
`dsd_analog_rx_core_reset()` under a running code) and checks the detector is unlocked at once and the old code never
shows again. A reset that kept a held lock fails both. `APP_CONTROL_RX_TONE_VIEW`, `UI_NCURSES_PRINTER_HELPERS`,
`UI_QT_METRICS_MODEL` and `UI_QT_QML_CALL_LISTS` pin the `DCS D023N / D047I` text (both spellings of the signal,
canonical first, with the second one's code and polarity beside the first's), the names the view refuses (an unsupported
code, a rotation alias, a non-canonical polarity), and the received code kept apart from a configured policy value.
`RUNTIME_ANALOG_TONES` pins that every standard signal has exactly two standard spellings, the canonical normal code and
one inverted code, against the golden alias table, and the label that names both; and that every supported code's word
carries 11 or 12 ones in either polarity (000's carries 7), which the balance slicer relies on. `RUNTIME_ANALOG_TONES`
and `DSP_ANALOG_DCS_GOLAY_XCHECK` pin the published words of 023, the inverted 023, 047, 020 and 000 (onfreq's DPL/DCS
page), 047 a standard code of its own and 020 a word of another rotation class, so check bits that do not follow from
023's, before and after the mapping onto `Golay24.hpp`.

Long-run sweeps (offline, the `DSP_ANALOG_DCS` signal model through the pure receive core, a random code and polarity
per start with the onset anywhere in a word; `<dsd-neo/dsp/analog_rx.h>` sets its ceilings above the slowest event):

- Lock at 10 dB, 7,000,000 starts: 500,000 each with 75 and 750 us at 48 kHz, p95 369 and 370 ms, the slowest 429 and
  422 ms; 1,000,000 each with 75 and 750 us at 8, 44.1 and 78.125 kHz, p95 367-370 ms, p99.99 398-412 ms, the slowest
  437 and 445 ms at 8 kHz, 428 and 434 ms at 44.1 kHz and 428 and 424 ms at 78.125 kHz (75, then 750 us). No code's own
  waveform read as a CTCSS tone first. An earlier 7,000,000-start sweep, on other seeds and before the balance slicer,
  found it twice (D274N as 67.0 Hz at 8 kHz, D122N as 77.0 Hz at 78.125 kHz, both 75 us), shown for 161 and 71 ms
  until the code outranked it; `DSP_ANALOG_DCS` replays both on this detector. Under a rule that let the first lock
  keep the publication, D122N showed only after 535 ms, when the tone was lost.
- Lock at 3 dB, 8,500,000 starts: with 75 us, p95 376, 382, 385 and 399 ms at 8, 44.1, 48 and 78.125 kHz (500,000,
  500,000, 1,000,000 and 1,000,000 starts), the slowest 868, 830, 861 and 1,022 ms; with 750 us, p95 385, 392, 396
  and 415 ms (500,000, 500,000, 2,000,000 and 2,500,000 starts), p99.9 at most 690 ms, the slowest 781, 910, 992 and
  1,224 ms. 23 starts took longer than a second, all at 78.125 kHz and 21 of them with 750 us, where the demodulator's
  DC block sags the most and the de-emphasis smears a code's isolated bits. None read a tone first.
- The acquisition rule, 100,000 starts per rate (8, 44.1, 48 and 78.125 kHz) and de-emphasis at 3 and 10 dB, against the
  same detector asking for both windows exactly: at 3 dB p95 377-416 ms against 436-666 ms (past the 450 ms target in
  7 rows of 8), at most 0.1% of starts past 700 ms against 0.3-3.5%, and none past the 1,500 ms ceiling against 8 (the
  slowest 1,662 ms); at 10 dB the slowest 422 ms against 553 ms, 2 of 800,000 past 520 ms.
- A DC step at the code's onset, from a carrier off frequency, of 1, 2 and 4 times the code's deviation up or down,
  50,000 starts per rate (8, 44.1, 48 and 78.125 kHz) and condition, 75 us (and 750 us for the 4x step through the
  demodulator's DC block and a DC-coupled input): through the demodulator's DC block, a DC-coupled PCM input and a 1 Hz
  coupling, every one of the 1,200,000 starts at 10 dB with a 1 or 2x step within 520 ms (the slowest 473 ms,
  DC-coupled) and p95 at most 421 ms at 3 dB (the slowest 882 ms); with a 4x step 1 of 1,000,000 at 10 dB past 520 ms
  (526 ms, DC-coupled at 48 kHz with 750 us, the next 496 ms) and at 3 dB p95 up to 474 ms (a 1 Hz coupling; 473 ms
  through the demodulator's DC block at 8 kHz with 750 us), the slowest 1,020 ms. Through a 10 Hz coupling every start
  at 10 dB within 520 ms (the slowest 511 ms), and at 3 dB p95 482-544 ms (475-483 ms without a step). A DC-coupled
  input with no step: p95 367 ms at 10 dB and 377 ms at 3 dB. Once a code's own waveform read as a CTCSS tone first
  (D225I as 77.0 Hz for 50 ms, a 2x step through a 10 Hz coupling at 44.1 kHz and 10 dB). The detector before the
  balance slicer, through a DC-coupled input with a 2x step at 48 kHz and 10 dB: p50 617 ms, 19,990 of 20,000 starts
  past 520 ms; with a 4x step p50 837 ms, every one past it.
- PCM input through a sound card's coupling in place of the demodulator's DC block (a one-pole high-pass, 75 and 750 us,
  75 then 750 us in each pair below): with a 10 Hz corner at 10 dB, 200,000 starts each at 44.1 and 48 kHz, p95
  371-372 ms, the slowest 441 and 451 ms at 44.1 kHz and 477 and 465 ms at 48 kHz; at 3 dB, 200,000 each at 44.1 kHz and
  500,000 each at 48 kHz, p95 475 and 525 ms at 44.1 kHz and 483 and 535 ms at 48 kHz, p99.9 837-930 ms, the slowest
  1,372 and 1,605 ms at 44.1 kHz and 1,463 and 1,702 ms at 48 kHz. With a 15 Hz corner at 48 kHz and 75 us, 100,000
  starts each: 5 past 520 ms at 10 dB (the slowest 649 ms), and at 3 dB p95 716 ms, the slowest 2,284 ms. A droop
  slicer that matched the 10 Hz coupling (0.63 a bit in place of 0.55) was slower there (p95 518 against 485 ms at
  48 kHz, 75 us, 3 dB, the same 100,000 starts).
- Loss when the code stops under a live carrier, 1,000,000 stops at 3 and 10 dB over the same rates and de-emphasis:
  p95 at most 328 ms, p99.9 at most 377 ms, the slowest 591 ms. About 9 stops in 100,000 take longer than 450 ms: the
  noise that follows reads as the code once more (within one bit of the word expected next, or exactly at another
  place in it), which starts the 32 bits over. The balance slicer holds only on an exact read: allowed a bit of slack,
  it read noise as the code about twice as often (25 against 14 of 200,000 stops past 450 ms at 48 kHz, 75 us, 3 dB).
- Loss on the turn-off tone, 1,000,000 at 3 and 10 dB: p95 at most 134 ms (750 us at 3 dB; 97-109 ms otherwise), p99.9
  at most 224 ms, the slowest 353 ms (78.125 kHz, 750 us, 3 dB, a turn-off the detector before the balance slicer reads
  the same, which is why the ceiling is 400 ms and not 350).
- Loss under a flickering carrier (openings of 1-60 ms between dropouts of 10-199 ms from the stop), 16,000 stops at 8,
  44.1, 48 and 78.125 kHz, 3 and 10 dB: p95 at most 548 ms, the slowest 619 ms (a loss that counted only bits read with
  the carrier open, with no span, took p95 1,270 ms and the slowest 1,755 ms at 48 kHz). After a single 120 ms dropout
  with the code gone, 16,000 returns: lost p95 368 ms after the carrier came back, the slowest 591 ms (twice in 16,000
  the noise read as the code once more).
- The same code at another place in its word, a shift of 1-22 bits and a random share of a bit, half on a continuous
  carrier and half after a 120 or 190 ms gap: at 10 and 20 dB, of 7,040 restarts at 8, 44.1, 48 and 78.125 kHz one
  showed `none` briefly (48 kHz, 10 dB, after a 190 ms gap, as the detector before the balance slicer does too) and none
  another code (a hold that followed only the expected place dropped 396 of 440 to `none` at 48 kHz, 20 dB); at 3 dB,
  0-19 of 440 per condition dropped and locked again.
- Holds (random codes, 60 s each): at 3 dB with nothing else in the band, a held code dropped once in 200 minutes with
  750 us at 78.125 kHz and once in 200 minutes with 75 us at 48 kHz, never in 200 minutes with 750 us at 48 kHz, each
  time locking again, and never in 20 minutes at 10 dB; with a steady 130 Hz component 3 dB above the code or 134.4 Hz
  at its power, about once or twice a minute at 3 dB and never at 10 dB; under transmitter-filtered speech 10 and 20 dB
  above the code never in 12 minutes each; under unfiltered speech 10 dB above it, outside the contract, about 7 times
  a minute (the code shown 96% of the time; before the balance slicer 9 times a minute and 94%); at 0 dB in-band,
  outside the contract, about twice a minute. A DC-coupled input with a 4x step at the onset of each 10 s hold, at 3 and
  10 dB, and the demodulator's DC block with the same step at 8 kHz and at 48 kHz with 750 us, 3 dB: 200 holds each, no
  loss.

Known gaps and caveats:

- **X2-TDMA** has no usable public sample and is untested here.
- **dPMR** decodes only what its CCH CRC-7 verifies (issue #407), so the `dpmr` off-air capture publishes nothing:
  it carries no recoverable CCH, which is why the CRC was thought to be broken. `DECODE_IQ_DPMR_MARGINAL` pins that
  it still syncs and still publishes no identity; `DECODE_IQ_DPMR_SYNTH` is the accept case.
- **P25 Phase 2** asserts SACCH framing only. Full payload decode needs the
  system WACN/SYSID/CC via `-X`, which the public sample does not identify.
- Call-state timers run on the capture's clock under replay (issue #572), so an
  assertion that depends on one holds in `fast` and `realtime` replay alike.
  `-T` and `-Y` replays are outside that guarantee (see
  [Replay determinism](#replay-determinism-issue-572)): under `-T` the P25
  trunking watchdog checks its timers at a real-time cadence, and under `-Y`
  replay refuses the scanner's retunes, so its hangtime and visit timers cannot
  follow the scan they time.
- **AM** cases run `-fM`, native AM reception (issue #524): `DECODE_IQ_ANALOG_AM_*` in the table below. Under `-fA` the
  FM monitor demodulates `am_airband_real`, which measures an FM discriminator on an AM signal, so `-fA` cannot stand in
  for an AM case (`DECODE_IQ_ANALOG_REAL_CTCSS_NOFALSE_AM` uses it under `-fA` only as no-false-lock material for the
  tone detector). The excerpt serves `-fA` for another reason: its carrier sits within a few hertz of 0 Hz, where the
  modulation auto-switch (`frame_sync_maybe_auto_switch_modulation()` in `src/dsp/dsd_frame_sync.c`) votes for CQPSK.
  The switch used to run in analog-only mode too, because `-fA` does not set `opts->mod_cli_lock`: it applied the P25
  CQPSK demod profile to the RTL front end, which then delivered CQPSK symbols instead of monitor audio, after 0 or 20
  ms of monitor audio in `fast` replay and 680 ms in `realtime` (the switch's dwell was then timed by the wall
  clock). The analog family (`dsd_opts_is_analog_family()`) now stands the switch down, so the front end stays on the
  monitor path whatever the carrier offset, and replay of the excerpt is sample-deterministic. Two tests pin it:
  `FRAME_SYNC_INTERNAL_HELPERS` feeds the switch CQPSK-favouring metrics under the analog preset and requires no vote
  and no demod profile, with no clock involved, and `DECODE_IQ_ANALOG_NO_MOD_AUTO_SWITCH` replays the excerpt under
  `-fA` and requires all 8000 ms on the monitor path. The vote is not the hunt's only request: every symbol profile the
  sync hunt asks for goes through `rtl_maybe_apply_demod_profile()`, which sends the RTL front end nothing in
  analog-only mode, so a two-level profile the hunt re-normalises when its dwell runs out (a live switch to `-fA` from
  D-STAR leaves the hunt on 4800/2) cannot narrow the monitor to a digital channel either; `FRAME_SYNC_INTERNAL_HELPERS`
  pins that guard on both paths. The analog replay host still warns whenever the front end delivers CQPSK symbols, since
  symbols are not samples at the output rate and its stream clock then does not measure stream time, and every
  registered `-fA` case fails on that warning (`NOT_EXPECTED`); `tools/replay_ab.sh` leaves such repeats out (its
  `off_path` column). An AM case on this excerpt (`-fM`) carries the same guard.

### Analog monitor audio checks

The `DECODE_IQ_ANALOG_*` audio cases (CTest labels `iq-decode` and `analog`, radio builds only) check what the analog
FM monitor lets a listener hear; the received-tone cases `DECODE_IQ_ANALOG_CTCSS_*` and `DECODE_IQ_ANALOG_DCS_*` carry
the same labels and match a log line, and `DECODE_IQ_ANALOG_REAL_CTCSS_*`, `DECODE_IQ_ANALOG_DCS_023N_HOST` and
`DECODE_IQ_ANALOG_DCS_023I_HOST` also read the received tone or code through this host (see
[Received tone (CTCSS) on the analog monitor](#received-tone-ctcss-on-the-analog-monitor) and
[Received code (DCS) on the analog monitor](#received-code-dcs-on-the-analog-monitor)).
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
audio block is scored as delivered, 20 ms at the monitor's rate, after the voice band-pass and the gain stage: the
AGC by default, or a fixed gain with `-n N`.

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
| `--analog-scan-row ROW` | Enter row `ROW` of the `-C` channel map before the replay, the way the conventional scanner commits it (its class, then its own options), and print `Scan row applied: ROW <class>; width <W>; squelch <S>`, each with `(row)` when the row sets it. I/Q replay cannot retune, so one row is all a run visits (issue #526). |
| `--analog-iq-gain-db DB` | Replay a copy of the capture with every sample scaled by `DB` about the cu8 midpoint: the same air, noise included, as a receiver with that much more (or less) gain records it. It works on the committed fixtures only (`tests/fixtures/iq`, opened by their directory entries there, never by a path from the command line or a sidecar). The copy goes to a private temporary directory the host removes at exit, and the host prints `capture scaled by +10.0 dB, N of M bytes clipped`; it stops (exit 2) when more than one byte in a thousand would clip, or when the capture is not a committed fixture. The auto squelch's `_HOT` cases use it, so a hotter receiver costs no committed fixture. |

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
| `DECODE_IQ_ANALOG_NFM_TONE` | `nfm_tone_synth` | tone SNR 37.5 dB, in-band 52.3 dB, RMS -13.2 dBFS at the AGC's -10 dBFS peak, audible 1500 of 1500 ms from 0 ms, no clipping; a 1 kHz probe reads 0.00 dBc | SNR ≥ 30, captured and audible ≥ 1400 ms, first audible ≤ 100 ms, in-band ≥ 45, RMS -14.5 to -12 dBFS and peak ≤ -9.5 dBFS (the window `AM_TONE` shares), clip 0, 1 kHz probe within ±1 dBc |
| `DECODE_IQ_ANALOG_NFM_TONE_FIXED50` | `nfm_tone_synth`, `-n 50` | RMS -15.9 dBFS, peak -12.4 dBFS: the fixed gain's calibration (the reference signal at -12 dBFS peak, -15 dBFS RMS) | RMS -16.9 to -14.9 dBFS, captured ≥ 1400 ms, clip 0 |
| `DECODE_IQ_ANALOG_NFM_ADJACENT` | `nfm_adjacent_synth`, `-v 0 -n 50` | tone SNR 36.6 dB; 12.5 kHz probe -76.6 dBc | SNR ≥ 30, captured ≥ 1400 ms, 12.5 kHz ≤ -40 dBc |
| `DECODE_IQ_ANALOG_NFM_TONE_8K` | `nfm_tone_synth`, `-v 0 -n 50 --nfm-bandwidth-hz 8000` | tone SNR 28.0 dB (the 8 kHz channel trims the 3 kHz-deviation sidebands), in-band 40.8 dB, RMS -15.9 dBFS, 1 kHz tone -15.9 dBFS; 12.5 kHz probe -92.8 dBc | SNR ≥ 22, in-band ≥ 36, captured and audible ≥ 1400 ms, first audible ≤ 100 ms, RMS -22 to -10 dBFS, clip 0, the 1 kHz tone at -17.5 to -14.5 dBFS, and 12.5 kHz ≤ -86 dBc (the default width reads -78.4) |
| `DECODE_IQ_ANALOG_NFM_TONE_16K` | `nfm_tone_synth`, `-v 0 -n 50 --nfm-bandwidth-hz 16000` | measures the same as the default: tone SNR 36.8 dB, in-band 42.7 dB, 1 kHz tone -15.9 dBFS | `NFM_TONE_8K`'s level bounds with SNR ≥ 36, which neither other width meets, and in-band ≥ 40 |
| `DECODE_IQ_ANALOG_NFM_TONE_25K` | `nfm_tone_synth`, `-v 0 -n 50 --nfm-bandwidth-hz=25000` | tone SNR 35.0 dB (more receiver noise in the wider channel), in-band 42.6 dB, RMS -15.9 dBFS, 1 kHz tone -15.9 dBFS; 12.5 kHz probe -60.6 dBc | `NFM_TONE_8K`'s level bounds with SNR ≥ 30, in-band ≥ 36, and 12.5 kHz ≥ -70 dBc (the default width reads -78.4) |
| `DECODE_IQ_ANALOG_NFM_BW_8K` | `nfm_adjacent_synth`, `-v 0 -n 50 --nfm-bandwidth-hz 8000` | tone SNR 28.0 dB; 12.5 kHz probe -92.6 dBc | SNR ≥ 22, captured ≥ 1400 ms, 12.5 kHz ≤ -86 dBc (the default width reads -76.6) |
| `DECODE_IQ_ANALOG_NFM_BW_25K` | `nfm_adjacent_synth`, `-v 0 -n 50 --nfm-bandwidth-hz 25000` | the neighbour is in the passband: 12.5 kHz probe -19.9 dBc, tone SNR 8.2 dB (the beat counts as noise), tone level -15.9 dBFS | captured ≥ 1400 ms, 12.5 kHz ≥ -30 dBc, 1 kHz level -18 to -14 dBFS |
| `DECODE_IQ_SCAN_NFM_TONE` | `nfm_adjacent_synth` through an `nfm` map row with `--nfm-bandwidth-hz 12500` on a `-fa -v 0 -n 50` session (`--analog-scan-row 0`) | tone SNR 37.4 dB; 12.5 kHz probe -97.3 dBc (the same row without a width: 36.6 dB, -76.6 dBc) | row banner with `width 12.5 kHz (row)`, SNR ≥ 30, captured ≥ 1400 ms, 12.5 kHz ≤ -86 dBc |
| `DECODE_IQ_SCAN_AM_TONE` | `am_tone_synth` through an `am` map row with `--am-bandwidth-hz 8333` on a `-fa` session (`--analog-scan-row 0`) | tone SNR 31.6 dB, in-band 43.0 dB, RMS -13.5 dBFS, audible 1500 of 1500 ms from 0 ms, no clipping (the wider channel lets in more noise than `-fM`'s 6 kHz default, 31.7 dB and 50.4 dB); the FM monitor finds no tone there, so the tone is the AM detector's | row banner with `width 8.333 kHz (row)`, SNR ≥ 25, captured and audible ≥ 1400 ms, in-band ≥ 36, `NFM_TONE`'s level window, clip 0 |
| `DECODE_IQ_SCAN_AM_WIDTH` | `am_adjacent_synth` through an `am` map row with `--am-bandwidth-hz 20000` (`--analog-scan-row 1`), `-v 0 -n 50` | 8333 Hz probe +0.3 dBc (row 0's 8.333 kHz: -93.2 dBc; `-fM`'s 6 kHz default: -103.5 dBc) | row banner with `width 20 kHz (row)`, captured ≥ 1400 ms, 8333 Hz ≥ -10 dBc, which fails if the row's width does not apply |
| `DECODE_IQ_SCAN_AM_SQUELCH_STEADY_CARRIER` | `am_airband_real` through an `am` map row with `--squelch-db -20` (the carrier reads about -13 dB) | captured 7980 of 8000 ms: the carrier, measured whole, holds the gate open throughout (the pooled-mean measurement, which reads an 0 Hz carrier as A²(1 - sin 2φ)/4, let 5940 ms through, chopped; at -18 dB 4580 ms, at -16 dB 2720 ms) | captured ≥ 7800 ms |
| `DECODE_IQ_ANALOG_SILENT_STREAM_TIME` | `nfm_tone_synth` under `-fi` (monitor off) | total 1500 ms, no audio at all | audible 0 ms, total 1400 to 1600 ms |
| `DECODE_IQ_ANALOG_NFM_REAL_CTCSS_SMOKE` | `nfm_ctcss_real`, `-n 50`, audible from -40 dBFS | captured 6000 ms, audible 4200 ms, in-band 18.4 dB, RMS -32.8 dBFS | captured ≥ 5800, audible ≥ 3500, in-band ≥ 14, RMS ≤ -25 dBFS |
| `DECODE_IQ_ANALOG_NFM_REAL_SQUELCH_A_SMOKE` | `nfm_squelch_real_a`, `-n 50`, audible from -40 dBFS | captured 4000 ms, audible 1700 ms from 460 ms, in-band 23.7 dB | captured ≥ 3900, audible ≥ 1200, first audible 250 to 1000 ms, in-band ≥ 18 |
| `DECODE_IQ_ANALOG_NFM_REAL_SQUELCH_B_SMOKE` | `nfm_squelch_real_b`, `-n 50`, audible from -40 dBFS | captured 4000 ms, audible 3100 ms, in-band 25.5 dB | captured ≥ 3900, audible ≥ 2500, in-band ≥ 20 |
| `DECODE_IQ_ANALOG_PARITY_NFM_REAL` | `nfm_ctcss_real` under `-fA` (default chain) | RMS -23.9 dBFS, no clipping: the level `dmr_voice` decodes to (-24.9 dBFS RMS) within 1 dB | captured ≥ 5800, RMS -27.9 to -21.9 dBFS (DMR voice ±3 dB), clip 0 |
| `DECODE_IQ_ANALOG_PARITY_AM_REAL` | `am_airband_real` under `-fM` (default chain) | RMS -23.9 dBFS, no clipping | captured ≥ 7800, `PARITY_NFM_REAL`'s window, clip 0 |
| `DECODE_IQ_ANALOG_SOURCE_MONITOR_FIXED` | `p25p1_c4fm_vc` under `-fs -8 -n 50` (the source monitor during digital decoding) | captured 3000 ms, peak -9.9 dBFS, no clipping (main: 141440 clipped samples, -0.03 dBFS RMS) | captured ≥ 2900, peak ≤ -6 dBFS, clip 0 |
| `DECODE_IQ_ANALOG_NO_MOD_AUTO_SWITCH` | `am_airband_real` under `-fA` (monitor path only, not AM reception) | total and captured 8000 ms, no CQPSK symbols (before the fix: total 751 ms, captured 0 to 20 ms, CQPSK warning) | captured ≥ 7800 ms, total 7800 to 8200 ms |
| `DECODE_IQ_ANALOG_REAL_CTCSS_1514` | `nfm_ctcss_real` | tone 151.4, first lock at 300 ms, locked 88.67% | tone lock ≤ 400 ms; tone 151.4 and no other tone logged |
| `DECODE_IQ_ANALOG_REAL_CTCSS_NOFALSE_SQUELCH_A`, `_B`, `_AM` | `nfm_squelch_real_a`, `nfm_squelch_real_b`, `am_airband_real` | `none` verdict, no tone, locked 0.00% | `none` logged, `tone=NA`, 0.00% locked, no tone logged |
| `DECODE_IQ_ANALOG_AM_TONE` | `am_tone_synth` under `-fM` | tone SNR 31.7 dB, in-band 50.4 dB, RMS -13.5 dBFS at the AGC's -10 dBFS peak, audible 1500 of 1500 ms from 0 ms, no clipping; a 1 kHz probe reads 0.00 dBc | SNR ≥ 25, captured and audible ≥ 1400 ms, first audible ≤ 100 ms, in-band ≥ 40, `NFM_TONE`'s level window, clip 0, 1 kHz probe within ±1 dBc |
| `DECODE_IQ_ANALOG_AM_TONE_FIXED50` | `am_tone_synth` under `-fM -n 50` | RMS -15.0 dBFS, peak -11.5 dBFS | RMS -16 to -14 dBFS, captured ≥ 1400 ms, clip 0 |
| `DECODE_IQ_ANALOG_AM_ADJ_6K` | `am_adjacent_synth` under `-fM -v 0 -n 50` (6 kHz default) | tone SNR 31.9 dB; 8333 Hz probe -103.5 dBc | SNR ≥ 25, captured ≥ 1400 ms, 8333 Hz ≤ -40 dBc |
| `DECODE_IQ_ANALOG_AM_ADJ_20K` | `am_adjacent_synth` under `-fM -v 0 -n 50 --am-bandwidth-hz 20000` | 8333 Hz probe +0.3 dBc (tone SNR -0.4 dB: the beat dominates) | captured ≥ 1400 ms, 8333 Hz ≥ -10 dBc |
| `DECODE_IQ_ANALOG_AM_REAL` | `am_airband_real` under `-fM -n 50`, audible from -40 dBFS | captured 8000 ms, audible 5720 ms, in-band 34.4 dB, RMS -18.6 dBFS, no clipping | captured ≥ 7800, audible ≥ 4500, in-band ≥ 28, RMS -24 to -13 dBFS, clip 0; no `Received tone:` logged |
| `DECODE_IQ_ANALOG_POLICY_ALLOW_MATCH` | `nfm_ctcss_synth_1000`, `--tone-allow 100.0` | first audible 280 ms (the tone locks at 300 ms, within the read that crosses it), audible 1720 of 2000 ms; `Tone filter: allowed (CTCSS 100.0 Hz)` | first audible 100 to 400 ms, audible ≥ 1600, total ≥ 1900; no `rejected` or `pending` line |
| `DECODE_IQ_ANALOG_POLICY_ALLOW_NOTONE` | `nfm_notone_synth`, `--tone-allow 100.0` | audible 0 of 2000 ms; `Tone filter: rejected (no tone)` | audible 0, total 1900 to 2100 ms; no `allowed` line |
| `DECODE_IQ_ANALOG_POLICY_BLOCK_NOTONE` | `nfm_notone_synth`, `--tone-block 100.0` | first audible 800 ms (the window's end), audible 1200 ms; `Tone filter: allowed (no tone)` | first audible 780 to 860 ms, audible ≥ 1100, total ≥ 1900; no `rejected` line |
| `DECODE_IQ_ANALOG_POLICY_BLOCK_DCS` | `nfm_dcs_synth_023n`, `--tone-block D023N` | audible 0 of 2000 ms; `Tone filter: rejected (DCS D023N / D047I)` | audible 0, total 1900 to 2100 ms; no `allowed` line |
| `DECODE_IQ_ANALOG_POLICY_ALLOW_DCS_ALIAS` | `nfm_dcs_synth_023i`, `--tone-allow D023I` | first audible 340 ms, audible 1660 ms; `Tone filter: allowed (DCS D047N / D023I)`: the listed spelling matches the signal the detector names D047N | first audible ≤ 600 ms, audible ≥ 1300, total ≥ 1900; no `rejected` line |
| `DECODE_IQ_ANALOG_REAL_CTCSS_POLICY` | `nfm_ctcss_real`, `--tone-allow 151.4` | first audible 280 ms, audible 5320 of 6000 ms: muted again over the reverse burst and tone gap at 3.7-4.0 s (`Tone filter: pending (tone lost)`) until the tone locks again | first audible 100 to 400 ms, audible ≥ 4500, total ≥ 5800; no `rejected` line |

Every `-fA` case in the table also fails if the host warns that the front end delivered CQPSK symbols instead of
monitor audio (see the `am_airband_real` note above).

The tone filter cases (`DECODE_IQ_ANALOG_POLICY_*` and `DECODE_IQ_ANALOG_REAL_CTCSS_POLICY`, issue #527) read the
filter's mute through the first-audible and audible-time metrics, and every never-audible case pairs its 0 ms bound with
stream time, so a replay that delivered nothing could not pass. Two unit tests sit under them. `DSP_ANALOG_TONE_POLICY`
drives the verdict state machine (`src/dsp/analog_tone_policy.c`) with no audio and no clock: each read is a
publication built as the tap would publish it (carrier, detector state, locked tone or code, the DCS candidate) plus the
samples and rate it covered, so the 800 ms window, its DCS extension and every transition are pinned in sample time,
the window's end to the read that crosses it at several rates and read sizes. `DSP_SYMBOL_REPLAY` covers the sink the
verdict gates: both live outputs muted together while a check runs or after a rejection, the `-6` raw WAV ungated, the
output with the filter off byte-identical to a session without one, the AM and `-8` monitors judging nothing, the
`Tone filter:` log lines, a changed policy's fresh check included, and the -Y hangtime stamp, which only traffic the
policy passes leaves (no-tone bursts shorter than the window stamp nothing). What the scanners do with a check is
engine-side. `ENGINE_NO_CARRIER_RESET` cannot run the monitor block (the DSP test seam is not linked into the engine
test), so it feeds such bursts through the real tap and sets the `-Y` hangtime anchor itself, to where the monitor's
stamp leaves it, and checks the step rule against it on the decode clock's TEST source, held at each whole second it
checks (see [Decode time in unit tests](#decode-time-in-unit-tests)): with a burst in every second of `-t`, the pass at
`-t` after the row's landing holds and the pass a second later steps, and allowed traffic holds the row through `-t`
after its last block and steps a second later. A check, or traffic whose verdict turned ALLOWED part-way through a
monitor block (at 22050 Hz, before the block's end stamps it), holds the row with `-t` run out. `ENGINE_TRUNK_SCAN`
shows the bursts, on its injected clock, restart neither the activity hold nor the idle dwell, and a check that spans
the end of an activity or operator hold arm the dwell as a quiet tick would.

The cases above can only show that bounds hold. The `DECODE_IQ_ANALOG_NEG_*` negative controls show that a missed
bound fails: they run the host through `tests/analog_replay_fail_check.cmake`, which requires its exit status (1 for
a missed bound), the named `ANALOG AUDIO FAIL:` line, an `ANALOG METRIC:` line (so the replay was scored) and no
`ANALOG AUDIO OK`, and can name a bound that must keep holding.

| Case | Run | Must fail on |
| --- | --- | --- |
| `DECODE_IQ_ANALOG_NEG_ADJACENT_UNFILTERED` | `DECODE_IQ_ANALOG_NFM_ADJACENT`'s 12.5 kHz bound with `DSD_NEO_CHANNEL_LPF=0`, options spelled `--opt=VALUE` | the probe's measured level: about -20 dBc against -40 (-76.6 with the channel filter); "not measured" does not count |
| `DECODE_IQ_ANALOG_NEG_AUDIBLE_NOT_SILENT` | the silence pattern (`--analog-max-audible-ms 0`, `--analog-min-total-ms 1400`) on `nfm_tone_synth` | audible ms, while the stream-time bound holds |
| `DECODE_IQ_ANALOG_NEG_TONE_SNR` | a 60 dB tone SNR floor on `nfm_tone_synth` (37.5 dB) | tone SNR |
| `DECODE_IQ_ANALOG_NEG_TONE_NOT_MEASURED` | an SNR bound without `--analog-expect-tone-hz` | tone SNR "not measured" |
| `DECODE_IQ_ANALOG_NEG_BAD_BOUND` | a malformed bound value | exit status 2 before the replay starts |
| `DECODE_IQ_ANALOG_NEG_MISSING_VALUE` | a host option as the last argument, with no value | exit status 2 and "needs a value" before the replay starts |
| `DECODE_IQ_ANALOG_NEG_TONE_LOCK_MS` | a 50 ms tone-lock bound on `nfm_ctcss_synth_1000` (300 ms) | tone lock ms, on the measured value |
| `DECODE_IQ_ANALOG_NEG_TONE_LOCK_NOT_MEASURED` | a 400 ms tone-lock bound on `nfm_notone_synth`, where no tone locks | tone lock ms "not measured" |
| `DECODE_IQ_ANALOG_NEG_AM_TONE_THROUGH_FM` | `DECODE_IQ_ANALOG_AM_TONE`'s 25 dB tone SNR floor on `am_tone_synth` under `-fA` (the FM monitor) | tone SNR, measured at -21.4 dB: the AM case's tone is the AM detector's |
| `DECODE_IQ_ANALOG_NEG_PARITY_FIXED` | `DECODE_IQ_ANALOG_PARITY_NFM_REAL`'s window on `nfm_ctcss_real` at `-n 50` | RMS, measured at -32.8 dBFS: the parity holds only with the AGC |

When a later change adds a bound or a new kind of check to the host, add a negative control beside it.

A few things about these numbers. Since issue #518 the default monitor chain is the voice band-pass (300-3400 Hz,
`-v 0x9`; its FM high-pass takes every CTCSS tone 40 dB down and a DCS signal about 32 dB) and the analog AGC (`-n 0`), which takes a
steady tone's peaks to -10 dBFS and real speech to about the level digital voice decodes to: `dmr_voice` plays at
-24.9 dBFS RMS, `nfm_ctcss_real` and `am_airband_real` at -23.9 dBFS each, where the old chain played them at -44.6
and -53.7 dBFS. Its gain depends on the signal, so a case that measures the excerpt rather than the AGC pins the fixed
reference gain, `-n 50`, which takes 1 kHz at 3 kHz deviation, or AM at 50%, to -12 dBFS peak (-15 dBFS RMS) live and
in replay alike; the smoke cases count audible blocks from -40 dBFS there, where the speech stands clear of the quiet
carrier between words. At the fixed gain FM quieting shows as level: receiver noise (`noise_floor` under `-fA`)
measures -12.8 dBFS RMS against -32.8 dBFS for `nfm_ctcss_real`, whose speech deviation is small (0.2-0.6 kHz RMS)
against a channel CNR of roughly 20 dB; under the AGC both sit near the target (-21.1 and -23.9 dBFS). The band-pass's
3.4 kHz low-pass takes 12.5 kHz 54 dB down, so the cases that measure the channel filter (its width, a neighbour) run
with the voice filters off and the fixed gain (`-v 0 -n 50`). Use `-v 0` when the question is what the demodulator did:
it measures a tone SNR of 36.8 dB and an in-band ratio of 42.7 dB on `nfm_tone_synth` at the default width.

The 12.5 kHz probe on `nfm_adjacent_synth` reads -76.6 dBc (`-v 0 -n 50`), against -78.4 dBc on `nfm_tone_synth`,
which has no interferer: at the default channel width it measures the floor, so its bound catches a channel filter that lets the
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
| `am_tone_synth` | synthetic, seed 5241 | 1.5 s: 1 kHz at 50% AM depth on a 0 Hz carrier, complex noise 30 dB under the carrier across 48 kHz (#524) |
| `am_adjacent_synth` | synthetic, seed 5242 | the same plus an unmodulated carrier 8.333 kHz up at -6 dB (the next 8.33 kHz airband channel) |
| `nfm_ctcss_synth_1000` | synthetic, seed 5221000 | 2 s: voice-band audio at up to 4 kHz deviation plus CTCSS 100.0 Hz at 600 Hz deviation, receiver noise at baseband (#522) |
| `nfm_ctcss_synth_670` | synthetic, seed 5220670 | the same with CTCSS 67.0 Hz |
| `nfm_ctcss_synth_drop` | synthetic, seed 5221001 | 2.5 s: CTCSS 100.0 Hz that stops at 1.2 s while the carrier and voice carry on |
| `nfm_notone_synth` | synthetic, seed 5220000 | 2 s: the same voice and noise with no tone |
| `nfm_dcs_synth_023n` | synthetic, seed 5230023 | 2 s: the same voice and noise plus DCS D023N at 600 Hz deviation, NRZ low-passed below 300 Hz (#523) |
| `nfm_dcs_synth_023i` | synthetic, seed 5231023 | the same with D023 in inverted polarity, the signal of D047N |
| `nfm_dcs_synth_noisy` | synthetic, seed 5232023 | the same with D023N's word two bits wrong in every repetition: no code |
| `nfm_dcs_synth_drop` | synthetic, seed 5233023 | 2 s: D023N that stops at 1.2 s, with no turn-off tone, while the carrier and voice carry on |
| `nfm_burst_synth` | synthetic, seed 5311 | 0.75 s: complex noise at 3 LSB a component, the 1 kHz test tone at 3 kHz deviation keyed from 0.30 to 0.55 s (2 ms ramps) 16 dB over the noise across 48 kHz, unnormalised so a +10 dB copy stays within the cu8 rails (auto squelch) |
| `am_burst_synth` | synthetic, seed 5312 | the same with 1 kHz at 50% AM depth on a carrier 12 dB over the noise |

`tools/build_iq_fixtures.py` pins each zip's SHA-256, reads the WAV members sample-exact (u8 centred on 127.5),
shifts each wanted carrier to 0 Hz by an offset measured over the excerpt (`ANALOG_EXCERPTS` documents each), and
resamples to 48 kHz in the frequency domain. The synthetics regenerate offline and byte for byte with
`python3 tools/build_iq_fixtures.py --derived-only`; the excerpts need the network:
`python3 tools/build_iq_fixtures.py --only nfm_ctcss_real` (and so on).

The whole analog effort (issue #518) stays within 5 MB of new fixture bytes. The six #518 fixtures take 2.4 MB, the
`nxdn48_attenuated` replay that #521 committed 0.58 MB and the four received-tone synthetics of #522 0.82 MB
(`nfm_ctcss_synth_drop` runs 2.5 s: after its tone stops at 1.2 s it still has to lose the tone and reach the no-tone
verdict), 3.8 MB in all, which leaves about 1.2 MB. A 48 kHz cu8 fixture takes 96 kB a second, so keep each later
synthetic fixture to 2 s (192 kB) or less. The two AM synthetics of #524 (1.5 s each) take 0.29 MB and the four DCS
synthetics of #523 0.77 MB (the no-code case reuses `nfm_notone_synth`), 4.86 MB in all, which leaves 0.14 MB. The
auto squelch's two burst synthetics (0.75 s each) take 0.14 MB (4,992,000 bytes in all), which spends the budget: its
hotter-receiver twins add nothing, since the analog replay host scales a copy at run time (`--analog-iq-gain-db`), and
its noise and real-carrier cases reuse `noise_floor` and the excerpts. The noise squelch's replay cases add none either:
they reuse the same bursts, copies and excerpts. A pull request that adds analog fixtures states the running total.

#### Auto squelch classifier (design gate)

The floor-relative squelch (`--squelch auto[+N]`, issue #518) learns a channel's noise floor from windows it classes as
noise, so it needs a classifier that tells "noise only" from "carrier present" on the channel-filtered complex baseband
the demodulator sees, at every channel plan the analog monitor runs. `tools/squelch_model.py` (numpy/scipy, offline;
CI does not run it) decided its design before any C was written. It builds the plans from the repository itself:
`tools/squelch_model_taps.cpp`, compiled against a configured build tree's static libraries, prints the taps
`full_demod()` designs and the half-band taps, and a numpy port of `dsd_firdes_low_pass()` cross-checks every designed
plan. 420 combinations of NFM 8-25 kHz, unset NFM (with `DSD_NEO_CHANNEL_LPF` unset, `=1` and `=0`) and AM 5-20 kHz
over 30 rate chains (the RTL DSP bandwidths, the rates Airspy, Airspy Mini and SDDC devices force, and I/Q replay with
and without post-decimation) leave 253 that run and 196 distinct filters. Signals: channel-filtered complex Gaussian
noise, FM (dead carrier, tones, real speech from the NFM excerpts) at half and full rated deviation, AM (dead carrier,
tones, real airband speech) at 30, 60 and 100% depth, each at 0, +/-1 and +/-2.5 kHz offset, at 0 to 20 dB CNR. The
targets per window are: noise read as carrier at most 1e-4, a 6 dB carrier missed at most 1%, a carrier of 6 dB or more
read as noise (it would raise the floor) at most 1e-4, noise left undecided at most 25%.

The planned classifier (20 ms windows; envelope CV^2 and phase coherence C at a lag from the taps' autocorrelation;
fixed thresholds) met every target on 162 of the 196 filters: narrow plans (under about 400 effective samples per
window) missed 1-5% of 6 dB carriers, among them the fading real capture and AM with full-depth tones, and the
unfiltered NFM default at the 4 kHz RTL rate read noise as carrier 3.4-3.9 times in 10,000. The scheme the auto
squelch uses passes 195: 40 ms windows; coherence measured against its expected value on noise, beta = (pi/4)
rho_n(L) 2F1(1/2, 1/2; 2; rho_n(L)^2), with rho_n the noise autocorrelation through the channel taps and the last
half-band stage; and both features normalised by the window's effective sample count N_eff = N / sum rho_n(m)^2,
X = (CV^2 - 1) sqrt(N_eff) and Y = |mean(u_k conj(u_{k-L})) - beta| sqrt(N_eff). A window is a carrier when X <= -7.0
or Y >= 3.3, and noise when X >= -3.8 and Y <= 2.1. Worst cases over the 195: noise read as carrier 1.0e-4 per window
(about 10 events, so about +/-32%), a 6 dB carrier missed 1.1%, no carrier of 6 dB or more read as noise (0.4% at
3 dB), noise left undecided at most 3.7%. The one plan that misses its target, unset NFM with `DSD_NEO_CHANNEL_LPF=1`
forcing the legacy filter on at the 8 kHz RTL rate, misses 1.1% of 6 dB carriers; it is accepted as marginal. The
model does not cover spurs or adjacent channels: a steady tone inside the channel is a carrier to the coherence test,
and with these thresholds a birdie from -12 dB (widest plans) to -3.6 dB (narrowest) against the noise reads as one.
`python3 tools/squelch_model.py` reproduces the report (`build/squelch_model/report.md`) in about 25 minutes on 8
workers; `--quick` takes about a minute.

#### Auto squelch replay cases

The `DECODE_IQ_ANALOG_SQL_AUTO_*` cases replay `nfm_burst_synth` and `am_burst_synth` (noise, a carrier from 0.30 to
0.55 s, noise) under `--squelch auto` and hold each to one set of bounds: first audible block at 300-360 ms, 240-320
ms audible at -40 dBFS, stream time 700-800 ms, no clipping. Both measure 320 ms and 280 ms through FM and AM: the
floor is learned from the opening windows, the gate opens 20 ms into the carrier and closes within a window of its end.

| Case | What it pins |
| --- | --- |
| `_NFM_BURST`, `_AM_BURST` | The gate's edges on a burst after learned noise. |
| `_NFM_BURST_HOT`, `_AM_BURST_HOT` | The same capture 10 dB hotter (`--analog-iq-gain-db 10`, 0 bytes clipped), the same bounds and measurements: the setting follows the floor, not the receiver. |
| `_NFM_MARGIN`, `_AM_MARGIN_HOT` | Each carrier stands about 20 dB over its channel's noise (it still opens at `auto+20` and not at `auto+21`, `+22` on AM); under `auto+30` nothing is audible at either level. |
| `_NOISE_FLOOR`, `_AM_NOISE_FLOOR` | Ten seconds of receiver noise (`noise_floor`) never open it. Its 16 LSB noise cannot be scaled 10 dB without clipping, so it has no hot twin. |
| `_NFM_REAL_CARRIER`, `_AM_REAL_CARRIER` | A carrier is never learned as floor: `nfm_ctcss_real` and `am_airband_real` hold their carrier throughout and play whole after the first 40 ms window (5940 of 6000 ms, 7940 of 8000), within 60 ms of their audible time with the squelch off. |
| `DECODE_IQ_SCAN_SQL_AUTO_ROW` | A `-Y` row's own `--squelch auto` on a digital (`-fa`) session gates the burst as `-fA` does. |
| `DECODE_IQ_ANALOG_NEG_SQL_LEVEL_HOT` | The burst bounds under a fixed `-35 dB` on the hot copy fail: that level sits 2.5 dB over the burst's noise at its own level and 7.5 dB under the hot copy's, which opens on noise from the first block (740 ms audible). |
| `DECODE_IQ_ANALOG_NEG_IQ_GAIN_CLIPS` | A gain that would clip the copy stops the host (exit 2). |
| `DECODE_IQ_ANALOG_SQL_AUTO_{NFM,AM}_DETERMINISM` | Fast, jittered and realtime replays hand the sink the same gated audio (the replay sink's hash, 14 writes of 960). GNU `--wrap` builds only, like the other sink cases; the monitor reads one sample at a time, so there is no short-read leg, and 280 ms of audio cannot back up a stalled sink. |

What each case can catch was checked by mutation. A tracker that never learns a floor passes the burst cases (while
learning, the gate opens on carrier windows only, which is what lets a scan land mid-transmission) and fails both
margin cases. A sink that ignores the per-sample flags fails every FM case and both determinism cases; the AM cases
still pass, because the AM detector writes silence for a closed sample itself (`am_demod_flagged()`: its output,
the envelope over the held carrier, means nothing without a carrier), so on AM they pin the detector's handling of
the flags instead. Level mode cannot be held to these bounds on replay at all: it gates each demod block whole on the
power of its first samples, as it did before the auto squelch, and a replay's demod block is one 64 KiB capture chunk,
683 ms at 48 kHz. Both chunks of the 0.75 s burst start on noise, so a fixed threshold that keeps that noise out never
plays the carrier; on a 2 s version (carrier from 0.5 to 1.5 s) a fixed `-20 dB` plays from 683 ms to the end. The
auto squelch's per-sample gate opens 20 ms into the carrier and closes within a window of its end.

#### Noise squelch design gate

The NFM noise squelch (`--squelch noise[+N]`, issue #518) opens when the FM discriminator's output above the voice band
quiets by N dB. The question before any C was written: does a quieting statistic read about 0 dB on noise at any level,
follow the carrier-to-noise ratio, and stay well above the highest threshold while a strong carrier carries wanted
modulation, at every plan the FM monitor runs? `tools/noise_squelch_model.py` (numpy/scipy, offline; CI does not run it)
answers it on `tools/squelch_model.py`'s tap harness and rate chains, with `dsd_fm_demod()`'s discriminator modelled
exactly. It covers 365 distinct plans:

- 119 plans from the 171 NFM combinations the harness runs: every width, the unset default and every rate chain.
- 246 plans at custom widths: 11.0-12.4 kHz in 100 Hz steps and 13-25 kHz in 1 kHz steps, on every chain at 24 kHz or
  above.

For each plan the model runs:

- 600 s of noise.
- A tone sweep at the width's rated deviation, at every offset the tone's own Carson bandwidth leaves in the channel.
  Tones run from 300 Hz in 5 Hz steps up to 800 Hz, where a tone's harmonics crowd the band, then in 25 Hz steps to
  3000 Hz.
- Real NFM speech: the three excerpts' audio at rated deviation.
- CNR sweeps, and the real excerpts as recorded.
- Stress cases outside the gate: a carrier 1 kHz off centre past its Carson bandwidth, and 1.5 times rated deviation.

The gate: wanted modulation keeps its 1st-percentile Q at least 6 dB above N = 30, and noise reaches N = 3 in at most
1e-4 of the 20 ms evaluations.

A single band above voice fails the gate. The channel filter truncates a strong tone's FM sidebands, so its 2nd and 3rd
harmonics land in the band as lines, and they cap Q at 12.5-29 dB:

- `single`, worst 12.5 dB.
- The first plan's [W/2 - 1800, W/2 - 600] band, `high1200`: 15.9 dB.

Cutting the band ([3.8 kHz, the taps' -1 dB point less 800 Hz], under 0.45 fs) into sub-bands and taking the
best-quieted one finds a sub-band clear of the lines. But the best of many noisy readings sits above 0 dB on noise: the
`sW-max` candidates let noise reach N = 3 in 1-29% of evaluations. So every guarded candidate takes
Q = max(Q_sum, Q_max - G dB), Q_sum from the sub-bands' summed powers (the whole band, as tight on noise as one
band-pass). How the band is cut decides the wanted side:

| Candidate | Plans passing (of 293 with a band) | Worst wanted p1 | Why |
| --- | --- | --- | --- |
| 500 Hz sub-bands, at most 9 (`s500-guard4`) | 31 | 25.8 dB | too few on narrow channels: at 11.2 kHz two sub-bands, and a harmonic on their boundary leaves neither clear |
| 300 Hz, at most 15 (`s300-guard4`) | 203 | 32.9 dB | a tone whose harmonics fall near the boundaries leaves no sub-band clear: 90 plans fail, at 11.7-13 kHz and 23 kHz |
| 250 Hz, at most 18 (`s250-guard5`) | 286 | 35.7 dB | 580-585 Hz tones on 11.7 and 11.8 kHz channels; noise reaches N = 3 twice as often |
| 300 Hz and a set staggered half a sub-band between them (`s300x-guard4`) | 293 | 36.6 dB | the design |

The staggered set puts a line that sits on a boundary of one set inside a band-pass of the other. Q_max takes the best
of all 2K - 1 band-passes; Q_sum stays on the K sub-bands. Its 4 dB guard leaves the most wanted margin: 5 and 6 dB
lower noise's loudest window but fail 5 and 11 plans. With `s300x-guard4`:

- All 293 plans with at least 1200 Hz of band pass. Wanted modulation keeps 36.6 dB at worst (a 590 Hz tone at its
  Carson edge on 11.8 kHz behind the half-band), and speech keeps 42.5 dB.
- Noise reaches N = 3 in at most one 20 ms evaluation in 29,970 (3.3e-5) and never N = 4. Its loudest window over
  600 s is 3.65 dB.
- The 72 plans with no band run the auto squelch:
  - every 8 and 10 kHz NFM width;
  - every chain at a DSP rate under 12 kHz;
  - the legacy WIDE filters (`DSD_NEO_CHANNEL_LPF=1`) at 11.7 and 12 kHz;
  - the custom widths 11.0 and 11.1 kHz;
  - 11.2 kHz on the chains whose taps reach their -1 dB point at 5790 Hz, 10 Hz short of the 5.8 kHz that 1200 Hz of
    band needs.

| Figure (293 plans) | Result |
| --- | --- |
| Calibration, 2 s through the half-band, channel taps and discriminator | within 0.59 dB of the 600 s reference in every band-pass (0.97 dB skipping the half-band) |
| Noise holding an open gate (Q at or over the close threshold) | 1.8e-2 of evaluations at N = 3 (close under 1.5 dB), 3.3e-5 at N = 6, none at N = 10 |
| A receiver's slope (one pole at the channel edge) | noise's median Q -3.79..+1.07 dB |
| Median Q at 0, 10, 20 and 30 dB CNR (1 kHz tone) | 1.7-3.9, 11.7-24.8, 21.8-35.2 and 31.6-45.1 dB: wider channels read more quieting at the same in-channel CNR |
| The real excerpts as recorded (receiver noise and neighbours included) | the two squelch captures keep 21 dB at the 1st percentile through every plan of 12.5 kHz or wider. `nfm_ctcss_real` reads 19.7-22.0 dB (median) through 12.5-23 kHz plans, 5.7-20.7 dB through 24 kHz ones and 1.4-8.1 dB through 25 kHz and wider: its neighbour 12.5 kHz away falls inside those channels, as it falls in their audio |
| Stress cases (outside the gate) | a carrier 1 kHz past its Carson edge, or at 1.5 times rated deviation, can close the gate (1st percentile down to 1.5 dB): its own sidebands fall in the band as noise does |

`python3 tools/noise_squelch_model.py` reproduces the report (`build/noise_squelch_model/noise_squelch_report.md`, and
`noise_squelch_results.json` beside it) in about 55 minutes on 5 workers; `--quick` (30 s of noise per plan, shorter
tones) is a smoke run. The C core is held to the model's design by two suites:

- `DSP_NFM_NOISE_SQUELCH`: the band-passes' response against the closed-form Butterworth band-pass, the calibration
  against 60 s of noise, noise never opening at N = 3 on six plans, quieting independent of the input level, the
  hysteresis, and block-cut bit identity.
- `DSP_NFM_NOISE_SQUELCH_SWEEP` (the same binary with `--sweep`, 600 s timeout): full-deviation tones on the model's
  grid never closing the gate at N = 30. It runs the plans where the gate reads its lowest: 11.2 kHz at 48 kHz, 11.8
  kHz at 62500 Hz, 12.4 kHz at 39062 Hz and 13 kHz at 46875 Hz behind the half-band, and 25 kHz. Its lowest Q is
  37.0 dB, and it fails without the staggered set.

#### Noise squelch replay cases

The `DECODE_IQ_ANALOG_SQL_NOISE_*` cases replay the same burst under `--squelch noise`, held to bounds of their own:
first audible block at 330-380 ms, 200-270 ms audible. The burst measures 340 ms and 240 ms at both levels: the gate
opens on the first full 40 ms window of carrier and closes within a window of its end. Auto's 320 and 280 ms miss
those bounds, so a noise setting run as auto fails them.

| Case | What it pins |
| --- | --- |
| `_NFM_BURST`, `_NFM_BURST_HOT` | The gate's edges, and the same measurements 10 dB hotter: quieting does not follow the level. |
| `_NFM_MARGIN`, `_NFM_MARGIN_HOT` | The burst quiets the band by about 22 dB (it opens at `noise+20`, not at `+25`); under `noise+30` nothing is audible at either level. |
| `_NOISE_FLOOR` | Ten seconds of `noise_floor` never open it at the lowest N (`noise+3`). |
| `_NFM_REAL_CARRIER` | `nfm_ctcss_real` plays whole after the first window (4160 of the 4200 ms audible with the squelch off). |
| `_NARROW_AS_AUTO` | An 8 kHz channel has no band above voice: the auto squelch runs the setting, and the burst measures as under `--squelch auto`. |
| `_AM_REFUSED` | `-fM --squelch noise` stops at startup with its message, before any replay. |
| `DECODE_IQ_SCAN_SQL_NOISE_ROW` | A `-Y` nfm row's own `--squelch noise` gates the burst as `-fA` does. |
| `DECODE_IQ_ANALOG_SQL_NOISE_NFM_DETERMINISM` | Fast, jittered and realtime replays hand the sink the same gated audio (GNU `--wrap` builds). |

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
(`D023N = D047I`, and `D023I = D047N` for the inverted waveform). The DCS detector (#523) publishes the normal-polarity
spelling (`dsd_dcs_canonical()`) and shows and logs both, normal first (`DCS D023N / D047I`; the alias table is
in the [CLI guide](cli.md#received-code-dcs-on-the-analog-monitor)), so an oracle label and the detector's name
the same two spellings. The wiki's CTCSS page, which links the I/Q recording, carries audio samples at 151.4,
173.8 and 186.2 Hz without saying which tones the recording holds; the oracle finds the first two. A maintainer
has confirmed the CTCSS labels in the table, "none" for both squelch captures included, and the received-tone
cases pin them: `DECODE_IQ_ANALOG_REAL_CTCSS_1514` the 151.4 Hz of `nfm_ctcss_real`, and the
`DECODE_IQ_ANALOG_REAL_CTCSS_NOFALSE_*` cases the absence of any tone on both squelch captures (see
[Received tone (CTCSS) on the analog monitor](#received-tone-ctcss-on-the-analog-monitor), which also says why the
neighbour's 173.8 Hz has no case). No committed excerpt carries DCS, so no DCS label is pinned; the DCS detector
(#523) is held to the "none" labels instead, by the same `DECODE_IQ_ANALOG_REAL_CTCSS_NOFALSE_*` cases and by
`DECODE_IQ_ANALOG_REAL_CTCSS_1514`, all of which fail on any `Received tone: DCS` line. A real DCS accept fixture is a
maintainer follow-up: record a transmitter with a known code in both polarities (and its turn-off tone), add it under
`LOCAL_SOURCES`, and pin it here.

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

The QML suites, and the C++ cases that load a production screen (`UI_QT_CONTROLLER`,
`UI_QT_RADIO_REFERENCE`), run the QML collector to completion, as the app does: each calls
`dsd_qt::applyQmlGcPolicy()` (`src/ui/qt/qml_gc_policy.h`) before its first engine exists.
`UI_QT_QML_GC_POLICY` pins the rule. With Qt's incremental collector, Qt 6.11.2 crashed the suites in
`QQmlConnections::connectSignalsToMethods()` whenever the host was busy, for example under a parallel
`ctest` with other builds running. A test that builds its own `QQmlEngine` calls the policy first. To
watch the incremental collector again, set `QV4_GC_TIMELIMIT` to a slice length in milliseconds
(Qt's default is 5): the policy keeps any integer it finds.

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

### Decode time in unit tests

Decode decisions and decoded-output stamps read the decode clock (`<dsd-neo/runtime/decode_clock.h>`), never the
platform clock (`docs/code-quality-guardrails.md` has the rule). So a test that steps a decode window, an expiry or a
stamp drives the clock's TEST source, rather than sleeping through the window or wrapping `time()` at link time:

```c
static const uint64_t k_t0_ns = 1000000000000000000ULL; /* far from any platform clock reading */
dsd_decode_clock_use_test(k_t0_ns); /* before anything stamps, init included */
/* ... set up and run the code under test ... */
dsd_decode_clock_test_set_ns(k_t0_ns + 1250000000ULL); /* 1.25 s later */
/* ... check the window, reading now back through dsd_decode_now_*() ... */
dsd_decode_clock_use_system(); /* on every return path */
```

- Select TEST before anything stamps. Init seeds stamps from the clock (`initState()`, `p25_sm_init_ctx()`), and a
  stamp taken on the system clock read against a TEST value compares two origins.
- TEST holds one value for the monotonic and the wall reads alike (`dsd_decode_time()` is its whole seconds), so a rule
  that compares wall seconds, such as the `-Y` hangtime step in `ENGINE_NO_CARRIER_RESET`, is stepped a second at a
  time.
- Time moves only when the test moves it. A loop that waits for decode time to pass steps the clock itself, as the P25
  call-skip case in `P25_P2_FRAME_LOCKOUT_SM` does, 100 ms a voice burst. Unstepped, it never ends on TEST, and on
  SYSTEM it sits out the whole window in real time.
- A case that orders two stamps, such as a stop and the start that follows it, runs on TEST and steps between the two
  events. On SYSTEM, two events sent back to back can land in one reading of a coarse clock (a virtualised CI runner),
  so a strict comparison fails now and then: `P25_SM_UNIFIED_CORE` runs such cases through
  `run_on_test_decode_clock()` and steps with `decode_clock_step()` (issue #619).
- The source is process-wide, so put it back with `dsd_decode_clock_use_system()` on every return path, failures
  included, or the next case in the binary inherits it. Leaving TEST carries no value into SYSTEM. Leaving REPLAY does:
  SYSTEM's monotonic reads then go on from the replay's capture time for the rest of the process, so a case that runs
  a replay leave (`dsd_engine_decode_clock_leave_replay()`) leaves later cases reading SYSTEM there; they use TEST or
  run before it.
- Real-time reads (`dsd_realtime_*()`: sleeps, condvar deadlines, ring, device and socket timeouts) do not move with
  TEST. A case that needs both steps the decode clock and lets the real one run.
- `dsd_decode_clock_use_replay(anchor_s)` and `dsd_decode_clock_set_media_ns()` (monotone: a smaller value is ignored)
  stand in for a replay's capture time the same way, as `ENGINE_NO_CARRIER_RESET` and `APP_COMMAND_QUEUE` do to test a
  replay leave.
- A test that compiles protocol, core, app-control or terminal sources directly instead of linking `dsd-neo_runtime`
  gets the clock from `dsd-neo_test_decode_clock`: add its target to `_DSD_NEO_DECODE_CLOCK_TEST_TARGETS` in
  `tests/CMakeLists.txt`.

### Temporary files and directories

A test must remove every temporary file and directory it creates, on its failure
paths as well as when it passes. `dsd_test_mkstemp()`, `dsd_test_mkdtemp()` and
`dsd_test_capture_stderr_begin()` in `tests/test_support/test_support.h` create
them under `dsd_test_tmpdir()`: `DSD_NEO_TEST_TMPDIR`, else `TMPDIR` (`TEMP` or
`TMP` on Windows), else the working directory. Create them with these helpers,
not at a fixed path such as `/tmp`, which the check below does not look at. The
temp directory's own path can contain a dot, so when the code under test reads
meaning from a path's text, such as an extension, open the file by a bare name
from a `dsd_test_temp_cwd_enter()` directory instead. Remove each file with
`remove()`, including any the code under test wrote there, such as a P25
control-channel cache file or an I/Q capture's sidecar, then remove the
directory with `dsd_test_rmdir()`. `dsd_test_remove_temp_dir()` does both from a
list of file names. Never remove a directory with `remove()`: the native Windows
CRT does not remove directories, so it would stay behind there. Check the result
of the directory removal and fail the test if it fails. The removal succeeds
only on an empty directory, so the check also catches a file the test forgot to
list or a new file the code under test starts writing.
`dsd_test_capture_stderr_read()` deletes the capture file it reads. A test that
does not read its capture removes `cap.path` itself after
`dsd_test_capture_stderr_end()`.

A failed `assert()` aborts the test before its cleanup runs. Once a temporary
file or directory exists, check with a counted failure instead: print what
failed, remove the file or directory, then return the failure. A write that
fails while the test sets up its file counts the same way. An open that fails
can still have created the file, so remove it on that path too. Close every
handle on a file before removing it, including one the code under test hands
back from an open the test expects it to refuse: the native Windows CRT cannot
remove an open file.

Keep large objects off the stack. A Windows main thread gets a 1 MiB stack where
Linux gives 8 MiB, so a test that declares a `dsd_state`, an `Event_History_I`
(3.7 MB) or another multi-megabyte object as a local crashes only on Windows,
which CTest reports as a segfault. Make such a local `static` and clear it before
use. GCC and Clang build every test with `-Wframe-larger-than=786432`, so a frame
over 768 KiB fails the Linux build instead.

Production writes CSV, INI and group files in text mode, so on Windows their
lines end in CRLF. Compare such a file's contents after reading it in text mode
(`"r"`), and binary output, such as an MBE recording, byte for byte (`"rb"`).

To check the whole suite, run it against an empty directory, which must still
be empty afterwards. That does not catch a file a test creates by a relative
path, which lands in its working directory: `build/dev-debug/tests` for most
tests, the source tree for the `TOOLS_*` scripts. So compare both before and
after the run too:

```sh
T=$(mktemp -d)
find build/dev-debug/tests | sort > "$T.build"
git status --porcelain --ignored > "$T.src"
DSD_NEO_TEST_TMPDIR=$T TMPDIR=$T ctest --preset dev-debug --output-on-failure
ls -A "$T"   # prints nothing
find build/dev-debug/tests | sort | diff "$T.build" -   # prints nothing
git status --porcelain --ignored | diff "$T.src" -   # prints nothing
rmdir "$T"; rm "$T.build" "$T.src"
```

Repeat this in a `-DDSD_ENABLE_QT_UI=ON` build with `-R '^UI_QT'` and
`QT_QPA_PLATFORM=offscreen` when a change touches the Qt tests.

## Continuous Integration

GitHub Actions runs tests and quality checks on pull requests, primary-branch
pushes, tags, schedules, and manual dispatches. Coverage varies by event:
cross-platform builds, sanitizer tests, static analysis, workflow linting,
secret scanning, OSV scanning, repository guardrails for secret redaction and
workflow source/download pinning, fuzz smoke tests, and install/package
validation run where their workflows declare those events. Dependency review is
PR-only, release tag validation is tag-only, and extended fuzzing is scheduled
or manually dispatched. The backend matrix builds and tests all five
SDR-backend combinations — `both`, `rtl_only`, `soapy_only`, `airspy_only` and
`neither` — on pull requests as well as pushes, so the radio-off build is proven
before a merge rather than after one.

The full ctest suite also runs, as a required check on every pull request, on
each platform a release ships for:

| Check | Configuration | Covers |
|---|---|---|
| `backend-matrix (both, arm64)` | `dev-debug`, native arm64 runner | the aarch64 AppImage; unsigned `char`; the NEON SIMD paths |
| `android shape (arm64, headless, forced radio pipeline)` | the Android option set on arm64 | the Android ABI's instruction set (not Bionic) |
| `Linux • RelWithDebInfo • ctest (x86-64-v3, fast-math)` | `perf-bench` with `-march=x86-64-v3`, GCC | optimized, fast-math code generation like the shipped builds, with AVX2/FMA |
| `Linux • RelWithDebInfo • ctest (x86-64-v3, fast-math, clang)` | the same with Clang | the macOS release toolchain's fast-math code generation; `-Wnan-infinity-disabled` makes a NaN or infinity test left in fast-math code a build error (see "Fast-math and non-finite values" in `docs/code_map.md`) |
| `macOS • Debug • ctest (arm64)` | `dev-debug`, AppleClang | the DMG; libc++, ld64 and BSD libc |
| `Windows • Debug • ctest (MSVC x64)` | `win-msvc-debug` with Ninja | the ZIP; MSVC, the debug CRT and 32-bit `long` |

Tests that need GNU ld `--wrap` seams register only on Linux, and the `TOOLS_*`
script tests only where bash is (not Windows), so the macOS and Windows suites
are smaller than the Linux one; each job prints its registered test count before
running.

Some tests drive an external tool: two `TOOLS_*` tests need ripgrep,
`TOOLS_GCC_FANALYZER` needs GNU GCC's analyzer under the name `gcc`,
`TOOLS_IWYU` needs git and Python, and `ANDROID_LOCATION_JVM` needs Java and the
Kotlin compiler. Without its tool such a test skips, and passes, on a developer
machine. Every CI job that runs the suite sets `DSD_NEO_REQUIRE_TEST_TOOLS=1`,
which turns that skip into a failure, and installs what its runner image lacks:
ripgrep everywhere, Homebrew's GCC as `gcc` for the macOS test step, Java and
Kotlin in the Clang container, and the pinned Kotlin compiler
(`KOTLIN_COMPILER_VERSION` and `KOTLIN_COMPILER_SHA256` in
`tools/ci-dependency-pins.env`) on the arm64 legs.

A job skipped by an `if:` still reports its check, as a success. A required job
that only runs on pull requests therefore lives in a workflow that only pull
requests start (`windows-pr`, `macos-pr`, `guardrails-pr`), with no event
condition. In a workflow that a push or a dispatch can also start, a skipped run
on a pull request's head commit could stand in for the real one.

The Windows Debug job's vcpkg dependencies come from a binary cache that pull
requests only read. A change to the dependency set (`vcpkg.json`, the overlay
ports or the baseline) builds them from source on every pull-request run until
the push-side twin has run once: `Windows • Debug • ctest (MSVC x64, cache
seed)` in `windows-ctest-seed`. Dispatch `windows-ctest-seed` on the branch to
seed it.

To run the Windows suite locally, configure and build the `win-msvc-debug`
preset and run `ctest --preset win-msvc-debug`; the test preset puts the vcpkg
debug DLLs on `PATH`. A Debug-CRT assertion or `abort()` in a test is reported on
stderr rather than in a dialog (`tests/test_support/win_crt_report.c`).

## Decode-Quality A/B on Real Captures

The `iq-decode` suite answers whether a change still decodes; it does not answer
whether it decodes *as well*. Symbol-timing, slicer and demodulator changes need
the second question answered, and one replay cannot answer it. Replay is
repeatable (issue #572, see
[Replay determinism](#replay-determinism-issue-572)), so a capture replayed
again decodes exactly as before, but that is one draw: which frames a capture
yields depends on where the decoder's state stands when each transmission
arrives, the sync hunt's above all, and a change that moves the front end's
timing by a few samples can move that draw by more than the effect being looked
for. Measure on real captures, more than one where you can, and on several
realizations of each: copies with every event shifted by a fraction of a
symbol, which `tools/replay_ab.sh` makes, one per repeat.

`tools/replay_ab.sh` replays one I/Q capture through two or more builds and
`tools/replay_ab_report.py` reports the result:

```sh
cmake --build --preset dev-debug -j --target dsd-neo
cp build/dev-debug/apps/dsd-cli/dsd-neo /tmp/dsd-neo.after   # and one for 'before'

tools/replay_ab.sh --capture ~/captures/nxdn.json --mode -fi \
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
- **What `voice` counts.** Lines that label a frame `Voice`, and ProVoice frames,
  which print `VOICE` after their sync (trunked `PV`, or conventional `PV_C` with
  its addresses in between) and carry four IMBE voice frames each.
  Startup `NOTICE:` lines are left out: the ProVoice and EDACS presets print two
  that mention voice, and before issue #588 they were the whole count for a
  ProVoice run, so its errors were divided by 2 rather than by its frames.
  D-STAR prints nothing labelled `Voice`, so its unit is the superframe: each
  `Sync: ±DSTAR VOICE` or `Sync: ±DSTAR HEADER` line opens one of 21 AMBE voice
  frames. Before issue #599 a D-STAR run counted none, and the report dropped it.
  The log is read as text whatever bytes it holds: a D-STAR header decoded from
  noise prints its callsigns raw, and a UTF-8 `grep` then takes the log for a
  binary file and stops printing lines at the first of them.
- **Soft-decision error counts are not a quality score on their own.** mbelib's
  soft Golay reports how many data bits of the codeword it chose differ from the
  hard decisions, so a soft decode that lands on the right codeword where the
  hard one did not can report as many corrections, or more. For a change between
  hard and soft decoding (issues #588 and #599), compare the builds' `-Z` IMBE or
  AMBE payloads against a reference decode as well.
- **Paired per repeat.** Builds run round-robin with the order rotated each
  repeat, every build replays the same realization of the capture in a repeat,
  and the report compares within a repeat. Before issue #572 a fixed order
  credited the better slot to whichever build held it; with repeatable replay
  the order changes nothing unless determinism has regressed.
- **Each repeat is its own realization.** Replay is deterministic, so repeats of
  a capture as recorded are all one draw of it, and an interval paired over them
  claims a precision the capture does not have
  ([Replay determinism](#replay-determinism-issue-572) measures how much).
  Repeat r therefore replays `<out>/realizations/r<r>.json`, a copy of the
  sidecar whose `data_file` is the original data's path, made absolute the way
  replay resolves it (the sidecar's directory as given, with no `..`
  collapsed), and whose every event `byte_offset` is
  s_r = floor((r-1) P / reps) + (r-1) input samples later, held to the bytes
  replay reads, where P = round(sample rate / 2400) is one 2400-baud symbol:
  640 samples at 1.536 Msps, 20 at 48 kHz. Those bytes are `data_bytes`, or
  the data file's size when that is smaller or `data_bytes` is 0, rounded down
  to whole samples, as replay computes them. Repeat 1 is the capture as
  recorded; a sidecar with an event past those bytes, which replay refuses,
  stops the run before it replays anything.
  A later shift that lands on a whole multiple of the front end's total
  decimation D (`sample_rate_hz` over `demod_rate_hz`, else `base_decimation`
  x `post_downsample`) moves up one sample, so no repeat after the first hands
  every dwell the recorded output samples, only later; at 1.536 Msps,
  `--reps 24` would otherwise shift its 23rd repeat by 608 samples, 19 x 32.
  With `--reps 1` only repeat 1 runs, unshifted, and the header says so. Only
  cu8 and cf32 captures (2 and 8 bytes a sample) can be shifted; any other
  `sample_format` stops the run before it replays anything. Each progress line
  prints its repeat's `shift=`, `summary.tsv` records it in its last column,
  `shift`, and the report's first line counts the realizations. More repeats
  buy more realizations again: the default `--reps 12` is a floor for a result,
  and the measurements in [Replay determinism](#replay-determinism-issue-572)
  took 16 and 32.
- **A capture with no events** has nothing to shift. `replay_ab.sh` warns once
  (`the capture has no events: every repeat replays the same realization;
  compare it across captures`), replays every repeat as recorded and says so in
  its header, and the report notes that every repeat replayed one realization.
  Such a capture is one draw however many repeats it gets, so compare builds
  across captures.
- **Repeats that share an event schedule.** Events held at the end of the
  bytes replay reads stop moving, and two shifts can coincide, so two repeats
  can put every event in the same place. A repeat whose events land where an
  earlier repeat's did replays the same realization: its progress lines say
  `(same realization as rN)`, and `summary.tsv` records repeat N's shift, so
  the report counts the realization once. When every event sits at the end,
  every repeat is one realization, and the run warns once as for a capture with
  no events (`every event sits at the end of the bytes replay reads, where no
  shift moves it: every repeat replays the same realization; compare it across
  captures`). When only some repeats collapse, it prints one notice
  (`N of M repeats replay the same event schedule as an earlier repeat ..., so
  the run has K distinct realizations`), and the report notes that a
  realization several repeats replayed counts more than once in its interval.
- **`--no-realizations`** replays every repeat as recorded on purpose: the
  determinism control, in which each build's repeats must decode alike (an `sd`
  of 0) as well as pair to `+0.00 +/- 0.00`.
- **Run the baseline against itself first**, as a copy under another name
  (replay_ab.sh refuses two builds with the same basename). Replay is
  deterministic and both copies replay the same realization in a repeat, so
  that control must read `+0.00 +/- 0.00` in every column, with no differing
  repeat; any spread is a determinism regression to fix before anything is
  measured. Before issue #572 the control measured a noise floor instead, about
  0.1 errors per voice frame over 12 repeats on the captures behind issue #444.
- Leave `-T` and `-Y` out of `--mode`: those replays are outside the
  determinism guarantee. Otherwise load on the machine no longer changes what a
  replay decodes, and `--rate realtime`, the default, decodes what `fast` does,
  in the capture's own time.
- **Judge an acquisition change across draws.** Under `-fa`, which calls a
  capture yields depends on the hunt's phase when each arrives, so a change that
  moves the hunt's timeline or the front end's latency by a few samples can win
  or lose a call on one capture with nothing decoding better or worse. The
  realizations are such draws; replay several captures as well, and compare a
  pinned mode (`-fi`, `-f1`) beside `-fa`.
  [Replay determinism](#replay-determinism-issue-572) has a worked case.

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
`tone_lock_ms` and `tone_lock_pct`, then the run's exit status `rc` and `off_path`, 1 when the host warned that the
front end delivered CQPSK symbols instead of monitor samples, and last the repeat's `shift`, as in a digital run.
`probe_dbc` needs `--analog-expect-tone-hz`, so on a real capture, which has no test tone, it is `NA` and `probe_dbfs`
is the probe's level. The report pairs each column per repeat, pairs probe levels only between builds that probed the
same frequency (a wrapper that puts its own `--analog-probe-hz` first changes which probe comes first), and gives the
tone label each build settled on. A repeat that exited non-zero or ran off the monitor path is left out of every column
and counted in a per-build warning, even though the host prints its metrics before it exits: it measured a crash, a
timeout or the modulation auto-switch, not the build. So give the host no `--analog-*` bounds in `--mode`, since a
missed bound exits 1. Its `n` column counts the repeats in which a build measured that column. The paired interval needs
at least two paired repeats and reads `n/a` with one (`--reps 1`, or every other repeat left out), in the digital report
too, since one pair says nothing about the spread. A build missing a column that another build measured gets an explicit
`NA` row, and a build with no usable repeat (it crashed, timed out, left the monitor path, or is not an analog replay
host) makes the report warn and exit 1, rather than leave the other builds' rows looking like a clean result.
`tests/tools/test_replay_ab_report.py` (stdlib only) covers the per-repeat pairing, the A-vs-A control, probe frequency
keying, the coverage reporting, crashed and off-path repeats, the single-pair interval, the received-tone columns, the
duplicate-name refusal and the realizations: the shifted copies, their hold at the bytes replay reads (with `data_bytes`
0 or past the data file too), the cf32 stride, the shift moved off a multiple of the decimation, `data_file` made
absolute as replay resolves it (also under a symlinked directory) or kept when already absolute, paths with spaces,
`--reps 1`, the no-events warning, repeats whose events collapse onto one schedule (all of them or some),
`--no-realizations`, an unknown `sample_format` and the report's reading of `shift`. CTest runs it as four tests:
`TOOLS_REPLAY_AB_REPORT` scores canned summaries and runs wherever Python does, and `TOOLS_REPLAY_AB_ANALOG_METRIC`,
`TOOLS_REPLAY_AB_DIGITAL_METRIC` and `TOOLS_REPLAY_AB_REALIZATIONS` drive the real `replay_ab.sh` with fake hosts, so
they are registered only outside Windows where bash and coreutils `timeout` are found.

`tone`, `tone_lock_ms` and `tone_lock_pct` come from the host's `ANALOG METRIC:` line, which reads them from the
decoder's received-tone publication (`dsd_state::analog_rx`) after every block the monitor delivers; a host built
before a field existed reads `NA` for it. The host defines the fields and replay_ab.sh and the report pair them; the
CTCSS detector (#522) fills them for CTCSS, the DCS detector (#523) adds its label, and a detector that needs another
statistic adds its own field and column. The contract (also in the file comment of `tests/engine/analog_replay.c`),
which matters because replay_ab.sh splits the line on spaces:

- `tone=<label>`: the received tone or code with no whitespace, `151.4` (Hz, one decimal) for CTCSS, and for DCS both
  spellings of the code's signal, canonical first, joined by a slash, such as `D023N/D047I`: the log's
  `DCS D023N / D047I` as one token, since a receiver cannot tell the two apart (see
  [Tone and code labels](#tone-and-code-labels)); `NA` when none was confirmed. When the label changes during a run, the
  last one confirmed.
- `tone_lock_ms=<ms>`: stream time of the first confirmed lock, on the same clock as `first_audible_ms`: the end of
  the block after which the publication first read locked, with two decimals; `NA` when nothing locked.
- `tone_lock_pct=<pct>`: the share of the delivered audio (`captured_ms`) in blocks after which the publication read
  locked, 0 to 100 with two decimals: `0.00` when nothing locked, `NA` when no audio came out. On a capture whose tone
  is continuous it says how steadily the tone was held; on one with no tone it is the false-lock share and should read
  `0.00`.

replay_ab.sh names each build by its basename and refuses two with the same one, so copy each tree's host to its own
name. Per-variant flags or settings within one build go in a wrapper script per variant; its name is the variant's
name. The example compares main with a branch and, on the branch, the land-mobile de-emphasis (`DSD_NEO_DEEMPH=nfm`)
with the default. A channel-width variant is the same kind of wrapper passing `--nfm-bandwidth-hz`
(`printf '#!/bin/sh\nexec /tmp/ab/analog_replay.branch --nfm-bandwidth-hz 12500 "$@"\n' > /tmp/ab/nfm_12k5`), or
`--am-bandwidth-hz` under `-fM`; a fixed-gain variant passes `-n 50`, since the default gain is the AGC (issue #518),
which brings every block's peaks to the same level and so hides level differences between builds. AM has no main-branch
baseline before #524: its variants are paired against the branch's default width, and the FM monitor cases (`-fA` on
the NFM excerpts and on `am_airband_real`) are paired against main to show them unchanged. A design constant with no
runtime setting, such as the AM carrier time constant (`DSD_AM_CARRIER_TAU_MS`), is varied with temporary builds that
change only that constant, paired against the default build; revert the constant afterwards and check the rebuilt
default host is byte-identical to the one measured.

```sh
mkdir -p /tmp/ab
cmake --build --preset dev-debug -j --target dsd-neo_test_analog_replay
cp build/dev-debug/tests/dsd-neo_test_analog_replay /tmp/ab/analog_replay.branch   # and analog_replay.main
printf '#!/bin/sh\nDSD_NEO_DEEMPH=nfm exec /tmp/ab/analog_replay.branch "$@"\n' > /tmp/ab/deemph_nfm
chmod +x /tmp/ab/deemph_nfm

tools/replay_ab.sh --metric analog --capture tests/fixtures/iq/nfm_ctcss_real.iq.json \
    --mode "-fA -v 0 --analog-probe-hz 12500" --reps 3 --out /tmp/ab/ctcss \
    /tmp/ab/analog_replay.main /tmp/ab/analog_replay.branch /tmp/ab/deemph_nfm
tools/replay_ab_report.py /tmp/ab/ctcss/summary.tsv --baseline analog_replay.main
```

`-v 0` takes the monitor's voice filters out of the measurement (see
[Analog monitor audio checks](#analog-monitor-audio-checks)); leave it out when the question is what a listener gets.
The 12.5 kHz probe there reads the neighbour channel's leakage as `probe_dbfs`, over about 75 Hz around 12.5 kHz in
each 20 ms block, no finer.
Run the control first, one host against a copy of itself. I/Q replay is deterministic (issue #572), so the control must
read `+0.00 +/- 0.00` with no differing repeats, as the digital one must: 12 realtime repeats on `nfm_ctcss_real` and on
`nfm_tone_synth` did, for every column. The analog excerpts in `tests/fixtures/iq` have no events, so `replay_ab.sh`
warns and replays each as recorded in every repeat: there two or three repeats show the control (hence `--reps 3` above)
and more add nothing, and a difference between builds is one draw of each excerpt, to be compared across the excerpts. A
longer capture with events gets a realization per repeat, as a digital one does. A run whose log carries the host's
CQPSK-symbols warning left the monitor path: a build from before the analog family stood the modulation auto-switch down
does that on `am_airband_real` (see the `am_airband_real` note under
[Full-chain modulation decode tests](#full-chain-modulation-decode-tests)), and such a run measures the auto-switch, not
the change, and on a build from before issue #572 is not deterministic either; the report leaves it out as `off_path`
and warns, and a build with no other repeats fails the report. A difference that shows up in a control is a determinism
regression in the build or the harness, not the change's. Attach the report to the pull request with the capture, flags
and repeat count.

### Analog listen-test sign-off

Measured metrics do not replace listening. Before an analog DSP change merges, a maintainer listens to the paired
builds on the real excerpts, at `--iq-replay-rate realtime` with audio output on, and records the result in the pull
request:

- [ ] NFM at the default width and at 12.5 and 25 kHz on `nfm_ctcss_real`, `nfm_squelch_real_a` and
  `nfm_squelch_real_b`: speech intelligible, no new distortion, hiss or clicks; the neighbour channel of
  `nfm_ctcss_real` audible only at the widest setting (#525).
- [ ] AM on `am_airband_real` at several widths and with the AGC: speech intelligible, level steady across the
  excerpt, no pumping or clipping (#524).
- [ ] DCS on the air (#523), against a radio or repeater with a published DPL: a radio set to D023N reads
  `DCS D023N / D047I`, one set to D023I reads `DCS D047N / D023I`, and the turn-off code ends the lock (the N polarity
  and the bit order, which the synthetic fixtures cannot check).
- [ ] Tone filtering: allowed traffic opens within the detection window, rejected and untoned traffic stays silent,
  and nothing leaks at the start of a rejected transmission (#527).
- [ ] Level and AGC (#518), with the defaults (`-n 0`, `-v 0x9`): NFM, AM and digital voice at matching loudness in a
  mixed scan; no pumping in pauses (hiss does not swell between words); a squelch tail or key-up noise does not leave
  the next transmission quiet; AM static does not duck the speech after it for seconds; a 151.4 Hz CTCSS tone is
  inaudible; and the `-8` source monitor under digital decoding plays without clipping.
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
