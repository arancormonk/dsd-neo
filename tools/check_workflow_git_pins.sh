#!/usr/bin/env bash
set -euo pipefail

repo_root=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$repo_root"

if ! command -v rg > /dev/null 2>&1; then
  echo "ripgrep is required for workflow Git pin guardrail checks." >&2
  exit 2
fi

violations=$(
  rg -n \
    -e 'git[[:space:]]+clone[^#\n]*(https://github\.com|\$repo|"\$repo")' \
    -e 'git[[:space:]]+ls-remote[^\n]*https://github\.com' \
    .github/workflows tools \
    --glob '!tools/fetch-pinned-git.sh' \
    --glob '!tools/check_workflow_git_pins.sh' |
    grep -Ev 'aur\.archlinux\.org' ||
    true
)

if [[ -n "$violations" ]]; then
  echo "Unpinned public GitHub checkout detected. Use tools/fetch-pinned-git.sh and tools/ci-dependency-pins.env:" >&2
  printf '%s\n' "$violations" >&2
  exit 1
fi
