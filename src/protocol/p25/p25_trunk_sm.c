// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * Unified P25 trunking state machine.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_sm_ui.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/p25_p2_audio_ring.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
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

static int do_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason,
                      int arm_failed_vc_backoff_on_accept);
static void p25_sm_diagf(dsd_opts* opts, const dsd_state* state, const p25_sm_ctx_t* ctx, const char* event,
                         const char* format, ...) DSD_ATTR_FORMAT(printf, 5, 6);

// Serialize release-to-CC operations to avoid duplicate retunes when multiple
// threads (watchdog tick + decoder) request release concurrently.
static atomic_int g_p25_sm_release_lock = 0;

#define P25_FAILED_VC_RETUNE_BACKOFF_DEFAULT_S 10.0

static inline double
now_monotonic(void) {
    return dsd_time_now_monotonic_s();
}

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
p25_retune_backoff_explicitly_disabled(const dsdneoRuntimeConfig* cfg) {
    return (cfg && cfg->p25_retune_backoff_is_set && cfg->p25_retune_backoff_s <= 0.0) ? 1 : 0;
}

static double
p25_failed_vc_retune_backoff_s(const dsd_opts* opts) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (p25_retune_backoff_explicitly_disabled(cfg)) {
        return 0.0;
    }
    if (cfg && cfg->p25_retune_backoff_is_set && cfg->p25_retune_backoff_s > 0.0) {
        return cfg->p25_retune_backoff_s;
    }
    if (opts && opts->p25_retune_backoff_s > 0.0) {
        return opts->p25_retune_backoff_s;
    }
    return P25_FAILED_VC_RETUNE_BACKOFF_DEFAULT_S;
}

