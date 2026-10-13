# Local TETRA ACELP Decoder Sources

This directory is a local preparation area for the ETSI EN 300 395-2 TETRA
ACELP decoder used by `tools/tetra/acelp_adapter`.

The ETSI archive and extracted `C-CODE` tree are not redistributed by this
repository. Download `en_30039502v010301p0.zip` from ETSI, place it here, then
run:

```powershell
powershell -ExecutionPolicy Bypass -File third_party\tetra_acelp\prepare.ps1
```

The script extracts the archive to:

```text
third_party/tetra_acelp/reference/C-CODE
```

Build the adapter from the repository root:

```powershell
cmake -S tools/tetra/acelp_adapter -B build/tetra-acelp-adapter `
  -DETSI_TETRA_C_CODE="$PWD/third_party/tetra_acelp/reference/C-CODE"
cmake --build build/tetra-acelp-adapter --config Release
```

Run dsd-neo with the adapter:

```powershell
$env:TETRA_VOCODER_CMD = '"' + "$PWD\build\tetra-acelp-adapter\Release\tetra-acelp-adapter.exe" + '"'
```

If you know the live TETRA traffic colour code, optionally set:

```powershell
$env:TETRA_TCH_CC_FILTER = '2'
```

Do not commit the ETSI archive, extracted sources, patched sources, or built
adapter binaries.
