// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/protocol/dmr/dmr_block.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/trunk_tuning_hooks.h"

static int DSD_ATTR_USED
dsd_engine_current_demod_rate(const dsd_opts* opts, const dsd_state* state) {
    int demod_rate = dsd_opts_current_input_timing_rate(opts);
#ifdef USE_RADIO
    if (opts && opts->audio_in_type == AUDIO_IN_RTL && state && state->rtl_ctx) {
        int rtl_rate = (int)rtl_stream_output_rate(state->rtl_ctx);
        if (rtl_rate > 0) {
            demod_rate = rtl_rate;
        }
    }
#else
    (void)state;
#endif
    return demod_rate;
}

static long int DSD_ATTR_USED
dsd_engine_resolve_cc_freq(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    return (state->p25_cc_freq != 0) ? state->p25_cc_freq : state->trunk_cc_freq;
}

static int DSD_ATTR_USED
dsd_engine_compute_cc_sps(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || state->p25_cc_freq == 0) {
        return 0;
    }
    const int sym_rate = (state->p25_cc_is_tdma == 1) ? 6000 : 4800;
    return dsd_opts_compute_sps_rate(opts, sym_rate, dsd_engine_current_demod_rate(opts, state));
}

static void DSD_ATTR_USED
dsd_engine_select_p25_sps_profile(dsd_state* state, int is_tdma) {
    if (!state) {
        return;
    }
    state->sps_hunt_idx = is_tdma ? DSD_FRAME_SYNC_SPS_PROFILE_6000_4 : DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    state->sps_hunt_counter = 0;
}

static int
dsd_engine_is_p25_profile_retune(const dsd_opts* opts, const dsd_state* state, int ted_sps) {
    if (!opts || !state || opts->trunk_enable != 1 || ted_sps <= 0) {
        return 0;
    }
    if (dsd_engine_trunk_scan_active_p25_ctx() != NULL) {
        return 1;
    }
    if (state->rf_mod == 2) {
        return 0;
    }
    if (DSD_SYNC_IS_DMR(state->synctype) || DSD_SYNC_IS_DMR(state->lastsynctype) || DSD_SYNC_IS_NXDN(state->synctype)
        || DSD_SYNC_IS_NXDN(state->lastsynctype) || DSD_SYNC_IS_EDACS(state->synctype)
        || DSD_SYNC_IS_EDACS(state->lastsynctype)) {
        return 0;
    }
    return 1;
}

static void DSD_ATTR_USED
dsd_engine_apply_cc_symbol_timing(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || state->p25_cc_freq == 0) {
        return;
    }
    const int sym_rate = (state->p25_cc_is_tdma == 1) ? 6000 : 4800;
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, sym_rate, dsd_engine_current_demod_rate(opts, state));
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    state->rf_mod = (state->p25_cc_is_tdma == 1) ? 1 : ((opts->mod_qpsk == 1) ? 1 : 0);
    dsd_engine_select_p25_sps_profile(state, state->p25_cc_is_tdma == 1);
}

static void DSD_ATTR_USED
dsd_engine_reset_return_to_cc_state(dsd_opts* opts, dsd_state* state) {
    const double ended_m = dsd_time_now_monotonic_s();
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        if (dsd_call_state_end(state, (uint8_t)slot, ended_m) > 0) {
            dsd_event_sync_slot(opts, state, (uint8_t)slot);
        }
    }
    (void)dsd_recent_activity_clear_all(state);
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));

    dmr_reset_blocks(opts, state);

    state->payload_algid = 0;
    state->payload_algidR = 0;
    state->payload_keyid = 0;
    state->payload_keyidR = 0;
    state->payload_mi = 0;
    state->payload_miR = 0;
    state->payload_miP = 0;
    state->payload_miN = 0;
    state->p25_vc_freq[0] = 0;
    state->p25_vc_freq[1] = 0;
    state->trunk_vc_freq[0] = 0;
    state->trunk_vc_freq[1] = 0;
    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    state->p25_crypto_state[0] = DSD_P25_CRYPTO_UNKNOWN;
    state->p25_crypto_state[1] = DSD_P25_CRYPTO_UNKNOWN;
    DSD_MEMSET(&state->p25_p1_crypto_conflict, 0, sizeof(state->p25_p1_crypto_conflict));
    DSD_MEMSET(state->p25_p2_rekey, 0, sizeof(state->p25_p2_rekey));
    state->p25_p2_active_slot = -1;
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    opts->trunk_is_tuned = 0;
}

