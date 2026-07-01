#!/usr/bin/env bash
set -euo pipefail

DIR=$(cd "$(dirname "$0")" && pwd)

# Prefer bundled libraries
export DYLD_FALLBACK_LIBRARY_PATH="$DIR/lib:${DYLD_FALLBACK_LIBRARY_PATH:-}"

# Prefer bundled ncurses terminal descriptions when the portable package
# includes them. This keeps Homebrew ncurses usable on systems without
# Homebrew's terminfo database installed.
export TERMINFO_DIRS="$DIR/share/terminfo:/usr/share/terminfo:${TERMINFO_DIRS:-}"

exec "$DIR/bin/dsd-neo" "$@"
