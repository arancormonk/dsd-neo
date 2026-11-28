# SPDX-License-Identifier: GPL-3.0-or-later
"""The quiet calibration driver's outputs (issue #834): every record lands, and a run whose output fails says so.

The driver (dsd-neo_test_signal_quiet_calibrate) runs on a small source set this check writes: one digital and one
analog transmitter of tones inside their widths, and the adjacent-channel row the neighbour scenes take, at 1.536 MS/s,
three trials each of the four ".../1536k/grid0" conditions (one analog, three noise alone).

- A good run exits 0 and writes the CSV, --dump (8 bytes a trial: ratio and rise) and --trial-log (12 bytes a trial:
  occupancy, clip crossings, status), rows in the CSV's order. Each completed trial's status carries the detector's own
  decision: valid exactly when no void cause is set, quiet only when valid, and per row the quiet bits count the CSV's
  quiet column and the void bits its invalid column. So does a selection run (both estimators) under the guard and the
  occupancy check, whose decision the detector makes there too: with the check on everywhere (share 0.000001), every
  trial whose share is over it is void for occupancy.
- A --dump or --trial-log that cannot be opened (a directory) stops the run before it starts: nonzero, nothing written.
- One whose writes do not land (/dev/full, where present) exits nonzero once the run ends.
- The driver attests what it ran on one "run:" line (phase, salt, veto, guard, threshold, estimator, geometry,
  response, depression, ADC, and for a --windows run its sub-window count), and --bounds carries each condition's
  metadata (group, kind, rate, classes, widths, S/N, the run's columns) and an occupied capture's asked and achieved
  share, contrast and placement.
- A run that completes prints one "complete:" record: the run line's fields, the trials, conditions and rows, and the
  sha256 of the CSV, dump and trial log it closed, which match the files.
- --bounds gives each condition's channel width, the width a depression is measured in: a noise-only row's is its own
  class's (6.25 kHz for noise/6k25) or analog width.
- The late groups (--add cross, raised) only append: the conditions of --add masks,occupancy keep their order and
  names (and so their seeds), and cross adds the -8 masks crossed with neighbours, offsets and droop.
- A run with a trial that reaches the clip level fails (B8), with or without --trial-log: nonzero, the condition named,
  no completion record. The analog transmitter at 90 dB S/N clips once --scene-ceiling 1000 stops its scene from being
  scaled down; at the default ceiling it is scaled under the clip level and the run completes with no crossing.
- The completion record's digests are of the bytes written, so an output that cannot be read back (a FIFO, where the
  platform has them) closes the run with a digest that matches what its reader got, instead of hanging.
- --threshold and --estimator are refused outside validation (selection judges both estimators at no threshold, the
  snr phase at none), so no run attests a parameter it did not use; selection's attestation stays as it was.
- The snr phase renders at the scene's scale and fails on a render that clips, as the trials do: the 90 dB scene fails
  at --scene-ceiling 1000 and measures its S/N at the default ceiling; --seed-salt gives it fresh seeds (0 the shipped
  ones).
- A trial the detector did not measure (an analog width it refuses) fails the run with no completion record; a
  reading the guard or the occupancy check voided is a measurement and still completes (the selection run above).
- Two outputs naming one file (the same path, another spelling of it, a hard link), or an output that is a symbolic
  link, whether or not its target is there, are refused before any is opened.
- --scene-ceiling never goes under its default: scaled toward the power floor, every ratio would read 0 dB.
- --depression and --adc-noise-db must be whole tenths of a dB, the precision the CSV records them at.
"""
import argparse
import csv
import errno
import hashlib
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading

TRIALS = 3
FILTER = "/1536k/grid0"
WANT_ROWS = ["analog/ana/1536k/grid0", "noise/12k5/1536k/grid0", "noise/6k25/1536k/grid0", "noise/nfm_12k5/1536k/grid0"]
SOURCE_SAMPLES = 96000  # two seconds at 48 kHz, longer than any trial
STATUS_DONE = 0
VOID_SHIFT = 16
VALID = 1 << 24
QUIET = 1 << 25