#ifdef USE_RADIO
typedef struct {
    int active;
    int rf_mod;
    int p25_vc_cqpsk_override;
    int rtl_cqpsk_enable;
    int rtl_symbol_rate_hz;
    int rtl_symbol_levels;
    int rtl_channel_profile;
    int rtl_ted_sps;
    int rtl_ted_sps_override;
} dsd_engine_rtl_profile_snapshot;

static rtl_stream_retune_gain_profile
dsd_engine_trunk_scan_gain_profile(const dsd_opts* opts, const dsd_state* state) {
    rtl_stream_retune_gain_profile profile;
    DSD_MEMSET(&profile, 0, sizeof(profile));
    if (!opts || !state || opts->trunk_scan_enabled != 1 || dsd_engine_trunk_scan_target_count(state) == 0) {
        return profile;
    }

    profile.tuner_gain_is_set = 1;
    if (opts->rtl_gain_value > 0) {
        profile.tuner_gain_tenth_db = opts->rtl_gain_value * 10;
        profile.tuner_gain_is_auto = 0;
        profile.tuner_autogain_is_set = 1;
        profile.tuner_autogain_on = 0;
        return profile;
    }

    profile.tuner_gain_tenth_db = 0;
    profile.tuner_gain_is_auto = 1;
    int saved_autogain = 0;
    if (dsd_engine_trunk_scan_saved_tuner_autogain(state, &saved_autogain)) {
        profile.tuner_autogain_is_set = 1;
        profile.tuner_autogain_on = saved_autogain ? 1 : 0;
    }
    return profile;
}

static void
dsd_engine_prepare_retune_profile_for_target(const dsd_opts* opts, const dsd_state* state, uint32_t target_freq_hz,
                                             int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                             int ted_sps, int persist_ted_override) {
    rtl_stream_retune_gain_profile gain_profile = dsd_engine_trunk_scan_gain_profile(opts, state);
    rtl_stream_prepare_retune_profile_for_target_with_gain(target_freq_hz, cqpsk_enable, symbol_rate_hz, levels,
                                                           channel_profile, ted_sps, persist_ted_override,
                                                           &gain_profile);
}

static void
dsd_engine_rtl_profile_snapshot_capture(const dsd_opts* opts, const dsd_state* state,
                                        dsd_engine_rtl_profile_snapshot* snapshot) {
    if (!snapshot) {
        return;
    }
    DSD_MEMSET(snapshot, 0, sizeof(*snapshot));
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL) {
        return;
    }
    snapshot->active = 1;
    snapshot->rf_mod = state->rf_mod;
    snapshot->p25_vc_cqpsk_override = state->p25_vc_cqpsk_override;
    (void)rtl_stream_get_cqpsk_status(&snapshot->rtl_cqpsk_enable, NULL);
    (void)rtl_stream_get_symbol_profile_full(&snapshot->rtl_symbol_rate_hz, &snapshot->rtl_symbol_levels,
                                             &snapshot->rtl_channel_profile);
    snapshot->rtl_ted_sps = rtl_stream_get_ted_sps();
    snapshot->rtl_ted_sps_override = rtl_stream_get_ted_sps_override();
}

