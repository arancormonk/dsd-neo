# Single-Tuner Trunk Scan

Single-tuner trunk scan lets one retunable receiver rotate across several explicit targets instead of staying on one
system. Use it when you want one DSD-neo instance to check a small set of P25 trunk, DMR trunk, DMR conventional,
and NXDN (trunk, NXDN96 conventional, and NXDN48 conventional) targets, but you do not have a separate receiver for
each system.

The scan coordinator parks on one target, watches for activity, and moves to the next idle target after the configured
dwell time. Trunking state and per-target channel maps are kept separate, so a channel number or learned control-channel
state from one system is not reused on another.

## Requirements

- A retuning path is required:
  - Direct RTL-family input opened by DSD-neo, such as `-i rtl:...`, `-i rtltcp:...`, or SoapySDR through the radio
    input path.
  - Or an external tuner controlled by rigctl, usually with TCP PCM input: `-i tcp -U 4532`.
- IQ replay cannot be used with trunk scan because replay timelines cannot retune to unrelated live frequencies.
- Conventional scanner mode (`-Y`) cannot be combined with trunk scan.
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
id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation,rtl_gain
county-p25,p25-trunk,851012500,,3000,,P25 control channel,cqpsk,18
city-dmr,dmr-trunk,456318750,dmr_t3_chan.csv,3000,,DMR Tier III control channel,auto,
plant-dmr,dmr-conventional,461112500,,1500,1200,one-frequency DMR,gfsk,auto
site-nxdn,nxdn-trunk,461037500,,3000,,NXDN Type-C control channel,auto,
field-nxdn,nxdn-conventional,461550000,,1500,1200,one-frequency NXDN96,gfsk,
field-nxdn48,nxdn48-conventional,461556250,,1500,1200,one-frequency NXDN48 (6.25 kHz),gfsk,
```

The repository includes a starter file at `examples/trunk_scan_targets.csv`.

Column behavior:

| Column | Required | Meaning |
|--------|----------|---------|
| `id` | Yes | Unique short name shown in the terminal status row and Call Info, as the `[id]` prefix on event-history rows, `-J` log lines and the rdio `talkgroup_tag` fallback, and in log messages. Keep it under 64 bytes. |
| `type` | Yes | `p25-trunk`, `dmr-trunk`, `dmr-conventional`, `nxdn-trunk`, `nxdn-conventional` (NXDN96, 12.5 kHz), or `nxdn48-conventional` (NXDN48, 6.25 kHz). |
| `frequency_hz` | Yes | Initial park/control frequency in decimal Hz. Suffixes such as `M` are not accepted in CSV. |
| `chan_csv` | No | Channel map for a trunk target. Paths are resolved relative to the target CSV file. Leave empty for conventional DMR and both conventional NXDN types. |
| `dwell_ms` | No | Idle dwell for this target. Empty uses the CLI/config default. Valid range: `250..600000`. |
| `activity_hold_ms` | No | Conventional DMR/NXDN (NXDN96 and NXDN48) activity hold for this target. Empty uses the CLI/config default. Valid range: `250..600000`. |
| `notes` | No | Ignored by DSD-neo. Use it for local notes. |
| `modulation` | No | Demod hint for this target. Empty preserves global/default handling. `auto` uses target defaults even when a global `-m` lock is set. P25 accepts `auto`, `c4fm`, `cqpsk`; DMR and both NXDN rates accept `auto`, `gfsk`. |
| `rtl_gain` | No | RTL-family tuner gain for this target. Empty uses the global/default gain. `0` or `auto` requests device automatic gain. `1..49` requests manual dB gain. |

A target's `chan_csv` may carry the optional `name` column described in [csv-formats.md](csv-formats.md), and it
parses, but trunk scan discards the names. Each target's channel map is parked in a per-target snapshot that carries
the frequencies positionally and no names, so a name kept from one target would sit beside the next target's list.
Under `--trunk-scan` the channel being heard is labelled by the target `id` instead.

Target list limits and validation:

- No fixed target-count limit. Each parked target reserves a snapshot of decoder state (~80 KB), and the list is
  capped by a 256 MB budget for those snapshots - a few thousand targets. A CSV past the cap is rejected while
  parsing, with an error naming the budget.
- Blank rows are skipped.
- Every data row must contain the seven fields above.
- The header may have optional columns after `notes`, but the first seven header names must match the required prefix.
  Recognized optional columns are matched by header name; missing trailing optional data fields are treated as empty.
- Frequency values must be at least `1`. Normal 64-bit builds accept values up to `4294967295`; 32-bit builds may reject
  values above `LONG_MAX`.
- Duplicate `id` values are rejected.
- Duplicate `(type, frequency_hz)` pairs are rejected.
- `chan_csv` is only valid for `p25-trunk`, `dmr-trunk`, and `nxdn-trunk` targets.
- `modulation` values are target-type specific: `cqpsk`/`c4fm` are P25-only, and `gfsk` is valid for DMR and both NXDN
  target rates.
- `nxdn-conventional` and `nxdn48-conventional` are distinct types, so the same frequency may appear once as each.
- `rtl_gain` only affects RTL-family inputs opened by DSD-neo. It is ignored when scan retuning is done through rigctl
  against a non-RTL audio input.
- The parser is intentionally small. It can handle a quoted `chan_csv` that contains a comma, but it is not a full CSV
  parser and does not support escaped quotes.

## CLI Usage

For a mixed scan with an RTL-SDR (the shipped starter file contains P25, DMR, NXDN96 and NXDN48 rows, so use `-fa`;
`-ft` is enough for a list with no NXDN targets):

```sh
dsd-neo -fa -i rtl:0:851.0125M:22:0:48:0:2 --trunk-scan examples/trunk_scan_targets.csv -G examples/group.csv --frontend terminal
```

For an external receiver that sends PCM audio over TCP and is tuned through rigctl:

```sh
dsd-neo -ft -i tcp -U 4532 --trunk-scan ~/radio/trunk_scan_targets.csv -G ~/radio/group.csv --frontend terminal
```

Optional timing controls:

```sh
dsd-neo -ft -i rtl:0:851.0125M:22:0:48:0:2 \
  --trunk-scan ~/radio/trunk_scan_targets.csv \
  --trunk-scan-dwell-ms 5000 \
  --trunk-scan-activity-hold-ms 2000 \
  --frontend terminal
