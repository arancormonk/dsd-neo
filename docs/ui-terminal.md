# Terminal UI (ncurses) Guide

This guide covers the interactive terminal UI enabled with `--frontend terminal`: how to open the menu overlay and the most useful
hotkeys.

For CLI flags and inputs/outputs, see `docs/cli.md`.

## Start the UI

- Enable UI: `dsd-neo --frontend terminal ...`
- Quit: `q`
- Open the menu overlay: `Enter`

Tip: Many screens print a short hotkey hint line in the footer while you’re running.

## Menu Overlay

Press `Enter` to open the nonblocking menu overlay. While it is open, keypresses go to the menu (hotkeys are not
processed).

Controls:

- Move selection: `Up`/`Down`, `PageUp`/`PageDown`, `Home`/`End`
- Select / open submenu: `Enter`, `Right`
- Back / close: `Esc`, `Left`
- Help for the highlighted row: `h`

`q` does not back out of the menu, its pickers, the help overlay or the RadioReference panel. It quits the program, and only from the main screen — inside the menu it is
inert, so one keypress too many after the menu closes cannot exit the decoder.

For the same reason `End` stops on `Advanced` rather than on `Quit DSD-neo`: the jump keys never park the highlight
on the quit row, so `End` followed by `Enter` cannot end a running decode. Reaching it takes a deliberate `Down`,
`PageDown`, or the `q` hotkey.

Rows come in three kinds:

- **Action rows** are the selectable ones: they run something, open a prompt (a trailing `...`), or open a
  submenu (a trailing `>`, added by the menu itself).
- **Status rows** are dimmed, read-only readouts such as `Source: RTL-SDR dev 0` or `Output: Pulse [default]`. The
  highlight steps over them.
- **Separators** are horizontal rules that group a submenu; the highlight steps over them too.

How to read a row:

- A state shows in brackets: `Trunking [On]`, `Mute [Off]`. Selecting the row flips it.
- A trailing `...` means the row opens a prompt or picker; the current value sits in the brackets:
  `Hangtime... [1.0 s]`.
- A key on the right edge of the row is the main-screen hotkey for the same action: `Trunking [On]` carries `t`,
  `Digital gain... [auto]` carries `+ -`. Nearly every hotkey in the tables below appears on its menu row, so the
  menu is the place to learn them. The exceptions are the shifted aliases (`X`, `O`) and the four modifiers that
  adjust a visualizer rather than toggle it (`<` `>` for the constellation gate, `,` `.` for the FFT size); those
  are named in their row's help text instead.

The root is the receiver's signal chain — Input, Decoder, Trunking, Encryption, Audio, Recording & logs — then
Display, then housekeeping. Put another way: to find a setting, ask which signal it acts on.

### Menu map

Bracketed states and values are examples. `(status)` marks a read-only row; `─────` a separator. Rows marked
*RTL-SDR input only* appear while the RTL-SDR front end is the input; rows marked *RadioReference builds* need a build
with libcurl and expat.

