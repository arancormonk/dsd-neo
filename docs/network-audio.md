# Network Audio I/O (TCP/UDP/stdin/stdout)

This document describes the raw audio stream formats used by DSD-neo for network and pipe I/O. These interfaces are
intentionally simple: they are headerless streams/datagrams with no framing metadata.

If you just want “what flag do I type”, start with `docs/cli.md`.

## PCM Input (`-i tcp`, `-i udp`, `-i -`)

DSD-neo accepts raw PCM input in three equivalent ways:

- **TCP**: `-i tcp[:host:port]` (bare `tcp` connects to `localhost:7355`)
- **UDP**: `-i udp[:bind_addr:port]` (default `127.0.0.1:7355`)
- **stdin**: `-i -`

### Input format

- Sample type: **signed 16-bit integer**
- Endianness: **little-endian** (`s16le`)
- Channels: **mono**
- Sample rate: controlled by `-s <Hz>` (default `48000`)
- Container/framing: **none** (raw PCM stream, or UDP datagrams containing raw PCM bytes)

Notes:

- For UDP input, DSD-neo reads each datagram and widens it to samples. Datagrams with an odd byte are truncated to an
  even byte count (whole `int16_t` samples).
- If UDP bursts faster than the internal ring can drain, samples may be dropped. Prefer steady packet sizes (e.g., ~20ms
  of audio per datagram).
- For TCP input, DSD-neo is the client. `tcp:<host>:<port>` must name the computer and port where the PCM producer is
  listening. `localhost` only reaches a producer on the same computer. To use a LAN address, configure the producer to
  listen on its LAN interface or `0.0.0.0`, and allow inbound TCP through the host firewall.
- With rigctl enabled (`-U <port>`), a TCP input host is also used as the rigctl host. For SDR++ on another PC, allow
  both the TCP audio port, commonly `7355`, and the rigctl port, commonly `4532`.
- From the terminal UI, Input > Switch source > `UDP audio...` binds the address and port at once and `TCP audio...`
  (or `8`) connects at once; either keeps the running input when it cannot (a held port, a refused or unanswered
  connection). A TCP connect gives up after 10 seconds; a host name is looked up through the system resolver, which
  bounds that wait itself, and a reconnect reuses the address it found. While a UDP sender is silent between
  transmissions, menu commands still apply after about half a second; a sender that stops in the middle of a frame is
  waited for, as before.

### Squelch on PCM input

The analog FM monitor (`-fA`) on PCM input takes a level squelch (`--squelch -50`, against the input level) or the
noise squelch (`--squelch noise[+N]`), which learns what noise alone puts above the voice band from the stream itself
and opens when a carrier quiets it by N dB, as a radio's squelch does. It needs that noise in the stream: feed it the
discriminator, or an SDR program's FM audio with its audio low-pass off, at 12 kHz or more. On low-passed audio it says
so once and stays off. See "Noise squelch on audio input" in `docs/cli.md`.

## UDP Audio Output (`-o udp`)

`-o udp[:host:port]` sends decoded audio to a UDP “blaster” socket (default `127.0.0.1:23456`).

### Digital decoded voice (default UDP port)

The primary UDP output carries decoded digital voice:

- Sample rate: **8000 Hz**
- Channels:
  - Often **stereo** (2 channels, interleaved) by default
- Sample type:
  - Default: **`s16le`** (signed 16-bit little-endian)
  - With `-y`: **`f32le`** (32-bit float little-endian): the same samples as the default, each the `s16le` value /
    32768, so always within ±1. Each block the default sends as one datagram goes out as one `f32le` datagram, twice the
    size. Two writers keep their own format: M17 voice is still sent as `s16le`, and the alert tones are synthesized as
    float directly (a different waveform from their `s16le` form).

### Analog/source monitor (UDP port + 2)

With `-o udp`, DSD-neo also opens an **analog monitor** UDP socket on `<port + 2>` (for example, `23458` when the
base port is `23456`) whenever something writes analog or source audio:

- at start, for source monitoring (`-8`), the analog monitor presets (`-fA` for FM, `-fM` for native AM) and
  ProVoice;
- during a session, when the decode mode switches to Analog, AM (or ProVoice) from the terminal menu, the Qt/Android
  app or a config apply, even while output is muted. If that socket cannot be opened, the failure is logged once and
  analog audio stays silent.

Under `-fM` the stream carries the AM detector's audio, at the same level live FM has at about 6 kHz deviation for 100%
modulation; a switch between AM and Analog keeps the socket, since both write it.

The CTCSS/DCS tone filter (`--tone-allow`, `--tone-block`, `[analog] tone_filter`; see "Tone filter" in
`docs/cli.md`) mutes this stream and the local analog output together: nothing is sent while a transmission is being
checked or after the filter rejected it. The `-6` raw WAV is not gated by it.

