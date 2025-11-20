// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Tests: reset paths, zero-input stability, and symbol ring semantics.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <stdio.h>
#include <string.h>

static int
arrays_equal(const int16_t* a, const int16_t* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    cqpsk_eq_state_t st;
    memset(&st, 0, sizeof(st));
    cqpsk_eq_init(&st);

    // Verify reset_all produces identity and clears histories
    st.c_i[1] = 123;
    st.c_q[2] = -77;
    st.wl_enable = 1;
    st.cw_i[0] = 99;
    st.b_i[0] = 10;
    st.d_i[0] = 10;
    st.head = 3;
    cqpsk_eq_reset_all(&st);
    if (st.c_i[0] != (1 << 14) || st.c_q[0] != 0) {
        fprintf(stderr, "RESET_ALL: center tap not identity\n");
        return 1;
    }
    for (int k = 1; k < st.num_taps; k++) {
        if (st.c_i[k] != 0 || st.c_q[k] != 0) {
            fprintf(stderr, "RESET_ALL: taps not cleared\n");
            return 1;
        }
    }
    for (int k = 0; k < st.num_taps; k++) {
        if (st.cw_i[k] != 0 || st.cw_q[k] != 0) {
            fprintf(stderr, "RESET_ALL: WL taps not cleared\n");
            return 1;
        }
    }
    for (int i = 0; i < 4; i++) {
        if (st.b_i[i] != 0 || st.b_q[i] != 0 || st.d_i[i] != 0 || st.d_q[i] != 0) {
            fprintf(stderr, "RESET_ALL: DFE state not cleared\n");
            return 1;
        }
    }
    if (st.head != -1 || st.update_count != 0 || st.sym_count != 0 || st.sym_len != 0) {
        fprintf(stderr, "RESET_ALL: runtime counters not cleared\n");
        return 1;
    }

    // Zero-input stability with LMS on: updates should be skipped (low energy) and buffer unchanged
    st.lms_enable = 1;
    st.update_stride = 1;
    st.sym_stride = 4; // default-like
    int16_t zbuf[16];
    for (int i = 0; i < 16; i++) {
        zbuf[i] = 0;
    }
    int16_t zref[16];
    memcpy(zref, zbuf, sizeof(zbuf));
    cqpsk_eq_process_block(&st, zbuf, 1 /*len<2: early return*/);
    if (!arrays_equal(zbuf, zref, 16)) {
        fprintf(stderr, "ZERO: early return mutated buffer\n");
        return 1;
    }
    cqpsk_eq_process_block(&st, zbuf, 16);
    if (!arrays_equal(zbuf, zref, 16)) {
        fprintf(stderr, "ZERO: processing zeros mutated buffer\n");
        return 1;
    }

    // Symbol ring semantics: with sym_stride=4, ticks at indices 3,7,11,15
    cqpsk_eq_reset_all(&st);
    st.lms_enable = 0; // ensure identity response for ring semantics
    st.sym_stride = 4;
    const int P = 16; // 16 pairs -> 4 symbol ticks
    int16_t ibuf[P * 2];
    for (int k = 0; k < P; k++) {
        int16_t i = (k & 1) ? 5000 : -5000;
        int16_t q = (k & 2) ? 4000 : -4000;
        ibuf[2 * k + 0] = i;
        ibuf[2 * k + 1] = q;
    }
    int16_t ref[P * 2];
    memcpy(ref, ibuf, sizeof(ref));
    cqpsk_eq_process_block(&st, ibuf, P * 2);

    int16_t syms[16];
    int n = cqpsk_eq_get_symbols(&st, syms, 8);
    if (n != 4) {
        fprintf(stderr, "RING: expected 4 symbols, got %d\n", n);
        return 1;
    }
    // Expected captures at indices 3,7,11,15
    const int idxs[4] = {3, 7, 11, 15};
    for (int t = 0; t < 4; t++) {
        int idx = idxs[t];
        int16_t ai = syms[2 * t + 0], aq = syms[2 * t + 1];
        int16_t ei = ref[2 * idx + 0], eq = ref[2 * idx + 1];
        if (ai != ei || aq != eq) {
            fprintf(stderr, "RING: symbol %d mismatch vs expected sample %d (got %d,%d exp %d,%d)\n", t, idx, ai, aq,
                    ei, eq);
            return 1;
        }
    }

    return 0;
}
