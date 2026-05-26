## Summary

- 

## Review

- [ ] Changes were reviewed against the surrounding module design.
- [ ] Behavior, ownership boundaries, and failure modes were reviewed by a human.
- [ ] Risky or broad changes have a second reviewer, or the solo-maintainer exception and extra guardrails are documented.
- [ ] Copied, generated, or bulk-written code has been read, understood, and adapted to DSD-neo conventions.
- [ ] Non-trivial commits include a DCO sign-off, or the exception is explained.

## Quality Review

- [ ] New parser, decoder, file input, network/radio input, allocator, thread, or dependency changes include focused tests.
- [ ] Protocol, crypto, runtime, IO, workflow, dependency, and release changes received extra scrutiny.
- [ ] Static-analysis suppressions include a reason and are limited to the narrowest scope.
- [ ] No new raw C memory/string/formatting APIs, direct third-party includes, shell execution, or broad workflow permissions were added without justification.

## Tests And Guardrails

- [ ] `cmake --build --preset dev-debug -j`
- [ ] `ctest --preset dev-debug --output-on-failure`
- [ ] `tools/preflight_ci.sh`
- [ ] `tools/quality_preflight.sh` for broad or high-risk changes
- [ ] `tools/cmake_format_check.sh` for CMake changes
- [ ] `tools/zizmor.sh` for workflow changes
- [ ] `tools/osv_scan.sh` for dependency input changes
- [ ] `tools/check_secret_redaction.sh` and `tools/check_workflow_git_pins.sh` for security-sensitive changes

## Risk

- Security/dependency impact:
- CLI/UI behavior impact:
- Compatibility or packaging impact:
