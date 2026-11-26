// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The noise the dynamic squelches see through a replay or network I/Q reader's resampler (issue #899); see
   squelch_reader_model.h. Not built yet: every build refuses, so the plans behind a reader are the ones before. */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/squelch_reader_model.h>
#include <dsd-neo/runtime/mem.h>
#include <stddef.h>

void
dsd_squelch_reader_model_init(dsd_squelch_reader_model* m) {
    if (!m) {
        return;
    }
    DSD_MEMSET(m, 0, sizeof(*m));
    dsd_iq_resampler_init(&m->reader);
}

void
dsd_squelch_reader_model_release(dsd_squelch_reader_model* m) {
    if (!m) {
        return;
    }
    dsd_iq_resampler_free(&m->reader);
    dsd_squelch_reader_model_init(m);
}

int
dsd_squelch_reader_model_matches(const dsd_squelch_reader_model* m, const dsd_iq_reader_plan* plan, int passes) {
    (void)m;
    (void)plan;
    (void)passes;
    return 0;
}

int
dsd_squelch_reader_model_build(dsd_squelch_reader_model* m, const dsd_iq_reader_plan* plan,
                               const dsd_noise_squelch_stage* stages, int stage_count) {
    (void)plan;
    (void)stages;
    (void)stage_count;
    dsd_squelch_reader_model_release(m);
    return -1;
}

int
dsd_squelch_reader_model_floor_plan(dsd_squelch_reader_model* m, dsd_squelch_floor_plan* out, const float* taps,
                                    int taps_len, int rate_hz) {
    (void)m;
    (void)taps;
    (void)taps_len;
    (void)rate_hz;
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    return -1;
}

int
dsd_squelch_reader_model_floor_plan_exact(dsd_squelch_reader_model* m, dsd_squelch_floor_plan* out, const float* taps,
                                          int taps_len, int rate_hz) {
    return dsd_squelch_reader_model_floor_plan(m, out, taps, taps_len, rate_hz);
}

int
dsd_squelch_reader_model_noise_plan(const dsd_squelch_reader_model* m, dsd_noise_squelch_plan* out, const float* taps,
                                    int taps_len, const dsd_noise_squelch_stage* stages, int stage_count, int rate_hz) {
    (void)m;
    (void)taps;
    (void)taps_len;
    (void)stages;
    (void)stage_count;
    (void)rate_hz;
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    return -1;
}
