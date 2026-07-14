# Security Requirements

DSD-neo is a native C/C++ command-line decoder and signal-processing tool. Its
main security goal is to process potentially malformed radio, file, and network
inputs without memory corruption, credential exposure, unexpected process
execution, or unsafe release behavior.

## What Users Can Expect

- Public CLI and library paths should validate externally supplied files,
  frames, packets, metadata, and configuration values before use.
- Parser and decoder changes should keep reads and writes within documented
  buffers and state objects.
- Normal decode paths should not execute shells or spawn external processes.
- Network-facing features are explicit opt-in inputs or outputs, not hidden
  background listeners.
- PCM UDP input and M17 frame listeners default to loopback unless the user
  explicitly binds a wider address such as `0.0.0.0`.
- Release and CI workflows should use least-privilege permissions and avoid
  exposing release credentials to pull-request code.
- Security vulnerabilities should be handled through the private reporting
  process in `SECURITY.md`.

## What Users Should Not Expect

- DSD-neo is not a sandbox. Users processing untrusted captures or network
  streams should still use normal OS isolation, least privilege, and media
  hardening practices.
- DSD-neo does not provide patent licensing advice or patent clearance.
- DSD-neo does not authenticate an upstream radio system, SDR device, capture
  file, TCP/UDP peer, or rdio-scanner endpoint.
- DSD-neo does not provide encrypted transport for UDP/TCP audio, M17 UDP, or
  local control channels. Use a trusted local network, VPN, SSH tunnel, TLS
  proxy, or equivalent transport protection when crossing trust boundaries.
- DSD-neo does not provide secure secret storage. User-supplied radio keys,
  keystream CSVs, and rdio API keys remain local files, command-line arguments,
  or process memory controlled by the user.
- Floating-point DSP output is not guaranteed to be bit-identical across all
  compiler, architecture, SIMD, and optimization combinations.

## Threat Model

Relevant threats include:

- malformed radio frames, IQ metadata, WAV metadata, config files, CSV files, or
  network packets triggering buffer overflows or undefined behavior
- crafted inputs driving excessive CPU work, disk writes, or unstable state
  transitions
- mistakes in SIMD, DSP, protocol, FEC, crypto, or architecture-specific code
  paths
- accidental release of generated binaries, private captures, credentials,
  radio keys, API keys, or signing material
- compromised CI/CD dependencies or workflow changes
- vulnerabilities in vendored third-party code
- disclosure of local recordings or metadata through misconfigured export paths
  or rdio API endpoints

Out-of-scope threats include:

- physical radio-layer attacks outside the DSD-neo process
- legal authorization to monitor or decode any radio system
- patent enforcement or licensing disputes
- malicious local users who can already read the same config, key, capture, or
  recording files as the DSD-neo process
- compromise of GitHub, AUR, package-hosting, or SDR driver infrastructure
  outside the project's control

## Trust Boundaries

- Radio samples, symbols, protocol frames, capture files, CSV files, config
  files, and network packets are untrusted input.
- User-provided local radio keys, keystream maps, and rdio API keys are
  sensitive data and should not be logged, committed, or attached to issues.
- Vendored code under `src/third_party/` and inherited codec/protocol code is
  treated as external or legacy code and reviewed separately from new
  project-owned code.
- CI workflow files are security-sensitive because they control analyzer,
  packaging, release, SBOM, and attestation behavior.
- Release credentials, GitHub Actions secrets, AUR credentials, and signing
  material are sensitive resources and must not be exposed to untrusted
  pull-request code.

## Cryptography and Credentials

DSD-neo includes crypto helpers for interoperability with deployed land-mobile
radio protocols and vendor privacy modes. These helpers include AES, DES/TDEA,
RC4, RC2, and protocol-specific keystream handling.

Those algorithms are not a general-purpose security boundary for DSD-neo users.
Weak algorithms such as DES, RC4, and RC2 are retained only so DSD-neo can
decode interoperable protocol traffic when a user is legally authorized and
explicitly supplies the required key or keystream material. They should not be
used as a basis for new systems or new confidentiality designs.

The project does not:

- generate cryptographic keys or nonces for users
- store passwords for authenticating external users
- provide key exchange or perfect-forward-secrecy protocols
- implement TLS
- promise encrypted storage for radio keys, API keys, captures, or recordings

Users should protect local config and key files with OS permissions, avoid
passing sensitive keys through shared shell history, and prefer encrypted
transport when using remote rdio API or network audio endpoints. Rdio API
uploads do not follow redirects; configure the final trusted endpoint directly.

## Secure Design Controls

The project applies the following controls:

- explicit CMake source lists instead of broad source globbing
- public API declarations separated under `include/dsd-neo/`
- target-scoped warnings and warnings-as-errors by default
- project safe API wrappers for memory, string, and formatting calls
- broad CTest coverage across runtime, platform, DSP, IO, engine, FEC, crypto,
  protocol, core, and UI modules
- default-on Release-like compiler/linker hardening for supported Clang, GCC,
  and MSVC targets, with release verification in CI
- exclusive private sibling temp files for atomic user config, IQ metadata, and
  Rdio sidecar replacement
- redaction guardrails for key/keystream output paths, with intentional radio key/keystream reveal limited to the CLI-only
  `--show-keys` flag and shared redaction formatter helpers
- sanitizer CI for AddressSanitizer and UndefinedBehaviorSanitizer
- ThreadSanitizer preset for threading-sensitive local validation
- libFuzzer smoke targets for selected file, metadata, config, and protocol
  parsers
- static analysis through CodeQL, Semgrep, clang-tidy, cppcheck, scan-build,
  GCC `-fanalyzer`, include-what-you-use, and lizard
- Gitleaks and GitHub secret scanning for credential leakage
- OSV-Scanner and dependency review for dependency vulnerabilities
- pinned GitHub Actions, pinned CI source checkouts, pinned workflow downloads,
  and zizmor workflow security checks
- branch protection requiring status checks before merge

## Solo Maintainer Operating Mode

DSD-neo is allowed to operate as a solo-maintainer project, but the absence of
a routine second reviewer is treated as residual risk, not as implicit approval.
For security-sensitive, workflow, release, parser, crypto, dependency, or broad
runtime changes, the maintainer should:

- keep the change narrowly scoped and document the risk in the pull request or
  release notes
- run `tools/preflight_ci.sh` or a broader equivalent before merge
- avoid bypassing failing guardrails unless the bypass and mitigation are
  documented
- seek outside review for high-impact security or release-signing changes when
  practical

## Vulnerability Handling

Vulnerability reports are handled through `SECURITY.md`. Confirmed
vulnerabilities should result in a fix, release note or advisory, affected
version information, and reporter credit unless anonymity is requested.
