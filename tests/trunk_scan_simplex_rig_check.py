# SPDX-License-Identifier: GPL-3.0-or-later
"""A real --trunk-scan of simplex digital targets on a tunable input at 72 kHz (issue #890).

The paced rtl_tcp scene of tools/bench_scan_skip_silent.py (its Scene, with channels of this script's own) serves five
channels 2 MHz apart, one target each, which a target CSV lists in this order:

  nfm      nfm-conventional        the nfm_tone_synth call, twice over (3 s of tone), from the first landing on it
  nxdn48   nxdn48-conventional     receiver noise only
  ysf      ysf-conventional        the ysf_synth call, twice over (4 s of V/D mode 2 frames), from the first landing
  dmo      tetra-dmo-conventional  the tetra_dmo_call_synth_72k direct mode call (3.8 s), from the first landing on it
  tmo      tetra-dmo-conventional  trunked (TMO) TETRA only, tetra_bsch_synth_72k, on the air the whole run

A call starts when the receiver first tunes to its channel and plays once; after it the channel is noise. The decoder
runs the list at the 72 kHz DSP bandwidth (-i rtltcp:...:72), a TETRA rate, so the TETRA DMO targets can be visited,
with every target's dwell and activity hold DWELL_MS and HOLD_MS from the target CSV, over far longer session
defaults (SESSION_DWELL_MS, SESSION_HOLD_MS) a target that ignored its own would run, and a visit cap of VISIT_CAP_MS.
Its control API (--api) streams the metrics, the receive profile the front end and the decoder run. The run lasts
until every target has had two whole visits (at most --seconds). From the scene's own record of every retune (the
visits) and the API's lines, it checks:

  * every visit lands its target's own profile (ROWS): once the metrics show it, they show nothing else for the rest
    of the visit, and every visit long enough to read shows it. A digital target's profile is its symbol rate, slicer
    levels, rf_mod (0 C4FM, 1 CQPSK, 2 GFSK), channel profile and filter width and the decoder output rate; the analog
    target's is the FM monitor's width and output rate;
  * the nfm, ysf and dmo targets hold while their call plays: their first visit lasts at least the call, and ends
    within HOLD_MS, DWELL_MS and LEAVE_PAD_S of the call's end, short of the visit cap;
  * every later visit to them, and every visit to nxdn48 and tmo, is an idle one: it ends within DWELL_MS and
    LEAVE_PAD_S. A tmo hold would last to the visit cap, since its TETRA never stops;
  * the tmo target's TETRA is decoded all the same (its network's BSCH is printed), so it is heard and not held.

A visit's length is read in stream time (the scene's own sample count), its profile from the API lines between the
retune commands as they arrive (RigScene). That does not make the run independent of the runner: the decoder's dwell
and hold run on its real clock, and the paced source catches up after a blocked send, so a stalled decoder can see a
visit end early or late by about the stall. What keeps a slow runner passing is the slack: LEAVE_PAD_S on every bound,
an idle visit's bound (DWELL_MS and LEAVE_PAD_S, 5 s) far under the visit cap (15 s) a held trunked target would run to,
and a held visit's bound well under what the session defaults would give. The run's wall time is bounded by --seconds
and the CTest TIMEOUT. A visit's profile is judged only on the metrics read between the subscription and the API's
close, so a visit the subscription began late in or the run's end cut short is judged on what was read of it, and
every target needs a visit read for READABLE_S or more that showed its profile. The API is the decoder's own once it
says it listens on the port: a port another loopback test took first fails the decoder's bind, and the run starts
again on another one.

Usage: trunk_scan_simplex_rig_check.py DSD_NEO FIXTURE_DIR [--seconds S] [--keep DIR] [--explore]
Exit 0 on success, 1 on a failed check, 77 when numpy is missing (the rig needs it). Serves the loopback interface
only and never opens hardware.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

try:
    import numpy as np
except ImportError:
    print(f"SKIP: the scan rig needs numpy, which {sys.executable} does not have")
    sys.exit(77)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bench_scan_skip_silent as bench  # noqa: E402
import build_iq_fixtures as bif  # noqa: E402

BASE_HZ = 431000000
SPACING_HZ = 2000000
DSP_KHZ = 72
# The capture rate the RTL front end asks the source for at a 72 kHz DSP bandwidth (16 times it, 4 half-band passes).
CAPTURE_HZ = 1152000
DWELL_MS = 2000
HOLD_MS = 1000
# The session's own dwell and hold: a target that ran these in place of its CSV row's would fail every bound.
SESSION_DWELL_MS = 8000
SESSION_HOLD_MS = 6000
VISIT_CAP_MS = 15000
SNR_DB = 30.0
# A visit's length is read from the scene's retunes in stream time: the samples the paced source delivered. The decoder
# reads a little behind the stream, its dwell and hold run on its real clock, and its ticks, its tune completion and the
# source's blocks each add a little more: this much past a bound still passes. A held tmo visit would run to the visit
# cap, and a target on the session's dwell or hold past its bound by more than that.
LEAVE_PAD_S = 3.0
# The metrics lines read for a visit's profile: those that came this long after its retune command arrived, and up to
# END_GUARD_S before the next one, as the API's lines and the source's commands come on two sockets read apart.
SETTLE_S = 0.3
END_GUARD_S = 0.3
# A visit this long or longer has metrics lines enough to show its profile.
READABLE_S = 0.9
# Runs started again when another process takes the API port before the decoder binds it (wait_for_api()).
API_PORT_ATTEMPTS = 3
SANITIZER = re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|LeakSanitizer|runtime error:")
TMO_BSCH = re.compile(r"\[TETRA BSCH\] MCC=460 MNC=4242")

TETRA = {"symbol_rate_hz": 18000, "symbol_levels": 4, "rf_mod": 1, "channel_profile": 6,
         "channel_bandwidth_hz": 22500, "output_rate_hz": 72000}
# id, type, fixture or None, times the fixture plays in a row, how it plays ("call": once from the first landing;
# "air": all the time), options, the profile its landing publishes (docs/api.md "metrics")
ROWS = (
    ("nfm", "nfm-conventional", "nfm_tone_synth", 2, "call", "--nfm-bandwidth-hz 12500 --squelch-db -42",
     {"channel_bandwidth_hz": 12500, "output_rate_hz": 48000}),
    ("nxdn48", "nxdn48-conventional", None, 0, None, "",
     {"symbol_rate_hz": 2400, "symbol_levels": 4, "rf_mod": 2, "channel_profile": 1, "channel_bandwidth_hz": 6250,
      "output_rate_hz": 72000}),
    ("ysf", "ysf-conventional", "ysf_synth", 2, "call", "",
     {"symbol_rate_hz": 4800, "symbol_levels": 4, "rf_mod": 0, "channel_profile": 2, "channel_bandwidth_hz": 12500,
      "output_rate_hz": 72000}),
    ("dmo", "tetra-dmo-conventional", "tetra_dmo_call_synth_72k", 1, "call", "", TETRA),
    ("tmo", "tetra-dmo-conventional", "tetra_bsch_synth_72k", 1, "air", "", TETRA),
)


class RigScene(bench.Scene):
    """The bench's scene, noting when each frequency command arrives (its stream-time tunes are noted as the next block
    goes out, which a decoder that stopped reading delays)."""

    def __init__(self, channels, seed):
        super().__init__(channels, seed)
        self.commanded = []  # (monotonic s, Hz) of every frequency command, as it arrived

    def commands(self, connection):
        pending = b""
        while not self.stop.is_set():
            try:
                block = connection.recv(128)
                if not block:
                    return
                pending += block
                while len(pending) >= 5:
                    command, value = struct.unpack("!BI", pending[:5])
                    pending = pending[5:]
                    if command == 1:
                        self.commanded.append((time.monotonic(), value))
                        self.frequency = value
                    elif command == 2 and value:
                        self.rate = value
            except socket.timeout:
                continue
            except OSError:
                return


class Row:
    """One channel of the scene, duck-typed for bench_scan_skip_silent.Scene: its frequency, the start of the content on
    air at a scene time, and that content at a stream rate."""

    def __init__(self, index, spec, fixture_dir):
        self.index = index
        self.id, self.kind, fixture, repeats, self.play, self.options, self.profile = spec
        self.frequency = BASE_HZ + index * SPACING_HZ
        self.scene = None
        self.cache = {}
        self.baseband = None
        self.native_rate = None
        if fixture is not None:
            sidecar = json.loads((fixture_dir / f"{fixture}.iq.json").read_text(encoding="utf-8"))
            samples = np.tile(bif.load_cu8_fixture(fixture_dir / sidecar["data_file"]), repeats)
            # SNR_DB over the scene's noise in a 12.5 kHz channel, on the power of the signal (a burst train's bursts,
            # not its gaps).
            power = np.abs(samples) ** 2
            on = power[power > 0.1 * float(np.mean(power))]
            self.baseband = samples / math.sqrt(float(np.mean(on))) * 10.0 ** (SNR_DB / 20.0)
            self.native_rate = int(sidecar["sample_rate_hz"])

    @property
    def seconds(self):
        return 0.0 if self.baseband is None else len(self.baseband) / self.native_rate

    def first_landing(self):
        """Scene time of the first retune onto this channel, or None."""
        for t, _wall, centre, _rate in list(self.scene.tunes):
            if abs(centre - self.frequency) < SPACING_HZ / 2:
                return t
        return None

    def call_at(self, t):
        if self.baseband is None:
            return None
        if self.play == "air":
            return math.floor(t / self.seconds) * self.seconds
        start = self.first_landing()
        if start is None or t < start or t >= start + self.seconds:
            return None
        return start

    def playing_at(self, t):
        """The start of the content the scene plays at scene time t (Scene.block() asks this since issue #834's
        follow-up): call_at(), as a row's content has no silent lead."""
        return self.call_at(t)

    def at_rate(self, rate):
        if rate not in self.cache:
            call = bif.resample_to_fixture_rate(self.baseband, self.native_rate, rate)
            self.cache[rate] = call.astype(np.complex64)
        return self.cache[rate]


