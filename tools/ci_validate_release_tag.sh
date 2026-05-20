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

{
  echo "version=${project_version}"
  echo "archive_prefix=dsd-neo-${project_version}"
  echo "release_title=dsd-neo ${project_version}"
} >> "${GITHUB_OUTPUT:-/dev/null}"

echo "Validated release tag ${tag} for dsd-neo ${project_version}."
