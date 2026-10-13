# TETRA tools

## Speech codec

dsd-neo decodes a TETRA traffic channel down to 137-bit ACELP speech frames and
plays the PCM returned by the command named in `TETRA_VOCODER_CMD`. The ETSI
EN 300 395-2 codec is not in this repository and the dsd-neo build does not
compile one: downloading the standard from ETSI does not make its sources
redistributable here. Do not commit the ETSI archive, its `C-CODE` tree or a
binary built from it; `tools/tetra/etsi/` and `tools/tetra/acelp_adapter/C-CODE/`
are gitignored for local copies.

The command stays running for the whole session and speaks a fixed pipe
protocol, one exchange per speech frame:

- dsd-neo writes 137 bytes to its stdin, one per bit, each 0 or 1;
- the command writes 480 bytes to stdout, 240 signed 16-bit little-endian PCM
  samples (30 ms at 8 kHz, mono), and flushes.

Two exchanges make one TCH/FS block. Diagnostics go to stderr, because any extra
stdout byte breaks the PCM framing. dsd-neo bounds each read with a two-second
deadline and reports a missing command, a start failure, short output and a
timeout as the vocoder status. Linux and macOS start the command with `sh -c`,
Windows with `cmd.exe /C`.

- `acelp_adapter/` builds that command from a local copy of the ETSI decoder:
  configure it as its own CMake project with `-DETSI_TETRA_C_CODE` pointing at
  the `C-CODE` directory, then set `TETRA_VOCODER_CMD` to the
  `tetra-acelp-adapter` executable. See `acelp_adapter/README.md`.
- `vocoder_stub.py` answers every frame with silence. The `TETRA_ACELP_*` tests
  use it to exercise the pipe, including its `--short-output` and `--hang`
  failure modes; it says nothing about speech quality.

```sh
export TETRA_VOCODER_CMD="python3 $PWD/tools/tetra/vocoder_stub.py"
dsd-neo -fT -i rtl:0:392.5M:0:0:72
```

## Crypto libraries

TETRA air-interface decryption uses one pinned upstream library, built the
same way on every platform:

| Package | Upstream | Provides |
| --- | --- | --- |
| `tetracrypto` | MidnightBlueLabs/TETRA_crypto (Apache-2.0) | TEA1, TEA2, TEA3, HURDLE, TAA1 |

Windows and Android builds get it from the overlay port in
`vcpkg-ports/tetracrypto`. Linux and macOS builds install the same CMake
project with `tools/install_tetra_crypto_deps.sh <prefix>`. Both read the
commit and archive pins in `tools/ci-dependency-pins.env`. The port
force-includes a small endian compatibility header so its sources compile
unchanged on MSVC.

## Real-air fixtures

`prepare_real_air_fixture.py` reproduces the derived TETRA real-air fixtures
in `tests/fixtures/iq` from the preserved upstream captures and checks every
input and output hash; `tests/fixtures/iq/tetra_scbs_real_air.PROVENANCE.md`
records the sources.
