// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DQPSK slicer smoke test via CQPSK path with DFE enabled.
 *
 * Generates two symbols at 45° and 135° with 4 SPS. With axis-aligned slicer,
 * the last decision is (-A, +A). With DQPSK-aware slicer, rotate-back yields
 * (0, +A). We verify dfe decision history reflects this difference.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
build_two_sym_45_135(int16_t* iq, int sps, int amp_q0) {
    /* symbol 0: 45° (A, A); symbol 1: 135° (-A, A) */
    for (int n = 0; n < sps; n++) {
        iq[2 * n + 0] = (int16_t)amp_q0;
        iq[2 * n + 1] = (int16_t)amp_q0;
    }
    for (int n = 0; n < sps; n++) {
        iq[2 * (sps + n) + 0] = (int16_t)(-amp_q0);
        iq[2 * (sps + n) + 1] = (int16_t)amp_q0;
    }
}

static int
run_once(int dqpsk_mode, int32_t* out_di, int32_t* out_dq) {
    const int sps = 4;
    const int symN = 2;
    const int amp_q0 = 8192; /* avoid clipping */
    int16_t buf[2 * sps * symN];
    build_two_sym_45_135(buf, sps, amp_q0);

    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->ted_sps = sps; /* sym_stride = sps */
    cqpsk_init(s);
    /* Enable DFE with 1 tap so decisions shift-in at symbol ticks */
    cqpsk_runtime_set_params(-1, -1, -1, -1, -1, 1, 1, -1);
    /* Toggle DQPSK mode */
    cqpsk_runtime_set_dqpsk(dqpsk_mode);

    s->lowpassed = buf;
    s->lp_len = 2 * sps * symN;
    cqpsk_process_block(s);

    if (out_di) {
        *out_di = s->cqpsk_eq.d_i[0];
    }
    if (out_dq) {
        *out_dq = s->cqpsk_eq.d_q[0];
    }
    free(s);
    return 0;
}

int
main(void) {
    const int A_q14 = (1 << 14);
    int32_t di_axis = 0, dq_axis = 0;
    int32_t di_dqpsk = 0, dq_dqpsk = 0;

    if (run_once(0, &di_axis, &dq_axis) != 0) {
        fprintf(stderr, "axis run failed\n");
        return 1;
    }
    if (run_once(1, &di_dqpsk, &dq_dqpsk) != 0) {
        fprintf(stderr, "dqpsk run failed\n");
        return 1;
    }

    /* Expect last decision differs on I component only:
       axis (-A, +A) vs dqpsk (0, 2*A). 2*A arises from rotate-back with
       normalized previous symbol at 45°, yielding doubled imag component. */
    if (!(di_axis == -A_q14 && dq_axis == +A_q14)) {
        fprintf(stderr, "axis decisions unexpected: di=%d dq=%d\n", di_axis, dq_axis);
        return 1;
    }
    if (!(di_dqpsk == 0 && dq_dqpsk == +2 * A_q14)) {
        fprintf(stderr, "dqpsk decisions unexpected: di=%d dq=%d\n", di_dqpsk, dq_dqpsk);
        return 1;
    }

    return 0;
}
