# DSD-neo CLI Guide

Friendly, practical overview of the `dsd-neo` command line. This covers what you’ll use day-to-day, grouped by task. For a quick reference, run `dsd-neo -h`.

## Cheatsheet

- Help: `dsd-neo -h` | UI/logs: `--frontend terminal` (`-N` alias), `-Z` | List devices: `-O`
- Inputs: `-i pulse | file.wav | rtl[:...] | rtltcp[:...] | soapy[:args[:freq[:gain[:ppm[:bw[:sql[:vol]]]]]]] | tcp[:host[:port]] | udp[:bind_addr[:port]] | m17udp[:bind_addr[:port]] | -`
- Outputs: `-o pulse | null | udp[:host[:port]] | m17udp[:host[:port]] | -`
- Record/Logs/Debug: `-6 file.wav`, `-w file.wav`, `-P`, `-7 ./calls`, `-d ./mbe`, `-J events.log`, `--frame-log frames.log`, `--p25-sm-log p25-sm.log`, `-L lrrp.log`, `-Q dsp.bin`, `-c symbols.bin`, `-r *.mbe`, `--dmr-debug-burst`
- IQ capture/replay: `--iq-capture <path>`, `--iq-capture-format cu8|cf32`, `--iq-capture-max-mb <n>`, `--iq-replay <path>`, `--iq-replay-rate fast|realtime`, `--iq-loop`, `--iq-info <path>`
- Levels/Audio: `-g 0|1..50`, `-n 0..100`, `-nm`, `-8`, `-V 0|1|2|3`, `-z 0|1|2`, `-y`, `-v 0xF`
- Modes: `-fa | -fs | -fr | -f1 | -f2 | -fd | -fx | -fy | -fz | -fU | -fi | -fn | -fp | -fh | -fH | -fe | -fE | -fm`
- Inversions/filtering: `-xx`, `-xr`, `-xd`, `-xz`, `-l`, `-q`
- Trunking/scan: `-T`, `-Y`, `--trunk-scan targets.csv`, `-C chan.csv`, `-G group.csv`, `-W`, `-E`, `-p`, `-e`, `-I 1234`, `-U 4532`, `-B 12000`, `-t 1`, `--enc-lockout|--enc-follow`
- RTL‑SDR strings: `-i rtl:dev:freq:gain:ppm:bw:sql:vol[:bias=on|off]` or `-i rtltcp:host:port:freq:gain:ppm:bw:sql:vol[:bias=on|off]`
- Soapy selection: `-i soapy`, `-i soapy:driver=airspy[,serial=...]`, or `-i soapy[:args]:freq[:gain[:ppm[:bw[:sql[:vol]]]]]` (discover args with `SoapySDRUtil --find`)
- RTL retune control: `--rtl-udp-control <port>` binds to loopback by default; use
  `--rtl-udp-control-bind <ipv4>` for explicit remote exposure (see `docs/udp-control.md`)
- M17 encode: `-fZ -M M17:CAN:SRC:DST[:RATE[:VOX]]`, `-fP`, `-fB`
- Keys: `-b`, `-H '<hex...>'`, `-R`, `-1`, `-2`, `-! '<hex...>'`, `-@ '<hex...>'`, `-5 '<hex...>'`, `-9`, `-A`, `-S bits:hex[:offset[:step]]`, `-k keys.csv`, `-K keys_hex.csv`, `--dmr-baofeng-pc5 <hex>`, `--dmr-csi-ee72 <hex>`, `--dmr-vertex-ks-csv <file>`, `--dmr-force-algid <hex>`, `--show-keys`, `-4`, `-0`, `-3`
- Tools: `--calc-lcn file`, `--calc-cc-freq 451.2375`, `--calc-cc-lcn 50`, `--calc-step 12500`, `--calc-start-lcn 1`, `--auto-ppm`, `--auto-ppm-snr 6`, `--rtltcp-autotune`, `--rdio-mode off|dirwatch|api|both`

## Quick Start

