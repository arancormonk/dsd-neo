# Terminal UI (ncurses) Guide

This guide covers the interactive terminal UI enabled with `-N`: how to open the menu overlay and the most useful
hotkeys.

For CLI flags and inputs/outputs, see `docs/cli.md`.

## Start the UI

- Enable UI: `dsd-neo -N ...`
- Quit: `q`
- Open the menu overlay: `Enter`

Tip: Many screens print a short hotkey hint line in the footer while you’re running.

## Menu Overlay

Press `Enter` to open the nonblocking menu overlay. While it is open, keypresses go to the menu (hotkeys are not
processed).

Common controls:

- Move selection: arrow keys (`Up`/`Down`), `PageUp`/`PageDown`, `Home`/`End`
- Select / open submenu: `Enter`, `Right`
- Back / close: `Esc`, `q`/`Q`, `Left`
- Item help: `h`

Group policy reload:

- In the Trunking/Import menu path, importing a group list (`-G` CSV) performs a full policy reload. On parse failure,
  the currently loaded list remains active.

Config profiles:

- In the Config menu, `Load Profile...` lists `[profile.NAME]` sections from the active config path, or the default
  config path when no config has been loaded yet. Loading a profile applies it to the running session and disables
  autosave, matching CLI `--profile NAME` behavior.

## DSP Status

The DSP status panel shows RTL DSP loop state when RTL input support is available. CQPSK mode reports FLL,
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

The default RTL input line shows the SQL threshold but does not duplicate channel power. Enable the DSP panel when you
need to inspect post-channel-filter squelch power. `RF Level` and `Squelch` are measured at different stages and are not
expected to match exactly.

The low-level threshold is controlled by `--input-level-warn-db` or `DSD_NEO_INPUT_WARN_DB` and defaults to `-40 dBFS`.
Hot/clipping advisories use fixed v1 thresholds: peak at or above `-1.0 dBFS` is `HOT`, and at least `0.1%` clipped or
near-rail samples is `CLIP`. Footer messages are rate-limited. RF low-level status remains persistent but does not
produce repeated low-level footer messages because a quiet channel and too little RF gain are not reliably
distinguishable from raw receiver samples alone.

## Hotkeys (Main Screen)

Keys are case-sensitive. Some commands only make sense in specific modes (for example, trunking controls require
trunking/scanner to be enabled, and RTL controls require RTL input).

### General

| Key | Action |
|---|---|
| `q` | Quit |
| `c` | Toggle compact view |
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
| `L` | Cycle active trunking channels |
| `g` | Toggle follow group calls |
| `w` | Toggle allow/white-list mode (uses imported group list) |
| `u` | Toggle follow private calls |
| `d` | Toggle follow data calls |
| `e` | Toggle follow encrypted calls (P25) |
| `k` / `l` | Set/clear talkgroup hold from the most recent TG (slot-aware) |
| `!` / `@` | Lock out slot 1 / slot 2 (where applicable) |

### Slots, gain & privacy

| Key | Action |
|---|---|
| `1` / `2` | Toggle synth/playback for slot 1 / slot 2 |
| `3` | Cycle TDMA slot preference (slot 1 / slot 2 / auto) |
| `+` / `-` | Digital gain up/down |
| `*` / `/` | Analog gain up/down |
| `v` | Cycle input volume multiplier (non-RTL inputs) |
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
| `n` | Toggle constellation normalization |
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
| `M` | Toggle P25 Phase 2 modulation helper |
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
