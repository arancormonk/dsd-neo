# Single-Tuner Trunk Scan

Single-tuner trunk scan lets one retunable receiver rotate across several explicit targets instead of staying on one
system. Use it when you want one DSD-neo instance to check a small set of P25 trunk, DMR trunk, and one-frequency DMR
channels, but you do not have a separate receiver for each system.

The scan coordinator parks on one target, watches for activity, and moves to the next idle target after the configured
dwell time. Trunking state and per-target channel maps are kept separate, so a channel number or learned control-channel
state from one system is not reused on another.

## Requirements

- A retuning path is required:
  - Direct RTL-family input opened by DSD-neo, such as `-i rtl:...`, `-i rtltcp:...`, or SoapySDR through the radio
    input path.
  - Or an external tuner controlled by rigctl, usually with TCP PCM input: `-i tcp -U 4532`.
- IQ replay cannot be used with trunk scan because replay timelines cannot retune to unrelated live frequencies.
- Legacy conventional scanner mode (`-Y`) cannot be combined with trunk scan.
- Global channel maps (`-C` or `[trunking] chan_csv`) cannot be used while trunk scan is active. Put each trunk target's
  optional channel map in the target CSV `chan_csv` column instead.
- A group list (`-G` or `[trunking] group_csv`) is still global and applies to every target.

## Target CSV

Start with a target list CSV. The header must begin with this exact prefix:

```csv
id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes
```

Example:

```csv
id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes
county-p25,p25-trunk,851012500,,3000,,P25 control channel
city-dmr,dmr-trunk,456318750,dmr_t3_chan.csv,3000,,DMR Tier III control channel
plant-dmr,dmr-conventional,461112500,,1500,1200,one-frequency DMR
```

The repository includes a starter file at `examples/trunk_scan_targets.csv`.

Column behavior:

| Column | Required | Meaning |
|--------|----------|---------|
| `id` | Yes | Unique short name used in log messages. Keep it under 64 bytes. |
| `type` | Yes | `p25-trunk`, `dmr-trunk`, or `dmr-conventional`. |
| `frequency_hz` | Yes | Initial park/control frequency in decimal Hz. Suffixes such as `M` are not accepted in CSV. |
| `chan_csv` | No | Channel map for a trunk target. Paths are resolved relative to the target CSV file. Leave empty for conventional DMR. |
| `dwell_ms` | No | Idle dwell for this target. Empty uses the CLI/config default. Valid range: `250..600000`. |
| `activity_hold_ms` | No | Conventional DMR activity hold for this target. Empty uses the CLI/config default. Valid range: `250..600000`. |
| `notes` | No | Ignored by DSD-neo. Use it for local notes. |

Target list limits and validation:

- Maximum 32 targets.
- Blank rows are skipped.
- Every data row must contain the seven fields above.
- The header may have extra columns after `notes`, but the first seven header names must match the required prefix.
- Frequency values must be at least `1`. Normal 64-bit builds accept values up to `4294967295`; 32-bit builds may reject
  values above `LONG_MAX`.
- Duplicate `id` values are rejected.
- Duplicate `(type, frequency_hz)` pairs are rejected.
- `chan_csv` is only valid for `p25-trunk` and `dmr-trunk` targets.
- The parser is intentionally small. It can handle a quoted `chan_csv` that contains a comma, but it is not a full CSV
  parser and does not support escaped quotes.

## CLI Usage

For a mixed P25/DMR scan with an RTL-SDR:

```sh
dsd-neo -ft -i rtl:0:851.0125M:22:0:48:0:2 --trunk-scan examples/trunk_scan_targets.csv -G examples/group.csv -N
```

For an external receiver that sends PCM audio over TCP and is tuned through rigctl:

```sh
dsd-neo -ft -i tcp -U 4532 --trunk-scan ~/radio/trunk_scan_targets.csv -G ~/radio/group.csv -N
```

Optional timing controls:

```sh
dsd-neo -ft -i rtl:0:851.0125M:22:0:48:0:2 \
  --trunk-scan ~/radio/trunk_scan_targets.csv \
  --trunk-scan-dwell-ms 5000 \
  --trunk-scan-activity-hold-ms 2000 \
  -N
```

- `--trunk-scan-dwell-ms <ms>` sets the default idle dwell for targets whose `dwell_ms` column is empty. Default:
  `3000`.
- `--trunk-scan-activity-hold-ms <ms>` sets the default hold time after allowed conventional DMR activity. Default:
  `1200`.
- Per-target CSV values override these defaults.

