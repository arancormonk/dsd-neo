// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * Unified P25 trunking state machine.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_sm_ui.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"
#include "p25_trunk_sm_internal.h"

#if !defined(_MSC_VER)
extern void closeMbeOutFile(dsd_opts* opts, dsd_state* state) DSD_ATTR_WEAK;
extern void closeMbeOutFileR(dsd_opts* opts, dsd_state* state) DSD_ATTR_WEAK;
extern void dsd_p25p2_flush_partial_audio_slot(dsd_opts* opts, dsd_state* state, int slot) DSD_ATTR_WEAK;
#endif

static int do_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason, int recover_stale_ctx);
static void p25_sm_diagf(dsd_opts* opts, const dsd_state* state, const p25_sm_ctx_t* ctx, const char* event,
                         const char* format, ...) DSD_ATTR_FORMAT(printf, 5, 6);
#ifdef USE_RADIO
static int p25_sm_hold_release_for_vc_cqpsk_reacquire(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state,
                                                      const char* reason, double now_m);
#endif

// Serialize release-to-CC operations to avoid duplicate retunes when multiple
// threads (watchdog tick + decoder) request release concurrently.
static atomic_int g_p25_sm_release_lock = 0;

#define P25_ENCRYPTED_CALL_CACHE_TTL_S         10
#define P25_STALE_REGRANT_QUIET_S              2.0
#define P25_STALE_REGRANT_PROBE_DELAY_S        2.0
#define P25_STALE_REGRANT_MAX_AGE_S            10.0
#define P25_CC_HUNT_ACQUIRE_GRACE_S            2.0
#define P25_CC_RETURN_REACQUIRE_NO_SYNC_PASSES 1U
#define P25_VC_CQPSK_MODE_RETRY_DELAY_S        0.8
#define P25_VC_CQPSK_REACQUIRE_MIN_DELAY_S     1.25
#define P25_VC_CQPSK_REACQUIRE_MIN_REMAINING_S 1.0
#define P25_VC_CQPSK_REACQUIRE_HOLD_S          0.75
#define P25_VC_CQPSK_REACQUIRE_NO_SYNC_PASSES  3U

static const char*
p25_tune_result_name(dsd_trunk_tune_result result) {
    switch (result) {
        case DSD_TRUNK_TUNE_RESULT_OK: return "ok";
        case DSD_TRUNK_TUNE_RESULT_DEFERRED: return "deferred";
        case DSD_TRUNK_TUNE_RESULT_PENDING: return "pending";
        case DSD_TRUNK_TUNE_RESULT_FAILED: return "failed";
        case DSD_TRUNK_TUNE_RESULT_TIMEOUT: return "timeout";
        default: return "unknown";
    }
}

static const char*
p25_grant_provenance_name(p25_sm_grant_provenance_e provenance) {
    switch (provenance) {
        case P25_SM_GRANT_PROVENANCE_ASSIGNMENT: return "assignment";
        case P25_SM_GRANT_PROVENANCE_UPDATE: return "update";
        default: return "unknown";
    }
}

static const char*
p25_sm_cc_acquisition_origin_name(p25_sm_cc_acquisition_origin_e origin) {
    switch (origin) {
        case P25_SM_CC_ACQUISITION_NONE: return "none";
        case P25_SM_CC_ACQUISITION_RETURN: return "return";
        case P25_SM_CC_ACQUISITION_HUNT_PROBE: return "hunt-probe";
        default: return "unknown";
    }
}

static double
p25_sm_cc_acquire_grace_s(const p25_sm_ctx_t* ctx, double cc_grace) {
    if (cc_grace <= 0.0) {
        return 0.0;
    }
    if (ctx && ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_HUNT_PROBE
        && cc_grace > P25_CC_HUNT_ACQUIRE_GRACE_S) {
        return P25_CC_HUNT_ACQUIRE_GRACE_S;
    }
    return cc_grace;
}

static long
p25_diag_state_vc_freq(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    if (state->p25_vc_freq[0] != 0) {
        return state->p25_vc_freq[0];
    }
    if (state->p25_vc_freq[1] != 0) {
        return state->p25_vc_freq[1];
    }
    if (state->trunk_vc_freq[0] != 0) {
        return state->trunk_vc_freq[0];
    }
    return state->trunk_vc_freq[1];
}

static void
p25_sm_diagf(dsd_opts* opts, const dsd_state* state, const p25_sm_ctx_t* ctx, const char* event, const char* format,
             ...) {
    if (!dsd_p25_sm_log_enabled(opts) || !event) {
        return;
    }

    char extra[2048] = {0};
    if (format && format[0] != '\0') {
        va_list args;
        va_start(args, format);
        DSD_VSNPRINTF(extra, sizeof extra, format, args);
        va_end(args);
        extra[sizeof extra - 1] = '\0';
    }

    const char* sm_state = ctx ? p25_sm_state_name(ctx->state) : "unknown";
    const long cc = state ? state->p25_cc_freq : 0;
    const long trunk_cc = state ? state->trunk_cc_freq : 0;
    const long vc = p25_diag_state_vc_freq(state);
    const long ctx_vc = ctx ? ctx->vc_freq_hz : 0;
    const unsigned int tune_count = state ? state->p25_sm_tune_count : (ctx ? ctx->tune_count : 0);
    const unsigned int release_count = state ? state->p25_sm_release_count : (ctx ? ctx->release_count : 0);
    const unsigned int cc_return_count = ctx ? ctx->cc_return_count : (state ? state->p25_sm_cc_return_count : 0);

    dsd_p25_sm_logf(opts,
                    "event=%s state=%s cc=%ld trunk_cc=%ld vc=%ld ctx_vc=%ld tunes=%u releases=%u "
                    "cc_returns=%u %s",
                    event, sm_state, cc, trunk_cc, vc, ctx_vc, tune_count, release_count, cc_return_count, extra);
}

static int
p25_diag_freq_in_lcn_list(const dsd_state* state, long freq) {
    if (!state || freq <= 0) {
        return 0;
    }
    int count = state->lcn_freq_count;
    if (count < 0) {
        count = 0;
    }
    if (count > 26) {
        count = 26;
    }
    for (int i = 0; i < count; i++) {
        if (state->trunk_lcn_freq[i] == freq) {
            return 1;
        }
    }
    return 0;
}

static int
p25_diag_freq_in_neighbors(const dsd_state* state, long freq) {
    if (!state || freq <= 0) {
        return 0;
    }
    for (int i = 0; i < state->p25_nb_count && i < P25_NB_MAX; i++) {
        if (state->p25_nb_entries[i].freq == freq) {
            return 1;
        }
    }
    return 0;
}

static int
p25_diag_freq_in_current_site_candidates(const dsd_state* state, long freq) {
    if (!state || freq <= 0) {
        return 0;
    }
    const dsd_trunk_cc_candidates* cc = dsd_trunk_cc_candidates_peek(state);
    if (!cc || cc->count <= 0 || cc->count > DSD_TRUNK_CC_CANDIDATES_MAX) {
        return 0;
    }
    for (int i = 0; i < cc->count; i++) {
        if (cc->candidates[i] == freq && (cc->flags[i] & DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE) != 0) {
            return 1;
        }
    }
    return 0;
}

static int
p25_sm_valid_nac_int(int nac) {
    return nac > 0 && nac != 0xFFF && nac <= 0xFFF;
}

static int
p25_sm_valid_nac_ull(unsigned long long nac) {
    return nac > 0 && nac != 0xFFFULL && nac <= 0xFFFULL;
}

static long
p25_sm_current_tuner_freq_hz(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    if (opts->use_rigctl == 1) {
        long freq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        return (freq > 0) ? freq : 0;
    }
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->rtlsdr_center_freq > 0) {
        return (long)opts->rtlsdr_center_freq;
    }
    return 0;
}

static void
p25_sm_seed_cc_modulation_from_current_sync(dsd_state* state) {
    if (!state) {
        return;
    }
    // Ordinary P25P2 sync is often a traffic-channel MAC on mixed FDMA-CC/P2-VC
    // systems. Only LCCH context proves that the seeded CC uses TDMA timing.
    if (state->p2_is_lcch == 1) {
        state->p25_cc_is_tdma = 1;
    } else if (DSD_SYNC_IS_P25P1(state->synctype)) {
        state->p25_cc_is_tdma = 0;
    }
}

static void
p25_sm_seed_cc_from_known_trunk_alias_if_unknown(dsd_state* state) {
    if (!state || state->p25_cc_freq != 0 || state->trunk_cc_freq <= 0) {
        return;
    }
    state->p25_cc_freq = state->trunk_cc_freq;
    p25_sm_seed_cc_modulation_from_current_sync(state);
    if (state->trunk_lcn_freq[0] == 0) {
        state->trunk_lcn_freq[0] = state->trunk_cc_freq;
    }
}

void
p25_sm_seed_cc_from_current_tuner_if_unknown(const dsd_opts* opts, dsd_state* state) {
    if (!state || state->p25_cc_freq != 0) {
        return;
    }
    p25_sm_seed_cc_from_known_trunk_alias_if_unknown(state);
    if (state->p25_cc_freq != 0) {
        p25_sm_diagf((dsd_opts*)opts, state, NULL, "cc_seed", "source=trunk-alias freq=%ld", state->p25_cc_freq);
        return;
    }
    if (!opts || opts->trunk_is_tuned == 1) {
        return;
    }
    long freq = p25_sm_current_tuner_freq_hz(opts);
    if (freq <= 0) {
        return;
    }
    state->p25_cc_freq = freq;
    state->trunk_cc_freq = freq;
    p25_sm_seed_cc_modulation_from_current_sync(state);
    if (state->trunk_lcn_freq[0] == 0) {
        state->trunk_lcn_freq[0] = freq;
    }
    p25_sm_diagf((dsd_opts*)opts, state, NULL, "cc_seed", "source=current-tuner freq=%ld", freq);
}

static int
p25_sm_current_cc_nac(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    if (state->p2_hardset && p25_sm_valid_nac_ull(state->p2_cc)) {
        return (int)state->p2_cc;
    }
    if (DSD_SYNC_IS_P25P2(state->lastsynctype) && p25_sm_valid_nac_ull(state->p2_cc)) {
        return (int)state->p2_cc;
    }
    if (p25_sm_valid_nac_int(state->nac)) {
        return state->nac;
    }
    if (p25_sm_valid_nac_ull(state->p2_cc)) {
        return (int)state->p2_cc;
    }
    return 0;
}

static void
p25_sm_set_expected_cc_nac(p25_sm_ctx_t* ctx, const dsd_state* state, int replace) {
    if (!ctx || (!replace && p25_sm_valid_nac_int(ctx->expected_cc_nac))) {
        return;
    }
    int nac = p25_sm_current_cc_nac(state);
    if (p25_sm_valid_nac_int(nac)) {
        ctx->expected_cc_nac = nac;
        ctx->nac_mismatch_count = 0;
    }
}

static void
p25_sm_reset_cc_reacquire_tracking(p25_sm_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    ctx->t_cc_reacquire_m = 0.0;
    ctx->t_cc_first_no_sync_m = 0.0;
    ctx->cc_reacquire_attempted = 0;
    ctx->cc_no_sync_passes = 0U;
}

static void
p25_sm_reset_vc_reacquire_tracking(p25_sm_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    ctx->t_vc_reacquire_m = 0.0;
    ctx->t_vc_first_no_sync_m = 0.0;
    ctx->vc_reacquire_eligible = 0;
    ctx->vc_reacquire_attempted = 0;
    ctx->vc_no_sync_passes = 0U;
}

static void
p25_sm_note_vc_decode_activity(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, const char* source, int slot,
                               double now_m) {
    if (!ctx || !ctx->vc_reacquire_eligible) {
        return;
    }
    if (now_m <= 0.0) {
        now_m = dsd_time_now_monotonic_s();
    }
    if (ctx->t_vc_reacquire_m > 0.0) {
        p25_sm_diagf(opts, state, ctx, "vc_reacquire_result",
                     "result=activity source=%s slot=%d latency=%.3f freq=%ld ch=0x%04X", source ? source : "unknown",
                     slot, now_m - ctx->t_vc_reacquire_m, ctx->vc_freq_hz, ctx->vc_channel & 0xFFFF);
    }
    ctx->t_vc_reacquire_m = 0.0;
    ctx->t_vc_first_no_sync_m = 0.0;
    ctx->vc_reacquire_eligible = 0;
    ctx->vc_no_sync_passes = 0U;
}

void
p25_sm_note_vc_frame_sync(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state) {
    if (!ctx || ctx->state != P25_SM_TUNED || !ctx->vc_is_tdma || ctx->vc_data_call) {
        return;
    }
    p25_sm_note_vc_decode_activity(ctx, opts, state, "frame-sync", -1, dsd_time_now_monotonic_s());
}

void
p25_sm_note_cc_no_sync_pass(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    if (!ctx || ctx->state != P25_SM_ON_CC || ctx->cc_tune_pending || !ctx->cc_sync_pending
        || ctx->cc_acquisition_origin != P25_SM_CC_ACQUISITION_RETURN || ctx->t_cc_tune_m <= 0.0) {
        return;
    }

    const double now_m = dsd_time_now_monotonic_s();
    if (now_m < ctx->t_cc_tune_m) {
        return;
    }
    if (ctx->cc_no_sync_passes < UINT32_MAX) {
        ctx->cc_no_sync_passes++;
    }
    if (ctx->t_cc_first_no_sync_m <= 0.0) {
        ctx->t_cc_first_no_sync_m = now_m;
    }
}

static void
p25_sm_log_vc_reacquire_no_activity(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, const char* reason) {
    if (!ctx || ctx->t_vc_reacquire_m <= 0.0) {
        return;
    }
    const double now_m = dsd_time_now_monotonic_s();
    p25_sm_diagf(opts, state, ctx, "vc_reacquire_result",
                 "result=no-activity reason=%s elapsed=%.3f freq=%ld ch=0x%04X", reason ? reason : "unknown",
                 now_m - ctx->t_vc_reacquire_m, ctx->vc_freq_hz, ctx->vc_channel & 0xFFFF);
}

static void
p25_sm_start_cc_grace_after_tune(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double tune_start_m,
                                 const char* source, p25_sm_cc_acquisition_origin_e origin) {
    if (!ctx) {
        return;
    }
    ctx->cc_tune_request_id = 0U;
    ctx->cc_tune_pending = 0;
    p25_sm_reset_cc_reacquire_tracking(ctx);
    ctx->cc_acquisition_origin = origin;
    ctx->t_cc_sync_m = tune_start_m;
    if (state) {
        const double decoded_cc_m = state->p25_last_cc_msg_time_m;
        if (state->last_cc_sync_time_m <= 0.0 || state->last_cc_sync_time_m < tune_start_m) {
            state->last_cc_sync_time = time(NULL);
            state->last_cc_sync_time_m = tune_start_m;
        }
        if (state->last_cc_sync_time_m > ctx->t_cc_sync_m && decoded_cc_m <= tune_start_m) {
            // Absorb a newer raw-sync timestamp only when no decoded CC block
            // already proves activity after the tune boundary. A block decoded
            // before asynchronous completion is resolved must remain eligible
            // to satisfy the strict decoded-after-tune check below.
            ctx->t_cc_sync_m = state->last_cc_sync_time_m;
        }
    }
    ctx->t_cc_tune_m = ctx->t_cc_sync_m;
    ctx->cc_sync_pending = 1;
    p25_sm_diagf(opts, state, ctx, "cc_acquire_start",
                 "source=%s origin=%s effective_grace=%.3f tune_start_m=%.3f tune_m=%.3f last_cc_m=%.3f "
                 "decoded_cc_m=%.3f",
                 source ? source : "unknown", p25_sm_cc_acquisition_origin_name(origin),
                 p25_sm_cc_acquire_grace_s(ctx, ctx->config.cc_grace_s), tune_start_m, ctx->t_cc_tune_m,
                 state ? state->last_cc_sync_time_m : 0.0, state ? state->p25_last_cc_msg_time_m : 0.0);
}

static void
p25_sm_wait_for_cc_tune_completion(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, uint64_t request_id,
                                   const char* source, p25_sm_cc_acquisition_origin_e origin) {
    if (!ctx) {
        return;
    }
    ctx->cc_tune_request_id = request_id;
    ctx->cc_tune_pending = 1;
    p25_sm_reset_cc_reacquire_tracking(ctx);
    ctx->cc_acquisition_origin = origin;
    ctx->t_cc_tune_m = 0.0;
    ctx->cc_sync_pending = 1;
    if (state && state->p25_cc_eval_freq != 0) {
        state->p25_cc_eval_start_m = 0.0;
    }
    p25_sm_diagf(opts, state, ctx, "cc_tune_pending",
                 "source=%s origin=%s effective_grace=%.3f request=%llu eval_freq=%ld", source ? source : "unknown",
                 p25_sm_cc_acquisition_origin_name(origin), p25_sm_cc_acquire_grace_s(ctx, ctx->config.cc_grace_s),
                 (unsigned long long)ctx->cc_tune_request_id, state ? state->p25_cc_eval_freq : 0);
}

static void
p25_sm_start_cc_acquisition_for_result(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state,
                                       dsd_trunk_tune_result tune_result, uint64_t request_id, double tune_start_m,
                                       const char* source, p25_sm_cc_acquisition_origin_e origin) {
    if (tune_result == DSD_TRUNK_TUNE_RESULT_PENDING && request_id != 0U) {
        p25_sm_wait_for_cc_tune_completion(ctx, opts, state, request_id, source, origin);
        return;
    }
    if (request_id != 0U) {
        double completed_m = 0.0;
        if (dsd_trunk_tuning_request_status(request_id, &completed_m) == DSD_TRUNK_TUNE_RESULT_OK
            && completed_m > 0.0) {
            tune_start_m = completed_m;
        }
    }
    p25_sm_start_cc_grace_after_tune(ctx, opts, state, tune_start_m, source, origin);
    if (state && state->p25_cc_eval_freq != 0) {
        state->p25_cc_eval_start_m = ctx->t_cc_tune_m;
    }
}

static int
p25_sm_refresh_cc_sync_from_state(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, const char* trigger) {
    if (!ctx) {
        return 0;
    }
    if (ctx->cc_sync_pending) {
        const double decoded_cc_m = state ? state->p25_last_cc_msg_time_m : 0.0;
        if (decoded_cc_m > ctx->t_cc_tune_m) {
            const double tune_m = ctx->t_cc_tune_m;
            const double reacquire_m = ctx->t_cc_reacquire_m;
            const int reacquire_attempted = ctx->cc_reacquire_attempted;
            const p25_sm_cc_acquisition_origin_e origin = ctx->cc_acquisition_origin;
            ctx->t_cc_sync_m = decoded_cc_m;
            ctx->t_cc_tune_m = 0.0;
            ctx->cc_sync_pending = 0;
            ctx->cc_acquisition_origin = P25_SM_CC_ACQUISITION_NONE;
            p25_sm_reset_cc_reacquire_tracking(ctx);
            p25_sm_set_expected_cc_nac(ctx, state, 1);
            p25_sm_diagf(opts, state, ctx, "cc_reacquired",
                         "source=%s origin=%s decoded_cc_m=%.3f tune_m=%.3f last_cc_m=%.3f expected_nac=0x%03X "
                         "reacquire_attempted=%d soft_reacquire=%d reacquire_latency=%.3f",
                         trigger ? trigger : "state", p25_sm_cc_acquisition_origin_name(origin), decoded_cc_m, tune_m,
                         state ? state->last_cc_sync_time_m : 0.0, ctx->expected_cc_nac, reacquire_attempted,
                         reacquire_m > 0.0 ? 1 : 0,
                         reacquire_m > 0.0 && decoded_cc_m > reacquire_m ? decoded_cc_m - reacquire_m : 0.0);
            return 1;
        }
        p25_sm_set_expected_cc_nac(ctx, state, 0);
        return 0;
    }
    if (state && state->last_cc_sync_time_m > ctx->t_cc_sync_m) {
        ctx->t_cc_sync_m = state->last_cc_sync_time_m;
        p25_sm_set_expected_cc_nac(ctx, state, 1);
        return 1;
    }
    p25_sm_set_expected_cc_nac(ctx, state, 0);
    return 0;
}

static void
p25_sm_cancel_pending_cc_acquisition(p25_sm_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    ctx->cc_tune_request_id = 0U;
    ctx->cc_tune_pending = 0;
    ctx->t_cc_tune_m = 0.0;
    ctx->cc_sync_pending = 0;
    ctx->cc_acquisition_origin = P25_SM_CC_ACQUISITION_NONE;
    p25_sm_reset_cc_reacquire_tracking(ctx);
}

// Determine if channel is TDMA based on IDEN hints.
// Uses bitmask semantics for p25_chan_tdma_explicit[iden]:
//   bit0 (0x01) = has FDMA/non-TDMA entry
//   bit1 (0x02) = has TDMA entry (channel types 3, 4, or 5)
// Values: 0=unknown, 1=FDMA only, 2=TDMA only, 3=both (context-selected)
static inline int
is_tdma_channel(const dsd_state* state, int channel) {
    if (!state) {
        return 0;
    }
    int iden = (channel >> 12) & 0xF;
    if (iden >= 0 && iden < 16) {
        int explicit_hint = state->p25_chan_tdma_explicit[iden];

        if (explicit_hint == 0x03) {
            return (DSD_SYNC_IS_P25P2(state->synctype) || state->p25_cc_is_tdma == 1) ? 1 : 0;
        }
        // If bit1 is set (TDMA entry exists), this is a TDMA channel.
        if (explicit_hint & 0x02) {
            return 1;
        }
        // If only bit0 is set (FDMA entry exists, no TDMA entry), this is FDMA.
        if (explicit_hint & 0x01) {
            return 0;
        }

        // Neither bit set (explicit_hint == 0): no explicit IDEN info available.
        // Fall back to system-level TDMA knowledge. This covers systems with P25p1
        // CQPSK control channels that have not sent IDEN_UP_TDMA yet, preventing
        // Phase 2 grants from being treated as FDMA and avoiding SPS mismatch on VC hops.
        if (state->p25_sys_is_tdma == 1) {
            return 1;
        }
        return 0;
    }
    return 0;
}

static inline int
channel_slot(const dsd_state* state, int channel) {
    return is_tdma_channel(state, channel) ? ((channel & 1) ? 1 : 0) : -1;
}

// Compute TED SPS from the active input timing rate, using live RTL output when available.
static inline int
p25_ted_sps_for_bw(const dsd_opts* opts, int sym_rate_hz) {
    int demod_rate = dsd_opts_current_input_timing_rate(opts);
#ifdef USE_RADIO
    if (opts && opts->audio_in_type == AUDIO_IN_RTL) {
        int rtl_rate = (int)dsd_rtl_stream_metrics_hook_output_rate_hz();
        if (rtl_rate > 0) {
            demod_rate = rtl_rate;
        }
    }
#endif
    return dsd_opts_compute_sps_rate(opts, sym_rate_hz, demod_rate);
}

// Compute TED SPS for control channel based on CC type.
static inline int
cc_ted_sps(const dsd_opts* opts, const dsd_state* state) {
    int sym_rate = (state && state->p25_cc_is_tdma == 1) ? 6000 : 4800;
    return p25_ted_sps_for_bw(opts, sym_rate);
}

// Log status tag for debugging
static void
sm_log(const dsd_opts* opts, dsd_state* state, const char* tag) {
    if (!opts || opts->verbose < 1 || !tag) {
        return;
    }
    if (state) {
        DSD_SNPRINTF(state->p25_sm_last_reason, sizeof(state->p25_sm_last_reason), "%s", tag);
        state->p25_sm_last_reason_time = time(NULL);
        int idx = state->p25_sm_tag_head % 8;
        DSD_SNPRINTF(state->p25_sm_tags[idx], sizeof(state->p25_sm_tags[idx]), "%s", tag);
        state->p25_sm_tag_time[idx] = state->p25_sm_last_reason_time;
        state->p25_sm_tag_head++;
        if (state->p25_sm_tag_count < 8) {
            state->p25_sm_tag_count++;
        }
    }
    if (opts->verbose > 1) {
        DSD_FPRINTF(stderr, "\n[P25 SM] %s\n", tag);
    }
}

static int
p25_sm_ui_mode_from_ctx(const p25_sm_ctx_t* ctx) {
    if (!ctx) {
        return DSD_P25_SM_MODE_UNKNOWN;
    }
    switch (ctx->state) {
        case P25_SM_IDLE: return DSD_P25_SM_MODE_UNKNOWN;
        case P25_SM_ON_CC: return DSD_P25_SM_MODE_ON_CC;
        case P25_SM_HUNTING: return DSD_P25_SM_MODE_HUNTING;
        case P25_SM_TUNED:
            if (ctx->slots[0].voice_active || ctx->slots[1].voice_active) {
                return DSD_P25_SM_MODE_FOLLOW;
            }
            return ctx->vc_activity_seen ? DSD_P25_SM_MODE_HANG : DSD_P25_SM_MODE_ARMED;
    }
    return DSD_P25_SM_MODE_UNKNOWN;
}

