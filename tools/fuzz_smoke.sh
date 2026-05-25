#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

usage() {
  cat << 'USAGE'
Usage: tools/fuzz_smoke.sh [--no-build]

Configures/builds the fuzz-asan-debug preset and runs bounded libFuzzer smoke
passes for all dsd-neo fuzz targets.

Environment:
  DSD_FUZZ_RUNS          libFuzzer -runs value (default: 1000)
  DSD_FUZZ_MAX_LEN       libFuzzer -max_len value (default: 65536)
  DSD_FUZZ_TIMEOUT       libFuzzer -timeout value (default: 10)
  DSD_FUZZ_RSS_LIMIT_MB  libFuzzer -rss_limit_mb value (default: 2048)
USAGE
}

BUILD=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build)
      BUILD=0
      shift
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

if [[ $BUILD -eq 1 ]]; then
  cmake --preset fuzz-asan-debug
  cmake --build --preset fuzz-asan-debug -j
fi

BUILD_DIR="build/fuzz-asan-debug"
if [[ ! -d "$BUILD_DIR" ]]; then
  echo "Fuzz build directory not found: $BUILD_DIR" >&2
  exit 1
fi

mapfile -t FUZZERS < <(
  find "$BUILD_DIR/tests/fuzz" -maxdepth 1 -type f -perm -111 -name 'dsd-neo_fuzz_*' | sort
)

if [[ ${#FUZZERS[@]} -eq 0 ]]; then
  echo "No fuzz targets found under $BUILD_DIR/tests/fuzz" >&2
  exit 1
fi

RUNS="${DSD_FUZZ_RUNS:-1000}"
MAX_LEN="${DSD_FUZZ_MAX_LEN:-65536}"
TIMEOUT="${DSD_FUZZ_TIMEOUT:-10}"
RSS_LIMIT_MB="${DSD_FUZZ_RSS_LIMIT_MB:-2048}"
LOG_FILE=".fuzz-smoke.local.out"
WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/dsd-neo-fuzz-smoke.XXXXXX")
trap 'rm -rf "$WORK_DIR"' EXIT

set +e
{
  echo "fuzzers: ${#FUZZERS[@]}"
  for fuzzer in "${FUZZERS[@]}"; do
    name=$(basename "$fuzzer")
    corpus="tests/fuzz/corpus/${name#dsd-neo_fuzz_}"
    output_corpus="$WORK_DIR/${name#dsd-neo_fuzz_}"
    mkdir -p "$output_corpus"
    echo "==> $name"
    if [[ -d "$corpus" ]]; then
      "$fuzzer" "-runs=$RUNS" "-max_len=$MAX_LEN" "-timeout=$TIMEOUT" "-rss_limit_mb=$RSS_LIMIT_MB" \
        "$output_corpus" "$corpus"
    else
      "$fuzzer" "-runs=$RUNS" "-max_len=$MAX_LEN" "-timeout=$TIMEOUT" "-rss_limit_mb=$RSS_LIMIT_MB" \
        "$output_corpus"
    fi
  done
} 2>&1 | tee "$LOG_FILE"
rc=${PIPESTATUS[0]}
set -e

if [[ $rc -eq 0 ]]; then
  echo "Fuzz smoke completed. Full output in $LOG_FILE"
else
  echo "Fuzz smoke failed. See $LOG_FILE for details." >&2
fi
exit "$rc"
