# SPDX-License-Identifier: GPL-3.0-or-later
"""The -Y step's frame-boundary rule over real calls (issue #834 item 3).

Each call fixture, followed by as much of the receiver-noise fixture as the leave bound needs, is replayed through the
real engine on a landed -Y row (tests/engine/y_call_hold_replay.c) of a two-row map, with no visit cap, in each of
LEGS: a decoder set, "typed" (the row's own mode) or "fa" (an untyped list under -fa, the default set), a hangtime (-t)
and skip-silent off or on. --hangtime and --sets narrow the legs; --all-legs runs every set at every --hangtime with
skip-silent off and on. From the host's timeline it checks:

  * no frame-boundary departure while the call's frames are still coming: no step that a frame-boundary yield led to
    before the call's last sync;
  * the row is left once the call ends: a step after the call's last sync, within the hangtime plus LEAVE_PAD_S
    (LEAVE_PAD_NXDN_S for NXDN, whose rows are held 2 s past each checked frame).

A call with no PROVEN or PARTIAL note in its decoder set is the case the rule exists to leave: it is reported as
"unchecked" and judged only for leaving within the same bound, counted from the landing.

A step noCarrier() takes between the call's frames (a hunt pass that found no sync) is the rule from before the
frame-boundary yield and is reported, not failed.

Usage: y_call_hold_check.py [--hangtime S[,S...]] [--sets typed,fa] [--all-legs] HOST FIXTURE_DIR [NAME...]
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
    "dpmr_synth": "dpmr",
    "ysf_synth": "ysf",
    "nxdn48_synth": "nxdn48",
}
# The decoder sets a row runs: "typed", the row's own mode from a typed map; "fa", an untyped list under -fa, the
# default set (X2-TDMA and ProVoice in it, whose frames no check can prove).
SETS = ("typed", "fa")
# The legs a run covers, as (decoder set, hangtime, skip-silent): skip-silent off and on at -t 1 on the typed row, where
# the frames hold it whatever the setting says; -t 2, where the leave bound grows with the hangtime; and the -fa set,
# whose extra decoders must not hold a row the call left.
LEGS = (("typed", 1, False), ("typed", 1, True), ("typed", 2, True), ("fa", 1, True))
# The calls that get no PROVEN or PARTIAL note in their decoder set, by (fixture, set): the off-air dpmr capture, whose
# CRCs never pass, and dstar under -fa, which the default set does not decode as D-STAR within the capture. Only these
# are judged as unchecked; any other call with no note fails (a fixture whose checks stopped passing), and one of these
# that gains a note fails too, so the set is kept to what the decoders do.
EXPECTED_UNCHECKED = {("dpmr", "typed"), ("dpmr", "fa"), ("dstar", "fa")}
# Bytes per complex sample, by the sidecar's sample_format.
SAMPLE_BYTES = {"cu8": 2, "cs16": 4, "cf32": 8}
TAIL = "noise_floor"
# How long the row may stay on after the call's last sync, past the hangtime. Every protocol: the hangtime is compared
# in whole seconds (up to 1 s over), plus up to a second for the hunt pass that steps. NXDN also holds a -Y row 2 s
# past each frame whose own CRCs proved its content (nxdn_finish_decoded_frame(), from issue #406, so the gap to the
# next frame does not read as loss of signal); with that hold on top, its bound is 3.5 s. Measured at -t 1: 0.4-1.9 s
# for the others, 3.0-4.1 s for NXDN (2 s hold + 1 s hangtime + rounding + a pass, from its last checked frame); about
# a second more at -t 2.
LEAVE_PAD_S = 2.0
LEAVE_PAD_NXDN_S = 3.5
# How much longer than the leave bound the replay runs, so a leave on time is always inside it: the last sync may come
# up to 0.3 s after the call's end, and a step needs its hunt pass. A leave later than the bound fails either way, as
# over the bound or as no leave before the run ended.
END_MARGIN_S = 1.0
LINE = re.compile(r"YHOLD (landed|sync|step) t=([0-9.]+)(?: proven=(\d+))?(?: partial=(\d+))?(?: via=(\w+))?")


def with_tail(fixtures, name, out_dir, bound_s, unchecked):
    """The fixture and as much of the noise tail as the leave bound needs: the bound after the call's end, or for an
    unchecked call, which is judged from the landing, what the bound needs beyond the call, if anything."""
    meta = json.loads((fixtures / f"{name}.iq.json").read_text(encoding="utf-8"))
    tail = json.loads((fixtures / f"{TAIL}.iq.json").read_text(encoding="utf-8"))
    if (meta["sample_rate_hz"], meta["sample_format"]) != (tail["sample_rate_hz"], tail["sample_format"]):
        raise SystemExit(f"{name}: the noise tail does not match the fixture's rate or format")
    if meta["sample_format"] not in SAMPLE_BYTES:
        raise SystemExit(f"{name}: sample format {meta['sample_format']} has no size here")
    data = (fixtures / meta["data_file"]).read_bytes()
    sample_bytes = SAMPLE_BYTES[meta["sample_format"]]
    call_s = len(data) / sample_bytes / meta["sample_rate_hz"]
    run_end_s = (max(call_s, bound_s) if unchecked else call_s + bound_s) + END_MARGIN_S
    tail_data = (fixtures / tail["data_file"]).read_bytes()
    tail_bytes = round((run_end_s - call_s) * meta["sample_rate_hz"]) * sample_bytes
    if tail_bytes > len(tail_data):
        raise SystemExit(f"{name}: the noise tail is shorter than the {run_end_s - call_s:.1f} s the bound needs")
    data += tail_data[:tail_bytes]
    (out_dir / f"{name}_tail.iq").write_bytes(data)
    meta.update({"data_file": f"{name}_tail.iq", "data_bytes": len(data), "notes": "fixture + receiver-noise tail"})
    path = out_dir / f"{name}_tail.iq.json"
    path.write_text(json.dumps(meta, indent=1), encoding="utf-8")
    return path, call_s, meta["center_frequency_hz"]


def run_case(host, fixtures, out_dir, name, skip, hangtime, decoder_set="typed"):
    mode = CALLS[name]
    pad = LEAVE_PAD_NXDN_S if mode.startswith("nxdn") else LEAVE_PAD_S
    expected = (name, decoder_set) in EXPECTED_UNCHECKED
    replay, call_s, centre = with_tail(fixtures, name, out_dir, hangtime + pad, expected)
    chan = out_dir / f"{name}_map.csv"
    if decoder_set == "typed":
        rows = f"channel,frequency_hz,name,mode,options\n1,{centre},call,{mode},\n2,{centre + 1000000},other,{mode},\n"
    else:
        rows = f"channel,frequency_hz,name\n1,{centre},call\n2,{centre + 1000000},other\n"
    chan.write_text(rows, encoding="ascii")
    cmd = [host, "--frontend", "none", "-Y", "-C", str(chan), "-t", str(hangtime)]
    if decoder_set == "fa":
        cmd += ["-fa"]
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
    # A call that gets no PROVEN or PARTIAL note in this decoder set carries nothing the rule can tell from noise, and
    # the rule exists to let such a row go: it is not judged for a cut, only for leaving within the bound, counted from
    # the landing, where its content window starts. Which calls those are is fixed in EXPECTED_UNCHECKED.
    info["unchecked"] = info["proven_total"] == 0 and info["partial_total"] == 0
    if info["unchecked"] != expected:
        if expected:
            return [f"{name} ({decoder_set}) is expected unchecked but got notes: update EXPECTED_UNCHECKED"], info
        return [f"{name} ({decoder_set}) got no PROVEN or PARTIAL note: its checks stopped passing"], info
    if info["unchecked"]:
        first = [rel(t) for t, _via in steps]
        info["left_at"] = round(first[0], 2) if first else None
        if not first:
            failures.append("the row with no checked content was never left (run ended)")
        elif first[0] > hangtime + pad:
            failures.append(
                f"the row with no checked content left {first[0]:.2f} s after landing (over {hangtime + pad} s)"
            )
        return failures, info
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
    parser.add_argument("--hangtime", help="only these -t hangtimes in seconds, comma-separated")
    parser.add_argument("--sets", help="only these decoder sets, comma-separated: typed, fa")
    parser.add_argument("--all-legs", action="store_true", help="every set at every hangtime, skip-silent off and on")
    args = parser.parse_args()
    fixtures = Path(args.fixtures).resolve()
    names = args.names or list(CALLS)
    hangtimes = [int(value) for value in args.hangtime.split(",")] if args.hangtime else None
    sets = args.sets.split(",") if args.sets else None
    if args.all_legs:
        legs = [(s, t, skip) for s in sets or SETS for t in hangtimes or (1, 2) for skip in (False, True)]
    else:
        legs = [leg for leg in LEGS if (not sets or leg[0] in sets) and (not hangtimes or leg[1] in hangtimes)]
    cases = [(name, skip, hangtime, decoder_set) for decoder_set, hangtime, skip in legs for name in names]
    failed = 0
    with tempfile.TemporaryDirectory(prefix="dsd-y-call-hold-") as temporary:
        root = Path(temporary)

        def run(case):
            name, skip, hangtime, decoder_set = case
            out_dir = root / f"{name}-{int(skip)}-{hangtime}-{decoder_set}"
            out_dir.mkdir()
            return run_case(args.host, fixtures, out_dir, name, skip, hangtime, decoder_set)

        with ThreadPoolExecutor(max_workers=4) as pool:
            for (name, skip, hangtime, decoder_set), (failures, info) in zip(cases, pool.map(run, cases)):
                label = f"{name} {decoder_set} -t {hangtime} skip-silent {'on' if skip else 'off'}"
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
