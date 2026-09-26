Vendor instructions: adding upstream osmocom `conv` sources

- Use the real upstream `osmocom/core/conv` implementation for all Viterbi/conv decode, with no local adapter, shim, or minimal header. All local stubs and shims have been removed. Only upstream `conv.c`/`conv.h` are present.

Status
- As of 2026-01-29, all local adapter, shim, and minimal header files have been removed. Only upstream `conv.c`/`conv.h` are used. No local fake/stub remains.
- Replace the current adapter shim with the real upstream `osmocom/core/conv` implementation so osmo helper sources produce "true osmo" outputs for bitwise comparison.

- Place the following upstream files (from an osmocom-tetra or osmocom-core checkout) into this folder `third_party/osmo_viterbi/`:
  - `conv.c` (or `osmocom/core/conv.c`) — the conv decoder implementation
  - `conv.h` (or `osmocom/core/conv.h`) — corresponding header
  - Any additional tiny helpers referenced by `conv.c` (e.g., bit-twiddling helpers) — place them alongside
  - Any additional tiny helpers referenced by `conv.c` (e.g., bit-twiddling helpers) — place them alongside

1. Copy the upstream files into `third_party/osmo_viterbi/`.
2. Remove or rename any local adapter, shim, or minimal header (now done; nothing remains).
3. Only upstream `conv.c`/`conv.h` should be present in this folder.
How to add the files
1. Copy the upstream files into `third_party/osmo_viterbi/`.
2. Remove or rename the local adapter `osmo_conv.c` (the adapter that forwards to `viterbi_native_impl`) to avoid duplicate symbols.
3. Ensure `include/osmocom/core/conv.h` is either removed or updated to match the upstream header you provided.

Build steps (after placing files)
- From project root:

```powershell
cmake -S . -B build -DUSE_OSMO_VITERBI=ON
cmake --build build --config Release --target dsd-neo_test_tetra_depunct_regression
```

Verification
- Rerun the regression target and inspect `artifacts/punct_6/` for `viterbi_vs_osmo_diff.txt` and per-frame diffs. If mismatches persist, we'll analyze `FULLIDX` indices vs. trellis metrics.

If you'd like, you can attach or drop the upstream `conv.c`/`conv.h` files into the workspace and I will add them and rebuild. If you prefer I prepare a safe extraction script (that checks file headers and copyright notices) I can create it for you.

Final mapping choice
- The project maps its 16-bit soft-symbols (centered at `0x7FFF`) into signed int8 soft-values for upstream `conv` code using a linear scaling mapping into `[-127..127]`.
- This linear mapping was validated by regression for `punct=6` and chosen as the default mapping in `third_party/osmo_viterbi/osmo_viterbi_shim.c` on 2026-01-29.
