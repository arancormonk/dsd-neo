# DMR TG→Key-ID Map: Deferred Findings Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the `--dmr-tg-key-csv` map apply a row only when the mapped key ID actually holds the kind of key material the call's ALG needs, and close the five satellite defects the same review left open.

**Architecture:** A new shared enum `dsd_key_material_need` names what an ALG requires. `keyring.c` answers "does key ID *K* satisfy need *N*?" as a pure predicate; `dsd_audio_gate.c` owns the single voice ALGID→need table; `dmr_block_crypto.c` translates its own data-ALG numbering locally. The three map resolver entry points gain a `need` parameter, and every call site computes its own. No module learns another module's ALG numbering.

**Tech Stack:** C11, CMake ≥ 3.20, CTest. Build with `cmake --build --preset dev-debug -j`, test with `ctest --preset dev-debug -R <NAME> --output-on-failure`.

**Spec:** `docs/superpowers/specs/2026-08-19-dmr-tg-key-map-deferred-findings-design.md`

## Global Constraints

- Include project headers only as `#include <dsd-neo/<module>/<header>>`.
- All project files carry an SPDX identifier. New public headers under `include/dsd-neo/core/` use `// SPDX-License-Identifier: GPL-3.0-or-later` and the copyright line `Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>`.
- `.clang-format`: LLVM-based, 4-space indent, 120 columns, braces on **all** control statements. Run `tools/format.sh` before every commit.
- Warnings are errors (`DSD_WARNINGS_AS_ERRORS=ON`). `.clang-tidy` promotes `clang-analyzer-*`, `security-*`, `bugprone-*`, `performance-*`, `portability-*` to errors.
- Use `<dsd-neo/core/safe_api.h>` wrappers (`DSD_MEMSET`, `DSD_FPRINTF`, `DSD_SNPRINTF`, …). Raw C memory/string/formatting APIs are blocked by `semgrep/dsd-neo.yml`.
- No `exit()` in `src/`, `include/`, `apps/`. No `assert()` in production code.
- Every commit needs a DCO sign-off: `git commit -s`.
- Commit message style, from `git log`: `<scope>: <lowercase imperative>` — e.g. `dmr: gate the tg key map on the alg's material need`.
- Enum `switch` statements list every enumerator and place the fallback **after** the switch rather than using `default:` — this satisfies `-Wswitch` without tripping covered-switch-default.
- `src/core/vocoder/keyring.c` must gain **zero** project-level undefined symbols. `tests/ui/test_ui_menu_services.c` links it standalone alongside `src/app_control/menu_services.c`; pulling any other project TU into `keyring.c` breaks `UI_MENU_SERVICES`.

---

### Task 1: The `need` vocabulary and its keyring predicate

**Files:**
- Create: `include/dsd-neo/core/key_material.h`
- Modify: `include/dsd-neo/core/keyring.h`
- Modify: `src/core/vocoder/keyring.c`
- Test: `tests/core/test_core_dmr_tg_key_map.c`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  - `typedef enum { DSD_KEY_NEED_NONE = 0, DSD_KEY_NEED_SCALAR, DSD_KEY_NEED_AES_2, DSD_KEY_NEED_AES_3, DSD_KEY_NEED_AES_4, DSD_KEY_NEED_QUARTET } dsd_key_material_need;` in `<dsd-neo/core/key_material.h>`
  - `int keyring_kid_satisfies_need(const dsd_state* state, int key_id, dsd_key_material_need need);` in `<dsd-neo/core/keyring.h>` — returns 1 when the key ID holds material satisfying `need`, 0 otherwise (including for `DSD_KEY_NEED_NONE` and a NULL `state`).

- [ ] **Step 1: Write the failing test**

Append to `tests/core/test_core_dmr_tg_key_map.c`, immediately before `main()`:

```c
// The gate that decides whether a --dmr-tg-key-csv row applies must ask what the call's ALG
// actually needs. Two failures ride on this. Aliasing: AES segments live at
// kid + {0x000, 0x101, 0x201, 0x301} in one flat rkey_array, so segment N of key K is the same
// cell as the scalar of key K + offset -- key 0x0C "has AES material" whenever an unrelated key
// sits at 0x10D. Kind: a scalar is not an AES key, and treating one as the other zeroes the slot
// key and arms a session-permanent lockout on a call the signaled key would have decrypted.
static int
test_kid_satisfies_need(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    int rc = 0;

    // A scalar-only key id.
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    rc |= expect_eq("need-scalar-hit", keyring_kid_satisfies_need(&state, 0x03, DSD_KEY_NEED_SCALAR), 1);
    rc |= expect_eq("need-aes2-miss", keyring_kid_satisfies_need(&state, 0x03, DSD_KEY_NEED_AES_2), 0);
    rc |= expect_eq("need-quartet-miss", keyring_kid_satisfies_need(&state, 0x03, DSD_KEY_NEED_QUARTET), 0);

    // The alias: nothing imported at 0x0C, an unrelated scalar parked on its segment-1 cell.
    state.rkey_array[0x10D] = 0xDEADULL;
    state.rkey_array_loaded[0x10D] = 1U;
    rc |= expect_eq("need-alias-scalar", keyring_kid_satisfies_need(&state, 0x0C, DSD_KEY_NEED_SCALAR), 0);
    rc |= expect_eq("need-alias-aes2", keyring_kid_satisfies_need(&state, 0x0C, DSD_KEY_NEED_AES_2), 0);

    // A real AES-128 import: two contiguous segments.
    state.rkey_array[0x40] = 0x1111ULL;
    state.rkey_array_loaded[0x40] = 1U;
    state.rkey_array[0x141] = 0x2222ULL;
    state.rkey_array_loaded[0x141] = 1U;
    rc |= expect_eq("need-aes2-hit", keyring_kid_satisfies_need(&state, 0x40, DSD_KEY_NEED_AES_2), 1);
    rc |= expect_eq("need-aes3-miss", keyring_kid_satisfies_need(&state, 0x40, DSD_KEY_NEED_AES_3), 0);
    rc |= expect_eq("need-aes4-miss", keyring_kid_satisfies_need(&state, 0x40, DSD_KEY_NEED_AES_4), 0);

    // Extended to AES-256.
    state.rkey_array[0x241] = 0x3333ULL;
    state.rkey_array_loaded[0x241] = 1U;
    state.rkey_array[0x341] = 0x4444ULL;
    state.rkey_array_loaded[0x341] = 1U;
    rc |= expect_eq("need-aes3-hit", keyring_kid_satisfies_need(&state, 0x40, DSD_KEY_NEED_AES_3), 1);
    rc |= expect_eq("need-aes4-hit", keyring_kid_satisfies_need(&state, 0x40, DSD_KEY_NEED_AES_4), 1);
    rc |= expect_eq("need-quartet-hit", keyring_kid_satisfies_need(&state, 0x40, DSD_KEY_NEED_QUARTET), 1);

    // A loaded-but-zero cell activates as A_i == 0, so it must not satisfy an AES need. This is
    // the distinction keyring_kid_kirisun_complete()'s comment already draws against
    // keyring_aes_segments_complete(): classification has to predict activation.
    state.rkey_array[0x141] = 0ULL;
    rc |= expect_eq("need-aes2-loaded-zero", keyring_kid_satisfies_need(&state, 0x40, DSD_KEY_NEED_AES_2), 0);

    // NONE never maps, and a NULL state never maps.
    rc |= expect_eq("need-none", keyring_kid_satisfies_need(&state, 0x03, DSD_KEY_NEED_NONE), 0);
    rc |= expect_eq("need-null-state", keyring_kid_satisfies_need(NULL, 0x03, DSD_KEY_NEED_SCALAR), 0);
    return rc;
}
```

Register it in `main()` by adding this line directly after `rc |= test_kid_material_reports_without_activating();`:

```c
    rc |= test_kid_satisfies_need();
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -20
```

Expected: FAIL at compile time — `implicit declaration of function 'keyring_kid_satisfies_need'` and `'DSD_KEY_NEED_SCALAR' undeclared`.

- [ ] **Step 3: Create the shared header**

Create `include/dsd-neo/core/key_material.h`:

