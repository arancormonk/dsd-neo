#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Checks that a libairspy and a librtlsdr binary (Linux .so or macOS .dylib)
# were built from sources carrying dsd-neo's desktop patches
# (tools/usb_dependency_patches.sh). Neither patch exports a symbol, so each
# probe is something only the patched source puts in the binary:
# - libairspy: the stop's wait for cancelled transfers backs off with usleep(),
#   which upstream libairspy never calls at AIRSPY_SHA, so the library imports
#   it. Recheck that when the pin moves (TOOLS_AIRSPY_PACKAGING expects the
#   pinned upstream source to fail this probe).
# - librtlsdr: the reader reports "no transfer left" when it has run out of
#   transfers.
set -euo pipefail

usage() {
  echo "Usage: tools/check_patched_usb_libs.sh --airspy <libairspy> --rtlsdr <librtlsdr>" >&2
  exit 2
}

airspy=""
rtlsdr=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --airspy)
      [[ $# -ge 2 ]] || usage
      airspy="$2"
      shift 2
      ;;
    --rtlsdr)
      [[ $# -ge 2 ]] || usage
      rtlsdr="$2"
      shift 2
      ;;
    *) usage ;;
  esac
done
[[ -n "$airspy" && -n "$rtlsdr" ]] || usage

undefined_symbols() {
  if [[ "$(uname -s)" == Darwin ]]; then
    nm -u "$1"
  else
    # The dynamic table: a bundled library may have lost its symbol table.
    nm -D --undefined-only "$1"
  fi
}

failed=0
for lib in "$airspy" "$rtlsdr"; do
  if [[ ! -f "$lib" ]]; then
    echo "ERROR: $lib does not exist" >&2
    failed=1
  fi
done
[[ $failed -eq 0 ]] || exit 1

if undefined_symbols "$airspy" | grep -Eq '(^|[[:space:]])_?usleep(@.*)?$'; then
  echo "$airspy: built with the libairspy transfer patch"
else
  echo "ERROR: $airspy does not import usleep(): it was not built with" \
    "0001-libairspy-wait-for-cancelled-transfers.patch" >&2
  failed=1
fi

if LC_ALL=C grep -aq 'no transfer left, canceling' "$rtlsdr"; then
  echo "$rtlsdr: built with the librtlsdr transfer patches"
else
  echo "ERROR: $rtlsdr lacks the reader's \"no transfer left\" message: it was not built with" \
    "0003-librtlsdr-account-for-every-transfer.patch" >&2
  failed=1
fi

exit "$failed"
