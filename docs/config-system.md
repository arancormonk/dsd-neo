# DSD-neo Configuration System (Design)

This document describes the configuration file system implemented in `dsd-neo`.
It is intended for developers and contributors; user-focused docs can be
derived from this.

The goal is to let users avoid re-entering common options on every run while
preserving strict backward compatibility with existing CLI / environment
workflows.

---

## High-Level Goals

- **Persist user preferences** such as input source, output backend, decode
  mode, trunking basics, and UI behavior.
- **Non-breaking behavior** for existing users:
  - If no config file exists, behavior is identical to current releases.
  - CLI arguments and environment variables always take precedence over
    config file values.
- **Simple and dependency-free** implementation using an INI-style file
  format with a small internal parser.
- **Future-proof**:
  - Config files are versioned.
  - Unknown keys are ignored (silently), so older binaries can safely
    read newer configs and vice versa where possible.

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

Design principles:

- **Whitespace is ignored** around keys, the `=` sign, and values.
- **Case-insensitive booleans** (e.g., `True`, `YES`, `0`).
- **Unknown sections/keys are ignored**; they do not cause load failure.
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
chan_csv = "/path/to/dmr_t3_chan.csv"
group_csv = "/path/to/group.csv"
allow_list = true

[mode]
decode = "auto"             # auto / p25p1 / p25p2 / dmr / nxdn48 / ...
```

---

## Config Discovery and Locations

Config can be provided explicitly or discovered automatically:

### Explicit Path

- CLI: `--config /path/to/config.ini`
- Environment: `DSD_NEO_CONFIG=/path/to/config.ini`

When both are present, CLI wins:

1. `--config PATH`
2. `DSD_NEO_CONFIG`

If the file cannot be read or parsed from an explicit path:

- Log a warning (path + reason).
- Fall back to running **without** config (defaults + env + CLI).

### Default Path

If neither `--config` nor `DSD_NEO_CONFIG` is set, we look for a default
per-user config. Behavior:

- On Unix-like systems:
  - If `$XDG_CONFIG_HOME` is set:
    - `${XDG_CONFIG_HOME}/dsd-neo/config.ini`
  - Else:
    - `${HOME}/.config/dsd-neo/config.ini`
- On Windows:
  - `%APPDATA%\dsd-neo\config.ini`

If the default file exists and is readable, it is loaded. If it does not
exist, we skip config and run as today.

---

## Precedence Rules

The effective options are derived from multiple sources in a strict order:

1. **Built-in defaults**
   - Whatever `initOpts()` / `initState()` currently configure.
2. **Config file**
   - Loaded from explicit or default path.
   - Sets baseline user preferences.
3. **Environment variables**
   - Both the existing DSP/runtime env (parsed in `config.cpp`) and the
     env mapping in `src/runtime/cli/args.c`.
4. **CLI arguments**
   - Short options and long options handled in `dsd_parse_args()`.

Rules:

- Later sources override earlier ones.
- Config never overrides env or CLI; it is strictly lower precedence than
  both, higher only than built-in defaults.
- This preserves all existing usage: any automation that relies on env or
  CLI continues to work exactly as before, regardless of config presence.

---

## Opting Out of Config

Users may want to bypass config entirely, for example when debugging:

- CLI: `--no-config`
- Environment: `DSD_NEO_NO_CONFIG=1`

Behavior:

- If `--no-config` is present, the application **does not attempt to
  load** any config file (explicit or default).
- If `DSD_NEO_NO_CONFIG` is set and no explicit `--config` path is
  provided, config discovery and loading are skipped.
- CLI always overrides environment for config loading: an explicit
  `--config PATH` will be honored even when `DSD_NEO_NO_CONFIG` is set.

---

## Internal Data Structures

To keep concerns separated and to avoid coupling the parser to the
full `dsd_opts`/`dsd_state` types, we use a dedicated struct (implemented
in `include/dsd-neo/runtime/config.h`):

```c
typedef enum {
    DSDCFG_INPUT_UNSET = 0,
    DSDCFG_INPUT_PULSE,
    DSDCFG_INPUT_RTL,
    DSDCFG_INPUT_RTLTCP,
    DSDCFG_INPUT_FILE,
    DSDCFG_INPUT_TCP,
    DSDCFG_INPUT_UDP
} dsdneoUserInputSource;

