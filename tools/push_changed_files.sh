#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

# Paths one pushed ref changes, for .githooks/pre-push (and tools/preflight_ci.sh,
# which runs it). The hook used to compute these inline with
# `git diff --name-only ... || true`, so a diff that failed (a remote SHA the
# clone does not have, say) read as "nothing changed" and the hook exited without
# checking anything, and git's quoting of a name with non-ASCII bytes, quotes or
# tabs kept that file out of every check.

if ((BASH_VERSINFO[0] < 4 || (BASH_VERSINFO[0] == 4 && BASH_VERSINFO[1] < 4))); then
  echo "push-changed-files: needs bash 4.4 or later (this is ${BASH_VERSION})" >&2
  exit 2
fi

usage() {
  cat << 'USAGE'
Usage: tools/push_changed_files.sh <remote-name> <local-ref> <local-sha> <remote-sha>

Writes the paths the push of <local-sha> adds, copies, modifies or renames
(--diff-filter=ACMR), NUL-separated and unquoted, to stdout. The arguments are
one line of a pre-push hook's input, plus the remote's name.

- An existing ref is compared with what the remote has (<remote-sha>...<local-sha>).
- A new ref, or one whose <remote-sha> this clone does not have, is compared with
  the remote's default branch, else with the nearest merge base among the remote's
  branches, else with the empty tree (every file).

Exits non-zero, with a message, when no comparison can be made.
USAGE
}

# Help only as the sole argument: git allows a remote named -h or --help, and the
# hook passes the remote's name first.
if [[ $# -eq 1 && ("$1" == "-h" || "$1" == "--help") ]]; then
  usage
  exit 0
fi
if [[ $# -ne 4 ]]; then
  usage >&2
  exit 2
fi

remote_name="$1"
local_ref="$2"
local_sha="$3"
remote_sha="$4"
zeros="0000000000000000000000000000000000000000"

remote_head_ref=$(git symbolic-ref -q "refs/remotes/${remote_name}/HEAD" 2> /dev/null || true)
if [[ -z "$remote_head_ref" ]]; then
  if git show-ref --verify --quiet "refs/remotes/${remote_name}/main"; then
    remote_head_ref="refs/remotes/${remote_name}/main"
  elif git show-ref --verify --quiet "refs/remotes/${remote_name}/master"; then
    remote_head_ref="refs/remotes/${remote_name}/master"
  fi
fi

resolve_new_ref_base() {
  local base=""
  for base in \
    "$remote_head_ref" \
    "refs/remotes/${remote_name}/main" \
    "refs/remotes/${remote_name}/master"; do
    if [[ -n "$base" ]] && git show-ref --verify --quiet "$base"; then
      printf '%s\n' "$base"
      return 0
    fi
  done
  return 1
}

select_best_remote_merge_base() {
  local ref=""
  local merge_base=""
  local distance=""
  local best_base=""
  local best_distance=""
  local remote_tracking_refs=()

  mapfile -t remote_tracking_refs < <(
    git for-each-ref --format='%(refname)' "refs/remotes/${remote_name}" 2> /dev/null || true
  )
  for ref in "${remote_tracking_refs[@]}"; do
    merge_base=$(git merge-base "$local_sha" "$ref" 2> /dev/null || true)
    if [[ -z "$merge_base" ]]; then
      continue
    fi
    distance=$(git rev-list --count "${merge_base}..${local_sha}" 2> /dev/null || true)
    if [[ -z "$distance" ]]; then
      continue
    fi
    if [[ -z "$best_base" || "$distance" -lt "$best_distance" ]]; then
      best_base="$merge_base"
      best_distance="$distance"
    fi
  done

  if [[ -n "$best_base" ]]; then
    printf '%s\n' "$best_base"
    return 0
  fi
  return 1
}

diff_or_die() {
  if ! git diff -z --name-only --diff-filter=ACMR "$@"; then
    echo "push-changed-files: git diff $* failed for ${local_ref}" >&2
    exit 1
  fi
}

if [[ "$remote_sha" != "$zeros" ]] && git cat-file -e "${remote_sha}^{commit}" 2> /dev/null; then
  diff_or_die "${remote_sha}...${local_sha}"
  exit 0
fi

if [[ "$remote_sha" != "$zeros" ]]; then
  echo "push-changed-files: ${remote_sha} (the remote's ${local_ref}) is not in this clone; comparing with the remote's branches instead." >&2
fi

if base_ref=$(resolve_new_ref_base); then
  diff_or_die "${base_ref}...${local_sha}"
  exit 0
fi

if fallback_base=$(select_best_remote_merge_base); then
  echo "push-changed-files: using merge-base fallback for ${local_ref} (no remote default branch found)." >&2
  diff_or_die "${fallback_base}..${local_sha}"
  exit 0
fi

empty_tree=$(git hash-object -t tree /dev/null)
echo "push-changed-files: using empty-tree fallback for ${local_ref} (no remote default branch or merge-base found)." >&2
diff_or_die "${empty_tree}" "${local_sha}^{tree}"
