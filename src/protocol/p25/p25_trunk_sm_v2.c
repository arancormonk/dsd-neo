// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Simplified unified P25 trunking state machine (v2).
 */

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm_v2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Weak fallbacks for tuning functions (overridden by io/control when linked)
 * ============================================================================ */

__attribute__((weak)) void
trunk_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || !state || freq <= 0) {
        return;
    }
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    opts->p25_is_tuned = 1;
    opts->trunk_is_tuned = 1;
    double nowm = dsd_time_now_monotonic_s();
    state->last_vc_sync_time = time(NULL);
    state->p25_last_vc_tune_time = state->last_vc_sync_time;
    state->last_vc_sync_time_m = nowm;
    state->p25_last_vc_tune_time_m = nowm;
}

__attribute__((weak)) void
return_to_cc(dsd_opts* opts, dsd_state* state) {
    UNUSED2(opts, state);
}

__attribute__((weak)) void
trunk_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq) {
    UNUSED(opts);
    if (!state || freq <= 0) {
        return;
    }
    state->trunk_cc_freq = freq;
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

// Forward declaration for do_release (used by handle_enc)
static void do_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason);

static inline double
now_monotonic(void) {
    return dsd_time_now_monotonic_s();
}

// Determine if channel is TDMA based on IDEN hints
static inline int
is_tdma_channel(const dsd_state* state, int channel) {
    if (!state) {
        return 0;
    }
    int iden = (channel >> 12) & 0xF;
    if (iden >= 0 && iden < 16) {
        int is_tdma = (state->p25_chan_tdma[iden] & 0x1) ? 1 : 0;
        if (!is_tdma && state->p25_sys_is_tdma == 1) {
            is_tdma = 1; // Fallback until IDEN_UP_TDMA arrives
        }
        return is_tdma;
    }
    return 0;
}

static inline int
channel_slot(const dsd_state* state, int channel) {
    return is_tdma_channel(state, channel) ? ((channel & 1) ? 1 : 0) : -1;
}

// Log status tag for debugging
static void
sm_log(dsd_opts* opts, dsd_state* state, const char* tag) {
    if (!opts || opts->verbose < 1 || !tag) {
        return;
    }
    if (state) {
        snprintf(state->p25_sm_last_reason, sizeof(state->p25_sm_last_reason), "%s", tag);
        state->p25_sm_last_reason_time = time(NULL);
        int idx = state->p25_sm_tag_head % 8;
        snprintf(state->p25_sm_tags[idx], sizeof(state->p25_sm_tags[idx]), "%s", tag);
        state->p25_sm_tag_time[idx] = state->p25_sm_last_reason_time;
        state->p25_sm_tag_head++;
        if (state->p25_sm_tag_count < 8) {
            state->p25_sm_tag_count++;
        }
    }
    if (opts->verbose > 1) {
        fprintf(stderr, "\n[P25 SM v2] %s\n", tag);
    }
}

// Set state with logging
static void
set_state(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, p25_sm_state_e new_state, const char* reason) {
    if (!ctx || ctx->state == new_state) {
        return;
    }
    p25_sm_state_e old = ctx->state;
    ctx->state = new_state;

    // Update legacy state->p25_sm_mode for UI compatibility
    if (state) {
        switch (new_state) {
            case P25_SM_IDLE: state->p25_sm_mode = DSD_P25_SM_MODE_UNKNOWN; break;
            case P25_SM_ON_CC: state->p25_sm_mode = DSD_P25_SM_MODE_ON_CC; break;
            case P25_SM_TUNED:
                // Map TUNED to appropriate legacy mode based on voice activity
                if (ctx->slots[0].voice_active || ctx->slots[1].voice_active) {
                    state->p25_sm_mode = DSD_P25_SM_MODE_FOLLOW;
                } else if (ctx->t_voice_m > 0.0) {
                    state->p25_sm_mode = DSD_P25_SM_MODE_HANG;
                } else {
                    state->p25_sm_mode = DSD_P25_SM_MODE_ARMED;
                }
                break;
            case P25_SM_HUNTING: state->p25_sm_mode = DSD_P25_SM_MODE_HUNTING; break;
        }
    }

    if (opts && opts->verbose > 0) {
        fprintf(stderr, "\n[P25 SM v2] %s -> %s (%s)\n", p25_sm_v2_state_name(old), p25_sm_v2_state_name(new_state),
                reason ? reason : "");
    }
    sm_log(opts, state, reason);
}

