#!/usr/bin/env bash
set -euo pipefail

# Run cppcheck locally for static analysis:
# - Analyzes C/C++ sources with project-specific settings
# - Complements clang-tidy with different analysis techniques
# - Fails on error-level issues; warnings are informational.

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)
cd "$ROOT_DIR"

if ! command -v cppcheck > /dev/null 2>&1; then
  echo "cppcheck not found. Please install it (e.g., apt-get install cppcheck)." >&2
  exit 1
fi

# Parse arguments
usage() {
  cat << 'USAGE'
Usage: tools/cppcheck.sh [--strict] [--verbose|-v] [--jobs N] [--] [files...]

Options:
  --strict    Enable all checks, inconclusive findings, and exhaustive value-flow;
              treat findings as errors.
  --verbose   Show detailed output during analysis.
  --jobs N    Number of cppcheck worker processes (default: detected CPU count).

Arguments:
  files...    Optional list of translation units to analyze (e.g., src/foo.c).
              When omitted, analyzes the src/ and include/ trees.

Environment:
  CPPCHECK_BUILD_DIR   Build/cache directory used by cppcheck
                       for cross-translation-unit state (default: .cppcheck-build).
USAGE
}

STRICT=0
VERBOSE=0
JOBS=""
REQUESTED_FILES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --strict)
      STRICT=1
      shift
      ;;
    --verbose | -v)
      VERBOSE=1
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
    --help | -h)
      usage
      exit 0
      ;;
    --)
      shift
      REQUESTED_FILES+=("$@")
      break
      ;;
    *)
      if [[ "$1" == -* ]]; then
        echo "Unknown option: $1" >&2
        usage >&2
        exit 1
      fi
      REQUESTED_FILES+=("$1")
      shift
      ;;
  esac
done

echo "cppcheck version:"
cppcheck --version

# Detect number of CPU cores for parallel analysis unless --jobs said otherwise.
if [[ -z "$JOBS" ]]; then
  JOBS=$(nproc 2> /dev/null || sysctl -n hw.ncpu 2> /dev/null || echo 4)
