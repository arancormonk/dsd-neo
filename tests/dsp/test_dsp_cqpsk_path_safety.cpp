// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Safety tests: pre-init getters, null/short input handling, and auto-init.
 */

#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <stdlib.h>
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
    int lms = 0, taps = 0, mu = 0, stride = 0, wl = 0, dfe = 0, dfe_taps = 0, cma_left = 0;
    int dq = 0;

    /* Pre-init getters should fail (no bound demod) */
    if (cqpsk_runtime_get_params(&lms, &taps, &mu, &stride, &wl, &dfe, &dfe_taps, &cma_left) != -1) {
        fprintf(stderr, "expected get_params to fail before init\n");
        return 1;
    }
    if (cqpsk_runtime_get_dqpsk(&dq) != -1) {
        fprintf(stderr, "expected get_dqpsk to fail before init\n");
        return 1;
    }

    /* Null demod should be a no-op */
    cqpsk_process_block(NULL);

    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        fprintf(stderr, "alloc demod_state failed\n");
        return 1;
    }
    memset(s, 0, sizeof(*s));

    /* No buffer -> no-op, but also binds and inits on first call */
    s->lowpassed = NULL;
    s->lp_len = 0;
    cqpsk_process_block(s);

    /* Short length (<2) no-op: verify buffer unchanged */
    int16_t buf[2] = {1234, -5678};
    int16_t ref[2] = {1234, -5678};
    s->lowpassed = buf;
    s->lp_len = 1; /* odd/short */
    cqpsk_process_block(s);
    if (!arrays_equal(buf, ref, 2)) {
        fprintf(stderr, "short block modified unexpectedly\n");
        free(s);
        return 1;
    }

    /* Auto-init on first process when eq not initialized */
    demod_state* s2 = (demod_state*)malloc(sizeof(demod_state));
    if (!s2) {
        fprintf(stderr, "alloc demod_state s2 failed\n");
        free(s);
        return 1;
    }
    memset(s2, 0, sizeof(*s2));
    int16_t v[2] = {0, 0};
    s2->lowpassed = v;
    s2->lp_len = 2;
    cqpsk_process_block(s2);
    if (cqpsk_runtime_get_params(&lms, &taps, &mu, &stride, &wl, &dfe, &dfe_taps, &cma_left) != 0) {
        fprintf(stderr, "runtime_get_params failed after auto-init\n");
        free(s2);
        free(s);
        return 1;
    }

    free(s2);
    free(s);
    return 0;
}