typedef enum {
    DSDCFG_OUTPUT_UNSET = 0,
    DSDCFG_OUTPUT_PULSE,
    DSDCFG_OUTPUT_NULL
} dsdneoUserOutputBackend;

typedef enum {
    DSDCFG_MODE_UNSET = 0,
    DSDCFG_MODE_AUTO,
    DSDCFG_MODE_P25P1,
    DSDCFG_MODE_P25P2,
    DSDCFG_MODE_DMR,
    DSDCFG_MODE_NXDN48,
    DSDCFG_MODE_NXDN96,
    DSDCFG_MODE_X2TDMA,
    DSDCFG_MODE_YSF,
    DSDCFG_MODE_DSTAR,
    DSDCFG_MODE_EDACS_PV,
    DSDCFG_MODE_DPMR,
    DSDCFG_MODE_M17,
    DSDCFG_MODE_TDMA,
    DSDCFG_MODE_ANALOG
} dsdneoUserDecodeMode;

typedef struct dsdneoUserConfig {
    int version; /* schema version, currently 1 */

    /* [input] */
    int has_input;
    dsdneoUserInputSource input_source;
    char pulse_input[256];
    int rtl_device;
    char rtl_freq[64];
    int rtl_gain;
    int rtl_ppm;
    int rtl_bw_khz;
    int rtl_sql;
    int rtl_volume;
    char rtltcp_host[128];
    int rtltcp_port;
    char file_path[1024];
    int file_sample_rate;
    char tcp_host[128];
    int tcp_port;
    char udp_addr[64];
    int udp_port;

    /* [output] */
    int has_output;
    dsdneoUserOutputBackend output_backend;
    char pulse_output[256];
    int ncurses_ui; /* bool */

    /* [mode] */
    int has_mode;
    dsdneoUserDecodeMode decode_mode;

    /* [trunking] */
    int has_trunking;
    int trunk_enabled;
    char trunk_chan_csv[1024];
    char trunk_group_csv[1024];
    int trunk_use_allow_list;
} dsdneoUserConfig;
```

Notes:

- The struct is intentionally conservative and includes only the subset of
  options that make sense to persist as user preferences.
- DSP-tuning and experimental runtime controls remain in
  `dsdneoRuntimeConfig` (`include/dsd-neo/runtime/config.h`) and continue
  to be configured primarily via environment variables.

---

## Parser and Serializer APIs

Key functions (all shipped in the current tree):

```c
/* Resolve the platform-specific default path (no I/O). */
const char* dsd_user_config_default_path(void);

/* Load config from a given path into cfg.
 * Returns 0 on success, non-zero on error (file missing, unreadable, or
 * parse error). On error, cfg is left zeroed. */
int dsd_user_config_load(const char* path, dsdneoUserConfig* cfg);

/* Atomically write cfg to the given path (for interactive save).
 * Returns 0 on success, non-zero on error. */
int dsd_user_config_save_atomic(const char* path, const dsdneoUserConfig* cfg);
```

Implementation details:

- **Parsing**:
  - Read file line-by-line.
  - Track current section; parse `key = value` pairs.
  - Normalize section names and keys to lowercase for matching.
  - Parse booleans and integers with bounds checking where sensible.
  - On unknown section/key or malformed lines: silently skip and
    continue (best-effort).
- **Writing**:
  - Write to a temp file `config.ini.tmp` in the same directory.
  - `fsync()` if appropriate, then atomically replace the final path
    (`rename()` on POSIX; `MoveFileEx(..., MOVEFILE_REPLACE_EXISTING)`
    on Windows).
  - Ensure parent directory exists; on Unix, use restrictive permissions
    (`0700` for directory, `0600` for file) where possible.

---

## Mapping Config to Runtime Options

We keep a small translation layer between `dsdneoUserConfig` and the
existing runtime types (`dsd_opts`, `dsd_state`):

```c
/* Apply config-derived defaults to opts/state before env + CLI. */
void dsd_apply_user_config_to_opts(const dsdneoUserConfig* cfg,
                                   dsd_opts* opts,
                                   dsd_state* state);