def free_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


class PortTaken(Exception):
    """The API port free_port() found was bound by another process before the decoder could bind it."""


def wait_for_api(proc, output, port, deadline_s=20.0):
    """Wait until the decoder says it listens on @p port, so the API connected to is its own and no other test's that
    took the port between free_port() and the decoder's bind. PortTaken when its bind failed; RuntimeError when it
    exited or said neither in time."""
    listening = f"API: listening on 127.0.0.1:{port}"
    refused = (f"API: failed to bind 127.0.0.1:{port}", f"API: failed to listen on 127.0.0.1:{port}")
    until = time.monotonic() + deadline_s
    seen = 0
    while time.monotonic() < until:
        lines = list(output)
        for _t, line in lines[seen:]:
            if listening in line:
                return
            if any(text in line for text in refused):
                raise PortTaken(port)
        seen = len(lines)
        if proc.poll() is not None:
            raise RuntimeError(f"the decoder exited with {proc.returncode} before its API listened")
        time.sleep(0.05)
    raise RuntimeError(f"the decoder did not say it listens on API port {port} within {deadline_s:.0f} s")


def write_targets(rows, path):
    with open(path, "w", encoding="ascii") as out:
        out.write("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation,rtl_gain,options\n")
        for row in rows:
            out.write(f"{row.id},{row.kind},{row.frequency},,{DWELL_MS},{HOLD_MS},,,,{row.options}\n")


