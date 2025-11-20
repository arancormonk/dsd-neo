// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Runtime parameter behavior: clamps, toggles, and side-effects.
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
        fprintf(stderr, "alloc demod_state failed\n");
        return 1;
    }
    memset(s, 0, sizeof(*s));
    cqpsk_init(s);

    int lms, taps, mu, stride, wl, dfe, dfe_taps, cma_left;

    /* Even taps -> enforced odd; over-max -> capped */
    cqpsk_runtime_set_params(-1, 6, -1, -1, -1, -1, -1, -1);
    if (cqpsk_runtime_get_params(NULL, &taps, NULL, NULL, NULL, NULL, NULL, NULL) != 0 || taps != 7) {
        fprintf(stderr, "taps odd enforcement failed: %d\n", taps);
        free(s);
        return 1;
    }
    cqpsk_runtime_set_params(-1, 100, -1, -1, -1, -1, -1, -1);
    if (cqpsk_runtime_get_params(NULL, &taps, NULL, NULL, NULL, NULL, NULL, NULL) != 0 || taps != CQPSK_EQ_MAX_TAPS) {
        fprintf(stderr, "taps max clamp failed: %d\n", taps);
        free(s);
        return 1;
    }

    /* mu clamp to 128; update_stride unchanged on invalid (0) */
    int prev_stride = 0;
    if (cqpsk_runtime_get_params(NULL, NULL, NULL, &prev_stride, NULL, NULL, NULL, NULL) != 0) {
        fprintf(stderr, "get update_stride failed\n");
        free(s);
        return 1;
    }
    cqpsk_runtime_set_params(-1, -1, 200, 0, -1, -1, -1, -1);
    if (cqpsk_runtime_get_params(NULL, NULL, &mu, &stride, NULL, NULL, NULL, NULL) != 0) {
        fprintf(stderr, "get mu/stride failed\n");
        free(s);
        return 1;
    }
    if (mu != 128) {
        fprintf(stderr, "mu clamp failed: %d\n", mu);
        free(s);
        return 1;
    }
    if (stride != prev_stride) {
        fprintf(stderr, "stride changed on invalid set: %d->%d\n", prev_stride, stride);
        free(s);
        return 1;
    }

    /* WL disable resets WL taps */
    s->cqpsk_eq.wl_enable = 1;
    s->cqpsk_eq.cw_i[0] = 123;
    s->cqpsk_eq.cw_q[3] = -77;
    cqpsk_runtime_set_params(-1, -1, -1, -1, 0, -1, -1, -1);
    if (cqpsk_runtime_get_params(NULL, NULL, NULL, NULL, &wl, NULL, NULL, NULL) != 0 || wl != 0) {
        fprintf(stderr, "WL disable state not reflected\n");
        free(s);
        return 1;
    }
    if (!(all_zero_i16(s->cqpsk_eq.cw_i, CQPSK_EQ_MAX_TAPS) && all_zero_i16(s->cqpsk_eq.cw_q, CQPSK_EQ_MAX_TAPS))) {
        fprintf(stderr, "WL taps not cleared on disable\n");
        free(s);
        return 1;
    }

    /* DFE enable/disable reset behavior */
    s->cqpsk_eq.dfe_enable = 0;
    for (int i = 0; i < 4; i++) {
        s->cqpsk_eq.b_i[i] = (int16_t)(i + 1);
        s->cqpsk_eq.b_q[i] = (int16_t)(2 * (i + 1));
        s->cqpsk_eq.d_i[i] = 1000 * (i + 1);
        s->cqpsk_eq.d_q[i] = -1000 * (i + 1);
    }
    cqpsk_runtime_set_params(-1, -1, -1, -1, -1, 1, -1, -1);
    if (cqpsk_runtime_get_params(NULL, NULL, NULL, NULL, NULL, &dfe, &dfe_taps, NULL) != 0 || dfe != 1) {
        fprintf(stderr, "DFE enable not reflected\n");
        free(s);
        return 1;
    }
    if (!(all_zero_i16(s->cqpsk_eq.b_i, 4) && all_zero_i16(s->cqpsk_eq.b_q, 4) && all_zero_i32(s->cqpsk_eq.d_i, 4)
          && all_zero_i32(s->cqpsk_eq.d_q, 4))) {
        fprintf(stderr, "DFE not cleared on enable\n");
        free(s);
        return 1;
    }
    /* Disable clears as well */
    for (int i = 0; i < 4; i++) {
        s->cqpsk_eq.b_i[i] = (int16_t)(i + 1);
        s->cqpsk_eq.b_q[i] = (int16_t)(2 * (i + 1));
        s->cqpsk_eq.d_i[i] = 1000 * (i + 1);
        s->cqpsk_eq.d_q[i] = -1000 * (i + 1);
    }
    cqpsk_runtime_set_params(-1, -1, -1, -1, -1, 0, -1, -1);
    if (cqpsk_runtime_get_params(NULL, NULL, NULL, NULL, NULL, &dfe, NULL, NULL) != 0 || dfe != 0) {
        fprintf(stderr, "DFE disable not reflected\n");
        free(s);
        return 1;
    }
    if (!(all_zero_i16(s->cqpsk_eq.b_i, 4) && all_zero_i16(s->cqpsk_eq.b_q, 4) && all_zero_i32(s->cqpsk_eq.d_i, 4)
          && all_zero_i32(s->cqpsk_eq.d_q, 4))) {
        fprintf(stderr, "DFE not cleared on disable\n");
        free(s);
        return 1;
    }

    /* LMS enable injects default CMA warmup; disable resets to identity */
    s->cqpsk_eq.lms_enable = 0;
    s->cqpsk_eq.cma_warmup = 0;
    cqpsk_runtime_set_params(1, -1, -1, -1, -1, -1, -1, -1);
    if (cqpsk_runtime_get_params(&lms, NULL, NULL, NULL, NULL, NULL, NULL, &cma_left) != 0) {
        fprintf(stderr, "get lms/cma failed\n");
        free(s);
        return 1;
    }
    if (!(lms == 1 && cma_left == 1200)) {
        fprintf(stderr, "CMA warmup on LMS enable failed: lms=%d cma=%d\n", lms, cma_left);
        free(s);
        return 1;
    }
    /* Make taps non-identity, then disable LMS -> identity */
    s->cqpsk_eq.c_i[1] = 77;
    s->cqpsk_eq.c_q[2] = -55;
    cqpsk_runtime_set_params(0, -1, -1, -1, -1, -1, -1, -1);
    if (s->cqpsk_eq.c_i[0] != (1 << 14)) {
        fprintf(stderr, "identity center tap not restored\n");
        free(s);
        return 1;
    }
    for (int k = 1; k < CQPSK_EQ_MAX_TAPS; k++) {
        if (s->cqpsk_eq.c_i[k] != 0 || s->cqpsk_eq.c_q[k] != 0) {
            fprintf(stderr, "taps not cleared on LMS disable at k=%d\n", k);
            free(s);
            return 1;
        }
    }

    free(s);
    return 0;
}
