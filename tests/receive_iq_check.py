# SPDX-License-Identifier: GPL-3.0-or-later
"""Receive admission through actual I/Q, decoding, audio and event publication."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import wave


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("fixtures")
    args = parser.parse_args()
    fixtures = Path(args.fixtures).resolve()
    with tempfile.TemporaryDirectory(prefix="dsd-receive-iq-") as temporary:
        root = Path(temporary)

        def replay(name, fixture, mode, options, denied=False, service=None):
            directory = root / name
            directory.mkdir()
            wav = directory / "decoded.wav"
            events = directory / "events.log"
            result = subprocess.run(
                [args.binary, "--frontend", "none", mode, *options,
                 "--iq-replay", str(fixtures / (fixture + ".iq.json")),
                 "-o", "null", "-w", str(wav), "-J", str(events)],
                cwd=directory, capture_output=True, text=True, timeout=60, check=False)
            output = result.stdout + result.stderr
            assert result.returncode == 0, output
            assert not re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:", output), output
            with wave.open(str(wav)) as recording:
                frames = recording.readframes(recording.getnframes())
            history = events.read_text() if events.exists() else ""
            if denied:
                assert not frames, f"{name}: rejected audio was recorded"
                assert "TGT:" not in history and "SRC:" not in history, history
            else:
                assert frames and any(frames), f"{name}: accepted audio is absent"
            if service:
                assert "[" + service + "]" in history, history
            print(name, "OK")
            return frames

        for fixture, mode, option, value in [
            ("dmr_voice", "-fr", "dcc", "2"),
            ("nxdn48", "-fi", "ran", "1"),
            ("p25p1_c4fm_vc", "-f1", "nac", "293"),
        ]:
            baseline = replay(option + "-default", fixture, mode, [])
            allowed = replay(option + "-allowed", fixture, mode, ["--" + option + "-allow", value])
            # Checking deliberately withholds early media until the access code is proven.
            assert len(allowed) <= len(baseline)
            disabled = replay(option + "-disabled", fixture, mode, ["--no-" + option + "-filter"])
            assert baseline == disabled, option + ": the disabled policy changes decoded PCM"
            replay(option + "-blocked", fixture, mode,
                   ["--" + option + "-allow", value, "--" + option + "-block", value], denied=True)
        replay("slot-block", "dmr_voice", "-fr", ["--slot-block", "1/2"], denied=True)
        replay("fdma-no-slot", "p25p1_c4fm_vc", "-f1", ["--slot-block", "1/2"])
        replay("nxdn-name", "nxdn48", "-fi", ["--ran-name", "1=Hospital / Dispatch"],
               service="Hospital / Dispatch")
        replay("p25-name", "p25p1_c4fm_vc", "-f1", ["--nac-name", "0x293=Public Safety"],
               service="Public Safety")


if __name__ == "__main__":
    main()
