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

## DSP Status

The DSP status panel shows RTL DSP loop state when RTL input support is available. In CQPSK mode, the CMA equalizer
lines report:

- `CMA EQ`: equalizer state (`Off`, `Init`, `Warm`, `Run`), tap count, adaptation step, and adapted symbol count.
- `CMA Metric`: output magnitude squared (`mag2`), target modulus (`tgt`), constant-modulus error (`err`), largest
  non-center tap (`side`), and total tap energy (`E`).

For a useful equalizer, `syms` should increase, `mag2` should settle near `tgt`, `err` should settle lower after
acquisition, and `side` should become non-zero when the equalizer is learning multipath/ISI correction.

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
