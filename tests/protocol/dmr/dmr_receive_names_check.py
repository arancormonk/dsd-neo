# SPDX-License-Identifier: GPL-3.0-or-later
"""Service names on one shared DMR frequency, end to end (issue #637).

Three repeaters share a frequency and differ only by colour code: Mall (CC 2), Hospital (CC 7) and Municipality
(CC 14). One conventional row names them by colour code and must label every call by the code it was received with,
never by the previous transmission's, and keep each call's audio in its own per-call recording.

The calls come from dsd-neo_test_dmr_synth_gen, each with its own talkgroup, source and voice:
- silence-between: the three calls with two seconds of silence between them (a carrier boundary);
- back-to-back: the three calls joined as generated (each ends with a terminator and idle bursts);
- colour-code-change: one continuous call whose colour code changes twice (--cc-change), with no header, talkgroup or
  source change, so only the received colour code can separate the services;
- slot-change: calls on TS1, TS2 and TS1 again, named by slot. The single-slot decoder (-fr) carries both slots on its
  one lane, so a call on the other slot than the one before must still be one call: one row, logged with its own
  over-the-air slot, and one recording.

Audio oracle: each recording correlates (at least MIN_CORRELATION) with the audio it must hold, and a recording one
burst off correlates near zero. A separate call's recording is as long as that call decoded alone and matches it. The
colour-code-change recordings are pieces, in order, of the same call decoded with no colour-code change, and each
matches the piece of it at its place: the first by its audio, the later ones by their loudness, frame by frame
(ENVELOPE_CORRELATION), since the vocoder synthesizes a frame after a gap differently. They are not compared sample by
sample: the vocoder's synthesis depends on the frames before it, which differ at each new call. The single-slot decoder takes an EMB's colour code only through the
colour-code gate (issue #834): the EMB has no checksum, and noise decodes to a random code. Burst A carries no colour
code (its centre is the sync word), and the gate takes a new code only when four EMBs in a row name it (B to E), so a
piece runs up to four bursts (A to D) past its colour code's last superframe. The relock closes the voice gate until
the new code repeats on the next superframe, so up to four bursts (E, F, A, B) between two pieces play under neither,
and the call itself plays from its first EMB that agrees with an earlier code (at most two bursts held at its start).
"""
import argparse
import math
from pathlib import Path
import re
import subprocess
import tempfile
import struct
import wave

RATE = 8000
SUPERFRAME = 6 * 3 * 160  # six bursts of three 20 ms vocoder frames at 8 kHz
BURST = 3 * 160
MIN_CORRELATION = 0.9
FRAME = 160  # one 20 ms vocoder frame at 8 kHz
ENVELOPE_CORRELATION = 0.95
NAMES = ["--dcc-name", "2=Mall", "--dcc-name", "7=Hospital", "--dcc-name", "14=Municipality"]
# name, colour code, talkgroup, source, generator duration (s), voice offset (s)
CALLS = [
    ("Mall", 2, 100, 1001, 4, 0),
    ("Hospital", 7, 700, 7001, 7, 11),
    ("Municipality", 14, 1400, 14001, 5, 23),
]
# The continuous call: Mall's colour code until superframe 10, Hospital's until 25, then Municipality's.
CHANGES = [(10, 7), (25, 14)]