```text
Main Menu
├── Input
│   ├── Source: <current input>                      (status)
│   ├── Switch source
│   │   ├── Pulse Audio (mic/line)
│   │   ├── Pulse input device...
│   │   ├── RTL-SDR                                  (RTL-SDR builds)
│   │   ├── TCP audio... [Off]                   8
│   │   ├── UDP audio...
│   │   ├── WAV / raw file...
│   │   └── Symbol file (.bin/.raw/.sym)...
│   ├── Input volume... [4X]                     v
│   ├── Low-input warning... [-40.0 dBFS]
│   ├── Invert signal [Off]                      i
│   └── RTL-SDR                                   (RTL-SDR input only)
│       ├── Frequency... [769.768750 MHz]
│       ├── Gain... [AGC]
│       ├── PPM correction... [0]                { }
│       ├── Bandwidth... [48 kHz]
│       ├── Squelch (dB)...
│       ├── Volume multiplier... [1]             v
│       ├── Auto-PPM [On]
│       ├── Tuner autogain [On]
│       ├── Bias tee [Off]
│       ├── rtl_tcp adaptive buffering [On]
│       ├── ─────
│       ├── Device index...
│       ├── Restart stream
│       ├── Auto-PPM & rtl_tcp
│       │   ├── Auto-PPM SNR threshold... [6.0 dB]
│       │   ├── Auto-PPM minimum power... [-80.0 dB]
│       │   ├── Auto-PPM zero-lock PPM... [0.60]
│       │   ├── Auto-PPM zero-lock Hz... [60]
│       │   ├── Auto-PPM freeze [Off]
│       │   ├── ─────
│       │   ├── rtl_tcp prebuffer... [30 ms]
│       │   ├── rtl_tcp SO_RCVBUF... [system default]
│       │   ├── rtl_tcp SO_RCVTIMEO... [Off]
│       │   └── rtl_tcp MSG_WAITALL [Off]
│       └── IQ & CQPSK timing
│           ├── IQ balance [Off]                     (not on the CQPSK path)
│           ├── IQ DC block [On]
│           ├── IQ DC shift k... [5]
│           ├── CQPSK timing gain... [75 x0.001]     (CQPSK path only)
│           └── CQPSK timing bias (EMA) <n>          (status, CQPSK path only)
├── Decoder
│   ├── Mode... [Auto]
│   ├── Modulation [C4FM]                        m
│   ├── P25 Phase 2 modulation lock [Off]        M
│   ├── CQPSK path [On]                              (RTL-SDR input only)
│   ├── Audio filters
│   │   ├── Low-pass filter [Off]                V
│   │   ├── High-pass filter [Off]               B
│   │   ├── Pulse-shaping band-pass [Off]        N
│   │   ├── Digital high-pass filter [Off]       H
│   │   └── Cosine filter [On]
│   ├── Per-protocol inversion
│   │   ├── Invert X2-TDMA [Off]
│   │   ├── Invert DMR [Off]
│   │   ├── Invert dPMR [Off]
│   │   └── Invert M17 [Off]
│   ├── Relaxed CRC checks [Off]                 F
│   ├── DMR / TDMA
│   │   ├── DMR late entry [Off]
│   │   ├── Slot 1 audio [On]                    1
│   │   ├── Slot 2 audio [On]                    2
│   │   ├── Slot preference... [Auto]            3
│   │   └── DMR reset                            D
│   ├── ProVoice ESK mask [Off]                  A   (ProVoice modes only)
│   ├── ProVoice EA mode [Off]                   S   (ProVoice modes only)
│   └── M17 encoder user data... [<unset>]
├── Trunking
│   ├── Trunking [Off]                           t
│   ├── Conventional scanning [Off]              y
│   ├── Return to control channel                C   (while trunking or scanning)
│   ├── Next channel                             L   (while trunking or scanning)
│   ├── Scan hold [Off]                          Y   (while scanning with -Y or --trunk-scan)
│   ├── Avoid current channel                    b   (while scanning with -Y or --trunk-scan)
│   ├── Clear avoids [0]                             (while scanning with -Y or --trunk-scan)
│   ├── ─────
│   ├── Follow
│   │   ├── Group calls [On]                     g
│   │   ├── Private calls [Off]                  u
│   │   ├── Data calls [Off]                     d
│   │   ├── Allow-list mode [Off]                w
│   │   ├── Talkgroup hold... [none]             k/l
│   │   ├── Hangtime... [1.0 s]
│   │   ├── Reverse mute [Off]
│   │   ├── Lock out talkgroup on slot 1         !
│   │   └── Lock out talkgroup on slot 2         @
│   ├── Channels & groups
│   │   ├── Import channel map CSV...
│   │   ├── Import group list CSV...
│   │   ├── Import P25 band plan CSV...
│   │   ├── Export learned P25 band plan...
│   │   ├── ─────                                    (RadioReference builds)
│   │   ├── Import from RadioReference...            (RadioReference builds)
│   │   ├── Imported RadioReference systems...       (RadioReference builds)
│   │   ├── RadioReference username... [(not set)]   (RadioReference builds)
│   │   └── RadioReference application key...        (RadioReference builds without a baked-in key)
│   ├── P25
│   │   ├── Prefer CC candidates [On]
│   │   ├── LCW explicit retune [Off]
│   │   ├── Phase 2 parameters...
│   │   └── Timing
│   │       ├── VC grace... [0.000 s]
│   │       ├── Minimum follow dwell... [0.000 s]
│   │       ├── Grant-to-voice timeout... [0.000 s]
│   │       ├── CC hunt grace... [0.000 s]
│   │       ├── Safety-net extra... [0.000 s]
│   │       ├── Safety-net margin... [0.000 s]
│   │       ├── Phase 1 error hold... [0.0%]
│   │       └── Phase 1 error hold time... [0.000 s]
│   ├── ─────
│   ├── Rigctl: <host:port> [Off]                9
│   └── Rigctl setmod bandwidth...
├── Encryption
│   ├── Mute encrypted audio [On]
│   ├── Lock out encrypted calls [Off]           e
│   ├── Clear lockouts [0]
│   ├── ─────
│   ├── Keys
│   │   ├── Basic privacy key (decimal)...
│   │   ├── Hytera privacy key (hex)...
│   │   ├── NXDN/dPMR scrambler key (decimal)...
│   │   ├── RC4 / DES key (hex)...
│   │   ├── AES-128/256 key (hex)...
│   │   ├── ─────
│   │   ├── Force basic/scrambler key [Off]      4
│   │   └── Force RC4 key [Off]                  6
│   ├── Import keys CSV (decimal)...
│   ├── Import keys CSV (hex)...
│   └── Vendor keystreams
│       ├── TYT AP (PC4) keystream...
│       ├── Retevis AP (RC2) keystream...
│       ├── TYT EP (AES) keystream...
│       ├── Kenwood DMR scrambler...
│       ├── Anytone BP keystream...
│       └── Straight XOR keystream...
├── Audio
│   ├── Output: <current sink>                       (status)
│   ├── Mute [Off]                               x
│   ├── Switch output
│   │   ├── Pulse digital output
│   │   ├── Pulse output device...
│   │   └── UDP output...
│   ├── Digital gain... [auto]                   + -
│   ├── Analog gain... [50]                      * /
│   ├── Source audio monitor [Off]
│   ├── Deemphasis [Off]
│   ├── Audio low-pass... [Off]
│   ├── ─────
│   ├── Call alert beep [Off]                    a
│   └── Alert on... [All]
├── Recording & logs
│   ├── Symbol capture
│   │   ├── Record symbols... [Off]              R
│   │   ├── Stop recording [Off]                 r
│   │   ├── ─────
│   │   ├── Replay last capture [none]           Space
│   │   └── Stop replay [Off]                    s
│   ├── WAV files
│   │   ├── Per-call WAV [Off]                   P/p
│   │   ├── Static WAV file...
│   │   └── Raw input WAV file...
│   ├── Event log
│   │   ├── Event log file... [off]
│   │   ├── Stop event log
│   │   ├── Payload logging to console [Off]     z
│   │   └── Clear event history
│   ├── LRRP output
│   │   ├── LRRP output: off                         (status)
│   │   ├── Write to ~/lrrp.txt (QGIS)
│   │   ├── Write to ./DSDPlus.LRRP
│   │   ├── Custom file...
│   │   └── Stop LRRP output
│   └── DSP structured output...
├── Display
│   ├── Compact view [Off]                       c
│   ├── Sections
│   │   ├── Channels [On]
│   │   ├── DSP panel [Off]                          (RTL-SDR input only)
│   │   ├── ─────
│   │   ├── P25 metrics [Off]
│   │   ├── P25 affiliations [Off]
│   │   ├── P25 group affiliation [Off]          T
│   │   ├── P25 neighbors [Off]
│   │   ├── P25 IDEN plan [Off]
│   │   ├── P25 CC candidates [Off]
│   │   └── P25 callsign decode [Off]
│   ├── Visualizers                               (RTL-SDR input only)
│   │   ├── Constellation [Off]                  o
│   │   ├── Constellation normalization [Radial] n   (constellation on only)
│   │   ├── Eye diagram [Off]                    E
│   │   ├── Eye diagram Unicode [On]             U    (eye diagram on only)
│   │   ├── Eye diagram color [On]               G    (eye diagram on only)
│   │   ├── FSK histogram [Off]                  K
│   │   └── Spectrum analyzer [Off]              f
│   └── Event history
│       ├── Mode [Short]                         h
│       ├── Slot [1+2]                           \
│       ├── Previous event                       [
│       └── Next event                           ]
├── Config
│   ├── Load config...
│   ├── Load profile...
│   ├── ─────
│   ├── Save config
│   ├── Save config as...
│   └── Save as default config
├── Advanced
│   ├── Realtime scheduling [Off]
│   ├── Intra-block multithreading [Off]
│   ├── SSE FTZ/DAZ [On]
│   ├── Freeze symbol window [Off]
│   ├── Simulate no-carrier event                Z
│   ├── ─────
│   └── Environment variable...
├── ─────
└── Quit DSD-neo                                 q
```

