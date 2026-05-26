# Contributing to DSD-neo

DSD-neo accepts changes through GitHub issues and pull requests. Keep changes
small, focused, and reviewable.

## Contribution Process

1. Open or reference an issue for user-visible behavior changes, API changes,
   dependency changes, release changes, and security-sensitive work.
2. Create a branch from `main`.
3. Make the smallest coherent change that solves the problem.
4. Add or update tests and documentation when behavior, public API, build
   behavior, release behavior, or security posture changes.
5. Run the relevant local checks before opening a pull request.
6. Open a pull request against `main` and complete the pull request template.
7. Address review comments and wait for required CI checks to pass before merge.

Security vulnerabilities must not be reported through public issues or pull
requests. Use the private reporting process in `SECURITY.md`.

## Legal Certification

Non-trivial contributions must use the Developer Certificate of Origin 1.1
sign-off. Add this line to each commit message:

```text
Signed-off-by: Your Name <your.email@example.com>
```

The sign-off means you certify that you have the right to submit the work under
the project's license. The DCO text is published at:

https://developercertificate.org/

Use `git commit -s` to add the sign-off automatically.

## Acceptable Contribution Requirements

A contribution is acceptable only when it meets the requirements below or the
pull request explains why an exception is appropriate.

- The change is compatible with GPL-3.0-or-later and retained third-party
  notices.
- Project-maintained source files include an SPDX license identifier near the
  top of the file.
- New or changed public APIs are documented in `include/dsd-neo/` comments and
  in user-facing documentation when needed.
- Major functionality changes include focused automated tests.
- Bug fixes include regression tests when practical.
- Generated build outputs, compiled binaries, private captures, credentials,
  radio keys, and machine-specific configuration are not committed.
- Vendored third-party code stays under `src/third_party/`, keeps upstream
  license notices, and is documented in dependency records.
- Workflow, dependency, packaging, release, parser, decoder, crypto, radio IO,
  and security changes receive extra review.
- Static-analysis suppressions are narrow and explain why the local exception is
  acceptable.

## Coding Standards

The project uses C11, C++14, and CMake.

- C and C++ code follows the repository `.clang-format` configuration, based on
  LLVM style with 4-space indentation and a 120-column limit.
- CMake files are formatted with `gersemi` through
  `tools/cmake_format_check.sh`.
- Public headers live under `include/dsd-neo/<module>/` and should be included
  as `<dsd-neo/...>`.
- Keep module boundaries aligned with `docs/code_map.md`.
- Prefer project safe API wrappers over raw C memory, string, and formatting
  APIs in project-owned code.
- Avoid direct inclusion of bundled third-party headers outside approved wrapper
  and integration points.
- Do not spawn shells or external processes from project-owned C/C++ without
  explicit design review.

Run formatting with:

```sh
tools/format.sh
```

## Tests and Local Checks

Run the smallest useful check set for the change, then broaden it for risky
changes.

```sh
cmake --preset dev-debug
cmake --build --preset dev-debug -j
ctest --preset dev-debug --output-on-failure
```

Normal pre-push check:

```sh
tools/preflight_ci.sh
```

Broad or high-risk changes:

```sh
tools/quality_preflight.sh
```

Additional focused checks:

- CMake changes: `tools/cmake_format_check.sh`
- Workflow changes: `tools/workflow_lint.sh` and `tools/zizmor.sh`
- Dependency input changes: `tools/osv_scan.sh`
- Security-sensitive workflow/release changes: `tools/check_secret_redaction.sh`,
  `tools/check_workflow_git_pins.sh`, and
  `tools/check_workflow_download_pins.sh`
- Sanitizer-sensitive code: configure and build `asan-ubsan-debug`, then run
  `ctest --preset asan-ubsan-debug --output-on-failure`
- Fuzz-facing changes: `tools/fuzz_smoke.sh`

## Test Policy

Major new functionality must add or update automated tests. For this project,
"major" includes public API changes, protocol behavior changes, DSP changes,
radio/network/file input changes, dependency updates that affect compiled code,
security-sensitive behavior, workflow changes that gate releases, and
packaging/install changes.

The preferred test style is a focused regression test under the matching
`tests/<area>/` tree that can run through CTest. If a behavior cannot be tested
directly, the pull request must explain the reason and include the best
practical substitute, such as a golden fixture, analyzer rule, install/consume
check, or fuzz target update.

## Review Requirements

Every pull request is reviewed for:

- correctness and compatibility with surrounding module design
- security impact, including untrusted input handling and dependency changes
- API and CLI compatibility
- licensing and attribution
- test coverage and documentation coverage
- packaging and release impact

Risky or broad changes should receive a second human review before merge.
