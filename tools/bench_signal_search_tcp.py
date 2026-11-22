# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

"""Manual end-to-end Signal search benchmarks against a paced loopback rtl_tcp source.

Requires NumPy. Uses an ephemeral loopback port and never opens physical hardware.

With the dsd-neo_test_signal_search_bench executable (the default form) it checks RF publication: an NFM transmitter
stays at 163.0125 MHz while the receiver changes capture centres, and the first detection must be published within
100 ms of the capture block that completed it.

With --rig and the dsd-neo_test_signal_search_rig executable (the search the app starts: app-control queue, observer
and history store) it measures whole searches on tools/bench_scan_skip_silent.py's scene, at the capture rate the
receiver asks for (1.536 Msps at a 48 kHz DSP bandwidth, 1.152 Msps at 72 kHz):

  --scene empty       450-460 MHz, 12.5 kHz steps, 16 kHz width, analog only, noise only: the time of each whole
                      pass with nothing to visit (--passes of them; the first, which waits for the stream, is left
                      out)
  --scene empty25     the same with 25 kHz steps and a 25 kHz width, the TETRA preset's (a wider detector window
                      leaves less of each capture usable, so a range takes more spans)
  --scene candidates  450-452 MHz Auto, 12.5 kHz steps, 16 kHz width: DMR at a span's centre (450.2 MHz), P25 Phase 1
                      near a span's edge (450.3875 MHz) and a weak DMR (451.0 MHz, 20 dB over the noise in 12.5 kHz),
                      each on until 3 s after the search lands on it: which are detected and confirmed, and the time
                      from landing to confirmation
  --scene tetra       426.6-426.8, 467.8625-467.9625 and 432.975-433.025 MHz TETRA ranges (builds with TETRA
                      identification only, at 72 kHz): the real-air TETRA call, the marginal real-air control channel
                      and the synthetic direct mode call, landing to confirmation

and for each the process's CPU time per second of run, the demodulator's busy share and the input ring drops (from
the RTL perf CSV). Each run writes result.json to --out; --summary DIR... prints one table over earlier runs.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_iq_fixtures as bif  # noqa: E402

FIXTURE_DIR = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "iq"


class Source:
    def __init__(self, port=0):
        self.socket = socket.socket()
        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.socket.bind(("127.0.0.1", port))
        self.socket.listen(1)
        self.socket.settimeout(30)
        self.port = self.socket.getsockname()[1]
        self.stop = threading.Event()
        self.frequency = 163000000
        self.rate = 1536000
        self.thread = threading.Thread(target=self.serve, daemon=True)

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
                        self.frequency = value
                    elif command == 2 and value:
                        self.rate = value
            except socket.timeout:
                continue
            except OSError:
                return

    def stream(self, connection):
        rng = np.random.default_rng(537)
        phase, sample_time = 0.0, 0.0
        count = 8192
        index = np.arange(count)
        deadline = time.monotonic()
        while not self.stop.is_set():
            rate = self.rate
            delta = 163012500 - self.frequency
            deviation = 2000 * np.sin(2 * np.pi * 900 * (sample_time + index / rate))
            angles = phase + np.cumsum(2 * np.pi * (delta + deviation) / rate)
            phase = float(angles[-1] % (2 * np.pi))
            sample_time += count / rate
            amplitude = 20 if abs(delta) < rate / 2 else 0
            iq = np.empty(count * 2)
            iq[::2] = 127.5 + amplitude * np.cos(angles) + rng.normal(0, 0.7, count)
            iq[1::2] = 127.5 + amplitude * np.sin(angles) + rng.normal(0, 0.7, count)
            connection.sendall(np.clip(np.rint(iq), 0, 255).astype("u1").tobytes())
            deadline += count / rate
            self.stop.wait(max(0, deadline - time.monotonic()))

    def serve(self):
        try:
            connection, _ = self.socket.accept()
        except OSError:
            return
        with connection:
            connection.settimeout(1)
            connection.sendall(b"RTL0" + struct.pack("!II", 1, 1))
            reader = threading.Thread(target=self.commands, args=(connection,), daemon=True)
            reader.start()
            try:
                self.stream(connection)
            except OSError:
                pass  # The benchmark closes the source after its first detection.
            finally:
                self.stop.set()
                reader.join(timeout=2)

    def close(self):
        self.stop.set()
        self.socket.close()
        self.thread.join(timeout=3)


class Transmitter:
    """One transmitter of a rig scene, duck-typed for bench_scan_skip_silent.Scene: its frequency, the start of what is
    on the air at a scene time, and that content at a stream rate.

    The content is a committed fixture read in its own format and rate (build_iq_fixtures.load_fixture_iq()), the
    (samples, rate) given as samples, or with neither band-limited noise 24 kHz wide. It is looped from the start of
    the scene, at snr_db over the scene's noise in a 12.5 kHz channel (on the power of the bursts of a burst train, not
    its gaps). With on_s it stops that long after the search first lands on it (landed, a scene time, which run_rig()
    sets).
    """

    def __init__(self, name, frequency, fixture=None, snr_db=30.0, on_s=None, step=12500, fixture_dir=FIXTURE_DIR,
                 samples=None):
        self.id = name
        self.frequency = frequency
        self.on_s = on_s
        self.step = step
        self.landed = None
        self.scene = None  # the scene serving it, which run_rig() sets
        self.cache = {}
        if samples is not None:
            samples, rate = samples
        elif fixture is None:
            rate = 24000
            rng = np.random.default_rng(748)
            samples = rng.standard_normal(10 * rate) + 1j * rng.standard_normal(10 * rate)
        else:
            samples, rate, _centre = bif.load_fixture_iq(Path(fixture_dir) / f"{fixture}.iq.json")
            samples = samples - np.mean(samples)
        power = np.abs(samples) ** 2
        on = power[power > 0.1 * float(np.mean(power))]
        self.baseband = samples / math.sqrt(float(np.mean(on))) * 10.0 ** (snr_db / 20.0)
        self.native_rate = rate

    @property
    def seconds(self):
        return len(self.baseband) / self.native_rate

    @property
    def stop_at(self):
        return None if self.landed is None or self.on_s is None else self.landed + self.on_s

    def owns(self, frequency):
        return abs(frequency - self.frequency) < self.step / 2

    def call_at(self, t):
        if self.stop_at is not None and t >= self.stop_at:
            return None
        return math.floor(t / self.seconds) * self.seconds

    def at_rate(self, rate):
        if rate not in self.cache:
            self.cache[rate] = bif.resample_to_fixture_rate(self.baseband, self.native_rate, rate).astype(np.complex64)
        return self.cache[rate]


def children_cpu_s():
    """User and system CPU seconds of the reaped child processes so far (None where the platform has no getrusage)."""
    try:
        import resource  # POSIX only
    except ImportError:
        return None
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return usage.ru_utime + usage.ru_stime


# The status phases dsd-neo_test_signal_search_rig prints (dsd_signal_search_phase) once a visit has landed: identifying,
# listening and held. Tuning (2) is published with the hit when the tune is queued, before the receiver hears the
# channel, so it is no landing.
LANDED_PHASES = (3, 4, 5)


def landed_hit(message):
    """The hit of a rig status line that shows a live visit landed on it (a landed phase, a hit, not ended), or None."""
    if not message or message.get("rig") != "status" or message.get("phase") not in LANDED_PHASES:
        return None
    hit = message.get("hit") or {}
    return hit if hit.get("hit_id") and not hit.get("ended") else None


def capture_rate(dsp_khz):
    """The rate the RTL front end asks an rtl_tcp server for at this DSP bandwidth (rtl_downsample_passes_for_rate_in():
    the bandwidth times the power of two nearest 1.2 Msps)."""
    rate = dsp_khz * 1000
    while rate * 2 <= 1600000:
        rate *= 2
    return rate


def read_rig(pipe, lines, scene, transmitters):
    """The rig's lines, stamped with the scene time and the monotonic time they arrived; marks each transmitter's first
    landing: the first status of a live visit to it once the tune has landed (landed_hit())."""
    for raw in pipe:
        text = raw.decode("utf-8", "replace").rstrip("\r\n")
        stamp = scene.sample_time
        message = None
        # The decoder shares stdout and can leave a line open ("Sync: +TETRA NDB "): a rig line follows on it.
        at = text.find('{"rig":')
        if at >= 0:
            try:
                message = json.loads(text[at:])
            except ValueError:
                message = None
        lines.append((stamp, time.monotonic(), text, message))
        hit = landed_hit(message)
        if hit is None:
            continue
        for tx in transmitters:
            if tx.landed is None and tx.owns(hit["frequency_hz"]):
                tx.landed = stamp


def run_rig(rig, transmitters, ranges, dsp_khz, work, seconds, done=None, passes=1000, perf=False, seed=748):
    """Run dsd-neo_test_signal_search_rig on a paced scene of @p transmitters, searching @p ranges (each (name, start,
    end, step, width, digital, probe_ms, max_visit_ms)) at a @p dsp_khz DSP bandwidth, until @p passes passes have
    ended, done(scene) says so, or @p seconds pass. Returns the run's record."""
    # Here, not at the top: bench_scan_skip_silent imports this module.
    import bench_scan_skip_silent as scan_bench

    work = Path(work)
    scene = scan_bench.Scene(transmitters, seed=seed)
    rate = capture_rate(dsp_khz)
    for tx in transmitters:
        tx.scene = scene
        # Resampled before the run: done on the paced source's thread, it would stall the stream.
        tx.at_rate(rate)
    env = {k: v for k, v in os.environ.items() if not k.startswith("DSD_NEO_")}
    env.update(
        DSD_SEARCH_RIG_STORE=str(work / "history.jsonl"),
        DSD_SEARCH_RIG_RANGES=";".join(",".join(str(v) for v in r) for r in ranges),
        DSD_SEARCH_RIG_PASSES=str(passes),
        DSD_SEARCH_RIG_SECONDS=str(int(seconds) + 10),
        DSD_SEARCH_RIG_STOP=str(work / "stop"),
    )
    if perf:
        env.update(DSD_NEO_RTL_PERF_CSV="1", DSD_NEO_RTL_PERF_INTERVAL_MS="1000")
    first = ranges[0][1]
    cmd = [str(Path(rig).resolve()), "--no-config", "--frontend", "none", "-i",
           f"rtltcp:127.0.0.1:{scene.port}:{first}:0:0:{dsp_khz}:0:1", "-o", "null"]
    scene.thread.start()
    lines = []
    (work / "stop").unlink(missing_ok=True)
    before = children_cpu_s()
    proc = subprocess.Popen(cmd, cwd=work, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    reader = threading.Thread(target=read_rig, args=(proc.stdout, lines, scene, transmitters), daemon=True)
    reader.start()
    started = time.monotonic()
    try:
        while proc.poll() is None and time.monotonic() - started < seconds:
            if done is not None and done(scene):
                break
            time.sleep(0.25)
        (work / "stop").write_bytes(b"")
        proc.wait(timeout=30)
    except (subprocess.TimeoutExpired, OSError):
        proc.kill()
        proc.wait()
    wall_s = time.monotonic() - started
    after = children_cpu_s()
    # The whole run's CPU time, startup and teardown included, from the reaped child's resource usage.
    cpu_s = after - before if before is not None and after is not None else None
    reader.join(timeout=10)
    scene.close()
    record = {"command": cmd, "exit": proc.returncode, "tunes": list(scene.tunes), "lines": lines,
              "cpu_s": cpu_s, "wall_s": wall_s, "dsp_khz": dsp_khz}
    if perf:
        record["perf"] = scan_bench.perf_summary(str(work / "dsd-neo-rtl-perf.csv"))
    return record


def rig_messages(record, kind):
    return [(t, wall, m) for t, wall, _text, m in record["lines"] if m and m.get("rig") == kind]


def rig_rows(record, kind="history"):
    return [m["row"] for _t, _wall, m in rig_messages(record, kind)]


def confirm_time(record, tx):
    """Seconds of stream time from the search's first landing on @p tx to the first status that shows its hit
    confirmed, or None."""
    for t, _wall, m in rig_messages(record, "status"):
        hit = m["hit"]
        if tx.landed is not None and t >= tx.landed and hit["confirmed"] and tx.owns(hit["frequency_hz"]):
            return t - tx.landed
    return None


def pass_times(record):
    """Stream seconds of every whole pass, from the status line that starts it to the one that starts the next."""
    starts = []
    for t, _wall, m in rig_messages(record, "status"):
        if m["pass"] and (not starts or m["pass"] != starts[-1][0]):
            starts.append((m["pass"], t))
    return [(b[1] - a[1]) for a, b in zip(starts, starts[1:])]


EMPTY_RANGES = [("empty", 450000000, 460000000, 12500, 16000, 0, 3000, 30000)]
EMPTY25_RANGES = [("empty25", 450000000, 460000000, 25000, 25000, 0, 3000, 30000)]
CANDIDATE_RANGES = [("candidates", 450000000, 452000000, 12500, 16000, 1, 3000, 30000)]
TETRA_RANGES = [
    ("tmo", 426600000, 426800000, 25000, 25000, 10, 4000, 30000),
    ("cc144", 467862500, 467962500, 12500, 25000, 10, 4000, 30000),
    ("dmo", 432975000, 433025000, 25000, 25000, 10, 4000, 30000),
]


def scene_setup(name):
    """(transmitters, ranges, passes) of a --scene."""
    if name == "empty":
        return [], EMPTY_RANGES, None
    if name == "empty25":
        return [], EMPTY25_RANGES, None
    if name == "candidates":
        return [
            Transmitter("centre", 450200000, "dmr_voice", on_s=3.0),
            Transmitter("edge", 450387500, "p25p1_c4fm_cc", on_s=3.0),
            Transmitter("weak", 451000000, "dmr_voice", snr_db=20.0, on_s=3.0),
        ], CANDIDATE_RANGES, None
    return [
        Transmitter("tmo", 426700000, "tetra_call_real_air_72k", on_s=3.0, step=25000),
        Transmitter("cc144", 467912500, "tetra_cc_marginal_real_air_144k", on_s=3.0),
        Transmitter("dmo", 433000000, "tetra_dmo_call_synth_72k", on_s=3.0, step=25000),
    ], TETRA_RANGES, None


def rig_main(args):
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    transmitters, ranges, _ = scene_setup(args.scene)
    passes = args.passes + 1 if not transmitters else 1000

    def done(scene):
        stops = [tx.stop_at for tx in transmitters]
        return bool(transmitters) and None not in stops and scene.sample_time > max(stops) + 4.0

    load = os.getloadavg()
    record = run_rig(args.rig, transmitters, ranges, args.bw, out, args.seconds, done=done, passes=passes,
                     perf=True, seed=args.seed)
    passes_s = pass_times(record)[1:] if not transmitters else []
    rows = rig_rows(record)
    hits = {}
    for tx in transmitters:
        mine = [r for r in rows if tx.owns(r["frequency_hz"])]
        hits[tx.id] = {
            "detected": bool(mine),
            "confirmed": any(r["confirmed"] for r in mine),
            "protocol": mine[0]["protocol"] if mine else None,
            "confirm_s": confirm_time(record, tx),
        }
    result = {
        "rig": str(args.rig),
        "label": args.label,
        "scene": args.scene,
        "seed": args.seed,
        "bw_khz": args.bw,
        "exit": record["exit"],
        "pass_s": passes_s,
        "hits": hits,
        "cpu_s": record["cpu_s"],
        "wall_s": record["wall_s"],
        "cpu_per_s": record["cpu_s"] / record["wall_s"] if record["cpu_s"] is not None and record["wall_s"] else None,
        "perf": record["perf"],
        "load_average_start": load,
        "load_average_end": os.getloadavg(),
        "rates": sorted({rate for _t, _w, _c, rate in record["tunes"][1:]}),
    }
    (out / "result.json").write_text(json.dumps(result, indent=1), encoding="utf-8")
    (out / "record.json").write_text(json.dumps(record, default=str), encoding="utf-8")
    print(json.dumps(result))
    return 0 if record["exit"] == 0 else 1


def summary(dirs):
    """One line per (label, scene, bandwidth): medians over the runs."""
    groups = {}
    for d in dirs:
        result = json.loads((Path(d) / "result.json").read_text(encoding="utf-8"))
        groups.setdefault((result["label"], result["scene"], result["bw_khz"]), []).append(result)
    for (label, scene, bw), results in sorted(groups.items()):
        passes = [p for r in results for p in r["pass_s"]]
        cpu = [r["cpu_per_s"] for r in results if r["cpu_per_s"] is not None]
        busy = [r["perf"]["demod_busy_pct"] for r in results if r["perf"]["demod_busy_pct"] is not None]
        drops = [r["perf"]["input_drops"] for r in results if r["perf"]["input_drops"] is not None]
        line = f"{label:8s} {scene:10s} {bw:3d} kHz  runs {len(results)}"
        if passes:
            line += (f"  pass median {statistics.median(passes):.3f} s (n {len(passes)}, "
                     f"{min(passes):.3f}-{max(passes):.3f})")
        if cpu:
            line += f"  CPU {statistics.median(cpu):.4f} s/s"
        if busy:
            line += f"  demod busy {statistics.median(busy):.2f} %"
        if drops:
            line += f"  drops max {max(drops)}"
        print(line)
        names = sorted({name for r in results for name in r["hits"]})
        for name in names:
            got = [r["hits"][name] for r in results if name in r["hits"]]
            times = [h["confirm_s"] for h in got if h["confirm_s"] is not None]
            text = f"    {name:8s} detected {sum(h['detected'] for h in got)}/{len(got)}"
            text += f" confirmed {sum(h['confirmed'] for h in got)}/{len(got)}"
            if times:
                text += (f" landing->confirmed median {statistics.median(times):.3f} s "
                         f"({min(times):.3f}-{max(times):.3f})")
            print(text)


def main():
    if "--rig" in sys.argv[1:] or "--summary" in sys.argv[1:]:
        parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
        parser.add_argument("--rig", type=Path, help="dsd-neo_test_signal_search_rig executable")
        parser.add_argument("--bw", type=int, choices=(48, 72), default=48, help="DSP bandwidth, kHz")
        parser.add_argument("--scene", choices=("empty", "empty25", "candidates", "tetra"), default="empty")
        parser.add_argument("--seed", type=int, default=748, help="the scene's noise")
        parser.add_argument("--passes", type=int, default=3, help="empty scene: whole passes measured")
        parser.add_argument("--seconds", type=float, default=180.0, help="the longest a run may take")
        parser.add_argument("--label", default="", help="a name for the build, kept in result.json")
        parser.add_argument("--out", type=Path, help="the run's folder")
        parser.add_argument("--summary", nargs="+", type=Path, help="print one table over these runs' folders")
        args = parser.parse_args()
        if args.summary:
            summary(args.summary)
            return 0
        if not args.rig or not args.out:
            parser.error("--rig and --out are required")
        return rig_main(args)
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("binary", type=Path, help="dsd-neo_test_signal_search_bench executable")
    args = parser.parse_args()
    source = Source()
    source.thread.start()
    env = dict(os.environ, DSD_SEARCH_BENCH_MODE="hit")
    try:
        result = subprocess.run(
            [str(args.binary.resolve()), "--frontend", "none", "-fA", "-i",
             f"rtltcp:127.0.0.1:{source.port}:163M:0:0:48:0:1", "-o", "null"],
            env=env, capture_output=True, text=True, timeout=30, check=False,
        )
    finally:
        source.close()
    print(result.stdout, end="")
    match = re.search(r"visits=1 .*max_report_latency_us=(\d+) hit_frequency_hz=163012500", result.stdout)
    if result.returncode or not match:
        print(result.stderr, end="")
        raise SystemExit("Expected a detection at the synthetic transmitter frequency")
    if int(match.group(1)) >= 100000:
        raise SystemExit("RF publication exceeded 100 ms after the completing capture block arrived")


if __name__ == "__main__":
    sys.exit(main())
