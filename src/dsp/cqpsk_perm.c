// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * CQPSK permutation state management.
 */

#include <dsd-neo/dsp/cqpsk_perm.h>

#include <stdatomic.h>
#include <stdint.h>

/* All 24 permutations of dibit mappings (0..3).
 * Each row maps input dibit [0,1,2,3] to output dibit.
 * Used to correct constellation rotation discovered during sync detection. */
const int kCqpskPerms[CQPSK_PERM_COUNT][4] = {
    {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 2, 3, 1}, {0, 3, 1, 2}, {0, 3, 2, 1}, {1, 0, 2, 3}, {1, 0, 3, 2},
    {1, 2, 0, 3}, {1, 2, 3, 0}, {1, 3, 0, 2}, {1, 3, 2, 0}, {2, 0, 1, 3}, {2, 0, 3, 1}, {2, 1, 0, 3}, {2, 1, 3, 0},
    {2, 3, 0, 1}, {2, 3, 1, 0}, {3, 0, 1, 2}, {3, 0, 2, 1}, {3, 1, 0, 2}, {3, 1, 2, 0}, {3, 2, 0, 1}, {3, 2, 1, 0},
};

/* Global CQPSK permutation state packed into a single atomic 64-bit value.
 * This ensures atomic updates of both index and hamming distance together,
 * avoiding TOCTOU races when multiple threads access the state.
 *
 * Bit layout:
 *   bits  0-31: hamming distance (best_ham)
 *   bits 32-63: permutation index (perm_idx)
 */
static _Atomic uint64_t g_cqpsk_state = ((uint64_t)CQPSK_HAMMING_INIT); /* idx=0, ham=INIT */

/* Helper macros for packing/unpacking state */
#define PACK_STATE(idx, ham) (((uint64_t)(idx) << 32) | ((uint64_t)(ham) & 0xFFFFFFFF))
#define UNPACK_IDX(state)    ((int)((state) >> 32))
#define UNPACK_HAM(state)    ((int)((state) & 0xFFFFFFFF))

int
cqpsk_apply_perm(int perm_idx, int dibit) {
    if (perm_idx >= 0 && perm_idx < CQPSK_PERM_COUNT) {
        return kCqpskPerms[perm_idx][dibit & 0x3];
    }
    return dibit & 0x3;
}

void
cqpsk_perm_reset(void) {
    atomic_store(&g_cqpsk_state, PACK_STATE(0, CQPSK_HAMMING_INIT));
}

int
cqpsk_perm_get_idx(void) {
    uint64_t state = atomic_load(&g_cqpsk_state);
    return UNPACK_IDX(state);
}

int
cqpsk_perm_get_best_ham(void) {
    uint64_t state = atomic_load(&g_cqpsk_state);
    return UNPACK_HAM(state);
}

int
cqpsk_perm_update(int new_idx, int new_ham) {
    uint64_t current = atomic_load(&g_cqpsk_state);

    for (;;) {
        int current_ham = UNPACK_HAM(current);
        int current_idx = UNPACK_IDX(current);

        /* Update if we found a better Hamming distance, OR if we found
         * a different permutation with equal ham (phase rotation recovery).
         * Only reject if it's the same permutation with same-or-worse ham. */
        if (new_ham > current_ham) {
            return 0; /* strictly worse - reject */
        }
        if (new_ham == current_ham && new_idx == current_idx) {
            return 0; /* same permutation, same ham - no change needed */
        }
        /* Either better ham, or same ham but different permutation (slip recovery) */

        /* Prepare new state: always update ham, possibly update idx */
        uint64_t desired = PACK_STATE(new_idx, new_ham);

        /* Atomically update if state hasn't changed since we read it.
         * If another thread modified state, retry with the new value. */
        if (atomic_compare_exchange_weak(&g_cqpsk_state, &current, desired)) {
            /* Success - return whether the permutation index changed */
            return (new_idx != current_idx) ? 1 : 0;
        }
        /* CAS failed, 'current' now contains the updated value - retry loop */
    }
}

/* QPSK phase rotation indices (0°, 90°, 180°, 270°).
 * These correspond to cyclic dibit rotations caused by Costas loop
 * locking at different phase angles after carrier re-acquisition.
 *   0°:   {0,1,2,3} = index 0
 *   90°:  {1,2,3,0} = index 9
 *   180°: {2,3,0,1} = index 16
 *   270°: {3,0,1,2} = index 18
 */
static const int kPhaseRotationIndices[4] = {0, 9, 16, 18};

/* Compute Hamming distance between mapped dibits and expected sync pattern.
 * Variable-length version for supporting both P25P1 (24) and P25P2 (20) patterns. */
static int
compute_ham_n(int perm_idx, const int* raw_dibits, const char* expected_sync, int sync_len) {
    int ham = 0;
    for (int k = 0; k < sync_len; k++) {
        int d = raw_dibits[k] & 0x3;
        int md = cqpsk_apply_perm(perm_idx, d);
        int expect_n = expected_sync[k] - '0';
        if (md != expect_n) {
            ham++;
        }
    }
    return ham;
}

/* Fixed-length Hamming distance for P25P1 (24 dibits) - backward compatibility. */
static int
compute_ham(int perm_idx, const int* raw_dibits, const char* expected_sync) {
    return compute_ham_n(perm_idx, raw_dibits, expected_sync, CQPSK_P25P1_SYNC_LEN);
}

