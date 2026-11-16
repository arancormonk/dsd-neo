# SPDX-License-Identifier: GPL-3.0-or-later
"""Raw I/Q over the network decodes end to end (issue #743).

Each case sends a committed fixture, converted to the wire format a sending program would use, to a dsd-neo running
`-i iqtcp:` or `-i iqudp:`, paced at real time as a live sender is. The stream is a live source: what a block holds
depends on when the samples arrived, so the cases count CRC-gated decodes with margin rather than compare lines with a
replay. Nothing ends a live stream, so once the counts are met (or a 60 s deadline passes) the decoder is stopped with
SIGINT, as Ctrl-C, and must exit 0 with no sanitizer report.

  tcp     (a) tetra_scbs_real_air_50k.cs16 as it is, cs16 at 50 kHz in 1024-byte writes, as SDR++ sends it; TETRA
              resamples it to 72 kHz. (b) tetra_call_real_air_72k as cf32 at 72 kHz with its FREQ: the call is
              published to the event history on its carrier's frequency. (c) call_48254/tetra.wav (IQ_WAV_FIXTURES) as
              cs16 at 48254 Hz, an odd rate resampled to 72 kHz. (d) dmr_voice as cs8 at 48 kHz.
  udp     (d) in 1472-byte datagrams (GNU Radio's UDP Sink size, under macOS's 9216-byte default limit) with three
              empty datagrams among them, sent once the decoder says it is receiving.
  analog  nfm_tone_synth and am_tone_synth as cs16 under -fA / -fM at a fixed gain: the monitor's 48 kHz audio
              carries the 1 kHz tone at the level a replay plays it; and the NFM tone at 24 kHz, which the input
              resamples to 48 kHz, the same.
"""
import argparse
import array
import math
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from iq_stream_live_check import Output, free_port, isolated_env, start, stop, stop_after

DEADLINE_S = 60.0
# A run that has met its counts gets this long to finish what it is decoding before it is stopped.
SETTLE_S = 1.0
BYTES_PER_SAMPLE = {"cs8": 2, "cs16": 4, "cf32": 8}


# ---- Wire formats a sender writes, from the committed fixtures --------------------------------------------------


def cu8_to_cs8(data):
    """The sender's view of a cu8 byte: signed, about zero."""
    return data.translate(bytes((value - 128) & 0xFF for value in range(256)))