class Api:
    """The decoder's control API: every line it streams, stamped with the monotonic time it arrived."""

    def __init__(self, port, deadline_s=20.0):
        self.lines = []
        until = time.monotonic() + deadline_s
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.monotonic() > until:
                    raise
                time.sleep(0.1)
        self.sock.settimeout(1)
        self.sock.sendall(b'{"id":1,"cmd":"subscribe","topics":["metrics"]}\n')
        # No metrics line can come before this: the start of every visit's observation window.
        self.subscribed = time.monotonic()
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.read, daemon=True)
        self.thread.start()

    def read(self):
        pending = b""
        while not self.stop.is_set():
            try:
                block = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return
            if not block:
                return
            pending += block
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                try:
                    message = json.loads(line)
                except ValueError:
                    continue
                if isinstance(message, dict) and message.get("type") == "metrics":
                    self.lines.append((time.monotonic(), message))

    def close(self):
        self.stop.set()
        try:
            self.sock.close()
        except OSError:
            # The decoder may already have closed the connection; closing our end is best effort.
            pass
        self.thread.join(timeout=3)


def stamp_output(pipe, sink):
    for raw in pipe:
        sink.append((time.monotonic(), raw.decode("utf-8", "replace").rstrip("\r\n")))


def visits(tunes, end, rows):
    """(row, start s, end s, start monotonic, end monotonic) per visit in scene order, from the scene's retunes; the
    last one runs to the run's end (end, a (scene s, monotonic) pair) and has a None end s. A retune to the channel on
    air (a rate change) goes on with its visit; one off every channel is a visit of None."""
    out = []
    for i, (t, wall, centre, _rate) in enumerate(tunes):
        row = min(rows, key=lambda r: abs(r.frequency - centre))
        if abs(row.frequency - centre) >= SPACING_HZ / 2:
            row = None
        if out and out[-1][0] is row:
            continue
        if out:
            out[-1] = out[-1][:2] + (t,) + out[-1][3:4] + (wall,)
        out.append((row, t, None, wall, end[1]))
    return out


