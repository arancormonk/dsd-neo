// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
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
    return RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
}
#endif

#if defined(DSD_NEO_TEST_RTL_WRAP)
static int g_check_p25_tick_guard = 0;
static int g_p25_tick_guard_held_during_context_update = 0;

static int
p25_tick_guard_is_held(void) {
    if (!p25_sm_tick_guard_try_enter()) {
        return 1;
    }
    p25_sm_tick_guard_leave();
    return 0;
}

// GNU ld --wrap entry points must keep the reserved __real_* and __wrap_* names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_p25_sm_await_pending_cc_tune(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, uint64_t request_id,
                                        const char* source);

int
__wrap_p25_sm_await_pending_cc_tune(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, uint64_t request_id,
                                    const char* source) {
    if (g_check_p25_tick_guard) {
        g_p25_tick_guard_held_during_context_update = p25_tick_guard_is_held();
    }
    return __real_p25_sm_await_pending_cc_tune(ctx, opts, state, request_id, source);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static int g_p25_tick_guard_held_during_tune = 0;
static int g_rtl_tune_calls = 0;
static uint32_t g_rtl_tune_freq = 0;
static uint64_t g_rtl_tune_token = 0U;
static int g_rtl_tune_result = RTL_STREAM_TUNE_OK;
static int g_rtl_output_rate = 48000;
static int g_rtl_cqpsk_enable = 0;
static int g_rtl_symbol_rate_hz = 6000;
static int g_rtl_symbol_levels = 4;
static int g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
static int g_rtl_ted_sps = 8;
static int g_rtl_ted_sps_override = 8;
static int g_rtl_fsk_reacquire_requests = 0;
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
    g_rtl_tune_token = 0U;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_output_rate = 48000;
    g_rtl_cqpsk_enable = 1;
    g_rtl_symbol_rate_hz = 6000;
    g_rtl_symbol_levels = 4;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    g_rtl_ted_sps = 8;
    g_rtl_ted_sps_override = 8;
    g_rtl_fsk_reacquire_requests = 0;
    g_pending_active = 0;
    g_pending_target_freq_hz = 0;
    g_pending_cqpsk = -1;
    g_pending_symbol_rate_hz = 0;
    g_pending_symbol_levels = 0;
    g_pending_channel_profile = 0;
    g_pending_ted_sps = 0;
    g_pending_ted_override = 0;
    g_check_p25_tick_guard = 0;
    g_p25_tick_guard_held_during_tune = 0;
    g_p25_tick_guard_held_during_context_update = 0;
}

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
uint32_t
__wrap_rtl_stream_output_rate(const RtlSdrContext* ctx) {
    (void)ctx;
    return (uint32_t)g_rtl_output_rate;
}

