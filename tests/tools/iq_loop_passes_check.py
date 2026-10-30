#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay an I/Q capture under --iq-loop and require every pass to decode the same.

dsd-neo runs until --passes passes have completed (the replay logs each rewind), then is stopped. Each pass's decoded
lines are the text from the first "[" of every line holding --line-marker, with the decode-clock timestamp in front
dropped (the clock runs on across passes). Every pass after the first must decode line for line what the second did,
and match each --count pattern equally often. The first pass starts from a cold decoder, which needs a burst or two to
synchronise where a rewound one is already synchronised, so it may match each pattern up to --first-pass-slack times
fewer.
"""

import argparse
import queue
import re
import subprocess
import sys
import threading
import time

REWIND = re.compile(r"IQ replay: --iq-loop rewound to the start of the capture \(pass (\d+)\)\.")
SANITIZER = re.compile(r"AddressSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|LeakSanitizer|runtime error:")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dsd-bin", required=True)
    ap.add_argument("--fixture", required=True)
    ap.add_argument("--mode", default="-fT")
    ap.add_argument("--passes", type=int, default=3)
    ap.add_argument("--line-marker", default="[TETRA")
    ap.add_argument("--count", action="append", default=[], help="a regex each pass must match equally often")
    ap.add_argument("--min-count", type=int, default=1, help="the fewest matches of each --count pattern per pass")
    ap.add_argument("--first-pass-slack", type=int, default=0,
                    help="how many fewer matches of a --count pattern the cold first pass may have")
    ap.add_argument("--timeout", type=float, default=300.0)
    args = ap.parse_args()

    cmd = [args.dsd_bin, "--frontend", "none", *args.mode.split(), "--iq-loop", "--iq-replay", args.fixture, "-o",
           "null"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    # The lines come through a reader thread, so the deadline holds while dsd-neo prints nothing (a hung replay) as well
    # as while it prints.
    lines = queue.Queue()

    def read_lines():
        assert proc.stdout is not None
        for raw in proc.stdout:
            lines.put(raw)
        lines.put(None)

    threading.Thread(target=read_lines, daemon=True).start()
    passes = [[]]
    log = []
    deadline = time.monotonic() + args.timeout
    stopped = False
    try:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                log.append(f"iq_loop_passes_check: timed out after {args.timeout:g} s")
                break
            try:
                raw = lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if raw is None:
                break
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            log.append(line)
            # The rewind can land mid-line, after a sync prefix the decoder printed for the pass that ended.
            parts = REWIND.split(line)
            for i, part in enumerate(parts):
                if i % 2 == 1:
                    passes.append([])
                    continue
                if args.line_marker in part:
                    passes[-1].append(part[part.index(args.line_marker):])
            if len(passes) > args.passes:
                stopped = True
                break
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()

    text = "\n".join(log)
    if SANITIZER.search(text):
        print("iq_loop_passes_check: sanitizer report detected:\n" + text, file=sys.stderr)
        return 1
    if not stopped:
        print(f"iq_loop_passes_check: {len(passes) - 1} passes completed (rc={proc.returncode}), wanted "
              f"{args.passes}\n" + "\n".join(log[-60:]), file=sys.stderr)
        return 1
    done = passes[:args.passes]
    rc = 0
    for pattern in args.count:
        rx = re.compile(pattern)
        counts = [sum(1 for line in p if rx.search(line)) for p in done]
        print(f"iq_loop_passes_check: /{pattern}/ per pass: {counts}")
        later = counts[1:]
        if (min(counts) < args.min_count or len(set(later)) != 1
                or not later[0] - args.first_pass_slack <= counts[0] <= later[0]):
            print(f"iq_loop_passes_check: /{pattern}/ differs between passes or is under {args.min_count}: {counts}",
                  file=sys.stderr)
            rc = 1
    for k in range(2, len(done)):
        if done[k] != done[1]:
            print(f"iq_loop_passes_check: pass {k + 1} decoded differently from pass 2", file=sys.stderr)
            for a, b in zip(done[1], done[k]):
                if a != b:
                    print(f"  pass 2: {a}\n  pass {k + 1}: {b}", file=sys.stderr)
                    break
            rc = 1
    if rc == 0:
        print(f"iq_loop_passes_check: {args.passes} passes, {len(done[1])} decoded lines per pass after the first")
    return rc


if __name__ == "__main__":
    sys.exit(main())
