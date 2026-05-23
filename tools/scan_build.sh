#!/usr/bin/env bash
set -euo pipefail

# Run Clang Static Analyzer via scan-build on a dedicated temporary build tree.
# Intended for CI or explicit local runs (heavier than per-file analyzers).
# Excludes vendored third-party code under src/third_party.

ROOT_DIR=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat <<'USAGE'
Usage: tools/scan_build.sh [--strict] [--jobs N] [--build-dir DIR] [--output-dir DIR] [--target NAME]... [--reuse-build-dir] [--reuse-output-dir] [--cmake-arg ARG]...

Options:
  --strict          Fail on analyzer bugs and enable extra security/portability checkers.
  --jobs N          Parallel build jobs (default: detected CPU count).
  --build-dir DIR   Build directory (default: build/scan-build-debug).
  --output-dir DIR  scan-build report output dir (default: .scan-build.local).
  --target NAME     Build only the specified CMake target (repeatable).
  --reuse-build-dir Keep existing build directory instead of deleting it first.
  --reuse-output-dir
                    Keep existing scan-build output directory instead of deleting it first.
  --cmake-arg ARG   Extra CMake configure argument (repeatable).
USAGE
}

STRICT=0
JOBS=""
BUILD_DIR="build/scan-build-debug"
OUTPUT_DIR=".scan-build.local"
CMAKE_ARGS=()
TARGETS=()
REUSE_BUILD_DIR=0
REUSE_OUTPUT_DIR=0

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
    --target)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --target" >&2
        exit 2
      fi
      TARGETS+=("$2")
      shift 2
      ;;
    --reuse-build-dir)
      REUSE_BUILD_DIR=1
      shift
      ;;
    --reuse-output-dir)
      REUSE_OUTPUT_DIR=1
      shift
      ;;
    --cmake-arg)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --cmake-arg" >&2
        exit 2
      fi
      CMAKE_ARGS+=("$2")
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
  --exclude "$ROOT_DIR/src/third_party"
)
if [[ $STRICT -eq 1 ]]; then
  SCAN_ARGS+=(
    --status-bugs
    -analyze-headers
    --force-analyze-debug-code
    -maxloop 8
    -enable-checker security.ArrayBound
    -enable-checker security.FloatLoopCounter
    -enable-checker security.PointerSub
    -enable-checker security.VAList
    -enable-checker security.cert.env.InvalidPtr
    -enable-checker security.insecureAPI.DeprecatedOrUnsafeBufferHandling
    -enable-checker security.insecureAPI.rand
    -enable-checker security.insecureAPI.strcpy
    -enable-checker optin.portability.UnixAPI
  )
fi

for d in "$BUILD_DIR" "$OUTPUT_DIR"; do
  if [[ -z "$d" || "$d" == "/" ]]; then
    echo "Refusing to remove unsafe path: '$d'" >&2
    exit 2
  fi
done

if [[ $REUSE_BUILD_DIR -eq 0 ]]; then
  rm -rf "$BUILD_DIR"
fi
if [[ $REUSE_OUTPUT_DIR -eq 0 ]]; then
  rm -rf "$OUTPUT_DIR"
fi

BUILD_CMD=(cmake --build "$BUILD_DIR" -j "$JOBS")
if [[ ${#TARGETS[@]} -gt 0 ]]; then
  BUILD_CMD+=(--target "${TARGETS[@]}")
fi

set +e
{
  echo "scan-build version:"
  print_scan_build_version
  echo "Excluding analyzer path: $ROOT_DIR/src/third_party"
  if [[ ${#TARGETS[@]} -gt 0 ]]; then
    echo "Scoped build targets: ${TARGETS[*]}"
  else
    echo "Scoped build targets: <all>"
  fi
  echo "Reuse build dir: $([[ $REUSE_BUILD_DIR -eq 1 ]] && echo yes || echo no)"
  echo "Reuse output dir: $([[ $REUSE_OUTPUT_DIR -eq 1 ]] && echo yes || echo no)"
  echo ""
  echo "Configuring analysis build in $BUILD_DIR ..."
  scan-build "${SCAN_ARGS[@]}" cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "${CMAKE_ARGS[@]}"
  echo ""
  echo "Building under scan-build ..."
  scan-build "${SCAN_ARGS[@]}" "${BUILD_CMD[@]}"
} 2>&1 | tee "$LOG_FILE"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "scan-build completed. Reports in $OUTPUT_DIR, log in $LOG_FILE"
else
  echo "scan-build found issues or failed. Reports in $OUTPUT_DIR, log in $LOG_FILE" >&2
fi

exit "$rc"
