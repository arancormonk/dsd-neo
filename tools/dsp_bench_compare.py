#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare two dsd-neo DSP benchmark CSV files."""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Row:
    case: str
    metric: float
    item_unit: str
    simd_impl: str
    rate_hz: str
    profile: str
    tap_count: str
    variant: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path, help="Baseline benchmark CSV")
    parser.add_argument("candidate", type=Path, help="Candidate benchmark CSV")
    parser.add_argument(
        "--metric",
        default="median_ns_per_item",
        choices=("median_ns_per_item", "median_ns_per_call", "items_per_second"),
        help="CSV metric to compare",
    )
    parser.add_argument("--filter", default="", help="Only compare case names containing this substring")
    parser.add_argument("--threshold", type=float, default=5.0, help="Percent delta threshold to mark changes")
    parser.add_argument(
        "--sort",
        default="abs-delta",
        choices=("abs-delta", "case"),
        help="Sort output by absolute percent delta or case name",
    )
    return parser.parse_args()


def read_csv(path: Path, metric: str, case_filter: str) -> dict[str, Row]:
    rows: dict[str, Row] = {}
    with path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        missing = {"case", metric} - set(reader.fieldnames or ())
        if missing:
            raise ValueError(f"{path}: missing CSV columns: {', '.join(sorted(missing))}")
        for raw in reader:
            case = raw.get("case", "")
            if not case or (case_filter and case_filter not in case):
                continue
            try:
                value = float(raw.get(metric, "nan"))
            except ValueError:
                value = math.nan
            if not math.isfinite(value):
                continue
            rows[case] = Row(
                case=case,
                metric=value,
                item_unit=raw.get("item_unit", ""),
                simd_impl=raw.get("simd_impl", ""),
                rate_hz=raw.get("rate_hz", ""),
                profile=raw.get("profile", ""),
                tap_count=raw.get("tap_count", ""),
                variant=raw.get("variant", ""),
            )
    return rows


def pct_delta(base: float, cand: float, metric: str) -> float:
    if base == 0.0:
        return math.nan
    delta = ((cand - base) / base) * 100.0
    if metric == "items_per_second":
        delta = -delta
    return delta


def main() -> int:
    args = parse_args()
    try:
        baseline = read_csv(args.baseline, args.metric, args.filter)
        candidate = read_csv(args.candidate, args.metric, args.filter)
    except (OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    common = sorted(set(baseline) & set(candidate))
    if not common:
        print("No common benchmark cases found.", file=sys.stderr)
        return 1

    compared = []
    for case in common:
        b = baseline[case]
        c = candidate[case]
        delta = pct_delta(b.metric, c.metric, args.metric)
        compared.append((case, b, c, delta))

    if args.sort == "abs-delta":
        compared.sort(key=lambda row: abs(row[3]) if math.isfinite(row[3]) else -1.0, reverse=True)

    threshold = abs(args.threshold)
    print(f"metric={args.metric} threshold={threshold:.2f}%")
    print("case,baseline,candidate,delta_pct,status,unit,simd,rate_hz,profile,tap_count,variant")
    for case, base, cand, delta in compared:
        status = "same"
        if math.isfinite(delta) and abs(delta) >= threshold:
            status = "faster" if delta < 0.0 else "slower"
        print(
            f"{case},{base.metric:.6f},{cand.metric:.6f},{delta:.3f},{status},"
            f"{cand.item_unit},{cand.simd_impl},{cand.rate_hz},{cand.profile},{cand.tap_count},{cand.variant}"
        )

    missing_candidate = sorted(set(baseline) - set(candidate))
    missing_baseline = sorted(set(candidate) - set(baseline))
    if missing_candidate:
        print(f"\nMissing from candidate: {len(missing_candidate)}", file=sys.stderr)
    if missing_baseline:
        print(f"New in candidate: {len(missing_baseline)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
