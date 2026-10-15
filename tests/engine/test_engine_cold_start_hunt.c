// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The startup control-channel hunt through the real engine loop (issue #703): it runs on the decoder thread from the
 * outer loop and between frames of a continuous sync, an input switch pending its boundary holds it, a mode change
 * reaches DMR even while another protocol keeps syncing, and a hunt whose owner validated control named is finished
 * between frames.
 *
 * getFrameSync() and processFrame() are fakes. The first fake sync, after the engine set its input up, turns rigctl on
 * (a peer no request reaches: the trunk tuning and rigctl query hooks are replaced) and loads a three-row channel map;
 * each later call advances the decode clock.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
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

#define T0_NS   1000000000000000000ULL
#define ROW0_HZ 451000000L
#define ROW1_HZ 452000000L
#define ROW2_HZ 453000000L

enum scenario {
    SC_OUTER_LOOP = 0,
    SC_SYNCED_LOOP,
    SC_NAMED_BETWEEN_FRAMES,
    SC_INPUT_SWITCH,
    SC_MODE_CHANGE_DMR,
};

static enum scenario g_scenario;
static int g_calls;
static uint64_t g_now_ms;
static int g_tunes;
static long g_first_tune;
static long g_hw_freq;
static uint64_t g_tunes_before_ms; // decode time of the last tune
static int g_validated_sent;
static int g_dmr_armed_seen; // DMR's startup window was armed before the mode change
static int g_failures;

static int
expect(int cond, const char* what) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL [scenario %d]: %s\n", (int)g_scenario, what);
        g_failures++;
        return 1;
    }
    return 0;
}

static dsd_trunk_tune_result
stub_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    g_tunes++;
    if (g_first_tune == 0) {
        g_first_tune = freq;
    }
    g_tunes_before_ms = g_now_ms;
    g_hw_freq = freq;
    opts->rtlsdr_center_freq = (uint32_t)freq;
    state->trunk_cc_freq = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
stub_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)freq;
    (void)ted_sps;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static dsd_trunk_tune_result
stub_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
}

static long int
stub_query(const dsd_opts* opts) {
    (void)opts;
    return g_hw_freq;
}

static void
advance(uint64_t ms) {
    g_now_ms += ms;
    dsd_decode_clock_test_set_ns(T0_NS + g_now_ms * 1000000ULL);
}

/* After the engine set its input up: a rigctl-moved radio parked on a dead start frequency, and a channel map. */
static void
install_session(dsd_opts* opts, dsd_state* state) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_cc_request = stub_tune_to_cc;
    hooks.tune_to_freq_request = stub_tune_to_freq;
    hooks.return_to_cc_request = stub_return_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){.get_current_freq_hz = stub_query});
    opts->use_rigctl = 1;
    opts->trunk_enable = 1;
    DSD_SNPRINTF(opts->chan_in_file, sizeof(opts->chan_in_file), "%s", "site.csv");
    state->trunk_lcn_freq[0] = ROW0_HZ;
    state->trunk_lcn_freq[1] = ROW1_HZ;
    state->trunk_lcn_freq[2] = ROW2_HZ;
    state->lcn_freq_count = 3;
}

static int
finish(void) {
    dsd_exitflag_store(1);
    return DSD_SYNC_NONE;
}

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

/* Frames decode to nothing, except the one validated P25 control block the named-owner scenario sends on its probe. */
void
processFrame(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    if (g_scenario == SC_NAMED_BETWEEN_FRAMES && g_tunes == 1 && !g_validated_sent && g_now_ms >= 6000U) {
        p25_sm_note_validated_cc(opts, state);
        g_validated_sent = 1;
    }
}

// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int
__wrap_getFrameSync(dsd_opts* opts, dsd_state* state) {
    g_calls++;
    if (g_calls == 1) {
        install_session(opts, state);
    }
    advance(250U);
    switch (g_scenario) {
        case SC_OUTER_LOOP:
            if (g_now_ms > 7000U) {
                return finish();
            }
            return DSD_SYNC_NONE;
        case SC_SYNCED_LOOP:
            // Unvalidated DMR data syncs that never stop: the outer loop runs only before the first one.
            if (g_now_ms > 7000U) {
                return finish();
            }
            return DSD_SYNC_DMR_BS_DATA_POS;
        case SC_NAMED_BETWEEN_FRAMES:
            if (g_now_ms > 7500U) {
                return finish();
            }
            return DSD_SYNC_P25P1_POS;
        case SC_INPUT_SWITCH:
            if (g_now_ms == 4750U) {
                state->input_boundary = 1; // an input switch committed just before the window runs out
            }
            if (g_now_ms > 11000U) {
                return finish();
            }
            return DSD_SYNC_NONE;
        case SC_MODE_CHANGE_DMR:
            if (g_now_ms == 2750U) {
                g_dmr_armed_seen = dmr_sm_cold_start_active(dmr_sm_get_ctx());
            }
            if (g_now_ms == 3000U) {
                opts->frame_nxdn48 = opts->frame_nxdn96 = opts->frame_provoice = 1; // switched to -fa mid-session
                opts->frame_p25p1 = opts->frame_p25p2 = 1;
            }
            if (g_now_ms > 9000U) {
                return finish();
            }
            return DSD_SYNC_DMR_BS_DATA_POS;
    }
    return finish();
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static int
run_scenario(enum scenario sc, int ft) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    g_scenario = sc;
    g_calls = 0;
    g_now_ms = 0U;
    g_tunes = 0;
    g_first_tune = 0;
    g_hw_freq = 450000000L;
    g_tunes_before_ms = 0U;
    g_validated_sent = 0;
    g_dmr_armed_seen = 0;
    dsd_decode_clock_use_test(T0_NS);
    dsd_exitflag_store(0);
    dsd_trunk_tuning_requests_reset();

    initOpts(opts);
    initState(state);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;
    opts->frame_dstar = opts->frame_x2tdma = opts->frame_ysf = opts->frame_m17 = 0;
    opts->frame_nxdn48 = opts->frame_nxdn96 = opts->frame_provoice = opts->frame_dpmr = 0;
    opts->frame_dmr = 1;
    opts->frame_p25p1 = opts->frame_p25p2 = ft ? 1 : 0;

    const int rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    expect(rc == 0, "the engine run ends cleanly");

    p25_sm_ctx_t* p25 = p25_sm_get_ctx();
    dmr_sm_ctx_t* dmr = dmr_sm_get_ctx();
    switch (sc) {
        case SC_OUTER_LOOP:
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ, "the outer loop runs the startup hunt");
            expect(g_tunes_before_ms > 5000U, "not before the window ran out");
            break;
        case SC_SYNCED_LOOP:
            expect(g_tunes >= 1 && g_first_tune == ROW0_HZ, "continuous unvalidated syncs do not starve the hunt");
            expect(state->trunk_cc_freq == 0 && state->p25_cc_freq == 0, "a probe is no control channel");
            break;
        case SC_NAMED_BETWEEN_FRAMES:
            expect(g_validated_sent == 1, "the validated block was decoded on the probe");
            expect(p25->cold_start_hunt == 0 && state->p25_cc_freq == ROW0_HZ,
                   "a hunt whose owner was named is finished between frames");
            expect(g_tunes == 1, "and never probes again");
            break;
        case SC_INPUT_SWITCH:
            expect(g_tunes >= 1 && g_tunes_before_ms > 10000U,
                   "an input switch commit holds the probe until a full window after its boundary");
            break;
        case SC_MODE_CHANGE_DMR:
            expect(g_dmr_armed_seen == 1, "DMR armed its startup window between frames");
            expect(g_tunes == 0 && dmr->cold_start_armed_m <= 0.0 && dmr->cold_start_hunt == 0,
                   "a mode change releases DMR's startup window while frames keep syncing");
            break;
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_decode_clock_use_system();
    p25_sm_init_ctx(p25, NULL, NULL);
    dmr_sm_init_ctx(dmr, NULL, NULL);
    freeState(state);
    free(state);
    free(opts);
    return 0;
}

int
main(void) {
    run_scenario(SC_OUTER_LOOP, 1);
    run_scenario(SC_SYNCED_LOOP, 1);
    run_scenario(SC_NAMED_BETWEEN_FRAMES, 1);
    run_scenario(SC_INPUT_SWITCH, 1);
    run_scenario(SC_MODE_CHANGE_DMR, 0);
    if (g_failures == 0) {
        printf("engine cold start hunt: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
