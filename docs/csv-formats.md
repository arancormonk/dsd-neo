# CSV Input Formats

DSD-neo uses small, purpose-built CSV importers for trunking helpers (channel maps, group lists) and key lists. These
parsers are intentionally minimal and **not** full RFC 4180 CSV parsers.

If you want known-good starting points, see `examples/` in the repository.

## General Rules (All CSV Imports)

- The **first line is treated as a header and is ignored**. Keep the header line (it can be any text).
- Fields are split on literal commas (`,`). Quoting/escaping is **not supported**.
  - Do not include commas inside a field.
- Avoid blank lines and comment-only lines (they may be parsed as data).
- Extra columns after the required ones are ignored. Use them for notes/labels.

## Channel Map CSV (`-C <file>` / `[trunking] chan_csv`)

Purpose: Map a trunking channel number to an RF frequency.

Required columns:

1. `channel_number` (decimal integer, `0 <= channel_number < 65535`)
2. `frequency_hz` (integer Hz)

Notes:

- `frequency_hz` is parsed as an integer (no `K/M/G` suffixes).
- Extra columns are ignored; use them for labels like "default CC".
- For EDACS-style workflows, DSD-neo also records the `frequency_hz` values in **row order** as an LCN frequency list,
  so keep rows in the LCN order you want.

Example:

```csv
ChannelNumber(dec),frequency(Hz),note
999,862093750,default cc
1,863093750
2,862093750
```

## Group List CSV (`-G <file>` / `[trunking] group_csv`)

Purpose: Provide labels and allow/block behavior for talkgroups.

Required columns:

1. `id` (decimal integer; talkgroup ID or radio ID depending on protocol context)
2. `mode` (string)
3. `name` (string)

Notes:

- The first line is treated as header text and is required.
- Legacy/default behavior uses only the first 3 columns; extra columns are ignored.
- `mode` is matched literally by features that consult it:
  - `A` usually means allow/normal.
  - `B` and `DE` are treated as locked out.
- Names are not CSV-escaped; avoid commas and line breaks in fields.

Extended policy columns are supported only when the header opts into this exact ordered prefix starting at column 4:

1. `priority` (0..100, default `0`)
2. `preempt` (`true`/`false`, default `false`)
3. `audio` (`on`/`off`, default from `mode`)
4. `record` (`on`/`off`, default from `mode`)
5. `stream` (`on`/`off`, default from `mode`)
6. `tags` (free text metadata)

Important behavior:

- The header must contain `priority` in column 4 and continue in that order for the available policy columns.
- If the header is legacy/unknown (for example `id,mode,name,metadata`), optional policy parsing is disabled and extra
  columns remain legacy metadata.
- `id` supports exact IDs (`1201`) and ranges (`1200-1299`).
  - Exact rows populate both runtime alias display state and policy.
  - Range rows are policy-only and are not inserted as exact aliases.
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

## DMR Tier III LCN Calculator Input (`--calc-lcn <file>`)

The `--calc-lcn` one-shot tool is more flexible than the CSV imports above:

- It scans each line for the first numeric field and treats it as a frequency.
- Frequencies may be in **Hz** (e.g., `451237500`) or **MHz** (e.g., `451.2375`).
- The output is printed to stdout as `lcn,freq` CSV.
