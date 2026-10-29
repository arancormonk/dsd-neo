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
# compared across captures. Two repeats can also land every event in the same
# place, when the events sit at the end of the bytes replay reads, where they
# stop moving, or when two shifts coincide: the later repeat replays the same
# realization as the earlier one, records that repeat's shift so the report
# counts it once, and its progress lines say so. The run warns as for a capture
# with no events when every repeat collapses onto one, and names how many
# distinct realizations it has when some do. --no-realizations replays every
# repeat as recorded on purpose: that is the determinism control. Two
# properties this keeps from before issue #572, which cost nothing now and keep
# a determinism regression from passing for a difference in builds:
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
# TETRA metric (--metric tetra --tetra-wanted MCC/MNC/CC): counts what each run printed, in these summary.tsv columns
# after shift. Voice-frame errors say little about a TETRA receive chain that decodes weak signals, so the columns are
# the decoded content itself, each from CRC- or FEC-gated lines:
#
#   wanted_bsch    BSCH identity lines (CRC-gated) equal to --tetra-wanted
#   other_bsch     BSCH identity lines of another, well-formed identity
#   unknown_bsch   "[TETRA BSCH] MCC=" lines whose MCC, MNC and CC do not parse; never counted as either of the above
#   tch_ok         accepted speech: "[TETRA TCH/FS] ... dec_bits=274 crc=ok mismatches=m" with m <= 28
#                  (TETRA_TCH_MAX_MISMATCH in src/protocol/tetra/tetra.c)
#   tch_bad        every other TCH/FS line: a bad CRC, more mismatches, another bit count or an unparsable line
#   aach_fec       bursts whose AACH decoded ("AACH a/b/c]", not "AACH ?]")
#   first_wanted_s the first wanted BSCH's time from the capture's start, in whole seconds, or NA
#   completed      1 when the replay printed "IQ replay: end of capture after <N> samples", else 0; a run without it
#                  stopped early or lost its reader, which is not a run that decoded nothing
#   samples        N from that notice, or NA
#   wanted         the --tetra-wanted identity as MCC/MNC/0xCC, repeated per row so the report can check that one
#                  identity was fixed for every build
#
# An identity is never promoted by how often it occurs: a capture dominated by another network reads as
# wanted_bsch 0 and a large other_bsch. first_wanted_s uses the decode clock's HH:MM:SS stamp of the line that
# printed the BSCH (a replay's stamps start at the capture's capture_started_utc, or 2000-01-01T00:00:00Z when it has
# none, a WAV's start time, or no usable one), less the capture's start, in the local time zone both use. Its
# resolution is therefore one second (a stamp is the whole second the BSCH fell in), and it assumes the capture
# starts and ends within one day and one UTC offset.
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
#   --capture <path>   I/Q capture to replay (required): a sidecar JSON, or a WAV recording. A WAV is replayed
#                      as it is, one realization: it has no events to shift
#   --mode <flags>     Decoder flags, e.g. "-fi" (required; quote if several)
#   --reps <n>         Repeats per build, each its own realization of the
#                      capture (default 12)
#   --no-realizations  Replay every repeat as recorded: the determinism
#                      control, where every repeat must decode alike
#   --rate <mode>      --iq-replay-rate value: realtime (default) or fast
#   --metric <kind>    digital (default), analog or tetra. analog also scores the host's
#                      ANALOG METRIC / ANALOG PROBE lines. Give the host no
#                      --analog-* bounds: summary.tsv records each run's exit
#                      status, and the report leaves non-zero ones out. tetra
#                      counts what a TETRA decode (run it with -fT -Z) printed:
#                      see the TETRA metric below
#   --tetra-wanted <MCC/MNC/CC>
#                      The network and colour code the capture carries
#                      (--metric tetra, required there), e.g. 228/8889/0x01 or
#                      228/8889/1. Fix it for every build in a comparison
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
tetra_wanted=""
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
    --tetra-wanted)
      tetra_wanted="${2:-}"
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
if [ "$metric" != digital ] && [ "$metric" != analog ] && [ "$metric" != tetra ]; then
  echo "error: --metric must be digital, analog or tetra, not '$metric'" >&2
  usage 1
