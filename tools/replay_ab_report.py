#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a tools/replay_ab.sh run.

Repeats are matched blocks: every build replays the same capture inside the same
repeat, under the same machine conditions. The comparison that survives the
pipeline's run-to-run variation is therefore the per-repeat difference, not the
difference of the means -- on the captures behind issue #444 the within-build
spread was large enough to reverse a blocked comparison.

Digital runs report errors per decoded voice frame. A build that loses sync
decodes fewer frames and accrues fewer errors without being better, so the raw
total flatters exactly the regressions worth catching.

Analog runs (replay_ab.sh --metric analog) report each analog column the host
measured -- tone SNR, in-band ratio, clipped samples, audible and first-audible
time, RMS level, the first probe's level (dBFS, and dBc against a test tone), and
time to tone lock -- paired per repeat the same way, plus the received tone label
each build settled on. Probe levels are keyed by the probe's frequency, so builds
that probed different frequencies are never paired. A repeat whose host exited
non-zero (summary.tsv's rc column: a crash, a timeout, or a run the host rejected)
or warned that the front end left the monitor path (off_path: it delivered CQPSK
symbols, so the repeat measures the modulation auto-switch rather than the build)
is left out of every column and counted in a warning, even when the host printed
its metrics first. Columns no build measured are left out; a build that has no
value for a column another build measured gets an explicit NA row, and the exit
status is 1 when a build produced no usable analog measurement in any repeat,
since a report of the remaining builds would read like a clean result.
"""

from __future__ import annotations

import argparse
import math
import statistics
import sys
from collections import Counter, defaultdict
from pathlib import Path

ANALOG_NUMERIC = ("tone_snr_db", "inband_db", "clip", "audible_ms", "first_audible_ms", "rms_dbfs", "probe_dbfs",
                  "probe_dbc", "tone_lock_ms")
ANALOG_LABELS = ("tone",)
# A probe level means something only at the probe's frequency (probe_hz).
PROBE_COLUMNS = ("probe_dbfs", "probe_dbc")


def read_rows(path: Path) -> list[dict[str, str]]:
    """Read summary.tsv into one dict per run, keyed by the header's column names."""
    with path.open() as handle:
        header = handle.readline().rstrip("\n").split("\t")
        if header[:1] != ["variant"]:
            raise SystemExit(f"{path}: not a replay_ab summary (unexpected header)")
        rows = []
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) >= 6:
                rows.append(dict(zip(header, parts)))
    return rows


def load(path: Path) -> dict[int, dict[str, tuple[float, int, int]]]:
    """Read summary.tsv into {repeat: {build: (err_per_voice, errs, voice)}}."""
    by_rep: dict[int, dict[str, tuple[float, int, int]]] = defaultdict(dict)
    for row in read_rows(path):
        if row["errs"] == "NA" or int(row["voice"]) == 0:
            continue
        errs, voice = int(row["errs"]), int(row["voice"])
        by_rep[int(row["rep"])][row["variant"]] = (errs / voice, errs, voice)
    return by_rep


def parse_number(text: str | None) -> float | None:
    if text is None or text == "NA" or text == "":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def run_flags(row: dict[str, str]) -> list[str]:
    """Why a repeat is not a usable measurement, if it is not: a non-zero exit status, or the host's warning that
    the front end left the monitor path. Summaries from before replay_ab.sh recorded either have neither column."""
    reasons = []
    if row.get("rc") not in (None, "", "NA", "0"):
        reasons.append("exit")
    if row.get("off_path") == "1":
        reasons.append("off_path")
    return reasons


def analog_key(name: str, row: dict[str, str]) -> str:
    """Column name, with the probe frequency appended to probe levels (probe_dbfs@12500.0)."""
    hz = row.get("probe_hz")
    if name in PROBE_COLUMNS and hz not in (None, "", "NA"):
        return f"{name}@{hz}"
    return name


def load_analog(rows: list[dict[str, str]]) -> dict[str, dict[int, dict[str, object]]]:
    """{column: {repeat: {build: value}}} for every analog column some run measured."""
    columns: dict[str, dict[int, dict[str, object]]] = {}
    for name in ANALOG_NUMERIC + ANALOG_LABELS:
        for row in rows:
            raw = row.get(name)
            value = parse_number(raw) if name in ANALOG_NUMERIC else (raw if raw not in (None, "", "NA") else None)
            if value is not None:
                by_rep = columns.setdefault(analog_key(name, row), defaultdict(dict))
                by_rep[int(row["rep"])][row["variant"]] = value
    return columns


def paired(by_rep: dict[int, dict[str, object]], build: str, baseline: str) -> tuple[str, str]:
    """Mean per-repeat difference from the baseline with a 95% interval, and how many repeats differ."""
    diffs = [by_rep[r][build] - by_rep[r][baseline] for r in sorted(by_rep)
             if build in by_rep[r] and baseline in by_rep[r]]
    if build == baseline or not diffs:
        return "-", "-"
    half = 1.96 * statistics.stdev(diffs) / math.sqrt(len(diffs)) if len(diffs) > 1 else 0.0
    differ = sum(1 for d in diffs if abs(d) > 1e-9)
    return f"{statistics.fmean(diffs):+.2f} +/- {half:.2f}", f"{differ}/{len(diffs)}"