```c
// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief What kind of key material an encryption algorithm needs.
 *
 * Shared vocabulary, not keyring-specific: the ALG tables live with the modules that own their
 * numbering (voice IDs in core/audio, DMR data IDs in protocol/dmr), while the keyring answers
 * whether a key ID satisfies a need. Its own header so that including it costs nothing --
 * <dsd-neo/core/audio.h> otherwise pulls only forward-declaration headers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_MATERIAL_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_MATERIAL_H_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Key material an ALG requires before a key ID can serve it.
 *
 * DSD_KEY_NEED_AES_4 and DSD_KEY_NEED_QUARTET are not redundant. AES_4 asks only that all four
 * segment cells be non-zero; QUARTET additionally requires keyring_aes_segment_count() to report
 * four, which is what keyring_kid_kirisun_complete() predicts about activation. Four non-zero
 * cells of which only two are flagged rkey_array_loaded satisfies the first and not the second.
 */
typedef enum {
    DSD_KEY_NEED_NONE = 0, /**< ALG consumes no keyring material; a map row cannot help it. */
    DSD_KEY_NEED_SCALAR,   /**< rkey_array[kid] non-zero. */
    DSD_KEY_NEED_AES_2,    /**< First 2 segments non-zero (DMR AES-128, P25 AES-128). */
    DSD_KEY_NEED_AES_3,    /**< First 3 segments non-zero (P25 TDEA). */
    DSD_KEY_NEED_AES_4,    /**< All 4 segments non-zero (DMR/P25 AES-256). */
    DSD_KEY_NEED_QUARTET,  /**< Kirisun 0x36/0x37: a complete, all-non-zero quartet. */
} dsd_key_material_need;

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_KEY_MATERIAL_H_H */
```

- [ ] **Step 4: Declare the predicate**

In `include/dsd-neo/core/keyring.h`, add to the include block (keeping alphabetical order, so directly after `#include <dsd-neo/core/call_state.h>`):

```c
#include <dsd-neo/core/key_material.h>
```

Then add this declaration directly after the `keyring_kid_kirisun_complete()` declaration:

```c
/**
 * @brief Whether an imported key ID holds the material @p need calls for.
 *
 * The gate `keyring_dmr_effective_kid()` applies to a --dmr-tg-key-csv row. "Has any bytes at
 * all" is the wrong question twice over: a scalar cannot serve an AES ALG, and the flat
 * rkey_array aliases segment N of key K onto the scalar of key K + offset, so one unrelated
 * import can make an empty key ID look populated. Requiring the ALG's actual material answers
 * both -- an accidental match then needs as many colliding keys as the ALG needs segments.
 *
 * Tests non-zero segment cells rather than rkey_array_loaded, because a loaded-but-zero cell
 * activates as A_i == 0. Classification must predict activation.
 *
 * @return 1 when @p key_id satisfies @p need; 0 otherwise, including for DSD_KEY_NEED_NONE.
 */
int keyring_kid_satisfies_need(const dsd_state* state, int key_id, dsd_key_material_need need);
```

- [ ] **Step 5: Implement the predicate**

In `src/core/vocoder/keyring.c`, add this directly after `keyring_kid_material()`:

```c
// All of the first `count` AES segment cells non-zero. Distinct from
// keyring_aes_segments_complete(), which accepts a cell that is rkey_array_loaded but zero-valued:
// such a cell activates as A_i == 0, so accepting it here would let the map install a key the
// decryptability gate then rejects -- exactly the divergence this predicate exists to prevent.
static int
keyring_aes_segments_nonzero(const dsd_state* state, int key_id, unsigned int count) {
    if (state == NULL || count > 4U) {
        return 0;
    }
    for (unsigned int i = 0; i < count; i++) {
        if (keyring_rkey_value(state, key_id + k_aes_segment_offsets[i]) == 0ULL) {
            return 0;
        }
    }
    return 1;
}

int
keyring_kid_satisfies_need(const dsd_state* state, int key_id, dsd_key_material_need need) {
    if (state == NULL) {
        return 0;
    }
    switch (need) {
        case DSD_KEY_NEED_SCALAR: return keyring_rkey_value(state, key_id) != 0ULL ? 1 : 0;
        case DSD_KEY_NEED_AES_2: return keyring_aes_segments_nonzero(state, key_id, 2U);
        case DSD_KEY_NEED_AES_3: return keyring_aes_segments_nonzero(state, key_id, 3U);
        case DSD_KEY_NEED_AES_4: return keyring_aes_segments_nonzero(state, key_id, 4U);
        case DSD_KEY_NEED_QUARTET: return keyring_kid_kirisun_complete(state, key_id);
        case DSD_KEY_NEED_NONE: break;
    }
    return 0;
}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -5
ctest --preset dev-debug -R CORE_DMR_TG_KEY_MAP --output-on-failure
```

Expected: PASS, `CORE_DMR_TG_KEY_MAP: OK`.

- [ ] **Step 7: Confirm the keyring stayed link-clean**

```bash
ctest --preset dev-debug -R UI_MENU_SERVICES --output-on-failure
```

Expected: PASS. If this fails to link, `keyring_aes_segments_nonzero()` reached outside `keyring.c` — it must use only that file's existing statics (`keyring_rkey_value`, `k_aes_segment_offsets`).

- [ ] **Step 8: Format and commit**

```bash
tools/format.sh
git add include/dsd-neo/core/key_material.h include/dsd-neo/core/keyring.h src/core/vocoder/keyring.c tests/core/test_core_dmr_tg_key_map.c
git commit -s -m "core: add a key-material need vocabulary and its keyring predicate"
```

---

### Task 2: The single voice ALGID table

**Files:**
- Modify: `include/dsd-neo/core/audio.h`
- Modify: `src/core/audio/dsd_audio_gate.c:159-197`
- Test: `tests/core/test_core_dmr_voice_alg_gate.c`

**Interfaces:**
- Consumes: `dsd_key_material_need` from Task 1.
- Produces: `dsd_key_material_need dsd_dmr_alg_key_need(int algid);` in `<dsd-neo/core/audio.h>`.

**Critical:** do not edit any existing assertion in `tests/core/test_core_dmr_voice_alg_gate.c`. They are the regression guard proving the `dsd_dmr_voice_alg_can_decrypt()` rewrite is behavior-preserving. Only append.

- [ ] **Step 1: Write the failing test**

In `tests/core/test_core_dmr_voice_alg_gate.c`, insert this block inside `main()` directly before the closing `if (rc == 0) {`:

```c
    // One table, not two. dsd_dmr_voice_alg_can_decrypt() is derived from this, so the ALG
    // knowledge the map gate consults and the knowledge the decryptability gate consults cannot
    // drift apart.
    rc |= expect_eq("need-hytera", (int)dsd_dmr_alg_key_need(0x02), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-dmr-rc4", (int)dsd_dmr_alg_key_need(0x21), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-dmr-des", (int)dsd_dmr_alg_key_need(0x22), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-p25-des", (int)dsd_dmr_alg_key_need(0x81), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-p25-desxl", (int)dsd_dmr_alg_key_need(0x9F), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-p25-rc4", (int)dsd_dmr_alg_key_need(0xAA), (int)DSD_KEY_NEED_SCALAR);
    rc |= expect_eq("need-dmr-aes128", (int)dsd_dmr_alg_key_need(0x24), (int)DSD_KEY_NEED_AES_2);
    rc |= expect_eq("need-p25-aes128", (int)dsd_dmr_alg_key_need(0x89), (int)DSD_KEY_NEED_AES_2);
    rc |= expect_eq("need-p25-tdea", (int)dsd_dmr_alg_key_need(0x83), (int)DSD_KEY_NEED_AES_3);
    rc |= expect_eq("need-dmr-aes256", (int)dsd_dmr_alg_key_need(0x25), (int)DSD_KEY_NEED_AES_4);
    rc |= expect_eq("need-p25-aes256", (int)dsd_dmr_alg_key_need(0x84), (int)DSD_KEY_NEED_AES_4);
    rc |= expect_eq("need-kirisun36", (int)dsd_dmr_alg_key_need(0x36), (int)DSD_KEY_NEED_QUARTET);
    rc |= expect_eq("need-kirisun37", (int)dsd_dmr_alg_key_need(0x37), (int)DSD_KEY_NEED_QUARTET);
    // Unclassified ALGs cannot be decrypted, so a map row cannot help them.
    rc |= expect_eq("need-clear", (int)dsd_dmr_alg_key_need(0x00), (int)DSD_KEY_NEED_NONE);
    rc |= expect_eq("need-vertex", (int)dsd_dmr_alg_key_need(0x07), (int)DSD_KEY_NEED_NONE);
    rc |= expect_eq("need-scrambler", (int)dsd_dmr_alg_key_need(0x80), (int)DSD_KEY_NEED_NONE);
    rc |= expect_eq("need-unknown", (int)dsd_dmr_alg_key_need(0x7E), (int)DSD_KEY_NEED_NONE);
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -20
```

