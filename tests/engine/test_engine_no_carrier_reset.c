// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/scan_voice_gate.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <math.h>
#include <stdbool.h>
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

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static int g_check_p25_tick_guard = 0;

static int
p25_tick_guard_is_held(void) {
    if (!p25_sm_tick_guard_try_enter()) {
        return 1;
    }
    p25_sm_tick_guard_leave();
    return 0;
}

#endif

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static int g_p25_tick_guard_held_during_tune = 0;
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
__wrap_rtl_stream_prepare_retune_profile_for_target_with_gain(uint32_t target_freq_hz, int cqpsk_enable,
                                                              int symbol_rate_hz, int levels, int channel_profile,
                                                              int ted_sps, int persist_ted_override,
                                                              const rtl_stream_retune_gain_profile* gain_profile) {
    (void)gain_profile;
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
    if (g_check_p25_tick_guard) {
        g_p25_tick_guard_held_during_tune = p25_tick_guard_is_held();
    }
    g_rtl_tune_calls++;
    g_rtl_tune_freq = center_freq_hz;
    if (g_rtl_tune_result == RTL_STREAM_TUNE_OK) {
        apply_pending_profile(center_freq_hz);
    }
    return g_rtl_tune_result;
}

int
__wrap_rtl_stream_tune_tagged(RtlSdrContext* ctx, uint32_t center_freq_hz, uint64_t request_id) {
    (void)request_id;
    return __wrap_rtl_stream_tune(ctx, center_freq_hz);
}

int
__wrap_rtl_stream_request_fsk_reacquire(void) {
    g_rtl_fsk_reacquire_requests++;
    return 1;
}

// Rigctl needs a success path here, not just the socket-failure one the rest of the file uses:
// the interesting case is a hop whose rigctl leg lands and whose RTL leg then does not.
static int g_rigctl_setfreq_ok = 0;
static int g_rigctl_setfreq_calls = 0;
static long int g_rigctl_setfreq_freq = 0;

