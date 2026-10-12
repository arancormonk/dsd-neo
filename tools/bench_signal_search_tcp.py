# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>

"""Manual end-to-end RF publication benchmark against a paced loopback rtl_tcp source.

Requires NumPy. Uses an ephemeral loopback port and never opens physical hardware.
The NFM transmitter stays at 163.0125 MHz while the receiver changes capture centres.
"""

import argparse
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import threading
import time

import numpy as np


class Source:
    def __init__(self):
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen(1)
        self.socket.settimeout(30)
        self.port = self.socket.getsockname()[1]
        self.stop = threading.Event()
        self.frequency = 163000000
        self.rate = 1536000
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def commands(self, connection):
        pending = b""
        while not self.stop.is_set():
            try:
                block = connection.recv(128)
                if not block:
                    return
                pending += block
                while len(pending) >= 5:
                    command, value = struct.unpack("!BI", pending[:5])
                    pending = pending[5:]
                    if command == 1:
                        self.frequency = value
                    elif command == 2 and value:
                        self.rate = value
            except socket.timeout:
                continue
            except OSError:
                return

    def stream(self, connection):
        rng = np.random.default_rng(537)
        phase, sample_time = 0.0, 0.0
        count = 8192
        index = np.arange(count)
        deadline = time.monotonic()
        while not self.stop.is_set():
            rate = self.rate
            delta = 163012500 - self.frequency
            deviation = 2000 * np.sin(2 * np.pi * 900 * (sample_time + index / rate))
            angles = phase + np.cumsum(2 * np.pi * (delta + deviation) / rate)
            phase = float(angles[-1] % (2 * np.pi))
            sample_time += count / rate
            amplitude = 20 if abs(delta) < rate / 2 else 0
            iq = np.empty(count * 2)
            iq[::2] = 127.5 + amplitude * np.cos(angles) + rng.normal(0, 0.7, count)
            iq[1::2] = 127.5 + amplitude * np.sin(angles) + rng.normal(0, 0.7, count)
            connection.sendall(np.clip(np.rint(iq), 0, 255).astype("u1").tobytes())
            deadline += count / rate
            self.stop.wait(max(0, deadline - time.monotonic()))

    def serve(self):
        try:
            connection, _ = self.socket.accept()
        except OSError:
            return
        with connection:
            connection.settimeout(1)
            connection.sendall(b"RTL0" + struct.pack("!II", 1, 1))
            reader = threading.Thread(target=self.commands, args=(connection,), daemon=True)
            reader.start()
            try:
                self.stream(connection)
            except OSError:
                pass  # The benchmark closes the source after its first detection.
            finally:
                self.stop.set()
                reader.join(timeout=2)

    def close(self):
        self.stop.set()
        self.socket.close()
        self.thread.join(timeout=3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path, help="dsd-neo_test_signal_search_bench executable")
    args = parser.parse_args()
    source = Source()
    source.thread.start()
    env = dict(os.environ, DSD_SEARCH_BENCH_MODE="hit")
    try:
        result = subprocess.run(
            [str(args.binary.resolve()), "--frontend", "none", "-fA", "-i",
             f"rtltcp:127.0.0.1:{source.port}:163M:0:0:48:0:1", "-o", "null"],
            env=env, capture_output=True, text=True, timeout=30, check=False,
        )
    finally:
        source.close()
    print(result.stdout, end="")
    match = re.search(r"visits=1 .*max_report_latency_us=(\d+) hit_frequency_hz=163012500", result.stdout)
    if result.returncode or not match:
        print(result.stderr, end="")
        raise SystemExit("Expected a detection at the synthetic transmitter frequency")
    if int(match.group(1)) >= 100000:
        raise SystemExit("RF publication exceeded 100 ms after the completing capture block arrived")


if __name__ == "__main__":
    main()
