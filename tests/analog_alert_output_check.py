# SPDX-License-Identifier: GPL-3.0-or-later
"""Analog calls sound no call alert (issue #667 A1), end to end on the analog monitor's outputs.

With -a, a call alert used to sound at the start and end of every analog row. On -o - its 8 kHz beep blocks were
spliced into the 48 kHz monitor stream, 3840 bytes more per row; on -o udp they went to the digital base port, while
the analog monitor streams to base+2. One I/Q replay of a CTCSS NFM transmission (one analog row in -J) must therefore
write the same stdout bytes with and without -a, and over UDP send every byte to base+2 and nothing to the base port,
with and without -a alike.
"""
import argparse
from pathlib import Path
import re
import select
import socket
import subprocess
import tempfile
import threading

ANALOG_ROW = re.compile(r" ANALOG(?: NFM| AM)?;")
SANITIZER = re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|LeakSanitizer|runtime error:")


def run(binary, fixture, directory, output, alert):
    """One replay with an -J log of its own; returns its stdout bytes, stderr text and analog rows."""
    run.count += 1
    events = directory / f"events-{run.count}.log"
    command = [binary, "--frontend", "none", "-fA", "--squelch", "-60", *(["-a"] if alert else []),
               "-J", str(events), "--iq-replay", str(fixture), "-o", output]
    result = subprocess.run(command, cwd=directory, capture_output=True, timeout=110, check=False)
    err = result.stderr.decode("utf-8", "replace")
    assert result.returncode == 0, err
    assert not SANITIZER.search(err), err
    history = events.read_text() if events.exists() else ""
    rows = [line for line in history.splitlines() if ANALOG_ROW.search(line)]
    return result.stdout, err, rows


run.count = 0


def bind_port_pair():
    """Two UDP sockets on 127.0.0.1, ports base and base+2, both free."""
    for _ in range(50):
        base = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        base.bind(("127.0.0.1", 0))
        port = base.getsockname()[1]
        if port + 2 > 65535:
            base.close()
            continue
        analog = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            analog.bind(("127.0.0.1", port + 2))
        except OSError:
            base.close()
            analog.close()
            continue
        return port, base, analog
    raise SystemExit("no free UDP port pair")


def drain(sockets, counts, stop):
    """Count the bytes each socket receives until stop is set and nothing more is waiting."""
    while True:
        ready, _, _ = select.select(sockets, [], [], 0.2)
        for sock in ready:
            counts[sock.fileno()] += len(sock.recv(65536))
        if not ready and stop.is_set():
            return


def udp_run(binary, fixture, directory, alert):
    """One replay to -o udp; returns the bytes the digital base port and the analog base+2 port received."""
    port, base, analog = bind_port_pair()
    counts = {base.fileno(): 0, analog.fileno(): 0}
    stop = threading.Event()
    reader = threading.Thread(target=drain, args=([base, analog], counts, stop))
    reader.start()
    try:
        _, _, rows = run(binary, fixture, directory, "udp:127.0.0.1:" + str(port), alert)
    finally:
        stop.set()
        reader.join()
    result = counts[base.fileno()], counts[analog.fileno()], rows
    base.close()
    analog.close()
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary")
    parser.add_argument("fixture")
    args = parser.parse_args()
    binary = str(Path(args.binary).resolve())
    fixture = Path(args.fixture).resolve()
    failures = []
    with tempfile.TemporaryDirectory(prefix="dsd-analog-alert-") as temporary:
        directory = Path(temporary)

        plain, _, rows = run(binary, fixture, directory, "-", False)
        assert len(rows) == 1, rows
        alerted, _, alert_rows = run(binary, fixture, directory, "-", True)
        assert alert_rows == rows, (rows, alert_rows)
        assert plain and len(plain) % 2 == 0, len(plain)
        if alerted != plain:
            failures.append(f"-a changed stdout: {len(plain)} bytes without, {len(alerted)} with")
        else:
            print(f"stdout OK: {len(plain)} bytes with and without -a")

        digital, analog, rows = udp_run(binary, fixture, directory, False)
        assert len(rows) == 1, rows
        assert digital == 0 and analog > 0, (digital, analog)
        alert_digital, alert_analog, rows = udp_run(binary, fixture, directory, True)
        assert len(rows) == 1, rows
        if alert_digital != 0 or alert_analog != analog:
            failures.append(f"-a over UDP: {alert_digital} bytes to the digital port (want 0), {alert_analog} to base+2 "
                            f"(want {analog}, as without -a)")
        else:
            print(f"udp OK: {analog} bytes to base+2, none to the base port, with and without -a")
    if failures:
        raise SystemExit("\n".join(failures))


if __name__ == "__main__":
    main()
