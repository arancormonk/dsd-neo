# Terminal UI (ncurses) Guide

This guide covers the interactive terminal UI enabled with `--frontend terminal`: how to open the menu overlay and the most useful
hotkeys.

For CLI flags and inputs/outputs, see `docs/cli.md`.

## Start the UI

- Enable UI: `dsd-neo --frontend terminal ...`
- Quit: `q` (takes effect at once, also while the decoder waits on a receiver that stopped delivering samples or an
  rtl_tcp server it is reconnecting to)
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

The keypad Enter key is `Enter` on the main screen, in the menu, prompts, help, pickers and the RadioReference panel,
on Windows too. Under tmux with `TERM=tmux-256color`, whose terminal database has no keypad Enter (`kent`), that key
arrives as `Esc O M`, and its `Esc` cancels a prompt or backs out of the menu; use the main Enter key there. In a
prompt, a character the field cannot hold (a non-ASCII character in a UTF-8 terminal, a character beyond Latin-1 on
Windows) is typed as one `?` rather than dropped, so a pasted `145．5` reads `145?5` and is refused instead of tuning
1455 MHz. The Windows keypad `+ - * /` keys type their characters. A number prompt refuses a value outside the integer
range with `Invalid integer input` instead of wrapping it.

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
*radio input only* appear while a radio is the input (an RTL-SDR, an rtl_tcp server, a SoapySDR device or an Airspy),
*Airspy input only* while an Airspy is, and *not on an Airspy* while any other input is. The `RTL-SDR` submenu under
Input holds the tuning of whichever radio runs. Rows marked *radio builds* need a build with the radio pipeline (any
SDR backend), and *RadioReference builds* one with libcurl and expat.

```text
Main Menu
├── Input
│   ├── Source: <current input>                      (status)
│   ├── Switch source
│   │   ├── Pulse Audio (mic/line)
│   │   ├── Pulse input device...
│   │   ├── Airspy R2 / Mini                         (radio builds)
│   │   ├── RTL-SDR                                  (radio builds)
│   │   ├── TCP audio... [Off]                   8
│   │   ├── UDP audio...
│   │   ├── WAV / raw file...
│   │   └── Symbol file (.bin/.raw/.sym)...
│   ├── Input volume... [4X]                     v
│   ├── Squelch... [off]                         (radio and audio input: a level in dB, off, auto[+N] or noise[+N])
│   ├── NFM bandwidth... [default]               (-fA, or a width set; radio, or audio input with rigctl)
│   ├── AM bandwidth... [default]                (-fM, a width set, or a low rate; radio, or audio input with rigctl)
│   ├── Low-input warning... [-40.0 dBFS]
│   ├── Invert signal [Off]                      i
│   ├── Airspy receiver controls                 (Airspy input only)
│   │   ├── Serial (16 hex digits; empty selects first device)...
│   │   ├── Sample rate in samples/s (auto selects automatically)...
│   │   ├── Gain mode: sensitivity, linearity, manual...
│   │   ├── Sensitivity index (0..21)...
│   │   ├── Linearity index (0..21)...
│   │   ├── LNA index (0..15)...
│   │   ├── Mixer index (0..15)...
│   │   ├── VGA index (0..15)...
│   │   ├── LNA AGC (0 off, 1 on)...
│   │   ├── Mixer AGC (0 off, 1 on)...
│   │   └── Bias tee (0 off, 1 on)...
│   └── RTL-SDR                                  (radio input only)
│       ├── Frequency... [769.768750 MHz]        (MHz; [-] with no frequency; hidden under --trunk-scan and during an I/Q replay)
│       ├── Gain... [AGC]                        (not on an Airspy; [20] (target: 10) when a trunk-scan target has its own gain)
│       ├── PPM correction... [0]                { }  (not on an Airspy)
│       ├── DSP bandwidth... [48 kHz]
│       ├── Volume multiplier... [1]             v
│       ├── Tuner autogain [On]                      (not on an Airspy)
│       ├── Bias tee [Off]                           (not on an Airspy)
│       ├── ─────
│       ├── Device index...                          (not on an Airspy)
│       ├── Restart stream
│       ├── Auto-PPM & rtl_tcp                       (not on an Airspy)
│       │   ├── Auto-PPM [On]
│       │   ├── Auto-PPM SNR threshold... [6.0 dB]
│       │   ├── Auto-PPM minimum power... [-80.0 dB]
│       │   ├── Auto-PPM zero-lock PPM... [0.60]
│       │   ├── Auto-PPM zero-lock Hz... [60]
│       │   ├── Auto-PPM freeze [Off]
│       │   ├── ─────
│       │   ├── rtl_tcp adaptive buffering [On]
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
│   ├── CQPSK path [On]                              (radio input only)
│   ├── Receive filters and names
│   │   ├── Slot filter...
│   │   ├── DCC filter...
│   │   ├── RAN filter...
│   │   ├── NAC filter...
│   │   └── Service names...
│   ├── Audio filters
│   │   ├── Low-pass filter (960 Hz) [Off]       V
│   │   ├── High-pass filter (960 Hz) [Off]      B
│   │   ├── Voice band-pass [On]                 N
│   │   ├── Digital high-pass filter [On]        H
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
│   │   ├── Data calls [Off]                     d   (edits configured value; adds (target: On/Off) when a parked row differs)
│   │   ├── Allow-list mode [Off]                w
│   │   ├── Talkgroup hold... [none]             k/l
│   │   ├── Hangtime... [1.0 s]
│   │   ├── Voice-only scan [Off]
│   │   ├── Voice qualify... [1000 ms]
│   │   ├── Voice hold... [2000 ms]
│   │   ├── Reverse mute [Off]
│   │   ├── Save user TG lockouts [On]
│   │   ├── Clear temporary TG avoids - current list [0]
│   │   ├── Lock out talkgroup on slot 1         !
│   │   └── Lock out talkgroup on slot 2         @
│   ├── Channels & groups
│   │   ├── Import channel map CSV...
│   │   ├── Import group list CSV...
│   │   ├── Import source ID list CSV...
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
│   ├── Lock out encrypted calls [Off]           e   (edits configured value; adds (target: On/Off) when a parked row differs)
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
│   ├── Analog gain... [auto]                    * /
│   ├── Source audio monitor [Off]
│   ├── Deemphasis [Off]
│   ├── Audio low-pass... [Off]
│   ├── Tone filter... [off]
│   ├── ─────
│   ├── Call alert beep [Off]                    a
│   └── Alert on... [All]
├── Recording & logs
│   ├── Symbol capture
│   │   ├── Record symbols... [Off]              R
│   │   ├── Stop recording [Off]                 r
│   │   ├── ─────
│   │   ├── Replay last capture [none]           Space (the capture closed last this session)
│   │   └── Stop replay [Off]                    s (a file or symbol playback, onto Pulse input)
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
│   │   ├── DSP panel [Off]                          (radio input only)
│   │   ├── ─────
│   │   ├── P25 metrics [Off]
│   │   ├── P25 affiliations [Off]
│   │   ├── P25 group affiliation [Off]          T
│   │   ├── P25 neighbors [Off]
│   │   ├── P25 IDEN plan [Off]
│   │   ├── P25 CC candidates [Off]
│   │   └── P25 callsign decode [Off]
│   ├── Visualizers                               (radio input only)
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

### Switching the input

Every row under Input > Switch source closes the input in use and opens the new one, and so do `Replay last capture`
(`Space`), `Stop replay` (`s`), the `8` key and a TCP connect. The new input opens first. A switch that cannot open it
changes nothing: the old input keeps running exactly as it was, and the message reads `Failed: ...` (the log says why).
A switch that opens it starts a new reception: the received tone, the call and the decoder's acquisition start again,
and the PCM noise squelch learns the new input afresh. The radio rows (`RTL-SDR`, `Airspy R2 / Mini`) are the exception
while a radio already runs: its stream stops before the new device opens, and a start that fails reopens the radio that
ran, as a restart does, so that radio comes back rather than running on untouched (an I/Q capture it was writing can
stop, and the message says so). From an audio input, a radio that does not start leaves the audio input running.

- `WAV / raw file...` opens a regular file the way `-i` opens a path: a `.wav` by its header (at its own rate), a
  `.rrc` at 48 kHz, `.bin`, `.raw` and `.sym` as symbol files, anything else as headerless PCM16LE at the raw rate
  (`-s`). A named pipe or a device is refused here, because opening one waits for a writer; give it at startup with `-i`.
  A WAV's header rate holds for that file only: the next file, UDP or TCP input opens at the raw rate again (Pulse opens
  at its own capture rate, as at startup).
- `UDP audio...` binds the address and port now. A port another program holds refuses the switch. Switching from one
  UDP port to another has to stop the running UDP input first, since it may hold the port: if the new port does not bind,
  the old one binds again (a new reception), and the message says so.
- `Pulse Audio (mic/line)` opens the default capture device, `Pulse input device...` the one chosen.
- `RTL-SDR` reopens the radio that runs, or goes back to the radio the session ran before a switch to an audio input
  (an rtl_tcp server or a SoapySDR device, say), or opens an RTL-SDR when none ran. It never opens an Airspy: from one,
  it goes back to the radio an audio switch left, else to an RTL-SDR. `Airspy R2 / Mini` always opens the Airspy.
- A switch names the input it opened (`udp:127.0.0.1:7355`, `tcp:host:port`, `pulse:<device>`, the file), so a saved
  config reopens the same input. Startup tells a file's kind by its extension, so a symbol capture reopens as one only
  when its name ends in `.bin` (`.raw` or `.sym` for a float symbol stream); a capture recorded under another name
  reopens as PCM audio.

While a scan runs (`-Y` with rows, or `--trunk-scan` with targets), the audio-input switches and `Stop replay` are
refused with `Unsupported: stop the scan first`: the scan's saved timing belongs to the running input. The M17 IP frame
receiver, the M17 encoders and MBE playback read no switched input, so they refuse every switch.

While a UDP sender is silent, a menu command is applied once the silence has lasted about half a second. A TCP server
that went away is noticed when a read times out (the socket's receive timeout, 1.5 seconds by default); an M17
session's reconnect then gives way to a menu command between its attempts. A sender that stops in the middle of a
frame is waited for, as before.

### Frequency entry

Input > RTL-SDR > `Frequency...` tunes the radio that runs. The row reads the tuned frequency in MHz
(`Frequency... [851.012500 MHz]`, `[-]` while none is known). Its prompt,
`Frequency in MHz, e.g. 146.52 (or add Hz, k, M, G)`, opens empty and reads a number the way a radio keypad does
(issue #670; it used to read every number as Hz):

- A number without a unit below 1000000 is MHz: `146.52`, `446`, `1296`, and `0000146` is 146 MHz.
- A number without a unit of 1000000 or more is Hz, so a pasted `851012500` still tunes 851.0125 MHz.
- A number without a unit from 4295 to 999999 is refused with
  `Too large as MHz. Add a unit, e.g. 146520k or 146.52M`: it is too large for MHz and too small for Hz. This
  catches a dropped `.` (`14652`), a kHz habit (`146520`) and the radio keypad habit that types 146.560 MHz as
  `146560`.
- A unit always wins: `Hz`, `k` or `kHz`, `M` or `MHz`, `G` or `GHz`, in any case and with or without a space
  (`146520k`, `146.52 MHz`, `0.85G`). `m` is mega, as in `-i`.
- A value finer than 1 Hz is refused (`162550000.5`, `146.5200005`): `Finer than 1 Hz. Use at most 6 decimals in MHz`.
  Trailing zeros are fine (`146.5200000`).
- A `,` is refused with `Use '.' for decimals, e.g. 146.52`, as a decimal comma or a thousands separator. The keypad
  decimal key types what the keyboard layout gives it: `,` on a German layout, which is refused rather than read as
  another number.
- Nothing above 4294.967295 MHz can be entered, in any unit (`Above the 4294.967295 MHz input limit`). On Windows the
  tune is refused above 2147.483647 MHz with `Above 2147.483647 MHz, this build's limit`; under P25 trunking a
  control channel above 2147.483647 MHz is refused on every platform with `Above 2147.483647 MHz, the trunking limit`.