static time_t
p25_backoff_wall_seconds(double backoff_s) {
    if (backoff_s <= 0.0) {
        return 0;
    }
    if (backoff_s < 1.0) {
        return 1;
    }
    return (time_t)(backoff_s + 0.999);
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
    if (DSD_SYNC_IS_P25P2(state->synctype)) {
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
    if (!opts || opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1) {
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
p25_sm_start_cc_grace_after_tune(p25_sm_ctx_t* ctx, dsd_state* state, double tune_start_m) {
    if (!ctx) {
        return;
    }
    ctx->t_cc_sync_m = tune_start_m;
    if (state) {
        if (state->last_cc_sync_time_m <= 0.0 || state->last_cc_sync_time_m < tune_start_m) {
            state->last_cc_sync_time = time(NULL);
            state->last_cc_sync_time_m = tune_start_m;
        }
        if (state->last_cc_sync_time_m > ctx->t_cc_sync_m) {
            // CC retune hooks update this as tune metadata before any CC frame decodes.
            // Absorb that timestamp now so the next watchdog tick does not relatch NAC from it.
            ctx->t_cc_sync_m = state->last_cc_sync_time_m;
        }
    }
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

// Set state with logging
static void
set_state(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, p25_sm_state_e new_state, const char* reason) {
    if (!ctx || ctx->state == new_state) {
        return;
    }
    p25_sm_state_e old = ctx->state;
    ctx->state = new_state;

    // Update state->p25_sm_mode for UI - direct 1:1 mapping
    if (state) {
        switch (new_state) {
            case P25_SM_IDLE: state->p25_sm_mode = DSD_P25_SM_MODE_UNKNOWN; break;
            case P25_SM_ON_CC: state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC; break;
            case P25_SM_TUNED: state->p25_sm_mode = DSD_P25_SM_MODE_ON_VC; break;
            case P25_SM_HUNTING: state->p25_sm_mode = DSD_P25_SM_MODE_HUNTING; break;
        }
    }

    if (opts && opts->verbose > 0) {
        DSD_FPRINTF(stderr, "\n[P25 SM] %s -> %s (%s)\n", p25_sm_state_name(old), p25_sm_state_name(new_state),
                    reason ? reason : "");
    }
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "state", "old=%s new=%s reason=%s", p25_sm_state_name(old),
                 p25_sm_state_name(new_state), reason ? reason : "none");
    sm_log(opts, state, reason);
}

/* ============================================================================
 * Grant Filtering (preserve existing policy logic)
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
}

static int
p25_enc_tg_cache_find_active(dsd_state* state, int tg, time_t now) {
    if (!state || tg <= 0) {
        return -1;
    }
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        time_t until = state->p25_enc_tg_cache_until[i];
        if (until <= 0) {
            continue;
        }
        if (now >= until) {
            p25_enc_tg_cache_clear_entry(state, i);
            continue;
        }
        if (state->p25_enc_tg_cache_tg[i] == (uint32_t)tg) {
            return i;
        }
    }
    return -1;
}

static void
p25_enc_tg_cache_clear_tg(dsd_state* state, int tg) {
    if (!state || tg <= 0) {
        return;
    }
    for (int i = 0; i < DSD_P25_ENC_TG_CACHE_DEPTH; i++) {
        if (state->p25_enc_tg_cache_tg[i] == (uint32_t)tg) {
            p25_enc_tg_cache_clear_entry(state, i);
        }
    }
}

static int
p25_enc_tg_cache_refresh_until(const dsd_opts* opts, time_t now, time_t* out_until) {
    if (!out_until) {
        return 0;
    }
    double backoff_s = p25_failed_vc_retune_backoff_s(opts);
    time_t backoff_wall = p25_backoff_wall_seconds(backoff_s);
    if (backoff_wall <= 0) {
        return 0;
    }
    *out_until = now + backoff_wall;
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
p25_sm_note_encrypted_call(dsd_opts* opts, dsd_state* state, int tg) {
    if (!opts || !state || tg <= 0 || opts->trunk_tune_enc_calls != 0) {
        return;
    }
    if (p25_patch_tg_key_is_clear(state, tg) || p25_patch_sg_key_is_clear(state, tg)) {
        return;
    }

    time_t now = time(NULL);
    time_t until = 0;
    if (!p25_enc_tg_cache_refresh_until(opts, now, &until)) {
        return;
    }

    int idx = p25_enc_tg_cache_find_active(state, tg, now);
    if (idx < 0) {
        idx = p25_enc_tg_cache_choose_slot(state, now);
    }
    if (idx < 0) {
        return;
    }

    state->p25_enc_tg_cache_tg[idx] = (uint32_t)tg;
    state->p25_enc_tg_cache_until[idx] = until;
    p25_sm_diagf(opts, state, NULL, "enc_tg_cache_arm", "tg=%d idx=%d until=%ld", tg, idx, (long)until);
    sm_log(opts, state, "enc-tg-cache-arm");
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

static int
p25_grant_uses_voice_enc_cache(const p25_grant_eval_ctx_t* eval_ctx) {
    return (eval_ctx && !eval_ctx->data_call) ? 1 : 0;
}

static void
p25_grant_clear_transient_cache_if_clear(dsd_state* state, const p25_grant_eval_ctx_t* eval_ctx) {
    if (!state || !eval_ctx || !p25_grant_uses_voice_enc_cache(eval_ctx) || eval_ctx->is_indiv || eval_ctx->tg <= 0) {
        return;
    }
    if ((eval_ctx->svc_valid && !eval_ctx->encrypted_call) || eval_ctx->enc_override_clear) {
        p25_enc_tg_cache_clear_tg(state, eval_ctx->tg);
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
    if (!opts || !state || !eval_ctx || eval_ctx->is_indiv || eval_ctx->tg <= 0 || opts->trunk_tune_enc_calls != 0) {
        return 0;
    }
    if (!p25_grant_uses_voice_enc_cache(eval_ctx)) {
        return 0;
    }
    if (eval_ctx->svc_valid) {
        return 0;
    }
    if (p25_grant_patch_clear_key(state, eval_ctx)) {
        p25_enc_tg_cache_clear_tg(state, eval_ctx->tg);
        return 0;
    }

    time_t now = time(NULL);
    int idx = p25_enc_tg_cache_find_active(state, eval_ctx->tg, now);
    if (idx < 0) {
        return 0;
    }

    time_t until = 0;
    if (p25_enc_tg_cache_refresh_until(opts, now, &until)) {
        state->p25_enc_tg_cache_until[idx] = until;
    } else {
        p25_enc_tg_cache_clear_entry(state, idx);
        return 0;
    }
    p25_sm_diagf(opts, state, NULL, "grant_enc_cache_skip", "tg=%d idx=%d until=%ld", eval_ctx->tg, idx, (long)until);
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
                                              eval_ctx->data_call, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &candidate)
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
    if (eval_ctx->is_indiv) {
        return dsd_tg_policy_evaluate_private_call(
            opts, state, (uint32_t)ev->src, (uint32_t)ev->dst, eval_ctx->encrypted_call, eval_ctx->data_call,
            DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_ALLOW, DSD_TG_POLICY_HOLD_COMPAT_GRANT, out_decision);
    }
    return p25_grant_eval_group_policy(opts, state, ev, eval_ctx, out_decision);
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
        p25_emit_enc_lockout_once(opts, state, 0, eval_ctx->tg, eval_ctx->svc);
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
    if (route->slot == -1 || route->requires_tuner_retune) {
        return 1;
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
}

static void
p25_sm_clear_slot_activity(p25_sm_ctx_t* ctx) {
    p25_sm_clear_one_slot_activity(ctx, 0);
    p25_sm_clear_one_slot_activity(ctx, 1);
}

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
    ctx->slots[slot].last_grant_m = 0.0;
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
p25_grant_slot_matches_moved_target(const p25_sm_slot_ctx_t* slot_ctx, const p25_sm_event_t* ev, int target_id,
                                    int data_call) {
    if (!slot_ctx || !ev || !slot_ctx->grant_active || slot_ctx->target_id != target_id) {
        return 0;
    }
    if (slot_ctx->data_call != (data_call ? 1 : 0)) {
        return 0;
    }

    const int is_group = ev->is_group ? 1 : 0;
    if (slot_ctx->is_group != is_group) {
        return 0;
    }

    return is_group ? (slot_ctx->ota_tg == ev->tg) : (slot_ctx->dst == ev->dst);
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
            state->p25_p2_enc_lockout_muted[s] = 0;
            p25_p2_audio_ring_reset(state, s);
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
    slot_ctx->last_grant_m = now_m;
    slot_ctx->tg = target_id;
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
    // Data grants do not use per-slot pending voice timing, so reused carrier
    // data grants need their own timeout window.
    if (!reused_carrier || ctx->t_tune_m <= 0.0 || data_call) {
        ctx->t_tune_m = now_m;
    }
    if (!reused_carrier || ctx->t_voice_m <= 0.0 || data_call) {
        ctx->t_voice_m = 0.0;
    }
    if (reused_carrier) {
        p25_grant_refresh_reused_carrier_watchdogs(state, now_m);
    }
    if (!reused_carrier) {
        ctx->vc_cqpsk_retry_done = 0;
    }
    p25_grant_store_slot_context(ctx, ev, freq, target_id, eval_ctx, slot, now_m);
    if (state && !reused_carrier) {
        // Clear any stale one-shot VC CQPSK override from a previous attempt.
        state->p25_vc_cqpsk_override = -1;
    }
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
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, freq, ted_sps);
    if (dsd_trunk_tune_result_is_ok(tune_result)) {
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
    if (opts->p25_is_tuned != 1 && opts->trunk_is_tuned != 1) {
        return 0;
    }
    return (state->p25_vc_freq[0] == freq || state->p25_vc_freq[1] == freq || state->trunk_vc_freq[0] == freq
            || state->trunk_vc_freq[1] == freq)
               ? 1
               : 0;
}

static int
p25_grant_slot_duplicate_matches(const p25_sm_slot_ctx_t* slot_ctx, const dsd_tg_policy_call_route* route, long freq,
                                 int target_id, int data_call) {
    if (!slot_ctx || !route) {
        return 0;
    }
    if (slot_ctx->freq_hz != freq || slot_ctx->target_id != target_id || slot_ctx->data_call != data_call) {
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
p25_grant_handle_duplicate(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state,
                           const dsd_tg_policy_call_route* route, const dsd_tg_policy_decision* decision, long freq,
                           int target_id, const p25_grant_eval_ctx_t* eval_ctx, double now_m) {
    int data_call = (eval_ctx && eval_ctx->data_call) ? 1 : 0;
    if (!ctx || !route || ctx->state != P25_SM_TUNED || ctx->vc_freq_hz != freq
        || !p25_grant_decoder_tuned_to_freq(opts, state, freq)) {
        return 0;
    }
    if (route->slot >= 0 && route->slot <= 1) {
        if (!p25_grant_slot_duplicate_matches(&ctx->slots[route->slot], route, freq, target_id, data_call)) {
            return 0;
        }
    } else if (!p25_grant_fallback_duplicate_matches(ctx, target_id, data_call)) {
        return 0;
    }

    p25_grant_refresh_duplicate_slot(ctx, state, route, now_m, data_call);
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

static int
p25_retune_block_slot_matches(int blocked_slot, int grant_slot) {
    return blocked_slot == grant_slot;
}

static void
p25_retune_block_clear_history_entry(dsd_state* state, int idx) {
    if (!state || idx < 0 || idx >= DSD_P25_RETUNE_BLOCK_HISTORY_DEPTH) {
        return;
    }
    state->p25_retune_block_history_until[idx] = 0;
    state->p25_retune_block_history_freq[idx] = 0;
    state->p25_retune_block_history_slot[idx] = -1;
}

static int
p25_retune_block_log_match(const dsd_opts* opts, dsd_state* state, int channel, long freq, int slot, time_t until) {
    p25_sm_diagf((dsd_opts*)opts, state, NULL, "grant_backoff_skip", "ch=0x%04X freq=%ld slot=%d until=%ld",
                 channel & 0xFFFF, freq, slot, (long)until);
    sm_log(opts, state, "grant-vc-backoff");
    if (opts && opts->verbose > 1) {
        DSD_FPRINTF(stderr, "\n[P25 SM] grant-vc-backoff ch=0x%04X freq=%ld slot=%d until=%ld\n", channel & 0xFFFF,
                    freq, slot, (long)until);
    }
    return 1;
}

static int
p25_grant_retune_blocked(const dsd_opts* opts, dsd_state* state, long freq, int slot, int channel) {
    if (!state || freq <= 0) {
        return 0;
    }

    time_t now = time(NULL);
    for (int i = 0; i < DSD_P25_RETUNE_BLOCK_HISTORY_DEPTH; i++) {
        time_t until = state->p25_retune_block_history_until[i];
        if (until <= 0) {
            continue;
        }
        if (now >= until) {
            p25_retune_block_clear_history_entry(state, i);
            continue;
        }
        if (state->p25_retune_block_history_freq[i] == freq
            && p25_retune_block_slot_matches(state->p25_retune_block_history_slot[i], slot)) {
            state->p25_retune_block_freq = state->p25_retune_block_history_freq[i];
            state->p25_retune_block_slot = state->p25_retune_block_history_slot[i];
            state->p25_retune_block_until = until;
            return p25_retune_block_log_match(opts, state, channel, freq, slot, until);
        }
    }

    if (state->p25_retune_block_until <= 0) {
        return 0;
    }

    if (now >= state->p25_retune_block_until) {
        state->p25_retune_block_until = 0;
        state->p25_retune_block_freq = 0;
        state->p25_retune_block_slot = -1;
        return 0;
    }

    if (state->p25_retune_block_freq != freq || !p25_retune_block_slot_matches(state->p25_retune_block_slot, slot)) {
        return 0;
    }

    return p25_retune_block_log_match(opts, state, channel, freq, slot, state->p25_retune_block_until);
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

    p25_sm_start_cc_grace_after_tune(ctx, state, now_m);
    p25_sm_set_expected_cc_nac(ctx, state, 0);
    set_state(ctx, opts, state, P25_SM_ON_CC, "grant-cc-seed");
}

typedef struct {
    dsd_tg_policy_call_route route;
    int slot;
    int target_id;
    int needs_retune;
    int clear_policy_slot_only;
    int reused_carrier;
    int ted_sps;
    long freq;
    double now_m;
} p25_grant_route_ctx_t;

static void
p25_grant_log_freq(dsd_opts* opts, const dsd_state* state, const p25_sm_ctx_t* ctx, const p25_sm_event_t* ev, long freq,
                   const p25_freq_trace_t* freq_trace) {
    if (!freq_trace) {
        return;
    }
    p25_sm_diagf(opts, state, ctx, "grant_freq",
                 "ch=0x%04X freq=%ld source=%s failure=%s iden=%d type=%d tdma=%d denom=%d step=%d cached=%d "
                 "ambiguous=%d base=%ld spacing=%ld tg=%d src=%d dst=%d svc=0x%02X",
                 ev->channel & 0xFFFF, freq, freq_trace->source, freq_trace->failure[0] ? freq_trace->failure : "none",
                 freq_trace->iden, freq_trace->chan_type, freq_trace->use_tdma, freq_trace->denom, freq_trace->step,
                 freq_trace->cached, freq_trace->ambiguous, freq_trace->base_hz, freq_trace->spacing_hz, ev->tg,
                 ev->src, ev->dst, ev->svc_bits);
}

static int
p25_grant_should_clear_slot_only(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state,
                                 const p25_sm_event_t* ev, long freq, int slot) {
    if (!ctx || !state || !ev || slot < 0 || slot > 1) {
        return 0;
    }
    return (ctx->state == P25_SM_TUNED && ctx->vc_freq_hz == freq && ctx->vc_is_tdma
            && p25_grant_decoder_tuned_to_freq(opts, state, freq) && is_tdma_channel(state, ev->channel))
               ? 1
               : 0;
}

static int
p25_grant_prepare_route(const p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev,
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

    out->now_m = now_monotonic();
    out->slot = channel_slot(state, ev->channel);
    if (!eval_ctx->data_call && p25_grant_retune_blocked(opts, state, out->freq, out->slot, ev->channel)) {
        return 0;
    }
    out->needs_retune = (ctx->state == P25_SM_TUNED && ctx->vc_freq_hz != 0 && ctx->vc_freq_hz != out->freq) ? 1 : 0;
    out->clear_policy_slot_only = p25_grant_should_clear_slot_only(ctx, opts, state, ev, out->freq, out->slot);
    out->reused_carrier = out->clear_policy_slot_only;
    out->target_id = p25_grant_target_id(ev, decision);
    p25_grant_fill_route(&out->route, ev, out->freq, out->slot, out->needs_retune, out->target_id);
    return 1;
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

    // Skip if already tuned to same frequency AND same TG (avoid bounce on duplicate grants)
    // Different TG/call type should still trigger a new tune
    if (p25_grant_handle_duplicate(ctx, opts, state, &grant.route, &decision, grant.freq, grant.target_id, &eval_ctx,
                                   grant.now_m)) {
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
    p25_grant_clear_moved_target_slots(ctx, state, grant.slot, ev, grant.target_id, grant.freq, eval_ctx.data_call);
    if (grant.clear_policy_slot_only) {
        p25_grant_clear_one_slot_state(ctx, grant.slot);
    } else {
        p25_grant_clear_slot_state(ctx);
    }
    p25_grant_clear_replaced_policy_tg(state, grant.slot, grant.clear_policy_slot_only);
    p25_grant_store_vc_context(ctx, state, ev, grant.freq, grant.target_id, &eval_ctx, grant.now_m, grant.slot,
                               grant.reused_carrier);
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
                 "prio=%d preempt=%d data=%d reused_carrier=%d",
                 ev->channel & 0xFFFF, grant.freq, grant.slot, grant.target_id, ev->tg, ev->src, grant.needs_retune,
                 decision.priority, decision.preempt_requested, eval_ctx.data_call, grant.reused_carrier);

    set_state(ctx, opts, state, P25_SM_TUNED, "grant");
}

void
p25_sm_apply_group_grant_policy(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src) {
    if (!opts || !state || opts->p25_trunk != 1) {
        return;
    }

    p25_sm_event_t ev = p25_sm_ev_group_grant(channel, 0, tg, src, svc_bits);
    dsd_tg_policy_decision decision;
    if (grant_allowed(opts, state, &ev, &decision, NULL)) {
        p25_sm_diagf(opts, state, NULL, "grant_policy_only", "ch=0x%04X tg=%d src=%d svc=0x%02X policy_tg=%u",
                     channel & 0xFFFF, tg, src, svc_bits & 0xFF, decision.target_id);
    }
}

// Helper to check if slot has decryption capability
static int
slot_can_decrypt(const dsd_state* state, int slot, int algid) {
    if (!state || algid == 0 || algid == 0x80) {
        return 1; // Clear or no encryption
    }
    unsigned long long key = (slot == 0) ? state->R : state->RR;
    int aes_loaded = state->aes_key_loaded[slot];
    if ((algid == 0xAA || algid == 0x81 || algid == 0x9F) && key != 0) {
        return 1;
    }
    if ((algid == 0x84 || algid == 0x89) && aes_loaded == 1) {
        return 1;
    }
    return 0;
}

static void
handle_voice_start(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, int slot, const char* why) {
    if (!ctx) {
        return;
    }

    double now_m = now_monotonic();
    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    // Update slot activity
    ctx->slots[s].voice_active = 1;
    ctx->slots[s].last_active_m = now_m;

    // NOTE: Audio gating is managed by MAC_PTT/MAC_ACTIVE handlers in xcch.c,
    // ENC event handler, and ESS processing in frame.c.
    // This event just marks voice as active for state machine timing purposes.

    ctx->t_voice_m = now_m;
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "voice_activity", "kind=%s slot=%d now_m=%.3f freq=%ld tg=%d", why, slot,
                 now_m, ctx->vc_freq_hz, ctx->vc_tg);
    sm_log(opts, state, why);
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
p25_voice_end_can_release_explicit(const p25_sm_ctx_t* ctx, int other) {
    if (!ctx->vc_is_tdma) {
        return 1;
    }
    if (other < 0 || other > 1) {
        return 1;
    }
    if (ctx->slots[other].grant_active) {
        return 0;
    }
    if (ctx->slots[other].last_active_m <= 0.0) {
        return 1;
    }
    return ctx->slots[other].voice_active ? 0 : 1;
}

static int
p25_voice_end_preserve_recent_idle_grant(const p25_sm_ctx_t* ctx, int slot, int is_explicit_end, double observed_m) {
    if (!ctx || slot < 0 || slot > 1 || is_explicit_end || observed_m <= 0.0) {
        return 0;
    }
    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    return (slot_ctx->grant_active && slot_ctx->last_grant_m > observed_m) ? 1 : 0;
}

static void
p25_voice_end_try_explicit_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot, int is_explicit_end) {
    if (!is_explicit_end || ctx->state != P25_SM_TUNED || !opts || !state) {
        return;
    }

    // Explicitly clear audio gating for this slot - the call is terminated.
    // This is done here because xcch.c sets p25_p2_audio_allowed AFTER calling
    // p25_sm_emit_end(), so we need to clear it here to avoid false "active" state.
    state->p25_p2_audio_allowed[slot] = 0;

    // For explicit end (MAC_END_PTT), this slot is done. Don't wait for ring buffer
    // to drain - the audio output will continue playing buffered samples while we
    // return to CC. The ring buffer check is only relevant for hangtime-based release.

    // For TDMA (P25P2), check the other slot - but only if it ever had call activity
    // during this VC tune. If last_active_m == 0, the other slot never received
    // PTT/ACTIVE, so we shouldn't wait for it.
    int other = slot ^ 1;
    if (p25_voice_end_can_release_explicit(ctx, other)) {
        // All active slots terminated - release immediately like P25P1 Call Termination
        do_release(ctx, opts, state, "call-end", 0);
    }
}

static void
handle_voice_end(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot, const char* why, int is_explicit_end,
                 double observed_m) {
    if (!ctx) {
        return;
    }

    int s = (slot >= 0 && slot <= 1) ? slot : 0;
    int preserve_recent_grant = p25_voice_end_preserve_recent_idle_grant(ctx, s, is_explicit_end, observed_m);

    // Mark voice inactive but keep last_active_m for hangtime tracking
    ctx->slots[s].voice_active = 0;
    if (!preserve_recent_grant) {
        ctx->slots[s].grant_active = 0;
    }
    if (state && !preserve_recent_grant) {
        (void)dsd_tg_policy_clear_active_call(state, ctx->vc_is_tdma ? s : -1);
    }

    // NOTE: Audio gating is managed by MAC_END/MAC_IDLE handlers in xcch.c
    // which set p25_p2_audio_allowed[slot] = 0.

    sm_log(opts, state, why);
    p25_sm_diagf(opts, state, ctx, "voice_activity", "kind=%s slot=%d explicit=%d freq=%ld tg=%d other_active=%d", why,
                 slot, is_explicit_end, ctx->vc_freq_hz, ctx->vc_tg, p25_voice_end_diag_other_active(ctx, state, s));

    // For explicit call termination (MAC_END_PTT or TDU), check if we should
    // release immediately rather than waiting for hangtime. This matches P25P1
    // behavior where LCW 0x4F (Call Termination) triggers immediate release.
    p25_voice_end_try_explicit_release(ctx, opts, state, s, is_explicit_end);
}

static int
p25_sm_slot_waiting_for_voice(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot) {
    if (!ctx || slot < 0 || slot > 1) {
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
    double latest_grant_m = 0.0;
    if (!ctx || !ctx->vc_is_tdma) {
        return 0.0;
    }
    for (int s = 0; s < 2; s++) {
        if (!p25_sm_slot_waiting_for_voice(ctx, state, s)) {
            continue;
        }
        if (ctx->slots[s].last_grant_m > latest_grant_m) {
            latest_grant_m = ctx->slots[s].last_grant_m;
        }
    }
    return latest_grant_m;
}

static void
p25_enc_lockout_clear_slot_grant(p25_sm_ctx_t* ctx, dsd_state* state, int slot) {
    if (!ctx || !state || slot < 0 || slot > 1) {
        return;
    }
    ctx->slots[slot].grant_active = 0;
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

static void
handle_cc_sync(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }
    ctx->t_cc_sync_m = now_monotonic();
    p25_sm_set_expected_cc_nac(ctx, state, 1);
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "cc_sync", "expected_nac=0x%03X nac=0x%03X last_cc_m=%.3f",
                 ctx->expected_cc_nac, state ? state->nac : 0, state ? state->last_cc_sync_time_m : 0.0);

    if (ctx->state == P25_SM_IDLE || ctx->state == P25_SM_HUNTING) {
        set_state(ctx, opts, state, P25_SM_ON_CC, "cc-sync");
    }
}

static void
handle_enc(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (!ctx || !ev || !opts || !state) {
        return;
    }

    int slot = (ev->slot >= 0 && ev->slot <= 1) ? ev->slot : 0;
    int algid = ev->algid;
    int tg = ev->tg;
    int can_decrypt = 0;
    int allow_audio = 0;

    // Store encryption params in slot context
    ctx->slots[slot].algid = algid;
    ctx->slots[slot].keyid = ev->keyid;
    ctx->slots[slot].tg = tg;
    can_decrypt = slot_can_decrypt(state, slot, algid);
    allow_audio = dsd_p25p2_decode_audio_allowed(opts, state, slot, algid);

    // Skip lockout processing if encryption lockout is disabled
    if (opts->trunk_tune_enc_calls != 0) {
        // Even when encrypted calls are permitted, keep the gate aligned with
        // the current media policy so allow-list/TG-hold blocks are not reopened.
        state->p25_p2_audio_allowed[slot] = allow_audio;
        state->p25_p2_enc_lockout_muted[slot] = 0;
        return;
    }

    // Skip if we're not currently tuned to a voice channel
    if (ctx->state != P25_SM_TUNED) {
        return;
    }

    // Decryptable encrypted calls still need to respect the current media
    // policy; do not reopen slots that the caller already blocked.
    if (can_decrypt) {
        state->p25_p2_audio_allowed[slot] = allow_audio;
        state->p25_p2_enc_lockout_muted[slot] = 0;
        return;
    }

    // Single-indication lockout: trigger immediately on encrypted stream
    p25_sm_diagf(opts, state, ctx, "enc_lockout",
                 "slot=%d algid=0x%02X keyid=0x%04X tg=%d can_decrypt=%d allow_audio=%d", slot, algid, ev->keyid, tg,
                 can_decrypt, allow_audio);
    sm_log(opts, state, "enc-lockout");

    if (tg > 0) {
        p25_emit_enc_lockout_once(opts, state, (uint8_t)slot, tg, 0x40);
    }

    // Gate audio for this slot
    state->p25_p2_audio_allowed[slot] = 0;
    state->p25_p2_enc_lockout_muted[slot] = 1;
    p25_p2_audio_ring_reset(state, slot);

    // Clear voice activity indicator to prevent audio routing logic from
    // treating this locked-out slot as having active voice
    ctx->slots[slot].voice_active = 0;
    p25_enc_lockout_clear_slot_grant(ctx, state, slot);
    if (slot == 0) {
        state->dmrburstL = 0;
        // Reset voice counters to prevent stale state from affecting later calls
        state->fourv_counter[0] = 0;
        state->voice_counter[0] = 0;
        state->DMRvcL = 0;
        state->dropL = 256;
    } else {
        state->dmrburstR = 0;
        // Reset voice counters to prevent stale state from affecting later calls
        state->fourv_counter[1] = 0;
        state->voice_counter[1] = 0;
        state->DMRvcR = 0;
        state->dropR = 256;
    }

    // Check if opposite slot is active - only release if both slots are quiet
    int other = slot ^ 1;
    int other_active = p25_enc_lockout_other_slot_active(ctx, state, other);

    if (!other_active) {
        do_release(ctx, opts, state, "enc-lockout", 0);
    } else {
        p25_sm_diagf(opts, state, ctx, "enc_lockout_slot_only", "slot=%d other=%d", slot, other);
        sm_log(opts, state, "enc-lockout-slot-only");
    }
}

/* ============================================================================
 * Release to CC
 * ============================================================================ */

static int
p25_release_should_return_to_cc(const p25_sm_ctx_t* ctx, const dsd_opts* opts) {
    const int opts_tuned = (opts && (opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1)) ? 1 : 0;
    return (ctx && (ctx->state == P25_SM_TUNED || opts_tuned)) ? 1 : 0;
}

static int
p25_release_return_to_cc_accepted(const p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int had_force_release,
                                  double* out_tune_start_m) {
    if (ctx->vc_is_tdma && opts && state) {
        dsd_p25_optional_hook_p25p2_flush_partial_audio(opts, state);
    }

    *out_tune_start_m = now_monotonic();
    p25_sm_diagf(opts, state, ctx, "release_cc_attempt", "freq=%ld ch=0x%04X tg=%d force=%d tdma=%d data=%d",
                 state ? ((state->p25_cc_freq != 0) ? state->p25_cc_freq : state->trunk_cc_freq) : 0,
                 ctx->vc_channel & 0xFFFF, ctx->vc_tg, had_force_release, ctx->vc_is_tdma, ctx->vc_data_call);
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_return_to_cc(opts, state);
    if (dsd_trunk_tune_result_is_ok(tune_result)) {
        p25_sm_diagf(opts, state, ctx, "release_cc_result", "result=%s tune_start_m=%.3f",
                     p25_tune_result_name(tune_result), *out_tune_start_m);
        return 1;
    }

    p25_sm_diagf(opts, state, ctx, "release_cc_result", "result=%s tune_start_m=%.3f",
                 p25_tune_result_name(tune_result), *out_tune_start_m);
    sm_log(opts, state, tune_result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "release-cc-deferred" : "release-cc-failed");
    if (state && had_force_release) {
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
    ctx->t_tune_m = 0.0;
    ctx->t_voice_m = 0.0;
    ctx->release_count++;
    ctx->cc_return_count++;
}

static void
p25_retune_block_remember_failure(dsd_state* state, long freq, int slot, time_t until) {
    int replace_idx = -1;
    time_t now = time(NULL);
    if (!state || freq <= 0 || until <= now) {
        return;
    }

    for (int i = 0; i < DSD_P25_RETUNE_BLOCK_HISTORY_DEPTH; i++) {
        if (state->p25_retune_block_history_until[i] > 0 && state->p25_retune_block_history_freq[i] == freq
            && p25_retune_block_slot_matches(state->p25_retune_block_history_slot[i], slot)) {
            replace_idx = i;
            break;
        }
        if (replace_idx < 0 && state->p25_retune_block_history_until[i] <= now) {
            replace_idx = i;
        }
    }

    if (replace_idx < 0) {
        replace_idx = (int)(state->p25_retune_block_next % DSD_P25_RETUNE_BLOCK_HISTORY_DEPTH);
        state->p25_retune_block_next++;
    }

    state->p25_retune_block_history_freq[replace_idx] = freq;
    state->p25_retune_block_history_slot[replace_idx] = slot;
    state->p25_retune_block_history_until[replace_idx] = until;
}

static int
p25_backoff_slot_context_valid(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot) {
    if (!ctx || slot < 0 || slot > 1) {
        return 0;
    }
    const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[slot];
    return p25_sm_slot_waiting_for_voice(ctx, state, slot) && slot_ctx->freq_hz > 0;
}

static void
p25_arm_failed_vc_retune_slot_backoff(const p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state,
                                      const p25_sm_slot_ctx_t* slot_ctx, int slot, double backoff_s, time_t until) {
    state->p25_retune_block_freq = slot_ctx->freq_hz;
    state->p25_retune_block_slot = slot;
    state->p25_retune_block_until = until;
    p25_retune_block_remember_failure(state, slot_ctx->freq_hz, slot, until);
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "grant_backoff_arm", "ch=0x%04X freq=%ld slot=%d seconds=%.3f until=%ld",
                 slot_ctx->channel & 0xFFFF, slot_ctx->freq_hz, slot, backoff_s, (long)until);
}

static int
p25_arm_failed_vc_retune_slot_backoffs(const p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state,
                                       double backoff_s, time_t until) {
    int armed_slots = 0;
    for (int s = 0; s < 2; s++) {
        const p25_sm_slot_ctx_t* slot_ctx = &ctx->slots[s];
        if (!p25_backoff_slot_context_valid(ctx, state, s)) {
            continue;
        }
        p25_arm_failed_vc_retune_slot_backoff(ctx, opts, state, slot_ctx, s, backoff_s, until);
        armed_slots++;
    }
    return armed_slots;
}

static int
p25_vc_has_observed_voice(const p25_sm_ctx_t* ctx) {
    return (ctx && (ctx->t_voice_m > 0.0 || ctx->slots[0].voice_active || ctx->slots[1].voice_active)) ? 1 : 0;
}

static void
p25_arm_failed_vc_retune_fallback_backoff(const p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state,
                                          double backoff_s, time_t until) {
    state->p25_retune_block_freq = ctx->vc_freq_hz;
    state->p25_retune_block_slot = channel_slot(state, ctx->vc_channel);
    state->p25_retune_block_until = until;
    p25_retune_block_remember_failure(state, ctx->vc_freq_hz, state->p25_retune_block_slot, until);
    p25_sm_diagf((dsd_opts*)opts, state, ctx, "grant_backoff_arm", "ch=0x%04X freq=%ld slot=%d seconds=%.3f until=%ld",
                 ctx->vc_channel & 0xFFFF, ctx->vc_freq_hz, state->p25_retune_block_slot, backoff_s, (long)until);
}

static void
p25_arm_failed_vc_retune_backoff(const p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state) {
    if (!ctx || !state || ctx->vc_freq_hz <= 0) {
        return;
    }
    double backoff_s = p25_failed_vc_retune_backoff_s(opts);
    time_t backoff_wall = p25_backoff_wall_seconds(backoff_s);
    if (backoff_wall <= 0) {
        return;
    }

    time_t until = time(NULL) + backoff_wall;
    int armed_slots = p25_arm_failed_vc_retune_slot_backoffs(ctx, opts, state, backoff_s, until);
    if (armed_slots == 0) {
        if (p25_vc_has_observed_voice(ctx)) {
            return;
        }
        if (ctx->vc_data_call || p25_sm_has_pending_data_grant(ctx)) {
            p25_sm_diagf((dsd_opts*)opts, state, ctx, "grant_backoff_skip", "reason=data-grant ch=0x%04X freq=%ld",
                         ctx->vc_channel & 0xFFFF, ctx->vc_freq_hz);
            return;
        }
        p25_arm_failed_vc_retune_fallback_backoff(ctx, opts, state, backoff_s, until);
    }
    sm_log(opts, state, "grant-vc-backoff-arm");
    if (opts && opts->verbose > 1) {
        DSD_FPRINTF(stderr, "\n[P25 SM] grant-vc-backoff-arm ch=0x%04X freq=%ld slot=%d %.3fs\n",
                    ctx->vc_channel & 0xFFFF, ctx->vc_freq_hz, state->p25_retune_block_slot, backoff_s);
    }
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
        state->p25_p2_enc_lockout_muted[0] = 0;
        state->p25_p2_enc_lockout_muted[1] = 0;
        state->p25_policy_tg[0] = 0;
        state->p25_policy_tg[1] = 0;
        state->p25_sm_release_count++;
    }
    if (opts) {
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
    }
}

static int
do_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason,
           int arm_failed_vc_backoff_on_accept) {
    double tune_start_m = 0.0;
    int had_force_release = 0;
    if (!ctx) {
        return 0;
    }

    // Avoid double-return-to-CC thrash if multiple callers attempt to release at
    // nearly the same time (e.g., explicit call termination + watchdog tick).
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_p25_sm_release_lock, &expected, 1)) {
        return 0;
    }

    if (state) {
        had_force_release = (state->p25_sm_force_release != 0) ? 1 : 0;
        // Clear any pending forced-release request; we're handling teardown now.
        state->p25_sm_force_release = 0;
    }

    if (!p25_release_should_return_to_cc(ctx, opts)) {
        p25_sm_diagf(opts, state, ctx, "release_skip", "reason=%s opts_tuned=%d", reason ? reason : "none",
                     opts ? ((opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1) ? 1 : 0) : 0);
        atomic_store(&g_p25_sm_release_lock, 0);
        return 0;
    }

    p25_sm_diagf(opts, state, ctx, "release_request", "reason=%s force=%d arm_backoff=%d", reason ? reason : "none",
                 had_force_release, arm_failed_vc_backoff_on_accept);
    sm_log(opts, state, reason);

    // Return to CC. On failure/defer, leave VC state untouched so the watchdog
    // can retry through the same state machine instead of pretending the tuner moved.
    if (!p25_release_return_to_cc_accepted(ctx, opts, state, had_force_release, &tune_start_m)) {
        atomic_store(&g_p25_sm_release_lock, 0);
        return 0;
    }

    if (had_force_release || arm_failed_vc_backoff_on_accept) {
        p25_arm_failed_vc_retune_backoff(ctx, opts, state);
    }

    p25_release_clear_context(ctx);
    p25_release_clear_decoder_state(opts, state);
    p25_sm_start_cc_grace_after_tune(ctx, state, tune_start_m);

    // Transition to ON_CC state
    set_state(ctx, opts, state, P25_SM_ON_CC, "release->cc");

    atomic_store(&g_p25_sm_release_lock, 0);
    return 1;
}

