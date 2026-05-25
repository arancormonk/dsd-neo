# Supply-Chain Guardrails

DSD-neo keeps release and CI dependencies explicit so analyzer results and binary artifacts are reproducible.

## GitHub Actions

- Workflow security is checked with `tools/workflow_lint.sh`, Semgrep workflow rules, CodeQL Actions analysis, Gitleaks, OpenSSF Scorecard, and `tools/zizmor.sh`.
- The current zizmor policy in `.github/zizmor.yml` requires every external `uses:` action to be pinned to a full commit SHA.
- For new or refreshed third-party actions in release, packaging, signing, attestation, or upload workflows, resolve the upstream tag to the immutable commit and pin that SHA.
- Workflow changes that add secrets, write permissions, artifact publication, release upload, or external actions need human review.

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