// Check if a slot has recent activity (voice active or within hangtime)
static inline int
slot_is_active(const p25_sm_ctx_t* ctx, int slot, double hangtime, double now_m) {
    if (!ctx || slot < 0 || slot > 1) {
        return 0;
    }
    if (ctx->slots[slot].voice_active) {
        return 1;
    }
    if (ctx->slots[slot].last_active_m > 0.0 && (now_m - ctx->slots[slot].last_active_m) < hangtime) {
        return 1;
    }
    return 0;
}

// Check if any slot is active
static inline int
any_slot_active(const p25_sm_ctx_t* ctx, double hangtime, double now_m) {
    return slot_is_active(ctx, 0, hangtime, now_m) || slot_is_active(ctx, 1, hangtime, now_m);
}

/* ============================================================================
 * Grant Filtering (preserve existing policy logic)
 * ============================================================================ */

typedef enum {
    GRANT_GROUP = 0,
    GRANT_INDIV = 1,
} grant_kind_e;

static int
grant_allowed(dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (!opts || !state || !ev) {
        return 0;
    }

    grant_kind_e kind = ev->is_group ? GRANT_GROUP : GRANT_INDIV;
    int svc_bits = ev->svc_bits;
    int tg = ev->tg;

    // Data call policy
    if ((svc_bits & 0x10) && opts->trunk_tune_data_calls == 0) {
        sm_log(opts, state, kind == GRANT_INDIV ? "v2-indiv-blocked-data" : "v2-grant-blocked-data");
        return 0;
    }

    if (kind == GRANT_INDIV) {
        // Individual (private) call gating
        if (opts->trunk_tune_private_calls == 0) {
            sm_log(opts, state, "v2-indiv-blocked-private");
            return 0;
        }
        if ((svc_bits & 0x40) && opts->trunk_tune_enc_calls == 0) {
            sm_log(opts, state, "v2-indiv-blocked-enc");
            return 0;
        }
        if (state->tg_hold != 0) {
            sm_log(opts, state, "v2-indiv-blocked-hold");
            return 0;
        }
        return 1;
    }

    // Group grant gating
    if (opts->trunk_tune_group_calls == 0) {
        sm_log(opts, state, "v2-grant-blocked-group");
        return 0;
    }

    // Group grant: ENC policy with patch override
    if ((svc_bits & 0x40) && opts->trunk_tune_enc_calls == 0) {
        if (p25_patch_tg_key_is_clear(state, tg) || p25_patch_sg_key_is_clear(state, tg)) {
            sm_log(opts, state, "v2-enc-override-clear");
        } else {
            sm_log(opts, state, "v2-grant-blocked-enc");
            p25_emit_enc_lockout_once(opts, state, 0, tg, svc_bits);
            return 0;
        }
    }

    // Group list mode check
    char mode[8] = {0};
    if (tg > 0) {
        for (unsigned int i = 0; i < state->group_tally; i++) {
            if (state->group_array[i].groupNumber == (unsigned long)tg) {
                snprintf(mode, sizeof(mode), "%s", state->group_array[i].groupMode);
                break;
            }
        }
    }
    if (strcmp(mode, "DE") == 0 || strcmp(mode, "B") == 0) {
        sm_log(opts, state, "v2-grant-blocked-mode");
        return 0;
    }

    // TG Hold
    if (state->tg_hold != 0 && (uint32_t)tg != state->tg_hold) {
        sm_log(opts, state, "v2-grant-blocked-hold");
        return 0;
    }

    // Track RID<->TG mapping
    if (ev->src > 0 && tg > 0) {
        p25_ga_add(state, (uint32_t)ev->src, (uint16_t)tg);
    }

    return 1;
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

static void
handle_grant(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (!ctx || !ev || !opts || !state) {
        return;
    }

    // Check grant policy
    if (!grant_allowed(opts, state, ev)) {
        return;
    }

    // Compute frequency from channel
    long freq = process_channel_to_freq(opts, state, ev->channel);
    if (freq == 0) {
        sm_log(opts, state, "v2-grant-no-freq");
        return;
    }

    // Skip if already tuned to same frequency AND same TG (avoid bounce on duplicate grants)
    // Different TG/call type should still trigger a new tune
    if (ctx->state == P25_SM_TUNED && ctx->vc_freq_hz == freq && ctx->vc_tg == ev->tg) {
        sm_log(opts, state, "v2-grant-same-freq");
        return;
    }

    double now_m = now_monotonic();

    // Store VC context
    ctx->vc_freq_hz = freq;
    ctx->vc_channel = ev->channel;
    ctx->vc_tg = ev->tg;
    ctx->vc_src = ev->src;
    ctx->vc_is_tdma = is_tdma_channel(state, ev->channel);
    ctx->t_tune_m = now_m;
    ctx->t_voice_m = 0.0;

    // Clear slot activity and audio gates
    for (int s = 0; s < 2; s++) {
        ctx->slots[s].voice_active = 0;
        ctx->slots[s].allow_audio = 0;
        ctx->slots[s].last_active_m = 0.0;
        ctx->slots[s].enc_pending = 0;
        ctx->slots[s].enc_pending_tg = 0;
        ctx->slots[s].enc_confirmed = 0;
        ctx->slots[s].algid = 0;
        ctx->slots[s].keyid = 0;
        ctx->slots[s].tg = 0;
    }

    // Set symbol timing based on channel type
    if (ctx->vc_is_tdma) {
        state->samplesPerSymbol = 8;
        state->symbolCenter = 3;
        state->p25_p2_active_slot = channel_slot(state, ev->channel);
    } else {
        state->samplesPerSymbol = 10;
        state->symbolCenter = 4;
        state->p25_p2_active_slot = -1;
    }

    // Tune to VC
    trunk_tune_to_freq(opts, state, freq);
    ctx->tune_count++;
    ctx->grant_count++;
    if (state) {
        state->p25_sm_tune_count++;
    }

    set_state(ctx, opts, state, P25_SM_TUNED, "grant");
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
handle_voice_start(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot, const char* why) {
    if (!ctx) {
        return;
    }

    double now_m = now_monotonic();
    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    // Update slot activity
    ctx->slots[s].voice_active = 1;
    ctx->slots[s].last_active_m = now_m;

    // NOTE: Audio gating is NOT changed here. Audio gating is managed by:
    // 1. MAC_PTT/MAC_ACTIVE handlers in xcch.c (which set p25_p2_audio_allowed)
    // 2. ENC event handler (which gates based on encryption lockout)
    // 3. ESS processing in frame.c (which enables for clear/decryptable streams)
    //
    // This event just marks voice as active for state machine timing purposes.
    // The existing code in xcch.c/frame.c manages p25_p2_audio_allowed directly.

    ctx->t_voice_m = now_m;

    // Update UI mode to FOLLOW while in TUNED state
    if (state && ctx->state == P25_SM_TUNED) {
        state->p25_sm_mode = DSD_P25_SM_MODE_FOLLOW;
    }

    sm_log(opts, state, why);
}

static void
handle_voice_end(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, int slot, const char* why) {
    if (!ctx) {
        return;
    }

    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    // Mark voice inactive but keep last_active_m for hangtime tracking
    ctx->slots[s].voice_active = 0;

    // NOTE: Audio gating is managed by MAC_END/MAC_IDLE handlers in xcch.c
    // which set p25_p2_audio_allowed[slot] = 0. We don't change it here
    // to maintain compatibility with existing audio gating flow.

    // Update UI mode to HANG if all slots quiet (but stay in TUNED state)
    int all_quiet = (!ctx->slots[0].voice_active && !ctx->slots[1].voice_active);
    if (all_quiet && state && ctx->state == P25_SM_TUNED) {
        state->p25_sm_mode = DSD_P25_SM_MODE_HANG;
    }

    sm_log(opts, state, why);
}

static void
handle_cc_sync(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }
    ctx->t_cc_sync_m = now_monotonic();

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

    // Store encryption params in slot context
    ctx->slots[slot].algid = algid;
    ctx->slots[slot].keyid = ev->keyid;
    ctx->slots[slot].tg = tg;

    // Skip lockout processing if encryption lockout is disabled
    if (opts->trunk_tune_enc_calls != 0) {
        // Still update audio gating based on decryptability
        ctx->slots[slot].allow_audio = slot_can_decrypt(state, slot, algid) ? 1 : 0;
        state->p25_p2_audio_allowed[slot] = ctx->slots[slot].allow_audio;
        return;
    }

    // Skip if we're not currently tuned to a voice channel
    if (ctx->state != P25_SM_TUNED) {
        return;
    }

    // Skip if stream is clear or we have a key
    if (slot_can_decrypt(state, slot, algid)) {
        ctx->slots[slot].enc_pending = 0;
        ctx->slots[slot].enc_confirmed = 0;
        ctx->slots[slot].allow_audio = 1;
        state->p25_p2_audio_allowed[slot] = 1;
        return;
    }

    // Hardened dual-indication logic: require two consecutive ENC indications
    // for the same TG before triggering lockout
    if (ctx->slots[slot].enc_pending == 0 || ctx->slots[slot].enc_pending_tg != tg) {
        // First indication: remember and wait for confirmation
        ctx->slots[slot].enc_pending = 1;
        ctx->slots[slot].enc_pending_tg = tg;
        sm_log(opts, state, "v2-enc-pending");
        return;
    }

    // Second consecutive indication for same TG: confirmed encrypted
    ctx->slots[slot].enc_confirmed = 1;
    sm_log(opts, state, "v2-enc-confirmed");

    // Mark TG as encrypted in group array
    if (tg > 0) {
        int idx = -1;
        int was_de = 0;
        for (unsigned int i = 0; i < state->group_tally; i++) {
            if (state->group_array[i].groupNumber == (unsigned long)tg) {
                idx = (int)i;
                break;
            }
        }
        if (idx >= 0) {
            was_de = (strcmp(state->group_array[idx].groupMode, "DE") == 0);
            if (!was_de) {
                snprintf(state->group_array[idx].groupMode, sizeof state->group_array[idx].groupMode, "%s", "DE");
            }
        } else if (state->group_tally < (unsigned)(sizeof(state->group_array) / sizeof(state->group_array[0]))) {
            state->group_array[state->group_tally].groupNumber = (uint32_t)tg;
            snprintf(state->group_array[state->group_tally].groupMode,
                     sizeof state->group_array[state->group_tally].groupMode, "%s", "DE");
            snprintf(state->group_array[state->group_tally].groupName,
                     sizeof state->group_array[state->group_tally].groupName, "%s", "ENC LO");
            state->group_tally++;
        }

        // Emit lockout event (once per TG)
        if (idx < 0 || !was_de) {
            p25_emit_enc_lockout_once(opts, state, (uint8_t)slot, tg, 0x40);
        }
    }

    // Gate audio for this slot
    ctx->slots[slot].allow_audio = 0;
    state->p25_p2_audio_allowed[slot] = 0;
    p25_p2_audio_ring_reset(state, slot);

    // Check if opposite slot is active - only release if both slots are quiet
    int other = slot ^ 1;
    int other_active =
        ctx->slots[other].voice_active || ctx->slots[other].allow_audio || (state->p25_p2_audio_ring_count[other] > 0);

    if (!other_active) {
        do_release(ctx, opts, state, "v2-enc-lockout");
    } else {
        sm_log(opts, state, "v2-enc-lockout-slot-only");
    }
}

/* ============================================================================
 * Release to CC
 * ============================================================================ */

static void
do_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    if (!ctx) {
        return;
    }

    sm_log(opts, state, reason);

    // Clear all slot state
    for (int s = 0; s < 2; s++) {
        ctx->slots[s].voice_active = 0;
        ctx->slots[s].allow_audio = 0;
        ctx->slots[s].last_active_m = 0.0;
        ctx->slots[s].enc_pending = 0;
        ctx->slots[s].enc_pending_tg = 0;
        ctx->slots[s].enc_confirmed = 0;
        ctx->slots[s].algid = 0;
        ctx->slots[s].keyid = 0;
        ctx->slots[s].tg = 0;
    }

    // Clear VC context
    ctx->vc_freq_hz = 0;
    ctx->vc_channel = 0;
    ctx->vc_tg = 0;
    ctx->vc_src = 0;
    ctx->t_tune_m = 0.0;
    ctx->t_voice_m = 0.0;

    ctx->release_count++;
    ctx->cc_return_count++;

    // Clear legacy state fields
    if (state) {
        state->p25_p2_audio_allowed[0] = 0;
        state->p25_p2_audio_allowed[1] = 0;
        state->p25_p2_active_slot = -1;
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
        // Clear encryption state
        state->payload_algid = 0;
        state->payload_algidR = 0;
        state->payload_keyid = 0;
        state->payload_keyidR = 0;
        state->payload_miP = 0;
        state->payload_miN = 0;
        // Update release counter
        state->p25_sm_release_count++;
    }

    // Return to CC
    return_to_cc(opts, state);

    // Transition to ON_CC state
    set_state(ctx, opts, state, P25_SM_ON_CC, "release->cc");
}

