# SPDX-License-Identifier: GPL-3.0-or-later
"""The -Y step's frame-boundary rule over real calls (issue #834 item 3).

Each call fixture, followed by the receiver-noise fixture, is replayed through the real engine on a landed -Y row
(tests/engine/y_call_hold_replay.c) of a two-row typed map, with -t 1 (or --hangtime, comma-separated for several) and no
visit cap, skip-silent off and on. From the
host's timeline it checks:

  * no frame-boundary departure while the call's frames are still coming: no step that a frame-boundary yield led to
    before the call's last sync;
  * the row is left once the call ends: a step after the call's last sync, within the hangtime plus LEAVE_PAD_S
    (LEAVE_PAD_NXDN_S for NXDN, whose rows are held 2 s past each checked frame).

A step noCarrier() takes between the call's frames (a hunt pass that found no sync) is the rule from before the
frame-boundary yield and is reported, not failed. Usage: y_call_hold_check.py [--hangtime S[,S...]] HOST FIXTURE_DIR [NAME...]
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

# fixture name: the row's mode
CALLS = {
    "dpmr": "dpmr",
    "dstar": "dstar",
    "m17": "m17",
    "nxdn48": "nxdn48",
    "nxdn48_attenuated": "nxdn48",
    "nxdn96": "nxdn96",
    "ysf": "ysf",
    "m17_lsf_weak": "m17",
    "nxdn96_weak_0db": "nxdn96",
    "dmr_bs_weak": "dmr",
}
TAIL = "noise_floor"
# How long the row may stay on after the call's last sync, past the hangtime. Every protocol: the hangtime is compared
# in whole seconds (up to 1 s over), plus up to a second for the hunt pass that steps. NXDN also holds a -Y row 2 s
# past each frame that passed its own check (nxdn_finish_decoded_frame(), from issue #406, so the gap to the next frame
# does not read as loss of signal); with that hold on top, its bound is 3.5 s. Measured at -t 1: 0.4-1.9 s for the
# others, 3.9-4.1 s for NXDN (2 s hold + 1 s hangtime + rounding + a pass, from its last checked frame); about a
# second more at -t 2.
LEAVE_PAD_S = 2.0
LEAVE_PAD_NXDN_S = 3.5
LINE = re.compile(r"YHOLD (landed|sync|step) t=([0-9.]+)(?: proven=(\d+))?(?: partial=(\d+))?(?: via=(\w+))?")


def with_tail(fixtures, name, out_dir):
    meta = json.loads((fixtures / f"{name}.iq.json").read_text(encoding="utf-8"))
    tail = json.loads((fixtures / f"{TAIL}.iq.json").read_text(encoding="utf-8"))
    if (meta["sample_rate_hz"], meta["sample_format"]) != (tail["sample_rate_hz"], tail["sample_format"]):
        raise SystemExit(f"{name}: the noise tail does not match the fixture's rate or format")
    data = (fixtures / meta["data_file"]).read_bytes()
    call_s = len(data) / 2.0 / meta["sample_rate_hz"]
    data += (fixtures / tail["data_file"]).read_bytes()
    (out_dir / f"{name}_tail.iq").write_bytes(data)
    meta.update({"data_file": f"{name}_tail.iq", "data_bytes": len(data), "notes": "fixture + receiver-noise tail"})
    path = out_dir / f"{name}_tail.iq.json"
    path.write_text(json.dumps(meta, indent=1), encoding="utf-8")
    return path, call_s, meta["center_frequency_hz"]


def run_case(host, fixtures, out_dir, name, skip, hangtime):
    replay, call_s, centre = with_tail(fixtures, name, out_dir)
    mode = CALLS[name]
    pad = LEAVE_PAD_NXDN_S if mode.startswith("nxdn") else LEAVE_PAD_S
    chan = out_dir / f"{name}_map.csv"
    chan.write_text(
        f"channel,frequency_hz,name,mode,options\n1,{centre},call,{mode},\n2,{centre + 1000000},other,{mode},\n",
        encoding="ascii",
    )
    cmd = [host, "--frontend", "none", "-Y", "-C", str(chan), "-t", str(hangtime)]
    cmd += ["--scan-skip-silent" if skip else "--no-scan-skip-silent", "--iq-replay", str(replay), "-o", "null"]
    result = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=240, check=False)
    if result.returncode != 0:
        return [f"host exited {result.returncode}:\n{result.stderr[-2000:]}"], {}
    landed, syncs, steps = None, [], []
    for line in (result.stdout + result.stderr).splitlines():
        match = LINE.search(line)
        if not match:
            continue
        kind, t = match.group(1), float(match.group(2))
        if kind == "landed":
            landed = t
        elif kind == "sync":
            syncs.append((t, int(match.group(3)), int(match.group(4) or 0)))
        else:
            steps.append((t, match.group(5)))
    if landed is None:
        return ["the host never landed row zero"], {}
    rel = lambda t: t - landed  # noqa: E731
    call = [(rel(t), p, q) for t, p, q in syncs if rel(t) <= call_s + 0.3]
    if not call:
        return ["no sync in the call"], {}
    last_sync = call[-1][0]
    proven_times = [t for (t, p, _q), (_t0, p0, _q0) in zip(call[1:], call[:-1]) if p > p0]
    info = {
        "call_s": call_s,
        "syncs": len(call),
        "proven_total": call[-1][1],
        "partial_total": call[-1][2],
        "last_sync": round(last_sync, 3),
        "proven_at": [round(t, 2) for t in proven_times],
        "steps": [(round(rel(t), 2), via) for t, via in steps],
    }
    failures = []
    for t, via in steps:
        if via == "frame" and rel(t) < last_sync:
            failures.append(f"frame-boundary departure at {rel(t):.2f} s, frames still coming until {last_sync:.2f} s")
    after = [rel(t) for t, _via in steps if rel(t) >= last_sync]
    info["left_at"] = round(after[0], 2) if after else None
    if not after:
        failures.append(f"the row was not left after the call (last sync {last_sync:.2f} s, run ended)")
    elif after[0] - last_sync > hangtime + pad:
        failures.append(f"left {after[0] - last_sync:.2f} s after the call's last sync (over {hangtime + pad} s)")
    return failures, info


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("host")
    parser.add_argument("fixtures")
    parser.add_argument("names", nargs="*")
    parser.add_argument("--hangtime", default="1", help="the -t hangtime in seconds, or several, comma-separated")
    args = parser.parse_args()
    fixtures = Path(args.fixtures).resolve()
    names = args.names or list(CALLS)
    hangtimes = [int(value) for value in args.hangtime.split(",")]
    cases = [(name, skip, hangtime) for hangtime in hangtimes for name in names for skip in (False, True)]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="dsd-y-call-hold-") as temporary:
        root = Path(temporary)

        def run(case):
            name, skip, hangtime = case
            out_dir = root / f"{name}-{int(skip)}-{hangtime}"
            out_dir.mkdir()
            return run_case(args.host, fixtures, out_dir, name, skip, hangtime)

        with ThreadPoolExecutor(max_workers=4) as pool:
            for (name, skip, hangtime), (failures, info) in zip(cases, pool.map(run, cases)):
                label = f"{name} -t {hangtime} skip-silent {'on' if skip else 'off'}"
                status = "FAIL" if failures else "ok"
                summary = {key: value for key, value in info.items() if key not in ("steps", "proven_at")}
                summary["proven_first_last"] = info["proven_at"][:1] + info["proven_at"][-1:] if info else []
                summary["left_at"] = info.get("left_at")
                print(f"{status}: {label}: {json.dumps(summary)}")
                for failure in failures:
                    print(f"  {failure}")
                failed += bool(failures)
    if failed:
        print(f"{failed} case(s) failed")
        sys.exit(1)


if __name__ == "__main__":
    main()
