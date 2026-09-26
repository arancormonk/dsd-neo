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
# Each <build> is a path to a dsd-neo binary; its basename names it in the report,
# so two builds may not share one. For --metric analog each <build> is an analog
# replay host (dsd-neo_test_analog_replay) copied under a name of its own, or a
# wrapper script that execs one with per-variant flags; docs/testing.md has both.
#
# Options:
#   --capture <path>   I/Q capture sidecar JSON to replay (required)
#   --mode <flags>     Decoder flags, e.g. "-fi" (required; quote if several)
#   --reps <n>         Repeats per build (default 12)
#   --rate <mode>      --iq-replay-rate value: realtime (default) or fast
#   --metric <kind>    digital (default) or analog: also score the host's
#                      ANALOG METRIC / ANALOG PROBE lines. Give the host no
#                      --analog-* bounds: summary.tsv records each run's exit
#                      status, and the report leaves non-zero ones out
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
metric=digital
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
    --metric)
      metric="${2:-}"
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
if [ "$metric" != digital ] && [ "$metric" != analog ]; then
  echo "error: --metric must be digital or analog, not '$metric'" >&2
  usage 1
fi
if [ ! -r "$capture" ]; then
  echo "error: cannot read capture '$capture'" >&2
  exit 1
fi
for ((i = 0; i < ${#builds[@]}; i++)); do
  b=${builds[$i]}
  if [ ! -x "$b" ]; then
    echo "error: '$b' is not an executable dsd-neo build" >&2
    exit 1
  fi
  # The basename keys the logs and the report, so two builds sharing one would
  # overwrite each other's logs and be scored as a single build.
  for ((j = 0; j < i; j++)); do
    if [ "$(basename "${builds[$j]}")" = "$(basename "$b")" ]; then
      echo "error: '${builds[$j]}' and '$b' have the same name ($(basename "$b"));" \
        "copy or wrap them under distinct names" >&2
      exit 1
    fi
  done
done

if [ -z "$out" ]; then
  out=$(mktemp -d "${TMPDIR:-/tmp}/dsd-neo-replay-ab.XXXXXX")
fi
mkdir -p "$out"
summary="$out/summary.tsv"
analog_keys=(tone_snr_db inband_db clip audible_ms first_audible_ms rms_dbfs)
probe_keys=(hz dbfs dbc)
printf 'variant\tcase\trep\terrs\tvoice\tsync' > "$summary"
printf '\t%s' "${analog_keys[@]}" probe_hz probe_dbfs probe_dbc \
  tone tone_lock_ms tone_lock_pct rc off_path >> "$summary"
printf '\n' >> "$summary"

# Value of key=value on the last (or, with which=first, the first) line of the log
# that starts with prefix, or NA. The line is split on spaces, so no value may
# contain one. tone= (a label such as 151.4 or D023N), tone_lock_ms= (stream
# time of the first lock) and tone_lock_pct= (share of the delivered audio the
# tone was locked for) follow the contract in tests/engine/analog_replay.c's file
# comment; a host that predates a field reads NA, like every analog column of a
# digital run.
line_value() {
  local log=$1 prefix=$2 key=$3 which=${4:-last} value pick=(tail -n 1)
  [ "$which" = first ] && pick=(head -n 1)
  value=$(grep -E "^${prefix}" "$log" | "${pick[@]}" | tr ' ' '\n' | sed -n "s/^${key}=//p" | head -n 1 || true)
  printf '%s' "${value:-NA}"
}

# Quoted flags are several arguments, not one.
read -r -a mode_args <<< "$mode"
nbuilds=${#builds[@]}

echo "replaying $(basename "$capture") through $nbuilds builds, $reps repeats each, $rate pacing, $metric metric"
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
    analog=()
    for key in "${analog_keys[@]}"; do
      analog+=("$(line_value "$log" 'ANALOG METRIC:' "$key")")
    done
    # The first probe the host was given, with its frequency: a wrapper that puts
    # its own --analog-probe-hz first probes elsewhere, and the report pairs probe
    # levels only between builds that probed the same frequency. dbc needs
    # --analog-expect-tone-hz; dbfs does not, so it is the level on real captures.
    for key in "${probe_keys[@]}"; do
      analog+=("$(line_value "$log" 'ANALOG PROBE:' "$key" first)")
    done
    for key in tone tone_lock_ms tone_lock_pct; do
      analog+=("$(line_value "$log" 'ANALOG METRIC:' "$key")")
    done
    # off_path is 1 when the analog replay host warned that the RTL front end left
    # the monitor path and delivered CQPSK symbols (the -fA modulation auto-switch):
    # such a repeat measures the switch, not the build, and is not deterministic.
    # The text is the host's warning in tests/engine/analog_replay.c. With rc, it
    # lets replay_ab_report.py leave bad repeats out instead of pairing them.
    off_path=0
    if grep -qF 'CQPSK symbols instead of monitor samples' "$log"; then
      off_path=1
    fi
    {
      printf '%s\t%s\t%s\t%s\t%s\t%s' \
        "$name" "$(basename "$capture")-$rate" "$r" "${errs:-NA}" "$voice" "$sync"
      printf '\t%s' "${analog[@]}" "$rc" "$off_path"
      printf '\n'
    } >> "$summary"
    exit_note=$([ "$rc" -ne 0 ] && echo " (exit $rc)" || true)
    if [ "$off_path" -eq 1 ]; then
      exit_note="$exit_note (off the monitor path)"
    fi
    if [ "$metric" = analog ]; then
      # analog[] follows the summary header: snr inband clip audible first rms probe_hz probe_dbfs ...
      printf '  r%-3s %-24s snr=%-8s inband=%-8s audible_ms=%-9s rms=%-8s probe_dbfs=%-8s%s\n' \
        "$r" "$name" "${analog[0]}" "${analog[1]}" "${analog[3]}" "${analog[5]}" "${analog[7]}" "$exit_note"
    else
      printf '  r%-3s %-24s errs=%-6s voice=%-5s sync=%-5s%s\n' \
        "$r" "$name" "${errs:-NA}" "$voice" "$sync" "$exit_note"
    fi
  done
done

echo
echo "wrote $summary"
echo "report with: $ROOT_DIR/tools/replay_ab_report.py --metric $metric $summary"
