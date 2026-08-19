# CSV Input Formats

DSD-neo uses small, purpose-built CSV importers for trunking helpers (channel maps, group lists) and key lists. These
parsers are intentionally minimal and **not** full RFC 4180 CSV parsers.

If you want known-good starting points, see `examples/` in the repository.

## General Rules (Unless A Format Says Otherwise)

- The **first line is treated as a header and is ignored**. Keep the header line (it can be any text).
- Fields are split on literal commas (`,`). Quoting/escaping is **not supported**.
  - Do not include commas inside a field.
- Avoid blank lines and comment-only lines (they may be parsed as data).
- Extra columns after the required ones are ignored. Use them for notes/labels.
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

- **Their header line names its origin.** `DEC,Mode,Name (generated from RadioReference)` and
  `ChannelNumber(dec),frequency(Hz) (generated from RadioReference; do not delete this line)`. Both parsers discard
  physical line 1 unconditionally, so the text is for humans — but deleting it eats the first data row.
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

1. `channel_number` (decimal integer, `0 <= channel_number < 65535`)
2. `frequency_hz` (integer Hz)

Notes:

- `frequency_hz` is parsed as an integer (no `K/M/G` suffixes).
- `frequency_hz` must be between 100000 (100 kHz) and 6000000000 (6 GHz) — the range any supported front end can
  reach. A row outside it (including `0`) is skipped with a warning, and its slot in the LCN list below is left at 0 so
  later rows keep their LCN numbers. This is what tells a channel map apart from a decimal key list, which has the same
  `number,number` shape.
- Extra columns are ignored; use them for labels like "default CC".
- For EDACS-style workflows, DSD-neo also records the `frequency_hz` values in **row order** as an LCN frequency list,
  so keep rows in the LCN order you want. The LCN list stores at most 26 frequencies.

Example:

```csv
ChannelNumber(dec),frequency(Hz),note
999,862093750,default cc
1,863093750
2,862093750
```

## Trunk Scan Target CSV (`--trunk-scan <file>` / `[trunk_scan] targets_csv`)

Purpose: Rotate one tuner across explicit P25 trunk, DMR trunk, and one-frequency DMR targets. See
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
| `id` | Yes | Unique short target name used in log messages. Empty or too-long IDs are rejected. |
| `type` | Yes | One of `p25-trunk`, `dmr-trunk`, or `dmr-conventional`. |
| `frequency_hz` | Yes | Decimal Hz only. Normal 64-bit builds accept `1..4294967295`; 32-bit builds may reject values above `LONG_MAX`. Do not use `K`/`M`/`G` suffixes in CSV. |
| `chan_csv` | No | Optional channel-map path for trunk targets. Paths are resolved relative to this CSV. Leave empty for conventional DMR. |
| `dwell_ms` | No | Per-target idle dwell (`250..600000`). Empty uses `--trunk-scan-dwell-ms` or `[trunk_scan] idle_dwell_ms`. |
| `activity_hold_ms` | No | Per-target conventional DMR activity hold (`250..600000`). Empty uses `--trunk-scan-activity-hold-ms` or `[trunk_scan] activity_hold_ms`. |
| `notes` | No | Ignored. Use for local notes. |
| `modulation` | No | Per-target demod hint. Empty preserves global/default handling. `auto` uses target defaults and overrides global `-m` locks for that target. P25 accepts `auto`, `c4fm`, `cqpsk`; DMR accepts `auto`, `gfsk`. |
| `rtl_gain` | No | Per-target RTL-family tuner gain. Empty uses the global/default gain. `0` or `auto` requests device automatic gain. `1..49` requests manual dB gain. Ignored for non-RTL retuning paths. |

Validation notes:

- Maximum 32 targets.
- Duplicate IDs and duplicate `(type, frequency_hz)` rows are rejected.
- `chan_csv` on `dmr-conventional` rows is rejected.
- Global `-C`/`[trunking] chan_csv` is rejected in trunk scan mode so channel maps do not leak across systems.
- One tuner can only monitor the active target; traffic on other targets can be missed.
- This parser can handle a quoted `chan_csv` field containing a comma, but it is not a full RFC 4180 parser and does not
  support escaped quotes.

Example:

```csv
id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,modulation,rtl_gain
county-p25,p25-trunk,851012500,,3000,,primary P25 control channel,cqpsk,18
city-dmr,dmr-trunk,452012500,dmr_channels.csv,3000,,DMR Tier III control channel,auto,
plant,dmr-conventional,461112500,,1500,1200,one-frequency DMR,gfsk,auto
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
  does nothing.
- Group voice calls only. Private (unit-to-unit) calls put the destination radio ID where the
  talkgroup would be, and DMR radio IDs share the talkgroup number space, so they are never matched
  against this file. Encrypted data bursts also keep the signaled key ID.
- The OTA key ID keeps being reported in logs and event history; the map only steers key selection,
  and each applied override is announced once per call. The announcement says so when the mapped key
  ID has no imported key material — the override still stands, but nothing will decrypt.
- Duplicate talkgroups are allowed; the last occurrence replaces earlier rows.
- Maximum rows: `256`.

Example (the key IDs are the ones in `examples/multi_key_hex.csv`; a key ID with no imported key
material blocks decryption for that talkgroup rather than falling back to the signaled one):

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
