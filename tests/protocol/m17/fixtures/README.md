# M17 Reference Vectors

These fixtures are compact, deterministic M17 Air Interface vectors for unit tests. Normal CI consumes only
`m17_reference_vectors.h`; it does not require GNU Radio, gr-m17, libm17, M17_Implementations, or network access.

Oracle inputs used for this fixture set:

- gr-m17: `https://github.com/M17-Project/gr-m17` at `36267b114b41920b3b62d9545afe7d7c854801bf`
- M17_Implementations: `https://github.com/M17-Project/M17_Implementations` at `f80793f169751b1fd2de6aaf8de52f862360142c`
- libm17: `https://github.com/M17-Project/libm17` at `07926d08ade8509f1ce55d0289f229ab44cbcf51`
- GNU Radio: `3.10.12.0`

AES-CTR note: libm17 does not implement encryption. gr-m17 and M17_Implementations both build the AES counter by
copying the 112-bit nonce into bytes 0-13 and using `FN & 0x7FFF` in bytes 14-15. The final transmitted frame number
`0xFFFF` therefore uses AES counter suffix `0x7F, 0xFF`, not `0xFF, 0xFF`.

Vector parameters:

- LSF: destination `N0CALL`, source `DSD-NEO`, TYPE `0x0C95`, Meta/IV bytes `00 01 ... 0D`, CRC `0x17A0`
- Stream: FN `0x0123`, payload bytes `30 31 ... 3F`, LICH chunk 0 from the LSF above
- Packet: application bytes `05 48 45 4C 4C 4F`, packet CRC `0xB6EE`, EOF metadata byte `0xA0`
- BERT: PRBS9 seed `0x001`, 197 payload bits, final LFSR `0x18F`
- AES stream: AES-128 key `00 01 ... 0F`, nonce `00 01 ... 0D`, transmitted FN `0xFFFF`, plaintext `A0 A1 ... AF`
- Signed stream: four 16-byte signature chunks at `0x7FFC`, `0x7FFD`, `0x7FFE`, `0xFFFF`

Validate fixture provenance explicitly with:

```sh
python3 tests/protocol/m17/fixtures/generate_m17_reference_vectors.py \
  --gr-m17 /path/to/gr-m17 \
  --m17-implementations /path/to/M17_Implementations \
  --libm17 /path/to/libm17 \
  --output tests/protocol/m17/fixtures/m17_reference_vectors.h
```

The helper validates the upstream commits, AES counter-mask source evidence, and required committed fixture markers, then
normalizes the fixture header newline. It is not a networked or automatic vector generator.
