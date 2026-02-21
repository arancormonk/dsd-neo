# Network Audio I/O (TCP/UDP/stdin/stdout)

This document describes the raw audio stream formats used by DSD-neo for network and pipe I/O. These interfaces are
intentionally simple: they are headerless streams/datagrams with no framing metadata.

If you just want “what flag do I type”, start with `docs/cli.md`.

## PCM Input (`-i tcp`, `-i udp`, `-i -`)

DSD-neo accepts raw PCM input in three equivalent ways:

- **TCP**: `-i tcp[:host:port]` (default `127.0.0.1:7355`)
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

## UDP Audio Output (`-o udp`)

`-o udp[:host:port]` sends decoded audio to a UDP “blaster” socket (default `127.0.0.1:23456`).

### Digital decoded voice (default UDP port)

The primary UDP output carries decoded digital voice:

- Sample rate: **8000 Hz**
- Channels:
  - Often **stereo** (2 channels, interleaved) by default
  - Use `-nm` (or `-fr` for DMR) to force **mono** in common DMR workflows
- Sample type:
  - Default: **`s16le`** (signed 16-bit little-endian)
  - With `-y`: **`f32le`** (32-bit float little-endian)

### Analog/source monitor (UDP port + 2)

If you enable source monitoring (`-8`) while using `-o udp`, DSD-neo also opens an **analog monitor** UDP socket on
`<port + 2>` (for example, `23458` when the base port is `23456`):

- Sample rate: **48000 Hz**
- Channels: **mono**
- Sample type: **`s16le`**

## Listen to UDP Output (Examples)

These examples use `socat` to receive UDP datagrams and feed a player that can consume raw PCM from stdin.

Digital voice (default port `23456`, 8 kHz):

```bash
# PCM16LE stereo (common default)
socat -u UDP-RECV:23456,reuseaddr STDOUT | ffplay -nodisp -f s16le -ar 8000 -ac 2 -i -

# PCM16LE mono (if you run DSD-neo with -nm / -fr)
socat -u UDP-RECV:23456,reuseaddr STDOUT | ffplay -nodisp -f s16le -ar 8000 -ac 1 -i -

# Float32 stereo (if you run DSD-neo with -y)
socat -u UDP-RECV:23456,reuseaddr STDOUT | ffplay -nodisp -f f32le -ar 8000 -ac 2 -i -
```

Analog/source monitor (port `23458`, 48 kHz mono):

```bash
socat -u UDP-RECV:23458,reuseaddr STDOUT | ffplay -nodisp -f s16le -ar 48000 -ac 1 -i -
```

## stdout Audio Output (`-o -`)

`-o -` writes the same raw decoded audio stream to stdout. The format matches the “Digital decoded voice” description
above (rate/channels/type depend on mode and `-y`).

This can be useful when you want to keep transport out of DSD-neo (pipe into another tool, or re-packetize yourself).

## Troubleshooting

- Garbled audio usually means the **wrong sample format** (mono vs stereo, `s16le` vs `f32le`, or wrong sample rate).
- If you need a self-describing file format, prefer WAV output (`-w` / `-P`) rather than UDP/stdout.
