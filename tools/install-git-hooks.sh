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

echo "Done. Commits auto-format staged C/C++ files; pushes run local CI-style strict checks (CMake install runtime destinations, security guardrails including workflow source/download pins and vcpkg overlay pins, format, CMake format, clang-tidy, cppcheck, iwyu, fanalyzer, semgrep, zizmor, OSV scan, shell/workflow lint, lizard) on changed paths."
echo "Tip: run tools/preflight_ci.sh to execute the same local quality gates without pushing."
echo "Tip: set DSD_HOOK_RUN_SCAN_BUILD=1 for the heavier full scan-build pass."
