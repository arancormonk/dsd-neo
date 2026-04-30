#!/usr/bin/env bash
set -euo pipefail

# Compute the changed-file target sets used by pull-request CI.
# This mirrors the local pre-push targeting rules: changed source files are
# analyzed directly, and changed headers expand to a small set of including TUs.

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat <<'USAGE'
Usage: tools/ci_changed_files.sh --base <ref-or-sha> [--head <ref-or-sha>] [--out-dir <dir>]

Writes newline-delimited target files for CI jobs:
  changed_paths.txt
  format_files.txt
  analysis_tus.txt
  cppcheck_sources.txt
  semgrep_targets.txt

Options:
  --base REF      Base ref/SHA for the PR diff.
  --head REF      Head ref/SHA for the PR diff (default: HEAD).
  --out-dir DIR   Output directory (default: .ci/changed-files).
  --no-header-expansion
                  Do not expand changed headers to including translation units.
  -h, --help      Show this help.
USAGE
}

BASE_REF=""
HEAD_REF="HEAD"
OUT_DIR=".ci/changed-files"
MAX_TUS_PER_HEADER=5
EXPAND_HEADERS=1

while [[ $# -gt 0 ]]; do
  case "$1" in
    --base)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --base" >&2
        exit 2
      fi
      BASE_REF="$2"
      shift 2
      ;;
    --head)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --head" >&2
        exit 2
      fi
      HEAD_REF="$2"
      shift 2
      ;;
    --out-dir)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --out-dir" >&2
        exit 2
      fi
      OUT_DIR="$2"
      shift 2
      ;;
    --no-header-expansion)
      EXPAND_HEADERS=0
      shift
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

if [[ -z "$BASE_REF" ]]; then
  echo "Missing required --base ref/SHA" >&2
  usage >&2
  exit 2
fi

if ! git rev-parse --verify "$BASE_REF^{commit}" >/dev/null 2>&1; then
  echo "Unable to resolve base ref/SHA: $BASE_REF" >&2
  exit 2
fi
if ! git rev-parse --verify "$HEAD_REF^{commit}" >/dev/null 2>&1; then
  echo "Unable to resolve head ref/SHA: $HEAD_REF" >&2
  exit 2
fi

write_list() {
  local path="$1"
  shift
  mkdir -p "$(dirname "$path")"
  : > "$path"
  if [[ $# -gt 0 ]]; then
    printf '%s\n' "$@" > "$path"
  fi
}

sort_unique_array() {
  if [[ $# -eq 0 ]]; then
    return 0
  fi
  printf '%s\n' "$@" | sort -u
}

escape_rg() {
  printf '%s' "$1" | sed -e 's/[][(){}.^$*+?|\\]/\\&/g'
}

mkdir -p "$OUT_DIR"

mapfile -t changed_paths < <(
  git diff --name-only --diff-filter=ACMR "${BASE_REF}...${HEAD_REF}" || \
    git diff --name-only --diff-filter=ACMR "$BASE_REF" "$HEAD_REF"
)

if [[ ${#changed_paths[@]} -eq 0 ]]; then
  write_list "$OUT_DIR/changed_paths.txt"
  write_list "$OUT_DIR/format_files.txt"
  write_list "$OUT_DIR/analysis_tus.txt"
  write_list "$OUT_DIR/cppcheck_sources.txt"
  write_list "$OUT_DIR/semgrep_targets.txt"
else
  mapfile -t changed_paths < <(sort_unique_array "${changed_paths[@]}")
fi

changed_sources=()
changed_headers=()
format_files=()
semgrep_targets=()

for p in "${changed_paths[@]}"; do
  case "$p" in
    build/*|src/third_party/*) continue ;;
  esac

  if [[ ! -e "$p" && ! -L "$p" ]]; then
    continue
  fi

  case "$p" in
    src/*|include/*|apps/*|tests/*|tools/*) semgrep_targets+=("$p") ;;
  esac

  case "$p" in
    *.c|*.cc|*.cpp|*.cxx)
      changed_sources+=("$p")
      format_files+=("$p")
      ;;
    *.h|*.hh|*.hpp|*.hxx)
      changed_headers+=("$p")
      format_files+=("$p")
      ;;
  esac
done

analysis_tus=("${changed_sources[@]}")
if [[ $EXPAND_HEADERS -eq 1 && ${#changed_headers[@]} -gt 0 ]]; then
  if command -v rg >/dev/null 2>&1; then
    for hdr in "${changed_headers[@]}"; do
      include_key=""
      pattern=""
      if [[ "$hdr" == include/* ]]; then
        include_key="${hdr#include/}"
        include_key=$(escape_rg "$include_key")
        pattern="^[[:space:]]*#[[:space:]]*include[[:space:]]*[<\\\"]${include_key}[>\\\"]"
      else
        include_key=$(basename "$hdr")
        include_key=$(escape_rg "$include_key")
        pattern="^[[:space:]]*#[[:space:]]*include[[:space:]]*[<\\\"][^>\\\"]*${include_key}[>\\\"]"
      fi

      mapfile -t matches < <(
        rg -l --glob '!src/third_party/**' \
          -g'*.c' -g'*.cc' -g'*.cpp' -g'*.cxx' \
          "$pattern" src apps tests examples 2>/dev/null \
          | head -n "$MAX_TUS_PER_HEADER" || true
      )
      analysis_tus+=("${matches[@]}")
    done
  else
    echo "ci-changed-files: rg not found; header include expansion skipped." >&2
  fi
fi

cppcheck_sources=()
for p in "${analysis_tus[@]}"; do
  case "$p" in
    src/*) cppcheck_sources+=("$p") ;;
  esac
done

if [[ ${#format_files[@]} -gt 0 ]]; then
  mapfile -t format_files < <(sort_unique_array "${format_files[@]}")
fi
if [[ ${#semgrep_targets[@]} -gt 0 ]]; then
  mapfile -t semgrep_targets < <(sort_unique_array "${semgrep_targets[@]}")
fi
if [[ ${#analysis_tus[@]} -gt 0 ]]; then
  mapfile -t analysis_tus < <(sort_unique_array "${analysis_tus[@]}")
fi
if [[ ${#cppcheck_sources[@]} -gt 0 ]]; then
  mapfile -t cppcheck_sources < <(sort_unique_array "${cppcheck_sources[@]}")
fi

write_list "$OUT_DIR/changed_paths.txt" "${changed_paths[@]}"
write_list "$OUT_DIR/format_files.txt" "${format_files[@]}"
write_list "$OUT_DIR/analysis_tus.txt" "${analysis_tus[@]}"
write_list "$OUT_DIR/cppcheck_sources.txt" "${cppcheck_sources[@]}"
write_list "$OUT_DIR/semgrep_targets.txt" "${semgrep_targets[@]}"

echo "ci-changed-files: base=${BASE_REF} head=${HEAD_REF}"
echo "ci-changed-files: changed=${#changed_paths[@]} format=${#format_files[@]}" \
  "tus=${#analysis_tus[@]} cppcheck=${#cppcheck_sources[@]} semgrep=${#semgrep_targets[@]}"

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  {
    echo "out_dir=$OUT_DIR"
    echo "changed_paths=${#changed_paths[@]}"
    echo "format_files=${#format_files[@]}"
    echo "analysis_tus=${#analysis_tus[@]}"
    echo "cppcheck_sources=${#cppcheck_sources[@]}"
    echo "semgrep_targets=${#semgrep_targets[@]}"
  } >> "$GITHUB_OUTPUT"
fi