Changing the UDP output target during a session moves an open port + 2 socket to the new host and port: it is closed,
since it still sends to the old target, and reopened for the new one at once, so source monitoring turned back on
later still has it. With no port + 2 socket open, the change opens one only when the current mode writes analog or
source audio, and otherwise the next switch to Analog, AM or ProVoice does.

The analog/source monitor stream is:

- Sample rate: **48000 Hz**, at every input rate: audio input at another rate is converted to 48 kHz before it is sent
  (see "The analog outputs" in `docs/cli.md`), EDACS analog voice included
- Channels: **mono**
- Sample type: **`s16le`**
- Datagrams: at most 960 samples (1920 bytes) of monitor or EDACS audio each; the M17 encoder's baseband (`-8` while
  encoding) goes out in datagrams of 1920 samples (3840 bytes)

A send that fails (no route, the network down) drops that datagram; the first failure of a run is logged, then at
most one line a minute per socket with a count, and one line when sends go through again.

## Listen to UDP Output (Examples)

These examples use `socat` to receive UDP datagrams and feed a player that can consume raw PCM from stdin.

Digital voice (default port `23456`, 8 kHz):

```bash
# PCM16LE stereo (common default)
socat -u UDP-RECV:23456,reuseaddr STDOUT | ffplay -nodisp -f s16le -ar 8000 -ac 2 -i -

# Float32 stereo (if you run DSD-neo with -y)
socat -u UDP-RECV:23456,reuseaddr STDOUT | ffplay -nodisp -f f32le -ar 8000 -ac 2 -i -
```

Analog/source monitor (port `23458`, 48 kHz mono):

```bash
socat -u UDP-RECV:23458,reuseaddr STDOUT | ffplay -nodisp -f s16le -ar 48000 -ac 1 -i -
```

## stdout Audio Output (`-o -`)

`-o -` writes the same raw decoded audio stream to stdout. The format matches the “Digital decoded voice” description
above (rate/channels/type depend on mode and `-y`). Some writers send analog audio instead, as **48 kHz mono `s16le`**:

- The analog monitor in the analog-only modes (`-fA`, `-fM`, or a switch to Analog or AM): no digital writer shares
  the stream then, so it carries the monitor's audio alone, as the UDP port + 2 stream does, whatever `-y` says.
- EDACS analog voice, at every input rate, without `-y` (with `-y` the stream is `f32le`, so EDACS analog voice is not
  written to stdout; use a sound device or UDP for it).
- The M17 encoder's baseband with `-8`.

ProVoice's decoded voice keeps the digital format. The `-8` source monitor while decoding digital modes is not written
to stdout: its 48 kHz audio would interleave with the decoded voice's 8 kHz stream. DSD-neo warns once at startup when
`-o -` and `-8` meet; use `-o udp` (port + 2) or a sound device for it.

Writes go out in whole sample frames, so the stream never shifts by a byte:

- **The reader goes away** (`-o - | head -c 1000`, a closed socket) or a **write fails** (`-o - > /dev/full`, a full
  disk): one error in the log, then stdout audio stays off for the rest of the session. Decoding, the `-w`/`-P`
  recordings, trunk following and the other outputs carry on, and a normal shutdown still finalizes the WAV files.
  DSD-neo never dies of SIGPIPE.
- **The reader is slow**: stdout is an ordinary blocking pipe, so DSD-neo waits for it, as any pipe writer does, and a
  reader that stops reading without closing the pipe holds up decoding. A non-blocking stdout that is full drops whole
  blocks instead of waiting, with one warning and a count at most once a minute after it.

This can be useful when you want to keep transport out of DSD-neo (pipe into another tool, or re-packetize yourself).

## M17 UDP/IP Frames (`m17udp`)

`m17udp` is a separate M17 frame transport, not raw PCM audio:

- Input: `-i m17udp[:bind_addr:port]` (defaults `127.0.0.1:17000`; use `0.0.0.0` only when LAN access is intended)
- Output: `-o m17udp[:host:port]` (default `127.0.0.1:17000`)
- Decode M17 UDP/IP input with `-fU`.

Do not feed `m17udp` into raw PCM tools such as `ffplay -f s16le`; use the `udp` output backend for decoded audio.

## Troubleshooting

- `tcp:localhost:7355` works but `tcp:<LAN-IP>:7355` fails: the PCM producer is usually listening only on loopback, the
  LAN IP is not the producer computer's IPv4 address, or the host firewall is blocking the port.
- Garbled audio usually means the **wrong sample format** (mono vs stereo, `s16le` vs `f32le`, or wrong sample rate).
- If you need a self-describing file format, prefer WAV output (`-w` / `-P`) rather than UDP/stdout.