/* ============================================================================
 * CC Hunting Helpers
 * ============================================================================ */

// Default hunting interval: try a new candidate every 2 seconds
#define CC_HUNT_INTERVAL_S 2.0

// Get next CC candidate (with cooldown check)
static int
next_cc_candidate(dsd_state* state, long* out_freq, double now_m) {
    if (!state || !out_freq) {
        return 0;
    }
    for (int tries = 0; tries < state->p25_cc_cand_count; tries++) {
        if (state->p25_cc_cand_idx >= state->p25_cc_cand_count) {
            state->p25_cc_cand_idx = 0;
        }
        int idx = state->p25_cc_cand_idx++;
        long f = state->p25_cc_candidates[idx];
        if (f != 0 && f != state->p25_cc_freq) {
            // Skip candidates in cooldown
            double cool_until = (idx >= 0 && idx < 16) ? state->p25_cc_cand_cool_until[idx] : 0.0;
            if (cool_until > 0.0 && now_m < cool_until) {
                continue;
            }
            *out_freq = f;
            state->p25_cc_cand_used++;
            return 1;
        }
    }
    return 0;
}

// Get next LCN frequency from user-provided list
static int
next_lcn_freq(dsd_state* state, long* out_freq) {
    if (!state || !out_freq || state->lcn_freq_count <= 0) {
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
    return 0;
}

// Try tuning to next CC candidate or LCN freq
static void
try_next_cc(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double now_m) {
    if (!ctx || !state) {
        return;
    }
    long cand = 0;

    // First try discovered CC candidates (if preference enabled)
    if (opts && opts->p25_prefer_candidates == 1 && next_cc_candidate(state, &cand, now_m)) {
        trunk_tune_to_cc(opts, state, cand);
        state->p25_cc_eval_freq = cand;
        state->p25_cc_eval_start_m = now_m;
        ctx->t_cc_sync_m = now_m; // Reset grace timer
        set_state(ctx, opts, state, P25_SM_ON_CC, "hunt-cand");
        sm_log(opts, state, "v2-hunt-cand-tune");
        return;
    }

    // Fall back to user-provided LCN list
    if (next_lcn_freq(state, &cand)) {
        trunk_tune_to_cc(opts, state, cand);
        ctx->t_cc_sync_m = now_m; // Reset grace timer
        set_state(ctx, opts, state, P25_SM_ON_CC, "hunt-lcn");
        sm_log(opts, state, "v2-hunt-lcn-tune");
        return;
    }

    // No candidates - stay in HUNTING and wait for CC_SYNC
}

/* ============================================================================
 * Public API
 * ============================================================================ */

const char*
p25_sm_v2_state_name(p25_sm_state_e state) {
    switch (state) {
        case P25_SM_IDLE: return "IDLE";
        case P25_SM_ON_CC: return "ON_CC";
        case P25_SM_TUNED: return "TUNED";
        case P25_SM_HUNTING: return "HUNT";
        default: return "?";
    }
}

void
p25_sm_v2_init(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));

    // Set defaults
    ctx->config.hangtime_s = 0.75;
    ctx->config.grant_timeout_s = 4.0;
    ctx->config.cc_grace_s = 2.0;

    // Override from opts if available
    if (opts) {
        if (opts->trunk_hangtime > 0.0) {
            ctx->config.hangtime_s = opts->trunk_hangtime;
        }
        if (opts->p25_grant_voice_to_s > 0.0) {
            ctx->config.grant_timeout_s = opts->p25_grant_voice_to_s;
        }
    }

    // Override from environment
    const char* env_hang = getenv("DSD_NEO_P25_HANGTIME");
    if (env_hang && env_hang[0]) {
        double v = atof(env_hang);
        if (v >= 0.0 && v <= 10.0) {
            ctx->config.hangtime_s = v;
        }
    }
    const char* env_grant = getenv("DSD_NEO_P25_GRANT_TIMEOUT");
    if (env_grant && env_grant[0]) {
        double v = atof(env_grant);
        if (v >= 0.0 && v <= 30.0) {
            ctx->config.grant_timeout_s = v;
        }
    }
    const char* env_cc = getenv("DSD_NEO_P25_CC_GRACE");
    if (env_cc && env_cc[0]) {
        double v = atof(env_cc);
        if (v >= 0.0 && v <= 30.0) {
            ctx->config.cc_grace_s = v;
        }
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
    } else {
        ctx->state = P25_SM_IDLE;
        if (state) {
            state->p25_sm_mode = DSD_P25_SM_MODE_UNKNOWN;
        }
    }

    ctx->initialized = 1;

    if (opts && opts->verbose > 0) {
        fprintf(stderr,
                "\n[P25 SM v2] Init: hangtime=%.2fs grant_timeout=%.2fs cc_grace=%.2fs "
                "state=%s\n",
                ctx->config.hangtime_s, ctx->config.grant_timeout_s, ctx->config.cc_grace_s,
                p25_sm_v2_state_name(ctx->state));
    }
}

