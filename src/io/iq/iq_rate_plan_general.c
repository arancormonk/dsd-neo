// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The general I/Q rate plan (dsd_iq_plan_general_rate(), issues #701, #743) and the plan helpers the replay planner
 * shares with it (iq_rate_plan_internal.h).
 *
 * Apart from iq_replay.c so a caller that plans a raw I/Q stream links the planner without the replay's file and
 * metadata code.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/platform/platform.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "iq_rate_plan_internal.h"

static void plan_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 3, 4);

static void
plan_error(char* err_buf, size_t err_buf_size, const char* fmt, ...) {
    if (!err_buf || err_buf_size == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    (void)DSD_VSNPRINTF(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
    err_buf[err_buf_size - 1] = '\0';
}

void
iq_rate_plan_fill(dsd_iq_replay_rate_plan* plan, uint32_t capture_hz, uint32_t dsp_in_hz, uint32_t base, uint32_t post,
                  uint32_t target_hz, int kept) {
    DSD_MEMSET(plan, 0, sizeof(*plan));
    plan->capture_rate_hz = capture_hz;
    plan->dsp_input_rate_hz = dsp_in_hz;
    plan->base_decimation = base;
    plan->post_downsample = post;
    plan->demod_rate_hz = dsp_in_hz / base / post;
    plan->target_dsp_hz = kept ? 0U : target_hz;
    plan->kept_recorded = kept ? 1 : 0;
    plan->resample = dsp_in_hz != capture_hz ? 1 : 0;
}

int
iq_rate_plan_chain(dsd_rate_plan_backend backend, uint32_t request_hz, uint32_t nominal_hz, uint32_t readback_hz,
                   uint32_t target_hz, int passes, dsd_rate_plan* out) {
    dsd_rate_plan_input in;
    DSD_MEMSET(&in, 0, sizeof(in));
    in.backend = backend;
    in.request_hz = request_hz;
    in.programmed_hz = nominal_hz;
    in.readback_hz = readback_hz;
    in.target_hz = target_hz;
    in.max_passes = passes;
    return dsd_rate_plan_compute(&in, out) == DSD_RATE_PLAN_OK && out->passes == passes;
}

static dsd_rate_plan_backend
rate_plan_general_backend(dsd_iq_replay_origin origin) {
    switch (origin) {
        case DSD_IQ_REPLAY_ORIGIN_WAV: return DSD_RATE_PLAN_BACKEND_WAV;
        case DSD_IQ_REPLAY_ORIGIN_RESAMPLED: return DSD_RATE_PLAN_BACKEND_REPLAY_RESAMPLED;
        default: break;
    }
    return DSD_RATE_PLAN_BACKEND_REPLAY_EXACT;
}

/* What a general plan's refusals call its source's rate and its chain: a replay's capture, or a stream. */
typedef struct {
    const char* rate_owner; /* "capture": "the capture's rate" */
    const char* runner;     /* "replay": "the 72000 Hz replay chain", "a replay can resample" */
} general_plan_words;

static general_plan_words
general_plan_words_of(dsd_iq_plan_source source) {
    if (source == DSD_IQ_PLAN_SOURCE_STREAM) {
        return (general_plan_words){"stream", "stream"};
    }
    return (general_plan_words){"capture", "replay"};
}

/* The general rule's plan: the source at @p fs resampled (or not) to @p r = D x 2^k and decimated by 2^k to D. It runs
 * no step, as forced, as every replay did before issue #806. */
static int
rate_plan_fill_general(dsd_iq_plan_source source, uint32_t fs, uint32_t r, uint32_t k, uint32_t d,
                       dsd_iq_replay_rate_plan* plan, char* err_buf, size_t err_buf_size) {
    iq_rate_plan_fill(plan, fs, r, 1U << k, 1U, d, 0);
    if (source == DSD_IQ_PLAN_SOURCE_WAV) {
        plan->origin = DSD_IQ_REPLAY_ORIGIN_WAV;
    } else {
        plan->origin = plan->resample ? DSD_IQ_REPLAY_ORIGIN_RESAMPLED : DSD_IQ_REPLAY_ORIGIN_EXACT;
    }
    if (!iq_rate_plan_chain(rate_plan_general_backend(plan->origin), r, r, 0U, d, (int)k, &plan->chain)) {
        plan_error(err_buf, err_buf_size, "the %u Hz %s chain cannot be planned%s%s", (unsigned)r,
                   general_plan_words_of(source).runner, plan->chain.message[0] ? ": " : "", plan->chain.message);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    plan->complex_rate_hz = plan->chain.step_out_hz;
    plan->forced = 1;
    plan->forced_provenance = DSD_IQ_REPLAY_FORCED_PLAN;
    return DSD_IQ_OK;
}

/* The general rule's R = D x 2^k and k for a source at @p fs (dsd_iq_plan_general_rate()): the source's own rate when
 * it is one, else the rate it resamples to, or a refusal. */
static int
rate_plan_general_rate_of(dsd_iq_plan_source source, uint32_t fs, uint32_t d, uint64_t* out_r, uint32_t* out_k,
                          char* err_buf, size_t err_buf_size) {
    for (uint32_t k = 0U; k <= DSD_IQ_REPLAY_MAX_PLAN_PASSES; k++) {
        if ((uint64_t)d << k == (uint64_t)fs) {
            *out_r = fs;
            *out_k = k;
            return DSD_IQ_OK;
        }
    }
    const general_plan_words words = general_plan_words_of(source);
    uint64_t r = d;
    uint32_t k = 0U;
    if (fs < d) {
        if ((uint64_t)d > (uint64_t)fs * DSD_IQ_REPLAY_MAX_UPSAMPLE_RATIO) {
            plan_error(err_buf, err_buf_size,
                       "the %s's rate (%u Hz) would need more than a %ux upsample to reach the %u Hz DSP rate",
                       words.rate_owner, (unsigned)fs, (unsigned)DSD_IQ_REPLAY_MAX_UPSAMPLE_RATIO, (unsigned)d);
            return DSD_IQ_ERR_RATE_CHAIN;
        }
    } else {
        while (k < DSD_IQ_REPLAY_MAX_PLAN_PASSES && ((uint64_t)d << (k + 1U)) <= (uint64_t)fs) {
            k++;
        }
        r = (uint64_t)d << k;
        if ((uint64_t)fs >= 2U * r) {
            plan_error(err_buf, err_buf_size,
                       "the %s's rate (%u Hz) is too fast to resample to the %u Hz DSP rate (more than a %u x 2 "
                       "decimation)",
                       words.rate_owner, (unsigned)fs, (unsigned)d, (unsigned)(1U << DSD_IQ_REPLAY_MAX_PLAN_PASSES));
            return DSD_IQ_ERR_RATE_CHAIN;
        }
    }
    *out_r = r;
    *out_k = k;
    return DSD_IQ_OK;
}

int
dsd_iq_plan_general_rate(dsd_iq_plan_source source, uint32_t source_rate_hz, uint32_t target_dsp_hz,
                         dsd_iq_replay_rate_plan* out_plan, char* err_buf, size_t err_buf_size) {
    if (!out_plan) {
        return DSD_IQ_ERR_INVALID_ARG;
    }
    const general_plan_words words = general_plan_words_of(source);
    const uint32_t fs = source_rate_hz;
    const uint32_t d = target_dsp_hz;
    if (d == 0U) {
        plan_error(err_buf, err_buf_size, "no DSP target rate to plan the %s for", words.runner);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    if (fs < DSD_IQ_REPLAY_MIN_CAPTURE_RATE_HZ) {
        plan_error(err_buf, err_buf_size, "the %s's rate (%u Hz) is below the %u Hz a %s can resample",
                   words.rate_owner, (unsigned)fs, (unsigned)DSD_IQ_REPLAY_MIN_CAPTURE_RATE_HZ, words.runner);
        return DSD_IQ_ERR_RATE_CHAIN;
    }
    uint64_t r = 0U;
    uint32_t k = 0U;
    dsd_iq_replay_rate_plan plan = {0};
    int rc = rate_plan_general_rate_of(source, fs, d, &r, &k, err_buf, err_buf_size);
    if (rc == DSD_IQ_OK) {
        rc = rate_plan_fill_general(source, fs, (uint32_t)r, k, d, &plan, err_buf, err_buf_size);
    }
    if (rc == DSD_IQ_OK) {
        *out_plan = plan;
    }
    return rc;
}