Expected: FAIL at compile time — `implicit declaration of function 'dsd_dmr_alg_key_need'`.

- [ ] **Step 3: Declare the accessor**

In `include/dsd-neo/core/audio.h`, add to the include block:

```c
#include <dsd-neo/core/key_material.h>
```

Then add directly above the existing `dsd_dmr_voice_alg_can_decrypt()` declaration:

```c
/**
 * @brief Key material the DMR/P25 voice ALGID @p algid requires.
 *
 * The project's single voice ALGID table. dsd_dmr_voice_alg_can_decrypt() is derived from it, and
 * the --dmr-tg-key-csv resolver gates on it, so the ALG knowledge behind "can this key decrypt?"
 * and behind "may this map row apply?" is one table rather than two that can drift.
 *
 * Unclassified ALGIDs return DSD_KEY_NEED_NONE, which reads as "no keyring material selects this"
 * -- consistent with dsd_dmr_voice_alg_can_decrypt() already reporting 0 for them.
 */
dsd_key_material_need dsd_dmr_alg_key_need(int algid);
```

- [ ] **Step 4: Implement it and derive the existing predicate**

In `src/core/audio/dsd_audio_gate.c`, add `#include <dsd-neo/core/key_material.h>` to the include block. Then replace the whole region from `static int dsd_alg_list_contains(...)` through the end of `dsd_dmr_voice_alg_can_decrypt()` (currently lines 159-197) with:

```c
dsd_key_material_need
dsd_dmr_alg_key_need(int algid) {
    switch (algid) {
        case 0x02: /* Hytera Enhanced */
        case 0x21: /* DMR RC4 */
        case 0x22: /* DMR DES */
        case 0x81: /* P25 DES */
        case 0x9F: /* P25 DES-XL */
        case 0xAA: /* P25 RC4 */
            return DSD_KEY_NEED_SCALAR;
        case 0x24: /* DMR AES-128 */
        case 0x89: /* P25 AES-128 */
            return DSD_KEY_NEED_AES_2;
        case 0x83: /* P25 TDEA */
            return DSD_KEY_NEED_AES_3;
        case 0x25: /* DMR AES-256 */
        case 0x84: /* P25 AES-256 */
            return DSD_KEY_NEED_AES_4;
        case 0x36: /* Kirisun */
        case 0x37: /* Kirisun */
            return DSD_KEY_NEED_QUARTET;
        default: return DSD_KEY_NEED_NONE;
    }
}

int
dsd_dmr_voice_alg_can_decrypt(int algid, unsigned long long r_key, int aes_loaded) {
    switch (dsd_dmr_alg_key_need(algid)) {
        case DSD_KEY_NEED_SCALAR: return (r_key != 0ULL) ? 1 : 0;
        case DSD_KEY_NEED_AES_2:
        case DSD_KEY_NEED_AES_3:
        case DSD_KEY_NEED_AES_4: return (aes_loaded == 1) ? 1 : 0;
        // Kirisun decides on the quartet, which this signature cannot see: only
        // dsd_dmr_voice_kid_can_decrypt() and dsd_dmr_voice_slot_can_decrypt() answer that family.
        case DSD_KEY_NEED_QUARTET:
        case DSD_KEY_NEED_NONE: break;
    }
    return 0;
}
```

The `kRKeyAlgs` and `kAesLoadedAlgs` static arrays and `dsd_alg_list_contains()` are deleted — `dsd_alg_list_contains()` has no other caller in the file (verify with `grep -n dsd_alg_list_contains src/core/audio/dsd_audio_gate.c`, which must return nothing after the edit).

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -5
ctest --preset dev-debug -R CORE_DMR_VOICE_ALG_GATE --output-on-failure
```

Expected: PASS, `CORE_DMR_VOICE_ALG_GATE: OK`. Every pre-existing assertion must pass **unmodified** — in particular `kirisun-generic-needs-slot` (`0x36` → 0) and `vertex-unknown` (`0x07` → 0).

- [ ] **Step 6: Confirm the audio2 stub list is unchanged**

```bash
ctest --preset dev-debug -R CORE_AUDIO2_HELPERS --output-on-failure
```

Expected: PASS. `tests/core/test_core_audio2_helpers.c:294` stubs `dsd_dmr_voice_slot_can_decrypt`, meaning that target links `dsd_audio2.c` without `dsd_audio_gate.c`. If it fails to link, `dsd_audio2.c` gained a reference to `dsd_dmr_alg_key_need()` — it should not; revert that reference rather than growing the stub list.

- [ ] **Step 7: Format and commit**

```bash
tools/format.sh
git add include/dsd-neo/core/audio.h src/core/audio/dsd_audio_gate.c tests/core/test_core_dmr_voice_alg_gate.c
git commit -s -m "core: derive the dmr voice alg gate from one alg-need table"
```

---

### Task 3: Thread the need through the map resolver

**Files:**
- Modify: `include/dsd-neo/core/keyring.h`
- Modify: `src/core/vocoder/keyring.c`
- Modify: `src/core/vocoder/dsd_mbe.c:353`
- Modify: `src/protocol/dmr/dmr_flco.c:644`
- Modify: `src/protocol/dmr/dmr_pi.c:247`
- Modify: `src/protocol/dmr/dmr_block_crypto.c:127`
- Modify: `src/core/file/dsd_file.c:1606`, `:1888`, `:1977`
- Test: `tests/protocol/dmr/test_dmr_block_crypto_key_map.c`, `tests/protocol/dmr/test_dmr_flco_privacy_modes.c`, `tests/core/test_core_dmr_tg_key_map.c`

**Interfaces:**
- Consumes: `keyring_kid_satisfies_need()` (Task 1), `dsd_dmr_alg_key_need()` (Task 2).
- Produces, all in `<dsd-neo/core/keyring.h>`:
  - `uint8_t keyring_dmr_effective_kid(const dsd_state* state, uint32_t target, int target_is_group, dsd_key_material_need need, uint8_t signaled_kid, int* out_mapped);`
  - `uint8_t keyring_dmr_kid_for_call(const dsd_state* state, const dsd_call_snapshot* call, dsd_key_material_need need, uint8_t signaled_kid, int* out_mapped);`
  - `uint8_t keyring_dmr_slot_kid_for_call(dsd_state* state, int slot, const dsd_call_snapshot* call, dsd_key_material_need need, uint8_t signaled_kid);`
  - `need` sits before `signaled_kid` in all three, grouping the "what are we resolving for" inputs ahead of the fallback value.

This is one atomic signature change: the tree does not compile between the header edit and the last call-site edit. Do steps 3–5 in one pass before building.

- [ ] **Step 1: Write the failing tests**

Append to `tests/protocol/dmr/test_dmr_block_crypto_key_map.c`, before `main()`:

```c
// The data path's ALG numbering is its own (0 BP, 1 RC4, 2 DES, 4 AES-128, 5 AES-256, 7 VTX), so
// it translates locally rather than normalizing into the voice space -- where data-2 (DES) and
// voice-0x02 (Hytera Enhanced) already collide. A row may only apply when the mapped key id holds
// what THAT alg consumes.
static int
test_data_alg_need_gates_the_map(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    // AES-128 burst, row pointing at a scalar-only key id: the row must not apply.
    DSD_MEMSET(&state, 0, sizeof(state));
    seed_keyring_and_map(&state);
    state.payload_algid = 4; /* data AES-128 */
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;
    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("aes-row-scalar-only-unmapped", ctx.mapped, 0);
    rc |= expect_eq("aes-row-scalar-only-kid", ctx.kid, 0x03);

    // RC4 burst, row pointing at an AES-only key id: the row must not apply either.
    DSD_MEMSET(&state, 0, sizeof(state));
    state.keyloader = 1;
    state.payload_algid = 1; /* data RC4 */
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    state.rkey_array[0x1CC] = 0x2222ULL; /* segment 1 of key id 0xCB */
    state.rkey_array_loaded[0x1CC] = 1U;
    state.dmr_tg_key_map_tg[0] = 123U;
    state.dmr_tg_key_map_kid[0] = 0xCB;
    state.dmr_tg_key_map_count = 1;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;
    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("rc4-row-aes-only-unmapped", ctx.mapped, 0);
    rc |= expect_eq("rc4-row-aes-only-kid", ctx.kid, 0x03);
    rc |= expect_eq("rc4-row-aes-only-rkey", (long long)ctx.rkey, 0xAAAAALL);

    // Moto BP (alg 0) reads state->K and the BPK table, never the keyring: no row applies.
    DSD_MEMSET(&state, 0, sizeof(state));
    seed_keyring_and_map(&state);
    state.payload_algid = 0;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;
    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("bp-never-mapped", ctx.mapped, 0);

    // VTX STD (alg 7) has no decrypt branch at all.
    state.payload_algid = 7;
    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("vtx-never-mapped", ctx.mapped, 0);
    return rc;
}
```

Register it in that file's `main()` after `rc |= test_wide_signaled_kid_bypasses_the_map();`:

```c
    rc |= test_data_alg_need_gates_the_map();