int
p25_sm_v2_enabled(dsd_opts* opts) {
    UNUSED(opts);
    // v2 is now the only implementation - always enabled
    return 1;
}

void
p25_sm_v2_event(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (!ctx || !ev) {
        return;
    }

    // Auto-initialize if needed
    if (!ctx->initialized) {
        p25_sm_v2_init(ctx, opts, state);
    }

    switch (ev->type) {
        case P25_SM_EV_GRANT: handle_grant(ctx, opts, state, ev); break;

        case P25_SM_EV_PTT: handle_voice_start(ctx, opts, state, ev->slot, "ptt"); break;

        case P25_SM_EV_ACTIVE: handle_voice_start(ctx, opts, state, ev->slot, "active"); break;

        case P25_SM_EV_END: handle_voice_end(ctx, opts, state, ev->slot, "end"); break;

        case P25_SM_EV_IDLE: handle_voice_end(ctx, opts, state, ev->slot, "idle"); break;

        case P25_SM_EV_TDU:
            // P1 terminator - treat as end on slot 0
            handle_voice_end(ctx, opts, state, 0, "tdu");
            break;

        case P25_SM_EV_CC_SYNC: handle_cc_sync(ctx, opts, state); break;

        case P25_SM_EV_VC_SYNC:
            // Voice sync - update activity timestamp
            if (ctx->state == P25_SM_TUNED) {
                ctx->t_voice_m = now_monotonic();
            }
            break;

        case P25_SM_EV_SYNC_LOST:
            // Handled in tick based on timeouts
            break;

        case P25_SM_EV_ENC: handle_enc(ctx, opts, state, ev); break;
    }
}