bool
__wrap_SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    g_rigctl_setfreq_calls++;
    g_rigctl_setfreq_freq = freq;
    return g_rigctl_setfreq_ok ? true : false;
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

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
static int
test_typed_scan_tune_boundaries(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    int rc = 0;
    reset_rtl_profile_fakes();
    dsd_trunk_tuning_requests_reset();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->lcn_freq_count = 2;
    state->trunk_lcn_freq[0] = state->trunk_lcn_freq[1] = 941012500;
    rc |= expect_true("typed mode metadata", dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NXDN48) == 0);
    rc |= expect_true("typed P25 metadata", dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_P25) == 0);
    state->last_cc_sync_time = time(NULL);
    noCarrier(opts, state);
    rc |= expect_true("typed startup waits for scheduled entry", g_rtl_tune_calls == 0 && !opts->frame_nxdn48);
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("typed automatic mode and profile commit",
                      opts->frame_nxdn48 && state->lcn_freq_roll == 1 && g_rtl_symbol_rate_hz == 2400
                          && g_rtl_symbol_levels == 4 && g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_6K25);
    state->lcn_scan_hold = 1;
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("typed hold prevents automatic entry", g_rtl_tune_calls == 1 && state->lcn_freq_roll == 1);
    state->lcn_scan_hold = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    noCarrier(opts, state);
    const uint64_t pending = dsd_trunk_tuning_pending_request();
    rc |= expect_true("typed timeout leaves outgoing mode", pending != 0 && opts->frame_nxdn48
                                                                && state->lcn_freq_roll == 1
                                                                && dsd_engine_channel_scan_pending(opts, state));
    apply_pending_profile(941012500);
    dsd_trunk_tuning_request_publish(pending, DSD_TRUNK_TUNE_RESULT_OK);
    rc |= expect_true("typed async completion commits",
                      !dsd_engine_channel_scan_pending(opts, state) && opts->frame_p25p1 && opts->frame_p25p2
                          && !opts->frame_dmr && state->lcn_freq_roll == 2 && g_rtl_symbol_rate_hz == 4800);
    opts->use_rigctl = 1;
    opts->setmod_bw = 0;
    g_rigctl_setfreq_ok = 1;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    state->last_cc_sync_time -= 11;
    noCarrier(opts, state);
    rc |= expect_true("typed dual-backend partial failure leaves row", state->lcn_freq_roll == 2 && opts->frame_p25p1);
    rc |= expect_true("typed partial failure closes frame gate",
                      !dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1));
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);
    rc |= expect_true("typed recovery commits new row", opts->frame_nxdn48 && state->lcn_freq_roll == 1);
    rc |= expect_true("typed recovery reopens frame gate",
                      dsd_trunk_tuning_frame_is_dispatchable(dsd_trunk_tuning_generation(), 1));
    g_rigctl_setfreq_ok = 0;
    dsd_engine_channel_scan_leave(opts, state);
    state->rtl_ctx = NULL;
    free_test_runtime(opts, state);
    dsd_trunk_tuning_requests_reset();
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    dsd_call_observation observation = {0};
    observation.protocol = DSD_SYNC_P25P1_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 5001U;
    observation.policy_target_id = 5001U;
    observation.ota_source_id = 6001U;
    observation.observed_m = 1.0;
    rc |= expect_true("seed canonical call", dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_event_sync_slot(opts, state, 0U);

    // DMR payload and soft-decision history share noCarrier's generic reset path.
    // Seed both buffers with sentinels and move the payload pointer into the
    // dibit buffer to catch regressions that reset through the wrong backing
    // store after carrier loss.
    for (int i = 0; i < 200; i++) {
        state->dmr_payload_buf[i] = 0x7F7F7F7F;
        if (state->dmr_soft_buf != NULL) {
            state->dmr_soft_buf[i].reliability = 0xA5U;
        }
    }

    state->dmr_payload_p = state->dibit_buf + 321;
    if (state->dmr_soft_buf != NULL) {
        state->dmr_soft_p = state->dmr_soft_buf + 321;
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
    state->data_header_dd_format[0] = 0x16U;
    state->data_header_dd_format[1] = 0x18U;
    state->data_header_bit_padding[0] = 16U;
    state->data_header_bit_padding[1] = 7U;
    // The --dmr-tg-key-csv lookup reads dmr_data_target_is_group[] to tell a data PDU's talkgroup
    // from a colliding radio id, so a stale group flag surviving carrier loss would qualify the
    // next target written. Both slots are seeded so a slot-0-only reset still fails.
    state->dmr_lrrp_target[0] = 1234U;
    state->dmr_lrrp_target[1] = 5678U;
    state->dmr_data_target_is_group[0] = 1U;
    state->dmr_data_target_is_group[1] = 1U;
    // NXDN's CRC evidence is per-transmission: carrying it across a carrier loss would let
    // the next channel's first noise frame stop a scan and unmute (issue #398).
    state->nxdn_confirmed = 1;
    state->nxdn_confirm_weak_streak = 1;
    state->nxdn_confirm_frame_evidence = 2;
    // YSF's FICH evidence is per-transmission the same way (issue #391): processYSF() reports
    // productive to the SPS hunt for as long as this flag stands, so carrying it across a carrier
    // loss would let the next channel's first FICH failure buy dwell it validated nothing for.
    state->ysf_fich_confirmed = 1U;
    // D-STAR and ProVoice carry the same kind of evidence (issue #421). Both lean on a weak
    // streak that only means anything while the frames stay adjacent, so carrying a streak --
    // or a confirmation -- across a carrier loss would let the next channel's first false
    // match buy the 1992 or 736 symbols it consumed.
    state->dstar_confirmed = 1;
    state->dstar_confirm_weak_streak = 1;
    state->dstar_confirm_frame_evidence = 2;
    state->provoice_confirmed = 1;
    state->provoice_confirm_weak_streak = 1;
    state->provoice_confirm_frame_evidence = 2;

    noCarrier(opts, state);

    rc |= expect_true("nxdn-confirmation-reset", state->nxdn_confirmed == 0 && state->nxdn_confirm_weak_streak == 0
                                                     && state->nxdn_confirm_frame_evidence == 0);
    rc |= expect_true("ysf-fich-confirmation-reset", state->ysf_fich_confirmed == 0U);
    rc |= expect_true("dstar-confirmation-reset", state->dstar_confirmed == 0 && state->dstar_confirm_weak_streak == 0
                                                      && state->dstar_confirm_frame_evidence == 0);
    rc |= expect_true("provoice-confirmation-reset", state->provoice_confirmed == 0
                                                         && state->provoice_confirm_weak_streak == 0
                                                         && state->provoice_confirm_frame_evidence == 0);

    dsd_call_snapshot ended_call;
    rc |= expect_true("no-carrier retains canonical snapshot", dsd_call_state_get(state, 0U, &ended_call) == 1);
    rc |= expect_true("no-carrier ends canonical call", ended_call.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("no-carrier commits canonical history",
                      state->event_history_s[0].Event_History_Items[1].target_id == 5001U);

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
    rc |= expect_true("dmr-short-data-metadata-reset",
                      state->data_header_dd_format[0] == 0U && state->data_header_dd_format[1] == 0U
                          && state->data_header_bit_padding[0] == 0U && state->data_header_bit_padding[1] == 0U);
    rc |= expect_true("dmr-data-target-reset", state->dmr_lrrp_target[0] == 0U && state->dmr_lrrp_target[1] == 0U);
    rc |= expect_true("dmr-data-target-group-flag-reset",
                      state->dmr_data_target_is_group[0] == 0U && state->dmr_data_target_is_group[1] == 0U);

    for (int i = 0; i < 200; i++) {
        if (state->dmr_payload_buf[i] != 0) {
            DSD_FPRINTF(stderr, "dmr payload buf[%d] not reset: %d\n", i, state->dmr_payload_buf[i]);
            rc = 1;
            break;
        }
    }

    if (state->dmr_soft_buf != NULL) {
        rc |= expect_true("dmr-soft-pointer-buffer", state->dmr_soft_p == state->dmr_soft_buf + 200);
        for (int i = 0; i < 200; i++) {
            if (state->dmr_soft_buf[i].reliability != 0U) {
                DSD_FPRINTF(stderr, "dmr soft buf[%d] not reset: %u\n", i,
                            (unsigned)state->dmr_soft_buf[i].reliability);
                rc = 1;
                break;
            }
        }
    }

    // A recent P25 voice-channel sync means noCarrier should keep trunk tuning
    // state intact even when the control-channel timer is stale. This preserves
    // an active voice call rather than forcing an unnecessary control-channel
    // reacquisition.
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL);
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;

    noCarrier(opts, state);

    rc |= expect_true("p25-vc-sync-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("p25-vc-sync-preserves-freq", state->p25_vc_freq[0] == 851012500);

#ifdef USE_RADIO
    // A CQPSK recovery queued by frame sync must also suppress the generic
    // noCarrier return that runs later in the same no-sync cycle. This hold is
    // state-machine-owned and does not refresh the voice-sync timestamp.
    const int saved_audio_in_type = opts->audio_in_type;
    const double recovery_now_m = dsd_time_now_monotonic_s();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 851000000;
    state->trunk_cc_freq = 851000000;
    state->last_vc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time_m = recovery_now_m - 11.0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 851012500;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 851012500;

    p25_sm_ctx_t* recovery_ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(recovery_ctx, opts, state);
    recovery_ctx->state = P25_SM_TUNED;
    recovery_ctx->vc_freq_hz = 851012500;
    recovery_ctx->vc_channel = (2 << 12) | 2;
    recovery_ctx->vc_tg = 7001;
    recovery_ctx->vc_is_tdma = 1;
    recovery_ctx->t_tune_m = recovery_now_m - 1.0;
    recovery_ctx->t_vc_reacquire_m = recovery_now_m;
    recovery_ctx->vc_reacquire_eligible = 1;
    recovery_ctx->vc_reacquire_attempted = 1;
    recovery_ctx->slots[0].grant_active = 1;
    recovery_ctx->slots[0].freq_hz = recovery_ctx->vc_freq_hz;
    recovery_ctx->slots[0].last_grant_m = recovery_ctx->t_tune_m;

    noCarrier(opts, state);

    rc |= expect_true("p25-vc-reacquire-hold-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("p25-vc-reacquire-hold-preserves-freq",
                      state->p25_vc_freq[0] == 851012500 && state->p25_vc_freq[1] == 851012500);
    rc |= expect_true("p25-vc-reacquire-hold-preserves-sync-deadline",
                      fabs(state->last_vc_sync_time_m - (recovery_now_m - 11.0)) <= 1.0e-9);

    recovery_ctx->t_vc_reacquire_m = 0.0;
    opts->audio_in_type = saved_audio_in_type;
    p25_sm_init_ctx(recovery_ctx, opts, state);
#endif

    // Once both control and voice sync are stale, the same reset path should
    // clear the tuned flags and cached voice frequencies so scanning can resume
    // from a clean trunking state.
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;

    noCarrier(opts, state);

    rc |= expect_true("p25-stale-vc-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-stale-vc-clears-freq", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->last_cc_sync_time = time(NULL);
    state->last_vc_sync_time = time(NULL) - 3;
    state->p25_vc_freq[0] = 851012500;
    state->p25_vc_freq[1] = 851012500;
    state->trunk_vc_freq[0] = 851012500;
    state->trunk_vc_freq[1] = 851012500;
    state->p25_p2_active_slot = 0;
    observation.protocol = DSD_SYNC_P25P2_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_GROUP_VOICE;
    observation.ota_target_id = 7001U;
    observation.policy_target_id = 7001U;
    observation.ota_source_id = 8001U;
    observation.channel = 1U;
    observation.frequency_hz = 851012500;
    observation.observed_m = 0.0;
    rc |= expect_true("p25-no-cc-hangtime-seeds-call",
                      dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);

    noCarrier(opts, state);

    rc |= expect_true("p25-no-cc-hangtime-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-no-cc-hangtime-clears-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0
                                                          && state->trunk_vc_freq[0] == 0
                                                          && state->trunk_vc_freq[1] == 0);
    rc |= expect_true("p25-no-cc-hangtime-clears-active-slot", state->p25_p2_active_slot == -1);
    dsd_call_snapshot stale_call = {0};
    rc |= expect_true("p25-no-cc-hangtime-retains-call-snapshot", dsd_call_state_get(state, 0U, &stale_call) > 0);
    rc |= expect_true("p25-no-cc-hangtime-ends-active", stale_call.phase == DSD_CALL_PHASE_ENDED);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->trunk_cc_freq = 851012500;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL);
    state->trunk_vc_freq[0] = 852012500;
    state->trunk_vc_freq[1] = 852012500;

    noCarrier(opts, state);

    rc |= expect_true("generic-vc-sync-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("generic-vc-sync-preserves-freq",
                      state->trunk_vc_freq[0] == 852012500 && state->trunk_vc_freq[1] == 852012500);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->dmr_rest_channel = 4;
    state->trunk_chan_map[4] = 851012500;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 852012500;
    state->trunk_vc_freq[1] = 852012500;

    noCarrier(opts, state);

    rc |= expect_true("dmr-rest-only-stale-clears-rest", state->dmr_rest_channel == -1);
    rc |= expect_true("dmr-rest-only-stale-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("dmr-rest-only-stale-clears-vc", state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);
    rc |= expect_true("dmr-rest-only-stale-keeps-cc-empty", state->p25_cc_freq == 0 && state->trunk_cc_freq == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_enable = 1;
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

    // An accepted return to the control channel retunes the receiver, so a call still open on the
    // voice channel was left behind rather than faded. Reporting that as a sync loss would leave it
    // reacquirable, and the next transmission to appear on the control channel inside the window
    // would be folded into its history row.
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->p25_cc_freq = 0;
    state->trunk_cc_freq = 936000000;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 936500000;
    state->trunk_vc_freq[1] = 936500000;

    dsd_call_observation retuned_call = {0};
    retuned_call.protocol = DSD_SYNC_NXDN_POS;
    retuned_call.slot = 0U;
    retuned_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    retuned_call.ota_target_id = 7001U;
    retuned_call.policy_target_id = 7001U;
    retuned_call.ota_source_id = 8001U;
    retuned_call.observed_m = 1.0;
    rc |=
        expect_true("cc-return-seeds-call", dsd_call_state_observe(state, &retuned_call, DSD_CALL_BOUNDARY_BEGIN) == 1);

    noCarrier(opts, state);

    dsd_call_snapshot retuned_snapshot;
    rc |= expect_true("cc-return-retains-snapshot", dsd_call_state_get(state, 0U, &retuned_snapshot) == 1);
    rc |= expect_true("cc-return-ends-call", retuned_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("cc-return-ends-call-explicitly", retuned_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP) && DSD_NEO_TEST_RTL_WRAP
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // The scanner half of the same rule. noCarrier() steps the scanner before it finalizes calls,
    // so a hop that succeeds leaves the finalizer closing a call that belongs to the frequency the
    // receiver just left.
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 938012500;
    state->trunk_lcn_freq[1] = 939012500;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_tune_calls = 0;

    dsd_call_observation scanned_call = {0};
    scanned_call.protocol = DSD_SYNC_NXDN_POS;
    scanned_call.slot = 0U;
    scanned_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    scanned_call.ota_target_id = 7101U;
    scanned_call.policy_target_id = 7101U;
    scanned_call.ota_source_id = 8101U;
    scanned_call.observed_m = 1.0;
    rc |= expect_true("scanner-hop-seeds-call",
                      dsd_call_state_observe(state, &scanned_call, DSD_CALL_BOUNDARY_BEGIN) == 1);

    noCarrier(opts, state);

    dsd_call_snapshot scanned_snapshot;
    rc |= expect_true("scanner-hop-retuned", g_rtl_tune_calls > 0);
    rc |= expect_true("scanner-hop-advanced", state->lcn_freq_roll == 1);
    rc |= expect_true("scanner-hop-retains-snapshot", dsd_call_state_get(state, 0U, &scanned_snapshot) == 1);
    rc |= expect_true("scanner-hop-ends-call", scanned_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |=
        expect_true("scanner-hop-ends-call-explicitly", scanned_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // Scan lists past 26 entries spill into a heap tail; the scanner step has to
    // hop through those via dsd_state_trunk_lcn_slot() exactly like the embedded
    // slots, and wrap from the tail back to the head of the list.
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    rc |= expect_true("scanner-ext-tail-reserve", dsd_state_trunk_lcn_reserve(state, 30) == 0);
    for (int i = 0; i < 30; i++) {
        *dsd_state_trunk_lcn_slot(state, i) = 944012500 + 12500 * (long)i;
    }
    state->lcn_freq_count = 30;
    state->lcn_freq_roll = 26;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_tune_calls = 0;

    noCarrier(opts, state);

    rc |= expect_true("scanner-ext-tail-hop-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944337500U);
    rc |= expect_true("scanner-ext-tail-hop-advanced", state->lcn_freq_roll == 27);

    state->lcn_freq_roll = 29;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-ext-tail-last-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944375000U);
    rc |= expect_true("scanner-ext-tail-last-advanced", state->lcn_freq_roll == 30);

    // roll == count wraps to the head on the next pass (engine.c clamps before reading).
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-ext-tail-wraps-to-head", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944012500U);
    rc |= expect_true("scanner-ext-tail-wrap-advanced", state->lcn_freq_roll == 1);

    // A scan hold pauses the rotation where it stands: the dwell may have expired, but the
    // receiver stays put, the roll does not move, and a call still open on the row is not
    // reported as an explicit release because nothing moved.
    state->lcn_scan_hold = 1;
    state->lcn_freq_roll = 5;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t held_dwell_started = state->last_cc_sync_time;
    g_rtl_tune_calls = 0;
    dsd_call_observation held_call = {0};
    held_call.protocol = DSD_SYNC_NXDN_POS;
    held_call.slot = 0U;
    held_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    held_call.ota_target_id = 7201U;
    held_call.policy_target_id = 7201U;
    held_call.ota_source_id = 8201U;
    held_call.observed_m = 1.0;
    rc |=
        expect_true("scanner-hold-seeds-call", dsd_call_state_observe(state, &held_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    noCarrier(opts, state);
    dsd_call_snapshot held_snapshot;
    rc |= expect_true("scanner-hold-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("scanner-hold-keeps-roll", state->lcn_freq_roll == 5);
    // The dwell timer is left alone under hold: the release command restarts it, so the row gets a
    // full hangtime then rather than hopping the instant the hold comes off.
    rc |= expect_true("scanner-hold-leaves-dwell-alone", state->last_cc_sync_time == held_dwell_started);
    rc |= expect_true("scanner-hold-retains-snapshot", dsd_call_state_get(state, 0U, &held_snapshot) == 1);
    rc |= expect_true("scanner-hold-ends-call-as-sync-loss",
                      held_snapshot.phase == DSD_CALL_PHASE_ENDED
                          && held_snapshot.end_reason != (uint8_t)DSD_CALL_END_EXPLICIT);
    state->lcn_scan_hold = 0;

    // An avoided row is stepped over in the same pass, so the hop lands on the next row the
    // operator still wants and the roll moves past the avoided one.
    rc |= expect_true("scanner-avoid-set", dsd_state_trunk_lcn_avoid_set(state, 6U, 1) == 0);
    state->lcn_freq_roll = 6;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-avoid-skips-row", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944100000U);
    rc |= expect_true("scanner-avoid-advanced-past", state->lcn_freq_roll == 8);

    // Avoided rows at the end of the heap tail wrap the walk back to the head.
    rc |= expect_true("scanner-avoid-tail-27", dsd_state_trunk_lcn_avoid_set(state, 27U, 1) == 0);
    rc |= expect_true("scanner-avoid-tail-28", dsd_state_trunk_lcn_avoid_set(state, 28U, 1) == 0);
    rc |= expect_true("scanner-avoid-tail-29", dsd_state_trunk_lcn_avoid_set(state, 29U, 1) == 0);
    state->lcn_freq_roll = 27;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-avoid-wraps-to-head", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 944012500U);
    rc |= expect_true("scanner-avoid-wrap-advanced", state->lcn_freq_roll == 1);

    // Every row avoided (the UI refuses this, but the flags can be set directly): no hop.
    for (int i = 0; i < 30; i++) {
        rc |= expect_true("scanner-avoid-all-set", dsd_state_trunk_lcn_avoid_set(state, (size_t)i, 1) == 0);
    }
    state->lcn_freq_roll = 3;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("scanner-avoid-all-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("scanner-avoid-all-keeps-roll", state->lcn_freq_roll == 3);
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // Voice-gated scan (issue #381): a synced row with no decoded voice steps as soon as the
    // qualify window lapses even while the hangtime would keep parking there; decoded voice
    // holds the row past qualify; an unsynced visit and a disabled gate keep the hangtime
    // rule; the operator hold wins over all of it. Anchors ride the real monotonic clock
    // with second-scale margins, and every hop here uses a frequency no earlier case tunes
    // so engine.c's tune cache cannot swallow the retune.
    reset_rtl_profile_fakes();
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scan_voice_only = 1;
    opts->scan_voice_qualify_ms = 1000;
    opts->scan_voice_hold_ms = 2000;
    opts->trunk_hangtime = 10;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 947012500;
    state->trunk_lcn_freq[1] = 948012500;
    state->trunk_lcn_freq[2] = 949012500;
    state->trunk_lcn_freq[3] = 950012500;
    state->lcn_freq_count = 4;
    state->lcn_freq_roll = 0;

    // An IDLE row: synced frames, no voice media. The hangtime (10 s, deadline 1 s old)
    // would keep parking here, but the qualify window (1 s, synced 5 s ago) has lapsed.
    double gate_now_m = dsd_time_now_monotonic_s();
    state->last_cc_sync_time = time(NULL) - 1;
    dsd_scan_voice_gate_note_retune(state, gate_now_m - 5.0);
    dsd_scan_voice_gate_tick(opts, state, 1, gate_now_m - 5.0);
    dsd_call_observation gate_idle_call = {0};
    gate_idle_call.protocol = DSD_SYNC_NXDN_POS;
    gate_idle_call.slot = 0U;
    gate_idle_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    gate_idle_call.ota_target_id = 7201U;
    gate_idle_call.policy_target_id = 7201U;
    gate_idle_call.ota_source_id = 8201U;
    gate_idle_call.observed_m = gate_now_m - 5.0;
    rc |= expect_true("voice-gate-idle-seeds-call",
                      dsd_call_state_observe(state, &gate_idle_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    rc |= expect_true("voice-gate-idle-due",
                      dsd_scan_voice_gate_should_step(opts, state, dsd_time_now_monotonic_s()) != 0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    dsd_call_snapshot gate_idle_snapshot;
    rc |= expect_true("voice-gate-idle-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 947012500U);
    rc |= expect_true("voice-gate-idle-advanced", state->lcn_freq_roll == 1);
    rc |= expect_true("voice-gate-idle-retains-snapshot", dsd_call_state_get(state, 0U, &gate_idle_snapshot) == 1);
    rc |= expect_true("voice-gate-idle-ends-call", gate_idle_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("voice-gate-idle-ends-call-explicitly",
                      gate_idle_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);
    gate_now_m = dsd_time_now_monotonic_s();
    rc |= expect_true("voice-gate-idle-resets-sync", state->scan_voice_gate_sync_m < 0.0);
    rc |= expect_true("voice-gate-idle-resets-voice", state->scan_voice_gate_voice_m < 0.0);
    rc |= expect_true("voice-gate-idle-restamps-arrive", fabs(state->scan_voice_gate_arrive_m - gate_now_m) < 5.0);

    // A terminator-ended call discovered on an unsynced tick still obeys a custom five-second
    // voice hold even though the legacy hangtime (1 s, deadline 11 s old) is due. This is the
    // post-dispatch shape of DMR BS: one processFrame() consumes the voice and terminator before
    // the engine gets its first gate tick, so sync_m is not available to select the gate.
    opts->trunk_hangtime = 1;
    opts->scan_voice_hold_ms = 5000;
    state->lcn_freq_roll = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    gate_now_m = dsd_time_now_monotonic_s();
    dsd_scan_voice_gate_note_retune(state, gate_now_m - 3.0);
    dsd_call_observation gate_voice_call = {0};
    gate_voice_call.protocol = DSD_SYNC_NXDN_POS;
    gate_voice_call.slot = 0U;
    gate_voice_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    gate_voice_call.ota_target_id = 7202U;
    gate_voice_call.policy_target_id = 7202U;
    gate_voice_call.ota_source_id = 8202U;
    gate_voice_call.observed_m = gate_now_m - 2.8;
    rc |= expect_true("voice-gate-voice-seeds-call",
                      dsd_call_state_observe(state, &gate_voice_call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    rc |= expect_true("voice-gate-voice-seeds-media",
                      dsd_call_state_update_media(state, 0U, 1, gate_now_m - 2.8) == 1
                          && dsd_call_state_update_media(state, 0U, 1, gate_now_m - 2.6) == 1);
    rc |= expect_true("voice-gate-voice-terminates-before-tick",
                      dsd_call_state_end_ex(state, 0U, gate_now_m - 2.5, DSD_CALL_END_TERMINATOR) == 1);
    dsd_scan_voice_gate_tick(opts, state, 0, gate_now_m);
    rc |= expect_true("voice-gate-tail-phase", state->scan_voice_gate_phase == (uint8_t)DSD_SCAN_VOICE_GATE_TAIL);
    rc |= expect_true("voice-gate-sync-remains-unset", state->scan_voice_gate_sync_m < 0.0);
    rc |= expect_true("voice-gate-retained-media-arms", state->scan_voice_gate_voice_m > 0.0);
    rc |= expect_true("voice-gate-voice-holds",
                      dsd_scan_voice_gate_should_step(opts, state, dsd_time_now_monotonic_s()) == 0);
    const time_t voice_hold_deadline = state->last_cc_sync_time;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-voice-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("voice-gate-voice-keeps-roll", state->lcn_freq_roll == 1);
    rc |= expect_true("voice-gate-voice-keeps-deadline", state->last_cc_sync_time == voice_hold_deadline);
    opts->scan_voice_hold_ms = 2000;

    // Gate off: the retained gate anchor above is ignored and the hangtime rule decides alone.
    opts->scan_voice_only = 0;
    state->last_cc_sync_time = time(NULL);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-off-fresh-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("voice-gate-off-fresh-keeps-roll", state->lcn_freq_roll == 1);
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-off-stale-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 948012500U);
    rc |= expect_true("voice-gate-off-stale-advanced", state->lcn_freq_roll == 2);

    // Gate on but never synced this visit: the gate abstains and the hangtime rule steps.
    // The voice epoch above is retired first so its media cannot hold the visit instead.
    opts->scan_voice_only = 1;
    gate_now_m = dsd_time_now_monotonic_s();
    // The voice epoch above is already ended by the earlier noCarrier passes; retiring it
    // again is a no-op so its media cannot hold the visit instead.
    (void)dsd_call_state_end_ex(state, 0U, gate_now_m, DSD_CALL_END_EXPLICIT);
    state->lcn_freq_roll = 2;

    state->last_cc_sync_time = time(NULL) - 11;
    gate_now_m = dsd_time_now_monotonic_s();
    dsd_scan_voice_gate_note_retune(state, gate_now_m);
    dsd_scan_voice_gate_tick(opts, state, 0, gate_now_m);
    rc |= expect_true("voice-gate-unsynced-abstains",
                      dsd_scan_voice_gate_should_step(opts, state, dsd_time_now_monotonic_s()) == 0);
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-unsynced-retuned", g_rtl_tune_calls > 0 && g_rtl_tune_freq == 949012500U);
    rc |= expect_true("voice-gate-unsynced-advanced", state->lcn_freq_roll == 3);

    // The operator hold wins: qualify lapsed, hangtime due, but the row stays put.
    state->lcn_freq_roll = 3;
    state->last_cc_sync_time = time(NULL) - 11;
    state->lcn_scan_hold = 1;
    gate_now_m = dsd_time_now_monotonic_s();
    dsd_scan_voice_gate_note_retune(state, gate_now_m - 5.0);
    dsd_scan_voice_gate_tick(opts, state, 1, gate_now_m - 5.0);
    rc |= expect_true("voice-gate-hold-due",
                      dsd_scan_voice_gate_should_step(opts, state, dsd_time_now_monotonic_s()) == 0);
    const time_t operator_hold_deadline = state->last_cc_sync_time;
    g_rtl_tune_calls = 0;
    noCarrier(opts, state);
    rc |= expect_true("voice-gate-hold-no-retune", g_rtl_tune_calls == 0);
    rc |= expect_true("voice-gate-hold-keeps-roll", state->lcn_freq_roll == 3);
    rc |= expect_true("voice-gate-hold-keeps-deadline", state->last_cc_sync_time == operator_hold_deadline);
    state->lcn_scan_hold = 0;

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // With both backends configured the rigctl leg runs first. If it lands and the RTL leg then
    // fails, the scan step is abandoned -- but the radio has already moved off the frequency the
    // open call was decoded from. The end must still be EXPLICIT: reporting sync loss would leave
    // the call reacquirable by whatever the new frequency happens to carry.
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 0;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    // Frequencies no earlier case in this file uses: engine.c caches the last rigctl and RTL tune
    // in file statics that outlive free_test_runtime(), and a repeat would be skipped as a no-op.
    state->trunk_lcn_freq[0] = 942012500;
    state->trunk_lcn_freq[1] = 943012500;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t partial_hop_scan_time = state->last_cc_sync_time;
    g_rigctl_setfreq_ok = 1;
    g_rigctl_setfreq_calls = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    g_rtl_tune_calls = 0;

    dsd_call_observation partial_hop_call = {0};
    partial_hop_call.protocol = DSD_SYNC_NXDN_POS;
    partial_hop_call.slot = 0U;
    partial_hop_call.kind = DSD_CALL_KIND_GROUP_VOICE;
    partial_hop_call.ota_target_id = 7201U;
    partial_hop_call.policy_target_id = 7201U;
    partial_hop_call.ota_source_id = 8201U;
    partial_hop_call.observed_m = 1.0;
    rc |= expect_true("partial-hop-seeds-call",
                      dsd_call_state_observe(state, &partial_hop_call, DSD_CALL_BOUNDARY_BEGIN) == 1);

    noCarrier(opts, state);

    dsd_call_snapshot partial_hop_snapshot;
    rc |= expect_true("partial-hop-moved-rigctl", g_rigctl_setfreq_calls > 0 && g_rigctl_setfreq_freq == 942012500);
    rc |= expect_true("partial-hop-rtl-failed", g_rtl_tune_calls > 0);
    // The step itself is still abandoned: the candidate and the deadline are untouched so the next
    // pass retries this entry rather than skipping it.
    rc |= expect_true("partial-hop-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("partial-hop-keeps-deadline", state->last_cc_sync_time == partial_hop_scan_time);
    rc |= expect_true("partial-hop-retains-snapshot", dsd_call_state_get(state, 0U, &partial_hop_snapshot) == 1);
    rc |= expect_true("partial-hop-ends-call", partial_hop_snapshot.phase == DSD_CALL_PHASE_ENDED);
    rc |= expect_true("partial-hop-ends-call-explicitly",
                      partial_hop_snapshot.end_reason == (uint8_t)DSD_CALL_END_EXPLICIT);
    g_rigctl_setfreq_ok = 0;
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
#endif

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->scanner_mode = 1;
    state->trunk_lcn_freq[0] = 938012500;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t missing_backend_scan_time = state->last_cc_sync_time;

    noCarrier(opts, state);

    rc |= expect_true("scanner-missing-backend-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("scanner-missing-backend-keeps-deadline", state->last_cc_sync_time == missing_backend_scan_time);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->scanner_mode = 1;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    state->trunk_lcn_freq[0] = 938012500;
    state->lcn_freq_count = 1;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    const time_t failed_rigctl_scan_time = state->last_cc_sync_time;

    noCarrier(opts, state);

    rc |= expect_true("rigctl-scanner-failure-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("rigctl-scanner-failure-keeps-deadline", state->last_cc_sync_time == failed_rigctl_scan_time);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->trunk_cc_freq = 939012500;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 939512500;
    state->trunk_vc_freq[1] = 939512500;

    noCarrier(opts, state);

    rc |= expect_true("rigctl-direct-failure-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("rigctl-direct-failure-preserves-vc",
                      state->trunk_vc_freq[0] == 939512500 && state->trunk_vc_freq[1] == 939512500);
    rc |= expect_true("rigctl-direct-failure-preserves-cc", state->trunk_cc_freq == 939012500);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // noCarrier can run from control pumping inside guarded frame dispatch.
    // Guard contention must defer the P25 return without blocking or clearing
    // the voice state, then allow the next main-loop pass to complete it.
    opts->trunk_enable = 1;
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
        rc |= expect_true("p25-nocarrier-contention-preserves-state", opts->trunk_is_tuned == 1
                                                                          && state->p25_vc_freq[0] == 771056250
                                                                          && state->p25_vc_freq[1] == 771056250);
        p25_sm_tick_guard_leave();
    }

    noCarrier(opts, state);
    rc |= expect_true("p25-nocarrier-contention-retries", opts->trunk_is_tuned == 0 && state->p25_vc_freq[0] == 0);

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
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->trunk_cc_freq = 940012500;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 940512500;
    state->trunk_vc_freq[1] = 940512500;

    noCarrier(opts, state);

    rc |= expect_true("rtl-direct-missing-context-does-not-tune", g_rtl_tune_calls == 0);
    rc |= expect_true("rtl-direct-missing-context-preserves-tuned", opts->trunk_is_tuned == 1);
    rc |= expect_true("rtl-direct-missing-context-preserves-vc",
                      state->trunk_vc_freq[0] == 940512500 && state->trunk_vc_freq[1] == 940512500);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_cc_freq = 941012500;
    state->lastsynctype = DSD_SYNC_NXDN_POS;
    state->last_cc_sync_time = time(NULL) - 11;
    state->last_vc_sync_time = time(NULL) - 11;
    state->trunk_vc_freq[0] = 941512500;
    state->trunk_vc_freq[1] = 941512500;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;

    noCarrier(opts, state);

    rc |= expect_true("rtl-direct-failure-attempts", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 941012500U);
    rc |= expect_true("rtl-direct-failure-preserves-state", opts->trunk_is_tuned == 1
                                                                && state->trunk_vc_freq[0] == 941512500
                                                                && state->trunk_vc_freq[1] == 941512500);

    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    noCarrier(opts, state);

    rc |= expect_true("rtl-direct-timeout-retries-uncached", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 941012500U);
    rc |= expect_true("rtl-direct-timeout-preserves-state", opts->trunk_is_tuned == 1
                                                                && state->trunk_vc_freq[0] == 941512500
                                                                && state->trunk_vc_freq[1] == 941512500);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);

    rc |= expect_true("rtl-direct-success-retries-uncached", g_rtl_tune_calls == 3 && g_rtl_tune_freq == 941012500U);
    rc |= expect_true("rtl-direct-success-clears-voice-state",
                      opts->trunk_is_tuned == 0 && state->trunk_vc_freq[0] == 0 && state->trunk_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    g_rtl_output_rate = 96000;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
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
    state->p25_p1_validated_rf_mod = 1;
    state->p25_p1_nid_evidence = 1;
    state->p25_p1_nid_evidence_symbolcnt = 4321U;

    noCarrier(opts, state);

    rc |= expect_true("p25-rtl-nocarrier-cc-retune", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    /* Issue #423: which modulation carried the last validated P25p1 frame is system knowledge,
     * not acquisition state, so losing the carrier must not erase it -- otherwise every fade
     * costs an LSM control channel the chain it had already earned. */
    rc |= expect_true("p25-rtl-nocarrier-keeps-learned-modulation", state->p25_p1_validated_rf_mod == 1);
    /* Issue #400: what a decoded NID vouches for is the other kind of thing. The benefit of the
     * doubt it lends the failures around it is about a transmission in progress, so it cannot
     * outlast the carrier -- the next one proves itself again. */
    rc |= expect_true("p25-rtl-nocarrier-clears-nid-evidence",
                      state->p25_p1_nid_evidence == 0 && state->p25_p1_nid_evidence_symbolcnt == 0U);
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
    rc |= expect_true("p25-rtl-nocarrier-clear-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-clear-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0);
    rc |= expect_true("p25-rtl-nocarrier-uses-return-grace",
                      p25_sm_get_ctx()->cc_acquisition_origin == P25_SM_CC_ACQUISITION_RETURN);

    // A controller wait timeout remains correlated until the controller
    // publishes the physical retune result.
    reset_rtl_profile_fakes();
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
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

    rc |= expect_true("p25-rtl-nocarrier-timeout-accepted", g_rtl_tune_calls == 1 && g_rtl_tune_freq == 769868750U);
    const uint64_t pending_cc_request_id = pending_ctx->cc_tune_request_id;
    rc |= expect_true(
        "p25-rtl-nocarrier-timeout-waits-for-completion",
        pending_ctx->cc_tune_pending == 1 && pending_ctx->t_cc_tune_m == 0.0 && pending_cc_request_id != 0U
            && pending_ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_RETURN
            && dsd_trunk_tuning_request_status(pending_cc_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    rc |= expect_true("p25-rtl-nocarrier-timeout-serializes-tune", g_p25_tick_guard_held_during_tune == 1);
    int guard_released = p25_sm_tick_guard_try_enter();
    rc |= expect_true("p25-rtl-nocarrier-timeout-releases-guard", guard_released == 1);
    if (guard_released) {
        p25_sm_tick_guard_leave();
    }
    rc |= expect_true("p25-rtl-nocarrier-timeout-keeps-frame-gate-closed",
                      !dsd_trunk_tuning_frame_is_current(dsd_trunk_tuning_generation()));
    dsd_trunk_tuning_request_publish(pending_cc_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    p25_sm_tick_ctx(pending_ctx, opts, state);
    rc |= expect_true("p25-rtl-nocarrier-completion-starts-acquisition",
                      pending_ctx->cc_tune_pending == 0 && pending_ctx->t_cc_tune_m > 0.0
                          && pending_ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_RETURN);
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

    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-failure-retries-uncached", g_rtl_tune_calls == 2 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-failure-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("rtl-scanner-failure-keeps-deadline", state->last_cc_sync_time == deferred_scan_time);

    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-timeout-retries-uncached", g_rtl_tune_calls == 3 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-timeout-keeps-candidate", state->lcn_freq_roll == 0);
    rc |= expect_true("rtl-scanner-timeout-keeps-deadline", state->last_cc_sync_time == deferred_scan_time);

    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    noCarrier(opts, state);

    rc |= expect_true("rtl-scanner-deferred-retries", g_rtl_tune_calls == 4 && g_rtl_tune_freq == 773456250U);
    rc |= expect_true("rtl-scanner-retry-advances-candidate", state->lcn_freq_roll == 1);
    rc |= expect_true("rtl-scanner-retry-restarts-deadline", state->last_cc_sync_time > deferred_scan_time);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->scanner_mode = 1;
    opts->trunk_enable = 1;
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
    opts->trunk_enable = 1;
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
    opts->trunk_enable = 1;
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
    rc |= expect_true("p25-rtl-nocarrier-mixed-no-identity-clears-tuned", opts->trunk_is_tuned == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
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
    opts->trunk_enable = 1;
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
    rc |= expect_true("p25-rtl-nocarrier-deferred-preserves-tuned", opts->trunk_is_tuned == 1);
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
    rc |= expect_true("p25-rtl-nocarrier-retry-clears-tuned", opts->trunk_is_tuned == 0);
    rc |= expect_true("p25-rtl-nocarrier-retry-clears-vc", state->p25_vc_freq[0] == 0 && state->p25_vc_freq[1] == 0
                                                               && state->trunk_vc_freq[0] == 0
                                                               && state->trunk_vc_freq[1] == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
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
    rc |= expect_true("p25-rtl-nocarrier-failed-clears-tuned", opts->trunk_is_tuned == 0);
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
    opts->trunk_enable = 1;
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
    opts->trunk_enable = 1;
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
    opts->trunk_enable = 1;
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
    rc |= expect_true("generic-rtl-repeated-nocarrier-clears-tuned", opts->trunk_is_tuned == 0);

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    reset_rtl_profile_fakes();
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
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
    opts->trunk_enable = 1;
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

    // An unresolved generic tune keeps dispatch gated. Once it fails, a
    // controller timeout remains pending until its exact completion arrives.
    reset_rtl_profile_fakes();
    g_rtl_tune_result = RTL_STREAM_TUNE_TIMEOUT;
    g_rtl_channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_enable = 1;
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

    rc |= expect_true("generic-rtl-recovery-pending-suppresses-retune",
                      failed_request_id != 0U && g_rtl_tune_calls == 0 && opts->trunk_is_tuned == 1
                          && dsd_trunk_tuning_request_status(failed_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    dsd_trunk_tuning_request_publish(failed_request_id, DSD_TRUNK_TUNE_RESULT_FAILED);
    rc |= expect_true("generic-rtl-recovery-seeds-failed-gate",
                      dsd_trunk_tuning_request_status(failed_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_FAILED
                          && !dsd_trunk_tuning_frame_is_current(failed_generation));

    noCarrier(opts, state);

    const uint64_t recovery_generation = dsd_trunk_tuning_generation();
    const uint64_t recovery_request_id = dsd_trunk_tuning_pending_request();
    rc |=
        expect_true("generic-rtl-recovery-timeout-remains-pending",
                    g_rtl_tune_calls == 1 && g_rtl_tune_freq == 936000000U && recovery_generation == failed_generation
                        && recovery_request_id > failed_request_id
                        && dsd_trunk_tuning_request_status(recovery_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING);
    rc |= expect_true("generic-rtl-recovery-timeout-stages-state",
                      opts->trunk_is_tuned == 1 && state->trunk_vc_freq[0] == 0 && state->trunk_cc_freq == 936000000
                          && state->p25_cc_freq == 0);
    rc |= expect_true("generic-rtl-recovery-timeout-keeps-gate-closed",
                      !dsd_trunk_tuning_frame_is_current(recovery_generation));
    rc |=
        expect_true("generic-rtl-recovery-preserves-profile", g_rtl_channel_profile == RTL_STREAM_CHANNEL_PROFILE_WIDE);

    dsd_trunk_tuning_request_publish(recovery_request_id, DSD_TRUNK_TUNE_RESULT_OK);
    noCarrier(opts, state);
    const uint64_t completed_recovery_generation = dsd_trunk_tuning_generation();
    rc |= expect_true("generic-rtl-recovery-completion-commits-state",
                      opts->trunk_is_tuned == 0 && state->trunk_vc_freq[0] == 0 && state->trunk_cc_freq == 936000000
                          && state->p25_cc_freq == 0);
    rc |=
        expect_true("generic-rtl-recovery-completion-opens-gate",
                    completed_recovery_generation == failed_generation + 1U && dsd_trunk_tuning_pending_request() == 0U
                        && dsd_trunk_tuning_frame_is_current(completed_recovery_generation));

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

    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // Key-file sets arm keyloader and are transmission-scoped. Their canonical
    // AES bytes must be erased with the scalar aliases on carrier loss.
    state->keyloader = 1;
    state->K = 7ULL;
    state->K1 = state->H = 0x0011223344556677ULL;
    state->A1[0] = state->A1[1] = state->K1;
    state->aes_key_loaded[0] = state->aes_key_loaded[1] = 1;
    state->aes_key_segments[0] = state->aes_key_segments[1] = 2U;
    DSD_MEMSET(state->aes_key, 0xA5, sizeof(state->aes_key));
    noCarrier(opts, state);
    int aes_cleared = 1;
    for (size_t i = 0U; i < sizeof(state->aes_key); i++) {
        if (state->aes_key[i] != 0U) {
            aes_cleared = 0;
            break;
        }
    }
    rc |= expect_true("keyloader-carrier-reset-clears-aes-bytes", aes_cleared && state->K == 0ULL && state->K1 == 0ULL
                                                                      && state->H == 0ULL
                                                                      && state->aes_key_loaded[0] == 0);

    // Embedded direct keys deliberately use keyloader=0, matching -b/-H:
    // noCarrier preserves them and must not alter the operator's mute policy.
    dsd_key_set direct;
    DSD_MEMSET(&direct, 0, sizeof(direct));
    if (dsd_key_set_load_direct(&direct, "00112233445566778899AABBCCDDEEFF", "7") != DSD_KEY_DIRECT_OK) {
        free_test_runtime(opts, state);
        return 1;
    }
    dsd_key_set_install(state, &direct);
    dsd_key_set_free(&direct);
    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;
    noCarrier(opts, state);
    rc |= expect_true("direct-key-carrier-reset-persists", state->keyloader == 0 && state->K == 7ULL
                                                               && state->K1 == 0x0011223344556677ULL
                                                               && state->aes_key[15] == 0xFFU);
    rc |= expect_true("direct-key-carrier-reset-preserves-mute-policy",
                      opts->dmr_mute_encL == 1 && opts->dmr_mute_encR == 1);

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

#if defined(DSD_NEO_TEST_RTL_WRAP) && DSD_NEO_TEST_RTL_WRAP
    free_test_runtime(opts, state);
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    // Per-row keys under -Y. A hop onto a keyed row installs its set, a hop
    // back onto an unkeyed row restores the globals, a zero-frequency row
    // parks in place keeping the previous set, and a failed tune leg swaps
    // nothing.
    opts->scanner_mode = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->trunk_hangtime = 1;
    state->rtl_ctx = (RtlSdrContext*)state;
    state->trunk_lcn_freq[0] = 940012500;
    state->trunk_lcn_freq[1] = 941012500;
    state->trunk_lcn_freq[2] = 0;
    state->lcn_freq_count = 3;
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    state->keyloader = 0;
    state->K = 0xBEEFULL;
    state->rkey_array[3] = 111ULL;
    state->rkey_array_loaded[3] = 1U;
    {
        dsd_key_set ks;
        DSD_MEMSET(&ks, 0, sizeof(ks));
        ks.entries = (dsd_key_set_entry*)calloc(1U, sizeof(*ks.entries));
        if (ks.entries == NULL) {
            free_test_runtime(opts, state);
            return 1;
        }
        ks.count = 1U;
        ks.present = 1;
        ks.keyloader = 1;
        ks.entries[0].index = 9U;
        ks.entries[0].value = 999ULL;
        ks.entries[0].loaded = 1U;
        if (dsd_state_trunk_lcn_keys_set(state, 0U, &ks) != 0) {
            free_test_runtime(opts, state);
            return 1;
        }
    }
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
    g_rtl_tune_calls = 0;

    noCarrier(opts, state);
    rc |= expect_true("rowkey-hop-installs", state->rkey_array[9] == 999ULL && state->keyloader == 1 && state->K == 0ULL
                                                 && state->lcn_freq_roll == 1);

    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rowkey-hop-restores", state->rkey_array[9] == 0ULL && state->keyloader == 0
                                                 && state->K == 0xBEEFULL && state->rkey_array[3] == 111ULL
                                                 && state->lcn_freq_roll == 2);

    // Re-park on the keyed row, then step onto the zero-frequency row: it parks
    // in place, so the installed set stays.
    state->lcn_freq_roll = 0;
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rowkey-rehop-installs", state->rkey_array[9] == 999ULL && state->scan_keys_active_set == 1);
    state->lcn_freq_roll = 2;
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
    rc |= expect_true("rowkey-zero-row-keeps-set",
                      state->rkey_array[9] == 999ULL && state->scan_keys_active_set == 1 && state->lcn_freq_roll == 3);

    // A failed tune leg leaves the receiver (and the installed set) where it was.
    state->lcn_freq_roll = 1;
    state->last_cc_sync_time = time(NULL) - 11;
    g_rtl_tune_result = RTL_STREAM_TUNE_FAILED;
    noCarrier(opts, state);
    rc |= expect_true("rowkey-failed-tune-keeps-roll", state->lcn_freq_roll == 1);
    rc |=
        expect_true("rowkey-failed-tune-keeps-set", state->rkey_array[9] == 999ULL && state->scan_keys_active_set == 1);
    g_rtl_tune_result = RTL_STREAM_TUNE_OK;
#endif

    dsd_rtl_stream_metrics_hooks_set(NULL);
#endif

    free_test_runtime(opts, state);

#if defined(USE_RADIO) && defined(DSD_NEO_TEST_RTL_WRAP)
    rc |= test_typed_scan_tune_boundaries();
#endif

    if (rc == 0) {
        printf("ENGINE_NO_CARRIER_RESET: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