### Decoder mode

**Decoder -> Mode** opens a picker of the decode presets — Auto, P25 (Phase 1 + 2), P25 Phase 1, P25 Phase 2, DMR,
DMR (single slot), NXDN48, NXDN96, X2-TDMA, YSF, D-STAR, EDACS / ProVoice, dPMR, M17, Analog — and switches which
protocols are decoded without restarting, the same way the CLI `-f` presets do at startup. The row reads the live
preset back (`Mode... [P25 Phase 1]`), and the footer toasts `Decoding <mode>` once the change has applied.

Group policy reload:

- Importing a group list (`-G` CSV) from **Trunking -> Channels & groups** performs a full policy reload. On parse
  failure, the currently loaded list remains active.

Config profiles:

- In the Config menu, `Load profile...` lists `[profile.NAME]` sections from the active config path, or the default
  config path when no config has been loaded yet. Loading a profile applies it to the running session and disables
  autosave, matching CLI `--profile NAME` behavior.

RadioReference import:

- **Trunking -> Channels & groups -> Import from RadioReference...** opens the import wizard: sign in, find a
  system by ZIP code, by country/state/county, or by system ID, choose the site (or, for a Conventional
  Networked system, the repeaters), and review the preview of what would be generated before importing.
- Inside the system panel: `Up`/`Down`/`PageUp`/`PageDown`/`Home`/`End` move, `Space` selects a site,
  `p`/`s`/`e` cycle the partial-encryption, simulcast and ESK options, `Enter` imports, `Esc` goes back.
