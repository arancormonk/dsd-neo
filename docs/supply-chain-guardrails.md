# Supply-Chain Guardrails

DSD-neo keeps release and CI dependencies explicit so analyzer results and binary artifacts are reproducible.

## GitHub Actions

- Workflow security is checked with `tools/workflow_lint.sh`, Semgrep workflow rules, CodeQL Actions analysis, Gitleaks, OpenSSF Scorecard, and `tools/zizmor.sh`.
- The current zizmor policy in `.github/zizmor.yml` requires every external `uses:` action to be pinned to a full commit SHA.
- For new or refreshed third-party actions in release, packaging, signing, attestation, or upload workflows, resolve the upstream tag to the immutable commit and pin that SHA.
- Public GitHub source checkouts in CI must go through `tools/fetch-pinned-git.sh` with SHAs from `tools/ci-dependency-pins.env`; `tools/check_workflow_git_pins.sh` blocks floating `git clone` and `git ls-remote` usage in workflows and CI helper scripts.
- Workflow changes that add secrets, write permissions, artifact publication, release upload, or external actions need human review.

## Pinned CI Source Checkouts

`tools/ci-dependency-pins.env` is the checked-in source of truth for CI-only GitHub source dependencies such as `mbelib-neo`, `codec2`, `rtl-sdr`, `include-what-you-use`, and AppImage helper projects.

To refresh one of these pins:

```bash
repo=https://github.com/arancormonk/mbelib-neo
git ls-remote "$repo" HEAD
```

Update the matching SHA in `tools/ci-dependency-pins.env`, then run `tools/check_workflow_git_pins.sh` and the affected CI/local build path. For `mbelib-neo` and `codec2`, keep the CI pin aligned with the vcpkg overlay `REF` unless there is a documented reason to test a different commit.

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
