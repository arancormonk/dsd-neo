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
"""
import argparse
import csv
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

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


def run(driver, root, out, extra, phase=None):
    args = [driver] + (phase or VALIDATE) + [
        "--sources", str(root), "--modes-file", str(root / "modes.tsv"), "--out", str(out),
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
    return rc


if __name__ == "__main__":
    sys.exit(main())
