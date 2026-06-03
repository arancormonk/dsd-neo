#!/usr/bin/env bash
set -euo pipefail

tag="${RELEASE_TAG:-${GITHUB_REF_NAME:-}}"
repo="${RELEASE_REPOSITORY:-${GITHUB_REPOSITORY:-}}"
auth_token="${GH_TOKEN:-${GITHUB_TOKEN:-}}"
timeout_seconds="${RELEASE_WAIT_TIMEOUT_SECONDS:-600}"
interval_seconds="${RELEASE_WAIT_INTERVAL_SECONDS:-5}"

if [[ -z "${tag}" ]]; then
  echo "RELEASE_TAG or GITHUB_REF_NAME is required." >&2
  exit 2
fi
if [[ -z "${repo}" ]]; then
  echo "RELEASE_REPOSITORY or GITHUB_REPOSITORY is required." >&2
  exit 2
fi
if [[ -z "${auth_token}" ]]; then
  echo "GH_TOKEN or GITHUB_TOKEN is required." >&2
  exit 2
fi
if [[ ! "${timeout_seconds}" =~ ^[0-9]+$ ]] || [[ "${timeout_seconds}" -le 0 ]]; then
  echo "RELEASE_WAIT_TIMEOUT_SECONDS must be a positive integer." >&2
  exit 2
fi
if [[ ! "${interval_seconds}" =~ ^[0-9]+$ ]] || [[ "${interval_seconds}" -le 0 ]]; then
  echo "RELEASE_WAIT_INTERVAL_SECONDS must be a positive integer." >&2
  exit 2
fi
if ! command -v gh > /dev/null 2>&1; then
  echo "GitHub CLI (gh) is required to wait for release metadata." >&2
  exit 2
fi

export GH_TOKEN="${auth_token}"

deadline=$((SECONDS + timeout_seconds))
while ((SECONDS <= deadline)); do
  if gh release view "${tag}" --repo "${repo}" > /dev/null 2>&1; then
    echo "Release ${tag} is available."
    exit 0
  fi

  if ((SECONDS + interval_seconds > deadline)); then
    break
  fi
  sleep "${interval_seconds}"
done

echo "Timed out waiting for release ${tag} in ${repo}." >&2
exit 1