- Show help: `dsd-neo -h`
- PulseAudio in, play out, UI on: `dsd-neo -i pulse -o pulse --frontend terminal`
- UDP audio in to PulseAudio out: `dsd-neo -i udp:0.0.0.0:7355 -o pulse --frontend terminal`
- Follow DMR trunking (TCP PCM input + rigctl): `dsd-neo -fs -i tcp -U 4532 -T -C dmr_t3_chan.csv -G group.csv --frontend terminal`
- Follow DMR trunking (RTL‑SDR): `dsd-neo -fs -i rtl:0:450M:26:-2:48:0:2 -T -C connect_plus_chan.csv -G group.csv --frontend terminal`
- Follow DMR trunking (SoapySDR): `dsd-neo -fs -i soapy:driver=airspy -T -C connect_plus_chan.csv -G group.csv --frontend terminal`
- Scan several P25/DMR targets with one tuner: `dsd-neo -ft -i rtl:0:851.0125M:22:0:48:0:2 --trunk-scan examples/trunk_scan_targets.csv -G examples/group.csv --frontend terminal`
- Capture RTL I/Q + metadata: `dsd-neo -i rtl:0:851.375M:22:0:48:0:2 --iq-capture p25-control.iq --frontend terminal`
- Inspect a capture: `dsd-neo --iq-info p25-control.iq.json`
- Replay a capture through demod: `dsd-neo --iq-replay p25-control.iq.json -f1 --frontend terminal`
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
- `--print-config` prints the effective config as INI after all env/CLI overrides.
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
- RTL‑TCP: `-i rtltcp[:host:port[:freq:gain:ppm:bw:sql:vol[:bias[=on|off]]]]`
- SoapySDR: `-i soapy[:args[:freq[:gain[:ppm[:bw[:sql[:vol]]]]]]]`
- TCP raw PCM16LE input (mono): `-i tcp[:host:port]` (bare `tcp` connects to `localhost:7355`; sample rate uses `-s`, default 48000)
  For a LAN host, use the IPv4 address of the computer running the TCP audio producer, and make sure that producer is
  listening on that LAN interface, not just on `localhost`.
- UDP PCM16 input: `-i udp[:bind_addr:port]` (defaults 127.0.0.1:7355)
- M17 UDP/IP frame input: `-i m17udp[:bind_addr:port]` (defaults `127.0.0.1:17000`; use `0.0.0.0` only when LAN access is intended; use with `-fU`)
- stdin (raw PCM16LE mono): `-i -` (sample rate uses `-s`)

- Set sample rate: `-s <rate>` (WAV/TCP/UDP; 48k or 96k typical)

TCP/UDP PCM input format notes

- Sample format is signed PCM16LE (little-endian), mono, headerless stream/datagrams.
- With `-i tcp:<host>:<port> -U <rigctl_port>`, rigctl connects to the same `<host>` as the TCP audio input. For SDR++
  on another PC, allow inbound TCP for both the audio port, commonly `7355`, and rigctl, commonly `4532`.
- See `docs/network-audio.md` for practical send/receive examples and UDP output details.

Other input options

- `--input-volume <1..16>` scale non‑RTL input samples (file/UDP/TCP) by an integer factor.
- `--input-level-warn-db <dB>` low input-level advisory threshold in dBFS (default −40). This only affects LOW
  advisories; DSD-neo never changes gain automatically. TCP PCM input keeps LOW/HOT/CLIP visible in the persistent
  input status, but suppresses repeated console level warnings while the decoder is idle/searching.

Tip: If paths or names contain spaces, wrap them in single quotes.

## Outputs (`-o`)

- PulseAudio: `-o pulse` or a specific sink like `-o pulse:alsa_output.pci-0000_0d_00.3.analog-stereo`
- Null (no audio): `-o null`
- UDP audio out (raw PCM): `-o udp[:host:port]` (default 127.0.0.1:23456). See `docs/network-audio.md`.
- M17 UDP/IP frame out: `-o m17udp[:host:port]` (default 127.0.0.1:17000)
- stdout (raw decoded audio): `-o -` (see `docs/network-audio.md`)

## Display & UI

- `--frontend terminal` Use the terminal UI (`-N` is the supported short alias)
- `--frontend native` remains accepted and maps to headless mode. The retired native provider was a non-rendering
  scaffold; this keeps existing invocations working without restoring its provider/threading layer.
- `-Z` Log MBE/PDU payloads to the console (verbose), including encrypted P25 voice frames when media is gated
- `--frame-log <file>` Append one-line timestamped frame traces (separate from event log)
- `--p25-sm-log <file>` Append one-line P25 state-machine decision diagnostics (separate from stdout/stderr, event log, and frame log)
- `-O` List PulseAudio input sources and output sinks
- The terminal input section shows advisory `Input Level`/`RF Level` health when metrics are available. `LOW` uses
  `--input-level-warn-db`; `HOT` means peak at or above `-1.0 dBFS`; `CLIP` means at least `0.1%` clipped or near-rail
  samples. TCP PCM idle/search level advisories are status-only to avoid console spam. These advisories never adjust
  gain automatically.
- For RTL-family inputs, the optional DSP panel shows post-channel-filter `Squelch` power against the SQL threshold.
  This is separate from the raw receiver `RF Level` health line.
- UI hotkeys and menu navigation: `docs/ui-terminal.md`
- `-j` P25: force-enable LCW explicit retune (format `0x44`; enabled by default)
- `-^` P25: prefer CC candidates during control channel hunt

