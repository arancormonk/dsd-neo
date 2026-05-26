#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

project_version="$(
  sed -nE 's/^[[:space:]]*project\([[:space:]]*dsd-neo[[:space:]]+VERSION[[:space:]]+([0-9]+[.][0-9]+[.][0-9]+).*$/\1/p' \
    "${repo_root}/CMakeLists.txt" |
    head -n1
)"

if [[ -z "${project_version}" ]]; then
  echo "Failed to read dsd-neo project version from CMakeLists.txt." >&2
  exit 1
fi

ref="${GITHUB_REF:-}"
tag="${GITHUB_REF_NAME:-}"
event="${GITHUB_EVENT_NAME:-}"
created="${RELEASE_TAG_CREATED:-${GITHUB_EVENT_CREATED:-}}"

if [[ "${ref}" != refs/tags/v* ]]; then
  {
    echo "version="
    echo "archive_prefix="
    echo "release_title="
  } >> "${GITHUB_OUTPUT:-/dev/null}"
  echo "Not a release tag; project version is ${project_version}."
  exit 0
fi

if [[ ! "${tag}" =~ ^v[0-9]+[.][0-9]+[.][0-9]+$ ]]; then
  echo "Release tags must use vX.Y.Z format; got '${tag}'." >&2
  exit 1
fi

if [[ "${event}" != "push" || "${created}" != "true" ]]; then
  echo "Release publishing requires a newly created version tag push; refusing to publish ${tag}." >&2
  exit 1
fi

version="${tag#v}"
if [[ "${version}" != "${project_version}" ]]; then
  echo "Tag ${tag} does not match project version ${project_version}." >&2
  exit 1
fi

if ! command -v gpg > /dev/null 2>&1; then
  echo "gpg is required to verify release tag signatures." >&2
  exit 1
fi

if ! git rev-parse -q --verify "refs/tags/${tag}^{tag}" > /dev/null; then
  git fetch --force origin "refs/tags/${tag}:refs/tags/${tag}" > /dev/null 2>&1 || true
fi

if ! git rev-parse -q --verify "refs/tags/${tag}^{tag}" > /dev/null; then
  echo "Release tag ${tag} must be an annotated signed tag; lightweight tags are not allowed." >&2
  exit 1
fi

trusted_key="${repo_root}/release-keys/arancormonk-2026.pgp"
if [[ ! -f "${trusted_key}" ]]; then
  echo "Trusted release key not found: ${trusted_key}" >&2
  exit 1
fi

gnupg_home="$(mktemp -d)"
cleanup_gnupg_home() {
  rm -rf "${gnupg_home}"
}
trap cleanup_gnupg_home EXIT
chmod 700 "${gnupg_home}"

GNUPGHOME="${gnupg_home}" gpg --batch --import "${trusted_key}" > /dev/null
ownertrust="$(
  GNUPGHOME="${gnupg_home}" gpg --batch --with-colons --fingerprint --list-keys |
    awk -F: '$1 == "pub" { trust_next_fpr = 1; next } trust_next_fpr && $1 == "fpr" { print $10 ":6:"; trust_next_fpr = 0 }'
)"
if [[ -z "${ownertrust}" ]]; then
  echo "Trusted release key import did not produce any public key fingerprints." >&2
  exit 1
fi
printf '%s\n' "${ownertrust}" | GNUPGHOME="${gnupg_home}" gpg --batch --import-ownertrust > /dev/null
if ! GNUPGHOME="${gnupg_home}" git verify-tag "${tag}" > /dev/null; then
  echo "Release tag ${tag} is not signed by a trusted DSD-neo release key." >&2
  exit 1
fi

{
  echo "version=${project_version}"
  echo "archive_prefix=dsd-neo-${project_version}"
  echo "release_title=dsd-neo ${project_version}"
} >> "${GITHUB_OUTPUT:-/dev/null}"

echo "Validated release tag ${tag} for dsd-neo ${project_version}."
