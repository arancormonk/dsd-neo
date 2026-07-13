# Core CSV Import Fuzz Corpus

Files in this corpus are selector-prefixed fuzz seeds, not user-facing CSV examples.

The first byte selects which importer the fuzz harness exercises, then the remaining bytes are written to a temporary
file and parsed as that CSV format:

- `selector % 5 == 0`: group list (`csvGroupImportPath`)
- `selector % 5 == 1`: channel map (`csvChanImport`)
- `selector % 5 == 2`: decimal key list (`csvKeyImportDec`)
- `selector % 5 == 3`: hex key list (`csvKeyImportHex`)
- `selector % 5 == 4`: Vertex keystream map (`csvVertexKsImport`)

Use `docs/csv-formats.md` and `examples/` for copyable user CSV samples.
