#!/usr/bin/env bash
set -euo pipefail

# Run clang-tidy locally in a way that mirrors CI:
# - Ensures a compile_commands.json database exists (dev-debug preset)
# - Analyzes both sources and headers using the repo's .clang-tidy
# - Fails if any clang-analyzer-* or bugprone-* diagnostics are found

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$ROOT_DIR"

if ! command -v clang-tidy >/dev/null 2>&1; then
  echo "clang-tidy not found. Please install it (e.g., apt-get install clang-tidy)." >&2
  exit 1
fi
if ! command -v rg >/dev/null 2>&1; then
  echo "ripgrep (rg) not found. Please install it (e.g., apt-get install ripgrep)." >&2
  exit 1
fi

# Prefer compile_commands from the dev-debug preset; otherwise, use top-level if present.
PDB_DIR="build/dev-debug"
PDB_FILE="$PDB_DIR/compile_commands.json"
if [ ! -f "$PDB_FILE" ]; then
  if [ -f "compile_commands.json" ]; then
    PDB_DIR="."
  else
    echo "Configuring CMake preset 'dev-debug' to generate compile_commands.json..."
    cmake --preset dev-debug >/dev/null
  fi
fi

# Collect files: sources and headers within the repo (excluding build directory)
mapfile -t FILES < <(git ls-files '*.c' '*.h' ':!:build/**')
if [ ${#FILES[@]} -eq 0 ]; then
  echo "No C sources/headers found to analyze."
  exit 0
fi

echo "Using compilation database: $PDB_DIR"
echo "Analyzing ${#FILES[@]} files with clang-tidy..."
echo "clang-tidy version:"
clang-tidy --version | sed -n '1,2p'

# Run clang-tidy with project config and capture output
LOG_FILE=".clang-tidy.local.out"

# Optional strict mode: use alternate config enabling extra checks
CONFIG_FILE=".clang-tidy"
if [[ ${1-} == "--strict" ]]; then
  if [[ -f .clang-tidy.strict ]]; then
    CONFIG_FILE=".clang-tidy.strict"
    echo "Strict mode: using config $CONFIG_FILE"
  else
    echo "Strict mode requested, but .clang-tidy.strict not found; falling back to $CONFIG_FILE"
  fi
fi

if [[ -f "$CONFIG_FILE" ]]; then
  CFG_PATH=$(readlink -f "$CONFIG_FILE" 2>/dev/null || echo "$CONFIG_FILE")
  echo "Using config file: $CFG_PATH"
else
  echo "Config file not found: $CONFIG_FILE (clang-tidy will use built-in defaults)"
fi

clang-tidy -p "$PDB_DIR" --config-file "$CONFIG_FILE" "${FILES[@]}" 2>&1 | tee "$LOG_FILE" >/dev/null || true

# Fail only on curated high-signal checks (must align with WarningsAsErrors)
ERROR_REGEX='\[(clang-analyzer[^]]*|security-[^]]*|bugprone-(macro-parentheses|branch-clone|integer-division|signed-char-misuse|implicit-widening-of-multiplication-result|unsafe-functions|too-small-loop-variable|suspicious-string-compare)|misc-redundant-expression|cert-(str34-c|flp30-c))[^]]*\]$'
if rg -n "$ERROR_REGEX" "$LOG_FILE" >/dev/null; then
  echo "clang-tidy found curated analyzer/security/bugprone issues. See $LOG_FILE for details." >&2
  # Print a brief summary of counts by check for convenience
  echo "Summary (error checks):" >&2
  rg -n "$ERROR_REGEX" "$LOG_FILE" | sed -E 's/.*\[([^]]+)\]$/\1/' | awk -F',' '{print $1}' | sort | uniq -c | sort -nr >&2
  exit 1
fi

echo "clang-tidy clean for curated error checks. Full output in $LOG_FILE"