- A sign, an exponent (`1e6`), a second `.` or a character the prompt shows as `?` is refused with
  `Not a frequency. Type MHz, e.g. 146.52`; a letter suffix that is not a unit (`0xff`, `5X`) with `Unknown unit. Use Hz, k, M or G`; 0 with
  `Frequency must be above 0`; more than 32 characters with `Too long: at most 32 characters`.

A refused entry reopens the prompt with the text typed and the reason inside it, so a typo is fixed in place. `Esc`, or
`Enter` on an empty field, cancels. The toast names the frequency in MHz: `Applied: RTL frequency -> 851.012500 MHz`
names the frequency the receiver took, `Accepted: RTL frequency -> 851.012500 MHz (pending)` one still being applied,
`Deferred; retry: RTL frequency -> ...` one the receiver could not take yet (retry it), and `Failed: ...` one that did
not tune; ` (scanner stopped)` follows a tune that ended a `-Y` scan. A manual tune from Qt/Android or the control API
(`Applied: tuned -> 851.012500 MHz`) and the `-Y` avoid (`Avoiding 769.768750 MHz (1 avoided)`) name their frequency
the same way. During single-system P25 trunking the entry selects a new control channel
(`Applied: P25 control channel -> 851.012500 MHz`; see "Trunking & Scanning" in `docs/cli.md`). The row is hidden
during `--trunk-scan`, which owns the tuner, and during an I/Q replay, which plays the tuning its capture recorded.

### Decoder mode

**Decoder -> Mode** opens a picker of the decode presets — Auto, P25 (Phase 1 + 2), P25 Phase 1, P25 Phase 2, DMR,
DMR (single slot), NXDN48, NXDN96, X2-TDMA, YSF, D-STAR, EDACS / ProVoice, dPMR, M17, TETRA, Analog, AM — and switches
which protocols are decoded without restarting, the same way the CLI `-f` presets do at startup. The row reads the live
preset back (`Mode... [P25 Phase 1]`), and the footer toasts `Decoding <mode>` once the change has applied.