void
p25_sm_v2_tick(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state) {
    if (!ctx) {
        return;
    }

    // Auto-initialize if needed
    if (!ctx->initialized) {
        p25_sm_v2_init(ctx, opts, state);
    }

    double now_m = now_monotonic();
    double hangtime = ctx->config.hangtime_s;
    double grant_timeout = ctx->config.grant_timeout_s;
    double cc_grace = ctx->config.cc_grace_s;

    switch (ctx->state) {
        case P25_SM_IDLE:
            // Nothing to do
            break;

        case P25_SM_ON_CC:
            // Sync CC timestamp from state if more recent
            if (state && state->last_cc_sync_time_m > ctx->t_cc_sync_m) {
                ctx->t_cc_sync_m = state->last_cc_sync_time_m;
            }
            // CC candidate evaluation cooldown: if we tuned to a candidate
            // and no CC activity appeared within the eval window, penalize
            if (state && state->p25_cc_eval_freq != 0) {
                double eval_dt = (state->p25_cc_eval_start_m > 0.0) ? (now_m - state->p25_cc_eval_start_m) : 0.0;
                double eval_window_s = 3.0;
                if (eval_dt >= eval_window_s) {
                    double cc_ts = ctx->t_cc_sync_m;
                    if (state->last_cc_sync_time_m > 0.0 && state->last_cc_sync_time_m < cc_ts) {
                        cc_ts = state->last_cc_sync_time_m;
                    }
                    int stale = (cc_ts <= 0.0) || ((now_m - cc_ts) >= eval_window_s);
                    if (stale) {
                        // Apply cooldown to the candidate
                        for (int i = 0; i < state->p25_cc_cand_count && i < 16; i++) {
                            if (state->p25_cc_candidates[i] == state->p25_cc_eval_freq) {
                                state->p25_cc_cand_cool_until[i] = now_m + 10.0;
                                break;
                            }
                        }
                    }
                    state->p25_cc_eval_freq = 0;
                    state->p25_cc_eval_start_m = 0.0;
                }
            }
            // Check for CC loss
            {
                double cc_ts = ctx->t_cc_sync_m;
                // State's timestamp takes precedence - if it's 0, CC is lost
                if (state) {
                    if (state->last_cc_sync_time_m <= 0.0) {
                        // Explicit loss - state says no CC sync
                        cc_ts = 0.0;
                    } else if (state->last_cc_sync_time_m < cc_ts) {
                        // State's timestamp is older
                        cc_ts = state->last_cc_sync_time_m;
                    }
                }
                int cc_lost = 0;
                if (cc_ts <= 0.0 && ctx->t_cc_sync_m > 0.0) {
                    // State explicitly cleared CC timestamp
                    cc_lost = 1;
                } else if (cc_ts > 0.0) {
                    double dt_cc = now_m - cc_ts;
                    if (dt_cc > cc_grace) {
                        cc_lost = 1;
                    }
                }
                if (cc_lost) {
                    set_state(ctx, opts, state, P25_SM_HUNTING, "cc-lost");
                    // Try CC candidates immediately
                    ctx->t_hunt_try_m = now_m;
                    try_next_cc(ctx, opts, state, now_m);
                }
            }
            break;

        case P25_SM_TUNED: {
            // Unified TUNED state handles: waiting for voice, active voice, and hangtime
            int has_voice = ctx->slots[0].voice_active || ctx->slots[1].voice_active;

            if (has_voice) {
                // Voice is active - update timestamp
                ctx->t_voice_m = now_m;
            } else if (ctx->t_voice_m > 0.0) {
                // Voice was active before, now in hangtime
                double dt_voice = now_m - ctx->t_voice_m;
                if (dt_voice >= hangtime) {
                    // Hangtime expired - release
                    do_release(ctx, opts, state, "v2-hangtime-expired");
                }
            } else {
                // Never saw voice - check grant timeout
                if (ctx->t_tune_m > 0.0) {
                    double dt_tune = now_m - ctx->t_tune_m;
                    if (dt_tune >= grant_timeout) {
                        // No voice seen within timeout - return to CC
                        do_release(ctx, opts, state, "v2-grant-timeout");
                    }
                }
            }
            break;
        }

        case P25_SM_HUNTING:
            // Try next CC candidate periodically
            if (ctx->t_hunt_try_m <= 0.0 || (now_m - ctx->t_hunt_try_m) >= CC_HUNT_INTERVAL_S) {
                ctx->t_hunt_try_m = now_m;
                try_next_cc(ctx, opts, state, now_m);
            }
            break;
    }

    // Age affiliation/neighbor tables (1 Hz)
    if (state) {
        p25_aff_tick(state);
        p25_ga_tick(state);
        p25_nb_tick(state);
    }
}

