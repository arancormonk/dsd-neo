# Code Quality Guardrails

DSD-neo relies on layered checks because no single compiler, analyzer, or review pass catches every bad change. These guardrails apply to hand-written, copied, generated, and bulk-edited code equally.

## Review Expectations

- A human owns every submitted change and is responsible for understanding the behavior, failure modes, and module boundaries it touches.
- Broad or risky changes should get a second reviewer when one is available. Treat protocol parsing, crypto, runtime/threading, IO, workflow, dependency, and release changes as risky by default.
- Solo-maintainer changes that cannot get a second reviewer must document the higher-risk areas touched, keep the diff focused, and pass the required local/CI guardrails before merge or release. CODEOWNERS ownership by the same maintainer does not count as independent review.
- Copied, generated, or bulk-written code must be adapted to local conventions before review, not accepted as a black box.
- Static-analysis suppressions must explain why the warning is a false positive or why the local exception is acceptable. Keep suppressions close to the narrowest affected code.
- Do not add unrelated refactors to quality or security fixes. Smaller diffs make analyzer output and review results more reliable.

## High-Risk Change Checklist

Use this checklist when a change touches parser/decoder logic, external input, concurrency, allocation, dependencies, workflows, or packaging:

- Add or update focused tests under the matching `tests/<area>/` tree.
- For new file, network, radio, or protocol input, consider a fuzz target or corpus entry.
- For new public APIs, verify headers live under `include/dsd-neo/<module>/` and are included as `<dsd-neo/...>`.
- For new dependencies, document the dependency in CMake, README/install notes, and third-party notices as applicable.
- For workflow changes, keep `GITHUB_TOKEN` permissions least-privilege and avoid interpolating untrusted GitHub context directly into shell scripts.
- For release changes, verify SBOM, artifact, attestation, and release-hardening checks still run on tag builds.
- For release tags, verify the tag is annotated and signed by a trusted key before publishing.
- For dependency or workflow changes, also check the supply-chain policy in `docs/supply-chain-guardrails.md`.

## Required Local Checks

Run the smallest useful set before opening a PR, then broaden it when the change is risky.

- Normal C/C++ changes: `cmake --build --preset dev-debug -j` and `ctest --preset dev-debug --output-on-failure`.
- Normal pre-push check: `tools/preflight_ci.sh` (concurrent lanes sized to the machine, every failure reported at the end; `DSD_HOOK_SERIAL=1` for one-at-a-time streaming output).
  The hook checks the files `tools/push_changed_files.sh` lists for each pushed ref. When the remote's commit is
  not in the clone it compares with the remote's default branch, and when no comparison works the push fails
  instead of going out unchecked. CI's pull-request jobs take their files from `tools/ci_changed_files.sh` the same
  way: names stay raw (`git diff -z`), and a diff that fails fails the job. When a change touches
  `tools/ci-dependency-pins.env`, `cmake/Pffft.cmake`, `cmake/TetraCrypto.cmake` or a vendored snapshot, both run
  cppcheck, clang-tidy, IWYU and GCC `-fanalyzer` over the whole tree, as the push to main does: a dependency's
  headers can move a finding into a file the change did not touch.
- Broad or high-risk changes: `tools/quality_preflight.sh`.
- Sanitizer-sensitive code: `ctest --preset asan-ubsan-debug --output-on-failure` after configuring/building the matching preset.
- Threading changes: `ctest --preset tsan-debug --output-on-failure` where the affected tests are supported by TSan.
- Fuzz-facing changes: `tools/fuzz_smoke.sh`.
- Cross-module includes or new headers: `tools/check_arch_rules.sh` (also run by pre-push and CI).
- CMake changes: `tools/cmake_format_check.sh`.
- Workflow changes: `tools/workflow_lint.sh` and `tools/zizmor.sh`.
- Dependency input changes: `tools/osv_scan.sh`.
- Repository security guardrails: `tools/check_secret_redaction.sh`, `tools/check_workflow_git_pins.sh`, and `tools/check_workflow_download_pins.sh`.

## Tool Reference

