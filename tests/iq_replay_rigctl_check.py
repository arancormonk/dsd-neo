# SPDX-License-Identifier: GPL-3.0-or-later
"""An I/Q replay with trunking and a rigctl peer (-T -U) sends the peer no retune (issue #743 B12).

A replay plays the tuning its capture recorded, so every trunking retune is refused. A rigctl peer beside it used to
take the voice grants the control channel names: the peer moved a radio the session does not hear while the replay
went on playing the control channel. A fake rigctl peer logs every command it receives; the decoder must connect to it,
refuse the retunes it decodes (its notice says so), and send it no frequency.
"""
import argparse
import re
import socket
import subprocess
import tempfile
import threading
from pathlib import Path

from iq_stream_live_check import isolated_env

FIXTURE = "p25p1_cqpsk_cc"
REFUSED = re.compile(r"Retune ignored during IQ replay")
SANITIZER = re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:")


class FakeRigctl:
    """A rigctl peer on a loopback port: answers the frequency and mode reads, takes every other command."""

    def __init__(self):
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.server.settimeout(0.2)
        self.port = self.server.getsockname()[1]
        self.commands = []
        self.connections = 0
        self.lock = threading.Lock()
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.serve)

    def serve(self):
        handlers = []
        while not self.done.is_set():
            try:
                connection, _ = self.server.accept()
            except socket.timeout:
                continue
            with self.lock:
                self.connections += 1
            handler = threading.Thread(target=self.handle, args=(connection,))
            handler.start()
            handlers.append(handler)
        self.server.close()
        for handler in handlers:
            handler.join()

    def handle(self, connection):
        connection.settimeout(0.2)
        pending = b""
        while not self.done.is_set():
            try:
                data = connection.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                command = line.decode(errors="replace").strip()
                with self.lock:
                    self.commands.append(command)
                try:
                    if command == "f" or command.startswith("\\get_freq"):
                        connection.sendall(b"851000000\n")
                    elif command == "m" or command.startswith("\\get_mode"):
                        connection.sendall(b"FM\n12500\n")
                    else:
                        connection.sendall(b"RPRT 0\n")
                except OSError:
                    break
        connection.close()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.done.set()
        self.thread.join()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("fixtures")
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    fixture = Path(args.fixtures).resolve() / (FIXTURE + ".iq.json")
    with tempfile.TemporaryDirectory(prefix="dsd-replay-rigctl-") as temporary, FakeRigctl() as peer:
        result = subprocess.run([binary, "--frontend", "none", "--iq-replay", str(fixture), "-f1", "-mq", "-T", "-U",
                                 str(peer.port), "-o", "null"],
                                cwd=temporary, env=isolated_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=120, check=False)
    log = result.stdout.decode(errors="replace")
    assert not SANITIZER.search(log), log
    assert result.returncode == 0, log
    assert peer.connections >= 1, "the decoder never connected to the rigctl peer:\n" + log
    tunes = [command for command in peer.commands if command.startswith(("F ", "\\set_freq"))]
    assert not tunes, "the rigctl peer was retuned during the replay: " + ", ".join(tunes)
    assert REFUSED.search(log), "the replay refused no retune (no grant decoded?):\n" + log
    print("IQ REPLAY RIGCTL OK:", len(peer.commands), "peer commands, none a retune")


if __name__ == "__main__":
    main()
