// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Unit test: CQPSK path init, runtime params, and pass-through processing. */

#include <cstdlib>
#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_state.h>
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
    // Allocate demod_state on heap (large struct)
    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        fprintf(stderr, "alloc demod_state failed\n");
        return 1;
    }
    memset(s, 0, sizeof(*s));
    s->ted_sps = 10;            // typical SPS for 48k/4.8k
    s->cqpsk_lms_enable = 0;    // default off
    s->cqpsk_mu_q15 = 0;        // keep default inside path
    s->cqpsk_update_stride = 0; // keep default inside path

    cqpsk_init(s);

    // Verify derived defaults from SPS: taps and sym_stride
    int lms, taps, mu, stride, wl, dfe, dfe_taps, cma_left;
    if (cqpsk_runtime_get_params(&lms, &taps, &mu, &stride, &wl, &dfe, &dfe_taps, &cma_left) != 0) {
        fprintf(stderr, "runtime_get_params failed\n");
        return 1;
    }
    if (taps != 7) { // sps>=8 chooses 7 taps by default
        fprintf(stderr, "unexpected taps=%d (want 7)\n", taps);
        return 1;
    }
    if (stride <= 0) {
        fprintf(stderr, "unexpected update_stride=%d\n", stride);
        return 1;
    }

    // Process pass-through block
    const int pairs = 20;
    int16_t buf[pairs * 2];
    int16_t ref[pairs * 2];
    for (int k = 0; k < pairs; k++) {
        int16_t i = (int16_t)(k * 100 - 1000);
        int16_t q = (int16_t)(1000 - k * 50);
        buf[2 * k + 0] = i;
        buf[2 * k + 1] = q;
        ref[2 * k + 0] = i;
        ref[2 * k + 1] = q;
    }
    s->lowpassed = buf;
    s->lp_len = pairs * 2;
    cqpsk_process_block(s);
    if (!arrays_equal(buf, ref, pairs * 2)) {
        fprintf(stderr, "CQPSK path pass-through mismatch\n");
        return 1;
    }

    // Toggle DFE parameters and DQPSK and verify via getters
    cqpsk_runtime_set_params(-1, -1, -1, -1, -1, 1, 2, -1);
    if (cqpsk_runtime_get_params(NULL, NULL, NULL, NULL, NULL, &dfe, &dfe_taps, NULL) != 0) {
        fprintf(stderr, "runtime_get_params failed (post-set)\n");
        return 1;
    }
    if (!(dfe == 1 && dfe_taps == 2)) {
        fprintf(stderr, "DFE toggles mismatch: dfe=%d taps=%d\n", dfe, dfe_taps);
        return 1;
    }
    cqpsk_runtime_set_dqpsk(1);
    int dq = 0;
    if (cqpsk_runtime_get_dqpsk(&dq) != 0 || dq != 1) {
        fprintf(stderr, "DQPSK toggle mismatch\n");
        return 1;
    }

    free(s);
    return 0;
}