### P25 Follower (Advanced)

- `--p25-vc-grace <s>` seconds after VC tune before eligible to return to CC
- `--p25-min-follow-dwell <s>` minimum follow dwell after first voice
- `--p25-grant-voice-timeout <s>` max seconds from grant to voice before returning
- `--p25-mac-hold <s>` keep MAC activity eligible for audio for this many seconds after the last MAC
- `--p25-ring-hold <s>` ring gate window (seconds) used when deciding whether a slot still has recent audio activity
- `--p25-cc-grace <s>` CC hunt grace window; delay hunting for a new control channel by this many seconds after loss
- `--p25-force-release-extra <s>` safety‑net extra seconds beyond hangtime
- `--p25-force-release-margin <s>` safety‑net hard margin seconds beyond extra
- `--p25-p1-err-hold-pct <pct>` P25p1 IMBE error percentage threshold to extend hang
- `--p25-p1-err-hold-sec <s>` additional seconds to hold when threshold exceeded

## DMR Burst Debugging

`--dmr-debug-burst` emits one console line to stderr for each synced, fully assembled DMR burst. Use it when
troubleshooting live DMR decode and you need bracketed post-demod no-CACH payload bytes without enabling verbose `-Z`
payload logging or changing `-Q` OK-DMRlib structured output.

Example:

```sh
dsd-neo -fs -i rtl:0:450M:26:-2:48:0:2 --dmr-debug-burst
```

Output format:

```text
Debug Demod +Sync slot=<1|2> type=0xNN: [AA][BB]...[CC]
```

Notes:

- The dump is DMR-only and only runs for synced 144-dibit bursts.
- The byte stream is the 33-byte no-CACH payload range also used by `-Q` DMR output.
- Voice bursts report `type=0x10`; data bursts use the decoded DMR data burst type.
- The option writes to stderr only. It does not imply `-Q`, `-Z`, symbol capture, or payload file logging.

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

- `-6 <file>` Save raw audio WAV (48k/mono). Large files (≈360 MB/hour)
- `-w <file>` Save decoded audio to a single WAV (mutually exclusive with `-P`)
- `-P` Per‑call WAV saving (auto‑named files in a folder; mutually exclusive with `-w`)
- `-7 <dir>` Set folder for per‑call WAVs (use before `-P`)
- `--rdio-mode <off|dirwatch|api|both>` Enable rdio-scanner export from finalized per-call WAV calls
- `--rdio-system-id <N>` Set rdio-scanner system ID (required for API upload mode)
- `--rdio-api-url <url>` Set rdio-scanner API base URL (default `http://127.0.0.1:3000`)
- `--rdio-api-key <key>` Set API key for `/api/trunk-recorder-call-upload`
- `--rdio-upload-timeout-ms <ms>` API timeout per call (default 5000 ms)
- `--rdio-upload-retries <n>` API upload attempts per call (default 1)
- `--rdio-api-delete-after-upload` Delete the per-call WAV after a successful API-only upload
- `-r <files>` Play saved MBE files
- `-c <file>` Save symbol captures to a .bin file
- `--symbol-capture-format <soft|legacy>` Select the current soft/v2 writer. `legacy` remains accepted as an alias; it
  does not reactivate the removed one-byte writer.
  The current writer emits `DSDNSYM2` version 2 records with soft-decision metrics. The reader also accepts historical
  headerless dibit captures so persisted files remain replayable; a file carrying `DSDNSYM2` magic with an unsupported
  version or record size is rejected rather than reinterpreted as headerless data. Remove the headerless reader after
  those captures are migrated or their support window ends. Neither stored format carries the NXDN symbol rate, so NXDN
  capture replay requires `-fi` or `-fn` instead of `-fa`.
- `-d <dir>` Save raw MBE vocoder frames in this folder
- `-J <file>` Append event log output
- `--frame-log <file>` Append frame-level one-line timestamped traces
- `--p25-sm-log <file>` Append P25 state-machine health and frequency-decision traces. Grant traces identify initial
  assignments versus updates; post-call stale-update handling reports guard, validation-probe, and activity outcomes.

For rdio-scanner API uploads that should not persist on disk, use API-only mode with a RAM-backed per-call WAV directory
and post-upload deletion, for example `-7 /dev/shm/dsd-neo-rdio -P --rdio-mode api --rdio-api-delete-after-upload`.
Rdio API uploads do not follow HTTP redirects; use the final trusted HTTP/HTTPS endpoint directly.
DirWatch modes keep the WAV and JSON files because the watcher needs stable files to ingest.
- `-L <file>` Append LRRP (location) data
- `-Q <file>` Write structured DSP or M17 stream data to `./DSP/<file>`
- `-q` Reverse mute: mute clear audio, unmute encrypted audio

