// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "test_support.h"

static int g_typed;
static int g_pending;
static int g_stage_before_loop;
static int g_sync_calls;
static int g_tune_calls;
static uint64_t g_tune_request;
static int g_wait_polls;
static int g_pending_seen;
static time_t g_test_wall;

// NOLINTBEGIN(misc-use-internal-linkage)
void printFrameInfo(dsd_opts* opts, dsd_state* state);
void LFSRN(const char* input, char* output, dsd_state* state);

void
printFrameInfo(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
LFSRN(const char* input, char* output, dsd_state* state) {
    (void)input;
    (void)output;
    (void)state;
}

void
processFrame(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    assert(0 && "the timing regression never supplies a synced frame");
}

// NOLINTEND(misc-use-internal-linkage)

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
dsd_trunk_tune_result __wrap_dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long freq, int sps,
                                                          uint64_t* request);
int __wrap_getFrameSync(dsd_opts* opts, dsd_state* state);
bool __wrap_SetFreq(dsd_socket_t sockfd, long freq);
time_t __real_time(time_t* result);
time_t __wrap_time(time_t* result);

time_t
__wrap_time(time_t* result) {
    if (!g_test_wall) {
        return __real_time(result);
    }
    if (result) {
        *result = g_test_wall;
    }
    return g_test_wall;
}

bool
__wrap_SetFreq(dsd_socket_t sockfd, long freq) {
    (void)sockfd;
    assert(freq > 0);
    return true;
}

dsd_trunk_tune_result
__wrap_dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long freq, int sps, uint64_t* request) {
    (void)opts;
    (void)state;
    (void)sps;
    assert(freq > 0);
    ++g_tune_calls;
    *request = dsd_trunk_tuning_request_begin();
    g_tune_request = *request;
    if (g_pending) {
        dsd_trunk_tuning_request_mark_ready(*request);
    } else {
        dsd_trunk_tuning_request_complete(*request, DSD_TRUNK_TUNE_RESULT_OK);
    }
    return g_pending ? DSD_TRUNK_TUNE_RESULT_PENDING : DSD_TRUNK_TUNE_RESULT_OK;
}

int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    ++g_sync_calls;
    assert(state->lcn_freq_roll == 1);
    assert(state->last_cc_sync_time >= time(NULL) - 1);
    assert(state->scan_timing.reason == DSD_SCAN_STAY_HANGTIME);
    assert(state->scan_timing.span_ms == 3000U);
    /* The outgoing anchor was a minute old. The first snapshot on this frequency
     * must already contain the new visit's positive countdown. */
    assert(state->scan_timing.deadline_m > dsd_time_now_monotonic_s());
    assert(fabs(state->scan_timing.started_m - state->last_cc_sync_time_m) < 1.1);
    if (g_typed) {
        assert(opts->frame_dmr == 1);
        assert(!dsd_engine_channel_scan_waiting(state));
        assert(g_tune_calls == 1);
    }
    dsd_exitflag_store(1);
    return DSD_SYNC_NONE;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
complete_pending_tune(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    if (!g_pending || !g_tune_request
        || dsd_trunk_tuning_request_status(g_tune_request, NULL) != DSD_TRUNK_TUNE_RESULT_PENDING) {
        return;
    }
    /* Wait until the pending phase was published. Bound the wait so a missing
     * publication fails instead of hanging the test. */
    ++g_wait_polls;
    assert(g_wait_polls < 10);
    if (state->scan_timing.reason == DSD_SCAN_STAY_RETUNE_PENDING) {
        assert(state->scan_timing.deadline_m < 0.0);
        g_pending_seen = 1;
        dsd_trunk_tuning_request_complete(g_tune_request, DSD_TRUNK_TUNE_RESULT_OK);
    }
}

