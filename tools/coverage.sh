#!/usr/bin/env bash
set -euo pipefail

# Coverage runner for repository sources.
# Defaults to a clean project-wide src/ report. For the old protocol-only slice,
# run with COVERAGE_SCOPE=protocol and optionally set PROTO_COVERAGE="p25 dmr nxdn".

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/coverage-debug"
BUILD_PRESET=${BUILD_PRESET:-coverage-debug-clean}
TEST_PRESET=${TEST_PRESET:-coverage-debug}
COVERAGE_SCOPE=${COVERAGE_SCOPE:-src}

cmake --preset coverage-debug >/dev/null
cmake --build --preset "$BUILD_PRESET" -j >/dev/null
ctest --preset "$TEST_PRESET" --output-on-failure

if ! command -v lcov >/dev/null 2>&1 || ! command -v genhtml >/dev/null 2>&1; then
  echo "lcov/genhtml are required for tools/coverage.sh." >&2
  echo "Install lcov, or run an explicit llvm-cov flow manually." >&2
  exit 1
fi

echo "Generating lcov report..."
pushd "$BUILD_DIR" >/dev/null
lcov --capture --directory . --output-file coverage.info >/dev/null

case "$COVERAGE_SCOPE" in
  src|project)
    REPORT_NAME="coverage.src.info"
    REPORT_DIR="coverage_html"
    REPORT_LABEL="project src/"
    lcov --extract coverage.info "${ROOT_DIR}/src/*" -o "$REPORT_NAME" >/dev/null
    ;;
  protocol)
    REPORT_NAME="coverage.protocol.info"
    REPORT_DIR="coverage_protocol_html"
    PROTO_COVERAGE=${PROTO_COVERAGE:-"p25 dmr"}
    REPORT_LABEL="protocol slice (${PROTO_COVERAGE}) + fec"
    EXTRACT_ARGS=()
    for proto in $PROTO_COVERAGE; do
      EXTRACT_ARGS+=("${ROOT_DIR}/src/protocol/${proto}/*")
    done
    EXTRACT_ARGS+=("${ROOT_DIR}/src/fec/*")
    lcov --extract coverage.info "${EXTRACT_ARGS[@]}" -o "$REPORT_NAME" >/dev/null
    ;;
  *)
    echo "Unknown COVERAGE_SCOPE: $COVERAGE_SCOPE" >&2
    echo "Expected one of: src, project, protocol" >&2
    exit 1
    ;;
esac

genhtml "$REPORT_NAME" --output-directory "$REPORT_DIR" >/dev/null
printf 'Coverage summary (%s)\n' "$REPORT_LABEL"
lcov --summary "$REPORT_NAME"
popd >/dev/null

echo "Coverage HTML: $BUILD_DIR/$REPORT_DIR/index.html"