- An import leaves you on the site list with the selection released, so importing a second site of the
  same system — a second county of a statewide network — costs one more `Enter` and no second fetch.
  Each site is stored as its own set of files.
- The password is asked once per program run and held in memory only. The username, and in a build without
  a baked application key the key itself, are set from **RadioReference username...** and **RadioReference
  application key...** in the same submenu and stored in the config file under `[radioreference]`.
- **Trunking -> Channels & groups -> Imported RadioReference systems...** lists your imported systems (one
  row per stored import, showing the system and the site it covers, with a `*` in the left gutter when the
  running session is decoding one of its files). Several rows can name one system, one per site.
  Selecting a row offers **Use this system** (re-apply it offline, exactly as the import did),
  **Refresh from RadioReference** (re-fetch and rebuild that site's files), and **Delete imported
  files** (after a confirmation naming the system and the site).
- **Trunking -> Channels & groups -> Import channel map CSV...** and **Import group list CSV...** open a
  chooser of the imports directory's files of that kind, with an **Enter a path...** row that falls back to
  typing a path. **Import P25 band plan CSV...** loads a band plan (one row per identifier, `docs/csv-formats.md`)
  into the running decoder for a P25 site that never broadcasts `IDEN_UP`; **Export learned P25 band plan...**
  writes the identifier tables learned this session (every target's, under trunk scan) to a path you are asked
  for, in the same format, so the next run can import them. Under trunk scan the import is refused with a
  message (a target list's `p25_bandplan_csv` column is the way in) while the export still works and covers
  every target. The export's suggested file name carries the WACN/SYS when they are known and trunk scan is
  off; under trunk scan the merged multi-target file keeps the generic name.
- P25 channels are shown as four hex digits followed by the same channel as `<identifier>-<channel>` in
  parentheses (`Active Ch: 2A46 (2-2630)`, `CH:2A46 (2-2630)` in the secondary control channel list, and the
  learned Channels panel), which is the spelling a channel map CSV accepts as its first column.
- Messages from these flows are shown where you are looking: inside the prompt (a refused ZIP code or
  system ID), on the row under a chooser's title, in the `Fetching` box (which names the stage in progress),
  and on the system panel's status row. A message too long for one row wraps onto the next, borrowing the
  key-hint row until it expires; long messages stay up longer than short ones.
- The RadioReference rows are hidden in a build without libcurl or expat; the Imported RadioReference
  systems row is hidden until the imports directory resolves, and the application-key row is hidden in a
  build with a baked-in key. See `docs/radioreference-import.md`.

### Keys

A `-Y` channel-map row or a `--trunk-scan` target can carry its own key files (`keys_hex_csv`/`keys_dec_csv`
columns; see `docs/csv-formats.md`). While the row or target is parked its keys replace the global keyring;
leaving it restores the globals. The Import keys CSV rows edit the globals underneath a parked row, so a
runtime import survives the next hop. The single-key rows (Basic privacy, Hytera, scrambler, RC4/DES, AES)
disarm the keyring, so a parked keyed row overrides them again on its next hop.

## DSP Status

The DSP status panel shows RTL DSP loop state when RTL input support is available. CQPSK mode reports FLL band-edge,
carrier/Costas, NCO, and timing-recovery state for the active OP25-style chain.

For RTL-family inputs, the optional DSP panel also shows `Squelch`, which compares post-channel-filter power against the
configured SQL threshold. This is an advanced squelch diagnostic and is separate from the raw `RF Level` health line.

## Input Level Health

The input section shows a persistent advisory line when recent input-level metrics are available:

```text
| Input Level: OK rms -23.1 dBFS peak -5.4 dBFS clip 0.0%
| RF Level: CLIP rms -6.0 dBFS peak 0.0 dBFS clip 0.3% lower RF gain or add filtering/attenuation
```

`Input Level` is used for PCM-like audio inputs. `RF Level` is used for RTL-SDR, rtl_tcp, and SoapySDR receiver
samples measured before demodulation. The line is advisory only; DSD-neo never changes input volume, RF gain, AGC, or
filtering automatically.

The default RTL input line shows the SQL threshold, reading `off` when the squelch is disabled, but does not
duplicate channel power. Enable the DSP panel when you
need to inspect post-channel-filter squelch power. `RF Level` and `Squelch` are measured at different stages and are not
expected to match exactly.

The low-level threshold is controlled by `--input-level-warn-db`, `DSD_NEO_INPUT_WARN_DB`, or the `[input]`
`input_warn_db` user-config key, and defaults to `-40 dBFS`. Changes made through the terminal menu persist through
config autosave on exit when config loading is enabled.

Hot/clipping advisories use fixed thresholds: peak at or above `-1.0 dBFS` is `HOT`, and at least `0.1%` clipped or
near-rail samples is `CLIP`. Footer messages are rate-limited. RF low-level status remains persistent but does not
produce repeated low-level footer messages because a quiet channel and too little RF gain are not reliably
distinguishable from raw receiver samples alone. TCP PCM input suppresses repeated LOW/HOT/CLIP footer messages while
the decoder is idle/searching; the persistent `Input Level` line still shows the current status.

## Hotkeys (Main Screen)

Keys are case-sensitive. Some commands only make sense in specific modes (for example, trunking controls require
trunking/scanner to be enabled, and RTL controls require RTL input). Nearly every hotkey below also appears on its
menu row, right-aligned, so the menu is the place to learn them; the shifted aliases (`X`, `O`) and the visualizer
modifiers (`<` `>`, `,` `.`) are named in their row's help text rather than in its hotkey column.

### General

| Key | Action |
|---|---|
| `q` | Quit |
| `c` | Toggle compact scanner view |
| `h` | Cycle history mode |
| `x` / `X` | Toggle mute |
| `z` | Toggle payload logging (`-Z`-like) |
| `a` | Toggle call alert beeps |
| `T` | Toggle P25 group affiliation section |

### Trunking / scanning

| Key | Action |
|---|---|
| `t` | Toggle trunking |
| `y` | Toggle conventional scanning |
| `C` | Return to control channel (when following a voice channel) |
| `L` | Next channel: step the `-Y` scan list (skipping avoided rows), cycle trunking channels, or move to the next `--trunk-scan` target |
| `Y` | Hold the scan on the channel or target on air; press again to resume (`L` still moves while held) |
| `b` | Avoid the channel or target on air for the rest of the session and step to the next one |
| `g` | Toggle follow group calls |
| `w` | Toggle allow/white-list mode (uses imported group list) |
| `u` | Toggle follow private calls |
| `d` | Toggle follow data calls |
| `e` | Toggle encrypted call lockout (P25/DMR/NXDN trunking) |
| `k` / `l` | Set/clear talkgroup hold from the most recent TG (slot-aware) |
| `!` / `@` | Lock out slot 1 / slot 2 (where applicable) |

### Slots, gain & privacy

| Key | Action |
|---|---|
| `1` / `2` | Toggle synth/playback for slot 1 / slot 2 |
| `3` | Cycle TDMA slot preference (slot 1 / slot 2 / auto) |
| `+` / `-` | Digital gain up/down |
| `*` / `/` | Analog gain up/down |
| `v` | Cycle the input volume multiplier (non-RTL inputs) or the RTL monitor gain (RTL input) |
| `4` | Toggle force privacy key over identifiers |
| `6` | Toggle force RC4 key over missing PI/LE identifiers |

### Filters

| Key | Action |
|---|---|
| `V` | Toggle low-pass filter |
| `B` | Toggle high-pass filter |
| `N` | Toggle pulse-shaping band-pass filter |
| `H` | Toggle digital high-pass filter |

### Visualizers (RTL input builds)

| Key | Action |
|---|---|
| `O` / `o` | Toggle constellation view |
| `n` | Switch constellation normalization between radial (p99) and unit circle |
| `<` / `>` | Adjust constellation gate |
| `E` | Toggle eye diagram |
| `U` | Toggle eye diagram Unicode/ASCII |
| `G` | Toggle eye diagram color |
| `K` | Toggle FSK histogram |
| `f` | Toggle spectrum analyzer |
| `,` / `.` | Decrease/increase spectrum FFT size |

### Device/DSP actions

| Key | Action |
|---|---|
| `{` / `}` | RTL PPM down/up |
| `i` | Toggle inversion |
| `m` | Cycle modulation optimization mode |
| `M` | Toggle and retain the P25 Phase 2 C4FM/QPSK modulation selection |
| `F` | Toggle aggressive sync/CRC relax helpers |
| `D` | DMR reset (useful when a system goes off the rails) |
| `A` | Toggle ProVoice ESK mask (ProVoice modes) |
| `S` | Toggle ProVoice standard/EA mode (ProVoice modes) |
| `Z` | Simulate “no carrier” event |
| `8` | Connect/reconnect TCP audio input |
| `9` | Connect/reconnect rigctl |

### Capture / playback

| Key | Action |
|---|---|
| `R` | Start/save symbol capture |
| `r` | Stop symbol capture |
| `P` | Start per-call WAV saving |
| `p` | Stop per-call WAV saving |
| `Space` | Replay last captured audio (where supported) |
| `s` | Stop playback |
| `[` / `]` | Event history previous/next |
| `\\` | Toggle event history slot (or toggle M17 TX in encoder mode) |

## Channel Labels

While scanning, the screen says which channel you are listening to.

The Input Output section names the `-Y` scan list row the receiver is parked on, at the end of its row:

```
| Scan Mode:  Frequency: 462.012500 MHz Speed: 2.00 sec Channel: Marion
```

The name comes from the optional `name` column of the channel map (see `docs/csv-formats.md`); a row without one
renders exactly as it always did. The name goes last because it is the one field whose length you choose: the
frequency and speed keep their columns, and on a narrow terminal it is the name that reaches the edge.

While `Y` holds the scan, `HOLD` appears ahead of the name with the other facts about the channel on air. While
`b` has taken rows out of the rotation for the session, `Avoids: N` closes the row: it counts the rows that are
out of the list, and says nothing about the channel you are hearing, which under `-Y` is never an avoided one.

```
| Scan Mode:  Frequency: 462.012500 MHz Speed: 2.00 sec HOLD Channel: Marion Avoids: 2
```

With `--trunk-scan` the same section names the target on air and its place in the rotation:

```
| Trunk Scan:  Target: county-p25 (3/6)
```

`county-p25` is the target's `id` column (see `docs/trunk-scan.md`), and `(3/6)` is its place in the rotation. The row
shows no frequency: the protocol panels below it already carry the one being decoded. While a target is on air the
Scan Mode row above it shows no name, so the screen never names two channels at once.

Call Info repeats the answer on its own first line, because compact view hides the Input Output section:

```
| Target: county-p25
```

It reads `Target:` for the active `--trunk-scan` target, `Channel:` for the name of the `-Y` row on air when trunk
scan is not running, and nothing at all when neither scanner is running or the row has no name. The word matches the
Input Output row it repeats: a target is a whole system, and "channel" already means a channel number lower in Call
Info. Event history rows carry the same label as a bracketed prefix — see "Event History Rows" below.

## Compact View

Press `c` (or use Menu -> Display -> Compact view) to collapse the main screen to a scanner-style
layout. While active, the header shows a `Compact (c)` indicator and the frame renders only:

- the header banner and any transient status toast;
- a condensed `Status` block: decoder mode, demod/symbol rate, tuner Busy/Free (when trunking), SNR meter,
  input level, output mute state, and slot on/off states;
- the full Call Info section (the `Channel:`/`Target:` line while scanning, per-slot TGT/SRC, active channels,
  tuned frequency, TG HOLD);
- the event history, which expands into the freed rows.

Suppressed while compact: the Input Output section, visual aids (including any enabled visualizers — their
toggles are remembered and restored when leaving compact; switching one on while compact shows a brief
"hidden in compact view" toast), the detailed Audio Decode section, the optional P25 sections, and the
Channels list. The condensed Status block also omits the Voice Error counters and the `CRC/(RAS)` decoder
indicator from the full Audio Decode section — leave compact view to inspect those. The setting is
session-only and is not persisted to the config file.

## Event History Rows

While scanning, an event history row names the channel it was heard on: the channel name from a
`-Y` scan list, or the active `--trunk-scan` target's id, in brackets between the row's timestamp
and its protocol token — `2026-04-30 09:12:04 [Fire Dispatch] P25p1 TGT: 00050061; ...`. It answers
the question a bare `TGT: 00000000` row cannot, which is where encrypted traffic was heard. Data
notices (SMS, LRRP, registrations) carry the same prefix. A receiver that is not scanning a named
channel renders exactly what it always did, and Short mode (`h`) still drops only the date, so the
prefix survives into the compact row. The channel is resolved once, when the transmission's first
row renders, so a call that outlives a scan step keeps the channel it was actually heard on — and a
call heard on a scan-list row with no name stays unlabelled rather than picking up the name of the
next channel. The bracketed name is for reading, not for parsing: a channel name may itself contain
`]`.