Use `-ft` or an equivalent config mode (`mode.decode = "tdma"`) for mixed P25 and DMR scan lists. Narrower modes can be
used when every target is the same protocol.

## Config Usage

`[trunk_scan]` can enable the same feature from a config file:

```ini
version = 1

[input]
source = "rtl"
rtl_device = 0
rtl_freq = "851.0125M"
rtl_gain = 22
rtl_ppm = 0
rtl_bw_khz = 48

[mode]
decode = "tdma"

[output]
ncurses_ui = true

[trunking]
group_csv = "~/radio/group.csv"
allow_list = false
tune_enc_calls = true

[trunk_scan]
enabled = true
targets_csv = "~/radio/trunk_scan_targets.csv"
idle_dwell_ms = 3000
activity_hold_ms = 1200
```

Validate the config before using it:

```sh
dsd-neo --validate-config ~/radio/config.ini
```

Config notes:

- `targets_csv` supports the same path expansion as other config paths (`~`, `$VAR`, and `${VAR}`).
- `targets_csv` is required when `enabled = true`.
- `[trunking] chan_csv` is rejected when trunk scan is enabled.
- Profiles can enable trunk scan. A profile may inherit `trunk_scan.targets_csv` from the base config.
- If trunk scan is inherited from a config file, one-off CLI arguments that select another input, mode, channel map,
  file/replay input, trunking mode, or legacy scan mode disable the inherited scan for that run. UI-only flags and
  trunk-scan timing overrides keep it enabled.

## Runtime Behavior

At startup DSD-neo:

1. Loads the global group list, if one is configured.
2. Loads the target CSV.
3. Imports each target's `chan_csv` into that target's isolated state.
4. Tunes the first target.

During scanning:

- Idle targets rotate after their dwell time.
- P25 and DMR trunk targets stay parked while their trunking state machine is following an active call.
- Conventional DMR targets stay parked only after allowed activity is decoded. The allow/block list, private-call
  tuning, data-call tuning, and encrypted-call tuning controls all apply to that decision.
- When a retune fails, DSD-neo logs a warning, briefly cools that target down, and tries another eligible target.

Expected log messages include:

```text
Trunk scan target 'county-p25' at 851012500 Hz
Trunk scan enabled with 3 targets
Trunk scan target 'city-dmr' retune failed; cooling down briefly
```

## Troubleshooting

`--trunk-scan requires a target CSV path`

Set `--trunk-scan <path>` on the CLI, or set both `enabled = true` and `targets_csv = "..."`
in `[trunk_scan]`.

`trunk scan target CSV header must start with ...`

The first line must begin with `id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes`.

`row N has invalid frequency_hz`

Use decimal Hz only, for example `851012500`. Frequency suffixes such as `851.0125M` are valid in CLI/config tuning
fields, but not in the target CSV.

`--trunk-scan cannot be combined with global -C/channel-map config`

Move channel maps into the target CSV `chan_csv` column. Conventional DMR rows must leave `chan_csv` empty.

`--trunk-scan requires an open RTL input or rigctl tuning`

Use an RTL-family radio input, or add rigctl control for an external receiver. File, PulseAudio, UDP, TCP, and stdin
inputs cannot scan by themselves because DSD-neo has no tuner to retune.

`--trunk-scan cannot use IQ replay input because replay cannot retune`

Run trunk scan against live input. IQ replay remains useful for testing one captured control/voice path, but not for
rotating across unrelated scan targets.

## Limitations

- Only P25 trunk, DMR trunk, and one-frequency DMR conventional targets are supported.
- There is one active receiver. Traffic on targets that are not currently parked can be missed.
- Group policy is global across all scan targets.
- Target CSV files are simple comma-delimited files, not full RFC 4180 CSV.
- The feature is not a replacement for a multi-receiver trunking setup when missed calls are unacceptable.

## Migrating From Existing Trunking Runs

Existing single-system trunking commands still work:

```sh
dsd-neo -ft -i rtl:0:851.0125M:22:0:48:0:2 -T -C dmr_t3_chan.csv -G group.csv -N
```

To scan multiple systems with one receiver:

1. Keep `-G group.csv` if you want the same labels and allow/block policy for all targets.
2. Remove global `-C` or `[trunking] chan_csv`.
3. Put each trunk system's channel map in the target CSV `chan_csv` column.
4. Replace `-T` with `--trunk-scan targets.csv`.

Legacy `-Y` conventional scanning remains a separate mode for fast conventional sync scanning and is mutually exclusive
with trunk scan.
