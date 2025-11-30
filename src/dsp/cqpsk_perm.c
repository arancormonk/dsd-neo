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

        /* Only update if we found a better (or equal) Hamming distance */
        if (new_ham > current_ham) {
            return 0; /* no change - current is better */
        }

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