AM (`-fM`) needs an I/Q radio input. On a PCM input the picker's AM row reads `AM (needs an I/Q radio input)`, and
choosing it sends nothing: the footer gives the reason
(`AM demodulation needs an IQ radio input; monitor externally demodulated AM audio with -fA`). TETRA (`-fT`) needs an
I/Q radio input or a symbol capture: elsewhere its row reads `TETRA (needs an I/Q radio input)` and choosing it gives
its reason the same way. A running AM session
whose input is switched to a PCM one falls back to Analog, and the footer says why. **Input -> AM bandwidth...
[default]**, right after `NFM bandwidth...` below `Squelch...`, sets the AM channel-filter width (5000..20000 Hz, 0 for
the 6000 default) as **NFM bandwidth...** sets the NFM one (below): offered on a radio input while AM is the configured
mode, or while an explicit AM width is set under another mode (Analog included, so a width a switch from NFM to AM would
be refused for can be narrowed first), or while the running stream's DSP rate cannot filter the 6000 default but can
filter a narrower AM width (a replay or device forcing 7.5 kHz), it passes the value as typed, the engine refuses one
outside the range or one the DSP rate cannot filter, keeping the width it had, and the label reads the setting back
(`[8 kHz]`, or `[default]`). It edits the configured AM width, which a save writes and a scan row's leave keeps: under a
`-Y` row with its own decode mode (a digital row, or an `nfm` row, which runs FM) the width applies when the scanner
returns to a blank row, and the row stays offered while an `nfm` row runs over the AM session. An `am` scan row runs the
AM monitor on any session, so the row is offered while one is on air: under an `am` row without a width of its own the
edit applies live, and under one with its own `--am-bandwidth-hz` it is the configured default's, which the row
overrides until it leaves (the message says the channel overrides it). **DSP bandwidth...** refuses a DSP bandwidth that
cannot filter the AM width while AM is the configured mode, or on any session while the scan has an `am` row or target
without a width of its own (the 6 kHz default needs 8 kHz or more). The status line shows the width in force next to the
DSP bandwidth: `DSP-BW: 48 kHz;` on the source row and `Analog: AM 6 kHz (default);` on the channel row. On audio input with a rigctl peer the row reads `AM
passband... [default]` and sets the AM passband the peer is asked for instead (below), offered while an `am` scan row is
on air or an explicit AM width is set. The Auto-PPM switch leads the **Auto-PPM & rtl_tcp** submenu, above the settings
it governs.

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
- **Trunking -> Channels & groups -> Import source ID list CSV...** prompts for an `id,name[,tags]`
  CSV of source radio ID names. These labels carry no allow/block or media policy and apply in conventional
  decode and trunk scan alike. A successful import replaces the source list; an unreadable file, a load
  failure, or a file with no usable rows keeps the current list and path. Source labels prefer this list,
  then fall back to the group list's exact row, including learned talker aliases. See `docs/csv-formats.md`.
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

A `-Y` channel-map row or a `--trunk-scan` target can carry key files (`keys_hex_csv`/`keys_dec_csv`) or embedded
Basic Privacy, Hytera, or AES values (`single_key_dec`/`single_key_hex`; see `docs/csv-formats.md`). While the row or
target is parked its keys replace the globals; leaving restores them. The Import keys CSV rows edit the globals
underneath a parked row, so a runtime import survives the next hop. The single-key menu rows disarm the keyring, so
a parked keyed row overrides them again on its next hop.

## Input Output Rows