/* ============================================================================
 * CC Hunting Helpers
 * ============================================================================ */

// Default hunting interval: try a new candidate every 5 seconds (aligned with op25 CC_HUNT_TIME)
#define CC_HUNT_INTERVAL_S 5.0

// Get next CC candidate (with cooldown check)
static int
next_cc_candidate(dsd_state* state, long* out_freq, double now_m) {
    return dsd_trunk_cc_candidates_next_with_flags(state, now_m, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE, out_freq);
}

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
    if (((opts && opts->p25_prefer_candidates == 1) || !has_user_lcn_list) && next_cc_candidate(state, &cand, now_m)) {
        p25_sm_diagf(opts, state, ctx, "hunt_tune_attempt",
                     "source=current-site-candidate freq=%ld sps=%d in_cand=%d in_lcn=%d in_neighbor=%d", cand, sps,
                     p25_diag_freq_in_current_site_candidates(state, cand), p25_diag_freq_in_lcn_list(state, cand),
                     p25_diag_freq_in_neighbors(state, cand));
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, cand, sps);
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
        state->p25_cc_eval_start_m = now_m;
        p25_sm_start_cc_grace_after_tune(ctx, state, now_m);
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
        dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, cand, sps);
        if (!dsd_trunk_tune_result_is_ok(tune_result)) {
            p25_sm_diagf(opts, state, ctx, "hunt_tune_result", "source=%s freq=%ld result=%s", source, cand,
                         p25_tune_result_name(tune_result));
            sm_log(opts, state,
                   tune_result == DSD_TRUNK_TUNE_RESULT_DEFERRED ? "hunt-lcn-deferred" : "hunt-lcn-failed");
            return;
        }
        p25_sm_diagf(opts, state, ctx, "hunt_tune_result", "source=%s freq=%ld result=%s", source, cand,
                     p25_tune_result_name(tune_result));
        p25_sm_start_cc_grace_after_tune(ctx, state, now_m);
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
            ctx->t_cc_sync_m = now_monotonic();
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
    double now_m = now_monotonic();
    int sync_slot = p25_sm_vc_sync_slot(ctx, state, slot);
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
    handle_voice_start(ctx, opts, state, ev->slot, "ptt");
}