static void
dsd_engine_rtl_profile_snapshot_restore(dsd_state* state, const dsd_engine_rtl_profile_snapshot* snapshot) {
    if (!state || !snapshot || !snapshot->active) {
        return;
    }
    rtl_stream_clear_pending_retune_profile();
    state->rf_mod = snapshot->rf_mod;
    state->p25_vc_cqpsk_override = snapshot->p25_vc_cqpsk_override;
    rtl_stream_toggle_cqpsk(snapshot->rtl_cqpsk_enable);
    if (snapshot->rtl_ted_sps > 0) {
        rtl_stream_set_ted_sps_no_override(snapshot->rtl_ted_sps);
    }
    if (snapshot->rtl_ted_sps_override > 0) {
        rtl_stream_set_ted_sps(snapshot->rtl_ted_sps_override);
    } else {
        rtl_stream_clear_ted_sps_override();
    }
    if (snapshot->rtl_symbol_rate_hz > 0 && (snapshot->rtl_symbol_levels == 2 || snapshot->rtl_symbol_levels == 4)) {
        (void)rtl_stream_set_symbol_profile(snapshot->rtl_symbol_rate_hz, snapshot->rtl_symbol_levels,
                                            snapshot->rtl_channel_profile);
    }
}

static void
dsd_engine_prepare_p25_cc_rtl_chain(const dsd_opts* opts, dsd_state* state, long int target_freq_hz, int ted_sps) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init();
        cfg = dsd_neo_get_config();
    }

    const int default_cqpsk = (state->p25_cc_is_tdma == 1 || opts->mod_qpsk == 1) ? 1 : 0;
    int trunk_scan_cqpsk_request = 0;
    const int has_trunk_scan_cqpsk_request =
        dsd_engine_trunk_scan_active_p25_cqpsk_request(state, &trunk_scan_cqpsk_request);
    const int want_cqpsk = has_trunk_scan_cqpsk_request ? trunk_scan_cqpsk_request : default_cqpsk;
    state->rf_mod = want_cqpsk ? 1 : 0;
    const int cqpsk_request = (has_trunk_scan_cqpsk_request || !cfg || !cfg->cqpsk_is_set) ? want_cqpsk : -1;
    const int sym_rate = (state->p25_cc_is_tdma == 1) ? 6000 : 4800;
    const int profile = want_cqpsk ? RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK : RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    dsd_engine_prepare_retune_profile_for_target(opts, state, (uint32_t)target_freq_hz, cqpsk_request, sym_rate, 4,
                                                 profile, ted_sps, 0);
}

static void
dsd_engine_prepare_dmr_cc_rtl_chain(const dsd_opts* opts, const dsd_state* state, long int target_freq_hz,
                                    int ted_sps) {
    int retune_ted_sps = ted_sps;
    if (state->rtl_ctx) {
        retune_ted_sps = dsd_opts_compute_sps_rate(opts, 4800, (int)rtl_stream_output_rate(state->rtl_ctx));
    }
    dsd_engine_prepare_retune_profile_for_target(opts, state, (uint32_t)target_freq_hz, 0, 4800, 4,
                                                 RTL_STREAM_CHANNEL_PROFILE_12K5, retune_ted_sps, 0);
}

static void
dsd_engine_prepare_current_cc_rtl_chain(const dsd_opts* opts, const dsd_state* state, long int target_freq_hz,
                                        int ted_sps) {
    if (ted_sps > 0) {
        int symbol_rate_hz = 0;
        int levels = 0;
        int channel_profile = RTL_STREAM_CHANNEL_PROFILE_WIDE;
        (void)rtl_stream_get_symbol_profile_full(&symbol_rate_hz, &levels, &channel_profile);
        dsd_engine_prepare_retune_profile_for_target(opts, state, (uint32_t)target_freq_hz, -1, symbol_rate_hz, levels,
                                                     channel_profile, ted_sps, 0);
    } else {
        rtl_stream_clear_pending_retune_profile();
    }
}

static void
dsd_engine_prepare_cc_rtl_chain(const dsd_opts* opts, dsd_state* state, long int target_freq_hz, int ted_sps) {
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL) {
        return;
    }
    if (dsd_engine_is_p25_profile_retune(opts, state, ted_sps)) {
        dsd_engine_prepare_p25_cc_rtl_chain(opts, state, target_freq_hz, ted_sps);
        return;
    }
    if (state->rf_mod == 2 && ted_sps > 0) {
        dsd_engine_prepare_dmr_cc_rtl_chain(opts, state, target_freq_hz, ted_sps);
        return;
    }
    dsd_engine_prepare_current_cc_rtl_chain(opts, state, target_freq_hz, ted_sps);
}
#endif

