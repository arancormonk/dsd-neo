// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stddef.h>
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
static dsd_trunk_tune_result g_tune_to_cc_request_result = DSD_TRUNK_TUNE_RESULT_OK;
static uint64_t g_last_request_id = 0U;

static dsd_trunk_tune_result
fake_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    g_tune_to_freq_calls++;
    g_last_freq = freq;
    g_last_ted_sps = ted_sps;
    g_last_request_id = request_id;
    return g_tune_to_cc_request_result;
}

static dsd_trunk_tune_result
fake_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    g_tune_to_cc_calls++;
    g_last_cc_freq = freq;
    g_last_ted_sps = ted_sps;
    g_last_request_id = request_id;
    return g_tune_to_cc_request_result;
}

static dsd_trunk_tune_result
fake_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    g_return_to_cc_calls++;
    g_last_request_id = request_id;
    return g_tune_to_cc_request_result;
}

int
main(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = fake_tune_to_freq;
    hooks.tune_to_cc_request = fake_tune_to_cc;
    hooks.return_to_cc_request = fake_return_to_cc;
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
    assert(dsd_trunk_tuning_hook_tune_to_freq(&opts, &state, 852000000, 123, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    tune_generation = dsd_trunk_tuning_generation();
    assert(g_tune_to_freq_calls == 1);
    assert(g_last_freq == 852000000);
    assert(g_last_ted_sps == 123);

    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851000000, 456, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    tune_generation = dsd_trunk_tuning_generation();
    assert(g_tune_to_cc_calls == 1);
    assert(g_last_cc_freq == 851000000);
    assert(g_last_ted_sps == 456);

    assert(dsd_trunk_tuning_hook_return_to_cc(&opts, &state, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    assert(g_return_to_cc_calls == 1);

    // Missing tune backends must fail without fabricating decoder-state changes.
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    assert(dsd_trunk_tuning_hook_tune_to_freq(&opts, &state, 853000000, 0, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(opts.trunk_is_tuned == 0);
    assert(state.p25_vc_freq[0] == 0);
    assert(state.trunk_vc_freq[0] == 0);

    assert(dsd_trunk_tuning_hook_return_to_cc(&opts, &state, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(opts.trunk_is_tuned == 0);
    assert(state.p25_vc_freq[0] == 0);
    assert(state.trunk_vc_freq[0] == 0);

    tune_generation = dsd_trunk_tuning_generation();
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851500000, 0, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(state.trunk_cc_freq == 0);
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 0, 0, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_generation() == tune_generation);

    uint64_t pending_request = 0U;
    hooks = (dsd_trunk_tuning_hooks){0};
    hooks.tune_to_cc_request = fake_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_tune_to_cc_request_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851700000, 0, &pending_request)
           == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(pending_request != 0U && g_last_request_id == pending_request);
    assert(dsd_trunk_tuning_pending_request() == pending_request);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_publish(pending_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    dsd_trunk_tuning_request_complete(pending_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    hooks = (dsd_trunk_tuning_hooks){0};
    hooks.tune_to_cc_request = fake_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_tune_to_cc_request_result = DSD_TRUNK_TUNE_RESULT_PENDING;
    g_last_request_id = 0U;
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851700000, 0, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(g_last_request_id != 0U);
    assert(dsd_trunk_tuning_pending_request() == g_last_request_id);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_publish(g_last_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_request_status(g_last_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == g_last_request_id);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    uint64_t wrapper_recovery_request = dsd_trunk_tuning_request_begin();
    assert(wrapper_recovery_request > g_last_request_id);
    dsd_trunk_tuning_request_complete(wrapper_recovery_request, DSD_TRUNK_TUNE_RESULT_OK);
    tune_generation++;
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t early_completion = dsd_trunk_tuning_request_begin();
    assert(early_completion != 0U);
    dsd_trunk_tuning_request_publish(early_completion, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_request_status(early_completion, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_mark_ready(early_completion);
    tune_generation++;
    assert(dsd_trunk_tuning_request_status(early_completion, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t ready_before_completion = dsd_trunk_tuning_request_begin();
    assert(ready_before_completion != 0U);
    dsd_trunk_tuning_request_mark_ready(ready_before_completion);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_publish(ready_before_completion, DSD_TRUNK_TUNE_RESULT_OK);
    tune_generation++;
    assert(dsd_trunk_tuning_request_status(ready_before_completion, NULL) == DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t backend_commit_owner_rollback = dsd_trunk_tuning_request_begin();
    assert(backend_commit_owner_rollback != 0U);
    dsd_trunk_tuning_request_publish(backend_commit_owner_rollback, DSD_TRUNK_TUNE_RESULT_OK);
    dsd_trunk_tuning_request_complete(backend_commit_owner_rollback, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_request_status(backend_commit_owner_rollback, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_pending_request() == backend_commit_owner_rollback);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    uint64_t mismatch_recovery = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_complete(mismatch_recovery, DSD_TRUNK_TUNE_RESULT_OK);
    tune_generation++;
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t backend_rollback_owner_commit = dsd_trunk_tuning_request_begin();
    assert(backend_rollback_owner_commit != 0U);
    dsd_trunk_tuning_request_publish(backend_rollback_owner_commit, DSD_TRUNK_TUNE_RESULT_FAILED);
    dsd_trunk_tuning_request_complete(backend_rollback_owner_commit, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_request_status(backend_rollback_owner_commit, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_pending_request() == backend_rollback_owner_commit);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    mismatch_recovery = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_complete(mismatch_recovery, DSD_TRUNK_TUNE_RESULT_OK);
    tune_generation++;
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t stale_request = dsd_trunk_tuning_request_begin();
    uint64_t latest_request = dsd_trunk_tuning_request_begin();
    assert(stale_request != 0U && latest_request != 0U && stale_request != latest_request);
    dsd_trunk_tuning_request_complete(stale_request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_pending_request() == latest_request);
    assert(dsd_trunk_tuning_generation() == tune_generation + 1U);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    tune_generation = dsd_trunk_tuning_generation();
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_publish(latest_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == latest_request);
    assert(dsd_trunk_tuning_request_status(latest_request, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_complete(latest_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t mode_exit_failure = dsd_trunk_tuning_request_begin();
    assert(mode_exit_failure != 0U);
    dsd_trunk_tuning_request_publish(mode_exit_failure, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == mode_exit_failure);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    assert(!dsd_trunk_tuning_frame_is_dispatchable(tune_generation, 1));
    assert(dsd_trunk_tuning_pending_request() == mode_exit_failure);
    assert(dsd_trunk_tuning_frame_is_dispatchable(tune_generation, 0));
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t mode_exit_pending = dsd_trunk_tuning_request_begin();
    assert(mode_exit_pending != 0U);
    dsd_trunk_tuning_request_mark_ready(mode_exit_pending);
    assert(!dsd_trunk_tuning_frame_is_dispatchable(tune_generation, 0));
    assert(dsd_trunk_tuning_pending_request() == mode_exit_pending);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_publish(mode_exit_pending, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_frame_is_dispatchable(tune_generation, 0));
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t older_request = dsd_trunk_tuning_request_begin();
    uint64_t newer_request = dsd_trunk_tuning_request_begin();
    assert(older_request != 0U && newer_request > older_request);
    dsd_trunk_tuning_request_publish(newer_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    dsd_trunk_tuning_request_complete(older_request, DSD_TRUNK_TUNE_RESULT_OK);
    tune_generation++;
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_pending_request() == newer_request);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_complete(newer_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    uint64_t failed_request = dsd_trunk_tuning_request_begin();
    assert(failed_request != 0U);
    dsd_trunk_tuning_request_publish(failed_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    uint64_t recovery_request = dsd_trunk_tuning_request_begin();
    assert(recovery_request > failed_request);
    dsd_trunk_tuning_request_complete(recovery_request, DSD_TRUNK_TUNE_RESULT_OK);
    tune_generation++;
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    for (int i = 0; i < 64; i++) {
        uint64_t failed_id = dsd_trunk_tuning_request_begin();
        assert(failed_id != 0U);
        dsd_trunk_tuning_request_publish(failed_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    }
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    recovery_request = dsd_trunk_tuning_request_begin();
    assert(recovery_request != 0U);
    dsd_trunk_tuning_request_complete(recovery_request, DSD_TRUNK_TUNE_RESULT_OK);
    tune_generation++;
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    hooks = (dsd_trunk_tuning_hooks){0};
    hooks.tune_to_cc_request = fake_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    g_tune_to_cc_request_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
    tune_generation = dsd_trunk_tuning_generation();
    assert(dsd_trunk_tuning_hook_tune_to_cc(&opts, &state, 851700000, 0, NULL) == DSD_TRUNK_TUNE_RESULT_DEFERRED);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_pending_request() == 0U);

    uint64_t abandoned_request = dsd_trunk_tuning_request_begin();
    assert(abandoned_request != 0U);
    dsd_trunk_tuning_request_mark_ready(abandoned_request);
    assert(!dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_requests_reset();
    assert(dsd_trunk_tuning_pending_request() == 0U);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));
    dsd_trunk_tuning_request_publish(abandoned_request, DSD_TRUNK_TUNE_RESULT_OK);
    assert(dsd_trunk_tuning_generation() == tune_generation);
    assert(dsd_trunk_tuning_frame_is_current(tune_generation));

    assert(dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_OK));
    assert(dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_PENDING));
    assert(!dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_DEFERRED));
    assert(!dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_TIMEOUT));
    assert(!dsd_trunk_tune_result_is_ok(DSD_TRUNK_TUNE_RESULT_FAILED));
    assert(dsd_trunk_tune_result_is_complete(DSD_TRUNK_TUNE_RESULT_OK));
    assert(!dsd_trunk_tune_result_is_complete(DSD_TRUNK_TUNE_RESULT_PENDING));

    return 0;
}
