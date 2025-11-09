// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * DQPSK vs axis slicer affects DFE feedback and thus output samples.
 *
 * Use 3 symbols (4 SPS): 45°, 135°, 45°. With DFE enabled and b_i[0] > 0,
 * the third symbol's output (sample at index 11) differs between slicers
 * because the previous symbol's decision d[0] (from symbol 2) differs.
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
build_three_sym_45_135_45(int16_t* iq, int sps, int amp_q0) {
    for (int n = 0; n < sps; n++) {
        iq[2 * n + 0] = (int16_t)amp_q0; /* 45° */
        iq[2 * n + 1] = (int16_t)amp_q0;
    }
    for (int n = 0; n < sps; n++) {
        iq[2 * (sps + n) + 0] = (int16_t)(-amp_q0); /* 135° */
        iq[2 * (sps + n) + 1] = (int16_t)amp_q0;
    }
    for (int n = 0; n < sps; n++) {
        iq[2 * (2 * sps + n) + 0] = (int16_t)amp_q0; /* 45° */
        iq[2 * (2 * sps + n) + 1] = (int16_t)amp_q0;
    }
}

static int
run_capture_last(int dqpsk_mode, int* out_yI_q0, int* out_yQ_q0) {
    const int sps = 4;
    const int symN = 3;
    const int amp_q0 = 8192;
    const int last_sample = sps * symN - 1; /* 11 */
    int16_t buf[2 * sps * symN];
    build_three_sym_45_135_45(buf, sps, amp_q0);

    demod_state* s = (demod_state*)malloc(sizeof(demod_state));
    if (!s) {
        return -1;
    }
    memset(s, 0, sizeof(*s));
    s->ted_sps = sps; /* sym_stride = sps */
    cqpsk_init(s);

    /* Enable DFE with 1 feedback tap and set b_i[0] to max for visible effect */
    cqpsk_runtime_set_params(-1, -1, -1, -1, -1, 1, 1, -1);
    s->cqpsk_eq.b_i[0] = (int16_t)((1 << 14) - 1); /* 32767 */
    s->cqpsk_eq.b_q[0] = 0;

    /* Toggle slicer mode */
    cqpsk_runtime_set_dqpsk(dqpsk_mode);

    s->lowpassed = buf;
    s->lp_len = 2 * sps * symN;
    cqpsk_process_block(s);

    *out_yI_q0 = buf[2 * last_sample + 0];
    *out_yQ_q0 = buf[2 * last_sample + 1];
    free(s);
    return 0;
}

int
main(void) {
    int yI_axis = 0, yQ_axis = 0;
    int yI_dq = 0, yQ_dq = 0;
    if (run_capture_last(0, &yI_axis, &yQ_axis) != 0) {
        fprintf(stderr, "axis run failed\n");
        return 1;
    }
    if (run_capture_last(1, &yI_dq, &yQ_dq) != 0) {
        fprintf(stderr, "dqpsk run failed\n");
        return 1;
    }

    /* Expect I differs due to different previous decisions (axis vs DQPSK). */
    int diffI = yI_axis - yI_dq;
    if (diffI < 0) {
        diffI = -diffI;
    }
    if (diffI < 1) {
        fprintf(stderr, "DFE feedback effect too small on I: axis=%d dq=%d diff=%d\n", yI_axis, yI_dq, diffI);
        return 1;
    }

    return 0;
}
