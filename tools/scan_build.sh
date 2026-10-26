#!/usr/bin/env bash
set -euo pipefail

# Run Clang Static Analyzer via scan-build on a dedicated temporary build tree.
# Intended for CI or explicit local runs (heavier than per-file analyzers).
# Excludes vendored third-party code under src/third_party and android/third_party
# (a desktop test builds the vendored librtlsdr against stub libusb).

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
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
    --strict)
      STRICT=1
      shift
      ;;
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
    -h | --help)
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

if ! command -v scan-build > /dev/null 2>&1; then
  echo "scan-build not found. Please install clang-tools." >&2
  exit 1
fi
if ! command -v cmake > /dev/null 2>&1; then
  echo "cmake not found. Please install cmake." >&2
  exit 1
fi

print_scan_build_version() {
  if scan-build --version > /dev/null 2>&1; then
    scan-build --version
    return 0
  fi

  if scan-build -version > /dev/null 2>&1; then
    scan-build -version
    return 0
  fi

  echo "scan-build path: $(command -v scan-build)"
  return 0
}

if [[ -z "$JOBS" ]]; then
  JOBS=$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)
fi

LOG_FILE=".scan-build.local.out"
SCAN_ARGS=(
  --keep-empty
  -o "$OUTPUT_DIR"
  --exclude "$ROOT_DIR/src/third_party"
  --exclude "$ROOT_DIR/android/third_party"
)
if [[ $STRICT -eq 1 ]]; then
  # No --status-bugs: the strict verdict counts the reports itself, in count_reports_but_clang_header_false_positives().
  SCAN_ARGS+=(
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

# -analyze-headers also analyzes every function in Clang's own intrinsic headers. cetintrin.h's _rdsspd_i32() and
# _rdsspq_i64() pass an uninitialized value to their builtin on purpose (the header silences -Wuninitialized for it), so
# every file that includes <immintrin.h> reports those two functions as "Uninitialized argument value". scan-build's
# --exclude matches whole directories only, and excluding Clang's include directory would also hide real bugs that end
# there (a bad pointer reaching _mm256_loadu_ps()). So exactly these reports are left out of the strict count, matched
# by file, function and bug type (they stay in the HTML output); any other report, in that header or elsewhere, still
# fails the run. Only the run directories this invocation added are counted (see previous_runs), as --status-bugs
# did. Prints how many reports are left.
count_reports_but_clang_header_false_positives() {
  local analyzer_clang resource_dir run report left=0
  analyzer_clang="$(dirname "$(command -v scan-build)")/clang"
  if [[ ! -x "$analyzer_clang" ]]; then
    analyzer_clang=clang
  fi
  resource_dir=$("$analyzer_clang" -print-resource-dir 2> /dev/null || true)
  while IFS= read -r -d '' run; do
    if grep -q -F -x -- "$run" <<< "$previous_runs"; then
      continue
    fi
    while IFS= read -r -d '' report; do
      if [[ -n "$resource_dir" ]] \
        && grep -q -F -x "<!-- BUGFILE $resource_dir/include/cetintrin.h -->" "$report" \
        && grep -q -F -x "<!-- BUGTYPE Uninitialized argument value -->" "$report" \
        && grep -q -E -x "<!-- FUNCTIONNAME _rdssp(d_i32|q_i64) -->" "$report"; then
        echo "Not counting Clang header false positive: $report" >&2
        continue
      fi
      left=$((left + 1))
    done < <(find "$run" -maxdepth 1 -name 'report-*.html' -print0)
  done < <(find -H "$OUTPUT_DIR" -mindepth 1 -maxdepth 1 -type d -print0 2> /dev/null)
  echo "$left"
}

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

# The run directories a reused output directory already holds; the strict verdict skips them. -H (above) follows an
# output directory given as a symlink, as scan-build itself does when it writes there.
previous_runs=""
if [[ -d "$OUTPUT_DIR" ]]; then
  previous_runs=$(find -H "$OUTPUT_DIR" -mindepth 1 -maxdepth 1 -type d)
fi

set +e
{
  echo "scan-build version:"
  print_scan_build_version
  echo "Excluding analyzer paths: $ROOT_DIR/src/third_party $ROOT_DIR/android/third_party"
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
    "${CMAKE_ARGS[@]}" || exit $?
  for dependency in DSD_PFFFT_SOURCE_DIR DSD_TETRA_CRYPTO_SOURCE_DIR; do
    dependency_source_dir=$(sed -n "s/^$dependency:INTERNAL=//p" "$BUILD_DIR/CMakeCache.txt")
    if [[ -n "$dependency_source_dir" ]]; then
      SCAN_ARGS+=(--exclude "$dependency_source_dir")
    fi
  done
  echo ""
  echo "Building under scan-build ..."
  scan-build "${SCAN_ARGS[@]}" "${BUILD_CMD[@]}"
} 2>&1 | tee "$LOG_FILE"
rc=${PIPESTATUS[0]}
set -e

if [[ $STRICT -eq 1 && $rc -eq 0 ]]; then
  bugs=$(count_reports_but_clang_header_false_positives 2>> "$LOG_FILE")
  echo "scan-build: $bugs bug(s) counted, Clang header false positives aside" | tee -a "$LOG_FILE"
  if [[ $bugs -gt 0 ]]; then
    rc=1
  fi
fi

if [[ $rc -eq 0 ]]; then
  echo "scan-build completed. Reports in $OUTPUT_DIR, log in $LOG_FILE"
else
  echo "scan-build found issues or failed. Reports in $OUTPUT_DIR, log in $LOG_FILE" >&2
fi

exit "$rc"