def cu8_to_cs16(data):
    """cu8 scaled exactly to 16 bits: (b - 127.5) * 256."""
    out = array.array("h", (256 * value - 32640 for value in data))
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def cu8_to_cf32(data):
    """cu8 as the floats the cu8 reader makes of it: (b - 127.5) / 127.5."""
    out = array.array("f", ((value - 127.5) / 127.5 for value in data))
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def cs16_halve_rate(data):
    """cs16 I/Q at half its rate: a 31-tap Hamming-windowed sinc low-pass cut at 10/48 of the rate (flat to about 7 kHz
    at 48 kHz, which holds an NFM channel), then every second sample."""
    taps = 31
    cut = 10.0 / 48.0
    middle = taps // 2
    kernel = []
    for n in range(taps):
        x = n - middle
        ideal = 2.0 * cut if x == 0 else math.sin(2.0 * math.pi * cut * x) / (math.pi * x)
        kernel.append(ideal * (0.54 - 0.46 * math.cos(2.0 * math.pi * n / (taps - 1))))
    gain = sum(kernel)
    kernel = [k / gain for k in kernel]
    samples = array.array("h")
    samples.frombytes(data)
    if sys.byteorder == "big":
        samples.byteswap()
    i_in = samples[0::2].tolist()
    q_in = samples[1::2].tolist()
    out = array.array("h")
    for first in range(0, len(i_in) - taps + 1, 2):
        for channel in (i_in, q_in):
            window = channel[first:first + taps]
            value = round(sum(k * v for k, v in zip(kernel, window)))
            out.append(max(-32768, min(32767, value)))
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def wav_s24_to_cs16(path, rate_hz):
    """A 2-channel 24-bit PCM WAV's samples, each cut to its upper 16 bits (an arithmetic >> 8)."""
    data = Path(path).read_bytes()
    assert data[:4] == b"RIFF" and data[8:12] == b"WAVE", f"{path}: not a RIFF WAVE file"
    fmt = None
    pcm = None
    offset = 12
    while offset + 8 <= len(data):
        chunk_id, size = struct.unpack_from("<4sI", data, offset)
        body = data[offset + 8:offset + 8 + size]
        if chunk_id == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", body)
        elif chunk_id == b"data":
            pcm = body
        offset += 8 + size + (size & 1)
    assert fmt is not None and pcm is not None, f"{path}: no fmt or data chunk"
    _, channels, rate, _, _, bits = fmt
    assert (channels, rate, bits) == (2, rate_hz, 24), f"{path}: {channels} channels, {rate} Hz, {bits}-bit"
    whole = len(pcm) - len(pcm) % 6
    out = bytearray(whole // 3 * 2)
    out[0::2] = pcm[1:whole:3]
    out[1::2] = pcm[2:whole:3]
    return bytes(out)


# ---- Senders, paced at real time ---------------------------------------------------------------------------------


def paced(payload, chunk, byte_rate, done):
    """Yield @p payload in @p chunk-byte pieces, each when a real-time sender has it, until @p done is set."""
    started = time.monotonic()
    for offset in range(0, len(payload), chunk):
        if done.is_set():
            return
        delay = started + offset / byte_rate - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        yield offset, payload[offset:offset + chunk]


class TcpSender:
    """An I/Q server, as SDR++'s IQ Exporter in TCP (Server) mode: sends the payload to the first connection at real
    time, then keeps that connection open and quiet, as a sender with nothing more to send does. Counts the
    connections it accepts, while the payload is sent and up to the stop: a stream that reconnected would show in a
    second one."""

    def __init__(self, payload, chunk, byte_rate):
        self.payload = payload
        self.chunk = chunk
        self.byte_rate = byte_rate
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(4)
        self.server.settimeout(0.2)
        self.port = self.server.getsockname()[1]
        self.connections = 0
        self.sent = threading.Event()
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.serve)

    def send(self, connection):
        try:
            for _, piece in paced(self.payload, self.chunk, self.byte_rate, self.done):
                connection.sendall(piece)
        except OSError:
            pass
        self.sent.set()

    def serve(self):
        held = []
        sender = None
        while not self.done.is_set():
            try:
                connection, _ = self.server.accept()
            except socket.timeout:
                continue
            held.append(connection)
            if sender is None:
                sender = threading.Thread(target=self.send, args=(connection,))
                sender.start()
        # A connection made just before the stop is still in the backlog: count it too.
        self.server.setblocking(False)
        while True:
            try:
                held.append(self.server.accept()[0])
            except OSError:
                break
        if sender is not None:
            sender.join()
        self.connections = len(held)
        for connection in held:
            connection.close()
        self.server.close()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.done.set()
        self.thread.join()


class UdpSender:
    """One UDP sender, as GNU Radio's UDP Sink with Header None: the payload in datagrams of @p chunk bytes at real
    time, with an empty datagram before the first, one halfway and one after the last, which the stream ignores."""

    def __init__(self, port, payload, chunk, byte_rate):
        self.port = port
        self.payload = payload
        self.chunk = chunk
        self.byte_rate = byte_rate
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sent = threading.Event()
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.send)

    def send(self):
        target = ("127.0.0.1", self.port)
        halfway = len(self.payload) // self.chunk // 2 * self.chunk
        self.sock.sendto(b"", target)
        for offset, piece in paced(self.payload, self.chunk, self.byte_rate, self.done):
            if offset == halfway:
                self.sock.sendto(b"", target)
            self.sock.sendto(piece, target)
        self.sock.sendto(b"", target)
        self.sent.set()

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.done.set()
        self.thread.join()
        self.sock.close()


# ---- Running a case ----------------------------------------------------------------------------------------------


class Case:
    """One stream: the decoder's mode flags, the wire format and rate, the payload, and what the run must print: at
    least `counts[regex]` matches of each regex, each of `present`, none of `absent`, and when `event` is set, an
    event history (-J) that matches it."""

    def __init__(self, name, mode, rate_hz, fmt, payload, chunk, counts, present=(), absent=None, freq_hz=None,
                 event=None):
        self.name = name
        self.mode = mode
        self.rate_hz = rate_hz
        self.fmt = fmt
        self.payload = payload
        self.chunk = chunk
        self.counts = counts
        self.present = present
        self.absent = absent
        self.freq_hz = freq_hz
        self.event = event

    def spec(self, kind, port):
        spec = f"{kind}:127.0.0.1:{port}:{self.rate_hz}:{self.fmt}"
        return spec if self.freq_hz is None else f"{spec}:{self.freq_hz}"

    def byte_rate(self):
        return self.rate_hz * BYTES_PER_SAMPLE[self.fmt]


