// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
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

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static int g_rtl_tune_calls = 0;
static uint32_t g_rtl_tune_freq = 0;
static int g_rtl_tune_result = RTL_STREAM_TUNE_OK;
static int g_rtl_output_rate = 48000;
static int g_rtl_cqpsk_enable = 0;
static int g_rtl_symbol_rate_hz = 6000;
static int g_rtl_symbol_levels = 4;
static int g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
static int g_rtl_ted_sps = 8;
static int g_rtl_ted_sps_override = 8;
static int g_pending_active = 0;
static uint32_t g_pending_target_freq_hz = 0;
static int g_pending_cqpsk = -1;
static int g_pending_symbol_rate_hz = 0;
static int g_pending_symbol_levels = 0;
static int g_pending_channel_profile = 0;
static int g_pending_ted_sps = 0;
static int g_pending_ted_override = 0;

static void
reset_rtl_profile_fakes(void) {
    g_rtl_tune_calls = 0;
    g_rtl_tune_freq = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_output_rate = 48000;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_ted_sps = 8;
    g_rtl_ted_sps_override = 8;
    g_pending_active = 0;
    g_pending_target_freq_hz = 0;
    g_pending_cqpsk = -1;
    g_pending_symbol_rate_hz = 0;
    g_pending_symbol_levels = 0;
    g_pending_channel_profile = 0;
    g_pending_ted_sps = 0;
    g_pending_ted_override = 0;
}

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
uint32_t
__wrap_rtl_stream_output_rate(const RtlSdrContext* ctx) {
    (void)ctx;
    return (uint32_t)g_rtl_output_rate;
}

int
__wrap_rtl_stream_dsp_get(int* cqpsk_enable, int* fll_enable, int* ted_enable) {
    if (cqpsk_enable) {
        *cqpsk_enable = g_rtl_cqpsk_enable;
    }
    if (fll_enable) {
        *fll_enable = 0;
    }
    if (ted_enable) {
        *ted_enable = 1;
    }
    return 0;
}

int
__wrap_rtl_stream_get_symbol_profile_full(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = g_rtl_symbol_rate_hz;
    }
    if (out_levels) {
        *out_levels = g_rtl_symbol_levels;
    }
    if (out_channel_profile) {
        *out_channel_profile = g_rtl_channel_profile;
    }
    return 0;
}

int
__wrap_rtl_stream_get_ted_sps(void) {
    return g_rtl_ted_sps;
}

int
__wrap_rtl_stream_get_ted_sps_override(void) {
    return g_rtl_ted_sps_override;
}

void
__wrap_rtl_stream_prepare_retune_profile_for_target(uint32_t target_freq_hz, int cqpsk_enable, int symbol_rate_hz,
                                                    int levels, int channel_profile, int ted_sps,
                                                    int persist_ted_override) {
    g_pending_active = 1;
    g_pending_target_freq_hz = target_freq_hz;
    g_pending_cqpsk = cqpsk_enable;
    g_pending_symbol_rate_hz = symbol_rate_hz;
    g_pending_symbol_levels = levels;
    g_pending_channel_profile = channel_profile;
    g_pending_ted_sps = ted_sps;
    g_pending_ted_override = persist_ted_override ? 1 : 0;
}

static void
apply_pending_profile(uint32_t target_freq_hz) {
    if (!g_pending_active) {
        return;
    }
    if (g_pending_target_freq_hz != 0 && g_pending_target_freq_hz != target_freq_hz) {
        return;
    }
    if (g_pending_cqpsk >= 0) {
        g_rtl_cqpsk_enable = g_pending_cqpsk ? 1 : 0;
    }
    if (g_pending_symbol_rate_hz > 0) {
        g_rtl_symbol_rate_hz = g_pending_symbol_rate_hz;
        g_rtl_symbol_levels = g_pending_symbol_levels;
        g_rtl_channel_profile = g_pending_channel_profile;
    }
    if (g_pending_ted_sps > 0) {
        g_rtl_ted_sps = g_pending_ted_sps;
        g_rtl_ted_sps_override = g_pending_ted_override ? g_pending_ted_sps : 0;
    }
    g_pending_active = 0;
    g_pending_target_freq_hz = 0;
}

