# OpenSSF OSPS Baseline Evidence

This document records DSD-neo's local evidence for OpenSSF OSPS Baseline Level
1. Repository-hosted settings such as branch protection and MFA must also remain
enabled in GitHub.

## Repository and Access Controls

- Sensitive repository resources are hosted on GitHub. Maintainers with access
  to those resources must keep multi-factor authentication or passkeys enabled
  on their GitHub accounts.
- New collaborators are added through GitHub repository permissions. Access is
  intentionally granted by the maintainer, and elevated access is not granted by
  default.
- The primary branch is `main`. GitHub branch protection is required for `main`,
  including required status checks before merge. Direct commits to `main` are
  blocked by the protected branch workflow. Required status checks include the
  repository guardrails job for secret redaction and workflow source/download
  pinning.
- `.github/CODEOWNERS` assigns ownership for the repository and
  security-sensitive paths. While DSD-neo has one maintainer, code-owner review
  routes ownership but is not independent second-party review evidence.
  Required status checks, release tag verification, and workflow guardrails are
  the enforceable controls for solo-maintainer changes.
- Deleting the protected `main` branch is disabled in GitHub branch protection.
- Version release tags matching `v*.*.*` are protected by a GitHub tag ruleset:
  deletion and force updates are blocked, and signed tags are required.
  Creation remains limited by repository write/admin permissions.

## Build, Release, and Secrets Controls

- CI/CD workflows must treat GitHub event metadata, branch names, issue titles,
  pull request fields, and other user-controlled values as untrusted input.
  Workflow scripts must pass untrusted values through action inputs or
  environment variables instead of directly interpolating them into shell
  scripts.
- Pull request workflows use least-privilege `GITHUB_TOKEN` permissions. The
  default GitHub Actions workflow permission for the repository is read-only,
  and workflows keep `GITHUB_TOKEN` read-only by default. GitHub Release and
  nightly publication elevate `contents: write` only in trusted upstream non-PR
  release/nightly paths and publish with the workflow `GITHUB_TOKEN`. AUR
  update credentials are likewise restricted to upstream non-PR AUR update
  paths.
- Official project channels and distribution channels are GitHub HTTPS URLs and
  AUR HTTPS URLs.
- The project prevents accidental credential storage through contributor policy,
  GitHub secret scanning, Gitleaks CI, and review of workflow and release
  changes.

## Documentation and Project Scope

- `README.md` documents basic usage, build, installation, CLI examples, release
  artifacts, project layout, and public headers.
- `docs/issue-reporting.md` documents defect reporting through GitHub Issues.
- GitHub Issues, pull requests, and Discussions are public mechanisms for
  proposed changes and usage obstacles.
- `CONTRIBUTING.md` documents the contribution process and acceptable
  contribution requirements.
- This project has one authoritative source repository:
  `https://github.com/arancormonk/dsd-neo`.

## Licensing and Repository Contents

- Source code is licensed under GPL-3.0-or-later. Third-party license notices
  are retained in `THIRD_PARTY.md`, `COPYRIGHT`, and vendored source paths.
- Release install trees include `LICENSE`, `COPYRIGHT`, `THIRD_PARTY.md`, and
  required third-party license texts.
- The public Git repository records changes, authors, and timestamps.
- Generated executable artifacts, compiled binaries, release archives, private
  captures, credentials, radio keys, and machine-specific configuration must not
  be committed.
- Tracked non-source binary assets are limited to reviewable project images and
  a public upstream attribution PGP key; generated executables and release
  packages are not stored in the source repository.

## Dependency and Vulnerability Contacts

- Direct compiled and tooling dependencies are documented in
  `docs/dependencies.md`, CMake metadata, `.github/requirements/`, workflow
  files, vcpkg overlay metadata, and Dependabot configuration.
- Security contacts and private vulnerability reporting are documented in
  `SECURITY.md`.