def analog_coverage(rows: list[dict[str, str]]) -> dict[str, set[int]]:
    """{build: usable repeats in which it measured any numeric analog column}. The host always prints audible_ms
    and clip, so an unflagged repeat without any is one whose ANALOG METRIC line never came (a build that is not an
    analog replay host); a flagged repeat is not usable whatever it printed."""
    covered: dict[str, set[int]] = {row["variant"]: set() for row in rows}
    for row in rows:
        if not run_flags(row) and any(parse_number(row.get(name)) is not None for name in ANALOG_NUMERIC):
            covered[row["variant"]].add(int(row["rep"]))
    return covered


def print_flagged(rows: list[dict[str, str]], builds: list[str], reps: list[int]) -> None:
    """Warns, per build, about the repeats run_flags() leaves out and why."""
    for build in builds:
        exits: Counter[str] = Counter()
        off_path = left_out = 0
        for row in rows:
            flags = run_flags(row) if row["variant"] == build else []
            left_out += 1 if flags else 0
            off_path += 1 if "off_path" in flags else 0
            if "exit" in flags:
                exits[row["rc"]] += 1
        if not left_out:
            continue
        why = []
        if exits:
            why.append(f"{sum(exits.values())} exited non-zero (status {', '.join(sorted(exits))})")
        if off_path:
            why.append(f"{off_path} ran off the monitor path (the host warned of CQPSK symbols)")
        print(f"\nwarning: {build}: {left_out} of {len(reps)} repeats left out of the report: {'; '.join(why)}. "
              "Read their logs.")


def probed_frequencies(rows: list[dict[str, str]]) -> dict[str, set[str]]:
    """{build: probe frequencies it reported}."""
    probed: dict[str, set[str]] = defaultdict(set)
    for row in rows:
        if row.get("probe_hz") not in (None, "", "NA"):
            probed[row["variant"]].add(row["probe_hz"])
    return probed


def print_analog_numeric(columns: dict[str, dict[int, dict[str, object]]], builds: list[str], reps: list[int],
                         baseline: str, probed: dict[str, set[str]]) -> None:
    print(f"{'metric':<22} {'build':<24} {'n':>5} {'mean':>9} {'median':>9} {'sd':>7}  {'paired vs baseline':>20}  "
          f"{'differ':>6}")
    for key, by_rep in columns.items():
        if key.split("@")[0] not in ANALOG_NUMERIC:
            continue
        for build in builds:
            vals = [by_rep[r][build] for r in reps if r in by_rep and build in by_rep[r]]
            count = f"{len(vals)}/{len(reps)}"
            if not vals:
                # A build that probed another frequency is reported under that one; anything else missing a
                # column some build measured is shown, not dropped.
                if "@" in key and probed.get(build) and key.split("@", 1)[1] not in probed[build]:
                    continue
                print(f"{key:<22} {build:<24} {count:>5} {'NA':>9} {'NA':>9} {'NA':>7}  {'-':>20}  {'-':>6}")
                continue
            sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
            diff, differ = paired(by_rep, build, baseline)
            print(f"{key:<22} {build:<24} {count:>5} {statistics.fmean(vals):9.2f} {statistics.median(vals):9.2f} "
                  f"{sd:7.2f}  {diff:>20}  {differ:>6}")
    probe_freqs = sorted({key.split("@", 1)[1] for key in columns if "@" in key}, key=float)
    if len(probe_freqs) > 1:
        print(f"\nnote: builds probed different frequencies ({', '.join(probe_freqs)} Hz); probe levels are "
              "paired only at the same frequency.")


def print_analog_coverage(coverage: dict[str, set[int]], builds: list[str], reps: list[int]) -> int:
    """Warns about builds that measured nothing in some or all repeats; 1 when a build measured nothing at all."""
    status = 0
    for build in builds:
        have = len(coverage.get(build, ()))
        if have == 0:
            print(f"\nwarning: {build} produced no usable analog measurement in any of {len(reps)} repeats (it "
                  "crashed, timed out, left the monitor path, or is not an analog replay host); read its logs "
                  "before trusting this report.")
            status = 1
        elif have < len(reps):
            print(f"\nnote: {build} produced usable analog measurements in only {have} of {len(reps)} repeats; "
                  "paired columns use the repeats both builds measured.")
    return status


def print_analog_labels(columns: dict[str, dict[int, dict[str, object]]], builds: list[str], reps: list[int]) -> None:
    for name in ANALOG_LABELS:
        by_rep = columns.get(name)
        if not by_rep:
            continue
        print(f"\n{'label':<22} {'build':<24} {'value':>9} {'agree':>7}")
        for build in builds:
            seen = [by_rep[r][build] for r in reps if r in by_rep and build in by_rep[r]]
            if not seen:
                print(f"{name:<22} {build:<24} {'NA':>9} {'0/' + str(len(reps)):>7}")
                continue
            # Most common label; a tie goes to the one seen in the earliest repeat.
            value = Counter(seen).most_common()[0][1]
            label = next(v for v in seen if seen.count(v) == value)
            print(f"{name:<22} {build:<24} {label:>9} {f'{value}/{len(reps)}':>7}")