def write_tones(path, half_width_hz, seed):
    """Twenty tones of random frequency within +-half_width_hz and random phase, as complex float32 at 48 kHz."""
    state = seed
    tones = []
    for _ in range(20):
        state = (state * 1103515245 + 12345) % (1 << 31)
        freq = (state / (1 << 31) * 2.0 - 1.0) * half_width_hz
        state = (state * 1103515245 + 12345) % (1 << 31)
        tones.append((2.0 * math.pi * freq / 48000.0, state / (1 << 31) * 2.0 * math.pi))
    buf = bytearray()
    for k in range(SOURCE_SAMPLES):
        re = sum(math.cos(w * k + p) for w, p in tones) / len(tones)
        im = sum(math.sin(w * k + p) for w, p in tones) / len(tones)
        buf += struct.pack("<ff", re, im)
    path.write_bytes(bytes(buf))


def write_inputs(root):
    for row, half, seed in (("dig", 3000.0, 1), ("ana", 4000.0, 2), ("p25p1", 3000.0, 3)):
        write_tones(root / f"{row}.cf32", half, seed)
    rows = "".join(f"{row}\t{row}\t12500\t48000\t{SOURCE_SAMPLES}\t0.05\t0\n" for row in ("dig", "ana", "p25p1"))
    (root / "sources.tsv").write_text("row\tmode\twidth\trate\tsamples\tpower\tburst\n" + rows)
    (root / "modes.tsv").write_text(
        "name\ttag\trow\treception\twidth\tclass\tanalog\n"
        "dig\tdig\tdig\t6.0\t12500\t12k5\t0\n"
        "ana\tnfm_12k5\tana\t8.0\t12500\tanalog\t1\n"
    )


VALIDATE = ["--phase", "validate", "--threshold", "5", "--estimator", "trimmed", "--guard", "8.7"]
SELECT = ["--phase", "select", "--guard", "8.7", "--occupancy-veto", "0.000001"]
OCCUPANCY = 8  # DSD_SIGNAL_QUIET_VOID_OCCUPANCY


def run(driver, root, out, extra, phase=None, modes="modes.tsv"):
    args = [driver] + (phase or VALIDATE) + [
        "--sources", str(root), "--modes-file", str(root / modes), "--out", str(out),
        "--trials", str(TRIALS), "--jobs", "2", "--rates", "1536000", "--filter", FILTER] + extra
    return subprocess.run(args, capture_output=True, text=True, timeout=120, check=False)


def fail(message):
    print(f"FAIL: {message}", file=sys.stderr)
    return 1


def check_good_run(driver, root, phase=None):
    select = phase is not None
    out, dump, log = root / "good.csv", root / "good.dump", root / "good.log"
    done = run(driver, root, out, ["--dump", str(dump), "--trial-log", str(log)], phase)
    if done.returncode != 0:
        return fail(f"a good run exits {done.returncode}: {done.stderr}")
    with out.open(newline="") as f:
        rows = list(csv.DictReader(f))
    want = [c for c in WANT_ROWS for _ in ((0, 1) if select else (0,))]
    if [r["condition"] for r in rows] != want:
        return fail(f"rows {[r['condition'] for r in rows]}")
    if dump.stat().st_size != 8 * TRIALS * len(rows) or log.stat().st_size != 12 * TRIALS * len(rows):
        return fail(f"dump {dump.stat().st_size} B, trial log {log.stat().st_size} B")
    records = list(struct.iter_unpack("<fII", log.read_bytes()))
    rc = 0
    for i, row in enumerate(rows):
        quiet = invalid = 0
        for occupancy, status in ((r[0], r[2]) for r in records[i * TRIALS:(i + 1) * TRIALS]):
            if status & 0xFF != STATUS_DONE or not math.isfinite(occupancy):
                rc |= fail(f"{row['condition']}: status {status:#x}, occupancy {occupancy}")
                continue
            causes = (status >> VOID_SHIFT) & 0xFF
            valid = bool(status & VALID)
            if valid == bool(causes) or (status & QUIET and not valid) or status & ~(VALID | QUIET | 0xFF0000):
                rc |= fail(f"{row['condition']}: inconsistent decision {status:#x}")
            if select and bool(causes & OCCUPANCY) != (occupancy > 0.000001):
                rc |= fail(f"{row['condition']}: share {occupancy}, causes {causes:#x}")
            quiet += bool(status & QUIET)
            invalid += not valid
        if select:
            if invalid != int(row["invalid"]):
                rc |= fail(f"{row['condition']}: bits say {invalid} invalid; CSV {row['invalid']}")
            continue
        if quiet != int(row["quiet"]) or invalid != int(row["invalid"]):
            rc |= fail(f"{row['condition']}: bits say {quiet} quiet, {invalid} invalid; CSV {row['quiet']}, "
                       f"{row['invalid']}")
        if int(row["quiet"]) + int(row["busy"]) != TRIALS:
            rc |= fail(f"{row['condition']}: quiet + busy != trials")
    return rc


