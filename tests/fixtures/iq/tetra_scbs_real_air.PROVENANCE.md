# TETRA SCBS real-air fixture provenance

`tetra_scbs_real_air_50k.cs16` is the unmodified 300,000-byte capture added to
[MattCheramie/GopherTrunk](https://github.com/MattCheramie/GopherTrunk) by commit
[`825dbf0dd3a496e7a2fa5291b205201df1f065bf`](https://github.com/MattCheramie/GopherTrunk/commit/825dbf0dd3a496e7a2fa5291b205201df1f065bf).
It is redistributed under that repository's Apache License 2.0; the upstream
license text is preserved as `tetra_scbs_real_air.APACHE-2.0.txt`.

Upstream identifies it as 1.5 seconds of headerless, little-endian interleaved
CS16 IQ sampled at 50 kHz from a TETRA Single Carrier Base Station control
channel at 467.913 MHz. The upstream regression says the issue reporter made
the capture and independently checked the cell with a commercial receiver and
SDR++; expected identity is MCC 250, MNC 13, colour code `0x2C`.

- Upstream path: `cmd/gophertrunk/testdata/tetra-scbs-cc.cs16`
- Git blob SHA-1: `fdc3706761e679a836e394771f5ad88fce150064`
- Raw SHA-256: `dcd4f3dbeceb09243e20f3632d2543462a13db05fa3815354e4a597a34263fd0`
- Capture timestamp and receiver settings beyond the published values: not provided upstream

The decoder's CQPSK path currently requires an integer number of samples per
18 ksym/s symbol. `tetra_scbs_real_air_54k.iq` is therefore a deterministic
linear interpolation of the raw capture to 54 kHz (3 samples/symbol), retaining
the original 1.5-second duration. Its SHA-256 is
`601c843b35fdaa3a31e07ed402519af96b3fdf2f5433f67f6a8502385f450ea6`.
Run:

```text
python tools/tetra/prepare_real_air_fixture.py \
  --source tests/fixtures/iq/tetra_scbs_real_air_50k.cs16 \
  --output tests/fixtures/iq/tetra_scbs_real_air_54k.iq --verify
```

`tetra_cc_marginal_real_air_144k.iq` is a second unmodified Apache-2.0
capture from the same upstream repository, added by commit
[`0f9192caf36fcc2226975007f459713ed752199a`](https://github.com/MattCheramie/GopherTrunk/commit/0f9192caf36fcc2226975007f459713ed752199a).
Upstream identifies it as approximately two seconds from an Airspy
`on_cc_sync_loss` automatic recording during reacquisition of system 250/13 at
467.9125 MHz. It is interleaved little-endian CS16 at 144 kHz, with about 10 dB
in-channel SNR and an ISI-smeared constellation.

- Upstream path: `internal/scanner/ccdecoder/testdata/tetra_cc_sync_loss_2s_144k.cs16`
- Git blob SHA-1: `2636cc7489e3f855a5b69c003483a27694207e6c`
- SHA-256: `61dd6b917a390c49552e6564679e588bc047dae3dd9ec89a3a1ea4201f5d9399`
- Expected identity: MCC 250, MNC 13, colour code `0x2C`

`DECODE_IQ_TETRA_REAL_AIR_MARGINAL_CC` requires at least 15 CRC-clean identity
matches. The current Windows Debug run produces 26 from 30 detected sync
bursts. This threshold measures dsd-neo's own receiver and is not compared
numerically with the upstream implementation's equalizer A/B statistics.

`tetra_cc_marginal_real_air_144k_inverted.iq` is a deterministic complex
conjugate of that unmodified capture. It reverses every differential phase
step while retaining the source recording's noise, fading, timing error, and
ISI. CS16 Q values use saturating negation for the otherwise unrepresentable
`-INT16_MIN`; its SHA-256 is
`4e8c4ffcc3a93946857c32c888cc1e67fd7347152729a4a5c3c440806c0e890e`.
The corresponding negative-polarity regression also produces 26 identity
matches and requires at least 15. Reproduce or verify it with:

```text
python tools/tetra/prepare_real_air_fixture.py \
  --conjugate-source tests/fixtures/iq/tetra_cc_marginal_real_air_144k.iq \
  --conjugate-output tests/fixtures/iq/tetra_cc_marginal_real_air_144k_inverted.iq \
  --verify
```