def tally(case, log):
    return {pattern: len(re.findall(pattern, log)) for pattern in case.counts}


def met(case, log):
    """The counts and lines @p case needs are in @p log. Its event is not waited on: a call still on the air is
    published when it ends, which for the last call is when the decoder stops."""
    if any(count < case.counts[pattern] for pattern, count in tally(case, log).items()):
        return False
    return all(re.search(pattern, log) for pattern in case.present)


def wait_decoded(case, process, output, sender, measure):
    """Wait until the run has printed what @p case needs, the decoder has ended, or the deadline has passed. Under
    --measure, wait for the whole payload instead, so the counts printed are what the stream decodes in full."""
    deadline = time.monotonic() + DEADLINE_S
    while time.monotonic() < deadline and process.poll() is None:
        if measure:
            if sender.sent.is_set():
                time.sleep(3.0)
                return
        elif met(case, output.text()):
            time.sleep(SETTLE_S)
            return
        time.sleep(0.1)


def check(case, label, log, event_path, measure):
    counts = tally(case, log)
    summary = ", ".join(f"{count} x /{pattern}/" for pattern, count in counts.items())
    events = event_path.read_text(errors="replace") if event_path.exists() else ""
    print(f"{label}: {summary}" + (f", event {bool(re.search(case.event, events))}" if case.event else ""))
    if measure:
        return
    for pattern, count in counts.items():
        assert count >= case.counts[pattern], f"{label}: /{pattern}/ {count} times, want {case.counts[pattern]}:\n{log}"
    for pattern in case.present:
        assert re.search(pattern, log), f"{label}: no /{pattern}/ in the output:\n{log}"
    if case.absent is not None:
        hit = re.search(case.absent, log)
        assert not hit, f"{label}: unexpected {hit.group(0)!r} in the output:\n{log}"
    if case.event is not None:
        assert re.search(case.event, events), f"{label}: no /{case.event}/ in the event history:\n{events}\n{log}"


def event_args(case, cwd):
    event_path = Path(cwd) / (case.name + "_events.log")
    return event_path, (["-J", str(event_path)] if case.event is not None else [])


def run_tcp(binary, cwd, case, measure):
    event_path, events = event_args(case, cwd)
    with TcpSender(case.payload, case.chunk, case.byte_rate()) as sender:
        process, output = start(binary, cwd, case.mode + events + ["-i", case.spec("iqtcp", sender.port)])
        log = stop_after(process, output,
                         lambda: wait_decoded(case, process, output, sender, measure))
    check(case, "TCP " + case.name, log, event_path, measure)
    assert sender.connections == 1, f"TCP {case.name}: {sender.connections} connections, want 1:\n{log}"


# The UDP stream's NOTICE once its socket is bound and read.
def udp_receiving(port):
    return rf"I/Q stream: receiving on 127\.0\.0\.1:{port}\b"


# Its refusal of a port it cannot bind, matched by the port alone: any line of the stream's that names the port.
def udp_port_line(port):
    return rf"I/Q stream: [^\n]*127\.0\.0\.1:{port}\b"


# How long a decoder that said it is receiving must stay up before the payload is sent to it.
UDP_SETTLE_S = 0.3


def start_udp(binary, cwd, case, args):
    """Start the decoder on a free UDP port and wait until it says it is receiving there and is still running a moment
    later. The port is free when it is picked but can be taken before the decoder binds it; the decoder then exits
    refusing it, and a new port is tried."""
    for _ in range(5):
        port = free_port(socket.SOCK_DGRAM)
        process, output = start(binary, cwd, case.mode + args + ["-i", case.spec("iqudp", port)])
        deadline = time.monotonic() + DEADLINE_S
        while time.monotonic() < deadline and process.poll() is None:
            if re.search(udp_receiving(port), output.text()):
                time.sleep(UDP_SETTLE_S)
                if process.poll() is None:
                    return port, process, output
                break
            time.sleep(0.05)
        if process.poll() is None:
            stop(process, output)
            raise AssertionError(f"UDP {case.name}: no receiving notice on port {port}:\n{output.text()}")
        output.thread.join()
        log = output.text()
        if process.returncode == 0 or not re.search(udp_port_line(port), log):
            raise AssertionError(f"UDP {case.name}: the decoder exited {process.returncode} before receiving:\n{log}")
    raise AssertionError(f"UDP {case.name}: no free port could be bound in 5 tries")


