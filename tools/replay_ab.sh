#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

# A/B two dsd-neo builds by replaying the same I/Q capture through both and
# reporting the decode quality each achieved.
#
# Why this exists: a change to the symbol timing, the slicer or the demodulator
# cannot be judged from one number. Issue #444 asked for several runs of a real
# capture, because decoding then varied run to run by more than the difference
# between two builds. Since issue #572 an I/Q replay is deterministic: a build
# decodes a capture the same way on every run, fast or realtime, loaded or idle
# (replays under -T or -Y excepted, so leave them out of --mode). Identical
# repeats would then all be one draw of the capture: which frames a replay
# yields depends on where the decoder, the -fa sync hunt above all, stands when
# each transmission arrives, and a change that moves the front end's output by a
# fraction of a sample, such as a filter's latency, moves that draw. Paired over
# identical repeats, two builds read a tight interval that the capture does not
# support.
#
# So each repeat replays its own realization of the capture. Repeat r (from 1)
# replays a copy of the sidecar, <out>/realizations/r<r>.json, whose data_file is
# the original data and whose every event byte_offset moves forward by
#
#   s_r = floor((r-1) * P / reps) + (r-1) input samples,  P = round(rate / 2400),
#
# held to the bytes replay reads (data_bytes, or the data file's size when that
# is smaller or data_bytes is 0, in whole samples). P is one symbol of the
# slowest common digital rate at the capture's own rate: 640 samples at
# 1.536 Msps, 20 at 48 kHz. Repeat 1 is the capture as recorded, and a sidecar
# with an event past those bytes, which replay refuses, gets no copies. The
# first term spreads the shifts over a symbol and the (r-1) adds a sample a
# repeat. A shift after the first that still lands on a whole multiple of the
# front end's total decimation D (sample_rate_hz over demod_rate_hz, else
# base_decimation x post_downsample, else 1) moves up one sample, when D is
# above 1: replay restarts the chunk grid and the filters at
# each event, so a shift by such a multiple hands each dwell the same output
# samples, only later, and is no new draw of the instants the decoder samples
# the signal at. Every build replays the same copy in a repeat, so the report
# pairs like with like, and a build against a copy of itself still reads
# +0.00 +/- 0.00; any spread in that control is a determinism regression to fix
# before comparing builds. More repeats buy more realizations, so a result wants
# the default 12 or more, and other captures besides (docs/testing.md, "Replay
# determinism").
#
# A capture with no events has nothing to shift: every repeat replays it as
# recorded, and the run warns that its repeats are one realization, to be
# compared across captures. Events held at the end of the bytes replay reads
# stop moving, so two shifts can land every event in the same place: a repeat
# whose events do that replays the same realization as the earlier repeat,
# records that repeat's shift so the report counts it once, and its progress
# lines say so. The run warns as for a capture with no events when every repeat
# collapses onto one, and names how many distinct realizations it has when some
# do. --no-realizations replays every repeat as recorded on purpose: that is the
# determinism control. Two properties this keeps from before issue #572, which
# cost nothing now and keep a determinism regression from passing for a
# difference in builds:
#
#   * Round-robin, not blocked. Running all of build A and then all of build B
#     measured whatever else the machine was doing as if it were the build.
#   * Rotated order within each repeat. A fixed order inside a repeat credited the
#     better slot to whichever build held it; a control of one build against
#     itself once scored the two slots 0.26 err/frame apart.
#
# The default --rate stays realtime, which runs each replay in the capture's own
# time; fast decodes the same thing sooner.
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
#   --reps <n>         Repeats per build, each its own realization of the
#                      capture (default 12)
#   --no-realizations  Replay every repeat as recorded: the determinism
#                      control, where every repeat must decode alike
#   --rate <mode>      --iq-replay-rate value: realtime (default) or fast
#   --metric <kind>    digital (default) or analog: also score the host's
#                      ANALOG METRIC / ANALOG PROBE lines. Give the host no
#                      --analog-* bounds: summary.tsv records each run's exit
#                      status, and the report leaves non-zero ones out
#   --out <dir>        Where to write logs, summary.tsv and the realizations'
#                      sidecars (default: mktemp -d)
#
# Example:
#   tools/replay_ab.sh --capture ~/captures/nxdn.json --mode -fi \
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
realizations=1
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
    --no-realizations)
      realizations=0
      shift
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
if ! [[ $reps =~ ^[1-9][0-9]*$ ]]; then
  echo "error: --reps must be a positive whole number, not '$reps'" >&2
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

