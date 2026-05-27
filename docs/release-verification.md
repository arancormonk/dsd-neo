# Release Verification

Official releases are published through GitHub Releases:

https://github.com/arancormonk/dsd-neo/releases

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

Verify the signature with the trusted DSD-neo release key checked into the repository:

```sh
gpg --import release-keys/arancormonk-2026.pgp
git tag -v vX.Y.Z
```

The release workflow pins this exact primary key fingerprint before trusting the
checked-in key file:

```text
5FAF 0C47 C8E1 F95D 33CD 83B1 E42E 43AD D853 F280
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
and Windows ZIP assets when the corresponding workflow completes successfully.
Release publication jobs use `contents: write` only in trusted upstream
release/nightly paths and publish with the workflow `GITHUB_TOKEN`.
Packaging workflows also verify release hardening before upload: Linux ELF
PIE/RELRO/BIND_NOW, macOS Mach-O PIE and staged dylib `@rpath` install names,
and Windows PE ASLR/NX/high-entropy virtual addresses.

Download assets from the release page, then verify an artifact attestation with
GitHub CLI when available:

```sh
gh attestation verify dsd-neo-<asset-name> --repo arancormonk/dsd-neo
```

Review the matching SPDX SBOM file when present:

```sh
less dsd-neo-<asset-name>.spdx.json
```

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
