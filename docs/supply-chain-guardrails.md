# Supply-Chain Guardrails

DSD-neo keeps release and CI dependencies explicit so analyzer results and binary artifacts are reproducible.

## GitHub Actions

- Workflow security is checked with `tools/workflow_lint.sh`, Semgrep workflow rules, CodeQL Actions analysis, Gitleaks, OpenSSF Scorecard, and `tools/zizmor.sh`.
- The current zizmor policy in `.github/zizmor.yml` requires every external `uses:` action to be pinned to a full commit SHA.
- For new or refreshed third-party actions in release, packaging, signing, attestation, or upload workflows, resolve the upstream tag to the immutable commit and pin that SHA.
- Public GitHub source checkouts in CI must go through `tools/fetch-pinned-git.sh` with SHAs from `tools/ci-dependency-pins.env`; `tools/check_workflow_git_pins.sh` blocks floating `git clone` and `git ls-remote` usage in workflows and CI helper scripts.
- Release and packaging workflows must not execute mutable downloaded helper binaries. AppImage helper tools are built from pinned source SHAs, AppImage and CI fallback container images are pinned by digest, and installer downloads must be SHA256-verified before execution. `tools/check_workflow_download_pins.sh` enforces these rules for workflow and CI helper images.
- Workflow changes that add secrets, write permissions, artifact publication, release upload, or external actions need human review.

## Pinned CI Source Checkouts

`tools/ci-dependency-pins.env` is the checked-in source of truth for CI-only GitHub source dependencies such as `mbelib-neo`, `codec2`, `rtl-sdr`, `include-what-you-use`, AppImage helper projects, CI container digests, and installer SHA256 values.

To refresh one of these pins:

```bash
repo=https://github.com/arancormonk/mbelib-neo
git ls-remote "$repo" HEAD
```

Update the matching SHA in `tools/ci-dependency-pins.env`, then run `tools/check_workflow_git_pins.sh`, `tools/check_workflow_download_pins.sh`, and the affected CI/local build path. For `mbelib-neo` and `codec2`, keep the CI pin aligned with the vcpkg overlay `REF` unless there is a documented reason to test a different commit.

For AppImage helper projects, refresh the source SHA only after checking the upstream changes. Do not switch back to `releases/download/continuous` helper AppImages. If a container base image or CMake installer changes, update the digest or SHA256 in `tools/ci-dependency-pins.env` in the same change as the workflow update.

## AUR SSH Host Keys

The AUR update workflow currently discovers `aur.archlinux.org`'s SSH host key at runtime with `ssh-keyscan`, then uses `StrictHostKeyChecking=yes` against that discovered key. This prevents silent host-key changes after setup within the same job, but it is still trust-on-first-use for each run and remains vulnerable if the runner's network path is intercepted during key discovery.

The robust control is to store an out-of-band verified AUR ED25519 host key in this repository or in a GitHub Actions secret, write that exact key to `known_hosts`, and fail the job if the live host key differs. Refresh that pin only after verifying the new key through Arch/AUR-controlled channels outside the CI network path. Until that pin exists, treat AUR SSH host-key discovery as an accepted residual risk for the AUR publish jobs.

## vcpkg Overlay Ports

Overlay ports under `vcpkg-ports/` must use immutable `REF` plus `SHA512`, not `HEAD_REF`, for normal CI and release builds.

To refresh a pinned GitHub source:

```bash
repo=arancormonk/mbelib-neo
sha=$(git ls-remote "https://github.com/${repo}" refs/heads/main | cut -f1)
echo "$sha"
curl -fsSL "https://github.com/${repo}/archive/${sha}.tar.gz" | sha512sum
```

Update the matching `REF` and `SHA512` in the portfile, then run the affected vcpkg build or packaging workflow. Use the same process for `arancormonk/codec2`.

## Vulnerability Scanning

- `tools/osv_scan.sh` runs OSV-Scanner source scanning for lockfiles, manifests, and vendored C/C++ dependency fingerprints.
- PR CI runs OSV only when dependency inputs change; scheduled guardrails run a full repository scan.
- If OSV reports a vulnerability that is not exploitable in DSD-neo, add the narrowest possible `osv-scanner.toml` ignore with a reason and expiry date.