## IQ Capture And Replay

- `--iq-capture <path>` Capture raw I/Q plus metadata sidecar.
- `--iq-capture-format <cu8|cf32>` Capture format request (`cu8` default).
- `--iq-capture-max-mb <n>` Capture byte cap in MiB (`0` unlimited).
- `--iq-replay <path>` Replay capture metadata/data through the RTL pipeline.
- `--iq-replay-rate <fast|realtime>` Replay pacing mode (`fast` default).
- `--iq-loop` Loop replay when EOF is reached.
- `--iq-info <path>` Print capture metadata summary and exit.

Notes

- Replay and capture are mutually exclusive in one invocation.
- `--iq-replay` and `--iq-info` accept either the data file or the `.json` metadata path.
- Retuned captures with v2 replay event timelines can be replayed. Older retuned captures without an event timeline are
  reported by `--iq-info` and rejected by `--iq-replay`.
- `-i iqreplay:...` is intentionally not a supported public input form; use `--iq-replay`.
- More details and format notes: `docs/iq-capture-replay.md`.

## Levels & Audio

- `-g <num>` Digital output gain. `0` = auto; `1` ≈ 2%; `50` = 100%
- `-n <num>` Analog output gain (0–100%)
- `-nm` Compatibility spelling for the retired DMR mono override. It is accepted without changing the active preset;
  DMR uses the current mixer, and the removed mono decoder is not reactivated.
- `-z <0|1|2>` TDMA slot preference (0 = slot 1, 1 = slot 2, 2 = auto)
- `-8` Monitor the source audio (helpful when mixing analog/digital)
- `-V <0|1|2|3>` TDMA voice synthesis (0 = off; 1 = slot 1; 2 = slot 2; 3 = both; default 3)
- `-y` Use experimental float audio output
- `-a` Enable call alert beep (UI)

## Modes & Decoders (`-f`)

- Auto: `-fa`
- Passive analog monitor: `-fA`
- Trunking helper: `-ft` (P25p1 CC + P25p1/p2/DMR voice)
- DMR simplex (BS/MS): `-fs`; `-fr` remains accepted as an alias for the same current DMR preset/mixer
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

- `-fa` enables the complete digital candidate set and hunts these profiles in order, skipping any profile that has no
  enabled candidate:

  | Hunt profile | Symbol rate | Levels | Enabled candidates |
  | --- | ---: | ---: | --- |
  | 0 | 4800 | 4 | P25 Phase 1 (C4FM/CQPSK), DMR, NXDN96, YSF, M17 |
  | 1 | 2400 | 4 | NXDN48, dPMR |
  | 2 | 9600 | 2 | ProVoice, EDACS |
  | 3 | 6000 | 4 | P25 Phase 2 (CQPSK), X2-TDMA |
  | 4 | 4800 | 2 | D-STAR |

  A detected sync locks the active rate, level count, timing, and RTL-family channel profile. Passive analog monitoring
  (`-fA`) and already-framed M17 UDP input (`-fU`) are not frame-sync hunt candidates.
- The three Auto entry points intentionally differ: CLI `-fa` installs the complete matrix above; config
  `decode = "auto"` preserves the protocol flags established during initialization or by the current overlay; and the
  interactive Auto choice preserves the current candidate set. This lets configs and interactive setup retain a
  deliberately narrowed scanner while `-fa` remains the explicit full-search preset.
- In TCP PCM mode, SPS hunting still runs when no signal is present, but repeated idle `Sync: no sync` and `SPS hunt`
  console diagnostics are suppressed.
- P25p2 on a single frequency may require `-X` (below) if MAC_SIGNAL is missing.

## Mode Tweaks & Advanced

- Inversions: `-xx` X2 non‑inverted, `-xr` DMR inverted, `-xd` dPMR inverted, `-xz` M17 inverted
- Disable DMR/dPMR/NXDN/M17 input filtering: `-l`
- Analog filter bitmap (advanced): `-v <hex>` (bitmask for HPF/LPF/PBF)
- Modulation optimizations: `-ma` (auto), `-mc` (C4FM), `-mg` (GFSK), `-mq` (QPSK), `-m2` (P25p2 QPSK 6000 sps)
- Relax CRC checks: `-F` (P25p2 MAC_SIGNAL, DMR RAS/CRC, NXDN SACCH/FACCH/CAC/F2U, M17 LSF/PKT)
- M17 signed voice-stream verification: `--m17-signature-public-key <hex>` accepts a 64-byte secp256r1 public key as
  raw `X||Y` hex.