static void
dsd_engine_maybe_drain_audio(dsd_opts* opts, const dsd_state* state) {
#ifdef USE_RADIO
    if (opts && state && opts->audio_in_type == AUDIO_IN_RTL && opts->use_rigctl != 1 && state->rtl_ctx) {
        return;
    }
#else
    (void)state;
#endif

    /* Avoid blocking drain when invoked from SM tick context. */
    if (!p25_sm_in_tick()) {
        dsd_drain_audio_output(opts);
    }
}

static void
dsd_engine_update_vc_tune_state(dsd_opts* opts, dsd_state* state, long int freq) {
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->trunk_is_tuned = 1;
    /* Reset activity timers so noCarrier() does not immediately force a return
     * to CC before we have a chance to acquire sync on the new VC. */
    state->last_vc_sync_time = time(NULL);
    state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
    state->last_cc_sync_time = state->last_vc_sync_time;
    state->last_cc_sync_time_m = state->last_vc_sync_time_m;
    state->p25_last_vc_tune_time = state->last_vc_sync_time;
    state->p25_last_vc_tune_time_m = state->last_vc_sync_time_m;
}

static dsd_trunk_tune_result
dsd_engine_tune_with_backend(const dsd_opts* opts, dsd_state* state, long int freq, uint64_t request_id) {
    if (opts->use_rigctl == 1) {
        if (opts->setmod_bw != 0) {
            if (!SetModulation(opts->rigctl_sockfd, opts->setmod_bw)) {
                DSD_FPRINTF(stderr, "Rigctl modulation update failed for bandwidth %d.\n", opts->setmod_bw);
            }
        }
        if (!SetFreq(opts->rigctl_sockfd, freq)) {
            DSD_FPRINTF(stderr, "Rigctl frequency update failed for %ld Hz.\n", freq);
            return DSD_TRUNK_TUNE_RESULT_FAILED;
        }
#ifdef USE_RADIO
        if (opts->audio_in_type == AUDIO_IN_RTL) {
            rtl_stream_apply_pending_retune_profile_for_target((uint32_t)freq);
        }
#endif
        return DSD_TRUNK_TUNE_RESULT_OK;
    }
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
#ifdef USE_RADIO
    if (state->rtl_ctx) {
        int rc = request_id != 0U ? rtl_stream_tune_tagged(state->rtl_ctx, (uint32_t)freq, request_id)
                                  : rtl_stream_tune(state->rtl_ctx, (uint32_t)freq);
        if (rc == RTL_STREAM_TUNE_OK) {
            return DSD_TRUNK_TUNE_RESULT_OK;
        }
        if (rc == RTL_STREAM_TUNE_DEFERRED) {
            return DSD_TRUNK_TUNE_RESULT_DEFERRED;
        }
        if (rc == RTL_STREAM_TUNE_TIMEOUT) {
            /* The controller still owns the request after the bounded wait.
             * Tagged calls publish their terminal result asynchronously. */
            return DSD_TRUNK_TUNE_RESULT_PENDING;
        }
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    return DSD_TRUNK_TUNE_RESULT_FAILED;
#else
    (void)state;
    (void)request_id;
    return DSD_TRUNK_TUNE_RESULT_FAILED;
#endif
}

static void
dsd_engine_maybe_reset_p25p2_state(const dsd_opts* opts, const dsd_state* state, int ted_sps) {
    int p25p2_sps = dsd_opts_compute_sps_rate(opts, 6000, dsd_engine_current_demod_rate(opts, state));
    if (ted_sps == p25p2_sps) {
        p25_p2_frame_reset();
    }
}

#ifdef USE_RADIO
static int
dsd_engine_resolve_vc_cqpsk(const dsd_state* state, int rf_mod) {
    int want_cqpsk = (rf_mod == 1) ? 1 : 0;
    if (!state || state->rf_mod != 1 || state->p25_p2_active_slot == -1) {
        return want_cqpsk;
    }
    if (state->p25_vc_cqpsk_override == 0 || state->p25_vc_cqpsk_override == 1) {
        return state->p25_vc_cqpsk_override;
    }
    if (state->p25_vc_cqpsk_pref == 1) {
        return 1;
    }
    return want_cqpsk;
}

static void
dsd_engine_prepare_vc_rtl_chain(const dsd_opts* opts, dsd_state* state, long int target_freq_hz, int ted_sps) {
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return;
    }
    if (!dsd_engine_is_p25_profile_retune(opts, state, ted_sps)) {
        rtl_stream_clear_pending_retune_profile();
        return;
    }

    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init();
        cfg = dsd_neo_get_config();
    }

    int trunk_scan_cqpsk_request = 0;
    const int has_trunk_scan_cqpsk_request =
        dsd_engine_trunk_scan_active_p25_cqpsk_request(state, &trunk_scan_cqpsk_request);
    /* The target request only bypasses the runtime lock; VC modulation still follows the grant/profile state. */
    int want_cqpsk = dsd_engine_resolve_vc_cqpsk(state, state->rf_mod);
    int cqpsk_request = (has_trunk_scan_cqpsk_request || !cfg || !cfg->cqpsk_is_set) ? want_cqpsk : -1;
    const int sym_rate = (state->p25_p2_active_slot != -1) ? 6000 : 4800;
    const int profile = want_cqpsk ? RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK : RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
    dsd_engine_prepare_retune_profile_for_target(opts, state, (uint32_t)target_freq_hz, cqpsk_request, sym_rate, 4,
                                                 profile, ted_sps, ted_sps > 0 ? 1 : 0);

    /* One-shot override is consumed by this tune attempt. */
    state->p25_vc_cqpsk_override = -1;
}

