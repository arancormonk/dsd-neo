# Core CSV Import Fuzz Corpus

Files in this corpus are selector-prefixed fuzz seeds, not user-facing CSV examples.

The first byte selects which importer the fuzz harness exercises, then the remaining bytes are written to a temporary
file and parsed as that CSV format:

- `selector % 6 == 0`: group list (`csvGroupImportPath`)
- `selector % 6 == 1`: channel map (`csvChanImport`)
- `selector % 6 == 2`: decimal key list (`csvKeyImportDec`)
- `selector % 6 == 3`: hex key list (`csvKeyImportHex`)
- `selector % 6 == 4`: Vertex keystream map (`csvVertexKsImport`)
- `selector % 6 == 5`: DMR talkgroup->key ID map (`csvDmrTgKeyImport`)

Use `docs/csv-formats.md` and `examples/` for copyable user CSV samples.
