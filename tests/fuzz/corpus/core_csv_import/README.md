# Core CSV Import Fuzz Corpus

Files in this corpus are selector-prefixed fuzz seeds, not user-facing CSV examples.

The first byte selects which importer the fuzz harness exercises, then the remaining bytes are written to a temporary
file and parsed as that CSV format:

- `selector % 8 == 0`: group list (`csvGroupImportPath`) -- seeded by `group.csv`, `malformed.csv` (`H`)
- `selector % 8 == 1`: channel map (`csvChanImport`) -- seeded by `channel.csv`, `channel_direct.csv`, and
  `channel_direct_conflict.csv`, `scan_options.csv` (`I`)
- `selector % 8 == 2`: decimal key list (`csvKeyImportDec`) -- seeded by `key_dec.csv` (`J`)
- `selector % 8 == 3`: hex key list (`csvKeyImportHex`) -- seeded by `key_hex.csv` (`K`)
- `selector % 8 == 4`: Vertex keystream map (`csvVertexKsImport`) -- seeded by `vertex.csv` (`L`)
- `selector % 8 == 5`: DMR talkgroup->key ID map (`csvDmrTgKeyImport`) -- seeded by `dmr_tg_key.csv` (`M`)
- `selector % 8 == 6`: P25 band plan (`csvP25BandplanImportPath`) -- seeded by `p25_bandplan.csv` (`N`)
- `selector % 8 == 7`: source ID aliases (`csvSrcImportPath`) -- seeded by `src.csv` (`O`)

The seeds use `H`..`O` (72..79) because 72 is a multiple of 8, so each letter lands on its own
importer. Changing the modulus in `tests/fuzz/fuzz_core_csv_import.c` silently re-points every seed
at a different importer -- re-letter the first byte of each seed in the same commit, or the format a
seed is named for loses its coverage.

Use `docs/csv-formats.md` and `examples/` for copyable user CSV samples.
