# Standalone TETRA ACELP adapter

This program connects a locally obtained ETSI EN 300 395-2 speech decoder to
dsd-neo's `TETRA_VOCODER_CMD` pipe protocol. ETSI sources are **not** included
in this repository. Obtain the `en_30039502v010301p0.zip` archive from ETSI
and extract its `C-CODE` sources separately; osmo-tetra provides a patch set.
The original uppercase `C-CODE` files compile with MSVC in this adapter. On
platforms where `long` is 64 bits, apply osmo-tetra's 64-bit and symbol patches
first, then use matching source filename case in `CMakeLists.txt`.

The repository provides a local preparation helper under
`third_party/tetra_acelp/`. Put `en_30039502v010301p0.zip` there and run:

```powershell
powershell -ExecutionPolicy Bypass -File third_party\tetra_acelp\prepare.ps1
```

Build on Windows with an MSVC developer environment:

```powershell
cmake -S tools/tetra/acelp_adapter -B build/tetra-acelp-adapter `
  -DETSI_TETRA_C_CODE="$PWD/third_party/tetra_acelp/reference/C-CODE"
cmake --build build/tetra-acelp-adapter --config Release
$env:TETRA_VOCODER_CMD = '"E:\Git\dsd-neo\build\tetra-acelp-adapter\Release\tetra-acelp-adapter.exe"'
# Start dsd-neo from this PowerShell session.
```

When the CB colour code is known from a strong recurring TCH stream, set
`TETRA_TCH_CC_FILTER` to `0`, `1`, `2`, or `3` before starting dsd-neo. This
keeps other colour-code bursts out of the stateful speech decoder. It does not
replace voice-channel assignment or repair missing radio frames.

The adapter reads 137 bytes per frame (each byte 0 or 1), prepends a valid-frame
indicator for the ETSI decoder, and writes 240 signed 16-bit little-endian PCM
samples per frame at 8 kHz. It preserves decoder state across frames. It cannot
recover encrypted voice or mark bad frames because the current dsd-neo pipe
protocol carries neither keys nor a bad-frame indicator.

For a local comparison, configure with `-DBUILD_REFERENCE_SDECODER=ON` and
compare identical frames against `tetra-reference-sdecoder`. A 12-frame pipe
comparison on MSVC produced byte-identical PCM. This checks adaptation, not
codec conformance or correctness of live-air bit order. Validate with ETSI
conformance vectors before claiming speech correctness. Do not publish the ETSI
source or a derivative binary without reviewing its applicable rights.
