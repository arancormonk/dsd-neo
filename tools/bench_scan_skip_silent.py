# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

"""Manual benchmark of the scan skip-silent setting against a paced loopback rtl_tcp scene.

The scene is --silent channels that carry receiver noise only and three active channels on finite call schedules:
NFM voice (a 1 kHz tone at 1.5 kHz deviation), a simplex DMR burst train (MS voice bursts, 27.5 ms every 60 ms) and
P25 Phase 1 C4FM voice. Each active channel makes a 4 s call every 20 s, a third of that period after the previous
channel's, and is silent between calls; --jitter moves each call's start by up to that many seconds either way, so a
scan's rotation cannot lock onto the schedule's period, and --seed sets the schedule's phase, the moves and the noise.
Channels sit 2 MHz apart, so the receiver's capture holds one channel at a time. The calls are the reception
calibration's generators (tools/scan_quiet_reception.py) at an S/N over the noise in a 12.5 kHz channel: --level
strong is 30 dB, far above every mode's reception threshold; --level near is 2 dB above each mode's threshold (NFM 9.5,
DMR MS 7.5 and P25 8.0 dB, the calibration's kept thresholds).

The scene runs on tools/bench_signal_search_tcp.py's paced rtl_tcp source, logging every retune. The tool runs the
real dsd-neo against it for --seconds with --scan-skip-silent or --no-scan-skip-silent, --scan-max-visit-ms 8000 and
one of three scans: --trunk-scan conventional targets (--scan trunk), a typed -Y channel map (--scan typed) or an
untyped -Y channel map under -ft (--scan untyped, which has no analog decoder, so it cannot hear the NFM calls). Silent
channels take the three modes in turn. NFM rows and targets are 12.5 kHz wide with a -42 dB squelch, 5 to 6 dB over
the scene's noise in the channel. It reads the retune log, dsd-neo's output (each line stamped as it arrives), its
frame log, its per-call recordings and their journal, and its RTL perf CSV, and reports:

  idle visits   time per visit to a silent channel, from the retune onto it to the next retune (median, p95); the
                first visit (start-up) and the last (cut off) are left out
  skips         the skips dsd-neo logged, and of those the skips of an active channel whose call was on air from the
                landing to the skip (a call read as silent) or started between them (too late for the reading)
  calls         scheduled calls inside the run, caught (one frame decoded: a P25 LDU whose link control or encryption
                sync decoded, a DMR AMBE frame whose Golay-protected bits match the generator's; NFM: a call recording
                holding the tone) or missed, and how many the receiver visited at all
  retention     per caught call, frames decoded over frames sent while the receiver sat on the channel; NFM: seconds
                of the tone recorded over the call's seconds the receiver sat on the channel
  CPU           the demodulator's busy time (full_demod, metrics and output write, from the RTL perf CSV) and the
                whole process's CPU time, each per second of run
  drops         the input ring drops the RTL stream counted (a retune that discards a stale block counts too)

Usage, from the repository root:
    python3 tools/bench_scan_skip_silent.py --dsd build/dev-release/apps/dsd-cli/dsd-neo \\
        --dmr-synth-gen build/dev-debug/tests/dsd-neo_test_dmr_synth_gen --scan typed --skip on --jitter 3 --out DIR
    python3 tools/bench_scan_skip_silent.py --summary DIR... (one table over earlier runs' result.json files)
    python3 tools/bench_scan_skip_silent.py --rescore --dmr-synth-gen ... --out DIR (score a run again)

A frontend (the Qt or Android app) is measured with --serve PORT --skip on|off --out DIR: the tool serves the scene
on that loopback port for --seconds, to one client after another, and writes the lists (trunk.csv is the scan list's
target CSV) and the retunes to DIR. Then put the app's logcat (adb logcat -v epoch) in a file, its recording journal
in DIR/calls.jsonl and its recordings in DIR/calls/, and score the run with --analyze LOGCAT --out DIR: its digital
calls are scored by their recordings' length, as it writes no frame log. Requires NumPy. Never opens physical
hardware, and serves the loopback interface only.
"""

import argparse
import collections
import json
import math
import os
import re
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bench_signal_search_tcp as tcp_bench
import build_iq_fixtures as bif
import scan_quiet_reception as reception

