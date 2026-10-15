# TETRA speech codec library from the ETSI reference code

This directory builds a TETRA speech codec library for dsd-neo's `--tetra-codec` option from a locally obtained
copy of the ETSI EN 300 395-2 reference C code. The ETSI sources are **not** part of this repository and the dsd-neo
build never compiles them: this is a separate CMake project you point at your own copy. Do not commit the ETSI
archive, its `C-CODE` tree or a library built from it; `tools/tetra/etsi/` and `tools/tetra/acelp_adapter/C-CODE/`
are ignored by git for local copies.

`tetra_codec_etsi.c` implements the four functions of the codec interface in
`include/dsd-neo/protocol/tetra/tetra_codec_abi.h` over the reference source decoder: each 137-bit speech frame
dsd-neo passes, with its bad-frame flag, becomes the decoder's serial input, and its 240 samples at 8 kHz come back.
A frame whose CRC failed is concealed by the decoder, as the standard describes. The reference decoder keeps its state
in file-scope variables, so the library serves one decoder at a time, which is how dsd-neo uses it.

## Getting the sources

Download `en_30039502v010301p0.zip` (EN 300 395-2 V1.3.1) from ETSI and extract its `C-CODE` directory. The code
predates 64-bit platforms and takes `long` to be 32 bits wide. On 64-bit Linux and macOS, where `long` is 64 bits, its
32-bit fixed-point type and the `long` variables and casts that stand for it have to become `int` before building, or
the decoder no longer computes what the standard specifies. 64-bit Windows keeps `long` at 32 bits and needs no
change. The project accepts the file names in the archive's upper case or in lower case; if your compiler cannot find
a header one of the sources includes, the `#include` line and the file name differ in case and one of them needs
renaming.

## Building

Linux and macOS:

```sh
cmake -S tools/tetra/acelp_adapter -B build/tetra-codec -DETSI_TETRA_C_CODE=/path/to/C-CODE -DCMAKE_BUILD_TYPE=Release
cmake --build build/tetra-codec
dsd-neo -fT --tetra-codec build/tetra-codec/libtetra-codec.so -i rtl:0:392.5M:0:0:72   # .dylib on macOS
```

Windows, from a Visual Studio developer prompt (PowerShell):

```powershell
cmake -S tools/tetra/acelp_adapter -B build/tetra-codec -DETSI_TETRA_C_CODE="C:\path\to\C-CODE"
cmake --build build/tetra-codec --config Release
dsd-neo -fT --tetra-codec build\tetra-codec\Release\tetra-codec.dll -i rtl:0:392.5M:0:0:72
```

The library must match dsd-neo's architecture (a 64-bit dsd-neo loads only a 64-bit library). dsd-neo loads it the
first time TETRA speech needs decoding and logs one line when it does. A library it cannot use is refused with one
error naming the reason, and TETRA decoding carries on without audio.

## Checking the build

Configure with `-DBUILD_REFERENCE_SDECODER=ON` to also build the ETSI file decoder, `tetra-reference-sdecoder`, and
compare its PCM with what the library produces for the same frames. That checks the adaptation, not the codec's
conformance: validate against the ETSI conformance vectors before relying on the speech. Review the ETSI terms before
sharing a library built from the reference code.
