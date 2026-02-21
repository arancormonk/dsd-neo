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

- Only the first 3 columns are used; extra columns are ignored.
- `mode` is matched literally by features that consult it:
  - `A` usually means allow/normal.
  - `B` and `DE` are treated as locked out in the UI helpers.
- Names are not CSV-escaped; avoid commas in `name`.

Example:

```csv
DEC,Mode(A=Allow; B=Block; DE=Enc),Name,Tag
1449,A,Fire Dispatch,Fire
22033,DE,Law Dispatch,Law
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

## DMR Tier III LCN Calculator Input (`--calc-lcn <file>`)

The `--calc-lcn` one-shot tool is more flexible than the CSV imports above:

- It scans each line for the first numeric field and treats it as a frequency.
- Frequencies may be in **Hz** (e.g., `451237500`) or **MHz** (e.g., `451.2375`).
- The output is printed to stdout as `lcn,freq` CSV.