fi
if [[ ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "Invalid jobs value: $JOBS" >&2
  exit 2
fi
CPPCHECK_BUILD_DIR="${CPPCHECK_BUILD_DIR:-.cppcheck-build}"
mkdir -p "$CPPCHECK_BUILD_DIR"

# Build cppcheck arguments
# Note: cppcheck supports multiple --std flags; it applies the appropriate
# standard based on file extension (.c -> C standard, .cpp -> C++ standard)
CPPCHECK_ARGS=(
  "--enable=warning,performance,portability"
  --std=c11
  --std=c++14
  "-D__has_include(x)=0"
  --suppress=missingIncludeSystem
  "-DQ_OBJECT="
  "-DQ_PROPERTY(x)="
  "-DQ_ENUM(x)="
  "-DQ_SIGNALS=public"
  "-DQ_SLOTS="
  "-DQ_INVOKABLE="
  "-DQ_EMIT="
  # The NDK's jni.h is not on the include path here, so JNIEXPORT/JNICALL would
  # otherwise stay unknown identifiers and be parsed as part of the return type --
  # which makes a `JNIEXPORT void JNICALL` entry point look non-void and every bare
  # `return;` in it a missingReturn. Empty is what jni.h expands JNICALL to, and
  # JNIEXPORT's visibility attribute has no bearing on analysis.
  "-DJNIEXPORT="
  "-DJNICALL="
  --cppcheck-build-dir="$CPPCHECK_BUILD_DIR"
  --inline-suppr
  -I include
  -I src/dsp
  -I src/ui/terminal
  -I src/ui/terminal/menus
  -I src/ui/qt
  -I src/third_party
  -I src/third_party/pffft
  -j "$JOBS"
  --error-exitcode=1
)

# Strict mode: enable all checks and deeper value-flow analysis.
if [[ $STRICT -eq 1 ]]; then
  echo "Strict mode: enabling all checks, inconclusive findings, and exhaustive value-flow"
  CPPCHECK_ARGS=(
    --enable=all
    --inconclusive
    --check-level=exhaustive
    --max-ctu-depth=4
    --force
    --std=c11
    --std=c++14
    "-D__has_include(x)=0"
    --suppress=missingIncludeSystem
    "-DQ_OBJECT="
    "-DQ_PROPERTY(x)="
    "-DQ_ENUM(x)="
    "-DQ_SIGNALS=public"
    "-DQ_SLOTS="
    "-DQ_INVOKABLE="
    "-DQ_EMIT="
    # See the note on the non-strict argument list above.
    "-DJNIEXPORT="
    "-DJNICALL="
    --cppcheck-build-dir="$CPPCHECK_BUILD_DIR"
    --inline-suppr
    -I include
    -I src/dsp
    -I src/ui/terminal
    -I src/ui/terminal/menus
    -I src/ui/qt
    -I src/third_party
    -I src/third_party/pffft
    -j "$JOBS"
    --error-exitcode=1
  )
fi

# Verbose mode
if [[ $VERBOSE -eq 1 ]]; then
  CPPCHECK_ARGS+=(--verbose)
fi

# Suppress known false positives or low-value warnings for this codebase
# Format string mismatches with %d and unsigned are a recurring C-code issue
CPPCHECK_ARGS+=(
  --suppress=invalidPrintfArgType_sint
  --suppress=invalidPrintfArgType_uint
  --suppress=normalCheckLevelMaxBranches
  --suppress=unmatchedSuppression
  --suppress=unusedFunction
  --suppress=constParameter
  --suppress=toomanyconfigs
  --suppress=checkersReport
  --suppress='*:src/third_party/*'
  --suppress='*:*/src/third_party/*'
  --suppress='*:android/third_party/*'
  --suppress='*:*/android/third_party/*'
  # uninitMemberVarNoCtor on dsd_state is suppressed at the struct itself, with a
  # cppcheck-suppress-begin/end pair in include/dsd-neo/core/state.h: a suppression
  # here would have to name the file, and that covers the seventeen other types
  # declared in it as well.
  -i src/third_party
  -i android/third_party
)

LOG_FILE=".cppcheck.local.out"

FILES=()
if [[ ${#REQUESTED_FILES[@]} -gt 0 ]]; then
  for f in "${REQUESTED_FILES[@]}"; do
    f="${f#./}"
    case "$f" in
      build/* | src/third_party/* | android/third_party/*) continue ;;
    esac
    case "$f" in
      *.c | *.cc | *.cpp | *.cxx) FILES+=("$f") ;;
    esac
  done

  if [[ ${#FILES[@]} -eq 0 ]]; then
    echo "No translation units found to analyze from requested paths."
    exit 0
  fi

  mapfile -t FILES < <(printf '%s\n' "${FILES[@]}" | sort -u)
  echo "Analyzing ${#FILES[@]} file(s) with cppcheck..."
else
  echo "Analyzing src/, include/ and android/ directories..."
fi
echo ""

# Select analysis targets.
CPPCHECK_TARGETS=(src/ include/ android/)
if [[ ${#FILES[@]} -gt 0 ]]; then
  CPPCHECK_TARGETS=("${FILES[@]}")
fi

# Run cppcheck and capture output
# Use --template for consistent output format
if cppcheck "${CPPCHECK_ARGS[@]}" \
  --template='{file}:{line}: {severity}: {message} [{id}]' \
  "${CPPCHECK_TARGETS[@]}" 2>&1 | tee "$LOG_FILE"; then
  echo ""
  echo "cppcheck passed. Full output in $LOG_FILE"
else
  EXIT_CODE=$?
  echo ""
  echo "cppcheck found issues. See $LOG_FILE for details." >&2

  # Print summary by severity
  echo ""
  echo "Summary by severity:" >&2
  grep -E ': (error|warning|style|performance|portability):' "$LOG_FILE" 2> /dev/null |
    sed -E 's/.*: (error|warning|style|performance|portability):.*/\1/' |
    sort | uniq -c | sort -rn >&2 || true

  exit $EXIT_CODE
fi