def run_udp(binary, cwd, case, measure):
    event_path, events = event_args(case, cwd)
    port, process, output = start_udp(binary, cwd, case, events)
    sender = UdpSender(port, case.payload, case.chunk, case.byte_rate())

    def body():
        with sender:
            wait_decoded(case, process, output, sender, measure)

    log = stop_after(process, output, body)
    check(case, "UDP " + case.name, log, event_path, measure)


# ---- Analog ------------------------------------------------------------------------------------------------------


class StderrOutput(Output):
    """The decoder's log when its stdout carries audio."""

    def read(self):
        for raw in self.process.stderr:
            with self.lock:
                self.lines.append(raw.decode(errors="replace"))


AUDIO_HZ = 48000
TONE_HZ = 1000.0


def tone_measures(pcm):
    """RMS level (dBFS) and frequency (from rising zero crossings at 48 kHz) of the steady tone in s16 mono audio:
    the first 0.3 s, where the receiver settles, and the last 0.1 s, where the stream ended, are left out."""
    samples = array.array("h")
    samples.frombytes(pcm[:len(pcm) - len(pcm) % 2])
    if sys.byteorder == "big":
        samples.byteswap()
    steady = samples[int(0.3 * AUDIO_HZ):len(samples) - int(0.1 * AUDIO_HZ)]
    if len(steady) < AUDIO_HZ // 2:
        return None, None, len(samples)
    rms = math.sqrt(sum(value * value for value in steady) / len(steady)) / 32768.0
    level = 20.0 * math.log10(rms) if rms > 0 else -200.0
    rising = [i for i in range(1, len(steady)) if steady[i - 1] < 0 <= steady[i]]
    if len(rising) < 2:
        return level, None, len(samples)
    frequency = (len(rising) - 1) * AUDIO_HZ / (rising[-1] - rising[0])
    return level, frequency, len(samples)