static void
dsd_engine_log_queued_ted_override(const dsd_opts* opts, const dsd_state* state, long int freq, int ted_sps) {
    if (opts->audio_in_type != AUDIO_IN_RTL || ted_sps <= 0) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init();
        cfg = dsd_neo_get_config();
    }
    if (cfg && cfg->debug_cqpsk_enable) {
        DSD_FPRINTF(stderr, "[TUNE] VC freq=%ld Hz rf_mod=%d sps=%d center=%d ted_sps=%d\n", freq, state->rf_mod,
                    state->samplesPerSymbol, state->symbolCenter, ted_sps);
    }
}
#endif

/**
 * @brief Return to the P25 control channel, draining audio and resetting state.
 *
 * Handles both rigctl- and RTL-backed pipelines and refreshes internal timers,
 * last-tuned frequencies, and channel tracking metadata.
 *
 * @param opts Decoder options (tuning targets and sockets).
 * @param state Decoder state to reset.
 */
dsd_trunk_tune_result
dsd_engine_return_to_cc_request(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    dsd_trunk_tune_result tune_result = DSD_TRUNK_TUNE_RESULT_OK;
    if (!opts || !state) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    // Audio drain is handled by dsd_engine_trunk_tune_to_cc_request() before the hardware retune.

    // Tune back to the control channel when known (best-effort). Prefer the
    // explicit CC when available; otherwise fall back to any tracked CC.
    // Avoid sending a zero/unknown frequency to the tuner which can wedge the
    // pipeline at DC and delay CC hunting.
    const long int cc = dsd_engine_resolve_cc_freq(state);
    const int trunk_enabled = (opts->trunk_enable == 1);
    if (trunk_enabled && cc != 0) {
        const int cc_sps = dsd_engine_compute_cc_sps(opts, state);
        tune_result = dsd_engine_trunk_tune_to_cc_request(opts, state, cc, cc_sps, request_id);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            return tune_result;
        }
    }

    dsd_engine_reset_return_to_cc_state(opts, state);

    /* Keep symbol timing aligned with the current control-channel mode. */
    dsd_engine_apply_cc_symbol_timing(opts, state);
    return tune_result;
}

