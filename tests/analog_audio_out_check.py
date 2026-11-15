# SPDX-License-Identifier: GPL-3.0-or-later
"""The analog replay host's --analog-audio-out file, read back.

tools/scan_quiet_reception.py scores its voice rows from this file, so its level matters, not only its length. One
replay writes it; the file must hold one native float32 per delivered sample (the count the "ANALOG AUDIO OUT:" line
prints), full scale 1.0, and its RMS must be the "ANALOG METRIC:" line's rms_dbfs within RMS_TOLERANCE_DB. A scale,
format or stale-slice error in the writer fails here, where the printed sample count alone would pass.
"""
import argparse
from array import array
import math
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

# The metric line prints rms_dbfs to 0.01 dB (at most 0.005 dB of rounding); float32 storage adds far less.
RMS_TOLERANCE_DB = 0.01
SANITIZER = re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|LeakSanitizer|runtime error:")
METRIC = re.compile(r"^ANALOG METRIC: .* rms_dbfs=(-?[0-9]+\.[0-9]+) ", re.MULTILINE)
AUDIO_OUT = re.compile(r"^ANALOG AUDIO OUT: rate_hz=([0-9]+) samples=([0-9]+)$", re.MULTILINE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("fixture")
    parser.add_argument("mode", help="the host's decoder and bound options, one shell-quoted string")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dsd-analog-audio-out-") as temporary:
        path = Path(temporary) / "audio.f32"
        command = [str(Path(args.binary).resolve()), "--frontend", "none", *shlex.split(args.mode),
                   "--analog-audio-out", str(path), "--iq-replay", str(Path(args.fixture).resolve()), "-o", "null"]
        result = subprocess.run(command, capture_output=True, timeout=110, check=False)
        err = result.stderr.decode("utf-8", "replace") + result.stdout.decode("utf-8", "replace")
        assert result.returncode == 0, err
        assert not SANITIZER.search(err), err
        assert "ANALOG AUDIO OK" in err, err
        metric = METRIC.search(err)
        out = AUDIO_OUT.search(err)
        assert metric and out, err
        samples = int(out.group(2))
        data = path.read_bytes()
        audio = array("f")
        assert samples > 0 and len(data) == samples * audio.itemsize, (samples, len(data))
        audio.frombytes(data)
        rms_dbfs = 10.0 * math.log10(math.fsum(x * x for x in audio) / samples)
        want = float(metric.group(1))
        print(f"file: {samples} samples at {out.group(1)} Hz, RMS {rms_dbfs:.4f} dBFS; metric line {want:.2f} dBFS")
        if abs(rms_dbfs - want) > RMS_TOLERANCE_DB:
            raise SystemExit(f"FAIL: the file's RMS {rms_dbfs:.4f} dBFS is not the metric line's {want:.2f} dBFS "
                             f"(tolerance {RMS_TOLERANCE_DB} dB)")
    print("ANALOG AUDIO OUT FILE OK")


if __name__ == "__main__":
    main()
