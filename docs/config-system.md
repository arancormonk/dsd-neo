# DSD-neo Configuration System

This document describes the configuration file system in `dsd-neo`, covering
file format, options, profiles, validation, and CLI integration.

The goal is to let users avoid re-entering common options on every run while
preserving strict backward compatibility with existing CLI / environment
workflows.

---

## Goals

- **Persist user preferences** such as input source, output backend, decode
  mode, trunking basics, and UI behavior.
- **Non-breaking behavior** for existing users:
  - If no config file exists, behavior is identical to current releases.
  - CLI arguments and environment variables always take precedence over
    config file values.
- **Simple INI-style format** that's easy to read and edit.
- **Future-proof**: Config files are versioned; unknown keys generate
  warnings but do not prevent loading.
- **Validation** with helpful diagnostics including line numbers.
- **User experience enhancements**:
  - Template generation (`--dump-config-template`).
  - Path expansion (`~`, `$VAR`, `${VAR}`).
  - Profile support for switching between configurations.
  - Include directive for modular configs.

---

## Config File Format

We use a minimal INI-style format:

- Sections: `[section-name]`
- Key-value pairs: `key = value`
- Comments: lines starting with `#` or `;`
- Values:
  - Strings (unquoted or double-quoted).
  - Integers (parsed with `strtol`).
  - Booleans (`true` / `false`, `yes` / `no`, `on` / `off`, `1` / `0`).
  - Paths (support `~` and `$VAR` expansion).
  - Frequencies (support K/M/G suffix, e.g., `851.375M`).

Design principles:

- **Whitespace is ignored** around keys, the `=` sign, and values.
- **Case-insensitive booleans** (e.g., `True`, `YES`, `0`).
- **Unknown sections/keys generate warnings** but do not cause load failure.
- A top-level `version` key defines the config schema version.

Example:

```ini
version = 1

[input]
source = "rtl"              # pulse / rtl / rtltcp / file / tcp / udp
rtl_device = 0
rtl_freq = "851.375M"       # supports K/M/G suffix or raw Hz

[output]
backend = "pulse"           # pulse / null / (future output types)
ncurses_ui = true           # map to -N / use_ncurses_terminal

[trunking]
enabled = true
chan_csv = "~/dsd-neo/dmr_t3_chan.csv"   # path expansion supported
group_csv = "$HOME/dsd-neo/group.csv"
allow_list = true

[mode]
decode = "auto"             # auto / p25p1 / p25p2 / dmr / nxdn48 / ...
```

---

## Path Expansion

Configuration values of type `PATH` support shell-like expansion:

- `~` expands to the user's home directory (`$HOME` on Unix,
  `%USERPROFILE%` on Windows).
- `$VAR` expands to environment variable `VAR`.
- `${VAR}` expands to environment variable `VAR` (braced form).
- Missing variables expand to empty string (no error).

Path expansion is applied to:
- `[input] file_path`
- `[trunking] chan_csv`
- `[trunking] group_csv`
- Include directive paths

---

## Profile Support

Profiles allow defining multiple named configurations in a single file.
Use `--profile NAME` to activate a specific profile.

### Syntax

Profiles are defined using `[profile.NAME]` sections with dotted key syntax:

```ini
version = 1

[input]
source = "pulse"

[mode]
decode = "auto"

[trunking]
enabled = false

[profile.p25_trunk]
# Dotted syntax: section.key = value
mode.decode = "p25p1"
trunking.enabled = true
trunking.chan_csv = "~/p25/channels.csv"

[profile.local_dmr]
input.source = "rtl"
input.rtl_device = 0
input.rtl_freq = "446.5M"
input.rtl_gain = 30
mode.decode = "dmr"

[profile.scanner]
mode.decode = "auto"
output.ncurses_ui = true
```

### Usage

```bash
# Load base config
dsd-neo --config config.ini

# Load base config with profile overlay
dsd-neo --config config.ini --profile p25_trunk

# List available profiles
dsd-neo --config config.ini --list-profiles
```

### Behavior

- Base config is loaded first.
- Profile keys override base config values.
- If `--profile NAME` is specified but profile doesn't exist, an error is
  returned.
- Profile sections do not affect base loading (they are skipped unless
  `--profile` is specified).

---

## Include Directive

Config files can include other files using the `include` directive:

```ini
# Main config file
include = "/etc/dsd-neo/system.ini"
include = "~/.config/dsd-neo/local.ini"

version = 1

[input]
source = "rtl"  # overrides anything from includes
```

### Rules

- Include directives must appear **before** any section headers.
- Paths support expansion (`~`, `$VAR`, `${VAR}`).
- Maximum include depth: 3 (to prevent infinite recursion).
- Circular includes are detected and silently skipped.
- Included files are processed first; main file values override includes.
- Profile sections in included files are ignored (profiles are only read
  from the main config file).

---

## Config Discovery and Locations

Config loading is **opt-in**. By default, no config file is loaded unless
explicitly requested.

### Enabling Config

