#!/usr/bin/env bash
set -euo pipefail

tag="${RELEASE_TAG:-${GITHUB_REF_NAME:-}}"
repo="${RELEASE_REPOSITORY:-${GITHUB_REPOSITORY:-}}"
title="${RELEASE_TITLE:-}"
auth_token="${GH_TOKEN:-${GITHUB_TOKEN:-}}"

if [[ -z "${tag}" ]]; then
  echo "RELEASE_TAG or GITHUB_REF_NAME is required." >&2
  exit 2
fi
if [[ -z "${repo}" ]]; then
  echo "RELEASE_REPOSITORY or GITHUB_REPOSITORY is required." >&2
  exit 2
fi
if [[ -z "${title}" ]]; then
  echo "RELEASE_TITLE is required." >&2
  exit 2
fi
if [[ -z "${auth_token}" ]]; then
  echo "GH_TOKEN or GITHUB_TOKEN is required." >&2
  exit 2
fi
if ! command -v gh > /dev/null 2>&1; then
  echo "GitHub CLI (gh) is required to create release metadata." >&2
  exit 2
fi

export GH_TOKEN="${auth_token}"

if gh release view "${tag}" --repo "${repo}" > /dev/null 2>&1; then
  echo "Release ${tag} already exists; leaving existing release notes unchanged."
  exit 0
fi

set +e
create_output="$(
  gh release create "${tag}" \
    --repo "${repo}" \
    --title "${title}" \
    --generate-notes \
    --verify-tag 2>&1
)"
create_rc=$?
set -e

if [[ ${create_rc} -eq 0 ]]; then
  printf '%s\n' "${create_output}"
  echo "Created release metadata for ${tag}."
  exit 0
fi

if gh release view "${tag}" --repo "${repo}" > /dev/null 2>&1; then
  echo "Release ${tag} was created concurrently; leaving existing release notes unchanged."
  exit 0
fi

printf '%s\n' "${create_output}" >&2
exit "${create_rc}"
