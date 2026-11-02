# SPDX-License-Identifier: GPL-3.0-or-later
"""M17 and EDACS decode the same from a PCM file, from stdin and over TCP (issue #667 D12).

stdin and TCP audio are read through one buffered PCM reader with bounded waits; a file still goes through libsndfile.
The capture's discriminator audio (-6, from --iq-replay) is decoded each way, and the decoded protocol lines must be
the same. The last one is left out: where the stream ends inside a frame differs between a file's end, stdin's and a
closed connection's.
"""
import argparse
from pathlib import Path
import re
import signal
import socket
import subprocess
import tempfile
import threading
import time
import wave

CASES = [
    ("m17", "-fz", re.compile(r"M17")),
    ("edacs", "-fh", re.compile(r"EDACS")),
]
STAMP = re.compile(r"\d\d:\d\d:\d\d(\.\d+)?")
SANITIZER = re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:")


def decoded(text, pattern):
    lines = [STAMP.sub("T", line) for line in text.splitlines() if pattern.search(line)]
    assert not SANITIZER.search(text), text
    return lines[:-1]


def run(binary, mode, source, cwd, stdin=None):
    result = subprocess.run([binary, "--frontend", "none", mode, "-o", "null", "-i", source], cwd=cwd, input=stdin,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120, check=False)
    return result.stdout.decode(errors="replace")


def run_tcp(binary, mode, pcm, cwd):
    """Serve the audio over a loopback connection, in 20 ms chunks a little faster than real time, then close it. An
    M17-only session retries a lost server without end, so the decoder is stopped (SIGINT, as Ctrl-C) a while after."""
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    port = server.getsockname()[1]

    def serve():
        connection, _ = server.accept()
        for start in range(0, len(pcm), 1920):
            connection.sendall(pcm[start:start + 1920])
            time.sleep(0.004)
        connection.close()
        server.close()

    thread = threading.Thread(target=serve)
    thread.start()
    # The output goes to a file: nothing drains a pipe while the audio is served.
    log_path = Path(cwd) / ("tcp_" + mode.strip("-") + ".log")
    with log_path.open("wb") as log:
        decoder = subprocess.Popen(
            [binary, "--frontend", "none", mode, "-o", "null", "-i", f"tcp:127.0.0.1:{port}"],
            cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        try:
            thread.join()
            try:
                decoder.wait(timeout=3)
            except subprocess.TimeoutExpired:
                decoder.send_signal(signal.SIGINT)
                decoder.wait(timeout=60)
        finally:
            if decoder.poll() is None:
                decoder.kill()
                decoder.wait()
    return log_path.read_text(errors="replace")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("fixtures")
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    fixtures = Path(args.fixtures).resolve()
    with tempfile.TemporaryDirectory(prefix="dsd-pcm-paths-") as temporary:
        for fixture, mode, pattern in CASES:
            raw = Path(temporary) / (fixture + "_raw.wav")
            made = subprocess.run([binary, "--frontend", "none", mode, "--iq-replay",
                                   str(fixtures / (fixture + ".iq.json")), "-o", "null", "-6", str(raw)],
                                  cwd=temporary, capture_output=True, timeout=120, check=False)
            assert made.returncode == 0 and raw.exists(), made.stdout + made.stderr
            with wave.open(str(raw)) as recording:
                assert recording.getframerate() == 48000 and recording.getnchannels() == 1
                pcm = recording.readframes(recording.getnframes())
            from_file = decoded(run(binary, mode, str(raw), temporary), pattern)
            from_stdin = decoded(run(binary, mode, "-", temporary, stdin=pcm), pattern)
            from_tcp = decoded(run_tcp(binary, mode, pcm, temporary), pattern)
            assert len(from_file) >= 20, f"{fixture}: the file decoded too little:\n" + "\n".join(from_file)
            assert from_stdin == from_file, f"{fixture}: stdin decodes differently"
            assert from_tcp == from_file, f"{fixture}: TCP decodes differently"
            print(fixture, "OK", len(from_file), "lines")
    print("PCM INPUT PATHS OK")


if __name__ == "__main__":
    main()