def check_unopenable(driver, root):
    rc = 0
    for option in ("--dump", "--trial-log"):
        out = root / f"unopenable{option}.csv"
        done = run(driver, root, out, [option, str(root)])
        if done.returncode == 0 or f"cannot write {root}" not in done.stderr:
            rc |= fail(f"{option} to a directory: exit {done.returncode}, {done.stderr!r}")
        if "conditions done" in done.stderr:
            rc |= fail(f"{option} to a directory: the run went ahead")
    return rc


def check_full_disk(driver, root):
    if not os.path.exists("/dev/full"):
        return 0
    rc = 0
    for option in ("--dump", "--trial-log"):
        done = run(driver, root, root / f"full{option}.csv", [option, "/dev/full"])
        if done.returncode == 0 or "write to /dev/full failed" not in done.stderr:
            rc |= fail(f"{option} to /dev/full: exit {done.returncode}, {done.stderr!r}")
    return rc


def condition_names(driver, root, groups):
    bounds = root / f"bounds-{groups.replace(',', '-')}.csv"
    done = subprocess.run([driver, "--phase", "select", "--sources", str(root), "--modes-file", str(root / "modes.tsv"),
                           "--out", os.devnull, "--trials", "1", "--jobs", "1", "--rates", "1536000", "--filter",
                           "NO-SUCH", "--add", groups, "--bounds", str(bounds)],
                          capture_output=True, text=True, timeout=120, check=False)
    if done.returncode != 0:
        return None
    with bounds.open(newline="") as f:
        return [r["condition"] for r in csv.DictReader(f)]


def check_late_groups(driver, root):
    early = condition_names(driver, root, "masks,occupancy")
    late = condition_names(driver, root, "masks,occupancy,cross,raised")
    if early is None or late is None:
        return fail("a --bounds run failed")
    if late[:len(early)] != early or len(set(late)) != len(late):
        return fail("the late groups moved or repeated an earlier condition")
    added = {name.split("/")[0] for name in late[len(early):]}
    if added != {"mask_cross"}:
        return fail(f"late groups added {sorted(added)}")
    return 0


def check_attestation(driver, root):
    done = run(driver, root, root / "attest.csv",
               ["--seed-salt", "9", "--occupancy-veto", "0.05", "--bounds", str(root / "attest-bounds.csv")])
    want = "run: phase=validate salt=9 veto=0.05 guard=8.7 threshold=5 estimator=trimmed default,flat,0.0,none"
    if done.returncode != 0 or want not in done.stderr.splitlines():
        return fail(f"attestation: exit {done.returncode}, {[l for l in done.stderr.splitlines() if l.startswith('run:')]}")
    records = [line for line in done.stderr.splitlines() if line.startswith("complete: ")]
    digest = {k: hashlib.sha256((root / f"attest{ext}").read_bytes()).hexdigest() if ext else ""
              for k, ext in (("csv", ".csv"), ("dump", ""), ("log", ""))}
    fields = dict(f.split("=", 1) for f in records[0].split() if "=" in f) if len(records) == 1 else {}
    if (len(records) != 1 or not records[0].startswith("complete: " + want[len("run: "):])
            or fields.get("trials") != str(TRIALS) or fields.get("rows") != "4" or fields.get("conditions") != "4"
            or fields.get("csv") != digest["csv"] or fields.get("dump") != "none" or fields.get("log") != "none"):
        return fail(f"completion record {records}")
    with (root / "attest-bounds.csv").open(newline="") as f:
        rows = list(csv.DictReader(f))
    columns = ["condition", "bound", "scale", "group", "kind", "rate_hz", "classes", "analog_width_hz",
               "grid_shift_hz", "snr_db", "geometry", "response", "depression_db", "adc_noise_db", "requested_share",
               "achieved_share", "contrast_db", "placement", "channel_width_hz"]
    if not rows or list(rows[0].keys()) != columns:
        return fail(f"bounds columns {list(rows[0].keys()) if rows else None}")
    noise = [r for r in rows if r["condition"] == "noise/12k5/1536k/grid0"]
    if len(noise) != 1 or noise[0]["kind"] != "noise" or noise[0]["group"] != "noise" or noise[0]["rate_hz"] != "1536000":
        return fail(f"bounds metadata {noise}")
    # The channel width a depression is measured in: each noise-only row's own class (or analog) width.
    widths = {r["condition"]: r["channel_width_hz"] for r in rows if r["condition"].startswith("noise/")
              and r["condition"].endswith("/1536k/grid0")}
    want_widths = {"noise/12k5/1536k/grid0": "12500", "noise/6k25/1536k/grid0": "6250",
                   "noise/nfm_12k5/1536k/grid0": "12500"}
    if widths != want_widths:
        return fail(f"noise-only channel widths {widths}")
    short = run(driver, root, root / "attest-short.csv",
                ["--seed-salt", "9", "--occupancy-veto", "0.05", "--windows", "3"])
    lines = short.stderr.splitlines()
    want_short = want + " windows=3"
    completed = [line for line in lines if line.startswith("complete: " + want_short[len("run: "):] + " trials=")]
    if short.returncode != 0 or want_short not in lines or len(completed) != 1:
        return fail(f"--windows attestation: exit {short.returncode}, "
                    f"{[l for l in lines if l.startswith(('run:', 'complete:'))]}")
    return 0