- Format: `tools/format.sh` (requires `clang-format`; see `.clang-format`).
- Static analysis:
  - `tools/clang_tidy.sh` (promotes broad bugprone/performance/portability findings; targeted TUs supported).
  - `tools/cppcheck.sh` (use `--strict` for broader checks).
  - `tools/iwyu.sh` (include hygiene via include-what-you-use; excludes `src/third_party`).
  - `tools/gcc_fanalyzer.sh` (GCC `-fanalyzer` path-sensitive diagnostics; C translation units only, excludes
    `src/third_party`).
  - `tools/scan_build.sh` (Clang Static Analyzer via `scan-build`, heavier full-build pass; excludes `src/third_party`;
    supports repeatable `--cmake-arg` passthrough).
  - `tools/semgrep.sh` (additional SAST and project guardrail rules; use `--strict` to fail on findings; excludes
    `src/third_party`).
  - `tools/shell_lint.sh` (ShellCheck plus `shfmt -d` for shell scripts and hooks).
  - `tools/workflow_lint.sh` (actionlint for GitHub Actions workflows).
  - `tools/zizmor.sh` (GitHub Actions security analysis with SARIF support).
  - `tools/osv_scan.sh` (OSV dependency and vendored C/C++ vulnerability scanning).
  - `tools/cmake_format_check.sh` (CMake formatting with gersemi; use `--fix` to rewrite).
  - `tools/gitleaks.sh` (secret scanning with SARIF output for GitHub code scanning).
  - `tools/lizard.sh` (function complexity and length; `--strict` fails at CCN 15 or 100 lines).
- Security guardrails:
  - `tools/check_arch_rules.sh` (module include boundaries and forbidden constructs from `cmake/arch_rules.cmake`; see
    `docs/code_map.md`).
  - `tools/check_secret_redaction.sh` (blocks formatted key/keystream output outside the redaction formatter helpers).
  - `tools/check_workflow_git_pins.sh` (blocks floating public GitHub source checkouts in workflows and CI helper
    scripts).
  - `tools/check_workflow_download_pins.sh` (blocks mutable release helper downloads and digestless AppImage container
    refs).
  - `tools/check_release_hardening.sh` (verifies Linux ELF PIE/RELRO/BIND_NOW, macOS Mach-O PIE/@rpath, and hardening
    compile flags).
  - `tools/check_release_hardening.ps1` (verifies Windows PE ASLR, NX, and high-entropy VA hardening).
  - `tools/check_vcpkg_overlay_pins.sh` (vcpkg overlay source pins match `tools/ci-dependency-pins.env` and the USB
    library overlays carry the current patch series; `--fix` rewrites them).
  - `tools/check_install_runtime_destinations.sh` (install rules with library destinations also name a RUNTIME
    destination, for Windows DLLs).