static void
p25_sm_update_ui_mode(const p25_sm_ctx_t* ctx, dsd_state* state) {
    if (state) {
        state->p25_sm_mode = p25_sm_ui_mode_from_ctx(ctx);
    }
}

// Set state with logging
static void
set_state(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, p25_sm_state_e new_state, const char* reason) {
    if (!ctx) {
        return;
    }
    if (ctx->state == new_state) {
        p25_sm_update_ui_mode(ctx, state);
        return;
    }
    p25_sm_state_e old = ctx->state;
    ctx->state = new_state;
    p25_sm_update_ui_mode(ctx, state);

    if (opts && opts->verbose > 0) {
        DSD_FPRINTF(stderr, "\n[P25 SM] %s -> %s (%s)\n", p25_sm_state_name(old), p25_sm_state_name(new_state),
                    reason ? reason : "");
    }
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "state", "old=%s new=%s reason=%s", p25_sm_state_name(old),
                 p25_sm_state_name(new_state), reason ? reason : "none");
    sm_log(opts, state, reason);
}

/* Return 1 after successful completion, 0 while still pending, and -1 on failure. */
static int
p25_sm_resolve_pending_cc_tune(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx || !ctx->cc_tune_pending) {
        return 1;
    }

    double completed_m = 0.0;
    const uint64_t request_id = ctx->cc_tune_request_id;
    dsd_trunk_tune_result result = dsd_trunk_tuning_request_status(request_id, &completed_m);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        return 0;
    }

    const p25_sm_cc_acquisition_origin_e origin = ctx->cc_acquisition_origin;
    ctx->cc_tune_request_id = 0U;
    ctx->cc_tune_pending = 0;
    if (result == DSD_TRUNK_TUNE_RESULT_OK) {
        if (completed_m <= 0.0) {
            completed_m = dsd_time_now_monotonic_s();
        }
        p25_sm_start_cc_grace_after_tune(ctx, opts, state, completed_m, "async-complete", origin);
        if (state && state->p25_cc_eval_freq != 0) {
            state->p25_cc_eval_start_m = ctx->t_cc_tune_m;
        }
        p25_sm_diagf(opts, state, ctx, "cc_tune_complete",
                     "request=%llu origin=%s effective_grace=%.3f completed_m=%.3f", (unsigned long long)request_id,
                     p25_sm_cc_acquisition_origin_name(origin), p25_sm_cc_acquire_grace_s(ctx, ctx->config.cc_grace_s),
                     completed_m);
        set_state(ctx, opts, state, P25_SM_ON_CC, "cc-tune-complete");
        return 1;
    }

    ctx->t_cc_tune_m = 0.0;
    ctx->t_cc_sync_m = completed_m > 0.0 ? completed_m : dsd_time_now_monotonic_s();
    ctx->cc_sync_pending = 0;
    ctx->cc_acquisition_origin = P25_SM_CC_ACQUISITION_NONE;
    p25_sm_reset_cc_reacquire_tracking(ctx);
    ctx->t_hunt_try_m = 0.0;
    p25_sm_diagf(opts, state, ctx, "cc_tune_complete", "request=%llu origin=%s result=%s",
                 (unsigned long long)request_id, p25_sm_cc_acquisition_origin_name(origin),
                 p25_tune_result_name(result));
    set_state(ctx, opts, state, P25_SM_HUNTING, "cc-tune-failed");
    return -1;
}

int
p25_sm_restart_pending_cc_acquisition(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double tune_start_m,
                                      const char* source) {
    if (!ctx) {
        return 0;
    }
    const char* reason = source ? source : "external-retune";
    if (tune_start_m <= 0.0) {
        tune_start_m = dsd_time_now_monotonic_s();
    }
    p25_sm_start_cc_grace_after_tune(ctx, opts, state, tune_start_m, reason, P25_SM_CC_ACQUISITION_RETURN);
    if (state && state->p25_cc_eval_freq != 0) {
        state->p25_cc_eval_start_m = ctx->t_cc_tune_m;
    }
    ctx->t_hunt_try_m = 0.0;
    set_state(ctx, opts, state, P25_SM_ON_CC, reason);
    return 1;
}

int
p25_sm_await_pending_cc_tune(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, uint64_t request_id,
                             const char* source) {
    if (!ctx || request_id == 0U) {
        return 0;
    }
    const char* reason = source ? source : "external-retune";
    p25_sm_wait_for_cc_tune_completion(ctx, opts, state, request_id, reason, P25_SM_CC_ACQUISITION_RETURN);
    ctx->t_hunt_try_m = 0.0;
    set_state(ctx, opts, state, P25_SM_ON_CC, reason);
    return 1;
}

/* ============================================================================
 * Grant Filtering
 * ============================================================================ */

// Service option bit helpers
#define SVC_IS_DATA(svc) (((svc) & 0x10) != 0)
#define SVC_IS_ENC(svc)  (((svc) & 0x40) != 0)

static const char*
grant_block_log_tag(int is_indiv, uint32_t block_reasons) {
    if (block_reasons & DSD_TG_POLICY_BLOCK_HOLD) {
        return is_indiv ? "indiv-blocked-hold" : "grant-blocked-hold";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED) {
        return "indiv-blocked-private";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_GROUP_DISABLED) {
        return "grant-blocked-group";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_DATA_DISABLED) {
        return is_indiv ? "indiv-blocked-data" : "grant-blocked-data";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) {
        return is_indiv ? "indiv-blocked-enc" : "grant-blocked-enc";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) {
        return is_indiv ? "indiv-blocked-allowlist" : "grant-blocked-allowlist";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_MODE) {
        return is_indiv ? "indiv-blocked-mode" : "grant-blocked-mode";
    }
    return is_indiv ? "indiv-blocked-policy" : "grant-blocked-policy";
}

static int
lookup_policy_priority_for_tg(const dsd_state* state, int tg) {
    dsd_tg_policy_lookup lookup;
    if (!state || tg < 0) {
        return 0;
    }
    if (dsd_tg_policy_lookup_id(state, (uint32_t)tg, &lookup) != 0) {
        return 0;
    }
    if (lookup.match == DSD_TG_POLICY_MATCH_NONE) {
        return 0;
    }
    return lookup.entry.priority;
}

static void
log_preempt_decision(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state,
                     const dsd_tg_policy_call_route* route, const dsd_tg_policy_decision* decision, const char* reason,
                     int allowed) {
    int current_priority = 0;
    if (!ctx || !opts || !state || !route || !decision || !reason || opts->verbose < 1) {
        return;
    }
    current_priority = lookup_policy_priority_for_tg(state, ctx->vc_tg);
    DSD_FPRINTF(stderr,
                "\n[P25 SM] preempt-%s reason=%s current_tg=%d current_prio=%d candidate_tg=%u candidate_prio=%d "
                "ch=0x%04X freq=%ld slot=%d\n",
                allowed ? "allow" : "deny", reason, ctx->vc_tg, current_priority, route->target_id, decision->priority,
                route->channel & 0xFFFF, route->freq_hz, route->slot);
}

typedef struct {
    int svc;
    int svc_valid;
    int data_call;
    int encrypted_call;
    int enc_override_clear;
    int probe_call;
    int is_indiv;
    int tg;
} p25_grant_eval_ctx_t;

static int
p25_sm_svc_bits_valid(int svc_bits) {
    return svc_bits >= 0;
}

static p25_grant_eval_ctx_t
p25_grant_eval_ctx_from_event(const p25_sm_event_t* ev) {
    p25_grant_eval_ctx_t ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    if (!ev) {
        return ctx;
    }
    ctx.svc_valid = p25_sm_svc_bits_valid(ev->svc_bits);
    ctx.svc = ctx.svc_valid ? ev->svc_bits : 0;
    if (ev->data_call_override > 0) {
        ctx.data_call = 1;
    } else if (ev->data_call_override < 0) {
        ctx.data_call = 0;
    } else {
        ctx.data_call = (ctx.svc_valid && SVC_IS_DATA(ctx.svc)) ? 1 : 0;
    }
    ctx.encrypted_call = (ctx.svc_valid && SVC_IS_ENC(ctx.svc)) ? 1 : 0;
    ctx.is_indiv = !ev->is_group;
    ctx.tg = ctx.is_indiv ? ev->dst : ev->tg;
    return ctx;
}

static void
p25_enc_tg_cache_clear_entry(dsd_state* state, int idx) {
    if (!state || idx < 0 || idx >= DSD_P25_ENC_TG_CACHE_DEPTH) {
        return;
    }
    state->p25_enc_tg_cache_until[idx] = 0;
    state->p25_enc_tg_cache_tg[idx] = 0;
    state->p25_enc_tg_cache_is_group[idx] = 0;
}

static int
p25_enc_tg_cache_find_active(dsd_state* state, int target, int is_group, time_t now) {
    if (!state || target <= 0) {
        return -1;
    }
    is_group = is_group ? 1 : 0;
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        time_t until = state->p25_enc_tg_cache_until[i];
        if (until <= 0) {
            continue;
        }
        if (now >= until) {
            p25_enc_tg_cache_clear_entry(state, i);
            continue;
        }
        if (state->p25_enc_tg_cache_tg[i] == (uint32_t)target
            && state->p25_enc_tg_cache_is_group[i] == (uint8_t)is_group) {
            return i;
        }
    }
    return -1;
}

static void
p25_enc_tg_cache_clear_target(dsd_state* state, int target, int is_group) {
    if (!state || target <= 0) {
        return;
    }
    is_group = is_group ? 1 : 0;
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        if (state->p25_enc_tg_cache_tg[i] == (uint32_t)target
            && state->p25_enc_tg_cache_is_group[i] == (uint8_t)is_group) {
            p25_enc_tg_cache_clear_entry(state, i);
        }
    }
}

static int
p25_enc_tg_cache_refresh_until(time_t now, time_t* out_until) {
    if (!out_until) {
        return 0;
    }
    *out_until = now + P25_ENCRYPTED_CALL_CACHE_TTL_S;
    return 1;
}

static int
p25_enc_tg_cache_choose_slot(dsd_state* state, time_t now) {
    int first_expired = -1;
    if (!state) {
        return -1;
    }
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        if (state->p25_enc_tg_cache_until[i] <= now) {
            first_expired = i;
            break;
        }
    }
    if (first_expired >= 0) {
        return first_expired;
    }
    int idx = (int)(state->p25_enc_tg_cache_next % DSD_P25_ENC_TG_CACHE_DEPTH);
    state->p25_enc_tg_cache_next++;
    return idx;
}

void
p25_sm_note_encrypted_call_typed(dsd_opts* opts, dsd_state* state, int target, int is_group) {
    if (!opts || !state || target <= 0 || opts->trunk_tune_enc_calls != 0) {
        return;
    }
    is_group = is_group ? 1 : 0;
    if (is_group && (p25_patch_tg_key_is_clear(state, target) || p25_patch_sg_key_is_clear(state, target))) {
        return;
    }

    time_t now = time(NULL);
    time_t until = 0;
    if (!p25_enc_tg_cache_refresh_until(now, &until)) {
        return;
    }

    int idx = p25_enc_tg_cache_find_active(state, target, is_group, now);
    if (idx < 0) {
        idx = p25_enc_tg_cache_choose_slot(state, now);
    }
    if (idx < 0) {
        return;
    }

    state->p25_enc_tg_cache_tg[idx] = (uint32_t)target;
    state->p25_enc_tg_cache_is_group[idx] = (uint8_t)is_group;
    state->p25_enc_tg_cache_until[idx] = until;
    p25_sm_diagf(opts, state, NULL, "enc_tg_cache_arm", "kind=%s target=%d idx=%d until=%ld",
                 is_group ? "group" : "private", target, idx, (long)until);
    sm_log(opts, state, "enc-tg-cache-arm");
}

void
p25_sm_clear_encrypted_call_cache(dsd_state* state) {
    if (!state) {
        return;
    }
    DSD_MEMSET(state->p25_enc_tg_cache_until, 0, sizeof(state->p25_enc_tg_cache_until));
    DSD_MEMSET(state->p25_enc_tg_cache_tg, 0, sizeof(state->p25_enc_tg_cache_tg));
    DSD_MEMSET(state->p25_enc_tg_cache_is_group, 0, sizeof(state->p25_enc_tg_cache_is_group));
    state->p25_enc_tg_cache_next = 0;
    dsd_trunk_scan_hook_p25_encrypted_call_cache_clear(state);
}

static void
p25_grant_apply_clear_override(const dsd_opts* opts, const dsd_state* state, p25_grant_eval_ctx_t* eval_ctx) {
    if (!opts || !state || !eval_ctx) {
        return;
    }
    // Preserve P25 regroup clear-key override before policy evaluation.
    if (!eval_ctx->is_indiv && eval_ctx->encrypted_call && opts->trunk_tune_enc_calls == 0 && eval_ctx->tg > 0) {
        if (p25_patch_tg_key_is_clear(state, eval_ctx->tg) || p25_patch_sg_key_is_clear(state, eval_ctx->tg)) {
            eval_ctx->encrypted_call = 0;
            eval_ctx->enc_override_clear = 1;
        }
    }
}

static void
p25_grant_apply_crypto_probe(const dsd_opts* opts, p25_grant_eval_ctx_t* eval_ctx) {
    if (!opts || !eval_ctx || eval_ctx->data_call || eval_ctx->enc_override_clear || opts->trunk_tune_enc_calls != 0) {
        return;
    }
    if (!eval_ctx->svc_valid || eval_ctx->encrypted_call) {
        eval_ctx->probe_call = 1;
    }
}

static int
p25_grant_uses_voice_enc_cache(const p25_grant_eval_ctx_t* eval_ctx) {
    return (eval_ctx && !eval_ctx->data_call) ? 1 : 0;
}

static void
p25_grant_clear_transient_cache_if_clear(dsd_state* state, const p25_grant_eval_ctx_t* eval_ctx) {
    if (!state || !eval_ctx || !p25_grant_uses_voice_enc_cache(eval_ctx) || eval_ctx->tg <= 0) {
        return;
    }
    if ((eval_ctx->svc_valid && !eval_ctx->encrypted_call) || eval_ctx->enc_override_clear) {
        p25_enc_tg_cache_clear_target(state, eval_ctx->tg, !eval_ctx->is_indiv);
    }
}

static int
p25_grant_patch_clear_key(const dsd_state* state, const p25_grant_eval_ctx_t* eval_ctx) {
    if (!state || !eval_ctx || eval_ctx->is_indiv || eval_ctx->tg <= 0) {
        return 0;
    }
    return (p25_patch_tg_key_is_clear(state, eval_ctx->tg) || p25_patch_sg_key_is_clear(state, eval_ctx->tg)) ? 1 : 0;
}

static int
p25_grant_transient_enc_cache_blocks(dsd_opts* opts, dsd_state* state, const p25_grant_eval_ctx_t* eval_ctx) {
    if (!opts || !state || !eval_ctx || eval_ctx->tg <= 0 || opts->trunk_tune_enc_calls != 0) {
        return 0;
    }
    if (!p25_grant_uses_voice_enc_cache(eval_ctx)) {
        return 0;
    }
    if (eval_ctx->svc_valid && !eval_ctx->encrypted_call) {
        return 0;
    }
    if (p25_grant_patch_clear_key(state, eval_ctx)) {
        p25_enc_tg_cache_clear_target(state, eval_ctx->tg, 1);
        return 0;
    }

    time_t now = time(NULL);
    int is_group = !eval_ctx->is_indiv;
    int idx = p25_enc_tg_cache_find_active(state, eval_ctx->tg, is_group, now);
    if (idx < 0) {
        return 0;
    }

    time_t until = 0;
    if (p25_enc_tg_cache_refresh_until(now, &until)) {
        state->p25_enc_tg_cache_until[idx] = until;
    } else {
        p25_enc_tg_cache_clear_entry(state, idx);
        return 0;
    }
    p25_sm_diagf(opts, state, NULL, "grant_enc_cache_skip", "kind=%s target=%d idx=%d until=%ld",
                 is_group ? "group" : "private", eval_ctx->tg, idx, (long)until);
    sm_log(opts, state, "grant-enc-cache");
    return 1;
}

static int
p25_grant_policy_candidate_is_better(const dsd_tg_policy_decision* candidate, const dsd_tg_policy_decision* best,
                                     int have_best) {
    if (!candidate || !candidate->tune_allowed) {
        return 0;
    }
    if (!have_best || !best) {
        return 1;
    }
    if (candidate->tg_hold_match != best->tg_hold_match) {
        return candidate->tg_hold_match ? 1 : 0;
    }
    if (candidate->priority != best->priority) {
        return (candidate->priority > best->priority) ? 1 : 0;
    }
    return (candidate->preempt_requested && !best->preempt_requested) ? 1 : 0;
}

static int
p25_grant_eval_group_policy(const dsd_opts* opts, const dsd_state* state, const p25_sm_event_t* ev,
                            const p25_grant_eval_ctx_t* eval_ctx, dsd_tg_policy_decision* out_decision) {
    uint16_t members[8] = {0};
    int member_count =
        p25_patch_collect_active_wgids(state, eval_ctx->tg, members, sizeof(members) / sizeof(members[0]));
    dsd_tg_policy_decision best;
    dsd_tg_policy_decision first;
    int have_best = 0;
    int have_first = 0;
    DSD_MEMSET(&best, 0, sizeof(best));
    DSD_MEMSET(&first, 0, sizeof(first));

    for (int i = 0; i <= member_count; i++) {
        uint32_t target = (i == 0) ? (uint32_t)eval_ctx->tg : (uint32_t)members[i - 1];
        dsd_tg_policy_decision candidate;
        if (i > 0 && (target == 0U || target == (uint32_t)eval_ctx->tg)) {
            continue;
        }
        if (dsd_tg_policy_evaluate_group_call(opts, state, target, (uint32_t)ev->src, eval_ctx->encrypted_call,
                                              eval_ctx->data_call, &candidate)
            != 0) {
            return -1;
        }
        if (!have_first) {
            first = candidate;
            have_first = 1;
        }
        if (p25_grant_policy_candidate_is_better(&candidate, &best, have_best)) {
            best = candidate;
            have_best = 1;
        }
    }

    if (!have_first) {
        return -1;
    }
    *out_decision = have_best ? best : first;
    return 0;
}

static int
p25_grant_eval_policy(const dsd_opts* opts, const dsd_state* state, const p25_sm_event_t* ev,
                      const p25_grant_eval_ctx_t* eval_ctx, dsd_tg_policy_decision* out_decision) {
    if (!opts || !state || !ev || !eval_ctx || !out_decision) {
        return -1;
    }
    p25_grant_eval_ctx_t policy_ctx = *eval_ctx;
    if (policy_ctx.probe_call) {
        policy_ctx.encrypted_call = 0;
    }
    if (policy_ctx.is_indiv) {
        return dsd_tg_policy_evaluate_private_grant(opts, state, (uint32_t)ev->src, (uint32_t)ev->dst,
                                                    policy_ctx.encrypted_call, policy_ctx.data_call, out_decision);
    }
    return p25_grant_eval_group_policy(opts, state, ev, &policy_ctx, out_decision);
}

static int
p25_grant_handle_policy_block(dsd_opts* opts, dsd_state* state, const p25_grant_eval_ctx_t* eval_ctx,
                              const dsd_tg_policy_decision* decision) {
    if (!opts || !state || !eval_ctx || !decision || decision->tune_allowed) {
        return 0;
    }
    p25_sm_diagf((dsd_opts*)opts, state, NULL, "grant_block",
                 "reason=%s tg=%d policy_tg=%u svc=0x%02X data=%d enc=%d indiv=%d block=0x%08X",
                 grant_block_log_tag(eval_ctx->is_indiv, decision->block_reasons), eval_ctx->tg, decision->target_id,
                 eval_ctx->svc, eval_ctx->data_call, eval_ctx->encrypted_call, eval_ctx->is_indiv,
                 decision->block_reasons);
    sm_log(opts, state, grant_block_log_tag(eval_ctx->is_indiv, decision->block_reasons));
    if (p25_grant_uses_voice_enc_cache(eval_ctx) && !eval_ctx->is_indiv && eval_ctx->tg > 0
        && (decision->block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED)) {
        p25_emit_enc_lockout_once_typed(opts, state, 0, eval_ctx->tg, eval_ctx->svc, 1);
    }
    return 1;
}

static void
p25_grant_track_src_tg(dsd_state* state, const p25_sm_event_t* ev, const p25_grant_eval_ctx_t* eval_ctx) {
    if (!state || !ev || !eval_ctx) {
        return;
    }
    // Track RID<->TG mapping for accepted group grants only.
    if (ev->src > 0 && eval_ctx->tg > 0) {
        p25_ga_add(state, (uint32_t)ev->src, (uint16_t)eval_ctx->tg);
    }
}

static int
grant_allowed(dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev, dsd_tg_policy_decision* out_decision,
              p25_grant_eval_ctx_t* out_eval_ctx) {
    p25_grant_eval_ctx_t eval_ctx;
    dsd_tg_policy_decision decision;
    if (!opts || !state || !ev) {
        return 0;
    }

    eval_ctx = p25_grant_eval_ctx_from_event(ev);
    p25_grant_apply_clear_override(opts, state, &eval_ctx);
    p25_grant_apply_crypto_probe(opts, &eval_ctx);
    p25_grant_clear_transient_cache_if_clear(state, &eval_ctx);
    if (p25_grant_eval_policy(opts, state, ev, &eval_ctx, &decision) != 0) {
        return 0;
    }

    if (p25_grant_handle_policy_block(opts, state, &eval_ctx, &decision)) {
        if (out_decision) {
            *out_decision = decision;
        }
        if (out_eval_ctx) {
            *out_eval_ctx = eval_ctx;
        }
        return 0;
    }

    if (p25_grant_transient_enc_cache_blocks(opts, state, &eval_ctx)) {
        if (out_eval_ctx) {
            *out_eval_ctx = eval_ctx;
        }
        return 0;
    }

    if (!eval_ctx.is_indiv && eval_ctx.enc_override_clear) {
        sm_log(opts, state, "enc-override-clear");
    }
    p25_grant_track_src_tg(state, ev, &eval_ctx);

    if (out_decision) {
        *out_decision = decision;
    }
    if (out_eval_ctx) {
        *out_eval_ctx = eval_ctx;
    }
    return 1;
}

static int
p25_grant_target_id(const p25_sm_event_t* ev, const dsd_tg_policy_decision* decision) {
    if (!ev || !decision) {
        return 0;
    }
    if (decision->target_id > 0) {
        return (int)decision->target_id;
    }
    return ev->is_group ? ev->tg : ev->dst;
}

static int
p25_grant_ota_target_id(const p25_sm_event_t* ev) {
    if (!ev) {
        return 0;
    }
    return ev->is_group ? ev->tg : ev->dst;
}

static int
p25_source_id_known(int src) {
    // 0 means unavailable. 0xFFFFFF is commonly emitted by the fixed network
    // equipment and does not identify a subscriber call epoch.
    return src > 0 && src != 0xFFFFFF;
}

static void
p25_stale_regrant_guard_clear(p25_sm_recent_call_end_t* guard) {
    if (!guard) {
        return;
    }
    DSD_MEMSET(guard, 0, sizeof(*guard));
    guard->slot = -1;
}

static int
p25_stale_regrant_guard_slot_eligible(const p25_sm_slot_ctx_t* slot_ctx, int target) {
    return slot_ctx && !slot_ctx->data_call && slot_ctx->last_end_m > 0.0 && slot_ctx->freq_hz > 0 && target > 0;
}

static int
p25_stale_regrant_guard_same_epoch(const p25_sm_recent_call_end_t* guard, const p25_sm_slot_ctx_t* slot_ctx, int slot,
                                   int target) {
    return guard && slot_ctx && guard->valid && guard->freq_hz == slot_ctx->freq_hz && guard->slot == slot
           && guard->target == target && guard->src == slot_ctx->last_end_src
           && guard->is_group == (slot_ctx->is_group ? 1 : 0);
}