static int
start_scan(dsd_opts* opts, dsd_state* state, void* context) {
    (void)context;
    opts->scanner_mode = 1;
    opts->use_rigctl = 1;
    opts->setmod_bw = 0;
    opts->trunk_hangtime = 2.0f;
    state->last_cc_sync_time = time(NULL) - 60;
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s() - 60.0;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    *dsd_state_trunk_lcn_slot(state, 0) = 150000000;
    *dsd_state_trunk_lcn_slot(state, 1) = 151000000;
    if (g_typed) {
        assert(dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_DMR) == 0);
    }
    if (g_stage_before_loop) {
        assert(dsd_engine_channel_scan_step(opts, state) == 0);
        assert(dsd_engine_channel_scan_waiting(state));
    }
    return 0;
}

static void
test_scan_retune_publication(int typed, int pending, int staged) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof(opts->audio_out_dev), "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;
    g_typed = typed;
    g_pending = pending;
    g_stage_before_loop = staged;
    g_sync_calls = g_tune_calls = 0;
    g_tune_request = 0U;
    g_wait_polls = g_pending_seen = 0;
    dsd_runtime_set_control_pump(complete_pending_tune);
    const dsd_engine_lifecycle_hooks hooks = {.start = start_scan};
    assert(dsd_engine_run_with_lifecycle(opts, state, &hooks) == 0);
    assert(g_sync_calls == 1);
    assert(g_pending_seen == pending);
    dsd_runtime_set_control_pump(NULL);
    freeState(state);
    free(state);
    free(opts);
}

static void
test_legacy_deadline_matches_rotation(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    initOpts(opts);
    initState(state);
    opts->scanner_mode = 1;
    state->lcn_freq_count = 2;
    /* Zero-frequency rows advance the visit without needing a physical tuner. */
    *dsd_state_trunk_lcn_slot(state, 0) = 0;
    const float budgets[] = {0.0f, 0.25f, 2.0f, 2.5f};
    for (size_t i = 0; i < sizeof(budgets) / sizeof(budgets[0]); ++i) {
        opts->trunk_hangtime = budgets[i];
        state->lcn_freq_roll = 0;
        state->last_cc_sync_time = 100;
        dsd_engine_scan_y_timing_tick(opts, state, 1000.0, 100.0);
        const double deadline = state->scan_timing.deadline_m;
        assert(deadline > 1000.0);
        g_test_wall = 100 + (time_t)(deadline - 1000.0) - 1;
        noCarrier(opts, state);
        assert(state->lcn_freq_roll == 0);
        ++g_test_wall;
        noCarrier(opts, state);
        assert(state->lcn_freq_roll == 1);
    }
    g_test_wall = 0;
    freeState(state);
    free(state);
    free(opts);
}

