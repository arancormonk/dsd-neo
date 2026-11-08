# DSD-neo CLI Guide

Friendly, practical overview of the `dsd-neo` command line. This covers what you’ll use day-to-day, grouped by task. For a quick reference, run `dsd-neo -h`.

## Cheatsheet

- Help: `dsd-neo -h` | UI/logs: `--frontend terminal` (`-N` alias), `-Z` | List devices: `-O`
- Inputs: `-i pulse | file.wav | rtl[:...] | rtltcp[:...] | soapy[:args[:freq[:gain[:ppm[:bw[:sql[:vol]]]]]]] | tcp[:host[:port]] | udp[:bind_addr[:port]] | m17udp[:bind_addr[:port]] | -`
- Outputs: `-o pulse | null | udp[:host[:port]] | m17udp[:host[:port]] | -`
- Record/Logs/Debug: `-6 file.wav`, `-w file.wav`, `-P`, `-7 ./calls`, `--wav-journal calls.jsonl`, `--record-unknown-only`, `--analog-record-max-gap-ms 2000`, `--analog-record-preroll-ms 200`, `-d ./mbe`, `-J events.log`, `--frame-log frames.log`, `--p25-sm-log p25-sm.log`, `-L lrrp.log`, `-Q dsp.bin`, `-c symbols.bin`, `-r *.mbe`, `--dmr-debug-burst`, `--dmr-debug-unsynced`
- IQ capture/replay: `--iq-capture <path>`, `--iq-capture-format cu8|cf32`, `--iq-capture-max-mb <n>`, `--iq-replay <path>`, `--iq-replay-rate fast|realtime`, `--iq-loop`, `--iq-info <path>`
- Levels/Audio: `-g 0|1..50`, `-n 0|1..100`, `-nm`, `-8`, `-V 0|1|2|3`, `-z 0|1|2`, `-y`, `-v 0x9`
- Modes: `-fa | -fs | -fr | -f1 | -f2 | -fd | -fx | -fy | -fz | -fU | -fi | -fn | -fp | -fh | -fH | -fe | -fE | -fm | -fA | -fM` (Auto, `-fa`, is the default)
- Analog: `-fA` (NFM monitor), `--nfm-bandwidth-hz <Hz>` (8000..25000, default 16000); `-fM` (native AM, radio/I/Q inputs only), `--am-bandwidth-hz <Hz>` (5000..20000, default 6000); `--tone-allow <list>` / `--tone-block <list>` / `--no-tone-filter` (CTCSS/DCS receive policy on the FM monitor, e.g. `67.0/100.0/D023N`); `--squelch auto[+N]` (open N dB over each analog channel's learned noise floor, radio inputs); `--squelch noise[+N]` (open an FM channel at N dB of quieting, as a radio's squelch does; on audio input from a reference it learns); see [Analog reception](#analog-reception--fa--fm), [Tone filter](#tone-filter-ctcssdcs-receive-policy), [Auto squelch](#auto-squelch---squelch-auto) and [Noise squelch](#noise-squelch---squelch-noise)
- Inversions/filtering: `-xx`, `-xr`, `-xd`, `-xz`, `-l`, `-q`
- Trunking/scan: `-T`, `-Y`, `--trunk-scan targets.csv` (repeatable; `--trunk-scan-off targets.csv` adds a list that starts off; `--trunk-scan-hold <target-id>` starts held on one target; P25/DMR/NXDN96/NXDN48 trunk and conventional targets and analog NFM and AM conventional targets; each type selects its decoder class), `-C chan.csv`, `-G group.csv`, `--src-csv src.csv`, `--p25-bandplan plan.csv`, `--p25-bandplan-export plan.csv`, `-W`, `-E`, `-p`, `-e`, `-I 1234`, `-U 4532`, `-B 12000`, `-t 1`, `--enc-lockout|--enc-follow`, `--tg-lockout-session|--tg-lockout-persist`, `--scan-voice-only`, `--scan-voice-qualify-ms <ms>`, `--scan-voice-hold-ms <ms>`, `--scan-max-visit-ms <ms>`
- RTL‑SDR strings: `-i rtl:dev:freq:gain:ppm:bw:sql:vol[:bias=on|off]` or `-i rtltcp:host:port:freq:gain:ppm:bw:sql:vol[:bias=on|off]`
- Soapy selection: `-i soapy`, `-i soapy:driver=airspy[,serial=...]`, or `-i soapy[:args]:freq[:gain[:ppm[:bw[:sql[:vol]]]]]` (discover args with `SoapySDRUtil --find`)
- RTL retune control: `--rtl-udp-control <port>` binds to loopback by default; use
  `--rtl-udp-control-bind <ipv4>` for explicit remote exposure (see `docs/udp-control.md`). It stays off under
  `--trunk-scan`, which owns the tuner.
- Control/telemetry API (desktop builds): `--api <port>` serves NDJSON control and telemetry on `127.0.0.1:<port>`
  (`0` disables); `--api-bind <ipv4>` moves it, and a non-loopback bind requires `--api-token <secret>` or
  `--api-token-file <path>` (one line of 1..255 bytes; a file that cannot be read is an error). See
  [docs/api.md](api.md).
- M17 encode: `-fZ -M M17:CAN:SRC:DST[:RATE[:VOX]]`, `-fP`, `-fB`
- Keys: `-b`, `-H '<hex...>'`, `-R`, `-1`, `-2`, `-! '<hex...>'`, `-@ '<hex...>'`, `-5 '<hex...>'`, `-9`, `-A`, `-S bits:hex[:offset[:step]]`, `-k keys.csv`, `-K keys_hex.csv`, `--dmr-baofeng-pc5 <hex>`, `--dmr-csi-ee72 <hex>`, `--dmr-vertex-ks-csv <file>`, `--dmr-tg-key-csv <file>`, `--dmr-force-algid <hex>`, `--tetra-key mcc,mnc,<hex>[,teaN]`, `--tetra-key mcc,mnc,<hex>,la=N,cc=N,carrier=N`, `--show-keys`, `-4`, `-0`, `-3`
- Tools: `--calc-lcn file`, `--calc-cc-freq 451.2375`, `--calc-cc-lcn 50`, `--calc-step 12500`, `--calc-start-lcn 1`, `--auto-ppm`, `--auto-ppm-snr 6`, `--rtltcp-autotune`, `--rdio-mode off|dirwatch|api|both`

## Quick Start

- Show help: `dsd-neo -h`
- PulseAudio in, play out, UI on: `dsd-neo -i pulse -o pulse --frontend terminal`
- UDP audio in to PulseAudio out: `dsd-neo -i udp:0.0.0.0:7355 -o pulse --frontend terminal`
- Follow DMR trunking (TCP PCM input + rigctl): `dsd-neo -fs -i tcp -U 4532 -T -C dmr_t3_chan.csv -G group.csv --frontend terminal`
- Follow DMR trunking (RTL‑SDR): `dsd-neo -fs -i rtl:0:450M:26:-2:48:0:2 -T -C connect_plus_chan.csv -G group.csv --frontend terminal`
- Follow DMR trunking (SoapySDR): `dsd-neo -fs -i soapy:driver=airspy -T -C connect_plus_chan.csv -G group.csv --frontend terminal`
- Scan several P25/DMR/NXDN targets with one tuner: `dsd-neo -fa -i rtl:0:851.0125M:22:0:48:0:2 --trunk-scan examples/trunk_scan_targets.csv -G examples/group.csv --frontend terminal` (each target selects its own decoder class, so the global preset only matters outside trunk scan)
- Capture RTL I/Q + metadata: `dsd-neo -i rtl:0:851.375M:22:0:48:0:2 --iq-capture p25-control.iq --frontend terminal`
- Inspect a capture: `dsd-neo --iq-info p25-control.iq.json`
- Replay a capture through demod: `dsd-neo --iq-replay p25-control.iq.json -f1 --frontend terminal`
- Replay with separate talkgroup and source radio names: `dsd-neo --iq-replay p25-control.iq.json -f1 -G examples/group.csv --src-csv examples/src.csv --frontend terminal`
- Play saved MBE files: `dsd-neo -r *.mbe`
- Decode MBE to a WAV (no speaker output): `dsd-neo -o null -w decoded.wav -r call.mbe`

Tip: If you run with no arguments and no config is loaded, `dsd-neo` starts the interactive setup (respects
`DSD_NEO_NO_BOOTSTRAP`). When a config file is enabled and loads successfully, a no-arg run reuses it; use
`--interactive-setup` to force the wizard.

## Configuration Files

- Config loading is opt-in: use `--config` to enable, optionally with a path (e.g. `--config /path/to/config.ini`).
- Convenience: `dsd-neo /path/to/config.ini` (single positional `*.ini`) is treated as `--config /path/to/config.ini`.
- Default path (when bare `--config` is passed and `DSD_NEO_CONFIG` is not set): `${XDG_CONFIG_HOME:-$HOME/.config}/dsd-neo/config.ini`.
- Alternatively, set `DSD_NEO_CONFIG=<path>` environment variable to enable config loading (this is the only way for a no-arg run to load a config).
- Config path precedence: explicit `--config /path/to/config.ini` or a positional `*.ini` > `DSD_NEO_CONFIG` > default path for bare `--config`.
- Explicit config paths may be absolute, relative, or use `~`/environment expansion; include paths are resolved relative to the containing config file.
- `--interactive-setup` runs the wizard even when a config exists.
- `--print-config` prints the effective config as INI after all env/CLI overrides. An `rtl:`/`rtltcp:` input spec's
  tuning (frequency, gain, PPM, DSP bandwidth, squelch, volume) is read into the `rtl_*` keys first, as the engine reads
  it when the input opens.
- In Soapy mode, shorthand `-i soapy[:args]:freq[:gain[:ppm[:bw[:sql[:vol]]]]]` is normalized first, so output shows
  `soapy_args` plus shared `rtl_*` tuning keys.
- When config is enabled, the final settings are autosaved on exit. Explicit `--profile NAME` runs disable autosave for
  that process. See `docs/config-system.md` for details.

## Inputs (`-i`)

- PulseAudio: `-i pulse` (default). List sources/sinks: `-O`.
- PulseAudio by name/index: `-i pulse:<index|name>` (use `-O` to discover values)
- WAV file: `-i file.wav` (48 kHz mono). For other rates (e.g., DSDPlus 96 kHz): add `-s 96000`.
  Persisted discriminator captures that were historically saved as headerless PCM16LE with a `.wav` suffix remain
  replayable at the configured rate. Remove that mislabeled-file fallback after those captures are migrated or their
  support window ends; explicitly raw stream and file inputs remain supported independently.
- OP25/FME capture BIN: `-i file.bin`.
- RTL‑SDR (USB): `-i rtl` or advanced string:
  - `rtl:dev:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]`
  - Examples: `rtl:0:851.375M:22:-2:24:0:2`, `rtl:1:450M:0:0:12:0:2`
  - `sql` is a power squelch in dB and is **off** when set to `0`, which is what the examples above use. The startup
    banner and the terminal input line say so (`SQ=off`, `SQL: off`). Give a negative value (`-60`) to gate on power,
    `auto` / `auto+N` for the [auto squelch](#auto-squelch---squelch-auto), which opens N dB over each analog
    channel's own noise floor whatever the receiver, or `noise` / `noise+N` for the
    [noise squelch](#noise-squelch---squelch-noise), which opens an FM channel at N dB of quieting.
    `--squelch <setting>` sets the same thing on any radio input and wins over the spec. A `-Y` channel-map row or
    `--trunk-scan` target can replace it while that row is on air with `--squelch-db <dB>` or `--squelch <setting>` in
    its `options` column (whole dB, `-100..0`, `0` = off, `auto[+N]` on analog rows, `noise[+N]` on nfm rows, omitted
    = inherit this value); see `docs/csv-formats.md`.
    An analog session makes calls (history rows, `-J` lines, recordings) only under a squelch above -110 dB, the
    startup default: see [Analog calls need a squelch](#analog-calls-need-a-squelch-on-a-radio-input).
- RTL‑TCP: `-i rtltcp[:host:port[:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]]]`
  Connects to an rtl_tcp server (`rtl_tcp` itself, or another server that speaks its protocol) for unsigned 8-bit
  I/Q, and tunes it over the connection. It is the only network input that carries I/Q: `tcp` and `udp` below carry
  demodulated PCM audio. For TETRA use `bw` 72 (`-fT -i rtltcp:host:port:freq:gain:ppm:72`).
- SoapySDR: `-i soapy[:args[:freq[:gain[:ppm[:bw[:sql[:vol]]]]]]]`
- TCP raw PCM16LE input (mono): `-i tcp[:host:port]` (bare `tcp` connects to `localhost:7355`; sample rate uses `-s`, default 48000)
  For a LAN host, use the IPv4 address of the computer running the TCP audio producer, and make sure that producer is
  listening on that LAN interface, not just on `localhost`.
- UDP PCM16 input: `-i udp[:bind_addr:port]` (defaults 127.0.0.1:7355)
- M17 UDP/IP frame input: `-i m17udp[:bind_addr:port]` (defaults `127.0.0.1:17000`; use `0.0.0.0` only when LAN access is intended; use with `-fU`)
- stdin (raw PCM16LE mono): `-i -` (sample rate uses `-s`)
- A TCP or stdin source that goes quiet keeps its input, in every mode. TCP audio stays connected however long its
  sender is silent (a squelched network audio source). Only a connection that closes or fails is lost: decoding then
  reconnects, while an EDACS analog call and the M17 stream encoder (`-fZ`) close the input and end the session, as
  before. A sender that goes away without closing the connection (its machine lost power, or the network dropped) counts
  as a failed connection about 35 seconds after its last data, when TCP keepalive gets no answer (longer on a system
  that does not let a program set the keepalive timing). While stdin or TCP sends nothing, the decoder still ends an
  analog call on time, applies menu commands and runs its timers, as on UDP. An EDACS analog call, on TCP or UDP, hears
  a quiet spell of half a second or more as silence and ends as it does on a quiet channel; a shorter stall is waited
  out. The M17 encoder waits for audio. Ctrl-C ends a silent stdin or TCP wait at once. A source that stops in the
  middle of a digital frame is waited for, as before. On Windows only a pipe (`rtl_fm ... | dsd-neo -i -`) is waited on
  this way: console stdin and a redirected file block in their read until data comes.

- Set sample rate: `-s <rate>` (WAV/TCP/UDP; 48k or 96k typical). A WAV file's header rate holds for that file only:
  an input switch away from it (Input > Switch source in the terminal UI) opens the next file, UDP or TCP input at the
  `-s` rate again (Pulse opens at its own capture rate, as at startup).
- The analog outputs always run at 48 kHz: the analog and source monitor (local audio, the UDP analog stream, and
  `-o -` in the analog monitor modes), the `-6` raw WAV and EDACS analog voice. Audio input at another rate is converted, so it plays at the input's speed.
  8, 9.6, 12, 16 and 24 kHz input reaches the decoder at 48 kHz already. Any other rate from 1 kHz to 768 kHz (44.1,
  22.05, 32, 96 kHz, ...) is converted on the way out, exactly for the usual rates and within 250 ppm for any other,
  while decoding runs at the input's own rate as before. At a rate outside that range the analog outputs stay silent
  and an error names the rate. A monitor block whose samples arrived at two rates (an input switch or config apply
  landing part-way through it) is left out of every analog output, `-6` included. A block that straddles a retune at
  one rate is not played either, but `-6`, which records the input as it arrives, keeps it.

TCP/UDP PCM input format notes

- Sample format is signed PCM16LE (little-endian), mono, headerless stream/datagrams.
- With `-i tcp:<host>:<port> -U <rigctl_port>`, rigctl connects to the same `<host>` as the TCP audio input. For SDR++
  on another PC, allow inbound TCP for both the audio port, commonly `7355`, and rigctl, commonly `4532`.
- See `docs/network-audio.md` for practical send/receive examples and UDP output details.

Other input options

- `--input-volume <1..16>` scale non‑RTL input samples (file/UDP/TCP) by an integer factor.
- `--input-level-warn-db <dB>` low input-level advisory threshold in dBFS (default −40). This only affects LOW
  advisories; DSD-neo never changes gain automatically. TCP PCM input keeps LOW/HOT/CLIP visible in the persistent
  input status, but suppresses repeated console level warnings while the decoder is idle/searching. Also persistable
  as `[input] input_warn_db` in the user config (autosaved on exit when config loading is enabled) and adjustable at
  runtime from the terminal menu.

Tip: If paths or names contain spaces, wrap them in single quotes.

## Outputs (`-o`)

- PulseAudio: `-o pulse` or a specific sink like `-o pulse:alsa_output.pci-0000_0d_00.3.analog-stereo`
- Null (no audio): `-o null`
- UDP audio out (raw PCM): `-o udp[:host:port]` (default 127.0.0.1:23456). See `docs/network-audio.md`.
- M17 UDP/IP frame out: `-o m17udp[:host:port]` (default 127.0.0.1:17000)
- stdout (raw decoded audio): `-o -` (see `docs/network-audio.md`). In the analog monitor modes (`-fA`, `-fM`) it
  carries the monitor's 48 kHz mono audio. A reader that goes away (`-o - | head`) or a write that fails costs stdout
  audio one logged error: decoding, recording (`-w`, `-P`) and trunk following carry on.
- A local sound device that fails mid-session (a PulseAudio or PipeWire restart, a device error) is reopened behind the
  scenes, with one line in the log when it fails and one when it plays again.

## Display & UI

- `--frontend terminal` Use the terminal UI (`-N` is the supported short alias)
- `--frontend native` remains accepted and maps to headless mode. The retired native provider was a non-rendering
  scaffold; this keeps existing invocations working without restoring its provider/threading layer.
- `-Z` Log MBE/PDU payloads to stderr (verbose), including encrypted P25 voice frames when media is gated, and an NXDN
  frame sync whose LICH then fails (`Sync: NXDN96 LICH ERR`), which otherwise prints nothing.
  With `--frontend terminal` / `-N`, redirect stderr to capture them:
  `dsd-neo -Z --frontend terminal 2> console_log.txt`. The terminal frontend suppresses stderr when it is a TTY
  to protect the screen, and warns before opening the UI if payload logging is enabled without a redirect.
  Already-redirected stderr is preserved; `--frontend none -Z` leaves console payload output visible.
- `--frame-log <file>` Append one-line timestamped frame traces (separate from event log)
- Under `-fT` the terminal frontend (`-N`) shows the TETRA working mode, network, call following,
  control and traffic channels, MM status and codec health in Call Info, as the Qt/Android monitor does.
  TETRA calls, short data and status messages go to the call and event history like every protocol's.
  Per-frame diagnostics remain on stderr and can be captured with `2> tetra.log`.
- `--p25-sm-log <file>` Append one-line P25 state-machine decision diagnostics (separate from stdout/stderr, event log, and frame log)
- `-O` List PulseAudio input sources and output sinks
- The terminal input section shows advisory `Input Level`/`RF Level` health when metrics are available. `LOW` uses
  `--input-level-warn-db`; `HOT` means peak at or above `-1.0 dBFS`; `CLIP` means at least `0.1%` clipped or near-rail
  samples. TCP PCM idle/search level advisories are status-only to avoid console spam. These advisories never adjust
  gain automatically. A radio input whose receiver has delivered no samples for 2 s reads `RF Level: No input`, and
  its last level warns about nothing.
- For RTL-family inputs, the optional DSP panel shows post-channel-filter `Squelch` power against the SQL threshold.
  This is separate from the raw receiver `RF Level` health line.
- UI hotkeys and menu navigation: `docs/ui-terminal.md`
- `-j` P25: force-enable LCW explicit retune (format `0x44`; enabled by default). With it off, an explicit update for
  a preempt talkgroup still preempts (see [Priority and preemption](csv-formats.md#priority-and-preemption))
- `-^` P25: prefer CC candidates during control channel hunt

### P25 Follower (Advanced)

- `--p25-vc-grace <s>` seconds after VC tune before eligible to return to CC
- `--p25-min-follow-dwell <s>` minimum follow dwell after first voice
- `--p25-grant-voice-timeout <s>` max seconds from grant to voice before returning
- `--p25-mac-hold <s>` keep MAC activity eligible for audio for this many seconds after the last MAC
- `--p25-cc-grace <s>` CC hunt grace window; delay hunting for a new control channel by this many seconds after loss,
  and at session start (see [Startup control-channel hunt](#startup-control-channel-hunt))
- `--p25-force-release-extra <s>` safety‑net extra seconds beyond hangtime
- `--p25-force-release-margin <s>` safety‑net hard margin seconds beyond extra
- `--p25-p1-err-hold-pct <pct>` P25p1 IMBE error percentage threshold to extend hang
- `--p25-p1-err-hold-sec <s>` additional seconds to hold when threshold exceeded

## DMR Burst Debugging

`--dmr-debug-burst` emits one console line to stderr for each synced DMR burst. Use it when
troubleshooting live DMR decode and you need bracketed post-demod payload bytes without enabling verbose `-Z`
payload logging or changing `-Q` OK-DMRlib structured output. `--dmr-debug-unsynced` complements it with a raw
dump of demodulated data while no sync has been achieved. Both flags compose and are deliberately CLI-only
(debug aids are not persisted to the config schema).

Example:

```sh
dsd-neo -fs -i rtl:0:450M:26:-2:48:0:2 --dmr-debug-burst --dmr-debug-unsynced
```

Output formats:

```text
Debug Demod +Sync slot=<1|2> type=0xNN: [AA][BB]...[CC]   # normal 144-dibit burst (33 bytes, no CACH)
Debug Demod +Sync RC: [AA][BB]...[CC]                     # standalone Reverse Channel burst (12 bytes)
Debug Demod -Sync: [AA][BB]...[CC]                        # unsynced demod chunk (36 bytes)
```

Notes:

- The dump is DMR-only. Normal synced bursts dump the 33-byte no-CACH payload range also used by `-Q` DMR
  output; voice bursts report `type=0x10`, data bursts use the decoded DMR data burst type.
- Standalone Reverse Channel (RC) bursts (ETSI TS 102 361-1 clause 6.4.1) are 96-bit/10 ms inbound bursts;
  their dump is the full burst in over-the-air order, so the RC sync `[77][D5][5F][7D][FD][77]` is visible at
  the centre as a polarity/alignment check. RC detection and command decode (e.g. `RC: Cease Transmission
  Request;`) are always on when DMR decoding is enabled; only the hex dump needs `--dmr-debug-burst`.
- `--dmr-debug-unsynced` prints non-overlapping 144-dibit (36-byte) chunks of raw demod output while hunting
  for sync. Chunk boundaries are arbitrary and symbol thresholds may still be uncalibrated, so the bytes are
  best-effort and not aligned to burst boundaries. Expect noise hex on an idle channel.
- The options write to stderr only. They do not imply `-Q`, `-Z`, symbol capture, or payload file logging.

Windows console runs:

- A no-argument run still starts the interactive setup when no config is loaded.
- For a console debug run, pass explicit CLI arguments, for example:
  `dsd-neo.exe -fs -i tcp --dmr-debug-burst`.
- To suppress the interactive setup in `cmd.exe`, run `set DSD_NEO_NO_BOOTSTRAP=1` before starting `dsd-neo.exe`.
- To suppress it in PowerShell, run `$env:DSD_NEO_NO_BOOTSTRAP = "1"` before starting `dsd-neo.exe`.
- To reuse a saved config, pass `--config "%APPDATA%\dsd-neo\config.ini"` or set
  `DSD_NEO_CONFIG=%APPDATA%\dsd-neo\config.ini` in `cmd.exe`.
- In PowerShell, the equivalent config environment variable is
  `$env:DSD_NEO_CONFIG = "$env:APPDATA\dsd-neo\config.ini"`.

## Recording & Files

- `-6 <file>` Save raw audio WAV (48k/mono), in every decode mode, with or without a sync (NXDN, dPMR and M17 noise
  is held back by those protocols' CRC confirmation, not left out of the recording): the input before the monitor's
  filters, converted to 48 kHz when it runs
  at another rate (see "The analog outputs" under Inputs). On radio input the analog monitor's audio is saved at the
  level a PCM input at the reference has: a 1 kHz tone at 3 kHz deviation, or AM at 50% modulation, peaks at -12 dBFS
  at the default RTL `vol` of 2, whatever `-n` is, so open-squelch FM noise can clip. Audio input, and the FSK
  discriminator output digital decoding reads on radio input, are saved as they are. Large files (≈360 MB/hour)
- `-w <file>` Save decoded audio to a single WAV (mutually exclusive with `-P`)
- `-P` Per‑call WAV saving (auto‑named files in a folder; mutually exclusive with `-w`). A finished call is
  published under a name no other recording holds: a name already taken draws another number, so an earlier
  recording is never replaced. That holds outright where the filesystem offers a no-replace rename or hard links;
  on one with neither, the name is claimed with an empty file first, which another program can see for a moment.
  A per-call WAV holds digital voice at the level and with the high-pass it plays with: the same file with or without
  `-y`, with `-o null`, muted, or under any preset (issue #653). The system, target and source in a file's name have
  the characters a file name cannot hold (`/`, as in an M17 or D-STAR callsign, `\`, `:`, `*`, `?`, `"`, `<`, `>`,
  `|`, control characters, a byte that is not part of valid UTF-8) and dots and spaces replaced with `_`; the call
  history and rdio-scanner metadata keep them as received. A recording whose write falls short (a full disk) ends there and is published with what it holds;
  one that does not close cleanly keeps its `TEMP_` name. A slot file that fails to open opens again at the slot's
  next call end. Each kind of failure (open, write, close, rename) is logged the first time in a run and counted after.
  Conventional NFM calls are recorded too, at 48 kHz, by default each starting a moment before the call was picked up
  (see [Where an NFM recording starts](#where-an-nfm-recording-starts---analog-record-preroll-ms)).
- Recordings (`-w`, `-P`, `-d`) follow each talkgroup's `record` policy: a lockout (`B`/`DE`), avoid, skip, an
  allow-list or Hold miss, or `record=off` keeps the call out of the per-call WAV, the static WAV and MBE capture,
  live and in SDRTrunk JSON playback. A matching Hold (`-I`) overrides a row's lockout or `record=off`, as it does
  for audio; avoids and skips still apply. The static WAV holds only what is heard: on a two-slot mix a record-blocked
  slot's channel is written silent while the other slot is still written.
- `--record-unknown-only` Record only talkgroups the group list (`-G`) does not know: a call an exact, range or
  digit-pattern row (not the `*` catch-all) matches is kept out of the per-call WAV, the static WAV and MBE capture as
  if the row said `record=off`, and so is a private call when either radio has an exact row. Learned radio-ID aliases
  (mode `D`) and `--src-csv` rows do not count. A matching Hold
  still records; avoids and skips still block; tuning and playback are unchanged. INI:
  `[recording] unknown_talkgroups_only`. A running session changes it through the API's `record_unknown_only_set`
  (`docs/api.md`).
- `-7 <dir>` Set folder for per‑call WAVs, before or after `-P`; the folder is created when decoding starts if it does
  not exist. A path longer than 511 bytes is refused.
- `--wav-journal <file>` Append one JSON line to `<file>` for each per-call WAV published (`publish`: its name, start,
  length, and the call history row it belongs to), each one deleted for holding no audio (`discard`) or kept under
  its `TEMP_` name (`error`), and `link`/`unlink` lines for a recording closed before its call's history row
  (a WAV stop mid-call, a conventional analog call's end) once that row commits or never will. A program that manages
  the recordings, such as the app's call recording store, reads it to tie files to calls. A journal that ends in a
  torn line gets a newline first. A record's `start` is when the call started, or when the recording opened if that was
  later (recording switched on mid-call); an analog recording's is its own first sample (an NFM one's pre-roll
  included). A recording received while a Signal search was listening to a detection names that detection (`search`),
  so the app can list a detection's recordings. A journal that cannot be opened or written is turned off for the rest
  of the run with one error in the log; decoding and recording go on. A run with a journal keeps every file it
  uploads: while the journal is set, `--rdio-api-delete-after-upload` (or the config's `rdio_api_delete_after_upload`)
  deletes nothing, with one warning in the log, because the journal's owner manages the recordings' retention. The
  option keeps its value, so a config saved from the run (the exit's autosave included) still has it. The record format
  is in `docs/code_map.md` ("Recording journal").
- `--analog-record-max-gap-ms <ms>` (or `--analog-record-max-gap-ms=<ms>`) Keep a conventional analog call in one
  recording, and one call history row, through a pause of up to `<ms>`. It works only with per-call recording on (`-P`);
  without it, calls end as before. When the signal comes back in time, the pause is saved as silence (its end as heard,
  with the pre-roll below), so the recording keeps time. A longer pause ends the call at its last audio, and the next
  transmission starts a new one. 200 or less turns it off, the default: a call then ends 200 ms after its carrier drops,
  as before. A value outside 0 to 10000 is refused. Digital calls are not affected. INI: `[recording]
  analog_max_gap_ms`. How pauses are counted, and the limits, are under [Pauses in an analog
  recording](#pauses-in-an-analog-recording---analog-record-max-gap-ms).
- `--analog-record-preroll-ms <ms>` (or `--analog-record-preroll-ms=<ms>`) Start each new conventional NFM recording
  with up to `<ms>` of the audio heard just before the call, so its first word is not cut off. It works only with
  per-call recording on (`-P`). Only audio the tone filter allows is kept: audio it is still checking or has rejected
  never is. In a recording that resumes after a pause (`--analog-record-max-gap-ms`), the last `<ms>` of the pause is
  saved as heard rather than as silence, the same way. 200 is the default; 0 turns it off, and recordings then start as
  before. A value outside 0 to 1000 is refused. Setting it in a running session (the API, the terminal menu, the app)
  drops the audio held so far, even when the value is the same; a config applied in a running session drops it only when
  it changes the value. INI: `[recording] analog_preroll_ms`. What it holds, and the limits, are under [Where an NFM
  recording starts](#where-an-nfm-recording-starts---analog-record-preroll-ms).
- `--rdio-mode <off|dirwatch|api|both>` Enable rdio-scanner export from finalized per-call WAV calls, digital calls and
  conventional analog calls ([Analog calls in rdio-scanner](#analog-calls-in-rdio-scanner))
- `--rdio-system-id <N>` Set rdio-scanner system ID (required for API upload mode)
- `--rdio-analog-system-id <N>` rdio-scanner system ID for conventional NFM calls, 0 to 65535. 0, the default, uses
  `--rdio-system-id`. With it set, API uploads of NFM calls work even when `--rdio-system-id` is not set. INI:
  `[recording] rdio_analog_system_id`.
- `--rdio-analog-talkgroup <N>` rdio-scanner talkgroup for NFM calls heard with no known frequency (an audio input),
  0 to 2147483647. 0, the default, leaves those calls unexported. A call with a frequency always uses the frequency in
  kHz. INI: `[recording] rdio_analog_talkgroup`. Both values are refused outside their range, never clamped.
- `--rdio-api-url <url>` Set rdio-scanner API base URL (default `http://127.0.0.1:3000`)
- `--rdio-api-key <key>` Set API key for `/api/trunk-recorder-call-upload`
- `--rdio-upload-timeout-ms <ms>` API timeout per call (default 5000 ms)
- `--rdio-upload-retries <n>` API upload attempts per call (default 1)
- `--rdio-api-delete-after-upload` Delete the per-call WAV after a successful API-only upload; files are kept while
  `--wav-journal` is set
- A call whose talkgroup or radio policy row has `stream=off` is recorded but not exported (no sidecar, no upload);
  see `docs/csv-formats.md`. An NFM call has no talkgroup or radio, so no row's `stream` applies to it.
- `-r <files>` Play saved MBE files
  SDRTrunk JSON exports support P25, DMR, and NXDN enhanced-half-rate voice. NXDN playback accepts clear voice,
  the 15-bit scrambler, DES-OFB, and AES-256-OFB; use `-R`, `-1`, or `-H`, respectively, or a key CSV indexed by
  the recorded key ID. For an encrypted NXDN scrambler export without algorithm metadata, use `-4 -R <key>`.
  DES/AES require a recorded 64-bit `encryption_mi`; an absent IV is not guessed. SACCH tags align the scrambler
  to each 16-frame superframe and DES/AES to their 32-frame sessions. Exports that omit the positions of
  FACCH-stolen voice within an RF frame cannot fully reconstruct that frame's keystream alignment.
  NXDN full-rate voice is not supported by this reader.
  DMR AES-128/AES-256 exports use ALGID `0x24`/`0x25` and a 32-bit MI. For metadata-free DMR exports, use
  `-H '<key>' --dmr-force-algid 24` (or `25`); audio remains muted until a Golay/CRC-verified late-entry MI
  supplies the next superframe's context. For example:
  `dsd-neo -o null -w decoded.wav -H '<key>' --dmr-force-algid 25 -r call.mbe`.
  These are known-key playback modes, not key recovery.
- `-c <file>` Save symbol captures to a .bin file
- `--symbol-capture-format <soft|legacy>` Select the current soft/v2 writer. `legacy` remains accepted as an alias; it
  does not reactivate the removed one-byte writer.
  The current writer emits `DSDNSYM2` version 2 records with soft-decision metrics. The reader also accepts historical
  headerless dibit captures so persisted files remain replayable; a file carrying `DSDNSYM2` magic with an unsupported
  version or record size is rejected rather than reinterpreted as headerless data. Remove the headerless reader after
  those captures are migrated or their support window ends. Neither stored format carries the NXDN symbol rate, so NXDN
  capture replay requires `-fi` or `-fn` instead of `-fa`.
- `-d <dir>` Save raw MBE vocoder frames in this folder, with or without a trailing separator (created if missing;
  a path longer than 1023 bytes is refused). A capture file is created by the first frame written to it, so a call
  none of whose frames may be recorded leaves no file. Each file holds one protocol's frames in one record format,
  named by its 4-byte header and extension: `.imb` (IMBE: P25 Phase 1, ProVoice, YSF full rate), `.amb` (AMBE 2450:
  DMR, P25 Phase 2, NXDN, dPMR, X2-TDMA, YSF V/D1 and V/D2) or `.dmb` (D-STAR AMBE 2400); `-r` plays them back.
  Each record starts with an error byte. An `.amb` record dsd-neo writes packs the frame's C0 and total error counts
  (high bit set, C0 in bits 6-4, the total in bits 3-0), so `-r` repeats and mutes the frames the live decode did; a
  byte without the high bit (older captures, other tools) is a total alone and replays as before. `.imb` and `.dmb`
  records hold the total.
  Files are capture segments: a frame of another protocol or format, a call end the protocol signals, a lost
  carrier, a retune and a trunk-scan switch each end the open capture, and the next frame starts another. Two-slot
  protocols write one file per slot (`_S1`, `_S2`); X2-TDMA writes both slots into one. Each frame is saved by its
  own verdict, not the previous frame's, the one that decides its audio and its per-call WAV: encryption judged by
  the protocol that owns the call (DMR, P25 Phase 1 and Phase 2, NXDN, dPMR), the encrypted-audio mute and reverse
  mute (`-q`), plus talkgroup policy; X2-TDMA, D-STAR, ProVoice and YSF (all three voice modes) classify no
  encryption, so policy alone decides theirs; and for SDRTrunk JSON playback, slot 1's policy once playback's own
  decode and crypto checks pass. YSF frames are captured once a FICH has verified the transmission on the carrier.
- `-J <file>` Append event log output. Every line starts with a `YYYY-MM-DD HH:MM:SS` stamp, so the file
  filters, sorts and splits by time with ordinary line tools. An event's detail lines (`Text:` for a decoded
  text message, `Talker Alias:`, `GPS:`, `DSD-neo:` for a decoder notice) follow its line and repeat that
  event's own stamp rather than reading the clock again; a voice transmission reacquired after a sync loss
  appends a `Reacquired:` continuation carrying the stamp of the row it extends. While scanning a `-Y` list
  or rotating `--trunk-scan` targets, each line names the channel it was heard on in brackets between the
  timestamp and the protocol token. A [service name](#receive-filters-and-service-names) that labels the traffic
  takes that place, scanning or not; other lines from a receiver that is not scanning a named channel are unchanged.
  A DMR, P25 Phase 2 or X2-TDMA line ends with the over-the-air slot, `Slot 1;` or `Slot 2;`. The single-slot DMR
  decoder (`-fr`, `-nm`) carries both slots on one lane and logs the slot its bursts' CACH proved (`Slot 1;` while
  none is proven). In the terminal, `\` cycles Slot 1 / Slot 2 / Slots 1+2; the merged view tags DMR base-station,
  P25 Phase 2 and X2-TDMA rows `[S1]`/`[S2]` by decoder lane (the single-slot decoder's rows are all `[S1]`), while
  FDMA rows carry no slot tag. An analog line names its mode after the `ANALOG` token, `NFM` or `AM` as dsd-neo
  demodulated it, and no mode on audio input, where another receiver did. It names the tuned frequency as digital lines
  do, `FREQ: 154.430000 MHz;`, and leaves it out when there is none, as on audio input (earlier versions wrote
  `Frequency: 154430000 Hz;`, and `Frequency: 0 Hz;` on audio input). On audio input an analog line names the
  frequency a `-Y` row (stepped to by the scan or by `C`/`J`) or a conventional `--trunk-scan` target confirmed it
  tuned its rigctl peer to; digital lines on audio input still name none, and neither a frequency changed on the peer
  itself nor one tuned with `=` is named. Last comes the CTCSS tone or DCS code the call was received with, as Call
  Info's `Rx tone:` line writes it:
  `ANALOG NFM; FREQ: 154.430000 MHz; CTCSS 100.0 Hz;`,
  `ANALOG NFM; FREQ: 154.430000 MHz; DCS D023N / D047I;`, `ANALOG AM; FREQ: 119.100000 MHz;`, and on audio input
  `ANALOG; CTCSS 100.0 Hz;`. A call with no tone, or whose tone was never confirmed, names none; a tone lost just
  before the end (a reverse burst) still shows. Earlier versions wrote `NFM` on every analog line, audio input included,
  and no tone. A reception shorter than 500 ms makes no analog line (see `-P` below for how it is counted), and on a
  radio input neither does one heard while the squelch is off or at -110 dB or below (see
  [Analog calls need a squelch](#analog-calls-need-a-squelch-on-a-radio-input)). An analog
  line is stamped with the time its call ended, even when it is written later (an input that
  stopped sending held the end back); earlier versions stamped it when it was written. The control API's `event` text
  is the same row.
  A call line names the system and site the call was heard on, and a DMR trunked call's own channel by its numbering
  (`LSN:`, `LPCN:`, `LCN:`), from the parts the carrier sent: see [Event History Rows](ui-terminal.md#event-history-rows)
  for the tokens per protocol. A part never received is left out, never printed as 0.
  DMR and P25 Phase 1 data accepted with a failed header, packet, or confirmed-block CRC under `-F`
  carry `[CRC ERR]` after the timestamp, on both the event and its detail lines. History exposes these
  as warnings. Strict CRC mode does not publish these failed data PDUs, including MNIS, UDT, USBD and
  encrypted-PDU notices. DMR's separate relaxed-CRC configuration follows the same marking policy.
  DMR content that `-F` accepted only as Motorola RAS (Restricted Access to System; the burst line shows
  `-RAS`) carries `[RAS]` instead: RAS systems alter the standard CRC on purpose, so it could not be
  verified, but nothing is known to have failed, and the row keeps its ordinary severity and colour. This
  covers voice calls (including a call reached through a RAS-accepted grant), data headers, confirmed
  blocks, MBC/UDT and USBD. A burst counts as RAS only when its BPTC block decodes to a valid codeword
  within 4 bit errors of what was received, the codeword's reserved bits are set (RAS systems in the field
  send the value 4; any nonzero value counts), and the codeword's standard check fails: the CRC, or the
  RS(12,9) parity of a voice LC header or terminator. The payload decoded is that codeword's. Eligible
  bursts are the CSBK, MBC header, data header, voice LC header and terminator, USBD, confirmed rate-1/2
  data block, and the PI header with Motorola's FID 0x10 only; byte 1 0x68 (Hytera) excludes a CSBK, MBC
  header, voice LC header or terminator and a proprietary (DPF 15) data header, where byte 1 is a FID or
  MFID. A RAS burst received with more than 4 bit errors, or one of the rare few-error patterns the BPTC
  decoder carries further than 4, is not accepted, as no check can vouch for it; it shows `(CRC ERR)`.
  Without `-F` RAS content is never accepted, and a voice LC header or terminator with its reserved bits
  set is never RS-corrected, so a RAS LC cannot be "corrected" into a different, valid one. RAS is still
  detected without `-F`: two bursts with that signature on the same carrier confirm that it uses RAS. The
  terminal's DMR header then shows `RAS;` after the colour code (`RAS (-F decodes);` while Relaxed CRC is off),
  the carrier's system identity (the spectrum legend, the Qt/Android site line) shows `RAS`, the Qt/Android site
  sheet shows **Access RAS** with the same explanation and a **Turn on Relaxed CRC** button, and while Relaxed CRC
  is off the console prints once per confirmation
  `DMR RAS (Restricted Access to System) detected: control and data fail the standard CRC; -F decodes them`.
  Under a scan row's own `--strict-crc` with `-F` on, the header shows `RAS (channel sets strict CRC);` and the
  notice adds `, but this channel sets strict CRC itself`: `-F` already applies to the channels without their own.
  The detection belongs to the carrier: a retune (a trunk follow or control channel return included), a trunk-scan
  target switch, another colour code on the carrier, the DMR reset and the gap between transmissions clear it, and
  the next carrier proves its own. It notes a property of the bursts, never anything they decode. A burst
  whose reserved bits read set and whose check failed is decoded from the nearest codeword when that lies
  within 4 bits; if that codeword's own check passes, the burst is an ordinary checked one, with or without
  `-F`. A transmission that also fails a check renders `[CRC ERR]`, which outranks `[RAS]`; both markers
  stay on a voice row through delayed commits and reacquisition. `[CRC ERR]` means a CRC check failed, not
  that every decoded field is wrong. Absence of either marker is not a universal verification guarantee for
  every protocol. Each `-J` line carries the marker its row had when the
  line was written. A failure that reaches an already committed row through late detail (an alias or
  position decoded after the row committed) updates history and the API feed but appends nothing to
  `-J`; a reacquired transmission that merges into the row appends its `Reacquired:` continuation with
  the merged row's marker. A DMR embedded talker alias or GPS LC whose checksum failed is never applied.
  A `-L` location file never receives a position decoded from a PDU that would carry either marker
  (see `-L`).

  ```text
  2026-04-30 09:12:04 [Fire Dispatch] P25p1 TGT: 00050061; SRC: 00001234;
  2026-04-30 09:12:04 Talker Alias: ENGINE 12
  2026-04-30 09:12:11 TMS SRC: 1234; DST: 42; Slot 1;
  2026-04-30 09:12:11 Text: MEET AT THE NORTH GATE
  2026-04-30 09:12:15 [CRC ERR] MNIS TGT: 42; SRC: 1234; IP ID: 5501;
  2026-04-30 09:12:18 [RAS] DMR TGT: 00000200; SRC: 00004321; Slot 2;
  2026-04-30 09:12:20 DMR TGT: 00000100; SRC: 00000000; Slot 1;
  2026-04-30 09:12:20 Reacquired: DMR TGT: 00000100; SRC: 00004321; Slot 1;
  ```
- `--frame-log <file>` Append frame-level one-line timestamped traces
- `--p25-sm-log <file>` Append P25 state-machine health and frequency-decision traces. Grant traces identify initial
  assignments versus updates; post-call stale-update handling reports guard, validation-probe, and activity outcomes.

Per-call WAVs are written per *segment*, not per history row. When sync is lost mid-transmission and the same call is
reacquired within the reacquisition window, the Activity history keeps one row for the whole transmission while each
segment still closes, names, and exports its own recording. A flapping call therefore appears as one history row
referencing several recordings and several rdio uploads. Empty segments (44-byte WAVs) are deleted rather than exported,
so brief flaps cost nothing.

Each rdio-scanner sidecar's `start_time` is when its segment began, or when the WAV was opened if per-call WAV was
switched on partway through the call. `stop_time` is that plus the recording's length in whole seconds, and the API
upload sends the same values. The WAV file name keeps the time of the segment's last activity.

For rdio-scanner API uploads that should not persist on disk, use API-only mode with a RAM-backed per-call WAV directory
and post-upload deletion, for example `-7 /dev/shm/dsd-neo-rdio -P --rdio-mode api --rdio-api-delete-after-upload`.
Rdio API uploads do not follow HTTP redirects; use the final trusted HTTP/HTTPS endpoint directly.
DirWatch modes keep the WAV and JSON files because the watcher needs stable files to ingest.
At shutdown the CLI waits for queued API uploads to finish, whether the input ended or you stopped it (Ctrl+C, `q` in
the terminal UI, the control API's `quit`); each upload can take up to `--rdio-upload-retries` times
`--rdio-upload-timeout-ms`. In the Android app, any stop or restart the user starts does not wait: Stop listening (in
the app or the notification), switching systems, retrying after a failure, leaving Signal search or its Stop, Back or
closing the window with background listening off, and "Dismiss failure and choose a site". The upload in flight is
abandoned and the uploads still queued are not sent, one log line gives the count, and their WAV and JSON files stay on
disk. A session that ends on its own there (end of a replay, input closed, receiver unplugged) still waits as the CLI
does.
- `-L <file>` Append LRRP (location) data. Rows are gated by the dispatch CRC verdict for DMR data
  PDUs (LRRP, LOCN, LIP, UDT NMEA), DMR voice-LC embedded GPS, and P25 Phase 1 data PDUs reaching
  the IP/UDP LRRP ports; P25 Harris/APX voice-LC GPS and NXDN are unchanged. Under `-F`, CRC-failed
  data still decodes on screen and reaches history / the `-J` log marked `[CRC ERR]`, with LRRP
  position tokens shown as suppressed (`CRC ERR`) and no location row appended. Bursts accepted as
  RAS under `-F` are also excluded because their CRC could not be verified; the burst line
  shows `-RAS`, history marks them `[RAS]`, and a withheld LRRP position reads `suppressed (RAS)`. An
  embedded GPS row names the source of the call it rode on, so under `-F` a checked position can carry
  the source ID of a RAS-accepted voice header.
- `--lrrp-extra-port <n>` Also decode UDP port `<n>` as LRRP. Repeatable, at most 8 ports.
  The registered location port 4001 is always decoded; this adds ports a system uses
  instead of it, which would otherwise be reported as `Unknown UDP Port`. Ports the decoder
  already recognises (4005 ARS, 4007 TMS, and so on) keep their own service; the mapping
  only applies to ports that would otherwise be unknown. It covers compressed UDP/IPv4 data
  (SAP 3) as well as IPv4 datagrams. Config key: `mode.dmr_lrrp_ports`
  (comma-separated); a CLI list replaces the config list.
- `--dmr-udp-comp-port <idx>=<port>` Decode DMR compressed-UDP port index `<idx>` (95-127) as UDP port `<port>`, e.g.
  `--dmr-udp-comp-port 97=4005`. Repeatable, one pair per option; the `--dmr-udp-comp-port=<idx>=<port>` form also
  works. A compressed UDP/IPv4 header (ETSI TS 102 361-3) carries a 7-bit port index instead of the port. ETSI defines
  indices 0-2 and reserves 3-94, but leaves 95-127 to the manufacturer, and the radio's codeplug links them to UDP
  ports, so nothing on the air names the port. A mapped index decodes through the same UDP port services as
  uncompressed IP (4005 ARS, 4007 TMS, 4001 LRRP, ...), with `--lrrp-extra-port` ports decoded as LRRP too. Without a
  mapping, only ARS is recognised, and only when the payload is exactly one ARS message. An index outside 95-127, a
  port outside 1-65535, or a second, different port for an index is an error; repeating the same pair is not. Config
  key: `mode.dmr_udp_comp_ports` (comma-separated, e.g. `"97=4005,98=4007"`); the first CLI entry replaces the config
  list.
- `-Q <file>` Write structured DSP or M17 stream data to `./DSP/<file>`
- `-q` Reverse mute: mute clear audio, play encrypted audio, whether or not a loaded key decrypts it. It applies to
  the protocols whose encryption the decoder classifies: DMR, P25 (encrypted P25 without a key plays only while
  encrypted calls are followed), NXDN and dPMR, in both output formats. YSF, D-STAR, X2-TDMA and ProVoice are not
  affected. SDRTrunk JSON playback ignores it. WAV recordings and MBE capture (`-d`) follow the same verdict.
  Keystream exhaustion keeps a frame muted even under reverse mute.

## IQ Capture And Replay

- `--iq-capture <path>` Capture raw I/Q plus metadata sidecar. When `<path>` has no extension, `.iq` is added, so
  `--iq-capture mycap` writes `mycap.iq` and `mycap.iq.json`.
- `--iq-capture-format <cu8|cf32>` Capture format request (`cu8` default).
- `--iq-capture-max-mb <n>` Capture byte cap in MiB (`0` unlimited).
- `--iq-replay <path>` Replay capture metadata/data, or a two-channel WAV I/Q recording from other SDR software (its
  header gives the format and rate; no sidecar), through the RTL pipeline.
- `--iq-replay-rate <fast|realtime>` Replay pacing mode (`fast` default). In both modes the decoder paces the front end:
  the demodulator starts on the next capture chunk (one live transfer at most, 5.33 ms of capture) only once the decoder
  has read everything before it, so `fast` no longer demodulates ahead of the decoder (up to about 43 s at 48 kHz in
  older builds). What the decoder asks of the front end mid-replay (the Auto hunt narrowing the channel filter onto a
  candidate, a reacquire) lands at the start of the next capture chunk, fast or realtime. The decode clock follows the
  capture: timestamps show capture time (from the sidecar's `capture_started_utc`), and call gaps, call durations and
  protocol windows measure air time. So a replay without `-T` or `-Y` decodes the same way on every run, in either mode.
  `realtime` also holds the reader to the capture's own sample clock; `fast` runs as fast as decoding allows (see
  `docs/iq-capture-replay.md`).
- `--iq-loop` Loop replay when EOF is reached.
- `--iq-info <path>` Print capture metadata summary and exit (for a WAV recording: its encoding, rate, centre and start
  time with where each came from, and the rate chain it would replay at).

Notes

- Replay and capture are mutually exclusive in one invocation.
- Every stream start opens the capture anew and writes over the file, so a restart (a DSP bandwidth, an input switch, a
  loaded config that reopens the device) starts a new recording. A DSP bandwidth, an Input > Switch source, a loaded
  config that reopens the device or an Airspy setting that reopens it, whose start fails, puts back the input that ran
  and starts it again without the capture instead, so that restart never writes over the file: the capture stays off
  for the rest of the session, and the log says so (the message too when it fits, `...; I/Q capture stopped`). A
  switch from Pulse or another PCM input, whose radio stream kept running behind it and recording, stops that stream
  and puts only the PCM input back; the capture stops for the session the same way, so no later radio start writes
  over what that stream recorded.
  The recording made so far is kept when the failed start stopped before it opened the capture file (a width it
  refused, a device that did not open, a capture file it could not open); a start that failed after opening it (its
  workers, an Airspy that did not stream), or one a config undid because its stream could not run the scan row on air,
  had already written the file anew, and the log says that instead.
- `--iq-replay` and `--iq-info` accept either the data file or the `.json` metadata path, or a WAV recording. A file
  with a RIFF, RF64 or BW64 WAVE header replays as a WAV whatever its extension; a sidecar beside it, or one whose
  `data_file` names it, is ignored with a warning.
- A WAV recording's centre comes from its `auxi` chunk or a frequency token in its file name (`..._426700000Hz.wav`),
  and its start time from the `auxi` chunk or SDR#'s `YYYYMMDD_HHMMSSZ` name tokens. With no centre the replay still
  runs; with no start time decode times start at 2000-01-01T00:00:00Z. The channel must sit at the recording's centre:
  a replay cannot be retuned.
- Every replay plans its rate chain for the decode mode: a capture whose rate cannot reach the mode's DSP rate through
  the half-band decimators is resampled first (a 48,254 Hz recording to 72 kHz under `-fT`), and a sidecar capture with
  no DSP bandwidth asked for keeps the chain it recorded where the mode can run it. A DSP bandwidth asked for (the DSP
  bandwidth row, `RTL_SET_BW`, a config `[input]` `rtl_bw_khz`) sets the replay's DSP rate; an `-i` spec's does not
  reach it, since `--iq-replay` replaces the input. A capture no chain reaches (below 8 kHz, say) is refused at start.
- A DSP bandwidth change, a decode mode switch that needs another rate (TETRA's), or a loaded config whose `[mode]` or
  `rtl_bw_khz` does, re-plans a running replay and reopens it where it stopped: the replay resumes about a chunk
  later (a few ms of capture are skipped), and the decode clock stays on the capture. A change no chain can run is
  refused and the replay keeps running. An analog width is held to the rate the new chain runs at, and a config's
  gain reopens nothing (a replay plays its capture's gain).
- A capture that cannot be read to its end (an I/O error partway, or a data file cut short while it replays) ends the
  replay after the samples already read, as its end would, and `dsd-neo` exits with status 1.
- Retuned captures with v2 replay event timelines can be replayed. Older retuned captures without an event timeline are
  reported by `--iq-info` and rejected by `--iq-replay`.
- `--iq-replay` refuses a capture replay cannot convert (`cf32` stamped with a stage other than
  `post_driver_cf32_pre_ring`, or `cs16` with one other than `post_mute_pre_widen`), naming its format and stage, and
  `dsd-neo` exits with status 1.
- `-i iqreplay:...` is intentionally not a supported public input form; use `--iq-replay`.
- Raw I/Q over the network comes only from an rtl_tcp server (`-i rtltcp:...`); `-i tcp` and `-i udp` carry PCM audio.
  See "Raw I/Q Over The Network" in `docs/iq-capture-replay.md`.
- More details, the rate plan's rules and format notes: `docs/iq-capture-replay.md`.

## Levels & Audio

- `-g <num>` Digital output gain, the same in integer and `-y` float output and in the WAVs. `0` = auto: a peak
  normalizer to about -0.8 dBFS over the last 0.5 s, at most x50, cutting at once on a louder peak and rising by at
  most 5% per 20 ms, its level carried from call to call. `1`–`50` is a fixed multiplier on the vocoder's reference
  level (`1` ≈ 2% of the range, `50` = 100%), saturating at full scale. Since issue #653 `-g N` under `-y` is this
  fixed multiplier too, where it used to scale a float auto gain that kept adapting.
- `-n <num>` Analog output gain. `0` (the default; anything under `1` too) is auto: an AGC brings analog voice to the
  level digital voice plays at, FM and AM alike, never above -10 dBFS peak and at most 18 dB above the reference gain.
  `1`–`100` is a fixed gain, `50` the reference: there a 1 kHz tone at 3 kHz deviation, or AM at 50% modulation, plays at
  -12 dBFS peak (-15 dBFS RMS) at the default RTL `vol` of 2, live and in I/Q replay alike; `100` is 6 dB louder. The
  menu's Analog gain... prompt and the `*` / `/` keys work over the same range; from auto they step from `50`, and
  stepping below `1` goes back to auto. Since issue #518 a fixed `-n` plays RTL monitor audio about 9 dB louder than
  before, and up to 18 dB more where the old default filter (see `-v`) cut it.
- `-nm` Enable the DMR single-slot mono decoder without changing the active decode preset.
- `-z <0|1|2>` TDMA slot preference (0 = slot 1, 1 = slot 2, 2 = auto). When both slots carry audible voice, the
  preferred slot plays alone, in both ears (with one-channel output, alone); auto plays both. It applies to DMR and P25
  Phase 2, in integer and `-y` float output, and stays as set: decoding never changes it. A talkgroup hold prefers the
  held slot.
- `-8` Monitor the source audio (helpful when mixing analog/digital). It plays on a sound device or UDP (port + 2);
  with `-o -` while decoding digital modes it is not written to stdout, which carries the decoded voice, and a warning
  says so at startup.
- `-V <0|1|2|3>` TDMA voice synthesis (0 = off; 1 = slot 1; 2 = slot 2; 3 = both; default 3). A switched-off slot
  is not played on any audio output nor written to the static `-w` WAV, a call held with a talkgroup hold included,
  and stays off until you switch it on: neither a hold nor a return to a P25 control channel turns it back on.
  The switches apply to X2-TDMA too. Per-call WAVs (`-P`) and MBE capture (`-d`) have their own gates and do not
  follow the switches.
- `-y` Write digital voice to stdout (`-o -`) and UDP as 32-bit float samples (`f32le`) instead of 16-bit integers.
  The audio is the same as without `-y`: the same gain (`-g`), digital high-pass (`-v` 0x8), routing (a single call in
  both ears, and at full level on one channel) and muting, and `-w` and `-P` record the same WAVs. Each float sample is
  the 16-bit sample / 32768, so none is past full scale. A sound device plays 16-bit samples either way.
  M17 Codec2 voice follows this path too. EDACS analog voice is not written to stdout under `-y`.
- P25 Phase 2 voice plays as each timeslot pair completes (every 60 ms of air time), in integer and `-y` float output
  alike: both slots side by side, every frame once, 18 frames per slot per 360 ms superframe. A slot whose call starts
  or ends mid-superframe is heard from its first frame to its last while the other slot plays on; a lost voice burst
  plays as silence in its place. One active slot plays in both ears; with one-channel output two slots are averaged.
- `-a` Enable call alert beep (UI). The alert is the same clean tone in integer and `-y` float output. Conventional
  analog calls sound no alert: the beep would land in the analog audio on `-o -` (on UDP output an alert goes to the
  digital port, never the analog one).

## Modes & Decoders (`-f`)

`-f` takes one of the letters below. Any other letter (`-f3`, say) stops startup with
`-f3 is not a decode mode; see dsd-neo -h for the -f letters` instead of leaving the session in Auto; a Qt or
Android session whose saved extra arguments carry one refuses to start the same way.

- Auto (the default): `-fa`, or no `-f` at all. Every digital mode except TETRA (NXDN48 and NXDN96 included; see
  the hunt table below)
- Passive analog monitor: `-fA` (NFM on a radio input; see [Analog reception](#analog-reception--fa--fm) below)
- Native AM receiver: `-fM` (I/Q radio inputs only; see [Native AM](#native-am--fm) below)
- Trunking helper: `-ft` (P25p1 CC + P25p1/p2/DMR voice)
- DMR simplex (BS/MS): `-fs` uses the dual-slot decoder; `-fr` uses the single-slot mono decoder
- P25 Phase 1 only: `-f1`
- P25 Phase 2 only (6000 sps): `-f2`
- D‑STAR: `-fd`
- X2‑TDMA: `-fx` (4800 sps, P25 Phase 1 modulation: C4FM, or CQPSK/LSM with `-mq` on simulcast)
- YSF: `-fy`
- M17: `-fz` (radio); `-fU` (M17 UDP/IP frame)
- NXDN48: `-fi` (6.25 kHz)
- NXDN96: `-fn` (12.5 kHz)
  With `-T` on an NXDN Type-C system (NXDN48 or NXDN96), the decoder keeps to the site whose control channel it chose:
  the site information (SITE_INFO or SRV_INFO) heard while no call is followed names it, and only that site's messages
  set the site shown and, on a DFA site, the base and step channel frequencies are counted from. Messages naming other
  sites (a CCH_INFO, the adjacent sites, another site's broadcasts heard on a traffic channel) are printed only. A DFA
  site moves the control channel only when its own CCH_INFO names one current or new, never for a candidate added or
  removed; under plain `-T` the first such move is then kept like a channel map row. A composite control channel, one
  that carries calls itself, is read as one once the session has decoded Type-C trunking there (a CAC, a CCH_INFO or a
  SITE_INFO); until then its frames read as Type-D, so a session started on one during a call shows Type-D details until
  that call ends or one of those messages arrives. A voice channel can also be another site's control channel: while a
  call is followed there, that control channel's traffic is printed only (`(other channel's control; log only)`) and
  none of its calls is followed, and when the call's end is missed the decoder goes back to its own control channel once
  the hangtime (`-t`) has passed since the call's last voice, though the other control channel keeps the frequency busy.
  Known limits: the site is taken from any site information whose CRC passes while no call is followed, so a noise frame
  naming another site can leave DFA channels unknown until the site's next SITE_INFO, and a call followed before any
  site information was heard shows no site until the decoder is back on the control channel.
- ProVoice: `-fp`
- EDACS/ProVoice: `-fh` (standard), `-fH` (with ESK 0xA0)
  - Custom AFS bit splits: `-fh344`, `-fH434`
- EDACS EA/ProVoice: `-fe` (standard), `-fE` (with ESK 0xA0)
- dPMR: `-fm`
- TETRA: `-fT`. Like every `-f` selector it is replaced by a later one (`-fT -fp` decodes ProVoice alone). TETRA's
  pi/4-DQPSK runs at 18 ksym/s and its symbols are timed at a whole number of samples each, 2 to 8, so
  it decodes only at a DSP rate that is a multiple of 18 kHz from 36 to 144 kHz. `-fT` therefore sets the DSP
  bandwidth to 72 kHz (4 samples per symbol), as `-fe` sets 24 kHz; a bandwidth field in the `-i` spec still names
  another, and a Qt TETRA system without its own bandwidth uses 72 kHz too. Every radio input runs at its DSP
  bandwidth exactly (an RTL-SDR or rtl_tcp input by halving its rate; a SoapySDR or Airspy device, which picks its own
  rate, by halving and then resampling it), so 72 kHz decodes TETRA on each of them. A stream that opens TETRA at
  another bandwidth warns once, with the fix in its input's own form (`TETRA needs a 72 kHz DSP bandwidth, not 48 kHz;
  dsd-neo does not change it on its own, so set it to 72 kHz (-i rtl:dev:freq:gain:ppm:72)`; `-i airspy:freq:72` on an
  Airspy, `-i soapy:args:freq:gain:ppm:72` on a SoapySDR device). A switch to TETRA at run time keeps TETRA selected
  and the bandwidth as it is, and says so in its toast (`Decoding TETRA: needs a 72 kHz DSP bandwidth; it stays 48 kHz
  until you set it`); nothing decodes until the bandwidth is 72 kHz. An I/Q replay is re-planned to a TETRA rate
  instead, at start and on a switch to TETRA (which reopens it where it stopped, `Decoding TETRA: replay resampled to
  72 kHz`); a sidecar capture whose recorded chain already runs TETRA at a multiple of 18 kHz from 36 to 144 kHz keeps
  it, and one from a device that picked its own rate (an Airspy's) keeps its halving and is resampled after it to
  72 kHz, as live. Only a capture no plan can resample
  (below 8 kHz) is refused (`...: this replay's capture cannot be resampled to a TETRA rate`). Leaving TETRA returns
  the replay to the chain its DSP bandwidth plans.
  A TETRA downlink has a receive chain of its own. It follows the carrier with a tracking mixer that moves the signal
  back towards the centre by up to 8 kHz either way, filters the 25 kHz TETRA channel (flat to ±11.25 kHz, about
  28 dB down by ±13.75 kHz and 75 dB from ±15 kHz, shown as a 22.5 kHz channel width), levels the signal, and applies
  the receive half of TETRA's root-raised-cosine pulse (roll-off 0.35, the matched filter for what TETRA transmitters
  send). Then it corrects the carrier offset that is left, recovers the symbol timing, and slices the pi/4-DQPSK
  symbols at their quadrant boundaries. The carrier loop pulls in wide and narrows once its estimate holds, so the
  frequency error the receiver reports holds steady: on a clean real-air capture it varied by 12 Hz (standard
  deviation) where a loop kept wide varied by 62 Hz. It cannot narrow sooner than about 0.13 s after the stream starts
  or the carrier moves (25 steps of 96 symbols: one to start, 16 to fill its error window and 8 holding steady), and
  on a noisy test signal (about 10 dB SNR) it narrowed after 0.23 s. The chain runs only at the DSP rates above, with
  the channel filter on. When
  TETRA is selected but the chain cannot run, the stream runs the generic CQPSK path instead, which has neither the
  TETRA channel filter nor the matched filter and decodes no TETRA at a rate outside that range. It logs the cause once
  per stream, with what fixes it: a rate outside that range (`TETRA receive chain inactive: TETRA cannot decode at
  48000 Hz; it needs a multiple of 18 kHz from 36 to 144 kHz (a 72 kHz DSP bandwidth).`), a timing (TED)
  samples-per-symbol setting from another mode that does not match the DSP rate (`...: a timing setting from another
  mode is in force (8 samples per symbol, where 72000 Hz needs 4), so TETRA cannot decode.`), or the channel filter turned off
  (`...: the channel filter is off (DSD_NEO_CHANNEL_LPF=0); unset DSD_NEO_CHANNEL_LPF or set it to 1.`).
  Tuning tolerance: tune to the carrier's own frequency. These cases were measured on test signals at 72 kHz, the rate
  `-fT` sets; other rates were not measured.
  - On a clear channel, decoding held with the radio up to about ±10 kHz off the carrier. 12 kHz off, it held with
    the radio above the carrier (228 of 232 identities) and decoded about a fifth with it below (49); 14 kHz below,
    it was lost.
  - Next to a much stronger carrier (another TETRA network 25 kHz away, 10 to 40 dB stronger), keep any tuning error
    towards the stronger carrier within about 2 kHz: from 3 kHz towards it the decode was lost. Tuned up to 10 kHz the
    other way, away from it, decoding held.
  - In none of those cases did the decoder decode the stronger neighbour's network instead. Tuned 10 kHz towards a
    carrier 20 dB stronger, it decoded neither carrier and never named the other network. That is how it behaved in
    those cases, not a guarantee.

  The frequency error the receiver reports for a TETRA stream (the terminal, Qt, the API and `--auto-ppm` read it) is
  only accurate within about ±8 kHz, the tracking mixer's reach. Past it the rest of the offset is estimated behind
  the matched filter, which cuts into the band edges the estimate reads, so the reported error falls short while
  decoding still holds: a real-air call 10 kHz off read about 0.7 kHz short one way and 1.3 kHz short the other.
  Within that range the estimate also carries a bias of a few hundred hertz that depends on what the carrier sends
  (on that call, about 190 Hz above the carrier's own offset).

  A receiver's own frequency error adds to how far off you tune, so correct it: give the receiver's error in parts per
  million in the `ppm` field of the `-i` spec (`-i rtl:dev:freq:gain:ppm`, or `rtl_ppm` in a config), or let
  `--auto-ppm` track an RTL-SDR's error (see Tools & Extras). TETRA base stations hold their frequency to within
  0.2 ppm, so most of the offset a downlink shows is the receiver's.
  TETRA reads its symbols from an I/Q radio input (RTL-SDR, rtl_tcp, SoapySDR, Airspy, `--iq-replay`) or from a symbol
  capture of them (`-c` writes one; `-i capture.bin`, `.raw` or `.sym` replays it; a float symbol file, `.raw` or
  `.sym`, holds them as the CQPSK demodulator recovered them, outer levels ±3 and inner ±1, and TETRA reads them as
  they are, where the other modes scale a float symbol file by 10000): discriminator audio has lost the
  phase steps of its pi/4-DQPSK, so `-fT` on a PCM input (Pulse, a WAV file, stdin, TCP or UDP audio) stops at startup
  with `-fT: TETRA needs an I/Q radio input or a symbol capture: its pi/4-DQPSK symbols cannot be read from demodulated
  audio`, as `-fM` does. A config whose `[mode] decode = tetra` meets such an input keeps the mode and logs that nothing
  decodes there. A switch to TETRA on one (terminal picker, Qt chip, API) is refused with that reason; the terminal
  picker's entry reads `TETRA (needs an I/Q radio input)` there and the Qt setup wizard greys its chip out (it offers it
  for a `.bin` capture). A running TETRA session whose input moves to a PCM one keeps the mode, and a toast says once
  that nothing decodes there.
  Speech plays through mbelib-neo's TETRA speech decoder (EN 300 395-2 full-rate ACELP), part of the vocoder library
  every build links; no codec or option is needed. A speech burst plays once its CRC has passed; later bursts whose CRC
  fails are concealed by the decoder, and a run of them ends the speech until the next good burst.
  With `-T` the decoder follows a call's traffic channel off a TETRA control channel, once a block whose CRC passed
  has been decoded on the carrier it is on, and returns `-t` seconds after the call's speech ends, or 4 seconds after
  the tune when no speech follows while nothing keeps the channel for the call (its usage marker in the channel's
  ACCESS-ASSIGN PDUs, or a message to one of its parties there, restarts those 4 seconds before the first speech; EN
  300 392-2 clause 23.5.6.1.2), and leaves once three ACCESS-ASSIGN PDUs say the channel is no longer the call's. It follows a call whose D-SETUP named its called party, as the group list, `-I`,
  priority and preemption rows and the tune controls decide; while it follows one, only that call's timeslots play,
  under the downlink usage marker its allocation assigned and while it has receive permission there: from its call
  signalling (the transmission grant), or from three ACCESS-ASSIGN PDUs in a row carrying its usage marker on its
  traffic channel, until three without it or 18 frames without one (EN 300 392-2 clause 23.8.2.3.2). A call followed
  onto another carrier plays that way, its traffic slot carrying no signalling of it (direct mode speech always
  plays). Calls are set up on the cell's main carrier, the one its SYSINFO names (every carrier of the cell sends that
  SYSINFO). A `-T` session started on another carrier of the cell (a secondary carrier, a rounded or wrong start
  frequency) moves there once a block whose CRC passed has proved the carrier it is on: it logs `[TETRA SM] on a
  secondary carrier (X Hz); moving to the main carrier Y Hz` and counts as on the control channel only once that main
  carrier's own SYSINFO is heard. The move needs a radio it can tune: a live RTL-SDR, rtl_tcp, SoapySDR or Airspy
  stream, or a rigctl peer (`-U`) behind an audio input; an I/Q replay or a symbol capture stays where it is. A replay
  whose recorded centre is within 8 kHz of the main carrier (the receive chain's tracking range) shows the control
  channel there; one further off, or on another carrier, shows the trunk state as idle, though it still decodes what
  the capture holds. The
  move waits while a manual tune is still pending or a scan owns the tuner. A move the radio refuses or fails is tried again after
  5 seconds, then 15, then every 60; one it defers after 2 seconds. A rigctl peer is taken to be on the frequency
  DSD-neo last sent it, so one already on the main carrier is not moved. Each main carrier is moved to once, so a cell
  whose carriers name each other is not followed back and forth (it logs `[TETRA SM] main carrier Y Hz was moved to once
  already; staying on this carrier` once), until an import, another radio or input, or trunking turned off and on
  again starts over. Nothing moves back to the carrier the session started on: when the main carrier cannot be
  received there, it logs `[TETRA SM] no SYSINFO on main carrier Y Hz after 10 s` once; start on a carrier this receiver
  hears, or leave trunking off. To monitor one traffic carrier on purpose, run without `-T`. Without `-T` one
  timeslot of the carrier is heard at a time, until its speech stops for a second. TETRA is not a `--trunk-scan` target
  type. Air-encrypted speech and signalling decrypt with `--tetra-key`, an 80-bit network key or a TEA1 reduced key of
  one channel (see Keys & Privacy), so `-T` follows the calls of a security class 2 or 3 cell too.
  After a call the decoder goes back to the control channel it left. TETRA cells reuse frequencies, so a traffic channel
  can carry another cell's bursts too, and while the decoder follows a call it does not take them on there: another
  cell's SYNC (another network or colour code), its SYSINFO (another main carrier or location area) and direct mode
  bursts are ignored (`-v 1` logs `another cell on the followed carrier; not taken on`, `...; not applied` or
  `direct mode on the followed carrier; not taken on`), the control channel stays the one the call left, and Return to
  CC, skip and lockout go back to it. Turning trunking off and on during a call returns to it at once, since what was
  heard meanwhile cannot be trusted. Known limits: a cell with the same network and colour code on the same frequency
  cannot be told apart, by the decoder or by a radio, so its messages and speech count as the followed cell's and can
  keep or move the call (only its SYSINFO is ignored, and the return still goes to the control channel the call left). A
  call moved to another cell (a channel allocation with its cell change flag set, logged with
  `cell change: for another cell; logged only`) is not followed, nor are umbrella channels shared by cells (EN 300 392-2
  Annex S); such a message changes nothing, though its short data and status messages still reach the event history.
  When such a message is sent in pieces, the first piece is acted on as any other, because the cell change flag only
  arrives with the last piece. The registration (MM) messages in a cell-change message change nothing that is shown, but
  an encryption algorithm a repeated decryption proved in it is still remembered. Speech counts as the followed cell's
  when its own checks pass, and no test drives this with encoded speech. A direct mode channel taken on while trunking
  was off ends at another cell's SYNC, which is harmless because the decoder then returns to the control channel the
  call left. If the cell moves its control channel to another frequency during a call, the decoder still returns to the
  old one and moves on when that carrier's SYSINFO names the new one; if the old frequency has gone silent it stays
  there until you retune, since TETRA has no control channel hunt. When neither a SYNC nor `--tetra-net` gave the
  network before the call was followed, another cell's SYNC cannot be told apart for that call; with colour code 0 and a
  `--tetra-net` network that is not the cell's, a call followed before the cell's first SYNC ignores the cell's own SYNC
  for that call. ACCESS-ASSIGN PDUs carry no checksum, so they keep a silent call's channel, or give it receive
  permission, only while the followed cell's own checked blocks were heard on the channel in the last 4 seconds (a live
  cell sends its SYNC about once a second); three saying the channel is no longer the call's can still end a call early,
  as noise can, and the decoder then returns to the control channel.
  `--tetra-uplink` decodes a trunked mode uplink carrier, the radios' transmissions, instead of a downlink: its control
  uplink bursts (random access) and its normal uplink bursts. The uplink does not run the downlink's receive chain,
  which lost bursts the generic CQPSK path decodes: it runs that path with the TETRA channel filter (at the same DSP
  rates, with the channel filter on; otherwise it logs `TETRA uplink channel filter inactive: ...` once and decodes
  without it), and takes each burst's symbol timing and carrier phase from the burst's training sequence, since every
  burst comes from another radio, short and after silence or noise, too soon for the loops to pull in. That timing reads
  128 symbols ahead, so a burst reaches the decoder about 7 ms after it ends (and still before a closing squelch's
  silence, the end of a replay, an `--iq-loop` rewind, or a replayed retune or stream gap; a live retune or stall
  recovery drops those 7 ms). An uplink carries no network identity, so give the one its cell scrambles with as
  `--tetra-net mcc,mnc,colour` (MCC 0-1023, MNC 0-16383, colour code 0-63 in decimal or `0x` hexadecimal; the downlink's
  Network row shows all three). `--tetra-net` also lets a downlink session descramble before its first BSCH, whose
  identity replaces it; it shows on no row until a block descrambled with it passed its CRC. On an uplink the TETRA row
  shows the uplink's air encryption, from the encrypted flag of its radios' MAC headers, and the algorithm a radio's
  clear registration proposes (`radios propose TEA1`, see "Which TEA a network uses" under Keys & Privacy).

Notes

- Auto (`-fa`, and a session that names no `-f`) enables the complete digital candidate set and hunts these profiles
  in order, skipping any profile that has no enabled candidate:

  | Hunt profile | Symbol rate | Levels | Enabled candidates |
  | --- | ---: | ---: | --- |
  | 0 | 4800 | 4 | P25 Phase 1 (C4FM/CQPSK), X2-TDMA (C4FM/CQPSK), DMR, NXDN96, YSF, M17 |
  | 1 | 2400 | 4 | NXDN48, dPMR |
  | 2 | 9600 | 2 | ProVoice, EDACS |
  | 3 | 6000 | 4 | P25 Phase 2 (CQPSK) |
  | 4 | 4800 | 2 | D-STAR |

  A sync that a protocol turns into decoded frames locks the active rate, level count, timing, and RTL-family channel
  profile; a sync on its own does not (see the dwell note below). Passive analog monitoring
  (`-fA`) and already-framed M17 UDP input (`-fU`) are not frame-sync hunt candidates.
- On RTL-family FSK input the decoder's symbol timing follows the hunt profile from the moment the hunt selects it. The
  matching front-end channel profile is requested asynchronously and is applied by the demod thread on its next block,
  so the two are briefly out of step after every hunt step; decoding does not wait for the front end to catch up.
- A retune -- a scanner hop, a trunking tune, or a replay RESET -- keeps the symbol profile the front end is on and
  recomputes only the timing samples-per-symbol, and then only if the output rate changed. The profile is derived from
  the enabled decoders at stream open only.
- The hunt dwells on each candidate profile for a bounded budget of symbols searched for sync, so a full rotation over
  the five profiles takes roughly six seconds at 48 kHz. A transmission that starts mid-rotation and lasts less than one
  rotation can therefore be missed entirely even though the same capture decodes under its native preset -- Auto
  converges on signals that persist, such as a control channel or a call of a few seconds, not on isolated short
  bursts. Name the narrower preset when you already know the mode and cannot afford the search.
- What holds a profile is decoded frames, not detected syncs. The dwell is a net budget: symbols spent searching for a
  sync count against it, and symbols a frame handler consumes are credited back. A profile carrying real traffic sits at
  zero, because a decoded frame costs hundreds of symbols and the next sync follows within a few. Credit is counted per
  sync, and only once a handler reaches a frame's worth of symbols, so a matcher that recognises a marker and bails --
  the permissive 4800/4 matchers produce these on signals belonging to another profile -- buys nothing however often it
  fires, and the dwell does not depend on that cadence. Frame sync also sees a verdict where the protocol produces one:
  a YSF frame before the transmission's first FICH CRC has held, a failed EDACS BCH, a failed D-STAR header CRC, a
  failed P25 Phase 1 NID, a dPMR frame whose CCH CRC-7 has not passed recently, an NXDN or M17 frame whose
  transmission has not yet passed a CRC, and a D-STAR voice or ProVoice frame whose transmission has not yet proved
  itself all buy no dwell however many symbols they consumed.
  Handlers that report no verdict are credited as before: every DMR, P25 Phase 2 and X2-TDMA frame is
  unconditionally productive, and a false match on one of those still delays the hunt in proportion to what it
  swallows. That delay is bounded rather than a hold, because the symbols a handler eats are symbols the search
  never spends: a profile is held only where syncs arrive closer together than twice the block behind each one. All
  three sit behind 20- and 24-symbol exact matchers that noise does not reach. dPMR was the fourth and the only one
  it did reach -- one 12-symbol pattern per polarity, against the 372 symbols an FS2 frame reads, so hits falling
  within 744 symbols would hold the profile against the one per ~2048 noise produces -- and it reports a verdict
  since issue #407: a passing CCH CRC-7 proves the profile for the next two seconds, and a frame that decodes
  nothing outside that window buys no dwell even at the full frame cadence.
- A sync the decoder deliberately declines to process costs the profile nothing either way. Trunking skips the DMR
  direct-mode paths outright, and any frame arriving while a retune is still in flight is dropped rather than
  dispatched; neither reads a symbol, so neither can earn credit, and the search that found the sync used to stand
  charged with nothing to pay it back -- enough, on a trunked system, to rotate the profile off a voice channel the
  control channel had just granted. Those searches are now refunded, so the cycle comes out neutral. It is only ever
  a refund of that one cycle, so a stream of false matches on a declined path still cannot hold a profile that is
  finding nothing.
- While the tuner is parked on a trunked voice channel, the hunt holds whatever profile the grant tuned it to and
  does not rotate. The channel and its symbol rate were chosen deliberately, so a call fading toward the noise floor
  -- crediting less than the search between its syncs burns -- keeps its timing instead of having it changed mid-call.
  Giving the channel up is still hangtime's decision, unchanged: when the call ends the tuner returns to the control
  channel and the hunt resumes there. Control channels are not held this way, so Auto still searches and still
  converges on them.
- On an RTL input with no modulation lock, P25 Phase 1 on the 4800/4 profile can arrive on either demodulator chain:
  C4FM, or CQPSK for an LSM simulcast system, which the C4FM chain reads only in part. Nothing before a decoded frame
  can tell them apart, so alternate 4800/4 dwells that expire with nothing decoded try the CQPSK chain. That chain
  cannot read DMR, NXDN96, YSF or M17, so when any of those is enabled a trial that has decoded nothing by half its
  dwell gives the rest of the dwell back to C4FM and the other modes. Half is counted in symbols the search spent, the
  hunt's own measure of a dwell (2700 of 5400 on the usual three-pass dwell; frames a handler read do not count). The
  first frame whose NID decodes, on either chain, starts one comparison: over the next 1800 symbols (about 0.4 s) the
  hunt adds up what that chain's decoded frames read. A P25p1 carrier sends its frames back to back, so a chain that found every
  frame's sync within a frame of the last one's end is reading all of it and keeps it; one that let a frame go by
  gives the other chain 1800 symbols of its own, and the chain that read more keeps the profile and is remembered, so
  returning to that carrier after a voice grant starts on it without comparing again. A transmission that ends before
  either chain has decoded a second frame decides nothing, and a remembered chain is forgotten once a dwell on it
  decodes nothing. A trunked tune that comes mid-comparison lands on the chain leading so far, except a Phase 2 voice
  grant, which is CQPSK whatever the control channel's chain. Without this, a C4FM chain reading about half of a
  simulcast control channel's TSBKs held it for as long as the channel stayed up.
  `-mq`, `-mc` and the other modulation locks skip all of it, on either side of the `-f` option.
- D-STAR voice and ProVoice prove a transmission rather than a frame, because neither has a per-frame check worth the
  name. A D-STAR superframe spends 1992 symbols and a ProVoice frame 736, and the vocoder error counts both leave
  behind are soft corrections rather than a verdict. So a CRC-16/X.25 -- the D-STAR RF header, or the header
  rebroadcast that slow data carries through a transmission -- confirms outright, and failing that, a second frame
  arriving before the carrier drops does: each sits behind its own exact sync word, 24 symbols for D-STAR and 32 for
  ProVoice, which noise does not supply twice in a row. A real call therefore pays at most one frame of withheld
  credit at the very start, and none at all when it opens with a decodable header, while an isolated false match on
  either profile buys nothing.
- NXDN announces nothing until a frame's content checks out. Its 10-symbol sync word and one-parity-bit LICH are weak
  enough that receiver noise clears both, so a frame that reaches the protocol layer does not by itself refresh the
  scan hold, synthesize voice, publish a RAN, or open a call record. One CRC of 12 bits or more (FACCH, CAC, UDCH,
  PICH/TCH, a full SACCH superframe) is proof on its own; the 6- and 7-bit CRCs on SACCH and SCCH have to repeat on
  two consecutive frames. A real call confirms on its first FACCH, or within two frames when only SACCH is passing,
  so the cost is at most one frame of audio at the very start of a transmission.
- dPMR and NXDN48 share the 2400/4 profile, and their sync matchers are not comparable in strength: dPMR's Frame Sync
  2 is a single exact 12-symbol pattern, while NXDN's takes five variants per polarity over ten symbols sliced on sign
  alone and so fires on roughly one arbitrary window in a hundred. The 372 dibits behind an accepted FS2 are dPMR's
  frame, so for that frame's span the NXDN48 matcher stays off the profile: an NXDN match inside it would consume
  symbols the frame needed and warm-start the slicer thresholds from ten symbols of dPMR payload, which left Auto
  decoding corrupted dPMR identities on captures the `-fm` preset reads cleanly. The suppression lasts one frame and
  is re-armed per FS2, so it costs a real NXDN48 signal nothing -- such a signal produces no FS2 matches at all -- and
  it does not apply to NXDN96, which lives on 4800/4 where dPMR cannot match, nor to any build with dPMR disabled.
- The 4800/4 profile carries P25p1, DMR, NXDN96, YSF and M17 together, and the same imbalance applies there: P25p1's
  sync is one of two exact 24-symbol patterns, while NXDN96's is the ten-symbol word above and M17's preamble is an
  alternating run that any 4800-baud signal presents. So for the span of the frame an accepted P25p1 sync opened,
  neither of those two can take a sync on the profile -- a false frame inside it consumes the symbols the next sync
  needed, warm-starts the slicer from P25 payload, and takes the `lastsynctype` that keeps the C4FM threshold tracker
  running between one P25p1 frame and the next. Under Auto that had left a P25 Phase 1 C4FM control channel decoding
  no NAC at all where its own `-f1` preset reads 26. Two deliberate limits: the NXDN window still contributes its
  level estimate while inside the span, because that blend is the wideband reference the CQPSK chain depends on, and
  a sync the CQPSK chain carried closes the span rather than opening one, for the same reason. The span is re-armed
  by every P25p1 sync, so continuous traffic stays covered, and it lapses within a fifth of a second of P25 going
  quiet. It costs a real NXDN96 or M17 signal nothing, since neither produces P25p1 sync matches.
- M17's sync words are eight symbols and its preamble an alternating run, and the frame sync reads them on the sign
  of each symbol alone, so receiver noise used to supply both: an eight-window run followed by a word within one
  error opened a few M17 frames a second on an idle channel. A preamble now has to run for 32 windows (a transmission
  sends 192 symbols of it) before the LSF or BERT word behind it opens the transmission. A transmission joined after
  its preamble -- the receiver tuned or rotated onto it mid-stream -- is opened by its own frames instead: every M17
  frame is 192 symbols, so a stream or BERT sync word read exactly, with the same word exactly one and two frames
  before it, opens that frame. That costs the frame landed in and the two after it, about 120 ms, and noise almost
  never offers it. A join opens that one frame: the next continues only on M17's own evidence (the stream's LICH, a
  BERT PRBS lock). A packet is never joined: its one CRC covers every packet frame, so a packet joined after its
  first frame cannot pass it.
- NXDN96 follows the receiver rules of NXDN TS 1-A (5.3, Table 5.9-3). It acquires on two frame sync words a frame
  (192 symbols, 40 ms) apart, within a symbol either way, each read on four levels within two bit errors -- the
  ten-symbol word read on its signs alone matches about one window in a hundred, and receiver noise opened about two
  NXDN96 frames a second on it. A transmission's first frame is not read: the specification also lets a single frame
  sync word behind the preamble acquire, but on weak calls that start left the decoder reading every later frame's
  control channel wrong, so it is not used. Once acquired, the frames that follow are timed from the last frame whose
  link information (LICH) was good, up to five missed in a row (the frame right after a good one on its signs, later
  ones on four levels too). A lost frame sync word, or a LICH that fails its parity check, usually costs that frame
  alone; on a weak signal the next frame sync word must then also read within two bit errors, so it can cost more.
  Five LICHs in a row that pass their parity but are rejected (a value no NXDN frame uses, or inbound on a trunked
  session) lose the lock. Both rules remember every
  hit, since a transmission's payload matches these short words several times a frame. A symbol capture replays each
  symbol at its dibit's level, which the four-level reading reads as it does a live signal.
- A transmission the hunt has stepped away from is protected the same way, for as long as it may still be running.
  Both spans above cover one frame, which is no help across a profile change: under Auto the hunt can rotate off
  2400/4 while an NXDN48 call is still on air, and by the time the rotation reaches 4800/4 the NXDN96 and M17
  matchers there are offered that same 2400-baud signal read at twice its rate -- and take it, printing frames whose
  content is noise. So a handler that proves its profile now records which profile it proved, and while a proof of
  2400/4 is recent enough that its transmission could still be running, those two matchers stand down on 4800/4.
  A proof is a passing check, never a sync: dPMR's CCH CRC-7 and the NXDN confirmation CRCs above, both of which a
  real NXDN96 or M17 signal is incapable of producing, so neither can arm this against itself. Only the frame that
  passed the check records one, so the noise syncs that follow a transmission cannot keep the guard armed. The span
  runs one full rotation of the hunt from the last proof -- long enough that the guard is still standing through the
  whole of the first 4800/4 dwell it reaches, and short enough that a transmission which has genuinely ended frees the
  profile again within a few seconds. As with the P25p1 span, the NXDN window keeps contributing its level estimate
  throughout; what is withheld is the sync.
- Recording that proof is deliberately all it does. Making NXDN report the profile as *proven* to the hunt, the way
  dPMR and P25 Phase 1 do when their own checks pass, would also restart the dwell and hold the rate against
  rotation -- which sounds like the better fix and measured worse. Restarting the dwell keeps the hunt's budget away
  from the exit that runs the end-of-call housekeeping, and on the four-channel NXDN48 capture behind this guard that
  cost decoded calls: ten rotated replays per build scored 75 NXDN48 syncs and 11 voice calls without it against 66
  and 9 with, every paired repeat worse. So Auto still rotates off a live NXDN48 call exactly as before; what changed
  is only that the matchers it rotates onto no longer claim the call while it is still running.
- All three Auto entry points install the complete matrix above: CLI `-fa`, config `decode = "auto"`, and the
  interactive Auto choice select the same decoder set. Only `-fa` also resets the demodulator to C4FM and the audio
  layout to stereo; the config path leaves those to its `demod` and `dmr_mono` keys, and the interactive path to the
  wizard's own answers. To scan a deliberately narrowed set, name the narrower preset rather than Auto.
- Auto is also the default. A session that names no `-f` (as the Qt and Android Auto choice does) starts on the same
  decoder set, C4FM demodulator and stereo audio layout as `-fa`, a config whose `[mode]` has no `decode` key keeps
  that set, and such a session autosaves `decode = "auto"`. A modulation flag alone keeps it too: `-mq` (which a
  RadioReference import of a simulcast P25 site sets) runs Auto on the locked QPSK chain, whose symbol-rate search
  steps only between profiles of the locked timing (4800/4 and 4800/2 at 48 kHz), never to NXDN48, dPMR or ProVoice.
  Selecting Auto on a session already running the whole Auto set changes nothing (no call ends, a modulation pick
  stays); on a narrowed set it widens it back out.
- EDACS under Auto: Auto enables the EDACS/ProVoice decoder, but EDACS frames are processed only once an EDACS
  addressing mode is selected: `-fh -fa` (or `-fe -fa`), a switch to Auto from an EDACS/ProVoice preset, a config with
  `edacs_addressing = "standard"` or `"extended"` (or the older `edacs_ea`), or the EA toggle (terminal menu, `ProVoice EA mode`), whose first press under plain Auto selects
  standard addressing. Until then the session is no EDACS session: no raw analog stream or UDP analog socket
  (port + 2) opens for it, `-T` keeps `-8` on, the dynamic squelch treats its channels as digital, and talkgroup
  lockout and Skip work on P25, DMR and NXDN calls. Once EDACS is selected, Auto behaves as the EDACS/ProVoice preset
  does for those (the analog sink opens then, and on a trunked `-o udp` run `-8` turns off, as under `-fh -T`; so does
  a switch to EDACS/ProVoice or a config apply that selects EDACS).
  ProVoice alone (`-fp`) is an EDACS session without an addressing mode, also after an earlier `-fh/-fH/-fe/-fE`, and
  its config saves `edacs_addressing = "none"` to keep it so. Any other preset clears the addressing, so
  EDACS, then DMR, then Auto is plain Auto again; a scan row's own class keeps the configured addressing.
  Only `-fz` (M17 alone) waits for a TCP audio server that refuses or drops the connection; under Auto a refused
  connect is an error and a dropped one ends the input, as in any other mode.
- In TCP PCM mode, SPS hunting still runs when no signal is present, but repeated idle `Sync: no sync` and `SPS hunt`
  console diagnostics are suppressed.
- P25p2 on a single frequency without trunking recovers the scrambling code from the channel's own scrambled
  FACCH/SACCH; `-X` (below) sets it by hand.

## Analog reception (`-fA`, `-fM`)

`-fA` is the analog monitor. On a radio input (RTL-SDR, rtl_tcp, SoapySDR, Airspy or `--iq-replay`) the front end
demodulates narrowband FM (NFM) through a channel filter and plays what it hears; on PCM input (Pulse, files, UDP or
TCP audio) the audio arrives already demodulated and is monitored as it is. Broadcast (wideband) FM is out of scope.
`-fM` runs the same monitor with an AM envelope detector in place of the FM discriminator, on radio inputs only
([Native AM](#native-am--fm) below). Each has its own channel width; the rules below are written for NFM and hold for
AM too, apart from its range, its default and what is said of the default there.

### Channel width

`--nfm-bandwidth-hz <Hz>` (or `--nfm-bandwidth-hz=<Hz>`) sets the NFM channel-filter width; the config key is
`[analog] nfm_bandwidth_hz` (see `docs/config-system.md`). The width is the full RF channel centred on the tuned
frequency, and it is the protected passband: its edges sit at +/- width/2, where the response is about -0.3 dB. The
filter's cutoff is width/2 + 600 Hz with a fixed 1200 Hz Blackman transition outside the passband, so the response is
about -30 dB at width/2 + 1200 Hz and -50 dB or better from about width/2 + 1450 Hz. The transition is not
configurable. The width is not the tuner bandwidth (`soapy_bandwidth_hz`), not the DSP bandwidth (the `bw` field of an
RTL input, `rtl_bw_khz`), and not the audio bandwidth.

- Values are whole Hz from 8000 to 25000, with no `k` suffix: `12500` for a 12.5 kHz channel plan, `11250` for
  11.25 kHz, `25000` for a wide 25 kHz channel. The default is 16000 (16 kHz), which is the WIDE profile's
  channel filter bit for bit at DSP rates from about 19.1 to 102.7 kHz. That takes in the 24, 48 and 72 kHz DSP
  bandwidths every radio input runs at. Below that range the unset default is DSP-limited (see below).
- Match the width to the channel plan in use. A width wider than the channel spacing lets the adjacent channel into
  the demodulator: on a 12.5 kHz plan, 25000 passes the neighbours 12.5 kHz either side, which can clip the audio and
  raise its level.
- A value outside the range, or anything but whole Hz, is refused with an error that names the option, the range and
  the value, and `dsd-neo` exits. A width is never clamped to something else.
- An explicit width, 16000 included, always runs the channel filter, and `DSD_NEO_CHANNEL_LPF=0` with an explicit
  width is an error. Left unset, the default keeps the historical rule: the filter runs only at DSP rates of 20 kHz or
  more (`DSD_NEO_CHANNEL_LPF` overrides that), and below them the DSP rate bounds the channel instead. Frontends then
  show the width the rate leaves, marked *DSP-limited*, so existing low-rate `-fA` command lines keep working
  unchanged.
- On PCM input the option parses but filters nothing, and says so with a warning. With a rigctl peer (`-U`), which
  demodulates what a PCM input hears, the width is the FM passband the peer is asked for, in place of `-B`, while the FM
  monitor runs (`-B` under [Trunking & Scanning](#trunking--scanning)); left unset there it is not 16 kHz but `-B`, or
  the peer's own passband with `-B` 0. The start says so in a notice instead of the warning: `NOTICE: --nfm-bandwidth-hz
  filters nothing on PCM input; with rigctl (-U) it is the FM passband the peer is asked for, in place of -B, while the
  FM monitor runs.`
- AM's width, `--am-bandwidth-hz` (`[analog] am_bandwidth_hz`), takes whole Hz from 5000 to 20000, default 6000. AM
  always runs its channel filter, so its unset default is held to the rate like an explicit width: never DSP-limited,
  refused where the rate cannot filter it, and refused under `DSD_NEO_CHANNEL_LPF=0`.

The width has to fit the DSP rate it runs at: its cutoff must stay within 0.9 x Nyquist (width/2 + 600 Hz <= 0.45 x
rate), and the filter within its 288 taps. For every radio input (RTL-SDR, rtl_tcp, SoapySDR and Airspy) the DSP
rate is the DSP bandwidth, since a SoapySDR or Airspy device's own rate is resampled to it:

| DSP bandwidth (`bw`, `rtl_bw_khz`) | Widest channel it filters | NFM widths that fit | AM widths that fit |
| --- | --- | --- | --- |
| 72 kHz | 63.6 kHz | all, 8 to 25 kHz | all, 5 to 20 kHz |
| 48 kHz (default) | 42 kHz | all, 8 to 25 kHz | all, 5 to 20 kHz |
| 24 kHz | 20.4 kHz | 8 to 20.4 kHz | all, 5 to 20 kHz |
| 16 kHz | 13.2 kHz | 8 to 13.2 kHz | 5 to 13.2 kHz |
| 12 kHz | 9.6 kHz | 8 to 9.6 kHz | 5 to 9.6 kHz |
| 8 kHz | 6 kHz | none; only the unset default runs there, DSP-limited | 5 to 6 kHz (the default fits) |
| 6, 4 kHz | 4.2, 2.4 kHz | none; only the unset default runs there, DSP-limited | none: AM is refused |

A width the DSP rate cannot filter is refused with the width, the rate, the widest width that rate filters and the
DSP bandwidths that would fit, for example `NFM bandwidth 25 kHz does not fit the 24 kHz DSP rate (the largest width
it fits is 20.4 kHz); set the RTL DSP bandwidth to 48 or 72 kHz` (`set the DSP bandwidth to ...` on a SoapySDR or
Airspy device). For every radio input this happens at startup, from the input spec alone, before the input looks for
its device. Whether a SoapySDR device can deliver a rate dsd-neo can resample to the bandwidth is known only when it
opens, and one that cannot does not start. An I/Q replay is checked when the stream starts, against the rate its plan
runs at. Its rate follows the DSP bandwidth (a bandwidth change re-plans its rate chain, resampling the capture where it
must), so the fix names the DSP bandwidths that fit, as for a radio: `...; set the DSP bandwidth to 48 or 72 kHz`.

`--validate-config` applies the same check to a config whose `[mode] decode` is `analog` (an explicit
`nfm_bandwidth_hz`) or `am` (`am_bandwidth_hz`, its default included) and whose `[input]` builds a radio input with
`rtl_bw_khz`: an `rtl` or `rtltcp` source that sets `rtl_freq` (an rtl_tcp source without one connects at the 48 kHz
default), or a `soapy` or `airspy` source.

A refusal while running (the terminal row, the Radio sheet, a loaded config, a switch to Analog) is a short message
that names the width, the rate, the widest width it filters and the fix, and the full text goes to the log. The fix
is a DSP bandwidth on every input: `NFM 25 kHz does not fit the 24 kHz DSP rate (max 20.4 kHz); use a 48 or 72 kHz DSP
bandwidth`. On a running I/Q replay the message names the rate the replay runs at.
A DSP bandwidth refused for the width in use says to narrow the width first, or, at 4, 6 or 8 kHz, where no NFM width fits, to leave
the width unset first (`DSP BW 8 kHz cannot filter NFM 8 kHz (max 6 kHz); leave the NFM width unset first`). The front
end checks a width again where the change lands, between DSP blocks: should a retune have moved its rate in the
meantime and the width no longer fit, the width is put back to the one the front end kept, with the same message,
rather than staying configured while the filter runs another. A switch to Analog the front end refuses that way stays
on the digital family, so the decoder goes back to the mode it had, with the message (`Failed: Analog -> ...`); a
switch to AM, or between AM and Analog, is put back the same way (`Failed: AM -> ...`). Switches made one after
another (AM, then Analog) go back to the mode the front end still runs: the one before the first when it never took
the first, or the first when it did. An AM or NFM width changed just before such a switch, which the front end never
took, goes back to the width the front end runs when the moved rate cannot filter it; after a scan stop, the rate a
row left may filter neither, and the width of the mode put back is then settled as for a refused scan stop (the decode
mode row under When changes apply). AM refusals read the same way with AM's values (`AM 20 kHz does not fit the 16 kHz
DSP rate (max 13.2 kHz); use a 24, 48 or 72 kHz DSP bandwidth`),
and a DSP bandwidth that filters no AM width at all says to keep a wider one (`DSP BW 6 kHz cannot filter AM 6 kHz
(max 4.2 kHz); no AM width fits it; keep a wider DSP bandwidth`), since narrowing cannot help and AM has no unset
default that runs without its filter.

With `DSD_NEO_CHANNEL_LPF=0` every explicit width on a radio input is refused, at any rate, since the channel filter it
needs is off (`NFM 12.5 kHz needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off`). A change that would
reopen the device with one (a loaded config's `[input]`, a DSP bandwidth, Input > Switch source > RTL-SDR or Airspy)
is refused before anything is torn down, and the running input stays. On PCM input, which runs no channel filter, the
width is only stored (with a rigctl peer, the passband the peer is asked for, below), and a switch to Analog there goes
ahead, as a PCM start with the same width does; a switch from PCM to a radio input is held to the width.

The terminal shows the width in force on the input's channel row, after the squelch (the source row above it carries
`DSP-BW:`): `Analog: NFM 12.5 kHz;` for an explicit
width, `Analog: NFM 16 kHz (default);` for the unset default, and `Analog: NFM 12 kHz (DSP-limited);` when the rate
bounds it; under AM, `Analog: AM 6 kHz (default);` and so on. With the stream stopped, a radio input (RTL-SDR, rtl_tcp,
SoapySDR or Airspy) shows what the next start runs at its DSP bandwidth, and while a scan row runs a digital protocol
the line shows what the monitor returns to at the running rate: where the stream runs no channel filter for the unset
default, the default reads as that rate, DSP-limited. The stream decides that when it opens, from the DSP bandwidth it
opens at (below 20 kHz, or as `DSD_NEO_CHANNEL_LPF` says), and keeps it when the device delivers another rate. While an
`nfm` or `am` scan row with its own width (`--nfm-bandwidth-hz`, `--am-bandwidth-hz`) is on air, the row's width is the
one in force and the line names the configured width of that kind the row overrides: `Analog: NFM 12.5 kHz (row; default
16 kHz);`, `Analog: AM 8.333 kHz (row; default 6 kHz);`. An analog row shows the field on a digital session too. The
terminal sets the width from Input > NFM bandwidth..., right after Squelch..., whose label shows the setting (`[12.5
kHz]`, or `[default]`), while `-fA` is the configured mode or an `nfm` scan row is on air on a radio input (a scan row
running a digital protocol does not hide it), and the AM width from AM bandwidth... beside it while `-fM` is or an `am`
scan row is on air. The Qt and Android Radio sheet shows the same reading under the decode chips, and under any other
chip while an `nfm` or `am` scan row is on air (a row's own width reads first, with a `row` badge naming the configured
default of its kind the stepper edits, 16 kHz or 6 kHz while unset), with a stepper over 8, 11.25, 12.5, 16, 20 and 25
kHz (NFM) or 5, 6, 8, 10, 15 and 20 kHz (AM, under the AM chip or an `am` row) that skips the widths the DSP rate cannot
filter (the running stream's, or with none running the rate a radio input's DSP bandwidth sets), and *Use the default
width* to return an explicit width to the unset default. On PCM input without a rigctl peer it reads *not used on PCM
input* and the controls are disabled. On PCM input with a rigctl peer (`-U`), which demodulates what DSD-neo hears, the
width is the passband the peer is asked for while the monitor of its kind runs (`-B` under [Trunking &
Scanning](#trunking--scanning) has the rule), and no DSP rate bounds it or is checked: the terminal rows read `NFM
passband... [12.5 kHz]` and `AM passband... [default]`, an unset NFM width reading as `-B`, which stands in for it (`[-B
12.5 kHz]`, or `[peer's own]` without `-B`), and the input's channel row shows the passband after the squelch
(`Passband: NFM 12.5 kHz (-B);`, `Passband: AM 6 kHz (default);`) whenever a width is offered, on a digital session with
an explicit width the `-B` every tune asks for there. The Qt and Android monitor has a `PASSBAND` row reading the same,
whose Edit opens the Passband sheet: `NFM passband` and `AM passband` steppers over the same widths with no DSP bound,
`All channels | This channel` under a scan row that takes the edit, and *Use the default width*, which returns the NFM
width to `-B`; the Radio sheet's width controls stay disabled there, and its note points to the Monitor's Passband row.
The rows and the sheet offer NFM under `-fA` or an `nfm` row on air, AM under an `am` row on air, and either kind while
an explicit width of it is set. An edit that changes the passband in force is asked of the peer at once (`Applied: NFM
passband -> 12.5 kHz`, `Applied: NFM passband -> 12.5 kHz (-B)`), and one the peer refuses, or whose reply is lost, is
put back, the peer asked again for the passband the session then runs (`Refused: AM bandwidth -> 8 kHz: the rigctl peer
refused the passband`). Under another mode, the other analog kind's included, both keep the width control of a kind on a
radio input while an explicit width of it is set, with the setting as its reading (on the Radio sheet, a second width
control below the first under `-fA` or `-fM`): a switch to that mode is held to the width, so a width the device or the
capture cannot filter can be narrowed before the switch. The AM control is also offered, reading `default`, where the
DSP rate cannot filter AM's 6 kHz default but can filter a narrower AM width (a replay or device at 7.5 kHz filters up
to 5.55 kHz), since a switch to AM is refused there with word to narrow the width (on a replay, to change the DSP
bandwidth). The setup wizard offers the NFM and AM chips too, and picking either suggests no trunking. Its AM chip is
greyed out for a network or file source, with a note under the chips saying why, and on a radio source (USB, Airspy or
rtl_tcp) whose bandwidth (the system's, or the app default) is 4 or 6 kHz, where no AM width fits, the wizard waits at
the tune step with the reason and the bandwidths that fit; a system saved with that pair earlier fails to start with the
same reason. Both controls always edit the configured width, which is also what a saved config writes: a scan row's own
width is never saved. While a row that sets its own width is on air, an edit waits for the row to leave (or for a row
without a width), and the message says so: `Default NFM bandwidth -> 20 kHz; this channel overrides it (12.5 kHz)`, and
likewise for AM under an `am` row with its own width; an edit of the other kind's width is not shadowed. On any session,
while the scan has an analog row or target without a width of its own (an `nfm` row or `nfm-conventional` target, an
`am` row or `am-conventional` target), which runs the configured width of its kind when it comes on air, an edit of that
width the DSP rate cannot filter is refused whichever row is on air, rather than accepted and that row skipped at every
visit.

### When changes apply

| Change | When it applies |
| --- | --- |
| NFM or AM width from the terminal row, the Radio sheet, the Passband sheet or a loaded config | Live, on the next DSP block of a running analog monitor of that kind, with no reopen and no gap: the new filter runs over the signal already received, so no sample is dropped or repeated. The exception is an NFM edit between an explicit width and the unset default where the channel filter is off by default (`DSD_NEO_CHANNEL_LPF=0`, or a DSP rate below 20 kHz): it turns the filter off, dropping the samples it held back (about 1.5 ms), or on, starting it from an empty history. A width that changes the filter the front end runs also starts received-tone detection (below) over. Refused with a message, and the width unchanged, when the DSP rate it will run at cannot filter it (a config is then not applied at all); a width the front end refuses where it lands (a retune moved the rate) is put back, with the message. A width set while a scan row runs a digital protocol, or while CQPSK is toggled on under `-fA` or `-fM`, applies when the front end returns to the monitor; when a scan stops and the front end refuses the configured width at the rate a row's retune left it on, the front end keeps what it ran, the message says so, and the configured width takes the width a monitor of that kind kept, or else goes back to the kind's default, which the monitor then runs (`Refused: NFM 16 kHz does not fit the 16 kHz DSP rate; the monitor is back on the NFM default`; an AM default that rate cannot filter either leaves the width as it was, also when a retune refuses that default where it lands after it was asked for; a DSP-menu CQPSK toggle made before the front end took the return decides instead, and the width stays with no message, the refusal only logged); setting it under a scan row edits the configured width without disturbing the row, and an `nfm` row's own `--nfm-bandwidth-hz` stays in force until the row leaves. A width is held to the DSP rate as above on any session while the scan has an analog row or target of its kind (`nfm`/`nfm-conventional` for NFM, `am`/`am-conventional` for AM) without a width of its own, whichever row is on air, and an `am` row's own `--am-bandwidth-hz` stays in force until the row leaves as an `nfm` row's does. A width of the kind not in use is stored for the next switch to it. With no stream running, the next start uses it. On PCM input with a rigctl peer (`-U`) no channel filter runs and no DSP rate holds the width: while the monitor of that kind runs it is the passband the peer is asked for, so an edit that changes the passband in force is asked of the peer at once, strictly, and one the peer refuses, or whose reply is lost, is put back with the message, the peer asked again for the passband the session then runs; a loaded config whose width the peer refuses puts that width back, keeps the rest applied and fails (`Config applied; the rigctl peer refused AM 10 kHz: the AM width stays 8 kHz`). A width set while a typed digital row is on air, or the other kind's, is stored for the next tune that asks for it. |
| `-B` on PCM input with a rigctl peer (Input > `Rigctl setmod bandwidth...`) | While the FM monitor runs with no NFM width in force (a row's own or the configured one), `-B` is the FM passband the peer is asked for: an edit is asked of the peer at once, and one the peer refuses, or whose reply is lost, goes back to the `-B` it replaced, with the message (`Refused: Rigctl setmod BW -> 25000 Hz: the rigctl peer refused the passband`). Anywhere else it is stored for the next tune that asks for it: off the monitor (a digital mode, an RTL-family input) a tune sends it as before (on audio input the peer's own passband is read first, so a later return can put it back), and on the FM monitor with an NFM width in force the tune sends that width instead. |
| A command that moves the session onto, off or between monitors on PCM input with a rigctl peer (a decode-mode or input switch, a channel-map import, a loaded config's `[mode]` or `[input]`) | The peer is asked for the passband the session then runs at once. One brought by the command (a first rigctl connection, a switch onto PCM input) is asked only for a width, as at start; `-B` and its own passband wait for the next tune. Leaving the monitor for a digital mode puts the peer back on `-B`, or on its own passband with `-B` 0, at once when the monitor ran a passband this client set (a width, or `-B` standing in), best-effort as a tune's `-B` is; a `-B` edit on a digital session still waits for the next tune. A refusal leaves the command's change standing, with a warning in the log and the message (`Rigctl peer refused NFM 9 kHz (see log)`), and so does a refused return to `-B` after a loaded config; a loaded config refused on entering the monitor, its width unchanged, stays applied and fails (`Config applied; the rigctl peer refused NFM 12.5 kHz (see log)`). |
| Decode mode to or from Analog or AM (`-fA`, `-fM`, the NFM and AM chips, `[mode] decode`) | Live: the front end switches receive family, or between FM and AM, between DSP blocks. A switch to Analog with an explicit NFM width, or to AM with an AM width (its default included), the DSP rate cannot filter is refused, naming both, and the mode stays (under a scan row too); one the front end refuses where it lands (a retune moved the rate) puts the mode back, with the message. A switch made while a scan row runs a digital protocol reaches the front end when the scan stops; should a DSP-menu CQPSK toggle made before the front end took that return decide instead, or the scan go on (a channel map imported with the scanner on leaves the row, and the next row's tune decides, also when it lands before the front end took that return, which then never runs), the mode stays with no message (a refusal only logged), whether the front end stayed on the digital family or on the analog family (where an `-fA` session runs a typed row), and the next return to the monitor the front end refuses while it runs another family or kind, such as CQPSK toggled off or the scanner stopped on an `am` row, puts it back. A switch between FM and AM that a scan stop puts back this way asks the front end for the mode it restores, and when the rate a row's retune left cannot filter that mode's width either (NFM 16 kHz after a switch to AM under an NXDN48 row, left at 7.5 kHz), the width is settled as for any refused scan stop (the NFM or AM width row above): the width a monitor of that kind kept, else the kind's default, which the monitor then runs, and the message says so (`Refused: NFM 16 kHz does not fit the 7.5 kHz DSP rate; the monitor is back on the NFM default`; an AM default that rate cannot filter either leaves the AM width as it was). |
| DSP bandwidth (`bw`, `rtl_bw_khz`, DSP bandwidth...) | Reopens the device. On an I/Q replay it re-plans the replay's rate chain for the bandwidth and reopens the replay where it stopped, about a chunk later, with the decode clock kept on the capture; the bandwidth asked for is kept as asked (under TETRA the replay runs at 72 kHz and returns to it when TETRA ends), a bandwidth that plans the chain already running reopens nothing, and one no chain reaches is refused with the replay running as before (see "Run-time Re-plan" in `docs/iq-capture-replay.md`). On a radio input (RTL-SDR, rtl_tcp, SoapySDR or Airspy, each of which runs at its DSP bandwidth) a bandwidth the explicit NFM width, or the AM width (its default included), in use cannot run at is refused, naming both, from the DSP bandwidth... row and from a loaded config alike. The width in use is the configured one under `-fA` or `-fM`, the configured width of a kind on any session while the scan has an analog row or target of that kind without a width of its own, and the own width of an analog scan row on air (`DSP BW 12 kHz cannot filter the scan row's NFM 12.5 kHz (max 9.6 kHz); keep a wider DSP bandwidth` from the DSP bandwidth... row). A loaded config is checked at the bandwidth its `[input]` reopens a radio at (its `rtl_bw_khz`, or 48 kHz for a value that is not a DSP bandwidth, as at startup), from whatever input runs now, a SoapySDR or Airspy device included (an Airspy source over a running Airspy reopens it for a new sample rate, serial or DSP bandwidth); one that builds the input already running (an rtl_tcp source without `rtl_freq` for the host and port in use) reopens nothing and changes no rate. An Airspy source that changes only the volume also reopens the running Airspy, but at the rate it runs now, so the running front end checks the width first, as when nothing reopens, and checks the configured width even when the config leaves it as it was, since the reopened stream opens on it (also while a digital scan row runs, which leaves it unused): a width it cannot filter leaves the config unapplied. A bandwidth from the DSP bandwidth... row whose reopen does not start anyway (a device that does not open, or a SoapySDR device with no rate dsd-neo can resample to that bandwidth) puts the bandwidth that ran back and starts the input again, and says why: `Failed: the RTL-SDR input did not start (see log)`; with `--iq-capture` that restart runs without the capture (see IQ Capture And Replay). |
| Decode mode switch, or a loaded config's `[mode]` or `[input]` `rtl_bw_khz`, during an I/Q replay | When the new settings plan another rate chain (a switch to TETRA on a 48 kHz replay, or back), the replay reopens at it where it stopped, about a chunk later, with the decode clock kept on the capture, before the new mode runs (`Decoding TETRA: replay resampled to 72 kHz`). A plan no chain reaches is refused: the mode, symbol profile and DSP bandwidth go back and the replay keeps running (`Failed: TETRA -> ...`, `Config not applied: ...`). Otherwise nothing reopens. |
| A loaded config whose `[input]` reopens the running radio input | Reopens the device. When the reopened stream does not start (the device does not open, or its start refuses a width), the config is not applied to the receive side: the input and its device settings, the tuning, `[mode]` (the decoders and their timing, the EDACS variant, the extra LRRP ports), `[demod]` and `[analog]` (both widths and the tone filter) go back to what ran, that input is started again, and the message says why (`Config not applied: the rtl_tcp input did not start (see log)`). With `--iq-capture` that restart runs without the capture, which stops for the session, so it never writes over the recording (see IQ Capture And Replay for when the failed start already had). Under a scan row, the reopened stream opens on the configured settings, so its start checks the configured width at its DSP rate even while a digital row runs (the scan stops back onto that monitor), and the input put back after a rollback starts again on what its stream ran, the row's profile (a typed digital row's, whose rate need not fit the configured width); either stream is asked for the row's profile once the row is back. A row's own width the reopen bandwidth cannot filter leaves the config unapplied before anything reopens (see the DSP bandwidth row above); a reopened stream that still cannot run the analog row on air is rolled back the same way, with the message naming the width and the rate. A config that turns the scanner off (`[trunking] scanner = false`) leaves the row instead, so the reopened stream is not held to it. The config's other sections (output, trunking, logging, alerts, recording, DSP, a group list) stay applied. With no stream running before the load, the config's input is still opened; when that start fails there is nothing to put back, so the config stays applied with no input running, and the message says why (`Config applied; no input running: the SoapySDR input did not start (see log)`); an input it opens that cannot run the scan row on air is kept, with the whole config (its `[mode]` and `[analog]` widths too, even the width a row without its own runs and the new input refuses), and the message says so (`Config applied; the scan row cannot run: ...`). |
| Input > Switch source > RTL-SDR or Airspy | RTL-SDR reopens the input as an RTL-SDR, rtl_tcp or SoapySDR device at the DSP bandwidth, and Airspy opens the Airspy at it (a SoapySDR or Airspy device's own rate is resampled to the bandwidth). An explicit NFM width, or the AM width in use, that bandwidth cannot filter, the configured one (under `-fA` or `-fM`, or while the scan has an analog row or target of its kind without a width of its own) or that of an analog scan row on air, is refused, naming both and the DSP bandwidths that fit, and the running input stays; `DSD_NEO_CHANNEL_LPF=0` is checked first over the same widths. The start then checks the width it opens on, the one in force (the configured one under `-fA` or `-fM`, or that of an analog scan row on air). The switch leaves the scan row on air in place, so under a row that runs a digital protocol it opens on that row's digital profile and checks no analog width, and a configured width the bandwidth cannot filter that the switch did not hold is left to the scan: its return to the monitor when the scan stops is refused and settled as the NFM or AM width row above describes, and its analog rows or targets that run that width are skipped at every visit. A switch whose start fails anyway (a device that does not open) puts the input it replaced back, restarting it when it was a running radio stream (a PCM input was never closed; a radio stream that ran behind it stays stopped), and says why: `Failed: the Airspy input did not start (see log)`. With `--iq-capture` the restart runs without the capture, which stops for the session, so it never writes over the recording (see IQ Capture And Replay for when the failed start already had). Once the input it replaced runs again, a device error the failed start reported (an Airspy that did not open) is dropped, so the session's exit status is what it was before the switch; a DSP bandwidth, a loaded config and an Airspy setting that roll back do the same. |
| A radio stream restart while a scan row is on air: Input > Switch source, a DSP bandwidth, an Airspy setting, a gain, Device index..., Restart stream, or the restart of the input one of these puts back when its start fails | The new stream opens on the row's settings, but an open picks CQPSK or the FSK discriminator the way it does at startup (as `DSD_NEO_CQPSK` says when set), while the row's own tune ran the row's modulation. So the stream is asked for the row's profile as soon as it runs, timed for the rate it runs the row at: under `DSD_NEO_CQPSK=1` a P25 C4FM row is back on the FSK discriminator, under `DSD_NEO_CQPSK=0` a CQPSK row back on CQPSK, and an `nfm` or `am` row keeps the monitor it opened on. The decoder keeps its acquisition, and a restart that puts an input back resets nothing as for a new input. A loaded config under a scan row asks the same once the row is back (its row above). |
| A retune (scanner, trunking, manual tune) | The channel filter, de-emphasis, audio filter and squelch start from empty state on the new channel; a retune that lands on another DSP rate resolves the channel for that rate. |
| `--nfm-bandwidth-hz`, `--am-bandwidth-hz`, `[analog] nfm_bandwidth_hz`, `am_bandwidth_hz` at startup | When the stream opens. |
| Tone filter (`--tone-allow`, `--tone-block`, `--no-tone-filter`, `[analog] tone_filter` and `tone_list`) at startup | From the monitor's first read of audio. |
| Tone filter in a config loaded into a running session | From the next read of the monitor's audio: a policy that differs from the one in force starts the transmission on air over, muted for a fresh check; a list that only respells a code (`D047N` for `D023I`, one signal) is the same policy and changes nothing. A present `[analog]` section sets both keys, so one without them turns the filter off. While an `nfm` scan row with its own policy is on air, the loaded policy becomes the configured default and applies when the row leaves. |
| Tone filter from the terminal (Audio > Tone filter...) or the Qt/Android editor (the monitor's `TONE FILTER` row, Edit) | From the next read of the monitor's audio, as for a loaded config: a policy that differs from the one in force starts the transmission on air over, muted for a fresh check. A list the parser refuses, or allow or block without a list, is refused with the reason and changes nothing. The edit sets the configured policy: while an `nfm` scan row with its own policy is on air the row keeps it, the message says so, and the edit applies from the next row without one or when the row leaves. |

### De-emphasis and squelch

- The width changes only the channel filter. De-emphasis stays at its 75 us default (`DSD_NEO_DEEMPH`), so existing
  `-fA` audio sounds the same; `DSD_NEO_DEEMPH=nfm` selects the 750 us land-mobile curve (212 Hz corner).
- The channel squelch measures power after the channel filter, so the noise it sees scales with the width: halving
  the width lowers the noise power by about 3 dB. Re-check a squelch threshold (`sql`, `rtl_sql`) after changing the
  width. The auto and noise squelches (below) need no re-check: the auto squelch rescales its floor with the filter,
  and the noise squelch recalibrates on the new filter.

### Auto squelch (`--squelch auto`)

A fixed squelch threshold belongs to one receiver: the dongle, antenna, gain and channel width set the noise floor,
so a level that keeps noise out on one setup opens on noise, or chops weak traffic, on another. `--squelch auto[+N]`
opens the analog monitor N dB over the noise floor the demodulator learns on each channel (whole dB, 3 to 30; `auto`
alone is `auto+10`), so one setting suits every receiver and every channel of a scan.

- `--squelch <setting>` takes the squelch grammar: `off` or `0`, a negative level in dB (`-60`), a positive linear mean
  power (the legacy `sql` form), `auto[+N]`, or `noise[+N]` (the [noise squelch](#noise-squelch---squelch-noise)). It
  works with `--iq-replay`, which has no spec field, and wins over the `sql` field of an `rtl:`, `rtltcp:`, `soapy:` or
  Airspy spec (which takes `auto[+N]` and `noise[+N]` too), which wins over `[input] rtl_sql_mode` and
  `rtl_sql_margin_db` in a config (`docs/config-system.md`). The terminal's Input > `Squelch...` row and the Qt/Android
  radio panel's `dB | Auto | Noise` choice set it live; on audio input the Qt/Android monitor's `SQUELCH` row offers
  `dB | Noise`.
- Learning. The demodulator classes each 40 ms window of the channel-filtered I/Q as noise, carrier or undecided from
  its envelope and phase statistics, and takes the floor from noise windows only (5 of the last 8). Until it has one
  (`learning`) only carrier windows open, so landing in the middle of a transmission plays it. A carrier is never
  learned as the floor: a channel with no noise windows (a continuous carrier or a birdie) stays learning and plays,
  as with the squelch off; lock it out, or let `--scan-max-visit-ms` move a scan on.
- Gating. With a floor, the gate opens on a 20 ms stretch at the floor plus the margin and closes 3 dB lower or on a
  noise window. It is decided per sample and travels with the audio, so the edges fall on the right samples whatever
  the read sizes: the monitor ramps in over 5 ms and out over 10 ms to exact silence, and the analog AGC holds while
  the gate is closed. The floor follows the noise down quickly and up at 1 dB a second, and is learned again after
  5 s of noise more than 6 dB over it, or a window 20 dB under it.
- Per channel. Floors are kept for each frequency, tuner gain (an Airspy's gain settings), tuner AGC state, bias tee,
  device and channel rate for 30 minutes of received time, digital and fixed-level time included, so a scan coming
  back to a channel picks its floor up again; a channel within 5 MHz seeds a new channel's floor until three noise
  windows confirm it. A stream restart or retune starts the windows over and keeps the floor of the same channel.
- Where it runs: the analog monitors (`-fA`, `-fM`, `nfm` and `am` scan rows, `nfm-conventional` and
  `am-conventional` targets) on a radio input (RTL-SDR, rtl_tcp, SoapySDR, Airspy, `--iq-replay`). On a digital
  channel it is off: CRC and FEC already decide what is traffic there, and a control channel never shows its noise. On
  audio input (PCM, WAV, TCP, UDP) it has no channel power to learn a floor from, since FM noise plays louder than
  voice there: it is off, the engine says so at startup, and the terminal and Qt/Android editors refuse it. Use the
  [noise squelch](#noise-squelch-on-audio-input) or a level there.
- Two more readers of the gate on a radio input:
  - The M17 encoder's monitor: its VOX keys on the gate, and the closed samples reach the encoder as silence (see
    [M17 Encoding](#m17-encoding)).
  - EDACS analog voice: the tracker runs on the 9600 bit/s FSK path EDACS reads. A call ends once the gate has stayed
    closed for the level squelch's hold, less the 85 ms the gate can take to close: about 155 ms. EDACS runs analog
    voice at 48 kHz whatever rate it reads (the presets' 24 kHz, or an unresampled I/Q replay), and each converted
    sample keeps the gate's flag, so the hold is the same at every rate, and a carrier that drops ends no later than
    under a level squelch.
  - The hold is never under 80 ms, two of the squelch's windows, so neither the window a call's first decision takes
    nor one window read closed ends a call.
  - The tracker runs on the EDACS control channel too, a continuous carrier it never takes as its floor, and EDACS
    reads the gate only during a call.
  - The closed samples play as silence. The release watchdog applies only where nothing decides the call: no squelch,
    a dynamic setting on audio input, or no channel plan.

  On audio input the auto squelch runs for neither: the encoder's VOX has no squelch there (the engine warns; use a
  level), and an EDACS call ends on its release marker or the watchdog.
- What it shows: the terminal channel row reads `SQL: auto +10 dB (floor  -78.3 dB)`, `(learning)`, `(off: no radio
  input)`, `(off on digital)`, or in the rare front end it has no plan for `(off: no channel plan)`. On EDACS it reads
  `(EDACS analog calls)` between calls and the tracker's reading during one. The startup banner shows `SQ=auto+10dB`,
  and the DSP panel and the Qt/Android radio panel show the same.
- Scans: a row or target's own `--squelch auto[+N]` (`nfm` and `am` rows, `nfm-conventional` and `am-conventional`
  targets) replaces the default while it is on air, as `--squelch-db` does. Under the auto squelch an analog row's
  noise no longer holds the scan, so the open-squelch warning does not apply to it. See `docs/csv-formats.md`.

### Noise squelch (`--squelch noise`)

`--squelch noise[+N]` opens an FM channel when the noise its discriminator puts out above the voice band quiets by N dB
(whole dB, 3 to 30; `noise` alone is `noise+10`), as a radio's noise squelch does. A carrier quiets that noise by about
its carrier-to-noise ratio whatever the dongle, gain or antenna, so there is no floor to learn: the gate opens on the
first 40 ms of a transmission, on a channel it has never heard. N is dB of quieting, the measure FM receivers state
their sensitivity in.

- Measurement. The demodulator band-passes the discriminator's output from 3.8 kHz to just under the channel edge (the
  channel filter's -1 dB point less 800 Hz), in 300 Hz sub-bands (at most fifteen) and a second set staggered half a
  sub-band between them, and compares each 40 ms window's power, every 20 ms, with what noise alone puts there through
  the same filters (calibrated on the channel's own taps and the decimation stages ahead of them). Noise reads about 0
  dB; a carrier about its in-channel CNR plus 2-3 dB on a 12.5 kHz channel, so `noise+10` opens at about 7-8 dB CNR
  there (a wider channel reads more quieting at the same CNR). A strong tone's harmonics land in the band as lines when
  the channel filter truncates its sidebands; the reading takes the best-quieted band-pass, less 4 dB, where that beats
  the whole band, so full-deviation tones and speech never close the gate. `docs/testing.md` "Noise squelch design gate"
  has the figures.
- Gating. The gate opens on a window reading N or more and closes on one under max(N - 3, 1.5) dB: 3 dB of
  hysteresis, with the close threshold held to at least 1.5 dB so `noise+3` still closes on noise, which reads about
  0 dB. Like the auto squelch it is decided per sample: the same ramps, analog AGC hold and scan hold apply.
- Where it runs: the FM monitor (`-fA`, `nfm` rows, `nfm-conventional` targets) on a radio input. A channel has no room
  for the band when the band, from 3.8 kHz to the lower of its edge less 800 Hz and 0.45 times the channel rate (the
  rate before any post-decimation), is under 1200 Hz wide (the 8 and 10 kHz NFM widths, or a low DSP rate). Nor does a
  channel with no channel filter behind the half-band decimation at a channel rate of 16 kHz or less (the width unset at
  those rates, or `DSD_NEO_CHANNEL_LPF=0`): the decimation stages' gentle roll-off is then its only edge and would
  truncate a wide signal into the band; a replay with no decimation keeps its band. The auto squelch runs the setting on
  such a channel, with N as its margin, and the readout says `as auto`. AM has no discriminator: `-fM --squelch noise`
  stops at startup (`--squelch noise needs an FM channel and -fM is AM: use --squelch auto[+N] or a level.`), a noise
  setting from an input spec or the config runs as auto on the AM monitor and says so, the radio panel offers no Noise
  there, an `am` row or `am-conventional` target refuses `--squelch noise`, and an `am` row that inherits a noise
  default runs it as auto. Like auto, it is off on digital channels. On audio input it learns its reference from the
  input itself ([below](#noise-squelch-on-audio-input)).
- The M17 encoder's monitor runs it as the FM monitor does, or as auto where its channel has no band.
- EDACS analog voice runs it as auto, and the engine says so (`EDACS analog voice runs the noise squelch as
  auto+NdB`): the FSK discriminator EDACS reads puts out peak-normalised levels, not the radians the noise squelch is
  calibrated on.
- On audio input neither runs it: the PCM noise squelch below serves the FM monitor only.
- What it shows: `SQL: noise +10 dB (quieting 23 dB)`, `(starting)` before its first window, `(as auto: floor
  -78.3 dB)` where the auto squelch stands in, and the same `off` readings as auto; the startup banner
  `SQ=noise+10dB`; the Qt/Android radio panel's `dB | Auto | Noise` choice, whose buttons step N.
- Scans: a row or target's own `--squelch noise[+N]` (`nfm` rows, `nfm-conventional` targets) replaces the default
  while it is on air. See `docs/csv-formats.md`.

### Noise squelch on audio input

On audio input (Pulse, WAV and other files, stdin, TCP and UDP audio) the FM discriminator ran outside DSD-neo, so its
gain and filters are unknown and there are no channel taps to calibrate against. `--squelch noise[+N]` there learns
what noise alone puts above the voice band from the input itself, then gates as on a radio input: open at N dB of
quieting, closed under max(N - 3, 1.5) dB. Quieting reads about the carrier-to-noise ratio plus 1-2 dB.

- What to feed it: audio with the noise above voice left in, which is what a discriminator gives before any audio
  filtering: a scanner's discriminator tap, `rtl_fm` (`rtl_fm -M fm -s 48k ... | dsd-neo -i - -s 48000 -fA --squelch
  noise+10`), or SDR++, SDR# and similar programs with their audio low-pass off. De-emphasis is fine. The band is
  3.8 kHz to the lower of 6.5 kHz and 0.45 times the input's own rate, in 200 Hz sub-bands, and needs 1200 Hz: a
  12 kHz input has room (3.8-5.4 kHz), an 8 or 9.6 kHz input does not.
- Learning. Each 40 ms window, every 20 ms, measures the sub-bands above voice and the voice band (400-2600 Hz, and
  its four parts). Until four windows hold steady within 1.5 dB the squelch reads `learning` and stays closed; that
  first steady stretch becomes the reference, about 0.15 s into noise. A carrier can only quiet the band, so noise is
  the loudest it gets: when the band rises 4 dB or more from one steady stretch to the next, the louder side is noise
  and the reference moves there if it came up near the reference. Noise at a lower volume (the source turned down in
  noise, during a transmission or across a pause) reads quieter than the reference but keeps noise's spectrum: the
  voice band and each of its parts move as far as the band above it, and the band does not tilt. Once 0.4 s of it
  holds that spectrum, the reference steps down by the move, steps of 1.5 dB and more included; a step straight out of
  noise that moves the voice band as much as the band above it (the source's volume or AGC) is taken after 0.2 s. A
  carrier's own noise falls away toward the low voice frequencies, and speech or a tone fills some parts and not
  others, so a carrier never gathers that evidence and any stretch of it under the reference leaves the reference
  alone. A carrier that relays noise (a repeater passing a weak user's hiss) at just the level of the source's own noise
  does read as noise turned down; its speech then shows a carrier under the threshold, which undoes the step within a
  fraction of a second (through a pause or a passband switch too). Relayed noise in that carrier's later pauses steps it
  down again only after 0.6 s of them, and the step is then contested: speech under the threshold undoes it even when it
  is lighter than the relayed noise, while a filter switched on in the noise, or a volume that pumps, does not. Weak
  traffic on noise genuinely turned down (a carrier under the threshold) reads just as that speech does, and nothing
  tells the two apart: it undoes the step too, and plays; the noise after it steps the reference down again, or takes
  the step back, within about a second. Undoing takes a voice band that moves, so a steady tone or line in it seldom
  does; audio added after the discriminator that switches on and off, though, reads as modulation and can undo a step
  and hold the gate open on noise while it keeps switching. Between transmissions the reference follows the noise with a
  1 s time constant.
- Starting on a carrier: with no noise to learn from, the first steady stretch (the carrier, or a pause in its speech)
  becomes the reference, which is too low, so the gate stays closed or opens only in snatches until the first noise
  corrects it. A channel that never shows noise (a continuous broadcast such as a weather station) never settles that
  way: use a level for it. A steady tone strong enough to leave nothing above voice is the exception: it reads as no
  band (below) after about a second and then plays, as with the squelch off. A carrier held open 5 s with nothing
  changing in the voice band (an unmodulated or steady-tone carrier) is learned again, so it mutes after 5 s; speech
  keeps the gate open, and speech that the carrier carries later undoes that and plays.
- No band. When the input carries nothing above voice (its audio is low-passed, as most SDR programs' audio is by
  default) the squelch sees it within about 1 s of steady input and reads `off: no band above voice`: the monitor plays
  as with the squelch off, and the log says once `NOTICE: Noise squelch: nothing above the voice band on this input
  (its audio is low-passed); the squelch is off. ...`. An input whose rate leaves under 1200 Hz of band reads `off: no
  room above voice` and logs that at first use. Under `-Y` or a trunk scan the notice adds that rows hold on noise until
  `--scan-max-visit-ms`. A band that appears later turns the squelch back on after about 1 s.
- References per source. The squelch keeps up to eight references, one for each input stream, input rate, input volume
  (`--input-volume`, Input > `Input volume...`) and rigctl peer passband (`-B`, the configured NFM width, or a scan
  row's width), so a `-Y` scan over rows of different widths keeps each one's reference. A retune keeps the reference;
  reopening the input (an input switch from the terminal menu included, or a UDP input bound again), or a new rate or
  input volume, starts learning again. While the peer's passband is not known the squelch keeps one reference, through
  retunes too, but stores none for it to come back to; the first passband the client learns, and any change of it (a
  width or `-B` edited live, a row's or the configured one), starts the windows over on that passband's reference. A
  width request whose reply was lost leaves the passband unknown again, so the squelch learns afresh rather than keep
  the old width's reference. A pause in the stream (UDP or TCP audio that stops) starts the windows over, at most one
  20 ms read late.
- Where it runs: the FM monitor (`-fA`, `nfm` rows) on audio input. An `am` row on audio input runs no noise squelch
  (`off on AM audio`), and digital channels none. `--squelch auto` on audio input is off, with a startup warning. A
  level squelch works on audio input as before, against the input level.
- What it shows: `SQL: noise +10 dB (learning)`, `(quieting 23 dB)`, `(off: no band above voice)` on the terminal's
  input line, and the same in the Qt/Android monitor's `SQUELCH` row, whose editor offers `dB | Noise`.
- Scans: on a `-Y` scan over audio input (rigctl tuning an external receiver) an `nfm` row's noise no longer holds the
  scan while the noise squelch runs (it is closed while learning), as on a radio input; with no band or no room the
  row holds on noise as with the squelch off. `docs/testing.md` "PCM noise squelch design gate" has the figures behind
  these numbers.

### Native AM (`-fM`)

`-fM` receives AM voice (airband, HF broadcast and utility AM) by demodulating it from the radio's I/Q. It is its own
decode preset (`[mode] decode = am` in a config, "AM" in the terminal decoder picker and the Qt decode chips, entry 15
of the setup wizard): the analog monitor with an AM envelope detector in place of the FM discriminator.

- Inputs: only inputs that deliver I/Q to DSD-neo's own front end run it: RTL-SDR (`-i rtl`), `rtl_tcp`
  (`-i rtltcp`), SoapySDR (`-i soapy`), Airspy (`-i airspy`) and `--iq-replay`. A replay that keeps a sidecar's
  recorded chain decimating after the demodulator (`post_downsample` above 1) runs AM too: the channel filter and the
  detector run ahead of that decimator, at the rate they are designed for.
  PCM inputs (Pulse, WAV and
  other files, stdin, symbol files, TCP and UDP audio) arrive already demodulated, so `-fM` on one stops at startup
  with `AM demodulation needs an I/Q radio input; monitor externally demodulated AM audio with -fA`. That is the
  existing contract for such audio: `-fA` plays PCM input as it arrives, whatever demodulated it (an SDR program's AM
  output over TCP, say). A config whose `[mode] decode = am` meets a PCM input logs the same text and runs the Analog
  monitor for that session instead, with autosave off so the saved `decode = am` is kept; a toast says so, since
  nothing else the session changes is saved either. A decode-mode change to AM on a PCM session (terminal picker, Qt
  chip) is refused with the same reason; the terminal picker's AM entry reads `AM (needs an I/Q radio input)` there
  and the Qt chip is greyed out. A runtime config apply with `decode = am` on a PCM session (terminal Config > Load)
  applies, and the session runs the Analog monitor with that reason and autosave off, as a start does. A live switch of
  a running AM session's input to a PCM one (Pulse, a file, TCP or UDP audio) also falls back to the Analog monitor with
  that reason, but autosave stays on: the operator moved the session there, and autosave keeps the new input with the
  mode it runs.
- Channel width: `--am-bandwidth-hz <Hz>` (or `--am-bandwidth-hz=<Hz>`, `[analog] am_bandwidth_hz`) sets the AM
  channel filter, a whole number of Hz from 5000 to 20000; the default is 6000. It is the full RF passband centred on
  the tuned frequency, with the NFM width's filter response (see [Channel width](#channel-width) above), and it is
  neither the tuner bandwidth nor the DSP bandwidth nor the audio bandwidth. A value outside 5000..20000, or not whole
  Hz, is refused, never clamped: `--am-bandwidth-hz: AM bandwidth 25000 Hz is outside the supported range of 5000 to
  20000 Hz`. On a PCM input the option filters nothing, and says so; with a rigctl peer (`-U`) it is the AM passband
  the peer is asked for on `am` scan rows and `am-conventional` targets that set no width of their own, and the start
  says that instead. An edit of it there (Input > `AM passband...`, the Passband sheet, a loaded config) reaches the
  peer at once while such a row is on air, and goes back if the peer refuses it.
- The width has to fit the DSP rate as an NFM width does (the table above), and the unset default is held to it too,
  since AM always runs its channel filter; `DSD_NEO_CHANNEL_LPF=0` therefore refuses AM. A radio input's
  rate is its DSP bandwidth, so a width it cannot filter is refused at startup before the device opens, and without a
  dongle plugged in: `AM bandwidth 20 kHz does not fit the 16 kHz DSP rate (the largest width it fits is 13.2 kHz);
  set the RTL DSP bandwidth to 24, 48 or 72 kHz`, and a non-zero exit (`-fM -i rtl:0:118.1M:22:0:16 --am-bandwidth-hz
  20000`; `-fM` alone at a 6 kHz DSP bandwidth likewise). The interactive setup's AM entry is held to the same rule:
  after an RTL-SDR or rtl_tcp source at a 4 or 6 kHz DSP bandwidth (or one the configured AM width does not fit) it
  prints that text and asks for the decode mode again. An I/Q replay is checked when the stream starts, against the
  rate its plan runs at. While AM is the configured mode, the DSP bandwidth, a config's
  `[input]` and Input > Switch source > RTL-SDR are held to its width as they are to an explicit NFM width, and, while a
  `-Y` list or `--trunk-scan` has an `nfm` row or target without a width of its own, to the configured NFM width that
  row runs as well. On any other session (digital, or `-fA`) the same holds the configured AM width, the 6 kHz default
  included, while the list has an `am` row or `am-conventional` target without a width of its own, and the width in
  force while an `am` row with its own `--am-bandwidth-hz` is on air.
- Level: the detector divides the envelope by its own carrier estimate (a 50 ms average of the carrier), so the audio
  level is the modulation depth whatever the signal strength: 100% modulation peaks where live FM does at about 6 kHz
  deviation. A squelched block is silence and leaves the carrier estimate where it was, so audio resumes at its level
  when the squelch opens after a fade; a squelch closed longer than 100 ms ends the transmission, and whatever opens it
  next (usually another station at another level) is measured from its own first block, as after a retune, a mode
  change and a stream start. AM runs without de-emphasis. The I/Q DC blocker (`DSD_NEO_IQ_DC_BLOCK`, `[dsp]
  iq_dc_block`) would remove a carrier tuned to 0 Hz, and I/Q balance (`DSD_NEO_IQ_BALANCE`, `[dsp] iq_balance`) would
  read that carrier as an image and cancel it with its sidebands, so both stay off while AM runs, each with a one-time
  log note; they apply again to FM.
- Received-tone detection (CTCSS and DCS, below) is FM signalling and does not run for AM. The AM monitor's carrier is
  still kept as the FM monitor's is: under `-Y` a blank row of an `-fM` session holds on it under `-t`, as an `nfm` row
  does, whether or not audio plays (`-o null` and a muted frontend included), and the Scan Timing row reads `Carrier`
  while it holds.
- Live changes follow the [When changes apply](#when-changes-apply) table: switching between AM, the Analog (FM) monitor
  and the digital modes from a frontend applies to the running stream without reopening it, and so does a new AM width
  (Input > `AM bandwidth... [default]` in the terminal, or the Radio sheet's channel width stepper under the
  AM chip in the Qt and Android app; the terminal row takes any value, and the engine refuses what the rate cannot
  filter). Under `-Y` scanning the width is held to the running rate too, and it always edits the configured AM width,
  which a save writes and every row's leave keeps: on a blank row, which keeps the configured mode (its retune brings
  the AM monitor back after a digital or `nfm` row), it applies live; under a row with its own decode mode (a digital
  row, or an `nfm` row, which runs the FM monitor at its own or the configured NFM width) the row's profile runs, so the
  width applies when the scanner returns to a blank row. An `am` row runs the AM monitor on any session, at its own
  `--am-bandwidth-hz`, which an edit leaves in force until the row leaves, or at the configured AM width, which an edit
  reaches live. While an `nfm` row is on air the terminal and the Radio sheet read its NFM width, and the AM width keeps
  its own control. The terminal's channel row shows the width in force
  (`Analog: AM 6 kHz (default);`) after the squelch, and the Radio sheet shows the same reading.

### Received tone (CTCSS) on the analog monitor

While the passive analog monitor runs (`-fA`, which enables input monitoring), DSD-neo listens below the voice band
for a CTCSS (PL) tone and reports what it hears. Detection only reports: it never mutes or gates audio, it needs no
tone setting, and it runs with `-o null` too. Muting by tone is the separate
[tone filter](#tone-filter-ctcssdcs-receive-policy).

- Supported tones: the 50 tones radios commonly offer (the 39 of TIA-603-E and 11 more), 67.0-254.1 Hz (67.0, 69.3,
  71.9 ... 250.3, 254.1), and 150.0 Hz, which many radios offer as a 51st. A tone is confirmed from estimates within
  0.5 Hz of a table value, or further off
  by a transmitter's tone error (below), and a confirmed tone is held only while it stays within 0.8 Hz of it, or 0.5 %
  of it from 162.2 Hz up while its estimate is steady enough to say so -- 0.7 Hz for 150.0 and 151.4 Hz, which sit 1.4
  Hz apart, so no estimate is ever within reach of both. Any other frequency within the sub-audible band, such as 68.2
  Hz, reads as no tone rather than as its nearest neighbour, and a confirmed tone that moves off the table is dropped
  within about half a second. Near 0 dB in-band a noisy estimate can still confirm a neighbour for 200-260 ms before
  the same
  check drops it: over two hours of a continuous 0 dB carrier in offline seed sweeps, 68.2 Hz read as 67.0 or 69.3 Hz
  about ten times an hour and 161.0 or 166.7 Hz as a neighbour two or three times an hour; from 3 dB up 68.2 and 161.0
  Hz never did. 150.0 and 151.4 Hz are confirmed only after one more agreeing estimate while the estimate leans more
  than 0.3 Hz toward the other tone, which costs them some confirmation time (at 0 dB the p95 is about 350 ms on the
  table value and 390 ms for a radio 0.2 Hz off toward the other). They never read as each other on their values over
  two hours of 0 dB carrier; a radio 0.35 Hz off toward the other one did, one to five times in two hours. At the start
  of a transmission the other tone of the pair can still be named for up to 240 ms before the right one takes over: at
  0 dB, about once in 100,000 starts on the table value, once in 9,500 for a radio 0.2 Hz off toward the other and
  once in 1,400 for one 0.35 Hz off toward it. DCS (DPL) signalling does not read as a CTCSS
  tone: in the tests no DCS code did on a clean signal, nor did the codes nearest to a table tone at 10 or 0 dB in-band.
  Nor does a steady tone in the voice band, such as a 2300 Hz test tone alone on a clean carrier: a tone is confirmed
  only while it carries at least 1/100,000 (-50 dB) of the input's total power, far above what the decimating front end
  folds down from the voice band and far below any real CTCSS tone.
- Transmitter tone error: a tone slightly off its table value still reads as that value. Over 10,000 seeded starts
  each, tones of the table 0.2 and 0.35 Hz off were confirmed within 400 ms at 10 dB in-band tone-to-noise on
  all starts and on all but 1 (the slowest at 331 and 424 ms), and 0.2 Hz off at 0 dB on 98 starts in 100 (the slowest
  at 553 ms); 150.0 and 151.4 Hz set off toward each other take a little longer (above). Encoders are specified to
  0.5 % of their tone, and some radios run near that: the ones in issue #643 sent 150.6 Hz for 150.0. A tone up to 0.5 %
  off its table value (never past half the distance to its neighbour) reads as that value once a steady measurement of
  250 ms or longer places it there precisely enough, and no tone on its value is being confirmed instead: 150.6 Hz reads
  `CTCSS 150.0 Hz`, 150.8 Hz reads 151.4 Hz, and a tone midway between them (150.68-150.72 Hz) reads neither. Below 100
  Hz, where 0.5 % is less than 0.5 Hz, nothing changes. How soon depends on the level: from the start of a
  transmission at 20 dB in-band or better, 95 % of tones 0.4 % off are confirmed by the eighth 50 ms step, about 400
  ms (about 500 ms for 150.0 and 151.4 Hz, which need one more agreeing estimate while they lean toward each other),
  and every one within 650 ms; at 10 dB 150.6 Hz is confirmed in about 750 ms on 95 % of starts and within 1 s on all
  of them; at 0 dB in about 1.6 s on 95 %. A
  tone further off than its tolerance still reads `none`. The tone filter waits for its window to end before it rejects
  a transmission on a tone confirmed off its value (see [Tone filter](#tone-filter-ctcssdcs-receive-policy)).
- What is shown: the terminal's Call Info section carries an `Rx tone:` line (compact view too), and the Qt/Android
  monitor a `RECEIVED TONE` row. Both read `CTCSS 100.0 Hz` once a tone is confirmed, `detecting` while a carrier is
  being evaluated, `none` when the carrier carries no supported tone, and an em dash with no carrier (a hyphen on a
  terminal without UTF-8). The log prints `Received tone: CTCSS 100.0 Hz` or `Received tone: none` whenever that
  verdict changes.
- Timing contract, in sample time, for a tone in noise at 0 dB in-band tone-to-noise (0-290 Hz) or better, on its table
  value or up to 0.2 Hz off it (0.35 Hz at 10 dB): 95% of tones are confirmed within 400 ms of their start and 95% of
  stops under a live carrier are dropped within 350 ms (the p95 targets). Single events have ceilings of 700 ms for a
  start and 800 ms for a stop, above the slowest of the long-run sweeps, and the tests hold every timing row of their
  fixed seeds within both. Over 1,000,000 seeded starts per condition at 0 dB, on the table value and 0.2 Hz off it, none
  took longer than 700 ms (the slowest 651 ms), nor did any of 100,000 per condition at 10 dB (the slowest 452 ms). No
  stop of 800,000 at 0 and 10 dB took longer than 800 ms (the slowest 738 ms). Lock time in noise has no absolute bound:
  now and then noise keeps the usual 250 ms measurement from confirming a tone for well over half a second (on its own
  it took longer than 700 ms on about one start in 125,000 at 0 dB, the slowest 1.1 s), so while no tone is confirmed
  the detector also measures the last 400 and 600 ms, which average that noise away. Speech louder than the tone is
  outside the contract (below).
- Timing measured, in sample time, over 10,000 seeded starts per condition: a tone at 10 dB in-band is confirmed within
  400 ms of its start on every start (p95 255 ms, the slowest at 331 ms), and at 0 dB on all but about 3 starts in
  1,000 (p95 341 ms, the slowest at 447 ms). Under transmitted speech it can take longer, because a
  transmitter's voice high-pass still lets a high voice's fundamental leak below 300 Hz in bursts: with speech 10 dB
  louder than the tone, over 50,000 seeded starts 95% were confirmed within 301 ms, 1.4% took longer than 400 ms and
  0.16% longer than 700 ms (the slowest 1.3 s), and twice a voice holding 254.1 Hz read as that tone for 150-250 ms,
  once in place of the real tone. Over 100 minutes of that speech a confirmed tone at the top of the table (250.3 or
  254.1 Hz) was lost four times, reading `none` for under a second before it was confirmed again. A tone that stops
  while the carrier stays up is dropped within 350 ms on 98 stops in 100 at 0 and 10 dB alike (typically after
  200-315 ms; the slowest of 8,000 took 515 ms). When the transmitter sends a reverse burst (the end-of-message phase
  flip: 180 degrees, or 120 or 240 degrees on some radios) at 10 dB or more in-band, the tone is dropped within 150 ms:
  all of 40,000 bursts at 180 degrees (the slowest took 123 ms), and all but 16 and 14 of 40,000 each at 120 and 240
  degrees. Near 0 dB about one 180 degree burst in fourteen, and one 120 or 240 degree burst in six, is caught late or
  missed. A missed burst leaves the tone to the carrier drop that follows, which ends it within the 200 ms hangover. A
  carrier with no tone reads `detecting` until 500 ms of it have been evaluated and `none` by the next 50 ms step; a
  tone that starts after that, such as one a repeater adds after its kerchunk, is still confirmed within 400 ms of its
  own start. A tone that stops under a carrier that keeps dropping out, each time for less than the 200 ms hangover
  (squelch chatter on the noise after a transmission, say), is still dropped, though later than under a steady
  carrier: over 11,264 seeded stops under such carriers (openings of 1 to 60 ms between dropouts of 10 to 199 ms), 95%
  were dropped within 465 ms and all within 800 ms (the slowest 693 ms).
- The received tone is forgotten when the receiver moves or the session changes: a frequency set from a frontend or a
  spectrum tap, any other retune the RTL stream or the tuning hooks report (UDP retune, rigctl-driven tuning), a
  change of the analog channel filter the RTL front end runs (its width, or whether it runs), a
  `-Y` scan step (including one that failed after rigctl had already moved the radio), a manual channel cycle or scan
  avoid, a scan row or trunk-scan target change, a decode-mode change, an input switch (Pulse, WAV, UDP, TCP, RTL or
  symbol input, replay and stop-playback, from a frontend or a config apply, and the switch to live Pulse input when
  a WAV file ends), a dropped TCP audio connection, whether DSD-neo reconnects it or falls back to live Pulse input,
  stop, 200 ms without carrier, and an input that stops delivering audio for about half a second: a stdin, UDP or TCP
  stream (see below), or a live radio stream whose source stopped, such as an `rtl_tcp` server that went away while
  DSD-neo retries the connection, or a stalled device. After half a second of such an outage the row shows no carrier,
  and when the stream returns it starts a new reception; IQ replay never counts as paused, however slowly it is read.
  The frequent no-carrier cleanup between syncs does not clear it. On Pulse, stdin, UDP and TCP input such a boundary
  also skips the audio the input had already queued, which the old channel went on filling while a rigctl retune held
  the decoder, also when the boundary came in a digital mode and detection starts with the switch to the analog
  monitor: detection hears nothing until a read shows the input ran dry (20 ms or more of it that took at least half as
  long to arrive, not counting time spent playing the monitor audio, which on stdin input holds the decoder at
  real-time pace, and for at least an eighth of which DSD-neo waited on the input; a queue it drains never makes it
  wait, however long other work such as syncing a `-6` raw WAV to a slow disk holds it), and not that read either.
  With nothing queued that costs the new channel two 20 ms reads, unless other work holds DSD-neo for more than about
  17 ms of every 20 ms (then it waits on the input for less than an eighth of each read, and the new channel is heard
  after the 2 s limit). No more than 2 s of input is ever skipped, so stdin fed from a file faster than real time is
  heard again after that. Files and RTL-family streams are not skipped: a file holds no other channel, and an
  RTL-family stream clears its own output at a retune.
- Where it runs: analog-only decoding with input monitoring, on PCM inputs (TCP, UDP, Pulse, WAV, stdin) or on an
  RTL-family stream that outputs monitor audio. It does not run for the AM monitor (`-fM`), for the `-8` source monitor
  during digital decoding, for EDACS analog voice, or on symbol-file input, and the `Rx tone:` line and `RECEIVED TONE`
  row are shown exactly while it runs. The front end needs an input rate from 2400 Hz up to 320 kHz; outside that range
  detection logs that it is inactive, once each time the input moves to such a rate, and the row is left out. Detection
  reads the input at least every 20 ms of it, whatever the length of the blocks the monitor handles audio in (on PCM
  input 960 samples at the rate the monitor runs at: 20 ms at 48 kHz, which 8, 9.6, 12, 16 and 24 kHz input is brought
  up to first, but 384 ms at 2500 Hz; what the monitor plays is converted to 48 kHz after that), so at every supported
  rate the verdict the decoder publishes trails the times above by at most two such reads. The frontends show it at
  their next refresh: the decoder hands them a new snapshot at most every 50 ms while it hunts for sync, the terminal
  redraws at up to about 15 frames a second, and the Qt/Android monitor polls every 250 ms by default, so the screen can
  trail the published verdict by a few hundred milliseconds more. On RTL input, detection hears the monitor audio after
  the RTL monitor gain (`vol`), so a gain of 0 leaves it nothing to hear and it reads no carrier; to silence the
  monitor, mute the output instead.
- Externally demodulated audio (PCM inputs): the tone has to survive the producer. Feed the discriminator or flat audio
  with nothing below 300 Hz removed -- no voice high-pass, no de-emphasis that rolls off the low end -- and prefer
  48 kHz. Sound cards and receivers that high-pass their audio output remove CTCSS before DSD-neo sees it. PCM carrier
  detection only rejects digital silence, so hum on the audio path is heard too, between transmissions as well: a hum
  that dominates the sub-audible band on a table tone reads as that tone. Power-supply ripple in a 50 Hz mains region is
  exactly that -- a 100 Hz hum that reads `CTCSS 100.0 Hz` -- so keep ripple and ground loops off the audio feed. A
  producer on stdin, UDP or TCP that squelches by sending nothing at all (`rtl_fm` without `-E pad`, a UDP sender that
  stops between transmissions) pauses the stream instead: once nothing has arrived for about half a second, the row
  shows no carrier, and the audio that follows starts a new reception that inherits nothing from the last one (the read
  that spans the pause, up to 20 ms of audio from either side of it, is not used). A per-call recording (`-P`) also
  leaves out the whole block of audio the stream comes back in, up to about 117 ms at 8192 Hz, so each transmission
  loses its start ([Where an NFM recording starts](#where-an-nfm-recording-starts---analog-record-preroll-ms)).
  Padded output (`rtl_fm -E pad`) keeps samples flowing, and the 200 ms hangover then applies as on any other input.
  DSD-neo does not see a retune the producer makes on its own, such as `rtl_fm` scanning several frequencies: a hop that
  leaves a gap shorter than half a second (with padding, shorter than the 200 ms hangover) is not a new reception, so
  the previous channel's tone can still show on the next channel for a few hundred milliseconds, until the detector
  drops it as it drops a tone that stops (timing above). The same goes for audio a source still holds when DSD-neo moves
  it through rigctl -- in the producer's own pipeline, or captured by the sound server but not yet handed over -- which
  arrives after the retune like the new channel's. In offline runs of a clean tone followed by a carrier with no tone,
  about a tenth of a second of the old channel's tone at the start of a reception was enough to show it again (on most
  seeds from 110 ms, on every seed from 180 ms), for up to half a second.
- Talk-off: a voice whose fundamental holds within 0.5 Hz of a table tone for a third of a second, with weak harmonics,
  is indistinguishable from that tone in the time allowed and can be reported briefly. It is rare on transmitted voice
  (which the transmitter high-passes at 300 Hz) and most likely near the top of the table. In offline seed sweeps, two
  hours of unfiltered speech with no tone locked a tone three times (233.6, 241.8 and 254.1 Hz), and the same speech
  high-passed as a transmitter does once (254.1 Hz). Allowing for tone error added no talk-off on speech: 20 hours of
  each speech model locked the same tones, the same number of times, as before. A voice holding its pitch within
  0.5 % of a table tone is another matter, being exactly what a transmitter off its value sends: of 250,000 offline
  receptions of a tone beside a stronger steady line near another table tone, those that named the line's tone rose
  from 62 % to 74 %, while the real tone was confirmed exactly when it was before.
- Digital carriers: the analog monitor decodes no digital, so it cannot tell that a carrier is digital. This is true of
  `-fA` and of every other analog monitor: the `nfm` rows of a `-Y` scan, `nfm-conventional` `--trunk-scan` targets
  and Signal search's NFM visits. A digital carrier repeats its frames at a steady rate, which puts steady lines in the
  sub-audible band, and these can read as a CTCSS tone or a DCS code. Digital modes send their access code (colour
  code, NAC, RAN and the like) as bits in their frames, not as a sub-audible tone, so such a tone is the receiver's
  mistake, never something the transmitter sent (for P25, X2-TDMA and ProVoice, whose full specifications are sold or
  not public, this rests on secondary sources). Seen on real-air captures under `-fA`: a Capacity Plus rest channel
  read `CTCSS 233.6 Hz` from about 0.5 s (that capture repeated 15 times, 30 s, read it 15 times, for 21 s in all),
  and a TETRA control channel read `CTCSS 141.3 Hz` from about 1.9 s (not checked at Signal search's 16 kHz width).
  One of 300 synthetic signals made of TETRA frames read a DCS code for 320 ms. When a carrier switched from DMR to
  analog FM, the false 233.6 Hz still showed for about 200 ms after the switch, before the real tone or code took
  over, in the runs measured. These are observations from those runs, not bounds: other carriers and signals may read
  other tones, or for longer. While a false tone or code shows, everything that uses the received tone takes it as
  real: the tone filter allows or rejects the carrier by it, the `Rx tone:` line and the `RECEIVED TONE` row show it,
  analog call history names it (the `-J` event log line and the History rows), and a Signal search detection can keep
  it as its tone (`docs/signal-search.md`).

### Received code (DCS) on the analog monitor

Beside the CTCSS tone, the analog FM monitor listens for a DCS code (Digital-Coded Squelch: DPL, DCG, CDCSS). Like tone
detection it only reports: it never mutes or gates audio, it needs no setting, and it runs with `-o null` too. Muting by
code is the [tone filter](#tone-filter-ctcssdcs-receive-policy).

- Supported codes: the 104 codes radios commonly offer (the 83 standard codes and 21 more), 023, 025, 026, 031, 032,
  036, 043, 047, 051, 053, 054, 065, 071, 072,
  073, 074, 114, 115, 116, 122, 125, 131, 132, 134, 143, 145, 152, 155, 156, 162, 165, 172, 174, 205, 212, 223, 225,
  226, 243, 244, 245, 246, 251, 252, 255, 261, 263, 265, 266, 271, 274, 306, 311, 315, 325, 331, 332, 343, 346, 351,
  356, 364, 365, 371, 411, 412, 413, 423, 431, 432, 445, 446, 452, 454, 455, 462, 464, 465, 466, 503, 506, 516, 523,
  526, 532, 546, 565, 606, 612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743 and 754. A code is
  written as `D`, its three octal digits with leading zeros, and its polarity, `N` (normal) or `I` (inverted), such as
  `D023N`. A received code is shown by both spellings of its signal, the normal one first: `DCS D023N / D047I` (see
  Aliases). A code outside these reads as no code: the eight more some scanners detect (006, 007, 015, 017, 021, 050,
  141 and 214) are signals of their own and show none. 645, which some radios and decoders offer, sends the signal of
  D526N, so a radio set to D645N reads `DCS D526N / D325I`.
- What is sent: a DCS transmitter repeats one 23-bit word at 134.4 bit/s, the code's nine bits (least significant
  first), the fixed bits 0, 0, 1 and 11 Golay (23,12) check bits. Normal polarity sends a one as positive deviation (the
  carrier above the tuned frequency); inverted polarity sends the complement of the word. This convention and the bit
  order follow the published reference words; checking them on the air against a transmitter with a known code, in
  both polarities, is still to be done.
- Aliases: a receiver cannot tell where a word starts, and the complement of a code's word is another code's word
  shifted in time, so every supported code sent in inverted polarity is exactly the signal of one supported code in
  normal polarity: D023I is D047N, and D047I is D023N. Every signal therefore has two supported spellings, one in each
  polarity, and nothing in it says which of the two a transmitter was set to. DSD-neo shows both, the normal one first:
  a radio set to D023N or to D047I reads `DCS D023N / D047I`, and one set to D023I or to D047N reads
  `DCS D047N / D023I`. Every supported code appears twice below, once in each polarity; find a radio's setting to see
  what it reads as:

| Shown as | Shown as | Shown as | Shown as |
| --- | --- | --- | --- |
| D023N / D047I | D152N / D115I | D311N / D664I | D466N / D662I |
| D025N / D244I | D155N / D731I | D315N / D423I | D503N / D162I |
| D026N / D464I | D156N / D265I | D325N / D526I | D506N / D073I |
| D031N / D627I | D162N / D503I | D331N / D465I | D516N / D432I |
| D032N / D051I | D165N / D251I | D332N / D455I | D523N / D246I |
| D036N / D172I | D172N / D036I | D343N / D532I | D526N / D325I |
| D043N / D445I | D174N / D074I | D346N / D612I | D532N / D343I |
| D047N / D023I | D205N / D263I | D351N / D243I | D546N / D132I |
| D051N / D032I | D212N / D356I | D356N / D212I | D565N / D703I |
| D053N / D452I | D223N / D134I | D364N / D131I | D606N / D631I |
| D054N / D413I | D225N / D122I | D365N / D125I | D612N / D346I |
| D065N / D271I | D226N / D411I | D371N / D734I | D624N / D632I |
| D071N / D306I | D243N / D351I | D411N / D226I | D627N / D031I |
| D072N / D245I | D244N / D025I | D412N / D143I | D631N / D606I |
| D073N / D506I | D245N / D072I | D413N / D054I | D632N / D624I |
| D074N / D174I | D246N / D523I | D423N / D315I | D654N / D743I |
| D114N / D712I | D251N / D165I | D431N / D723I | D662N / D466I |
| D115N / D152I | D252N / D462I | D432N / D516I | D664N / D311I |
| D116N / D754I | D255N / D446I | D445N / D043I | D703N / D565I |
| D122N / D225I | D261N / D732I | D446N / D255I | D712N / D114I |
| D125N / D365I | D263N / D205I | D452N / D053I | D723N / D431I |
| D131N / D364I | D265N / D156I | D454N / D266I | D731N / D155I |
| D132N / D546I | D266N / D454I | D455N / D332I | D732N / D261I |
| D134N / D223I | D271N / D065I | D462N / D252I | D734N / D371I |
| D143N / D412I | D274N / D145I | D464N / D026I | D743N / D654I |
| D145N / D274I | D306N / D071I | D465N / D331I | D754N / D116I |

- What is shown: the terminal's `Rx tone:` line and the Qt/Android `RECEIVED TONE` row read `DCS D023N / D047I` once a
  code is confirmed, with `detecting`, `none` and the em dash as for CTCSS. The log prints
  `Received tone: DCS D023N / D047I` whenever the verdict changes. When a CTCSS tone and a DCS code both lock, the code
  is shown: a voice holding a tone's pitch on a coded channel never replaces it, and a tone that the code's own
  waveform raises in noise before the code is confirmed (see Timing) shows only until it is.
- Timing, in sample time, through the demodulator's DC block and 75 or 750 us de-emphasis at 8 to 78.125 kHz (for PCM
  input see the last item): a code is confirmed once its word has been read twice in a row, 46 bits, so typically
  345-370 ms after it starts. At 10 dB in-band signal-to-noise (0-290 Hz) or better every start is confirmed within
  520 ms (over 7,000,000 starts at 8, 44.1, 48 and 78.125 kHz the slowest took 445 ms). At 3 dB, 95 starts in 100 are
  confirmed within 450 ms and 999 in 1,000 within 690 ms; the ceiling is 1,500 ms, above the slowest of 8,500,000 starts
  (1,224 ms), and the slow tail comes mostly from 750 us de-emphasis at 78.125 kHz, which smears a code's isolated bits.
  Lock time in noise has no absolute bound. The tests hold every code in both polarities within 520 ms at 10 dB and
  700 ms at 3 dB on fixed seeds. A transmitter or receiver off frequency puts a DC level under the code, which steps in
  with the carrier; up to twice the code's deviation it keeps these bounds (over 1,200,000 such starts at 10 dB the
  slowest took 473 ms, and at 3 dB 95 in 100 were confirmed within 421 ms), and at four times one start in 1,000,000 at
  10 dB took 526 ms and at 3 dB 95 in 100 took up to 474 ms. A code that stops under a live carrier is dropped within
  350 ms on 95 stops in 100 (32 bits and the front end's delay), with a 600 ms ceiling above the slowest of 1,000,000
  stops (591 ms); the 134.4 Hz turn-off tone a transmitter sends as it unkeys drops it sooner, within 150 ms on 95 in 100
  (typically 80 ms), with a 400 ms ceiling (the slowest of 1,000,000 took 353 ms). The ceilings are in
  `<dsd-neo/dsp/analog_rx.h>`. One wrong bit in every word keeps the lock; two do not. A transmitter a little off
  134.4 bit/s (some send 134.3) locks and holds. A carrier with no code reads `detecting` until 500 ms of it and `none`
  after, so a code that takes longer than that to confirm, near 3 dB, shows `none` first. Rarely a code's own waveform
  in noise reads as a CTCSS tone before the code is confirmed, and that tone shows until it is: three times in
  16,800,000 starts at 10 dB over the long-run sweeps (D274N as 67.0 Hz for 161 ms, D122N as 77.0 Hz for 71 ms, D225I
  as 77.0 Hz for 50 ms).
- Holding: the turn-off tone ends a lock only once the code has gone too, so a steady component near 134.4 Hz under a
  code that is still read (an interferer, a voice holding its pitch) leaves the lock alone: at 10 dB it never ended one,
  and at 3 dB, where the noise now and then costs the code a bit, a component at the code's power or 3 dB above it ended
  one or two a minute, each confirmed again. The same code starting over at another place in its word (a radio that
  re-keys inside the 200 ms hangover without a turn-off tone, another transmitter behind a repeater whose carrier stays
  up) keeps the code shown: at 10 dB or better it showed `none` briefly once in 7,040 such restarts, on a steady carrier
  or after a gap of up to 190 ms, and never another code; at 3 dB, where the new place has to be read without an error,
  up to 5 in 100 showed `none` briefly before the code was confirmed again. What is read across a dropout says nothing
  about the code, so a word that holds any of it never counts toward the 32 bits, but time still runs: a code is dropped
  64 bits (476 ms) after it was last read, carrier open or closed. A code that stops under a carrier that keeps dropping
  out (squelch chatter after a transmission, say) is dropped within about 650 ms (over 16,000 such stops, 95 in 100
  within 548 ms, the slowest 619 ms), and a carrier that comes back from a 120 ms dropout without the code drops it about
  365 ms after it returns. Transmitter-filtered speech 10 and 20 dB above the code never lost a lock in the long runs.
  Unfiltered speech 10 dB above the code (a voice fundamental in the band, which a transmitter's voice high-pass
  removes) is outside these bounds: it lost the lock about 7 times a minute, each time confirming the code again, so the
  code showed 96% of the time.
- Rejection: in the tests random bits, the Golay code words that carry no supported code, every CTCSS tone and speech
  never read as a code. A signal one bit from a supported code's word may read as that code, the way DCS decoders
  tolerate a bit error; one two bits from every code's word does not. Noise reads as a code twice in a row about once
  in 6 x 10^8 bits for each of the detector's four droop slicers, some 50 days of continuous noise each, and about 2.3
  times as often for its balance slicer, which reads noise in windows balanced the way a code's words are; any of the
  five can lock, so the detector as a whole does so at most once in 10^8 bits, some 8 days. A digital carrier is
  another matter: see Digital carriers under [Received tone](#received-tone-ctcss-on-the-analog-monitor).
- The received code is forgotten at the same boundaries as the tone (retune, row or target change, decode-mode change,
  input switch, stop, 200 ms without carrier, a paused input stream), and detection runs where tone detection runs.
- Externally demodulated audio (PCM inputs): keep everything below 300 Hz, as for CTCSS. The timing above is for the RTL
  path, through the demodulator's own DC block (a one-pole high-pass at 3.7 Hz at 48 kHz). PCM input comes through the
  sound card's coupling instead, or through none, and a coupling sags long runs of equal bits faster the higher its
  corner; the detector allows for a corner up to about 10 Hz. A DC-coupled input keeps the bounds above, also when the
  producer hands on a frequency offset as a DC level (`rtl_fm` without `-E dc`, for one), up to the offsets under
  Timing. Through a 10 Hz coupling every start at 10 dB is still confirmed within 520 ms (over 800,000 starts at 44.1
  and 48 kHz the slowest took 477 ms), but at 3 dB it costs bits, and an AC-coupled input is outside the 3 dB bounds
  above, a known limitation: 95 starts in 100 are confirmed within 475-535 ms and 999 in 1,000 within 837-930 ms, with
  the slowest past the 1,500 ms ceiling (1,702 ms). A 15 Hz corner is slower still (at 10 dB, 5 starts in 100,000 past
  520 ms), so prefer a DC-coupled input or one with a low corner. The audio's polarity matters: an inverted audio path
  (some receivers' discriminator outputs, some sound cards) turns every code into the other signal of the same number,
  so a D023N transmitter reads as `DCS D047N / D023I`.

### Receive filters and service names

Receive filters apply to decoded output, call/data/location publication, per-call WAV/MBE recordings,
and scan activity. They leave raw input WAV, I/Q and symbol captures available for diagnostics.

| Kind | Allow / block | Values | Disable |
| --- | --- | --- | --- |
| Slot | `--slot-allow`, `--slot-block` | `1`, `2`; DMR, P25 Phase 2 and X2-TDMA only | `--no-slot-filter` |
| DCC | `--dcc-allow`, `--dcc-block` | DMR color codes `0..15` | `--no-dcc-filter` |
| RAN | `--ran-allow`, `--ran-block` | NXDN48/96 RANs `0..63` | `--no-ran-filter` |
| NAC | `--nac-allow`, `--nac-block` | P25 Phase 1/2 hexadecimal `001..FFE`, optional `0x` | `--no-nac-filter` |

Use `/` between values, for example `--dcc-allow 4/6/8 --dcc-block 6`. Allow and block may coexist;
block wins, and every applicable dimension must pass. Slot filters do not affect FDMA. An enabled
access filter stays **Checking** until reception supplies a validated identifier, including a block-only
filter. A configured Phase 2 descrambler seed is not proof of a received NAC. Existing CRC/FEC,
talkgroup, encryption and synthesis gates still apply. Control-channel acquisition remains available;
a grant without destination access-code evidence receives the existing bounded acquisition window. A trunking
receiver remembers the code it verified on each voice channel (up to 16), so it stops following grants to a channel
whose code the policy rejects; a policy edit that admits that code takes effect at the next grant. Only codes a voice
channel proved after its tune are remembered: a tune forgets the control channel's codes (with a filter on, their
confirmation locks too), so they never name, split or admit the voice channel's calls. A slot
filter judges the over-the-air slot even on the single-slot DMR decoder (`-fr`, `-nm`): a repeater's slot is proven
by two consecutive bursts' CACH, a direct-mode one by its sync word, and the first bursts stay Checking. A proven
move to the other slot starts a new call there, even with the same colour code; a call that opens on the other slot
with its voice header is one call there from the header on.
Rejected traffic does not refresh an activity hold. A manually held conventional channel stays selected and muted.

Names use repeatable `--slot-name`, `--dcc-name`, `--ran-name`, `--nac-name` and `--tone-name` options:

```sh
dsd-neo -i rtl:0:461.0M -fr --dcc-allow 4/7 --slot-name '1=Dispatch' --dcc-name '7=Hospital'
dsd-neo -i rtl:0:154.43M -fA -P --tone-name '141.3=20R Traveler' --tone-name 'D023N=County fire'
```

A matching access-code or tone name wins over a slot name, followed by the existing channel label.
Naming alone never filters traffic. The channel label and permanent scan-target ID remain unchanged.
Each scope accepts up to 32 selectors and names up to 63 UTF-8 bytes. Empty names, leading/trailing spaces,
commas, double quotes and control characters are refused. Equivalent DCS spellings share one selector;
conflicting names for that selector are refused. `--no-service-names` clears the inherited table.
A name for an identifier the filters in force reject (`--dcc-block 7 --dcc-name '7=Hospital'`, or a tone name outside
`--tone-allow`) is kept but never shows, since that traffic is never received. dsd-neo warns about it at start, when a
scan list or channel map loads (naming the row), in `--validate-config`, in import previews and in the notice of a live
edit, so a service can be blocked for a while without losing its name.

Live editors are on the Qt/Android monitor under **Signal & site**: **Receive filters** and **Service names**.
They offer **All channels** / **This channel**, with **Use default** and **List value** for session-only row edits.
The terminal equivalents are under **Decoder → Receive filters and names**.
Edits are validated and applied as a whole on the decoder thread; a stale destination is refused.
A policy change ends only an active call it now rejects; a call it still admits, or is checking, carries on, and a
name edit relabels it. Completed history retains its service
identity and label, and different confirmed identifiers do not merge into one call.

`-P` also records conventional analog transmissions, NFM (`-fA`) and AM (`-fM`) on a radio input and analog audio
input, independently of speaker muting or null playback.
History includes the received CTCSS/DCS identity; a confirmed tone change separates transmissions. AM carries no
CTCSS or DCS that dsd-neo detects, so an AM call names no tone. Earlier versions made no AM calls at all: no history
row, `-J` line or recording.

A reception becomes a call only once it has lasted 500 ms. The time counts the 200 ms hangover after the carrier
drops, and, under a tone filter, the time the tone is checked before it is allowed. A shorter reception (a kerchunk,
a radio keyed by mistake) leaves no history row, no `-J` line, no live call and no recording: what was recorded of it
is deleted, never published. So a burst with less than about 300 ms of signal leaves nothing. The time is counted on
whole blocks of audio: 20 ms on a radio input, 960 samples on audio input (384 ms at 2500 Hz). A call that counts
starts when its carrier opened, the tone check included, and its recording starts with its first allowed audio.
Earlier versions made a row and a recording of every reception, however short, and under a tone filter started the
row when the tone was allowed.
Carrier end (200 ms after the carrier drops, or after a longer pause with `--analog-record-max-gap-ms`, below),
rejection, retuning, an input discontinuity or stop closes the recording. Named per-call WAVs
include a sanitized `_SVC_` component. An analog recording has no talkgroup, source or call type, so it is named
`<date>_<time>_<nnnnn>_<mode>_<frequency in Hz>[_<tone>][_SVC_<name>].wav`
(`20261021_142839_51828_NFM_154430000_CTCSS_141.3_SVC_20R_Traveler.wav`). The mode is the one the call was heard in:
`NFM`, `AM` for an AM call, and `ANALOG` on audio input, where another receiver demodulated it
(`20261021_142839_51828_ANALOG_FREQ_UNKNOWN_CTCSS_141.3_SVC_20R_Traveler.wav`); earlier versions named every one
`NFM`. The tone is the received `CTCSS_<Hz>` or `DCS_D<code><N|I>` (the standard code, as `DCS_D023N`); it is left out
when no tone was received or it was not confirmed. A call with no known frequency (an audio input) has `FREQ_UNKNOWN`
in place of the frequency.
By default each new recording starts with up to 200 ms of what was heard just before the call
([below](#where-an-nfm-recording-starts---analog-record-preroll-ms)).
With `--rdio-mode`, analog recordings are exported to rdio-scanner too, named or not
([below](#analog-calls-in-rdio-scanner)). See
[receive configuration](config-system.md) and [row options](csv-formats.md).

#### Where an NFM recording starts (`--analog-record-preroll-ms`)

An NFM call begins when the squelch finds the carrier. The squelch needs a moment to decide, about 30 to 50 ms with
the noise squelch, so a recording that started there would miss the start of the first word. So each new recording
starts with up to `--analog-record-preroll-ms` (INI `[recording] analog_preroll_ms`, 200 ms by default) of the audio
heard just before the call, and then the call itself. 0 turns it off: a recording then starts where the call is
picked up, as before.

- It works only while per-call recording is on (`-P`). With it off, nothing is held.
- The pre-roll is whatever the input sent before the call. With an open squelch on the radio, or an SDR, that is
  mostly noise, so a long pre-roll puts a short hiss at the start of each recording. From a radio that squelches
  itself, it is silence.
- It holds only audio heard since the last call ended, and since the last break in the input (a retune, a scan step,
  an input switch, a stream that paused or reconnected). Audio from before such a break never reaches a recording.
- Under a tone filter, only audio the filter has allowed is held. Audio it is still checking, or has rejected, is
  never saved, so a recording under a tone filter starts when the tone is confirmed, usually with no pre-roll. The
  filter's verdict counts in steps of about 20 ms (see the
  [tone filter](#tone-filter-ctcssdcs-receive-policy), "What is muted").
- A call begins only on the carrier itself, not on the 200 ms the squelch holds open after the carrier drops. So a
  short burst whose tone is confirmed only after its carrier has gone leaves no recording and no history row.
- When the input comes back after a stall or a pause in the stream (stdin, UDP, TCP or radio input), the block of
  audio it comes back in is not recorded, with the pre-roll or without it: it may hold audio from before the pause. A
  block is 20 ms on radio input and on audio input at 48 kHz (8, 9.6, 12, 16 and 24 kHz input is brought up to
  48 kHz first); at other rates it is 960 samples, about 117 ms at 8192 Hz. So a stream that sends nothing between
  transmissions (`rtl_fm` without `-E pad`, a UDP sender that stops) loses the start of each one; keep it running
  (`rtl_fm -E pad`). A carrier too short to outlast that block begins no call; on radio input that holds for one of up
  to about 40 ms.
- Someone who keys up again within 200 ms of letting go is not a new call: the call goes on, and the start of their
  first word stays as the squelch left it.
- When a call held through a pause (`--analog-record-max-gap-ms`, below) resumes, the end of the pause is saved as
  heard rather than as silence, up to the pre-roll. Anything that breaks the audio during the pause, such as a tone
  check when the carrier comes back, makes the whole pause silence, as before.
- The recording's start time is its first sample, the pre-roll included, so the `--wav-journal` record's `start` and
  the rdio-scanner export's `start_time` can be a second or two earlier than the call's history row (two with a long
  pre-roll). Both are whole seconds.
- These drop the audio held so far, so the next recording may start without it: turning per-call recording off or
  on, a new recording folder, a new pre-roll value, and setting the pre-roll in a running session through the API,
  the terminal menu or the app, even to the value it already has. A retune, a scan step or an input switch drops it
  too, as above. A config applied in a running session drops it for these same reasons and no other, except that a new
  tone filter can start a fresh tone check, whose audio is never held.
- AM (`-fM`) calls have no pre-roll: the AM squelch opens without the noise squelch's delay, so a recording starts
  with the call. Digital calls are not affected.

#### Analog calls in rdio-scanner

With `--rdio-mode` set, each analog recording (NFM, AM, or an audio input's) is exported like a digital call's: a
DirWatch sidecar, an API upload, or both. rdio-scanner needs a talkgroup number, and an analog call has none, so
dsd-neo gives it one:

- The talkgroup is the frequency the call was heard on, in kHz, rounded down: 154.4375 MHz is talkgroup `154437`.
  This is rdio-scanner's own way of numbering conventional channels. The number is used only for the export; the call
  history, the journal and the app still show no talkgroup.
- On an audio input (PulseAudio, UDP, TCP or a WAV file, even with rigctl) the frequency is not known. Those calls are
  exported under `--rdio-analog-talkgroup <n>` when it is set, and are not exported otherwise. The first time that
  happens in a run, one log line says so.
- The talkgroup label (`talkgroup_tag`) is the channel's name from a `-Y` channel map or a `--trunk-scan` row, else the
  call's mode and frequency (`NFM 154.4375 MHz`, `AM 119.1000 MHz`), else the mode alone (`ANALOG` on an audio input).
  It is never a tone's service name: rdio-scanner can rename a talkgroup after each call's label, and every service on
  one frequency shares that frequency's talkgroup. The service name stays in the file name and the call history.
- The sidecar lists no radio (`srcList` is empty), is never encrypted, has the call's mode as its `short_name` (`NFM`,
  `AM` or `ANALOG`), and its `freq` is the call's frequency (0 below 1 MHz or when it is not known).
- `--rdio-analog-system-id <n>` sends analog calls to their own rdio-scanner system: the sidecar's `system` and the API
  upload's `system` field use it. In DirWatch mode rdio-scanner ignores the sidecar's `system`. rdio-scanner 6 files
  every call in a watched folder under that folder's system, so to keep analog calls apart there, run them in a separate
  dsd-neo session that writes to a second folder. rdio-scanner 7 picks the system by `short_name`, which is already the
  mode.
- An analog call has no talkgroup or radio list row, so `stream=off` never applies to it.

Limits:

- Calls on one frequency share one talkgroup, whatever their tone. The kHz are rounded down, so all frequencies in one
  whole kHz share one too: 154.4370 and 154.4379 MHz are both `154437`, while 154.4379 and 154.4381 MHz, closer
  together, are not.
- rdio-scanner drops a call it takes for a duplicate: same system and talkgroup, starting within its duplicate
  detection time frame (500 ms by default; `duplicateDetectionTimeFrame` and `disableDuplicateDetection` in its
  options). The sidecar's `start_time` is in whole seconds, so two transmissions on one channel that start in the same
  second can be dropped by the server. Shorten or turn off the duplicate detection if that happens. trunk-recorder
  sends whole seconds too.
- rdio-scanner 7 (not released yet) ignores the sidecar's top-level `freq` and files a call at the time it arrives,
  not at its `start_time`. dsd-neo does not work around that.

#### Analog calls need a squelch on a radio input

On a radio input (`rtl`, `rtltcp`, `soapy`, Airspy, `--iq-replay`) an analog reception becomes a call only when the
squelch keeps the receiver's own noise out. The startup squelch (-110 dB) and an off one let noise through, and noise
would hold one reception open for the whole session. So while the squelch is off or at -110 dB or below, analog calls
leave no history row, no `-J` line, no live call and no recording, and dsd-neo logs once:

`NOTICE: Analog call history and recordings are paused: the squelch is off or at -110 dB or below, so noise would
hold one call open. Set --squelch <dB>, --squelch auto or noise, or --tone-allow.`

Any one of these lifts it: a level above -110 dB (`--squelch -60`), `--squelch auto`, `--squelch noise` on NFM, or on
NFM a `--tone-allow` list, which lets only its tones through. A `--tone-block` list does not, as noise carries no tone
to block. On AM dsd-neo checks no tones, so the AM notice names only `--squelch <dB>` and `--squelch auto`. A squelch
set while dsd-neo runs (the terminal's `Squelch...` row, the Qt/Android radio panel, the API) or a scan row's own
`--squelch` applies at once: a call in progress ends when the squelch drops back to the default. The lift is logged once
too: `NOTICE: Analog call history and recordings are back on.` The monitor audio is not affected either way. Audio input
is never paused: another receiver may squelch it, and its silence closes the carrier. Earlier versions made analog calls
at any squelch. A known limit: the notices follow the squelch in force, so on a `-Y` scan whose rows differ (some rows
with their own `--squelch`, others left at the default) a paused or back-on notice is logged at each hop where that
changes.

#### Pauses in an analog recording (`--analog-record-max-gap-ms`)

By default an analog call ends 200 ms after its carrier drops (the carrier hangover). Every longer pause
starts a new call, with its own history row and its own recording. `--analog-record-max-gap-ms <ms>` (INI `[recording]
analog_max_gap_ms`) keeps the call going through a pause of up to `<ms>`, so one exchange stays one row and one
recording. It changes nothing else: the hangover, the squelch, the scan and the tone filter work as before.

- It works only while per-call recording is on (`-P`, INI `[recording] per_call_wav`). With recording off, a call ends
  200 ms after its carrier drops, as before. Recording turned off in a running session ends a paused call at its last
  audio, from the next block of audio; a change of recording folder does not.
- The pause counts from the last moment the carrier was there. Its first 200 ms is saved as received, as before:
  silence under the noise or auto squelch, the input itself under a level squelch. Nothing more is saved while the
  carrier is away.
- When the carrier comes back within the limit, the rest of the pause is saved as silence just before the new audio,
  so the recording keeps time. With the pre-roll on, the last part of it, up to the pre-roll, is saved as heard instead,
  unless something broke the audio during the pause (a tone check does). A pause exactly as long as the limit is still
  joined. A longer one ends the call at its last audio; the next transmission starts a new call and a new recording.
- 200 or less turns it off, the default. The largest value is 10000 (10 seconds). A change in a running session
  applies from the next block of audio: a lower limit, or off, ends a call already paused longer than that, and a
  higher one lets it wait longer. A call that already ended stays ended.
- While a call is paused it still shows as active, and its timer keeps running, in the terminal, the app and the API.
  Its history row and the event log entry come when the pause is decided (an analog call sounds no `-a` alert). The
  row ends at the last audio received, not when the wait ran out.
- A tone check counts as part of the pause. Under a tone filter, the decoder checks the tone again each time the
  carrier comes back, which takes about 0.4 to 0.8 seconds, and that time is saved as silence too. So a short limit,
  such as 500 ms, rarely joins transmissions under a tone filter; allow for the check.
- A different tone or code that is confirmed when the carrier comes back ends the paused call, and the new
  transmission starts its own call and recording. Without a tone filter, a different station that answers during
  the pause is saved into the paused recording until its tone is confirmed (usually within 0.4 seconds for a CTCSS
  tone, at most about 0.7 seconds on a strong signal, longer for some tones and for DCS), as a tone change inside one
  carrier is today. Audio the tone filter is still checking or has rejected is never saved. A weak toned carrier that
  reads as untoned before its tone locks is split there, at a resume as at a call start.
- Time is counted in the decoder's blocks of audio, so a pause is measured to within one block: 20 ms on radio input
  and on PCM input at 48 kHz (8, 9.6, 12, 16 and 24 kHz input is brought up to 48 kHz first); at other PCM rates a
  block is 960 samples, about 117 ms at 8192 Hz. A pause shorter than one block is not seen.
- An input that stops sending is not a pause in the signal, and still ends the call: a stream on stdin, UDP or TCP that
  sends nothing for about half a second (`rtl_fm` without `-E pad`, a UDP sender that stops between transmissions),
  or a radio input that stops delivering. The call ends at once, and its row ends at its last audio (on Windows, stdin
  that is a console or a redirected file ends it only when data comes back or the input ends). A shorter stall is
  joined without any
  silence. To join pauses, keep the stream running while the channel is quiet (`rtl_fm -E pad`, or a channelizer that
  keeps sending the channel's noise).
- A scan leaving the channel, a retune, an input switch, a decode mode change or stopping the decoder ends a paused
  call at once, with no silence added. Scanning is not held up by a paused call.
- AM (`-fM`) calls are held the same way. EDACS analog calls have their own recording and do not have this hold.
  There is no maximum file length. The shortest call is the 500 ms a reception needs to count as one (see
  [Receive filters and service names](#receive-filters-and-service-names)).

### Tone filter (CTCSS/DCS receive policy)

The tone filter decides, from the tone or code the monitor receives (above), which analog FM traffic is heard. It is a
setting, shown apart from what is received, and it is off by default: then the ordinary carrier squelch alone decides.

- `--tone-allow <list>` hears only traffic carrying a listed tone or code; `--tone-block <list>` mutes traffic carrying
  one and hears the rest; `--no-tone-filter` turns the filter off. One of the three per command line. The INI keys are
  `[analog] tone_filter = off|allow|block` and `tone_list` (see `docs/config-system.md`); a `-Y` channel-map row or
  `--trunk-scan` target of mode `nfm` takes the same three options for itself, and an `am` one refuses them (see
  [scoped row options](csv-formats.md#scoped-row-options)).
- `<list>` is CTCSS tones (any of the [supported tones](#received-tone-ctcss-on-the-analog-monitor), 150.0 Hz
  included) and DCS codes separated by `/`, such as `67.0/100.0/D023N`: a tone as `100` or `100.0`, a code as `D` and
  three octal digits with `N` or `I` (any case; a bare `D023` is `D023N`). Commas are refused
  (`use / between entries, not commas`), as are a tone or code outside those sets (161.0 Hz, say), an empty
  entry, an empty list and a signal listed twice. A code is matched by its signal, so both spellings of it work: a
  listed `D023I` matches a received `DCS D047N / D023I`, and listing `D023N` and `D047I` together is refused as the same
  signal twice. The polarity is part of the signal: on a source whose audio is inverted (see
  [Received code (DCS)](#received-code-dcs-on-the-analog-monitor)) every code arrives as the other polarity's signal,
  so a `D023N` transmitter reads as `DCS D047N / D023I` and a listed `D023N` does not match it (allow mutes it as not
  allowed, block lets it through). Fix the audio's polarity, or list the spelling the monitor shows (`D023I` for that
  transmitter). A refusal names the entry by its position and never repeats the text.
- The acquisition window: while a list policy is in force, each transmission starts muted. A confirmed tone or code is
  judged at once: allow passes a listed one and rejects any other, block rejects a listed one and passes any other.
  With nothing confirmed after 800 ms of the carrier (sample time, so a fast replay decides the same way), allow rejects
  the transmission (no tone) and block lets it through. The window is longer than the CTCSS lock ceiling (700 ms) by two
  detector hops; it runs on, to at most 1,600 ms, while the list holds a DCS code and the DCS detector has read a
  supported code's word once and is waiting for the second reading that confirms it, since a code at 3 dB can take up
  to the DCS lock ceiling (1,500 ms) to confirm. A CTCSS-only list, and a carrier with no such candidate, keep 800 ms:
  noise and speech with no code raise one at 800 ms on 1 to 3 receptions in 100, which then wait on average another
  250 ms. A tone confirmed off its table value by transmitter tone error (see
  [Received tone](#received-tone-ctcss-on-the-analog-monitor)) that the list does not pass is judged only when the
  window ends, at 800 ms or at the end of a DCS extension: until then the detector can still confirm another tone on
  its value, which is then judged as ever. So a scanner never leaves a transmission on such a tone before the window
  has run, and allow rejects an unlisted one, and block a listed one, that much later. One the list passes is judged
  at once.
- After the verdict the detectors keep listening for the whole transmission. Allow: the allowed tone lost (after the
  detector's own hold-over) mutes again and starts a fresh window, and an unlisted tone or code confirmed rejects it,
  while another listed one keeps it passing. Either list: a tone confirmed off its value that the list does not pass
  mutes passing traffic again and starts a fresh window, as a lost tone does, rather than rejecting it at once. Block: a
  blocked tone or code confirmed later rejects traffic that was
  passing, and losing a tone keeps a pass. A rejection holds until the carrier has been gone for the 200 ms hangover,
  except that a tone or code the list passes, confirmed later, lets the traffic through; another the list does not pass
  keeps it rejected and becomes its reason, so a rejection for want of a tone that later hears an unlisted tone reads
  `muted: not allowed` and logs that tone. The next transmission starts over, and so does the one on air when the policy
  itself changes (a loaded config, a scan row's own policy coming on air): it is muted for a fresh check.
- What is muted: the live monitor output and the UDP analog monitor (`-o udp`), together. A per-call recording (`-P`)
  never holds audio the filter is still checking or has rejected either, its pre-roll included. The `-6` raw WAV is a
  capture ahead of every gate, squelch and tone filter included, and keeps everything.
- A verdict counts from the 20 ms of audio it was reached on. The monitor works in blocks of audio, and on PCM input at
  rates other than 48 kHz a block holds several 20 ms pieces (a block is about 117 ms at 8192 Hz). When the filter lets
  traffic through part-way through a block, the part before is silence: it is not played, and the automatic gain does
  not follow it. A per-call recording that starts in that block starts at the verdict; one already running keeps the
  part as silence, so it keeps time. At 48 kHz and on radio input a block is one piece, so a verdict
  covers the whole block.
- Scanning: a transmission still being checked holds a `-Y` row or `--trunk-scan` target while its carrier lasts, with
  the stay reason `Tone check` and no countdown while the check runs (800 ms, or to 1,600 ms as above); a `-Y` pass
  keeps the row then even when `-t` has run out. Only traffic the policy passes counts as activity, though. The check
  restarts neither the `-t` hangtime nor the target's `activity_hold_ms`, and a trunk-scan target's idle dwell keeps
  running through it, so a transmission that ends before its verdict leaves no tail, and repeated short bursts that are
  never passed (kerchunks, noise) cannot park the scanner on a muted row: it moves on once `-t` has run out since the
  row was tuned or last passed traffic, or once the target's dwell has. Traffic the policy allows is activity like any
  carrier and leaves the ordinary tail. Rejected traffic holds nothing: `-Y` moves on at its next no-carrier pass
  (within about 375 ms), and trunk scan at its next tick, without waiting out the hangtime or the activity hold, and so
  does traffic rejected and ended before that pass or tick came: the rejection outlives its carrier for the scanner
  until the next carrier opens or the receiver moves. An operator hold keeps the row, muted. A fixed-frequency session
  mutes rejected traffic and stays, and so does a scan with nowhere else to go: a `-Y` list or `--trunk-scan` list with
  no other row or target to step to (the others avoided, or skipped at every visit for a width the DSP rate cannot
  filter, or none), or, for trunk scan, whose others are all cooling down from a failed retune. A `-Y` row on the
  frequency on air counts as somewhere to go only when it would judge the traffic otherwise: a row of another class (a
  mixed-mode repeater's `dmr` row, an `am` row, or a row inheriting a decode mode other than `-fA`), or an `nfm` row
  whose policy, its own or the configured one, passes the tone or code the traffic carries (or, after a `no tone`
  rejection, passes no tone). One that would reject it too, such as a second row with the same policy or every row of a
  list without modes, is nowhere to go. A trunk-scan target on that frequency is always of another type, since a target
  list holds one target per type and frequency. While a scan keeps rejected traffic this way, its Scan Timing row reads
  `Carrier` with no countdown; once that traffic ends, the ordinary tail runs.
- Where it applies: the analog FM monitor only (`-fA`, and `nfm` scan rows), on radio and PCM inputs alike, never the AM
  monitor (`-fM`, and `am` scan rows and `am-conventional` targets, which carry the configured policy past them
  untouched and hold on their carrier), wherever the received tone is detected. Set where nothing runs it, it has no
  effect, and the session says so once. At startup: for the AM monitor (`-fM`) or a digital mode without a scan list (a
  `-Y` scan with no channel map included, which stays on that mode until one is imported), and for a `-Y` list none of
  whose rows runs the FM monitor (no `nfm` row, and under `-fA` no row without a mode of its own either, since a typed
  row, an `am` one included, runs its own mode), checked when the list is imported from the command line or a config
  file. When the scan starts: for a `--trunk-scan` list with no `nfm-conventional` target, whatever the decode mode.
  During the session: when a loaded config sets a policy nothing runs, or a decode-mode change, channel map or scanner
  toggle takes the FM monitor away from the policy in force. The Call Info `Tone filter:` line and the Qt/Android
  `TONE FILTER` row leave the screen while no detection runs; the terminal's Audio > `Tone filter...` row stays.
- Live editing: the terminal's Audio > `Tone filter...` row and the Qt/Android monitor's `TONE FILTER` row (its Edit
  button) set the policy during a session, mode and list together. The terminal row reads the policy as the Call Info
  line does and opens a picker (Off, Allow list..., Block list..., Off and clear list) on the configured mode; allow and
  block then prompt for the list, filled in with the configured one, off keeps the configured list as it stands when the
  edit runs (so an edit or a loaded config made just before it is what it keeps; never a scan row's own), and Off and
  clear list removes it. The Qt/Android editor has an Off/Allow/Block selector and a list field, filled in with the
  configured policy, that shows why a list would be refused as it is typed (the same parser's message) and sends only
  one that would apply; off sends the list in the field, so clearing the field and choosing Off removes it. On the
  monitor the row, and so the editor, is there while detection runs on the channel on air, reading `off` with no policy:
  on a scan whose list mixes `nfm` rows or `nfm-conventional` targets with other modes, it comes and goes as those rows
  come on air, and the terminal row, always shown, edits the same policy in between. Both send the list as typed; a list
  the parser refuses, or allow or block without a list, is refused with the reason
  (`Refused: tone filter: use / between entries, not commas`) and changes nothing, and each edit is judged on its own,
  so a refused one never cancels an earlier edit still waiting to apply. An edit takes effect from the monitor's next
  read of audio (a changed policy starts the transmission on air over, muted for a fresh check), and a list policy set
  where nothing runs detection says once that it has no effect. The editors always edit the configured policy, the one a
  save writes (Config > Save or autosave, `[analog] tone_filter` and `tone_list`); a scan row's own policy is never
  saved. While an `nfm` row or `nfm-conventional` target with its own policy is on air, it stays in force, the edit
  applies from the next row without one or when the row leaves, and the message says so:
  `Default tone filter -> allow 100.0 Hz; this channel overrides it (block 67.0 Hz)`; the Qt/Android editor says so too,
  and both open on the configured policy, never the row's.
- What is shown: the terminal's Call Info section adds a `Tone filter:` line under `Rx tone:` while a policy is in
  force, and the Qt/Android monitor a `TONE FILTER` row under `RECEIVED TONE` wherever detection runs, reading `off`
  with no policy (its Edit button opens the live editor). Each shows the policy (`allow 100.0 Hz/D023N`;
  while a scan row's own policy runs, followed by the configured one it shadows,
  `block D023N (row; default allow 100.0 Hz)`) and, while a carrier is heard, what it does: `passing`,
  `muted: checking tone`, `muted: not allowed` or `muted: no tone`. The received row goes on showing only what was
  received. The log prints `Tone filter: allowed (CTCSS 100.0 Hz)`, `Tone filter: rejected (no tone)` and the like when
  the verdict or the value that decided it changes, and `Tone filter: pending (tone lost)` when an allowed tone is lost
  (not when a changed policy starts a fresh check). A terminal without UTF-8 shows a long list's overflow mark as
  `...+3`.

## Mode Tweaks & Advanced

- Inversions: `-xx` X2 non‑inverted, `-xr` DMR inverted, `-xd` dPMR inverted, `-xz` M17 inverted
  - DMR proves its polarity from the bursts (issue #729): a BS voice sync is an inverted data sync, so bursts read at
    the wrong polarity show as voice sync on every burst (a control channel or idle slot) or as data bursts whose slot
    type never decodes but whose voice frames do at the other polarity (a call). Either takes the other polarity for
    the session, with a notice, as `-xr` or the menu toggle would; `-xr` only sets where it starts.
- Disable DMR/dPMR/NXDN/M17 input filtering: `-l`
- Analog filter bitmap (advanced): `-v <hex>`, default `0x9`. `0x1` the analog monitor's voice band-pass: 300-3400 Hz,
  which takes every CTCSS tone at least 40 dB down on FM, and a DCS signal about 32 dB (a 200 Hz high-pass on AM); `0x2` a 960 Hz
  low-pass and `0x4` a 960 Hz high-pass on the analog monitor; `0x8` the digital voice high-pass: a 2nd-order
  Butterworth at 300 Hz (-3 dB at 300 Hz, about -19 dB at 100 Hz, within 0.05 dB of unity from 1 kHz up), run on each
  slot's decoded frames before the gain, so every output that takes digital voice gets it, in both output formats;
  each slot keeps its own filter state, which starts over at a new call or protocol. `-v 0` plays the demodulator's
  audio unfiltered.
- Modulation optimizations: `-ma` (auto), `-mc` (C4FM), `-mg` (GFSK), `-mq` (QPSK), `-m2` (P25p2 QPSK 6000 sps)
  - `-mc`, `-mg` and `-mq` lock the demodulator, and the lock holds on either side of a `-f` preset: `-mq -f1` and
    `-f1 -mq` both run P25 Phase 1 on the CQPSK chain. A preset sets its own modulation only when no lock is in force.
- Relax CRC checks: `-F` (P25p1 data, P25p2 MAC_SIGNAL, DMR RAS/CRC, M17 LSF/PKT; a DMR burst is accepted as RAS only
  from a near-clean codeword with its reserved bits set, see the RAS note under `-J`, which also covers the RAS
  indicator shown with or without `-F`). No effect on NXDN, which always requires
  CRC-verified content (see the NXDN note under "Modes & Decoders" above). A P25 Phase 2 MAC_SIGNAL that failed its CRC
  still acts on its grants, payload and voice, but no system broadcast in it is applied: no WACN/SYSID/NAC, control
  channel, site or LRA, channel identifier (IDEN) table or trust, secondary control channel, neighbour, system services,
  system time or protection parameters come from it. P25 Phase 1 TSBK and trunking MBT always need their CRCs.
  Relaxed CRC is one setting: `-F`, `--strict-crc`, the terminal's "Relaxed CRC checks" toggle (`F`), the Qt/Android
  Settings → Decoding → **Relaxed CRC** switch (which adds `-F` to every session the app starts) and a scan row's
  own `-F` or `--strict-crc` all turn every one of these gates on or off together, DMR's relaxed control-channel
  heartbeat included. While a scan row sets its own, the row decides until it leaves; the toggle edits the configured
  value beneath it and says so.
- M17 signed voice-stream verification: `--m17-signature-public-key <hex>` accepts a 64-byte secp256r1 public key as
  raw `X||Y` hex.
- P25p2 manual WACN/SYSID/CC: `-X <hex>` (e.g., `-X BEE00ABC123`). With all three parts nonzero the seed is hand-set, as
  the menu's P25 P2 parameters set it: no NID or network status rewrites it, and neither a control channel change nor
  another protocol's trunking clears it; it counts as the received NAC only once a burst decodes with it. Unless `-X`
  hand-sets it, a session without trunking finds the code itself: a scrambled FACCH or SACCH gives it, one at another
  superframe position in a later window confirms it, and the line "P25p2 scrambling code recovered: WACN ... SYSID ...
  NAC ..." reports it. A hand-set code is never replaced.
- DMR Tier III location area (DMRLA): `-D <0–10>`. A radio is programmed with how many of the top Site ID bits form
  its location area (ETSI TS 102 361-4 §6.3.2.2.3); no message sends it, so `-D n` tells dsd-neo. With `-D n` the top
  n bits are the area and the rest the site in it, shown `Site area.site`; with no `-D` or `-D 0` the Site ID is
  shown whole. `-D n` at or past the model's Site ID width (Tiny 3, Small 5, Large 8, Huge 10 bits) makes the whole
  Site ID the area, shown `Site <area+1>.1`.
  How Tier III numbers are shown depends on the maker's numbering. Standard numbering is the value sent + 1 (the
  network, the site and, with `-D`, the area and the site in it), as Hytera's programming software numbers them: system
  code `105B` (Small, network 2, site 27 = 11011₂ as sent) reads `TIII Small · Net 3 · Site 28 · SYS 105B`, `Site 4.4`
  with `-D 2` and `Site 28.1` with `-D 5`. A network dsd-neo recognises as Motorola Capacity Max, from its Motorola
  C_ALOHA (under `-F` also when RAS hides that message's CRC), shows the network and site as sent and the site whole,
  and ignores `-D`, because Motorola numbers them that way and Capacity Max uses no location area. Its site text says
  so: system code `2101` (Large, network 1, site 1 as sent) reads
  `TIII Large · Capacity Max · Net 1 · Site 1 · SYS 2101`. A RAS Capacity Max system decoded without `-F`, and the
  first moments before its C_ALOHA arrives, read as standard. Sites are told apart by the values sent, never by this
  text. Capacity Plus and Connect Plus numbers are shown as sent.
- Env (CQPSK timing):
  - `DSD_NEO_TED_GAIN=<float>` overrides the CQPSK/OP25 Gardner timing loop gain.
  - When using RTL CQPSK input, the symbol sampler may gently nudge `symbolCenter` based on the smoothed CQPSK Gardner residual.
  - If you are debugging symbol-center drift, consider freezing windows (`DSD_NEO_WINDOW_FREEZE=1`) while testing.

## Trunking & Scanning

Quick talkgroup lockouts (`!`/`@` in the terminal and **Avoid TG**/**Lock out** in Qt/Android) save to the configured global
groups file by default. Use `--tg-lockout-session` to keep them temporary, or `--tg-lockout-persist` to select
the default saving behavior explicitly. The equivalent configuration key is `[trunking] persist_tg_lockouts`.
The terminal **Save user TG lockouts** menu and Qt/Android **Settings → Listening → Save avoided talkgroups**
change this preference immediately for subsequent lockouts and preserve it through their normal settings save.
The Qt/Android button reads **Lock out** when saving is on and **Avoid TG** when it is off.

Temporary avoids block tuning, audio, recording and streaming without modifying the saved list. Explicit list
edits still save, and exports omit temporary avoids. Use Qt/Android **Clear temporary avoids and call skips — current
list** or terminal **Clear temporary TG avoids - current list** to undo them in the current scope. They also clear
on list reload/replacement or decoder stop; ordinary retunes and scan visits preserve them. Scan rows with their
own lists stay isolated; rows using the global list share its avoids. Changing the persistence setting does not
change the lifetime of existing blocks.

Qt/Android **Skip** leaves the call without touching the talkgroup list, blocking tuning and all media for its
target. On P25 a group-call skip lasts while the receiver keeps seeing the talkgroup's call (control-channel
grant updates, or the call on the voice channel the receiver is parked on for the other slot); it expires 15 s
after the receiver last saw it and after 10 minutes at most. Transmissions with gaps under 15 s are skipped
together, so the unit skipped is the conversation; a new call that starts within 15 s of the previous one ending
is missed; and if the receiver follows another call on a different carrier for longer than 15 s while the skipped
call continues, the skipped call is followed again when next seen (tap **Skip** again).

P25 private calls, DMR and NXDN skips last 15 s from the press in this version; an EDACS or ProVoice call has no
skip (and no quick lockout), while a P25, DMR or NXDN call is skipped or locked out whatever else is enabled (Auto
enables ProVoice too).
Skips never persist, are unaffected by `persist_tg_lockouts`, and survive scan-target revisits like session avoids,
subject to their expiry. **Clear temporary avoids and call skips — current list** clears both in the active scope;
list reload/replacement or decoder stop also clears them.

On P25, a lockout (`B`/`DE` row), avoid or skip of a patched supergroup blocks the whole patch: a member talkgroup
that is allowed, listed or held does not readmit the supergroup's grants. A member still selects the patch when
the supergroup is merely unlisted in allow-list mode (`-W`) or does not match the Hold. On a patched call, the
quick lockout (`!`/`@`, **Lock out**/**Avoid TG**) applies to the supergroup shown for the call, as **Skip** does, not
to the member talkgroup the grant matched. Blocking the supergroup in the list releases its active call, and a
blocked supergroup's call is muted by the same rules as any blocked talkgroup if the return to the control channel
is delayed.

- Enable trunking (NXDN/P25/EDACS/DMR): `-T`. The later of `-T`/`-Y` wins.
- Conventional scan mode: `-Y` (not trunking; scans for sync on the row's decoder class or the global decoders). The
  later of `-T`/`-Y` wins. For NXDN the hold is refreshed only by frames whose content passed a CRC, so an open squelch
  on an empty channel no longer parks the scan. `-Y` scans only a channel map you import (`-C`, a config `chan_csv`, or
  an import during the session). With none, at start or after Clear channel map, it stays on its frequency until one is
  imported: channels a DMR site announces over the air are recorded, but `-Y` never steps to them, and neither the voice
  gate nor `--scan-max-visit-ms` ends a reception on their account. The Scan Timing row reads `No channel map`
  meanwhile, with no countdown. With a list, channels learned over the air (a DMR site's announcements, a P25 or NXDN
  grant resolved) are not a new list: a row whose tune lands late still becomes the row on air, with its name and
  keys, and no row's warning or `Skipped at every visit` status line is repeated for them.
- Live per-channel edits: while a `-Y` typed channel map or a `--trunk-scan` list runs, the row on air's squelch,
  channel width and tone policy, and a trunk-scan target's tuner gain, can be changed for the rest of the session from
  the terminal's and Qt/Android's "this channel" editors, without touching the list or the configured defaults; see
  [Live per-channel edits](trunk-scan.md#runtime-behavior).
  A channel map with a `name` column (see `docs/csv-formats.md`) names the row being listened to in the Scan Mode row,
  in Call Info, and on the event history rows recorded while it is tuned. A map can load a per-row key set from
  `keys_hex_csv`/`keys_dec_csv`, or embed `-b`/`-H` equivalents in `single_key_dec`/`single_key_hex`; leaving or pausing
  `-Y` (scanner toggle, trunk set, tuner release, a frequency entry that is not one of its rows) hands the foreground
  keyring back to the global keys. A row may not mix direct and file sources. Stopping `-Y` with `y` ends the calls
  heard on the row on air under its name and keys before they go; starting it with `y` ends the calls heard before under
  the name they had (after a manual tune, the operator's), and from then on the calls heard are the scan's.
  While scanning, the terminal's Trunking menu and hotkeys hold the scan on the channel on air (`Y`), avoid it for the
  rest of the session (`b`), step to the next channel (`L`, skipping avoided rows) or back to the previous one (`J`,
  the same rows backward; the scan then carries on forward from there), list the avoided rows and put one back, and
  clear all avoids; see `docs/ui-terminal.md`. A hold never keeps the scan on an avoided channel: when the step `b` asks
  for fails, the scan, held or not, moves on promptly, even while the channel's carrier is heard, retrying at the step
  interval (`-t`, at least a second) while nothing takes it, and a hold then holds the channel it lands on. The avoids
  outlive `-Y`: with it stopped the DMR and NXDN control-channel hunts under `-T` still skip those rows, and Trunking >
  Avoids still lists them, puts them back and clears them.
  Voice-only scan: `--scan-voice-only` steps on unless decoded voice frames hold the row. `--scan-voice-qualify-ms
  <100..600000>` (default `1000`) is the window after sync in which voice must appear or the scan moves on;
  `--scan-voice-hold-ms <100..600000>` (default `2000`) is the time to stay after the last voice frame. Encrypted
  voice without a key holds unless the talkgroup policy blocks it; unknown identity counts as voice. The last-media
  time survives an over-the-air terminator, so the full hold still runs when a protocol closes the call before the
  scanner's next tick. The voice gate never applies to an analog row, where there is no decoded voice to wait for:
  an `nfm` or `am` row, and every row of an untyped list scanned under `-fA` or `-fM`, holds on its carrier under `-t`
  instead, so `--scan-voice-only` never blocks one.
  Per-visit ceiling: `--scan-max-visit-ms <ms>` (also `--scan-max-visit-ms=<ms>`; `0` disables, otherwise
  `1000..3600000`; default `0`) is the longest one visit to a row may last, with or without `--scan-voice-only`. It is a
  ceiling, not a reason to stay, so it can cut an ongoing call short: that is the point on an open microphone, and why
  it is off by default. The clock starts when the row parks and starts again from zero on every re-park; sync, decoded
  voice, the voice-gate hold and the `-t` hangtime never restart it. Expiry hops only while at least two rows are usable
  (a non-zero frequency, not avoided, and not an analog row skipped at every visit because the DSP rate cannot fit its
  width); with fewer the limit re-arms instead of firing, so a one-row list is unaffected, a busy row beside rows that
  are all skipped keeps its call, and a row that becomes usable later gets a full limit rather than an immediate hop.
  A `Y` hold suspends the limit and releasing it starts a fresh full limit, not the remainder; an `-I` talkgroup hold
  suspends it only while the call being followed matches the held talkgroup, and the limit runs again from zero once
  that call ends. Other calls on the row are still capped. Suspensions observed across stalled input restart at the
  first eligible decoder tick; operator hold release restarts at the command. A typed row can carry its own
  `--scan-max-visit-ms <ms>` in the `options` column, where `0` exempts that row; legacy untyped rows apply row keys but
  not row options, so they always use the global value. A failed legacy hop keeps the row and restarts the visit and
  qualification windows so decoding can resume before another attempt. Two caveats on legacy lists: a row whose
  frequency also appears on the next row "hops" to the same frequency, ending the call as an explicit hop rather than
  moving the receiver, and a low `-t` already steps a quiet row about a second after its last sync, so the cap only
  changes what happens on a row that keeps syncing. Optional channel-map `mode` values select `p25`, `dmr`, `nxdn96`,
  `nxdn48`, `dpmr`, `dstar`, `ysf`, `m17`, `nfm` (analog narrowband FM) or `am` (analog AM) for each row. See [the
  mixed-mode example](../examples/conventional_scan_modes.csv) and [the mixed analog
  example](../examples/conventional_scan_analog.csv). Declared rows work even when excluded by the global preset; blank
  rows inherit it. An `nfm` row runs the analog FM monitor at its own `--nfm-bandwidth-hz` width (the row's `options`)
  or the configured NFM width (16 kHz by default; on audio input with a rigctl peer, `-B` stands in for an unset one),
  and an `am` row the AM monitor at its own `--am-bandwidth-hz` width or the configured AM width (6 kHz by default),
  switching the receiver between the FM monitor, the AM monitor and the digital decoder at each row without reopening
  the device, and holds under `-t` for as long as its carrier (squelch open) lasts, whether or not audio is played:
  `-o null` and a muted frontend no longer let the scanner leave an active analog row. `--scan-voice-only` never applies
  to an analog row. Details and the options an analog row accepts: [csv-formats.md](csv-formats.md#analog-rows). Modes
  take effect at the first scheduled row entry, including manual `L` and `J` steps. Existing dwell and voice-hold
  defaults remain unchanged. The open audio sink retains its rate/channel count while logical DMR slot decoding may
  change.
  Global mode and modulation commands update the saved configuration, and exiting scanning restores it. Blank-mode rows
  retain that saved configuration while AUTO hunts. Loading a configuration that disables scanning releases the row's
  decoder and keys. Manual `L` and `J` skip zero-frequency placeholders, while automatic scanning continues to park on
  them for the configured dwell.
  Pausing for a manual tune (issue #671): a frequency you enter wins over the scan, typed list or untyped, and pauses it
  rather than ending it, unless it is one of the list's rows (next paragraph). That is the terminal's frequency entry
  (`=`, or Input > `Frequency...`), Qt/Android **Go to** and the API's `rtl_set_freq`; a tuner release (Qt/Android
  **Explore from here**, the API's `tuner_release`) pauses the scan the same way without a tune. Once the tune is
  accepted (applied, or still pending on the tuner) the row on air's calls end under its name and keys, the configured
  decoder, receive settings and keys come back, and the receiver stays on your frequency
  (`Applied: frequency -> 154.000000 MHz; scan paused`); a refused or deferred tune changes nothing. With a rigctl peer
  under a typed row, the peer takes your frequency with the row's mode and passband, and the pause then puts the
  configured ones back. The list, its place, the avoids and live per-channel edits (not a `Y` hold) wait for `y`
  (Qt/Android **Resume scan**, the API's `scan_resume`), which carries on with the row the scan would have stepped to
  next (`Scan resumed`, or `Resuming scan: waiting for the tune` until that row's tune lands; calls heard meanwhile stay
  the operator's). A resume with no row to tune, or whose tune fails, leaves the scan paused and says why. While a
  resume waits on its row, a frequency entry (not of a row) or a spectrum tap takes the receiver back and the scan stays
  paused (`; scan paused`), and so does a Signal search start that goes on (it says nothing of the scan; one refused
  leaves the resume waiting); a stream reopen that drops the row's tune does the same, saying
  `Scan stays paused: the resume to ... was interrupted` (a typed list's row, whose tune the stop fails:
  `tune to ... failed`). While paused, another entry (not of a row) or a spectrum tap only tunes; `Y`, `b`, `J` and `L`
  answer `Scan paused: resume it first`, while putting avoided rows back (one, or all with "Clear avoids") still works,
  since it never tunes. Turning trunking on, an input switch that runs, or a channel-map import, clear or RadioReference
  import that replaces the list stops the paused scan; a save writes `[trunking] scanner = true` for a paused scan, so
  the next start runs it. The terminal shows the pause as `| Scan Mode: PAUSED (y resumes)`, and the Trunking menu's `y`
  row reads `[Paused]`.
  Typing a row's frequency (issue #644): a frequency entry (`=`, Qt/Android **Go to**, the API's `rtl_set_freq`) that is
  exactly a row's `frequency_hz` does not pause the scan: the scan goes to that row and holds it there, with the row's
  mode, options, keys, name and receive settings, as on any visit. A running scan steps to it; a paused one (or one
  whose resume is still waiting) resumes at it; a scan already on that row only holds it. The status line says
  `Holding: Marion` (`Going to Marion: waiting for the tune` until the tune lands, `Resuming scan at Marion: ...` over a
  paused list), naming a row by its `name`, else `row N`. With several rows on the frequency, the one on air wins, else
  the first in the file; the message adds `(1 of 2)`, and `L` and `J` reach the others. In a list with neither `mode`
  nor `options`, rows next to each other on one frequency are one channel, as `L` and `J` treat them: the entry takes
  the one the scan is on, else the first. When that first row is avoided the entry is a plain one (unless the scan is
  already on another row of the run), though the timed step still visits the others. The Scan Mode row reads
  `HOLD (y continues)`: `y` (the API's `scanner_toggle` or `scan_resume`) lets the hold go and the scan carries on from
  that row, where with an ordinary hold it stops `-Y`; `Y` and Qt/Android **Hold** let it go too. `L`, `J` and `b` keep
  the hold on the row they move to, and any pause or stop ends it. A row that is avoided or whose width the input cannot
  take is not gone to: the entry pauses the scan as above, and its message ends with the reason (`; Marion is avoided`).
  Only a typed frequency does this: a spectrum tap and a tuner release keep the configured settings. From the terminal,
  a row's frequency gets the configured settings only once `-Y` is stopped, since `y` stops a running list rather than
  pause it in place. After the radio stream restarts (a reconnect, a config that reopens the input), the scan is not
  sure of the row on air until its next tune, so typing that row's frequency tunes it again, which ends a call in
  progress. Learned channels, a map used under `-T` and a stopped `-Y` list match nothing. Under `--trunk-scan` a
  target's frequency does the same: see [Typing a target's frequency](trunk-scan.md#runtime-behavior).
- Single-tuner trunk scan mode: `--trunk-scan <targets.csv>`
  - Rotates one tuner across CSV-defined P25 trunk, P25 conventional (`p25-conventional`), DMR trunk, DMR
    conventional, NXDN trunk (`nxdn-trunk` NXDN96, `nxdn48-trunk` NXDN48), NXDN96 conventional
    (`nxdn-conventional`), NXDN48 conventional (`nxdn48-conventional`) and analog NFM and AM conventional
    (`nfm-conventional`, `am-conventional`, held on carrier) targets. Full guide: `docs/trunk-scan.md`.
  - Several lists: repeat `--trunk-scan <file>` to scan the targets of several files in one session (at most 8), and
    give `--trunk-scan-off <file>` for a list that starts switched off; both also take the `=FILE` form. Lists are
    numbered in command-line order across both options, and a list's name is its file name without folder and
    extension. A ninth list, the same path twice, a path of 1024 bytes or more and a start with every list off are
    refused. Lists on the command line replace a config file's lists as a whole. How the lists behave and are switched
    while scanning: [Several lists](trunk-scan.md#several-lists).
  - Start held on one target: `--trunk-scan-hold <target-id>` (also `--trunk-scan-hold=<target-id>`) starts the scan
    parked on the target whose `id` is the one given, and holds it there as typing that target's frequency does: the
    hold comes once the target's park lands (on its learned control channel first, for a trunked target), and `y`
    continues the scan from it. A park that is refused, deferred or fails later leaves the scan running unheld, on to
    the next target, and the status line names the target once. Ids are unique across every list, so the id names one
    target in whichever list holds it. The start is refused, naming the id and the reason, when no target has the id,
    when the target's list starts off (`--trunk-scan-off`, or `lists_off` in a config), and when the target's width
    does not fit the input, for example `--trunk-scan-hold: target "fire"'s width does not fit (NFM 20 kHz does not
    fit the 16 kHz DSP rate)`. Without a trunk scan (`--trunk-scan`, or a config's `[trunk_scan]` lists) the option
    is refused, saying why none runs: an option on this command line turned off the config's scan, or the config's
    lists are off (`enabled = false`). The id is taken whatever it reads like, so an id that starts with `-` works
    in both spellings. The start logs whether the target is held, holds once its tune lands, or was refused or
    deferred; a hold that comes once the tune lands shows as `HOLD (y continues)` on the Trunk Scan row, with no
    `Holding:` message. It has no INI key and is never saved: a start that wants the hold gives it again.
  - Requires a live retuning path: RTL-family input opened by DSD-neo, or rigctl control such as `-U 4532`.
  - Use per-target `chan_csv` (and `p25_bandplan_csv`) entries in the target CSV; leave both empty on conventional rows,
    including `p25-conventional` and the analog types. Global `-C` and `--p25-bandplan` are rejected in this mode. P25
    trunk targets that are sites of one system (same WACN/SYS) share the band plan one of them learned over the air.
  - Optional per-target `modulation` and `rtl_gain` columns can override demod hints and RTL-family tuner gain for the
    active target. Both P25 types accept `auto`, `c4fm`, or `cqpsk`. Optional `keys_hex_csv`/`keys_dec_csv` columns load a per-target key set, while
    `single_key_dec`/`single_key_hex` embed `-b`/`-H` equivalents; a target cannot mix direct and file key sources.
    Leaving the target restores the global keys.
  - Idle dwell: `--trunk-scan-dwell-ms <250..600000>` (default `3000`).
  - Conventional DMR/P25/NXDN activity hold (both NXDN rates), and an analog target's hold after its carrier
    drops: `--trunk-scan-activity-hold-ms <250..600000>` (default `1200`).
  - Maximum time per visit: `--scan-max-visit-ms <ms>` (`0` disables, otherwise `1000..3600000`; default `0`).
    Unlike the activity hold above it applies to every target type, trunked and conventional, and it is a ceiling
    rather than a reason to stay: it can cut an ongoing call short, which is the point on a busy system that would
    otherwise starve the rest of the list. A target's `options` column can override it, and `0` there exempts that
    target. Full rules in `docs/trunk-scan.md`.
  - `p25-conventional` parks without a trunking state machine and holds after voice starts allowed by the
    group/private and encrypted-call policy. PDU data never refreshes its hold, so `-e` has no effect on that row.
    Phase 1 decode captures are available for replay checks, not proof of on-air target holds; Phase 2 conventional
    parking is untested on air.
  - Voice-only scan (`--scan-voice-only`): digital conventional targets hold only from decoded voice, with `dwell_ms` as
    the qualify window and `activity_hold_ms` as the hold; trunked targets are unchanged (control-only rotates after
    dwell) and show no `Voice:` marker on the status line, and an `nfm-conventional` or `am-conventional` target keeps
    holding on its carrier. A conventional target shows `VOICE` while its call is active and `TAIL` after the call ends
    while the hold remains.
  - Cannot be combined with conventional `-Y` scan mode or IQ replay.
  - Single-tuner limitation: systems not currently parked can be missed while another target is being monitored.
- Channel map CSV: `-C <file>` (e.g., `connect_plus_chan.csv`). The channel column takes decimal, `0x2A46` hex, or
  the `2-2630` identifier-channel form printed after every P25 channel in the event history. Give `-C` once: a session
  uses one channel map, and a second `-C` is refused. To scan several files, give each its own `--trunk-scan` list.
- File paths: a path that does not fit is refused with "path is too long", never cut short to another file's name.
  `-C`, `-G`, `--trunk-scan`, `--trunk-scan-off`, `-k`/`-K`, `-w`, `-6`, `-c`, `-J`, `-L`, the `-Q` file name,
  `--frame-log`, `--wav-journal` and `--p25-sm-log` take up to 1023 bytes. `-i` (up to 2047) and `-o` (up to 1023) also
  take device specs, so theirs is refused with "value is too long".
- P25 band plan CSV: `--p25-bandplan <file>` — one row per band-plan identifier (base, spacing, type, offset,
  bandwidth, optional WACN/SYS) for sites that never broadcast `IDEN_UP`; rows yield to a real `IDEN_UP`. Format
  in `docs/csv-formats.md`, starter in `examples/p25_bandplan.csv`.
- Export the learned P25 band plan at clean shutdown: `--p25-bandplan-export <file>` — writes the identifier
  tables learned this run (every target's under `--trunk-scan`, tagged with their WACN/SYS) in the same format, so
  the next run can load them with `--p25-bandplan`. The terminal menu has the same action live. LCW-only entries
  export with an empty type and yield to a TDMA definition when reimported; typed definitions take precedence
  within the same system scope. Export warns when the 64-row limit omits entries. See `docs/csv-formats.md` for
  the offset encoding and channel-map invalidation rules.
- Group list CSV (allow/block + labels, optional `priority/preempt/audio/record/stream` policy columns; exact IDs,
  ranges and digit patterns such as `12xx` or `*`): `-G <file>`; pattern starter in `examples/group_patterns.csv`
- Source ID list CSV (radio ID names; with a `mode` header column also block, mute, recording and export rules by
  radio): `--src-csv <file>` (also `--src-csv=<file>`); format in `docs/csv-formats.md`, starters in
  `examples/src.csv` and `examples/src_policy.csv`. Source names prefer this list, then the group list's exact row;
  OTA alias text is unchanged. Radio rules only restrict, and a talkgroup Hold does not lift them. Loads regardless
  of trunking and is allowed with `--trunk-scan`.
- CSV formats and examples: `docs/csv-formats.md` and `examples/`
- Use group list as allow/whitelist: `-W`
- Tune controls: `-E` disable group calls, `-p` disable private calls, `-e` enable data calls, `--enc-lockout`
  enable key-aware encryption lockout (P25, DMR, NXDN, TETRA), `--enc-follow` follow encrypted grants without lockout
  (default)
  - With `--enc-lockout`, otherwise allowed P25 voice grants whose encryption is set or not yet known are tuned briefly
    as silent classification probes. Voice is not decoded, played, recorded, or streamed during classification.
  - HDU/LDU2 (Phase 1) or MAC_PTT/ESS (Phase 2) metadata confirms whether the call is clear or has a matching,
    complete key for a supported algorithm. Clear and decryptable calls continue; missing-key and unsupported calls
    are suppressed. On Phase 2, a clear companion slot remains active and on its original stereo side.
  - A target confirmed encrypted without a usable key is locked out for the rest of the session — grants for it are
    skipped without retuning, with no retry backoff. Lockouts release when a grant or corroborated voice shows the
    target clear (or decryptable), when key material changes (each locked target then re-verifies with one silent
    probe on its next grant), or via the menu's "Clear lockouts" action, which purges every target's
    ledger including the copies parked by trunk scan. DMR and NXDN lockouts share the same session ledger instead of
    writing "ENC LO" rows into the group list. While `--enc-follow` is active the ledger is suspended rather than
    erased, so toggling back to `--enc-lockout` does not owe a fresh probe per target.
  - A locked P25 supergroup stays locked for the whole patch: its member talkgroups are not locked themselves, but
    they do not readmit the supergroup's grants.
  - TETRA needs no probe: the D-SETUP and D-TX GRANTED say whether a call is end-to-end encrypted, and the MAC header
    whether it is air interface encrypted. A voice call the listener cannot decrypt (end-to-end encryption, or air
    encryption without a `--tetra-key` for the carrier the call is on: an 80-bit key covers every carrier, a TEA1
    reduced key its own) is locked out instead of followed, and a followed call that turns out so is left.
- Hold talkgroup: `-I <dec>`
- rigctl over TCP: `-U <port>` (SDR++ default 4532)
- Set rigctl bandwidth (Hz): `-B <hertz>` (e.g., 7000–48000 by mode), the FM passband the peer is asked for wherever it
  does not demodulate the analog monitor: on digital modes, and on an RTL-family input, where the peer only follows the
  frequency and is asked for `-B` alone. On audio input, where the peer demodulates, the analog monitor asks for its own
  passband instead (issue #621): the AM monitor (an `am` row or `am-conventional` target) sends `M AM <width>` with its
  own AM width, else the configured one, else the 6 kHz default, and the FM monitor (`-fA`, an `nfm` row or
  `nfm-conventional` target) sends FM at the row's own `--nfm-bandwidth-hz`, else the configured NFM width, else `-B`,
  which stands in for an unset NFM width, else at the peer's own passband. A peer that refuses a width skips the row; a
  refused `-B` keeps the effect it always had: none at a scan row's tune (unless the peer may still be on an `am` row's
  AM), a failed untyped `-Y` step or manual tune. Where the session runs the peer's own passband (`-B` 0, or the FM
  monitor with neither a width nor `-B`) the untyped `-Y` step and a manual tune ask for FM at the peer's own passband,
  best-effort: that undoes a passband this client set, and sends nothing to a peer it never changed or one back on its
  own, a reconnect since or not. Before this client first changes a demodulator's passband it asks the peer what it
  runs (`m`; on audio input `-B` on a digital mode counts, since the peer's FM passband is what the mode is heard
  through), so a later return to the peer's own (a row with neither a width nor `-B` to ask for, after one that set a
  passband) sends FM at the passband the peer reported, since SDR++ and GQRX keep their passband for `0` (a peer that
  cannot answer `m` still gets `0`). A scan's reading lasts the scan; one taken for the session's own passband off a
  scan is retired once the peer is known to run its own passband again (it takes it back, or refuses the session's
  request while still on it) and is never sent again: the next change reads again what the operator may have changed on
  the peer meanwhile, and a read that fails leaves it unknown, so a later return sends `0`. Leaving the scan returns
  the peer to the passband the session runs, with its own AM passband put
  back too ([csv-formats.md](csv-formats.md#analog-rows)). The session's width is asked of the peer once the input is
  open at start, again on a rigctl reconnect, and when a symbol replay ends and the session falls back to Pulse, which
  the peer then demodulates, best-effort; `-B` and the peer's own passband wait for the first tune, as before. A width
  or `-B` changed while running (Input > `NFM passband...`, Input > `Rigctl setmod bandwidth...`, the Qt/Android
  Passband sheet, a loaded config) is asked of the peer at once while it is the passband in force, and
  goes back if the peer refuses it; leaving the monitor for a digital mode puts the peer back on `-B`, or its own
  passband, at once, best-effort ([When changes apply](#when-changes-apply)).
- Hang time after voice/sync loss (seconds): `-t <secs>`
  - This is not the idle dwell between `--trunk-scan` targets. DMR control/rest-channel acquisition has its own
    two-second window; use target `dwell_ms` to budget each visit. See [trunk-scan timing](trunk-scan.md).
  - Hangtime and dwell decide when a quiet channel is done; `--scan-max-visit-ms` ends a visit that is not done, so
    neither substitutes for the other.
  - Under `-Y` without `--scan-voice-only`, the terminal's `Scan Timing` row counts down to the first whole-second
    tick when the time since the last sync exceeds `-t`. The `Scan Mode` row shows the configured `-t` value;
    the countdown total is `floor(-t) + 1` seconds. For example, `-t 6` shows `Hangtime: 6.00 sec` above a
    countdown with a `7.0s` total, while `-t 0` shows `0.00 sec` above a `1.0s` total. These describe the setting
    and the scanner's timing window, respectively. The one-second window for `-t 0` means waiting for the next
    clock-second boundary, not a guaranteed full second of silence after the last sync. NXDN holds the scan two
    extra seconds after each confirmed frame, so its countdown can initially exceed the displayed total.
  - P25 Talk Complete, TDU, TDULC, MAC_END_PTT, MAC_IDLE, and MAC_HANGTIME mark a transmission boundary. They close
    that slot's media and start or refresh the traffic-carrier inactivity timer without returning to the control
    channel. A follow-up PTT/ACTIVE on the retained carrier opens a clean call epoch without retuning.
    Immediate matching Phase 2 MAC_PTT retransmissions within one second are coalesced into the active epoch. Clear
    calls are matched by their source/target identity because their MI and KID fields do not delimit a call; encrypted
    calls retain exact crypto and identity matching. A later copy or any intervening accepted boundary still opens a
    genuine follow-up epoch.
  - P25 returns immediately only for an explicit network/channel release, policy or encryption rejection, manual
    release, or physical carrier/sync loss. Otherwise the configured hang time expires before control-channel return.
  - `--p25-sm-log` distinguishes `transmission_end`, `traffic_hang`, `traffic_reuse`, `hang_expired`, and
    `channel_release` events.
  - One receiver can provide uninterrupted follow-up handling only while the accepted traffic stays on its currently
    tuned carrier/slot. Grants on other carriers, control-channel-only grants, simultaneous calls, and RF/USB sample
    loss remain best effort with a single narrowband tuner.
  - Env (advanced): Optional hangtime extension when P25p1 IMBE error % is high:
    - `DSD_NEO_P25P1_ERR_HOLD_PCT=<percent>` (default 0 = off)
    - `DSD_NEO_P25P1_ERR_HOLD_S=<seconds>` (default 0 = off)
  - Env (DMR): Hangtime and grant timeout overrides:
    - `DSD_NEO_DMR_HANGTIME=<seconds>` — post‑voice hangtime before returning to CC
    - `DSD_NEO_DMR_GRANT_TIMEOUT=<seconds>` — max seconds waiting for voice after grant
  - Env (priority preemption):
    - `DSD_NEO_TG_PREEMPT_MIN_DWELL_MS=<ms>` — minimum time a followed call is held before a preempt talkgroup may
      displace it, measured from when it was first followed (default `750`)
    - `DSD_NEO_TG_PREEMPT_COOLDOWN_MS=<ms>` — minimum time between two preemptions (default `1000`)
    - Rules and per-protocol support: [Priority and preemption](csv-formats.md#priority-and-preemption)

During single-system P25 trunking, **Input > Frequency...** (or `=`) selects a new control channel and keeps trunk
following enabled. This also works in mixed `-ft` mode when P25 is active, including after sync loss on a learned
P25 control channel. An accepted tune ends current calls and relearns the NAC and site, while preserving explicit
identity/modulation overrides, same-network band plans (including `--p25-bandplan`), channel mappings, groups, and keys.
Subsequent calls return to the newly selected channel.
A failed or deferred request leaves the previous channel and calls intact; retry a deferred request. Pending tunes
receive the configured CC acquisition grace after hardware completion. If completion fails, hunting retries the
selected channel. Old site candidates are discarded; disk candidates require a freshly learned site and its own
cache file. Under `--trunk-scan` a frequency entry selects no control channel: it pauses the scan on the frequency
entered, and `y` resumes it, unless it is a target's frequency, which holds the scan on that target (see
[Runtime Behavior](trunk-scan.md#runtime-behavior)).

### Startup control-channel hunt

A single-system trunked session (`-T` with a channel map, not `-Y` or `--trunk-scan`) that starts on a frequency with
no live control channel searches the channel map for one. If no control message that passed its CRC check arrives
within the grace window (P25 `--p25-cc-grace`, default 5 s; DMR and NXDN 6 s), the receiver tries the map's rows in file order,
about two seconds each, and stays on the first one whose control passes the check. List every control-channel
frequency in the map: the start frequency is retried only if the map lists it, which a RadioReference import always
does.

- The search needs a tuner the decoder controls: an RTL-SDR, rtl_tcp, SoapySDR or Airspy input, or rigctl. An I/Q
  replay never searches.
- P25 searches under `-1`, `-2`, `-ft` and Auto (`-fa`, or no `-f`), `-mq` included; DMR under `-fs` and
  `-fr`; NXDN under `-fi` and `-fn` (an IDAS or NEXEDGE import). A P25-driven search also finds a DMR or NXDN96
  control channel: the first validated DMR control message, NXDN CAC or confirmed IDAS Type-D SCCH ends the P25 search
  and hands the session to that protocol (an IDAS Type-D site is then never searched, as below).
- Each P25 probe starts on the P25 chain (4800 symbols/s), whatever noise synced or the symbol-rate search tried before
  it. While a probe listens, the symbol-rate search steps only between P25's 4800 and 6000 symbols/s profiles: from
  4800 it steps to 6000 as a P25-only set does, not to an NXDN48 or ProVoice rate. That step comes late in the
  two-second probe, so a Phase 2 control channel the session has not learned gets little time at 6000 symbols/s, as
  under a P25-only set. The full Auto rotation resumes once the search ends.
- Under Auto a P25-driven search does not find an NXDN48 control channel (2400 symbols/s): start a dead NXDN48 system
  with `-fi`, which a RadioReference import sets. Once NXDN48 control has decoded, NXDN recovery owns the session.
- An EDACS session never runs the P25 search: EDACS control names no system the search could stop on, so it would leave
  a working EDACS control channel. That covers `-fh -fa` (or `-fe -fa`) and a switch to Auto from an EDACS/ProVoice
  preset, which keep the EDACS addressing selected.
- NXDN has no other control-channel hunt, so the same search also runs after an NXDN control channel is lost: once a
  CAC that passed its CRC (only an outbound control channel sends one) has named the control channel, 6 s without
  validated NXDN content starts it, revisiting that control channel between the other rows, and the next such CAC ends
  it where it is. Under Auto this runs once validated NXDN control has named the system. A call's return to the
  control channel restarts the window when its tune completes.
- An IDAS Type-D site has no dedicated control channel: an SCCH whose CRC repeated (a confirmed transmission) ends a
  startup search where it is, and no search runs after that until a RadioReference apply points the session at another
  system. A Type-D site that is idle and silent at session start is
  searched like a dead one, until a call's confirmed traffic holds the receiver.
- A simulcast (LSM/CQPSK) P25 system needs `-mq`, which a RadioReference import sets for such sites, when the C4FM
  chain decodes nothing of its control channel: each row gets about two seconds, too short for the automatic
  modulation search to reach CQPSK on a control channel the session has not decoded yet. One the C4FM chain reads in
  part moves to CQPSK by itself (the chain comparison in the hunt notes above).
- DMR content that passed the colour-code check (a call on a Hytera XPT repeater, say) restarts the P25 and DMR
  windows, and an NXDN frame whose own CRCs confirmed the transmission (a call on a traffic channel) restarts the P25
  and NXDN windows, so a search never leaves a frequency mid-call. Neither names the system. Sync alone, which noise can
  produce, never holds the search.
- A frequency entry, **Next channel** (`L`) or **Previous channel** (`J`), a radio configuration change, a
  RadioReference apply or an input switch restarts the window on the new frequency.
- Nothing is shown or used as the control channel until a searched frequency passes the CRC check, and the saved
  system is not changed. Once validated control names the system, the search ends there; losing that control channel
  later uses the ordinary hunt (NXDN as above).
- DMR searches until a control channel passes its check, not until DMR is merely recognised: a valid DMR message that
  confirms no control channel (a data preamble, a Capacity Plus status naming a rest channel the map lacks, a Tier III
  or Connect Plus traffic channel) leaves the search running, so the receiver moves on once that frequency goes quiet.
  Under Auto the search starts once validated DMR content names DMR. A Hytera XPT site, which has no control
  channel, ends it where it is, and its recovery only ever retunes the frequency it parks on.
- DMR recovery keeps its six-second loss grace and two-second probe windows even while data syncs continue without
  valid control, including after a Capacity Plus rest channel goes inactive. A probe's window starts when its backend
  tune completes; accepted control on that frequency ends the hunt. Every DMR window holds while DMR content passes the
  colour-code check (a call on the rest channel's frequency, say), so recovery never leaves mid-transmission.
- A followed DMR call (a grant, or a call a Capacity Plus status followed) is held by its own voice that passed the
  colour-code check, not by a voice sync alone; with no such voice it returns at the grant timeout, otherwise a
  hangtime after the last of it. With data calls followed (`-e`), data that passed the check holds it too. Repeated statuses naming the followed call change nothing.
- A frequency entry, **Next channel** (`L`), **Previous channel** (`J`) or **Return to CC** during DMR recovery gives a
  followed call up and listens on the new frequency for a full window before the ordinary hunt resumes; nothing heard
  there is credited to the old control channel.
- The search runs on the decoder's own pass, so it needs an input that keeps delivering samples. RTL-family inputs
  always do, and a silent UDP stream gives way to it after a short pause; a TCP audio stream with rigctl that stops
  sending pauses it until audio resumes.

## RTL‑SDR details (`-i rtl` / `-i rtltcp`)

- Fields: `dev` (device index), `freq` (Hz/MHz), `gain` (0–49), `ppm`, `bw` (kHz: 4, 6, 8, 12, 16, 24, 48 or 72; anything else runs at the 48 kHz default), `sql` (negative = threshold in dB, `0` = off, positive = linear mean power, `auto[+N]` = the [auto squelch](#auto-squelch---squelch-auto), `noise[+N]` = the [noise squelch](#noise-squelch---squelch-noise)), `vol` (monitor gain, 0–3; typical 1–3), optional `bias[=on|off]`. `bias` alone (or `b`) turns the bias tee on; a value
  is one whole word: `on`/`off`, `1`/`0`, `true`/`false` or `yes`/`no` (any case). A value or trailing option it cannot
  read is ignored with a warning and leaves the bias tee as it was; the startup `RTL #N:` line ends in `BIAS=on` when
  it is on. 72 kHz is TETRA's bandwidth: it gives `-fT` a whole 4 samples per symbol at 18 ksym/s. The other modes run
  best at the 48 kHz default: at 72 kHz their channel filter would need more taps than it has, and runs its wider fixed
  prototype instead.
- A `sql` value that is neither a number, `auto[+N]` nor `noise[+N]` leaves the squelch as it was rather than switching
  it off. A
  disabled squelch is reported as `off` everywhere it is shown — the startup banner, the terminal input line, the DSP
  panel — so it is never mistaken for a threshold gating at the −120 dB display floor.
- `--squelch <setting>` wins over `sql`, and `sql` over the config's `rtl_sql` keys.
- A scan row or target may carry its own `--squelch-db`, or `--squelch` (the same option). It uses these same dB units
  but only the negative and `0` forms, plus `auto[+N]` on analog rows and `noise[+N]` on nfm rows: the linear positive
  form is refused there. While
  the row is on air it replaces this setting; leaving it, or stopping the scan, restores this one. The squelch menu and
  Qt panel still edit this configured value, and a save writes it, never the row's.
- For DMR data/LRRP on direct RTL input, use `bw=48` when possible, or at least `bw=24`; lower basebands may still decode voice but corrupt data PDUs.
- Note: when EDACS follows an analog voice call and nothing decides when it ends, a bounded fallback watchdog releases
  the voice channel if no release marker is detected. That covers `sql <= 0`, `--squelch auto`/`noise` on audio input,
  and a channel the auto squelch has no plan for. Under `--squelch auto` or `noise` on a radio input, the squelch's gate
  ends the call ([Auto squelch](#auto-squelch---squelch-auto)).
- RTL USB, RTL-TCP, SoapySDR, and IQ replay digital decode run in the symbol domain. The digital decoder receives one
  normalized float per FSK or CQPSK symbol decision; discriminator audio is not used for digital decode.
- The trailing `vol` field and `rtl_volume` config key are monitor/non-symbol gain only. They do not scale RTL-family
  digital symbols, nor the FSK discriminator output that EDACS analog voice and the `-8` source monitor play (it
  already peaks near full scale). `-8` enables the separate source monitor tap.
- Examples:
  - `-i rtl:0:851.375M:22:-2:24:0:2`
  - `-i rtltcp:192.168.1.10:1234:851.375M:22:-2:24:0:2`
- External retune control (RTL/RTL‑TCP): `--rtl-udp-control <port>` listens on `127.0.0.1` by default. Use
  `--rtl-udp-control-bind <ipv4>` only when the unauthenticated listener must be reachable remotely.

Stall recovery (`-i rtl`)

- A dongle can stay plugged in and stop delivering samples with no error anywhere (its USB transfers all left pending;
  closing and reopening it does not help, a USB port reset does). After 5 s with no samples DSD-neo resets that dongle's
  USB device, reopens it, restores what it had taken (crystal, direct sampling, sample rate, offset tuning, tuner
  bandwidth, PPM, frequency, gain mode and gain, test mode or the RTL AGC, IF gains, bias tee) and resumes on the same
  channel. The log shows `RTL-SDR: the receiver sent no samples for <ms> ms; resetting the USB device`, where `<ms>` is
  the silence the receiver accrued (about 5000; the measured value prints), and then `RTL-SDR: the USB device was reset
  and reopened; the stream resumes`, and the terminal UI and the Qt app show the toast `RTL-SDR stopped sending samples;
  reset and reopened`. Decoding picks up after the gap as after a fade: the frame or call it cut ends, while the control
  channel, the voice channel being followed and trunk-scan ownership stay, and hangtime decides as usual. An
  `--iq-capture` keeps recording across the reset and marks the gap (see IQ Capture And Replay).
- One reset per stall: if the reopened dongle sends nothing either, or it stalls again within 60 s of the last reset, or
  the reset or the restore fails, the session ends as it does when the receiver is unplugged, with the reason in the log
  (`RTL-SDR: the receiver sent no samples for <ms> ms, none since it was reset and reopened; the stream stops`, `... ms
  after it was last reset; the stream stops`, `... resetting it failed: <step>; the stream stops`). A dongle that only a
  power cycle clears ends the session this way.
- Recovery runs only when DSD-neo can tell which USB device the dongle is, so that a reset never reaches another
  dongle (one another program may be streaming from): the dongle must report a nonempty serial, and its complete
  manufacturer/product/serial triple must identify one readable device of the RTL-SDR's vendor-specific USB class
  (whatever its VID:PID). Unreadable candidates refuse recovery, except devices this process cannot open for lack
  of permission or a usable driver, as described in [the code map](code_map.md). The build needs libusb-1.0 (configure prints `libusb for RTL-SDR stall recovery: ON`). Otherwise the open logs
  `RTL-SDR: stall recovery is off for this receiver: <reason>` (for example `another device has the same serial (set a
  unique one with rtl_eeprom)`: many dongles ship with serial `00000001`; `rtl_eeprom -d <index> -s <serial>` gives
  each its own) and a stall ends the session as before. The Android app recovers a stalled RTL-SDR in place too,
  through its own USB descriptor (`USBDEVFS_RESET` on it, then a reopen of the same descriptor), after a 2 s grace that
  lets a detach own an unplug; when that ends the session, Reconnect resets the dongle before it opens it (see
  `android/README.md`). There the descriptor names the dongle, so no unique serial is needed; recovery is off only when
  the descriptor's USB identity cannot be read (`... its descriptor's USB identity could not be read (errno <n>)`).
  When Android refuses that reset, the dongle is reopened without one, and a stall that persists ends with `... none
  since it was reopened (its USB reset was refused); the stream stops`. On Windows a WinUSB reset may not be a full
  port reset; whether it worked is judged by the samples that follow.
- While the dongle is being reset, a frequency, gain, PPM or bias-tee change from a frontend or the control API is refused
  with `Failed: the RTL-SDR is being reset; try again in a moment` and changes nothing; every other command (menus, an
  input switch, quit) applies as soon as it is queued.
- A receiver that has gone quiet but is not yet past the 5 s limit still counts as streaming, so a command queued then
  waits for the samples or for the verdict, at most those 5 s.

rtl_tcp connections (`-i rtltcp`)

- The server must open with the standard 12-byte rtl_tcp header (`RTL0`, tuner type, gain count); everything after it
  is I/Q, so no samples are lost after the header. A server that sends no valid header within 10 s fails the open
  (`rtl_tcp: <host>:<port> sent no rtl_tcp header (...)`).
- A connection is lost when the server closes it, a read fails, `DSD_NEO_TCP_MAX_TIMEOUTS` receive timeouts
  (`DSD_NEO_TCP_RCVTIMEO` each, 3 × 2 s by default) pass in a row, the throughput watchdog trips, or a command to the
  server fails. DSD-neo then logs `rtl_tcp: <reason>; attempting to reconnect to <host>:<port>...` and retries, backing
  off 0.2 s more per attempt up to 2 s, until the server answers (`rtl_tcp: reconnected on attempt <n>.`) or the
  session ends. A reconnected server gets every setting the session last had taken (crystal, direct sampling,
  frequency, sample rate, offset tuning, PPM, gain and the RTL AGC or test mode, IF gains, bias tee), so a server that
  restarted with other defaults ends up where the session was, and decoding resumes after the gap as after a fade. The
  I/Q the server sends before it has taken those settings is dropped, and an `--iq-capture` marks the gap.
- While no server is connected, queued commands still apply: menus, an input switch (from the menu or a loaded config)
  and quit work at once. A frequency, PPM or bias-tee change is refused with `Failed: the rtl_tcp server is not
  connected` and changes nothing; the reconnected server gets the last settings that took. Ctrl+C, `q` and the control
  API's `quit` end the session at once, also while the reconnect waits or while a stalled USB dongle holds the decoder.

Advanced (env)

- `DSD_NEO_RTL_DIRECT=0|1|2|I|Q` — Direct sampling selection (0 off; 1 I‑ADC; 2 Q‑ADC).
- `DSD_NEO_RTL_OFFSET_TUNING=0|1` — Disable/enable offset tuning (default is to try enabling). Tuners that support it
  (E4000, FC0012/FC0013, FC2580) displace the LO and drop the Fs/4 shift; R820T/R828D and direct sampling refuse it
  and keep the Fs/4 shift.
- `DSD_NEO_RTL_XTAL_HZ` / `DSD_NEO_TUNER_XTAL_HZ` — Override crystal refs in Hz (optional).
- `DSD_NEO_RTL_IF_GAINS="stage:gain[,stage:gain]..."` — Set IF gain(s); gain in dB (e.g., `10`) or 0.1 dB (`125`).
- `DSD_NEO_RTL_TESTMODE=0|1` — Enable librtlsdr test mode (ramp) instead of I/Q (for diagnostics). On a USB dongle
  and over rtl_tcp test mode stays on through gain changes (and through an rtl_tcp reconnect), and turning it off gives
  the RTL AGC (`DSD_NEO_RTL_AGC`) back to automatic gain.
- `DSD_NEO_RTL_VERIFY=0|1` — Retry and verify local USB RTL-SDR control applies (default on).
- `DSD_NEO_RTL_VERIFY_ATTEMPTS=1..10` — Total local USB apply attempts when verification is enabled (default 10).
- An rtl_tcp reconnect and a USB stall recovery reapply every one of these settings the dongle had taken.

## SoapySDR details (`-i soapy`)

- Use this path for non-RTL radios exposed through Soapy modules. The backend requires SoapySDR 0.8.1 or newer.
- Typical workflow:
  1. Install SoapySDR 0.8.1 or newer and the module for your radio.
  2. Verify Soapy is installed and check plugin search paths: `SoapySDRUtil --info`
  3. Enumerate radios: `SoapySDRUtil --find`
  4. Probe one candidate and capture its args: `SoapySDRUtil --probe="driver=sdrplay"`
  5. Start with `-i soapy` (default args) or `-i soapy:<args>`
- `soapy[:args]` sets backend/device selection only.
- Optional shorthand supports RTL-style startup tuning in the same `-i` string:
  `soapy[:args]:freq[:gain[:ppm[:bw[:sql[:vol]]]]]`.
- A number with a `K`, `M` or `G` suffix in the `freq` place is always a frequency, never device args, signed or with
  an exponent too (`+5.8G`, `5.8e0G`). One outside 1 MHz to 4294.967295 MHz (`soapy:driver=hackrf:5.8G`,
  `soapy:500k`) is an error at startup and in `--print-config`:
  `Soapy frequency '5.8G' is out of range (1 to 4294.967295 MHz)`. A bare number out of range there still makes the
  whole string device args.
- Those trailing fields map to existing shared controls and keys:
  `rtl_freq`, `rtl_gain`, `rtl_ppm`, `rtl_bw_khz`, `rtl_sql`, `rtl_volume`.
- Additional `[input]` keys expose Soapy-specific capabilities: `soapy_profile`, `soapy_stream_format`,
  `soapy_antenna`, `soapy_clock`, `soapy_settings`, `soapy_gains`, and `soapy_bandwidth_hz`.
- `soapy_settings = "key=value[,rx:key=value...]"` writes driver settings through Soapy. For example, SDRplay
  modules may expose `rfnotch_ctrl`, `dabnotch_ctrl`, `biasT_ctrl`, `agc_setpoint`, or `rfgain_sel`.
- `soapy_gains = "NAME:dB[,NAME:dB...]"` uses named gain stages and takes precedence over aggregate `rtl_gain`.
- `digital_resample = "auto|on|off"` controls resampling of the digital FSK stream to the resampler target rate.
  `auto` engages only when the device picked its own rate, which is what devices with a coarse rate grid
  (RX-888/SDDC, Airspy, SDRplay) do, and the rate yields a non-integer samples-per-symbol. `on` resamples whenever the
  target rate can produce an integer samples-per-symbol; `off` always keeps the raw demod rate. The key is not
  Soapy-specific: it governs the whole rtl-family demod chain (RTL USB, RTL-TCP, SoapySDR, native Airspy, and IQ
  replay). A device that forces its
  own rate now runs at the DSP bandwidth, as an RTL-SDR does, so `auto` looks at that bandwidth: an Airspy at 24 kHz
  gives 4800 sym/s a whole 5 samples per symbol and is no longer resampled to 48 kHz, while at 12 kHz (2.5 samples)
  it still is. A mode change that moves the symbol rate (ProVoice's 9600 sym/s at 24 kHz) makes the same decision
  again.
- RX-888 family radios use the SDDC Soapy module and the `sddc` profile; see `docs/soapysdr.md` for the antenna,
  ADC clock, and throughput requirements.
- `--print-config` reflects shorthand as normalized config fields (`soapy_args` + `rtl_*`) rather than the raw input
  string.
- If your Soapy args string itself contains `:`, prefer config keys (`soapy_args` + `rtl_*`) to avoid ambiguity.
- `rtl_device` index selection is for `rtl` input and is ignored in Soapy mode.
- Set an explicit `rtl_freq` for predictable startup frequency (otherwise defaults may not match your target system).
- Some shortcuts are RTL/RTL-TCP specific and not supported in the Soapy backend path (RTL bias-tee UI/CLI shortcut,
  direct sampling, offset tuning, xtal/IF-gain/testmode controls, RTL-TCP autotune). Use `soapy_settings` for
  driver-specific controls when the Soapy module exposes them, such as SDRplay `biasT_ctrl`.
- dsd-neo has no native SDRplay input; SDRplay radios run through SoapySDR. Airspy R2 and Mini have a native input
  of their own (`-i airspy:...`, see [the native Airspy guide](airspy.md)), and can also run through SoapySDR.
- The Soapy backend requires an RX stream format of `CF32` or `CS16` from the driver; `soapy_stream_format = "auto"`
  prefers a supported native format first.

Troubleshooting:

- If you see `SoapySDR backend unavailable in this build.`, rebuild with Soapy enabled and SoapySDR 0.8.1 or newer
  installed.
- If Soapy device discovery fails, verify Soapy modules are installed and `SOAPY_SDR_PLUGIN_PATH` includes the module directory for your driver.
- If logs report `invalid args string` or `failed to create device`, re-check your `soapy:` args from `SoapySDRUtil --find` / `--probe`.
- If logs report `invalid soapy_settings`, `setting ... is unavailable`, or `failed to write setting`, compare your
  setting names and allowed values with `SoapySDRUtil --probe="<args>"`.
- If logs report `RX stream formats do not include CF32 or CS16`, that driver/device stream format is not currently usable in this backend.
- If logs report `SoapySDR: RX overflow count=...`, try lowering `rtl_bw_khz` (config key; for example 48 -> 16) and reduce system load.
- Capability support varies by driver/device. Some radios do not support one or more of: frequency correction (PPM), manual gain range, or bandwidth control.
- Sample rate and gain requests may be clamped or adjusted by the driver; if decode quality is poor, verify the applied values in your SDR driver tooling and tune within supported ranges.
- Full non-RTL setup guide: `docs/soapysdr.md`.

## M17 Encoding

- Stream encoder: `-fZ` with `-M M17:CAN:SRC:DST[:INPUT_RATE[:VOX]]`
- BERT encoder: `-fB`
- Packet encoder: `-fP`

The local encoders emit unencrypted, unsigned frames. M17 CSMA channel access is not implemented; see
`docs/m17-support.md`.

M17 `-M` details

- `CAN` 0–15 (default 7; values > 15 clamp to 15)
- `SRC`/`DST` up to 9 UPPER base40 chars (` ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.`)
- `INPUT_RATE` default 48000; a multiple of 8000 up to 48000 (any other value warns and uses 48000)
- `VOX` enable with `1` (default `0`)

Input, squelch and VOX

- On a radio input the encoder reads the FM monitor with de-emphasis on the WIDE channel, never CQPSK, whatever digital
  decode flags or `DSD_NEO_CQPSK` say.
- VOX decides per 40 ms read (two codec2 frames at 3200 bit/s). A read is heard when the squelch is open for any of it:
  - a level squelch: the channel power on a radio input, the input level on audio input;
  - `--squelch auto` or `noise` on a radio input: the gate on each sample. Closed samples fade to silence (5 ms in,
    10 ms out) before encoding, as a level squelch's closed block reaches the encoder silent.
- After 11 closed reads, at the next LICH superframe boundary (up to 240 ms more), the transmitter unkeys. The last
  frame carries the end-of-stream bit and the EOT marker follows it, without dead air, so a quick re-key is not
  delayed.
- On audio input `--squelch auto` and `noise` run no squelch for the encoder: VOX stays keyed, and the engine warns at
  startup. Use a level there.
- A transmission still open when the encoder stops (an exit, or the input ending) ends the same way, followed by 1 s of
  dead air, as a manual unkey's does.
- A manual unkey while VOX still hears a carrier ends that stream the same way, with its dead air; VOX then keys a new
  stream, with its own LSF and, when monitored, its own call.
- On a local audio output the encoder lets the last frame and the EOT marker play before it queues the dead air, so a
  full output cannot drop them. At shutdown it lets everything queued play before the output closes, a VOX unkey's
  end included. The packet encoder (`-fP`) ends the same way.

Monitor

- Without `-8` the stream encoder decodes each frame it sends and plays the voice on the digital output, opened at
  8 kHz mono, as a received M17 call plays: with `-g`, `-y` and the high-pass. With `-8` the 48 kHz transmit baseband
  plays on the analog output instead. An explicit `-8` works before or after `-fZ`.

Examples

- `dsd-neo -fZ -M M17:9:DSD-NEO:ARANCORMO -i pulse -6 m17signal.wav -8 --frontend terminal`
- `dsd-neo -fP -M M17:9:DSD-NEO:ARANCORMO -6 m17pkt.wav -8`

## Keys & Privacy (advanced)

By default, DSD-neo redacts radio keys and keystream material in logs and terminal status. Add `--show-keys` to reveal
those values for the current CLI run only. Successful direct-key loading itself does not emit diagnostics.
The `-b`, `-H`, `-R`, and `-1` options accept zero as a supplied key and arm decryption; encrypted-lockout still controls audibility.
A key load leaves the DMR encrypted-audio mute as it is: a call the key decrypts plays, one it cannot decrypt stays
muted (turn "Mute encrypted audio" off to hear undecryptable audio). It turns the P25 unmute override off, as before,
so undecodable P25 audio stays muted.
Presence is retained separately from the numeric value, including all-zero RC4 and AES keys, across startup,
live key commands and scoped scan-key restoration. Entering zero supplies a key; omitting the option starts without one.
Out-of-range decimal keys are rejected without changing the installed key.

- Basic Privacy key (decimal, 0–255): `-b <dec>`
- Hytera 10/32/64‑char BP or AES‑128/256 key (hex, groups of 16): `-H '<hex…>'`
- dPMR/NXDN scrambler (decimal, 0–32767): `-R <dec>`
- RC4/DES key (hex): `-1 <hex>`
- TYT Basic Privacy (16‑bit, hex, enforced): `-2 <hex>`
- TYT Advanced Privacy PC4 (128/256-bit hex stream, groups of 16): `-! '<hex…>'`
- Retevis Advanced Privacy RC2 (128/256-bit hex stream, groups of 16): `-@ '<hex…>'`
- TYT Enhanced Privacy AES‑128: `-5 '<16 hex digits> <16 hex digits>'` (two words, one quoted argument)
- Baofeng AP PC5 key override (hex): `--dmr-baofeng-pc5 <hex>` (32 or 64 hex chars)
- Connect Systems EE72 key override (hex): `--dmr-csi-ee72 <hex>` (18 hex chars)
- Vertex ALG `0x07` key->keystream map CSV: `--dmr-vertex-ks-csv <file>` (`key_hex,bits:hex[:offset[:step]]`)
- TETRA air interface cipher key: `--tetra-key mcc,mnc,<20 hex digits>[,tea1|tea2|tea3]` loads the 80-bit cipher key
  of one network: the SCK of a security class 2 cell or the CCK of a class 3 cell, never a derived encryption cipher
  key (ECK). Another 80-bit key for the same network replaces it. It decrypts trunked mode speech of calls whose MAC
  signalling is air encrypted; end-to-end encryption and direct mode encryption are never decrypted. A network does
  not broadcast its algorithm, so name it: a key that names `tea1`, `tea2` or `tea3` (either case) always runs that
  algorithm. A key that names none runs the network's algorithm only once its signalling (a D-OTAR SCK Provide of a
  trunked mode SCK or a GCK Provide, or the ciphering parameters of a D-LOCATION UPDATE COMMAND or REJECT) has named
  exactly one of TEA1-TEA3, and not
  while it has named two or more: a network may support several algorithms (EN 300 392-7 clause 6.3.1). TEA4 has no
  key stream generator here.
  The key stream generator takes the ECK that TB5 derives from the cipher key with the carrier number, location area
  and colour code of the carrier being received (EN 300 392-7 clause 6.3.2.2): the followed call's carrier while
  trunking (`-T`), otherwise the carrier whose downlink frequency is exactly the tuned frequency of an RTL-SDR or I/Q
  replay input, on the control channel too (a secondary carrier broadcasts the main carrier's SYSINFO as well). Until
  the carrier number, a SYSINFO's location area and a BSCH's colour code are known, nothing is decrypted.
  The key decrypts the air encrypted signalling too (EN 300 392-7 clause 6.7.1.2: a MAC-RESOURCE's channel allocation
  and TM-SDU, MAC-FRAG and MAC-END fragments, MAC-D-BLCK), so call set-up and release, channel allocations, short data
  and status messages of an encrypted cell are decoded and `-T` follows its calls. Encrypted signalling carries no
  check of its plaintext, so a wrong key, a wrong TEA or traffic under a key not loaded (a DCK, a group key, another
  key version) would decrypt to noise: a decrypted message is acted on only once a repeat proved it, a copy with the
  same header and length sent in another slot, so under another key stream, decrypting to the same plaintext under
  the same key (the network repeats its allocations, and the D-SETUPs of calls in progress). The first copy is not
  acted on, so a call is followed a repeat later than on a clear cell; a message sent once or of fewer than 48
  encrypted bits (most D-TX CEASED and D-RELEASE messages, a single SDS) is never acted on, and a network that never
  repeats its allocations is not followed. A key that names no TEA is tried with each of TEA1-TEA3, and the one a
  repeat proves is the network's from then on, for speech too. The addresses of encrypted signalling are encrypted
  short identities: the key decrypts them with TA61 (clause 4.2.6) once it decrypted the message, which gives the
  parties only when it is the SCK or CCK the cell uses for identities; a DCK or a group key given as the network key is
  not supported, and a TEA1 key recovered for one channel only gives wrong identities. The speech of a call decrypted
  this way runs the key and TEA that proved its signalling, until the cell announces another key (another SCK-VN or
  CCK-id, SCKN or GCK support in its SYSINFO, or the encryption mode's key version parity changing in its signalling)
  and a repeat proves the call again.
- TETRA TEA1 reduced key: `--tetra-key mcc,mnc,<8 hex digits>,la=N,cc=N,carrier=N` loads TEA1's 32-bit key register
  (the reduced ECK) of one channel: TEA1 compresses the ECK into that register before it generates anything, so the
  register alone sets the key stream, and since TB5 made that ECK from the channel's location area (0-16383), colour
  code (0-63) and carrier number (0-4095), it decrypts that channel only. It runs TEA1 whatever the network's
  signalling named, and on its channel it is preferred over the network's 80-bit key. It decrypts the traffic
  encrypted under the cipher key it came from (in a class 2 cell the SCK every terminal uses; in a class 3 cell one
  CCK or group key, not the DCK of individual traffic) until that key changes. It decrypts the encrypted signalling of
  its channel in the same way as an 80-bit key, but holds no 80-bit key for TA61: the addresses stay encrypted short
  identities, so a D-SETUP names no called party and `-T` does not follow the call. Another reduced key for the same
  channel replaces it; each carrier of a cell needs its own. The Network row of Call Info and of the Qt/Android
  monitor reads `MCC 262 · MNC 1001 · LA 4097 · CC 25 · carrier 654` once the location area and the carrier being
  received are known, every field the key is bound to, and the Security row names the key that applies there
  (`TEA1 reduced key`, `TEA2 key`).
- At most 16 TETRA keys of both kinds are held. A refused `--tetra-key` names the grammar, never the key.
- Which TEA a network uses. A network never broadcasts its algorithm: SYNC and SYSINFO carry none, and security class
  2 or an SCK says nothing about it (EN 300 392-7 clause 6.3.1: a network's SCK is common to every TEA it runs). Only
  messages addressed to radios name it, and the TETRA row of Call Info and of the Qt/Android monitor shows what a
  readable one named:
  - The network's own word, on the downlink: the ciphering parameters of a D-LOCATION UPDATE COMMAND or REJECT, or a
    D-OTAR key provide of trunked mode keys, which names those keys' algorithm (`TMO downlink · encrypted (TEA2)`).
    On an encrypted network these messages are usually encrypted too, and rare.
  - With the key: load the 80-bit key without a TEA. Once repeated encrypted signalling proves one of TEA1-TEA3,
    `-v 2` logs `[TETRA AIR] ... proved by a repeat under TEA1` and the TETRA row names it; that needs signalling the
    network repeats under that key, so loading the right key alone does not guarantee it. The Security row's
    `TEA1 key` is the key and algorithm that apply to the carrier, not that proof.
  - A clue from the uplink: a radio's U-LOCATION UPDATE DEMAND names the algorithm it proposes, which the network
    accepts or rejects (clause 6.6.2.1), so it is a radio's choice, not the network's confirmation. Received with
    `--tetra-uplink --tetra-net mcc,mnc,colour` while radios register, it shows as
    `TMO control uplink · clear · radios propose TEA1`. Only a registration sent in the clear is readable: a radio
    keeps the encryption state it starts a registration in until the network accepts it (clause 4.4.2.0). In a class 3
    cell a radio's first registration is clear when it visits from another network, or at home when the cell offers
    no DCK retrieval or it holds no valid keys (clause 6.6.2.2), and encrypted when it registers with keys it already
    holds. In a class 2 cell the standard leaves it open, and a radio holding the SCK may register encrypted. Only
    the first MAC block of a registration is read.
  - Otherwise the row reads `encrypted (TEA not broadcast)`.

  The uplink frequency is the downlink frequency minus the network's duplex spacing, or plus it under reverse
  operation (EN 300 392-2 clause 21.4.4.1). The `[TETRA SYSINFO]` line prints the duplex spacing code `ds=`, whose
  value in MHz for the band is in ETSI TS 100 392-15, and `rev=` (0 subtract, 1 add). Radios transmit at low power,
  so the uplink is heard near them.
- Kenwood 15‑bit scrambler (decimal): `-9 <dec>`
- Anytone 16‑bit BP (hex): `-A <hex>`
- Generic keystream (length:hexbytes, optional frame align): `-S <bits:hex[:offset[:step]]>` (e.g., `-S 49:123456789ABC80`, `-S 168:<hex>:0:49`)
- For Vertex Std voice ALG `0x07`, prefer `--dmr-vertex-ks-csv` for repeatable key->keystream mapping. Use `-S` for
  one-off manual keystream experiments.
- M17 stream scrambler: `--m17-scrambler-key <hex>` accepts a nonzero seed of 2, 4 or 6 hex digits (8/16/24 bit).
- M17 stream AES: `--m17-aes-key <hex>` accepts 32, 48 or 64 hex digits (128/192/256 bit). These are separate
  from M17 signature-verification keys. The generic `-H` grammar remains unchanged.
- `--no-decryption-keys` explicitly installs an empty set, including in supported scan-row options.
- `--dmr-tg-key-clear` explicitly clears group-to-key selection overrides. Scoped rows restore their baseline on exit.
- `--key-profile-ref <opaque-id>` attaches a nonsecret frontend profile revision (1–63 letters, digits, `_` or `-`)
  to the installed key set. It does not supply key material.
- Import keys CSV (decimal): `-k <file>`
- Import keys CSV (hex): `-K <file>`
- A runtime key import or clear (Keys menu, `IMPORT_KEYS_*` commands) edits the global keys even while a keyed
  `-Y` row or trunk-scan target is parked, so the change survives the next hop. Scalar key commands (`-b`/`-1`/
  `-H`/`-R` legacy menu entries) can disarm the keyloader and be overwritten by a keyed hop. The typed
  `KEY_DIRECT_SET`/`DECRYPTION_APPLY` frontend entry points instead update the global baseline beneath an
  active keyed row; the row keeps its effective keys. See [decryption scope](decryption-profiles.md).
- Each key option applies only to the protocol whose voice it decrypts, so one decoder (the default AUTO preset, a
  scan, or `-fa`) can carry keys for several systems: DMR privacy (`-b`, `-H` as a Hytera BP key, `-4`), the vendor
  scramblers (`-!`, `-@`, `-5`, `-9`, `-A`, `-2`, `--dmr-baofeng-pc5`) and `--dmr-force-algid` act on DMR voice only;
  the scrambler `-R` (and `-4 -R`) on NXDN and scrambled dPMR; P25 ALG IDs on P25 Phase 1 and 2 (and on DMR, which
  carries them). Clear dPMR, P25 Phase 2 and YSF voice is never put through another protocol's key. The generic `-S`
  keystream applies to DMR, P25 Phase 2 and clear dPMR voice. A P25 or DMR frame that would need keystream past what
  was generated for it (a long call whose MI never refreshes) is muted instead of being played with a wrong keystream.
- Force key over identifiers: `-4` (DMR BP/NXDN scrambler), `-0` (DMR RC4 when PI/LE missing),
  `--dmr-force-algid <hex>` (DMR ALGID when PI/LE missing; a fallback only — an ALG ID or KEY ID
  received over the air via PI header/LE always takes precedence; `-M` is reserved for M17 in DSD-neo).
  The forced value is installed on the first voice frame; a voice LC that arrives before that (every
  trunked call's first LC, since tuning clears the slot's ALG ID) is classified — for the call label and
  the `--enc-lockout` decision — under the same forced ALG ID and key the voice path is about to use,
  without writing it to the slot.
- Select DMR key by talkgroup: `--dmr-tg-key-csv <file>` (per-TG override of the signaled KEY ID;
  rows are `tg_dec,keyid_hex` into the `-K`/`-k` keyring — see `docs/csv-formats.md`)
- Conflicting outer force switches emit one warning describing the final selection. Existing precedence is retained:
  long-option ALGID settings are applied before short options, and the last `-4`/`-0` wins. Use per-row settings for
  mixed scans. `--no-force-key` explicitly disables forcing after those settings; `--strict-crc` similarly disables
  `-F`. `--no-scan-voice-only` is the inverse of `--scan-voice-only` (the last of these long switches wins).
- Channel maps and trunk-target lists can combine mode/type with a restricted `options` column, including direct
  `-1` RC4/DES and `-R` scrambler keys, group files, force settings, CRC policy and conventional voice-gate intervals.
  See [scoped row options](csv-formats.md#scoped-row-options) for the supported switches and inheritance rules.
  The outer conventional command remains `-Y -C <file>`; `-Y` does not take a filename argument.
- Disable DMR Late Entry IDs: `-3` (avoid false ENC). It turns off the DMRA single-burst encryption identifiers and
  reverse-channel parsing for the session; Kirisun and Hytera Enhanced signalling no longer turn them back on or off.
  A Kirisun or Hytera Enhanced call changes late-entry handling for its own slot and call only: Kirisun reads its own
  identifier (also under `-3`), and Hytera Enhanced skips DMRA late-entry MI collection.

## Tools & Extras

- DMR Tier III LCN calculator:
  - `--calc-lcn <file>` (CSV of freqs)
  - `--calc-cc-freq <freq>` and `--calc-cc-lcn <num>` (anchor)
  - `--calc-step <hz>` (override channel step)
  - `--calc-start-lcn <num>` (start when no anchor)
- RTL auto‑PPM drift correction:

  - `--auto-ppm` enable
  - `--auto-ppm-snr <dB>` set SNR gate (default 6)

- RTL‑TCP networking:
  - `--rtltcp-autotune` enable adaptive tuning of buffering/recv size for RTL‑TCP links.

## Environment Variables (Advanced Tuning)

These environment variables provide fine‑grained control for power users.

Export them before starting `dsd-neo`. An option that sets one of them (`--auto-ppm`, `--input-volume`, the P25
timing options and others below), an `[input] auto_ppm` or `[dsp]` config key, and the terminal menus' toggles and
environment editor override the variable inside the process only, for the rest of that session; they never change
the process environment, so a child process or `/proc/self/environ` still shows what was exported. An option wins over
an exported variable; an exported variable wins over the config keys. A host that runs one session after another in
one process (the Android app) starts each session from the exported values, without the last session's overrides.

Control/telemetry API (fallbacks for the `--api*` options, read only when the option is absent and the API is enabled)

- `DSD_NEO_API_PORT=<port>` — enable the API on this port (as `--api`); `--api 0` still disables it
- `DSD_NEO_API_BIND=<ipv4>` — bind address (as `--api-bind`); a non‑loopback bind requires a token
- `DSD_NEO_API_TOKEN=<secret>` — shared-secret token (as `--api-token`)
- `DSD_NEO_API_TOKEN_FILE=<path>` — read the token from this file (as `--api-token-file`)
- An invalid value is an argument error. Protocol, telemetry and command reference: [docs/api.md](api.md)

Auto‑PPM (RTL‑SDR)

- `DSD_NEO_AUTO_PPM=1` — enable carrier/error-based drift correction with spectrum fallback
- `DSD_NEO_AUTO_PPM_SNR_DB=<dB>` — SNR gate (default 6)
- `DSD_NEO_AUTO_PPM_PWR_DB=<dB>` — absolute peak gate (default −80)
- `DSD_NEO_AUTO_PPM_ZEROLOCK_PPM=<ppm>` — zero‑step lock guard (default 0.6)
- `DSD_NEO_AUTO_PPM_ZEROLOCK_HZ=<Hz>` — frequency lock guard (default 60)
- `DSD_NEO_AUTO_PPM_FREEZE=0/1` — freeze retunes during training (default 1)
- `DSD_NEO_P25_AFC_STATUS_GATE=1` — opt in to suppress P25 Phase 1 auto-PPM updates when status symbols classify a frame as subscriber-originated or unknown. Default is advisory only because this direction hint is not reliable on every system.
- `DSD_NEO_P25_SOFT_ERASURE_THRESHOLD=<0..255>` — shared P25 soft-decision erasure threshold override; defaults to 64. Lower values are more conservative; higher values expand ranked erasure retries.
- `DSD_NEO_P25P1_SOFT_ERASURE_THRESHOLD=<0..255>` / `DSD_NEO_P25P2_SOFT_ERASURE_THRESHOLD=<0..255>` — phase-specific soft erasure threshold overrides. P25P2 keeps a balanced minimum weakest-symbol prefix even when all symbols are above threshold.
- `DSD_NEO_P25_SOFT_HARD_OVERRIDE=0|1` — allow conservative soft candidates to override hard-corrected P25 FEC output; default is enabled with strict gates.

Resampler

- `DSD_NEO_RESAMP=48000` — target rate (default); `off` or `0` to disable

CQPSK timing

RTL-family FSK digital decode selects its own symbol timing and normalization internally, deriving samples-per-symbol
from the active SPS hunt profile rather than from the front end's published symbol rate. CQPSK uses the OP25-style
Gardner/Costas/FLL-band-edge symbol chain.

- `DSD_NEO_TED_GAIN=<float>` — CQPSK/OP25 Gardner timing loop gain override, 0.01 to 0.5 (the menu's range; a value
  outside it is taken as the nearest end, and one that is not a number is ignored). Without it the gain is OP25's
  0.025, and the TETRA receive chain's own 0.07 while that chain runs (`-fT`). The terminal's CQPSK timing gain menu
  row and the DSP panel's `g:` show the gain set, or 0.025 when none is, also while the TETRA chain runs 0.07
- `DSD_NEO_IQ_DC_BLOCK=1` — enable DC blocker
- `DSD_NEO_IQ_DC_SHIFT=<k>` — DC shift coefficient

Digital SNR squelch

- `DSD_NEO_SNR_SQL_DB=<dB>` — skip sync when SNR below threshold

Capture/retune behavior

- `DSD_NEO_COMBINE_ROT=0|1` — select the two-pass or combined CU8 rotation transform (default 1)
- `DSD_NEO_DISABLE_FS4_SHIFT=1` — disable +fs/4 capture shift
- `DSD_NEO_OUTPUT_CLEAR_ON_RETUNE=1` — clear output on retune
- `DSD_NEO_RETUNE_DRAIN_MS=<ms>` — drain time before retune
- `DSD_NEO_RETUNE_MUTE_MS=<ms>` — input mute around RTL retunes (range 10–1000). By default the pre-retune mute is
  120ms and the post-retune settle mute is 25ms on local USB tuners (120ms on rtl_tcp/SoapySDR, which buffer stale
  samples); setting this applies the same value to both windows

RTL‑TCP networking

- `DSD_NEO_TCP_PREBUF_MS=<ms>` — prebuffer duration (default 1000, range 5–1000)
- `DSD_NEO_TCP_RCVBUF=<bytes>` — OS socket receive buffer (default ~4 MiB)
- `DSD_NEO_TCP_BUFSZ=<bytes>` — user‑space read size (default ~16 KiB)
- `DSD_NEO_TCP_RCVTIMEO=<ms>` — socket receive timeout in milliseconds (default 2000)
- `DSD_NEO_TCP_WAITALL=0/1` — require full reads (default off)
- `DSD_NEO_TCP_STATS=1` — print throughput/queue stats
- `DSD_NEO_TCP_AUTOTUNE=1` — enable adaptive buffering/recv size for TCP links
- `DSD_NEO_TCP_MAX_TIMEOUTS=<n>` — consecutive receive timeouts (1–100, default 3) after which the connection counts as
  lost and is reconnected

RTL‑SDR driver options

- `DSD_NEO_RTL_DIRECT=0|1|2|I|Q` — direct sampling (0 off, 1 I‑ADC, 2 Q‑ADC)
- `DSD_NEO_RTL_OFFSET_TUNING=0|1` — offset tuning (default: try enable)
- `DSD_NEO_RTL_XTAL_HZ=<Hz>`, `DSD_NEO_TUNER_XTAL_HZ=<Hz>` — crystal overrides
- `DSD_NEO_RTL_IF_GAINS="stage:gain[,...]"` — IF stage gains (dB or 0.1 dB)
- `DSD_NEO_RTL_TESTMODE=0|1` — test mode (ramp source)
- `DSD_NEO_RTL_AGC=0|1` — RTL2832U AGC enable/disable (default on)
- `DSD_NEO_RTL_VERIFY=0|1`, `DSD_NEO_RTL_VERIFY_ATTEMPTS=1..10` — local USB apply verification/retry controls
- `DSD_NEO_TUNER_BW_HZ=<Hz|auto>` — override tuner bandwidth (`auto` or `0` = driver automatic). While hardware offset
  tuning is on the bandwidth is at least twice the LO offset (1.7 × the sample rate); a smaller value is raised.

Tuner autogain (experimental)

- `DSD_NEO_TUNER_AUTOGAIN=1` — enable automatic tuner gain adjustment. It is the default each stream opens with
  (before its workers run, also under a manual gain, where the supervisor walks that gain); a Tuner autogain toggle
  made since replaces it for later stream restarts until the default itself changes. An I/Q replay or an Airspy opens
  with it off.
- `DSD_NEO_TUNER_AUTOGAIN_PROBE_MS=<ms>` — probe interval
- `DSD_NEO_TUNER_AUTOGAIN_SEED_DB=<dB>` — initial gain seed
- `DSD_NEO_TUNER_AUTOGAIN_SPEC_SNR_DB=<dB>` — spectrum SNR threshold
- `DSD_NEO_TUNER_AUTOGAIN_INBAND_RATIO=<ratio>` — in‑band power ratio
- `DSD_NEO_TUNER_AUTOGAIN_UP_STEP_DB=<dB>` — gain up step size
- `DSD_NEO_TUNER_AUTOGAIN_UP_PERSIST=<n>` — persistence before increasing gain

Audio/DSP helpers

`DSD_NEO_DEEMPH` and `DSD_NEO_AUDIO_LPF` are monitor/non-symbol audio helpers for the RTL path and non-RTL sample-window
paths; they are not part of RTL-family digital FSK symbol decode.

- `DSD_NEO_DEEMPH=off|50|75|nfm` — deemphasis curve
- `DSD_NEO_AUDIO_LPF=<Hz>|off` — audio low‑pass filter cutoff (or disable)
- `DSD_NEO_COSTAS_BW=<float>`, `DSD_NEO_COSTAS_DAMPING=<float>` — Costas loop tuning
- `DSD_NEO_CHANNEL_LPF=0|1` — channel LPF enable/disable (auto-enabled at RTL DSP rates >=20 kHz; mode passbands protect nominal channel edges). An explicit analog width (`--nfm-bandwidth-hz`, `--am-bandwidth-hz`) and AM (`-fM`, its default width included) always run the filter, and `0` with either is an error
- `DSD_NEO_WINDOW_FREEZE=1` — freeze symbol‑center window timing for debugging
- `DSD_NEO_CQPSK=1` — enable CQPSK demodulation (`0` forces the FSK discriminator) for digital modes; the analog monitor always demodulates FM (`-fA`) or AM (`-fM`). A `--trunk-scan` target that names its own `modulation` (`auto`, `c4fm`, `cqpsk`) keeps it, and a DMR or NXDN target keeps the FSK discriminator, whatever this says, after an analog target as after a digital one
- `DSD_NEO_CQPSK_SYNC_INV=1`, `DSD_NEO_CQPSK_SYNC_NEG=1` — CQPSK sync polarity tweaks

Misc

- `DSD_NEO_PDU_JSON=1` — emit P25 PDU JSON to stderr
- `DSD_NEO_RT_SCHED=1` — enable real‑time thread scheduling (requires privileges)
- `DSD_NEO_RT_PRIO_USB|DSD_NEO_RT_PRIO_DONGLE|DSD_NEO_RT_PRIO_DEMOD=<1..99>` — per-thread RT priority (only used when `DSD_NEO_RT_SCHED=1`)
- `DSD_NEO_CPU_USB|DSD_NEO_CPU_DONGLE|DSD_NEO_CPU_DEMOD=<cpu>` — per-thread CPU affinity (only used when `DSD_NEO_RT_SCHED=1`)
- `DSD_NEO_FTZ_DAZ=1|0` — SSE flush‑to‑zero / denormals‑are‑zero for every decoding thread. Read once at start and
  fixed for the run: `1` turns both on, `0` turns both off (even when the build or the host started them on), and
  unset leaves the threads as they started. x86 and x86‑64 builds only (Windows included); elsewhere it is ignored.
  Advanced > `SSE FTZ/DAZ` in the terminal menu shows the mode in force
- `DSD_NEO_MT` — no longer used: the worker pool it enabled ran no work and was removed; the variable is ignored
- `DSD_NEO_NO_SIGNAL_HANDLERS=1` — do not install the `SIGINT`/`SIGTERM`/`SIGHUP` handlers; for hosts that embed
  the decoder in their own process and drive shutdown themselves (a host can also set it in-process with
  `dsd_env_set(DSD_ENV_LAYER_HOST, ...)` from `<dsd-neo/platform/env.h>`, as the Android app does). Without it, a
  hangup (`SIGHUP`, the terminal closed or an SSH session dropped) shuts down as `SIGINT` does, finalizing
  recordings and the call in progress, unless the session was started ignoring it (`nohup`)
- `DSD_NEO_LOG_SINK=stderr|platform` — destination for runtime log messages (default `stderr`). `platform`
  selects the operating system's native logging facility where one exists — on Android that is logcat under
  the `dsd-neo` tag, with severities mapped from the log level; elsewhere it behaves like `stderr`. Read once
  at first use; embedders can override it at any time with `dsd_neo_log_set_sink()`
- `DSD_NEO_INPUT_VOLUME=<1..16>` — scale non‑RTL input samples (env alternative to `--input-volume`)
- `DSD_NEO_INPUT_WARN_DB=<dB>` — low input-level advisory threshold in dBFS (default −40); overrides the
  `[input] input_warn_db` user-config key when set
- `DSD_NEO_RIGCTL_RCVTIMEO=<ms>` — rigctl socket receive timeout
- `DSD_NEO_TCPIN_BACKOFF_MS=<ms>` — TCP input read backoff

P25 trunking timing

- `DSD_NEO_P25_HANGTIME=<seconds>` — post‑voice hangtime before returning to CC
- `DSD_NEO_P25_GRANT_TIMEOUT=<seconds>` — max seconds waiting for voice after grant
- `DSD_NEO_P25_VC_GRACE=<seconds>` — grace after VC tune before eligible to return (also via `--p25-vc-grace`)
- `DSD_NEO_P25_MIN_FOLLOW_DWELL=<seconds>` — minimum follow dwell after first voice
- `DSD_NEO_P25_GRANT_VOICE_TO=<seconds>` — grant‑to‑voice timeout
- `DSD_NEO_P25_MAC_HOLD=<seconds>` — keep MAC activity eligible for audio (also via `--p25-mac-hold`)
- `DSD_NEO_P25_VOICE_HOLD=<seconds>` — voice activity hold window
- `DSD_NEO_P25_CC_GRACE=<seconds>` — CC hunt grace window (also via `--p25-cc-grace`)
- `DSD_NEO_P25_FORCE_RELEASE_EXTRA=<seconds>` — safety‑net extra beyond hangtime
- `DSD_NEO_P25_FORCE_RELEASE_MARGIN=<seconds>` — safety‑net hard margin
- `DSD_NEO_P25_WD_MS=<ms>` — P25 state machine watchdog interval (20–2000)
- `DSD_NEO_P25P1_ERR_HOLD_PCT=<percent>` — extend hangtime when P25p1 IMBE error % exceeds threshold (default 0 = off)
- `DSD_NEO_P25P1_ERR_HOLD_S=<seconds>` — additional hold seconds when threshold exceeded (default 0 = off)
- `DSD_NEO_CC_CACHE=0|1` — enable/disable loading historical control-channel cache files
- `DSD_NEO_CACHE_DIR=<path>` — locate historical control-channel cache files written by older desktop releases
  (the Android app sets none: no Android build wrote them)

DMR Tier III (env helpers for `--calc-lcn`)

- `DSD_NEO_DMR_T3_CALC_CSV=<file>` — CSV file of frequencies
- `DSD_NEO_DMR_T3_STEP_HZ=<Hz>` — channel step (e.g., 12500)
- `DSD_NEO_DMR_T3_CC_FREQ=<Hz>` — control channel anchor frequency
- `DSD_NEO_DMR_T3_CC_LCN=<n>` — control channel anchor LCN
- `DSD_NEO_DMR_T3_START_LCN=<n>` — start LCN when no anchor
- `DSD_NEO_DMR_T3_HEUR=1` — enable heuristic LCN fill

Debug (verbose/developer)

- `DSD_NEO_DEBUG_SYNC=1` — verbose sync detection output
- `DSD_NEO_DEBUG_CQPSK=1` — verbose CQPSK Gardner/Costas/FLL-band-edge state output
- `DSD_NEO_DEBUG_SYMBOL_TIMING=1|2` — symbol-timing diagnostics on the decoder's symbol grid
- `DSD_NEO_SYNC_WARMSTART=0` — disable sync warm-start calibration

### Symbol timing (`DSD_NEO_DEBUG_SYMBOL_TIMING`)

The decoder's sampling phase is fixed when a sync is acquired and held for the rest of the call, so a call decoded
at a poor phase stays at that phase. The matched filter switching on at that accept, or off at the end of the
call, no longer moves the phase with it (#444), so `off` describes the grid rather than the switch. Level `1`
prints one line per accepted frame sync:

```text
SYMTIMING: sync=29 win=13113313 sps=20 jitter=-1 off=0 accum=0
```

- `sync` — accepted sync type id; `win` — the eight decided dibits the measurement correlated over
- `sps` — `samplesPerSymbol` the grid is running; `jitter` — the latched zero-crossing index, `-1` when none
- `off` — sub-symbol offset, in samples, between the grid's symbol boundary and the one the signal supports
  (`0` is aligned, and `-` means the trace could not support a measurement — right after a retune, for instance)
- `accum` — the RTL FSK fractional-sps accumulator

Collect the `off` distribution over a call to see the phase the grid settled on, and compare runs to see whether it
is stable. Level `2` additionally enables the per-sample `+ - O X` trace and the per-symbol jitter line; that is tens
of thousands of characters per second, so prefer level `1` unless you need the within-symbol detail.

## Handy Examples

- UDP in → Pulse out with UI: `dsd-neo -i udp -o pulse --frontend terminal`
- RTL‑TCP in with terminal UI: `dsd-neo -i rtltcp:127.0.0.1:1234 --frontend terminal`
- SoapySDR in with explicit driver args: `dsd-neo -i soapy:driver=sdrplay --frontend terminal`
- SoapySDR with SDRplay settings in config: `soapy_settings = "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4"`
- SoapySDR args + RTL-style tuning in one input spec: `dsd-neo -i soapy:driver=sdrplay:851.375M:22:-2:24:0:2 --frontend terminal`
- Save per‑call WAVs to a folder: `dsd-neo -7 ./calls -P --frontend terminal`
- Strictly P25 Phase 1 from TCP audio: `dsd-neo -f1 -i tcp --frontend terminal`
- Capture I/Q while decoding RTL input: `dsd-neo -i rtl:0:851.375M:22:0:48:0:2 --iq-capture p25-control.iq --frontend terminal`
- Print capture metadata and replayability summary: `dsd-neo --iq-info p25-control.iq.json`
- Replay capture in realtime loop mode: `dsd-neo --iq-replay p25-control.iq.json --iq-replay-rate realtime --iq-loop --frontend terminal`
- Replay another SDR program's WAV I/Q recording for TETRA: `dsd-neo -fT --iq-replay SDRSharp_20261015_123456Z_426700000Hz_IQ.wav`

## Manual Validation Checklist

- [ ] `rtl` decode still works.
- [ ] `rtltcp` decode still works.
- [ ] `soapy` decode works with at least one SDRPlay path and one Airspy path (if hardware is available).

Tip: Many options can be mixed; start simple, add only what you need.

Native Airspy R2 and Mini input uses `-i airspy[:serial=<hex>][:frequency[:bw[:sql[:vol]]]]`. See
[the native Airspy guide](airspy.md) for USB access (udev rules, WinUSB on Windows), the DSP bandwidths and rates,
receiver controls, and CF32 capture.
