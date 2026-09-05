# CSV Input Formats

DSD-neo uses small, purpose-built CSV importers for trunking helpers (channel maps, group lists) and key lists. These
parsers are intentionally minimal and **not** full RFC 4180 CSV parsers.

If you want known-good starting points, see `examples/` in the repository.

## General Rules (Unless A Format Says Otherwise)

- The **first line is treated as a header and is ignored**. Keep the header line (it can be any text).
- Fields are split on literal commas (`,`). Quoting/escaping is **not supported**.
  - Do not include commas inside a field.
- Avoid blank lines and comment-only lines (they may be parsed as data).
- Extra columns after the required ones are ignored, except where a format names an optional column by header (such as
  a channel map's `name` or key columns). Use the rest for notes/labels.
- Imported text fields are copied into fixed-size runtime buffers. Keep short fields concise; long `mode` and `name`
  values are truncated in runtime display/policy state.

## Importing on Android

The Android app does not take CLI flags directly. Import CSVs through the UI instead: **Settings → Imported files**
manages the library (import, update, remove), and the add/edit-system wizard's **Trunking data** panel assigns a
channel map, talkgroup list, or key file to a system. Files are picked with the system document picker and copied into
app-private storage (`files/imports/`), so the original can live anywhere (Downloads, Drive, …) and is not read again
after import — use "Update from file" to pull in a changed original. Each import is validated immediately and the row
shows how many entries loaded ("412 talkgroups · 3 rows skipped"); a file whose rows all fail to parse is flagged
"No usable rows". While a session is running, long-press its title on the monitor screen to edit that system; saving
applies the files that changed to the live session immediately, including clearing a field to "None" — that unloads the
channel map, talkgroup list or keys from the running session. One limit is worth knowing: the gesture only works for a
session this app instance started (after the Activity is recreated while the service kept running, there is no
saved-system row to write back to).

Applying a channel map **replaces** the live one rather than merging into it, so anything the decoder learned on the
air is discarded along with the previous file's entries; a grant for a channel the new file omits stays unresolved
until the site announces it again.

The library validates against the kind you picked, by content rather than by file name. A channel map and a decimal key
file share the same `number,number` grammar and the header line is free text, so what separates them is the frequency
column: picking a key list as a channel map reports "No usable rows". Two files of the same kind are still
indistinguishable — nothing stops one site's map being picked for another.

Programmatic validation uses the same dry-run parser: `dsd_csv_validate_*` in `<dsd-neo/core/csv_validate.h>` reports
accepted/skipped/total row counts without touching live decoder state.

### Generated imports

The RadioReference import (`docs/radioreference-import.md`) writes into the same library through the same validator,
and its files are ordinary CSVs in the formats below — nothing reads them differently. Two things distinguish them:

- **Their header line names its origin.** `DEC,Mode,Name (generated from RadioReference)` for a group list; a
  trunked channel map gets `ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this
  line)`. A **conventional** channel map's header differs —
  `ChannelNumber(dec),frequency(Hz),name,(generated from RadioReference; do not delete this line)` — because its
  third field is exactly `name`, which is what opts every row into the channel map's optional name column (see
  below); a trunked map has no per-channel name to offer, so its header stays two-field and the note stays free
  text. Both parsers discard physical line 1 unconditionally, so the text is for humans — but deleting it eats the
  first data row.
