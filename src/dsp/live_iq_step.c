// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The live I/Q rate step's designs and their transaction (issue #806). See <dsd-neo/dsp/live_iq_step.h>.
 */

#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/live_iq_step.h>

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "dsd-neo/core/safe_api.h"
#ifdef DSD_NEO_TEST_HOOKS
#include <dsd-neo/platform/atomic_compat.h>
#include "live_iq_step_test_support.h"
#endif

#ifdef DSD_NEO_TEST_HOOKS
/* While set (dsd_live_iq_step_test_fail_design()), a design fails as when its memory is not there. */
static int g_live_iq_step_test_fail_design = 0;

void
dsd_live_iq_step_test_fail_design(int fail) {
    g_live_iq_step_test_fail_design = fail ? 1 : 0;
}

/* Clears of a running step (dsd_live_iq_step_test_clears()): written on the demod thread, read by a test thread. */
static dsd_atomic_u64 g_live_iq_step_test_clears;

uint64_t
dsd_live_iq_step_test_clears(void) {
    return dsd_atomic_u64_load_acquire(&g_live_iq_step_test_clears);
}
#endif

void
dsd_live_iq_step_init(dsd_live_iq_step* s) {
    if (!s) {
        return;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    dsd_iq_resampler_init(&s->active);
    dsd_iq_resampler_init(&s->retained);
    dsd_iq_resampler_init(&s->candidate);
}

int
dsd_live_iq_step_plan_equal(const dsd_live_iq_step_plan* a, const dsd_live_iq_step_plan* b) {
    if (!a || !b) {
        return 0;
    }
    return a->enabled == b->enabled && a->passes == b->passes && a->hb_out_num == b->hb_out_num
           && a->hb_out_shift == b->hb_out_shift && a->L == b->L && a->M == b->M && a->target_hz == b->target_hz
           && a->protect_hz == b->protect_hz && a->stop_hz == b->stop_hz;
}

int
dsd_live_iq_step_plan_out_hz(const dsd_live_iq_step_plan* plan) {
    if (!plan) {
        return 0;
    }
    if (plan->enabled) {
        return plan->target_hz;
    }
    return (plan->hb_out_shift == 0U && plan->hb_out_num > 0 && plan->hb_out_num <= (int64_t)INT32_MAX)
               ? (int)plan->hb_out_num
               : 0;
}

/* Whether a stepping plan's L/M takes hb_out to the target: within the planner's bound for an approximated ratio, plus
   a hair for the doubles the check is made in (every product is exact; only the two divisions round). */
static int
live_iq_step_ratio_agrees(const dsd_live_iq_step_plan* p) {
    const double out = (double)p->hb_out_num * (double)p->L;
    const double want = (double)p->M * ldexp((double)p->target_hz, (int)p->hb_out_shift);
    const double ppm = (out / want - 1.0) * 1e6;
    return fabs(ppm) <= DSD_IQ_RESAMPLER_MAX_APPROX_PPM + 1e-6;
}

static int
live_iq_step_plan_ok(const dsd_live_iq_step_plan* p) {
    if (p->passes < 0 || p->passes > DSD_IQ_RESAMPLER_MAX_RATE_SHIFT || p->hb_out_num < 1
        || p->hb_out_num > (int64_t)INT32_MAX || p->hb_out_shift > DSD_IQ_RESAMPLER_MAX_RATE_SHIFT) {
        return 0;
    }
    if (!p->enabled) {
        return 1;
    }
    if (p->L < 1 || p->M < 1 || p->L > DSD_IQ_RESAMPLER_MAX_RATIO_TERM || p->M > DSD_IQ_RESAMPLER_MAX_RATIO_TERM
        || p->target_hz < 1) {
        return 0;
    }
    return live_iq_step_ratio_agrees(p);
}

/* Whether a maximum block's outputs fit: the step never upsamples, and ceil(max_block_in x L / M) fits out_cap. */
static int
live_iq_step_fits(const dsd_live_iq_step_plan* p, size_t max_block_in, size_t out_cap) {
    if (p->L > p->M) {
        return 0;
    }
    const uint64_t most = ((uint64_t)max_block_in * (uint64_t)p->L + (uint64_t)p->M - 1U) / (uint64_t)p->M;
    return most <= (uint64_t)out_cap;
}

static void
live_iq_step_release_candidate(dsd_live_iq_step* s) {
    dsd_iq_resampler_free(&s->candidate);
    DSD_MEMSET(&s->candidate_plan, 0, sizeof(s->candidate_plan));
    s->candidate_set = 0;
    s->candidate_reuses_active = 0;
}

static int
live_iq_step_design(dsd_live_iq_step* s, const dsd_live_iq_step_plan* p) {
#ifdef DSD_NEO_TEST_HOOKS
    if (g_live_iq_step_test_fail_design) {
        s->design_status = DSD_IQ_RESAMPLER_ERR_ALLOC;
        return DSD_LIVE_IQ_STEP_ERR_DESIGN;
    }
#endif
    const int rc = dsd_iq_resampler_configure_ratio(&s->candidate, p->L, p->M, p->hb_out_num, p->hb_out_shift,
                                                    p->protect_hz, p->stop_hz, DSD_LIVE_IQ_STEP_PASS_PAIRS);
    s->design_status = rc;
    return rc == DSD_IQ_RESAMPLER_OK ? DSD_LIVE_IQ_STEP_OK : DSD_LIVE_IQ_STEP_ERR_DESIGN;
}

int
dsd_live_iq_step_prepare(dsd_live_iq_step* s, const dsd_live_iq_step_plan* plan, size_t max_block_in, size_t out_cap) {
    if (!s) {
        return DSD_LIVE_IQ_STEP_ERR_ARGS;
    }
    live_iq_step_release_candidate(s);
    s->design_status = DSD_IQ_RESAMPLER_OK;
    if (!plan || max_block_in == 0U || !live_iq_step_plan_ok(plan)) {
        return DSD_LIVE_IQ_STEP_ERR_ARGS;
    }
    if (plan->enabled && !live_iq_step_fits(plan, max_block_in, out_cap)) {
        return DSD_LIVE_IQ_STEP_ERR_CAPACITY;
    }
    const int reuses = dsd_live_iq_step_plan_equal(plan, &s->plan);
    if (plan->enabled && !reuses) {
        const int rc = live_iq_step_design(s, plan);
        if (rc != DSD_LIVE_IQ_STEP_OK) {
            live_iq_step_release_candidate(s);
            return rc;
        }
    }
    s->candidate_plan = *plan;
    s->candidate_set = 1;
    s->candidate_reuses_active = reuses;
    return DSD_LIVE_IQ_STEP_OK;
}

int
dsd_live_iq_step_commit(dsd_live_iq_step* s) {
    if (!s || !s->candidate_set) {
        return DSD_LIVE_IQ_STEP_ERR_ARGS;
    }
    if (s->candidate_reuses_active) {
        live_iq_step_release_candidate(s);
        return DSD_LIVE_IQ_STEP_OK;
    }
    if (!s->retained_set) {
        /* The retained slot is empty outside a commit: finish and rollback release it. */
        dsd_iq_resampler_swap(&s->active, &s->retained);
        s->retained_plan = s->plan;
        s->retained_set = 1;
    } else {
        dsd_iq_resampler_free(&s->active);
    }
    dsd_iq_resampler_swap(&s->active, &s->candidate);
    s->plan = s->candidate_plan;
    s->enabled = (s->plan.enabled && s->active.configured) ? 1 : 0;
    live_iq_step_release_candidate(s);
    return DSD_LIVE_IQ_STEP_OK;
}

void
dsd_live_iq_step_rollback(dsd_live_iq_step* s) {
    if (!s) {
        return;
    }
    live_iq_step_release_candidate(s);
    if (!s->retained_set) {
        return;
    }
    dsd_iq_resampler_swap(&s->active, &s->retained);
    dsd_iq_resampler_free(&s->retained);
    s->plan = s->retained_plan;
    DSD_MEMSET(&s->retained_plan, 0, sizeof(s->retained_plan));
    s->retained_set = 0;
    s->enabled = (s->plan.enabled && s->active.configured) ? 1 : 0;
    /* It last ran on the stream before the commit: what it holds is not this stream's past. */
    dsd_iq_resampler_clear(&s->active);
}

void
dsd_live_iq_step_finish(dsd_live_iq_step* s) {
    if (!s) {
        return;
    }
    live_iq_step_release_candidate(s);
    dsd_iq_resampler_free(&s->retained);
    DSD_MEMSET(&s->retained_plan, 0, sizeof(s->retained_plan));
    s->retained_set = 0;
}

void
dsd_live_iq_step_disable(dsd_live_iq_step* s) {
    if (!s) {
        return;
    }
    dsd_iq_resampler_free(&s->active);
    dsd_iq_resampler_free(&s->retained);
    dsd_iq_resampler_free(&s->candidate);
    dsd_live_iq_step_init(s);
}

void
dsd_live_iq_step_clear(dsd_live_iq_step* s) {
    if (!s) {
        return;
    }
#ifdef DSD_NEO_TEST_HOOKS
    if (s->enabled && s->active.configured) {
        (void)dsd_atomic_u64_fetch_add_release(&g_live_iq_step_test_clears, 1U);
    }
#endif
    dsd_iq_resampler_clear(&s->active);
}
