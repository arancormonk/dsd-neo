# Core CSV Import Fuzz Corpus

Files in this corpus are selector-prefixed fuzz seeds, not user-facing CSV examples.

The first byte selects which importer the fuzz harness exercises, then the remaining bytes are written to a temporary
file and parsed as that CSV format:

- `selector % 6 == 0`: group list (`csvGroupImportPath`) -- seeded by `group.csv`, `malformed.csv` (`B`)
- `selector % 6 == 1`: channel map (`csvChanImport`) -- seeded by `channel.csv` (`C`)
- `selector % 6 == 2`: decimal key list (`csvKeyImportDec`) -- seeded by `key_dec.csv` (`D`)
- `selector % 6 == 3`: hex key list (`csvKeyImportHex`) -- seeded by `key_hex.csv` (`E`)
- `selector % 6 == 4`: Vertex keystream map (`csvVertexKsImport`) -- seeded by `vertex.csv` (`F`)
- `selector % 6 == 5`: DMR talkgroup->key ID map (`csvDmrTgKeyImport`) -- seeded by `dmr_tg_key.csv` (`G`)

The seeds use `B`..`G` (66..71) because 66 is a multiple of 6, so each letter lands on its own
importer. Changing the modulus in `tests/fuzz/fuzz_core_csv_import.c` silently re-points every seed
at a different importer -- re-letter the first byte of each seed in the same commit, or the format a
seed is named for loses its coverage.

Use `docs/csv-formats.md` and `examples/` for copyable user CSV samples.
