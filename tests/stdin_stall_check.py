# SPDX-License-Identifier: GPL-3.0-or-later
"""A stdin source that stops sending ends its analog call on time (issue #667 A2, D12).

osmocom rtl_fm -l writes nothing while it is squelched, so `rtl_fm ... | dsd-neo -fA -i -` sees its stdin stall with
the pipe still open. The decoder reads stdin in bounded waits, so the stalled call ends on the stale deadline and its
history row is written during the stall, stamped with the call's own end, not with the time data came back.
"""
import argparse
import math
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

RATE = 48000
CHUNK = RATE // 50  # 20 ms
ANALOG_ROW = re.compile(r"^\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d) .*ANALOG;.*CTCSS 100\.0 Hz;", re.M)


def chunks(seconds):
    """Speech-band audio over a 100.0 Hz CTCSS tone, in 20 ms chunks of raw PCM16LE."""
    for start in range(0, int(seconds * RATE), CHUNK):
        yield b"".join(
            struct.pack("<h", int(6000 * math.sin(2 * math.pi * 1000 * i / RATE)
                                  + 2500 * math.sin(2 * math.pi * 100 * i / RATE)))
            for i in range(start, start + CHUNK))


def play(pipe, seconds):
    """Write the audio at its own pace, as a live source does."""
    t0 = time.monotonic()
    for n, block in enumerate(chunks(seconds)):
        pipe.write(block)
        pipe.flush()
        ahead = t0 + (n + 1) * CHUNK / RATE - time.monotonic()
        if ahead > 0:
            time.sleep(ahead)


def seconds_of_day(match):
    return int(match.group(1)) * 3600 + int(match.group(2)) * 60 + int(match.group(3))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dsd-stdin-stall-") as temporary:
        events = Path(temporary) / "events.log"
        log = (Path(temporary) / "decoder.log").open("wb")
        # The decoder's output goes to a file: a pipe nobody drains during the stall could block it.
        decoder = subprocess.Popen(
            [str(Path(args.binary).resolve()), "--frontend", "none", "-fA", "-i", "-", "-o", "null",
             "-J", str(events)],
            cwd=temporary, stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT)
        try:
            started = time.time()
            play(decoder.stdin, 3.0)
            stalled = time.time()
            # The pipe stays open and quiet. The call's stale deadline is half a second after its last block.
            time.sleep(2.5)
            during = events.read_text() if events.exists() else ""
            rows = list(ANALOG_ROW.finditer(during))
            assert len(rows) == 1, f"no analog row written during the stall:\n{during}"
            stamp = seconds_of_day(rows[0])
            end = time.localtime(stalled)
            start = time.localtime(started)
            end_s = end.tm_hour * 3600 + end.tm_min * 60 + end.tm_sec
            start_s = start.tm_hour * 3600 + start.tm_min * 60 + start.tm_sec
            # Stamped with the call's end (the stall's start, to the second, allowing for the decoder's lag), not its
            # start three seconds before, nor the time the row was written.
            assert end_s - 1 <= stamp <= end_s + 1, f"row stamped {stamp}, call ended {end_s}:\n{during}"
            assert stamp >= start_s + 2, f"row stamped {stamp}, call started {start_s}:\n{during}"
            play(decoder.stdin, 1.0)
            decoder.stdin.close()
            decoder.wait(timeout=60)
        finally:
            if decoder.poll() is None:
                decoder.kill()
                decoder.wait()
            log.close()
        output = (Path(temporary) / "decoder.log").read_text(errors="replace")
        assert decoder.returncode == 0, output
        assert not re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:",
                             output), output
        history = events.read_text()
        assert len(ANALOG_ROW.findall(history)) == 2, f"the resumed audio is a call of its own:\n{history}"
    print("STDIN STALL OK")


if __name__ == "__main__":
    main()