fi
if [ "$metric" = tetra ]; then
  if [ -z "$tetra_wanted" ]; then
    echo "error: --metric tetra needs --tetra-wanted MCC/MNC/CC" >&2
    usage 1
  fi
  if ! command -v python3 > /dev/null 2>&1; then
    echo "error: --metric tetra needs python3" >&2
    exit 1
  fi
  # Normalized to MCC/MNC/0xCC, the form summary.tsv records.
  if ! tetra_wanted=$(
    python3 - "$tetra_wanted" << 'PY'
import re
import sys

text = sys.argv[1]
match = re.fullmatch(r"([0-9]{1,4})/([0-9]{1,5})/(0[xX][0-9a-fA-F]{1,2}|[0-9]{1,2})", text)
if match:
    mcc, mnc = int(match.group(1)), int(match.group(2))
    cc = int(match.group(3), 16 if match.group(3)[:2] in ("0x", "0X") else 10)
    if mcc <= 1023 and mnc <= 16383 and cc <= 63:
        print(f"{mcc}/{mnc}/0x{cc:02x}")
        sys.exit(0)
sys.exit(1)
PY
  ); then
    echo "error: --tetra-wanted must be MCC/MNC/CC (MCC 0-1023, MNC 0-16383, CC 0-63, decimal or 0x hex), not" \
      "'$tetra_wanted'" >&2
    exit 1
  fi
elif [ -n "$tetra_wanted" ]; then
  echo "error: --tetra-wanted only applies to --metric tetra" >&2
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
# A WAV recording (RIFF, RF64 or the big-endian RIFX) is replayed as it is. Replay reads its header, not a sidecar.
capture_is_wav=0
case "$(head -c 4 "$capture" 2> /dev/null | tr -d '\0')" in
  RIFF | RF64 | RIFX) capture_is_wav=1 ;;
esac
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
# held at the end of the bytes replay reads stop moving, and two shifts can coincide, so two repeats can give one
# schedule, which is one realization.
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
elif [ "$capture_is_wav" -eq 1 ]; then
  echo "warning: a WAV capture has no events to shift: every repeat replays the same realization; compare it across" \
    "captures" >&2
  realization_note="none: a WAV capture is one realization, so every repeat replays it as recorded"
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
        echo "notice: $((reps - distinct)) of $reps repeats replay the same event schedule as an earlier repeat" \
          "(their progress lines name it), so the run has $distinct distinct realizations; events at the end of" \
          "the bytes replay reads, or shifts that coincide, do that" >&2
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
if [ "$metric" = tetra ]; then
  printf '\t%s' wanted_bsch other_bsch unknown_bsch tch_ok tch_bad aach_fec first_wanted_s completed samples \
    wanted >> "$summary"
fi
printf '\n' >> "$summary"

# The decode clock's start in the local time zone, as seconds into the day: replay stamps its lines with the
# capture's start time plus the capture time elapsed, as HH:MM:SS local time (src/engine/engine.c, anchor).
tetra_origin_sod() {
  local start=""
  if [ "$capture_is_wav" -eq 1 ]; then
    # What the first build reads from the recording's auxi chunk or its name.
    start=$("${builds[0]}" --iq-info "$capture" 2> /dev/null | sed -n 's/^  Start time: *\([0-9TZ:-]*\) (.*/\1/p' |
      head -n 1 || true)
  else
    start=$(python3 -c 'import json, sys
try:
    with open(sys.argv[1], encoding="utf-8", errors="replace") as handle:
        value = json.load(handle).get("capture_started_utc", "")
    print(value if isinstance(value, str) else "")
except (OSError, ValueError, AttributeError):
    print("")' "$capture" 2> /dev/null || true)
  fi
  python3 - "$start" << 'PY'
import calendar
import sys
import time

FLOOR, CEILING = 946684800, 18000000000
anchor = 0
try:
    anchor = calendar.timegm(time.strptime(sys.argv[1], "%Y-%m-%dT%H:%M:%SZ"))
except ValueError:
    pass
anchor = min(max(anchor, FLOOR), CEILING)
local = time.localtime(anchor)
print(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec)
PY
}

# Prints the TETRA metric fields of one log, tab separated, in summary.tsv's order from wanted_bsch to samples.
# Arguments: log, wanted identity (MCC/MNC/0xCC), origin (seconds into the day the capture starts at).
tetra_log_metrics() {
  python3 - "$@" << 'PY'
import bisect
import re
import sys

log, wanted, origin = sys.argv[1], sys.argv[2], int(sys.argv[3])
mcc, mnc, cc = wanted.split("/")
wanted_id = (int(mcc), int(mnc), int(cc, 16))
MAX_MISMATCH = 28  # TETRA_TCH_MAX_MISMATCH, src/protocol/tetra/tetra.c

with open(log, "rb") as handle:
    text = handle.read().decode("latin-1")

# A decode-clock stamp opens each "Sync:" line. A BSCH belongs to the last stamp before it.
stamps = [(m.start(), int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)))
          for m in re.finditer(r"(?<![0-9:])([0-9]{2}):([0-9]{2}):([0-9]{2}) Sync:", text)]
