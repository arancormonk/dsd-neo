# Core CSV Import Fuzz Corpus

Files in this corpus are selector-prefixed fuzz seeds, not user-facing CSV examples.

The first byte selects which importer the fuzz harness exercises, then the remaining bytes are written to a temporary
file and parsed as that CSV format:

- `selector % 7 == 0`: group list (`csvGroupImportPath`) -- seeded by `group.csv`, `malformed.csv` (`F`)
- `selector % 7 == 1`: channel map (`csvChanImport`) -- seeded by `channel.csv` (`G`)
- `selector % 7 == 2`: decimal key list (`csvKeyImportDec`) -- seeded by `key_dec.csv` (`H`)
- `selector % 7 == 3`: hex key list (`csvKeyImportHex`) -- seeded by `key_hex.csv` (`I`)
- `selector % 7 == 4`: Vertex keystream map (`csvVertexKsImport`) -- seeded by `vertex.csv` (`J`)
- `selector % 7 == 5`: DMR talkgroup->key ID map (`csvDmrTgKeyImport`) -- seeded by `dmr_tg_key.csv` (`K`)
- `selector % 7 == 6`: P25 band plan (`csvP25BandplanImportPath`) -- seeded by `p25_bandplan.csv` (`L`)

The seeds use `F`..`L` (70..76) because 70 is a multiple of 7, so each letter lands on its own
importer. Changing the modulus in `tests/fuzz/fuzz_core_csv_import.c` silently re-points every seed
at a different importer -- re-letter the first byte of each seed in the same commit, or the format a
seed is named for loses its coverage.

Use `docs/csv-formats.md` and `examples/` for copyable user CSV samples.