- P25p2 manual WACN/SYSID/CC: `-X <hex>` (e.g., `-X BEE00ABC123`)
- DMR Tier III Location Area n‑bits: `-D <0–10>`
- Env (CQPSK timing):
  - `DSD_NEO_TED_GAIN=<float>` overrides the CQPSK/OP25 Gardner timing loop gain.
  - When using RTL CQPSK input, the symbol sampler may gently nudge `symbolCenter` based on the smoothed CQPSK Gardner residual.
  - If you are debugging symbol-center drift, consider freezing windows (`DSD_NEO_WINDOW_FREEZE=1`) while testing.

## Trunking & Scanning

- Enable trunking (NXDN/P25/EDACS/DMR): `-T`
- Conventional scan mode: `-Y` (not trunking; scans for sync on enabled decoders)
- Single-tuner trunk scan mode: `--trunk-scan <targets.csv>`
  - Rotates one tuner across CSV-defined P25 trunk, DMR trunk, and one-frequency DMR targets. Full guide:
    `docs/trunk-scan.md`.
  - Requires a live retuning path: RTL-family input opened by DSD-neo, or rigctl control such as `-U 4532`.
  - Use per-target `chan_csv` entries in the target CSV; global `-C` is rejected in this mode.
  - Optional per-target `modulation` and `rtl_gain` columns can override demod hints and RTL-family tuner gain for the
    active target.
  - Cannot be combined with conventional `-Y` scan mode or IQ replay.
  - Idle dwell: `--trunk-scan-dwell-ms <250..600000>` (default `3000`).
  - Conventional DMR activity hold: `--trunk-scan-activity-hold-ms <250..600000>` (default `1200`).
  - Single-tuner limitation: systems not currently parked can be missed while another target is being monitored.
- Channel map CSV: `-C <file>` (e.g., `connect_plus_chan.csv`)
- Group list CSV (allow/block + labels, optional `priority/preempt/audio/record/stream` policy columns): `-G <file>`
- CSV formats and examples: `docs/csv-formats.md` and `examples/`
- Use group list as allow/whitelist: `-W`
- Tune controls: `-E` disable group calls, `-p` disable private calls, `-e` enable data calls, `--enc-lockout`
  enable key-aware P25 encryption lockout, `--enc-follow` follow encrypted grants without lockout (default)
  - With `--enc-lockout`, otherwise allowed P25 voice grants whose encryption is set or not yet known are tuned briefly
    as silent classification probes. Voice is not decoded, played, recorded, or streamed during classification.
  - HDU/LDU2 (Phase 1) or MAC_PTT/ESS (Phase 2) metadata confirms whether the call is clear or has a matching,
    complete key for a supported algorithm. Clear and decryptable calls continue; missing-key and unsupported calls
    are suppressed. On Phase 2, a clear companion slot remains active and on its original stereo side.
- Hold talkgroup: `-I <dec>`
- rigctl over TCP: `-U <port>` (SDR++ default 4532)
- Set rigctl bandwidth (Hz): `-B <hertz>` (e.g., 7000–48000 by mode)
- Hang time after voice/sync loss (seconds): `-t <secs>`
  - P25 Talk Complete, TDU, TDULC, MAC_END_PTT, MAC_IDLE, and MAC_HANGTIME mark a transmission boundary. They close
    that slot's media and start or refresh the traffic-carrier inactivity timer without returning to the control
    channel. A follow-up PTT/ACTIVE on the retained carrier opens a clean call epoch without retuning.
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
    - `DSD_NEO_TG_PREEMPT_MIN_DWELL_MS=<ms>` — minimum active call dwell before displacement (default `750`)
    - `DSD_NEO_TG_PREEMPT_COOLDOWN_MS=<ms>` — cooldown between displacement attempts (default `1000`)

## RTL‑SDR details (`-i rtl` / `-i rtltcp`)

- Fields: `dev` (device index), `freq` (Hz/MHz), `gain` (0–49), `ppm`, `bw` (kHz: 4, 6, 8, 12, 16, 24, 48), `sql` (dB or linear), `vol` (monitor gain, 0–3; typical 1–3), optional `bias[=on|off]`.
- For DMR data/LRRP on direct RTL input, use `bw=48` when possible, or at least `bw=24`; lower basebands may still decode voice but corrupt data PDUs.
- Note: For EDACS analog voice follow, `sql <= 0` now uses a bounded fallback watchdog to avoid indefinite VC hold when no release marker is detected.
- RTL USB, RTL-TCP, SoapySDR, and IQ replay digital decode run in the symbol domain. The digital decoder receives one
  normalized float per FSK or CQPSK symbol decision; discriminator audio is not used for digital decode.
