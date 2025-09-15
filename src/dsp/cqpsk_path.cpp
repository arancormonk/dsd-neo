// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/dsp/cqpsk_equalizer.h>
#include <dsd-neo/dsp/cqpsk_path.h>
#include <dsd-neo/dsp/demod_state.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Minimal CQPSK path wrapper.
 *
 * For now, applies a pass-through complex equalizer. Future updates:
 *  - Decision-directed LMS coefficient update
 *  - Optional soft limiter pre/post EQ
 */

typedef struct cqpsk_ctx_s {
    cqpsk_eq_state_t eq;
    int initialized;
} cqpsk_ctx_t;

/* Single-instance context; revisit if multiple demods are used concurrently. */
static cqpsk_ctx_t g_cqpsk_ctx;

void
cqpsk_init(struct demod_state* s) {
    (void)s;
    memset(&g_cqpsk_ctx, 0, sizeof(g_cqpsk_ctx));
    cqpsk_eq_init(&g_cqpsk_ctx.eq);
    /* Configure from demod_state first (CLI/runtime), then allow env to override if set */
    if (s) {
        g_cqpsk_ctx.eq.lms_enable = (s->cqpsk_lms_enable != 0);
        if (s->cqpsk_mu_q15 > 0) {
            g_cqpsk_ctx.eq.mu_q15 = (int16_t)s->cqpsk_mu_q15;
        }
        if (s->cqpsk_update_stride > 0) {
            g_cqpsk_ctx.eq.update_stride = s->cqpsk_update_stride;
        }
    }
    /* Optional env overrides for quick experiments */
    const char* lms = getenv("DSD_NEO_CQPSK_LMS");
    if (lms && (*lms == '1' || *lms == 'y' || *lms == 'Y' || *lms == 't' || *lms == 'T')) {
        g_cqpsk_ctx.eq.lms_enable = 1;
    }
    const char* mu = getenv("DSD_NEO_CQPSK_MU");
    if (mu) {
        int v = atoi(mu);
        if (v >= 1 && v <= 64) {
            g_cqpsk_ctx.eq.mu_q15 = (int16_t)v; /* very small steps */
        }
    }
    const char* stride = getenv("DSD_NEO_CQPSK_STRIDE");
    if (stride) {
        int v = atoi(stride);
        if (v >= 1 && v <= 32) {
            g_cqpsk_ctx.eq.update_stride = v;
        }
    }
    g_cqpsk_ctx.initialized = 1;
}

void
cqpsk_process_block(struct demod_state* s) {
    if (!g_cqpsk_ctx.initialized) {
        cqpsk_init(s);
    }
    if (!s || !s->lowpassed || s->lp_len < 2) {
        return;
    }
    /* In-place EQ on interleaved I/Q */
    cqpsk_eq_process_block(&g_cqpsk_ctx.eq, s->lowpassed, s->lp_len);
}