def check_clipping(driver, root):
    """B8: a trial at or over the clip level fails the run, whether or not --trial-log counts it there."""
    (root / "modes-loud.tsv").write_text((root / "modes.tsv").read_text().replace("ana\tnfm_12k5\tana\t8.0",
                                                                                  "ana\tnfm_12k5\tana\t90.0"))
    rc = 0
    clipped = "condition analog/ana/1536k/grid0: 3 trial(s) clip"
    for phase, name in ((VALIDATE, "validate"), (SELECT, "select")):
        for logged in (True, False):
            log = root / f"clip-{name}-{logged}.log"
            extra = ["--scene-ceiling", "1000"] + (["--trial-log", str(log)] if logged else [])
            done = run(driver, root, root / f"clip-{name}-{logged}.csv", extra, phase, "modes-loud.tsv")
            label = f"{name}, a clipping scene, trial log {'on' if logged else 'off'}"
            if done.returncode == 0 or clipped not in done.stderr or "complete:" in done.stderr:
                rc |= fail(f"{label}: exit {done.returncode}, {done.stderr!r}")
            if logged:
                records = list(struct.iter_unpack("<fII", log.read_bytes())) if log.exists() else []
                if not records or not all(r[1] > 0 for r in records[:TRIALS]):
                    rc |= fail(f"{label}: the trial log's crossings {[r[1] for r in records[:TRIALS]]}")
    log = root / "clip-scaled.log"
    done = run(driver, root, root / "clip-scaled.csv", ["--trial-log", str(log)], None, "modes-loud.tsv")
    records = list(struct.iter_unpack("<fII", log.read_bytes())) if log.exists() else []
    if done.returncode != 0 or "complete:" not in done.stderr or not records or any(r[1] for r in records):
        rc |= fail(f"the same scene scaled under the ceiling: exit {done.returncode}, {done.stderr!r}")
    return rc


def check_fifo_output(driver, root):
    """A --dump to a FIFO: the run ends, and its completion record's digest is of what the FIFO's reader got."""
    if not hasattr(os, "mkfifo"):
        return 0
    fifo = root / "dump.fifo"
    os.mkfifo(fifo)
    got = []

    def drain():
        with open(fifo, "rb") as f:
            got.append(f.read())

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    try:
        done = run(driver, root, root / "fifo.csv", ["--dump", str(fifo)])
    except subprocess.TimeoutExpired:
        return fail("a --dump to a FIFO: the run hangs after its trials")
    reader.join(10)
    records = [line for line in done.stderr.splitlines() if line.startswith("complete: ")]
    fields = dict(f.split("=", 1) for f in records[0].split() if "=" in f) if len(records) == 1 else {}
    if done.returncode != 0 or not got or fields.get("dump") != hashlib.sha256(got[0]).hexdigest():
        return fail(f"a --dump to a FIFO: exit {done.returncode}, {records}, {len(got[0]) if got else None} B read")
    if fields.get("csv") != hashlib.sha256((root / "fifo.csv").read_bytes()).hexdigest():
        return fail(f"a --dump to a FIFO: the CSV's digest {fields.get('csv')}")
    return 0


