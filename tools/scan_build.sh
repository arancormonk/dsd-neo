#!/usr/bin/env bash
set -euo pipefail

# Run Clang Static Analyzer via scan-build on a dedicated temporary build tree.
# Intended for CI or explicit local runs (heavier than per-file analyzers).
# Excludes third-party code under src/external.

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat <<'USAGE'
Usage: tools/scan_build.sh [--strict] [--jobs N] [--build-dir DIR] [--output-dir DIR]

Options:
  --strict          Fail if scan-build reports bugs (--status-bugs).
  --jobs N          Parallel build jobs (default: detected CPU count).
  --build-dir DIR   Build directory (default: build/scan-build-debug).
  --output-dir DIR  scan-build report output dir (default: .scan-build.local).
USAGE
}

STRICT=0
JOBS=""
BUILD_DIR="build/scan-build-debug"
OUTPUT_DIR=".scan-build.local"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict) STRICT=1; shift ;;
    --jobs)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --jobs" >&2
        exit 2
      fi
      JOBS="$2"
      shift 2
      ;;
    --build-dir)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --build-dir" >&2
        exit 2
      fi
      BUILD_DIR="$2"
      shift 2
      ;;
    --output-dir)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --output-dir" >&2
        exit 2
      fi
      OUTPUT_DIR="$2"
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if ! command -v scan-build >/dev/null 2>&1; then
  echo "scan-build not found. Please install clang-tools." >&2
  exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
  echo "cmake not found. Please install cmake." >&2
  exit 1
fi

print_scan_build_version() {
  if scan-build --version >/dev/null 2>&1; then
    scan-build --version
    return 0
  fi

  if scan-build -version >/dev/null 2>&1; then
    scan-build -version
    return 0
  fi

  echo "scan-build path: $(command -v scan-build)"
  return 0
}

if [[ -z "$JOBS" ]]; then
  JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
fi

LOG_FILE=".scan-build.local.out"
SCAN_ARGS=(
  --keep-empty
  -o "$OUTPUT_DIR"
  --exclude "$ROOT_DIR/src/external"
)
if [[ $STRICT -eq 1 ]]; then
  SCAN_ARGS+=(--status-bugs)
fi

for d in "$BUILD_DIR" "$OUTPUT_DIR"; do
  if [[ -z "$d" || "$d" == "/" ]]; then
    echo "Refusing to remove unsafe path: '$d'" >&2
    exit 2
  fi
done

rm -rf "$BUILD_DIR" "$OUTPUT_DIR"

set +e
{
  echo "scan-build version:"
  print_scan_build_version
  echo "Excluding analyzer path: $ROOT_DIR/src/external"
  echo ""
  echo "Configuring analysis build in $BUILD_DIR ..."
  scan-build "${SCAN_ARGS[@]}" cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  echo ""
  echo "Building under scan-build ..."
  scan-build "${SCAN_ARGS[@]}" cmake --build "$BUILD_DIR" -j "$JOBS"
} 2>&1 | tee "$LOG_FILE"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "scan-build completed. Reports in $OUTPUT_DIR, log in $LOG_FILE"
else
  echo "scan-build found issues or failed. Reports in $OUTPUT_DIR, log in $LOG_FILE" >&2
fi

exit "$rc"