static int
p25_stale_regrant_guard_arm(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, int slot) {
    if (!ctx || slot < 0 || slot > 1) {
        return 0;
    }

    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    const int target = slot_ctx->is_group ? slot_ctx->last_end_tg : slot_ctx->dst;
    if (!p25_stale_regrant_guard_slot_eligible(slot_ctx, target)) {
        return 0;
    }

    p25_sm_recent_call_end_t* guard = &ctx->recent_call_ends[slot];
    if (p25_stale_regrant_guard_same_epoch(guard, slot_ctx, slot, target)) {
        // Repeated terminators describe the same completed epoch. Preserve the
        // original boundary so they cannot extend the validation window.
        return 1;
    }
    guard->end_m = dsd_time_now_monotonic_s();
    // No CC assignment has been observed since returning from the VC yet. A
    // zero last-match timestamp makes the first ambiguous update eligible for
    // quarantine even when CC reacquisition itself took longer than quiet_s.
    guard->last_match_m = 0.0;
    guard->freq_hz = slot_ctx->freq_hz;
    guard->slot = slot;
    guard->target = target;
    guard->src = slot_ctx->last_end_src;
    guard->is_group = slot_ctx->is_group ? 1 : 0;
    guard->probe_attempted = 0;
    guard->valid = 1;
    p25_sm_diagf(opts, state, ctx, "grant_stale_guard_arm",
                 "freq=%ld slot=%d target=%d src=%d group=%d quiet=%.3f probe_after=%.3f max_age=%.3f", guard->freq_hz,
                 guard->slot, guard->target, guard->src, guard->is_group, P25_STALE_REGRANT_QUIET_S,
                 P25_STALE_REGRANT_PROBE_DELAY_S, P25_STALE_REGRANT_MAX_AGE_S);
    return 1;
}

static int
p25_stale_regrant_identity_matches(const p25_sm_recent_call_end_t* guard, const p25_sm_event_t* ev, long freq,
                                   int slot) {
    if (!guard || !ev) {
        return 0;
    }
    const int is_group = ev->is_group ? 1 : 0;
    const int target = p25_grant_ota_target_id(ev);
    return freq == guard->freq_hz && slot == guard->slot && is_group == guard->is_group && target == guard->target;
}

static int
p25_stale_regrant_has_new_source(const p25_sm_recent_call_end_t* guard, const p25_sm_event_t* ev) {
    return guard && ev && p25_source_id_known(guard->src) && p25_source_id_known(ev->src) && ev->src != guard->src;
}

static int
p25_stale_regrant_clear_for_new_epoch(const p25_sm_ctx_t* ctx, p25_sm_recent_call_end_t* guard, dsd_opts* opts,
                                      const dsd_state* state, const p25_sm_event_t* ev, long freq, int slot, int target,
                                      double age_s) {
    // A grant/assignment is a new call epoch. Updates describe continuing
    // assignments and can be indistinguishable from stale post-END traffic.
    if (ev->grant_provenance == P25_SM_GRANT_PROVENANCE_ASSIGNMENT) {
        p25_sm_diagf(opts, state, ctx, "grant_stale_guard_clear",
                     "reason=authoritative-assignment freq=%ld slot=%d target=%d src=%d age=%.3f", freq, slot, target,
                     ev->src, age_s);
        p25_stale_regrant_guard_clear(guard);
        return 1;
    }

    // When both grants identify their source, a changed RID proves this is a
    // new call even though the system reused the same TG, carrier, and slot.
    if (p25_stale_regrant_has_new_source(guard, ev)) {
        p25_sm_diagf(opts, state, ctx, "grant_stale_guard_clear",
                     "reason=new-source freq=%ld slot=%d target=%d old_src=%d new_src=%d age=%.3f", freq, slot, target,
                     guard->src, ev->src, age_s);
        p25_stale_regrant_guard_clear(guard);
        return 1;
    }

    return 0;
}

static int
p25_stale_regrant_update_blocked(const p25_sm_ctx_t* ctx, p25_sm_recent_call_end_t* guard, dsd_opts* opts,
                                 dsd_state* state, const p25_sm_event_t* ev, long freq, int slot, double age_s,
                                 double now_m, int* out_probe) {
    const int is_group = ev->is_group ? 1 : 0;
    const int target = p25_grant_ota_target_id(ev);
    // Bound the quarantine even if a system emits ambiguous assignments
    // continuously. This avoids starving a later legitimate call whose grant
    // happens to omit a source identifier.
    if (age_s >= P25_STALE_REGRANT_MAX_AGE_S) {
        p25_sm_diagf(opts, state, ctx, "grant_stale_guard_clear",
                     "reason=max-age freq=%ld ended_slot=%d slot=%d target=%d src=%d age=%.3f", freq, guard->slot, slot,
                     target, ev->src, age_s);
        p25_stale_regrant_guard_clear(guard);
        return 0;
    }

    const double quiet_s = guard->last_match_m > 0.0 ? now_m - guard->last_match_m : 0.0;
    if (quiet_s < 0.0) {
        p25_stale_regrant_guard_clear(guard);
        return 0;
    }

    // A matching assignment after a full observed quiet interval is eligible
    // again. Continuous source-less Motorola updates refresh this timestamp;
    // the independent maximum-age bound above prevents indefinite starvation.
    if (quiet_s >= P25_STALE_REGRANT_QUIET_S) {
        p25_sm_diagf(opts, state, ctx, "grant_stale_guard_clear",
                     "reason=quiet-gap freq=%ld slot=%d target=%d src=%d age=%.3f quiet=%.3f", freq, slot, target,
                     ev->src, age_s, quiet_s);
        p25_stale_regrant_guard_clear(guard);
        return 0;
    }

    // A live follow-up may be represented only by the same source-less update
    // sequence when its initial assignment was missed during the CC return.
    // Permit one validation tune after a short hold. If it produces no voice,
    // keep the identity quarantine active so later updates cannot loop tunes.
    if (age_s >= P25_STALE_REGRANT_PROBE_DELAY_S && !guard->probe_attempted) {
        guard->probe_attempted = 1;
        guard->last_match_m = now_m;
        if (out_probe) {
            *out_probe = 1;
        }
        p25_sm_diagf(opts, state, ctx, "grant_stale_probe",
                     "ch=0x%04X freq=%ld slot=%d target=%d src=%d provenance=%s age=%.3f quiet=%.3f",
                     ev->channel & 0xFFFF, freq, slot, target, ev->src, p25_grant_provenance_name(ev->grant_provenance),
                     age_s, quiet_s);
        return 0;
    }

    p25_sm_diagf(opts, state, ctx, "grant_stale_skip",
                 "ch=0x%04X freq=%ld ended_slot=%d slot=%d target=%d src=%d group=%d provenance=%s age=%.3f "
                 "quiet=%.3f remaining=%.3f probe_attempted=%d",
                 ev->channel & 0xFFFF, freq, guard->slot, slot, target, ev->src, is_group,
                 p25_grant_provenance_name(ev->grant_provenance), age_s, quiet_s, P25_STALE_REGRANT_QUIET_S - quiet_s,
                 guard->probe_attempted);
    guard->last_match_m = now_m;
    sm_log(opts, state, "grant-stale-skip");
    return 1;
}

static int
p25_grant_stale_regrant_blocked(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev,
                                long freq, int slot, int data_call, double now_m, int* out_probe) {
    if (out_probe) {
        *out_probe = 0;
    }
    if (!ctx || !ev || data_call || slot < 0 || slot > 1) {
        return 0;
    }

    p25_sm_recent_call_end_t* guard = &ctx->recent_call_ends[slot];
    if (!guard->valid || !p25_stale_regrant_identity_matches(guard, ev, freq, slot)) {
        return 0;
    }

    const double age_s = now_m - guard->end_m;
    if (guard->end_m <= 0.0 || age_s < 0.0) {
        p25_stale_regrant_guard_clear(guard);
        return 0;
    }

    const int target = p25_grant_ota_target_id(ev);
    if (p25_stale_regrant_clear_for_new_epoch(ctx, guard, opts, state, ev, freq, slot, target, age_s)) {
        return 0;
    }

    return p25_stale_regrant_update_blocked(ctx, guard, opts, state, ev, freq, slot, age_s, now_m, out_probe);
}

static void
p25_grant_fill_route(dsd_tg_policy_call_route* route, const p25_sm_event_t* ev, long freq, int slot, int needs_retune,
                     int target_id) {
    if (!route || !ev) {
        return;
    }
    DSD_MEMSET(route, 0, sizeof(*route));
    route->target_id = (uint32_t)target_id;
    route->source_id = (uint32_t)((ev->src > 0) ? ev->src : 0);
    route->freq_hz = freq;
    route->channel = ev->channel;
    route->slot = slot;
    route->requires_tuner_retune = needs_retune;
}

static int
p25_grant_candidate_displaces_active(const p25_sm_ctx_t* ctx, const dsd_state* state,
                                     const dsd_tg_policy_call_route* route) {
    if (!ctx || !state || !route) {
        return 0;
    }
    if (route->requires_tuner_retune) {
        return 1;
    }
    if (route->slot == -1) {
        return (ctx->slots[0].voice_active || ctx->slots[1].voice_active || state->p25_p2_audio_allowed[0]
                || state->p25_p2_audio_allowed[1])
                   ? 1
                   : 0;
    }
    if (route->slot >= 0 && route->slot <= 1) {
        const p25_sm_slot_ctx_t* active_slot = &ctx->slots[route->slot];
        if (active_slot->voice_active || active_slot->last_active_m > 0.0 || state->p25_p2_audio_allowed[route->slot]) {
            return 1;
        }
    }
    return 0;
}

static int
p25_grant_preempt_active_call_if_needed(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state,
                                        const dsd_tg_policy_call_route* route, const dsd_tg_policy_decision* decision,
                                        double now_m) {
    if (!ctx || !opts || !state || !route || !decision || ctx->state != P25_SM_TUNED) {
        return 1;
    }
    if (!p25_grant_candidate_displaces_active(ctx, state, route)) {
        return 1;
    }
    if (!decision->preempt_requested) {
        log_preempt_decision(ctx, opts, state, route, decision, "preempt-flag-off", 0);
        sm_log(opts, state, "grant-preempt-flag-off");
        return 0;
    }
    if (!dsd_tg_policy_should_preempt(opts, state, route, decision, now_m)) {
        log_preempt_decision(ctx, opts, state, route, decision, "policy-reject", 0);
        sm_log(opts, state, "grant-preempt-reject");
        return 0;
    }
    log_preempt_decision(ctx, opts, state, route, decision, "policy-allow", 1);
    sm_log(opts, state, "grant-preempt-accept");
    return do_release(ctx, opts, state, "grant-preempt", 0);
}

static void
p25_sm_clear_one_slot_activity(p25_sm_ctx_t* ctx, int slot) {
    if (!ctx || slot < 0 || slot > 1) {
        return;
    }
    ctx->slots[slot].voice_active = 0;
    ctx->slots[slot].last_active_m = 0.0;
    ctx->slots[slot].last_start_m = 0.0;
}

#ifdef USE_RADIO
static void
p25_sm_clear_slot_activity(p25_sm_ctx_t* ctx) {
    p25_sm_clear_one_slot_activity(ctx, 0);
    p25_sm_clear_one_slot_activity(ctx, 1);
}
#endif

static void
p25_grant_clear_one_slot_state(p25_sm_ctx_t* ctx, int slot) {
    if (!ctx || slot < 0 || slot > 1) {
        return;
    }
    p25_sm_clear_one_slot_activity(ctx, slot);
    ctx->slots[slot].algid = 0;
    ctx->slots[slot].keyid = 0;
    ctx->slots[slot].tg = 0;
    ctx->slots[slot].grant_active = 0;
    ctx->slots[slot].freq_hz = 0;
    ctx->slots[slot].channel = 0;
    ctx->slots[slot].target_id = 0;
    ctx->slots[slot].ota_tg = 0;
    ctx->slots[slot].src = 0;
    ctx->slots[slot].dst = 0;
    ctx->slots[slot].is_group = 0;
    ctx->slots[slot].data_call = 0;
    ctx->slots[slot].svc_bits = P25_SM_SVC_UNKNOWN;
    ctx->slots[slot].enc_override_clear = 0;
    ctx->slots[slot].last_grant_m = 0.0;
    ctx->slots[slot].crypto_attempt_m = 0.0;
    ctx->slots[slot].last_end_m = 0.0;
    ctx->slots[slot].last_end_tg = 0;
    ctx->slots[slot].last_end_src = 0;
    ctx->slots[slot].facch_end_m = 0.0;
    ctx->slots[slot].facch_end_tg = 0;
    ctx->slots[slot].facch_end_src = 0;
}

static void
p25_grant_clear_slot_state(p25_sm_ctx_t* ctx) {
    if (!ctx) {
        return;
    }
    for (int s = 0; s < 2; s++) {
        p25_grant_clear_one_slot_state(ctx, s);
    }
}

static void
p25_grant_clear_policy_slot(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    (void)dsd_tg_policy_clear_active_call(state, slot);
    state->p25_policy_tg[slot] = 0;
}

static int
p25_grant_slot_call_identity_matches(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev) {
    if (!slot_ctx || !ev) {
        return 0;
    }

    const int is_group = ev->is_group ? 1 : 0;
    if (slot_ctx->is_group != is_group) {
        return 0;
    }
    return is_group ? (slot_ctx->ota_tg == ev->tg) : (slot_ctx->dst == ev->dst);
}

static int
p25_grant_slot_matches_moved_target(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev, int target_id,
                                    int data_call) {
    if (!slot_ctx || !ev || !slot_ctx->grant_active || slot_ctx->target_id != target_id) {
        return 0;
    }
    if (slot_ctx->data_call != (data_call ? 1 : 0)) {
        return 0;
    }

    return p25_grant_slot_call_identity_matches(slot_ctx, ev);
}

static void
p25_grant_clear_moved_target_slots(p25_sm_ctx_t* ctx, dsd_state* state, int keep_slot, const p25_sm_event_t* ev,
                                   int target_id, long freq, int data_call) {
    if (!ctx || !ev || target_id <= 0) {
        return;
    }
    for (int s = 0; s < 2; s++) {
        const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[s];
        if (s == keep_slot || !p25_grant_slot_matches_moved_target(slot_ctx, ev, target_id, data_call)) {
            continue;
        }
        if (slot_ctx->freq_hz == freq && slot_ctx->channel == ev->channel) {
            continue;
        }
        p25_grant_clear_one_slot_state(ctx, s);
        p25_grant_clear_policy_slot(state, s);
        if (state) {
            state->p25_p2_audio_allowed[s] = 0;
            p25_p2_audio_ring_reset(state, s);
            p25_crypto_reset_slot(state, s);
        }
    }
}

static void
p25_grant_store_slot_context(p25_sm_ctx_t* ctx, const p25_sm_event_t* ev, long freq, int target_id,
                             const p25_grant_eval_ctx_t* eval_ctx, int slot, double now_m) {
    if (!ctx || !ev || slot < 0 || slot > 1) {
        return;
    }
    p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    slot_ctx->grant_active = 1;
    slot_ctx->freq_hz = freq;
    slot_ctx->channel = ev->channel;
    slot_ctx->target_id = target_id;
    slot_ctx->ota_tg = ev->is_group ? ev->tg : 0;
    slot_ctx->src = ev->src;
    slot_ctx->dst = ev->dst;
    slot_ctx->is_group = ev->is_group ? 1 : 0;
    slot_ctx->data_call = (eval_ctx && eval_ctx->data_call) ? 1 : 0;
    slot_ctx->svc_bits = ev->svc_bits;
    slot_ctx->enc_override_clear = (eval_ctx && eval_ctx->enc_override_clear) ? 1 : 0;
    slot_ctx->last_grant_m = now_m;
    slot_ctx->tg = target_id;
}

static int
p25_grant_logical_slot(const p25_sm_ctx_t* ctx, int slot) {
    if (!ctx) {
        return -1;
    }
    return ctx->vc_is_tdma ? slot : 0;
}

static void
p25_grant_refresh_reused_carrier_watchdogs(dsd_state* state, double now_m) {
    if (!state) {
        return;
    }
    time_t now = time(NULL);
    state->last_vc_sync_time = now;
    state->p25_last_vc_tune_time = now;
    state->last_vc_sync_time_m = now_m;
    state->p25_last_vc_tune_time_m = now_m;
}

static void
p25_grant_commit_decoder_tune(dsd_opts* opts, dsd_state* state, long freq) {
    if (!opts || !state || freq <= 0) {
        return;
    }
    const time_t now = time(NULL);
    const double now_m = dsd_time_now_monotonic_s();
    opts->trunk_is_tuned = 1;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    state->last_vc_sync_time = now;
    state->p25_last_vc_tune_time = now;
    state->last_vc_sync_time_m = now_m;
    state->p25_last_vc_tune_time_m = now_m;
}

static void
p25_grant_initialize_timing(p25_sm_ctx_t* ctx, dsd_state* state, double now_m, int reused_carrier, int data_call) {
    // Data grants do not use per-slot pending voice timing, so reused carrier
    // data grants need their own timeout window.
    if (!reused_carrier || ctx->t_tune_m <= 0.0 || data_call) {
        ctx->t_tune_m = now_m;
    }
    if (!reused_carrier || ctx->t_voice_m <= 0.0 || data_call) {
        ctx->t_voice_m = 0.0;
    }
    if (!reused_carrier) {
        ctx->t_hangtime_m = 0.0;
        ctx->vc_activity_seen = 0;
    }
    if (reused_carrier) {
        p25_grant_refresh_reused_carrier_watchdogs(state, now_m);
    } else {
        ctx->vc_cqpsk_retry_done = 0;
        p25_sm_reset_vc_reacquire_tracking(ctx);
        ctx->vc_reacquire_eligible = (ctx->vc_is_tdma && !data_call) ? 1 : 0;
    }
}

static void
p25_grant_begin_crypto_classification(p25_sm_ctx_t* ctx, dsd_state* state, const p25_sm_event_t* ev,
                                      const p25_grant_eval_ctx_t* eval_ctx, int slot, int data_call, double now_m) {
    if (!ctx || !state || !ev || data_call) {
        return;
    }
    const int crypto_slot = ctx->vc_is_tdma ? slot : 0;
    if (crypto_slot < 0 || crypto_slot > 1) {
        return;
    }
    const int force_clear = eval_ctx && eval_ctx->enc_override_clear;
    p25_crypto_begin_voice_call(state, ctx->vc_is_tdma ? DSD_P25_CRYPTO_PHASE2 : DSD_P25_CRYPTO_PHASE1, crypto_slot,
                                ev->svc_bits, force_clear);
    ctx->slots[crypto_slot].crypto_attempt_m =
        state->p25_crypto_state[crypto_slot] == DSD_P25_CRYPTO_ENCRYPTED_PENDING ? now_m : 0.0;
}

static void
p25_grant_clear_stale_cqpsk_override(dsd_state* state, int reused_carrier) {
    if (state && !reused_carrier) {
        state->p25_vc_cqpsk_override = -1;
    }
}

static void
p25_grant_store_vc_context(p25_sm_ctx_t* ctx, dsd_state* state, const p25_sm_event_t* ev, long freq, int target_id,
                           const p25_grant_eval_ctx_t* eval_ctx, double now_m, int slot, int reused_carrier) {
    if (!ctx || !ev) {
        return;
    }
    ctx->vc_freq_hz = freq;
    ctx->vc_channel = ev->channel;
    ctx->vc_tg = target_id;
    ctx->vc_src = ev->src;
    ctx->vc_is_tdma = is_tdma_channel(state, ev->channel);
    const int data_call = (eval_ctx && eval_ctx->data_call) ? 1 : 0;
    ctx->vc_data_call = data_call;
    p25_grant_initialize_timing(ctx, state, now_m, reused_carrier, data_call);
    p25_grant_store_slot_context(ctx, ev, freq, target_id, eval_ctx, p25_grant_logical_slot(ctx, slot), now_m);
    p25_grant_begin_crypto_classification(ctx, state, ev, eval_ctx, slot, data_call, now_m);
    // Clear any stale one-shot VC CQPSK override from a previous attempt.
    p25_grant_clear_stale_cqpsk_override(state, reused_carrier);
}

static int
p25_grant_configure_channel_profile_for_tdma(int vc_is_tdma, const dsd_opts* opts, dsd_state* state, int slot) {
    int ted_sps = p25_ted_sps_for_bw(opts, 4800);
    if (!state) {
        return ted_sps;
    }
    if (vc_is_tdma) {
        ted_sps = p25_ted_sps_for_bw(opts, 6000);
        state->p25_p2_active_slot = slot;
        // P25P2 TDMA always uses CQPSK modulation.
        state->rf_mod = 1;
    } else {
        state->p25_p2_active_slot = -1;
    }
    state->samplesPerSymbol = ted_sps;
    state->symbolCenter = dsd_opts_symbol_center(ted_sps);
    return ted_sps;
}

typedef struct {
    int p25_p2_active_slot;
    int rf_mod;
    int samples_per_symbol;
    int symbol_center;
    int vc_cqpsk_override;
} p25_grant_profile_snapshot_t;

static p25_grant_profile_snapshot_t
p25_grant_profile_snapshot_capture(const dsd_state* state) {
    p25_grant_profile_snapshot_t snapshot = {-1, 0, 0, 0, -1};
    if (!state) {
        return snapshot;
    }
    snapshot.p25_p2_active_slot = state->p25_p2_active_slot;
    snapshot.rf_mod = state->rf_mod;
    snapshot.samples_per_symbol = state->samplesPerSymbol;
    snapshot.symbol_center = state->symbolCenter;
    snapshot.vc_cqpsk_override = state->p25_vc_cqpsk_override;
    return snapshot;
}

static void
p25_grant_profile_snapshot_restore(dsd_state* state, const p25_grant_profile_snapshot_t* snapshot) {
    if (!state || !snapshot) {
        return;
    }
    state->p25_p2_active_slot = snapshot->p25_p2_active_slot;
    state->rf_mod = snapshot->rf_mod;
    state->samplesPerSymbol = snapshot->samples_per_symbol;
    state->symbolCenter = snapshot->symbol_center;
    state->p25_vc_cqpsk_override = snapshot->vc_cqpsk_override;
}

static int
p25_grant_try_tune_vc(const p25_sm_ctx_t* ctx, const p25_sm_event_t* ev, dsd_opts* opts, dsd_state* state, long freq,
                      int slot, int reused_carrier, int* out_ted_sps) {
    const int vc_is_tdma = is_tdma_channel(state, ev->channel);
    if (reused_carrier) {
        if (state && vc_is_tdma) {
            state->p25_p2_active_slot = slot;
            state->rf_mod = 1;
        }
        if (out_ted_sps) {
            *out_ted_sps = state ? state->samplesPerSymbol : 0;
        }
        p25_sm_diagf(opts, state, ctx, "grant_tune_reuse", "ch=0x%04X freq=%ld slot=%d tdma=%d tg=%d src=%d dst=%d",
                     ev->channel & 0xFFFF, freq, slot, vc_is_tdma, ev->tg, ev->src, ev->dst);
        return 1;
    }

    p25_grant_profile_snapshot_t snapshot = p25_grant_profile_snapshot_capture(state);
    const int ted_sps = p25_grant_configure_channel_profile_for_tdma(vc_is_tdma, opts, state, slot);
    p25_sm_diagf(opts, state, ctx, "grant_tune_attempt",
                 "ch=0x%04X freq=%ld slot=%d tdma=%d sps=%d tg=%d src=%d dst=%d", ev->channel & 0xFFFF, freq, slot,
                 vc_is_tdma, ted_sps, ev->tg, ev->src, ev->dst);
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq, ted_sps, NULL);
    if (dsd_trunk_tune_result_is_ok(tune_result)) {
        p25_grant_commit_decoder_tune(opts, state, freq);
        *out_ted_sps = ted_sps;
        p25_sm_diagf(opts, state, ctx, "grant_tune_result", "ch=0x%04X freq=%ld slot=%d tdma=%d sps=%d result=%s",
                     ev->channel & 0xFFFF, freq, slot, vc_is_tdma, ted_sps, p25_tune_result_name(tune_result));
        return 1;
    }

    p25_grant_profile_snapshot_restore(state, &snapshot);
    p25_sm_diagf(opts, state, ctx, "grant_tune_result", "ch=0x%04X freq=%ld slot=%d tdma=%d sps=%d result=%s",
                 ev->channel & 0xFFFF, freq, slot, vc_is_tdma, ted_sps, p25_tune_result_name(tune_result));
    sm_log(opts, state, tune_result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "grant-tune-deferred" : "grant-tune-failed");
    return 0;
}

