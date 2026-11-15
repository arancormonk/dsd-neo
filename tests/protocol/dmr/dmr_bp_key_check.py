# SPDX-License-Identifier: GPL-3.0-or-later
"""DMR Basic Privacy keys end to end: a call is keyed from its own talkgroup's key, or from a key set directly.

The calls come from dsd-neo_test_dmr_synth_gen. The talkgroup key autoload loads the key a keys file (-k, talkgroup to
key) holds for a call's talkgroup into the one working Basic Privacy key both slots share. A call whose talkgroup has
no key must not keep the key another call's talkgroup loaded: that key would decrypt it wrongly, or, where both use
it, decrypt a call the user holds no key for, and judge it decryptable.

- early frames: an RC4 call A on talkgroup 123 ends, then 1.5 s of silence, then a Basic Privacy call B (key 70,
  talkgroup 456, no voice LC header) whose first superframe is on a colour code `--dcc-allow 2` refuses: those frames
  are not admitted and come before B's LC, while the slot still holds A. With B's own key B decrypts and plays; with
  no key it is muted; with a key for A's talkgroup only it is muted too (its refused frames once loaded A's key, which
  stayed loaded into B's admitted frames).
- two slots: Basic Privacy key 33 on both slots, talkgroups 123 and 124, a key for 123 only: slot 1 decrypts, slot 2
  stays muted (no recording), however the frames of the two interleave.
- back to back: two Basic Privacy calls on slot 1, talkgroup 123 then 456, both key 33, a key for 123 only, joined
  on the burst grid with no carrier drop between them: the second stays muted.
- a key set directly (-b 33), with no keys file, decrypts every call of both signals, as before. (With a keys file
  as well, the no-carrier pass unloads the working key, the -b key with it, as before.)
"""
import argparse
import json
import re
import subprocess
import tempfile
import wave
from pathlib import Path

CALL_A = ("-t", "4", "--cc", "2", "--alg", "rc4", "--tg", "123", "--src", "1230", "--voice-t0", "9")
CALL_B = ("-t", "6", "--cc", "3", "--cc-change", "1:2", "--alg", "bp", "--bp-key", "70", "--tg", "456", "--src",
          "4001", "--no-hdr", "--lead", "0")
TWO_SLOTS = ("-t", "6", "--cc", "2", "--alg", "bp", "--bp-key", "33", "--tg", "123", "--src", "1001", "--s2", "voice")
FIRST = ("-t", "5", "--cc", "2", "--alg", "bp", "--bp-key", "33", "--tg", "123", "--src", "1001")
SECOND = ("-t", "5", "--cc", "2", "--alg", "bp", "--bp-key", "33", "--tg", "456", "--src", "4001", "--lead", "0",
          "--voice-t0", "11")
GAP_S = 1.5
# The generator's burst grid at 48 kHz: the first burst period starts at sample 9600 and each lasts 1440 samples
# (tests/protocol/dmr/dmr_synth.c), slot 1 in the even periods.
GRID_START = 9600
PERIOD = 1440
# A decrypted call carries its plaintext in nearly all of its frames (a few at the call's start, or at a colour-code
# change, are not decoded); a call of a two-call signal is counted only by the frames the other call does not carry.
MIN_PLAINTEXT_FRAMES = 60


def generate(generator, directory, name, args):
    subprocess.run([generator, "-o", f"{name}.wav", "-e", f"{name}.txt", *args], cwd=directory, check=True,
                   capture_output=True, timeout=60)
    with wave.open(str(directory / f"{name}.wav"), "rb") as w:
        rate, frames = w.getframerate(), w.readframes(w.getnframes())
    expected = {}
    for line in (directory / f"{name}.txt").read_text().splitlines():
        if line and not line.startswith("#"):
            fields = line.split()
            expected.setdefault(fields[0], set()).add(fields[4])
    return rate, frames, expected


def write_signal(path, rate, *parts):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(parts))


