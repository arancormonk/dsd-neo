// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Reset helpers: reset_all, reset_runtime, reset_wl
 */

#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int
all_zero_i16(const int16_t* a, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int
all_zero_i32(const int32_t* a, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != 0) {
            return 0;
        }
    }
    return 1;
}

int
main(void) {
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return 1;
    }
    memset(s, 0, sizeof(*s));
    cqpsk_init(s);

    /* Seed various state non-zero */
    s->cqpsk_eq.c_i[1] = 11;
    s->cqpsk_eq.c_q[2] = -9;
    s->cqpsk_eq.cw_i[0] = 5;
    s->cqpsk_eq.cw_q[1] = -3;
    s->cqpsk_eq.b_i[0] = 7;
    s->cqpsk_eq.b_q[1] = -8;
    for (int i = 0; i < 4; i++) {
        s->cqpsk_eq.d_i[i] = (i + 1) * 100;
        s->cqpsk_eq.d_q[i] = -(i + 1) * 200;
    }

    /* reset_wl only clears WL taps */
    cqpsk_reset_wl();
    if (!(all_zero_i16(s->cqpsk_eq.cw_i, CQPSK_EQ_MAX_TAPS) && all_zero_i16(s->cqpsk_eq.cw_q, CQPSK_EQ_MAX_TAPS))) {
        fprintf(stderr, "reset_wl did not clear WL taps\n");
        free(s);
        return 1;
    }
    if (!(s->cqpsk_eq.c_i[1] == 11 && s->cqpsk_eq.c_q[2] == -9)) {
        fprintf(stderr, "reset_wl altered FFE taps\n");
        free(s);
        return 1;
    }

    /* reset_runtime does not touch coefficients or b taps; clears histories/indices */
    int16_t c1 = s->cqpsk_eq.c_i[1], c2q = s->cqpsk_eq.c_q[2];
    int16_t b0 = s->cqpsk_eq.b_i[0];
    cqpsk_reset_runtime();
    if (!(s->cqpsk_eq.c_i[1] == c1 && s->cqpsk_eq.c_q[2] == c2q && s->cqpsk_eq.b_i[0] == b0)) {
        fprintf(stderr, "reset_runtime altered coefficients/taps\n");
        free(s);
        return 1;
    }
    if (!(s->cqpsk_eq.head == -1 && s->cqpsk_eq.update_count == 0 && s->cqpsk_eq.sym_count == 0
          && s->cqpsk_eq.cma_warmup == 0 && s->cqpsk_eq.sym_len == 0)) {
        fprintf(stderr, "reset_runtime did not clear counters/histories\n");
        free(s);
        return 1;
    }

    /* reset_all restores identity and clears WL/DFE + runtime */
    cqpsk_reset_all();
    if (s->cqpsk_eq.c_i[0] != (1 << 14)) {
        fprintf(stderr, "reset_all: center tap not identity\n");
        free(s);
        return 1;
    }
    for (int k = 1; k < CQPSK_EQ_MAX_TAPS; k++) {
        if (s->cqpsk_eq.c_i[k] != 0 || s->cqpsk_eq.c_q[k] != 0) {
            fprintf(stderr, "reset_all: non-center taps not cleared at %d\n", k);
            free(s);
            return 1;
        }
    }
    if (!(all_zero_i16(s->cqpsk_eq.cw_i, CQPSK_EQ_MAX_TAPS) && all_zero_i16(s->cqpsk_eq.cw_q, CQPSK_EQ_MAX_TAPS)
          && all_zero_i16(s->cqpsk_eq.b_i, 4) && all_zero_i16(s->cqpsk_eq.b_q, 4) && all_zero_i32(s->cqpsk_eq.d_i, 4)
          && all_zero_i32(s->cqpsk_eq.d_q, 4))) {
        fprintf(stderr, "reset_all did not clear WL/DFE\n");
        free(s);
        return 1;
    }

    free(s);
    return 0;
}