static void
p25_grant_debug_log_tdma(const dsd_opts* opts, const dsd_state* state, const p25_sm_ctx_t* ctx,
                         const p25_sm_event_t* ev, long freq, int ted_sps) {
    if (!opts || !state || !ctx || !ev || !ctx->vc_is_tdma) {
        return;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    const int debug_sync = (cfg && cfg->debug_sync_enable) ? 1 : 0;
    if (!debug_sync) {
        return;
    }
    DSD_FPRINTF(stderr,
                "[P25-SM] TDMA grant: ch=0x%04X freq=%ld slot=%d rf_mod=%d sps=%d center=%d ted_sps=%d "
                "tune_count=%u grant_count=%u\n",
                ev->channel & 0xFFFF, freq, state->p25_p2_active_slot, state->rf_mod, state->samplesPerSymbol,
                state->symbolCenter, ted_sps, ctx->tune_count, ctx->grant_count);
}

static int
p25_grant_decoder_tuned_to_freq(const dsd_opts* opts, const dsd_state* state, long freq) {
    if (!opts || !state || freq <= 0) {
        return 0;
    }
    if (opts->trunk_is_tuned != 1) {
        return 0;
    }
    return (state->p25_vc_freq[0] == freq || state->p25_vc_freq[1] == freq || state->trunk_vc_freq[0] == freq
            || state->trunk_vc_freq[1] == freq)
               ? 1
               : 0;
}

static int
p25_grant_slot_duplicate_matches(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev,
                                 const dsd_tg_policy_call_route* route, long freq, int target_id, int data_call) {
    if (!slot_ctx || !ev || !route) {
        return 0;
    }
    if (slot_ctx->freq_hz != freq || slot_ctx->target_id != target_id || slot_ctx->data_call != data_call) {
        return 0;
    }
    if (!p25_grant_slot_call_identity_matches(slot_ctx, ev)) {
        return 0;
    }
    return (slot_ctx->grant_active || slot_ctx->last_active_m > 0.0) ? 1 : 0;
}

static int
p25_grant_fallback_duplicate_matches(const p25_sm_ctx_t* ctx, int target_id, int data_call) {
    return ctx && ctx->vc_tg == target_id && ctx->vc_data_call == data_call;
}

static int
p25_grant_other_slot_voice_active(const p25_sm_ctx_t* ctx, int slot) {
    if (!ctx || slot < 0 || slot > 1) {
        return 0;
    }
    return ctx->slots[slot ^ 1].voice_active ? 1 : 0;
}

static void
p25_grant_refresh_duplicate_slot(p25_sm_ctx_t* ctx, dsd_state* state, const dsd_tg_policy_call_route* route,
                                 double now_m, int data_call) {
    if (!ctx || !route || route->slot < 0 || route->slot > 1) {
        return;
    }

    p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[route->slot];
    const int hangtime_refresh =
        (!slot_ctx->grant_active && slot_ctx->last_active_m > 0.0 && !slot_ctx->voice_active) ? 1 : 0;

    slot_ctx->grant_active = 1;
    slot_ctx->freq_hz = route->freq_hz;
    slot_ctx->channel = route->channel;
    slot_ctx->last_grant_m = now_m;
    if (hangtime_refresh && !data_call) {
        slot_ctx->last_active_m = 0.0;
        if (!p25_grant_other_slot_voice_active(ctx, route->slot)) {
            ctx->t_voice_m = 0.0;
        }
    }
    if (state && ctx->vc_is_tdma) {
        state->p25_p2_active_slot = route->slot;
    }
}

static int
p25_grant_service_options_are_explicit_clear(int svc_bits) {
    return svc_bits >= 0 && (svc_bits & 0x40) == 0;
}

static int
p25_grant_duplicate_crypto_slot(const p25_sm_ctx_t* ctx, const dsd_state* state, const p25_sm_event_t* ev,
                                const dsd_tg_policy_call_route* route) {
    if (!ctx || !state || !ev || !route) {
        return -1;
    }
    const int slot = p25_grant_logical_slot(ctx, route->slot);
    return slot >= 0 && slot <= 1 ? slot : -1;
}

static int
p25_grant_duplicate_crypto_needs_restart(int previous_svc, int current_svc, int previous_clear_override,
                                         int force_clear, dsd_p25_crypto_state crypto_state) {
    const int was_explicit_clear = p25_grant_service_options_are_explicit_clear(previous_svc);
    const int is_explicit_clear = p25_grant_service_options_are_explicit_clear(current_svc);
    const int classification_changed = !force_clear && was_explicit_clear != is_explicit_clear;
    const int clear_override_removed = previous_clear_override && !force_clear && !is_explicit_clear;
    const int unapplied_clear_override = force_clear && crypto_state != DSD_P25_CRYPTO_CLEAR;
    return classification_changed || clear_override_removed || unapplied_clear_override;
}

static void
p25_grant_refresh_duplicate_crypto(p25_sm_ctx_t* ctx, dsd_state* state, const p25_sm_event_t* ev,
                                   const dsd_tg_policy_call_route* route, const p25_grant_eval_ctx_t* eval_ctx,
                                   int data_call, double now_m) {
    const int crypto_slot = p25_grant_duplicate_crypto_slot(ctx, state, ev, route);
    if (crypto_slot < 0) {
        return;
    }
    p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[crypto_slot];
    const int previous_svc = slot_ctx->svc_bits;
    const int previous_clear_override = slot_ctx->enc_override_clear;
    const int force_clear = eval_ctx && eval_ctx->enc_override_clear;
    slot_ctx->svc_bits = ev->svc_bits;
    slot_ctx->enc_override_clear = force_clear ? 1 : 0;
    if (data_call) {
        return;
    }

    if (p25_grant_duplicate_crypto_needs_restart(previous_svc, ev->svc_bits, previous_clear_override, force_clear,
                                                 state->p25_crypto_state[crypto_slot])) {
        p25_grant_begin_crypto_classification(ctx, state, ev, eval_ctx, route->slot, 0, now_m);
    }
}

static int
p25_grant_handle_duplicate(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev,
                           const dsd_tg_policy_call_route* route, const dsd_tg_policy_decision* decision, long freq,
                           int target_id, const p25_grant_eval_ctx_t* eval_ctx, double now_m) {
    int data_call = (eval_ctx && eval_ctx->data_call) ? 1 : 0;
    if (!ctx || !ev || !route || ctx->state != P25_SM_TUNED || ctx->vc_freq_hz != freq
        || !p25_grant_decoder_tuned_to_freq(opts, state, freq)) {
        return 0;
    }
    if (route->slot >= 0 && route->slot <= 1) {
        if (!p25_grant_slot_duplicate_matches(&ctx->slots[route->slot], ev, route, freq, target_id, data_call)) {
            return 0;
        }
    } else if (!p25_grant_fallback_duplicate_matches(ctx, target_id, data_call)) {
        return 0;
    }

    p25_grant_refresh_duplicate_slot(ctx, state, route, now_m, data_call);
    p25_grant_refresh_duplicate_crypto(ctx, state, ev, route, eval_ctx, data_call, now_m);
    if (data_call) {
        ctx->t_tune_m = now_m;
        ctx->t_voice_m = 0.0;
    }
    p25_grant_refresh_reused_carrier_watchdogs(state, now_m);
    (void)dsd_tg_policy_note_active_call(state, route, decision, now_m);
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "grant_duplicate", "freq=%ld tg=%d target=%d slot=%d data=%d", freq,
                 ctx->vc_tg, target_id, route->slot, data_call);
    sm_log(opts, state, "grant-same-freq");
    return 1;
}

static void
p25_grant_store_policy_tg(dsd_state* state, const p25_sm_event_t* ev, int slot,
                          const dsd_tg_policy_decision* decision) {
    if (!state || !ev || !decision || !ev->is_group) {
        return;
    }

    uint32_t policy_tg =
        (decision->target_id > 0U && decision->target_id != (uint32_t)ev->tg) ? decision->target_id : 0U;
    if (slot >= 0 && slot <= 1) {
        state->p25_policy_tg[slot] = policy_tg;
        return;
    }
    state->p25_policy_tg[0] = policy_tg;
    state->p25_policy_tg[1] = 0;
}

static void
p25_grant_clear_replaced_policy_tg(dsd_state* state, int slot, int slot_only) {
    if (!state) {
        return;
    }
    if (slot_only && slot >= 0 && slot <= 1) {
        state->p25_policy_tg[slot] = 0;
        return;
    }
    state->p25_policy_tg[0] = 0;
    state->p25_policy_tg[1] = 0;
}

static void
p25_grant_seed_cc_before_vc_tune(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, double now_m) {
    if (!ctx || !state) {
        return;
    }
    if (state->p25_cc_freq == 0) {
        p25_sm_seed_cc_from_known_trunk_alias_if_unknown(state);
        if (state->p25_cc_freq != 0) {
            p25_sm_diagf((dsd_opts*)opts, state, ctx, "cc_seed", "source=trunk-alias-before-grant freq=%ld",
                         state->p25_cc_freq);
        }
    }
    if (state->p25_cc_freq == 0 || ctx->state != P25_SM_IDLE) {
        return;
    }

    if (state->last_cc_sync_time_m <= 0.0 || state->last_cc_sync_time_m < now_m) {
        state->last_cc_sync_time = time(NULL);
        state->last_cc_sync_time_m = now_m;
    }
    ctx->t_cc_sync_m =
        (state->p25_last_cc_msg_time_m > 0.0) ? state->p25_last_cc_msg_time_m : state->last_cc_sync_time_m;
    ctx->t_cc_tune_m = 0.0;
    ctx->cc_sync_pending = 0;
    ctx->cc_acquisition_origin = P25_SM_CC_ACQUISITION_NONE;
    p25_sm_reset_cc_reacquire_tracking(ctx);
    p25_sm_set_expected_cc_nac(ctx, state, 0);
    set_state(ctx, opts, state, P25_SM_ON_CC, "grant-cc-seed");
}

typedef struct {
    dsd_tg_policy_call_route route;
    int slot;
    int logical_slot;
    int target_id;
    int stale_regrant_probe;
    int needs_retune;
    int clear_policy_slot_only;
    int reused_carrier;
    int ted_sps;
    long freq;
    double now_m;
} p25_grant_route_ctx_t;

static int
p25_grant_is_sticky_block_candidate(const p25_sm_ctx_t* ctx, const dsd_state* state,
                                    const p25_grant_eval_ctx_t* eval_ctx, const p25_grant_route_ctx_t* grant) {
    if (!ctx || !state || !eval_ctx || !grant) {
        return 0;
    }
    if (ctx->state != P25_SM_TUNED || grant->slot < 0 || grant->slot > 1) {
        return 0;
    }
    return eval_ctx->probe_call && state->p25_crypto_state[grant->slot] == DSD_P25_CRYPTO_BLOCKED;
}

static int
p25_grant_matches_sticky_block(const p25_sm_ctx_t* ctx, const dsd_state* state, const p25_sm_event_t* ev,
                               const p25_grant_eval_ctx_t* eval_ctx, const p25_grant_route_ctx_t* grant) {
    if (!ev || !p25_grant_is_sticky_block_candidate(ctx, state, eval_ctx, grant)) {
        return 0;
    }

    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[grant->slot];
    if (slot_ctx->freq_hz != grant->freq || slot_ctx->channel != ev->channel || slot_ctx->target_id != grant->target_id
        || slot_ctx->data_call != (eval_ctx->data_call ? 1 : 0)) {
        return 0;
    }
    return p25_grant_slot_call_identity_matches(slot_ctx, ev);
}

static void
p25_grant_log_freq(dsd_opts* opts, const dsd_state* state, const p25_sm_ctx_t* ctx, const p25_sm_event_t* ev, long freq,
                   const p25_freq_trace_t* freq_trace) {
    if (!freq_trace) {
        return;
    }
    p25_sm_diagf(opts, state, ctx, "grant_freq",
                 "ch=0x%04X freq=%ld source=%s failure=%s iden=%d type=%d tdma=%d denom=%d step=%d cached=%d "
                 "ambiguous=%d base=%ld spacing=%ld tg=%d src=%d dst=%d svc=0x%02X provenance=%s",
                 ev->channel & 0xFFFF, freq, freq_trace->source, freq_trace->failure[0] ? freq_trace->failure : "none",
                 freq_trace->iden, freq_trace->chan_type, freq_trace->use_tdma, freq_trace->denom, freq_trace->step,
                 freq_trace->cached, freq_trace->ambiguous, freq_trace->base_hz, freq_trace->spacing_hz, ev->tg,
                 ev->src, ev->dst, ev->svc_bits, p25_grant_provenance_name(ev->grant_provenance));
}

static int
p25_grant_should_clear_slot_only(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state,
                                 const p25_sm_event_t* ev, long freq, int slot) {
    if (!ctx || !state || !ev) {
        return 0;
    }
    if (ctx->state != P25_SM_TUNED || ctx->vc_freq_hz != freq || !p25_grant_decoder_tuned_to_freq(opts, state, freq)) {
        return 0;
    }
    if (ctx->vc_is_tdma) {
        return (slot >= 0 && slot <= 1 && is_tdma_channel(state, ev->channel)) ? 1 : 0;
    }
    return is_tdma_channel(state, ev->channel) ? 0 : 1;
}

