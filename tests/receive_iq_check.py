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
    # The test-only scan host (tests/engine/scan_mode_replay.c): -C loads a real channel map and row 0's own options
    # are installed as the conventional scanner installs them, which I/Q replay cannot retune to.
    parser.add_argument("scan_host")
    args = parser.parse_args()
    fixtures = Path(args.fixtures).resolve()
    with tempfile.TemporaryDirectory(prefix="dsd-receive-iq-") as temporary:
        root = Path(temporary)

        def replay(name, fixture, mode, options, denied=False, service=None, unnamed=None, per_call=False, row=None):
            directory = root / name
            directory.mkdir()
            wav = directory / "decoded.wav"
            events = directory / "events.log"
            # -P and -w exclude each other. Analog audio reaches only the per-call recording (-w is digital-only).
            recording_option = ["-P"] if per_call else ["-w", str(wav)]
            binary = args.binary
            if row is not None:
                # A channel-map row carrying the options in its CSV cell, entered by the scan host.
                (directory / "map.csv").write_text("channel,frequency_hz,name,mode,options\n1,851375000,Row,"
                                                   + {"-fr": "dmr", "-fA": "nfm"}[mode] + "," + row + "\n")
                binary = args.scan_host
                options = [*options, "-C", "map.csv"]
            result = subprocess.run(
                [binary, "--frontend", "none", mode, *options,
                 "--iq-replay", str(fixtures / (fixture + ".iq.json")),
                 "-o", "null", *recording_option, "-J", str(events)],
                cwd=directory, capture_output=True, text=True, timeout=60, check=False)
            output = result.stdout + result.stderr
            assert result.returncode == 0, output
            assert row is None or "Scan override applied:" in output, output
            assert not re.search(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:",
                                 output), output
            calls = sorted((directory / "WAV").glob("*.wav")) if per_call else []
            if per_call:
                # Each fixture holds one transmission, so it makes at most one per-call recording.
                assert len(calls) <= 1, f"{name}: {[call.name for call in calls]}"
                recorded = calls[0] if calls else None
            else:
                recorded = wav
            frames = b""
            if recorded is not None:
                with wave.open(str(recorded)) as recording:
                    frames = recording.readframes(recording.getnframes())
            history = events.read_text() if events.exists() else ""
            if denied:
                assert not frames, f"{name}: rejected audio was recorded"
                assert not calls, f"{name}: rejected traffic made a recording"
                assert "TGT:" not in history and "SRC:" not in history and " NFM;" not in history, history
            else:
                assert frames and any(frames), f"{name}: accepted audio is absent"
            if service:
                assert "[" + service + "] " + mode_word(mode) in history, history
                if per_call:
                    # A conventional NFM call has no talkgroup, source or call type, so its service ends the name.
                    label = "_SVC_" + service.replace(" ", "_") + ("_" if mode != "-fA" else ".wav")
                    assert label in calls[0].name, f"{name}: {calls[0].name} lacks {label}"
            if unnamed:
                # Traffic no rule names is still received, under no service label.
                lines = [line for line in history.splitlines() if " " + mode_word(mode) + " " in line]
                assert lines, f"{name}: no call was published\n{history}"
                assert all("[" not in line for line in lines), history
                assert all("_SVC_" not in call.name for call in calls), [call.name for call in calls]
            print(name, "OK")
            return frames

        def mode_word(mode):
            return {"-fr": "DMR", "-fi": "NXDN", "-f1": "P25p1", "-fA": "ANALOG"}[mode]

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
        # The fixture's voice is on TS2. The single-slot decoder runs it on its one lane, so the slot filter must judge
        # the slot the CACH names, not the lane: blocking TS2 mutes it, blocking TS1 does not.
        replay("mono-slot-2-blocked", "dmr_voice", "-fr", ["--slot-block", "2"], denied=True)
        replay("mono-slot-1-blocked", "dmr_voice", "-fr", ["--slot-block", "1"])
        replay("fdma-no-slot", "p25p1_c4fm_vc", "-f1", ["--slot-block", "1/2"])
        replay("nxdn-name", "nxdn48", "-fi", ["--ran-name", "1=Hospital / Dispatch"],
               service="Hospital / Dispatch")
        replay("p25-name", "p25p1_c4fm_vc", "-f1", ["--nac-name", "0x293=Public Safety"],
               service="Public Safety")

        # Issue #637's shared repeater: one row named by the colour code on air (the fixture's CC 2), without filtering.
        # dmr_voice carries no voice LC, so it never publishes a call; the synthetic call does.
        dcc_names = ["--dcc-name", "2=Mall", "--dcc-name", "7=Hospital", "--dcc-name", "14=Municipality"]
        dmr_baseline = replay("dcc-name-default", "dmr_voice_synth", "-fr", [])
        assert replay("dcc-name", "dmr_voice_synth", "-fr", dcc_names, service="Mall") == dmr_baseline
        assert replay("dcc-name-other", "dmr_voice_synth", "-fr", ["--dcc-name", "7=Hospital"],
                      unnamed=True) == dmr_baseline
        replay("dcc-name-call", "dmr_voice_synth", "-fr", dcc_names, service="Mall", per_call=True)

        # Issue #637's noisy NFM channel: a tone name labels the call and its recording without filtering it, and
        # combines with the tone filter (the fixture's CTCSS is 100.0 Hz).
        tone_name = ["--tone-name", "100.0=20R Traveler"]
        nfm_baseline = replay("tone-name-default", "nfm_ctcss_synth_1000", "-fA", [], per_call=True)
        assert replay("tone-name", "nfm_ctcss_synth_1000", "-fA", tone_name, service="20R Traveler",
                      per_call=True) == nfm_baseline
        assert replay("tone-name-other", "nfm_ctcss_synth_1000", "-fA", ["--tone-name", "141.3=Other"],
                      unnamed=True, per_call=True) == nfm_baseline
        tone_allow = ["--tone-allow", "97.4/100.0"]
        allowed = replay("tone-allow", "nfm_ctcss_synth_1000", "-fA", tone_allow, unnamed=True, per_call=True)
        assert replay("tone-allow-name", "nfm_ctcss_synth_1000", "-fA", tone_allow + tone_name,
                      service="20R Traveler", per_call=True) == allowed
        replay("tone-allow-blocked", "nfm_ctcss_synth_1000", "-fA", ["--tone-allow", "97.4/141.3", *tone_name],
               denied=True, per_call=True)

        # The same, from a channel-map row's options cell rather than the command line: the rows the issue asks for.
        # Each baseline is the host's own unnamed row, since a dmr row runs the dual-slot decoder and the NFM row's
        # tone filter withholds the audio heard while its tone is acquired.
        row_baseline = replay("row-dcc-default", "dmr_voice_synth", "-fr", [], per_call=True, row="")
        assert replay("row-dcc-name", "dmr_voice_synth", "-fr", [], service="Mall Two", per_call=True,
                      row="--dcc-name '2=Mall Two' --dcc-name 7=Hospital") == row_baseline
        assert replay("row-dcc-name-other", "dmr_voice_synth", "-fr", [], unnamed=True, per_call=True,
                      row="--dcc-name 7=Hospital") == row_baseline
        replay("row-dcc-allow", "dmr_voice_synth", "-fr", [], denied=True, per_call=True, row="--dcc-allow 4/6/8")
        row_allow = "--tone-allow 97.4/100.0/141.3"
        row_tone_baseline = replay("row-tone-default", "nfm_ctcss_synth_1000", "-fA", [], unnamed=True,
                                   per_call=True, row=row_allow)
        assert replay("row-tone-name", "nfm_ctcss_synth_1000", "-fA", [], service="20R Traveler", per_call=True,
                      row=row_allow + " --tone-name '100.0=20R Traveler'") == row_tone_baseline
        # The reporter's own row: it hears 97.4 and 141.3 Hz only, so this 100.0 Hz call records nothing.
        replay("row-tone-reporter", "nfm_ctcss_synth_1000", "-fA", [], denied=True, per_call=True,
               row="--tone-allow 97.4/141.3 --tone-name '141.3=20R Traveler'")


if __name__ == "__main__":
    main()