int
__wrap_rtl_stream_get_cqpsk_status(int* cqpsk_enable, int* cqpsk_timing_active) {
    if (cqpsk_enable) {
        *cqpsk_enable = g_rtl_cqpsk_enable;
    }
    if (cqpsk_timing_active) {
        *cqpsk_timing_active = g_rtl_cqpsk_enable ? 1 : 0;
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

void
__wrap_rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t target_freq_hz, int cqpsk_enable,
                                                              int symbol_rate_hz, int levels, int channel_profile,
                                                              int ted_sps, int persist_ted_override,
                                                              const rtl_stream_retune_gain_profile* gain_profile) {
    (void)gain_profile;
    __wrap_rtl_stream_prepare_retune_profile_for_target(target_freq_hz, cqpsk_enable, symbol_rate_hz, levels,
                                                        channel_profile, ted_sps, persist_ted_override);
}

void
__wrap_rtl_stream_clear_pending_retune_profile(void) {
    g_pending_active = 0;
    g_pending_target_freq_hz = 0;
    g_pending_cqpsk = -1;
    g_pending_symbol_rate_hz = 0;
    g_pending_symbol_levels = 0;
    g_pending_channel_profile = 0;
    g_pending_ted_sps = 0;
    g_pending_ted_override = 0;
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

int
__wrap_rtl_stream_tune_tagged(RtlSdrContext* ctx, uint32_t center_freq_hz, uint64_t token) {
    g_rtl_tune_token = token;
    if (g_check_p25_tick_guard) {
        g_p25_tick_guard_held_during_tune = p25_tick_guard_is_held();
    }
    return __wrap_rtl_stream_tune(ctx, center_freq_hz);
}

int
__wrap_rtl_stream_request_fsk_reacquire(void) {
    g_rtl_fsk_reacquire_requests++;
    return 1;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

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
    state->p25_mac_frag[0].active = 1U;
    state->p25_mac_frag[0].opcode = 0x89U;
    state->p25_mac_frag[0].data_len = 4U;
    state->p25_mac_frag[0].collected = 2U;
    state->p25_mac_frag[0].data[0] = 0xAAU;
    state->p25_mac_frag[1].active = 1U;
    state->p25_mac_frag[1].opcode = 0x8AU;
    state->p25_mac_frag[1].data_len = 8U;
    state->p25_mac_frag[1].collected = 6U;
    state->p25_mac_frag[1].data[5] = 0xBBU;
    state->rtl_fsk_sps_num = 48000;
    state->rtl_fsk_sps_den = 4800;
    state->rtl_fsk_sps_accum = 2400;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    state->p25_crypto_state[1] = DSD_P25_CRYPTO_DECRYPTABLE;
    state->p25_p2_audio_allowed[0] = 1;
    state->p25_p2_audio_allowed[1] = 1;

    noCarrier(opts, state);

    rc |= expect_true("dmr-payload-pointer-buffer", state->dmr_payload_p == state->dmr_payload_buf + 200);
    rc |= expect_true("dmr-payload-pointer-not-dibit", state->dmr_payload_p != state->dibit_buf + 200);
    rc |= expect_true("dibit-pointer-reset", state->dibit_buf_p == state->dibit_buf + 200);
    rc |= expect_true("p25-mac-fragment-reset",
                      state->p25_mac_frag[0].active == 0U && state->p25_mac_frag[0].opcode == 0U
                          && state->p25_mac_frag[0].data_len == 0U && state->p25_mac_frag[0].collected == 0U
                          && state->p25_mac_frag[0].data[0] == 0U && state->p25_mac_frag[1].active == 0U
                          && state->p25_mac_frag[1].opcode == 0U && state->p25_mac_frag[1].data_len == 0U
                          && state->p25_mac_frag[1].collected == 0U && state->p25_mac_frag[1].data[5] == 0U);
    rc |= expect_true("rtl-fsk-sps-cache-reset",
                      state->rtl_fsk_sps_num == 0 && state->rtl_fsk_sps_den == 0 && state->rtl_fsk_sps_accum == 0);
    rc |= expect_true("p25-crypto-readiness-reset", state->p25_crypto_state[0] == DSD_P25_CRYPTO_UNKNOWN
                                                        && state->p25_crypto_state[1] == DSD_P25_CRYPTO_UNKNOWN);
    rc |= expect_true("p25-crypto-audio-gates-reset",
                      state->p25_p2_audio_allowed[0] == 0 && state->p25_p2_audio_allowed[1] == 0);

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

    opts->p25_trunk = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL);
    state->last_vc_sync_time = time(NULL) - 3;
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;
    state->trunk_vc_freq[0] = 851012500;
    state->trunk_vc_freq[1] = 851012500;
    state->p25_p2_active_slot = 0;
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Ch: stale");

    noCarrier(opts, state);

    rc |= expect_true("p25-no-cc-hangtime-clears-tuned", opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-no-cc-hangtime-clears-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0
                                                          && state->trunk_vc_freq[0] == 0
                                                          && state->trunk_vc_freq[1] == 0);
    rc |= expect_true("p25-no-cc-hangtime-clears-active-slot", state->p25_p2_active_slot == -1);
    rc |= expect_true("p25-no-cc-hangtime-clears-active", state->active_channel[0][0] == '\0');

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

    opts->trunk_enable = 1;
    opts->p25_trunk = 0;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 1;
    state->dmr_rest_channel = 4;
    state->trunk_chan_map[4] = 851012500;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 852012500;
    state->trunk_vc_freq[1] = 852012500;

    noCarrier(opts, state);

    rc |= expect_true("dmr-rest-only-stale-clears-rest", state->dmr_rest_channel == -1);
    rc |= expect_true("dmr-rest-only-stale-clears-tuned", opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0);
    rc |= expect_true("dmr-rest-only-stale-clears-vc", state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);
    rc |= expect_true("dmr-rest-only-stale-keeps-cc-empty", state->p25_cc_freq == 0 && state->trunk_cc_freq == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->p25_trunk = 0;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 0;
    state->trunk_cc_freq = 936000000;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 936500000;
    state->trunk_vc_freq[1] = 936500000;

    noCarrier(opts, state);

    rc |= expect_true("generic-trunk-cc-only-keeps-p25-empty",
                      state->p25_cc_freq == 0 && state->trunk_cc_freq == 936000000);
    rc |= expect_true("generic-trunk-cc-only-clears-vc", state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // noCarrier can run from control pumping inside guarded frame dispatch.
    // Guard contention must defer the P25 return without blocking or clearing
    // the voice state, then allow the next main-loop pass to complete it.
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->lastsynctype = DSD_SYNC_P25P1_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    int preheld_guard = p25_sm_tick_guard_try_enter();
    rc |= expect_true("p25-nocarrier-contention-setup", preheld_guard == 1);
    if (preheld_guard) {
        noCarrier(opts, state);
        rc |= expect_true("p25-nocarrier-contention-preserves-state",
                          opts->p25_is_tuned == 1 && opts->trunk_is_tuned == 1 && state->p25_vc_freq[0] == 771056250
                              && state->p25_vc_freq[1] == 771056250);
        p25_sm_tick_guard_leave();
    }

    noCarrier(opts, state);
    rc |= expect_true("p25-nocarrier-contention-retries",
                      opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0 && state->p25_vc_freq[0] == 0);

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
    g_rtl_output_rate = 96000;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    opts->mod_qpsk = 1;
    opts->slot1_on = 0;
    opts->slot2_on = 0;
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
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->sps_hunt_counter = 23;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-cc-retune",
                      g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U && g_rtl_tune_token != 0U);
    rc |= expect_true("p25-rtl-nocarrier-syncs-selected-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-rate", g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-cqpsk", g_rtl_cqpsk_enable == 1);
    rc |= expect_true("p25-rtl-nocarrier-cc-profile-ted", g_rtl_ted_sps == 20 && g_rtl_ted_sps_override == 0);
    rc |= expect_true("p25-rtl-nocarrier-dynamic-symbol-timing",
                      state->samplesPerSymbol == 20 && state->symbolCenter == 9);
    rc |= expect_true("p25-rtl-nocarrier-selects-four-level-profile",
                      state->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4 && state->sps_hunt_counter == 0);
    rc |= expect_true("p25-rtl-nocarrier-reenables-slots", opts->slot1_on == 1 && opts->slot2_on == 1);
    rc |= expect_true("p25-rtl-nocarrier-clear-tuned", opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-clear-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);

    // A controller wait timeout keeps frame dispatch closed and starts the CC
    // acquisition window only after the exact tagged request completes.
    reset_rtl_profile_fakes();
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 771056250;
    p25_sm_ctx_t* pending_ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(pending_ctx, opts, state);
    g_check_p25_tick_guard = 1;

    noCarrier(opts, state);
    g_check_p25_tick_guard = 0;

    uint64_t pending_request_id = pending_ctx->cc_tune_request_id;
    rc |= expect_true("p25-rtl-nocarrier-pending-tagged",
                      g_rtl_tune_calls == 1 && g_rtl_tune_token != 0U && pending_request_id == g_rtl_tune_token);
    rc |= expect_true("p25-rtl-nocarrier-pending-holds-acquisition",
                      pending_ctx->cc_tune_pending == 1 && pending_ctx->t_cc_tune_m == 0.0);
    rc |= expect_true("p25-rtl-nocarrier-pending-serializes-tune", g_p25_tick_guard_held_during_tune == 1);
    rc |= expect_true("p25-rtl-nocarrier-pending-serializes-context", g_p25_tick_guard_held_during_context_update == 1);
    int guard_released = p25_sm_tick_guard_try_enter();
    rc |= expect_true("p25-rtl-nocarrier-pending-releases-guard", guard_released == 1);
    if (guard_released) {
        p25_sm_tick_guard_leave();
    }
    rc |= expect_true("p25-rtl-nocarrier-pending-closes-frame-gate",
                      !dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));

    dsd_trunk_tuning_request_complete(pending_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    p25_sm_tick_ctx(pending_ctx, opts, state);
    rc |= expect_true("p25-rtl-nocarrier-completion-starts-acquisition",
                      pending_ctx->cc_tune_pending == 0 && pending_ctx->t_cc_tune_m > 0.0);
    rc |= expect_true("p25-rtl-nocarrier-completion-opens-frame-gate",
                      dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 773456250;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t deferred_scan_time = state->last_cc_sync_time;
    g_rtl_tune_result = RTL_STREAM_TUNE_DEFERRED;

    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-deferred-attempt", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-deferred-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("rtl-scanner-deferred-keeps-deadline", state->last_cc_sync_time == deferred_scan_time);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-deferred-retries", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-retry-advances-candidate", state->lcn_freq_roll == 1);
    rc |= expect_true("rtl-scanner-retry-restarts-deadline", state->last_cc_sync_time > deferred_scan_time);

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
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 769868750;
    state->p25_cc_is_tdma = 2;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-retune",
                      g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-profile",
                      g_rtl_symbol_rate_hz == 4800 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-sync",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);
    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-clears-tuned",
                      opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0);

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
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-delayed-selected-cc-retune",
                      g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-delayed-selected-cc-profile", g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("p25-rtl-nocarrier-delayed-selected-cc-sync",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);

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
    rc |= expect_true("p25-rtl-nocarrier-deferred-preserves-selected-cc",
                      state->p25_cc_freq == 769868750 && state->trunk_cc_freq == 769868750);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-deferred-retries", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-deferred-retry-profile",
                      g_rtl_symbol_rate_hz == 4800 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK);
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
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
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
    state->trunk_vc_freq[0] = 771056250;
    state->trunk_vc_freq[1] = 771056250;
    state->p25_p2_active_slot = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-failed-tune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    rc |= expect_true("p25-rtl-nocarrier-failed-clears-tuned", opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-failed-clears-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0
                                                                && state->trunk_vc_freq[0] == 0
                                                                && state->trunk_vc_freq[1] == 0);
    rc |= expect_true("p25-rtl-nocarrier-failed-clears-active-slot", state->p25_p2_active_slot == -1);

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
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 936000000;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 936500000;
    state->trunk_vc_freq[1] = 936500000;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;

    noCarrier(opts, state);

    rc |= expect_true("generic-rtl-nocarrier-stale-p25-alias-retune",
                      g_rtl_tune_calls == 1 && g_rtl_tune_freq == 936000000U);
    rc |= expect_true("generic-rtl-nocarrier-stale-p25-alias-cleared",
                      state->p25_cc_freq == 0 && state->trunk_cc_freq == 936000000);
    rc |= expect_true("generic-rtl-nocarrier-stale-p25-keeps-profile",
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
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 938000000;
    state->trunk_cc_freq = 938000000;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->p25_sys_is_tdma = 1;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL);
    state->p25_vc_freq[0] = 938500000;
    state->p25_vc_freq[1] = 938500000;
    state->trunk_vc_freq[0] = 938500000;
    state->trunk_vc_freq[1] = 938500000;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;

    noCarrier(opts, state);

    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;

    noCarrier(opts, state);

    rc |=
        expect_true("generic-rtl-repeated-nocarrier-cc-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 938000000U);
    rc |= expect_true("generic-rtl-repeated-nocarrier-preserves-cc-alias",
                      state->p25_cc_freq == 938000000 && state->trunk_cc_freq == 938000000);
    rc |= expect_true("generic-rtl-repeated-nocarrier-keeps-profile",
                      g_rtl_symbol_rate_hz == 6000 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);
    rc |= expect_true("generic-rtl-repeated-nocarrier-clears-tuned",
                      opts->p25_is_tuned == 0 && opts->trunk_is_tuned == 0);

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
    state->p25_cc_freq = 770168750;
    state->trunk_cc_freq = 770168750;
    state->p25_cc_is_tdma = 0;
    state->p2_cc = 0x293;
    state->dmr_rest_channel = 7;
    state->trunk_chan_map[7] = 0;
    state->synctype = DSD_SYNC_NONE;
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 771056250;
    state->p25_vc_freq[1] = 771056250;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-unmapped-rest-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 770168750U);
    rc |= expect_true("p25-rtl-nocarrier-unmapped-rest-profile", g_rtl_symbol_rate_hz == 4800);
    rc |= expect_true("p25-rtl-nocarrier-unmapped-rest-clears", state->dmr_rest_channel == -1);

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

    // A failed asynchronous generic voice tune keeps dispatch gated until a
    // correlated CC recovery establishes a newer coherent tuner boundary.
    reset_rtl_profile_fakes();
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->p25_trunk = 0;
    opts->trunk_enable = 1;
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->p25_cc_freq = 769868750;
    state->trunk_cc_freq = 936000000;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 936500000;
    state->trunk_vc_freq[1] = 936500000;

    const uint64_t failed_generation = dsd_trunk_tuning_generation();
    rc |= expect_true("generic-rtl-recovery-clean-gate", dsd_trunk_tuning_frame_is_current(failed_generation));
    const uint64_t failed_request_id = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_mark_ready(failed_request_id);

    noCarrier(opts, state);

    rc |= expect_true("generic-rtl-recovery-pending-suppresses-legacy",
                      failed_request_id != 0U && g_rtl_tune_calls == 0 && opts->trunk_is_tuned == 1
                          && dsd_trunk_tuning_request_status(failed_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    dsd_trunk_tuning_request_publish(failed_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_true("generic-rtl-recovery-seeds-failed-gate",
                      dsd_trunk_tuning_request_status(failed_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED
                          && !dsd_trunk_tuning_frame_is_current(failed_generation));

    noCarrier(opts, state);

    const uint64_t recovery_request_id = g_rtl_tune_token;
    rc |=
        expect_true("generic-rtl-recovery-retune-is-correlated",
                    g_rtl_tune_calls == 1 && g_rtl_tune_freq == 936000000U && recovery_request_id > failed_request_id);
    rc |= expect_true("generic-rtl-recovery-holds-retry-state",
                      opts->trunk_is_tuned == 1 && state->trunk_vc_freq[0] == 0 && state->trunk_cc_freq == 936000000
                          && state->p25_cc_freq == 0);
    rc |= expect_true("generic-rtl-recovery-remains-gated-while-pending",
                      dsd_trunk_tuning_request_status(recovery_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING
                          && !dsd_trunk_tuning_frame_is_current(failed_generation));
    rc |=
        expect_true("generic-rtl-recovery-preserves-profile", g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    dsd_trunk_tuning_request_publish(recovery_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_true("generic-rtl-recovery-failure-stays-gated",
                      dsd_trunk_tuning_request_status(recovery_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED
                          && !dsd_trunk_tuning_frame_is_current(failed_generation));

    // A newer unrelated success can retire the failed gate, but it must not
    // make the failed saved recovery request acceptable for this control channel.
    const uint64_t unrelated_request_id = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_complete(unrelated_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("generic-rtl-recovery-unrelated-success-opens-gate",
                      unrelated_request_id > recovery_request_id && dsd_trunk_tuning_pending_request() == 0U
                          && dsd_trunk_tuning_frame_is_current(failed_generation + 1U));

    noCarrier(opts, state);

    const uint64_t retry_request_id = g_rtl_tune_token;
    rc |= expect_true("generic-rtl-recovery-failure-retries-correlated",
                      g_rtl_tune_calls == 2 && retry_request_id > recovery_request_id
                          && dsd_trunk_tuning_request_status(retry_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING
                          && opts->trunk_is_tuned == 1);

    dsd_trunk_tuning_request_publish(retry_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    const uint64_t first_recovery_generation = dsd_trunk_tuning_generation();
    rc |= expect_true("generic-rtl-recovery-success-retires-failed-gate",
                      first_recovery_generation == failed_generation + 2U && dsd_trunk_tuning_pending_request() == 0U
                          && dsd_trunk_tuning_frame_is_current(first_recovery_generation));

    // A target switch while the recovery was in flight must establish a new
    // boundary instead of accepting the old completion for the current CC.
    state->trunk_cc_freq = 937000000;
    noCarrier(opts, state);

    const uint64_t switched_request_id = g_rtl_tune_token;
    rc |= expect_true("generic-rtl-recovery-target-switch-retunes",
                      g_rtl_tune_calls == 3 && g_rtl_tune_freq == 937000000U && switched_request_id > retry_request_id
                          && dsd_trunk_tuning_request_status(switched_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING
                          && !dsd_trunk_tuning_frame_is_current(first_recovery_generation));

    dsd_trunk_tuning_request_publish(switched_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    const uint64_t recovery_generation = dsd_trunk_tuning_generation();
    rc |= expect_true("generic-rtl-recovery-target-switch-commits",
                      recovery_generation == failed_generation + 3U && dsd_trunk_tuning_pending_request() == 0U
                          && dsd_trunk_tuning_frame_is_current(recovery_generation));

    // A legacy VC tune after recovery completion supersedes the saved request.
    // The next no-carrier pass must issue a new correlated CC return instead
    // of clearing VC state while the hardware remains on the newer target.
    dsd_trunk_tuning_hooks legacy_vc_hooks = {0};
    legacy_vc_hooks.tune_to_freq_result = dsd_engine_trunk_tune_to_freq;
    dsd_trunk_tuning_hooks_set(legacy_vc_hooks);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    const long superseding_vc_freq = 937500000;
    rc |= expect_true("generic-rtl-recovery-newer-legacy-vc-tune",
                      dsd_trunk_tuning_hook_tune_to_freq(opts, state, superseding_vc_freq, 0)
                          == DSD_TRUNK_TUNE_RESULT_OK);
    const uint64_t superseding_vc_generation = dsd_trunk_tuning_generation();
    rc |= expect_true("generic-rtl-recovery-newer-legacy-vc-owns-generation",
                      g_rtl_tune_calls == 4 && g_rtl_tune_freq == (uint32_t)superseding_vc_freq
                          && superseding_vc_generation == recovery_generation + 1U && opts->trunk_is_tuned == 1);

    state->last_vc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s() - 11.0;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    noCarrier(opts, state);

    const uint64_t superseding_recovery_request_id = g_rtl_tune_token;
    rc |= expect_true(
        "generic-rtl-recovery-superseded-success-retunes",
        g_rtl_tune_calls == 5 && g_rtl_tune_freq == 937000000U && superseding_recovery_request_id > switched_request_id
            && dsd_trunk_tuning_request_status(superseding_recovery_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING
            && opts->trunk_is_tuned == 1);

    dsd_trunk_tuning_request_publish(superseding_recovery_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    const uint64_t superseding_recovery_generation = dsd_trunk_tuning_generation();
    rc |= expect_true("generic-rtl-recovery-superseded-recovery-commits",
                      superseding_recovery_generation == superseding_vc_generation + 1U
                          && dsd_trunk_tuning_pending_request() == 0U);

    noCarrier(opts, state);
    rc |= expect_true("generic-rtl-recovery-current-success-commits-without-retune",
                      g_rtl_tune_calls == 5 && opts->trunk_is_tuned == 0 && state->trunk_vc_freq[0] == 0
                          && state->trunk_cc_freq == 937000000);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
#endif

    // Leaving trunking retires terminal correlated failures so scanner/manual
    // modes cannot inherit a process-wide frame-dispatch gate. In-flight
    // requests remain gated until their backend publishes a terminal result.
    const uint64_t inactive_generation = dsd_trunk_tuning_generation();
    const uint64_t inactive_failed_request = dsd_trunk_tuning_request_begin();
    dsd_trunk_tuning_request_publish(inactive_failed_request, DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_true("inactive-trunking-seeds-failed-gate",
                      inactive_failed_request != 0U && dsd_trunk_tuning_pending_request() == inactive_failed_request
                          && !dsd_trunk_tuning_frame_is_current(inactive_generation));
    opts->scanner_mode = 1;
    noCarrier(opts, state);
    rc |= expect_true("inactive-trunking-retires-failed-gate",
                      dsd_trunk_tuning_pending_request() == 0U && dsd_trunk_tuning_generation() == inactive_generation
                          && dsd_trunk_tuning_frame_is_current(inactive_generation));

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
#if defined(DSD_NEO_TEST_RTL_WRAP)
    g_rtl_fsk_reacquire_requests = 0;
    double reacquire_now_m = dsd_time_now_monotonic_s();
    time_t reacquire_now = time(NULL);
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = reacquire_now - 1;
    state->last_vc_sync_time = 0;
    state->last_cc_sync_time_m = reacquire_now_m - 1.0;
    state->last_vc_sync_time_m = 0.0;
    state->rtl_fsk_reacquire_last_sync_time = reacquire_now - 1;
    state->rtl_fsk_reacquire_last_sync_m = reacquire_now_m - 1.0;
    state->rtl_fsk_reacquire_gap_start_m = reacquire_now_m - 1.0;
    state->rtl_fsk_reacquire_last_request_m = 0.0;

    noCarrier(opts, state);

    rc |= expect_true("rtl-fsk-short-nosync-gap-does-not-reacquire", g_rtl_fsk_reacquire_requests == 0);

    g_rtl_fsk_reacquire_requests = 0;
    reacquire_now_m = dsd_time_now_monotonic_s();
    reacquire_now = time(NULL);
    state->lastsynctype = DSD_SYNC_NONE;
    state->last_cc_sync_time = reacquire_now - 11;
    state->last_vc_sync_time = 0;
    state->last_cc_sync_time_m = reacquire_now_m - 11.0;
    state->last_vc_sync_time_m = 0.0;
    state->rtl_fsk_reacquire_last_sync_time = reacquire_now - 11;
    state->rtl_fsk_reacquire_last_sync_m = reacquire_now_m - 11.0;
    state->rtl_fsk_reacquire_gap_start_m = reacquire_now_m - 11.0;
    state->rtl_fsk_reacquire_last_request_m = 0.0;

    noCarrier(opts, state);

    rc |= expect_true("rtl-fsk-long-nosync-gap-reacquires-once", g_rtl_fsk_reacquire_requests == 1);
    rc |= expect_true("rtl-fsk-long-nosync-gap-records-request", state->rtl_fsk_reacquire_last_request_m > 0.0);
#endif
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