- CLI: `--config` (uses default path) or `--config /path/to/config.ini`
- Environment: `DSD_NEO_CONFIG=/path/to/config.ini`

When both are present, CLI path wins:

1. `--config PATH` (if path provided)
2. `DSD_NEO_CONFIG`
3. Default path (if `--config` without path)

If the file cannot be read or parsed:

- Log a warning (path + reason).
- Fall back to running **without** config (defaults + env + CLI).

### Default Path

When `--config` is passed without a path argument, the default per-user
config location is used:

- On Unix-like systems:
  - If `$XDG_CONFIG_HOME` is set:
    - `${XDG_CONFIG_HOME}/dsd-neo/config.ini`
  - Else:
    - `${HOME}/.config/dsd-neo/config.ini`
- On Windows:
  - `%APPDATA%\dsd-neo\config.ini`

If the default file does not exist, it will be created when settings are
saved (e.g., after running the interactive setup wizard).

---

## Precedence Rules

Options are derived from multiple sources, with later sources overriding
earlier ones:

1. **Built-in defaults**
2. **Config file** (explicit or default path)
3. **Environment variables**
4. **CLI arguments** (highest priority)

This ensures:
- Config never overrides env or CLI.
- Existing scripts and automation work unchanged.
- Config provides convenient defaults that can be overridden when needed.

---

## Configuration Reference

**[input] section:**
| Key | Type | Description | Default |
|-----|------|-------------|---------|
| `source` | ENUM | Input source type | `pulse` |
| `pulse_source` | STRING | PulseAudio source device | (empty) |
| `rtl_device` | INT (0-255) | RTL-SDR device index | `0` |
| `rtl_freq` | FREQ | RTL-SDR frequency | `850M` |
| `rtl_gain` | INT (0-49) | RTL-SDR gain in dB | `0` |
| `rtl_ppm` | INT (-1000-1000) | Frequency correction | `0` |
| `rtl_bw_khz` | INT (6-48) | DSP bandwidth | `48` |
| `rtl_sql` | INT (-100-0) | Squelch level | `0` |
| `rtl_volume` | INT (1-10) | Volume multiplier | `2` |
| `rtltcp_host` | STRING | RTL-TCP hostname | `127.0.0.1` |
| `rtltcp_port` | INT (1-65535) | RTL-TCP port | `1234` |
| `file_path` | PATH | Input file path | (empty) |
| `file_sample_rate` | INT (8000-192000) | File sample rate | `48000` |
| `tcp_host` | STRING | TCP direct host | `127.0.0.1` |
| `tcp_port` | INT (1-65535) | TCP direct port | `7355` |
| `udp_addr` | STRING | UDP bind address | `127.0.0.1` |
| `udp_port` | INT (1-65535) | UDP port | `7355` |

**[output] section:**
| Key | Type | Description | Default |
|-----|------|-------------|---------|
| `backend` | ENUM | Audio output backend | `pulse` |
| `pulse_sink` | STRING | PulseAudio sink device | (empty) |
| `ncurses_ui` | BOOL | Enable ncurses UI | `false` |

**[mode] section:**
| Key | Type | Description | Default |
|-----|------|-------------|---------|
| `decode` | ENUM | Decode mode preset | `auto` |

**[trunking] section:**
| Key | Type | Description | Default |
|-----|------|-------------|---------|
| `enabled` | BOOL | Enable trunking | `false` |
| `chan_csv` | PATH | Channel map CSV | (empty) |
| `group_csv` | PATH | Group list CSV | (empty) |
| `allow_list` | BOOL | Use as allow list | `false` |

---

## Validation

The config system validates files and reports issues with line numbers:

- **Error**: Invalid enum value, type mismatch, parse failure
- **Warning**: Unknown key or section, integer out of range
- **Info**: Deprecated key usage (key still works)

```bash
# Validate a config file
dsd-neo --validate-config /path/to/config.ini

# Validate with strict mode (warnings become errors)
dsd-neo --validate-config /path/to/config.ini --strict-config
```

Exit codes:
- `0`: No errors (may have warnings)
- `1`: Errors present
- `2`: Only warnings (strict mode only)

Diagnostics are only printed when you run `--validate-config`; normal startup is
silent unless the config file cannot be opened.

---

## Template Generation

Generate a commented config template with all options:

```bash
dsd-neo --dump-config-template > config.ini
```

Output format:

```ini
# DSD-neo configuration template
# Generated by: dsd-neo --dump-config-template
#
# Uncomment and modify values as needed.
# Lines starting with # are comments.
#
# Precedence: CLI arguments > environment variables > config file > defaults

version = 1

[input]
# Input source type
# Allowed: pulse|rtl|rtltcp|file|tcp|udp
# source = "pulse"

# RTL-SDR device index (0-based)
# Range: 0 to 255
# rtl_device = 0

# RTL-SDR frequency (supports K/M/G suffix)
# Frequency (supports K/M/G suffix)
# rtl_freq = "851.375M"
...
```

---

## Notes on Input Sources

