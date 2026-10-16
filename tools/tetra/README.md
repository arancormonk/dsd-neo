# TETRA tools

## Speech

TETRA speech frames (137 bits, EN 300 395-2) decode through mbelib-neo's TETRA speech decoder
(`mbe_tetraDecodeFrame()`), which every dsd-neo build links; nothing has to be installed or named for TETRA speech to
play. A frame whose CRC failed, or that was stolen for signalling, reaches the decoder as a bad frame for it to
conceal. The speech path's tests decode through a stand-in (`tests/protocol/tetra/tetra_test_codec.c`) whose samples
say which frame they came from, and `TETRA_VOICE_CODEC` checks the output against mbelib-neo's decoder.

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
records the sources. The real-air call fixture, `tetra_call_real_air_72k`, is an
excerpt of a Signal Identification Wiki recording and is cut by
`tools/build_iq_fixtures.py --only tetra_call_real_air_72k` like the other wiki
fixtures (`docs/testing.md`).