def check_validation_only_options(driver, root):
    """--threshold and --estimator outside validation are refused; selection attests what it always did."""
    rc = 0
    for phase, extra in ((SELECT, ["--threshold", "5"]), (SELECT, ["--estimator", "trimmed"]),
                         (["--phase", "snr"], ["--threshold", "5"])):
        out = root / "refused.csv"
        done = run(driver, root, out, extra, phase)
        if done.returncode == 0 or "--threshold and --estimator are for --phase validate only" not in done.stderr:
            rc |= fail(f"{phase[1]} {extra}: exit {done.returncode}, {done.stderr!r}")
    done = run(driver, root, root / "select-attest.csv", [], SELECT)
    want = "run: phase=select salt=0 veto=1e-06 guard=8.7 threshold=0 estimator=both default,flat,0.0,none"
    if done.returncode != 0 or want not in done.stderr.splitlines():
        rc |= fail(f"selection's attestation: {[l for l in done.stderr.splitlines() if l.startswith('run:')]}")
    return rc


def snr_rows(driver, root, extra, modes="modes-loud.tsv"):
    out = root / "snr.csv"
    if out.exists():
        out.unlink()
    done = run(driver, root, out, extra, ["--phase", "snr"], modes)
    rows = list(csv.DictReader(out.open(newline=""))) if out.exists() else []
    return done, rows


def check_snr_phase(driver, root):
    """The snr phase: clipping fails it at the raised ceiling; the default ceiling's scale keeps the S/N; the salt."""
    rc = 0
    done, _ = snr_rows(driver, root, ["--scene-ceiling", "1000"])
    if done.returncode == 0 or "condition analog/ana/1536k/grid0:" not in done.stderr or "clip" not in done.stderr:
        rc |= fail(f"snr, a clipping scene: exit {done.returncode}, {done.stderr!r}")
    done, rows = snr_rows(driver, root, [])
    if done.returncode != 0 or len(rows) != 1 or abs(float(rows[0]["measured_snr_db"]) -
                                                     float(rows[0]["target_snr_db"])) > 1.0:
        rc |= fail(f"snr, the scene scaled under the ceiling: exit {done.returncode}, {rows}, {done.stderr!r}")
    measured = {}
    for salt in (None, "0", "5"):
        done, rows = snr_rows(driver, root, [] if salt is None else ["--seed-salt", salt], "modes.tsv")
        measured[salt] = [r["measured_snr_db"] for r in rows] if done.returncode == 0 else None
    if measured[None] is None or measured["0"] != measured[None] or measured["5"] == measured[None]:
        rc |= fail(f"snr seed salt: {measured}")
    return rc


def check_unmeasured(driver, root):
    """An analog width the detector refuses: every such trial unmeasured, the run fails, no completion record."""
    (root / "modes-wide.tsv").write_text((root / "modes.tsv").read_text().replace(
        "ana\tnfm_12k5\tana\t8.0\t12500\tanalog\t1", "ana\tnfm_30k\tana\t8.0\t30000\tanalog\t1"))
    done = run(driver, root, root / "wide.csv", [], None, "modes-wide.tsv")
    want = f"condition analog/ana/1536k/grid0: trials not measured: {TRIALS} refused, 0 unfinished, 0 failed"
    if done.returncode == 0 or want not in done.stderr or "complete:" in done.stderr:
        return fail(f"a refused width: exit {done.returncode}, {done.stderr!r}")
    return 0


