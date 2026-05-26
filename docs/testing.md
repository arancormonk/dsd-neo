# Testing Policy

DSD-neo uses automated tests, static analysis, sanitizers, and fuzz smoke tests
to reduce regression and security risk.

## Test Suites

The CTest suite includes focused tests for:

- runtime configuration, CLI parsing, rings, hooks, shutdown, and telemetry
- platform audio concealment, atomics, files, and timing primitives
- DSP filters, demodulators, FLL/TED, resamplers, SIMD helpers, and symbol paths
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

## Continuous Integration

GitHub Actions runs tests and quality checks on pull requests and pushes to the
primary branch. Required checks include cross-platform builds, sanitizer tests,
static analysis, workflow linting, dependency review, secret scanning, OSV
scanning, repository guardrails for secret redaction and workflow source/download
pinning, fuzz smoke tests, release tag validation, and install/package validation.

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
