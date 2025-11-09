// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Environment override parsing and clamping.
 */

#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
unset_all_env(void) {
    /* Core CQPSK envs */
    unsetenv("DSD_NEO_CQPSK_LMS");
    unsetenv("DSD_NEO_CQPSK_TAPS");
    unsetenv("DSD_NEO_CQPSK_MU");
    unsetenv("DSD_NEO_CQPSK_STRIDE");
    unsetenv("DSD_NEO_CQPSK_WL");
    unsetenv("DSD_NEO_CQPSK_WL_LEAK");
    unsetenv("DSD_NEO_CQPSK_WL_THR");
    unsetenv("DSD_NEO_CQPSK_WL_MU");
    unsetenv("DSD_NEO_CQPSK_ADAPT_HOLD");
    unsetenv("DSD_NEO_CQPSK_WL_THR_OFF");
    unsetenv("DSD_NEO_CQPSK_WL_EMA");
    unsetenv("DSD_NEO_CQPSK_DFE");
    unsetenv("DSD_NEO_CQPSK_DFE_TAPS");
    unsetenv("DSD_NEO_CQPSK_CMA");
    unsetenv("DSD_NEO_CQPSK_CMA_MU");
    unsetenv("DSD_NEO_CQPSK_DQPSK");
}

int
main(void) {
    /* TAPS even + over-max clamp */
    unset_all_env();
    setenv("DSD_NEO_CQPSK_TAPS", "12", 1);
    demod_state* s1 = (demod_state*)malloc(sizeof(demod_state));
    if (!s1) {
        return 1;
    }
    memset(s1, 0, sizeof(*s1));
    cqpsk_init(s1);
    if (s1->cqpsk_eq.num_taps != CQPSK_EQ_MAX_TAPS) {
        fprintf(stderr, "env taps clamp/odd failed: %d\n", s1->cqpsk_eq.num_taps);
        free(s1);
        return 1;
    }
    free(s1);

    /* MU and STRIDE clamped */
    unset_all_env();
    setenv("DSD_NEO_CQPSK_MU", "64", 1);
    setenv("DSD_NEO_CQPSK_STRIDE", "31", 1);
    demod_state* s2 = (demod_state*)malloc(sizeof(demod_state));
    if (!s2) {
        return 1;
    }
    memset(s2, 0, sizeof(*s2));
    cqpsk_init(s2);
    if (!(s2->cqpsk_eq.mu_q15 == 64 && s2->cqpsk_eq.update_stride == 31)) {
        fprintf(stderr, "env mu/stride failed: mu=%d stride=%d\n", s2->cqpsk_eq.mu_q15, s2->cqpsk_eq.update_stride);
        free(s2);
        return 1;
    }
    free(s2);

    /* WL gating params: leak clamp, percent/fraction parsing, EMA */
    unset_all_env();
    setenv("DSD_NEO_CQPSK_WL", "1", 1);
    setenv("DSD_NEO_CQPSK_WL_LEAK", "2", 1);  /* clamp up to 4 */
    setenv("DSD_NEO_CQPSK_WL_THR", "2.0", 1); /* percent -> ~655 */
    setenv("DSD_NEO_CQPSK_WL_MU", "5", 1);
    setenv("DSD_NEO_CQPSK_WL_THR_OFF", "10.0", 1); /* percent -> ~3277 */
    setenv("DSD_NEO_CQPSK_WL_EMA", "0.5", 1);      /* fraction -> 16384 */
    demod_state* s3 = (demod_state*)malloc(sizeof(demod_state));
    if (!s3) {
        return 1;
    }
    memset(s3, 0, sizeof(*s3));
    cqpsk_init(s3);
    if (s3->cqpsk_eq.wl_enable != 1) {
        fprintf(stderr, "env WL enable failed\n");
        free(s3);
        return 1;
    }
    if (s3->cqpsk_eq.wl_leak_shift != 4) {
        fprintf(stderr, "env WL leak clamp failed: %d\n", s3->cqpsk_eq.wl_leak_shift);
        free(s3);
        return 1;
    }
    if (s3->cqpsk_eq.wl_gate_thr_q15 != 655) {
        fprintf(stderr, "env WL thr (percent) failed: %d\n", s3->cqpsk_eq.wl_gate_thr_q15);
        free(s3);
        return 1;
    }
    if (s3->cqpsk_eq.wl_mu_q15 != 5) {
        fprintf(stderr, "env WL mu failed: %d\n", s3->cqpsk_eq.wl_mu_q15);
        free(s3);
        return 1;
    }
    if (s3->cqpsk_eq.wl_thr_off_q15 != 3277) {
        fprintf(stderr, "env WL thr_off (percent) failed: %d\n", s3->cqpsk_eq.wl_thr_off_q15);
        free(s3);
        return 1;
    }
    if (s3->cqpsk_eq.wl_improp_alpha_q15 != 16384) {
        fprintf(stderr, "env WL EMA alpha failed: %d\n", s3->cqpsk_eq.wl_improp_alpha_q15);
        free(s3);
        return 1;
    }
    free(s3);

    /* DFE default taps when enabled and DFE_TAPS unset */
    unset_all_env();
    setenv("DSD_NEO_CQPSK_DFE", "1", 1);
    demod_state* s4 = (demod_state*)malloc(sizeof(demod_state));
    if (!s4) {
        return 1;
    }
    memset(s4, 0, sizeof(*s4));
    cqpsk_init(s4);
    if (!(s4->cqpsk_eq.dfe_enable == 1 && s4->cqpsk_eq.dfe_taps == 2)) {
        fprintf(stderr, "env DFE default taps failed: en=%d taps=%d\n", s4->cqpsk_eq.dfe_enable, s4->cqpsk_eq.dfe_taps);
        free(s4);
        return 1;
    }
    free(s4);

    /* CMA warmup and CMA MU */
    unset_all_env();
    setenv("DSD_NEO_CQPSK_CMA", "1234", 1);
    setenv("DSD_NEO_CQPSK_CMA_MU", "7", 1);
    demod_state* s5 = (demod_state*)malloc(sizeof(demod_state));
    if (!s5) {
        return 1;
    }
    memset(s5, 0, sizeof(*s5));
    cqpsk_init(s5);
    if (!(s5->cqpsk_eq.cma_warmup == 1234 && s5->cqpsk_eq.cma_mu_q15 == 7)) {
        fprintf(stderr, "env CMA cfg failed: warm=%d mu=%d\n", s5->cqpsk_eq.cma_warmup, s5->cqpsk_eq.cma_mu_q15);
        free(s5);
        return 1;
    }
    free(s5);

    /* DQPSK decision mode */
    unset_all_env();
    setenv("DSD_NEO_CQPSK_DQPSK", "1", 1);
    demod_state* s6 = (demod_state*)malloc(sizeof(demod_state));
    if (!s6) {
        return 1;
    }
    memset(s6, 0, sizeof(*s6));
    cqpsk_init(s6);
    if (s6->cqpsk_eq.dqpsk_decision != 1) {
        fprintf(stderr, "env DQPSK enable failed\n");
        free(s6);
        return 1;
    }
    free(s6);

    return 0;
}