- The trailing `vol` field and `rtl_volume` config key are monitor/non-symbol gain only. They do not scale RTL-family
  digital symbols. `-8` enables the separate source monitor tap.
- Examples:
  - `-i rtl:0:851.375M:22:-2:24:0:2`
  - `-i rtltcp:192.168.1.10:1234:851.375M:22:-2:24:0:2`
- External retune control (RTL/RTL‑TCP): `--rtl-udp-control <port>` listens on `127.0.0.1` by default. Use
  `--rtl-udp-control-bind <ipv4>` only when the unauthenticated listener must be reachable remotely.

Advanced (env)

- `DSD_NEO_RTL_DIRECT=0|1|2|I|Q` — Direct sampling selection (0 off; 1 I‑ADC; 2 Q‑ADC).
- `DSD_NEO_RTL_OFFSET_TUNING=0|1` — Disable/enable offset tuning (default is to try enabling).
- `DSD_NEO_RTL_XTAL_HZ` / `DSD_NEO_TUNER_XTAL_HZ` — Override crystal refs in Hz (optional).
- `DSD_NEO_RTL_IF_GAINS="stage:gain[,stage:gain]..."` — Set IF gain(s); gain in dB (e.g., `10`) or 0.1 dB (`125`).
- `DSD_NEO_RTL_TESTMODE=0|1` — Enable librtlsdr test mode (ramp) instead of I/Q (for diagnostics).
- `DSD_NEO_RTL_VERIFY=0|1` — Retry and verify local USB RTL-SDR control applies (default on).
- `DSD_NEO_RTL_VERIFY_ATTEMPTS=1..10` — Total local USB apply attempts when verification is enabled (default 10).
- On rtl_tcp reconnects, these settings are automatically reapplied.

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
- Those trailing fields map to existing shared controls and keys:
  `rtl_freq`, `rtl_gain`, `rtl_ppm`, `rtl_bw_khz`, `rtl_sql`, `rtl_volume`.
- Additional `[input]` keys expose Soapy-specific capabilities: `soapy_profile`, `soapy_stream_format`,
  `soapy_antenna`, `soapy_clock`, `soapy_settings`, `soapy_gains`, and `soapy_bandwidth_hz`.
- `soapy_settings = "key=value[,rx:key=value...]"` writes driver settings through Soapy. For example, SDRplay
  modules may expose `rfnotch_ctrl`, `dabnotch_ctrl`, `biasT_ctrl`, `agc_setpoint`, or `rfgain_sel`.
- `soapy_gains = "NAME:dB[,NAME:dB...]"` uses named gain stages and takes precedence over aggregate `rtl_gain`.
- `--print-config` reflects shorthand as normalized config fields (`soapy_args` + `rtl_*`) rather than the raw input
  string.
- If your Soapy args string itself contains `:`, prefer config keys (`soapy_args` + `rtl_*`) to avoid ambiguity.
- `rtl_device` index selection is for `rtl` input and is ignored in Soapy mode.
- Set an explicit `rtl_freq` for predictable startup frequency (otherwise defaults may not match your target system).
- Some shortcuts are RTL/RTL-TCP specific and not supported in the Soapy backend path (RTL bias-tee UI/CLI shortcut,
  direct sampling, offset tuning, xtal/IF-gain/testmode controls, RTL-TCP autotune). Use `soapy_settings` for
  driver-specific controls when the Soapy module exposes them, such as SDRplay `biasT_ctrl`.
- Native SDRplay/Airspy APIs are intentionally out of scope for now; non-RTL radios are controlled through SoapySDR.
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
- `INPUT_RATE` default 48000; use multiples of 8000 up to 48000
- `VOX` enable with `1` (default `0`)

Examples

- `dsd-neo -fZ -M M17:9:DSD-NEO:ARANCORMO -i pulse -6 m17signal.wav -8 --frontend terminal`
- `dsd-neo -fP -M M17:9:DSD-NEO:ARANCORMO -6 m17pkt.wav -8`

## Keys & Privacy (advanced)

By default, DSD-neo redacts radio keys and keystream material in logs and terminal status. Add `--show-keys` to reveal
those values for the current CLI run only.

