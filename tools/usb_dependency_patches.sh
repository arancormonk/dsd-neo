#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The patches dsd-neo's desktop releases apply to libairspy and librtlsdr. They
# are the Android app's patch files (android/third_party/patches), so both
# consumers build the same change: libairspy's stop waits for every cancelled
# USB transfer, and librtlsdr's async reader accounts for every transfer (with
# 0002, which 0003 builds on). librtlsdr's 0001 adds an Android-only open path
# and stays out of the desktop series.
#
# The Windows vcpkg overlays list the same series and its digest in their
# portfiles; tools/check_vcpkg_overlay_pins.sh keeps them in step with this file.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
patch_dir="$repo_root/android/third_party/patches"

usage() {
  cat << 'USAGE'
Usage: tools/usb_dependency_patches.sh series <libairspy|librtlsdr>
       tools/usb_dependency_patches.sh digest <libairspy|librtlsdr>
       tools/usb_dependency_patches.sh apply <libairspy|librtlsdr> <source-dir>
       tools/usb_dependency_patches.sh fetch <libairspy|librtlsdr> <dest-dir>
       tools/usb_dependency_patches.sh fetch-all <dest-dir>

series     prints the patch file names, in order.
digest     prints the SHA-256 of the series' patch files concatenated in order.
apply      applies the series to a pinned upstream checkout (the top of a Git
           work tree, as tools/fetch-pinned-git.sh leaves it); any context
           mismatch fails.
fetch      checks out the pinned upstream source and applies the series.
fetch-all  fetches both into <dest-dir>/libairspy and <dest-dir>/librtlsdr,
           the layout DSD_TEST_DESKTOP_USB_SOURCES expects.
USAGE
}

series() {
  case "$1" in
    libairspy)
      echo 0001-libairspy-wait-for-cancelled-transfers.patch
      ;;
    librtlsdr)
      echo 0002-librtlsdr-check-async-buffer-allocation.patch
      echo 0003-librtlsdr-account-for-every-transfer.patch
      ;;
    *)
      echo "unknown library: $1 (expected libairspy or librtlsdr)" >&2
      return 2
      ;;
  esac
}

sha256_stdin() {
  if command -v sha256sum > /dev/null 2>&1; then
    sha256sum | cut -d' ' -f1
  else
    shasum -a 256 | cut -d' ' -f1
  fi
}

digest() {
  local names name
  names=$(series "$1")
  for name in $names; do
    cat "$patch_dir/$name"
  done | sha256_stdin
}

apply_series() {
  local lib="$1" dir="$2" names name top
  names=$(series "$lib")
  if ! top=$(git -C "$dir" rev-parse --show-toplevel 2> /dev/null) ||
    [[ "$(cd "$top" && pwd -P)" != "$(cd "$dir" && pwd -P)" ]]; then
    # git apply run below a work tree's top skips paths outside it silently.
    echo "$dir is not the top of a Git work tree" >&2
    return 1
  fi
  for name in $names; do
    git -C "$dir" apply --check "$patch_dir/$name"
    git -C "$dir" apply "$patch_dir/$name"
    echo "Applied $name to $dir"
  done
}

fetch() {
  local lib="$1" dest="$2"
  # shellcheck source=tools/ci-dependency-pins.env
  # shellcheck disable=SC1091
  source "$repo_root/tools/ci-dependency-pins.env"
  case "$lib" in
    libairspy)
      "$repo_root/tools/fetch-pinned-git.sh" https://github.com/airspy/airspyone_host "$AIRSPY_SHA" "$dest"
      ;;
    librtlsdr)
      "$repo_root/tools/fetch-pinned-git.sh" https://github.com/arancormonk/rtl-sdr "$RTL_SDR_SHA" "$dest"
      ;;
    *)
      series "$lib" > /dev/null
      ;;
  esac
  apply_series "$lib" "$dest"
}

command=${1:-}
case "$command" in
  series | digest)
    [[ $# -eq 2 ]] || {
      usage >&2
      exit 2
    }
    "$command" "$2"
    ;;
  apply)
    [[ $# -eq 3 ]] || {
      usage >&2
      exit 2
    }
    apply_series "$2" "$3"
    ;;
  fetch)
    [[ $# -eq 3 ]] || {
      usage >&2
      exit 2
    }
    fetch "$2" "$3"
    ;;
  fetch-all)
    [[ $# -eq 2 ]] || {
      usage >&2
      exit 2
    }
    fetch libairspy "$2/libairspy"
    fetch librtlsdr "$2/librtlsdr"
    ;;
  -h | --help)
    usage
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