The Input Output section keeps its layout still while its readings change (issue #669). Each reading takes a fixed
number of places, so a value that changes, or a status word that comes and goes, moves nothing after it. A reading
that rounds to zero never shows a minus sign (`0.0 dB`, not `-0.0 dB`). A value too large for its places shows as the
largest one that fits, marked `>` or `<` (`PPM: <-99`, `NCO= >+9999.9 Hz`), so the field does not grow. Rows that redraw
with live readings are cut at the screen edge instead of wrapping onto the next line. The cut falls only between whole
fields, never inside a reading, and is marked with `…` (`...` on a terminal without UTF-8 or with `DSD_FORCE_ASCII`).
The last screen column is never written, since writing it wraps the line.

Each radio or audio input has two rows. The source row comes first: the device and its receiver readings, with the
tuned frequency last. The channel row follows: the squelch and its live reading first, then the channel width with
where it came from. Each row puts its live readings ahead of configuration text, so a narrow terminal cuts the
configuration first:

```text
| RTL: 0; G:  40dB; PPM:    0; Mon: 2X; DSP-BW: 48 kHz; FRQ:  146.520000 MHz;
| SQL: noise +10 dB (quieting   23 dB);              Analog: NFM 12.5 kHz (row; default 16 kHz);
| UDP Signal Input: 127.0.0.1:7355; Pkts:    123; Drops:      0; 48 kHz; 1 Ch; IV:  1X; RIG: rig.local:4532;
| SQL: off; Passband: NFM 12.5 kHz (-B);
```

The squelch readout keeps the room of the longest status its setting can show, so the width after it holds its column
from `(learning)` to `(quieting   23 dB)`. The UDP counters take six places each: up to 999999 as digits, then tenths
of a unit (`1.2M`, `4.2G`). `FRQ:` is the tuned frequency in MHz to the Hz, in the eleven places the highest
frequency takes (`FRQ: 4294.967295 MHz;`), and `FRQ:           - MHz;` while none is known. An Airspy names itself on a
row of its own (`| Airspy: <serial>; <rate> samples/s;`) above its receiver row (`| G: ...`). A SoapySDR device's source
row starts `| Soapy;` and ends with its arguments (`Args: driver=...;`), so at 80 columns `FRQ:` still fits ahead of
the cut. Auto PPM and `External RTL Tuning on UDP` are rows of their own. Rows whose text changes only with
configuration or with a scan target wrap as before, so a key or a file path is never hidden: the trunking and EDACS
toggles, TG HOLD, key rows, file outputs, External RTL Tuning, M17 UDP and Reverse Mute.

## DSP Status

The DSP status panel shows the radio front end's DSP loop state in any build with the radio pipeline, whichever SDR
backend (RTL-SDR, SoapySDR or Airspy) it carries. CQPSK mode reports FLL band-edge, carrier/Costas, NCO, and
timing-recovery state for the active OP25-style chain.

For RTL-family inputs, the optional DSP panel also shows `Squelch`, which compares post-channel-filter power against the
configured SQL threshold. This is an advanced squelch diagnostic and is separate from the raw `RF Level` health line.
Under the [auto squelch](cli.md#auto-squelch---squelch-auto) it shows the gate, the channel power and the setting with
its floor: `Squelch: Open   ch: -58.2 dB sql:auto +10 dB (floor  -78.3 dB)` (`Closed` between transmissions, `Off`
where the auto squelch does not run), and under the [noise squelch](cli.md#noise-squelch---squelch-noise) the same with
its quieting: `Squelch: Open   ch: -58.2 dB sql:noise +10 dB (quieting   23 dB)`. Every DSP line is one screen row,
cut between whole fields at the edge, with its readings in fixed places (see Input Output Rows).

## Input Level Health

The input section shows a persistent advisory line when recent input-level metrics are available:

```text
| Input Level: OK      rms  -23.1 dBFS peak   -5.4 dBFS clip   0.0%
| RF Level: CLIP    rms   -6.0 dBFS peak    0.0 dBFS clip   0.3% lower RF gain or add filtering/attenuation
```

`Input Level` is used for PCM-like audio inputs. `RF Level` is used for RTL-SDR, rtl_tcp, and SoapySDR receiver
samples measured before demodulation. The line is advisory only; DSD-neo never changes input volume, RF gain, AGC, or
filtering automatically.

On a radio input the line follows the receiver's samples, not the decoder: when no samples have arrived for 2 seconds
(a stalled dongle, an rtl_tcp server that went quiet) or none have arrived yet, it reads `| RF Level: No input` with no
readings instead of the last level. A live receiver's stale level prints no warning either; an I/Q replay prints the
warnings its capture calls for, however long it pauses.

The channel row under the RTL input and the audio inputs (Pulse, TCP, UDP, WAV and stdin) shows the SQL setting,
reading `off` when the squelch is disabled, but does not duplicate channel power. Under the auto squelch the row shows
the margin and what the squelch is doing: `SQL: auto +10 dB (floor  -78.3 dB)`, `(learning)` until it has a floor, or
why it is off here: `(off: no radio input)`, `(off on digital)`. Under the noise squelch it shows the quieting it reads:
`SQL: noise +10 dB (quieting   23 dB)`, `(starting)` before its first window, or `(as auto: floor  -78.3 dB)` on an AM
or narrow channel,
where the auto squelch runs it. On audio input the [noise squelch](cli.md#noise-squelch-on-audio-input) reads
`(learning)` until it has a noise reference, then its quieting; `(off: no band above voice)` when the input carries
nothing above voice (its audio is low-passed), `(off: no room above voice)` when its rate leaves no band, and `(off on
AM audio)` on an AM monitor. The M17 encoder's RTL input line reads the tracker as the monitor's does. An EDACS session
reads `(EDACS analog calls)` between calls (`, as auto` under the noise squelch) and the tracker's reading during
one. On audio input both read `(off: no radio input)`: no dynamic squelch runs for them there (issue #625).
While a scan row or target overrides the squelch with `--squelch-db` or `--squelch`, the line shows the
row's setting first and the configured default beside it: `SQL: -60.0 dB (row; default -80.0 dB)`, `SQL: auto +6 dB
(floor -81.0 dB; row; default -60.0 dB)`. The `Squelch...` row is labelled with the configured setting
(`Squelch... [auto +10 dB]`) and its prompt takes a level in dB, `off`, `auto` / `auto+N` (N from 3 to 30 dB over
the floor) or `noise` / `noise+N` (N from 3 to 30 dB of quieting), opening on the setting in force (`-60.0`, `off`,
`auto+10`, `noise+10`). A level whose dB would not read back as itself opens as its linear power instead: one within
0.05 dB of full scale (`0.0` would read as off) and one below -200 dB (the prompt's lowest), so accepting the prompt
unchanged never turns the squelch off or moves it. On audio input the prompt takes a level, `off` or `noise[+N]`, and refuses `auto` with a toast,
since audio input has no channel power to learn a floor from. The AM monitor on its own refuses `noise` with a toast. It
always edits that configured default; if a
row is overriding it, the change takes effect when the scanner leaves the row, and the toast says so
(`Default squelch -75.0 dB; this channel overrides it (-60.0 dB)`). The M17 VOX `SQL:` field on a non-RTL input
reads the same way after the measured power, and the DSP panel's `Squelch` line marks a row's threshold with
`(row)`. Enable the DSP panel when you
need to inspect post-channel-filter squelch power. `RF Level` and `Squelch` are measured at different stages and are not
expected to match exactly.

While a scan row or target is on air that can take a "this channel" edit (issue #518), the `Squelch...`, `NFM
bandwidth...`, `AM bandwidth...`, `Gain...` and `Tone filter...` rows open a chooser before their prompt: `All channels
(default)...` goes on to the prompt above, which edits the configured default; `This channel (county-p25)...` prompts on
what the row runs now (whole dB from -100 to 0 for the squelch, 0 = off, `auto[+N]` on an `nfm` or `am` row on a
radio input, or `noise[+N]` on an `nfm` row; the
width in Hz; the gain, 0 = AGC; a tone picker and list) and changes that row alone for the rest of the session; `This
channel: use the default` (offered when the row's list sets the setting) makes it follow the configured default, and
`This channel: back to the list value` (offered while it runs an edit) drops the edit. The edit goes to the row named
when the chooser opened, at once if it is on air and from its next visit otherwise (`This channel (county-p25): squelch
-55 dB for this session`, `... from its next visit`). A width row is offered only under a row of its kind, and the gain
only for a `--trunk-scan` target on an RTL-family input other than an Airspy. Edits end when the scan does (under `-Y`,
when the scanner leaves or the channel map changes) and are never saved. On audio input with a rigctl peer the width
rows' chooser is titled `NFM passband` or `AM passband`, and their `This channel` prompt (`NFM passband Hz on this
channel (8000..25000)`) opens on the passband the row runs: its own width, the configured one it follows, or `-B`
standing in for an unset NFM width. See [Live per-channel edits](trunk-scan.md#runtime-behavior).

`Decoder > Receive filters and names` edits slot, DMR DCC, NXDN RAN and P25 NAC filters.
Each filter has separate allow and block lists and an enabled state. Enter identifiers
separated by `/`; NACs are hexadecimal (`293/370`). Block wins when a value appears
in both lists. An enabled filter waits in `Checking` until a validated identifier
arrives. Rejected traffic stays muted and creates no call history, per-call recording
or activity hold. Slot filters apply only to supported two-slot protocols. A manual
scan hold keeps the channel selected even when its traffic is rejected.

Its `Service names...` row edits a naming table using options
such as `--dcc-name "7=Hospital" --tone-name "141.3=Traveler"`. A matching access-code
or tone name takes precedence over a slot name; otherwise the existing channel label
is used. Naming does not enable filtering. Names appear in live calls, completed
history and named per-call WAVs; NFM calls include their received CTCSS/DCS tone.
The single-slot DMR decoder (`-fr`, `-nm`) shows every call in its `SLOT 1` block, its one
lane; a `--slot-name` there names the over-the-air slot the call was proven on, as the `-J`
slot field does.

Both editors offer `All channels` and, for a compatible active scan row, `This channel`.
`Use default` removes that field's override; `List value` restores its imported value.
Channel edits last for the session and leave the source CSV unchanged. The editor
captures its destination before prompting, so a scan move cannot redirect an edit to
another row. See [Receive filters and service names](cli.md#receive-filters-and-service-names)
for limits and identifier syntax.

`DSP bandwidth...` prompts with the selectable bandwidths (4, 6, 8, 12, 16, 24, 48 or 72 kHz; 72 is TETRA's) and snaps
another value to the nearest of them, the narrower of two as near. On an Airspy it refuses 72 kHz, since no Airspy
sample rate halves to it.

`DSP-BW:` on the RTL source row is the DSP bandwidth, the demodulator's sample rate that `DSP bandwidth...` sets. Under
`-fA` (or `-fM`, reading `Analog: AM ...;`) the channel row below it shows the analog channel width in force, `Analog:
NFM 12.5 kHz;`: the width the front end reports while a running stream runs the analog monitor, otherwise the configured
one, so a scan row running a digital protocol shows the width its leave returns to. The unset default reads `Analog: NFM
16 kHz (default);`, and `Analog: NFM 12 kHz (DSP-limited);` beside `DSP-BW: 12 kHz;` when the DSP rate rather than the
channel filter bounds the channel, which is what the unset default does below a 20 kHz DSP rate: no channel filter runs
there, so the rate itself is the bound. With the stream stopped, the line reads what the next start runs at `DSP-BW:`,
so a 12 kHz DSP bandwidth still shows `Analog: NFM 12 kHz (DSP-limited);`, and so does a scan row running a digital
protocol at a 12 kHz rate. An `nfm` or `am` scan row shows the field on any session, and one that sets its own width
reads `Analog: NFM 12.5 kHz (row; default 16 kHz);` (an `am` row's `--am-bandwidth-hz`:
`Analog: AM 8.333 kHz (row; default 6 kHz);`), naming the configured width of its kind it overrides.
`NFM bandwidth...` (Input, right after `Squelch...`; offered on a radio input while `-fA` is the configured mode or an
`nfm` scan row is on air, or while an explicit width is set under another mode, `-fM` included, so that a width a switch
to `-fA` would be refused for can be narrowed first) shows the configured setting, `[12.5 kHz]` or `[default]`, takes
any width from 8000 to 25000 Hz, or `0` for the default, and applies it live (while a row's own width is on air the edit
waits for the row to leave, and the message says the channel overrides it); a width the DSP rate cannot filter is
refused with a message naming both and the fix (on a digital session too, while the scan has an `nfm` row or target
without a width of its own, which runs the configured width), and a `DSP bandwidth...` value the explicit NFM width
cannot run at is refused with one saying to narrow the width first. Input > Switch source > RTL-SDR refuses the switch,
keeping the running input, when its DSP bandwidth cannot filter the explicit NFM width, and Switch source > Airspy
refuses it while `DSD_NEO_CHANNEL_LPF=0` turns off the filter the width needs; a switch whose start fails anyway (a
width the Airspy's rate cannot filter, a device that does not open) puts the running input back with the reason. The
Airspy's start checks only the width it opens on, the one in force, and a scan row that runs a digital protocol has
none: a configured width the Airspy's rate cannot filter then waits for the scan to stop, whose refused return to the
monitor settles it (see `docs/cli.md`, Analog reception). The Qt and Android Radio sheet shows the same reading, spelled
the same way, on any session while an `nfm` or `am` row is on air; a row's own width reads first there, with a `row`
badge and the configured default of its kind the stepper edits beside it (`12.5 kHz` over a `row` badge and
`default 16 kHz`; `default 6 kHz` under an `am` row while the AM width is unset).

On audio input with a rigctl peer (`-U`), which demodulates what DSD-neo hears, the two rows set the passband the peer
is asked for (`-B` in `docs/cli.md`, Trunking & Scanning, has the rule): `NFM passband...` while `-fA` is the configured
mode or an `nfm` scan row is on air, `AM passband...` while an `am` row is, and either while an explicit width of it is
set. No DSP rate bounds or checks them. An unset NFM width reads as what stands in for it, `NFM passband... [-B 12.5
kHz]`, or `[peer's own]` without `-B`, and the prompt says so (`NFM passband Hz (8000..25000; 0 = -B 12.5 kHz)`, `AM
passband Hz (5000..20000; 0 = default 6000)`). An edit that changes the passband in force is asked of the peer at once
(`Applied: NFM passband -> 12.5 kHz`, or `Applied: NFM passband -> 12.5 kHz (-B)` back on `-B`), and one the peer
refuses, or whose reply is lost, is put back (`Refused: NFM bandwidth -> 20 kHz: the rigctl peer refused the passband`);
Trunking > `Rigctl setmod bandwidth...` is asked of the peer the same way while `-B` stands in. The Pulse, TCP, UDP, WAV
and stdin channel rows show that passband after the squelch, `Passband: NFM 12.5 kHz (-B);` (`NFM 20 kHz`,
`AM 6 kHz (default)`, `NFM peer's own`, or `NFM 12.5 kHz (row; default -B 12.5 kHz)` while a row's own width is on air),
whenever the rows are offered; on a digital session that offers them for an explicit width, the field reads what every
tune asks the peer for there, `-B` (`NFM 12.5 kHz (-B)`), not the width set. The Pulse, TCP and UDP source rows' `RIG:` field
names the rigctl host and port the peer is reached at. The Qt and Android monitor shows the same passband in its
`PASSBAND` row, whose Edit opens the Passband sheet. Audio input without a rigctl peer takes no width: the rows are
hidden and the line shows none.

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
| `!` / `@` | Lock out slot 1 / slot 2 (where applicable); lifetime follows **Save user TG lockouts**, like Qt/Android **Avoid TG**/**Lock out** |

**Save user TG lockouts** defaults to On: quick lockouts save to a configured global groups file. Turn it Off
in **Trunking → Follow**, or start with `--tg-lockout-session`, to keep subsequent lockouts temporary. Clear them
using **Clear temporary TG avoids - current list**, reload the list, or stop the decoder. List edits still save;
changing this setting does not convert existing blocks. Scan rows with their own groups lists keep separate
avoids, while rows inheriting the global list share its avoids.

Qt/Android **Skip** is a separate call-skip action: it leaves the call without changing the talkgroup list and
expires independently of **Save user TG lockouts**. See [Skip lifetimes](cli.md#trunking--scanning). The terminal's
**Clear temporary TG avoids - current list** also clears call skips in that scope; Qt/Android labels this action
**Clear temporary avoids and call skips — current list**.

### Slots, gain & privacy

| Key | Action |
|---|---|
| `1` / `2` | Toggle synth/playback for slot 1 / slot 2 |
| `3` | Cycle TDMA slot preference (slot 1 / slot 2 / auto) |
| `+` / `-` | Digital gain up/down |
| `*` / `/` | Analog gain up/down (1–100; from auto they start at 50, and below 1 is auto again) |
| `v` | Cycle the input volume multiplier (non-RTL inputs) or the RTL monitor gain (RTL input) |
| `4` | Toggle force privacy key over identifiers |
| `6` | Toggle force RC4 key over missing PI/LE identifiers |

### Filters

| Key | Action |
|---|---|
| `V` | Toggle the analog 960 Hz low-pass filter |
| `B` | Toggle the analog 960 Hz high-pass filter |
| `N` | Toggle the analog voice band-pass (300-3400 Hz; takes CTCSS and DCS signalling out) |
| `H` | Toggle digital high-pass filter |

### Visualizers (radio builds, radio input)

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
| `F` | Toggle Relaxed CRC checks (the same setting as `-F` / `--strict-crc`) |
| `D` | DMR reset (useful when a system goes off the rails) |
| `A` | Toggle ProVoice ESK mask (ProVoice modes) |
| `S` | Toggle ProVoice standard/EA mode (ProVoice modes) |
| `Z` | Simulate “no carrier” event |
| `8` | Connect/reconnect TCP audio input |
| `9` | Connect/reconnect rigctl (the TCP input's host at the rigctl port); a reconnect replaces the old connection, and one that fails keeps it |

### Capture / playback

| Key | Action |
|---|---|
| `R` | Start/save symbol capture |
| `r` | Stop symbol capture |
| `P` | Start per-call WAV saving |
| `p` | Stop per-call WAV saving |
| `Space` | Replay the symbol capture closed last this session (a stop, a new capture or an hourly rotation closes one) |
| `s` | Stop a file or symbol playback and switch to the configured Pulse input; with no playback running it does nothing, and if Pulse does not open the playback runs on |
| `[` / `]` | Event history previous/next |
| `\\` | Toggle event history slot (or toggle M17 TX in encoder mode) |

## Channel Labels

While scanning, the screen says which channel you are listening to.

The Input Output section names the `-Y` scan list row the receiver is parked on, at the end of its row:

```
| Scan Mode: Frequency:  462.012500 MHz      Hangtime: 2.00 sec Channel: Marion
```

The name comes from the optional `name` column of the channel map (see `docs/csv-formats.md`); a row without one
shows none. The name goes last because it is the one field whose length you choose: the frequency and hangtime keep
their columns, and on a narrow terminal it is the name that reaches the edge.

While `Y` holds the scan, `HOLD` appears after the frequency in a room that stays there when it is gone, so nothing
moves when the hold starts or ends. While `b` has taken rows out of the rotation for the session, `Avoids: N` closes
the row: it counts the rows that are out of the list, and says nothing about the channel you are hearing, which under
`-Y` is never an avoided one. With `--scan-voice-only`, `Voice: QUALIFY|VOICE|TAIL` follows the frequency (and the
trunk-scan `(n/m)`) in a room of its own: QUALIFY while synced without voice, VOICE while voice media is active, TAIL
while holding past the last voice frame. A trunked `--trunk-scan` target shows no marker, since the gate leaves trunked
targets alone.

```
| Scan Mode: Frequency:  462.012500 MHz HOLD Hangtime: 2.00 sec Channel: Marion Avoids: 2
```

With `--trunk-scan` the same section names its place in the rotation and the target on air:

```
| Trunk Scan: (3/6)                Target: county-p25
```

`HOLD` and `[avoided]` follow the position in rooms that stay there when they are gone, then the target's id, then
the list's `Avoids: N`.

`county-p25` is the target's `id` column (see `docs/trunk-scan.md`), and `(3/6)` is its place in the rotation (right
aligned to the count's digits, `( 3/12)`). The row
shows no frequency: the protocol panels below it already carry the one being decoded. While a target is on air the
Scan Mode row above it shows no name, so the screen never names two channels at once.

### Scan Timing

Directly under whichever scanner row is active — Scan Mode under `-Y`, Trunk Scan under `--trunk-scan`, and only the
Trunk Scan one when both are running — a `Scan Timing` row says why the receiver is staying where it is and how much
of that is left:

```
| Scan Timing: Idle dwell          1.8s/  3.0s  hold 2.0s
| Scan Timing: Following call                   dwell 3.0s (suspended)  hang 2.0s
| Scan Timing: Voice tail          1.5s/  2.0s  dwell 3.0s (suspended)
| Scan Timing: Manual hold                      dwell 3.0s (paused)
| Scan Timing: Hangtime            1.4s/  3.0s
```

The phrase names the reason; the `remaining/total` pair after it, when there is one, is whichever window is actually
running out. The phrase takes the room of the longest one and the pair keeps its room when no window runs, so the
`Visit:` cap after them holds its column while the reason changes; the budgets that come and go with the reason
follow it. Each time takes six places: tenths of a second up to `999.9s`, then tenths of an hour (`48.0h`).
Countdowns are truncated, never rounded up: the row never shows more time than is left, and reads the same as the Qt
and Android panel.

Under `-Y` without `--scan-voice-only`, `Hangtime` on the upper Scan Mode row is the configured `-t` value.
The Scan Timing row shows the running window, which uses the scanner's whole-second comparison: this timer permits
a step only when the time since the recorded sync second exceeds `-t`. For `-t 6`, the upper row shows `6.00 sec` while the
countdown total is `7.0s`; for `-t 0`, those values are `0.00 sec` and `1.0s`. The setting has not changed.
Waiting for the next clock-second boundary with `-t 0` does not guarantee a full second of silence after the
last sync. NXDN's additional grace period is described below.

| Phrase | Staying because | Running timer |
| --- | --- | --- |
| `Retune pending` | a backend retune request has not resolved | none |
| `Retune retry` | the retune failed; cooling down before retrying in place | the retry cooldown |
| `Acquiring control` | the trunking state machine is hunting a control channel | none |
| `Following call` | the trunking state machine is following a call | none; `hang` is the budget |
| `Voice` | conventional voice media is active (`--scan-voice-only`) | the activity hold |
| `Voice tail` | holding past the last voice frame (`--scan-voice-only`) | the activity hold |
| `Activity hold` | allowed conventional activity was decoded | the activity hold |
| `Manual hold` | `Y` holds the scan here | none |
| `Qualify` | synced under `--scan-voice-only`, no allowed voice yet | the qualify window |
| `Idle dwell` | nothing holds the row | the idle dwell |
| `Hangtime` | `-Y` without `--scan-voice-only`: waiting out `-t` since the last sync | next whole second after `-t` |
| `Carrier` | an analog row's carrier is open (an `nfm` or `am` row, an `nfm-conventional` or `am-conventional` target, or a row of an untyped list scanned under `-fA` or `-fM`): squelch open, whether or not audio plays | `-Y`: the hangtime window; trunk scan: the activity hold; none while the scan keeps traffic the tone filter rejected |
| `Tone check` | an FM row's carrier (an `nfm` row or `nfm-conventional` target, or a row of an untyped list scanned under `-fA`) while the tone filter is still checking its tone or code, muted, for at most 800 ms (1,600 ms while the list holds a DCS code and a received code is still being confirmed) | none: the check is not activity, and the `-Y` hangtime or the target's hold or idle dwell runs on beneath it unchanged |

Which phrases you can see depends on the protocol: an NXDN trunked target has no state machine to report control
acquisition, so it never reads `Acquiring control`. An analog row (an `nfm` or `am` row or target, or any row of an
untyped list scanned under `-fA` or `-fM`) never reads `Voice`, `Voice tail` or `Qualify`, since the voice gate does not
apply to it: it reads `Carrier` while its carrier holds it (`Tone check` while the tone filter is checking an FM row),
then `Hangtime` (`-Y`) or `Activity hold` (trunk scan) for the tail. A check that ends before its verdict leaves no
tail of its own: the row reads whatever ran beneath it, and short bursts the tone filter never passes cannot hold it.
Traffic the tone filter rejects holds nothing: the row moves on at the scanner's next pass. When the scan has nowhere
else to take it, the row keeps it, muted, for as long as its carrier lasts, and reads `Carrier` with no countdown.
While a typed scan row is on air, the decoder picker's label names its class beside the configured mode,
`Mode... [DMR; scan nfm]` (`scan am` for an `am` row).

The values that follow are the *effective* ones for the row on air, after CSV and option overrides:

- `dwell` is the idle dwell, shown as a separate budget when it is not the running timer, including the interval
  after hold release before the next decoder tick arms it. `(suspended)` means something on the air has disarmed
  it — a call, a retune, a control-channel hunt — and `(paused)` means the operator hold has.
- `hold` is the activity hold, printed only when it is not itself the running timer. It appears on conventional
  and `-Y` rows only; a trunked target has none.
- `hang` is the active protocol's effective hangtime, and appears only while a trunked call is being followed.
  It includes `DSD_NEO_DMR_HANGTIME` / `DSD_NEO_P25_HANGTIME` overrides and the optional P25 Phase 1 error-hold
  extension. NXDN uses `-t`. A zero hangtime is omitted, and idle trunked targets show their dwell instead.
- `Visit: <remaining>/<cap>` is the per-visit cap (`--scan-max-visit-ms`, or the row's own value), and appears only
  when a cap applies to the row on air. `Visit: paused` means the cap is not counting down right now — a hold
  suspends it, a visit with nothing yet to count from has nothing to count, and a rotation with nowhere else to go
  (a single target, or every alternate avoided or cooling down) re-arms the cap instead of ever firing it.

With `--trunk-scan`, a suspended dwell starts again from zero once call following or the conventional hold ends
and no retune is pending. In particular, a conventional `Voice tail` is followed by a fresh `Qualify` window;
the tail and dwell waits add together. See the [scan timing example](trunk-scan.md#runtime-behavior) for how
`dwell_ms` and `activity_hold_ms` affect the time before rotation.

The row is not shown in compact view. The countdown is a published deadline differenced against the terminal's own
clock, so it keeps running while the input is stalled and stops at `0.0s`; the phrase beside it is only as fresh as the
last snapshot, which is published while samples flow. A narrow terminal cuts the row between whole fields at the edge
rather than wrapping it. On an NXDN row under `-Y`, the decoder keeps the scan two seconds past each confirmed frame,
so the `Hangtime` countdown can initially exceed its displayed total. That total is `floor(-t) + 1` seconds, matching the scanner's
strict whole-second comparison; even `-t 0` waits for the next whole second.

Call Info repeats the answer on its own first line, because compact view hides the Input Output section:

```
| Target: county-p25
```

It reads `Target:` for the active `--trunk-scan` target, `Channel:` for the name of the `-Y` row on air when trunk
scan is not running, and nothing at all when neither scanner is running or the row has no name. The word matches the
Input Output row it repeats: a target is a whole system, and "channel" already means a channel number lower in Call
Info. Event history rows carry the same label as a bracketed prefix, or the service name that labels the traffic —
see "Event History Rows" below.

## Received Tone

While the analog FM monitor runs (`-fA`), Call Info carries the sub-audible tone or DCS code the receiver hears, under
the channel line and in compact view too:

```
| Rx tone: CTCSS 100.0 Hz
| Rx tone: DCS D023N / D047I
```

It reads `CTCSS 100.0 Hz` once a supported tone is confirmed, and `DCS D023N / D047I` once a DCS code is: both codes
that send the same signal, since the receiver cannot tell which one the transmitter was set to, each as three octal
digits with leading zeros and the polarity, normal first. D023N and D047I both read `DCS D023N / D047I`, and D023I reads
`DCS D047N / D023I`. It reads `detecting` while a carrier is being evaluated, `none` when the carrier carries no
supported tone or code (or it was lost), and `—` when there is no carrier (`-` on a terminal without UTF-8), including
while a stdin, UDP or TCP stream, or a live radio stream such as an `rtl_tcp` connection, has stopped delivering audio
for about half a second. The line is shown exactly while detection runs, so it is absent outside the analog FM monitor,
while an RTL stream is not yet outputting monitor audio, and at an input rate detection cannot use (below 2400 Hz or
above 320 kHz). This is only what is received; it never reflects a tone filter setting, and it clears on a retune, a
manual channel cycle, a scan row or target change, a mode change, an input switch and stop. A retune the audio producer
makes on its own, such as `rtl_fm` scanning several frequencies into stdin, is not one of these: after a hop that leaves
a gap shorter than half a second, the previous channel's tone can show for a few hundred milliseconds until it is
dropped. The same text comes from the shared app-control view the Qt/Android monitor uses, so both always agree.
Supported tones and codes, timing, the DCS alias table and the requirements on externally demodulated audio are in the
CLI guide: [CTCSS](cli.md#received-tone-ctcss-on-the-analog-monitor) and
[DCS](cli.md#received-code-dcs-on-the-analog-monitor).

### Tone filter

While a CTCSS/DCS receive policy is in force on the analog FM monitor (`--tone-allow`, `--tone-block`, `[analog]
tone_filter`, or an `nfm` scan row's own), Call Info adds a `Tone filter:` line under `Rx tone:`, compact view
included:

```
| Rx tone: CTCSS 67.0 Hz
| Tone filter: allow 100.0 Hz/D023N — muted: not allowed
```

The line is the setting, never the received value: the policy (`allow` or `block` and its list, tones with their unit,
`…+N` for entries that do not fit; while a scan row's own policy runs, followed by the configured one it shadows, as in
`block D023N (row; default allow 100.0 Hz)`, and `off (row; default …)` for a row that turns the filter off), and while
a carrier is heard what it does with it: `passing`, `muted: checking tone` while the transmission is still being checked
(at most 800 ms, or 1,600 ms while the list holds a DCS code and a received code is still being confirmed),
`muted: not allowed` for a tone or code the policy rejects (including one heard after a rejection for want of a tone),
`muted: no tone` for an allow list's traffic with none. With no carrier the line shows the policy alone. A terminal
without UTF-8 gets `-` for the dash and `...+N` for the overflow mark. It is absent while no policy is in force, and
wherever tones are not detected. See [Tone filter](cli.md#tone-filter-ctcssdcs-receive-policy).

Audio > `Tone filter... [off]` edits the policy live. Its label reads the policy as the Call Info line does, in the same
lowercase (`Tone filter... [allow 100.0 Hz/D023N]`, and under a scan row with its own policy
`[block 67.0 Hz (row; default allow 100.0 Hz)]`; `...+N` for the overflow mark), and it opens a picker, `Off`,
`Allow list...`, `Block list...` and `Off and clear list`, on the configured mode. Allow and block then prompt for the
list of tones and codes separated by `/` (`67.0/100.0/D023N`), filled in with the configured list. Off keeps the
configured list as it stands when the edit runs (so an edit or a loaded config made just before it is what it keeps;
never a scan row's own), and `Off and clear list` turns the filter off and removes the list, so a save no longer writes
one. The list goes to the decoder as typed, and one it refuses, or an empty list for allow or block, is refused in the
status line with the reason (`Refused: tone filter: entry 2 is not a standard CTCSS tone or DCS code`) and changes
nothing; an earlier edit still waiting to apply is applied all the same. The row edits the configured policy, the one
Config > Save writes: while a scan row with its own policy is on air the row keeps it, and the status line says so
(`Default tone filter -> allow 100.0 Hz; this channel overrides it (block 67.0 Hz)`). The row is always there; set to a
list policy on a session where nothing detects tones, the log says once that it has no effect.

Under the TETRA preset (`-fT`), once a block has decoded, Call Info shows what the Qt/Android monitor shows: the
working mode with the air encryption and every algorithm the network's signalling named (a network does not broadcast
its algorithm: it shows only once a D-OTAR key provide of trunked mode keys, a D-LOCATION UPDATE COMMAND or REJECT, a
repeat proof of an encrypted message, or in direct mode the DMAC-SYNC of the transmission, named one; until then an
encrypted trunked carrier reads `encrypted (TEA not broadcast)`), on an uplink (`--tetra-uplink`) the algorithms its
radios' clear registrations proposed (`TMO control uplink · clear · radios propose TEA1`, a radio's choice, not the
network's confirmation), the network (its colour code in decimal, as the call history names one, with the
location area and the carrier being received once known: the fields a TEA1 reduced key is bound to), the security the
cell broadcasts (its security class or classes, SCKN and authentication, or `no air encryption`) with the loaded key
that applies to the carrier being received, or in direct mode the transmission's security class, where call following
stands while trunking is on, the control and traffic channels, the last MM status and the speech codec's health, each
line only once the decoder has something for it, and under them the latest decoded messages:

```text
| CALL   - TGT: [  100601] SRC: [  100702] Group Encrypted
|        | [Fire Dispatch] SRC: [Engine 21]
| TETRA: TMO downlink · encrypted (TEA1)
| Network: MCC 262 · MNC 1001 · LA 4097 · CC 25 · carrier 654
| Security: class 2 · SCKN 3 · authentication · TEA1 reduced key
| Trunk: traffic channel
| Control: carrier 321 · 392.0125 MHz
| Traffic: carrier 654 · TS 1,3 · 392.1250 MHz
| Codec: ready · 18 frames · 2 errors
| TETRA messages
| SDS ...
```

The live call heads those lines, in the P25/DMR slot rows' columns: the group (or, for a private call, the other
party) and the talker with the call's emergency and priority tags and its kind (`Group`, `Private`, `Data`, or `Voice`
for speech no call set-up has named yet, then `Encrypted` for an encrypted call), then their labels from the group
and radio lists. A party not yet decoded has blank brackets, an ended call stays for 3 seconds, and with no call both
lines stay in place with blank brackets so the lines below do not move:

```text
| CALL   - TGT: [  100601] SRC: [  100702] Group
|        | [Fire Dispatch] SRC: [Engine 21]
```

A terminal without UTF-8 gets ` - ` between the parts. Message text from the air is shown without control
characters, and cut on a character boundary at the screen's width. The `Network:` line names only a network this
carrier proved: a CRC-checked BSCH it sent, or a CRC-checked downlink block descrambled with that network's MCC, MNC
and colour code (every carrier of a cell scrambles with them, so a followed traffic channel names its colour code again
with its first checked block). A `--tetra-net` preset configures descrambling but shows no network until a block
descrambled with it passes its CRC.

## System Identity

Every system identity field in Call Info is what the carrier being decoded sent, in a message that passed its own check
(CRC, FEC or a confirmed transmission): never a Phase 2 seed, a `--tetra-net` preset no checked block proved, a control
channel the trunking state machine returns to, or a value another carrier left. The same record feeds the Qt/Android
monitor and the call history, and every change of carrier (a retune, a `-Y` step, a signal-search step, an input
switch, a trunk-scan target switch) clears it, so a section still showing the previous protocol after a retune prints
no old identity. A part not received yet prints dashes in its column:

```text
| P25p1  - WACN: BEE00 SYS: 348 NAC: 293; RFSS: 3 SITE: 21 Freq: 851.012500 MHz
| P25p2  - WACN: ----- SYS: --- NAC: ---; RFSS: -- SITE: -- Seed: BEE00.348.293 (set) Phase 2 Missing Parameters
| DMR BS - DCC: 01; Capacity Plus; Rest LSN: 03; Freq: 451.000000 MHz
```

- P25: NAC, WACN, SYS, RFSS/SITE and the `Site:` line's LRA as received. A Phase 2 seed set by hand (`-X`, or the
  menu) shows apart as `Seed: WACN.SYS.NAC (set)`; it is configuration, not what the carrier sent. The Phase 2
  parameter warnings still read the seed, as the descrambler does.
- DMR: the trunking brand, Tier III/Connect Plus/XPT site, the Hytera XPT site's free channel (`Free LCN: 4;`) and
  Capacity Plus rest LSN as received (the rest LSN's frequency is the channel map's); a Tier III system code alone shows
  as `SYS: 0A3F;`. A carrier that uses Motorola RAS (Restricted Access to System) shows `RAS;` after the colour code
  once two of its bursts carried the RAS signature, `RAS (-F decodes);` while Relaxed CRC is off, as it is by default:
  its control and data fail the standard CRC, so only `-F` (or the `F` toggle) decodes them. With `-F` on, a scan
  row's own `--strict-crc` shows `RAS (channel sets strict CRC);` instead: that channel keeps strict CRC, and pressing
  `F` would turn `-F` off for the others. The detection is the
  carrier's, so every change of carrier, a trunk follow included, a colour-code change and the DMR reset (`D`) clear
  it, and so does the gap between transmissions.

  ```text
  | DMR BS - DCC: 00; RAS (-F decodes);
  ```
- NXDN: the location (category, system, site) as received; IDAS shows the SCCH area, site code and Type-D system ID.
- EDACS: the site header shows once this carrier named a site, with the control channel LCN it named.
- M17: `CAN: --` until a CRC-checked LSF names one (0 is a CAN).
- `Freq:` is this system's control channel only where a checked message confirmed the carrier as one, at the frequency
  the radio input (or I/Q replay) was tuned to. On audio input from a rigctl peer (`-U`) it is absent: DSD-neo cannot
  tell which frequency the peer's audio comes from, since the peer can be retuned on its own and its audio arrives
  after a tune with no mark where the new frequency's begins.

With the spectrum analyzer on, its legend ends with the same identity in full, led by its protocol, marked
`(last heard)` once it is no longer live, and absent when the carrier sent none:

```text
| System: P25 · NAC 293 · SYS 348 · RFSS 3 · SITE 21 · WACN BEE00 · LRA 1A
```

A terminal without UTF-8 gets ` - ` between the parts.

## Compact View

Press `c` (or use Menu -> Display -> Compact view) to collapse the main screen to a scanner-style
layout. While active, the header shows a `Compact (c)` indicator and the frame renders only:

- the header banner and any transient status toast;
- a condensed `Status` block: decoder mode, demod/symbol rate, tuner Busy/Free (when trunking), SNR meter,
  input level, output mute state, and slot on/off states;
- the full Call Info section (the `Channel:`/`Target:` line while scanning, the `Rx tone:` and `Tone filter:` lines on
  the analog monitor, per-slot TGT/SRC, active channels, tuned frequency, TG HOLD);
- the event history, which expands into the freed rows.

Suppressed while compact: the Input Output section, visual aids (including any enabled visualizers — their
toggles are remembered and restored when leaving compact; switching one on while compact shows a brief
"hidden in compact view" toast), the detailed Audio Decode section, the optional P25 sections, and the
Channels list. The condensed Status block also omits the Voice Error counters and the `Relaxed CRC` decoder
indicator from the full Audio Decode section — leave compact view to inspect those. The setting is
session-only and is not persisted to the config file.

## Event History Rows

While scanning, an event history row names the channel it was heard on: the channel name from a
`-Y` scan list, or the active `--trunk-scan` target's id, in brackets between the row's timestamp
and its protocol token — `2026-04-30 09:12:04 [Fire Dispatch] P25p1 TGT: 00050061; ...`. It answers
the question a bare `TGT: 00000000` row cannot, which is where encrypted traffic was heard. Data
notices (SMS, LRRP, registrations) carry the same prefix. Traffic a service name labels (`--dcc-name 7=Hospital`
and the other [naming options](cli.md#receive-filters-and-service-names)) carries that name in the brackets instead,
scanning or not: `2026-04-30 09:12:04 [Hospital] DMR TGT: ...`. Any other row from a receiver that is not scanning a
named channel renders exactly what it always did, and Short mode (`h`) still drops only the date, so the
prefix survives into the compact row. The channel is resolved once, when the transmission's first
row renders, so a call that outlives a scan step keeps the channel it was actually heard on — and a
call heard on a scan-list row with no name stays unlabelled rather than picking up the name of the
next channel. The bracketed name is for reading, not for parsing: a channel name may itself contain
`]`.

### Scoped scan controls and loaded keys

The Input/Output pane reports loaded Motorola BP, RC4/DES and NXDN/dPMR scrambler keys while no forcing is active,
matching its existing Hytera indication (a scrambler value prints in decimal, an RC4/DES key in hex, and the two
keyring slots print separately when they differ). “Loaded” does not claim successful decryption. Values remain
redacted unless `--show-keys` is enabled. Explicit forcing (BP, RC4 or TYT) replaces these lines with its own status.

Force, CRC, mute and voice-gate controls edit the configured baseline while a scan row constrains their effective
values; menu labels show that baseline, and a CRC toggle under a row's own `-F` or `--strict-crc` says the row
overrides it (`Default relaxed CRC -> On; this channel overrides it (Off)`). These edits are policy, not acquisition:
they apply at once without ending the current call or re-hunting sync. Group-file imports also update the baseline,
with the parked row's group policy restored afterward. Scalar key entry retains its existing temporary behavior. See
the CSV documentation for the `options` column and per-row explicit-off switches.