/**
 * @brief Tune to a specific voice/control channel and mark the trunking state.
 *
 * Drains queued audio, tunes via rigctl or RTL stream, and updates timers and
 * bookkeeping fields so the SM can track the new voice channel.
 *
 * @param opts Decoder options with tuning configuration.
 * @param state Decoder state to update.
 * @param freq Target frequency in Hz.
 * @param ted_sps TED samples-per-symbol to set (0 = no override).
 */
dsd_trunk_tune_result
dsd_engine_trunk_tune_to_freq_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps,
                                      uint64_t request_id) {
    dsd_trunk_tune_result result = DSD_TRUNK_TUNE_RESULT_OK;
#ifdef USE_RADIO
    dsd_engine_rtl_profile_snapshot rtl_snapshot;
#endif
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }

    /* Trunking (P25): the SM sets state->rf_mod based on the grant (0=C4FM FDMA,
     * 1=QPSK family for TDMA/CQPSK). For mixed P25P1 C4FM CC + P25P2 TDMA voice,
     * we must also switch the RTL demod chain. Otherwise we end up slicing a
     * C4FM discriminator stream as if it were CQPSK symbols (or vice versa),
     * which manifests as choppy/garbled P25P2 audio on non-simulcast systems.
     *
     * Respect explicit CQPSK forcing via runtime config/env (DSD_NEO_CQPSK). */
#ifdef USE_RADIO
    dsd_engine_rtl_profile_snapshot_capture(opts, state, &rtl_snapshot);
    dsd_engine_prepare_vc_rtl_chain(opts, state, freq, ted_sps);
#endif

    // The Costas/TED state reset must happen AFTER the hardware retune completes,
    // which is handled by demod_reset_on_retune() in the controller thread.
    //
    // Resetting here (before the hardware retune) would make the DSP thread:
    //   1. See the reset Costas state (phase=0, freq=0)
    //   2. Continue processing samples from the OLD frequency
    //   3. Try to lock on the wrong signal, corrupting the loop state
    //   4. When the retune completes, the loop is in a bad state
    //
    // By deferring the reset to the controller thread, the DSP continues with
    // the current (correct for current signal) state until the hardware retune
    // completes, then resets and acquires on the new signal.
    //
    // This matches OP25's architecture where configure_tdma/set_omega are called
    // synchronously with the frequency change, not asynchronously before it.

    // TED samples-per-symbol, CQPSK mode, and symbol profile changes were queued
    // with the RTL controller above. They are applied after the hardware retune
    // completes so old-frequency samples are never processed with new-channel timing.
#ifdef USE_RADIO
    dsd_engine_log_queued_ted_override(opts, state, freq, ted_sps);
#endif

    dsd_engine_maybe_drain_audio(opts, state);
    result = dsd_engine_tune_with_backend(opts, state, freq, request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
#ifdef USE_RADIO
        dsd_engine_rtl_profile_snapshot_restore(state, &rtl_snapshot);
#endif
        return result;
    }

    // Reset modulation auto-detect state (ham tracking, vote counters) after a
    // confirmed tune so the decoder and tuner state do not diverge on failure.
    if (dsd_engine_is_p25_profile_retune(opts, state, ted_sps)) {
        dsd_engine_select_p25_sps_profile(state, state->p25_p2_active_slot != -1);
    }
    dsd_frame_sync_reset_mod_state();

    // Reset P25P2 frame processing state when tuning to a voice channel.
    // This is critical: without resetting the global bit buffers (p2bit, p2xbit),
    // ESS buffers (ess_a, ess_b), and counters (vc_counter, ts_counter), the
    // decoder will process new channel data using stale buffers from the previous
    // channel, causing decode failures. The symptom is: first P25P2 tune works,
    // but subsequent voice channel grants fail to lock with tanking EVM/SNR.
    // Only reset for P25P2 (ted_sps matching the TDMA symbol rate), not P25P1 or other modes.
    dsd_engine_maybe_reset_p25p2_state(opts, state, ted_sps);

    dsd_engine_update_vc_tune_state(opts, state, freq);
    return result;
}

