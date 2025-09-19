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
        /* If samples-per-symbol is known, pick a small odd tap count relative to it (cap at MAX). */
        if (s->ted_sps >= 4 && s->ted_sps <= 12) {
            int taps = 5;
            if (s->ted_sps >= 8) {
                taps = 7;
            }
            if (taps > CQPSK_EQ_MAX_TAPS) {
                taps = CQPSK_EQ_MAX_TAPS;
            }
            if ((taps & 1) == 0) {
                taps++; /* enforce odd */
            }
            g_cqpsk_ctx.eq.num_taps = taps;
            /* Use SPS as DFE symbol gating stride */
            g_cqpsk_ctx.eq.sym_stride = s->ted_sps;
        }
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
    const char* taps = getenv("DSD_NEO_CQPSK_TAPS");
    if (taps) {
        int v = atoi(taps);
        if (v < 1) {
            v = 1;
        }
        if (v > CQPSK_EQ_MAX_TAPS) {
            v = CQPSK_EQ_MAX_TAPS;
        }
        if ((v & 1) == 0) {
            v++; /* odd only */
        }
        g_cqpsk_ctx.eq.num_taps = v;
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
    /* Default symbol gating to update stride if SPS is unknown */
    if (g_cqpsk_ctx.eq.sym_stride <= 0) {
        g_cqpsk_ctx.eq.sym_stride = g_cqpsk_ctx.eq.update_stride;
    }
    const char* wl = getenv("DSD_NEO_CQPSK_WL");
    if (wl && (*wl == '1' || *wl == 'y' || *wl == 'Y' || *wl == 't' || *wl == 'T')) {
        g_cqpsk_ctx.eq.wl_enable = 1;
    }
    /* WL stability knobs: leakage and impropriety gate threshold */
    const char* wl_leak = getenv("DSD_NEO_CQPSK_WL_LEAK");
    if (wl_leak) {
        int v = atoi(wl_leak);
        if (v < 4) {
            v = 4; /* min */
        }
        if (v > 16) {
            v = 16; /* max */
        }
        g_cqpsk_ctx.eq.wl_leak_shift = v;
    }
    const char* wl_thr = getenv("DSD_NEO_CQPSK_WL_THR");
    if (wl_thr) {
        /* Accept either fraction (e.g., 0.02) or percent (e.g., 2.0) */
        double tv = atof(wl_thr);
        if (tv > 0.0) {
            if (tv >= 1.0) {
                tv = tv / 100.0; /* interpret as percent */
            }
            if (tv < 0.0001) {
                tv = 0.0001; /* clamp */
            }
            if (tv > 0.5) {
                tv = 0.5;
            }
            int thr_q15 = (int)(tv * 32768.0 + 0.5);
            if (thr_q15 < 1) {
                thr_q15 = 1;
            }
            if (thr_q15 > 32767) {
                thr_q15 = 32767;
            }
            g_cqpsk_ctx.eq.wl_gate_thr_q15 = thr_q15;
        }
    }
    const char* wl_mu = getenv("DSD_NEO_CQPSK_WL_MU");
    if (wl_mu) {
        int v = atoi(wl_mu);
        if (v >= 1 && v <= 64) {
            g_cqpsk_ctx.eq.wl_mu_q15 = v;
        }
    }
    const char* hold = getenv("DSD_NEO_CQPSK_ADAPT_HOLD");
    if (hold) {
        int v = atoi(hold);
        if (v < 8) {
            v = 8;
        }
        if (v > 1024) {
            v = 1024;
        }
        g_cqpsk_ctx.eq.adapt_min_hold = v;
    }
    const char* thr_off = getenv("DSD_NEO_CQPSK_WL_THR_OFF");
    if (thr_off) {
        double tv = atof(thr_off);
        if (tv > 0.0) {
            if (tv >= 1.0) {
                tv = tv / 100.0;
            }
            if (tv < 0.0001) {
                tv = 0.0001;
            }
            if (tv > 0.9) {
                tv = 0.9;
            }
            int toff = (int)(tv * 32768.0 + 0.5);
            if (toff < 1) {
                toff = 1;
            }
            if (toff > 32767) {
                toff = 32767;
            }
            g_cqpsk_ctx.eq.wl_thr_off_q15 = toff;
        }
    }
    const char* wl_ema = getenv("DSD_NEO_CQPSK_WL_EMA");
    if (wl_ema) {
        /* Fraction or percent */
        double tv = atof(wl_ema);
        if (tv > 0.0) {
            if (tv >= 1.0) {
                tv = tv / 100.0;
            }
            if (tv < 0.01) {
                tv = 0.01;
            }
            if (tv > 0.9) {
                tv = 0.9;
            }
            int a = (int)(tv * 32768.0 + 0.5);
            if (a < 1) {
                a = 1;
            }
            if (a > 32767) {
                a = 32767;
            }
            g_cqpsk_ctx.eq.wl_improp_alpha_q15 = a;
        }
    }
    const char* dfe = getenv("DSD_NEO_CQPSK_DFE");
    if (dfe && (*dfe == '1' || *dfe == 'y' || *dfe == 'Y' || *dfe == 't' || *dfe == 'T')) {
        g_cqpsk_ctx.eq.dfe_enable = 1;
    }
    const char* dfe_t = getenv("DSD_NEO_CQPSK_DFE_TAPS");
    if (dfe_t) {
        int v = atoi(dfe_t);
        if (v < 0) {
            v = 0;
        }
        if (v > 4) {
            v = 4;
        }
        g_cqpsk_ctx.eq.dfe_taps = v;
    } else if (g_cqpsk_ctx.eq.dfe_enable && g_cqpsk_ctx.eq.dfe_taps == 0) {
        g_cqpsk_ctx.eq.dfe_taps = 2; /* small default */
    }
    const char* cma = getenv("DSD_NEO_CQPSK_CMA");
    if (cma) {
        int v = atoi(cma);
        if (v < 0) {
            v = 0;
        }
        if (v > 20000) {
            v = 20000;
        }
        g_cqpsk_ctx.eq.cma_warmup = v; /* samples */
    }
    const char* cma_mu = getenv("DSD_NEO_CQPSK_CMA_MU");
    if (cma_mu) {
        int v = atoi(cma_mu);
        if (v >= 1 && v <= 64) {
            g_cqpsk_ctx.eq.cma_mu_q15 = (int16_t)v;
        }
    }
    g_cqpsk_ctx.initialized = 1;

    /* DQPSK decision mode (env: DSD_NEO_CQPSK_DQPSK=1) */
    const char* dq = getenv("DSD_NEO_CQPSK_DQPSK");
    if (dq && (*dq == '1' || *dq == 'y' || *dq == 'Y' || *dq == 't' || *dq == 'T')) {
        g_cqpsk_ctx.eq.dqpsk_decision = 1;
    }
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

extern "C" void
cqpsk_reset_all(void) {
    if (!g_cqpsk_ctx.initialized) {
        return;
    }
    cqpsk_eq_reset_all(&g_cqpsk_ctx.eq);
}

extern "C" void
cqpsk_reset_runtime(void) {
    if (!g_cqpsk_ctx.initialized) {
        return;
    }
    cqpsk_eq_reset_runtime(&g_cqpsk_ctx.eq);
}

extern "C" void
cqpsk_reset_wl(void) {
    if (!g_cqpsk_ctx.initialized) {
        return;
    }
    cqpsk_eq_reset_wl(&g_cqpsk_ctx.eq);
}

extern "C" void
cqpsk_runtime_set_params(int lms_enable, int taps, int mu_q15, int update_stride, int wl_enable, int dfe_enable,
                         int dfe_taps, int cma_warmup_samples) {
    if (!g_cqpsk_ctx.initialized) {
        return;
    }
    int prev_lms = g_cqpsk_ctx.eq.lms_enable;
    int prev_dfe = g_cqpsk_ctx.eq.dfe_enable;
    if (lms_enable >= 0) {
        g_cqpsk_ctx.eq.lms_enable = lms_enable ? 1 : 0;
    }
    if (taps >= 1) {
        if (taps > CQPSK_EQ_MAX_TAPS) {
            taps = CQPSK_EQ_MAX_TAPS;
        }
        if ((taps & 1) == 0) {
            taps++;
        }
        g_cqpsk_ctx.eq.num_taps = taps;
    }
    if (mu_q15 >= 1) {
        if (mu_q15 > 128) {
            mu_q15 = 128;
        }
        g_cqpsk_ctx.eq.mu_q15 = (int16_t)mu_q15;
    }
    if (update_stride >= 1) {
        g_cqpsk_ctx.eq.update_stride = update_stride;
    }
    if (wl_enable >= 0) {
        int prev = g_cqpsk_ctx.eq.wl_enable;
        g_cqpsk_ctx.eq.wl_enable = wl_enable ? 1 : 0;
        if (prev && !g_cqpsk_ctx.eq.wl_enable) {
            /* Reset WL taps on disable */
            cqpsk_eq_reset_wl(&g_cqpsk_ctx.eq);
        }
    }
    if (dfe_enable >= 0) {
        int newd = dfe_enable ? 1 : 0;
        g_cqpsk_ctx.eq.dfe_enable = newd;
        if (newd && !prev_dfe) {
            /* Safe-enable: clear DFE taps and decision history */
            cqpsk_eq_reset_dfe(&g_cqpsk_ctx.eq);
        } else if (!newd && prev_dfe) {
            /* Reset on disable to avoid stale feedback */
            cqpsk_eq_reset_dfe(&g_cqpsk_ctx.eq);
        }
    }
    if (dfe_taps >= 0) {
        if (dfe_taps > 4) {
            dfe_taps = 4;
        }
        g_cqpsk_ctx.eq.dfe_taps = dfe_taps;
    }
    if (cma_warmup_samples >= 0) {
        g_cqpsk_ctx.eq.cma_warmup = cma_warmup_samples;
    }
    /* If LMS just turned on and no explicit CMA requested, kick a small warmup for stability */
    if (!prev_lms && g_cqpsk_ctx.eq.lms_enable) {
        if (g_cqpsk_ctx.eq.cma_warmup <= 0) {
            g_cqpsk_ctx.eq.cma_warmup = 1200; /* ~1/4 second at 4800 sym/s */
        }
    } else if (prev_lms && !g_cqpsk_ctx.eq.lms_enable) {
        /* On LMS disable, reset EQ to identity to stabilize pipeline */
        cqpsk_eq_reset_all(&g_cqpsk_ctx.eq);
    }
}

extern "C" int
cqpsk_runtime_get_params(int* lms_enable, int* taps, int* mu_q15, int* update_stride, int* wl_enable, int* dfe_enable,
                         int* dfe_taps, int* cma_warmup_remaining) {
    if (!g_cqpsk_ctx.initialized) {
        return -1;
    }
    if (lms_enable) {
        *lms_enable = g_cqpsk_ctx.eq.lms_enable ? 1 : 0;
    }
    if (taps) {
        *taps = g_cqpsk_ctx.eq.num_taps;
    }
    if (mu_q15) {
        *mu_q15 = g_cqpsk_ctx.eq.mu_q15;
    }
    if (update_stride) {
        *update_stride = g_cqpsk_ctx.eq.update_stride;
    }
    if (wl_enable) {
        *wl_enable = g_cqpsk_ctx.eq.wl_enable ? 1 : 0;
    }
    if (dfe_enable) {
        *dfe_enable = g_cqpsk_ctx.eq.dfe_enable ? 1 : 0;
    }
    if (dfe_taps) {
        *dfe_taps = g_cqpsk_ctx.eq.dfe_taps;
    }
    if (cma_warmup_remaining) {
        *cma_warmup_remaining = g_cqpsk_ctx.eq.cma_warmup;
    }
    return 0;
}

extern "C" void
cqpsk_runtime_set_dqpsk(int enable) {
    if (!g_cqpsk_ctx.initialized) {
        return;
    }
    g_cqpsk_ctx.eq.dqpsk_decision = enable ? 1 : 0;
}

extern "C" int
cqpsk_runtime_get_dqpsk(int* enable) {
    if (!g_cqpsk_ctx.initialized) {
        return -1;
    }
    if (enable) {
        *enable = g_cqpsk_ctx.eq.dqpsk_decision ? 1 : 0;
    }
    return 0;
}
