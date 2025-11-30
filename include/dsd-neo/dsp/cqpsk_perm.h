// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * CQPSK dibit permutation table and constants for constellation rotation correction.
 *
 * This header consolidates the 24-permutation lookup table used by both sync detection
 * (dsd_frame_sync.c) and frame decoding (dsd_dibit.c) to ensure consistency.
 */

#ifndef DSD_NEO_DSP_CQPSK_PERM_H
#define DSD_NEO_DSP_CQPSK_PERM_H

#ifdef __cplusplus
extern "C" {
#endif

/* Number of possible dibit permutations (4! = 24). */
#define CQPSK_PERM_COUNT             24

/* Number of dibits in a P25 sync pattern. */
#define CQPSK_SYNC_LEN               24

/* Debug histogram reset period in samples. */
#define CQPSK_DEBUG_HISTOGRAM_PERIOD 4800

/* Initial Hamming distance threshold (effectively "no match found yet"). */
#define CQPSK_HAMMING_INIT           1000

/* Early accept threshold for smart search - if current/phase-rotation
 * permutation gives ham <= this value, skip full 24-permutation search. */
#define CQPSK_PERM_EARLY_ACCEPT      2

/* Lock threshold - once we achieve ham <= this value, consider the permutation
 * locked and don't search again until reset (sync loss). This prevents thrashing
 * between equally-good permutations that can occur with noisy/drifting signals. */
#define CQPSK_PERM_LOCK_THRESHOLD    1

/* CQPSK symbol level thresholds for 4-level slicer.
 * Symbol levels are at +/-1, +/-3; thresholds at +/-2.0 and 0. */
#define CQPSK_THRESH_UPPER           (2.0f)
#define CQPSK_THRESH_LOWER           (-2.0f)

/* All 24 permutations of dibit mappings (0..3).
 * Each row maps input dibit [0,1,2,3] to output dibit.
 * Used to correct constellation rotation discovered during sync detection.
 * Defined in cqpsk_perm.c to avoid duplicate symbols. */
extern const int kCqpskPerms[CQPSK_PERM_COUNT][4];

/* Invert a dibit (swap 0<->2, 1<->3) - common CQPSK operation. */
static inline int
cqpsk_invert_dibit(int dibit) {
    switch (dibit & 0x3) {
        case 0: return 2;
        case 1: return 3;
        case 2: return 0;
        default: return 1; /* case 3 */
    }
}

/* Apply permutation mapping to a dibit.
 * perm_idx must be in range [0, CQPSK_PERM_COUNT-1]. */
int cqpsk_apply_perm(int perm_idx, int dibit);

/* Reset global CQPSK permutation state.
 * Call when sync is lost or switching to a new signal source. */
void cqpsk_perm_reset(void);

/* Get current permutation index (for state propagation). */
int cqpsk_perm_get_idx(void);

/* Get current best Hamming distance. */
int cqpsk_perm_get_best_ham(void);

/* Update permutation state during sync search.
 * Returns 1 if permutation changed, 0 otherwise. */
int cqpsk_perm_update(int new_idx, int new_ham);

/* Smart permutation search with early exit and lock optimization.
 * Searches for best dibit mapping in order:
 *   0. Check lock state - if previously locked (ham <= LOCK_THRESHOLD), skip search
 *   1. Current permutation (often still valid)
 *   2. Four QPSK phase rotation candidates (most likely after carrier re-lock)
 *   3. Remaining permutations (full search fallback)
 * Early exits when ham <= CQPSK_PERM_EARLY_ACCEPT.
 *
 * @param raw_dibits     Pointer to CQPSK_SYNC_LEN raw dibits (0-3 values)
 * @param expected_sync  Expected sync pattern as ASCII '0'-'3' string
 * @param out_idx        Output: best permutation index found
 * @param out_ham        Output: Hamming distance for best permutation
 * @return -1 = locked (no search), 0 = current perm accepted,
 *          1 = phase rotation hit, 2 = full search */
int cqpsk_perm_search(const int* raw_dibits, const char* expected_sync, int* out_idx, int* out_ham);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_DSP_CQPSK_PERM_H */