class Runner:
    def __init__(self, binary, generator, root):
        self.binary, self.generator, self.root = binary, generator, root

    def generate(self, directory, name, options):
        result = subprocess.run([self.generator, "-o", name, *options], cwd=directory, capture_output=True,
                                text=True, timeout=60, check=False)
        assert result.returncode == 0, result.stdout + result.stderr
        match = re.search(r"(\d+) superframes per voice slot", result.stdout + result.stderr)
        self.superframes = int(match.group(1)) if match else None
        with wave.open(str(directory / name)) as source:
            return source.getparams(), source.readframes(source.getnframes())

    def decode(self, case, params, signal, names=NAMES, key=r"_SVC_([A-Za-z]+)_GROUP_", decoder=("-fr",)):
        """Decode @p signal with the naming rules; return the call lines and {key: (path, pcm)} of the recordings,
        keyed by what @p key captures from each recording's name (by default its service)."""
        directory = self.root / case
        directory.mkdir(exist_ok=True)
        with wave.open(str(directory / "input.wav"), "wb") as joined:
            joined.setparams(params)
            joined.writeframes(signal)
        events = directory / "events.log"
        result = subprocess.run(
            [self.binary, "--frontend", "none", *decoder, "-i", "input.wav", "-o", "null", "-P", "-J", str(events),
             *names],
            cwd=directory, capture_output=True, text=True, timeout=120, check=False)
        output = result.stdout + result.stderr
        assert result.returncode == 0, output
        assert not re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:",
                             output), output
        history = events.read_text() if events.exists() else ""
        calls = [line for line in history.splitlines() if " DMR TGT: " in line]
        recordings = {}
        for path in sorted((directory / "WAV").glob("*.wav")):
            match = re.search(r"_DMR_CC_\d+" + key, path.name)
            assert match and match.group(1) not in recordings, f"{case}: {path.name}"
            with wave.open(str(path)) as recording:
                assert recording.getframerate() == RATE, path.name
                recordings[match.group(1)] = (path.name, recording.readframes(recording.getnframes()))
        return calls, recordings, history


def samples(pcm):
    return struct.unpack(f"<{len(pcm) // 2}h", pcm)


def matches(pcm, reference):
    """Whether two equal-length 16-bit recordings hold the same audio (normalised correlation)."""
    a, b = samples(pcm), samples(reference)
    if len(a) != len(b) or not a:
        return False
    energy = math.sqrt(sum(x * x for x in a) * sum(y * y for y in b))
    return energy > 0 and sum(x * y for x, y in zip(a, b)) / energy >= MIN_CORRELATION


def envelope(pcm):
    """The log loudness of each 20 ms frame (its RMS, plus one so silence has a logarithm)."""
    s = samples(pcm)
    return [math.log(1.0 + math.sqrt(sum(x * x for x in s[i:i + FRAME]) / FRAME))
            for i in range(0, len(s) - FRAME + 1, FRAME)]


def envelope_matches(pcm, reference):
    """Whether two equal-length recordings get louder and quieter together, frame by frame, past the first two frames
    (the vocoder's synthesis settling after a gap): the correlation of their envelopes. One frame off falls well below."""
    a, b = envelope(pcm)[2:], envelope(reference)[2:]
    if len(a) != len(b) or len(a) < 2:
        return False
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    da, db = [x - ma for x in a], [y - mb for y in b]
    energy = math.sqrt(sum(x * x for x in da) * sum(y * y for y in db))
    return energy > 0 and sum(x * y for x, y in zip(da, db)) / energy >= ENVELOPE_CORRELATION


def place(pcm, reference, first, last):
    """The offset, a whole number of frames from @p first to @p last, where @p pcm's envelope matches @p reference's, or
    None."""
    count = len(pcm) // 2
    for offset in range(first, last + 1, FRAME):
        if envelope_matches(pcm, reference[2 * offset:2 * (offset + count)]):
            return offset
    return None


def check_labels(case, calls, history, expected):
    """Every published call with colour code @p cc carries exactly @p label, for each (cc, label) in @p expected."""
    assert calls, f"{case}: no call was published\n{history}"
    for cc, label in expected:
        mine = [line for line in calls if f"; CC: {cc:02d};" in line]
        assert mine and all(label in line for line in mine), f"{case}: CC {cc} is not {label!r}\n{history}"
    assert all(any(f"; CC: {cc:02d};" in line for cc, _ in expected) for line in calls), history