/**
 * @brief Tune to a P25 control channel candidate without marking as tuned.
 *
 * Avoids setting voice-tuned flags and only updates CC tracking metadata so
 * the state machine can continue its hunt.
 *
 * @param opts Decoder options with tuning configuration.
 * @param state Decoder state to update.
 * @param freq Target control channel frequency in Hz.
 * @param ted_sps TED samples-per-symbol to set (0 = no change). Caller should
 *        pass the CC SPS (4 for P25P2 TDMA CC, 5 for P25P1 CC).
 */
dsd_trunk_tune_result
dsd_engine_trunk_tune_to_cc_request(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    dsd_trunk_tune_result result = DSD_TRUNK_TUNE_RESULT_OK;
#ifdef USE_RADIO
    dsd_engine_rtl_profile_snapshot rtl_snapshot;
#endif
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
#ifndef USE_RADIO
    (void)ted_sps;
#else
    dsd_engine_rtl_profile_snapshot_capture(opts, state, &rtl_snapshot);
#endif

    // NOTE: Costas/TED reset is deferred to the controller thread after the hardware
    // retune completes. See dsd_engine_trunk_tune_to_freq_request for the full rationale.

    dsd_engine_maybe_drain_audio(opts, state);
    if (opts->audio_in_type == AUDIO_IN_RTL) {
#ifdef USE_RADIO
        dsd_engine_prepare_cc_rtl_chain(opts, state, freq, ted_sps);
#endif
    }
    result = dsd_engine_tune_with_backend(opts, state, freq, request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
#ifdef USE_RADIO
        dsd_engine_rtl_profile_snapshot_restore(state, &rtl_snapshot);
#endif
        return result;
    }
    // Reset modulation auto-detect state for fresh acquisition after a confirmed tune.
    if (dsd_engine_is_p25_profile_retune(opts, state, ted_sps)) {
        dsd_engine_select_p25_sps_profile(state, state->p25_cc_is_tdma == 1);
    }
    dsd_frame_sync_reset_mod_state();
    // Do not set trunk_is_tuned here; this is a CC hunt action.
    state->trunk_cc_freq = (long int)freq;
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    return result;
}

dsd_trunk_tune_result
dsd_engine_scan_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t* out_request_id) {
    dsd_trunk_tune_result result = DSD_TRUNK_TUNE_RESULT_OK;
    uint64_t tune_request_id = 0U;
#ifdef USE_RADIO
    dsd_engine_rtl_profile_snapshot rtl_snapshot;
#endif
    if (out_request_id) {
        *out_request_id = 0U;
    }
    if (!opts || !state || freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }

    tune_request_id = dsd_trunk_tuning_request_begin();
    if (tune_request_id == 0U) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    if (out_request_id) {
        *out_request_id = tune_request_id;
    }
#ifndef USE_RADIO
    (void)ted_sps;
#else
    dsd_engine_rtl_profile_snapshot_capture(opts, state, &rtl_snapshot);
    if (opts->audio_in_type == AUDIO_IN_RTL && ted_sps > 0) {
        dsd_engine_prepare_cc_rtl_chain(opts, state, freq, ted_sps);
    }
#endif

    dsd_engine_maybe_drain_audio(opts, state);
    result = dsd_engine_tune_with_backend(opts, state, freq, tune_request_id);
    if (!dsd_trunk_tune_result_is_ok(result)) {
#ifdef USE_RADIO
        dsd_engine_rtl_profile_snapshot_restore(state, &rtl_snapshot);
#endif
        dsd_trunk_tuning_request_complete(tune_request_id, result);
        return result;
    }

    dsd_frame_sync_reset_mod_state();
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    opts->trunk_is_tuned = 0;
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        dsd_trunk_tuning_request_mark_ready(tune_request_id);
        if (dsd_trunk_tuning_request_status(tune_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_OK) {
            result = DSD_TRUNK_TUNE_RESULT_OK;
        }
    } else {
        dsd_trunk_tuning_request_complete(tune_request_id, result);
    }
    return result;
}
