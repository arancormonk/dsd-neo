// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int g_tune_to_freq_calls = 0;
static int g_tune_to_cc_calls = 0;
static int g_return_to_cc_calls = 0;
static long int g_last_freq = 0;
static long int g_last_cc_freq = 0;
static int g_last_ted_sps = -1;
static dsd_trunk_tune_result g_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_OK;

static void
fake_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    g_tune_to_freq_calls++;
    g_last_freq = freq;
    g_last_ted_sps = ted_sps;
}

static void
fake_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    (void)opts;
    (void)state;
    g_tune_to_cc_calls++;
    g_last_cc_freq = freq;
    g_last_ted_sps = ted_sps;
}

static void
fake_return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_return_to_cc_calls++;
}

static dsd_trunk_tune_result
fake_tune_to_cc_result(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps) {
    fake_tune_to_cc(opts, state, freq, ted_sps);
    return g_tune_to_cc_result;
}

int
main(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq = fake_tune_to_freq;
    hooks.tune_to_cc = fake_tune_to_cc;
    hooks.return_to_cc = fake_return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    g_tune_to_freq_calls = 0;
    g_tune_to_cc_calls = 0;
    g_return_to_cc_calls = 0;
    g_last_freq = 0;
    g_last_cc_freq = 0;
    g_last_ted_sps = -1;

    uint64_t tune_generation = dsd_trunk_tuning_generation();
    assert(dsd_trunk_tuning_hook_tune_to_freq(&opts, &state, 852000000, 123) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    tune_generation = dsd_trunk_tuning_generation();
    assert(g_tune_to_freq_calls == 1);
    assert(g_last_freq == 852000000);
    assert(g_last_ted_sps == 123);

    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851000000, 456) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    tune_generation = dsd_trunk_tuning_generation();
    assert(g_tune_to_cc_calls == 1);
    assert(g_last_cc_freq == 851000000);
    assert(g_last_ted_sps == 456);

    assert(dsd_trunk_tuning_hook_return_to_cc(&opts, &state) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    assert(g_return_to_cc_calls == 1);

    // Verify fallback behavior when hooks are not installed
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(dsd_trunk_tuning_hook_tune_to_freq(&opts, &state, 853000000, 0) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts.p25_is_tuned == 1);
    assert(opts.trunk_is_tuned == 1);
    assert(state.p25_vc_freq[0] == 853000000);
    assert(state.trunk_vc_freq[0] == 853000000);

    assert(dsd_trunk_tuning_hook_return_to_cc(&opts, &state) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(opts.p25_is_tuned == 0);
    assert(opts.trunk_is_tuned == 0);
    assert(state.p25_vc_freq[0] == 0);
    assert(state.trunk_vc_freq[0] == 0);

    tune_generation = dsd_trunk_tuning_generation();
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851500000, 0) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    assert(state.trunk_cc_freq == 851500000);
    tune_generation = dsd_trunk_tuning_generation();
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 0, 0) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_generation() == tune_generation);

    hooks = (dsd_trunk_tuning_hooks){0};
    hooks.tune_to_cc_result = fake_tune_to_cc_result;
    dsd_trunk_tuning_hooks_set(hooks);
    g_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    tune_generation = dsd_trunk_tuning_generation();
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851600000, 0) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    g_tune_to_cc_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    tune_generation = dsd_trunk_tuning_generation();
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851700000, 0) == DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(dsd_trunk_tuning_generation() == tune_generation);

    assert(dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_OK));
    assert(dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_PENDING));
    assert(!dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_DEFERRED));
    assert(!dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_TIMEOUT));
    assert(!dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_FAILED));

    return 0;
}