def separate_calls(runner):
    """Three calls, alone and joined two ways: each joined recording is its own call's decode."""
    directory = runner.root / "calls"
    directory.mkdir()
    params = None
    generated = []
    references = {}
    for i, (name, cc, tg, src, seconds, offset) in enumerate(CALLS):
        params, signal = runner.generate(directory, f"{name}.wav", [
            "-t", str(seconds), "--cc", str(cc), "--tg", str(tg), "--src", str(src), "--seed", str(i + 1),
            "--voice-t0", str(offset)])
        generated.append(signal)
        _, alone, _ = runner.decode(f"alone-{name}", params, signal)
        assert list(alone) == [name], f"{name} alone: {list(alone)}"
        references[name] = alone[name][1]
    for case, gap_s in (("silence-between", 2.0), ("back-to-back", 0.0)):
        silence = b"\0\0" * int(params.framerate * gap_s)
        calls, recordings, history = runner.decode(case, params, silence.join(generated))
        check_labels(case, calls, history, [(cc, f"[{name}] DMR TGT: {tg:08d}; SRC: {src:08d}; CC: {cc:02d};")
                                            for name, cc, tg, src, _, _ in CALLS])
        assert sorted(recordings) == sorted(references), f"{case}: {[r[0] for r in recordings.values()]}"
        for name, (path, pcm) in recordings.items():
            assert matches(pcm, references[name]), f"{case}: {path} is not {name}'s own audio"