int
cqpsk_perm_search(const int* raw_dibits, const char* expected_sync, int* out_idx, int* out_ham) {
    int best_idx = cqpsk_perm_get_idx();
    int global_ham = cqpsk_perm_get_best_ham();

    /* 0. Check lock state - if we previously achieved a very good ham, verify the
     * current window still matches well before skipping search. This prevents
     * thrashing when locked, but allows re-search if constellation has drifted.
     * The lock is implicitly cleared by cqpsk_perm_reset() on sync loss. */
    if (global_ham <= CQPSK_PERM_LOCK_THRESHOLD) {
        /* Verify current window with locked permutation */
        int current_ham = compute_ham(best_idx, raw_dibits, expected_sync);
        if (current_ham <= CQPSK_PERM_EARLY_ACCEPT) {
            /* Still good - stay locked */
            *out_idx = best_idx;
            *out_ham = current_ham;
            return -1; /* locked - no search performed */
        }
        /* Current window degraded - fall through to search */
    }

    int best_ham = CQPSK_HAMMING_INIT;

    /* 1. Try current permutation first - often still valid */
    best_ham = compute_ham(best_idx, raw_dibits, expected_sync);
    if (best_ham <= CQPSK_PERM_EARLY_ACCEPT) {
        *out_idx = best_idx;
        *out_ham = best_ham;
        return 0; /* no search needed */
    }

    /* 2. Try the 4 phase rotation candidates - most likely after carrier re-lock */
    for (int i = 0; i < 4; i++) {
        int idx = kPhaseRotationIndices[i];
        if (idx == best_idx) {
            continue; /* already tried */
        }
        int ham = compute_ham(idx, raw_dibits, expected_sync);
        if (ham < best_ham) {
            best_ham = ham;
            best_idx = idx;
        }
        if (best_ham <= CQPSK_PERM_EARLY_ACCEPT) {
            *out_idx = best_idx;
            *out_ham = best_ham;
            return 1; /* found via phase rotation shortcut */
        }
    }

    /* 3. Full search - check remaining permutations */
    for (int mode = 0; mode < CQPSK_PERM_COUNT; mode++) {
        /* Skip indices we already checked */
        if (mode == cqpsk_perm_get_idx()) {
            continue;
        }
        int is_phase_rot = 0;
        for (int i = 0; i < 4; i++) {
            if (mode == kPhaseRotationIndices[i]) {
                is_phase_rot = 1;
                break;
            }
        }
        if (is_phase_rot) {
            continue;
        }

        int ham = compute_ham(mode, raw_dibits, expected_sync);
        if (ham < best_ham) {
            best_ham = ham;
            best_idx = mode;
        }
        /* Early exit on perfect match */
        if (best_ham == 0) {
            break;
        }
    }

    *out_idx = best_idx;
    *out_ham = best_ham;
    return 2; /* full search performed */
}

int
cqpsk_perm_search_n(const int* raw_dibits, const char* expected_sync, int sync_len, int* out_idx, int* out_ham) {
    int best_idx = cqpsk_perm_get_idx();
    int global_ham = cqpsk_perm_get_best_ham();

    /* Scale thresholds proportionally for shorter sync patterns.
     * P25P2 (20 dibits) should use ~83% of P25P1 (24 dibits) thresholds. */
    int early_accept = (CQPSK_PERM_EARLY_ACCEPT * sync_len + CQPSK_P25P1_SYNC_LEN - 1) / CQPSK_P25P1_SYNC_LEN;
    int lock_threshold = (CQPSK_PERM_LOCK_THRESHOLD * sync_len + CQPSK_P25P1_SYNC_LEN - 1) / CQPSK_P25P1_SYNC_LEN;

    /* 0. Check lock state - if we previously achieved a very good ham, verify the
     * current window still matches well before skipping search. */
    if (global_ham <= lock_threshold) {
        int current_ham = compute_ham_n(best_idx, raw_dibits, expected_sync, sync_len);
        if (current_ham <= early_accept) {
            *out_idx = best_idx;
            *out_ham = current_ham;
            return -1; /* locked - no search performed */
        }
    }

    int best_ham = CQPSK_HAMMING_INIT;

    /* 1. Try current permutation first - often still valid */
    best_ham = compute_ham_n(best_idx, raw_dibits, expected_sync, sync_len);
    if (best_ham <= early_accept) {
        *out_idx = best_idx;
        *out_ham = best_ham;
        return 0; /* no search needed */
    }

    /* 2. Try the 4 phase rotation candidates - most likely after carrier re-lock */
    for (int i = 0; i < 4; i++) {
        int idx = kPhaseRotationIndices[i];
        if (idx == best_idx) {
            continue;
        }
        int ham = compute_ham_n(idx, raw_dibits, expected_sync, sync_len);
        if (ham < best_ham) {
            best_ham = ham;
            best_idx = idx;
        }
        if (best_ham <= early_accept) {
            *out_idx = best_idx;
            *out_ham = best_ham;
            return 1; /* found via phase rotation shortcut */
        }
    }

    /* 3. Full search - check remaining permutations */
    for (int mode = 0; mode < CQPSK_PERM_COUNT; mode++) {
        if (mode == cqpsk_perm_get_idx()) {
            continue;
        }
        int is_phase_rot = 0;
        for (int i = 0; i < 4; i++) {
            if (mode == kPhaseRotationIndices[i]) {
                is_phase_rot = 1;
                break;
            }
        }
        if (is_phase_rot) {
            continue;
        }

        int ham = compute_ham_n(mode, raw_dibits, expected_sync, sync_len);
        if (ham < best_ham) {
            best_ham = ham;
            best_idx = mode;
        }
        if (best_ham == 0) {
            break;
        }
    }

    *out_idx = best_idx;
    *out_ham = best_ham;
    return 2; /* full search performed */
}