- **RTL-SDR (`source = "rtl"`)**: Uses the `rtl_*` keys for frequency,
  gain, PPM correction, bandwidth, squelch, and volume. Omitted values
  use sensible defaults.

- **RTL-TCP (`source = "rtltcp"`)**: Uses `rtltcp_host`/`rtltcp_port`
  for the network endpoint, plus the same `rtl_*` tuning keys.

- **PulseAudio (`source = "pulse"`)**: Use `pulse_source` to specify
  a particular input device. The older `pulse_input` key is accepted
  as an alias.

- **UDP (`source = "udp"`)**: Provide both `udp_addr` and `udp_port`; leaving
  them empty will not switch the input to UDP.

### Decode Modes

The `decode` key in `[mode]` configures the frame types and modulation.
Supported values: `auto`, `p25p1`, `p25p2`, `dmr`, `nxdn48`, `nxdn96`,
`x2tdma`, `ysf`, `dstar`, `edacs_pv`, `dpmr`, `m17`, `tdma`, `analog`.

If you choose RTL/RTLTCP input and omit specific tuning fields, DSD-neo falls
back to its built-in RTL defaults: center frequency 850 MHz, DSP bandwidth
48 kHz, and volume multiplier 2. The template still shows 851.375M as the
example frequency.

### Trunking

When `[trunking] enabled = true`:

- Trunking is activated for the selected mode.
- CSV paths (`chan_csv`, `group_csv`) are passed to the decoder.
- CSV files are not auto-imported just because they are listed in the config.
  They load only when triggered by CLI flags (`-C`/`-G`) or the interactive
  wizard.

---

## Startup Behavior

1. Config is loaded early, before CLI argument parsing.
2. CLI arguments and environment variables override config values.
3. One-shot commands (`--dump-config-template`, `--validate-config`,
   `--list-profiles`, `--print-config`) execute and exit immediately.
4. If no CLI args and no config file exists, the interactive bootstrap
   wizard runs.
5. When config exists: interactive bootstrap is skipped unless
   `--interactive-setup` is specified.

### File Sample Rate

When using `source = "file"` with a non-48 kHz sample rate:

- Set `file_sample_rate` to match your input file.
- Symbol timing is adjusted automatically.

---

## Interactive Bootstrap

The interactive bootstrap wizard (`--interactive-setup`) guides you
through selecting input, mode, trunking, and UI options.

### Auto-Saving

- When the wizard completes, settings are automatically saved to the
  config path (default or explicit).
- Auto-save only occurs when config is enabled (`--config` or `DSD_NEO_CONFIG`).
- Outside the wizard, the app also auto-saves the effective settings at exit
  whenever a config path is in use. To avoid having your file rewritten,
  simply run without the `--config` flag.

### Config and CLI Interaction

- If a config file exists and no CLI args are provided: skip the wizard,
  use config settings.
- If CLI args are provided: config is loaded first, then CLI overrides it.
- CLI:
  - `--interactive-setup` forces running the wizard even if a config
    exists; the resulting setup is automatically saved to the current
    config path.

---

## CLI Flags Reference

### Config Loading

| Flag | Description |
|------|-------------|
| `--config [PATH]` | Enable config loading; optionally specify path (uses default if omitted) |
| `--print-config` | Print effective config (as INI) and exit |

### Template and Validation

| Flag | Description |
|------|-------------|
| `--dump-config-template` | Print commented template with all options |
| `--validate-config [PATH]` | Validate config and report diagnostics |
| `--strict-config` | Treat warnings as errors (with `--validate-config`) |

### Profiles

| Flag | Description |
|------|-------------|
| `--profile NAME` | Load named profile (overrides base config) |
| `--list-profiles` | List all profile names in config file |

### Bootstrap

| Flag | Description |
|------|-------------|
| `--interactive-setup` | Force interactive wizard even if config exists |

---

## Versioning and Compatibility

- Config files use `version = 1` at the top.
- If `version` is missing, defaults to 1.
- Unknown keys generate warnings but don't prevent loading.
- This allows newer binaries to add options without breaking older configs.

---

## ncurses UI Config Menu

When running with the ncurses UI (`ncurses_ui = true` or `-N`), the
**Config** menu provides:

- **Save Config (Default)**: Save current settings to the default config path.
- **Save Config As...**: Save to a custom path.
- **Load Config...**: Load a config file and apply it to the running session.

### Live Reload

The following can be changed without restarting:

- PulseAudio input/output device
- RTL-SDR and RTLTCP tuning parameters (frequency, gain, PPM, etc.)
- TCP/UDP connection parameters
- File input path

Switching between different input types (e.g., Pulse → RTL) requires a
restart to take full effect.

---

## Summary

- INI-based config with clear precedence (CLI > env > config > defaults).
- Platform-specific default paths with automatic discovery.
- Validation with line-number diagnostics.
- Template generation for discoverability.
- Path expansion (`~`, `$VAR`, `${VAR}`) for portability.
- Profile support for switching configurations.
- Include directive for modular configs.
- Interactive bootstrap can persist user choices automatically.