static int
p25_grant_prepare_route(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev,
                        const dsd_tg_policy_decision* decision, const p25_grant_eval_ctx_t* eval_ctx,
                        p25_grant_route_ctx_t* out) {
    p25_freq_trace_t freq_trace;
    if (!ctx || !opts || !state || !ev || !decision || !eval_ctx || !out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->freq = process_channel_to_freq_trace(opts, state, ev->channel, &freq_trace);
    p25_grant_log_freq(opts, state, ctx, ev, out->freq, &freq_trace);
    if (out->freq == 0) {
        sm_log(opts, state, "grant-no-freq");
        return 0;
    }

    out->now_m = dsd_time_now_monotonic_s();
    out->slot = channel_slot(state, ev->channel);
    out->logical_slot = is_tdma_channel(state, ev->channel) ? out->slot : 0;
    if (p25_grant_stale_regrant_blocked(ctx, opts, state, ev, out->freq, out->logical_slot, eval_ctx->data_call,
                                        out->now_m, &out->stale_regrant_probe)) {
        return 0;
    }
    out->needs_retune = (ctx->state == P25_SM_TUNED && ctx->vc_freq_hz != 0 && ctx->vc_freq_hz != out->freq) ? 1 : 0;
    out->clear_policy_slot_only = p25_grant_should_clear_slot_only(ctx, opts, state, ev, out->freq, out->slot);
    out->reused_carrier = out->clear_policy_slot_only;
    out->target_id = p25_grant_target_id(ev, decision);
    p25_grant_fill_route(&out->route, ev, out->freq, out->slot, out->needs_retune, out->target_id);
    return 1;
}

static int
p25_grant_blocked_by_pending_cc(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev,
                                const p25_grant_route_ctx_t* grant) {
    if (!ctx || !ev || !grant || (!ctx->cc_tune_pending && !ctx->cc_sync_pending)) {
        return 0;
    }
    if (ctx->cc_tune_pending) {
        int tune_status = p25_sm_resolve_pending_cc_tune(ctx, opts, state);
        if (tune_status <= 0) {
            p25_sm_diagf(opts, state, ctx, "grant_cc_pending",
                         "ch=0x%04X freq=%ld slot=%d tg=%d src=%d request=%llu tune_pending=%d", ev->channel & 0xFFFF,
                         grant->freq, grant->slot, ev->tg, ev->src, (unsigned long long)ctx->cc_tune_request_id,
                         ctx->cc_tune_pending);
            return 1;
        }
    }
    (void)p25_sm_refresh_cc_sync_from_state(ctx, opts, state, "grant");
    if (!ctx->cc_sync_pending) {
        if (ctx->state == P25_SM_HUNTING) {
            set_state(ctx, opts, state, P25_SM_ON_CC, "grant-cc-reacquired");
        }
        return 0;
    }

    p25_sm_diagf(opts, state, ctx, "grant_cc_pending",
                 "ch=0x%04X freq=%ld slot=%d tg=%d src=%d tune_m=%.3f last_cc_m=%.3f decoded_cc_m=%.3f",
                 ev->channel & 0xFFFF, grant->freq, grant->slot, ev->tg, ev->src, ctx->t_cc_tune_m,
                 state ? state->last_cc_sync_time_m : 0.0, state ? state->p25_last_cc_msg_time_m : 0.0);
    return 1;
}

static void
p25_grant_start_stale_regrant_probe(p25_sm_ctx_t* ctx, const p25_grant_route_ctx_t* grant) {
    if (!ctx || !grant) {
        return;
    }
    ctx->vc_stale_regrant_probe = grant->stale_regrant_probe;
    ctx->vc_stale_regrant_probe_slot = grant->stale_regrant_probe ? grant->logical_slot : -1;
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

static void
handle_grant(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    dsd_tg_policy_decision decision;
    p25_grant_eval_ctx_t eval_ctx;
    p25_grant_route_ctx_t grant;
    if (!ctx || !ev || !opts || !state) {
        return;
    }
    DSD_MEMSET(&eval_ctx, 0, sizeof(eval_ctx));

    // Check grant policy
    if (!grant_allowed(opts, state, ev, &decision, &eval_ctx)) {
        return;
    }

    if (!p25_grant_prepare_route(ctx, opts, state, ev, &decision, &eval_ctx, &grant)) {
        return;
    }
    if (p25_grant_blocked_by_pending_cc(ctx, opts, state, ev, &grant)) {
        return;
    }
    if (p25_grant_matches_sticky_block(ctx, state, ev, &eval_ctx, &grant)) {
        p25_sm_diagf(opts, state, ctx, "grant_crypto_sticky_block", "ch=0x%04X freq=%ld slot=%d target=%d",
                     ev->channel & 0xFFFF, grant.freq, grant.slot, grant.target_id);
        sm_log(opts, state, "grant-crypto-sticky-block");
        return;
    }

    // Skip only true duplicate grants; same-RF grants for a different call context
    // still need normal grant handling so the slot context is replaced.
    if (p25_grant_handle_duplicate(ctx, opts, state, ev, &grant.route, &decision, grant.freq, grant.target_id,
                                   &eval_ctx, grant.now_m)) {
        p25_grant_store_policy_tg(state, ev, grant.slot, &decision);
        return;
    }

    if (!p25_grant_preempt_active_call_if_needed(ctx, opts, state, &grant.route, &decision, grant.now_m)) {
        return;
    }
    grant.clear_policy_slot_only = p25_grant_should_clear_slot_only(ctx, opts, state, ev, grant.freq, grant.slot);
    grant.reused_carrier = grant.clear_policy_slot_only;

    p25_grant_seed_cc_before_vc_tune(ctx, opts, state, grant.now_m);

    if (!p25_grant_try_tune_vc(ctx, ev, opts, state, grant.freq, grant.slot, grant.reused_carrier, &grant.ted_sps)) {
        return;
    }
    p25_sm_cancel_pending_cc_acquisition(ctx);
    p25_grant_clear_moved_target_slots(ctx, state, grant.slot, ev, grant.target_id, grant.freq, eval_ctx.data_call);
    if (grant.clear_policy_slot_only) {
        p25_grant_clear_one_slot_state(ctx, grant.logical_slot);
    } else {
        p25_grant_clear_slot_state(ctx);
    }
    p25_grant_clear_replaced_policy_tg(state, grant.logical_slot, grant.clear_policy_slot_only);
    p25_grant_store_vc_context(ctx, state, ev, grant.freq, grant.target_id, &eval_ctx, grant.now_m, grant.slot,
                               grant.reused_carrier);
    p25_grant_start_stale_regrant_probe(ctx, &grant);
    p25_grant_store_policy_tg(state, ev, grant.slot, &decision);
    if (!grant.reused_carrier) {
        ctx->tune_count++;
        state->p25_sm_tune_count++;
    }
    ctx->grant_count++;
    p25_grant_debug_log_tdma(opts, state, ctx, ev, grant.freq, grant.ted_sps);

    (void)dsd_tg_policy_note_active_call(state, &grant.route, &decision, grant.now_m);
    p25_sm_diagf(opts, state, ctx, "grant_accept",
                 "ch=0x%04X freq=%ld slot=%d target=%d ota_tg=%d src=%d needs_retune=%d "
                 "prio=%d preempt=%d data=%d reused_carrier=%d stale_probe=%d",
                 ev->channel & 0xFFFF, grant.freq, grant.slot, grant.target_id, ev->tg, ev->src, grant.needs_retune,
                 decision.priority, decision.preempt_requested, eval_ctx.data_call, grant.reused_carrier,
                 grant.stale_regrant_probe);

    set_state(ctx, opts, state, P25_SM_TUNED, "grant");
}

void
p25_sm_apply_group_grant_policy(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    if (!opts || !state || opts->trunk_enable != 1) {
        return;
    }

    p25_sm_event_t ev = p25_sm_ev_group_grant(channel, 0, tg, src, svc_bits);
    dsd_tg_policy_decision decision;
    if (grant_allowed(opts, state, &ev, &decision, NULL)) {
        p25_sm_diagf(opts, state, NULL, "grant_policy_only", "ch=0x%04X tg=%d src=%d svc=0x%02X policy_tg=%u",
                     channel & 0xFFFF, tg, src, svc_bits & 0xFF, decision.target_id);
    }
}

static int
p25_voice_state_slot_tg(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    return slot == 0 ? (int)state->lasttg : (int)state->lasttgR;
}

static int
p25_voice_state_slot_src(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    return slot == 0 ? state->lastsrc : state->lastsrcR;
}

static int
p25_voice_slot_diag_tg(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot) {
    const int decoded_tg = p25_voice_state_slot_tg(state, slot);
    if (decoded_tg > 0) {
        return decoded_tg;
    }
    if (!ctx || slot < 0 || slot > 1) {
        return 0;
    }
    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    if (slot_ctx->is_group && slot_ctx->ota_tg > 0) {
        return slot_ctx->ota_tg;
    }
    return slot_ctx->is_group ? slot_ctx->target_id : 0;
}

static int
p25_voice_slot_diag_src(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot) {
    const int decoded_src = p25_voice_state_slot_src(state, slot);
    if (decoded_src > 0) {
        return decoded_src;
    }
    if (!ctx || slot < 0 || slot > 1) {
        return 0;
    }
    return ctx->slots[slot].src;
}

static const char*
p25_voice_phase_name(const p25_sm_ctx_t* ctx) {
    return ctx && ctx->vc_is_tdma ? "p2" : "p1";
}

static void
p25_voice_set_state_identity(dsd_state* state, int slot, const p25_sm_event_t* ev) {
    if (!state || !ev || slot < 0 || slot > 1) {
        return;
    }
    const int target = ev->is_group ? ev->tg : ev->dst;
    if (slot == 0) {
        state->lasttg = target;
        state->lastsrc = ev->src;
        state->dmr_so = p25_sm_svc_bits_valid(ev->svc_bits) ? (unsigned int)ev->svc_bits : 0U;
    } else {
        state->lasttgR = target;
        state->lastsrcR = ev->src;
        state->dmr_soR = p25_sm_svc_bits_valid(ev->svc_bits) ? (unsigned int)ev->svc_bits : 0U;
    }
    state->gi[slot] = ev->is_group ? 0 : 1;
    state->p25_service_options_valid[slot] = p25_sm_svc_bits_valid(ev->svc_bits) ? 1 : 0;
}

static void
p25_voice_clear_state_source(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return;
    }
    if (slot == 0) {
        state->lastsrc = 0;
        state->payload_miP = 0;
        state->payload_algid = 0;
        state->payload_keyid = 0;
        state->dmr_so = 0;
    } else {
        state->lastsrcR = 0;
        state->payload_miN = 0;
        state->payload_algidR = 0;
        state->payload_keyidR = 0;
        state->dmr_soR = 0;
    }
    state->generic_talker_alias[slot][0] = '\0';
    state->generic_talker_alias_src[slot] = 0;
    state->p25_p2_audio_allowed[slot] = 0;
    state->p25_call_emergency[slot] = 0;
    state->p25_call_priority[slot] = 0;
    state->p25_call_is_packet[slot] = 0;
    state->p25_service_options_valid[slot] = 0;
    DSD_SNPRINTF(state->call_string[slot], sizeof(state->call_string[slot]), "%s", "                     ");
}

static void
p25_voice_flush_partial_audio(const p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot) {
    if (!ctx->vc_is_tdma) {
        return;
    }
#if !defined(_MSC_VER)
    if (dsd_p25p2_flush_partial_audio_slot == NULL) {
        return;
    }
#endif
    dsd_p25p2_flush_partial_audio_slot(opts, state, slot);
}

static void
p25_voice_close_slot_output(dsd_opts* opts, dsd_state* state, int slot) {
    if (!opts) {
        return;
    }
    if (slot == 0) {
        if (opts->mbe_out_f != NULL
#if !defined(_MSC_VER)
            && closeMbeOutFile != NULL
#endif
        ) {
            closeMbeOutFile(opts, state);
        }
        return;
    }
    if (opts->mbe_out_fR != NULL
#if !defined(_MSC_VER)
        && closeMbeOutFileR != NULL
#endif
    ) {
        closeMbeOutFileR(opts, state);
    }
}

static void
p25_voice_close_slot_media(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot) {
    if (!ctx || slot < 0 || slot > 1) {
        return;
    }
    if (state) {
        p25_voice_flush_partial_audio(ctx, opts, state, slot);
        p25_p2_audio_ring_reset(state, slot);
        p25_voice_close_slot_output(opts, state, slot);
        p25_voice_clear_state_source(state, slot);
        p25_crypto_reset_slot(state, slot);
    }
    ctx->slots[slot].voice_active = 0;
    ctx->slots[slot].last_active_m = 0.0;
    ctx->slots[slot].algid = 0;
    ctx->slots[slot].keyid = 0;
    ctx->slots[slot].src = 0;
    ctx->slots[slot].crypto_attempt_m = 0.0;
}

static int
p25_voice_start_build_identity(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot, const p25_sm_event_t* input,
                               p25_sm_event_t* out) {
    if (!ctx || !input || !out || slot < 0 || slot > 1) {
        return 0;
    }
    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    if (!slot_ctx->grant_active || slot_ctx->freq_hz <= 0) {
        return 0;
    }

    *out = *input;
    out->type = P25_SM_EV_GRANT;
    out->slot = ctx->vc_is_tdma ? slot : -1;
    out->channel = slot_ctx->channel;
    out->freq_hz = slot_ctx->freq_hz;
    out->grant_provenance = P25_SM_GRANT_PROVENANCE_ASSIGNMENT;
    out->data_call_override = -1;
    out->facch = 0;

    if (!input->identity_valid) {
        out->is_group = slot_ctx->is_group;
        out->tg = slot_ctx->is_group ? slot_ctx->ota_tg : 0;
        out->dst = slot_ctx->is_group ? 0 : slot_ctx->dst;
        out->src = p25_voice_state_slot_src(state, slot);
        if (!p25_source_id_known(out->src)) {
            out->src = slot_ctx->src;
        }
        out->svc_bits = slot_ctx->svc_bits;
    } else if (!p25_sm_svc_bits_valid(input->svc_bits)) {
        out->svc_bits = slot_ctx->svc_bits;
    }
    out->identity_valid = 1;
    return p25_grant_ota_target_id(out) > 0;
}

static int
p25_voice_start_target_changed(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev) {
    if (!slot_ctx || !ev || slot_ctx->is_group != (ev->is_group ? 1 : 0)) {
        return 1;
    }
    return ev->is_group ? (slot_ctx->ota_tg != ev->tg) : (slot_ctx->dst != ev->dst);
}

static int
p25_voice_start_source_changed(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev) {
    return slot_ctx && ev && p25_source_id_known(slot_ctx->src) && p25_source_id_known(ev->src)
           && slot_ctx->src != ev->src;
}

static int
p25_voice_start_service_changed(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev) {
    return slot_ctx && ev && p25_sm_svc_bits_valid(ev->svc_bits) && slot_ctx->svc_bits != ev->svc_bits;
}

typedef struct {
    int new_epoch;
    int service_changed;
    int requires_update;
} p25_voice_start_changes_t;

static p25_voice_start_changes_t
p25_voice_start_classify_changes(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* call_ev) {
    p25_voice_start_changes_t changes = {0, 0, 0};
    const int target_changed = p25_voice_start_target_changed(slot_ctx, call_ev);
    const int source_changed = p25_voice_start_source_changed(slot_ctx, call_ev);
    const int learned_source = !p25_source_id_known(slot_ctx->src) && p25_source_id_known(call_ev->src);

    changes.service_changed = p25_voice_start_service_changed(slot_ctx, call_ev);
    changes.new_epoch = !slot_ctx->voice_active || target_changed || source_changed;
    changes.requires_update = changes.new_epoch || changes.service_changed || learned_source;
    return changes;
}

static void
p25_voice_start_commit_identity(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot,
                                const p25_sm_event_t* call_ev, const dsd_tg_policy_decision* decision,
                                const p25_grant_eval_ctx_t* eval_ctx, const p25_voice_start_changes_t* changes,
                                double now_m) {
    p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    const int logical_slot = ctx->vc_is_tdma ? slot : 0;

    if (changes->new_epoch && slot_ctx->voice_active) {
        (void)dsd_tg_policy_clear_active_call(state, ctx->vc_is_tdma ? slot : -1);
        p25_voice_close_slot_media(ctx, opts, state, slot);
    }

    const int target_id = p25_grant_target_id(call_ev, decision);
    dsd_tg_policy_call_route route;
    p25_grant_fill_route(&route, call_ev, slot_ctx->freq_hz, ctx->vc_is_tdma ? slot : -1, 0, target_id);
    slot_ctx->target_id = target_id;
    slot_ctx->ota_tg = call_ev->is_group ? call_ev->tg : 0;
    slot_ctx->dst = call_ev->is_group ? 0 : call_ev->dst;
    slot_ctx->src = call_ev->src;
    slot_ctx->is_group = call_ev->is_group ? 1 : 0;
    slot_ctx->svc_bits = call_ev->svc_bits;
    slot_ctx->enc_override_clear = eval_ctx->enc_override_clear ? 1 : 0;
    slot_ctx->last_grant_m = now_m;
    slot_ctx->tg = target_id;
    ctx->vc_tg = target_id;
    ctx->vc_src = call_ev->src;
    p25_voice_set_state_identity(state, slot, call_ev);
    p25_grant_store_policy_tg(state, call_ev, logical_slot, decision);
    (void)dsd_tg_policy_note_active_call(state, &route, decision, now_m);
    if (changes->new_epoch || changes->service_changed) {
        p25_grant_begin_crypto_classification(ctx, state, call_ev, eval_ctx, slot, 0, now_m);
    }
}

static int
p25_voice_start_apply_identity(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot,
                               const p25_sm_event_t* input, double now_m, int* out_new_epoch) {
    p25_sm_event_t call_ev;
    if (out_new_epoch) {
        *out_new_epoch = !ctx->slots[slot].voice_active;
    }
    if (!p25_voice_start_build_identity(ctx, state, slot, input, &call_ev)) {
        return 1;
    }

    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    const p25_voice_start_changes_t changes = p25_voice_start_classify_changes(slot_ctx, &call_ev);
    if (out_new_epoch) {
        *out_new_epoch = changes.new_epoch;
    }

    if (!changes.requires_update) {
        return 1;
    }

    dsd_tg_policy_decision decision;
    p25_grant_eval_ctx_t eval_ctx;
    if (!grant_allowed(opts, state, &call_ev, &decision, &eval_ctx)) {
        p25_voice_close_slot_media(ctx, opts, state, slot);
        (void)do_release(ctx, opts, state, "policy-reject", 0);
        return 0;
    }

    p25_voice_start_commit_identity(ctx, opts, state, slot, &call_ev, &decision, &eval_ctx, &changes, now_m);
    return 1;
}

static void
p25_voice_start_clear_facch_end_tracking(p25_sm_ctx_t* ctx) {
    ctx->slots[0].facch_end_m = ctx->slots[1].facch_end_m = 0.0;
    ctx->slots[0].facch_end_tg = ctx->slots[1].facch_end_tg = 0;
    ctx->slots[0].facch_end_src = ctx->slots[1].facch_end_src = 0;
}

static void
p25_voice_start_clear_activity_guards(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, int slot,
                                      const char* why, double now_m) {
    if (ctx->recent_call_ends[slot].valid) {
        p25_sm_diagf(opts, state, ctx, "grant_stale_guard_clear",
                     "reason=in-band-activity freq=%ld slot=%d target=%d src=%d", ctx->vc_freq_hz, slot,
                     ctx->slots[slot].target_id, ctx->slots[slot].src);
        p25_stale_regrant_guard_clear(&ctx->recent_call_ends[slot]);
    }
    p25_voice_start_clear_facch_end_tracking(ctx);

    if (!ctx->vc_stale_regrant_probe || slot != ctx->vc_stale_regrant_probe_slot) {
        return;
    }
    const double latency_s = ctx->t_tune_m > 0.0 ? now_m - ctx->t_tune_m : 0.0;
    p25_sm_diagf(opts, state, ctx, "grant_stale_probe_result",
                 "result=activity source=%s slot=%d latency=%.3f freq=%ld ch=0x%04X", why, slot, latency_s,
                 ctx->vc_freq_hz, ctx->vc_channel & 0xFFFF);
    ctx->vc_stale_regrant_probe = 0;
    ctx->vc_stale_regrant_probe_slot = -1;
    p25_stale_regrant_guard_clear(&ctx->recent_call_ends[slot]);
}

static int
p25_voice_start_crypto_suppressed(const dsd_opts* opts, const dsd_state* state, int slot) {
    if (!opts || !state || opts->trunk_tune_enc_calls != 0) {
        return 0;
    }
    return p25_crypto_companion_suppressed(state, slot);
}

static int
p25_voice_start_wait_for_classification(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot, const char* why,
                                        double now_m) {
    if (!p25_voice_start_crypto_suppressed(opts, state, slot)) {
        return 0;
    }

    // The lockout policy may have been enabled after follow mode marked this
    // blocked slot active. Clear that stale activity before ignoring subsequent
    // voice indications so the tuned timer can release it.
    const double inactive_since_m = ctx->t_voice_m > 0.0 ? ctx->t_voice_m : now_m;
    ctx->slots[slot].voice_active = 0;
    ctx->slots[slot].last_active_m = 0.0;
    ctx->t_voice_m = 0.0;
    ctx->t_hangtime_m = inactive_since_m;
    p25_sm_diagf(opts, state, ctx, "voice_classification_wait", "kind=%s slot=%d crypto_state=%d", why, slot,
                 (int)state->p25_crypto_state[slot]);
    p25_sm_update_ui_mode(ctx, state);
    return 1;
}

static void
handle_voice_start(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev, const char* why) {
    if (!ctx || !ev || ctx->state != P25_SM_TUNED) {
        return;
    }

    double now_m = dsd_time_now_monotonic_s();
    int s = (ev->slot >= 0 && ev->slot <= 1) ? ev->slot : 0;
    int new_epoch = !ctx->slots[s].voice_active;
    if (!p25_voice_start_apply_identity(ctx, opts, state, s, ev, now_m, &new_epoch)) {
        return;
    }
    const int reused = ctx->vc_activity_seen && new_epoch;
    if (new_epoch) {
        ctx->slots[s].last_start_m = now_m;
    }
    p25_voice_start_clear_activity_guards(ctx, opts, state, s, why, now_m);
    p25_sm_note_vc_decode_activity(ctx, opts, state, why, s, now_m);

    ctx->vc_activity_seen = 1;
    ctx->t_hangtime_m = 0.0;

    if (p25_voice_start_wait_for_classification(ctx, opts, state, s, why, now_m)) {
        return;
    }

    // Update slot activity
    ctx->slots[s].voice_active = 1;
    ctx->slots[s].last_active_m = now_m;

    // NOTE: Audio gating is managed by MAC_PTT/MAC_ACTIVE handlers in xcch.c,
    // ENC event handler, and ESS processing in frame.c.
    // This event just marks voice as active for state machine timing purposes.

    ctx->t_voice_m = now_m;
    if (reused) {
        p25_sm_diagf(opts, state, ctx, "traffic_reuse", "phase=%s freq=%ld slot=%d tg=%d src=%d reason=%s",
                     p25_voice_phase_name(ctx), ctx->vc_freq_hz, s, p25_voice_slot_diag_tg(ctx, state, s),
                     p25_voice_slot_diag_src(ctx, state, s), why);
    }
    p25_sm_diagf(opts, state, ctx, "voice_activity",
                 "kind=%s slot=%d now_m=%.3f freq=%ld target=%d tg=%d src=%d grant=%d", why, ev->slot, now_m,
                 ctx->vc_freq_hz, ctx->slots[s].target_id, p25_voice_slot_diag_tg(ctx, state, s),
                 p25_voice_slot_diag_src(ctx, state, s), ctx->slots[s].grant_active);
    sm_log(opts, state, why);
    p25_sm_update_ui_mode(ctx, state);
}

static int
p25_voice_end_diag_other_active(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot) {
    int other = slot ^ 1;
    if (ctx->slots[other].voice_active) {
        return 1;
    }
    if (state && state->p25_p2_audio_allowed[other]) {
        return 1;
    }
    return 0;
}

static int
p25_voice_end_tg_conflicts(int event_tg, int current_tg) {
    return event_tg > 0 && current_tg > 0 && event_tg != current_tg;
}

static int
p25_voice_end_src_conflicts(int event_src, int current_src) {
    return p25_source_id_known(event_src) && p25_source_id_known(current_src) && event_src != current_src;
}

static int
p25_voice_end_grant_identity_conflicts(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev) {
    if (!slot_ctx->grant_active) {
        return 0;
    }
    if (slot_ctx->is_group && p25_voice_end_tg_conflicts(ev->tg, slot_ctx->ota_tg)) {
        return 1;
    }
    return p25_voice_end_src_conflicts(ev->src, slot_ctx->src);
}

static int
p25_voice_end_active_identity_conflicts(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev, int decoded_tg,
                                        int decoded_src) {
    if (!slot_ctx->voice_active) {
        return 0;
    }
    if (p25_voice_end_tg_conflicts(ev->tg, decoded_tg)) {
        return 1;
    }
    return p25_voice_end_src_conflicts(ev->src, decoded_src);
}

static int
p25_voice_end_identity_conflicts(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot, const p25_sm_event_t* ev) {
    if (!ctx || !ev || slot < 0 || slot > 1) {
        return 0;
    }

    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    const int decoded_tg = p25_voice_state_slot_tg(state, slot);
    const int decoded_src = p25_voice_state_slot_src(state, slot);

    // Once voice is active, the per-slot decoder identity is the closest
    // match to the MAC_END_PTT carried on that traffic channel.
    if (slot_ctx->voice_active) {
        return p25_voice_end_active_identity_conflicts(slot_ctx, ev, decoded_tg, decoded_src);
    }

    // Before PTT arrives, an accepted same-slot assignment is authoritative.
    // Do not let a delayed END from the preceding call erase it.
    return p25_voice_end_grant_identity_conflicts(slot_ctx, ev);
}

static const char*
p25_voice_end_newer_event_reason(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev) {
    if (ev->observed_m <= 0.0) {
        return NULL;
    }
    if (slot_ctx->last_grant_m > ev->observed_m) {
        return "newer-grant";
    }
    if (slot_ctx->last_active_m > ev->observed_m) {
        return "newer-activity";
    }
    if (slot_ctx->last_start_m > ev->observed_m) {
        return "newer-start";
    }
    return NULL;
}

static int
p25_voice_end_is_duplicate(const p25_sm_slot_ctx_t* slot_ctx) {
    return slot_ctx->last_end_m > 0.0 && slot_ctx->last_start_m <= slot_ctx->last_end_m && !slot_ctx->voice_active ? 1
                                                                                                                   : 0;
}

static const char*
p25_voice_end_reject_reason(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot, const p25_sm_event_t* ev) {
    if (!ctx || !ev || slot < 0 || slot > 1) {
        return NULL;
    }

    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    const char* newer_event_reason = p25_voice_end_newer_event_reason(slot_ctx, ev);
    if (newer_event_reason) {
        return newer_event_reason;
    }
    if (p25_voice_end_identity_conflicts(ctx, state, slot, ev)) {
        return "identity-mismatch";
    }
    if (p25_voice_end_is_duplicate(slot_ctx)) {
        return "duplicate";
    }
    return NULL;
}

static int
p25_voice_end_preserve_recent_idle_grant(const p25_sm_ctx_t* ctx, int slot, int is_explicit_end, double observed_m) {
    if (!ctx || slot < 0 || slot > 1 || is_explicit_end || observed_m <= 0.0) {
        return 0;
    }
    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    return (slot_ctx->grant_active && slot_ctx->last_grant_m > observed_m) ? 1 : 0;
}

static const char*
p25_voice_end_event_reject_reason(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot, int is_explicit_end,
                                  int arm_stale_regrant_guard, const p25_sm_event_t* ev) {
    if (!is_explicit_end || !arm_stale_regrant_guard || !ev) {
        return NULL;
    }
    return p25_voice_end_reject_reason(ctx, state, slot, ev);
}

static int
p25_voice_end_event_tg(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot, const p25_sm_event_t* ev) {
    return ev && ev->tg > 0 ? ev->tg : p25_voice_slot_diag_tg(ctx, state, slot);
}

static int
p25_voice_end_event_src(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot, const p25_sm_event_t* ev) {
    return ev && ev->src > 0 ? ev->src : p25_voice_slot_diag_src(ctx, state, slot);
}

static void
p25_voice_end_record(p25_sm_slot_ctx_t* slot_ctx, int is_explicit_end, int arm_stale_regrant_guard, double now_m,
                     int tg, int src) {
    (void)is_explicit_end;
    if (!arm_stale_regrant_guard) {
        return;
    }
    slot_ctx->last_end_m = now_m;
    slot_ctx->last_end_tg = tg;
    slot_ctx->last_end_src = src;
}

static int
handle_voice_end(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot, const char* why, int is_explicit_end,
                 int arm_stale_regrant_guard, const p25_sm_event_t* ev) {
    if (!ctx || ctx->state != P25_SM_TUNED) {
        return 0;
    }

    int s = (slot >= 0 && slot <= 1) ? slot : 0;
    const double observed_m = ev ? ev->observed_m : 0.0;
    const char* reject_reason =
        p25_voice_end_event_reject_reason(ctx, state, s, is_explicit_end, arm_stale_regrant_guard, ev);
    if (reject_reason) {
        p25_sm_diagf(opts, state, ctx, "voice_end_ignored",
                     "reason=%s slot=%d event_tg=%d event_src=%d current_tg=%d current_src=%d grant=%d active=%d",
                     reject_reason, s, ev->tg, ev->src, p25_voice_slot_diag_tg(ctx, state, s),
                     p25_voice_slot_diag_src(ctx, state, s), ctx->slots[s].grant_active, ctx->slots[s].voice_active);
        return 0;
    }

    const double now_m = dsd_time_now_monotonic_s();
    const int ended_tg = p25_voice_end_event_tg(ctx, state, s, ev);
    const int ended_src = p25_voice_end_event_src(ctx, state, s, ev);
    p25_sm_note_vc_decode_activity(ctx, opts, state, why, s, now_m);
    int preserve_recent_grant = p25_voice_end_preserve_recent_idle_grant(ctx, s, is_explicit_end, observed_m);

    if (preserve_recent_grant) {
        p25_sm_diagf(opts, state, ctx, "voice_end_ignored",
                     "reason=newer-grant slot=%d event_tg=%d event_src=%d current_tg=%d current_src=%d", s,
                     ev ? ev->tg : 0, ev ? ev->src : 0, p25_voice_slot_diag_tg(ctx, state, s),
                     p25_voice_slot_diag_src(ctx, state, s));
        return 0;
    }

    p25_voice_end_record(&ctx->slots[s], is_explicit_end, arm_stale_regrant_guard, now_m, ended_tg, ended_src);
    if (state) {
        (void)dsd_tg_policy_clear_active_call(state, ctx->vc_is_tdma ? s : -1);
    }
    p25_voice_close_slot_media(ctx, opts, state, s);
    ctx->vc_activity_seen = 1;
    ctx->t_voice_m = 0.0;
    ctx->t_hangtime_m = now_m;
    if (arm_stale_regrant_guard) {
        (void)p25_stale_regrant_guard_arm(ctx, opts, state, s);
    }

    sm_log(opts, state, why);
    p25_sm_diagf(opts, state, ctx, "transmission_end",
                 "phase=%s freq=%ld slot=%d tg=%d src=%d reason=%s explicit=%d other_active=%d",
                 p25_voice_phase_name(ctx), ctx->vc_freq_hz, s, ended_tg, ended_src, why, is_explicit_end,
                 p25_voice_end_diag_other_active(ctx, state, s));
    p25_sm_diagf(opts, state, ctx, "traffic_hang", "phase=%s freq=%ld slot=%d tg=%d src=%d reason=%s started_m=%.3f",
                 p25_voice_phase_name(ctx), ctx->vc_freq_hz, s, ended_tg, ended_src, why, ctx->t_hangtime_m);
    p25_sm_update_ui_mode(ctx, state);
    return 1;
}

static int
p25_facch_end_identity_matches(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev) {
    if (!slot_ctx || !ev || slot_ctx->facch_end_m <= 0.0) {
        return 0;
    }
    const int event_tg = ev->tg > 0 ? ev->tg : slot_ctx->last_end_tg;
    const int event_src = ev->src > 0 ? ev->src : slot_ctx->last_end_src;
    if (event_tg != slot_ctx->facch_end_tg) {
        return 0;
    }
    if (p25_source_id_known(event_src) || p25_source_id_known(slot_ctx->facch_end_src)) {
        return event_src == slot_ctx->facch_end_src;
    }
    return 1;
}

static int
p25_facch_all_slots_inactive(const p25_sm_ctx_t* ctx, const dsd_state* state) {
    if (!ctx) {
        return 0;
    }
    for (int slot = 0; slot < 2; slot++) {
        if (ctx->slots[slot].voice_active) {
            return 0;
        }
        if (state && (state->p25_p2_audio_allowed[slot] || state->p25_p2_audio_ring_count[slot] > 0)) {
            return 0;
        }
    }
    return 1;
}

static int
handle_facch_voice_end(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (!ctx || !ev || ev->slot < 0 || ev->slot > 1) {
        return P25_SM_END_IGNORED;
    }
    p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[ev->slot];
    const double observed_m = ev->observed_m > 0.0 ? ev->observed_m : dsd_time_now_monotonic_s();
    const double elapsed = observed_m - slot_ctx->facch_end_m;
    if (elapsed >= 0.0 && elapsed <= 1.0 && p25_facch_end_identity_matches(slot_ctx, ev)
        && p25_facch_all_slots_inactive(ctx, state)) {
        p25_sm_diagf(opts, state, ctx, "facch_release_hint", "phase=p2 freq=%ld slot=%d tg=%d src=%d elapsed=%.3f",
                     ctx->vc_freq_hz, ev->slot, slot_ctx->facch_end_tg, slot_ctx->facch_end_src, elapsed);
        return do_release(ctx, opts, state, "facch-double-end", 0) ? P25_SM_END_CHANNEL_RELEASED : P25_SM_END_IGNORED;
    }

    int applied = handle_voice_end(ctx, opts, state, ev->slot, "end", 1, 1, ev);
    if (!applied) {
        return P25_SM_END_IGNORED;
    }
    slot_ctx->facch_end_m = observed_m;
    slot_ctx->facch_end_tg = slot_ctx->last_end_tg;
    slot_ctx->facch_end_src = slot_ctx->last_end_src;
    return P25_SM_END_APPLIED;
}

static int
p25_sm_slot_waiting_for_voice(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot) {
    if (!ctx || slot < 0 || slot > 1) {
        return 0;
    }
    if (ctx->vc_activity_seen) {
        return 0;
    }
    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    if (!slot_ctx->grant_active || slot_ctx->data_call || slot_ctx->voice_active || slot_ctx->last_active_m > 0.0) {
        return 0;
    }
    if (state && state->p25_p2_audio_allowed[slot]) {
        return 0;
    }
    return 1;
}

static int
p25_sm_has_pending_voice_grant(const p25_sm_ctx_t* ctx, const dsd_state* state) {
    if (!ctx || !ctx->vc_is_tdma) {
        return 0;
    }
    return p25_sm_slot_waiting_for_voice(ctx, state, 0) || p25_sm_slot_waiting_for_voice(ctx, state, 1);
}

static int
p25_sm_has_pending_data_grant(const p25_sm_ctx_t* ctx) {
    if (!ctx || !ctx->vc_is_tdma) {
        return 0;
    }
    for (int s = 0; s < 2; s++) {
        if (ctx->slots[s].grant_active && ctx->slots[s].data_call) {
            return 1;
        }
    }
    return 0;
}

static double
p25_sm_pending_voice_grant_timeout_start_m(const p25_sm_ctx_t* ctx, const dsd_state* state) {
    double latest_wait_m = 0.0;
    if (!ctx || !ctx->vc_is_tdma) {
        return 0.0;
    }
    for (int s = 0; s < 2; s++) {
        if (!p25_sm_slot_waiting_for_voice(ctx, state, s)) {
            continue;
        }
        if (ctx->slots[s].last_grant_m > latest_wait_m) {
            latest_wait_m = ctx->slots[s].last_grant_m;
        }
        // An in-band crypto transition starts a new wait window even when the
        // original grant and tune are older than the configured timeout.
        if (state && state->p25_crypto_state[s] == DSD_P25_CRYPTO_ENCRYPTED_PENDING
            && ctx->slots[s].crypto_attempt_m > latest_wait_m) {
            latest_wait_m = ctx->slots[s].crypto_attempt_m;
        }
    }
    return latest_wait_m;
}

static void
p25_enc_lockout_clear_slot_grant(p25_sm_ctx_t* ctx, dsd_state* state, int slot) {
    if (!ctx || !state || slot < 0 || slot > 1) {
        return;
    }
    ctx->slots[slot].grant_active = 0;
    ctx->slots[slot].crypto_attempt_m = 0.0;
    (void)dsd_tg_policy_clear_active_call(state, ctx->vc_is_tdma ? slot : -1);
    state->p25_policy_tg[slot] = 0;
}

static int
p25_enc_lockout_other_slot_active(const p25_sm_ctx_t* ctx, const dsd_state* state, int other) {
    if (!ctx || !state || other < 0 || other > 1) {
        return 0;
    }
    return (ctx->slots[other].voice_active || ctx->slots[other].grant_active || state->p25_p2_audio_allowed[other]
            || (state->p25_p2_audio_ring_count[other] > 0))
               ? 1
               : 0;
}

static int
p25_enc_lockout_precheck(const p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, int slot, int allow_audio,
                         dsd_p25_crypto_state crypto_state) {
    if (opts->trunk_tune_enc_calls != 0) {
        state->p25_p2_audio_allowed[slot] = allow_audio;
        return 1;
    }
    if (opts->trunk_enable != 1 || ctx->state != P25_SM_TUNED) {
        return 1;
    }
    if (p25_crypto_audio_ready(state, slot)) {
        state->p25_p2_audio_allowed[slot] = allow_audio;
        return 1;
    }
    if (crypto_state != DSD_P25_CRYPTO_BLOCKED) {
        state->p25_p2_audio_allowed[slot] = 0;
        return 1;
    }
    return 0;
}

static void
p25_enc_lockout_clear_slot_burst(dsd_state* state, int slot) {
    if (slot == 0) {
        state->dmrburstL = 0;
    } else {
        state->dmrburstR = 0;
    }
}

static void
p25_enc_lockout_release_or_preserve_companion(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot) {
    if (!ctx->vc_is_tdma) {
        do_release(ctx, opts, state, "enc-lockout", 0);
        return;
    }

    const int other = slot ^ 1;
    if (!p25_enc_lockout_other_slot_active(ctx, state, other)) {
        do_release(ctx, opts, state, "enc-lockout", 0);
        return;
    }

    p25_sm_diagf(opts, state, ctx, "enc_lockout_slot_only", "slot=%d other=%d", slot, other);
    sm_log(opts, state, "enc-lockout-slot-only");
}

static void
handle_cc_sync(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }
    const double now_m = dsd_time_now_monotonic_s();
    if (state) {
        state->last_cc_sync_time = time(NULL);
        state->last_cc_sync_time_m = now_m;
    }
    if (ctx->cc_sync_pending) {
        (void)p25_sm_refresh_cc_sync_from_state(ctx, (dsd_opts*)opts, state, "sm-event");
    } else {
        ctx->t_cc_sync_m = state ? state->last_cc_sync_time_m : now_m;
        p25_sm_set_expected_cc_nac(ctx, state, 1);
    }
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "cc_sync",
                 "pending=%d expected_nac=0x%03X nac=0x%03X last_cc_m=%.3f decoded_cc_m=%.3f", ctx->cc_sync_pending,
                 ctx->expected_cc_nac, state ? state->nac : 0, state ? state->last_cc_sync_time_m : 0.0,
                 state ? state->p25_last_cc_msg_time_m : 0.0);

    if (!ctx->cc_sync_pending && (ctx->state == P25_SM_IDLE || ctx->state == P25_SM_HUNTING)) {
        set_state(ctx, opts, state, P25_SM_ON_CC, "cc-sync");
    }
}