def whole_visits(scene, rows):
    """Whole visits per row id so far."""
    counts = {row.id: 0 for row in rows}
    for row, _t0, t1, _w0, _w1 in visits(list(scene.tunes), (scene.sample_time, time.monotonic()), rows):
        if row is not None and t1 is not None:
            counts[row.id] += 1
    return counts


def run(args, rows, work):
    scene = RigScene(rows, seed=890)
    for row in rows:
        row.scene = scene
        # Resampled before the run: done on the paced source's thread at the first landing, it would stall the
        # stream for a second or more.
        if row.baseband is not None:
            row.at_rate(CAPTURE_HZ)
    targets = work / "targets.csv"
    write_targets(rows, targets)
    api_port = free_port()
    cmd = [
        str(Path(args.dsd).resolve()),
        "--no-config",
        "--frontend",
        "none",
        "-i",
        f"rtltcp:127.0.0.1:{scene.port}:{rows[0].frequency}:0:0:{DSP_KHZ}:0:1",
        "-o",
        "null",
        "--trunk-scan",
        str(targets),
        "--scan-max-visit-ms",
        str(VISIT_CAP_MS),
        "--trunk-scan-dwell-ms",
        str(SESSION_DWELL_MS),
        "--trunk-scan-activity-hold-ms",
        str(SESSION_HOLD_MS),
        "--no-scan-skip-silent",
        "--api",
        str(api_port),
    ]
    env = {k: v for k, v in os.environ.items() if not k.startswith("DSD_NEO_")}
    scene.thread.start()
    output = []
    proc = subprocess.Popen(cmd, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    reader = threading.Thread(target=stamp_output, args=(proc.stdout, output), daemon=True)
    reader.start()
    api = None
    metrics_end = None
    try:
        wait_for_api(proc, output, api_port)
        api = Api(api_port)
        started = time.monotonic()
        while proc.poll() is None and time.monotonic() - started < args.seconds:
            if min(whole_visits(scene, rows).values()) >= 2:
                break
            time.sleep(0.25)
    finally:
        # The metrics stop here: no visit's profile can be read past it.
        metrics_end = time.monotonic()
        if api is not None:
            api.close()
        proc.terminate()
        try:
            proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        reader.join(timeout=5)
        end = (scene.sample_time, time.monotonic())
        scene.close()
    return {
        "command": cmd,
        "exit": proc.returncode,
        "tunes": list(scene.tunes),
        "commanded": list(scene.commanded),
        "end": end,
        "metrics_start": api.subscribed if api is not None else metrics_end,
        "metrics_end": metrics_end,
        "api": api.lines if api is not None else [],
        "output": output,
    }


def profile_of(metrics, want):
    return {key: metrics.get(key) for key in want}


def command_visits(commanded, end, rows):
    """(row, start monotonic, end monotonic) per visit, from the retune commands as they arrived; a command to the
    channel on air goes on with its visit, the last visit runs to @p end."""
    out = []
    for wall, hz in commanded:
        row = min(rows, key=lambda r: abs(r.frequency - hz))
        if abs(row.frequency - hz) >= SPACING_HZ / 2:
            row = None
        if out and out[-1][0] is row:
            continue
        if out:
            out[-1] = (out[-1][0], out[-1][1], wall)
        out.append((row, wall, end))
    return out


def check_profile(row, visit, lines, observed):
    """(what the visit, a command_visits() entry, showed when it did not land row's profile, else None; whether it
    landed the profile over a window long enough to read it). Only the part of the visit the metrics were collected for
    counts, @p observed (subscription to the API's close): a visit the subscription began late in, or the run's end cut
    short, is judged as long as it was read."""
    _row, w0, w1 = visit
    w0, w1 = max(w0, observed[0]), min(w1, observed[1])
    readable = w1 - w0 >= READABLE_S
    shown = [profile_of(m, row.profile) for t, m in lines if visit[1] + SETTLE_S <= t and w0 <= t < w1 - END_GUARD_S]
    landed = next((i for i, p in enumerate(shown) if p == row.profile), None)
    if landed is None:
        return (f"never showed {row.profile}: {shown}" if readable or shown else None), False
    after = [p for p in shown[landed:] if p != row.profile]
    return (f"showed {after[0]} after landing {row.profile}" if after else None), readable and not after


def check(record, rows, explore):
    failures = []
    text = "\n".join(line for _t, line in record["output"])
    if SANITIZER.search(text):
        failures.append("sanitizer report in the decoder's output")
    if record["exit"] not in (0, -15):
        failures.append(f"the decoder exited with {record['exit']}")
    vs = visits(record["tunes"], record["end"], rows)
    if explore:
        for row, t0, t1, w0, w1 in vs:
            shown = {}
            for t, m in record["api"]:
                if w0 + SETTLE_S <= t < w1 and row is not None:
                    key = json.dumps(profile_of(m, row.profile), sort_keys=True)
                    shown[key] = shown.get(key, 0) + 1
            print(f"visit {row.id if row else '?':7s} {t0:7.2f} -> {'end' if t1 is None else f'{t1:7.2f}'}  {shown}")
        print("tmo BSCH lines:", len(TMO_BSCH.findall(text)))
        return failures
    dwell_s, hold_s, cap_s = DWELL_MS / 1000.0, HOLD_MS / 1000.0, VISIT_CAP_MS / 1000.0
    rates = {rate for _t, _w, _c, rate in record["tunes"][1:]}
    if rates != {CAPTURE_HZ}:
        failures.append(f"the front end ran the source at {sorted(rates)} Hz, not {CAPTURE_HZ} Hz alone")
    for row in rows:
        mine = [v for v in vs if v[0] is row]
        whole = [v for v in mine if v[2] is not None]
        if len(whole) < 2:
            failures.append(f"{row.id}: {len(whole)} whole visit(s); the run needs two of every target")
            continue
        observed = (record["metrics_start"], record["metrics_end"])
        read = 0
        for visit in (v for v in command_visits(record["commanded"], record["end"][1], rows) if v[0] is row):
            wrong, landed = check_profile(row, visit, record["api"], observed)
            read += 1 if landed else 0
            if wrong:
                at = visit[1] - record["commanded"][0][0]
                failures.append(f"{row.id}: the visit commanded {at:.2f} s in {wrong}")
        if read == 0:
            failures.append(f"{row.id}: no visit read for {READABLE_S:.1f} s or more showed its profile {row.profile}")
        idle = whole
        if row.play == "call":
            first, idle = whole[0], whole[1:]
            length = first[2] - first[1]
            if length < row.seconds:
                failures.append(f"{row.id}: its call of {row.seconds:.2f} s held it only {length:.2f} s")
            # The call's last frame restarts the hold; once it runs out, the idle dwell rotates.
            if length > row.seconds + hold_s + dwell_s + LEAVE_PAD_S or length >= cap_s:
                failures.append(
                    f"{row.id}: held {length:.2f} s, past its {row.seconds:.2f} s call, its {hold_s:.1f} s hold and "
                    f"its {dwell_s:.1f} s dwell"
                )
        for _row, t0, t1, _w0, _w1 in idle:
            if t1 - t0 > dwell_s + LEAVE_PAD_S:
                failures.append(f"{row.id}: an idle visit at {t0:.2f} s lasted {t1 - t0:.2f} s, past its dwell")
    if not TMO_BSCH.search(text):
        failures.append("tmo: no BSCH of its trunked network decoded; the target never heard its TETRA")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("dsd")
    parser.add_argument("fixture_dir", type=Path)
    parser.add_argument("--seconds", type=float, default=120.0, help="the longest the run may take (default 120)")
    parser.add_argument("--keep", type=Path, help="keep the run's files and a record.json in this folder")
    parser.add_argument("--explore", action="store_true", help="print the visits and profiles; check nothing")
    args = parser.parse_args()
    rows = [Row(i, spec, args.fixture_dir) for i, spec in enumerate(ROWS)]
    with tempfile.TemporaryDirectory(prefix="scan-rig-") as scratch:
        # Resolved: the decoder runs in it and is handed the target CSV's path, which a relative path would make
        # relative twice.
        work = args.keep.resolve() if args.keep else Path(scratch)
        work.mkdir(parents=True, exist_ok=True)
        # Another loopback test may bind the API port free_port() found before the decoder does: a run whose decoder
        # could not bind it starts again on another port.
        for attempt in range(API_PORT_ATTEMPTS):
            try:
                record = run(args, rows, work)
                break
            except PortTaken as taken:
                print(f"NOTE: API port {taken} was taken before the decoder bound it (attempt {attempt + 1})")
        else:
            print(f"FAIL: every API port of {API_PORT_ATTEMPTS} attempts was taken before the decoder bound it")
            return 1
        if args.keep:
            (work / "record.json").write_text(json.dumps(record, default=str), encoding="utf-8")
        failures = check(record, rows, args.explore)
    for row, t0, t1, _w0, _w1 in visits(record["tunes"], record["end"], rows):
        print(f"visit {row.id if row else '?':7s} {t0:7.2f} -> {'end' if t1 is None else f'{t1:7.2f}'}")
    if failures:
        for _t, line in record["output"][-60:]:
            print(line)
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("OK: every target landed its own profile, the calls held their targets and released them, TMO held nothing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