/* ============================================================================
 * Global Singleton
 * ============================================================================ */

static p25_sm_ctx_t g_sm_ctx;
static int g_sm_initialized = 0;

p25_sm_ctx_t*
p25_sm_v2_get(void) {
    if (!g_sm_initialized) {
        p25_sm_v2_init(&g_sm_ctx, NULL, NULL);
        g_sm_initialized = 1;
    }
    return &g_sm_ctx;
}

/* ============================================================================
 * Convenience Emit Functions
 * ============================================================================ */

void
p25_sm_v2_emit(dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev) {
    if (!p25_sm_v2_enabled(opts)) {
        return;
    }
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, ev);
}

void
p25_sm_v2_emit_ptt(dsd_opts* opts, dsd_state* state, int slot) {
    if (!p25_sm_v2_enabled(opts)) {
        return;
    }
    p25_sm_event_t ev = p25_sm_ev_ptt(slot);
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, &ev);
}

void
p25_sm_v2_emit_active(dsd_opts* opts, dsd_state* state, int slot) {
    if (!p25_sm_v2_enabled(opts)) {
        return;
    }
    p25_sm_event_t ev = p25_sm_ev_active(slot);
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, &ev);
}

void
p25_sm_v2_emit_end(dsd_opts* opts, dsd_state* state, int slot) {
    if (!p25_sm_v2_enabled(opts)) {
        return;
    }
    p25_sm_event_t ev = p25_sm_ev_end(slot);
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, &ev);
}

