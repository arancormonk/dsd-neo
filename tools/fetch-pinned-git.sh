#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 <repo-url> <commit-sha> <destination>" >&2
  exit 2
fi

repo_url=$1
want_sha=$2
dest=$3

if [[ ! "$want_sha" =~ ^[0-9a-fA-F]{40}$ ]]; then
  echo "invalid pinned SHA: $want_sha" >&2
  exit 2
fi

rm -rf "$dest"
mkdir -p "$(dirname "$dest")"
git init "$dest"
git -C "$dest" remote add origin "$repo_url"
git -C "$dest" fetch --depth 1 origin "$want_sha"
git -C "$dest" checkout --detach "$want_sha"

actual_sha=$(git -C "$dest" rev-parse HEAD)
if [[ "$actual_sha" != "$want_sha" ]]; then
  echo "pinned checkout mismatch for $repo_url: expected $want_sha, got $actual_sha" >&2
  exit 1
fi