static void
handle_crypto_pending(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (!ctx || !state || !ev || ev->slot < 0 || ev->slot > 1) {
        return;
    }

    const int slot = ev->slot;
    p25_sm_note_vc_decode_activity(ctx, opts, state, "crypto-pending", slot, dsd_time_now_monotonic_s());
    const dsd_p25_crypto_state previous = state->p25_crypto_state[slot];
    p25_crypto_mark_encrypted_pending(state, slot);
    if (state->p25_crypto_state[slot] != DSD_P25_CRYPTO_ENCRYPTED_PENDING) {
        return;
    }

    ctx->slots[slot].voice_active = 0;
    if (previous == DSD_P25_CRYPTO_ENCRYPTED_PENDING && ctx->slots[slot].crypto_attempt_m > 0.0) {
        return;
    }

    const double now_m = dsd_time_now_monotonic_s();
    ctx->slots[slot].crypto_attempt_m = now_m;
    if (opts && opts->trunk_tune_enc_calls == 0) {
        // Pending metadata suppresses voice events under lockout. Discard the
        // preceding clear-voice hangtime so this new deadline owns the wait.
        ctx->slots[slot].last_active_m = 0.0;
        ctx->t_voice_m = 0.0;
    }
    p25_sm_diagf(opts, state, ctx, "crypto_classification_start", "slot=%d previous=%d freq=%ld tg=%d", slot,
                 (int)previous, ctx->vc_freq_hz, ctx->vc_tg);
    sm_log(opts, state, "crypto-classify-start");
}

static void
handle_enc(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (!ctx || !ev || !opts || !state) {
        return;
    }

    int slot = (ev->slot >= 0 && ev->slot <= 1) ? ev->slot : 0;
    p25_sm_note_vc_decode_activity(ctx, opts, state, "enc", slot, dsd_time_now_monotonic_s());
    int algid = ev->algid;
    int tg = ev->tg;
    int allow_audio = 0;
    dsd_p25_crypto_state crypto_state = state->p25_crypto_state[slot];

    // Store encryption params in slot context
    ctx->slots[slot].algid = algid;
    ctx->slots[slot].keyid = ev->keyid;
    ctx->slots[slot].tg = tg;
    allow_audio = dsd_p25p2_decode_audio_allowed(opts, state, slot, algid);

    if (p25_enc_lockout_precheck(ctx, opts, state, slot, allow_audio, crypto_state)) {
        return;
    }

    // Single-indication lockout: trigger immediately on encrypted stream
    p25_sm_diagf(opts, state, ctx, "enc_lockout",
                 "slot=%d algid=0x%02X keyid=0x%04X tg=%d crypto_state=%d allow_audio=%d", slot, algid, ev->keyid, tg,
                 (int)crypto_state, allow_audio);
    sm_log(opts, state, "enc-lockout");

    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    int target = tg;
    int is_group = 1;
    if (slot_ctx->is_group && slot_ctx->ota_tg > 0) {
        target = slot_ctx->ota_tg;
    } else if (!slot_ctx->is_group && slot_ctx->dst > 0) {
        target = slot_ctx->dst;
        is_group = 0;
    }
    if (target > 0) {
        p25_emit_enc_lockout_once_typed(opts, state, (uint8_t)slot, target, 0x40, is_group);
    }

    // Gate audio for this slot
    state->p25_p2_audio_allowed[slot] = 0;

    // Clear voice activity indicator to prevent audio routing logic from
    // treating this locked-out slot as having active voice
    ctx->slots[slot].voice_active = 0;
    p25_enc_lockout_clear_slot_grant(ctx, state, slot);
    p25_enc_lockout_clear_slot_burst(state, slot);
    p25_enc_lockout_release_or_preserve_companion(ctx, opts, state, slot);
}

/* ============================================================================
 * Release to CC
 * ============================================================================ */

static int
p25_release_should_return_to_cc(const p25_sm_ctx_t* ctx, const dsd_opts* opts) {
    const int opts_tuned = (opts && (opts->trunk_is_tuned == 1)) ? 1 : 0;
    return (ctx && (ctx->state == P25_SM_TUNED || opts_tuned)) ? 1 : 0;
}

static int
p25_release_ctx_is_stale(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state) {
    const int opts_tuned = (opts && (opts->trunk_is_tuned == 1)) ? 1 : 0;
    const int state_has_vc = (state
                              && (state->p25_vc_freq[0] != 0 || state->p25_vc_freq[1] != 0
                                  || state->trunk_vc_freq[0] != 0 || state->trunk_vc_freq[1] != 0))
                                 ? 1
                                 : 0;
    return (ctx && ctx->state == P25_SM_TUNED && !opts_tuned && !state_has_vc) ? 1 : 0;
}

static int
p25_release_return_to_cc_accepted(const p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int had_force_release,
                                  double* out_tune_start_m, dsd_trunk_tune_result* out_tune_result,
                                  uint64_t* out_request_id) {
    if (ctx->vc_is_tdma && opts && state) {
        dsd_p25_optional_hook_p25p2_flush_partial_audio(opts, state);
    }

    *out_tune_start_m = dsd_time_now_monotonic_s();
    p25_sm_diagf(opts, state, ctx, "release_cc_attempt",
                 "freq=%ld ch=0x%04X tg=%d force=%d tdma=%d data=%d cc_tdma=%d cc_sps=%d",
                 state ? ((state->p25_cc_freq != 0) ? state->p25_cc_freq : state->trunk_cc_freq) : 0,
                 ctx->vc_channel & 0xFFFF, ctx->vc_tg, had_force_release, ctx->vc_is_tdma, ctx->vc_data_call,
                 state ? state->p25_cc_is_tdma : 0, cc_ted_sps(opts, state));
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_return_to_cc(opts, state, out_request_id);
    if (out_tune_result) {
        *out_tune_result = tune_result;
    }
    if (dsd_trunk_tune_result_is_ok(tune_result)) {
        p25_sm_diagf(opts, state, ctx, "release_cc_result", "result=%s tune_start_m=%.3f",
                     p25_tune_result_name(tune_result), *out_tune_start_m);
        return 1;
    }

    p25_sm_diagf(opts, state, ctx, "release_cc_result", "result=%s tune_start_m=%.3f",
                 p25_tune_result_name(tune_result), *out_tune_start_m);
    sm_log(opts, state, tune_result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "release-cc-deferred" : "release-cc-failed");
    if (state) {
        // Every accepted lifecycle teardown remains authoritative even when
        // the tuner temporarily defers or fails the CC return. Keep one latch
        // armed so the next SM tick retries instead of leaving an inert VC.
        state->p25_sm_force_release = 1;
    }
    return 0;
}

static void
p25_release_clear_context(p25_sm_ctx_t* ctx) {
    p25_grant_clear_slot_state(ctx);
    ctx->vc_freq_hz = 0;
    ctx->vc_channel = 0;
    ctx->vc_tg = 0;
    ctx->vc_src = 0;
    ctx->vc_data_call = 0;
    ctx->vc_stale_regrant_probe = 0;
    ctx->vc_stale_regrant_probe_slot = -1;
    ctx->t_tune_m = 0.0;
    ctx->t_voice_m = 0.0;
    ctx->t_hangtime_m = 0.0;
    ctx->vc_activity_seen = 0;
    p25_sm_reset_vc_reacquire_tracking(ctx);
    ctx->release_count++;
    ctx->cc_return_count++;
}

static void
p25_release_clear_decoder_state(dsd_opts* opts, dsd_state* state) {
    if (state) {
        (void)dsd_tg_policy_clear_active_call(state, -1);
        state->p25_p2_audio_allowed[0] = 0;
        state->p25_p2_audio_allowed[1] = 0;
        state->p25_p2_active_slot = -1;
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
        state->payload_algid = 0;
        state->payload_algidR = 0;
        state->payload_keyid = 0;
        state->payload_keyidR = 0;
        state->payload_miP = 0;
        state->payload_miN = 0;
        state->dmr_so = 0;
        state->dmr_soR = 0;
        state->p25_service_options_valid[0] = 0;
        state->p25_service_options_valid[1] = 0;
        p25_crypto_reset_slot(state, 0);
        p25_crypto_reset_slot(state, 1);
        state->p25_policy_tg[0] = 0;
        state->p25_policy_tg[1] = 0;
        state->p25_sm_release_count++;
    }
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
}

static void
p25_release_log_failed_vc(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, const char* reason,
                          int failed_stale_probe) {
    p25_sm_log_vc_reacquire_no_activity(ctx, opts, state, reason);
    if (failed_stale_probe) {
        p25_sm_diagf(opts, state, ctx, "grant_stale_probe_result",
                     "result=no-activity source=%s slot=%d freq=%ld ch=0x%04X", reason ? reason : "release",
                     ctx->vc_stale_regrant_probe_slot, ctx->vc_freq_hz, ctx->vc_channel & 0xFFFF);
    }
}

static int
p25_release_take_force_request(dsd_state* state) {
    if (!state) {
        return 0;
    }
    const int had_force_release = state->p25_sm_force_release != 0;
    state->p25_sm_force_release = 0;
    return had_force_release;
}

#ifdef USE_RADIO
static int
p25_release_hold_for_reacquire(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, const char* reason,
                               int had_force_release) {
    return had_force_release
           && p25_sm_hold_release_for_vc_cqpsk_reacquire(ctx, opts, state, reason, dsd_time_now_monotonic_s());
}
#endif

static int
p25_release_diag_slot(const p25_sm_ctx_t* ctx, const dsd_state* state) {
    int slot = ctx->vc_is_tdma && state ? state->p25_p2_active_slot : 0;
    if (slot < 0 || slot > 1) {
        slot = 0;
    }
    return slot;
}

static void
p25_release_log_channel(const p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, const char* reason) {
    const int slot = p25_release_diag_slot(ctx, state);
    const int src = ctx->slots[slot].src > 0 ? ctx->slots[slot].src : ctx->slots[slot].last_end_src;
    p25_sm_diagf(opts, state, ctx, "channel_release", "phase=%s freq=%ld slot=%d tg=%d src=%d reason=%s",
                 p25_voice_phase_name(ctx), ctx->vc_freq_hz, slot, ctx->slots[slot].target_id, src, reason);
}

static int
p25_release_locked(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    double tune_start_m = 0.0;
    dsd_trunk_tune_result tune_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    uint64_t tune_request_id = 0U;
    const int had_force_release = p25_release_take_force_request(state);
    const int failed_stale_probe = ctx && ctx->vc_stale_regrant_probe;
    const char* safe_reason = reason ? reason : "none";

    if (!p25_release_should_return_to_cc(ctx, opts)) {
        p25_sm_diagf(opts, state, ctx, "release_skip", "reason=%s opts_tuned=%d", safe_reason,
                     opts ? ((opts->trunk_is_tuned == 1) ? 1 : 0) : 0);
        atomic_store(&g_p25_sm_release_lock, 0);
        return 0;
    }

#ifdef USE_RADIO
    // The frame-sync no-sync path is stronger evidence of a stalled VC than a
    // fixed delay after a grant. Give one queued demodulator-only recovery a
    // bounded opportunity to produce decode activity, while leaving the
    // original grant timeout as the hard deadline.
    if (p25_release_hold_for_reacquire(ctx, opts, state, reason, had_force_release)) {
        atomic_store(&g_p25_sm_release_lock, 0);
        return 1;
    }
#endif

    p25_sm_diagf(opts, state, ctx, "release_request", "reason=%s force=%d stale_probe=%d", safe_reason,
                 had_force_release, failed_stale_probe);
    sm_log(opts, state, reason);

    // Return to CC. On failure/defer, leave VC state untouched so the watchdog
    // can retry through the same state machine instead of pretending the tuner moved.
    if (!p25_release_return_to_cc_accepted(ctx, opts, state, had_force_release, &tune_start_m, &tune_result,
                                           &tune_request_id)) {
        atomic_store(&g_p25_sm_release_lock, 0);
        return 0;
    }

    p25_release_log_channel(ctx, opts, state, safe_reason);

    p25_release_log_failed_vc(ctx, opts, state, reason, failed_stale_probe);

    p25_release_clear_context(ctx);
    p25_release_clear_decoder_state(opts, state);
    p25_sm_start_cc_acquisition_for_result(ctx, opts, state, tune_result, tune_request_id, tune_start_m, "release",
                                           P25_SM_CC_ACQUISITION_RETURN);

    // Transition to ON_CC state
    set_state(ctx, opts, state, P25_SM_ON_CC, "release->cc");

    atomic_store(&g_p25_sm_release_lock, 0);
    return 1;
}

static int
do_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason, int recover_stale_ctx) {
    if (!ctx) {
        return 0;
    }

    // Avoid double-return-to-CC thrash if multiple callers attempt to release at
    // nearly the same time (e.g., explicit call termination + watchdog tick).
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_p25_sm_release_lock, &expected, 1)) {
        return 0;
    }

    // Only public release paths normalize externally cleared decoder state.
    if (recover_stale_ctx && p25_release_ctx_is_stale(ctx, opts, state)) {
        const uint32_t tune_count = ctx->tune_count;
        const uint32_t grant_count = ctx->grant_count;
        p25_sm_diagf(opts, state, ctx, "release_stale_reset", "reason=%s", reason ? reason : "none");
        p25_release_clear_context(ctx);
        const uint32_t release_count = ctx->release_count;
        const uint32_t cc_return_count = ctx->cc_return_count;
        p25_release_clear_decoder_state(opts, state);
        p25_sm_init_ctx(ctx, opts, state);
        ctx->tune_count = tune_count;
        ctx->release_count = release_count;
        ctx->grant_count = grant_count;
        ctx->cc_return_count = cc_return_count;
        atomic_store(&g_p25_sm_release_lock, 0);
        return 1;
    }

    return p25_release_locked(ctx, opts, state, reason);
}

/* ============================================================================
 * CC Hunting Helpers
 * ============================================================================ */

// Default hunting interval: try a new candidate every 5 seconds (aligned with op25 CC_HUNT_TIME)
#define CC_HUNT_INTERVAL_S 5.0

// Get next LCN frequency from user-provided list
static int
next_lcn_freq(dsd_state* state, long* out_freq) {
    if (!state || !out_freq) {
        return 0;
    }
    if (state->lcn_freq_count <= 0) {
        long cc = (state->p25_cc_freq != 0) ? state->p25_cc_freq : state->trunk_cc_freq;
        if (cc > 0) {
            *out_freq = cc;
            return 1;
        }
        return 0;
    }
    if (state->lcn_freq_roll >= state->lcn_freq_count) {
        state->lcn_freq_roll = 0;
    }
    // Skip duplicates
    if (state->lcn_freq_roll > 0) {
        long prev = state->trunk_lcn_freq[state->lcn_freq_roll - 1];
        long cur = state->trunk_lcn_freq[state->lcn_freq_roll];
        if (prev == cur) {
            state->lcn_freq_roll++;
            if (state->lcn_freq_roll >= state->lcn_freq_count) {
                state->lcn_freq_roll = 0;
            }
        }
    }
    long f = (state->lcn_freq_roll < state->lcn_freq_count) ? state->trunk_lcn_freq[state->lcn_freq_roll] : 0;
    state->lcn_freq_roll++;
    if (f != 0) {
        *out_freq = f;
        return 1;
    }
    {
        long cc = (state->p25_cc_freq != 0) ? state->p25_cc_freq : state->trunk_cc_freq;
        if (cc > 0) {
            *out_freq = cc;
            return 1;
        }
    }
    return 0;
}

static int
p25_has_user_lcn_list(const dsd_opts* opts, const dsd_state* state) {
    if (opts && opts->chan_in_file[0] != '\0') {
        return 1;
    }
    if (!opts || !state || opts->trunk_scan_enabled != 1) {
        return 0;
    }

    // Trunk-scan seeds entry 0 with the target CC. Additional entries mean a
    // per-target chan_csv was imported; learned grant caches only populate the
    // sparse channel map and must not disable current-site CC candidate hunts.
    return (state->lcn_freq_count > 1) ? 1 : 0;
}

static int
next_primary_cc_freq(dsd_state* state, long* out_freq) {
    if (!state || !out_freq) {
        return 0;
    }
    long cc = (state->p25_cc_freq != 0) ? state->p25_cc_freq : state->trunk_cc_freq;
    if (cc <= 0) {
        return 0;
    }
    *out_freq = cc;
    return 1;
}

// Try tuning to next CC candidate or LCN freq
static void
try_next_cc(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!ctx || !state) {
        return;
    }
    long cand = 0;
    int sps = cc_ted_sps(opts, state);

    const int has_user_lcn_list = p25_has_user_lcn_list(opts, state);

    // In no-import operation, only explicit current-site candidates are eligible
    // before falling back to the known primary CC.
    if (((opts && opts->p25_prefer_candidates == 1) || !has_user_lcn_list)
        && dsd_trunk_cc_candidates_next(state, now_m, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE, &cand)) {
        p25_sm_diagf(opts, state, ctx, "hunt_tune_attempt",
                     "source=current-site-candidate freq=%ld sps=%d in_cand=%d in_lcn=%d in_neighbor=%d", cand, sps,
                     p25_diag_freq_in_current_site_candidates(state, cand), p25_diag_freq_in_lcn_list(state, cand),
                     p25_diag_freq_in_neighbors(state, cand));
        uint64_t tune_request_id = 0U;
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, cand, sps, &tune_request_id);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            p25_sm_diagf(opts, state, ctx, "hunt_tune_result", "source=current-site-candidate freq=%ld result=%s", cand,
                         p25_tune_result_name(tune_result));
            sm_log(opts, state,
                   tune_result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "hunt-cand-deferred" : "hunt-cand-failed");
            return;
        }
        p25_sm_diagf(opts, state, ctx, "hunt_tune_result", "source=current-site-candidate freq=%ld result=%s", cand,
                     p25_tune_result_name(tune_result));
        state->p25_cc_eval_freq = cand;
        state->p25_cc_eval_start_m = 0.0;
        p25_sm_start_cc_acquisition_for_result(ctx, opts, state, tune_result, tune_request_id, now_m, "hunt-cand",
                                               P25_SM_CC_ACQUISITION_HUNT_PROBE);
        set_state(ctx, opts, state, P25_SM_ON_CC, "hunt-cand");
        sm_log(opts, state, "hunt-cand-tune");
        return;
    }

    // Fall back only to an explicit user channel map; otherwise return to the known primary CC.
    if ((has_user_lcn_list ? next_lcn_freq(state, &cand) : next_primary_cc_freq(state, &cand))) {
        const char* source = has_user_lcn_list ? "user-lcn" : "primary-cc";
        p25_sm_diagf(opts, state, ctx, "hunt_tune_attempt",
                     "source=%s freq=%ld sps=%d in_cand=%d in_lcn=%d in_neighbor=%d", source, cand, sps,
                     p25_diag_freq_in_current_site_candidates(state, cand), p25_diag_freq_in_lcn_list(state, cand),
                     p25_diag_freq_in_neighbors(state, cand));
        uint64_t tune_request_id = 0U;
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, cand, sps, &tune_request_id);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            p25_sm_diagf(opts, state, ctx, "hunt_tune_result", "source=%s freq=%ld result=%s", source, cand,
                         p25_tune_result_name(tune_result));
            sm_log(opts, state,
                   tune_result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "hunt-lcn-deferred" : "hunt-lcn-failed");
            return;
        }
        p25_sm_diagf(opts, state, ctx, "hunt_tune_result", "source=%s freq=%ld result=%s", source, cand,
                     p25_tune_result_name(tune_result));
        p25_sm_start_cc_acquisition_for_result(ctx, opts, state, tune_result, tune_request_id, now_m, "hunt-lcn",
                                               P25_SM_CC_ACQUISITION_HUNT_PROBE);
        set_state(ctx, opts, state, P25_SM_ON_CC, "hunt-lcn");
        sm_log(opts, state, "hunt-lcn-tune");
        return;
    }

    // No candidates - stay in HUNTING and wait for CC_SYNC
    p25_sm_diagf(opts, state, ctx, "hunt_no_candidate", "has_user_lcn=%d prefer_candidates=%d", has_user_lcn_list,
                 opts ? opts->p25_prefer_candidates : 0);
}

/* ============================================================================
 * Public API - Core State Machine
 * ============================================================================ */

const char*
p25_sm_state_name(p25_sm_state_e state) {
    switch (state) {
        case P25_SM_IDLE: return "IDLE";
        case P25_SM_ON_CC: return "ON_CC";
        case P25_SM_TUNED: return "TUNED";
        case P25_SM_HUNTING: return "HUNT";
        default: return "?";
    }
}

