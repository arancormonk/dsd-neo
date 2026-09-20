#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
# Package transactions must fail the installer, including optional groups called
# inside an `if !`: POSIX shells disable errexit throughout those functions.
set -euo pipefail

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/apt-get" << 'FAKE'
#!/bin/sh
for arg; do
  [ "$arg" != "$FAIL_PACKAGE" ] || exit 42
done
FAKE
cat > "$WORK/apt-cache" << 'FAKE'
#!/bin/sh
[ "$2" != "${MISSING_PACKAGE:-}" ]
FAKE
cat > "$WORK/id" << 'FAKE'
#!/bin/sh
echo 0
FAKE
cat > "$WORK/git" << 'FAKE'
#!/bin/sh
echo "Dependency compilation reached after a failed package transaction" >&2
exit 98
FAKE
chmod +x "$WORK/apt-get" "$WORK/apt-cache" "$WORK/id" "$WORK/git"

check_failure() {
  local rc=0
  env PATH="$WORK:$PATH" FAIL_PACKAGE="$1" MISSING_PACKAGE="$2" \
    sh "$ROOT_DIR/tools/install_linux.sh" --yes --build-dir "$WORK/build" \
    --radio "$3" > "$WORK/output" 2>&1 || rc=$?
  if [[ "$rc" -ne 42 ]]; then
    cat "$WORK/output" >&2
    echo "Expected package failure 42 for $1 (missing: $2, radio: $3), got $rc" >&2
    exit 1
  fi
}

check_failure librtlsdr-dev '' auto
check_failure librtlsdr-dev libsoapysdr-dev auto
check_failure librtlsdr-dev '' required
check_failure libcodec2-dev '' off

echo "Installer preserves package transaction failures."
