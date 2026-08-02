#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Verify that Android ELF payloads are arm64 and use 16 KB page alignment.
#
# Google Play requires every native library in an app targeting SDK 35+ to load
# on devices with 16 KB pages, which means every PT_LOAD segment must be aligned
# to at least 0x4000. NDK r28 and newer do this by default; older toolchains need
# -Wl,-z,max-page-size=16384. Run this against the app library, the headless CLI
# and any bundled .so before shipping.
#
# Usage: tools/check_android_elf_alignment.sh <file> [file...]

set -euo pipefail

min_align=$((16 * 1024))

usage() {
  echo "Usage: ${0##*/} <elf-file> [elf-file...]" >&2
}

find_readelf() {
  local ndk_root candidate
  for ndk_root in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}"; do
    [[ -n "$ndk_root" ]] || continue
    for candidate in "$ndk_root"/toolchains/llvm/prebuilt/*/bin/llvm-readelf; do
      if [[ -x "$candidate" ]]; then
        printf '%s\n' "$candidate"
        return 0
      fi
    done
  done
  for candidate in llvm-readelf readelf; do
    if command -v "$candidate" > /dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  done
  return 1
}

if [[ $# -eq 0 ]]; then
  usage
  exit 2
fi

readelf_bin=$(find_readelf) || {
  echo "llvm-readelf or readelf is required for Android ELF alignment checks." >&2
  exit 2
}

failed=0

check_file() {
  local file=$1
  local headers machine load_lines align_hex align

  if [[ ! -f "$file" ]]; then
    echo "MISSING: $file" >&2
    failed=1
    return
  fi

  headers=$("$readelf_bin" -hW "$file")
  machine=$(printf '%s\n' "$headers" | awk -F: '/Machine:/ {gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
  if [[ "$machine" != *"AArch64"* ]]; then
    echo "FAIL: $file is not AArch64 (Machine: $machine)" >&2
    failed=1
    return
  fi

  load_lines=$("$readelf_bin" -lW "$file" | awk '$1 == "LOAD" {print $NF}')
  if [[ -z "$load_lines" ]]; then
    echo "FAIL: $file has no PT_LOAD segments" >&2
    failed=1
    return
  fi

  while IFS= read -r align_hex; do
    [[ -n "$align_hex" ]] || continue
    align=$((align_hex))
    if ((align < min_align)); then
      echo "FAIL: $file has a LOAD segment aligned to $align_hex (need >= 0x4000)" >&2
      failed=1
      return
    fi
  done <<< "$load_lines"

  echo "OK: $file (AArch64, every LOAD segment aligned to >= 16 KB)"
}

for target in "$@"; do
  check_file "$target"
done

exit "$failed"