static void
test_protocol_hangtime_publication(const char* protocol, float configured_hangtime) {
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof(path), "scan-timing");
    assert(fd >= 0);
    FILE* file = fdopen(fd, "w");
    assert(file);
    DSD_FPRINTF(file,
                "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
                "test,%s,851000000,,3000,,\n",
                protocol);
    assert(fclose(file) == 0);
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);
    initOpts(opts);
    initState(state);
    opts->trunk_scan_enabled = 1;
    opts->use_rigctl = 1;
    opts->trunk_hangtime = configured_hangtime;
    DSD_SNPRINTF(opts->trunk_scan_targets_csv, sizeof(opts->trunk_scan_targets_csv), "%s", path);
    char err[256] = {0};
    g_pending = 0;
    assert(dsd_engine_trunk_scan_init(opts, state, err, sizeof(err)) == 0);
    const double now = dsd_time_now_monotonic_s();
    if (strcmp(protocol, "p25-trunk") == 0) {
        p25_sm_ctx_t* ctx = dsd_engine_trunk_scan_active_p25_ctx();
        assert(ctx && ctx->config.hangtime_s == 7.0);
        ctx->state = P25_SM_TUNED;
        ctx->cc_tune_pending = 0;
        ctx->vc_freq_hz = 852000000;
        ctx->t_tune_m = now;
        ctx->config.grant_timeout_s = 30.0;
    } else {
        dmr_sm_ctx_t* ctx = dsd_engine_trunk_scan_active_dmr_ctx();
        assert(ctx && ctx->hangtime_s == 7.0);
        ctx->state = DMR_SM_TUNED;
        ctx->cc_tune_request_id = 0U;
        ctx->t_tune_m = now;
        ctx->t_voice_m = now;
    }
    opts->trunk_is_tuned = 1;
    dsd_engine_trunk_scan_tick(opts, state);
    assert(state->scan_timing.reason == DSD_SCAN_STAY_CALL_FOLLOW);
    assert(state->scan_timing.hang_ms == 7000U);
    dsd_app_scan_timing view;
    assert(dsd_app_scan_timing_view(opts, state, now, &view) == 1);
    assert(view.show_hang && view.hang_ms == 7000U);
    if (strcmp(protocol, "p25-trunk") == 0) {
        /* The optional error hold extends the same protocol budget. */
        state->p25_p1_voice_err_hist_count = 1;
        state->p25_p1_voice_err_hist_sum = 20;
        dsd_engine_trunk_scan_tick(opts, state);
        assert(state->scan_timing.hang_ms == 8500U);
    }
    state->p25_p1_voice_err_hist_count = 0;
    state->p25_p1_voice_err_hist_sum = 0;
    const double large_hangtimes[] = {172800.0, 1.0e9};
    for (size_t i = 0; i < sizeof(large_hangtimes) / sizeof(large_hangtimes[0]); ++i) {
        if (strcmp(protocol, "p25-trunk") == 0) {
            p25_sm_ctx_t* ctx = dsd_engine_trunk_scan_active_p25_ctx();
            ctx->config.hangtime_s = large_hangtimes[i];
        } else {
            dmr_sm_ctx_t* ctx = dsd_engine_trunk_scan_active_dmr_ctx();
            ctx->hangtime_s = large_hangtimes[i];
        }
        dsd_engine_trunk_scan_tick(opts, state);
        assert(state->scan_timing.hang_ms == (i == 0 ? 172800000U : UINT32_MAX));
    }
    dsd_engine_trunk_scan_shutdown(opts, state);
    assert(state->scan_timing.hang_ms == 0U);
    freeState(state);
    free(state);
    free(opts);
    assert(remove(path) == 0);
}

int
main(void) {
    test_legacy_deadline_matches_rotation();
    test_scan_retune_publication(0, 0, 0);
    test_scan_retune_publication(1, 0, 0);
    test_scan_retune_publication(1, 1, 0);
    test_scan_retune_publication(1, 1, 1);
    assert(dsd_test_setenv("DSD_NEO_DMR_HANGTIME", "7", 1) == 0);
    assert(dsd_test_setenv("DSD_NEO_P25_HANGTIME", "7", 1) == 0);
    assert(dsd_test_setenv("DSD_NEO_P25P1_ERR_HOLD_PCT", "10", 1) == 0);
    assert(dsd_test_setenv("DSD_NEO_P25P1_ERR_HOLD_S", "1.5", 1) == 0);
    dsd_neo_config_init();
    test_protocol_hangtime_publication("p25-trunk", 2.0f);
    test_protocol_hangtime_publication("dmr-trunk", 2.0f);
    test_protocol_hangtime_publication("p25-trunk", 0.0f);
    test_protocol_hangtime_publication("dmr-trunk", 0.0f);
    assert(dsd_test_unsetenv("DSD_NEO_DMR_HANGTIME") == 0);
    assert(dsd_test_unsetenv("DSD_NEO_P25_HANGTIME") == 0);
    assert(dsd_test_unsetenv("DSD_NEO_P25P1_ERR_HOLD_PCT") == 0);
    assert(dsd_test_unsetenv("DSD_NEO_P25P1_ERR_HOLD_S") == 0);
    dsd_neo_config_init();
    return 0;
}
