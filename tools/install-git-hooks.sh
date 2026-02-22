#!/usr/bin/env bash
set -euo pipefail

# Install repo-provided Git hooks by pointing core.hooksPath at .githooks

repo_root=$(cd "$(dirname "$0")/.." && pwd)

git -C "$repo_root" config core.hooksPath .githooks
echo "Configured core.hooksPath to .githooks"

hooks_dir="$repo_root/.githooks"
if [[ -d "$hooks_dir" ]]; then
  shopt -s nullglob
  for hook in "$hooks_dir"/*; do
    if [[ -f "$hook" ]]; then
      chmod +x "$hook"
      echo "Enabled $(basename "$hook") hook."
    fi
  done
  shopt -u nullglob
fi

echo "Done. Commits auto-format staged C/C++ files; pushes run local CI-style checks (format, clang-tidy, cppcheck, iwyu, fanalyzer, semgrep, optional scan-build) on changed paths."
echo "Tip: run tools/preflight_ci.sh to execute the same pre-push checks manually."