def run_analog(binary, cwd, name, mode, rate_hz, payload, level_dbfs, measure, chain=None):
    """Stream @p payload (cs16 at @p rate_hz) to -fA/-fM with the monitor on stdout, until its audio stops growing;
    the log must match @p chain when given."""
    audio_path = Path(cwd) / (name + ".raw")
    byte_rate = rate_hz * BYTES_PER_SAMPLE["cs16"]
    with TcpSender(payload, byte_rate // 100, byte_rate) as sender, audio_path.open("wb") as audio:
        spec = f"iqtcp:127.0.0.1:{sender.port}:{rate_hz}:cs16"
        process = subprocess.Popen([binary, "--no-config", "--frontend", "none"] + mode + ["-i", spec, "-o", "-"],
                                   cwd=cwd, env=isolated_env(), stdout=audio, stderr=subprocess.PIPE)
        output = StderrOutput(process)

        def body():
            deadline = time.monotonic() + DEADLINE_S
            size = -1
            while time.monotonic() < deadline and process.poll() is None:
                if sender.sent.is_set():
                    grown = audio_path.stat().st_size
                    if grown == size:
                        return
                    size = grown
                time.sleep(1.0)

        log = stop_after(process, output, body)
    pcm = audio_path.read_bytes()
    level, frequency, samples = tone_measures(pcm)
    print(f"ANALOG {name}: {samples} samples ({samples / AUDIO_HZ:.2f} s), RMS {level} dBFS, tone {frequency} Hz")
    if measure:
        return
    assert chain is None or re.search(chain, log), f"ANALOG {name}: no /{chain}/ in the output:\n{log}"
    assert len(pcm) % 2 == 0, f"ANALOG {name}: {len(pcm)} bytes of audio, not whole s16 samples:\n{log}"
    # The 1.5 s fixtures play at least 1.2 s; a replay plays about 1.5 s.
    assert samples >= int(1.2 * AUDIO_HZ), f"ANALOG {name}: {samples} samples of audio, want 1.2 s:\n{log}"
    assert level is not None and abs(level - level_dbfs) <= 1.0, \
        f"ANALOG {name}: RMS {level} dBFS, want {level_dbfs} +/- 1:\n{log}"
    # The tone at 1 kHz says the monitor's audio comes out at the 48 kHz the output is written at.
    assert frequency is not None and abs(frequency - TONE_HZ) <= 5.0, \
        f"ANALOG {name}: tone at {frequency} Hz, want 1000:\n{log}"


# ---- The cases ---------------------------------------------------------------------------------------------------

SANITY_ABSENT = r"TETRA needs a DSP rate|CLIP"


def tetra_cases(fixtures, wav_fixtures):
    call = (fixtures / "tetra_call_real_air_72k.iq").read_bytes()
    call_bsch = r"\[TETRA BSCH\] MCC=228 MNC=8889 CC=0x01"
    call_setup = (r"\[TETRA CMCE D-SETUP\] CC=1 call=113 timeout=0 circuit=0 enc=0 com=1 service=0 grant=1 request=0 "
                  r"priority=1 calling_SSI=100701")
    call_speech = r"\[TETRA TCH/FS\] CC=1  dec_bits=274  crc=ok"
    # Replay and every live run measured (dev-debug, ASan, TSan, TSan on one core) decode each payload in full: the
    # 50 kHz capture 79 identities, the call 232 identities, its D-SETUP twice and 48 TCH/S bursts whose CRC passes.
    return [
        Case("scbs_50k_cs16", ["-fT"], 50000, "cs16", (fixtures / "tetra_scbs_real_air_50k.cs16").read_bytes(), 1024,
             {r"\[TETRA BSCH\] MCC=250 MNC=13 CC=0x2c": 25},
             present=[r"Rate chain: capture=50000 Hz resampled to 72000 Hz"], absent=SANITY_ABSENT,
             freq_hz=467913000),
        Case("call_72k_cf32", ["-fT", "-Z"], 72000, "cf32", cu8_to_cf32(call), 5760,
             {call_bsch: 200, call_setup: 1, call_speech: 40}, absent=SANITY_ABSENT, freq_hz=426700000,
             event=(r"TETRA TGT: 00100601; SRC: 00100702; CC: 01; CARRIER: 1068; FREQ: 426\.700000 MHz; MCC: 228; "
                    r"MNC: 8889; LA: 1; Group;")),
        Case("call_48254_cs16", ["-fT", "-Z"], 48254, "cs16",
             wav_s24_to_cs16(wav_fixtures / "call_48254" / "tetra.wav", 48254), 3860,
             {call_bsch: 200, call_setup: 1, call_speech: 40},
             present=[r"Rate chain: capture=48254 Hz resampled to 72000 Hz"], absent=SANITY_ABSENT),
    ]


def dmr_case(fixtures, chunk, csbk):
    """dmr_voice decodes 51 CSBKs from a replay and from every live run measured, over TCP and UDP alike. UDP is held
    to fewer: a datagram the host drops is gone, where TCP's are sent again."""
    return Case("dmr_voice_cs8", ["-fs"], 48000, "cs8", cu8_to_cs8((fixtures / "dmr_voice.iq").read_bytes()), chunk,
                {r"Color Code=01 \| CSBK": csbk}, present=[r"decoding inverted polarity"],
                absent=r"\| VC[1-6]|DMR RAS \(Restricted Access to System\) detected")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wav-fixtures", type=Path, help="the IQ_WAV_FIXTURES output directory (tcp case c)")
    parser.add_argument("--measure", action="store_true",
                        help="send each whole payload and print the counts, without waiting on or checking them")
    parser.add_argument("check", choices=["tcp", "udp", "analog"])
    parser.add_argument("binary")
    parser.add_argument("fixtures", type=Path)
    args = parser.parse_args()
    if args.check == "tcp" and args.wav_fixtures is None:
        parser.error("tcp needs --wav-fixtures")
    binary = str(Path(args.binary).resolve())
    fixtures = args.fixtures.resolve()
    with tempfile.TemporaryDirectory(prefix="dsd-iq-net-") as temporary:
        if args.check == "tcp":
            for case in tetra_cases(fixtures, args.wav_fixtures) + [dmr_case(fixtures, 960, 40)]:
                run_tcp(binary, temporary, case, args.measure)
        elif args.check == "udp":
            run_udp(binary, temporary, dmr_case(fixtures, 1472, 35), args.measure)
        else:
            nfm = cu8_to_cs16((fixtures / "nfm_tone_synth.iq").read_bytes())
            am = cu8_to_cs16((fixtures / "am_tone_synth.iq").read_bytes())
            run_analog(binary, temporary, "nfm_tone", ["-fA", "-n", "50"], 48000, nfm, -15.06, args.measure)
            run_analog(binary, temporary, "am_tone", ["-fM", "-n", "50"], 48000, am, -15.0, args.measure)
            # The NFM tone at 24 kHz, which the input resamples to the monitor's 48 kHz: the same level and tone.
            run_analog(binary, temporary, "nfm_tone_24k", ["-fA", "-n", "50"], 24000, cs16_halve_rate(nfm), -15.06,
                       args.measure, chain=r"Rate chain: capture=24000 Hz resampled to 48000 Hz")
    print("IQ NET " + args.check.upper() + " OK")


if __name__ == "__main__":
    main()