def join_on_grid(first, second):
    """@p first up to a slot-1 period boundary inside its idle tail, then @p second from its first burst: one carrier."""
    periods = (len(first) // 2 - 2 * GRID_START) // PERIOD // 2 * 2
    return first[: 2 * (GRID_START + periods * PERIOD)] + second[2 * GRID_START:]


def decode(binary, directory, case, signal, extra):
    """Decodes @p signal; returns the decoded frames as (slot, data) pairs and the talkgroups of the calls recorded (a
    call is recorded only where it plays)."""
    work = directory / case
    (work / "calls").mkdir(parents=True)
    args = [binary, "--frontend", "none", "-o", "null", "-fs", "-i", str(directory / signal), "--frame-log",
            "frames.log", "-P", "-7", "calls", "--wav-journal", "calls.jsonl", "-Z", *extra]
    result = subprocess.run(args, cwd=work, capture_output=True, text=True, errors="replace", timeout=120,
                            check=False)
    output = result.stdout + result.stderr
    assert result.returncode == 0, f"{case}: decoder exited {result.returncode}"
    assert not re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:", output), case
    frames = re.findall(r"FRAME AMBE slot=(\d) data=([0-9A-F]{14})", (work / "frames.log").read_text())
    journal = work / "calls.jsonl"
    records = [json.loads(line) for line in journal.read_text().splitlines()] if journal.exists() else []
    recorded = sorted({r["target"] for r in records if r.get("type") == "publish"})
    return frames, recorded


def keys_file(directory, case, keys):
    path = directory / f"{case}-keys.csv"
    path.write_text("tg,key\n" + "".join(f"{tg},{key}\n" for tg, key in keys))
    return ["-k", str(path)]


def plaintext(frames, expected, slot):
    return sum(1 for s, data in frames if s == slot and data in expected)


def early_frames(binary, generator, root):
    rate, call_a, _ = generate(generator, root, "a", CALL_A)
    _, call_b, expected = generate(generator, root, "b", CALL_B)
    write_signal(root / "early.wav", rate, call_a, b"\0\0" * int(GAP_S * rate), call_b)
    b = expected["1"]
    failures = []
    frames, recorded = decode(binary, root, "early-right-key", "early.wav",
                              ["--dcc-allow", "2", *keys_file(root, "early-right", [(456, 70)])])
    if plaintext(frames, b, "1") < MIN_PLAINTEXT_FRAMES or "456" not in recorded:
        failures.append(f"early frames, right key: {plaintext(frames, b, '1')} plaintext frames, recordings {recorded}")
    for case, extra in (("early-no-key", []), ("early-stale-key", keys_file(root, "early-stale", [(123, 33)]))):
        frames, recorded = decode(binary, root, case, "early.wav", ["--dcc-allow", "2", *extra])
        if "456" in recorded or plaintext(frames, b, "1") != 0:
            failures.append(f"{case}: B plays (recordings {recorded}), {plaintext(frames, b, '1')} plaintext frames")
    return failures


def shared_key(binary, generator, root):
    rate, two, two_expected = generate(generator, root, "two", TWO_SLOTS)
    write_signal(root / "two-slots.wav", rate, two)
    _, first, first_expected = generate(generator, root, "first", FIRST)
    _, second, second_expected = generate(generator, root, "second", SECOND)
    write_signal(root / "back-to-back.wav", rate, join_on_grid(first, second))
    failures = []

    def check(case, frames, recorded, keyed, unkeyed):
        for name, frame_slot, expected, tg in keyed:
            if plaintext(frames, expected, frame_slot) < MIN_PLAINTEXT_FRAMES or tg not in recorded:
                failures.append(f"{case}: {name} does not decrypt ({plaintext(frames, expected, frame_slot)} "
                                f"plaintext frames, recordings {recorded})")
        for name, frame_slot, expected, tg in unkeyed:
            if plaintext(frames, expected, frame_slot) != 0 or tg in recorded:
                failures.append(f"{case}: {name}, which has no key, decrypts ({plaintext(frames, expected, frame_slot)} "
                                f"plaintext frames, recordings {recorded})")

    # A frame both calls of a signal carry (the voice source repeats some) names neither: only each call's own count.
    slot1 = ("slot 1 (talkgroup 123)", "1", two_expected["1"] - two_expected["2"], "123")
    slot2 = ("slot 2 (talkgroup 124)", "2", two_expected["2"] - two_expected["1"], "124")
    call1 = ("the first call (talkgroup 123)", "1", first_expected["1"] - second_expected["1"], "123")
    call2 = ("the next call (talkgroup 456)", "1", second_expected["1"] - first_expected["1"], "456")
    table = keys_file(root, "table", [(123, 33)])
    frames, recorded = decode(binary, root, "two-slots", "two-slots.wav", table)
    check("two slots", frames, recorded, [slot1], [slot2])
    frames, recorded = decode(binary, root, "back-to-back", "back-to-back.wav", table)
    check("back to back", frames, recorded, [call1], [call2])
    frames, recorded = decode(binary, root, "direct-key-two", "two-slots.wav", ["-b", "33"])
    check("direct key, two slots", frames, recorded, [slot1, slot2], [])
    frames, recorded = decode(binary, root, "direct-key-b2b", "back-to-back.wav", ["-b", "33"])
    check("direct key, back to back", frames, recorded, [call1, call2], [])
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("generator")
    args = parser.parse_args()
    binary, generator = str(Path(args.binary).resolve()), str(Path(args.generator).resolve())
    with tempfile.TemporaryDirectory(prefix="dsd-dmr-bp-key-") as temporary:
        root = Path(temporary)
        failures = early_frames(binary, generator, root) + shared_key(binary, generator, root)
        assert not failures, "\n".join(failures)


if __name__ == "__main__":
    main()