BASE_HZ = 451000000
SPACING_HZ = 2000000
CALL_S = 4.0
PERIOD_S = 20.0
NOISE_SIGMA = 4.0  # per I/Q component, in cu8 steps
BLOCK = 8192
CLASS_WIDTH_HZ = 12500
NFM_DEVIATION_HZ = 1500.0
NFM_SQUELCH_DB = -42
THRESHOLD_DB = {"nfm": 9.5, "dmr": 7.5, "p25": 8.0}
STRONG_DB = 30.0
NEAR_MARGIN_DB = 2.0
ACTIVE = ("nfm", "dmr", "p25")
P25_LDU_S = 0.18
DMR_FRAMES_PER_BURST = 3
SCHEDULE_PHASE_S = 7.3  # the schedule's shift per seed, in seconds (mod PERIOD_S)
SCHEDULE_S = 86400.0  # calls are scheduled for a day of scene time
LATE_S = 2.0  # how long after a call's end its decoded frames may still arrive
TONE_SHARE = 0.5  # a 20 ms block of NFM audio is the call's tone when the tone holds this share of its power
ANSI = re.compile(r"\x1b\[[0-9;]*m")
STAMPED = re.compile(r"^(\d+\.\d+) (.*)$")
SKIP = re.compile(r"NOTICE: scan: skipped quiet .* (\d+\.\d+) MHz: ")
P25_FRAME = re.compile(reception.P25P1_PASS)
DMR_SYSTEM = "DMR_CC_2"  # the recording journal's system names for the scene's DMR and P25 calls
P25_SYSTEM = "P25_293"
AMBE = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) FRAME AMBE slot=\d+ data=([0-9A-F]{14})")


class Channel:
    def __init__(self, index, kind):
        self.index = index
        self.kind = kind  # "silent", "nfm", "dmr" or "p25"
        self.frequency = BASE_HZ + index * SPACING_HZ
        self.starts: np.ndarray | None = None  # call start times, scene time s
        self.snr_db: float | None = None
        self.baseband: np.ndarray | None = None  # one call at 48 kHz, scaled to its S/N over unit noise in 12.5 kHz
        self.frames: list[tuple[float, float]] = []  # (start_s, end_s) of each frame inside a call
        self.expected: list[str] | None = None  # DMR: the AMBE frames a call sends
        self.cache: dict[int, np.ndarray] = {}  # stream rate -> the call at that rate

    @property
    def name(self):
        return f"{self.kind}-{self.index + 1:02d}"

    def calls(self, end_s):
        """Start times of the calls that end inside the run."""
        if self.starts is None:
            return []
        return [float(t) for t in self.starts if t + CALL_S <= end_s]

    def call_at(self, t):
        """The start of the call on air at scene time t, or None."""
        if self.starts is None:
            return None
        k = int(np.searchsorted(self.starts, t, side="right")) - 1
        return float(self.starts[k]) if k >= 0 and t < self.starts[k] + CALL_S + 0.1 else None

    def at_rate(self, rate):
        if rate not in self.cache:
            call = bif.resample_to_fixture_rate(self.baseband, bif.SAMPLE_RATE_HZ, rate)
            self.cache[rate] = call.astype(np.complex64)
        return self.cache[rate]


def dmr_burst_period(burst):
    """The 30 ms period an MS voice burst occupies (the LC header first, then six bursts a superframe)."""
    superframe, vc = divmod(burst, 6)
    return reception.dmr_ms_voice_period(superframe, vc + 1)


