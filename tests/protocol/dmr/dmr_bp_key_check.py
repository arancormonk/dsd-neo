# SPDX-License-Identifier: GPL-3.0-or-later
"""DMR Basic Privacy keys end to end: which talkgroup's key a call's early frames load.

The signal, from dsd-neo_test_dmr_synth_gen, with the receive policy admitting colour code 2 only (--dcc-allow 2):
- call A, RC4 on talkgroup 123, colour code 2: it ends, and the slot keeps it as its ended call;
- 1.5 s of silence;
- call B, Basic Privacy key 70 on talkgroup 456, with no voice LC header (late entry). Its first superframe is on
  colour code 3, which the policy refuses, so those frames are not admitted and open no call; its embedded LC names
  it at the end of that superframe, and from the next superframe on it is on colour code 2 and admitted.

Cases, by the keys file (-k, talkgroup to Basic Privacy key):
- right key (456 -> 70): B decrypts; its frames carry the plaintext the generator recorded and its audio plays.
- no key: B is muted.
- stale key (123 -> 33, call A's talkgroup): B has no key of its own, so it is muted. The refused frames before B's LC
  once loaded the key of the call the slot still held (A's talkgroup), which stayed loaded into B's admitted frames:
  B was judged decryptable and played, decrypted with A's key.
"""
import argparse
import re
import struct
import subprocess
import tempfile
import wave
from pathlib import Path

CALL_A = ("-t", "4", "--cc", "2", "--alg", "rc4", "--tg", "123", "--src", "1230", "--voice-t0", "9")
CALL_B = ("-t", "6", "--cc", "3", "--cc-change", "1:2", "--alg", "bp", "--bp-key", "70", "--tg", "456", "--src",
          "4001", "--no-hdr", "--lead", "0")
GAP_S = 1.5
# B's admitted superframes (1..14) hold 252 frames; a few at the colour-code change and the LC are not decoded.
MIN_PLAINTEXT_FRAMES = 200


def generate(generator, directory, name, args):
    subprocess.run([generator, "-o", f"{name}.wav", "-e", f"{name}.txt", *args], cwd=directory, check=True,
                   capture_output=True, timeout=60)
    with wave.open(str(directory / f"{name}.wav"), "rb") as w:
        rate, frames = w.getframerate(), w.readframes(w.getnframes())
    expected = [line.split()[4] for line in (directory / f"{name}.txt").read_text().splitlines()
                if line and not line.startswith("#")]
    return rate, frames, expected


def decode(binary, directory, case, keys):
    work = directory / case
    work.mkdir()
    args = [binary, "--frontend", "none", "-o", "null", "-fs", "-i", str(directory / "signal.wav"), "--frame-log",
            "frames.log", "-w", "audio.wav", "--dcc-allow", "2", "-Z"]
    if keys:
        (work / "keys.csv").write_text("tg,key\n" + "".join(f"{tg},{key}\n" for tg, key in keys))
        args += ["-k", "keys.csv"]
    result = subprocess.run(args, cwd=work, capture_output=True, text=True, errors="replace", timeout=120,
                            check=False)
    output = result.stdout + result.stderr
    assert result.returncode == 0, f"{case}: decoder exited {result.returncode}"
    assert not re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:", output), case
    frames = re.findall(r"FRAME AMBE slot=\d data=([0-9A-F]{14})", (work / "frames.log").read_text())
    with wave.open(str(work / "audio.wav"), "rb") as w:
        pcm = w.readframes(w.getnframes())
    peak = max((abs(s) for (s,) in struct.iter_unpack("<h", pcm)), default=0)
    return frames, peak


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("generator")
    args = parser.parse_args()
    binary, generator = str(Path(args.binary).resolve()), str(Path(args.generator).resolve())
    with tempfile.TemporaryDirectory(prefix="dsd-dmr-bp-key-") as temporary:
        root = Path(temporary)
        rate, call_a, _ = generate(generator, root, "a", CALL_A)
        _, call_b, expected = generate(generator, root, "b", CALL_B)
        with wave.open(str(root / "signal.wav"), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(call_a + b"\0\0" * int(GAP_S * rate) + call_b)
        plaintext = set(expected)
        failures = []

        frames, peak = decode(binary, root, "right-key", [(456, 70)])
        matched = sum(1 for f in frames if f in plaintext)
        if matched < MIN_PLAINTEXT_FRAMES or peak == 0:
            failures.append(f"right key: {matched} frames carry the plaintext (want {MIN_PLAINTEXT_FRAMES}), "
                            f"audio peak {peak}")
        for case, keys in (("no-key", []), ("stale-key", [(123, 33)])):
            frames, peak = decode(binary, root, case, keys)
            matched = sum(1 for f in frames if f in plaintext)
            if peak != 0 or matched != 0:
                failures.append(f"{case}: B plays (audio peak {peak}), {matched} frames carry the plaintext")
        assert not failures, "\n".join(failures)


if __name__ == "__main__":
    main()
