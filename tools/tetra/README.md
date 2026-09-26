TETRA speech codec

dsd-neo stops at a 137-bit speech frame and plays PCM from whatever command
`TETRA_VOCODER_CMD` names. The ETSI ACELP codec is not in this repository, and
the top-level CMake build does not compile one. Downloading the standard from
ETSI does not make those sources redistributable here.

- Automated tests point `TETRA_VOCODER_CMD` at `vocoder_stub.py` below. The stub
  returns silence and checks the pipe, not speech quality.
- For real speech, configure `acelp_adapter/` as its own CMake build with
  `-DETSI_TETRA_C_CODE` set to a local `C-CODE` directory, then set
  `TETRA_VOCODER_CMD` to the resulting `tetra-acelp-adapter` executable.
  A local preparation helper is in `third_party/tetra_acelp/`; build
  instructions are in `acelp_adapter/README.md`.
- Do not commit the ETSI archive, the `C-CODE` tree, or the adapter binary.
  A local checkout dropped under `tools/tetra/etsi/` or
  `acelp_adapter/C-CODE/` is gitignored.
- Linux and macOS launch the command with `sh -c`. Windows launches it with
  `cmd.exe /C`. Both sides use the same pipe: 137 bytes in (one byte per bit,
  value 0 or 1), 480 bytes out (240 PCM16LE samples at 8 kHz). Write diagnostics
  to stderr.

Vocoder stub and testing

Quick test for `TETRA_VOCODER_CMD` integration

- Make the stub executable (Linux/macOS):

```bash
chmod +x tools/tetra/vocoder_stub.py
```

- Example: run dsd-neo with the stub as the vocoder (POSIX shells):

```bash
export TETRA_VOCODER_CMD="$PWD/tools/tetra/vocoder_stub.py"
./build/<your_dsd_binary> [args]
```

- On Windows (PowerShell), set environment variable then run your dsd binary:

```powershell
$tetraPython = (Get-Command python).Source
$tetraStub = Join-Path $PWD 'tools\tetra\vocoder_stub.py'
$env:TETRA_VOCODER_CMD = '"{0}" "{1}"' -f $tetraPython, $tetraStub
# run your binary from the same session
```

Notes:
- The stub writes raw 16-bit little-endian PCM silence frames to stdout.
- Replace `TETRA_VOCODER_CMD` with the path to a real command-line vocoder when available.
- The command stays running across frames. For each frame it reads exactly
  137 bytes from stdin (one byte per bit, each 0 or 1), writes exactly 480 bytes
  (240 samples, 30 ms) of signed PCM16LE at 8 kHz mono, and flushes stdout. Two
  such exchanges make one decoded TCH/FS block pair.
- Send diagnostics to stderr; extra stdout bytes corrupt PCM framing.
- The decoder bounds PCM reads with a two-second deadline. Missing commands,
  short output and timeout are covered by the `TETRA_ACELP_*` CTest cases.
- The stub produces silence and does not establish ACELP speech correctness.
  Real voice acceptance requires a real codec and an independently checked
  audio reference. See [the six-stage checklist](../../docs/tetra-implementation-status.md).

## Field-capture acceptance

`verify_field_capture.py` checks a retuned IQ evidence bundle before it is used
to close the hardware trunk-tracking gate. A structural check validates the v2
event timeline, exact data size, sample alignment, zero capture/input drops,
and a captured CC-to-VC-to-CC sequence:

```bash
python tools/tetra/verify_field_capture.py capture.iq.json \
  --cc-hz 390000000 --vc-hz 391000000
```

The acceptance form additionally requires a provenance document based on
`FIELD_CAPTURE_PROVENANCE.template.md` and a log from
realtime replay. The log must contain, in order, a BSCH decode, channel
allocation, TCH/FS decode, and a later BSCH decode after the return. It writes a
machine-readable report containing hashes for all evidence files:

```bash
dsd-neo --iq-replay capture.iq.json --iq-replay-rate realtime -fT \
  --frontend terminal >capture.log 2>&1
python tools/tetra/verify_field_capture.py capture.iq.json \
  --cc-hz 390000000 --vc-hz 391000000 \
  --decode-log capture.log --provenance PROVENANCE.md --acceptance \
  --report ACCEPTANCE.json
```
