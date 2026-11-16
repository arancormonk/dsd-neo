# SPDX-License-Identifier: GPL-3.0-or-later
"""A raw I/Q stream on loopback, run as the CLI runs it (issue #743).

- A UDP stream says "I/Q stream: receiving on BIND:PORT" once, and only once its socket is bound: the port can no
  longer be bound by anyone else when the line appears.
- A TCP stream re-plans through a restart: a mode switch to TETRA over the control API reconnects to the sender, and
  says so. The tuning commands a stream cannot take reach no rigctl peer: rigctl_connect_cfg and manual_tune are
  refused, and the peer never sees a connection.
- -U beside a stream exits 1 before anything opens, and the rigctl peer never sees a connection.
"""
import argparse
import contextlib
import json
import os
import re
import signal
import socket
import subprocess
import tempfile
import threading
import time
from pathlib import Path

SANITIZER = re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|runtime error:")
DEADLINE_S = 30.0
DECODE_MODE_TETRA = 17


def free_port(kind):
    probe = socket.socket(socket.AF_INET, kind)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


class Output:
    """The decoder's merged output, read on a thread, so a test can wait for a line."""

    def __init__(self, process):
        self.process = process
        self.lines = []
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.read)
        self.thread.start()

    def read(self):
        for raw in self.process.stdout:
            with self.lock:
                self.lines.append(raw.decode(errors="replace"))

    def text(self):
        with self.lock:
            return "".join(self.lines)

    def wait_for(self, pattern, what):
        deadline = time.monotonic() + DEADLINE_S
        while time.monotonic() < deadline:
            if re.search(pattern, self.text()):
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.05)
        raise AssertionError("no " + what + " in the output:\n" + self.text())


def stop(process, output):
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=DEADLINE_S)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
        raise AssertionError("the decoder did not stop on SIGINT:\n" + output.text())
    output.thread.join()
    log = output.text()
    assert not SANITIZER.search(log), log
    assert process.returncode == 0, f"exit {process.returncode} after SIGINT:\n{log}"
    return log


def stop_after(process, output, body):
    """Run @p body, then stop the decoder whatever happened. A failure of @p body is the one raised: the stop's own
    checks (exit code, sanitizer output) are then not allowed to replace it."""
    try:
        body()
    except BaseException:
        with contextlib.suppress(AssertionError):
            stop(process, output)
        raise
    return stop(process, output)


def isolated_env():
    """The environment without DSD_NEO_CONFIG: --no-config does nothing, so this is what keeps a config out."""
    env = dict(os.environ)
    env.pop("DSD_NEO_CONFIG", None)
    return env


def start(binary, cwd, args):
    process = subprocess.Popen([binary, "--no-config", "--frontend", "none"] + args + ["-o", "null"], cwd=cwd,
                               env=isolated_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return process, Output(process)


class Sender:
    """A TCP I/Q server: counts the connections it accepts and sends each one cs16 silence at about real time."""

    def __init__(self):
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.server.settimeout(0.2)
        self.port = self.server.getsockname()[1]
        self.connections = 0
        self.lock = threading.Lock()
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.serve)

    def serve(self):
        senders = []
        while not self.done.is_set():
            try:
                connection, _ = self.server.accept()
            except socket.timeout:
                continue
            with self.lock:
                self.connections += 1
            sender = threading.Thread(target=self.send, args=(connection,))
            sender.start()
            senders.append(sender)
        self.server.close()
        for sender in senders:
            sender.join()

    def send(self, connection):
        block = bytes(4 * 480)  # 10 ms of 48 kS/s cs16
        try:
            while not self.done.is_set():
                connection.sendall(block)
                time.sleep(0.01)
        except OSError:
            pass
        connection.close()

    def count(self):
        with self.lock:
            return self.connections

    def wait_for(self, n, output):
        deadline = time.monotonic() + DEADLINE_S
        while time.monotonic() < deadline and self.count() < n:
            time.sleep(0.05)
        assert self.count() >= n, f"{self.count()} connection(s), want {n}:\n{output.text()}"

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.done.set()
        self.thread.join()


