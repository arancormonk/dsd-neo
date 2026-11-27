# SPDX-License-Identifier: GPL-3.0-or-later
"""The quiet check's calibration driver checks the sources of the conditions it will render, and only those (#834).

dsd-neo_test_signal_quiet_calibrate refuses, before it renders anything, a condition whose source is too short to
place its trial. It checks the conditions --filter selects for the phase it runs, so a short source that no selected
condition uses does not stop the run, and one that a selected condition uses does. The source set here is tiny and
made on the spot: P25 Phase 1 and NFM rows long enough for every condition, and a DMR MS burst train of 0.5 s, too
short for a 50-slice reading (--windows 50). Each run renders at most one condition, so the whole check takes seconds.

The snr phase renders a condition's wanted carriers only, so a neighbour condition runs there without the source its
neighbours take (the p25p1 row): on a second source set without that row it renders, places and batches the wanted
carriers alone, never the absent neighbour's (an index before the source list, which a sanitizer build reports).
"""
import array
import math
import os
import subprocess
import sys
import tempfile

RATE = 48000
LONG_S = 1.5  # long enough for every condition's trial (and the snr phase's longer render) at 50 slices
SHORT_S = 0.5  # the DMR MS train: too short for a 50-slice trial, 100 ms clear of both ends
WINDOWS = "50"


def write_row(folder, row, seconds, tone_hz, bursts):
    """One source row: complex float32 samples (a tone), and for a burst train its one-byte-per-sample on-mask (keyed
    27.5 ms every 60 ms after a 200 ms lead, the samples zero outside a burst). Returns the row's sources.tsv line."""
    n = round(seconds * RATE)
    on = bytearray(n)
    samples = array.array("f")
    for i in range(n):
        keyed = not bursts or (i >= RATE // 5 and (i - RATE // 5) % (RATE * 60 // 1000) < RATE * 275 // 10000)
        on[i] = 1 if keyed else 0
        phase = 2 * math.pi * tone_hz * i / RATE
        samples.extend((math.cos(phase), math.sin(phase)) if keyed else (0.0, 0.0))
    if sys.byteorder != "little":
        samples.byteswap()
    with open(os.path.join(folder, row + ".cf32"), "wb") as handle:
        samples.tofile(handle)
    if bursts:
        with open(os.path.join(folder, row + ".on"), "wb") as handle:
            handle.write(bytes(on))
    return f"{row}\t{row}\t12500\t{RATE}\t{n}\t1.0e+00\t{1 if bursts else 0}\tcheck\n"


def make_sources(folder):
    """sources.tsv and the three rows, and the modes file naming them (P25 Phase 1 and DMR MS digital, NFM analog)."""
    lines = ["row\tmode\twidth_hz\trate_hz\tsamples\tband_power\tburst\tsource\n"]
    lines.append(write_row(folder, "p25p1", LONG_S, 1200.0, False))
    lines.append(write_row(folder, "dmr_ms", SHORT_S, 1800.0, True))
    lines.append(write_row(folder, "nfm_12k5", LONG_S, 1000.0, False))
    with open(os.path.join(folder, "sources.tsv"), "w", encoding="ascii") as handle:
        handle.writelines(lines)
    modes = os.path.join(folder, "modes.tsv")
    with open(modes, "w", encoding="ascii") as handle:
        handle.write("name\ttag\trow\treception_db\twidth_hz\tclass\tanalog\n")
        handle.write("p25p1\tp25p1\tp25p1\t8.0\t12500\t12k5\t0\n")
        handle.write("dmr_ms\tdmr_ms\tdmr_ms\t7.5\t12500\t12k5\t0\n")
        handle.write("nfm_12k5\tnfm_12k5\tnfm_12k5\t9.5\t12500\tanalog\t1\n")
    return modes


def make_sources_without_neighbour(folder):
    """The NFM row alone (no p25p1 row for a neighbour), and the modes file naming it."""
    lines = ["row\tmode\twidth_hz\trate_hz\tsamples\tband_power\tburst\tsource\n"]
    lines.append(write_row(folder, "nfm_12k5", LONG_S, 1000.0, False))
    with open(os.path.join(folder, "sources.tsv"), "w", encoding="ascii") as handle:
        handle.writelines(lines)
    modes = os.path.join(folder, "modes.tsv")
    with open(modes, "w", encoding="ascii") as handle:
        handle.write("name\ttag\trow\treception_db\twidth_hz\tclass\tanalog\n")
        handle.write("nfm_12k5\tnfm_12k5\tnfm_12k5\t9.5\t12500\tanalog\t1\n")
    return modes


def run(driver, folder, modes, phase, condition):
    """The driver's exit code, its stderr and its output's lines (None when it wrote none), for one condition."""
    out = os.path.join(folder, f"{phase}-{condition.replace('/', '_')}.csv")
    cmd = [driver, "--phase", phase, "--sources", folder, "--modes-file", modes, "--windows", WINDOWS]
    cmd += ["--filter", condition, "--trials", "2", "--jobs", "1", "--out", out]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120, check=False)
    lines = None
    if os.path.exists(out):
        with open(out, encoding="ascii") as handle:
            lines = handle.read().splitlines()
    return proc.returncode, proc.stderr, lines


def main():
    driver = sys.argv[1]
    failures = []

    def expect(ok, message):
        print(("ok    " if ok else "FAIL  ") + message)
        if not ok:
            failures.append(message)

    with tempfile.TemporaryDirectory(prefix="quiet-calibrate-") as folder:
        modes = make_sources(folder)
        for phase in ("select", "snr"):
            # A selected condition on the short source is refused before anything is rendered or written.
            short = "digital/dmr_ms/1536k/off+0"
            code, err, lines = run(driver, folder, modes, phase, short)
            expect(code != 0, f"{phase}: a selected condition on the short DMR source is refused (exit {code})")
            named = f"condition {short}: source dmr_ms" in err and "too short" in err
            expect(named, f"{phase}: the refusal names the condition and its source: {err!r}")
            expect(lines is None, f"{phase}: nothing is written for a refused run")
            # The same source set, the short source excluded by the filter: the run goes ahead.
            for condition in ("digital/p25p1/1536k/off+0", "noise/12k5/1024k/grid0"):
                if phase == "snr" and condition.startswith("noise/"):
                    continue  # the snr phase measures wanted signals only
                code, err, lines = run(driver, folder, modes, phase, condition)
                expect(code == 0, f"{phase} {condition}: runs with the short source excluded (exit {code}): {err!r}")
                rows = [line.split(",")[0] for line in (lines or [])[1:]]
                only = bool(rows) and set(rows) == {condition}
                expect(only, f"{phase} {condition}: only that condition is written: {rows}")
    with tempfile.TemporaryDirectory(prefix="quiet-calibrate-") as folder:
        modes = make_sources_without_neighbour(folder)
        condition = "neighbour/nfm_12k5/1536k/12.5k/+50dB/both"
        code, err, lines = run(driver, folder, modes, "snr", condition)
        expect(code == 0, f"snr {condition} with no p25p1 row: runs (exit {code}): {err!r}")
        rows = [line.split(",")[0] for line in (lines or [])[1:]]
        expect(rows == [condition], f"snr {condition} with no p25p1 row: its row is written: {rows}")
        code, err, lines = run(driver, folder, modes, "select", condition)
        expect(code != 0 and "missing source" in err, f"select {condition} with no p25p1 row: refused (exit {code})")
    if failures:
        print(f"{len(failures)} check(s) failed")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
