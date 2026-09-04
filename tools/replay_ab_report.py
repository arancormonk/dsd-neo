#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a tools/replay_ab.sh run.

Repeats are matched blocks: every build replays the same capture inside the same
repeat, under the same machine conditions. The comparison that survives the
pipeline's run-to-run variation is therefore the per-repeat difference, not the
difference of the means -- on the captures behind issue #444 the within-build
spread was large enough to reverse a blocked comparison.

Errors are reported per decoded voice frame. A build that loses sync decodes
fewer frames and accrues fewer errors without being better, so the raw total
flatters exactly the regressions worth catching.
"""

from __future__ import annotations

import argparse
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def load(path: Path) -> dict[int, dict[str, tuple[float, int, int]]]:
    """Read summary.tsv into {repeat: {build: (err_per_voice, errs, voice)}}."""
    by_rep: dict[int, dict[str, tuple[float, int, int]]] = defaultdict(dict)
    with path.open() as handle:
        header = handle.readline().rstrip("\n").split("\t")
        if header[:1] != ["variant"]:
            raise SystemExit(f"{path}: not a replay_ab summary (unexpected header)")
        for line in handle:
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 6 or parts[3] == "NA":
                continue
            build, _case, rep, errs, voice, _sync = parts[:6]
            if int(voice) == 0:
                continue
            by_rep[int(rep)][build] = (int(errs) / int(voice), int(errs), int(voice))
    return by_rep


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("summary", type=Path, help="summary.tsv written by tools/replay_ab.sh")
    parser.add_argument("--baseline", help="build to compare against (default: the first one seen)")
    args = parser.parse_args()

    by_rep = load(args.summary)
    if not by_rep:
        raise SystemExit(f"{args.summary}: no usable rows")

    reps = sorted(by_rep)
    builds = sorted({b for row in by_rep.values() for b in row})
    baseline = args.baseline or by_rep[reps[0]].keys().__iter__().__next__()
    if baseline not in builds:
        raise SystemExit(f"baseline '{baseline}' not present; have: {', '.join(builds)}")

    print(f"repeats: {len(reps)}   baseline: {baseline}\n")
    print(f"{'build':>24}  {'err/voice':>9} {'median':>7} {'sd':>6}  {'voice':>6}  "
          f"{'paired vs baseline':>20}  {'better':>7}")
    for build in builds:
        vals = [by_rep[r][build][0] for r in reps if build in by_rep[r]]
        voices = [by_rep[r][build][2] for r in reps if build in by_rep[r]]
        diffs = [by_rep[r][build][0] - by_rep[r][baseline][0]
                 for r in reps if build in by_rep[r] and baseline in by_rep[r]]
        sd = statistics.stdev(vals) if len(vals) > 1 else 0.0
        if build != baseline and diffs:
            mean_d = statistics.fmean(diffs)
            half = 1.96 * statistics.stdev(diffs) / math.sqrt(len(diffs)) if len(diffs) > 1 else 0.0
            paired = f"{mean_d:+.2f} +/- {half:.2f}"
            better = f"{sum(1 for d in diffs if d < 0)}/{len(diffs)}"
        else:
            paired, better = "-", "-"
        print(f"{build:>24}  {statistics.fmean(vals):9.2f} {statistics.median(vals):7.2f} {sd:6.2f}  "
              f"{statistics.fmean(voices):6.1f}  {paired:>20}  {better:>7}")

    print("\nPaired column is the mean per-repeat difference in errors per voice frame,")
    print("with a 95% interval. Negative beats the baseline; an interval spanning 0 means")
    print("the run did not resolve a difference. Watch the voice column too: a build that")
    print("decodes noticeably fewer frames is losing sync, whatever its error rate says.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