void
p25_sm_init_ctx(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }

    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->vc_stale_regrant_probe_slot = -1;

    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();

    // Set defaults (aligned with op25 timing parameters)
    ctx->config.hangtime_s = 2.0;      // op25: TGID_HOLD_TIME = 2.0s
    ctx->config.grant_timeout_s = 3.0; // op25: TSYS_HOLD_TIME = 3.0s
    ctx->config.cc_grace_s = 5.0;      // op25: CC_HUNT_TIME = 5.0s

    // Override from opts if available (including zero for immediate release)
    if (opts) {
        if (opts->trunk_hangtime >= 0.0) {
            ctx->config.hangtime_s = opts->trunk_hangtime;
        }
        if (opts->p25_grant_voice_to_s > 0.0) {
            ctx->config.grant_timeout_s = opts->p25_grant_voice_to_s;
        }
    }

    // Override from runtime config (env/config)
    if (cfg && cfg->p25_hangtime_is_set) {
        ctx->config.hangtime_s = cfg->p25_hangtime_s;
    }
    if (cfg && cfg->p25_grant_timeout_is_set) {
        ctx->config.grant_timeout_s = cfg->p25_grant_timeout_s;
    }
    if (cfg && cfg->p25_cc_grace_is_set) {
        ctx->config.cc_grace_s = cfg->p25_cc_grace_s;
    }

    // Set initial state based on CC presence
    if (state && state->p25_cc_freq != 0) {
        ctx->state = P25_SM_ON_CC;
        // Sync CC timestamp from state
        if (state->last_cc_sync_time_m > 0.0) {
            ctx->t_cc_sync_m = state->last_cc_sync_time_m;
        } else {
            ctx->t_cc_sync_m = dsd_time_now_monotonic_s();
        }
        state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC;
        p25_sm_set_expected_cc_nac(ctx, state, 0);
    } else {
        ctx->state = P25_SM_IDLE;
        if (state) {
            state->p25_sm_mode = DSD_P25_SM_MODE_UNKNOWN;
        }
    }

    ctx->initialized = 1;
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "init",
                 "cc_grace=%.3f hangtime=%.3f grant_timeout=%.3f expected_nac=0x%03X", ctx->config.cc_grace_s,
                 ctx->config.hangtime_s, ctx->config.grant_timeout_s, ctx->expected_cc_nac);
}

static int
p25_sm_vc_sync_slot(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot) {
    if (!ctx) {
        return -1;
    }
    if (slot >= 0 && slot <= 1) {
        return slot;
    }
    if (!ctx->vc_is_tdma) {
        return 0;
    }
    if (state && state->p25_p2_active_slot >= 0 && state->p25_p2_active_slot <= 1) {
        return state->p25_p2_active_slot;
    }

    int candidate = -1;
    for (int s = 0; s < 2; s++) {
        if (!ctx->slots[s].grant_active || ctx->slots[s].data_call) {
            continue;
        }
        if (candidate >= 0) {
            return -1;
        }
        candidate = s;
    }
    return candidate;
}

static void
p25_sm_handle_vc_sync_event(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, int slot) {
    if (!ctx || ctx->state != P25_SM_TUNED) {
        return;
    }
    double now_m = dsd_time_now_monotonic_s();
    int sync_slot = p25_sm_vc_sync_slot(ctx, state, slot);
    p25_sm_note_vc_decode_activity(ctx, (dsd_opts*)opts, state, "sync", sync_slot, now_m);
    ctx->t_voice_m = now_m;
    if (sync_slot >= 0 && sync_slot <= 1 && ctx->slots[sync_slot].grant_active && !ctx->slots[sync_slot].data_call) {
        ctx->slots[sync_slot].last_active_m = now_m;
    }
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "vc_sync", "freq=%ld ch=0x%04X tg=%d tdma=%d slot=%d", ctx->vc_freq_hz,
                 ctx->vc_channel & 0xFFFF, ctx->vc_tg, ctx->vc_is_tdma, sync_slot);
#ifdef USE_RADIO
    /* Learn successful TDMA VC acquisition only for the OP25-style CQPSK timing
     * chain. */
    if (opts && state && ctx->vc_is_tdma && opts->audio_in_type == AUDIO_IN_RTL) {
        int cqpsk = 0;
        int timing = 0;
        dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk, &timing);
        if (cqpsk && timing) {
            state->p25_vc_cqpsk_pref = 1;
        }
    }
#else
    (void)opts;
    (void)state;
#endif
}

static void
p25_sm_handle_event_grant(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    handle_grant(ctx, (dsd_opts*)opts, state, ev);
}

static void
p25_sm_handle_event_ptt(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    handle_voice_start(ctx, (dsd_opts*)opts, state, ev, "ptt");
}

static void
p25_sm_handle_event_active(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    handle_voice_start(ctx, (dsd_opts*)opts, state, ev, "active");
}

static void
p25_sm_handle_event_end(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (ev->facch) {
        (void)handle_facch_voice_end(ctx, (dsd_opts*)opts, state, ev);
        return;
    }
    (void)handle_voice_end(ctx, (dsd_opts*)opts, state, ev->slot, "end", 1, 1, ev);
}

static void
p25_sm_handle_event_idle(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    // MAC_IDLE may occur during brief gaps - use hangtime, not immediate release.
    (void)handle_voice_end(ctx, (dsd_opts*)opts, state, ev->slot, "idle", 0, 0, ev);
}

static void
p25_sm_handle_event_tdu(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    (void)handle_voice_end(ctx, (dsd_opts*)opts, state, 0, "tdu", 0, 1, ev);
}

static void
p25_sm_handle_event_hangtime(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    (void)handle_voice_end(ctx, (dsd_opts*)opts, state, ev->slot, "mac-hangtime", 0, 0, ev);
}

static void
p25_sm_handle_event_cc_sync(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    (void)ev;
    handle_cc_sync(ctx, opts, state);
}

static void
p25_sm_handle_event_vc_sync(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    p25_sm_handle_vc_sync_event(ctx, opts, state, ev ? ev->slot : -1);
}

static void
p25_sm_handle_event_sync_lost(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    (void)ctx;
    (void)opts;
    (void)state;
    (void)ev;
    // Handled in tick based on timeouts.
}

static void
p25_sm_handle_event_enc(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    handle_enc(ctx, (dsd_opts*)opts, state, ev);
}

static void
p25_sm_handle_event_crypto_pending(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state,
                                   const p25_sm_event_t* ev) {
    handle_crypto_pending(ctx, (dsd_opts*)opts, state, ev);
}

typedef void (*p25_sm_event_handler_fn)(p25_sm_ctx_t*, const dsd_opts*, dsd_state*, const p25_sm_event_t*);

static const p25_sm_event_handler_fn g_p25_sm_event_handlers[] = {
    [P25_SM_EV_GRANT] = p25_sm_handle_event_grant,
    [P25_SM_EV_PTT] = p25_sm_handle_event_ptt,
    [P25_SM_EV_ACTIVE] = p25_sm_handle_event_active,
    [P25_SM_EV_END] = p25_sm_handle_event_end,
    [P25_SM_EV_IDLE] = p25_sm_handle_event_idle,
    [P25_SM_EV_TDU] = p25_sm_handle_event_tdu,
    [P25_SM_EV_HANGTIME] = p25_sm_handle_event_hangtime,
    [P25_SM_EV_CC_SYNC] = p25_sm_handle_event_cc_sync,
    [P25_SM_EV_VC_SYNC] = p25_sm_handle_event_vc_sync,
    [P25_SM_EV_SYNC_LOST] = p25_sm_handle_event_sync_lost,
    [P25_SM_EV_ENC] = p25_sm_handle_event_enc,
    [P25_SM_EV_CRYPTO_PENDING] = p25_sm_handle_event_crypto_pending,
};

static int
p25_sm_tick_handle_forced_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx || !state || state->p25_sm_force_release == 0) {
        return 0;
    }
    if (ctx->state == P25_SM_TUNED) {
        do_release(ctx, opts, state, "release-forced", 0);
    } else {
        state->p25_sm_force_release = 0;
    }
    return 1;
}

static void
p25_sm_tick_on_cc_sync_from_state(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state) {
    (void)p25_sm_refresh_cc_sync_from_state(ctx, opts, state, "tick");
}

#ifdef USE_RADIO
static int
p25_sm_cc_reacquire_context_ready(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state) {
    if (!ctx || !opts || !state) {
        return 0;
    }
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return 0;
    }
    if (ctx->cc_reacquire_attempted || !ctx->cc_sync_pending || ctx->cc_tune_pending) {
        return 0;
    }
    if (ctx->t_cc_tune_m <= 0.0 || ctx->cc_acquisition_origin != P25_SM_CC_ACQUISITION_RETURN) {
        return 0;
    }
    return 1;
}
#endif

static void
p25_sm_tick_try_cc_reacquire(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, double now_m, double cc_grace) {
#ifdef USE_RADIO
    if (!p25_sm_cc_reacquire_context_ready(ctx, opts, state)) {
        return;
    }

    const double acquire_grace = p25_sm_cc_acquire_grace_s(ctx, cc_grace);
    const double elapsed = now_m - ctx->t_cc_tune_m;
    const double remaining = acquire_grace - elapsed;
    if (ctx->cc_no_sync_passes < P25_CC_RETURN_REACQUIRE_NO_SYNC_PASSES || remaining <= 0.0) {
        return;
    }

    int cqpsk = 0;
    int timing = 0;
    (void)dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk, &timing);
    if (!cqpsk) {
        return;
    }

    const uint32_t generation = dsd_rtl_stream_metrics_hook_stream_generation();
    const double snr_db = dsd_rtl_stream_metrics_hook_snr_cqpsk_db();
    ctx->cc_reacquire_attempted = 1;
    const int request_rc = dsd_rtl_stream_metrics_hook_request_cqpsk_reacquire();
    if (request_rc > 0) {
        ctx->t_cc_reacquire_m = now_m;
    }
    const double no_sync_span = ctx->t_cc_first_no_sync_m > 0.0 ? now_m - ctx->t_cc_first_no_sync_m : 0.0;
    const char* result = request_rc > 0 ? "queued" : (request_rc == 0 ? "inactive" : "unavailable");
    p25_sm_diagf(opts, state, ctx, "cc_reacquire_request",
                 "trigger=frame-sync-no-progress result=%s rc=%d elapsed=%.3f remaining=%.3f no_sync_passes=%u "
                 "no_sync_span=%.3f cqpsk=%d timing=%d snr_db=%.3f generation=%u",
                 result, request_rc, elapsed, remaining, ctx->cc_no_sync_passes, no_sync_span, cqpsk, timing, snr_db,
                 generation);
#else
    (void)ctx;
    (void)opts;
    (void)state;
    (void)now_m;
    (void)cc_grace;
#endif
}

static int
p25_sm_tick_on_cc_nac_mismatch(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!ctx || !state || !p25_sm_valid_nac_int(ctx->expected_cc_nac) || !p25_sm_valid_nac_int(state->nac)) {
        if (ctx) {
            ctx->nac_mismatch_count = 0;
        }
        return 0;
    }
    if (state->nac == ctx->expected_cc_nac) {
        ctx->nac_mismatch_count = 0;
        return 0;
    }
    ctx->nac_mismatch_count++;
    if (ctx->nac_mismatch_count < 3) {
        return 0;
    }
    if (opts && opts->verbose > 0) {
        DSD_FPRINTF(stderr, "\n[P25 SM] NAC mismatch: expected 0x%03llX, got 0x%03X (%d consecutive)\n",
                    (unsigned long long)ctx->expected_cc_nac, state->nac, ctx->nac_mismatch_count);
    }
    p25_sm_diagf(opts, state, ctx, "cc_lost", "reason=nac-mismatch expected=0x%03X got=0x%03X consecutive=%d",
                 ctx->expected_cc_nac, state->nac, ctx->nac_mismatch_count);
    ctx->nac_mismatch_count = 0;
    p25_sm_reset_cc_reacquire_tracking(ctx);
    set_state(ctx, opts, state, P25_SM_HUNTING, "cc-lost-nac-mismatch");
    ctx->t_hunt_try_m = now_m;
    try_next_cc(ctx, opts, state, now_m);
    return 1;
}

static void
p25_sm_tick_on_cc_eval_cooldown(const p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    const double eval_window_s = 3.0;
    double eval_dt = 0.0;
    double cc_ts = 0.0;
    if (!ctx || !state || state->p25_cc_eval_freq == 0) {
        return;
    }
    // Return acquisition owns the longer deadline and applies cooldown on timeout.
    if (ctx->cc_sync_pending && ctx->t_cc_tune_m > 0.0 && ctx->cc_acquisition_origin == P25_SM_CC_ACQUISITION_RETURN) {
        return;
    }
    eval_dt = (state->p25_cc_eval_start_m > 0.0) ? (now_m - state->p25_cc_eval_start_m) : 0.0;
    if (eval_dt < eval_window_s) {
        return;
    }

    cc_ts = ctx->t_cc_sync_m;
    if (state->last_cc_sync_time_m > 0.0 && state->last_cc_sync_time_m < cc_ts) {
        cc_ts = state->last_cc_sync_time_m;
    }
    if (cc_ts <= 0.0 || (now_m - cc_ts) >= eval_window_s) {
        dsd_trunk_cc_candidates_set_cooldown(state, state->p25_cc_eval_freq, now_m + 10.0);
        p25_sm_diagf(opts, state, ctx, "hunt_candidate_cooldown", "freq=%ld seconds=10.000 eval_dt=%.3f",
                     state->p25_cc_eval_freq, eval_dt);
    }
    state->p25_cc_eval_freq = 0;
    state->p25_cc_eval_start_m = 0.0;
}

static int
p25_sm_tick_on_cc_is_lost(const p25_sm_ctx_t* ctx, const dsd_state* state, double now_m, double cc_grace,
                          int* out_acquire_timeout) {
    double cc_ts = 0.0;
    if (out_acquire_timeout) {
        *out_acquire_timeout = 0;
    }
    if (!ctx) {
        return 0;
    }
    if (ctx->cc_tune_pending) {
        return 0;
    }
    if (ctx->cc_sync_pending && ctx->t_cc_tune_m > 0.0) {
        const double acquire_grace = p25_sm_cc_acquire_grace_s(ctx, cc_grace);
        if ((now_m - ctx->t_cc_tune_m) > acquire_grace) {
            if (out_acquire_timeout) {
                *out_acquire_timeout = 1;
            }
            return 1;
        }
    }
    cc_ts = ctx->t_cc_sync_m;
    if (state) {
        if (state->last_cc_sync_time_m <= 0.0) {
            cc_ts = 0.0;
        } else if (state->last_cc_sync_time_m < cc_ts) {
            cc_ts = state->last_cc_sync_time_m;
        }
    }
    if (cc_ts <= 0.0 && ctx->t_cc_sync_m > 0.0) {
        return 1;
    }
    if (cc_ts > 0.0 && (now_m - cc_ts) > cc_grace) {
        return 1;
    }
    return 0;
}

static void
p25_sm_tick_on_cc(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double cc_grace) {
    int acquire_timeout = 0;
    int tune_status = p25_sm_resolve_pending_cc_tune(ctx, opts, state);
    if (tune_status == 0) {
        return;
    }
    if (tune_status < 0) {
        if (state && state->p25_cc_eval_freq != 0) {
            dsd_trunk_cc_candidates_set_cooldown(state, state->p25_cc_eval_freq, now_m + 10.0);
            state->p25_cc_eval_freq = 0;
            state->p25_cc_eval_start_m = 0.0;
        }
        ctx->t_hunt_try_m = now_m;
        try_next_cc(ctx, opts, state, now_m);
        return;
    }
    p25_sm_tick_on_cc_sync_from_state(ctx, opts, state);
    p25_sm_tick_try_cc_reacquire(ctx, opts, state, now_m, cc_grace);
    if (p25_sm_tick_on_cc_nac_mismatch(ctx, opts, state, now_m)) {
        return;
    }
    p25_sm_tick_on_cc_eval_cooldown(ctx, opts, state, now_m);
    if (!p25_sm_tick_on_cc_is_lost(ctx, state, now_m, cc_grace, &acquire_timeout)) {
        return;
    }
    if (acquire_timeout) {
        const p25_sm_cc_acquisition_origin_e origin = ctx->cc_acquisition_origin;
        p25_sm_diagf(opts, state, ctx, "cc_lost",
                     "reason=acquire-timeout origin=%s effective_grace=%.3f cc_grace=%.3f tune_m=%.3f "
                     "last_cc_m=%.3f decoded_cc_m=%.3f now_m=%.3f",
                     p25_sm_cc_acquisition_origin_name(origin), p25_sm_cc_acquire_grace_s(ctx, cc_grace), cc_grace,
                     ctx->t_cc_tune_m, state ? state->last_cc_sync_time_m : 0.0,
                     state ? state->p25_last_cc_msg_time_m : 0.0, now_m);
        if (state && state->p25_cc_eval_freq != 0) {
            dsd_trunk_cc_candidates_set_cooldown(state, state->p25_cc_eval_freq, now_m + 10.0);
            p25_sm_diagf(opts, state, ctx, "hunt_candidate_cooldown", "freq=%ld seconds=10.000 reason=acquire-timeout",
                         state->p25_cc_eval_freq);
            state->p25_cc_eval_freq = 0;
            state->p25_cc_eval_start_m = 0.0;
        }
    } else {
        p25_sm_diagf(opts, state, ctx, "cc_lost",
                     "reason=timeout cc_grace=%.3f last_cc_m=%.3f decoded_cc_m=%.3f now_m=%.3f", cc_grace,
                     state ? state->last_cc_sync_time_m : 0.0, state ? state->p25_last_cc_msg_time_m : 0.0, now_m);
    }
    p25_sm_reset_cc_reacquire_tracking(ctx);
    set_state(ctx, opts, state, P25_SM_HUNTING, "cc-lost");
    ctx->t_hunt_try_m = now_m;
    try_next_cc(ctx, opts, state, now_m);
}

static double
p25_sm_effective_hangtime(const dsd_state* state, double hangtime) {
    if (!state || hangtime <= 0.0) {
        return hangtime;
    }
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    double thr_pct = (cfg && cfg->p25p1_err_hold_pct_is_set) ? cfg->p25p1_err_hold_pct : 0.0;
    double add_s = (cfg && cfg->p25p1_err_hold_s_is_set) ? cfg->p25p1_err_hold_s : 0.0;
    if (thr_pct > 0.0 && add_s > 0.0 && state->p25_p1_voice_err_hist_len > 0) {
        double avg = (double)state->p25_p1_voice_err_hist_sum / (double)state->p25_p1_voice_err_hist_len;
        if (avg >= thr_pct) {
            return hangtime + add_s;
        }
    }
    return hangtime;
}

static void
p25_sm_tick_tuned_check_hangtime(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double hangtime) {
    double dt_hang = 0.0;
    double effective_hangtime = 0.0;
    if (!ctx || ctx->t_hangtime_m <= 0.0) {
        return;
    }
    dt_hang = now_m - ctx->t_hangtime_m;
    effective_hangtime = p25_sm_effective_hangtime(state, hangtime);
    if (dt_hang >= effective_hangtime) {
        int slot = ctx->vc_is_tdma && state ? state->p25_p2_active_slot : 0;
        if (slot < 0 || slot > 1) {
            slot = 0;
        }
        p25_sm_diagf(opts, state, ctx, "hang_expired",
                     "phase=%s freq=%ld slot=%d tg=%d src=%d reason=inactivity elapsed=%.3f hangtime=%.3f",
                     p25_voice_phase_name(ctx), ctx->vc_freq_hz, slot, ctx->slots[slot].target_id,
                     ctx->slots[slot].src > 0 ? ctx->slots[slot].src : ctx->slots[slot].last_end_src, dt_hang,
                     effective_hangtime);
        do_release(ctx, opts, state, "hangtime-expired", 0);
    }
}

#ifdef USE_RADIO
static double
p25_vc_cqpsk_reacquire_timeout_start_m(const p25_sm_ctx_t* ctx, const dsd_state* state) {
    double timeout_start_m = p25_sm_pending_voice_grant_timeout_start_m(ctx, state);
    if (ctx && ctx->t_tune_m > timeout_start_m) {
        timeout_start_m = ctx->t_tune_m;
    }
    return timeout_start_m;
}

static int
p25_vc_cqpsk_reacquire_timeout_expired(const p25_sm_ctx_t* ctx, const dsd_state* state, double now_m) {
    if (!ctx || ctx->config.grant_timeout_s <= 0.0) {
        return 0;
    }
    const double timeout_start_m = p25_vc_cqpsk_reacquire_timeout_start_m(ctx, state);
    return timeout_start_m > 0.0 && (now_m - timeout_start_m) >= ctx->config.grant_timeout_s;
}

static int
p25_vc_cqpsk_reacquire_tune_current(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state) {
    if (!ctx || !opts || !state) {
        return 0;
    }
    if (opts->trunk_enable != 1 || opts->trunk_is_tuned != 1 || ctx->vc_freq_hz <= 0
        || opts->audio_in_type != AUDIO_IN_RTL) {
        return 0;
    }
    return (state->p25_vc_freq[0] == ctx->vc_freq_hz || state->p25_vc_freq[1] == ctx->vc_freq_hz
            || state->trunk_vc_freq[0] == ctx->vc_freq_hz || state->trunk_vc_freq[1] == ctx->vc_freq_hz)
               ? 1
               : 0;
}

static int
p25_vc_cqpsk_reacquire_waiting_voice(const p25_sm_ctx_t* ctx, const dsd_state* state) {
    if (ctx->state != P25_SM_TUNED || !ctx->vc_is_tdma || ctx->vc_data_call || !ctx->vc_reacquire_eligible
        || ctx->vc_reacquire_attempted) {
        return 0;
    }
    if (ctx->t_voice_m > 0.0 || ctx->slots[0].voice_active || ctx->slots[1].voice_active
        || !p25_sm_has_pending_voice_grant(ctx, state)) {
        return 0;
    }
    return 1;
}

static int
p25_vc_cqpsk_reacquire_candidate(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, double now_m) {
    if (!p25_vc_cqpsk_reacquire_tune_current(ctx, opts, state) || !p25_vc_cqpsk_reacquire_waiting_voice(ctx, state)) {
        return 0;
    }
    return p25_vc_cqpsk_reacquire_timeout_expired(ctx, state, now_m) ? 0 : 1;
}

static int
p25_sm_try_vc_cqpsk_reacquire(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state, double now_m,
                              const char* trigger) {
    if (!p25_vc_cqpsk_reacquire_candidate(ctx, opts, state, now_m)) {
        return 0;
    }

    int cqpsk = 0;
    int timing = 0;
    (void)dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk, &timing);
    if (!cqpsk) {
        return 0;
    }

    const uint32_t generation = dsd_rtl_stream_metrics_hook_stream_generation();
    const double snr_db = dsd_rtl_stream_metrics_hook_snr_cqpsk_db();
    ctx->vc_reacquire_attempted = 1;
    const int request_rc = dsd_rtl_stream_metrics_hook_request_cqpsk_reacquire();
    if (request_rc > 0) {
        ctx->t_vc_reacquire_m = now_m;
    }
    const double timeout_start_m = p25_vc_cqpsk_reacquire_timeout_start_m(ctx, state);
    const double elapsed = timeout_start_m > 0.0 ? now_m - timeout_start_m : 0.0;
    const double remaining = ctx->config.grant_timeout_s > 0.0 ? ctx->config.grant_timeout_s - elapsed : -1.0;
    const double no_sync_span = ctx->t_vc_first_no_sync_m > 0.0 ? now_m - ctx->t_vc_first_no_sync_m : 0.0;
    const char* result = request_rc > 0 ? "queued" : (request_rc == 0 ? "inactive" : "unavailable");
    p25_sm_diagf(opts, state, ctx, "vc_reacquire_request",
                 "trigger=%s result=%s rc=%d elapsed=%.3f remaining=%.3f no_sync_passes=%u no_sync_span=%.3f "
                 "cqpsk=%d timing=%d snr_db=%.3f generation=%u pref=%d freq=%ld ch=0x%04X",
                 trigger ? trigger : "unknown", result, request_rc, elapsed, remaining, ctx->vc_no_sync_passes,
                 no_sync_span, cqpsk, timing, snr_db, generation, state->p25_vc_cqpsk_pref, ctx->vc_freq_hz,
                 ctx->vc_channel & 0xFFFF);
    return request_rc > 0 ? 1 : 0;
}

