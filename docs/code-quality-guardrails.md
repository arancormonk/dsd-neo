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
- Broad or high-risk changes: `tools/quality_preflight.sh`.
- Sanitizer-sensitive code: `ctest --preset asan-ubsan-debug --output-on-failure` after configuring/building the matching preset.
- Threading changes: `ctest --preset tsan-debug --output-on-failure` where the affected tests are supported by TSan.
- Fuzz-facing changes: `tools/fuzz_smoke.sh`.
- Cross-module includes or new headers: `tools/check_arch_rules.sh` (also run by pre-push and CI).
- CMake changes: `tools/cmake_format_check.sh`.
- Workflow changes: `tools/workflow_lint.sh` and `tools/zizmor.sh`.
- Dependency input changes: `tools/osv_scan.sh`.
- Repository security guardrails: `tools/check_secret_redaction.sh`, `tools/check_workflow_git_pins.sh`, and `tools/check_workflow_download_pins.sh`.

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

The repository intentionally blocks or flags patterns that are easy to reintroduce during large edits:

- Use the project safe API wrappers instead of raw C memory/string/formatting APIs in project-owned code.
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