def dmr_call(channel, work_dir):
    """The MS burst train's first CALL_S and the AMBE frames of the bursts that end inside it."""
    source = reception.dmr_ms_source(work_dir)
    symbol_s = 1.0 / 4800

    def burst_span(burst):
        first = reception.DMR_LEAD_SYMBOLS + dmr_burst_period(burst) * reception.DMR_SLOT_SYMBOLS
        return first * symbol_s, (first + reception.DMR_SLOT_SYMBOLS) * symbol_s

    bursts = [b for b in range(len(source.expected) // DMR_FRAMES_PER_BURST) if burst_span(b)[1] <= CALL_S]
    channel.frames = [burst_span(b) for b in bursts for _ in range(DMR_FRAMES_PER_BURST)]
    channel.expected = source.expected[: len(channel.frames)]
    return source.samples, float(np.mean(np.abs(source.samples[source.on]) ** 2))  # burst-on power


def schedule(seed, k, jitter_s):
    """Active channel k's call starts: every PERIOD_S, a third of it after the previous channel's, each moved by up to
    jitter_s either way. The seed sets the schedule's phase and the moves, so realizations differ."""
    rng = np.random.default_rng([seed, k])
    first = 2.0 + jitter_s + (seed * SCHEDULE_PHASE_S + k * PERIOD_S / len(ACTIVE)) % PERIOD_S
    count = int(SCHEDULE_S / PERIOD_S)
    return first + PERIOD_S * np.arange(count) + rng.uniform(-jitter_s, jitter_s, count)


def build_scene(silent, level, seed, jitter_s, work_dir):
    """The channels in frequency order, the three active ones spread among the silent ones."""
    count = silent + len(ACTIVE)
    kinds = ["silent"] * count
    for k, kind in enumerate(ACTIVE):
        kinds[(k * count) // len(ACTIVE) + count // (2 * len(ACTIVE))] = kind
    channels = [Channel(i, kind) for i, kind in enumerate(kinds)]
    n = round(CALL_S * bif.SAMPLE_RATE_HZ)
    for k, kind in enumerate(ACTIVE):
        channel = next(c for c in channels if c.kind == kind)
        channel.starts = schedule(seed, k, jitter_s)
        channel.snr_db = STRONG_DB if level == "strong" else THRESHOLD_DB[kind] + NEAR_MARGIN_DB
        if kind == "nfm":
            samples, power = reception.nfm_source(NFM_DEVIATION_HZ).samples, 1.0
        elif kind == "dmr":
            samples, power = dmr_call(channel, work_dir)
        else:
            samples = reception.p25p1_source().samples
            power = float(np.mean(np.abs(samples[:n]) ** 2))
            channel.frames = [(f * P25_LDU_S, (f + 1) * P25_LDU_S) for f in range(int(CALL_S / P25_LDU_S))]
        channel.baseband = samples[:n] / math.sqrt(power) * 10.0 ** (channel.snr_db / 20.0)
    return channels


def scene_channels(silent, level, seed, jitter_s):
    """build_scene with the DMR generator's work files in a temporary folder that is removed afterwards."""
    with tempfile.TemporaryDirectory(prefix="scene-") as work_dir:
        return build_scene(silent, level, seed, jitter_s, work_dir)


class Scene(tcp_bench.Source):
    """tools/bench_signal_search_tcp.py's paced rtl_tcp source, serving the scene and logging every retune."""

    def __init__(self, channels, seed, port=0, keep_serving=False):
        super().__init__(port)
        self.channels = channels
        self.keep_serving = keep_serving
        self.tunes = []  # (scene time s, monotonic s, centre Hz, rate Hz)
        self.started = None
        self.sample_time = 0.0
        self.sample_index = 0
        self.rng = np.random.default_rng(seed)

    def serve(self):
        if not self.keep_serving:
            super().serve()
            return
        self.socket.settimeout(1)
        while not self.stop.is_set():
            try:
                connection, _ = self.socket.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            with connection:
                connection.settimeout(1)
                connection.sendall(b"RTL0" + struct.pack("!II", 1, 1))  # the paced source's dongle info
                reader = threading.Thread(target=self.commands, args=(connection,), daemon=True)
                reader.start()
                try:
                    self.stream(connection)
                except OSError:
                    pass  # the client went away; wait for the next one
                reader.join(timeout=2)

    def block(self, centre, rate):
        """One block of the scene at this centre and rate: the noise plus every call in the capture."""
        iq = self.rng.standard_normal(2 * BLOCK, dtype=np.float32).view(np.complex64) * np.float32(NOISE_SIGMA)
        t0 = self.sample_time
        scale = np.float32(math.sqrt(2.0 * NOISE_SIGMA**2 * CLASS_WIDTH_HZ / rate))
        for channel in self.channels:
            delta = channel.frequency - centre
            start = channel.call_at(t0)
            if start is None or abs(delta) > 0.45 * rate:
                continue
            into = t0 - start
            call = channel.at_rate(rate)
            first = round(into * rate)
            if first >= len(call):
                continue
            piece = call[first : first + BLOCK]
            n = np.arange(self.sample_index, self.sample_index + len(piece)) % rate
            shift = np.exp(2j * np.pi * (delta / rate) * n).astype(np.complex64)
            iq[: len(piece)] += piece * shift * scale
        out = np.empty(2 * BLOCK, dtype=np.float32)
        out[::2], out[1::2] = iq.real + 127.5, iq.imag + 127.5
        return np.clip(np.rint(out), 0, 255).astype("u1").tobytes()

    def stream(self, connection):
        if self.started is None:
            self.started = time.monotonic()
        deadline = time.monotonic()
        tuned = None
        while not self.stop.is_set():
            rate, centre = self.rate, self.frequency
            if (centre, rate) != tuned:
                self.tunes.append((self.sample_time, time.monotonic(), centre, rate))
                tuned = (centre, rate)
            connection.sendall(self.block(centre, rate))
            self.sample_index += BLOCK
            self.sample_time += BLOCK / rate
            deadline += BLOCK / rate
            self.stop.wait(max(0, deadline - time.monotonic()))


# ---------------------------------------------------------------- channel lists and the run


def write_lists(channels, out_dir):
    """The target CSV (--scan trunk) and the typed and untyped channel maps (-Y). Silent channels take the modes in
    turn, so every mode has silent rows."""
    nfm_options = f"--nfm-bandwidth-hz {CLASS_WIDTH_HZ} --squelch-db {NFM_SQUELCH_DB}"
    target_type = {"nfm": "nfm-conventional", "dmr": "dmr-conventional", "p25": "p25-conventional"}
    paths = {key: os.path.join(out_dir, key + ".csv") for key in ("trunk", "typed", "untyped")}
    with open(paths["trunk"], "w", encoding="ascii") as trunk, open(paths["typed"], "w", encoding="ascii") as typed:
        trunk.write("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation,rtl_gain,options\n")
        typed.write("channel,frequency_hz,name,mode,options\n")
        for c in channels:
            kind = c.kind if c.kind != "silent" else ACTIVE[c.index % len(ACTIVE)]
            modulation = "c4fm" if kind == "p25" else ""
            options = nfm_options if kind == "nfm" else ""
            trunk.write(f"{c.name},{target_type[kind]},{c.frequency},,,,,{modulation},,{options}\n")
            typed.write(f"{c.index + 1},{c.frequency},{c.name},{kind},{options}\n")
    with open(paths["untyped"], "w", encoding="ascii") as untyped:
        untyped.write("channel,frequency_hz,name\n")
        for c in channels:
            untyped.write(f"{c.index + 1},{c.frequency},{c.name}\n")
    return paths


def dsd_command(args, port, lists):
    cmd = [str(Path(args.dsd).resolve()), "--frontend", "none", "-i", f"rtltcp:127.0.0.1:{port}:{BASE_HZ}:0:0:48:0:1"]
    cmd += ["-o", "null", "--scan-max-visit-ms", "8000", "--frame-log", "frames.log"]
    cmd += ["-P", "-7", "calls", "--wav-journal", "calls.jsonl", "--analog-record-preroll-ms", "0"]
    cmd += ["--scan-skip-silent" if args.skip == "on" else "--no-scan-skip-silent"]
    if args.scan == "trunk":
        cmd += ["--trunk-scan", lists["trunk"]]
    elif args.scan == "typed":
        cmd += ["-Y", "-C", lists["typed"]]
    else:
        cmd += ["-ft", "-Y", "-C", lists["untyped"]]
    return cmd + args.extra


def stamp_lines(pipe, log):
    """Each line dsd-neo writes, prefixed with the monotonic time it arrived (the retune log's clock)."""
    for raw in pipe:
        text = ANSI.sub("", raw.decode("utf-8", "replace")).rstrip("\n")
        log.write(f"{time.monotonic():.3f} {text}\n")


def process_cpu_s(pid):
    """utime + stime of the process and all its threads, in seconds."""
    try:
        with open(f"/proc/{pid}/stat", encoding="ascii") as handle:
            fields = handle.read().rsplit(")", 1)[1].split()
    except OSError:
        return None
    return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")


def run(args):
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    channels = scene_channels(args.silent, args.level, args.seed, args.jitter)
    lists = write_lists(channels, out_dir)
    scene = Scene(channels, args.seed)
    scene.thread.start()
    cmd = dsd_command(args, scene.port, lists)
    env = dict(os.environ, DSD_NEO_RTL_PERF_CSV="1", DSD_NEO_RTL_PERF_INTERVAL_MS="1000")
    wall_offset = time.time() - time.monotonic()
    with open(os.path.join(out_dir, "dsd.log"), "w", encoding="utf-8") as log:
        proc = subprocess.Popen(cmd, cwd=out_dir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        reader = threading.Thread(target=stamp_lines, args=(proc.stdout, log), daemon=True)
        reader.start()
        started = time.monotonic()
        while proc.poll() is None and time.monotonic() - started < args.seconds:
            time.sleep(0.25)
        cpu_s, wall_s = process_cpu_s(proc.pid), time.monotonic() - started
        proc.terminate()
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        reader.join(timeout=5)
    end_s = scene.sample_time
    scene.close()
    record = {
        "command": cmd,
        "scan": args.scan,
        "skip": args.skip,
        "level": args.level,
        "seed": args.seed,
        "silent": args.silent,
        "jitter_s": args.jitter,
        "exit": proc.returncode,
        "wall_s": wall_s,
        "process_cpu_s": cpu_s,
        "load_average_end": os.getloadavg(),
        "scene_started": scene.started,
        "wall_offset_s": wall_offset,
        "end_s": end_s,
        "tunes": scene.tunes,
    }
    with open(os.path.join(out_dir, "run.json"), "w", encoding="utf-8") as handle:
        json.dump(record, handle, indent=1)
    return record, channels


# ---------------------------------------------------------------- scoring


def visits(record, channels):
    """(channel, start s, end s) per visit, in scene time; retunes off the scene's channels are not visits."""
    by_centre = {}
    out = []
    tunes = record["tunes"]
    for (t, _wall, centre, _rate), nxt in zip(tunes, tunes[1:] + [(record["end_s"], 0, 0, 0)]):
        channel = by_centre.get(centre)
        if channel is None:
            near = min(channels, key=lambda c: abs(c.frequency - centre))
            channel = by_centre.setdefault(centre, near if abs(near.frequency - centre) < SPACING_HZ / 2 else None)
        out.append((channel, t, nxt[0]))
    return out


def read_log(path, started):
    """dsd-neo's stamped lines as (scene time s, text)."""
    lines = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = STAMPED.match(line.rstrip("\n"))
            if match:
                lines.append((float(match.group(1)) - started, match.group(2)))
    return lines


def frame_log_ambe(path, record):
    """The frame log's AMBE frames as (scene time s, hex); its stamps are whole wall-clock seconds."""
    out = []
    if not os.path.exists(path):
        return out
    offset = record["wall_offset_s"] + record["scene_started"]
    with open(path, encoding="ascii", errors="replace") as handle:
        for line in handle:
            match = AMBE.match(line)
            if match:
                wall = time.mktime(time.strptime(match.group(1), "%Y-%m-%d %H:%M:%S"))
                out.append((wall - offset, match.group(2)))
    return out


def journal_recordings(path, record, systems):
    """Each published call recording of these systems (journal "system" names) as (scene time s of its start, seconds
    heard): for NFM the seconds of the call's tone in it, for a digital call its length. The journal's start is a whole
    second. A recording missing from the journal's folder is looked for in calls/ beside the journal (a frontend's
    recordings pulled from the device)."""
    out = []
    if not os.path.exists(path):
        return out
    offset = record["wall_offset_s"] + record["scene_started"]
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            try:
                entry = json.loads(line)
            except ValueError:
                continue
            if entry.get("type") != "publish" or entry.get("system") not in systems:
                continue
            if entry["system"] != "NFM":
                out.append((float(entry["start"]) - offset, float(entry["duration_ms"]) / 1000.0))
                continue
            wav_path = os.path.join(entry["folder"], os.path.basename(entry["file"]))
            if not os.path.exists(wav_path):
                wav_path = os.path.join(os.path.dirname(path), "calls", os.path.basename(entry["file"]))
            out.append((float(entry["start"]) - offset, tone_seconds(wav_path)))
    return out


def tone_seconds(path):
    """Seconds of a recording whose 20 ms blocks are mostly the call's 1 kHz tone: the audio actually heard."""
    try:
        with wave.open(path, "rb") as wav:
            rate = wav.getframerate()
            audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float64)
            if wav.getnchannels() > 1:
                audio = audio.reshape(-1, wav.getnchannels())[:, 0]
    except (OSError, wave.Error, EOFError):
        return 0.0
    size = rate // 50
    blocks = audio[: len(audio) // size * size].reshape(-1, size)
    if len(blocks) == 0:
        return 0.0
    tone = np.exp(-2j * math.pi * bif.NFM_SYNTH_TONE_HZ * np.arange(size) / rate)
    tone_power = 2.0 * np.abs(blocks @ tone) ** 2 / size
    total = np.sum(blocks**2, axis=1)
    heard = (total > 0) & (tone_power > TONE_SHARE * np.maximum(total, 1e-12))
    return float(np.count_nonzero(heard)) * size / rate


def overlap(a0, a1, b0, b1):
    return max(0.0, min(a1, b1) - max(a0, b0))


def score_call(channel, start, on_channel, decoded, in_seconds):
    """Caught, frames sent while on the channel and frames decoded; in seconds instead when in_seconds (NFM, and a
    frontend's digital calls, which are scored by their recordings)."""
    if in_seconds:
        sent = sum(overlap(v0, v1, start, start + CALL_S) for v0, v1 in on_channel)
        heard = sum(length for t, length in decoded if start - 1.5 <= t <= start + CALL_S)
        return heard > 0.0, sent, min(heard, sent)
    sent = sum(1 for f0, f1 in channel.frames for v0, v1 in on_channel if v0 <= start + f0 and start + f1 <= v1)
    window = [x for t, x in decoded if start - 1.5 <= t <= start + CALL_S + LATE_S]
    if channel.kind == "dmr":
        got = sum(
            (collections.Counter(h[:6] for h in window) & collections.Counter(e[:6] for e in channel.expected)).values()
        )
    else:
        got = len(window)
    return got > 0, sent, min(got, sent)


def score(record, channels, out_dir):
    started = record["scene_started"]
    if started is None:
        raise SystemExit(f"dsd-neo never connected to the scene, so there is nothing to score (see {out_dir}/dsd.log)")
    lines = read_log(os.path.join(out_dir, "dsd.log"), started)
    all_visits = visits(record, channels)
    # Left out: the tunes off the scene before it (the source's own centre, when the stream started before dsd-neo's
    # first tune), the start-up visit before the first scan retune, and the last visit, which the run cut off.
    lead = next((i for i, (c, _v0, _v1) in enumerate(all_visits) if c is not None), len(all_visits))
    scored = all_visits[lead + 1 : -1]
    first_s = scored[0][1] if scored else record["end_s"]
    idle = [v1 - v0 for c, v0, v1 in scored if c is not None and c.kind == "silent"]
    journal = os.path.join(out_dir, "calls.jsonl")
    decoded = {
        "p25": [(t, 1) for t, text in lines if P25_FRAME.search(text)],
        "dmr": frame_log_ambe(os.path.join(out_dir, "frames.log"), record),
        "nfm": journal_recordings(journal, record, ("NFM",)),
    }
    in_seconds = {"nfm": True, "dmr": False, "p25": False}
    if record["scan"] == "frontend":  # no frame log and no decode lines: score its calls by their recordings
        decoded["dmr"] = journal_recordings(journal, record, (DMR_SYSTEM,))
        decoded["p25"] = journal_recordings(journal, record, (P25_SYSTEM,))
        in_seconds = dict.fromkeys(ACTIVE, True)
    calls = {}
    for channel in channels:
        if channel.kind == "silent":
            continue
        rows = []
        for start in channel.calls(record["end_s"]):
            if start < first_s:
                continue
            on = [(v0, v1) for c, v0, v1 in all_visits if c is channel and overlap(v0, v1, start, start + CALL_S) > 0]
            caught, sent, got = score_call(channel, start, on, decoded[channel.kind], in_seconds[channel.kind])
            rows.append({"start": start, "tuned": bool(on), "caught": caught, "sent": sent, "decoded": got})
        calls[channel.kind] = rows
    perf = perf_summary(os.path.join(out_dir, "dsd-neo-rtl-perf.csv"))
    skips = [(t, float(m.group(1)) * 1e6) for t, m in ((t, SKIP.search(text)) for t, text in lines) if m]
    result = {
        "scan": record["scan"],
        "skip": record["skip"],
        "level": record["level"],
        "seed": record["seed"],
        "run_s": record["wall_s"],
        "scene_s": record["end_s"],
        "jitter_s": record["jitter_s"],
        "load_average_end": record["load_average_end"],
        "idle_visits": len(idle),
        "idle_median_s": statistics.median(idle) if idle else None,
        "idle_p95_s": float(np.percentile(idle, 95)) if idle else None,
        "skips": len(skips),
        "skips_on_air": skips_on_air(skips, all_visits),
        "calls": calls,
        "process_cpu_pct": 100.0 * record["process_cpu_s"] / record["wall_s"] if record["process_cpu_s"] else None,
        **perf,
    }
    with open(os.path.join(out_dir, "result.json"), "w", encoding="utf-8") as handle:
        json.dump(result, handle, indent=1)
    return result


def skips_on_air(skips, visit_list):
    """Skips of an active channel with one of its calls on air: (on air from the landing to the skip, starting between
    them). The first would be a call read as silent; the second is a call that started during or after the reading,
    which the check cannot see (a documented limit)."""
    whole = started = 0
    for t, frequency in skips:
        landings = [v0 for c, v0, _v1 in visit_list if c is not None and c.frequency == round(frequency) and v0 <= t]
        channel = next((c for c, _v0, _v1 in visit_list if c is not None and c.frequency == round(frequency)), None)
        if channel is None or not landings or channel.kind == "silent":
            continue
        landed = landings[-1]
        for start in channel.calls(t + CALL_S):
            whole += start <= landed and t <= start + CALL_S
            started += landed < start < t
    return [whole, started]


def perf_summary(path):
    """The demodulator's busy time per second and the input drops, from the RTL perf CSV."""
    if not os.path.exists(path):
        return {"demod_busy_pct": None, "input_drops": None}
    with open(path, encoding="ascii") as handle:
        header = handle.readline().strip().split(",")
        rows = [dict(zip(header, line.strip().split(","))) for line in handle if line.strip()]
    rows = rows[1:]  # the first window holds the prebuffer
    if len(rows) < 2:
        return {"demod_busy_pct": None, "input_drops": None}
    span_ms = int(rows[-1]["time_ms"]) - int(rows[0]["time_ms"])
    busy_ns = sum(int(r["full_demod_ns"]) + int(r["post_metrics_ns"]) + int(r["output_write_ns"]) for r in rows[1:])
    return {
        "demod_busy_pct": 100.0 * busy_ns / (span_ms * 1e6) if span_ms > 0 else None,
        "input_drops": max(int(r["input_drops"]) for r in rows),
    }


# ---------------------------------------------------------------- summary


def call_totals(result, kind):
    rows = result["calls"].get(kind, [])
    caught = [r for r in rows if r["caught"]]
    ratios = [r["decoded"] / r["sent"] for r in caught if r["sent"] > 0]
    return len(caught), len(rows) - len(caught), statistics.median(ratios) if ratios else None, len(ratios)


def fmt(value, spec=".2f"):
    return "-" if value is None else format(value, spec)


def summary_table(results):
    lines = [
        (
            "| scan | level | jitter s | seed | skip | idle visits | idle median s | idle p95 s "
            "| skips (call on air / call starting) | NFM caught/missed (visited) retention "
            "| DMR caught/missed (visited) retention | P25 caught/missed (visited) retention "
            "| demod busy % | process CPU % | drops |"
        ),
        "|" + "---|" * 15,
    ]
    for r in sorted(results, key=lambda r: (r["scan"], r["level"], -r["jitter_s"], r["seed"], r["skip"])):
        cells = [r["scan"], r["level"], fmt(r["jitter_s"], "g"), str(r["seed"]), r["skip"], str(r["idle_visits"])]
        cells += [
            fmt(r["idle_median_s"]),
            fmt(r["idle_p95_s"]),
            f"{r['skips']} ({r['skips_on_air'][0]}/{r['skips_on_air'][1]})",
        ]
        for kind in ACTIVE:
            caught, missed, retention, _n = call_totals(r, kind)
            visited = sum(1 for row in r["calls"].get(kind, []) if row["tuned"])
            cells.append(f"{caught}/{missed} ({visited}) {fmt(retention)}")
        cells += [fmt(r["demod_busy_pct"]), fmt(r["process_cpu_pct"]), fmt(r["input_drops"], "d")]
        lines.append("| " + " | ".join(cells) + " |")
    return "\n".join(lines)


# ---------------------------------------------------------------- serving a frontend


def serve(args):
    """Serve the scene to whatever connects, for --seconds, rewriting OUT_DIR/run.json (the retunes so far) every
    second; --analyze scores it afterwards."""
    out_dir = os.path.abspath(args.out)
    os.makedirs(out_dir, exist_ok=True)
    channels = scene_channels(args.silent, args.level, args.seed, args.jitter)
    write_lists(channels, out_dir)
    scene = Scene(channels, args.seed, port=args.serve, keep_serving=True)
    scene.thread.start()
    print(f"serving the scene on 127.0.0.1:{scene.port}; lists in {out_dir}", flush=True)
    record = {"scan": "frontend", "skip": args.skip, "level": args.level, "seed": args.seed, "silent": args.silent}
    record.update({"jitter_s": args.jitter, "wall_offset_s": time.time() - time.monotonic()})
    record.update({"process_cpu_s": None, "load_average_end": None})
    started = time.monotonic()
    try:
        while time.monotonic() - started < args.seconds:
            time.sleep(1.0)
            record.update({"scene_started": scene.started, "end_s": scene.sample_time, "tunes": list(scene.tunes)})
            record["wall_s"] = scene.sample_time
            with open(os.path.join(out_dir, "run.json.tmp"), "w", encoding="utf-8") as handle:
                json.dump(record, handle)
            os.replace(os.path.join(out_dir, "run.json.tmp"), os.path.join(out_dir, "run.json"))
    except KeyboardInterrupt:
        pass
    finally:
        scene.close()


def analyze(args, record, channels):
    """Score a frontend's run: its logcat (adb logcat -v epoch) becomes OUT_DIR/dsd.log, on the scene's clock. The
    emulator's clock is matched to the scene's by its "Retune applied" lines against the retunes the scene saw."""
    logcat = re.compile(r"^\s*(\d+\.\d+)\s+\d+\s+\d+\s+[VDIWEF]\s+dsd-neo\s*:\s?(.*)$")
    lines = []
    with open(args.analyze, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = logcat.match(line.rstrip("\n"))
            if match:
                lines.append((float(match.group(1)) - record["wall_offset_s"], match.group(2)))
    retunes = [(t, int(m.group(1))) for t, text in lines for m in [re.search(r"Retune applied: (\d+) Hz", text)] if m]
    gaps = []
    for t, frequency in retunes:
        near = [mono for _s, mono, centre, _r in record["tunes"] if abs(centre - frequency) < SPACING_HZ / 2]
        if near:
            gaps.append(t - min(near, key=lambda mono: abs(mono - t)))
    shift = statistics.median(gaps) if gaps else 0.0
    with open(os.path.join(args.out, "dsd.log"), "w", encoding="utf-8") as log:
        log.writelines(f"{t - shift:.3f} {text}\n" for t, text in lines)
    result = score(record, channels, os.path.abspath(args.out))
    print(f"clock shift {shift:+.3f} s from {len(gaps)} retunes")
    print(summary_table([result]))


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dsd", help="the dsd-neo binary (a Release build)")
    parser.add_argument("--dmr-synth-gen", help="dsd-neo_test_dmr_synth_gen, which makes the DMR call")
    parser.add_argument("--scan", choices=("trunk", "typed", "untyped"), default="typed")
    parser.add_argument("--skip", choices=("on", "off"), default="on")
    parser.add_argument("--level", choices=("strong", "near"), default="strong")
    parser.add_argument("--silent", type=int, default=12, help="silent channels (default 12)")
    parser.add_argument("--seconds", type=float, default=600.0, help="run length (default 600)")
    parser.add_argument("--seed", type=int, default=1, help="noise and schedule seed (default 1)")
    parser.add_argument(
        "--jitter", type=float, default=0.0, help="move each call's start by up to this many seconds (default 0)"
    )
    parser.add_argument("--out", help="directory for the lists, logs and results")
    parser.add_argument("--serve", type=int, metavar="PORT", help="serve the scene on this loopback port only")
    parser.add_argument("--summary", nargs="+", metavar="DIR", help="print one table over these runs' results")
    parser.add_argument("--rescore", action="store_true", help="score the run in --out again without running it")
    parser.add_argument("--analyze", metavar="LOGCAT", help="score a --serve run in --out from a frontend's logcat")
    parser.add_argument("extra", nargs="*", help="extra dsd-neo arguments, after --")
    args = parser.parse_args()
    if args.summary:
        return args
    if not args.out:
        parser.error("give --out")
    if not args.dmr_synth_gen:
        parser.error("give --dmr-synth-gen")
    if not (args.serve or args.rescore or args.analyze or args.dsd):
        parser.error("give --dsd, or --serve PORT")
    if args.silent < 1:
        parser.error("--silent must be at least 1")
    reception.ARGS = argparse.Namespace(dsd=args.dsd, dmr_synth_gen=args.dmr_synth_gen)
    return args


def main():
    args = parse_args()
    if args.summary:
        results = []
        for out_dir in args.summary:
            with open(os.path.join(out_dir, "result.json"), encoding="utf-8") as handle:
                results.append(json.load(handle))
        print(summary_table(results))
        return
    if args.serve:
        serve(args)
        return
    if args.rescore or args.analyze:
        with open(os.path.join(args.out, "run.json"), encoding="utf-8") as handle:
            record = json.load(handle)
        channels = scene_channels(record["silent"], record["level"], record["seed"], record["jitter_s"])
        if args.analyze:
            analyze(args, record, channels)
        else:
            print(summary_table([score(record, channels, os.path.abspath(args.out))]))
        return
    record, channels = run(args)
    result = score(record, channels, os.path.abspath(args.out))
    print(summary_table([result]))
    if record["exit"] not in (0, -15):
        raise SystemExit(f"dsd-neo exited {record['exit']}")


if __name__ == "__main__":
    main()
