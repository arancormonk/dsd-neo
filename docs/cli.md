# DSD-neo CLI Guide

Friendly, practical overview of the `dsd-neo` command line. This covers what you’ll use day-to-day, grouped by task. For a quick reference, run `dsd-neo -h`.

## Cheatsheet

- Help: `dsd-neo -h` | UI/logs: `-N`, `-Z` | List devices: `-O`
- Inputs: `-i pulse | file.wav | rtl[:...] | rtltcp[:...] | tcp[:host:7355] | udp[:bind:7355] | m17udp[:bind:17000]`
- Outputs: `-o pulse | null | udp[:host:23456] | m17udp[:host:17000]`
- Record/Logs: `-6 file.wav`, `-w file.wav`, `-P`, `-7 ./calls`, `-d ./mbe`, `-J events.log`, `-L lrrp.log`, `-Q dsp.bin`, `-c symbols.bin`, `-r *.mbe`
- Levels/Audio: `-g 0|1..50`, `-n 0..100`, `-8`, `-V 1|2|3`, `-z 1|2`, `-y`, `-v 0xF`
- Modes: `-fa | -fs | -f1 | -f2 | -fd | -fx | -fy | -fz | -fU | -fi | -fn | -fp | -fh | -fH | -fe | -fE | -fm`
- Inversions/filtering: `-xx`, `-xr`, `-xd`, `-xz`, `-l`, `-u 3`, `-q`
- Trunking/scan: `-T`, `-Y`, `-C chan.csv`, `-G group.csv`, `-W`, `-E`, `-p`, `-e`, `-I 1234`, `-U 4532`, `-B 12000`, `-t 1`, `--enc-lockout|--enc-follow`
- RTL‑SDR strings: `-i rtl:dev:freq:gain:ppm:bw:sql:vol[:bias=on|off]` or `-i rtltcp:host:port:freq:gain:ppm:bw:sql:vol[:bias=on|off]`
- M17 encode: `-fZ -M M17:CAN:SRC:DST[:RATE[:VOX]]`, `-fP`, `-fB`
- Keys: `-b`, `-H '<hex...>'`, `-R`, `-1`, `-2`, `-! '<hex...>'`, `-@ '<hex...>'`, `-5 '<hex...>'`, `-9`, `-A`, `-S bits:hex`, `-k keys.csv`, `-K keys_hex.csv`, `-4`, `-0`, `-3`
- Tools: `--calc-lcn file`, `--calc-cc-freq 451.2375`, `--calc-cc-lcn 50`, `--calc-step 12500`, `--calc-start-lcn 1`, `--auto-ppm`, `--auto-ppm-snr 6`

## Quick Start

- Show help: `dsd-neo -h`
- PulseAudio in, play out, UI on: `dsd-neo -i pulse -o pulse -N`
- UDP audio in to PulseAudio out: `dsd-neo -i udp:0.0.0.0:7355 -o pulse -N`
- Follow DMR trunking (TCP IQ input + rigctl): `dsd-neo -fs -i tcp -U 4532 -T -C dmr_t3_chan.csv -G group.csv -N`
- Follow DMR trunking (RTL‑SDR): `dsd-neo -fs -i rtl:0:450M:26:-2:8 -T -C connect_plus_chan.csv -G group.csv -N`
- Play saved MBE files: `dsd-neo -r *.mbe`

## Inputs (`-i`)

- PulseAudio: `-i pulse` (default). List sources/sinks: `-O`.
- PulseAudio by name/index: `-i pulse:<index|name>` (use `-O` to discover values)
- OSS (legacy): `-i /dev/dsp` (deprecated; may require `padsp` on Linux)
- WAV file: `-i file.wav` (48 kHz mono). For other rates (e.g., DSDPlus 96 kHz): add `-s 96000`.
- OP25/FME capture BIN: `-i file.bin`.
- RTL‑SDR (USB): `-i rtl` or advanced string:
  - `rtl:dev:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]`
  - Examples: `rtl:0:851.375M:22:-2:24:0:2`, `rtl:00000001:450M:0:0:12`
- RTL‑TCP: `-i rtltcp[:host:port[:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]]]`
- Generic TCP IQ (SDR++/GRC): `-i tcp[:host:port]` (default port 7355)
- UDP PCM16 input: `-i udp[:bind_addr:port]` (defaults 127.0.0.1:7355)
- M17 UDP/IP input: `-i m17udp[:bind_addr:port]` (defaults 127.0.0.1:17000)

