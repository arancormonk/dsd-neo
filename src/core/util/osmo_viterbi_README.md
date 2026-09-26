How to drop in Osmo Viterbi sources

This project supports an optional Osmo Viterbi backend. To use the real Osmo implementation:

1. Obtain the Osmo Viterbi source file (e.g. `osmo_viterbi.c`) from the Osmo project and verify its license is compatible with your usage.
2. Place the file at: `third_party/osmo_viterbi/osmo_viterbi.c` relative to the repository root.
3. Configure the build with the CMake option `-DUSE_OSMO_VITERBI=ON` and run the normal build steps.

When the file is detected CMake will define `HAVE_OSMO_VITERBI_IMPL` during compilation and the wrapper in `src/core/util/osmo_viterbi_backend.c` will call the provided `osmo_viterbi_decode()` symbol. If the external source is not present, the repository will build a safe fallback implementation.

Notes:
- This repository does not ship Osmo sources. You are responsible for obtaining them and ensuring licensing compliance.
- The expected external symbol is `uint32_t osmo_viterbi_decode(uint8_t* out, const uint16_t* in, const uint16_t len);`. If the upstream symbol differs, you can either adapt the external file or modify `osmo_viterbi_backend.c` to match.
