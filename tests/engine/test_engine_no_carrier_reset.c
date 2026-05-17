// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#define DSD_NEO_MAIN
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#undef DSD_NEO_MAIN

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

#ifdef USE_RADIO
static int
fake_rtl_fsk_output_kind(void) {
    return RTL_STREAM_OUTPUT_SYMBOL_FSK;
}
#endif

int
ui_start(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    return 0;
}

void
ui_stop(void) {}

static int
init_test_runtime(dsd_opts** opts_out, dsd_state** state_out) {
    // dsd_state is multi-megabyte; keep it off the function stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        fprintf(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }

    initOpts(opts);
    initState(state);

    *opts_out = opts;
    *state_out = state;
    return 0;
}

static void
free_test_runtime(dsd_opts* opts, dsd_state* state) {
    if (state != NULL) {
        freeState(state);
    }
    free(state);
    free(opts);
}

int
main(void) {
    int rc = 0;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    for (int i = 0; i < 200; i++) {
        state->dmr_payload_buf[i] = 0x7F7F7F7F;
        if (state->dmr_reliab_buf != NULL) {
            state->dmr_reliab_buf[i] = 0xA5U;
        }
    }

    state->dmr_payload_p = state->dibit_buf + 321;
    if (state->dmr_reliab_buf != NULL) {
        state->dmr_reliab_p = state->dmr_reliab_buf + 321;
    }

    noCarrier(opts, state);

    rc |= expect_true("dmr-payload-pointer-buffer", state->dmr_payload_p == state->dmr_payload_buf + 200);
    rc |= expect_true("dmr-payload-pointer-not-dibit", state->dmr_payload_p != state->dibit_buf + 200);
    rc |= expect_true("dibit-pointer-reset", state->dibit_buf_p == state->dibit_buf + 200);

    for (int i = 0; i < 200; i++) {
        if (state->dmr_payload_buf[i] != 0) {
            fprintf(stderr, "dmr payload buf[%d] not reset: %d\n", i, state->dmr_payload_buf[i]);
            rc = 1;
            break;
        }
    }

    if (state->dmr_reliab_buf != NULL) {
        rc |= expect_true("dmr-reliab-pointer-buffer", state->dmr_reliab_p == state->dmr_reliab_buf + 200);
        for (int i = 0; i < 200; i++) {
            if (state->dmr_reliab_buf[i] != 0U) {
                fprintf(stderr, "dmr reliab buf[%d] not reset: %u\n", i, (unsigned)state->dmr_reliab_buf[i]);
                rc = 1;
                break;
            }
        }
    }

    opts->p25_trunk = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL);
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;

    noCarrier(opts, state);

    rc |= expect_true("p25-vc-sync-preserves-tuned", opts->p25_is_tuned == 1);
    rc |= expect_true("p25-vc-sync-preserves-alias", opts->trunk_is_tuned == 1);
    rc |= expect_true("p25-vc-sync-preserves-freq", state->p25_vc_freq[0] == 851012500);

    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;

    noCarrier(opts, state);

    rc |= expect_true("p25-stale-vc-clears-tuned", opts->p25_is_tuned == 0);
    rc |= expect_true("p25-stale-vc-clears-freq", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);

#ifdef USE_RADIO
    dsd_rtl_stream_metrics_hooks_set((dsd_rtl_stream_metrics_hooks){.output_kind = fake_rtl_fsk_output_kind});
    opts->audio_in_type = AUDIO_IN_RTL;
    state->rtl_ctx = (struct RtlSdrContext*)state;
    state->lastsynctype = DSD_SYNC_YSF_POS;
    state->rtl_fsk_reacquire_gap_start_m = dsd_time_now_monotonic_s() - 1.0;
    state->rtl_fsk_reacquire_last_sync_m = state->rtl_fsk_reacquire_gap_start_m - 1.0;
    state->rtl_fsk_reacquire_last_sync_time = time(NULL) - 2;
    double old_reacquire_sync_m = state->rtl_fsk_reacquire_last_sync_m;

    noCarrier(opts, state);

    rc |= expect_true("rtl-fsk-recovered-sync-clears-gap", state->rtl_fsk_reacquire_gap_start_m == 0.0);
    rc |= expect_true("rtl-fsk-recovered-sync-refreshes-timer",
                      state->rtl_fsk_reacquire_last_sync_m > old_reacquire_sync_m);
    dsd_rtl_stream_metrics_hooks_set((dsd_rtl_stream_metrics_hooks){0});
#endif

    free_test_runtime(opts, state);

    if (rc == 0) {
        printf("ENGINE_NO_CARRIER_RESET: OK\n");
    }
    return rc;
}