def check_aliased_outputs(driver, root):
    """Two outputs naming one file are refused before any output is opened: the file keeps what it held."""
    rc = 0
    keep = root / "keep.bin"
    keep.write_bytes(b"keep")
    os.link(keep, root / "keep-link.bin")
    spelled = f"{root}/./keep.bin"
    for extra in (["--trial-log", str(keep)], ["--dump", str(keep), "--trial-log", spelled],
                  ["--dump", str(root / "keep-link.bin"), "--trial-log", str(keep)], ["--bounds", str(keep)]):
        out = keep if extra[0] in ("--trial-log", "--bounds") else root / "alias.csv"
        done = run(driver, root, out, extra)
        if done.returncode == 0 or "name one file" not in done.stderr or keep.read_bytes() != b"keep":
            rc |= fail(f"aliased outputs {extra}: exit {done.returncode}, {done.stderr!r}, {keep.read_bytes()!r}")
    # A symbolic link as an output, to the CSV the run would create or to a file that is there: refused, nothing created
    # or written through it. Each case runs as soon as its link is made; only a host that cannot make symbolic links
    # (an unprivileged Windows runner, a filesystem without them) skips them.
    if hasattr(os, "symlink"):
        target = root / "linked.csv"
        for link, points_to in (("linked.dump", target), ("linked-keep.dump", keep)):
            try:
                os.symlink(points_to, root / link)
            except (NotImplementedError, PermissionError):
                return rc
            except OSError as err:
                # The filesystem or the account cannot make one: ENOTSUP/EOPNOTSUPP, ENOSYS, and on Windows
                # ERROR_NOT_SUPPORTED (50) and ERROR_PRIVILEGE_NOT_HELD (1314). Anything else is a failure.
                names = ("ENOTSUP", "EOPNOTSUPP", "ENOSYS")
                unsupported = {getattr(errno, name) for name in names if hasattr(errno, name)}
                if err.errno in unsupported or getattr(err, "winerror", None) in (50, 1314):
                    return rc
                raise
            done = run(driver, root, target, ["--dump", str(root / link)])
            if (done.returncode == 0 or "symbolic link" not in done.stderr or target.exists()
                    or keep.read_bytes() != b"keep"):
                rc |= fail(f"a link {link} as an output: exit {done.returncode}, {done.stderr!r}")
    return rc


def check_scene_ceiling_floor(driver, root):
    """--scene-ceiling under the default would scale scenes toward the power floor: refused; the default itself runs."""
    rc = 0
    for ceiling, ok in (("1e-8", False), ("0.5", False), ("0.9", True)):
        done = run(driver, root, root / "ceiling.csv", ["--scene-ceiling", ceiling])
        if (done.returncode == 0) != ok:
            rc |= fail(f"--scene-ceiling {ceiling}: exit {done.returncode}, {done.stderr[-200:]!r}")
    return rc


def check_response_grid(driver, root):
    """--depression and --adc-noise-db off the 0.1 dB grid are refused; on it they run."""
    rc = 0
    base = ["--response", "nominal", "--rates", "2500000", "--filter", "NO-SUCH"]
    for extra, ok in ((["--depression", "1.96"], False), (["--depression", "2"], True),
                      (["--adc-noise-db", "0.05"], False), (["--adc-noise-db", "-10"], True)):
        args = [driver, "--phase", "select", "--sources", str(root), "--modes-file", str(root / "modes.tsv"), "--out",
                str(root / "grid.csv"), "--trials", "1", "--jobs", "1"] + base + extra
        done = subprocess.run(args, capture_output=True, text=True, timeout=120, check=False)
        if (done.returncode == 0) != ok:
            rc |= fail(f"{extra}: exit {done.returncode}, {done.stderr[-300:]!r}")
    return rc


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("driver")
    driver = parser.parse_args().driver
    with tempfile.TemporaryDirectory(prefix="dsd-quiet-calibrate-outputs-") as temporary:
        root = Path(temporary)
        write_inputs(root)
        rc = check_good_run(driver, root)
        rc |= check_good_run(driver, root, SELECT)
        rc |= check_unopenable(driver, root)
        rc |= check_full_disk(driver, root)
        rc |= check_late_groups(driver, root)
        rc |= check_attestation(driver, root)
        rc |= check_clipping(driver, root)
        rc |= check_fifo_output(driver, root)
        rc |= check_validation_only_options(driver, root)
        rc |= check_snr_phase(driver, root)
        rc |= check_unmeasured(driver, root)
        rc |= check_aliased_outputs(driver, root)
        rc |= check_response_grid(driver, root)
        rc |= check_scene_ceiling_floor(driver, root)
    return rc


if __name__ == "__main__":
    sys.exit(main())