```

Then extend the **existing** mapped-TG fixture in `tests/protocol/dmr/test_dmr_flco_privacy_modes.c`
(lines 1917-2110: `mapped_tg_fixture`, `seed_mapped_tg_state()`, `assert_mapped_tg_classification()`,
`run_mapped_tg_lockout_probe()`). Reuse it rather than building a parallel one. Read
`run_mapped_tg_lockout_probe()` before editing — it returns 1 when the lockout armed.

Add a flag to `mapped_tg_fixture` (line 1928):

```c
    int row_material_mismatch; /* mapped kid holds AES segments but no scalar, while the ALG is
                                  RC4 and the SLOT already carries a working RC4 key */
```

In `seed_mapped_tg_state()`, turn the `if (fx->kirisun) { ... } else { ... }` material block into a
three-way by prepending this branch. Leave the existing two bodies untouched:

```c
    if (fx->row_material_mismatch) {
        // Segments 1..3 only. Segment 0 shares the scalar's cell, so leaving it zero is what makes
        // this key id AES-shaped and scalar-empty. The call runs RC4 (0x21), which keys off the
        // scalar, and the slot already carries a working one -- so applying this row makes the
        // call strictly worse off. That is precisely the case "has any material at all" could not
        // see: it looked at the AES segments and said yes.
        for (int i = 1; i < 4; i++) {
            state->rkey_array[MAPPED_TG_KID + kAesOffsets[i]] = 0xC0FFEE00ULL + (unsigned long long)i;
            state->rkey_array_loaded[MAPPED_TG_KID + kAesOffsets[i]] = 1U;
        }
        if (fx->slot == 0U) {
            state->R = 0xAAAAAULL;
        } else {
            state->RR = 0xAAAAAULL;
        }
    } else if (fx->kirisun) {
```

`row_material_mismatch` is never combined with `decoy_other_slot`: it seeds `fx->slot`'s own scalar,
which is the opposite of what the decoy exists to prove.

Then add the headline test, before that file's `main()`:

```c
// The failure this whole change exists to prevent. TG 123 runs ALG 0x21 (RC4, which keys off the
// scalar) and the slot already carries a working RC4 key. A map row points TG 123 at key id 0x7B,
// which holds AES segments and no scalar. Before the alg-need gate the row applied -- "has any
// material at all" saw the AES segments -- so activation zeroed the slot scalar, the call
// published ENCRYPTED, and dsd_enc_lockout_note() armed the session-permanent ledger, forcing a
// P_CLEAR that dropped the channel for a call the SIGNALED key decrypts. Nothing printed either,
// because from the resolver's view the row applied.
static void
test_row_with_wrong_material_is_not_applied(void) {
    assert_mapped_tg_classification(&(mapped_tg_fixture){.slot = 0U, .install_map_row = 1, .row_material_mismatch = 1},
                                    DSD_CALL_CRYPTO_DECRYPTABLE, 1U);
    assert_mapped_tg_classification(&(mapped_tg_fixture){.slot = 1U, .install_map_row = 1, .row_material_mismatch = 1},
                                    DSD_CALL_CRYPTO_DECRYPTABLE, 1U);

    // And the lockout must not arm. The probe returns 1 when it did.
    assert(run_mapped_tg_lockout_probe(
               &(mapped_tg_fixture){.slot = 0U, .install_map_row = 1, .row_material_mismatch = 1})
           == 0);
    assert(run_mapped_tg_lockout_probe(
               &(mapped_tg_fixture){.slot = 1U, .install_map_row = 1, .row_material_mismatch = 1})
           == 0);
}
```

Register it in that file's `main()` directly after `test_mapped_tg_is_not_locked_out();`. No new
includes are needed — the test drives `dmr_flco()` and never calls the keyring directly.

Next, the PI path must gate identically, or the LC-published and PI-published verdicts for one call
diverge. Append to `tests/protocol/dmr/test_dmr_pi_kirisun.c`, before its `main()`, mirroring the
existing `test_pi_mapped_tg_classifies_decryptable()`:

```c
// dmr_pi_publish_crypto() publishes the same verdict as the LC for the same call, so it has to gate
// on the same thing: a row whose key id cannot serve the call's ALG must not steer the
// PI-published classification either.
static void
test_pi_row_with_wrong_material_is_not_applied(void) {
    static dsd_opts opts;
    static dsd_state state;
    dsd_call_snapshot call;
    /* MFID 0x10 (DMRA); byte 0 low bits 0x01 normalize to ALGID 0x21 (RC4, keys off the scalar);
     * byte 2 is the signaled key id. */
    uint8_t rc4_pi[10] = {0x01, 0x10, 0x03, 0x11, 0x22, 0x33, 0x44, 0x00, 0x00, 0x00};

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    seed_active_voice_call(&state);
    state.currentslot = 0;
    state.keyloader = 1;
    /* The slot carries a working RC4 key, as it would once the signaled id was activated. */
    state.R = 0xAAAAAULL;
    /* Key id 0x7B holds AES segments 1..3 and no scalar: nothing RC4 can use. */
    state.rkey_array[0x7B + 0x101] = 0x1111ULL;
    state.rkey_array_loaded[0x7B + 0x101] = 1U;
    state.rkey_array[0x7B + 0x201] = 0x2222ULL;
    state.rkey_array_loaded[0x7B + 0x201] = 1U;
    state.rkey_array[0x7B + 0x301] = 0x3333ULL;
    state.rkey_array_loaded[0x7B + 0x301] = 1U;
    state.dmr_tg_key_map_tg[0] = 1001U; /* seed_active_voice_call()'s talkgroup */
    state.dmr_tg_key_map_kid[0] = 0x7B;
    state.dmr_tg_key_map_count = 1;

    dmr_pi(&opts, &state, rc4_pi, 1U, 0U);
    assert(dsd_call_state_get(&state, 0U, &call) > 0);
    /* The row cannot serve RC4, so the slot's own key decides -- and it decrypts. */
    assert(call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE);
    assert(call.audio_permitted == 1U);
    assert(call.kid == 0x03);
}
```

Register it in that file's `main()` after `test_pi_mapped_tg_classifies_decryptable();`.

Finally, in `tests/core/test_core_dmr_tg_key_map.c`, update the `activate_via_map()` helper to pass a need:

```c
    const uint8_t kid = keyring_dmr_slot_kid_for_call(state, slot, &call, dsd_dmr_alg_key_need(algid), signaled);
```

with `algid` read alongside `signaled`:

```c
    const int algid = (slot == 0) ? state->payload_algid : state->payload_algidR;
```

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -20
```

Expected: FAIL at compile time — too few arguments to `keyring_dmr_effective_kid` / `keyring_dmr_slot_kid_for_call`.

- [ ] **Step 3: Change the three resolver signatures**

In `include/dsd-neo/core/keyring.h`, change the three declarations to the signatures in the **Interfaces** block above, and add this paragraph to `keyring_dmr_effective_kid()`'s doc comment, replacing the sentence beginning "A map row for `target` replaces the OTA-signaled key ID, but only when the mapped ID has imported material":

```
 * A map row for `target` replaces the OTA-signaled key ID, but only when the mapped ID holds the
 * material @p need calls for: a row naming a key that cannot serve this ALG would otherwise zero
 * the slot key and shadow a signaled ID that would have worked, arming a session-permanent
 * lockout on a call that was decryptable. `need` comes from the caller because the ALG numbering
 * is the caller's -- voice IDs in core/audio, DMR data IDs in protocol/dmr.
```

In `src/core/vocoder/keyring.c`, update the three definitions to match, and change the gate inside `keyring_dmr_effective_kid()` from:

```c
    if (!keyring_kid_material(state, (int)kid, NULL, NULL)) {
```

to:

```c
    if (!keyring_kid_satisfies_need(state, (int)kid, need)) {
```

`keyring_dmr_kid_for_call()` forwards its `need` to `keyring_dmr_effective_kid()`; `keyring_dmr_slot_kid_for_call()` forwards its `need` to `keyring_dmr_kid_for_call()`.

- [ ] **Step 4: Add the data-path ALG table**

In `src/protocol/dmr/dmr_block_crypto.c`, add `#include <dsd-neo/core/key_material.h>` to the include block, then add this directly above `dmr_block_crypto_load_ctx()`:

```c
// The data path's own ALG numbering, translated locally rather than normalized into the voice
// space: data-2 (DES) and voice-0x02 (Hytera Enhanced) already collide there. Derived from what
// dmr_block_crypto_decrypt_payload() actually consumes -- algs 1 and 2 require ctx->rkey, alg 4
// reads parts[0..1], alg 5 reads parts[0..3], alg 0 (Moto BP) reads state->K and the BPK table
// instead of the keyring, and alg 7 (VTX STD) has no decrypt branch at all.
static dsd_key_material_need
dmr_block_alg_key_need(int alg) {
    switch (alg) {
        case 1: /* RC4 */
        case 2: /* DES */
            return DSD_KEY_NEED_SCALAR;
        case 4: /* AES-128 */ return DSD_KEY_NEED_AES_2;
        case 5: /* AES-256 */ return DSD_KEY_NEED_AES_4;
        default: return DSD_KEY_NEED_NONE;
    }
}
```

- [ ] **Step 5: Update all seven call sites**

`src/protocol/dmr/dmr_block_crypto.c:127` — insert the need between `target_is_group` and `signaled_kid`:

```c
        ctx->kid = (int)keyring_dmr_effective_kid(state, (uint32_t)state->dmr_lrrp_target[id_slot],
                                                  state->dmr_data_target_is_group[id_slot] != 0U,
                                                  dmr_block_alg_key_need(ctx->alg), (uint8_t)ctx->signaled_kid,
                                                  &ctx->mapped);
```

`src/core/vocoder/dsd_mbe.c:353` — `algid` is already in scope at line 337:

```c
            kid = (int)keyring_dmr_slot_kid_for_call(state, slot, call, dsd_dmr_alg_key_need(algid), (uint8_t)signaled);
```

`src/protocol/dmr/dmr_flco.c:644` — inside `dmr_flco_resolve_key_material()`. Read the ALGID the same way `dmr_flco_publish_crypto()` does and pass its need:

```c
        eff_kid = (int)keyring_dmr_effective_kid(ctx->state, ctx->target, is_group, dsd_dmr_alg_key_need((int)algid),
                                                 (uint8_t)signaled, &mapped);
```

`dmr_flco_resolve_key_material()` currently takes only `ctx`. Give it an `int algid` parameter and pass the value each of its two callers already computed — `dmr_flco_publish_crypto()` at line ~653 and `dmr_flco_slot_can_decrypt()` at line ~985. Do **not** re-derive the ALGID inside the resolver; both callers already have it and re-deriving reintroduces a second source of truth.

`src/protocol/dmr/dmr_pi.c:247` — `algid` is in scope at line 234:

```c
        eff_kid = (int)keyring_dmr_kid_for_call(state, &call, dsd_dmr_alg_key_need((int)algid), (uint8_t)kid, &mapped);
```

`src/core/file/dsd_file.c:1606`, `:1888`, `:1977` — all three have `ctx->alg_id` in voice numbering:

```c
                const uint8_t kid = keyring_dmr_effective_kid(state, ctx->target_id, sdrtrunk_json_target_is_group(ctx),
                                                              dsd_dmr_alg_key_need((int)ctx->alg_id),
                                                              (uint8_t)ctx->key_id, &mapped);
```

Add `#include <dsd-neo/core/audio.h>` to `src/protocol/dmr/dmr_block_crypto.c` is **not** needed (it uses `dmr_block_alg_key_need`), but `dsd_file.c`, `dmr_flco.c`, `dmr_pi.c` and `dsd_mbe.c` each need `<dsd-neo/core/audio.h>` for `dsd_dmr_alg_key_need()`. Check each file's include block and add it only where missing.

- [ ] **Step 6: Build and run the whole suite**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -5
ctest --preset dev-debug --output-on-failure 2>&1 | tail -30
```

Expected: all tests pass. Some pre-existing cases in `CORE_DMR_TG_KEY_MAP` seed AES segments while leaving `payload_algid` at `0x21` (RC4) or unset; under the new gate those rows correctly stop applying. Fix each by setting `state.payload_algid` to the ALG whose material the case actually seeds — `0x24` for a two-segment fixture, `0x25` or `0x36` for a quartet. That is the test expressing the new rule, not a regression. If a case cannot be made coherent that way, stop and report it rather than weakening the gate.

- [ ] **Step 7: Format and commit**

```bash
tools/format.sh
git add include/dsd-neo/core/keyring.h src/core/vocoder/keyring.c src/core/vocoder/dsd_mbe.c src/protocol/dmr/dmr_flco.c src/protocol/dmr/dmr_pi.c src/protocol/dmr/dmr_block_crypto.c src/core/file/dsd_file.c tests/
git commit -s -m "dmr: gate the tg key map on the alg's key-material need"
```

---

### Task 4: Split the notice latch and name the missing material

**Files:**
- Modify: `include/dsd-neo/core/state.h:443`
- Modify: `src/core/vocoder/keyring.c`
- Test: `tests/core/test_core_dmr_tg_key_map.c`, `tests/ui/test_ui_menu_services.c`

**Interfaces:**
- Consumes: `dsd_key_material_need` (Task 1), the threaded resolver (Task 3).
- Produces: `uint64_t dmr_tg_key_skip_epoch[2];` in `dsd_state`, reset by `keyring_dmr_tg_map_reset()` alongside `dmr_tg_key_note_epoch[2]`.

- [ ] **Step 1: Write the failing test**

Append to `tests/core/test_core_dmr_tg_key_map.c`, before `main()`:

```c
// The two notices report opposite outcomes and used to share one latch, stamped as a side effect
// of the query -- so whichever situation held on an epoch's first mappable frame permanently
// silenced the other for the rest of the call. A call that opens on a mapped TG whose key is not
// imported prints "skipped"; if the operator then imports that key mid-call the decoder starts
// using it, and the "applied" notice has to be able to say so.
static int
test_both_notices_can_print_in_one_epoch(void) {
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    map_one(&state, 123U, 0x7B); /* 0x7B has nothing imported yet */
    observe_group_call(&state, 0U, 123U);

    const int skipped_hits = capture_notice_hits(&state, 0, "has no");
    rc |= expect_eq("skip-notice-printed-once", skipped_hits, 1);

    // Operator imports the mapped key mid-call; the epoch does not change.
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;

    const int applied_hits = capture_notice_hits(&state, 0, "-> Key ID: 7B;");
    rc |= expect_eq("applied-notice-printed-once", applied_hits, 1);

    dsd_state_ext_free_all(&state);
    return rc;
}
```

Register it in `main()` **directly before** `rc |= test_notice_is_emitted_once_per_epoch();`, because that function redirects stderr and the file's comment says it must run last.

Then in `tests/ui/test_ui_menu_services.c`, extend the existing latch assertion. After line 912's `state.dmr_tg_key_note_epoch[0] = 42U;` add:

```c
    state.dmr_tg_key_skip_epoch[0] = 43U;
```

and after line 921's assertion add:

```c
    rc |= expect_int("keys clear drops the tg key skip latch", (int)state.dmr_tg_key_skip_epoch[0], 0);
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -20
```

Expected: FAIL at compile time — `'dsd_state' has no member named 'dmr_tg_key_skip_epoch'`.

- [ ] **Step 3: Add the second latch**

In `include/dsd-neo/core/state.h`, directly after `uint64_t dmr_tg_key_note_epoch[2];` (line 443):

```c
    /* Own latch, not shared with dmr_tg_key_note_epoch: the applied and skipped notices report
       opposite outcomes, so one latch let whichever fired first silence the other for the whole
       epoch -- even after the situation changed (a key imported mid-call, for instance). */
    uint64_t dmr_tg_key_skip_epoch[2];
```

- [ ] **Step 4: Give each notice its own latch and name the material**

In `src/core/vocoder/keyring.c`, add the skip array to `keyring_dmr_tg_map_reset()`:

```c
    state->dmr_tg_key_skip_epoch[0] = state->dmr_tg_key_skip_epoch[1] = 0U;
```

Change the latch helper to take the latch it stamps:

```c
// One notice per call epoch, not one per voice frame. dsd_call_state_get() only reports a hit for
// a non-zero epoch, so epoch 0 is the "never announced" sentinel and needs no valid flag. The
// caller passes which latch to stamp: the applied and skipped notices report opposite outcomes,
// so sharing one let whichever fired first silence the other for the rest of the epoch.
static int
keyring_dmr_tg_map_note_should_print(uint64_t* latch, uint64_t epoch) {
    if (*latch == epoch) {
        return 0;
    }
    *latch = epoch;
    return 1;
}
```

`keyring_dmr_tg_map_note()` passes `&state->dmr_tg_key_note_epoch[slot]`; `keyring_dmr_tg_map_note_skipped()` passes `&state->dmr_tg_key_skip_epoch[slot]`.

Add a label for the missing material and take the `need` in the skipped notice:

```c
static const char*
keyring_need_label(dsd_key_material_need need) {
    switch (need) {
        case DSD_KEY_NEED_SCALAR: return "scalar";
        case DSD_KEY_NEED_AES_2: return "16-byte AES";
        case DSD_KEY_NEED_AES_3: return "24-byte AES";
        case DSD_KEY_NEED_AES_4: return "32-byte AES";
        case DSD_KEY_NEED_QUARTET: return "Kirisun quartet";
        case DSD_KEY_NEED_NONE: break;
    }
    return NULL;
}

// Announced when a row matched but resolved to nothing, so a CSV typo is visible rather than
// looking like the map simply did not cover the talkgroup. Silent for DSD_KEY_NEED_NONE: there the
// row was never eligible because the ALG selects no keyring material at all, so reporting a
// missing key would point the operator at the wrong thing.
static void
keyring_dmr_tg_map_note_skipped(dsd_state* state, int slot, uint64_t epoch, uint32_t tg, uint8_t mapped_kid,
                                dsd_key_material_need need, uint8_t signaled_kid) {
    const char* label = keyring_need_label(need);
    if (label == NULL) {
        return;
    }
    if (!keyring_dmr_tg_map_note_should_print(&state->dmr_tg_key_skip_epoch[slot], epoch)) {
        return;
    }
    DSD_FPRINTF(stderr,
                "\n Slot %d DMR TG Key Map: TG %u -> Key ID: %02X has no %s key; using signaled Key ID: %02X;",
                slot + 1, tg, mapped_kid, label, signaled_kid);
}
```

Update the one call in `keyring_dmr_slot_kid_for_call()` to pass `need` through.

- [ ] **Step 5: Run the tests to verify they pass**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -5
ctest --preset dev-debug -R "CORE_DMR_TG_KEY_MAP|UI_MENU_SERVICES" --output-on-failure
```

Expected: both PASS. If `test_notice_is_emitted_once_per_epoch()` now fails on a needle that changed wording, update its needle to match the new message — the assertion being tested is the once-per-epoch latch, not the exact phrasing.

- [ ] **Step 6: Format and commit**

```bash
tools/format.sh
git add include/dsd-neo/core/state.h src/core/vocoder/keyring.c tests/core/test_core_dmr_tg_key_map.c tests/ui/test_ui_menu_services.c
git commit -s -m "dmr: give the tg key map's two notices separate epoch latches"
```

---

### Task 5: Data-path key precedence and slot coverage

**Files:**
- Modify: `src/protocol/dmr/dmr_block_crypto.c:50-70` (comment only), `:135-145` (comment only)
- Test: `tests/protocol/dmr/test_dmr_block_crypto_key_map.c`

**Interfaces:**
- Consumes: the threaded resolver and `dmr_block_alg_key_need()` (Task 3).
- Produces: nothing new. This task pins existing behavior that the review flagged as unverified.

- [ ] **Step 1: Write the failing tests**

Append to `tests/protocol/dmr/test_dmr_block_crypto_key_map.c`, before `main()`:

```c
// Precedence: an explicit --dmr-tg-key-csv row beats the global manual AES key (state->K1..K4,
// set by -2/-H and the keys menu). Safe now in a way it was not before the alg-need gate: a row
// can only apply for data alg 4 when cells kid+0x000 and kid+0x101 are non-zero, and those are
// exactly the cells dmr_block_load_aes_key() reads -- so its "all four zero, substitute K1..K4"
// fallback is unreachable whenever ctx->mapped is set, rather than being skipped because a lone
// RC4 scalar happened to sit in parts[0].
static int
test_mapped_aes_row_beats_manual_key(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.keyloader = 1;
    state.payload_algid = 4; /* data AES-128 */
    state.payload_keyid = 0x03;
    state.K1 = 0xEEEEEEEEEEEEEEEEULL;
    state.K2 = 0xDDDDDDDDDDDDDDDDULL;
    state.K3 = 0xCCCCCCCCCCCCCCCCULL;
    state.K4 = 0xBBBBBBBBBBBBBBBBULL;
    // Key id 0x40: a real two-segment AES-128 import.
    state.rkey_array[0x40] = 0x1111111111111111ULL;
    state.rkey_array_loaded[0x40] = 1U;
    state.rkey_array[0x141] = 0x2222222222222222ULL;
    state.rkey_array_loaded[0x141] = 1U;
    state.dmr_tg_key_map_tg[0] = 123U;
    state.dmr_tg_key_map_kid[0] = 0x40;
    state.dmr_tg_key_map_count = 1;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 123ULL;
    state.dmr_data_target_is_group[0] = 1U;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("manual-key-row-mapped", ctx.mapped, 1);
    rc |= expect_eq("manual-key-row-kid", ctx.kid, 0x40);
    // First byte of A1 comes from the row, not from K1.
    rc |= expect_eq("manual-key-row-wins", ctx.aes_key[0], 0x11);
    return rc;
}

// And with no row applying, the manual key fallback still works: the key id has nothing imported,
// so all four parts are zero and K1..K4 substitute exactly as before.
static int
test_manual_key_fallback_survives_without_a_row(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.keyloader = 1;
    state.payload_algid = 4;
    state.payload_keyid = 0x05; /* nothing imported at 0x05 */
    state.K1 = 0xEEEEEEEEEEEEEEEEULL;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 999ULL; /* unmapped */
    state.dmr_data_target_is_group[0] = 1U;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("no-row-unmapped", ctx.mapped, 0);
    rc |= expect_eq("no-row-manual-key-used", ctx.aes_key[0], 0xEE);
    return rc;
}

// The scalar fallback is slot-correct: R keys slot 1, RR keys slot 2. Reading R for both decrypted
// slot 2 with slot 1's key whenever the resolved id had nothing imported. Both directions are
// pinned here because only the slot-2 case had a test.
static int
test_slot1_fallback_uses_r(void) {
    static dsd_state state;
    dmr_block_crypto_ctx ctx;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.keyloader = 1;
    state.payload_algid = 1; /* data RC4 */
    state.payload_keyid = 0x05; /* nothing imported */
    state.R = 0x1234ULL;
    state.RR = 0x5678ULL;
    state.currentslot = 0;
    state.dmr_lrrp_target[0] = 999ULL;

    dmr_block_crypto_load_ctx(&state, 0U, 1, 12, &ctx);
    rc |= expect_eq("slot1-fallback-uses-r", (long long)ctx.rkey, 0x1234LL);
    return rc;
}
```

Register all three in that file's `main()`.

- [ ] **Step 2: Run the tests to verify they fail**

```bash
cmake --build --preset dev-debug -j
ctest --preset dev-debug -R DMR_BLOCK_CRYPTO_KEY_MAP --output-on-failure
```

Expected: the three new cases run. `test_slot1_fallback_uses_r` should PASS immediately (it pins behavior the earlier fix already produces); `test_mapped_aes_row_beats_manual_key` and `test_manual_key_fallback_survives_without_a_row` should also PASS if Task 3 landed correctly. If any FAILS, the alg-need threading in `dmr_block_crypto.c` is wrong — fix that, not the test.

- [ ] **Step 3: Record the invariants in comments**

In `src/protocol/dmr/dmr_block_crypto.c`, in `dmr_block_load_aes_key()`, above the `if (parts[0] == 0ULL && ...)` fallback:

```c
    // Manual-key fallback for a key id with nothing imported. Unreachable on the mapped path by
    // construction: dmr_block_alg_key_need() only lets a row apply for alg 4 when cells
    // kid+0x000 and kid+0x101 are non-zero, and for alg 5 when all four are -- exactly the cells
    // read above. So an explicit --dmr-tg-key-csv row beats -2/-H without the row ever being able
    // to silently substitute an unusable key for a working manual one.
```

Above the scalar fallback in `dmr_block_crypto_load_ctx()`, extend the existing comment:

```c
    // Slot-correct fallback: RR keys slot 2. Reading R for both slots decrypted slot 2 with
    // slot 1's key whenever the resolved id had nothing imported. No DMR key source is lost by
    // this: -1 (args.c) and DSD_APP_CMD_KEY_RC4DES_SET both write R and RR. The only writers of R
    // without RR are -R and DSD_APP_CMD_KEY_SCRAMBLER_SET, both NXDN/dPMR scrambler keys.
```

- [ ] **Step 4: Rebuild and re-run**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -5
ctest --preset dev-debug -R DMR_BLOCK_CRYPTO_KEY_MAP --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Format and commit**

```bash
tools/format.sh
git add src/protocol/dmr/dmr_block_crypto.c tests/protocol/dmr/test_dmr_block_crypto_key_map.c
git commit -s -m "dmr: pin the data-path key precedence and both slot fallbacks"
```

---

### Task 6: Forced-ALGID replay reports a keystream it does not have

**Files:**
- Modify: `src/core/file/dsd_file.c:1618-1633`
- Test: `tests/core/test_core_mbe_file_io.c`

**Interfaces:**
- Consumes: nothing from earlier tasks; independent of the alg-need work.
- Produces: nothing new.

- [ ] **Step 1: Write the failing test**

Append to `tests/core/test_core_mbe_file_io.c`, before `main()`. Model the fixture on the existing `test_sdrtrunk_json_dmr_tg_key_map_keys_forced_algid_keystream()` in the same file — read it first and reuse its `capture_sdrtrunk_replay_records()` harness and its `state.M` setup:

```c
// sdrtrunk_json_apply_forced_algid() runs on every token, and its RC4 branch only builds a
// keystream when both a key and an MI are known. sdrtrunk_json_context_init() runs once per FILE,
// so when that guard fails the PREVIOUS record's ctx->ks and ctx->ks_available stay in force and
// the next record decodes against a keystream built from a key it does not use -- while the
// decoder still reports it decryptable. "No key or no IV" means we have no keystream.
//
// Three all-zero AMBE frames make the decoded body purely a function of the keystream window each
// frame consumed (see build_sdrtrunk_map_json()'s comment), so an inherited keystream changes
// every byte after the error count. Records are fixed-width here, so the second record of the pair
// must be byte-identical to that same record replayed alone.
static int
test_sdrtrunk_forced_rc4_does_not_inherit_the_previous_keystream(void) {
    int rc = 0;
    /* Record 1 signals key id 3, which seed_sdrtrunk_dmr_replay_keys() imports. */
    static const char json_first[] = "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\","
                                     "\"encrypted\":\"true\",\"encryption_key_id\":\"3\","
                                     "\"encryption_mi\":\"001122334455667788\",\"to\":\"4567\","
                                     "\"from\":\"456\",\"time\":\"1700000000000\","
                                     "\"hex\":\"000000000000000000\"}";
    /* Record 2 signals key id 9, which nothing imports -- so state->R goes to 0 and the guard fails. */
    static const char json_second[] = "{\"version\":\"2\",\"protocol\":\"DMR\",\"call_type\":\"GROUP\","
                                      "\"encrypted\":\"true\",\"encryption_key_id\":\"9\","
                                      "\"encryption_mi\":\"001122334455667788\",\"to\":\"4567\","
                                      "\"from\":\"456\",\"time\":\"1700000001000\","
                                      "\"hex\":\"000000000000000000\"}";
    char pair[1024];
    static dsd_state state;
    unsigned char both[SDRTRUNK_MAP_RECORD_CAP];
    unsigned char alone[SDRTRUNK_MAP_RECORD_CAP];
    size_t both_len = 0;
    size_t alone_len = 0;

    DSD_SNPRINTF(pair, sizeof pair, "%s\n%s", json_first, json_second);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.M = 0x21; /* --dmr-force-algid 21 */
    rc |= capture_sdrtrunk_replay_records("sdrtrunk forced rc4 pair", pair, &state, both, sizeof both, &both_len);
    dsd_state_ext_free_all(&state);

    DSD_MEMSET(&state, 0, sizeof state);
    seed_sdrtrunk_dmr_replay_keys(&state);
    state.M = 0x21;
    rc |= capture_sdrtrunk_replay_records("sdrtrunk forced rc4 unkeyed alone", json_second, &state, alone,
                                          sizeof alone, &alone_len);
    dsd_state_ext_free_all(&state);

    rc |= expect_true("sdrtrunk forced rc4 pair wrote two records", alone_len > 0U && both_len == alone_len * 2U);
    rc |= expect_true("sdrtrunk forced rc4 does not inherit the previous keystream",
                      both_len == alone_len * 2U && memcmp(both + alone_len, alone, alone_len) == 0);
    return rc;
}
```

Register it in that file's `main()` after
`rc |= test_sdrtrunk_json_dmr_tg_key_map_keys_forced_algid_keystream();`.

If `both_len != alone_len * 2U`, `run_sdrtrunk_json()` did not treat the concatenated objects as two
records — inspect how it delimits them and adjust the separator in `pair` rather than weakening the
assertion.

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build --preset dev-debug -j
ctest --preset dev-debug -R CORE_MBE_FILE_IO --output-on-failure
```

Expected: FAIL — the record decodes against a stale keystream.

- [ ] **Step 3: Zero `ks_available` when the branch cannot build**

In `src/core/file/dsd_file.c`, replace:

```c
        if (ctx->alg_id == 0x21 && state->R != 0 && state->payload_mi != 0) {
```

...through the end of that block with:

```c
        if (ctx->alg_id == 0x21) {
            if (state->R != 0 && state->payload_mi != 0) {
                uint8_t iv64[8] = {0};
                iv64[4] = (uint8_t)((state->payload_mi >> 24ULL) & 0xFFULL);
                iv64[5] = (uint8_t)((state->payload_mi >> 16ULL) & 0xFFULL);
                iv64[6] = (uint8_t)((state->payload_mi >> 8ULL) & 0xFFULL);
                iv64[7] = (uint8_t)((state->payload_mi >> 0ULL) & 0xFFULL);
                ctx->ks_available =
                    (uint8_t)sdrtrunk_build_voice_keystream_bits(state, ctx->alg_id, effective_kid, iv64, ctx->rc4_db,
                                                                 ctx->rc4_mod, ctx->protocol, ctx->ks, sizeof(ctx->ks));
                // This path owns ctx->ks on every token once an MI is known, and it uses its own
                // payload_mi-derived IV. Recording the id it used keeps the map's rebuild below
                // from overwriting this keystream with one built from the raw "encryption_mi"
                // bytes.
                ctx->ks_key_id = effective_kid;
                ctx->ks_built = 1;
            } else {
                // No key or no IV yet: we have no keystream. Leaving the previous token's in place
                // kept the decoder XORing against an earlier key while still reporting the record
                // decryptable. Self-corrects on the next token once the MI lands, like everything
                // else in this file.
                ctx->ks_available = 0;
            }
        }
```

The `ctx->alg_id == 0x21` scope is load-bearing: forced AES (`0x22`–`0x25`) never builds in this branch and its `ks_available` is owned by the MI handler, so an unscoped zeroing would break forced-AES replay.

- [ ] **Step 4: Run the tests to verify they pass**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -5
ctest --preset dev-debug -R CORE_MBE_FILE_IO --output-on-failure
```

Expected: PASS, including the pre-existing `test_sdrtrunk_json_dmr_tg_key_map_keys_forced_algid_keystream()` — that case is the guard proving forced-AES replay was not zeroed.

- [ ] **Step 5: Format and commit**

```bash
tools/format.sh
git add src/core/file/dsd_file.c tests/core/test_core_mbe_file_io.c
git commit -s -m "file: report no keystream when forced rc4 replay cannot build one"
```

---

### Task 7: Document and pin the stale-ACTIVE snapshot window

**Files:**
- Modify: `src/core/vocoder/keyring.c` (comment on `keyring_dmr_tg_map_call_is_mappable()`)
- Modify: `src/protocol/dmr/dmr_pi.c:243` (comment)
- Test: `tests/core/test_core_dmr_tg_key_map.c`

**Interfaces:**
- Consumes: the threaded resolver (Task 3), `activate_via_map()` as updated in Task 3.
- Produces: nothing new.

- [ ] **Step 1: Write the failing test**

Append to `tests/core/test_core_dmr_tg_key_map.c`, before `main()` and before `test_notice_is_emitted_once_per_epoch()` in the registration order:

```c
// A transmission that ends without a decodable terminator leaves its epoch ACTIVE, so the next
// transmission's first voice frames resolve the map against the PREVIOUS talkgroup. Nothing can
// signal that staleness until the new voice LC opens an epoch -- every consumer of the canonical
// snapshot shares the exposure. What is guaranteed is that it self-heals: activation runs every
// frame, so the first frame after the new LC resolves correctly. This pins that guarantee.
static int
test_stale_active_epoch_self_heals_on_the_next_lc(void) {
    static dsd_state state;
    int rc = 0;

    DSD_MEMSET(&state, 0, sizeof(state));
    state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state.keyloader = 1;
    state.payload_algid = 0x21;
    state.payload_keyid = 0x03;
    state.rkey_array[0x03] = 0xAAAAAULL;
    state.rkey_array_loaded[0x03] = 1U;
    state.rkey_array[0x7B] = 0xBBBBBULL;
    state.rkey_array_loaded[0x7B] = 1U;
    map_one(&state, 100U, 0x7B);
    observe_group_call(&state, 0U, 100U);

    // TG 100 is mapped and its epoch is ACTIVE.
    rc |= expect_eq("stale-mapped-tg", activate_via_map(&state, 0), 0x7B);

    // The transmission ends with no decodable terminator: the epoch stays ACTIVE, so an unmapped
    // TG 200 transmission's first frames still resolve against TG 100.
    rc |= expect_eq("stale-still-keys-old-tg", activate_via_map(&state, 0), 0x7B);

    // TG 200's voice LC opens its epoch, and the very next frame resolves correctly.
    observe_group_call(&state, 0U, 200U);
    rc |= expect_eq("self-heals-on-new-epoch", activate_via_map(&state, 0), 0x03);
    rc |= expect_eq("self-heals-key", (long long)state.R, 0xAAAAALL);

    dsd_state_ext_free_all(&state);
    return rc;
}
```

Register it in `main()` directly before `rc |= test_notice_is_emitted_once_per_epoch();`.

- [ ] **Step 2: Run the test to verify it captures current behavior**

```bash
cmake --build --preset dev-debug -j
ctest --preset dev-debug -R CORE_DMR_TG_KEY_MAP --output-on-failure
```

Expected: PASS. This test pins a guarantee rather than fixing a defect, so it should be green immediately. If `self-heals-on-new-epoch` FAILS, the self-heal claim in the spec is wrong and the exposure is unbounded — stop and report that rather than adjusting the test.

- [ ] **Step 3: Record the reasoning**

In `src/core/vocoder/keyring.c`, append to the comment block above `keyring_dmr_tg_map_call_is_mappable()`:

```c
// The ACTIVE test bounds staleness rather than eliminating it. A transmission that ends without a
// decodable terminator leaves its epoch ACTIVE, so the next transmission's first voice frames
// resolve against the previous talkgroup. Nothing can signal that until the new voice LC opens an
// epoch -- there is no freshness signal to read, and every consumer of the canonical snapshot has
// the same exposure. It self-heals: keyring_activate_slot_with_kid() runs every frame, so the
// first frame after the new LC installs the right key. Pinned by
// test_stale_active_epoch_self_heals_on_the_next_lc().
```

In `src/protocol/dmr/dmr_pi.c`, extend the comment at line 243 (which currently reasons only about the "no snapshot yet" case) with:

```c
    // Same bounded staleness as the voice path: a missed terminator leaves the previous
    // transmission's epoch ACTIVE, so an early PI header can publish against the old talkgroup's
    // row. See keyring_dmr_tg_map_call_is_mappable() for why no freshness signal exists.
```

- [ ] **Step 4: Rebuild and re-run**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -5
ctest --preset dev-debug -R "CORE_DMR_TG_KEY_MAP|DMR_PI_KIRISUN" --output-on-failure
```

Expected: PASS.

- [ ] **Step 5: Format and commit**

```bash
tools/format.sh
git add src/core/vocoder/keyring.c src/protocol/dmr/dmr_pi.c tests/core/test_core_dmr_tg_key_map.c
git commit -s -m "dmr: document and pin the tg key map's stale-epoch window"
```

---

### Task 8: Full verification sweep

**Files:** none modified unless a check fails.

**Interfaces:**
- Consumes: Tasks 1-7.
- Produces: a clean tree ready to push.

- [ ] **Step 1: Full clean build and test**

```bash
cmake --build --preset dev-debug -j 2>&1 | tail -5
ctest --preset dev-debug --output-on-failure 2>&1 | tail -20
```

Expected: `100% tests passed`. The suite was 541 tests before this work; the new cases are added inside existing targets, so the count should stay 541.

- [ ] **Step 2: Architecture and call-state invariants**

```bash
ctest --preset dev-debug -R "ARCH_RULES|CANONICAL_CALL_STATE_AUDIT|VCPKG_DEPENDENCY_CONTRACT" --output-on-failure
```

Expected: PASS. `audio.h` and `keyring.h` both including `key_material.h` is core→core, but run the check rather than reason about it.

- [ ] **Step 3: Formatting and static analysis**

```bash
tools/format.sh
tools/cmake_format_check.sh
tools/semgrep.sh --strict
tools/clang_tidy.sh
```

Expected: all clean. `tools/semgrep.sh --strict` reports 0 findings.

- [ ] **Step 4: Sanitizer pass over the changed area**

```bash
cmake --preset asan-ubsan-debug && cmake --build --preset asan-ubsan-debug -j 2>&1 | tail -5
ctest --preset asan-ubsan-debug -R "CORE_DMR_TG_KEY_MAP|CORE_DMR_VOICE_ALG_GATE|DMR_BLOCK_CRYPTO_KEY_MAP|CORE_MBE_FILE_IO|DMR_FLCO_PRIVACY_MODES|DMR_PI_KIRISUN|UI_MENU_SERVICES" --output-on-failure
```

Expected: PASS with no ASan/UBSan reports. The new `key_id + offset` indexing is the reason this runs: `keyring_rkey_value()` bounds-checks, but the sanitizer confirms it.

- [ ] **Step 5: Preflight**

```bash
tools/preflight_ci.sh
tools/quality_preflight.sh
```

Expected: both clean. `quality_preflight.sh` is warranted here — this touches crypto key selection across five modules.

- [ ] **Step 6: Commit anything the tools rewrote**

```bash
git status --short
git add -A
git commit -s -m "dmr: apply formatter and analyzer fixups for the tg key map hardening"
```

Skip this step if `git status --short` is empty.
