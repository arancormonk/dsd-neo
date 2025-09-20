#!/usr/bin/env bash
set -euo pipefail

# Simple coverage runner for P25 code paths.
# Requires lcov/genhtml or llvm-cov depending on compiler.

ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build/coverage-debug"

cmake --preset coverage-debug >/dev/null
cmake --build --preset coverage-debug -j >/dev/null
ctest --preset coverage-debug -V || true

if command -v lcov >/dev/null 2>&1; then
  echo "Generating lcov report..."
  pushd "$BUILD_DIR" >/dev/null
  lcov --capture --directory . --output-file coverage.info >/dev/null 2>&1 || true
  # Filter to protocol/p25 sources
  lcov --extract coverage.info "${ROOT_DIR}/src/protocol/p25/*" -o coverage.p25.info >/dev/null 2>&1 || true
  genhtml coverage.p25.info --output-directory coverage_html >/dev/null 2>&1 || true
  popd >/dev/null
  echo "Coverage HTML: $BUILD_DIR/coverage_html/index.html"
else
  echo "lcov not found. For clang/llvm, run llvm-cov manually, e.g.:"
  echo "  llvm-cov show ./tests/dsd-neo_test_p25_p2_mac_json -format=html -instr-profile=default.profdata > coverage.html"
fi

