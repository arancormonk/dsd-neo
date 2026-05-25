# Release Verification

Official releases are published through GitHub Releases:

https://github.com/arancormonk/dsd-neo/releases

## Release Identifiers

Release tags use the form `vX.Y.Z`. The tag must match the project version
declared by CMake, or the release workflow fails before packaging.

Nightly assets are published from the moving `nightly` tag and are intended for
testing rather than stable deployment.

## Source Tag Verification

Fetch release tags from the authoritative repository:

```sh
git fetch --tags https://github.com/arancormonk/dsd-neo.git
```

Inspect the tag and commit:

```sh
git show --show-signature vX.Y.Z
```

If the tag is signed, verify the signature with the maintainer public key
published by GitHub:

```sh
curl -fsSL https://github.com/arancormonk.gpg | gpg --import
git tag -v vX.Y.Z
```

## Asset Integrity and Provenance

Release assets are distributed over GitHub HTTPS. Tag release workflows generate
SBOMs and GitHub artifact attestations for packaged Linux AppImage, macOS DMG,
and Windows ZIP assets when the corresponding workflow completes successfully.

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
tag, including build, test, static-analysis, sanitizer, install/package,
license-file, SBOM, attestation, and release validation jobs.
