# SPDX-License-Identifier: GPL-3.0-or-later
"""X2-TDMA direct-mode voice through the real frame sync (X2TDMA_DIRECT_MODE).

A direct-mode transmission carries its sync word on burst A of each voice superframe and nothing in the other
timeslot, so no later sync word corroborates a superframe: the next superframe's sync following it with no hunt gap
(exactly 24 symbols after the decoder finished reading it) is what does. Its voice plays only once a superframe of the
transmission was corroborated that way, and from then on through the call, a hunt gap included (x2tdma_voice.c).

The script writes float symbol files (one native-order float per symbol at the nominal levels) holding superframes of
random voice payload between random symbols, decodes them with `dsd-neo -fx -i <file>.sym -Z`, and counts the AMBE
frames the vocoder decoded:
- one lone superframe: no frame;
- eight superframes back to back: all but the first, 7 x 18 frames, which needs the frame sync to land the chain gap;
- the same with a 48-symbol hunt gap after the fourth: still 7 x 18, the transmission being corroborated already.
"""
import random
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# The sync word the decoder reads as an X2-TDMA voice burst in this file's polarity (the frame sync names it -X2-TDMA).
SYNC = "131331111333333311111131"
LEVELS = (1.0, 3.0, -1.0, -3.0)
FRAMES_PER_SUPERFRAME = 18


class Stream:
    def __init__(self, seed):
        self.rng = random.Random(seed)
        self.symbols = []

    def noise(self, count):
        self.symbols += [self.rng.choice(LEVELS) for _ in range(count)]

    def burst(self, sync):
        # CACH (naming timeslot 0), payload, sync word or embedded signalling, payload: 144 symbols.
        self.symbols += [LEVELS[0]] * 12
        self.noise(54)
        if sync:
            self.symbols += [-3.0 if c == "1" else 3.0 for c in SYNC]
        else:
            self.noise(24)
        self.noise(54)

    def superframe(self):
        for k in range(6):
            self.burst(k == 0)
            self.noise(144)  # the silent other timeslot

    def write(self, path):
        path.write_bytes(struct.pack(f"={len(self.symbols)}f", *self.symbols))


def decode(binary, directory, name, stream):
    path = directory / f"{name}.sym"
    stream.write(path)
    result = subprocess.run([binary, "--frontend", "none", "-fx", "-i", str(path), "-o", "null", "-Z"],
                            capture_output=True, text=True, errors="replace", timeout=120, check=False)
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert not re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", output), output
    syncs = len(re.findall(r"Sync: [+-]X2-TDMA .*VOICE", output))
    frames = len(re.findall(r"^ *AMBE [0-9A-F]+ err", output, re.MULTILINE))
    return syncs, frames


def case(binary, directory, name, superframes, gap_after=None, gap=0):
    stream = Stream(7)
    stream.noise(3000)
    for i in range(superframes):
        if gap_after is not None and i == gap_after:
            stream.noise(gap)
        stream.superframe()
    stream.noise(3000)
    return decode(binary, directory, name, stream)


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="dsd-x2tdma-direct-") as temporary:
        directory = Path(temporary)
        syncs, frames = case(binary, directory, "lone", 1)
        assert syncs == 1 and frames == 0, f"lone superframe: {syncs} syncs, {frames} frames"
        syncs, frames = case(binary, directory, "chained", 8)
        assert syncs == 8 and frames == 7 * FRAMES_PER_SUPERFRAME, f"chained: {syncs} syncs, {frames} frames"
        syncs, frames = case(binary, directory, "gap", 8, gap_after=4, gap=48)
        assert syncs == 8 and frames == 7 * FRAMES_PER_SUPERFRAME, f"hunt gap: {syncs} syncs, {frames} frames"
    print("X2TDMA_DIRECT_MODE: OK")


if __name__ == "__main__":
    main()
