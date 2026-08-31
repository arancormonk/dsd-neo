# dPMR CCH Reference Vectors

These fixtures are compact, deterministic dPMR Common Control Channel (CCH) vectors for unit tests. Normal CI consumes
only `dpmr_reference_vectors.h`; it needs no network access and no external checkout.

## Why they exist

Every stage of dsd-neo's CCH pipeline was inherited from the dsd-fme fork and had never been checked against anything
outside this tree: the CRC-7 test pinned values the implementation itself produced, and the one integration test stubbed
out `Hamming_12_8_decode()`. Nothing would have failed if the polynomial, the bit order, the interleave direction or the
Hamming matrix had been wrong — which mattered, because the CCH CRC-7 does not pass on any superframe of the committed
`dpmr` I/Q fixture (issue #407).

These vectors close that gap. They are produced by an independent model of the **encode** direction, a direction dsd-neo
never implements, so they are ground truth for the decoder rather than a snapshot of it.

## Provenance

The model follows ETSI TS 102 658 (dPMR):

- 41 CCH payload bits, then a 7-bit CRC with polynomial `X^7 + X^3 + 1`, giving 48 bits
- 48 bits as 6 bytes, each coded by a shortened Hamming(12,8) code, giving 72 bits
- a 6x12 block interleave, then the `X^9 + X^5 + 1` scrambler (clause 7.3), giving the 72 on-air bits of one CCH half

Every constant is cross-derived from [DSDcc](https://github.com/f4exb/dsdcc) at
`f27b32d2df131ae3a376fe72d3fb880ae1f9ede1` — an independent decoder that gates real dPMR traffic on this CRC, and whose
CCH stages are stage-for-stage equivalent to the ones under test here.

The generator refuses to emit vectors it disagrees with. It checks that the modelled scrambler reproduces the keystream
already committed in `tests/protocol/dpmr/test_dpmr_scrambler.c`, that `crc7(message || crc) == 0`, that every Hamming
codeword it builds has a zero syndrome, that each single-bit error yields the syndrome the correction table claims, that
exactly `{9, 13, 15}` are uncorrectable, and that the interleave round-trips.

## Vector parameters

- Two FS2 superframe parts. Frame numbers 0/1 carry the called party, 2/3 the calling party.
- Called AI ID `1806845`, dialling as `1234567`; calling AI ID `11206075`, dialling as `7654321`.
- Communication mode 1 (voice), version 0, comms format 1, no emergency, distinct 18-bit slow-data patterns per half.
- Channel code `0x5D5D55`, which is colour code 12 in the ETSI table.
- Voice dibits are deterministic filler; the vectors assert CCH content only.

## Regenerating

```sh
python3 tests/protocol/dpmr/fixtures/generate_dpmr_reference_vectors.py \
  --dsdcc /path/to/dsdcc \
  --output tests/protocol/dpmr/fixtures/dpmr_reference_vectors.h
```

`--dsdcc` is optional but is the point of the exercise: with it the helper pins DSDcc's commit and greps the exact source
lines the generator, matrix and taps are derived from, so a silent upstream change cannot go unnoticed. Output is passed
through the project's `clang-format`, so regeneration is idempotent.

`tests/protocol/dpmr/test_dpmr_reference_vectors.c` consumes the header: it checks each primitive in isolation, then
feeds a full 372-dibit frame through the real `processdPMRvoice()` with the real FEC linked in.