def report_analog(rows: list[dict[str, str]], baseline_arg: str | None) -> int:
    if not load_analog(rows):
        raise SystemExit("no analog columns measured; was this run with replay_ab.sh --metric analog "
                         "and an analog replay host?")
    columns = load_analog([row for row in rows if not run_flags(row)])
    reps = sorted({int(row["rep"]) for row in rows})
    builds = list(dict.fromkeys(row["variant"] for row in rows))
    baseline = baseline_arg or builds[0]
    if baseline not in builds:
        raise SystemExit(f"baseline '{baseline}' not present; have: {', '.join(builds)}")

    print(f"repeats: {len(reps)}   baseline: {baseline}   metric: analog\n")
    print_analog_numeric(columns, builds, reps, baseline, probed_frequencies(rows))
    print_analog_labels(columns, builds, reps)
    print_flagged(rows, builds, reps)
    status = print_analog_coverage(analog_coverage(rows), builds, reps)

    print("\nPaired column is the mean per-repeat difference from the baseline with a 95%")
    print("interval; 'differ' counts the repeats where the two builds disagreed at all. Run")
    print("the baseline against a copy of itself first: while the front end stays on the")
    print("monitor path, I/Q replay is sample-deterministic, so that control should read")
    print("+0.00 +/- 0.00 with 0 differing repeats. Repeats that exited non-zero or left")
    print("the monitor path (the host warned of CQPSK symbols) are not measurements of the")
    print("build: they are left out of every column and counted in a warning above.")
    print("Higher is better for tone_snr_db and inband_db, lower for clip, first_audible_ms")
    print("and tone_lock_ms, and lower for a probe that measures an interferer; whether")
    print("audible_ms or rms_dbfs should move depends on the case. 'n' counts the repeats")
    print("in which the build measured that column.")
    return status


def digital_row(by_rep: dict[int, dict[str, tuple[float, int, int]]], reps: list[int], build: str,
                baseline: str) -> str:
    """One build's line of the digital report."""
    vals = [by_rep[r][build][0] for r in reps if build in by_rep[r]]
    voices = [by_rep[r][build][2] for r in reps if build in by_rep[r]]
    diffs = [by_rep[r][build][0] - by_rep[r][baseline][0]
             for r in reps if build in by_rep[r] and baseline in by_rep[r]]
    sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
    if build != baseline and diffs:
        mean_d = statistics.fmean(diffs)
        half = 1.96 * statistics.stdev(diffs) / math.sqrt(len(diffs)) if len(diffs) > 1 else 0.0
        paired_text = f"{mean_d:+.2f} +/- {half:.2f}"
        better = f"{sum(1 for d in diffs if d < 0)}/{len(diffs)}"
    else:
        paired_text, better = "-", "-"
    return (f"{build:>24}  {statistics.fmean(vals):9.2f} {statistics.median(vals):7.2f} {sd:6.2f}  "
            f"{statistics.fmean(voices):6.1f}  {paired_text:>20}  {better:>7}")


def report_digital(path: Path, baseline_arg: str | None) -> int:
    by_rep = load(path)
    if not by_rep:
        raise SystemExit(f"{path}: no usable rows")

    reps = sorted(by_rep)
    builds = sorted({b for row in by_rep.values() for b in row})
    baseline = baseline_arg or by_rep[reps[0]].keys().__iter__().__next__()
    if baseline not in builds:
        raise SystemExit(f"baseline '{baseline}' not present; have: {', '.join(builds)}")

    print(f"repeats: {len(reps)}   baseline: {baseline}\n")
    print(f"{'build':>24}  {'err/voice':>9} {'median':>7} {'sd':>6}  {'voice':>6}  "
          f"{'paired vs baseline':>20}  {'better':>7}")
    for build in builds:
        print(digital_row(by_rep, reps, build, baseline))

    print("\nPaired column is the mean per-repeat difference in errors per voice frame,")
    print("with a 95% interval. Negative beats the baseline; an interval spanning 0 means")
    print("the run did not resolve a difference. Watch the voice column too: a build that")
    print("decodes noticeably fewer frames is losing sync, whatever its error rate says.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("summary", type=Path, help="summary.tsv written by tools/replay_ab.sh")
    parser.add_argument("--baseline", help="build to compare against (default: the first one seen)")
    parser.add_argument("--metric", choices=("auto", "digital", "analog"), default="auto",
                        help="which columns to report (default: analog when any analog column was measured)")
    args = parser.parse_args()

    metric = args.metric
    if metric == "auto":
        metric = "analog" if load_analog(read_rows(args.summary)) else "digital"
    if metric == "analog":
        return report_analog(read_rows(args.summary), args.baseline)
    return report_digital(args.summary, args.baseline)


if __name__ == "__main__":
    sys.exit(main())