```

- `--trunk-scan-dwell-ms <ms>` sets the default idle dwell for targets whose `dwell_ms` column is empty. Default:
  `3000`.
- `--trunk-scan-activity-hold-ms <ms>` sets the default hold time after allowed conventional DMR/NXDN activity
  (NXDN96 and NXDN48 alike). Default: `1200`.
- Per-target CSV values override these defaults.

Use the `-fa` (AUTO) command-line mode for mixed scan lists that contain NXDN targets: `-ft` enables the P25/DMR
decoders but neither NXDN rate, so NXDN rows would sit idle with a startup warning. The two NXDN rates are separate
decoders with separate mode presets -- `-fn` enables NXDN96 only and `-fi` enables NXDN48 only -- so **a list mixing
`nxdn-conventional` and `nxdn48-conventional` rows requires `-fa`**. Single-rate lists can use the narrower preset.
DSD-neo logs a warning at scan start for any target whose decoder is not enabled by the selected mode; it does not
silently flip mode-preset frame flags.

`mode.decode = "auto"` in a config file is equivalent to `-fa` for decoder selection, so it serves mixed lists too.
Use `mode.decode = "nxdn96"` or `mode.decode = "nxdn48"` when a single-rate list is all you want enabled.

## Config Usage

`[trunk_scan]` can enable the same feature from a config file:

```ini
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
frontend = "terminal"

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