stamp_pos = [pos for pos, _ in stamps]

BSCH = re.compile(r"\[TETRA BSCH\] MCC=([0-9]+) MNC=([0-9]+) CC=0x([0-9a-fA-F]+)(?=[ \n]|$)")
n_wanted = n_other = n_unknown = 0
first = None
for m in re.finditer(r"\[TETRA BSCH\] MCC=", text):
    full = BSCH.match(text, m.start())
    if not full:
        n_unknown += 1
        continue
    if (int(full.group(1)), int(full.group(2)), int(full.group(3), 16)) == wanted_id:
        n_wanted += 1
        if first is None:
            index = bisect.bisect_right(stamp_pos, m.start()) - 1
            if index >= 0:
                first = (stamps[index][1] - origin) % 86400
    else:
        n_other += 1

TCH = re.compile(r"\[TETRA TCH/FS\] CC=-?[0-9]+ +dec_bits=([0-9]+) +crc=(ok|bad) +mismatches=([0-9]+)")
tch_ok = tch_bad = 0
for m in re.finditer(r"\[TETRA TCH/FS\]", text):
    full = TCH.match(text, m.start())
    if full and full.group(1) == "274" and full.group(2) == "ok" and int(full.group(3)) <= MAX_MISMATCH:
        tch_ok += 1
    else:
        tch_bad += 1

aach = len(re.findall(r"AACH [0-9]+/[0-9]+/[0-9]+\]", text))
ends = re.findall(r"IQ replay: end of capture after ([0-9]+) samples", text)
print("\t".join([str(n_wanted), str(n_other), str(n_unknown), str(tch_ok), str(tch_bad), str(aach),
                 "NA" if first is None else str(first), "1" if ends else "0", ends[-1] if ends else "NA"]))
PY
}

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
tetra_origin=0
if [ "$metric" = tetra ]; then
  tetra_origin=$(tetra_origin_sod)
fi
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
    # which opens a superframe of 21 AMBE frames, or a DMR voice burst ("| VC1".."| VC6", "| VC*" for a
    # bootstrapped one), either slot. Startup notices that mention voice (the EDACS/ProVoice presets print two) are
    # not frames, and neither is a DMR Connect Plus activity update naming a "Group Voice" on a timeslot.
    voice=$(grep -a -v '^NOTICE:' "$log" | grep -a -v 'Activity Update' |
      grep -a -cE 'Voice|Sync: [+-]PV(_C)? .*VOICE|Sync: [+-]DSTAR (VOICE|HEADER)|Sync: [+-]DMR .*\| VC[1-6*]' || true)
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
    tetra_fields=""
    if [ "$metric" = tetra ]; then
      tetra_fields=$(tetra_log_metrics "$log" "$tetra_wanted" "$tetra_origin")
    fi
    {
      printf '%s\t%s\t%s\t%s\t%s\t%s' \
        "$name" "$(basename "$capture")-$rate" "$r" "${errs:-NA}" "$voice" "$sync"
      printf '\t%s' "${analog[@]}" "$rc" "$off_path" "${shifts[r]}"
      if [ "$metric" = tetra ]; then
        printf '\t%s\t%s' "$tetra_fields" "$tetra_wanted"
      fi
      printf '\n'
    } >> "$summary"
    exit_note=$([ "$rc" -ne 0 ] && echo " (exit $rc)" || true)
    if [ "$off_path" -eq 1 ]; then
      exit_note="$exit_note (off the monitor path)"
    fi
    if [ "${same_as[r]}" -ne "$r" ]; then
      exit_note="$exit_note (same realization as r${same_as[r]})"
    fi
    if [ "$metric" = tetra ]; then
      IFS=$'\t' read -r t_wanted t_other t_unknown t_ok t_bad t_aach t_first t_done _ <<< "$tetra_fields"
      # No end-of-capture notice: the replay stopped early or lost its reader, which is not a run that decoded nothing.
      if [ "$t_done" = 0 ]; then
        exit_note="$exit_note (incomplete: no end-of-capture notice)"
      fi
      printf '  r%-3s %-24s shift=%-5s wanted=%-5s other=%-5s unknown=%-4s ' \
        "$r" "$name" "${shifts[r]}" "$t_wanted" "$t_other" "$t_unknown"
      printf 'tch_ok=%-5s tch_bad=%-5s aach=%-5s first=%-5s%s\n' "$t_ok" "$t_bad" "$t_aach" "$t_first" "$exit_note"
    elif [ "$metric" = analog ]; then
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