void
p25_sm_v2_emit_idle(dsd_opts* opts, dsd_state* state, int slot) {
    if (!p25_sm_v2_enabled(opts)) {
        return;
    }
    p25_sm_event_t ev = p25_sm_ev_idle(slot);
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, &ev);
}

void
p25_sm_v2_emit_tdu(dsd_opts* opts, dsd_state* state) {
    if (!p25_sm_v2_enabled(opts)) {
        return;
    }
    p25_sm_event_t ev = p25_sm_ev_tdu();
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, &ev);
}

void
p25_sm_v2_emit_enc(dsd_opts* opts, dsd_state* state, int slot, int algid, int keyid, int tg) {
    if (!p25_sm_v2_enabled(opts)) {
        return;
    }
    p25_sm_event_t ev = p25_sm_ev_enc(slot, algid, keyid, tg);
    p25_sm_v2_event(p25_sm_v2_get(), opts, state, &ev);
}

/* ============================================================================
 * Neighbor Update and CC Candidate Functions
 * ============================================================================ */

void
p25_sm_v2_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count) {
    if (count <= 0 || !state || !freqs) {
        return;
    }
    // Lazy-load any persisted candidates once system identity is known
    p25_cc_try_load_cache(opts, state);

    for (int i = 0; i < count; i++) {
        long f = freqs[i];
        if (f == 0) {
            continue;
        }
        // Track neighbor list for UI
        p25_nb_add(state, f);
        // Add to candidate list (dedup + FIFO rollover)
        (void)p25_cc_add_candidate(state, f, 1);
    }
}

