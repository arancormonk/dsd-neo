// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <time.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static dsd_trunk_tuning_hooks g_trunk_tuning_hooks = {0};
static dsd_atomic_u64 g_trunk_tuning_generation = {1U};

uint64_t
dsd_trunk_tuning_generation(void) {
    return dsd_atomic_u64_load_acquire(&g_trunk_tuning_generation);
}

void
dsd_trunk_tuning_generation_advance(void) {
    (void)dsd_atomic_u64_fetch_add_release(&g_trunk_tuning_generation, 1U);
}

static dsd_trunk_tune_result
dsd_trunk_tuning_note_result(dsd_trunk_tune_result result) {
    if (dsd_trunk_tune_result_is_ok(result)) {
        dsd_trunk_tuning_generation_advance();
    }
    return result;
}

static dsd_trunk_tune_result
dsd_trunk_tuning_fallback_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)ted_sps;
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }

    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;

    double nowm = dsd_time_now_monotonic_s();
    state->last_vc_sync_time = time(NULL);
    state->p25_last_vc_tune_time = state->last_vc_sync_time;
    state->last_vc_sync_time_m = nowm;
    state->p25_last_vc_tune_time_m = nowm;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
dsd_trunk_tuning_fallback_return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
        state->last_vc_sync_time = 0;
        state->last_vc_sync_time_m = 0.0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
dsd_trunk_tuning_fallback_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)ted_sps;
    if (!state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    state->trunk_cc_freq = freq;
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    return DSD_TRUNK_TUNE_RESULT_OK;
}

void
dsd_trunk_tuning_hooks_set(dsd_trunk_tuning_hooks hooks) {
    g_trunk_tuning_hooks = hooks;
}

dsd_trunk_tune_result
dsd_trunk_tuning_hook_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    if (g_trunk_tuning_hooks.tune_to_freq_result) {
        return dsd_trunk_tuning_note_result(g_trunk_tuning_hooks.tune_to_freq_result(opts, state, freq, ted_sps));
    }
    if (g_trunk_tuning_hooks.tune_to_freq) {
        g_trunk_tuning_hooks.tune_to_freq(opts, state, freq, ted_sps);
        return dsd_trunk_tuning_note_result(DSD_TRUNK_TUNE_RESULT_OK);
    }
    return dsd_trunk_tuning_note_result(dsd_trunk_tuning_fallback_tune_to_freq(opts, state, freq, ted_sps));
}

dsd_trunk_tune_result
dsd_trunk_tuning_hook_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    if (g_trunk_tuning_hooks.tune_to_cc_result) {
        return dsd_trunk_tuning_note_result(g_trunk_tuning_hooks.tune_to_cc_result(opts, state, freq, ted_sps));
    }
    if (g_trunk_tuning_hooks.tune_to_cc) {
        g_trunk_tuning_hooks.tune_to_cc(opts, state, freq, ted_sps);
        return dsd_trunk_tuning_note_result(DSD_TRUNK_TUNE_RESULT_OK);
    }
    return dsd_trunk_tuning_note_result(dsd_trunk_tuning_fallback_tune_to_cc(opts, state, freq, ted_sps));
}

dsd_trunk_tune_result
dsd_trunk_tuning_hook_return_to_cc(dsd_opts* opts, dsd_state* state) {
    if (g_trunk_tuning_hooks.return_to_cc_result) {
        return dsd_trunk_tuning_note_result(g_trunk_tuning_hooks.return_to_cc_result(opts, state));
    }
    if (g_trunk_tuning_hooks.return_to_cc) {
        g_trunk_tuning_hooks.return_to_cc(opts, state);
        return dsd_trunk_tuning_note_result(DSD_TRUNK_TUNE_RESULT_OK);
    }
    return dsd_trunk_tuning_note_result(dsd_trunk_tuning_fallback_return_to_cc(opts, state));
}
