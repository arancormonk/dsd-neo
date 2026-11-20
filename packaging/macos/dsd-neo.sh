#!/usr/bin/env bash
set -euo pipefail

DIR=$(cd "$(dirname "$0")" && pwd)

# Prefer bundled libraries
export DYLD_FALLBACK_LIBRARY_PATH="$DIR/lib:${DYLD_FALLBACK_LIBRARY_PATH:-}"

# Prefer the bundled ncurses terminal descriptions. The bundled ncurses was
# built with a search path that only exists on the build machine, and macOS's
# own /usr/share/terminfo, which uses the same layout, covers what the bundle
# lacks.
export TERMINFO_DIRS="$DIR/share/terminfo:/usr/share/terminfo:${TERMINFO_DIRS:-}"

exec "$DIR/bin/dsd-neo" "$@"