int
p25_sm_v2_next_cc_candidate(dsd_state* state, long* out_freq) {
    if (!state || !out_freq) {
        return 0;
    }
    double nowm = now_monotonic();
    for (int tries = 0; tries < state->p25_cc_cand_count; tries++) {
        if (state->p25_cc_cand_idx >= state->p25_cc_cand_count) {
            state->p25_cc_cand_idx = 0;
        }
        int idx = state->p25_cc_cand_idx++;
        long f = state->p25_cc_candidates[idx];
        if (f != 0 && f != state->p25_cc_freq) {
            // Skip candidates currently in cooldown
            double cool_until = (idx >= 0 && idx < 16) ? state->p25_cc_cand_cool_until[idx] : 0.0;
            if (cool_until > 0.0 && nowm < cool_until) {
                continue;
            }
            *out_freq = f;
            state->p25_cc_cand_used++;
            return 1;
        }
    }
    return 0;
}

void
p25_sm_v2_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason) {
    if (!ctx) {
        ctx = p25_sm_v2_get();
    }
    do_release(ctx, opts, state, reason ? reason : "v2-explicit-release");
}

int
p25_sm_v2_audio_allowed(p25_sm_ctx_t* ctx, dsd_state* state, int slot) {
    if (!ctx) {
        ctx = p25_sm_v2_get();
    }
    if (!ctx || ctx->state != P25_SM_TUNED) {
        return 0;
    }

    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    // Check SM's allow_audio flag
    if (ctx->slots[s].allow_audio) {
        return 1;
    }

    // Fallback: check legacy state for compatibility during transition
    if (state && state->p25_p2_audio_allowed[s]) {
        return 1;
    }

    return 0;
}

void
p25_sm_v2_update_audio_gate(p25_sm_ctx_t* ctx, dsd_state* state, int slot, int algid, int keyid) {
    if (!ctx) {
        ctx = p25_sm_v2_get();
    }
    if (!ctx || !state) {
        return;
    }

    int s = (slot >= 0 && slot <= 1) ? slot : 0;

    // Store encryption info
    ctx->slots[s].algid = algid;
    ctx->slots[s].keyid = keyid;

    // Determine if audio should be allowed
    int allow = slot_can_decrypt(state, s, algid) ? 1 : 0;

    ctx->slots[s].allow_audio = allow;
    state->p25_p2_audio_allowed[s] = allow;
}
