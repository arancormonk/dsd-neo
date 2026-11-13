# SPDX-License-Identifier: GPL-3.0-or-later
"""DMR MS calls that end without a terminator, end to end (issue #834).

An MS (direct mode) call on colour code 2 from dsd-neo_test_dmr_synth_gen stops inside a voice superframe, as when the
carrier drops, and noise follows. The decoder reads the rest of that superframe from the noise. The EMB in each of those
bursts has only QR(16,7,6) and no checksum (ETSI TS 102 361-1 Table B.1), so about one noise word in four decodes, to
any colour code. Taking such a code as the carrier's ended the call and published the noise as another recording,
named DMR_CC_<random>__TGT_0_SRC_0.

Each case cuts the call at another point of a superframe, adds noise and decodes with per-call recordings and the
recording journal. The only recording published must be the call's, under its own colour code, talkgroup and source.
"""
import argparse
import json
from pathlib import Path
import random
import re
import struct
import subprocess
import tempfile
import wave

RATE = 48000
NOISE_S = 3.0
NOISE_RMS = 6000.0  # discriminator noise, as a receiver gives once the carrier is gone
# Cut points (seconds into the generated signal): the call's last superframes, at every burst of one.
CUTS = [3.00 + 0.06 * k for k in range(8)]
SEEDS = [1, 2]
CALL = {"system": "DMR_CC_2", "target": "100", "source": "1001"}


def generate(generator, directory):
    result = subprocess.run(
        [generator, "-o", "call.wav", "-t", "4", "--ms", "--no-term", "--cc", "2", "--lead", "0.3", "--tail", "0"],
        cwd=directory, capture_output=True, text=True, timeout=60, check=False)
    assert result.returncode == 0, result.stdout + result.stderr
    with wave.open(str(directory / "call.wav")) as source:
        assert source.getframerate() == RATE and source.getnchannels() == 1 and source.getsampwidth() == 2
        return source.getparams(), source.readframes(source.getnframes())


def noise(seed):
    rng = random.Random(seed)
    count = int(RATE * NOISE_S)
    return struct.pack(f"<{count}h", *(max(-32767, min(32767, round(rng.gauss(0.0, NOISE_RMS)))) for _ in range(count)))


def decode(binary, directory, params, signal):
    directory.mkdir()
    with wave.open(str(directory / "input.wav"), "wb") as joined:
        joined.setparams(params)
        joined.writeframes(signal)
    (directory / "calls").mkdir()
    result = subprocess.run(
        [binary, "--frontend", "none", "-i", "input.wav", "-o", "null", "-P", "-7", "calls", "--wav-journal",
         "calls.jsonl"],
        cwd=directory, capture_output=True, text=True, timeout=120, check=False)
    output = result.stdout + result.stderr
    assert result.returncode == 0, output
    assert not re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:", output), output
    journal = directory / "calls.jsonl"
    records = [json.loads(line) for line in journal.read_text().splitlines()] if journal.exists() else []
    return [r for r in records if r.get("type") == "publish"], sorted(p.name for p in (directory / "calls").iterdir())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("generator")
    args = parser.parse_args()
    binary, generator = str(Path(args.binary).resolve()), str(Path(args.generator).resolve())
    with tempfile.TemporaryDirectory(prefix="dsd-dmr-ms-tail-") as temporary:
        root = Path(temporary)
        params, call = generate(generator, root)
        failures = []
        for seed in SEEDS:
            tail = noise(seed)
            for cut in CUTS:
                case = f"cut-{cut:.2f}-seed-{seed}"
                kept = call[: 2 * int(cut * RATE)]
                published, files = decode(binary, root / case, params, kept + tail)
                names = [(r["system"], r["target"], r["source"], r["duration_ms"]) for r in published]
                if len(published) != 1 or any({k: r[k] for k in CALL} != CALL for r in published):
                    failures.append(f"{case}: published {names}, files {files}")
                elif published[0]["duration_ms"] < 2000:
                    failures.append(f"{case}: the call's recording holds {published[0]['duration_ms']} ms")
        assert not failures, "\n".join(failures)


if __name__ == "__main__":
    main()