int
__wrap_rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    g_rtl_tune_calls++;
    g_rtl_tune_freq = center_freq_hz;
    if (g_rtl_tune_result == RTL_STREAM_TUNE_OK) {
        apply_pending_profile(center_freq_hz);
    }
    return g_rtl_tune_result;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

int
ui_start(dsd_opts* opts, dsd_state* state) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)state;
    return 0;
}

void
ui_stop(void) { // NOLINT(misc-use-internal-linkage)
}

static int
init_test_runtime(dsd_opts** opts_out, dsd_state** state_out) {
    // dsd_state is multi-megabyte; keep it off the function stack.
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
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

    // DMR payload and reliability history share noCarrier's generic reset path.
    // Seed both buffers with sentinels and move the payload pointer into the
    // dibit buffer to catch regressions that reset through the wrong backing
    // store after carrier loss.
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
            DSD_FPRINTF(stderr, "dmr payload buf[%d] not reset: %d\n", i, state->dmr_payload_buf[i]);
            rc = 1;
            break;
        }
    }

    if (state->dmr_reliab_buf != NULL) {
        rc |= expect_true("dmr-reliab-pointer-buffer", state->dmr_reliab_p == state->dmr_reliab_buf + 200);
        for (int i = 0; i < 200; i++) {
            if (state->dmr_reliab_buf[i] != 0U) {
                DSD_FPRINTF(stderr, "dmr reliab buf[%d] not reset: %u\n", i, (unsigned)state->dmr_reliab_buf[i]);
                rc = 1;
                break;
            }
        }
    }

    // A recent P25 voice-channel sync means noCarrier should keep trunk tuning
    // state intact even when the control-channel timer is stale. This preserves
    // an active voice call rather than forcing an unnecessary control-channel
    // reacquisition.
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

    // Once both control and voice sync are stale, the same reset path should
    // clear the tuned flags and cached voice frequencies so scanning can resume
    // from a clean trunking state.
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;

    noCarrier(opts, state);

    rc |= expect_true("p25-stale-vc-clears-tuned", opts->p25_is_tuned == 0);
    rc |= expect_true("p25-stale-vc-clears-freq", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->p25_trunk = 0;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 1;
    state->trunk_cc_freq = 851012500;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL);
    state->trunk_vc_freq[0] = 852012500;
    state->trunk_vc_freq[1] = 852012500;

    noCarrier(opts, state);

    rc |= expect_true("generic-vc-sync-preserves-tuned", opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 1);
    rc |= expect_true("generic-vc-sync-preserves-freq",
                      state->trunk_vc_freq[0] == 852012500 && state->trunk_vc_freq[1] == 852012500);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769768750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 0;
    state->p25_p2_active_slot = 0;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;
    state->samplesPerSymbol = 8;
    state->symbolCenter = 3;
    state->rf_mod = 1;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-cc-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-syncs-selected-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-rate", g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-cqpsk", g_rtl_cqpsk_enable == 1);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-ted", g_rtl_ted_sps == 10 && g_rtl_ted_sps_override == 0);
    rc |= expect_true("p25-rtl-nocarrier-clear-tuned", opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-clear-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 771156250;
    state->lcn_freq_count = 1;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-cache-prime", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 769868750U);

    state->last_cc_sync_time = time(NULL) - 11;
    state->lcn_freq_roll = 0;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-cache-allows-scan-retune",
                      g_rtl_tune_calls == 3 && g_rtl_tune_freq == 771156250U);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-auto-delayed-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-auto-delayed-keeps-p25-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-auto-delayed-profile", g_rtl_symbol_rate_hz == 4800);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769768750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;
    state->trunk_vc_freq[0] = 771056250;
    state->trunk_vc_freq[1] = 771056250;
    g_rtl_tune_result = RTL_STREAM_TUNE_DEFERRED;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-deferred-tune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |=
        expect_true("p25-rtl-nocarrier-deferred-preserves-tuned", opts->p25_is_tuned == 1 && opts->trunk_is_tuned == 1);
    rc |= expect_true("p25-rtl-nocarrier-deferred-preserves-vc",
                      state->p25_vc_freq[0] == 771056250 && state->p25_vc_freq[1] == 771056250
                          && state->trunk_vc_freq[0] == 771056250 && state->trunk_vc_freq[1] == 771056250);
    rc |= expect_true("p25-rtl-nocarrier-deferred-restores-cc",
                      state->p25_cc_freq == 769768750 && state->trunk_cc_freq == 769868750);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-deferred-retries", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-retry-syncs-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-retry-clears-tuned", opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-retry-clears-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0
                                                               && state->trunk_vc_freq[0] == 0
                                                               && state->trunk_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 935000000;
    state->trunk_cc_freq = 935000000;
    state->p25_cc_is_tdma = 2;
    state->p25_p2_active_slot = 0;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;

    noCarrier(opts, state);

    rc |= expect_true("generic-rtl-nocarrier-slot-zero-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 935000000U);
    rc |= expect_true("generic-rtl-nocarrier-preserves-cc-alias",
                      state->p25_cc_freq == 935000000 && state->trunk_cc_freq == 935000000);
    rc |= expect_true("generic-rtl-nocarrier-keeps-profile",
                      g_rtl_symbol_rate_hz == 6000 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    opts->frame_p25p1 = 0;
    opts->frame_p25p2 = 0;
    opts->frame_dmr = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 852012500;
    state->trunk_cc_freq = 851012500;
    state->dmr_rest_channel = 7;
    state->trunk_chan_map[7] = 853012500;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;

    noCarrier(opts, state);

    rc |= expect_true("dmr-rtl-nocarrier-rest-cc-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 853012500U);
    rc |= expect_true("dmr-rtl-nocarrier-clears-rest", state->dmr_rest_channel == -1);
    rc |= expect_true("dmr-rtl-nocarrier-clears-stale-p25-cc",
                      state->trunk_cc_freq == 853012500 && state->p25_cc_freq == 0);
    rc |= expect_true("dmr-rtl-nocarrier-keeps-generic-profile",
                      g_rtl_symbol_rate_hz == 6000 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
#endif

    // Trunk scan keeps long-lived discovery state across carrier gaps. The test
    // keeps DMR confidence and P25 control-channel candidates populated while
    // still requiring transient P25 frame metrics to be reset.
    opts->trunk_scan_enabled = 1;
    state->dmr_color_code = 5;
    state->dmr_confidence_locked = 1;
    state->dmr_confidence_color_code = 5;
    state->dmr_confidence_candidate_cc = 5;
    state->dmr_confidence_candidate_count = 2;
    state->dmr_confidence_voice_sync_seen[0] = 1;
    state->p25_cc_cache_loaded = 1;
    state->p25_p1_fec_ok = 7;
    dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_get(state);
    if (cc == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: cc-candidates\n");
        rc = 1;
    } else {
        cc->count = 2;
        cc->idx = 1;
        cc->candidates[0] = 851012500L;
        cc->candidates[1] = 852012500L;
    }

    noCarrier(opts, state);

    rc |=
        expect_true("trunk-scan-preserves-dmr-confidence",
                    state->dmr_color_code == 5 && state->dmr_confidence_locked == 1
                        && state->dmr_confidence_color_code == 5 && state->dmr_confidence_candidate_cc == 5
                        && state->dmr_confidence_candidate_count == 2 && state->dmr_confidence_voice_sync_seen[0] == 1);
    rc |= expect_true("trunk-scan-preserves-p25-cache", state->p25_cc_cache_loaded == 1 && cc != NULL && cc->count == 2
                                                            && cc->idx == 1 && cc->candidates[0] == 851012500L
                                                            && cc->candidates[1] == 852012500L);
    rc |= expect_true("trunk-scan-still-resets-p25-metrics", state->p25_p1_fec_ok == 0);

#ifdef USE_RADIO
    // Radio builds also exercise the RTL/FSK reacquisition counters. A recovered
    // sync must close the current gap and refresh the last-sync timer without
    // depending on real hardware.
    dsd_rtl_stream_metrics_hooks hooks = {.output_kind = fake_rtl_fsk_output_kind};
    dsd_rtl_stream_metrics_hooks_set(&hooks);
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
    dsd_rtl_stream_metrics_hooks_set(NULL);
#endif

    free_test_runtime(opts, state);

    if (rc == 0) {
        printf("ENGINE_NO_CARRIER_RESET: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
