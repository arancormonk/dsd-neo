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
