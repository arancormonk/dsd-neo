# Core CSV Import Fuzz Corpus

Files in this corpus are selector-prefixed fuzz seeds, not user-facing CSV examples.

The first byte selects which importer the fuzz harness exercises, then the remaining bytes are written to a temporary
file and parsed as that CSV format:

- `selector % 9 == 0`: group list (`csvGroupImportPath`, then lookups on IDs taken from the input) -- seeded by
  `group.csv`, `group_patterns.csv` (digit patterns, ranges, exact rows), `malformed.csv` (`H`)
- `selector % 9 == 1`: channel map (`csvChanImport`) -- seeded by `channel.csv`, `channel_direct.csv`, and
  `channel_direct_conflict.csv`, `scan_options.csv` (`I`)
- `selector % 9 == 2`: decimal key list (`csvKeyImportDec`) -- seeded by `key_dec.csv` (`J`)
- `selector % 9 == 3`: hex key list (`csvKeyImportHex`) -- seeded by `key_hex.csv` (`K`)
- `selector % 9 == 4`: Vertex keystream map (`csvVertexKsImport`) -- seeded by `vertex.csv` (`L`)
- `selector % 9 == 5`: DMR talkgroup->key ID map (`csvDmrTgKeyImport`) -- seeded by `dmr_tg_key.csv` (`M`)
- `selector % 9 == 6`: P25 band plan (`csvP25BandplanImportPath`) -- seeded by `p25_bandplan.csv` (`N`)
- `selector % 9 == 7`: source ID aliases (`csvSrcImportPath`) -- seeded by `src.csv` (`O`)
- `selector % 9 == 8`: DMR Tier III channel overrides (`dsd_csv_dmr_chan_overrides_read`) -- seeded by
  `dmr_chan_overrides.csv` (`P`)

The seeds use `H`..`P` (72..80) because 72 is a multiple of 9, so each letter lands on its own
importer. Changing the modulus in `tests/fuzz/fuzz_core_csv_import.c` silently re-points every seed
at a different importer -- re-letter the first byte of each seed in the same commit, or the format a
seed is named for loses its coverage.

Use `docs/csv-formats.md` and `examples/` for copyable user CSV samples.