# Writes repeat r's realization of the capture, for r = 1..reps, to <dir>/r<r>.json: a copy of the sidecar whose
# data_file is the original data's absolute path and whose every event byte_offset moves forward by s_r whole samples,
# held to the bytes replay reads. Prints "symbol<TAB>P<TAB>D" and then "<r><TAB><s_r><TAB><path><TAB><q><TAB><s_q>"
# for each repeat, where q is the first repeat whose events landed where r's did (r itself, unless holding them at
# the end of the bytes replay reads made the two copies one schedule) and s_q its shift. Prints only "none", and
# writes nothing, when the capture has no events to shift. Fails, writing nothing, on a sidecar it cannot shift.
write_realizations() {
  python3 - "$@" << 'PY'
import json
import math
import os
import sys

capture, reps, outdir = sys.argv[1], int(sys.argv[2]), sys.argv[3]
BYTES_PER_SAMPLE = {"cu8": 2, "cf32": 8}


def fail(message):
    sys.exit(f"error: {capture}: {message}")


def whole(fields, key, minimum, where=""):
    value = fields.get(key)
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        fail(f"{where}{key} must be a whole number of at least {minimum}, not {value!r}")
    return value


def positive(key):
    value = meta.get(key)
    return value if isinstance(value, int) and not isinstance(value, bool) and value > 0 else None


def is_absolute(path):
    """path_is_absolute() in src/io/iq/iq_replay.c."""
    return path[:1] in ("/", "\\") or (path[:1].isascii() and path[:1].isalpha() and path[1:2] == ":")


def resolve_data_file(data_file):
    """resolve_data_path() in src/io/iq/iq_replay.c, made absolute: a relative data_file is appended to the sidecar
    path up to its last separator, as given, and the system resolves the result. Nothing collapses a ".." here, since
    the system follows a symlinked directory first and lands elsewhere."""
    if is_absolute(data_file):
        return data_file
    cut = max(capture.rfind("/"), capture.rfind("\\"))
    path = capture[:cut + 1] + data_file
    return path if is_absolute(path) else os.path.join(os.getcwd(), path)


def replay_bytes(path):
    """dsd_iq_replay_compute_effective_bytes() in src/io/iq/iq_replay.c: the whole data file when data_bytes is 0,
    else data_bytes held to the file's size, rounded down to whole samples."""
    try:
        size = os.stat(path).st_size
    except OSError as exc:
        fail(f"cannot read the size of data file {path!r}: {exc}")
    raw = size if data_bytes == 0 else min(data_bytes, size)
    return raw - raw % bytes_per_sample


def total_decimation(rate):
    """Input samples per output sample, D: a shift by a whole multiple of it moves every dwell by whole output samples.
    sample_rate_hz over demod_rate_hz (the smallest such shift when the two do not divide), else base_decimation x
    post_downsample, else 1."""
    demod = positive("demod_rate_hz")
    if demod:
        return rate // math.gcd(rate, demod)
    base, post = positive("base_decimation"), positive("post_downsample")
    return base * post if base and post else 1


# surrogateescape carries whatever bytes the sidecar's strings hold into the copies unchanged.
try:
    with open(capture, encoding="utf-8", errors="surrogateescape") as handle:
        meta = json.load(handle)
except (OSError, ValueError) as exc:
    fail(f"cannot read it as an I/Q capture sidecar (--capture takes the .json): {exc}")
if not isinstance(meta, dict):
    fail("not an I/Q capture sidecar (expected a JSON object)")
events = meta.get("events")
if not events:
    print("none")
    sys.exit(0)
if not isinstance(events, list) or not all(isinstance(event, dict) for event in events):
    fail("events must be an array of objects")
bytes_per_sample = BYTES_PER_SAMPLE.get(meta.get("sample_format"))
if bytes_per_sample is None:
    fail(f"cannot shift the events of sample_format {meta.get('sample_format')!r}, only of cu8 or cf32; "
         "pass --no-realizations to replay it as recorded")
rate = whole(meta, "sample_rate_hz", 1)
data_bytes = whole(meta, "data_bytes", 0)
for index, event in enumerate(events):
    whole(event, "byte_offset", 0, f"events[{index}].")
data_file = meta.get("data_file")
if not isinstance(data_file, str) or not data_file:
    fail("has no data_file")

# One symbol at 2400 baud, rounded to whole samples.
symbol = (rate + 1200) // 2400
decimation = total_decimation(rate)
copy = dict(meta)
# Replay resolves a relative data_file against the sidecar's directory, which the copies do not share.
copy["data_file"] = resolve_data_file(data_file)
# Replay refuses an event past the bytes it reads, so repeat 1 keeps every offset as recorded.
limit = replay_bytes(copy["data_file"])
if limit == 0:
    fail("no aligned I/Q samples available for replay")
for index, event in enumerate(events):
    if event["byte_offset"] > limit:
        fail(f"events[{index}].byte_offset {event['byte_offset']} is past the {limit} bytes replay reads")
os.makedirs(outdir, exist_ok=True)
rows = [f"symbol\t{symbol}\t{decimation}"]
# Event schedule (kind and offset of each event, in order) -> the first repeat that replays it, and its shift. Events
# held at the end of the bytes replay reads stop moving, so two shifts can give one schedule, which is one realization.
schedules = {}
for rep in range(1, reps + 1):
    shift = (rep - 1) * symbol // reps + (rep - 1)
    if rep > 1 and decimation > 1 and shift % decimation == 0:
        shift += 1
    copy["events"] = [dict(event, byte_offset=min(event["byte_offset"] + shift * bytes_per_sample, limit))
                      for event in events]
    schedule = tuple((event.get("kind"), event["byte_offset"]) for event in copy["events"])
    first_rep, first_shift = schedules.setdefault(schedule, (rep, shift))
    path = os.path.join(outdir, f"r{rep}.json")
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as handle:
        json.dump(copy, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    rows.append(f"{rep}\t{shift}\t{path}\t{first_rep}\t{first_shift}")
print("\n".join(rows))
PY
}

if [ -z "$out" ]; then
  out=$(mktemp -d "${TMPDIR:-/tmp}/dsd-neo-replay-ab.XXXXXX")
fi
mkdir -p "$out"

# Repeat r replays replays[r], whose events sit shifts[r] input samples after the capture's. same_as[r] is the first
# repeat that replays the same realization, r itself unless r's shifted events landed where an earlier repeat's did;
# shifts[r] is then that repeat's shift, so summary.tsv and the report count the realization once.
replays=()
shifts=()
same_as=()
for r in $(seq 1 "$reps"); do
  replays[r]=$capture
  shifts[r]=0
  same_as[r]=$r
done
if [ "$realizations" -eq 0 ]; then
  realization_note="off (--no-realizations): every repeat replays the capture as recorded, the determinism control"
else
  if ! command -v python3 > /dev/null 2>&1; then
    echo "error: per-repeat realizations need python3; pass --no-realizations to replay without them" >&2
    exit 1
  fi
  table=$(write_realizations "$capture" "$reps" "$out/realizations")
  if [ "$table" = none ]; then
    echo "warning: the capture has no events: every repeat replays the same realization; compare it across captures" >&2
    realization_note="none: the capture has no events to shift, so every repeat replays it as recorded"
  else
    distinct=0
    {
      IFS=$'\t' read -r _ symbol decimation
      while IFS=$'\t' read -r r _ path first_rep first_shift; do
        replays[r]=$path
        shifts[r]=$first_shift
        same_as[r]=$first_rep
        if [ "$first_rep" -eq "$r" ]; then
          distinct=$((distinct + 1))
        fi
      done
    } <<< "$table"
    if [ "$reps" -eq 1 ]; then
      realization_note="only repeat 1 runs, unshifted: the capture as recorded (--reps 2 or more gives each repeat"
      realization_note+=" its own realization)"
    elif [ "$distinct" -eq 1 ]; then
      # Repeat 2 is shifted, so its events landing where repeat 1's are means every event sits at the end of the
      # bytes replay reads.
      echo "warning: every event sits at the end of the bytes replay reads, where no shift moves it:" \
        "every repeat replays the same realization; compare it across captures" >&2
      realization_note="one: every event sits at the end of the bytes replay reads, so every repeat replays the"
      realization_note+=" capture as recorded"
    else
      if [ "$distinct" -lt "$reps" ]; then
        echo "notice: the run has $distinct distinct realizations over its $reps repeats: the rest replay the same" \
          "realization as an earlier repeat (their progress lines name it), since events held at the end of the" \
          "bytes replay reads stop moving" >&2
        realization_note="$distinct over $reps repeats"
      else
        realization_note="one per repeat"
      fi
      realization_note+=", every event moved ${shifts[1]} to ${shifts[reps]} input samples later"
      realization_note+=" (a 2400-baud symbol is $symbol samples"
      if [ "$decimation" -gt 1 ]; then
        realization_note+=", and no shift after the first is a multiple of the $decimation-sample decimation"
      fi
      realization_note+="), the same one for every build in a repeat"
    fi
  fi
fi

summary="$out/summary.tsv"
analog_keys=(tone_snr_db inband_db clip audible_ms first_audible_ms rms_dbfs)
probe_keys=(hz dbfs dbc)
printf 'variant\tcase\trep\terrs\tvoice\tsync' > "$summary"
# shift is last, so readers of the earlier columns by position still find them.
printf '\t%s' "${analog_keys[@]}" probe_hz probe_dbfs probe_dbc \
  tone tone_lock_ms tone_lock_pct rc off_path shift >> "$summary"
printf '\n' >> "$summary"

# Value of key=value on the last (or, with which=first, the first) line of the log
# that starts with prefix, or NA. The line is split on spaces, so no value may
# contain one. tone= (a label such as 151.4 or D023N/D047I), tone_lock_ms=
# (stream time of the first lock) and tone_lock_pct= (share of the delivered
# audio the tone was locked for) follow the contract in
# tests/engine/analog_replay.c's file comment; a host that predates a field
# reads NA, like every analog column of a digital run.
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
echo "realizations: $realization_note"
echo "results: $out"

for r in $(seq 1 "$reps"); do
  for ((k = 0; k < nbuilds; k++)); do
    build="${builds[$(((k + r - 1) % nbuilds))]}"
    name=$(basename "$build")
    log="$out/${name}__r${r}.log"
    set +e
    timeout 900 "$build" --frontend none "${mode_args[@]}" \
      --iq-replay "${replays[r]}" --iq-replay-rate "$rate" -o null > "$log" 2>&1
    rc=$?
    set -e
    # The log is read as text (-a) whatever it holds: a D-STAR header decoded from noise prints its callsigns as
    # raw bytes, which a UTF-8 grep takes for a binary file, and it then stops printing lines at the first of them.
    errs=$(grep -a -oE 'Total audio errors: [0-9]+' "$log" | tail -1 | grep -oE '[0-9]+$' || true)
    # Voice frames: lines that label one "Voice", or a ProVoice frame's " VOICE" after its sync, trunked (PV)
    # or conventional (PV_C, which prints its addresses in between), or a D-STAR sync, voice or header, each of
    # which opens a superframe of 21 AMBE frames. Startup notices that mention voice (the EDACS/ProVoice presets
    # print two) are not frames.
    voice=$(grep -a -v '^NOTICE:' "$log" | grep -a -cE 'Voice|Sync: [+-]PV(_C)? .*VOICE|Sync: [+-]DSTAR (VOICE|HEADER)' || true)
    sync=$(grep -a -cE 'Sync: ' "$log" || true)
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
      printf '\t%s' "${analog[@]}" "$rc" "$off_path" "${shifts[r]}"
      printf '\n'
    } >> "$summary"
    exit_note=$([ "$rc" -ne 0 ] && echo " (exit $rc)" || true)
    if [ "$off_path" -eq 1 ]; then
      exit_note="$exit_note (off the monitor path)"
    fi
    if [ "${same_as[r]}" -ne "$r" ]; then
      exit_note="$exit_note (same realization as r${same_as[r]})"
    fi
    if [ "$metric" = analog ]; then
      # analog[] follows the summary header: snr inband clip audible first rms probe_hz probe_dbfs ...
      printf '  r%-3s %-24s shift=%-5s snr=%-8s inband=%-8s audible_ms=%-9s rms=%-8s probe_dbfs=%-8s%s\n' \
        "$r" "$name" "${shifts[r]}" "${analog[0]}" "${analog[1]}" "${analog[3]}" "${analog[5]}" "${analog[7]}" \
        "$exit_note"
    else
      printf '  r%-3s %-24s shift=%-5s errs=%-6s voice=%-5s sync=%-5s%s\n' \
        "$r" "$name" "${shifts[r]}" "${errs:-NA}" "$voice" "$sync" "$exit_note"
    fi
  done
done

echo
echo "wrote $summary"
echo "report with: $ROOT_DIR/tools/replay_ab_report.py --metric $metric $summary"
