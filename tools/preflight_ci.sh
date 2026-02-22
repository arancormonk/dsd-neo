#!/usr/bin/env bash
set -euo pipefail

# Run the same checks as .githooks/pre-push without performing a push.
# Useful for catching CI failures locally before pushing.
# Also checks uncommitted (staged/unstaged/untracked) local changes.

usage() {
  cat <<'USAGE'
Usage: tools/preflight_ci.sh [--remote <name>] [--base <ref-or-sha>]

Runs pre-push checks for the push delta and current uncommitted changes.

Options:
  --remote NAME   Remote name to model push against (default: upstream remote, or origin).
  --base REF      Compare local HEAD against this ref/SHA (overrides upstream auto-detect).
  -h, --help      Show this help.

Environment:
  DSD_HOOK_RUN_SCAN_BUILD=1|0
    1 => run optional scan-build full rebuild (default: 0/off)
  DSD_HOOK_FAIL_ON_MISSING_TOOLS=1|0

Examples:
  tools/preflight_ci.sh
  tools/preflight_ci.sh --base origin/main
  DSD_HOOK_RUN_SCAN_BUILD=1 tools/preflight_ci.sh
USAGE
}

REMOTE_NAME=""
BASE_REF=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --remote)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --remote" >&2
        exit 2
      fi
      REMOTE_NAME="$2"
      shift 2
      ;;
    --base)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --base" >&2
        exit 2
      fi
      BASE_REF="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

repo_root=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$repo_root"

hook_path=".githooks/pre-push"
if [[ ! -x "$hook_path" ]]; then
  echo "Hook not found or not executable: $hook_path" >&2
  echo "Run tools/install-git-hooks.sh first." >&2
  exit 1
fi

zeros="0000000000000000000000000000000000000000"

local_ref=$(git symbolic-ref -q HEAD || echo "HEAD")
local_sha=$(git rev-parse HEAD)
branch_name=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "HEAD")
remote_ref="refs/heads/${branch_name}"
remote_sha="$zeros"

upstream_ref=$(git rev-parse --abbrev-ref --symbolic-full-name "@{upstream}" 2>/dev/null || true)

if [[ -n "$BASE_REF" ]]; then
  if ! remote_sha=$(git rev-parse "$BASE_REF" 2>/dev/null); then
    echo "Unable to resolve --base reference: $BASE_REF" >&2
    exit 2
  fi
  remote_ref="$BASE_REF"
fi

if [[ -z "$REMOTE_NAME" ]]; then
  if [[ -n "$upstream_ref" ]]; then
    REMOTE_NAME="${upstream_ref%%/*}"
  elif git remote get-url origin >/dev/null 2>&1; then
    REMOTE_NAME="origin"
  else
    REMOTE_NAME="$(git remote | head -n 1 || true)"
  fi
fi

if [[ -z "$REMOTE_NAME" ]]; then
  if [[ -z "$BASE_REF" ]]; then
    echo "No git remote found. Use --remote <name> or --base <ref-or-sha>." >&2
    exit 2
  fi
  # --base mode already has the comparison SHA, so a synthetic remote label is sufficient.
  REMOTE_NAME="origin"
fi

if [[ -z "$BASE_REF" && -n "$upstream_ref" ]]; then
  upstream_remote="${upstream_ref%%/*}"
  upstream_branch="${upstream_ref#*/}"
  if [[ "$upstream_remote" == "$REMOTE_NAME" ]]; then
    if remote_sha=$(git rev-parse "$upstream_ref" 2>/dev/null); then
      remote_ref="refs/heads/${upstream_branch}"
    fi
  fi
fi

remote_url=$(git remote get-url "$REMOTE_NAME" 2>/dev/null || true)

echo "preflight: local_ref=${local_ref} local_sha=${local_sha}"
echo "preflight: remote=${REMOTE_NAME} remote_ref=${remote_ref} remote_sha=${remote_sha}"

printf '%s %s %s %s\n' "$local_ref" "$local_sha" "$remote_ref" "$remote_sha" | "$hook_path" "$REMOTE_NAME" "$remote_url"

tmp_index="$(mktemp "${TMPDIR:-/tmp}/dsd-neo-preflight-index.XXXXXX")"
cleanup() {
  rm -f "$tmp_index"
}
trap cleanup EXIT

GIT_INDEX_FILE="$tmp_index" git read-tree "$local_sha"
GIT_INDEX_FILE="$tmp_index" git add -A -- .

worktree_tree=$(GIT_INDEX_FILE="$tmp_index" git write-tree)
head_tree=$(git rev-parse "${local_sha}^{tree}")

if [[ "$worktree_tree" != "$head_tree" ]]; then
  worktree_sha=$(
    printf 'preflight_ci synthetic worktree commit\n' | \
      GIT_AUTHOR_NAME=dsd-neo-preflight \
      GIT_AUTHOR_EMAIL=preflight@local \
      GIT_COMMITTER_NAME=dsd-neo-preflight \
      GIT_COMMITTER_EMAIL=preflight@local \
      git commit-tree "$worktree_tree" -p "$local_sha"
  )
  echo "preflight: running checks for uncommitted local changes"
  printf '%s %s %s %s\n' "$local_ref" "$worktree_sha" "$local_ref" "$local_sha" | "$hook_path" "$REMOTE_NAME" "$remote_url"
fi