- **Their library row records provenance**, so the file can be re-fetched later. In
  `files/imported_files.json` those rows carry five extra keys beyond the ordinary
  `name`/`path`/`type`/`importedAt`/`accepted`/`skipped`: `origin` (`"radioreference"`), `rrSid` (the RadioReference
  system ID), `rrSiteIds` (every selected site's RadioReference `siteId`, comma-joined in selection order — a
  Conventional Networked import selects several repeaters, and the RF site *number* cannot be used because a system
  numbers several sites the same), `rrKind` (`"group"` or `"chan"`), and `rrPartialEnc` (the "treat partly encrypted
  as encrypted" answer the import was given). A picked file has none of them, and a row missing any of them loads
  with it defaulted.

## Channel Map CSV (`-C <file>` / `[trunking] chan_csv`)

Purpose: Map a trunking channel number to an RF frequency.

Required columns:

1. `channel_number` (`0 <= channel_number < 65535`), in any of three spellings:
   - decimal: `10822`
   - hex with a `0x` prefix: `0x2A46` (what the P25 event history prints as `Active Ch: 2A46`)
   - `<iden>-<chan>`: `2-2630` (identifier `0..15`, dash, decimal channel `0..4095`; the form DSDPlus uses and the
     one printed in parentheses after every P25 channel in this program's event history)
2. `frequency_hz` (integer Hz)

Optional column:

3. `name` (free text) - a label for the channel, shown while you listen to it.

Notes:

- `frequency_hz` is parsed as an integer (no `K/M/G` suffixes).
- `frequency_hz` must be between 100000 (100 kHz) and 6000000000 (6 GHz) — the range any supported front end can
  reach. A row outside it (including `0`) is skipped with a warning, and its slot in the LCN list below is left at 0 so
  later rows keep their LCN numbers. This is what tells a channel map apart from a decimal key list, which has the same
  `number,number` shape.
- Optional headers after the two required columns are matched case-insensitively, with surrounding whitespace
  trimmed: `name`, `mode`, `keys_hex_csv`, `keys_dec_csv`, `single_key_hex`, `single_key_dec`, and `options` (`relevant_CLI_switches`). They may appear in
  any order, including after column 16. Unrecognized columns are ignored. The first `name` column wins; duplicate
  `mode` or key headers reject the file. This preserves legacy free-text note columns, including notes with commas.
- Channel-map headers and data rows are read in full up to 1 MiB (including the line ending and terminating NUL).
  Longer rows reject the import with an error; they are never split into additional channels.
- `mode` accepts `p25`, `dmr`, `nxdn96`, `nxdn48`, `dpmr`, `dstar`, `ysf`, and `m17`, case-insensitively and trimmed.
  Empty or missing values inherit the configured global decoder settings. Invalid nonempty values reject the import
  with file and row diagnostics, including on rows whose channel number is invalid.
- Under `-Y`, a declared mode selects its decoder class even when the global preset excludes it. P25 enables both
  phases and excludes DMR and X2-TDMA. Fully declared mixed lists work without `-fa`; untyped lists retain their
  existing behavior. The initial input uses global settings until the first scheduled row entry. Explicit global
  modulation locks remain effective. Manual `L` cycling and avoid/advance use the same row-entry rules and visit
  same-frequency rows with different metadata. Zero-frequency placeholders change neither the mode nor keys.
- Modes are stored by scan-list slot: duplicate channel numbers and repeated frequencies remain distinct rows.
  Trunk-scan target `type` is authoritative; `mode` values inside a target's `chan_csv` are validated and discarded.
- A `name` is trimmed of surrounding whitespace, capped at 63 bytes (never splitting a UTF-8 character), and must not
  contain a comma. It is stored per row of the LCN list below, so a row whose frequency was skipped keeps its name and
  the rest stay aligned. A row whose *channel number* does not parse is different: it takes no LCN slot at all, so it
  stores no name either.
- A row skipped for an unusable frequency keeps its name in the file's numbering but is never shown, because the
  scanner parks on the frequency it is already on rather than tuning such a row.
- Two optional columns carry per-row key files for the `-Y` scanner: `keys_hex_csv` (loaded like `-K`) and
  `keys_dec_csv` (loaded like `-k`). They opt in by header name at any column position past the frequency (like
  `name`, matched case-insensitively); a duplicated key header rejects the file. A row may fill both columns;
  they load into one per-row key set.
- Each key cell names a key file path, resolved relative to the channel map. Blank cells store nothing, and a row
  whose channel number does not parse takes no slot and stores nothing. A file-only key path on such a row is not
  opened. Paths cannot contain commas: the splitter does no quote handling.
- `single_key_dec` embeds the `-b` Motorola Basic Privacy key number directly in a row. It accepts unsigned decimal
  `0..255`. `single_key_hex` embeds the `-H` key: an optional leading `0x`, embedded ASCII whitespace, and exactly
  10, 32, or 64 hexadecimal digits are accepted. Both direct columns may be filled together. They are also matched
  case-insensitively at any position past the frequency.
- A row must choose one source family: any nonblank `single_key_dec`/`single_key_hex` value together with a nonblank
  `keys_dec_csv`/`keys_hex_csv` path rejects the import. This source conflict and direct-key syntax are validated even
  when the channel number does not parse, although that row still takes no slot. Blank direct cells are absent; an
  explicit decimal `0` or an all-zero hex key is present. A direct-key row installs a complete replacement key set,
  so scalar key families not supplied by that row and all file-backed keyring entries are cleared while it is active.
- Row keys take effect only under `-Y`: hopping onto a keyed row installs its set, hopping back onto an unkeyed
  row restores the global keys. Under plain trunking `-C` they are stored but never applied (one warning); under
  trunk-scan per-target `chan_csv` they are discarded, like `name`.
- Validation opens key files and validates direct values, so an unloadable path or malformed direct key fails the
  import. Diagnostics name the field but never repeat a direct key value. The Qt/Android picker flow supports direct
  values because they are embedded in the copied channel map; it does not copy companion per-row key files.
- Where a name shows: the end of the `-Y` conventional scanner's **Scan Mode** row, a `Channel:` line at the top of
  the Call Info panel, and as a prefix on the event history rows recorded while that channel is tuned. Encrypted
  traffic that reports no talkgroup still says which channel it was heard on. While a `--trunk-scan` target is on
  air its id is the label instead, and the Scan Mode row shows no name.
- Every column is positional, so an empty middle field is an empty frequency: `1,,851000000` is a row with no
  frequency and is skipped, not a channel at 851 MHz.
- **P25 keys.** A P25 channel is a 16-bit number: the 4-bit band-plan identifier in the top nibble and the 12-bit
  channel in the rest, `(iden << 12) | chan`. `2A46` hex, `2-2630`, and `10822` are the same channel. A row whose
  key is a valid spelling but cannot be a channel (`16-0`, `2-4096`, `15-4095` = the `65535` sentinel) is skipped
  like an unparsable one. Sites that broadcast their band plan (`IDEN_UP`) purge the map entries of a band the
  moment the plan arrives; a map is the workaround for sites that never do, and a [P25 band plan CSV](#p25-band-plan-csv---p25-bandplan-file--trunking-p25_bandplan_csv)
  replaces it with one row per identifier.
- For EDACS-style workflows, DSD-neo also records the `frequency_hz` values in **row order** as an LCN frequency list,
  so keep rows in the LCN order you want. An imported LCN list has no length limit; it is bounded only by memory.
  Site broadcasts never write into an imported list or shorten it - the list is positional, so a skipped row's 0
  holds that LCN's place. Frequencies learned from site broadcasts (with no list imported) are capped at 26.

Example:

```csv
ChannelNumber(dec),frequency(Hz),note
999,862093750,default cc
1,863093750
2,862093750
```

Example with names (`examples/conventional_scan_named.csv`):

```csv
channel,frequency_hz,name
1,462562500,GMRS 1
2,462587500,GMRS 2
```

Example with file-backed and direct per-row keys (`examples/conventional_scan_keyed.csv`):

```csv
channel,frequency_hz,name,keys_hex_csv,keys_dec_csv,single_key_dec,single_key_hex
1,462562500,System A,multi_key_hex.csv,,,
2,462587500,System B,,multi_key.csv,,
3,462612500,Shared,,,1,0000001F00
```

### Scoped row options

Channel maps and trunk-scan targets accept an optional `options` column. `relevant_CLI_switches` is an alias;
headers match case-insensitively and naming both rejects the file. Combine `mode` and `options` in the same map
(`examples/conventional_scan_options.csv`); run it with `-Y -C examples/conventional_scan_options.csv`.

Options are parsed once when the list is loaded. They are a restricted argument list, with the following switches:

| Switch | Meaning and accepted modes |
| --- | --- |
| `-b <decimal>` | Motorola BP number, `0..255`; DMR. |
| `-H <hex>` | Hytera/AES key; DMR accepts 10/32/64 digits, P25 32/64, NXDN 64. |
| `-1 <hex>` | Direct RC4/DES key, 1..16 digits; DMR/P25/NXDN. |
| `-R <decimal>` | Direct scrambler key, `0..32767`; NXDN/dPMR. |
| `-k <file>`, `-K <file>` | Decimal/hex key files; DMR/P25/NXDN. |
| `-G <file>` | Group names and policy for this row or system. |
| `-4` | Force loaded privacy keys over signalling; DMR/NXDN. |
| `-0`, `--dmr-force-algid <hex>` | DMR algorithm fallback when identifiers are missing. `-0` means ALGID `21`. |
| `-F`, `--strict-crc` | Relax CRC checks for DMR/P25/M17, or restore strict checks. |
| `--no-force-key` | Disable privacy forcing and algorithm fallback for this row. |
| `--scan-voice-only`, `--no-scan-voice-only` | Enable/disable the conventional voice gate. |
| `--scan-voice-qualify-ms`, `--scan-voice-hold-ms` | Conventional voice-gate intervals, `100..600000` milliseconds. |

Protocol-specific options require a declared `mode`; trunk targets use their `type`. Trunk-system targets reject
voice-gate options: their existing `dwell_ms` and `activity_hold_ms` columns retain their roles. Input/output,
frontend selection, decoder flags and scanner-wide `-t` are not accepted in `options`.

Omitted settings inherit the outer CLI/configuration, including forcing. Use `--no-force-key` on a normal mixed
clear/BP channel when forcing is configured globally. `-b 1` with normal signalling processes clear and BP calls;
`-4` deliberately applies loaded privacy keys even to frames marked clear and can corrupt those clear calls.
There is no automatic choice between forced Motorola and Hytera privacy.

The existing `single_key_dec` and `single_key_hex` columns still mean `-b` and `-H`, respectively. Use `options=-R 1`
for a direct NXDN scrambler and `options=-1 0123456789` for direct RC4. Loading a key does not itself enable forcing.
A direct source replaces the row's complete key set; unspecified families and keyring entries are cleared. Explicit
zero is a supplied value. Direct and file-backed key sources cannot be mixed, including across columns and options.
Compatible direct families may be combined, but duplicate definitions reject the import.

Within a row, conflicting force settings reject the import. `-0 --dmr-force-algid 21` is accepted because both request
the same fallback. ALGID `00` disables fallback; `01` and `16` are rejected in this option because they are reserved
non-algorithm markers internally. Received DMR algorithm/key identifiers retain precedence over algorithm fallback.

Separate switches with whitespace; quote an argument with single or double quotes to retain spaces. Backslashes are
literal, so `-G "C:\Radio Lists\groups.csv"` works without shell escaping. Hex keys may include an optional `0x`
prefix and whitespace inside a quoted argument. Long arguments also accept `--name=value`. CSV commas remain field
separators, including inside quotes. Unknown switches, positional text, malformed quotes and duplicate settings are
errors. Diagnostics name the row and option without repeating raw option text or key values.

File paths resolve relative to the containing CSV. Keys and group policies are loaded before scanning starts;
switching rows never reads these files. `-G` replaces the active group policy while parked. Labels and session policy
edits remain with that row's policy; unconfigured rows use the global policy. Global group imports and scoped setting
changes from the frontend update the saved baseline beneath the active row. Leaving or replacing the scan restores
that baseline. Frontend configuration saves preserve configured defaults, not temporary row overrides.

The Qt/Android picker supports embedded values. Companion files named by `-G`, `-k` or `-K` have the same limitation
as existing key-file columns: the picker does not copy companion files alongside the imported list.

Declared modes use these symbol profiles:

| Mode | Symbols per second | Levels |
| --- | ---: | ---: |
| P25 (both phases) | 4800 and 6000 | 4 |
| DMR, NXDN96, YSF, M17 | 4800 | 4 |
| NXDN48, dPMR | 2400 | 4 |
| D-STAR | 4800 | 2 |

## Trunk Scan Target CSV (`--trunk-scan <file>` / `[trunk_scan] targets_csv`)

Purpose: Rotate one tuner across explicit P25 trunk, DMR trunk, NXDN trunk, and one-frequency DMR, NXDN96 and
NXDN48 targets. See
`docs/trunk-scan.md` for the full setup workflow and troubleshooting guide.

The header must start with this exact prefix:

```csv
id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes
```

- Optional columns are allowed after `notes`; recognized optional columns are matched by header name.
- Every data row must contain the first seven fields, even when optional values are empty. Missing trailing optional
  fields are treated as empty.
- Blank data rows are skipped.

Columns:

| Column | Required | Behavior |
|--------|----------|----------|
| `id` | Yes | Unique short name shown in the terminal status row and Call Info, as the `[id]` prefix on event-history rows, `-J` log lines and the rdio `talkgroup_tag` fallback, and in log messages. Empty or too-long IDs are rejected. |
| `type` | Yes | One of `p25-trunk`, `dmr-trunk`, `dmr-conventional`, `nxdn-trunk`, `nxdn-conventional` (NXDN96, 12.5 kHz), or `nxdn48-conventional` (NXDN48, 6.25 kHz). |
| `frequency_hz` | Yes | Decimal Hz only. Normal 64-bit builds accept `1..4294967295`; 32-bit builds may reject values above `LONG_MAX`. Do not use `K`/`M`/`G` suffixes in CSV. |
| `chan_csv` | No | Optional channel-map path for trunk targets. Paths are resolved relative to this CSV. Leave empty for conventional DMR and both conventional NXDN types. |
| `dwell_ms` | No | Per-target idle dwell (`250..600000`). Empty uses `--trunk-scan-dwell-ms` or `[trunk_scan] idle_dwell_ms`. |
| `activity_hold_ms` | No | Per-target conventional DMR/NXDN (NXDN96 and NXDN48) activity hold (`250..600000`). Empty uses `--trunk-scan-activity-hold-ms` or `[trunk_scan] activity_hold_ms`. |
| `notes` | No | Ignored. Use for local notes. |
| `modulation` | No | Per-target demod hint. Empty preserves global/default handling. `auto` uses target defaults and overrides global `-m` locks for that target. P25 accepts `auto`, `c4fm`, `cqpsk`; DMR and both NXDN rates accept `auto`, `gfsk`. |
| `rtl_gain` | No | Per-target RTL-family tuner gain. Empty uses the global/default gain. `0` or `auto` requests device automatic gain. `1..49` requests manual dB gain. Ignored for non-RTL retuning paths. |
| `keys_hex_csv` | No | Per-target hex key file (`-K` format), resolved relative to this CSV. A row may fill both key columns; they load into one per-target key set. Empty uses the global keys. |
| `keys_dec_csv` | No | Per-target decimal key file (`-k` format), resolved relative to this CSV. Empty uses the global keys. |
| `single_key_dec` | No | Embedded `-b` Motorola Basic Privacy key number (`0..255`). Explicit `0` is present and overrides the global key. May be combined with `single_key_hex`, but not either key-file column. |
| `single_key_hex` | No | Embedded `-H` key: optional `0x`, whitespace ignored, exactly 10, 32, or 64 hex digits. May be combined with `single_key_dec`, but not either key-file column. |
| `options` | No | Scoped switches described above; target `type` determines which protocol options apply. |
| `p25_bandplan_csv` | No | Per-target [P25 band plan CSV](#p25-band-plan-csv---p25-bandplan-file--trunking-p25_bandplan_csv) for a `p25-trunk` target, resolved relative to this CSV. The rows are parked in the target's snapshot, so one exported multi-system file can be named on every P25 row: each target seeds only the rows that carry its own WACN/SYS (and rows that carry none). |

Validation notes:

- No fixed target-count limit. Each parked target reserves a snapshot of decoder state (~80 KB), and the list is
  capped by a 256 MB budget for those snapshots - a few thousand targets. A CSV past the cap is rejected while
  parsing, with an error naming the budget.
- Duplicate IDs and duplicate `(type, frequency_hz)` rows are rejected. `nxdn-conventional` and
  `nxdn48-conventional` are distinct types, so one frequency may appear once as each.
- Optional column names are exact-case in this format. Duplicate direct-key headers, malformed direct values, and
  rows that mix a direct value with a key-file path are rejected without echoing the key value.
- `chan_csv` and `p25_bandplan_csv` on conventional (`dmr-conventional`/`nxdn-conventional`/`nxdn48-conventional`)
  rows are rejected; a duplicated `p25_bandplan_csv` header is rejected, and a band plan that fails to load fails
  the whole import.
- Global `-C`/`[trunking] chan_csv` and `--p25-bandplan`/`[trunking] p25_bandplan_csv` are rejected in trunk scan
  mode so channel maps and band plans do not leak across systems.
- One tuner can only monitor the active target; traffic on other targets can be missed.
- This parser can handle a quoted `chan_csv` field containing a comma, but it is not a full RFC 4180 parser and does not
  support escaped quotes.

Example:

```csv
id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation,rtl_gain
county-p25,p25-trunk,851012500,,3000,,primary P25 control channel,cqpsk,18
city-dmr,dmr-trunk,452012500,dmr_channels.csv,3000,,DMR Tier III control channel,auto,
plant,dmr-conventional,461112500,,1500,1200,one-frequency DMR,gfsk,auto
site-nxdn,nxdn-trunk,461037500,,3000,,NXDN Type-C control channel,auto,
field-nxdn,nxdn-conventional,461550000,,1500,1200,one-frequency NXDN96 channel,gfsk,
field-nxdn48,nxdn48-conventional,461556250,,1500,1200,one-frequency NXDN48 (6.25 kHz) channel,gfsk,
```

## P25 Band Plan CSV (`--p25-bandplan <file>` / `[trunking] p25_bandplan_csv`)

Purpose: Give the decoder a P25 site's band plan (its `IDEN_UP` identifier table) when the site never broadcasts
one, so grants can be turned into frequencies without a per-channel map. The same file is what **Export learned
P25 band plan...** and `--p25-bandplan-export` write, so a plan learned on one run (or one site of a system)
can be loaded on the next.

Required columns (positional):

1. `iden` (`0..15`) - the band-plan identifier, the top hex digit of a P25 channel number
2. `base_hz` (integer Hz, a multiple of 5)
3. `spacing_hz` (integer Hz, a multiple of 125, at most 127875)

Optional columns (matched by header name; the first three keep their positional meaning when unnamed):

4. `type` - the P25 channel type as `IDEN_UP` carries it: `1` FDMA (default), `3` two-slot TDMA, `4` four-slot TDMA;
   types `0..2` are treated as FDMA, anything else as TDMA
5. `tx_offset_hz` (signed integer Hz, default `0`) - the mobile transmit offset; a multiple of `spacing_hz` on a row
   that gives a `bandwidth_hz` or has a TDMA `type` (the `IDEN_UP_VU`/`IDEN_UP_TDMA` encoding), a multiple of 250000
   on a standard FDMA row (the `IDEN_UP` encoding). Informational: the decoder tunes the repeater output
6. `bandwidth_hz` - `6250` or `12500` for a VHF/UHF (`IDEN_UP_VU`) identifier, empty or `0` for the standard form
7. `wacn` / `sysid` (hex, no prefix, as the UI prints them: `W:BEE00 S:3A1`) - both or neither. A row that names a
   system applies only while the receiver is on that WACN/SYS; a row without applies everywhere

Notes:

- Rows seed the identifier table at trust `prov` (unconfirmed). A real `IDEN_UP` from the site replaces the row the
  moment it arrives, and the table is re-seeded from the file whenever the receiver moves to another system (rows
  for the new system and rows without a system). When a file has both a row that names the current system and a
  row without one for the same identifier, the system's row wins.
- A row with a bad value is skipped with a warning naming the row; a duplicate identifier (same `iden`, FDMA/TDMA
  class and system) replaces the earlier row; a file with no usable row is refused and leaves the previous plan in
  place. At most 64 rows are kept.
- `--p25-bandplan` and `[trunking] p25_bandplan_csv` are refused with `--trunk-scan`; use the target list's
  `p25_bandplan_csv` column instead (see above).
- Under trunk scan, targets that turn out to be sites of the same system (same WACN/SYS) also share what one of
  them learned over the air: an identifier the parked target is missing is copied from another target's table at
  trust `prov` when the WACN/SYS match, and never otherwise.
- Terminal: **Trunking -> Channels & groups -> Import P25 band plan CSV...** loads one into the running decoder and
  **Export learned P25 band plan...** writes the current tables (every target's, under trunk scan) with their
  WACN/SYS. `--p25-bandplan-export <file>` writes the same file once at clean shutdown, which suits a headless run.
- Android: the imports library takes the file as the **P25 band plan** kind, and a saved system can name one; it is
  passed to the session as `--p25-bandplan`.
- Uniden Sentinel's custom band plan (base, spacing, offset, bandwidth per identifier) maps onto columns 1-6;
  SDRTrunk's identifier table (type/slots, base, spacing, bandwidth, transmit offset) onto the same columns.

Example (`examples/p25_bandplan.csv`):

```csv
iden,base_hz,spacing_hz,type,tx_offset_hz,bandwidth_hz,wacn,sysid
0,851006250,6250,1,-45000000,12500,,
1,762006250,6250,1,-30000000,12500,,
2,762006250,6250,3,-30000000,,BEE00,3A1
```

## Group List CSV (`-G <file>` / `[trunking] group_csv`)

Purpose: Provide labels and allow/block behavior for talkgroups.

Required columns:

1. `id` (decimal integer; talkgroup ID or radio ID depending on protocol context)
2. `mode` (string)
3. `name` (string)

Notes:

- The first line is treated as header text and is required.
- Basic/default behavior uses only the first 3 columns; extra columns are ignored.
- `mode` is matched literally by features that consult it:
  - `A` usually means allow/normal.
  - `B` and `DE` are treated as locked out.
- Names are not CSV-escaped; avoid commas and line breaks in fields.
- Exact IDs are decimal `uint32_t` values (`0..4294967295`). Runtime policy lookups import all valid exact
  rows that fit memory.

Extended policy columns are supported only when the header opts into this exact ordered prefix starting at column 4:

1. `priority` (0..100, default `0`)
2. `preempt` (`true`/`false`, default `false`)
3. `audio` (`on`/`off`, default from `mode`)
4. `record` (`on`/`off`, default from `mode`)
5. `stream` (`on`/`off`, default from `mode`)
6. `tags` (free text metadata; accepted for notes/round-tripping, not applied to runtime policy)

Important behavior:

- The header must contain `priority` in column 4 and continue in that order for the available policy columns.
- If the header is basic/unknown (for example `id,mode,name,metadata`), optional policy parsing is disabled and extra
  columns remain metadata-only values.
- `id` supports exact IDs (`1201`) and ranges (`1200-1299`).
  - Exact rows populate the runtime policy table used for labels and decisions.
  - Range rows are policy entries that match IDs within the configured range.
- `preempt`, `audio`, `record`, and `stream` accept `true`/`false`, `yes`/`no`, `on`/`off`, or `1`/`0`.
- Exact duplicates preserve first-match behavior.
- `audio=off` forces `record=off` and `stream=off`.
- `mode=B`/`DE` forces media fields off regardless of optional values.

Example:

```csv
DEC,Mode(A=Allow; B=Block; DE=Enc),Name,Tag
1449,A,Fire Dispatch,Fire
22033,DE,Law Dispatch,Law
```

Extended policy example:

```csv
id,mode,name,priority,preempt,audio,record,stream,tags
1201,A,Dispatch 1,80,true,on,on,on,primary
1202,A,Dispatch 2,40,false,on,off,on,secondary
1300-1399,A,Ops Range,10,false,on,on,on,wide
```

## Decimal Key List CSV (`-k <file>`)

Purpose: Import decimal key IDs and values for basic privacy/scrambler helpers.

Required columns:

1. `key_id` (decimal integer)
2. `value` (decimal integer)

Notes:

- The value is stored as a 40-bit quantity (higher bits are discarded).
- If `key_id` exceeds 16 bits, it is truncated to 24 bits and hashed down to 16 bits for storage.
- Extra columns are ignored.
- A channel-map row (`keys_dec_csv`) or trunk-scan target (`keys_dec_csv`) can name a file in this format to
  override the global keyring on that row or target; see the Channel Map and Trunk Scan Target sections above.

Example:

```csv
key id or tg id (dec),key number or value (dec)
2,70
12,48713912656
```

## Hex Key List CSV (`-K <file>`)

Purpose: Import hex key IDs and values.

Required columns:

1. `key_id` (hex integer)
2. `value0` (hex integer)

Optional columns (for longer multi-part keys):

3. `value1` (hex integer)
4. `value2` (hex integer)
5. `value3` (hex integer)

Notes:

- Each `valueN` is parsed as a hex integer. For long keys, DSD-neo stores the additional parts at internal offsets.
- Extra columns beyond these are ignored.
- A channel-map row (`keys_hex_csv`) or trunk-scan target (`keys_hex_csv`) can name a file in this format to
  override the global keyring on that row or target; see the Channel Map and Trunk Scan Target sections above.

Example:

```csv
keyid(hex),value0,value1,value2,value3
C197,A753BC945DE5E0F1,EC1970F8565154E6,D9DF2FAC6278FA93,B531D2CC046E93A2
```

## Vertex Key->Keystream Map CSV (`--dmr-vertex-ks-csv <file>`)

Purpose: Interim decrypt support for Vertex DMR ALG `0x07` by mapping an entered key value to a known AMBE keystream.

Required columns:

1. `key_hex` (hex key value; same representation as `-1`, optional `0x` prefix)
2. `keystream_spec` in `bits:hex[:offset[:step]]` format

Notes:

- Header row is ignored (required by importer convention).
- `keystream_spec` format matches the `-S` option exactly:
  - `bits` is decimal and must be `1..882`
  - `hex` is packed keystream bytes
  - Optional `offset` and `step` are decimal bit positions for frame-aligned application
- Duplicate keys are allowed; the last occurrence replaces earlier rows.
- Maximum rows: `64`.

Example:

```csv
key_hex,keystream_spec,notes
1234567891,49:ED0AED4AED4AED4A,Vertex sample key
0xA1B2C3D4E5,168:0123456789ABCDEF0123456789ABCDEF0123456789:0:49,frame aligned
```

## DMR Talkgroup->Key ID Map CSV (`--dmr-tg-key-csv <file>`)

Purpose: Select the decryption key by talkgroup instead of the OTA-signaled key ID. A row here is an
explicit per-talkgroup override: the mapped key ID is used in place of the signaled one for the whole
call, and unmapped talkgroups keep normal signaled-key-ID behavior. Keys themselves still come from
`-K`/`-k`; this file only picks which key ID a talkgroup uses.

Required columns:

1. `tg` (decimal talkgroup ID, `1..16777215`)
2. `keyid` (hex key ID, `00..FF`; indexes the `-K`/`-k` keyring)

Notes:

- Header row is ignored (required by importer convention).
- DMR only, and it needs the CSV keyring (`-K`/`-k`) loaded plus a known ALG ID (signaled, or via
  `--dmr-force-algid` on systems that don't signal one). Loading the map without `-K`/`-k` warns and
  does nothing. With `--dmr-force-algid`, the row is already consulted at the call's first voice LC —
  before the forced ALG ID has been written to the slot — so a mapped talkgroup is never classified
  or locked out against the key the slot happened to carry from the previous call.
- Applies everywhere a DMR call's effective key ID is resolved: voice, encrypted data bursts (PDUs),
  crypto classification, the `--enc-lockout` release decision, and sdrtrunk JSON replay input.
- Group voice calls only. Private (unit-to-unit) calls put the destination radio ID where the
  talkgroup would be, and DMR radio IDs share the talkgroup's 24-bit address space, so an individually
  addressed call or data PDU is never matched against this file. For data, the header's group/individual
  bit is recorded alongside the target so the PDU can still be classified after the header itself is
  gone.
- The OTA key ID is never rewritten: logs, event history, and the UI keep showing the signaled key ID,
  and the printed data-PDU header leads with it too, appending the override only when one applied.
- A row applies only when the mapped key ID holds the kind of key material the call's algorithm
  actually consumes: a scalar for RC4/DES/Hytera Enhanced, the AES segments for AES-128/AES-256 (P25
  TDEA needs three), or a complete, non-zero quartet for Kirisun (`0x36`/`0x37`). A key ID that holds
  material of some other kind — an AES quartet mapped to an RC4 talkgroup, a scalar mapped to an
  AES-128 one — does not apply, for the same reason an unimported key ID doesn't.
- A mapped key ID with no imported key material, or with material of the wrong kind for the call's
  algorithm, always falls back to the signaled key ID instead of blocking the talkgroup outright, so a
  typo in the key ID column — or a row that names a real but mismatched key — never silently kills
  decryption for that talkgroup.
- Console visibility of this differs by consumer: voice announces both an applied override and a
  fallback, once per call, except for algorithms that consume no keyring material, where the row is
  inert and silent (e.g. Vertex); data PDUs show an applied override in the PDU header line but stay
  silent on a fallback; crypto classification, the `--enc-lockout` decision, and sdrtrunk replay apply
  the map with no console notice either way.
- In sdrtrunk JSON replay, a map row wins over the older implicit "key indexed by talkgroup"
  convention; with no matching row, that older behavior is unchanged. This holds regardless of the
  order in which fields appear in the JSON record.
- Duplicate talkgroups are allowed; the last occurrence replaces earlier rows.
- Maximum rows: `256`.
- CLI-only: there's no TUI or Android import path to load this map, only `--dmr-tg-key-csv` at startup.
  Clearing keys from the TUI clears the map along with them, with no runtime path to reload it.

Example (the key IDs are the ones in `examples/multi_key_hex.csv`; a mapped key ID with no imported
key material falls back to the signaled key ID instead of blocking the talkgroup):

```csv
tg (dec),keyid (hex)
123,02
4567,0C
```

## DMR Tier III LCN Calculator Input (`--calc-lcn <file>`)

The `--calc-lcn` one-shot tool is more flexible than the CSV imports above:

- It scans each line for the first numeric field and treats it as a frequency.
- Frequencies may be in **Hz** (e.g., `451237500`) or **MHz** (e.g., `451.2375`).
- The output is printed to stdout as `lcn,freq` CSV.