class Listener:
    """A rigctl peer that only counts the connections it is offered."""

    def __init__(self):
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.server.settimeout(0.2)
        self.port = self.server.getsockname()[1]
        self.connections = 0
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.serve)

    def serve(self):
        while not self.done.is_set():
            try:
                connection, _ = self.server.accept()
            except socket.timeout:
                continue
            self.connections += 1
            connection.close()
        self.server.close()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.done.set()
        self.thread.join()


class Api:
    """One control API connection: sends requests and waits for their responses."""

    def __init__(self, port):
        deadline = time.monotonic() + DEADLINE_S
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        self.file = self.sock.makefile("rwb")
        self.next_id = 1

    def call(self, cmd, params=None):
        request = {"id": self.next_id, "cmd": cmd}
        if params is not None:
            request["params"] = params
        self.next_id += 1
        self.file.write((json.dumps(request) + "\n").encode())
        self.file.flush()
        deadline = time.monotonic() + DEADLINE_S
        while time.monotonic() < deadline:
            line = self.file.readline()
            if not line:
                break
            message = json.loads(line)
            if message.get("id") == request["id"]:
                return message
        raise AssertionError("no response to " + cmd)

    def close(self):
        self.sock.close()


def check_udp_notice(binary, cwd):
    port = free_port(socket.SOCK_DGRAM)
    process, output = start(binary, cwd, ["-fs", "-i", f"iqudp:127.0.0.1:{port}:48000:cs16"])
    bound_by_us = []

    def body():
        output.wait_for(rf"I/Q stream: receiving on 127\.0\.0\.1:{port} ", "UDP receiving notice")
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.bind(("127.0.0.1", port))
            bound_by_us.append(True)
        except OSError:
            pass
        probe.close()

    log = stop_after(process, output, body)
    assert not bound_by_us, "the notice came before the stream bound its port:\n" + log
    assert len(re.findall(r"I/Q stream: receiving on", log)) == 1, "one receiving notice wanted:\n" + log
    print("UDP NOTICE OK: receiving on port", port, "once, after the bind")


def check_tcp_replan_and_tuning(binary, cwd):
    api_port = free_port(socket.SOCK_STREAM)
    with Sender() as sender, Listener() as rig:
        process, output = start(binary, cwd, ["-fs", "-i", f"iqtcp:127.0.0.1:{sender.port}:48000:cs16", "--api",
                                              str(api_port)])

        def body():
            sender.wait_for(1, output)
            api = Api(api_port)
            try:
                response = api.call("decode_mode_set", {"value": DECODE_MODE_TETRA})
                assert response.get("ok"), response
                output.wait_for(r"re-planning the rate chain for the TETRA decoder", "TETRA re-plan")
                sender.wait_for(2, output)
                assert api.call("rigctl_connect_cfg", {"host": "127.0.0.1", "port": rig.port}).get("ok")
                assert api.call("manual_tune", {"value": 851375000}).get("ok")
                time.sleep(1.0)
            finally:
                api.close()

        log = stop_after(process, output, body)
    assert rig.connections == 0, "a rigctl peer beside the stream was connected:\n" + log
    print("TCP RE-PLAN OK:", sender.connections, "connections; rigctl peer untouched")


def check_rigctl_refused_at_start(binary, cwd):
    with Listener() as rig:
        udp = f"iqudp:127.0.0.1:{free_port(socket.SOCK_DGRAM)}:48000:cs16"
        result = subprocess.run([binary, "--no-config", "--frontend", "none", "-i", udp, "-U", str(rig.port), "-o",
                                 "null"],
                                cwd=cwd, env=isolated_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=60, check=False)
        time.sleep(0.3)
    log = result.stdout.decode(errors="replace")
    assert result.returncode == 1, log
    assert "-U: an I/Q stream cannot retune" in log, log
    assert rig.connections == 0, "the rigctl peer was connected:\n" + log
    print("RIGCTL AT START OK: -U refused, no connection")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    with tempfile.TemporaryDirectory(prefix="dsd-iq-stream-") as temporary:
        check_udp_notice(binary, temporary)
        check_tcp_replan_and_tuning(binary, temporary)
        check_rigctl_refused_at_start(binary, temporary)


if __name__ == "__main__":
    main()
