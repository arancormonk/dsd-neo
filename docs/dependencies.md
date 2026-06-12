# Dependency Management

DSD-neo keeps runtime, packaging, and analysis dependencies explicit so changes
are reviewable.

## Compiled Dependencies

Required dependencies are:

- C compiler with C11 support
- C++ compiler with C++14 support
- CMake 3.20 or newer
- OpenSSL 3.x libcrypto
- `mbe-neo` 2.x CMake package from mbelib-neo
- libsndfile
- curses backend: ncursesw/PDCurses
- audio backend: PulseAudio by default on Unix-like systems, PortAudio on
  Windows

Optional compiled dependencies are:

- librtlsdr for RTL-SDR input
- SoapySDR 0.8.1 or newer for non-RTL SDR devices; the CMake package must
  export an imported target (`SoapySDR` or `SoapySDR::SoapySDR`)
- Codec2 for additional vocoder paths
- libcurl for rdio-scanner API uploads
- PortAudio on non-Windows builds when selected
- help2man for generated man pages

Vendored compiled third-party components are:

- ezpwd Reed-Solomon under `src/third_party/ezpwd/`
- PFFFT/FFTPACK under `src/third_party/pffft/`
- Tiny AES code in `src/crypto/crypt-aes.c`

Vendored code retains upstream notices. License and attribution details are in
`THIRD_PARTY.md`.

Registry-managed vcpkg dependencies are pinned by the manifest
`builtin-baseline`. At baseline `f3e10653cc27d62a37a3763cd84b38bca07c6075`,
the `openssl` port resolves to OpenSSL `3.6.2` and the `curl` port resolves to
curl `8.19.0`; system-package builds enforce the OpenSSL requirement through
`find_package(OpenSSL 3.0 REQUIRED)`.

## Packaging Dependencies

Windows builds use vcpkg overlays under `vcpkg-ports/` and triplets under
`vcpkg-triplets/`. AppImage builds use pinned CI source checkouts for compiled
dependencies that are not suitable from the Ubuntu 20.04 base image, including
SoapySDR 0.8.1 or newer. Linux source-install validation uses pinned Docker
images from `tools/ci-dependency-pins.env` to exercise apt, dnf, zypper, apk,
and pacman bootstrap paths. Overlay ports, CI source checkouts, and validation
container images must use immutable source references and hashes as described in
`docs/supply-chain-guardrails.md`.

## Tooling Dependencies

CI and local quality tools are tracked through:

- `.github/requirements/*.in`
- `.github/requirements/*.txt`
- `.github/dependabot.yml`
- `.github/workflows/*.yml`
- `tools/*.sh`

Hashed Python requirements are used where Python tooling is installed in CI.
GitHub Actions are pinned to immutable commit SHAs by policy.

## Monitoring

The project monitors dependencies through:

- Dependabot for GitHub Actions and Python requirements
- GitHub dependency review on pull requests
- OSV-Scanner through `tools/osv_scan.sh`
- scheduled guardrail CI
- OpenSSF Scorecard

If a vulnerability is reported in a dependency:

1. Determine whether the vulnerable code is present and reachable.
2. Update or patch the dependency when exploitable.
3. If not exploitable, document the reason in the narrowest available
   suppression, VEX note, or advisory note.
4. Add tests or checks when the issue could regress.

## Update Policy

Dependency updates should:

- preserve license notices
- avoid unrelated refactors
- update documentation when dependency behavior or requirements change
- run focused protocol, DSP, runtime, or packaging tests as applicable
- run `tools/osv_scan.sh`
- receive human review when they affect compiled code, workflows, packaging, or
  release behavior