- Basic Privacy key (decimal): `-b <dec>`
- Hytera 10/32/64‑char BP or AES‑128/256 key (hex, groups of 16): `-H '<hex…>'`
- dPMR/NXDN scrambler (decimal): `-R <dec>`
- RC4/DES key (hex): `-1 <hex>`
- TYT Basic Privacy (16‑bit, hex, enforced): `-2 <hex>`
- TYT Advanced Privacy PC4 (128/256-bit hex stream, groups of 16): `-! '<hex…>'`
- Retevis Advanced Privacy RC2 (128/256-bit hex stream, groups of 16): `-@ '<hex…>'`
- TYT Enhanced Privacy AES‑128 (hex stream): `-5 '<hex…>'`
- Baofeng AP PC5 key override (hex): `--dmr-baofeng-pc5 <hex>` (32 or 64 hex chars)
- Connect Systems EE72 key override (hex): `--dmr-csi-ee72 <hex>` (18 hex chars)
- Vertex ALG `0x07` key->keystream map CSV: `--dmr-vertex-ks-csv <file>` (`key_hex,bits:hex[:offset[:step]]`)
- Kenwood 15‑bit scrambler (decimal): `-9 <dec>`
- Anytone 16‑bit BP (hex): `-A <hex>`
- Generic keystream (length:hexbytes, optional frame align): `-S <bits:hex[:offset[:step]]>` (e.g., `-S 49:123456789ABC80`, `-S 168:<hex>:0:49`)
- For Vertex Std voice ALG `0x07`, prefer `--dmr-vertex-ks-csv` for repeatable key->keystream mapping. Use `-S` for
  one-off manual keystream experiments.
- Import keys CSV (decimal): `-k <file>`
- Import keys CSV (hex): `-K <file>`
- Force key over identifiers: `-4` (DMR BP/NXDN scrambler), `-0` (DMR RC4 when PI/LE missing),
  `--dmr-force-algid <hex>` (DMR ALGID when PI/LE missing; `-M` is reserved for M17 in DSD-neo)
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

- RTL‑TCP networking:
  - `--rtltcp-autotune` enable adaptive tuning of buffering/recv size for RTL‑TCP links.

## Environment Variables (Advanced Tuning)

These environment variables provide fine‑grained control for power users.

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

RTL-family FSK digital decode selects its own symbol timing and normalization internally. CQPSK uses the OP25-style
Gardner/Costas/FLL-band-edge symbol chain.

- `DSD_NEO_TED_GAIN=<float>` — CQPSK/OP25 Gardner timing loop gain override
- `DSD_NEO_IQ_DC_BLOCK=1` — enable DC blocker
- `DSD_NEO_IQ_DC_SHIFT=<k>` — DC shift coefficient

Digital SNR squelch

- `DSD_NEO_SNR_SQL_DB=<dB>` — skip sync when SNR below threshold

Capture/retune behavior

- `DSD_NEO_COMBINE_ROT=0|1` — select the two-pass or combined CU8 rotation transform (default 1)
- `DSD_NEO_DISABLE_FS4_SHIFT=1` — disable +fs/4 capture shift
- `DSD_NEO_OUTPUT_CLEAR_ON_RETUNE=1` — clear output on retune
- `DSD_NEO_RETUNE_DRAIN_MS=<ms>` — drain time before retune
- `DSD_NEO_RETUNE_MUTE_MS=<ms>` — input mute around RTL retunes, default 120ms

RTL‑TCP networking

- `DSD_NEO_TCP_PREBUF_MS=<ms>` — prebuffer duration (default 1000, range 5–1000)
- `DSD_NEO_TCP_RCVBUF=<bytes>` — OS socket receive buffer (default ~4 MiB)
- `DSD_NEO_TCP_BUFSZ=<bytes>` — user‑space read size (default ~16 KiB)
- `DSD_NEO_TCP_RCVTIMEO=<ms>` — socket receive timeout in milliseconds (default 2000)
- `DSD_NEO_TCP_WAITALL=0/1` — require full reads (default off)
- `DSD_NEO_TCP_STATS=1` — print throughput/queue stats
- `DSD_NEO_TCP_AUTOTUNE=1` — enable adaptive buffering/recv size for TCP links
- `DSD_NEO_TCP_MAX_TIMEOUTS=<n>` — max consecutive timeouts before giving up

RTL‑SDR driver options

- `DSD_NEO_RTL_DIRECT=0|1|2|I|Q` — direct sampling (0 off, 1 I‑ADC, 2 Q‑ADC)
- `DSD_NEO_RTL_OFFSET_TUNING=0|1` — offset tuning (default: try enable)
- `DSD_NEO_RTL_XTAL_HZ=<Hz>`, `DSD_NEO_TUNER_XTAL_HZ=<Hz>` — crystal overrides
- `DSD_NEO_RTL_IF_GAINS="stage:gain[,...]"` — IF stage gains (dB or 0.1 dB)
- `DSD_NEO_RTL_TESTMODE=0|1` — test mode (ramp source)
- `DSD_NEO_RTL_AGC=0|1` — RTL2832U AGC enable/disable (default on)
- `DSD_NEO_RTL_VERIFY=0|1`, `DSD_NEO_RTL_VERIFY_ATTEMPTS=1..10` — local USB apply verification/retry controls
- `DSD_NEO_TUNER_BW_HZ=<Hz|auto>` — override tuner bandwidth (`auto` or `0` = driver automatic)