- Packaging checks: `tools/check_patched_usb_libs.sh` (libairspy and librtlsdr binaries carry DSD-neo's patches),
  `tools/check_macos_portable_deps.sh` (the macOS portable bundle's library dependencies) and
  `tools/check_android_elf_alignment.sh` (Android ELF payloads are arm64 with 16 KB page alignment).
- Licensing: `python3 tools/legal_inventory.py` (run by `tools/quality_preflight.sh`; see `legal/status.md`).
- Fuzzing: `tools/fuzz_smoke.sh` configures/builds the `fuzz-asan-debug` preset and runs bounded libFuzzer smoke passes.
- Git hooks: `tools/install-git-hooks.sh` enables auto-format on commit and a CI-aligned pre-push analysis pass
  (architecture rules, security guardrails including workflow source/download pins and vcpkg overlay pins,
  install-destination checks,
  clang-format, CMake format, clang-tidy, cppcheck, IWYU, GCC fanalyzer, Lizard, Semgrep, zizmor, OSV scan, and
  shell/workflow lint) on changed paths. The checks run in concurrent lanes sized to the machine's core count and every
  failure is reported at the end; `DSD_HOOK_JOBS=N` sets the worker budget shared out across the whole run (each check
  that runs keeps at least one worker, so a budget below the number of concurrent checks does not shrink it further)
  and `DSD_HOOK_SERIAL=1` runs the checks one at a time with streaming output, stopping at the first failure.
- Optional full scan-build pre-push/preflight pass: set `DSD_HOOK_RUN_SCAN_BUILD=1`. It rebuilds from scratch, since the
  analyzer only sees translation units the build recompiles; `DSD_HOOK_SCAN_BUILD_REUSE=1` takes the faster
  incremental answer, which covers only what changed since the last run.
- Manual preflight runner: `tools/preflight_ci.sh` runs the same CI-aligned checks as `pre-push` without pushing.
- Full quality preflight: `tools/quality_preflight.sh` enables missing-tool failures, includes scan-build, and runs the
  full local guardrail set, overlapping gitleaks with the pre-push checks and the whole-tree lint with the fuzz build;
  every failure is reported at the end.
- Supply-chain update policy: `docs/supply-chain-guardrails.md`.

## Project-Specific Guardrails

A gate that passes must have earned it. `tools/lib/check_runner.sh` runs the checks in concurrent lanes and captures each
one's output to a log it prints only for failures, so anything a *passing* check says would otherwise be discarded — and
what a tool says when it downgrades a real failure to a pass is exactly the line that matters. Two mechanisms carry it
to the verdict, and a change to any local check should use them rather than printing and hoping:

- A tool that passes while covering less than it was asked to prints one self-contained line containing `: NOTE: ` (for
  example `clang-tidy: NOTE: 2 requested file(s) are not in the compilation database and were not analyzed: …`). The
  runner collects those lines out of the passing check's log and prints them under "notes from checks that passed".
  Keep the whole note on the marked line: continuation lines are not collected.
- A gate that skips an analysis calls `runner_note_skipped "<what did not run, and why>"` (a missing tool calls
  `runner_note_missing` instead). Either one makes the run report what it did not cover rather than "all checks passed",
  and is fatal under `DSD_HOOK_FAIL_ON_MISSING_TOOLS=1`, which `tools/quality_preflight.sh` sets.

`tools/gcc_fanalyzer.sh` compiles each C translation unit with `-S -o /dev/null -fanalyzer`. Assembling is the point:
`-fanalyzer` is an interprocedural pass that never runs under `-fsyntax-only`, which the script passed for as long as it
existed, so the pre-push lane and the CI leg both reported a clean run over an empty analysis.
`tests/tools/test_gcc_fanalyzer.sh` seeds a double free and fails if the script stops reporting or counting it.

Two scope decisions keep that check worth reading:

- **C only.** GCC's manual says the analyzer "is only suitable for use on C code in this release"; on C++ it walks
  exception-unwind edges out of `extern "C"` callees that cannot throw and reports leaks no execution reaches. C++ units
  are skipped, and the count is printed.
- **`-Wanalyzer-null-dereference` is off.** `dsd_safe_memset_impl()` and its neighbours in `core/safe_api.h` compare
  their destination against NULL, which teaches the analyzer that the caller's own pointer parameter may be NULL; every
  `DSD_MEMSET(p, 0, sizeof *p)` followed by `p->field` then reads as a null dereference. That is how the project zeroes
  a struct, so the class covered 91 reports and no reachable path.

Every other analyzer diagnostic is fatal. Where the analyzer's model rather than the code is what fails, the suppression
is a `#pragma GCC diagnostic ignored` guarded by `#if defined(__GNUC__) && !defined(__clang__)` at the site with its
reason: the stderr and stdin capture helpers in tests, where `dup2()` returns a descriptor the process already owns and
must not close, and `config_profile_free_context()`, where the analyzer loses the tie between `pctx->n` and the count
that filled the array.

`tools/iwyu.sh` fails a translation unit that does not compile under IWYU's clang, in strict mode and out of it, and
prints its diagnostics. IWYU 0.27 exits 0 after a compile error such as a `-Werror` diagnostic and goes on to analyse
the unit, so the verdict comes from the output: a line that starts with `file:line:col: error:`, `fatal error:` or the
driver's bare `error:`. The summary counts these as `compile_errors`, apart from include suggestions. Before this check
matched a located diagnostic, units that failed to compile passed the strict gate as clean.
`tests/tools/test_iwyu.sh` drives the classification with a fake IWYU.

The repository intentionally blocks or flags patterns that are easy to reintroduce during large edits:

- Use the project safe API wrappers instead of raw C memory/string/formatting APIs in project-owned code.
- Compare floating-point values with an explicit tolerance, in production code as well as tests. The local Semgrep rule covers scalar `float`/`double` locals, parameters and local array subscripts declared in the same function, plus the float/double members of the shared option, state, config, demod and service structs by name (regenerate the list with `tools/semgrep_float_fields.py` when those structs change). Explicit literal sentinel checks remain allowed. It is a syntactic guardrail, not complete type analysis; CodeQL's `cpp/equality-on-floats` is the type-aware backstop. Strict scans run its positive and negative fixtures in `semgrep/dsd-neo.cpp` before scanning the tree.
- Read time through `<dsd-neo/runtime/decode_clock.h>`, so every clock read names its domain: `dsd_decode_time()`/`dsd_decode_now_*()` for decode decisions, decoded-output timestamps and anything compared with a decoded stamp, and `dsd_realtime_*()` for sleeps, waits, timeouts, pacing, UI throttles and perf. A raw read (`time()`, `clock_gettime()`, `gettimeofday()`, `dsd_time_monotonic_*()`, `std::chrono` clocks, `QDateTime::current*()`, `QElapsedTimer`, JavaScript `Date.now()`/`new Date()`) stays on the wall clock during an I/Q replay, so a decision made on it changes from run to run. `dsd-neo.no-direct-clock-read` covers C/C++ in `src/`, `include/`, `apps/` and `android/`. It exempts only the files that implement or sit below the two domains: `src/platform/`, `src/runtime/decode_clock.c`, `src/io/iq/iq_capture.c` and `src/io/radio/tcp_quality_metrics.cpp`, which cannot link runtime, and `src/ui/qt/realtime_clock.h`, the Qt frontend's one real-time helper. `dsd-neo.no-direct-clock-read-js` is a text match over the frontend's `.qml` and `.js`, because Semgrep has no QML parser. Tests are outside both rules, since they stub and wrap the platform clock; to step decode time they select the decode clock's TEST source (`docs/testing.md`, "Decode time in unit tests"). Strict scans run the fixtures in `semgrep/dsd-neo.c`, `semgrep/dsd-neo.cpp` and `semgrep/dsd-neo.js`.
- Write real control characters in format strings. `"\\n"` prints a literal `\` followed by `n` rather than breaking the line; this is easy to reintroduce when rewriting `fprintf` call sites in bulk, and has silently broken DMR console and structured-output dumps more than once. If a literal escape sequence is genuinely intended, emit it outside a format string or annotate the line with a narrow `nosemgrep` comment explaining why.
- Do not use wide-character conversions (`%lc`, `%ls`, `%C`, `%S`) in format strings, in any of the printf-family or curses `printw`-family wrappers, and do not call the wide-character output functions (`fwprintf`, `wprintf`, `swprintf`, `fputws`, `fputwc`, `putwc`, `putwchar` and their `v` variants). They hand a code unit to the C runtime to encode, and off-air text regularly contains one that has no encoding — a lone surrogate. glibc drops it; the Windows UCRT reports the failed conversion as a string of length -1 and then writes the stack to the stream until the process faults. Decode with `<dsd-neo/core/utf16.h>` and print through `dsd_unicode_fput_scalar()`.
- Do not execute shells or spawn processes from project-owned C/C++ without explicit design review.
- Do not include bundled third-party headers directly outside approved wrappers and integration points.
- Keep workflow scripts defensive: pass untrusted context through environment variables or action inputs, not direct expression interpolation in `run:` blocks.
- Keep vcpkg overlay ports pinned by immutable `REF` and `SHA512`.
- Keep release helper downloads immutable: build AppImage helper tools from pinned source, pin container images by digest, and verify executable installers by SHA256 before running them.
- Do not print radio keys, keystreams, API keys, or derived key material in logs, terminal UI, or test diagnostics except for intentional radio key/keystream reveal through the CLI-only `--show-keys` flag. Successful radio key/keystream messages must use the redaction formatter helpers; API keys and derived key material remain non-printable.
- Keep CI GitHub source dependencies pinned through `tools/ci-dependency-pins.env` and `tools/fetch-pinned-git.sh`.
- Keep analyzer and linter output actionable. Prefer fixing root causes over widening suppressions.
- Suppress a Semgrep match with a line-scoped `nosemgrep: <rule-id>` comment and a note saying why the rule does not apply. GitHub code scanning ignores the SARIF `suppressions` property Semgrep uses to mark those matches, so `tools/semgrep.sh` runs `cmake/sarif_drop_suppressed.cmake` over the SARIF before upload: without it every documented suppression arrives as an open alert that no change to the tree can close, and has to be dismissed by hand.
- Keep vendored code out of CodeQL results through `paths-ignore` in `.github/codeql/codeql-config.yml`. CodeQL applies that list only to files it indexes without a build; the traced C/C++ build analyzes every file it compiles, including vendored sources a test builds (`android/third_party/librtlsdr` for the `VENDORED_LIBRTLSDR_*` tests, `android/third_party/libairspy` for `VENDORED_LIBAIRSPY_TRANSFERS`). The `codeql` workflow therefore saves the SARIF, runs `tools/sarif_drop_ignored_paths.py` with the same config to drop results whose primary location matches the list, and uploads the result. The tool reads only plain top-level keys and a block list of quoted `*`/`**` globs, and fails on any other config shape, including a `paths` include list.