- Set WAV sample rate: `-s <rate>` (48k or 96k typical)

Tip: If paths or names contain spaces, wrap them in single quotes.

## Outputs (`-o`)

- PulseAudio: `-o pulse` or a specific sink like `-o pulse:alsa_output.pci-0000_0d_00.3.analog-stereo`
- Null (no audio): `-o null`
- UDP audio out (PCM16): `-o udp[:host:port]` (default 127.0.0.1:23456)
- M17 UDP/IP out: `-o m17udp[:host:port]` (default 127.0.0.1:17000)

## Display & UI

- `-N` Use the ncurses terminal UI
- `-Z` Log MBE/PDU payloads to the console (verbose)
- `-O` List PulseAudio input sources and output sinks
- `-j` P25: enable LCW explicit retune (format 0x44)
- `-^` P25: prefer CC candidates during control channel hunt

## Recording & Files

- `-6 <file>` Save raw audio WAV (48k/mono). Large files (≈360 MB/hour)
- `-w <file>` Save decoded audio to a single WAV
- `-P` Per‑call WAV saving (auto‑named files in a folder)
- `-7 <dir>` Set folder for per‑call WAVs (use before `-P`)
- `-r <files>` Play saved MBE files
- `-c <file>` Save symbol captures to a .bin file
- `-d <dir>` Save raw MBE vocoder frames in this folder
- `-J <file>` Append event log output
- `-L <file>` Append LRRP (location) data
- `-Q <file>` Write structured DSP or M17 stream data to `./DSP/<file>`
- `-q` Reverse mute: mute clear audio, unmute encrypted audio

## Levels & Audio

- `-g <num>` Digital output gain. `0` = auto; `1` ≈ 2%; `50` = 100%
- `-n <num>` Analog output gain (0–100%)
- `-8` Monitor the source audio (helpful when mixing analog/digital)
- `-V <1|2|3>` TDMA voice synthesis on slot 1, slot 2, or both (default 3)
- `-z <1|2>` TDMA slot preference for `/dev/dsp` output
- `-y` Use experimental float audio output
- `-a` Enable call alert beep (UI)

## Modes & Decoders (`-f`)

- Auto: `-fa`
- Passive analog monitor: `-fA`
- Trunking helper: `-ft` (P25p1 CC + P25p1/p2/DMR voice)
- DMR simplex (BS/MS): `-fs`
- P25 Phase 1 only: `-f1`
- P25 Phase 2 only (6000 sps): `-f2`
- D‑STAR: `-fd`
- X2‑TDMA: `-fx`
- YSF: `-fy`
- M17: `-fz` (radio); `-fU` (M17 UDP/IP frame)
- NXDN48: `-fi` (6.25 kHz)
- NXDN96: `-fn` (12.5 kHz)
- ProVoice: `-fp`
- EDACS/ProVoice: `-fh` (standard), `-fH` (with ESK 0xA0)
  - Custom AFS bit splits: `-fh344`, `-fH434`
- EDACS EA/ProVoice: `-fe` (standard), `-fE` (with ESK 0xA0)
- dPMR: `-fm`

Notes
- Some frame types cannot be auto‑detected (marked above).
- P25p2 on a single frequency may require `-X` (below) if MAC_SIGNAL is missing.

## Mode Tweaks & Advanced

- Inversions: `-xx` X2 non‑inverted, `-xr` DMR inverted, `-xd` dPMR inverted, `-xz` M17 inverted
- Disable DMR/dPMR/NXDN/M17 input filtering: `-l`
- Analog filter bitmap (advanced): `-v <hex>` (bitmask for HPF/LPF/PBF)
- Unvoiced speech quality: `-u <1–64>` (default 3)
- Modulation optimizations: `-ma` (auto), `-mc` (C4FM), `-mg` (GFSK), `-mq` (QPSK), `-m2` (P25p2 QPSK 6000 sps), `-mL` (CQPSK LMS, experimental)
- Relax CRC checks: `-F` (P25p2 MAC_SIGNAL, DMR RAS/CRC, M17 LSF/PKT)
- P25p2 manual WACN/SYSID/CC: `-X <hex>` (e.g., `-X BEE00ABC123`)
- DMR Tier III Location Area n‑bits: `-D <0–10>`

## Trunking & Scanning

