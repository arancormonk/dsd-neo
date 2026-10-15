# TETRA tools

## Speech codec

dsd-neo decodes a TETRA traffic channel down to 137-bit speech frames, checks each burst's CRC and plays the PCM a
speech codec library makes of them. The ETSI EN 300 395-2 codec is not in this repository and the dsd-neo build does
not compile one: downloading the standard from ETSI does not make its sources redistributable here. A user who has a
codec names its shared library with `--tetra-codec <file>`; without one TETRA decodes as usual and plays no speech.

The library exports four C functions, described in `include/dsd-neo/protocol/tetra/tetra_codec_abi.h`:

| Function | Does |
| --- | --- |
| `uint32_t dsd_tetra_codec_abi_version(void)` | Returns the interface version, 1 |
| `void* dsd_tetra_codec_create(void)` | Makes a decoder in its reset state, or returns NULL |
| `int dsd_tetra_codec_decode(void* codec, const uint8_t* bits, int bad_frame, int16_t* pcm)` | Decodes one speech frame: 137 bytes, each 0 or 1, to 240 samples at 8 kHz; returns 0 on success |
| `void dsd_tetra_codec_destroy(void* codec)` | Frees a decoder |

dsd-neo loads the library once, on its decoder thread, the first time TETRA speech needs decoding, and refuses one
that reports another interface version or lacks a function, with one error naming the reason. A frame whose CRC failed
reaches the codec with `bad_frame` set, for the codec to conceal. The library is opened by the path given, relative to
the working directory when it is not absolute, never from the system's library search path.

- `acelp_adapter/` builds such a library from a local copy of the ETSI reference decoder on Linux, macOS and
  Windows: configure it as its own CMake project with `-DETSI_TETRA_C_CODE` pointing at the `C-CODE` directory, then
  pass the library it builds to `--tetra-codec`. See `acelp_adapter/README.md`. Do not commit the ETSI archive, its
  `C-CODE` tree or a library built from it; `tools/tetra/etsi/` and `tools/tetra/acelp_adapter/C-CODE/` are ignored by
  git for local copies.
- The tests use a codec of their own (`tests/protocol/tetra/tetra_test_codec.c`) that makes no speech, only samples
  that say which frame they came from; it says nothing about speech quality.

```sh
dsd-neo -fT --tetra-codec /path/to/libtetra-codec.so -i rtl:0:392.5M:0:0:72
```

## Crypto libraries

TETRA air-interface decryption uses the TEA1, TEA2 and TEA3 keystream
generators from MidnightBlueLabs/TETRA_crypto (Apache-2.0). Like PFFFT, the
build downloads the archive pinned in `tools/ci-dependency-pins.env`, checks its
SHA-256 and compiles `tea1.c`, `tea2.c` and `tea3.c` into the internal
`dsd-neo_tetra_crypto` target on every platform, so no package has to be
installed first. `src/crypto/tetra_tea.c` is the only file that includes them
and packs the frame-number IV itself. Offline builds, updates and AUR packaging
are described in `docs/dependencies.md` ("Pinned TETRA_crypto source").

## Real-air fixtures

`prepare_real_air_fixture.py` reproduces the derived TETRA real-air fixtures
in `tests/fixtures/iq` from the preserved upstream captures and checks every
input and output hash; `tests/fixtures/iq/tetra_scbs_real_air.PROVENANCE.md`
records the sources.
