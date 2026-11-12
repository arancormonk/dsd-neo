# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay a generated I/Q fixture and compare the decoder's symbol decisions with the dibits the generator sent.

The decoder writes every symbol it decides to a -c capture (DSDNSYM2: a 16-byte header, then 10-byte records whose
first byte holds the dibit). The capture is aligned to <fixture>.dibits by the lag with the most agreement, and each
frame the transmission sent after the first --skip-frames (acquisition) and before the last (whose final symbols the
end of the clip cuts out of the filters) is checked outside its sync word, which the hunt records by sign only: a frame
is clean when every one of those symbols matches. The check passes when at
least --min-clean of those frames are clean, and prints the count either way."""

import argparse
import os
import subprocess
import sys
import tempfile

MAGIC = b"DSDNSYM2"
HEADER_SIZE = 16
RECORD_SIZE = 10
ALIGN_SYMBOLS = 2000
MAX_LAG = 400


def read_capture(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if data[: len(MAGIC)] != MAGIC:
        raise SystemExit(f"{path}: not a DSDNSYM2 symbol capture")
    body = data[HEADER_SIZE:]
    return [body[i] & 3 for i in range(0, len(body) - RECORD_SIZE + 1, RECORD_SIZE)]


def best_lag(got, sent):
    """The offset of the sent stream's first symbol in the capture."""
    best = None
    for lag in range(MAX_LAG + 1):
        window = got[lag : lag + ALIGN_SYMBOLS]
        if len(window) < ALIGN_SYMBOLS:
            break
        agree = sum(1 for a, b in zip(window, sent) if a == b)
        if best is None or agree > best[0]:
            best = (agree, lag)
    if best is None:
        raise SystemExit("the capture is shorter than the alignment window")
    return best[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary")
    parser.add_argument("fixture", help="the fixture's .iq.json; its dibits are read from <name>.dibits beside it")
    parser.add_argument("--mode", required=True, help="the decoder's frame type flags, space-separated")
    parser.add_argument("--frame", type=int, required=True, help="symbols per frame")
    parser.add_argument("--sync", type=int, required=True, help="sync symbols at the start of each frame")
    parser.add_argument("--skip-frames", type=int, default=2, help="frames left to acquisition")
    parser.add_argument("--min-clean", type=float, required=True, help="fraction of checked frames that must be clean")
    args = parser.parse_args()

    dibits_path = args.fixture[: -len(".iq.json")] + ".dibits"
    with open(dibits_path, encoding="ascii") as handle:
        sent = [int(c) for c in handle.read().strip()]
    with tempfile.TemporaryDirectory(prefix="dsd-symbol-check-") as work:
        capture = os.path.join(work, "symbols.bin")
        cmd = [args.binary, "--frontend", "none", *args.mode.split(), "--iq-replay", args.fixture, "-o", "null"]
        run = subprocess.run(cmd + ["-c", capture], capture_output=True, text=True, timeout=120, check=False)
        if run.returncode != 0:
            print(run.stderr[-4000:])
            raise SystemExit(f"decoder exited with {run.returncode}")
        if "runtime error:" in run.stderr or "Sanitizer" in run.stderr:
            print(run.stderr[-4000:])
            raise SystemExit("sanitizer report")
        got = read_capture(capture)

    lag = best_lag(got, sent)
    frames = len(sent) // args.frame
    checked = 0
    clean = 0
    dirty = []
    for frame in range(args.skip_frames, frames - 1):
        start = frame * args.frame
        body = range(start + args.sync, start + args.frame)
        if lag + body[-1] >= len(got):
            break
        checked += 1
        errors = sum(1 for k in body if got[lag + k] != sent[k])
        if errors == 0:
            clean += 1
        else:
            dirty.append(f"{frame}:{errors}")
    print(f"aligned at symbol {lag}; {clean} of {checked} frames clean outside the sync word")
    if dirty:
        print("frames with symbol errors (frame:errors): " + " ".join(dirty))
    if checked == 0 or clean < args.min_clean * checked:
        print(f"FAILED: need at least {args.min_clean:.0%} of the checked frames clean")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