/* Snapshot current opts/state into a user config (for save/print). */
void dsd_snapshot_opts_to_user_config(const dsd_opts* opts,
                                      const dsd_state* state,
                                      dsdneoUserConfig* cfg);
```

### Example: Input Mapping

- For `[input]` section with `source = "rtl"` and associated keys:
  - Build the `opts->audio_in_dev` string in the same format the CLI
    currently produces in `bootstrap_interactive()`:
    - `rtl:dev:freq:gain:ppm:bw:sql:vol`
  - Respect defaults for omitted values (e.g., gain, bandwidth, volume)
    by falling back to the initialized `dsd_opts` fields when a config
    key is missing.

- For `source = "rtltcp"`:
  - Use `rtltcp_host` / `rtltcp_port` for the network endpoint and the
    same `rtl_*` keys (`rtl_freq`, `rtl_gain`, etc.) to seed the tuner
    parameters when present.

- For `source = "pulse"`:
  - Use optional `pulse_source` to capture a specific PulseAudio input
    device; when omitted, keep the default Pulse source. The loader also
    accepts the older `pulse_input` key as an alias and the writer emits
    `pulse_source`.

### Example: Mode Mapping

- For `[mode]` with `decode = "dmr"`:
  - Set the same fields that the interactive prompt sets for `DMR` in
    `bootstrap_interactive()` (frame flags, modulation, output channels,
    etc.).

### Example: Trunking

- For `[trunking]` with `enabled = true`:
  - Set `opts->p25_trunk` / `opts->trunk_enable` as appropriate for the
    selected mode.
  - Copy CSV paths into `opts->chan_in_file` and `opts->group_in_file`.
  - Do **not** automatically import CSVs at config-apply time; instead,
    maintain the current import behavior in the main runtime flow so that
    error handling is consistent.

---

## Startup Flow with Config

The high-level startup sequence (simplified) becomes:

1. **Initialize defaults**
   - `initOpts(&opts);`
   - `initState(&state);`
2. **Honor `--no-config` / `DSD_NEO_NO_CONFIG`**
   - If `--no-config` is present, skip config loading entirely.
   - If `DSD_NEO_NO_CONFIG` is set and no `--config` is provided, skip
     config discovery and loading.
3. **Resolve config path**
   - If `--config PATH`, use that.
   - Else if `DSD_NEO_CONFIG`, use that.
   - Else use `dsd_user_config_default_path()` and check if file exists.
4. **Load config**
   - If path is set and file readable, call `dsd_user_config_load()`.
   - On success: `dsd_apply_user_config_to_opts(&cfg, &opts, &state);`
   - `dsd_user_config_load()` currently treats parse issues as
     non-fatal; as long as the file can be opened, unknown or malformed
     lines are skipped and the function returns success.
5. **Parse CLI (and CLI-related env)**
   - `dsd_parse_args(argc, argv, &opts, &state, &new_argc, &oneshot_rc);`
     - Handles long and short options.
     - Applies env mapping for CLI-related behavior (e.g.
       `DSD_NEO_TCP_AUTOTUNE`).
6. **Run or handle one-shot**
   - If `dsd_parse_args()` returns `DSD_PARSE_ONE_SHOT`, exit with
     `oneshot_rc`.
   - Otherwise, continue into normal decoder runtime.

This preserves existing semantics while inserting config effects early in
the chain.

### File sample rate scaling

When a user config specifies a non-48 kHz file input:

- `[input] source = "file"`, `file_path`, and `file_sample_rate` set.
- If the CLI does not override the sample rate, the startup path:
  - Applies `file_sample_rate` to `opts->wav_sample_rate`.
  - Adjusts `opts->wav_interpolator` and the symbol timing fields in
    `dsd_state` so that demodulation matches the effective sample rate.
- This mirrors the legacy `-s` CLI behavior without requiring users to
  worry about option ordering when using a config file.

---

## Interactive Bootstrap Integration

`apps/dsd-cli/main.c` provides `bootstrap_interactive()` that guides new
users through selecting input, mode, trunking, and UI options.

### Saving Config

At the end of `bootstrap_interactive()`:

- The CLI startup code enables an autosave path whenever config is not
  explicitly disabled and a config path (explicit, env, or default) is
  known.
- `bootstrap_interactive()` calls a helper that snapshots the current
  `dsd_opts`/`dsd_state` into a `dsdneoUserConfig` and saves it to that
  path via `dsd_user_config_save_atomic()`.
- If config was disabled via `--no-config` or `DSD_NEO_NO_CONFIG`, no
  autosave occurs.

### Using Config on Future Runs

- If a config file exists and:
  - No CLI args are provided, **skip** the interactive bootstrap entirely
    and run with config + env + defaults.
  - CLI args are provided, config is still loaded, but CLI can override it.
- CLI:
  - `--interactive-setup` forces running the wizard even if a config
    exists; the resulting setup is automatically saved to the current
    config path.

In addition, when running with CLI arguments but **without** an existing
config file:

- If config is enabled and a default config path can be determined, the
  final effective settings at exit are automatically saved there.
- This makes it easy to:
  - Start with explicit CLI flags,
  - Let `dsd-neo` snapshot them to a config file, and
  - Later run without CLI flags and reuse the same setup.

---

## User-Facing Flags for Config

We extend the CLI surface (implemented in `src/runtime/cli/args.c`) with:

- `--config PATH`
  - Use the given INI file as the config source.
- `--no-config`
  - Disable all config loading.
- `--print-config`
  - Print the **effective** user-config-style representation (as INI) to
    stdout.
  - Implementation:
    - After applying config + env + CLI, snapshot to a temporary
      `dsdneoUserConfig` and render it in INI format using
      `dsd_user_config_render_ini()`.
    - Do not run the decoder; exit after printing.

These flags are additive and do not alter existing option behavior.

---

## Versioning and Compatibility

Config files are explicitly versioned:

- The `version` key at the top of the file indicates the schema version.
- Current implementation stores the version but only supports schema
  `version = 1` and does not enforce or migrate between versions yet.

On load:

- If `version` is missing:
  - The loader defaults `cfg->version` to `1` and proceeds.
- If `version` is present but not `1`:
  - The value is preserved in `cfg->version` but no special handling is
    applied; all known keys are still parsed best-effort.

Unknown keys:

- Silently ignored; they do not cause load failure.
- This allows newer binaries to add config options without breaking older
  installed configs.

---

## Testing Strategy

Tests will live under `tests/runtime` (or a similar area) and should cover:

- Parsing:
  - Minimal valid config.
  - Config with comments, whitespace, unknown keys.
  - Malformed lines (ensure graceful handling).
- Mapping:
  - Given a specific INI config, verify resulting `dsd_opts` and
    `dsd_state` match expectations (e.g., known equivalent of a manual
    CLI invocation or interactive session).
- Precedence:
  - Config vs env vs CLI:
    - e.g., config sets DMR, env sets P25, CLI sets NXDN; verify NXDN
      wins.
- Versioning:
  - Load config with `version = 1` and unknown extra keys.
  - Load config missing `version` and ensure defaulting behavior.

---

## ncurses UI Integration and Live Reload

When running with the ncurses UI enabled (`[output] ncurses_ui = true` or `-N` /
`--ncurses-ui`), the top-level **Config** menu exposes runtime config helpers:
The ncurses UI always runs on its own thread; the legacy synchronous renderer
has been removed.

- `Save Config (Default)`
  - Snapshots the current `dsd_opts`/`dsd_state` into a `dsdneoUserConfig` and
    writes it atomically to `dsd_user_config_default_path()`.
- `Save Config As...`
  - Prompts for a path and writes the current snapshot there.
- `Load Config...`
  - Prompts for an INI path, loads it via `dsd_user_config_load()`, and applies
    it to the running session using the same mapping as startup.

To keep the UI responsive and safe, the apply step is routed through
`UI_CMD_CONFIG_APPLY` on the demod thread. The command takes a full
`dsdneoUserConfig` payload, calls `dsd_apply_user_config_to_opts()` and then
optionally restarts specific backends when they are already active and their
configuration changed.

### Live-reloadable backends

The following inputs / outputs are “live-reloadable” when configs are applied
via `Load Config...`:

- **PulseAudio input** (`[input] source = "pulse"`)
  - When Pulse input is already active (`audio_in_type == 0`) and the
    `audio_in_dev` string changes (e.g., `pulse:source1` → `pulse:source2`), the
    UI:
    - Closes the current Pulse input (`closePulseInput()`),
    - Updates `opts->pa_input_idx` from the new suffix, and
    - Reopens the stream (`openPulseInput()`).
- **PulseAudio output** (`[output] backend = "pulse"`)
  - When Pulse output is already active (`audio_out_type == 0`) and
    `audio_out_dev` changes (`pulse` or `pulse:sink`), the UI:
    - Closes the current Pulse output (`closePulseOutput()`),
    - Updates `opts->pa_output_idx`, and
    - Reopens (`openPulseOutput()`).
- **RTL dongle input** (`[input] source = "rtl"`)
  - When the RTL pipeline is already active (`audio_in_type == 3`) and
    `audio_in_dev` changes, the UI:
    - Updates `opts->rtl_dev_index`, tuner frequency (parsed from
      `rtl_freq` with K/M/G suffix support), gain, PPM, bandwidth, squelch, and
      volume from the config (falling back to existing defaults when omitted),
      and
    - Calls `svc_rtl_restart()` to rebuild and retune the RTL stream.
- **RTLTCP input** (`[input] source = "rtltcp"`)
  - When RTLTCP is already in use (`audio_in_type == 3` with `rtltcp_enabled`
    set) and the config changes host/port/tuner fields, the UI:
    - Updates `opts->rtltcp_hostname`/`opts->rtltcp_portno` and the same
      `rtl_*` tuner parameters as above, and
    - Calls `svc_rtl_restart()` to reconnect the RTLTCP-based RTL stream with
      the new settings.
- **TCP direct PCM input** (`[input] source = "tcp"`)
  - When TCP direct input is already active (`audio_in_type == 8` and
    `audio_in_dev` starts with `tcp:`) and the config changes host/port, the UI:
    - Updates `opts->tcp_hostname`/`opts->tcp_portno`,
    - Closes any existing `tcp_file_in` and socket, and
    - Reconnects using `svc_tcp_connect_audio()` (non-fatal on failure).
- **UDP direct PCM input** (`[input] source = "udp"`)
  - When UDP input is already active (`audio_in_type == 6` and
    `audio_in_dev` starts with `udp:`) and the config changes bind address or
    port, the UI:
    - Updates `opts->udp_in_bindaddr` and `opts->udp_in_portno`,
    - Stops the existing UDP backend via `udp_input_stop()`, and
    - Restarts it with `udp_input_start()` at the new address/port.
- **Simple file input** (`[input] source = "file"`)
  - When libsndfile-based file input is already active (`audio_in_type == 2`)
    and `audio_in_dev` changes, the UI:
    - Closes the current file and frees the `SF_INFO`,
    - Recreates `SF_INFO` using the current `opts->wav_sample_rate`, and
    - Reopens the path via `sf_open(...)` (logging errors instead of exiting
      on failure).

For all of the above, the backend type must already match (e.g., loading a
`source = "udp"` config while currently using Pulse input does **not** attempt
to hot-switch to UDP mid-run). Changes that alter the input/output *type* rather
than just its parameters are guaranteed to take full effect on the next program
start, not necessarily when reloading in-place.

### Non-reloadable / restart-only changes

Some config changes are only guaranteed to fully apply on the next launch:

- Switching between fundamentally different input backends (e.g.
  Pulse → RTL, file → UDP).
- Changes that rely on the broader startup path (e.g. CLI sequencing, one-shot
  flows, environment-driven heuristics).

In these cases, `Load Config...` will still update `opts`/`state` so saves or
`--print-config` reflect the new config, but the underlying I/O stack may not
be completely rebuilt until the process is restarted.

## Summary

- The config system is INI-based, with clearly defined precedence and
  platform-specific discovery.
- It introduces a dedicated `dsdneoUserConfig` structure and helper APIs
  to decouple parsing from runtime internals.
- Interactive bootstrap can optionally persist user choices to config,
  making `dsd-neo` friendlier for new users without impacting existing
  scripted or environment-driven setups.