- Enable trunking (NXDN/P25/EDACS/DMR): `-T`
- Conventional scan mode: `-Y` (not trunking; scans for sync on enabled decoders)
- Channel map CSV: `-C <file>` (e.g., `connect_plus_chan.csv`)
- Group list CSV (allow/block + labels): `-G <file>`
- Use group list as allow/whitelist: `-W`
- Tune controls: `-E` disable group calls, `-p` disable private calls, `-e` enable data calls, `--enc-lockout` do not tune encrypted P25 calls, `--enc-follow` allow encrypted (default)
- Hold talkgroup: `-I <dec>`
- rigctl over TCP: `-U <port>` (SDR++ default 4532)
- Set rigctl bandwidth (Hz): `-B <hertz>` (e.g., 7000–24000 by mode)
- Hang time after voice/sync loss (seconds): `-t <secs>`

## RTL‑SDR details (`-i rtl` / `-i rtltcp`)

- Fields: `dev` (index or 8‑digit serial), `freq` (Hz/MHz), `gain` (0–49), `ppm`, `bw` (kHz: 4, 6, 8, 12, 16, 24), `sql` (dB or linear), `vol` (1–3), optional `bias[=on|off]`.
- Examples:
  - `-i rtl:0:851.375M:22:-2:24:0:2`
  - `-i rtltcp:192.168.1.10:1234:851.375M:22:-2:24:0:2`

## M17 Encoding

- Stream encoder: `-fZ` with `-M M17:CAN:SRC:DST[:INPUT_RATE[:VOX]]`
- BERT encoder: `-fB`
- Packet encoder: `-fP`

M17 `-M` details
- `CAN` 1–15
- `SRC`/`DST` up to 9 UPPER base40 chars (` ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.`)
- `INPUT_RATE` default 48000; use multiples of 8000 up to 48000
- `VOX` enable with `1` (default `0`)

Examples
- `dsd-neo -fZ -M M17:9:DSD-neo:arancormonk -i pulse -6 m17signal.wav -8 -N`
- `dsd-neo -fP -M M17:9:DSD-neo:arancormonk -6 m17pkt.wav -8`

## Keys & Privacy (advanced)

- Basic Privacy key (decimal): `-b <dec>`
- Hytera 10/32/64‑char BP or AES‑128/256 key (hex, groups of 16): `-H '<hex…>'`
- dPMR/NXDN scrambler (decimal): `-R <dec>`
- RC4/DES key (hex): `-1 <hex>`
- TYT Basic Privacy (16‑bit, hex, enforced): `-2 <hex>`
- TYT Advanced Privacy PC4 (hex stream): `-! '<hex…>'`
- Retevis Advanced Privacy RC2 (hex stream): `-@ '<hex…>'`
- TYT Enhanced Privacy AES‑128 (hex stream): `-5 '<hex…>'`
- Kenwood 15‑bit scrambler (decimal): `-9 <dec>`
- Anytone 16‑bit BP (hex): `-A <hex>`
- Generic keystream (length:hexbytes): `-S <bits:hex>` (e.g., `-S 49:123456789ABC80`)
- Import keys CSV (decimal): `-k <file>`
- Import keys CSV (hex): `-K <file>`
- Force key over identifiers: `-4` (DMR BP/NXDN scrambler), `-0` (DMR RC4 when PI/LE missing)
- Disable DMR Late Entry IDs: `-3` (avoid false ENC)

## Tools & Extras

- DMR Tier III LCN calculator:
  - `--calc-lcn <file>` (CSV of freqs)
  - `--calc-cc-freq <freq>` and `--calc-cc-lcn <num>` (anchor)
  - `--calc-step <hz>` (override channel step)
  - `--calc-start-lcn <num>` (start when no anchor)
- RTL auto‑PPM drift correction:
  - `--auto-ppm` enable
  - `--auto-ppm-snr <dB>` set SNR gate (default 6)

## Handy Examples

- UDP in → Pulse out with UI: `dsd-neo -i udp -o pulse -N`
- RTL‑TCP in with ncurses UI: `dsd-neo -i rtltcp:127.0.0.1:1234 -N`
- Save per‑call WAVs to a folder: `dsd-neo -7 ./calls -P -N`
- Strictly P25 Phase 1 from TCP IQ: `dsd-neo -f1 -i tcp -N`

Tip: Many options can be mixed; start simple, add only what you need.