Tuner autogain (experimental)

- `DSD_NEO_TUNER_AUTOGAIN=1` — enable automatic tuner gain adjustment
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
- `DSD_NEO_CHANNEL_LPF=0|1` — channel LPF enable/disable (auto-enabled at RTL DSP rates >=20 kHz; mode passbands protect nominal channel edges)
- `DSD_NEO_WINDOW_FREEZE=1` — freeze symbol‑center window timing for debugging
- `DSD_NEO_CQPSK=1` — enable CQPSK demodulation
- `DSD_NEO_CQPSK_SYNC_INV=1`, `DSD_NEO_CQPSK_SYNC_NEG=1` — CQPSK sync polarity tweaks

Misc

- `DSD_NEO_MT=1` — enable light worker pool (2 threads)
- `DSD_NEO_PDU_JSON=1` — emit P25 PDU JSON to stderr
- `DSD_NEO_RT_SCHED=1` — enable real‑time thread scheduling (requires privileges)
- `DSD_NEO_RT_PRIO_USB|DSD_NEO_RT_PRIO_DONGLE|DSD_NEO_RT_PRIO_DEMOD=<1..99>` — per-thread RT priority (only used when `DSD_NEO_RT_SCHED=1`)
- `DSD_NEO_CPU_USB|DSD_NEO_CPU_DONGLE|DSD_NEO_CPU_DEMOD=<cpu>` — per-thread CPU affinity (only used when `DSD_NEO_RT_SCHED=1`)
- `DSD_NEO_FTZ_DAZ=1` — enable SSE flush‑to‑zero / denormals‑are‑zero
- `DSD_NEO_INPUT_VOLUME=<1..16>` — scale non‑RTL input samples (env alternative to `--input-volume`)
- `DSD_NEO_INPUT_WARN_DB=<dB>` — low input-level advisory threshold in dBFS (default −40)
- `DSD_NEO_RIGCTL_RCVTIMEO=<ms>` — rigctl socket receive timeout
- `DSD_NEO_TCPIN_BACKOFF_MS=<ms>` — TCP input read backoff

P25 trunking timing

- `DSD_NEO_P25_HANGTIME=<seconds>` — post‑voice hangtime before returning to CC
- `DSD_NEO_P25_GRANT_TIMEOUT=<seconds>` — max seconds waiting for voice after grant
- `DSD_NEO_P25_VC_GRACE=<seconds>` — grace after VC tune before eligible to return (also via `--p25-vc-grace`)
- `DSD_NEO_P25_MIN_FOLLOW_DWELL=<seconds>` — minimum follow dwell after first voice
- `DSD_NEO_P25_GRANT_VOICE_TO=<seconds>` — grant‑to‑voice timeout
- `DSD_NEO_P25_MAC_HOLD=<seconds>` — keep MAC activity eligible for audio (also via `--p25-mac-hold`)
- `DSD_NEO_P25_RING_HOLD=<seconds>` — ring gate window for recent audio activity (also via `--p25-ring-hold`)
- `DSD_NEO_P25_VOICE_HOLD=<seconds>` — voice activity hold window
- `DSD_NEO_P25_CC_GRACE=<seconds>` — CC hunt grace window (also via `--p25-cc-grace`)
- `DSD_NEO_P25_FORCE_RELEASE_EXTRA=<seconds>` — safety‑net extra beyond hangtime
- `DSD_NEO_P25_FORCE_RELEASE_MARGIN=<seconds>` — safety‑net hard margin
- `DSD_NEO_P25_WD_MS=<ms>` — P25 state machine watchdog interval (20–2000)
- `DSD_NEO_P25P1_ERR_HOLD_PCT=<percent>` — extend hangtime when P25p1 IMBE error % exceeds threshold (default 0 = off)
- `DSD_NEO_P25P1_ERR_HOLD_S=<seconds>` — additional hold seconds when threshold exceeded (default 0 = off)
- `DSD_NEO_CC_CACHE=0|1` — enable/disable loading historical control-channel cache files
- `DSD_NEO_CACHE_DIR=<path>` — locate historical control-channel cache files written by older releases

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
- `DSD_NEO_SYNC_WARMSTART=0` — disable sync warm-start calibration

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

## Manual Validation Checklist

- [ ] `rtl` decode still works.
- [ ] `rtltcp` decode still works.
- [ ] `soapy` decode works with at least one SDRPlay path and one Airspy path (if hardware is available).

Tip: Many options can be mixed; start simple, add only what you need.
