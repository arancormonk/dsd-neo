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

- Navigate: arrow keys (`Up`/`Down`)
- Select: `Enter`
- Back/close: `Esc`, `q`
- Scroll long lists: `PageUp`/`PageDown`, `Home`/`End` (where supported by that prompt)

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

### Trunking / scanning

| Key | Action |
|---|---|
| `t` | Toggle trunking |
| `y` | Toggle conventional scanning |
| `C` | Return to control channel (when following a voice channel) |
| `L` | Cycle active trunking channels |
| `w` | Toggle allow/white-list mode (uses imported group list) |
| `u` | Toggle follow private calls |
| `d` | Toggle follow data calls |
| `e` | Toggle follow encrypted calls (P25) |
| `k` / `l` | Set/clear talkgroup hold from the most recent TG (slot-aware) |
| `!` / `@` | Lock out slot 1 / slot 2 (where applicable) |

### Slots & gain

| Key | Action |
|---|---|
| `1` / `2` | Toggle synth/playback for slot 1 / slot 2 |
| `3` | Cycle TDMA slot preference (slot 1 / slot 2 / auto) |
| `+` / `-` | Digital gain up/down |
| `*` / `/` | Analog gain up/down |
| `v` | Cycle input volume multiplier (non-RTL inputs) |

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
