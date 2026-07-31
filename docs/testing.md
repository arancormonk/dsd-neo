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

Each case asserts on a decoded payload field (NAC, WACN/SYS, colour code, RAN,
callsign, site ID) rather than a sync count, so a silent framing or protocol
regression fails the test instead of merely moving a counter. The whole set runs
in about 7 seconds because I/Q replay defaults to `--iq-replay-rate fast`.

Covered: P25 Phase 1 C4FM (control and voice), P25 Phase 1 CQPSK/LSM (control and
voice), P25 Phase 2, DMR voice, DMR Tier III control, NXDN48, NXDN96, dPMR,
D-STAR, YSF, EDACS, and M17.

Fixture provenance and regeneration live in `tools/build_iq_fixtures.py`; see
`THIRD_PARTY.md` for sample attribution. After regenerating a fixture, re-verify
its decode margin (the original set still passed with ±45 counts of added
noise) and that a mismatched mode flag produces no match, so the assertions
stay robust rather than borderline. Sources that exist only as FM
discriminator audio are integrated back into complex baseband (FM demodulation
is invertible), so those fixtures exercise the same code path but carry none of
the original RF impairments. Where a genuine off-air I/Q recording exists (P25
C4FM/CQPSK, NXDN48/96, dPMR) it is used directly.

Known gaps and caveats:

- **ProVoice** and **X2-TDMA** have no usable public sample and are untested here.
- **P25 Phase 2** asserts SACCH framing only. Full payload decode needs the
  system WACN/SYSID/CC via `-X`, which the public sample does not identify.
- **M17** asserts only the LSF `CAN` field. The same source decodes cleanly from
  native 48 kHz baseband via `-i file.wav` but degrades to mostly LSF CRC errors
  through the I/Q chain; that discrepancy is unresolved.
- Fixtures are timing-insensitive by construction. Do not add assertions that
  depend on wall-clock call-state timers, because `fast` replay compresses them;
  use `--iq-replay-rate realtime` for that.

## Continuous Integration

GitHub Actions runs tests and quality checks on pull requests, primary-branch
pushes, tags, schedules, and manual dispatches. Coverage varies by event:
cross-platform builds, sanitizer tests, static analysis, workflow linting,
secret scanning, OSV scanning, repository guardrails for secret redaction and
workflow source/download pinning, fuzz smoke tests, and install/package
validation run where their workflows declare those events. Dependency review is
PR-only, release tag validation is tag-only, and extended fuzzing is scheduled
or manually dispatched.

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