Set `tune_enc_calls = false` to enable key-aware encryption lockout. Otherwise eligible encrypted or
encryption-unknown P25 voice grants are visited briefly and classified silently; only clear calls or calls with a
complete matching key for a supported algorithm continue. Missing-key calls remain silent and are released at
classification or grant timeout, while a clear companion Phase 2 slot is preserved. A target confirmed encrypted
without a usable key stays locked out for the rest of the session (no retry backoff); it re-verifies once after key
material changes, releases on clear evidence, and each scan target keeps its own lockout ledger. The menu's "Clear
Encryption Lockouts" action purges every target's ledger, not just the one currently on air.

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
  file/replay input, trunking mode, or conventional `-Y` scan mode disable the inherited scan for that run. UI-only
  flags and trunk-scan timing overrides keep it enabled.

## Runtime Behavior

At startup DSD-neo:

1. Loads the global group list, if one is configured.
2. Loads the target CSV.
3. Imports each target's `chan_csv` into that target's isolated state.
4. Tunes the first target.

During scanning:

- The terminal names the target on air: a `| Trunk Scan:  Target: county-p25 (3/6)` row in the Input Output section,
  and a `| Target: county-p25` line at the top of Call Info, which is the one that survives compact view.
- Idle targets rotate after their dwell time.
- The rotation can be driven from the terminal (Trunking menu, or the hotkeys): `Y` holds the scan on the parked
  target, `b` avoids the parked target for the rest of the session and moves on, `L` moves to the next eligible target
  now, and "Clear avoids" puts every avoided target back. A hold only pauses the idle dwell: the parked target's
  trunking state machine keeps following calls, and `L` still moves while held (the hold then applies to the new
  target). Avoiding the last usable target is refused, as is `L` on a single-target list. Avoids are not written back
  to the CSV. The Trunk Scan row shows `HOLD`, then `[avoided]` when every alternate failed to retune and the
  receiver fell back onto a target that was avoided, and last `Avoids: N`, the number of targets currently out of the
  rotation.
- A non-empty target `modulation` value overrides global CLI/config modulation locks for that target only.
- A target `rtl_gain` value is applied at the retune boundary. Manual per-target gain temporarily suspends supervisory
  tuner autogain; `auto` and global-auto targets restore the saved autogain setting.
- P25, DMR, and NXDN trunk targets stay parked while their trunking state machine is following an active call
  (NXDN stays parked while following an active grant and returns to its control channel at hangtime/release).
- `nxdn-trunk` targets follow the site-broadcast outbound control channel: when a DFA site announces a control
  channel that differs from the target's `frequency_hz`, DSD-neo adopts it (logging
  `NOTICE: NXDN trunking: site control channel is X MHz; following it`) and re-parks that target there from then on.
  A per-target `chan_csv` containing LCN rows pins the control channel instead, so an operator list always wins.
- Conventional DMR and conventional NXDN targets (both `nxdn-conventional` and `nxdn48-conventional`) stay parked
  only after allowed activity is decoded: a DMR voice header or data header, or an NXDN VCALL, DCALL or SDCALL header.
  NXDN48 and NXDN96 share a sync word and every decoded element, so one NXDN reporting path serves both. The allow/block list, private-call tuning,
  data-call tuning, and encrypted-call tuning controls all apply to that decision, so data headers refresh the hold
  only when data-call tuning is enabled (`-e`, or `tune_data_calls` in a config file); it is off by default.
- An `nxdn-trunk` target with a `chan_csv` reports channels it was granted but could not map, once per channel while
  it is parked (`NOTICE: NXDN trunking: grant: CH 12 has no frequency mapping in chan_csv (site.csv)`), and a summary
  for each such target at exit. Every target keeps its own list, so one target's gaps are never attributed to another.
- `nxdn48-conventional` targets park at 2400 sym/s with the 6.25 kHz channel filter; every other GFSK-family target
  parks at 4800 sym/s with the 12.5 kHz filter. Set `modulation = gfsk` on NXDN48 rows: that pins the symbol-rate
  hunt to the 2400 profile for the whole dwell, whereas an empty or `auto` column lets the hunt rotate through the
  other enabled rates during dead air, which also swings the channel filter. The parked target's type selects the
  symbol rate and channel filter even under a global `-m` modulation lock; the lock still governs symbol slicing, so
  DMR and NXDN rows under `-mc` or `-mq` need `modulation = gfsk` (or `auto`) to decode.
