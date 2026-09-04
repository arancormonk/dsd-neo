#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

# A/B two dsd-neo builds by replaying the same I/Q capture through both and
# reporting the decode quality each achieved.
#
# Why this exists: a change to the symbol timing, the slicer or the demodulator
# cannot be judged from one replay. Decoding runs on a threaded pipeline, so how
# much of a capture gets decoded varies run to run, and the difference between two
# builds is easily smaller than that variation. Issue #444 asked for exactly this
# -- several runs of a real capture, not one number -- and there was no way to do
# it. Two properties this handles that a hand-rolled loop usually does not:
#
#   * Round-robin, not blocked. Running all of build A and then all of build B
#     measures whatever else the machine was doing as if it were the build.
#   * Rotated order within each repeat. A fixed order inside a repeat credits the
#     better slot to whichever build holds it; a control of one build against
#     itself scored the two slots 0.26 err/frame apart.
#
# Report errors per decoded voice frame, never the raw error total: a build that
# loses sync decodes fewer frames and accrues fewer errors without being better.
#
# Usage:
#   tools/replay_ab.sh --capture <capture.json> --mode <flags> [options] <build>...
#
# Each <build> is a path to a dsd-neo binary; its basename names it in the report.
#
# Options:
#   --capture <path>   I/Q capture sidecar JSON to replay (required)
#   --mode <flags>     Decoder flags, e.g. "-fi" (required; quote if several)
#   --reps <n>         Repeats per build (default 12)
#   --rate <mode>      --iq-replay-rate value: realtime (default) or fast
#   --out <dir>        Where to write logs and summary.tsv (default: mktemp -d)
#
# Example:
#   tools/replay_ab.sh --capture ~/captures/nxdn.json --mode -fi --reps 12 \
#       /tmp/dsd-neo.before ./build/dev-debug/apps/dsd-cli/dsd-neo
#   tools/replay_ab_report.py <out>/summary.tsv

ROOT_DIR=$(git rev-parse --show-toplevel 2> /dev/null || pwd)

usage() {
  sed -n '/^# Usage:/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

capture=""
mode=""
reps=12
rate=realtime
out=""
builds=()

while [ $# -gt 0 ]; do
  case "$1" in
    --capture)
      capture="${2:-}"
      shift 2
      ;;
    --mode)
      mode="${2:-}"
      shift 2
      ;;
    --reps)
      reps="${2:-}"
      shift 2
      ;;
    --rate)
      rate="${2:-}"
      shift 2
      ;;
    --out)
      out="${2:-}"
      shift 2
      ;;
    -h | --help) usage 0 ;;
    --)
      shift
      builds+=("$@")
      break
      ;;
    -*)
      echo "unknown option: $1" >&2
      usage 1
      ;;
    *)
      builds+=("$1")
      shift
      ;;
  esac
done

if [ -z "$capture" ] || [ -z "$mode" ] || [ "${#builds[@]}" -lt 2 ]; then
  echo "error: --capture, --mode and at least two builds are required" >&2
  usage 1
fi
if [ ! -r "$capture" ]; then
  echo "error: cannot read capture '$capture'" >&2
  exit 1
fi
for b in "${builds[@]}"; do
  if [ ! -x "$b" ]; then
    echo "error: '$b' is not an executable dsd-neo build" >&2
    exit 1
  fi
done

if [ -z "$out" ]; then
  out=$(mktemp -d "${TMPDIR:-/tmp}/dsd-neo-replay-ab.XXXXXX")
fi
mkdir -p "$out"
summary="$out/summary.tsv"
printf 'variant\tcase\trep\terrs\tvoice\tsync\n' > "$summary"

# Quoted flags are several arguments, not one.
read -r -a mode_args <<< "$mode"
nbuilds=${#builds[@]}

echo "replaying $(basename "$capture") through $nbuilds builds, $reps repeats each, $rate pacing"
echo "results: $out"

for r in $(seq 1 "$reps"); do
  for ((k = 0; k < nbuilds; k++)); do
    build="${builds[$(((k + r - 1) % nbuilds))]}"
    name=$(basename "$build")
    log="$out/${name}__r${r}.log"
    set +e
    timeout 900 "$build" --frontend none "${mode_args[@]}" \
      --iq-replay "$capture" --iq-replay-rate "$rate" -o null > "$log" 2>&1
    rc=$?
    set -e
    errs=$(grep -oE 'Total audio errors: [0-9]+' "$log" | tail -1 | grep -oE '[0-9]+$' || true)
    voice=$(grep -c 'Voice' "$log" || true)
    sync=$(grep -cE 'Sync: ' "$log" || true)
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$name" "$(basename "$capture")-$rate" "$r" "${errs:-NA}" "$voice" "$sync" >> "$summary"
    printf '  r%-3s %-24s errs=%-6s voice=%-5s sync=%-5s%s\n' \
      "$r" "$name" "${errs:-NA}" "$voice" "$sync" "$([ "$rc" -ne 0 ] && echo " (exit $rc)")"
  done
done

echo
echo "wrote $summary"
echo "report with: $ROOT_DIR/tools/replay_ab_report.py $summary"