static void
p25_sm_handle_event_active(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    handle_voice_start(ctx, opts, state, ev->slot, "active");
}

static void
p25_sm_handle_event_end(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    // MAC_END_PTT is an explicit call termination - trigger immediate release check.
    handle_voice_end(ctx, (dsd_opts*)opts, state, ev->slot, "end", 1, ev->observed_m);
}

static void
p25_sm_handle_event_idle(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    // MAC_IDLE may occur during brief gaps - use hangtime, not immediate release.
    handle_voice_end(ctx, (dsd_opts*)opts, state, ev->slot, "idle", 0, ev->observed_m);
}

static void
p25_sm_handle_event_tdu(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    // P1 terminator - explicit call end, trigger immediate release check.
    handle_voice_end(ctx, (dsd_opts*)opts, state, 0, "tdu", 1, ev ? ev->observed_m : 0.0);
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

typedef void (*p25_sm_event_handler_fn)(p25_sm_ctx_t*, const dsd_opts*, dsd_state*, const p25_sm_event_t*);

static const p25_sm_event_handler_fn g_p25_sm_event_handlers[] = {
    [P25_SM_EV_GRANT] = p25_sm_handle_event_grant,         [P25_SM_EV_PTT] = p25_sm_handle_event_ptt,
    [P25_SM_EV_ACTIVE] = p25_sm_handle_event_active,       [P25_SM_EV_END] = p25_sm_handle_event_end,
    [P25_SM_EV_IDLE] = p25_sm_handle_event_idle,           [P25_SM_EV_TDU] = p25_sm_handle_event_tdu,
    [P25_SM_EV_CC_SYNC] = p25_sm_handle_event_cc_sync,     [P25_SM_EV_VC_SYNC] = p25_sm_handle_event_vc_sync,
    [P25_SM_EV_SYNC_LOST] = p25_sm_handle_event_sync_lost, [P25_SM_EV_ENC] = p25_sm_handle_event_enc,
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
p25_sm_tick_on_cc_sync_from_state(p25_sm_ctx_t* ctx, const dsd_state* state) {
    if (!ctx) {
        return;
    }
    if (state && state->last_cc_sync_time_m > ctx->t_cc_sync_m) {
        ctx->t_cc_sync_m = state->last_cc_sync_time_m;
        p25_sm_set_expected_cc_nac(ctx, state, 1);
    } else {
        p25_sm_set_expected_cc_nac(ctx, state, 0);
    }
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
p25_sm_tick_on_cc_is_lost(const p25_sm_ctx_t* ctx, const dsd_state* state, double now_m, double cc_grace) {
    double cc_ts = 0.0;
    if (!ctx) {
        return 0;
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
    p25_sm_tick_on_cc_sync_from_state(ctx, state);
    if (p25_sm_tick_on_cc_nac_mismatch(ctx, opts, state, now_m)) {
        return;
    }
    p25_sm_tick_on_cc_eval_cooldown(ctx, opts, state, now_m);
    if (!p25_sm_tick_on_cc_is_lost(ctx, state, now_m, cc_grace)) {
        return;
    }
    p25_sm_diagf(opts, state, ctx, "cc_lost", "reason=timeout cc_grace=%.3f last_cc_m=%.3f now_m=%.3f", cc_grace,
                 state ? state->last_cc_sync_time_m : 0.0, now_m);
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
    double dt_voice = 0.0;
    double effective_hangtime = 0.0;
    if (!ctx || ctx->t_voice_m <= 0.0) {
        return;
    }
    dt_voice = now_m - ctx->t_voice_m;
    effective_hangtime = p25_sm_effective_hangtime(state, hangtime);
    if (dt_voice >= effective_hangtime) {
        do_release(ctx, opts, state, "hangtime-expired", 0);
    }
}

#ifdef USE_RADIO
static int
p25_cqpsk_retry_candidate(const p25_sm_ctx_t* ctx, const dsd_opts* opts, const dsd_state* state, double dt_tune) {
    if (!ctx || !opts || !state || opts->audio_in_type != AUDIO_IN_RTL) {
        return 0;
    }
    if (!ctx->vc_is_tdma || ctx->vc_cqpsk_retry_done || dt_tune < 0.8 || state->p25_vc_cqpsk_pref == 1) {
        return 0;
    }
    return 1;
}

static int
p25_cqpsk_retry_runtime_allowed(p25_sm_ctx_t* ctx, const dsd_opts* opts, long vc_freq_hz) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(opts);
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
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_freq(opts, state, ctx->vc_freq_hz, ted_sps);
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
#endif

static void
p25_sm_tick_try_cqpsk_retry(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double dt_tune) {
#ifdef USE_RADIO
    if (!p25_cqpsk_retry_candidate(ctx, opts, state, dt_tune)) {
        return;
    }

    if (!p25_cqpsk_retry_runtime_allowed(ctx, opts, ctx->vc_freq_hz)) {
        return;
    }

    int ted_sps = p25_ted_sps_for_bw(opts, 6000);
    if (!p25_cqpsk_retry_tune(ctx, opts, state, ted_sps)) {
        return;
    }
    ctx->vc_cqpsk_retry_done = 1;
    // Restart the tune timer for retry to avoid immediate timeout from the new attempt.
    ctx->t_tune_m = now_m;
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
p25_sm_tick_tuned_wait_voice(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double grant_timeout) {
    double timeout_start_m = 0.0;
    if (!ctx) {
        return;
    }
    if (ctx->t_tune_m > 0.0) {
        p25_sm_tick_try_cqpsk_retry(ctx, opts, state, now_m, now_m - ctx->t_tune_m);
    }

    timeout_start_m = p25_sm_pending_voice_grant_timeout_start_m(ctx, state);
    if (ctx->t_tune_m > timeout_start_m) {
        timeout_start_m = ctx->t_tune_m;
    }
    if (timeout_start_m <= 0.0) {
        return;
    }
    if ((now_m - timeout_start_m) >= grant_timeout) {
        do_release(ctx, opts, state, "grant-timeout", 1);
    }
}

static void
p25_sm_tick_tuned(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m, double hangtime,
                  double grant_timeout) {
    if (!ctx) {
        return;
    }
    if (ctx->slots[0].voice_active || ctx->slots[1].voice_active) {
        ctx->t_voice_m = now_m;
        return;
    }
    if (ctx->t_voice_m > 0.0 && (p25_sm_has_pending_voice_grant(ctx, state) || p25_sm_has_pending_data_grant(ctx))) {
        ctx->t_voice_m = 0.0;
    }
    if (ctx->t_voice_m > 0.0) {
        p25_sm_tick_tuned_check_hangtime(ctx, opts, state, now_m, hangtime);
        return;
    }
    p25_sm_tick_tuned_wait_voice(ctx, opts, state, now_m, grant_timeout);
}

static void
p25_sm_tick_hunting(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!ctx) {
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

    if ((unsigned)ev->type > (unsigned)P25_SM_EV_ENC) {
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

    const double now_m = now_monotonic();

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
p25_sm_emit(dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    p25_sm_event(p25_sm_get_ctx(), opts, state, ev);
}

void
p25_sm_emit_ptt(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_ptt(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_active(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_active(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
}

void
p25_sm_emit_end(dsd_opts* opts, dsd_state* state, int slot) {
    p25_sm_event_t ev = p25_sm_ev_end(slot);
    p25_sm_event(p25_sm_get_ctx(), opts, state, &ev);
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
p25_sm_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    if (!ctx) {
        ctx = p25_sm_get_ctx();
    }
    do_release(ctx, opts, state, reason ? reason : "explicit-release", 0);
}

int
p25_sm_audio_allowed(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot) {
    if (!ctx) {
        ctx = p25_sm_get_ctx();
    }
    if (!ctx || ctx->state != P25_SM_TUNED) {
        return 0;
    }
    if (!state) {
        return 0;
    }

    int s = (slot >= 0 && slot <= 1) ? slot : 0;
    return state->p25_p2_audio_allowed[s] ? 1 : 0;
}

void
p25_sm_update_audio_gate(p25_sm_ctx_t* ctx, dsd_state* state, int slot, int algid, int keyid) {
    if (!ctx) {
        ctx = p25_sm_get_ctx();
    }
    if (!ctx || !state) {
        return;
    }

    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    // Store encryption info in slot context
    ctx->slots[s].algid = algid;
    ctx->slots[s].keyid = keyid;

    // Update audio gating in state (single source of truth)
    state->p25_p2_audio_allowed[s] = slot_can_decrypt(state, s, algid) ? 1 : 0;
    if (state->p25_p2_audio_allowed[s] != 0) {
        state->p25_p2_enc_lockout_muted[s] = 0;
    }
}

/* ============================================================================
 * Encryption Lockout Helper
 * ============================================================================ */

void
p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc_bits) {
    if (!opts || !state || tg <= 0) {
        return;
    }

    p25_sm_note_encrypted_call(opts, state, tg);

    // Prepare per-slot context. Keep live encryption fields intact: callers may
    // still need ALG/KID/MI after this helper returns to keep audio gates closed.
    if ((slot & 1) == 0) {
        state->lasttg = (uint32_t)tg;
        state->dmr_so = (uint16_t)svc_bits;
        state->p25_service_options_valid[0] = (svc_bits != 0) ? 1U : 0U;
    } else {
        state->lasttgR = (uint32_t)tg;
        state->dmr_soR = (uint16_t)svc_bits;
        state->p25_service_options_valid[1] = (svc_bits != 0) ? 1U : 0U;
    }
    state->p25_policy_tg[slot & 1] = 0;
    state->gi[slot & 1] = 0;

    // Compose event text and push
    Event_History_I* eh = (state->event_history_s != NULL) ? &state->event_history_s[slot & 1] : NULL;
    if (eh) {
        DSD_SNPRINTF(eh->Event_History_Items[0].internal_str, sizeof eh->Event_History_Items[0].internal_str,
                     "Target: %d; has been locked out; Encryption Lock Out Enabled.", tg);
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
p25_ga_remove(dsd_state* state, uint32_t rid, uint16_t tg) {
    if (!state || rid == 0 || tg == 0) {
        return;
    }
    int idx = p25_ga_find_idx(state, rid, tg);
    if (idx >= 0) {
        state->p25_ga_rid[idx] = 0;
        state->p25_ga_tg[idx] = 0;
        state->p25_ga_last_seen[idx] = 0;
        if (state->p25_ga_count > 0) {
            state->p25_ga_count--;
        }
    }
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
