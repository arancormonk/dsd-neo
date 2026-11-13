# Release Verification

Official releases are published through GitHub Releases:

https://github.com/arancormonk/dsd-neo/releases

The `release` workflow owns release metadata for version tags and creates the
generated release notes exactly once. Platform packaging workflows wait for that
release to exist, then upload their assets without rewriting the release body.

The [licensing review](../legal/status.md) records unresolved component,
fixture and corresponding-source findings. Artifact attestations authenticate
build provenance; they do not certify legal clearance. Current automatic Git
source archives do not include all fetched or bundled dependency sources.

## Release Identifiers

Release tags use the form `vX.Y.Z`. The tag must match the project version
declared by CMake and must be an annotated tag signed by the trusted DSD-neo
release key, and must point at a commit already contained in `origin/main`, or
the release workflow fails before packaging.

Nightly assets are published from the moving `nightly` tag on trusted upstream
main-branch builds. They are intended for testing rather than stable
deployment.

## Source Tag Verification

Fetch release tags from the authoritative repository:

```sh
git fetch --tags https://github.com/arancormonk/dsd-neo.git
```

Inspect the tag and commit:

```sh
git show --show-signature vX.Y.Z
```

Verify the signature with the trusted DSD-neo release keys checked into the repository:

```sh
gpg --import release-keys/arancormonk-desktop-2026.pgp release-keys/arancormonk-laptop-2026.pgp
git tag -v vX.Y.Z
```

The release workflow pins these exact primary key fingerprints before trusting the
checked-in key files:

```text
5FAF 0C47 C8E1 F95D 33CD 83B1 E42E 43AD D853 F280  (desktop)
3561 9AC5 0DF5 FB9E A053 296E 5C77 FAD4 4C3E 67A7  (laptop)
```

`DSD_Author.pgp` is retained only for upstream attribution and is not a
DSD-neo release-signing trust root.

GitHub repository rulesets should also protect `v*.*.*` tags, restrict release
tag creation to the maintainer/admin role, block tag deletion and force updates,
and require signed tags where the GitHub ruleset UI supports it. Workflow
verification remains mandatory even when repository rulesets are enabled.

## Asset Integrity and Provenance

Release assets are distributed over GitHub HTTPS. Tag release workflows generate
SBOMs and GitHub artifact attestations for packaged Linux AppImage, macOS DMG,
Windows ZIP, and Android APK assets when the corresponding workflow completes
successfully. The Android App Bundle handed to the Play Console is a workflow
artifact rather than a release asset, but a tag attests it the same way — verify
it with `gh attestation verify` (below) before uploading it to a Play track.
Release publication jobs use `contents: write` only in trusted upstream
release/nightly paths and publish with the workflow `GITHUB_TOKEN`.
Packaging workflows also verify release hardening before upload: Linux ELF
PIE/RELRO/BIND_NOW, macOS Mach-O PIE and staged dylib `@rpath` install names,
and Windows PE ASLR/NX/high-entropy virtual addresses. The Android APK is signed
with the project release key before upload; an unsigned APK is never published.
Confirm the signer of a downloaded APK against the fingerprint printed by the
`android-ci` run that published it, and check that it does not change between
releases:

```sh
apksigner verify --print-certs dsd-neo-android-arm64-app-<version>.apk
```

Download assets from the release page, then verify an artifact attestation with
GitHub CLI when available:

```sh
gh attestation verify dsd-neo-<asset-name> --repo arancormonk/dsd-neo
```

Review the matching SPDX SBOM file when present:

```sh
less dsd-neo-<asset-name>.spdx.json
```

The curated component set supplements the scanner and keeps unresolved licenses
and the aggregate conclusion at `NOASSERTION`. Read the embedded findings and
the packaged `legal/` and `licenses/mbelib-neo/` records. `filesAnalyzed: false`
on these supplemental components means they are review records, not a claim
that the scanner identified every embedded file or all runtime dependencies.

Each entry in the SBOM's `files` list names a file the asset ships: its
`fileName` is the path inside the asset (such as `bin/dsd-neo.exe` in the
Windows ZIP), or the asset's own file name for a file the scanner records whole
(the AppImage). Its `checksums` are the SHA1 and SHA256 of that file's bytes.
The packaging job checks every record against the asset before it uploads or
attests the SBOM, and fails instead of publishing a record it cannot tie to the
asset. The macOS DMG and Android APK SBOMs currently list no file records. To
check a shipped file against its record:

```sh
unzip -p dsd-neo-msvc-x86_64-native-<version>.zip bin/dsd-neo.exe | sha256sum
jq -r '.files[] | select(.fileName == "bin/dsd-neo.exe") | .checksums[] |
  select(.algorithm == "SHA256") | .checksumValue' dsd-neo-msvc-x86_64-native-<version>.zip.spdx.json
```

For the AppImage, compare `sha256sum` of the AppImage with the SHA256 of its one
file record. SBOMs published up to and including v2.12.1 keep the scanner's
placeholders: their file records carry an all-zero SHA1, and the Windows ZIP's
records name the build machine's temporary unpack folder instead of the path in
the ZIP. Published assets are never rewritten, so check files from those
releases against the asset's attestation or SHA256 digest instead.

Workflow concurrency distinguishes push, PR and dispatch events. Jobs following
the PR-only classifier explicitly require successful builds and `!cancelled()`
so tag runs are not skipped merely because that classifier did not run. DSD has
no mbelib-style validated manual release-recovery mode: dispatching a branch
does not authorize publishing a version tag or replacing its source identity.

For manually mirrored assets, compute and compare a local SHA256 digest:

```sh
sha256sum dsd-neo-<asset-name>
```

On macOS, use:

```sh
shasum -a 256 dsd-neo-<asset-name>
```

## Expected Release Author

The expected release author is the project maintainer, `arancormonk`, through
the GitHub Actions release workflows in this repository.

## Release Review

Before publication, release packaging must pass the required CI checks for the
tag, including signed-tag validation, build, test, static-analysis, sanitizer,
install/package, license-file, SBOM, attestation, and release validation jobs.