- When a retune fails, DSD-neo logs a warning, briefly cools that target down, and tries another eligible target.
  While held, a failed retune retries the held target after the cooldown instead of moving on.

Expected log messages include:

```text
Trunk scan target 'county-p25' at 851012500 Hz
Trunk scan enabled with 6 targets
2 trunk scan target(s) have no enabled NXDN96 decoder (first: 'site-nxdn'); use -fn or -fa to decode them
1 trunk scan target(s) have no enabled NXDN48 decoder (first: 'field-nxdn48'); use -fi or -fa to decode them
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

`row N has invalid modulation`

Use `auto`, `c4fm`, or `cqpsk` for P25 targets. Use `auto` or `gfsk` for DMR and both NXDN rates. Leave the field empty
to keep global/default modulation handling.

`row N has invalid rtl_gain`

Leave the field empty to inherit the global/default gain. Use `0` or `auto` for device automatic gain, or an integer
from `1` to `49` for manual RTL-family gain in dB.

`N trunk scan target(s) have no enabled <NAME> decoder (first: '<id>'); use <flags> to decode them`

The selected decode mode does not enable the decoder those targets need, so they park and dwell without ever
decoding. One line is logged per decoder class, not per target, and NXDN96 and NXDN48 are separate classes. Use
`-fa` for a mixed list, `-fn` for an NXDN96-only list, `-fi` for an NXDN48-only list, `-ft`/`-f1`/`-f2` for P25, or
`-fs`/`-ft` for DMR. A list holding both NXDN rates needs `-fa`, or `mode.decode = "auto"` in a config file, which
selects the same decoders. DSD-neo does not flip mode-preset frame flags for you.

`--trunk-scan cannot be combined with global -C/channel-map config`

Move channel maps into the target CSV `chan_csv` column. Conventional DMR and conventional NXDN rows (both NXDN
rates) must leave `chan_csv` empty.

`--trunk-scan requires an open RTL input or rigctl tuning`

Use an RTL-family radio input, or add rigctl control for an external receiver. File, PulseAudio, UDP, TCP, and stdin
inputs cannot scan by themselves because DSD-neo has no tuner to retune.

`--trunk-scan cannot use IQ replay input because replay cannot retune`

Run trunk scan against live input. IQ replay remains useful for testing one captured control/voice path, but not for
rotating across unrelated scan targets.

## Limitations

- P25 trunk, DMR trunk, DMR conventional, NXDN trunk, NXDN96 conventional, and NXDN48 conventional targets are
  supported. Trunked NXDN targets are 12.5 kHz NXDN96 only: 6.25 kHz NXDN48 Type-D control channels are not a
  trunk-scan target type, so an NXDN48 site's control channel cannot be followed. `-Y` with `-fi` remains available
  for scanning NXDN48 channels outside trunk scan.
- There is one active receiver. Traffic on targets that are not currently parked can be missed.
- Group policy is global across all scan targets.
- Target CSV files are simple comma-delimited files, not full RFC 4180 CSV.
- The feature is not a replacement for a multi-receiver trunking setup when missed calls are unacceptable.

## Migrating From Existing Trunking Runs

Existing single-system trunking commands still work:

```sh
dsd-neo -ft -i rtl:0:851.0125M:22:0:48:0:2 -T -C dmr_t3_chan.csv -G group.csv --frontend terminal
```

To scan multiple systems with one receiver:

1. Keep `-G group.csv` if you want the same labels and allow/block policy for all targets.
2. Remove global `-C` or `[trunking] chan_csv`.
3. Put each trunk system's channel map in the target CSV `chan_csv` column.
4. Replace `-T` with `--trunk-scan targets.csv`.

`-Y` conventional scanning remains a separate mode for fast conventional sync scanning and is mutually exclusive
with trunk scan.