def colour_code_change(runner):
    """One call whose colour code changes twice: three named calls that split its audio between them in order."""
    directory = runner.root / "change"
    directory.mkdir()
    common = ["-t", "15", "--cc", "2", "--seed", "4"]
    changes = [option for sf, cc in CHANGES for option in ("--cc-change", f"{sf}:{cc}")]
    params, changing = runner.generate(directory, "changing.wav", common + changes)
    _, unchanged = runner.generate(directory, "unchanged.wav", common)
    _, whole, _ = runner.decode("unchanged", params, unchanged)
    assert list(whole) == ["Mall"], f"unchanged: {list(whole)}"
    reference = whole["Mall"][1]
    # The bursts the voice gate held at the call's start: the reference's sample 0 is this far into the call.
    lead = runner.superframes * SUPERFRAME - len(reference) // 2
    assert lead in (0, BURST, 2 * BURST), f"the call lost {lead} samples at its start"
    calls, recordings, history = runner.decode("colour-code-change", params, changing)
    check_labels("colour-code-change", calls, history,
                 [(2, "[Mall] DMR TGT: 00000100; SRC: 00001001; CC: 02;"),
                  (7, "[Hospital] DMR TGT: 00000100; SRC: 00001001; CC: 07;"),
                  (14, "[Municipality] DMR TGT: 00000100; SRC: 00001001; CC: 14;")])
    assert sorted(recordings) == ["Hospital", "Mall", "Municipality"], [r[0] for r in recordings.values()]
    start = 0
    ends = [sf * SUPERFRAME - lead for sf, _ in CHANGES] + [len(reference) // 2]
    for name, end in zip(("Mall", "Hospital", "Municipality"), ends):
        path, pcm = recordings[name]
        count = len(pcm) // 2
        offset = place(pcm, reference, start, start + 4 * BURST)
        assert offset is not None, f"{path} is not the call's audio within four bursts of sample {start}"
        assert start != 0 or (offset == 0 and matches(pcm, reference[:2 * count])), f"{path} is not the call's start"
        assert end <= offset + count <= end + 4 * BURST, \
            f"{path} ends at sample {offset + count}, its colour code at {end}"
        start = offset + count
    assert start == len(reference) // 2, f"the pieces end at {start} of {len(reference) // 2} samples"


def slot_change(runner):
    """TS1, TS2 and TS1 calls in a row on the single-slot decoder (-fr, and -nm under the two-slot preset): each is one
    named row on its own slot, and one recording of its own audio. Under a slot filter the TS2 call is Checking until two
    bursts prove its slot: after one voice header the first voice burst is withheld (the voice gate holds it anyway),
    after a repeated header none is."""
    directory = runner.root / "slots"
    directory.mkdir()
    names = ["--slot-name", "1=SlotOne", "--slot-name", "2=SlotTwo"]
    # name, over-the-air slot, generator --tg (its slot 2 carries --tg + 1), seconds, voice offset
    calls = [("SlotOne", 1, 100, 4, 0), ("SlotTwo", 2, 200, 5, 7), ("SlotOne", 1, 300, 4, 14)]
    key = r"_SVC_[A-Za-z]+_GROUP_TGT_(\d+)_"
    params = None
    generated = []
    for i, (_, slot, tg, seconds, offset) in enumerate(calls):
        contents = ["--s1", "voice", "--s2", "idle"] if slot == 1 else ["--s1", "idle", "--s2", "voice"]
        params, signal = runner.generate(directory, f"call{i}.wav", [
            "-t", str(seconds), "--cc", "1", "--tg", str(tg), "--seed", str(i + 11), "--voice-t0", str(offset),
            *contents])
        generated.append(signal)
    _, repeated = runner.generate(directory, "call1h.wav", [
        "-t", "5", "--cc", "1", "--tg", "200", "--seed", "12", "--voice-t0", "7", "--s1", "idle", "--s2", "voice",
        "--hdr-repeat", "2"])
    for decoder in (("-fr",), ("-fs", "-nm")):
        tag = "nm" if "-nm" in decoder else "fr"
        references = {}
        for i, (_, slot, tg, _, _) in enumerate(calls):
            on_air = str(tg + slot - 1)
            _, alone, history = runner.decode(f"slot-alone-{tag}-{i}", params, generated[i], names, key, decoder)
            assert list(alone) == [on_air], f"{tag} call {i} alone: {list(alone)}\n{history}"
            references[on_air] = alone[on_air][1]
        for case, gap_s in (("slot-back-to-back", 0.0), ("slot-silence-between", 2.0)):
            case = f"{case}-{tag}"
            silence = b"\0\0" * int(params.framerate * gap_s)
            lines, recordings, history = runner.decode(case, params, silence.join(generated), names, key, decoder)
            assert len(lines) == len(calls), f"{case}: {len(lines)} rows for {len(calls)} calls\n{history}"
            for line, (name, slot, tg, _, _) in zip(lines, calls):
                on_air = tg + slot - 1
                assert f"[{name}] DMR TGT: {on_air:08d};" in line and line.rstrip().endswith(f"Slot {slot};"), \
                    f"{case}: {line!r} is not {name}'s TG {on_air} on slot {slot}\n{history}"
            assert sorted(recordings) == sorted(references), f"{case}: {[r[0] for r in recordings.values()]}"
            for tg, (path, pcm) in recordings.items():
                assert matches(pcm, references[tg]), f"{case}: {path} is not TG {tg}'s own audio"
        # Only TS2 passes the filter: one row, and its audio whole. The Checking burst a single voice header leaves
        # unproven at its start (the bootstrap burst) is burst A, which the single-slot decoder's voice gate holds in the
        # call decoded alone too (A has no EMB to corroborate the colour code; issue #834); a repeated header proves the
        # slot before it.
        for case, second, withheld in (("slot-allow-2", generated[1], 0), ("slot-allow-2-repeat", repeated, 0)):
            case = f"{case}-{tag}"
            _, alone, _ = runner.decode(case + "-alone", params, second, names, key, decoder)
            reference = alone["201"][1]
            joined = b"".join([generated[0], second, generated[2]])
            lines, recordings, history = runner.decode(case, params, joined, names + ["--slot-allow", "2"], key, decoder)
            assert len(lines) == 1 and "[SlotTwo] DMR TGT: 00000201;" in lines[0], f"{case}: {lines}\n{history}"
            assert list(recordings) == ["201"], f"{case}: {[r[0] for r in recordings.values()]}"
            path, pcm = recordings["201"]
            assert len(pcm) // 2 == len(reference) // 2 - withheld, \
                f"{case}: {path} holds {len(pcm) // 2} samples, expected {len(reference) // 2 - withheld}"
            assert matches(pcm, reference[len(reference) - len(pcm):]), f"{case}: {path} is not the call's own audio"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("generator")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dsd-dmr-receive-names-") as temporary:
        runner = Runner(str(Path(args.binary).resolve()), str(Path(args.generator).resolve()), Path(temporary))
        separate_calls(runner)
        colour_code_change(runner)
        slot_change(runner)


if __name__ == "__main__":
    main()
