#!/usr/bin/env bash
set -euo pipefail

# Coverage runner for repository-owned production sources.

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/coverage-debug"
BUILD_PRESET=${BUILD_PRESET:-coverage-debug-clean}
TEST_PRESET=${TEST_PRESET:-coverage-debug}
LCOV_BRANCH_ARGS=(--branch-coverage)
LCOV_IGNORE_ERRORS=(--ignore-errors "negative,inconsistent,unused,mismatch")
GENHTML_IGNORE_ERRORS=(--ignore-errors inconsistent)
THIRD_PARTY_EXCLUDES=("${ROOT_DIR}/src/third_party/*")

cmake --preset coverage-debug > /dev/null
cmake --build --preset "$BUILD_PRESET" -j > /dev/null
ctest --preset "$TEST_PRESET" --output-on-failure

if ! command -v lcov > /dev/null 2>&1 || ! command -v genhtml > /dev/null 2>&1; then
  echo "lcov/genhtml are required for tools/coverage.sh." >&2
  echo "Install lcov, or run an explicit llvm-cov flow manually." >&2
  exit 1
fi

echo "Generating lcov report..."
pushd "$BUILD_DIR" > /dev/null

# GCC/lcov can emit malformed negative or inconsistent counters for some
# optimized/generated paths even when CTest passes. Keep the report generation
# tolerant of those counters while still surfacing lcov warnings on stderr.
lcov "${LCOV_BRANCH_ARGS[@]}" --capture --directory . --output-file coverage.info "${LCOV_IGNORE_ERRORS[@]}" > /dev/null

REPORT_NAME="coverage.src.info"
REPORT_DIR="coverage_html"
REPORT_LABEL="project src/ excluding src/third_party"
lcov "${LCOV_BRANCH_ARGS[@]}" --extract coverage.info "${ROOT_DIR}/src/*" -o "$REPORT_NAME" \
  "${LCOV_IGNORE_ERRORS[@]}" > /dev/null

FILTERED_REPORT="${REPORT_NAME%.info}.filtered.info"
lcov "${LCOV_BRANCH_ARGS[@]}" --remove "$REPORT_NAME" "${THIRD_PARTY_EXCLUDES[@]}" -o "$FILTERED_REPORT" \
  "${LCOV_IGNORE_ERRORS[@]}" > /dev/null
mv "$FILTERED_REPORT" "$REPORT_NAME"

rm -rf -- "$REPORT_DIR"
genhtml "${LCOV_BRANCH_ARGS[@]}" "$REPORT_NAME" --output-directory "$REPORT_DIR" "${GENHTML_IGNORE_ERRORS[@]}" \
  > /dev/null
printf 'Coverage summary (%s)\n' "$REPORT_LABEL"
lcov "${LCOV_BRANCH_ARGS[@]}" --summary "$REPORT_NAME" "${LCOV_IGNORE_ERRORS[@]}"
popd > /dev/null

echo "Coverage HTML: $BUILD_DIR/$REPORT_DIR/index.html"
