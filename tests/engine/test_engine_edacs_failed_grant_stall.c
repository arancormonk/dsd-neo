// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Discussion #717, the engine's side. An EDACS grant's tune was accepted while still in flight (PENDING), so
 * the session marked itself tuned to the voice channel (opts->trunk_is_tuned = 1), and the tune then failed: the
 * radio never left the control channel, which goes on syncing. Nothing may keep the session there forever. The
 * hangtime return runs only from noCarrier(), which the synced-frames loop never reaches while every pass syncs, and
 * with trunking on the engine leaves the failed request's gate for an owner that never rolls it back, so every
 * control-channel frame is withheld. This drives the real engine loop with a getFrameSync() that keeps returning
 * EDACS syncs on a decode clock stepped 100 ms a frame, well past the hangtime, and checks that the session returns to
 * decoding the control channel.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static const uint64_t k_t0_ns = 1000000000000000000ULL;

enum { STALL_FRAMES = 100, STALL_STEP_MS = 100 };

static int g_get_frame_sync_calls = 0;
static int g_process_frame_calls = 0;
static int g_tuned_at_last_sync = -1;
static uint64_t g_failed_request = 0U;

void
printFrameInfo(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
}

void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

void
processFrame(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    g_process_frame_calls++;
}

/* The state a -fh -T session is in once edacs_try_tune_voice_call() accepted a PENDING tune to LCN 6 and that tune then
   failed (edacs_analog_call() left the call on EDACS_TUNE_LEAVE). */
static void
enter_failed_grant_state(dsd_opts* opts, dsd_state* state) {
    opts->trunk_enable = 1;
    opts->trunk_hangtime = 1.0f;
    opts->frame_provoice = 1;
    state->ea_mode = 0;
    state->trunk_cc_freq = 851006250L;
    state->trunk_lcn_freq[5] = 852512500L;
    state->edacs_tuned_lcn = 6;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 852512500L;
    opts->trunk_is_tuned = 1;
    state->last_vc_sync_time = dsd_decode_time();
    state->last_vc_sync_time_m = dsd_decode_now_mono_s();
    g_failed_request = dsd_trunk_tuning_request_begin();
    state->edacs_tune_request = g_failed_request; /* as edacs_tune_to_lcn() records an accepted PENDING tune */
    dsd_trunk_tuning_request_mark_ready(g_failed_request);
    dsd_trunk_tuning_request_publish(g_failed_request, DSD_TRUNK_TUNE_RESULT_FAILED);
}

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    g_get_frame_sync_calls++;
    dsd_decode_clock_test_set_ns(k_t0_ns + (uint64_t)g_get_frame_sync_calls * STALL_STEP_MS * 1000000ULL);
    if (g_get_frame_sync_calls == 1) {
        enter_failed_grant_state(opts, state);
    }
    if (g_get_frame_sync_calls <= STALL_FRAMES) {
        g_tuned_at_last_sync = opts->trunk_is_tuned;
        return DSD_SYNC_EDACS_POS;
    }
    dsd_exitflag_store(1);
    return DSD_SYNC_NONE;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

int
main(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }
    dsd_decode_clock_use_test(k_t0_ns);
    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;

    const int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);

    int test_rc = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "engine run failed rc=%d\n", rc);
        test_rc = 1;
    }
    if (g_tuned_at_last_sync != 0 || g_process_frame_calls == 0 || dsd_trunk_tuning_pending_request() != 0U) {
        DSD_FPRINTF(stderr,
                    "FAIL #717 engine: after a failed EDACS grant tune, %d control-channel syncs over %d ms (hangtime "
                    "1 s): trunk_is_tuned=%d at the last sync, %d frames dispatched, failed request %llu still gating "
                    "(pending=%llu)\n",
                    STALL_FRAMES, STALL_FRAMES * STALL_STEP_MS, g_tuned_at_last_sync, g_process_frame_calls,
                    (unsigned long long)g_failed_request, (unsigned long long)dsd_trunk_tuning_pending_request());
        test_rc = 1;
    }

    freeState(state);
    free(state);
    free(opts);
    dsd_decode_clock_use_system();
    if (test_rc == 0) {
        printf("ENGINE_EDACS_FAILED_GRANT_STALL: OK\n");
    }
    return test_rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