void
p25_sm_note_vc_no_sync_pass(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state) {
    const double now_m = dsd_time_now_monotonic_s();
    if (!p25_vc_cqpsk_reacquire_candidate(ctx, opts, state, now_m) || ctx->t_tune_m <= 0.0 || now_m < ctx->t_tune_m) {
        return;
    }

    if (ctx->vc_no_sync_passes < UINT32_MAX) {
        ctx->vc_no_sync_passes++;
    }
    if (ctx->t_vc_first_no_sync_m <= 0.0) {
        ctx->t_vc_first_no_sync_m = now_m;
    }

    const double elapsed_tune = now_m - ctx->t_tune_m;
    if (ctx->vc_no_sync_passes < P25_VC_CQPSK_REACQUIRE_NO_SYNC_PASSES
        || elapsed_tune < P25_VC_CQPSK_REACQUIRE_MIN_DELAY_S) {
        return;
    }

    const double timeout_start_m = p25_vc_cqpsk_reacquire_timeout_start_m(ctx, state);
    const double elapsed = timeout_start_m > 0.0 ? now_m - timeout_start_m : 0.0;
    const double remaining = ctx->config.grant_timeout_s > 0.0 ? ctx->config.grant_timeout_s - elapsed : -1.0;
    if (remaining >= 0.0 && remaining < P25_VC_CQPSK_REACQUIRE_MIN_REMAINING_S) {
        return;
    }

    (void)p25_sm_try_vc_cqpsk_reacquire(ctx, opts, state, now_m, "frame-sync-no-progress");
}

static int
p25_sm_hold_release_for_vc_cqpsk_reacquire(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state,
                                           const char* reason, double now_m) {
    if (!reason || strcmp(reason, "frame-sync-no-sync") != 0) {
        return 0;
    }

    if (ctx && ctx->t_vc_reacquire_m > 0.0) {
        return p25_sm_vc_reacquire_hold_active(ctx, opts, state, now_m);
    }

    return p25_sm_try_vc_cqpsk_reacquire(ctx, opts, state, now_m, "frame-sync-no-sync");
}

static int
p25_cqpsk_retry_candidate(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, double dt_tune) {
    if (!ctx || !opts || !state || opts->audio_in_type != AUDIO_IN_RTL) {
        return 0;
    }
    if (!ctx->vc_is_tdma || ctx->vc_cqpsk_retry_done || dt_tune < P25_VC_CQPSK_MODE_RETRY_DELAY_S
        || state->p25_vc_cqpsk_pref == 1) {
        return 0;
    }
    return 1;
}

static int
p25_cqpsk_retry_runtime_allowed(p25_sm_ctx_t* ctx, long vc_freq_hz) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init();
        cfg = dsd_neo_get_config();
    }
    if ((cfg && cfg->cqpsk_is_set) || vc_freq_hz <= 0) {
        return 0;
    }

    int cqpsk = 0;
    dsd_rtl_stream_metrics_hook_cqpsk_status(&cqpsk, NULL);
    if (!cqpsk) {
        return 1;
    }
    ctx->vc_cqpsk_retry_done = 1;
    return 0;
}

static int
p25_cqpsk_retry_tune(const p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int ted_sps) {
    int prev_override = state->p25_vc_cqpsk_override;
    int prev_sps = state->samplesPerSymbol;
    int prev_center = state->symbolCenter;
    state->p25_vc_cqpsk_override = 1;
    state->samplesPerSymbol = ted_sps;
    state->symbolCenter = dsd_opts_symbol_center(ted_sps);
    p25_sm_diagf(opts, state, ctx, "cqpsk_retry_attempt", "freq=%ld ch=0x%04X sps=%d", ctx->vc_freq_hz,
                 ctx->vc_channel & 0xFFFF, ted_sps);
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, ctx->vc_freq_hz, ted_sps, NULL);
    if (dsd_trunk_tune_result_is_ok(tune_result)) {
        p25_sm_diagf(opts, state, ctx, "cqpsk_retry_result", "freq=%ld result=%s", ctx->vc_freq_hz,
                     p25_tune_result_name(tune_result));
        return 1;
    }

    state->p25_vc_cqpsk_override = prev_override;
    state->samplesPerSymbol = prev_sps;
    state->symbolCenter = prev_center;
    p25_sm_diagf(opts, state, ctx, "cqpsk_retry_result", "freq=%ld result=%s", ctx->vc_freq_hz,
                 p25_tune_result_name(tune_result));
    sm_log(opts, state, tune_result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "cqpsk-retry-deferred" : "cqpsk-retry-failed");
    return 0;
}
#else
void
p25_sm_note_vc_no_sync_pass(p25_sm_ctx_t* ctx, dsd_opts* opts, const dsd_state* state) {
    (void)ctx;
    (void)opts;
    (void)state;
}
#endif

int
p25_sm_vc_reacquire_hold_active(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, double now_m) {
#ifdef USE_RADIO
    if (!ctx || !opts || !state || opts->audio_in_type != AUDIO_IN_RTL || ctx->state != P25_SM_TUNED
        || ctx->vc_freq_hz <= 0 || ctx->t_vc_reacquire_m <= 0.0 || now_m < ctx->t_vc_reacquire_m) {
        return 0;
    }
    if (state->p25_vc_freq[0] != ctx->vc_freq_hz && state->p25_vc_freq[1] != ctx->vc_freq_hz) {
        return 0;
    }
    if (p25_vc_cqpsk_reacquire_timeout_expired(ctx, state, now_m)) {
        return 0;
    }
    return (now_m - ctx->t_vc_reacquire_m) < P25_VC_CQPSK_REACQUIRE_HOLD_S ? 1 : 0;
#else
    (void)ctx;
    (void)opts;
    (void)state;
    (void)now_m;
    return 0;
#endif
}

static void
p25_sm_tick_try_cqpsk_retry(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double dt_tune) {
#ifdef USE_RADIO
    if (!p25_cqpsk_retry_candidate(ctx, opts, state, dt_tune)) {
        return;
    }

    if (!p25_cqpsk_retry_runtime_allowed(ctx, ctx->vc_freq_hz)) {
        return;
    }

    int ted_sps = p25_ted_sps_for_bw(opts, 6000);
    if (!p25_cqpsk_retry_tune(ctx, opts, state, ted_sps)) {
        return;
    }
    ctx->vc_cqpsk_retry_done = 1;
    // Restart the tune and per-slot classification timers for the new attempt.
    ctx->t_tune_m = now_m;
    const int slot_count = ctx->vc_is_tdma ? 2 : 1;
    for (int slot = 0; slot < slot_count; slot++) {
        if (ctx->slots[slot].grant_active && !ctx->slots[slot].data_call
            && state->p25_crypto_state[slot] == DSD_P25_CRYPTO_ENCRYPTED_PENDING) {
            ctx->slots[slot].crypto_attempt_m = now_m;
        }
    }
    ctx->t_voice_m = 0.0;
    p25_sm_clear_slot_activity(ctx);
    sm_log(opts, state, "cqpsk-retry-on");
#else
    (void)ctx;
    (void)opts;
    (void)state;
    (void)now_m;
    (void)dt_tune;
#endif
}

static void
p25_sm_tick_tuned_try_cqpsk_retry(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!ctx || ctx->t_tune_m <= 0.0 || ctx->slots[0].voice_active || ctx->slots[1].voice_active) {
        return;
    }
    if (ctx->t_voice_m > 0.0 && !p25_sm_has_pending_voice_grant(ctx, state) && !p25_sm_has_pending_data_grant(ctx)) {
        return;
    }
    const double dt_tune = now_m - ctx->t_tune_m;
    p25_sm_tick_try_cqpsk_retry(ctx, opts, state, now_m, dt_tune);
}

static void
p25_sm_tick_tuned_wait_voice(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double grant_timeout) {
    double timeout_start_m = 0.0;
    if (!ctx) {
        return;
    }

    timeout_start_m = p25_sm_pending_voice_grant_timeout_start_m(ctx, state);
    if (ctx->t_tune_m > timeout_start_m) {
        timeout_start_m = ctx->t_tune_m;
    }
    if (timeout_start_m <= 0.0) {
        return;
    }
    if ((now_m - timeout_start_m) >= grant_timeout) {
        do_release(ctx, opts, state, "grant-timeout", 0);
    }
}

static int
p25_sm_crypto_slot_expired(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot, double now_m,
                           double grant_timeout) {
    if (ctx->slots[slot].data_call || state->p25_crypto_state[slot] != DSD_P25_CRYPTO_ENCRYPTED_PENDING) {
        return 0;
    }
    if (ctx->vc_is_tdma && !ctx->slots[slot].grant_active) {
        return 0;
    }
    const double started_m = ctx->slots[slot].crypto_attempt_m;
    return started_m > 0.0 && (now_m - started_m) >= grant_timeout;
}

static void
p25_sm_block_expired_crypto_slot(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot) {
    p25_crypto_block_pending(state, slot);
    ctx->slots[slot].voice_active = 0;
    if (ctx->vc_is_tdma) {
        p25_enc_lockout_clear_slot_grant(ctx, state, slot);
    }
    p25_sm_diagf(opts, state, ctx, "crypto_classification_timeout", "slot=%d freq=%ld tg=%d", slot, ctx->vc_freq_hz,
                 ctx->vc_tg);
    sm_log(opts, state, "crypto-classify-timeout");
}

static int
p25_sm_expired_slot_has_active_companion(const p25_sm_ctx_t* ctx, const dsd_state* state, const int expired[2]) {
    if (!ctx->vc_is_tdma) {
        return 0;
    }
    for (int slot = 0; slot < 2; slot++) {
        if (expired[slot] && p25_enc_lockout_other_slot_active(ctx, state, slot ^ 1)) {
            return 1;
        }
    }
    return 0;
}

static int
p25_sm_tick_crypto_classification(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m,
                                  double grant_timeout) {
    if (!ctx || !opts || !state || opts->trunk_tune_enc_calls != 0) {
        return 0;
    }

    int expired[2] = {0, 0};
    int expired_any = 0;
    const int slot_count = ctx->vc_is_tdma ? 2 : 1;
    for (int slot = 0; slot < slot_count; slot++) {
        if (!p25_sm_crypto_slot_expired(ctx, state, slot, now_m, grant_timeout)) {
            continue;
        }

        expired[slot] = 1;
        expired_any = 1;
        p25_sm_block_expired_crypto_slot(ctx, opts, state, slot);
    }

    if (!expired_any) {
        return 0;
    }
    if (p25_sm_expired_slot_has_active_companion(ctx, state, expired)) {
        return 0;
    }

    do_release(ctx, opts, state, "grant-timeout", 0);
    return 1;
}

static void
p25_sm_tick_tuned(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double hangtime,
                  double grant_timeout) {
    if (!ctx) {
        return;
    }
    p25_sm_tick_tuned_try_cqpsk_retry(ctx, opts, state, now_m);
    if (p25_sm_tick_crypto_classification(ctx, opts, state, now_m, grant_timeout)) {
        return;
    }
    if (ctx->slots[0].voice_active || ctx->slots[1].voice_active) {
        ctx->t_voice_m = now_m;
        p25_sm_update_ui_mode(ctx, state);
        return;
    }
    if (ctx->t_hangtime_m > 0.0) {
        p25_sm_update_ui_mode(ctx, state);
        p25_sm_tick_tuned_check_hangtime(ctx, opts, state, now_m, hangtime);
        return;
    }
    p25_sm_update_ui_mode(ctx, state);
    if (!ctx->vc_activity_seen) {
        p25_sm_tick_tuned_wait_voice(ctx, opts, state, now_m, grant_timeout);
    }
}

static void
p25_sm_tick_hunting(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!ctx) {
        return;
    }
    int tune_status = p25_sm_resolve_pending_cc_tune(ctx, opts, state);
    if (tune_status == 0 || (tune_status > 0 && ctx->state == P25_SM_ON_CC)) {
        return;
    }
    if (tune_status < 0) {
        if (state && state->p25_cc_eval_freq != 0) {
            dsd_trunk_cc_candidates_set_cooldown(state, state->p25_cc_eval_freq, now_m + 10.0);
            state->p25_cc_eval_freq = 0;
            state->p25_cc_eval_start_m = 0.0;
        }
        ctx->t_hunt_try_m = now_m;
        try_next_cc(ctx, opts, state, now_m);
        return;
    }
    const double decoded_after_m = ctx->cc_sync_pending ? ctx->t_cc_tune_m : ctx->t_cc_sync_m;
    if (state && state->p25_last_cc_msg_time_m > decoded_after_m
        && p25_sm_refresh_cc_sync_from_state(ctx, opts, state, "hunt-tick")) {
        set_state(ctx, opts, state, P25_SM_ON_CC, "cc-activity");
        return;
    }
    if (ctx->t_hunt_try_m <= 0.0 || (now_m - ctx->t_hunt_try_m) >= CC_HUNT_INTERVAL_S) {
        ctx->t_hunt_try_m = now_m;
        try_next_cc(ctx, opts, state, now_m);
    }
}

static void
p25_sm_tick_age_tables(dsd_state* state) {
    if (!state) {
        return;
    }
    p25_aff_tick(state);
    p25_ga_tick(state);
    p25_nb_tick(state);
}

void
p25_sm_event(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    p25_sm_event_handler_fn handler = NULL;
    if (!ctx || !ev) {
        return;
    }

    // Auto-initialize if needed
    if (!ctx->initialized) {
        p25_sm_init_ctx(ctx, opts, state);
    }

    if ((unsigned)ev->type > (unsigned)P25_SM_EV_CRYPTO_PENDING) {
        return;
    }
    handler = g_p25_sm_event_handlers[(unsigned)ev->type];
    if (handler) {
        handler(ctx, opts, state, ev);
    }
}

void
p25_sm_tick_ctx(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }

    // Auto-initialize if needed
    if (!ctx->initialized) {
        p25_sm_init_ctx(ctx, opts, state);
    }

    if (p25_sm_tick_handle_forced_release(ctx, opts, state)) {
        return;
    }

    const double now_m = dsd_time_now_monotonic_s();

    switch (ctx->state) {
        case P25_SM_IDLE:
            // Nothing to do
            break;

        case P25_SM_ON_CC: p25_sm_tick_on_cc(ctx, opts, state, now_m, ctx->config.cc_grace_s); break;
        case P25_SM_TUNED:
            p25_sm_tick_tuned(ctx, opts, state, now_m, ctx->config.hangtime_s, ctx->config.grant_timeout_s);
            break;
        case P25_SM_HUNTING: p25_sm_tick_hunting(ctx, opts, state, now_m); break;
    }

    p25_sm_tick_age_tables(state);
}

/* ============================================================================
 * Global Singleton
 * ============================================================================ */

static p25_sm_ctx_t g_sm_ctx;
static int g_sm_initialized = 0;

p25_sm_ctx_t*
p25_sm_get_ctx(void) {
    p25_sm_ctx_t* scan_ctx = (p25_sm_ctx_t*)dsd_trunk_scan_hook_p25_ctx();
    if (scan_ctx) {
        return scan_ctx;
    }
    if (!g_sm_initialized) {
        p25_sm_init_ctx(&g_sm_ctx, NULL, NULL);
        g_sm_initialized = 1;
    }
    return &g_sm_ctx;
}

int
p25_sm_slot_grant_newer_than(int slot, double observed_m) {
    if (slot < 0 || slot > 1 || observed_m <= 0.0) {
        return 0;
    }

    const p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    return (slot_ctx->grant_active && slot_ctx->last_grant_m > observed_m) ? 1 : 0;
}

/* ============================================================================
 * Convenience Emit Functions
 * ============================================================================ */

void
p25_sm_emit_ptt(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_ptt(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_ptt_call(dsd_opts* opts, dsd_state* state, int slot, int tg, int dst, int src, int is_group, int svc_bits) {
    p25_sm_event_t ev = p25_sm_ev_ptt_call(slot, tg, dst, src, is_group, svc_bits);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_active(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_active(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_active_call(dsd_opts* opts, dsd_state* state, int slot, int tg, int dst, int src, int is_group,
                        int svc_bits) {
    p25_sm_event_t ev = p25_sm_ev_active_call(slot, tg, dst, src, is_group, svc_bits);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_end(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_end(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

int
p25_sm_emit_end_call_at(dsd_opts* opts, dsd_state* state, int slot, int tg, int src, double observed_m) {
    if (slot < 0 || slot > 1) {
        return 0;
    }
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    if (!ctx->initialized) {
        p25_sm_init_ctx(ctx, opts, state);
    }
    p25_sm_event_t ev = p25_sm_ev_end_call_at(slot, tg, src, observed_m);
    return handle_voice_end(ctx, opts, state, slot, "end", 1, 1, &ev);
}

int
p25_sm_emit_facch_end_call_at(dsd_opts* opts, dsd_state* state, int slot, int tg, int src, double observed_m) {
    if (slot < 0 || slot > 1) {
        return P25_SM_END_IGNORED;
    }
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    if (!ctx->initialized) {
        p25_sm_init_ctx(ctx, opts, state);
    }
    p25_sm_event_t ev = p25_sm_ev_facch_end_call_at(slot, tg, src, observed_m);
    return handle_facch_voice_end(ctx, opts, state, &ev);
}

void
p25_sm_emit_idle(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_idle(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_idle_at(dsd_opts* opts, dsd_state* state, int slot, double observed_m) {
    p25_sm_event_t ev = p25_sm_ev_idle_at(slot, observed_m);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_hangtime(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_hangtime(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_tdu(dsd_opts* opts, dsd_state* state) {
    p25_sm_event_t ev = p25_sm_ev_tdu();
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_enc(dsd_opts* opts, dsd_state* state, int slot, int algid, int keyid, int tg) {
    p25_sm_event_t ev = p25_sm_ev_enc(slot, algid, keyid, tg);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_crypto_pending(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_crypto_pending(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    if (!ctx) {
        ctx = p25_sm_get_ctx();
    }
    do_release(ctx, opts, state, reason ? reason : "explicit-release", 1);
}

/* ============================================================================
 * Encryption Lockout Helper
 * ============================================================================ */

void
p25_emit_enc_lockout_once_typed(dsd_opts* opts, dsd_state* state, uint8_t slot, int target, int svc_bits,
                                int is_group) {
    if (!opts || !state || target <= 0) {
        return;
    }
    is_group = is_group ? 1 : 0;

    p25_sm_note_encrypted_call_typed(opts, state, target, is_group);

    // Prepare per-slot context. Keep live encryption fields intact: callers may
    // still need ALG/KID/MI after this helper returns to keep audio gates closed.
    if ((slot & 1) == 0) {
        state->lasttg = (uint32_t)target;
        if (svc_bits != 0) {
            state->dmr_so = (uint16_t)svc_bits;
            state->p25_service_options_valid[0] = 1U;
        }
    } else {
        state->lasttgR = (uint32_t)target;
        if (svc_bits != 0) {
            state->dmr_soR = (uint16_t)svc_bits;
            state->p25_service_options_valid[1] = 1U;
        }
    }
    state->p25_policy_tg[slot & 1] = 0;
    state->gi[slot & 1] = (int8_t)(is_group ? 0 : 1);

    // Compose event text and push
    Event_History_I* eh = (state->event_history_s != NULL) ? &state->event_history_s[slot & 1] : NULL;
    if (eh) {
        DSD_SNPRINTF(eh->Event_History_Items[0].internal_str, sizeof eh->Event_History_Items[0].internal_str,
                     "Target: %d; has been locked out; Encryption Lock Out Enabled.", target);
        dsd_event_history_mark_dirty(eh);
        dsd_p25_optional_hook_watchdog_event_current(opts, state, (uint8_t)(slot & 1));
        if (strncmp(eh->Event_History_Items[1].internal_str, eh->Event_History_Items[0].internal_str,
                    sizeof eh->Event_History_Items[0].internal_str)
            != 0) {
            if (opts->event_out_file[0] != '\0') {
                uint8_t swrite = DSD_SYNC_IS_P25P2(state->lastsynctype) ? 1 : 0;
                dsd_p25_optional_hook_write_event_to_log_file(opts, state, (uint8_t)(slot & 1), swrite,
                                                              eh->Event_History_Items[0].event_string);
            }
            dsd_p25_optional_hook_push_event_history(eh);
            dsd_p25_optional_hook_init_event_history(eh, 0, 1);
        }
    } else if (opts->verbose > 1) {
        p25_sm_log_status(opts, state, "enc-lo-skip-nohist");
    }
}

/* ============================================================================
 * Affiliation (RID) Table Helpers
 * ============================================================================ */

#define P25_AFF_TTL_SEC ((time_t)15 * 60)

static int
p25_aff_find_idx(const dsd_state* state, uint32_t rid) {
    if (!state || rid == 0) {
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        if (state->p25_aff_rid[i] == rid) {
            return i;
        }
    }
    return -1;
}

static int
p25_aff_find_free(const dsd_state* state) {
    if (!state) {
        return -1;
    }
    for (int i = 0; i < 256; i++) {
        if (state->p25_aff_rid[i] == 0) {
            return i;
        }
    }
    return -1;
}

void
p25_aff_register(dsd_state* state, uint32_t rid) {
    if (!state || rid == 0) {
        return;
    }
    int idx = p25_aff_find_idx(state, rid);
    if (idx < 0) {
        idx = p25_aff_find_free(state);
        if (idx < 0) {
            time_t oldest = state->p25_aff_last_seen[0];
            int old_idx = 0;
            for (int i = 1; i < 256; i++) {
                if (state->p25_aff_last_seen[i] < oldest) {
                    oldest = state->p25_aff_last_seen[i];
                    old_idx = i;
                }
            }
            idx = old_idx;
        } else {
            state->p25_aff_count++;
        }
        state->p25_aff_rid[idx] = rid;
    }
    state->p25_aff_last_seen[idx] = time(NULL);
}

void
p25_aff_deregister(dsd_state* state, uint32_t rid) {
    if (!state || rid == 0) {
        return;
    }
    int idx = p25_aff_find_idx(state, rid);
    if (idx >= 0) {
        state->p25_aff_rid[idx] = 0;
        state->p25_aff_last_seen[idx] = 0;
        if (state->p25_aff_count > 0) {
            state->p25_aff_count--;
        }
    }
}

void
p25_aff_tick(dsd_state* state) {
    if (!state) {
        return;
    }
    time_t now = time(NULL);
    for (int i = 0; i < 256; i++) {
        if (state->p25_aff_rid[i] != 0) {
            time_t last = state->p25_aff_last_seen[i];
            if (last != 0 && (now - last) > P25_AFF_TTL_SEC) {
                state->p25_aff_rid[i] = 0;
                state->p25_aff_last_seen[i] = 0;
                if (state->p25_aff_count > 0) {
                    state->p25_aff_count--;
                }
            }
        }
    }
}

/* ============================================================================
 * Group Affiliation (RID↔TG) Table Helpers
 * ============================================================================ */

#define P25_GA_TTL_SEC ((time_t)30 * 60)

static int
p25_ga_find_idx(const dsd_state* state, uint32_t rid, uint16_t tg) {
    if (!state || rid == 0 || tg == 0) {
        return -1;
    }
    for (int i = 0; i < 512; i++) {
        if (state->p25_ga_rid[i] == rid && state->p25_ga_tg[i] == tg) {
            return i;
        }
    }
    return -1;
}

static int
p25_ga_find_free(const dsd_state* state) {
    if (!state) {
        return -1;
    }
    for (int i = 0; i < 512; i++) {
        if (state->p25_ga_rid[i] == 0 || state->p25_ga_tg[i] == 0) {
            return i;
        }
    }
    return -1;
}

void
p25_ga_add(dsd_state* state, uint32_t rid, uint16_t tg) {
    if (!state || rid == 0 || tg == 0) {
        return;
    }
    int idx = p25_ga_find_idx(state, rid, tg);
    if (idx < 0) {
        idx = p25_ga_find_free(state);
        if (idx < 0) {
            time_t oldest = state->p25_ga_last_seen[0];
            int old_idx = 0;
            for (int i = 1; i < 512; i++) {
                if (state->p25_ga_last_seen[i] < oldest) {
                    oldest = state->p25_ga_last_seen[i];
                    old_idx = i;
                }
            }
            idx = old_idx;
        } else {
            state->p25_ga_count++;
        }
        state->p25_ga_rid[idx] = rid;
        state->p25_ga_tg[idx] = tg;
    }
    state->p25_ga_last_seen[idx] = time(NULL);
}

void
p25_ga_tick(dsd_state* state) {
    if (!state) {
        return;
    }
    time_t now = time(NULL);
    for (int i = 0; i < 512; i++) {
        if (state->p25_ga_rid[i] != 0 && state->p25_ga_tg[i] != 0) {
            time_t last = state->p25_ga_last_seen[i];
            if (last != 0 && (now - last) > P25_GA_TTL_SEC) {
                state->p25_ga_rid[i] = 0;
                state->p25_ga_tg[i] = 0;
                state->p25_ga_last_seen[i] = 0;
                if (state->p25_ga_count > 0) {
                    state->p25_ga_count--;
                }
            }
        }
    }
}
