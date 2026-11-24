// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * p25_lcw.c
 * P25p1 Link Control Word Decoding
 *
 * LWVMOBILE
 * 2023-05 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25_patch.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/position_attribution.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_callsign.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25_cc_update.h"
#include "p25_heard_tg.h"
#include "p25_unit_activity.h"
#include "phase1/p25p1_current_call.h"

static void
p25_lcw_publish_activity(dsd_state* state, uint8_t index, dsd_call_kind kind, uint64_t target, uint64_t source,
                         uint16_t channel, const char* notice) {
    dsd_call_observation observation = {
        .protocol = state->lastsynctype,
        .slot = index & 1U,
        .kind = kind,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .channel = channel,
        .frequency_hz = dsd_state_trunk_chan_valid(channel) ? state->trunk_chan_map[channel] : 0,
    };
    (void)dsd_recent_activity_publish(state, index, &observation, notice, 0U);
}

static inline int
p25_lcw_signed_offset_units(int sign_bit, int raw_offset) {
    return sign_bit ? raw_offset : -raw_offset;
}

static void
p25_lcw_store_fdma_iden(const dsd_opts* opts, dsd_state* state, int iden, long int base_freq, int chan_spac,
                        int trans_off, uint8_t bw_vu, uint8_t flags) {
    if (!state || iden < 0 || iden >= 16) {
        return;
    }
    if (!dsd_p25_iden_params_usable(base_freq, chan_spac)) {
        DSD_FPRINTF(stderr, " (unusable; not applied)");
        return;
    }
    if (dsd_state_p25_iden_tdma_applies(state, iden)) {
        DSD_FPRINTF(stderr, "; TDMA identifier, kept");
        return;
    }

    p25_invalidate_chan_map_for_iden(state, iden);

    p25_iden_entry_t* e = &state->p25_iden_fdma[iden];
    const int provenance_applies =
        (e->wacn == 0ULL && e->sysid == 0ULL) || (e->wacn == state->p2_wacn && e->sysid == state->p2_sysid);
    if (!e->populated || (e->flags & P25_IDEN_F_UNTYPED) || !provenance_applies) {
        flags |= P25_IDEN_F_UNTYPED;
        e->chan_type = 1;
    }
    e->flags = flags;
    e->base_freq = base_freq;
    e->chan_spac = chan_spac;
    e->trans_off = trans_off;
    e->bw_vu = bw_vu;
    e->trust = (state->p25_cc_freq != 0 && opts && opts->trunk_is_tuned == 0) ? 2 : 1;
    e->populated = 1;
    e->wacn = state->p2_wacn;
    e->sysid = state->p2_sysid;
    e->rfss = state->p2_rfssid;
    e->site = state->p2_siteid;
    state->p25_chan_tdma_explicit[iden] |= 1;
    p25_resolve_pending_announcements(opts, state);
}

//new p25_lcw function here -- TIA-102.AABF-D LCW Format Messages (if anybody wants to fill the rest out)
typedef struct p25_lcw_ctx {
    dsd_opts* opts;
    dsd_state* state;
    uint8_t* bits;
    uint8_t lc_format;
    uint8_t lc_opcode;
    uint8_t lc_mfid;
    uint8_t lc_svcopt;
    uint8_t lc_pf;
    uint8_t lc_sf;
    uint8_t allow_voice_start;
    uint8_t lc_hard; /* the link control passed a hard RS decode (not a soft-erasure retry; spec 2.20) */
    int is_standard_mfid;
    const dsd_event_closing_call* closing; /* the call the TDULC carrying this LC ended; NULL otherwise */
    uint64_t ended_epoch;                  /* TDULC: the call it ended, or whose end it repeats (0: none) */
} p25_lcw_ctx;

typedef void (*p25_lcw_handler_fn)(p25_lcw_ctx* ctx);

typedef struct p25_lcw_handler_entry {
    uint8_t key;
    p25_lcw_handler_fn fn;
} p25_lcw_handler_entry;

static int
p25_lcw_format_has_service_options(uint8_t lc_format) {
    switch (lc_format) {
        case 0x4A:
        case 0x46:
        case 0x45:
        case 0x44:
        case 0x03:
        case 0x00: return 1;
        default: return 0;
    }
}

static void
p25_lcw_print_service_options(const p25_lcw_ctx* ctx) {
    if (ctx->lc_svcopt & 0x80) {
        DSD_FPRINTF(stderr, " Emergency");
    }
    if (ctx->lc_svcopt & 0x40) {
        DSD_FPRINTF(stderr, " Encrypted");
    }

    if (ctx->opts->payload == 1) {
        if (ctx->lc_svcopt & 0x20) {
            DSD_FPRINTF(stderr, " Duplex");
        }
        if (ctx->lc_svcopt & 0x10) {
            DSD_FPRINTF(stderr, " Packet");
        } else {
            DSD_FPRINTF(stderr, " Circuit");
        }
        if (ctx->lc_svcopt & 0x8) {
            DSD_FPRINTF(stderr, " R");
        }
        DSD_FPRINTF(stderr, " Priority %d", ctx->lc_svcopt & 0x7);
    }
}

static void
p25_lcw_mark_encrypted_voice_pending(p25_lcw_ctx* ctx, int talkgroup, int allow_regroup_clear_override) {
    if (!ctx || !ctx->opts || !ctx->state || (ctx->lc_svcopt & 0x40) == 0) {
        return;
    }
    if (allow_regroup_clear_override && talkgroup > 0
        && (p25_patch_tg_key_is_clear(ctx->state, talkgroup) || p25_patch_sg_key_is_clear(ctx->state, talkgroup))) {
        // An identity-bearing follow-up starts a clean crypto epoch and closes
        // its media gate. KEY=0 is already authoritative clear classification,
        // so reopen the Phase 1 gate immediately for this transmission.
        ctx->state->p25_p2_audio_allowed[0] = 1;
        return;
    }
    p25_sm_emit_crypto_pending(ctx->opts, ctx->state, 0);
}

/* The call a GPS link control belongs to (issue #800, spec N2): from a TDULC, only the call that TDULC ended, captured
 * just before its end (p25p1_tdulc.c); from an LDU1, slot 0's ACTIVE Phase 1 call. Never an ended call otherwise. */
typedef struct {
    dsd_position_attribution_call view;    /* open 0: no call */
    const dsd_event_closing_call* closing; /* the terminator's closing call when bound to it */
} p25_lcw_gps_call;

static void
p25_lcw_gps_call_get(const p25_lcw_ctx* ctx, p25_lcw_gps_call* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!ctx->allow_voice_start) {
        /* A TDULC binds its call only when it ended the call it captured (ctx->ended_epoch is that call's). A TDULC the
           trunk state machine declined to end (a header opened the call, no voice frame followed) leaves its call
           open, and its link control is not shown to be that call's: no call, and no fallback to the open one. A
           repeat of the end captures nothing. */
        if (ctx->ended_epoch != 0U && ctx->closing != NULL && ctx->closing->epoch == ctx->ended_epoch
            && dsd_position_attribution_call_from_closing(ctx->closing, &out->view)) {
            out->closing = ctx->closing;
        }
        return;
    }
    /* Only a P25 Phase 1 call: another protocol's call still open on the slot is not the one this link control
       rides on. */
    if (!dsd_position_attribution_call_from_slot(ctx->state, 0U, &out->view)
        || !DSD_SYNC_IS_P25P1(out->view.protocol)) {
        DSD_MEMSET(&out->view, 0, sizeof(out->view));
    }
}

/* The voice-user evidence (issue #800, spec 2.2): p25_p1_lc_attribution, the record of who a later GPS link control
 * of the call names. A hard-decoded voice user naming a radio by a complete local address records it once the call
 * (the one p25_lcw_gps_call_get() names: from a TDULC the call it ended, from an LDU1 slot 0's ACTIVE Phase 1 call) carries exactly
 * this target and source. Fail closed: any other voice user takes the record away unless it only repeats the record's
 * own source by a soft decode. That is one naming a special address in its source field (p25_source_id_is_subscriber():
 * no radio), a source that is not a complete local address (@p source_local 0: the Source ID Extension Required flag,
 * or LCO $4A), another radio by a soft decode, or a radio the call does not hold, whether or not the call keeps its
 * earlier talker for display. A source of 0 with @p source_local omits the field and leaves the record, as the trunk
 * state machine reads it. */
static void
p25_lcw_note_checked_identity(p25_lcw_ctx* ctx, uint32_t target, uint32_t source, uint8_t group, int source_local) {
    dsd_position_attribution* evidence = &ctx->state->p25_p1_lc_attribution;
    if (source == 0U && source_local) {
        return;
    }
    const int names_radio = source_local && p25_source_id_is_subscriber(source);
    if (names_radio && ctx->lc_hard) {
        p25_lcw_gps_call call;
        p25_lcw_gps_call_get(ctx, &call);
        if (call.view.open && call.view.target == (uint64_t)target
            && dsd_position_attribution_learn(ctx->state, &call.view, source, target, group,
                                              DSD_POSITION_ATTRIBUTION_BIND_EPOCH, evidence)) {
            return;
        }
    } else if (names_radio && evidence->call_epoch != 0U && source == evidence->source) {
        return;
    }
    dsd_position_attribution_clear(evidence);
}

/* A link control reaches the trunk state machine with its identity checked only when it was hard-decoded (spec 2.20); a
 * soft-erasure retry also succeeds, but its identity stays unchecked. The identity it names reaches the
 * trunk state machine with the verdict of that decode (spec 2.19), and a source that is not a complete local address
 * (@p source_local 0: Source ID Extension Required, LCO $4A) as p25_sm_event_t::source_nonlocal, never checked. */
static int
p25_lcw_emit_voice_user(p25_lcw_ctx* ctx, int tg, int dst, uint32_t source, int is_group, int source_local) {
    p25_sm_event_t ev = p25_sm_ev_active_call(0, tg, dst, (int)source, is_group, ctx->lc_svcopt);
    ev.identity_checked = ctx->lc_hard ? P25_SM_IDENTITY_CHECKED_ALL : 0U;
    ev.source_nonlocal = (uint8_t)(source_local ? 0U : 1U);
    return p25_sm_emit_voice_start(ctx->opts, ctx->state, &ev);
}

/* A talker alias belongs to its call. The voice user repeats in every LDU1 of a transmission while the alias is sent
 * in pieces across the superframes after it, so a voice user that goes on with (@p accepted) the open call the alias
 * was published for, under the reception it was published under (dsd_state::generic_talker_alias_epoch, _rx_gen),
 * naming that call's own talker the way the call's earlier voice users did, keeps it: dropping it at every LDU1 made the
 * alias flash on and off. Anything else drops it: a new call or talker, a start the trunk state machine declined, a
 * TDULC, another system named on the carrier, no source or a special address (a telephone interconnect, a dispatcher),
 * the same number switching between a local address and one needing a Source ID Extension (another system's radio),
 * an alias from an earlier call or set outside call enrichment. */
static void
p25_lcw_drop_alias_unless_continued(const p25_lcw_ctx* ctx, const dsd_call_snapshot* call, int accepted,
                                    uint32_t source, int source_local) {
    dsd_state* state = ctx->state;
    const int open = call != NULL;
    const uint8_t local = (uint8_t)(source_local != 0);
    const int locality_flipped =
        open && accepted && state->p25_p1_voice_user_epoch == call->epoch && state->p25_p1_voice_user_local != local;
    if (open && accepted) {
        state->p25_p1_voice_user_epoch = call->epoch;
        state->p25_p1_voice_user_local = local;
    }
    if (!accepted || !open || !p25_source_id_is_subscriber(source) || locality_flipped || call->ota_source_id != source
        || call->epoch != state->generic_talker_alias_epoch[0]
        || state->generic_talker_alias_rx_gen[0] != dsd_state_reception_generation(state)) {
        state->generic_talker_alias[0][0] = '\0';
    }
}

static int
p25_lcw_accept_private_voice_user(p25_lcw_ctx* ctx, uint32_t target, uint32_t source, int source_local) {
    ctx->state->dmr_so = ctx->lc_svcopt;

    const int accepted =
        ctx->allow_voice_start && p25_lcw_emit_voice_user(ctx, 0, (int)target, source, 0, source_local);
    dsd_call_snapshot call;
    const int open = p25p1_current_call(ctx->state, &call);
    p25_lcw_drop_alias_unless_continued(ctx, open ? &call : NULL, accepted, source, source_local);
    if (!accepted) {
        return 0;
    }
    p25_lcw_mark_encrypted_voice_pending(ctx, 0, 0);
    return 1;
}

/* A group voice user, standard (LCO 0x00) or a Motorola patched supergroup (LCGRGR, @p regroup 1): record its service
 * options and note the affiliation, then start the voice, dropping the previous talker's alias unless the call goes on
 * (p25_lcw_drop_alias_unless_continued()). A rejected start (or a TDULC, which starts none) returns before the LC's
 * encrypted service option marks the crypto pending. An accepted one from a hard RS decode proves the group of the
 * call it started or continued (issue #903): the LDU1's own checked link control for this transmission, a supergroup
 * when it is a regroup voice user. A soft-erasure retry proves nothing. */
static int
p25_lcw_accept_group_voice_user(p25_lcw_ctx* ctx, uint16_t group, uint32_t source, int source_local, int regroup) {
    ctx->state->dmr_so = ctx->lc_svcopt;

    if (source != 0 && group != 0) {
        p25_ga_add(ctx->state, (uint32_t)source, (uint16_t)group);
    }

    const int accepted = ctx->allow_voice_start && p25_lcw_emit_voice_user(ctx, group, 0, source, 1, source_local);
    dsd_call_snapshot call;
    const int open = p25p1_current_call(ctx->state, &call);
    p25_lcw_drop_alias_unless_continued(ctx, open ? &call : NULL, accepted, source, source_local);
    if (!accepted) {
        return 0;
    }
    if (open && ctx->lc_hard) {
        p25_heard_tg_note(ctx->opts, ctx->state, 0U, &call, group, regroup);
    }
    p25_lcw_mark_encrypted_voice_pending(ctx, group, 1);
    return 1;
}

static void
p25_lcw_handle_format_00(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Group Voice Channel User");
    uint8_t res = (uint8_t)convert_bits_into_output(&ctx->bits[24], 7);
    /* S, Source ID Extension Required (TIA-102.AABF-D §7.3.1): octet 3 bit 0. */
    uint8_t explicit_src = ctx->bits[31];
    uint16_t group = (uint16_t)convert_bits_into_output(&ctx->bits[32], 16);
    uint32_t source = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " - Group %d Source %d", group, source);
    UNUSED(res);

    (void)p25_lcw_accept_group_voice_user(ctx, group, source, explicit_src == 0U, 0);
    p25_lcw_note_checked_identity(ctx, group, source, 1U, explicit_src == 0U);
}

static void
p25_lcw_handle_format_03(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Unit to Unit Voice Channel User");
    uint32_t target = (uint32_t)convert_bits_into_output(&ctx->bits[24], 24);
    uint32_t source = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " - Target %d Source %d", target, source);

    (void)p25_lcw_accept_private_voice_user(ctx, target, source, 1);
    p25_lcw_note_checked_identity(ctx, target, source, 0U, 1);
}

static void p25_lcw_offer_preemption(p25_lcw_ctx* ctx, const uint16_t* channels, const uint16_t* groups,
                                     const int* svcs, int count);

static void
p25_lcw_handle_format_42(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Group Voice Channel Update - ");
    uint16_t channel1 = (uint16_t)convert_bits_into_output(&ctx->bits[8], 16);
    uint16_t group1 = (uint16_t)convert_bits_into_output(&ctx->bits[24], 16);
    uint16_t channel2 = (uint16_t)convert_bits_into_output(&ctx->bits[40], 16);
    uint16_t group2 = (uint16_t)convert_bits_into_output(&ctx->bits[56], 16);

    if (channel1 && group1) {
        DSD_FPRINTF(stderr, "Ch: %04X TG: %d; ", channel1, group1);
        char suf[32];
        p25_format_chan_suffix(ctx->state, channel1, -1, suf, sizeof suf);
        char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
        DSD_SNPRINTF(notice, sizeof notice, "Active Ch: %04X%s TG: %d; ", channel1, suf, group1);
        p25_lcw_publish_activity(ctx->state, 0U, DSD_CALL_KIND_GROUP_VOICE, group1, 0U, channel1, notice);
    } else {
        (void)dsd_recent_activity_clear(ctx->state, 0U);
    }

    if (channel2 && group2 && group1 != group2) {
        DSD_FPRINTF(stderr, "Ch: %04X TG: %d; ", channel2, group2);
        char suf[32];
        p25_format_chan_suffix(ctx->state, channel2, -1, suf, sizeof suf);
        char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
        DSD_SNPRINTF(notice, sizeof notice, "Active Ch: %04X%s TG: %d; ", channel2, suf, group2);
        p25_lcw_publish_activity(ctx->state, 1U, DSD_CALL_KIND_GROUP_VOICE, group2, 0U, channel2, notice);
    } else {
        (void)dsd_recent_activity_clear(ctx->state, 1U);
    }

    // The format carries no service options: octets 1-2 are channel 1, not the generic decoder's svcopt.
    const uint16_t channels[2] = {channel1, channel2};
    const uint16_t groups[2] = {group1, group1 != group2 ? group2 : 0U};
    const int svcs[2] = {P25_SM_SVC_UNKNOWN, P25_SM_SVC_UNKNOWN};
    p25_lcw_offer_preemption(ctx, channels, groups, svcs, 2);
}

static int
p25_lcw_trunk_cc_ready_for_grant(p25_lcw_ctx* ctx) {
    if (ctx->opts->trunk_enable != 1) {
        return 0;
    }
    return ctx->state->p25_cc_freq != 0 || ctx->state->trunk_cc_freq > 0;
}

/* Calls an FDMA voice channel announces in progress elsewhere (0x42, 0x44 without -j, Motorola LCGRGU). They reach
 * the trunking SM only as preemption candidates (p25_sm_event_t.preempt_only): a talkgroup whose policy asks to
 * preempt may take the receiver from a lower-priority call, and every other announcement leaves the SM untouched.
 * Several are offered audible before silent probe (as on Phase 2), then held talkgroup first, then highest priority,
 * and the first to take the receiver ends the offer. */
/* Whether candidate @p b goes before @p a: audible before silent probe, admitted first, then the held talkgroup, then
 * the higher priority. */
static int
p25_lcw_outranks(const p25_sm_grant_rank_t* b, int b_probe, const p25_sm_grant_rank_t* a, int a_probe) {
    if (b_probe != a_probe) {
        return b_probe < a_probe;
    }
    if (b->tune_allowed != a->tune_allowed) {
        return b->tune_allowed > a->tune_allowed;
    }
    if (b->hold_match != a->hold_match) {
        return b->hold_match > a->hold_match;
    }
    return b->priority > a->priority;
}

static void
p25_lcw_offer_preemption(p25_lcw_ctx* ctx, const uint16_t* channels, const uint16_t* groups, const int* svcs,
                         int count) {
    p25_sm_event_t events[2];
    p25_sm_grant_rank_t ranks[2];
    int probes[2];
    int order[2];
    int n = 0;
    if (!p25_lcw_trunk_cc_ready_for_grant(ctx) || ctx->opts->trunk_tune_group_calls != 1) {
        return;
    }
    for (int i = 0; i < count && n < 2; i++) {
        if (channels[i] == 0U || groups[i] == 0U) {
            continue;
        }
        events[n] = p25_sm_ev_group_grant_update(channels[i], 0, groups[i], 0, svcs[i]);
        events[n].preempt_only = 1;
        // The link control's own fields, checked only when it passed a hard RS decode (spec 2.20).
        events[n].identity_checked = ctx->lc_hard ? P25_SM_IDENTITY_CHECKED_ALL : 0U;
        if (!p25_sm_grant_candidate_rank(ctx->opts, ctx->state, &events[n], &ranks[n])) {
            DSD_MEMSET(&ranks[n], 0, sizeof(ranks[n]));
        }
        probes[n] = p25_sm_grant_is_silent_probe(ctx->opts, ctx->state, groups[i], svcs[i]);
        order[n] = n;
        n++;
    }
    if (n == 2 && p25_lcw_outranks(&ranks[1], probes[1], &ranks[0], probes[0])) {
        order[0] = 1;
        order[1] = 0;
    }
    p25_sm_ctx_t* sm = p25_sm_get_ctx();
    for (int i = 0; i < n; i++) {
        const long held_freq = sm->vc_freq_hz;
        p25_sm_event(sm, ctx->opts, ctx->state, &events[order[i]]);
        if (sm->vc_freq_hz != held_freq) {
            break;
        }
    }
}

static void
p25_lcw_store_current_site_status(p25_lcw_ctx* ctx, uint8_t lra, int lra_valid, int network_active,
                                  int network_active_valid, uint8_t rfssid, uint8_t siteid) {
    if (!ctx || !ctx->state) {
        return;
    }
    if (lra_valid) {
        p25_store_site_lra(ctx->state, lra);
    }
    if (network_active_valid) {
        p25_store_site_network_active(ctx->state, (uint8_t)(network_active ? 1U : 0U));
    }
    ctx->state->p2_rfssid = rfssid;
    ctx->state->p2_siteid = siteid;
    p25_confirm_idens_for_current_site(ctx->state);
}

static void
p25_lcw_update_primary_control_channel(p25_lcw_ctx* ctx, uint32_t wacn, uint16_t sysid, uint16_t channel, int seed_lcn0,
                                       const char* label) {
    if (!ctx || !ctx->state) {
        return;
    }

    /* The network status names the system the patch table belongs to, whatever the seed or the tuning gate below. */
    (void)dsd_p25_patch_note_system(ctx->state, wacn, sysid);
    long int cc_freq = process_channel_to_freq(ctx->opts, ctx->state, channel);
    int accepted_cc = p25_cc_update_primary_from_network_status(ctx->opts, ctx->state, cc_freq);
    const int metadata_allowed = accepted_cc || !p25_cc_update_is_voice_tuned(ctx->opts);
    if (metadata_allowed) {
        if (ctx->state->p2_hardset == 0) {
            (void)p25_update_system_identity(ctx->state, (unsigned long long)wacn, (unsigned long long)sysid);
        }
        ctx->state->p25_cc_is_tdma = 0;
    }
    if (accepted_cc) {
        const long neigh[1] = {ctx->state->p25_cc_freq};
        p25_cc_record_neighbor_frequencies(ctx->opts, ctx->state, neigh, 1);
        if (seed_lcn0 && !dsd_state_trunk_lcn_user_list_present(ctx->opts, ctx->state)
            && (ctx->state->trunk_lcn_freq[0] == 0 || ctx->state->trunk_lcn_freq[0] != ctx->state->p25_cc_freq)) {
            (void)dsd_state_trunk_lcn_freq_write(ctx->state, 0, ctx->state->p25_cc_freq);
        }
        p25_confirm_idens_for_current_site(ctx->state);
    } else if (cc_freq > 0) {
        DSD_FPRINTF(stderr, "\n  %s: ignoring CC update while voice-tuned (freq=%ld)", label, cc_freq);
    } else {
        DSD_FPRINTF(stderr, "\n  %s: ignoring invalid channel->freq (CHAN-T=%04X)", label, channel);
    }
}

static int
p25_lcw_format_44_hold_blocks_grant(const p25_lcw_ctx* ctx, uint16_t group) {
    return ctx->state->tg_hold != 0 && ctx->state->tg_hold != group;
}

static void
p25_lcw_warn_format_44_retune_disabled(p25_lcw_ctx* ctx) {
    if (ctx->state->p25_lcw_retune_disabled_warned != 0) {
        return;
    }
    ctx->state->p25_lcw_retune_disabled_warned = 1;
    DSD_FPRINTF(stderr,
                " [WARN: P25 LCW explicit retune is disabled; 0x44 grants may not be followed. Enable it from the "
                "terminal menu.] ");
}

static void
p25_lcw_handle_format_44_trunking(p25_lcw_ctx* ctx, uint16_t channel, uint16_t group) {
    if (!p25_lcw_trunk_cc_ready_for_grant(ctx)) {
        return;
    }
    if (ctx->opts->p25_lcw_retune == 0) {
        p25_lcw_warn_format_44_retune_disabled(ctx);
        // -j only governs following explicit updates; a preempting talkgroup announced this way still preempts.
        const uint16_t channels[1] = {channel};
        const uint16_t groups[1] = {group};
        const int svcs[1] = {ctx->lc_svcopt};
        p25_lcw_offer_preemption(ctx, channels, groups, svcs, 1);
        return;
    }
    if (ctx->opts->p25_lcw_retune == 1 && ctx->opts->trunk_tune_group_calls == 1
        && !p25_lcw_format_44_hold_blocks_grant(ctx, group)) {
        p25_sm_event_t ev = p25_sm_ev_group_grant_update(channel, 0, group, 0, ctx->lc_svcopt);
        ev.identity_checked =
            ctx->lc_hard ? P25_SM_IDENTITY_CHECKED_ALL : 0U; // the RS-decoded link control's own fields
        p25_sm_event(p25_sm_get_ctx(), ctx->opts, ctx->state, &ev);
    }
}

static void
p25_lcw_handle_format_44(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Group Voice Channel Update %s Explicit", dsd_unicode_or_ascii("–", "-"));
    uint16_t group1 = (uint16_t)convert_bits_into_output(&ctx->bits[24], 16);
    uint16_t channelt = (uint16_t)convert_bits_into_output(&ctx->bits[40], 16);
    uint16_t channelr = (uint16_t)convert_bits_into_output(&ctx->bits[56], 16);
    DSD_FPRINTF(stderr, "Ch: %04X TG: %d; ", channelt, group1);
    UNUSED(channelr);

    p25_lcw_handle_format_44_trunking(ctx, channelt, group1);

    char suf[32];
    p25_format_chan_suffix(ctx->state, channelt, -1, suf, sizeof suf);
    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    DSD_SNPRINTF(notice, sizeof notice, "Active Ch: %04X%s TG: %d; ", channelt, suf, group1);
    p25_lcw_publish_activity(ctx->state, 0U, DSD_CALL_KIND_GROUP_VOICE, group1, 0U, channelt, notice);
}

static void
p25_lcw_handle_format_45(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Unit to Unit Answer Request");
}

static void
p25_lcw_handle_format_46(p25_lcw_ctx* ctx) {
    uint16_t timer = (uint16_t)convert_bits_into_output(&ctx->bits[32], 16);
    uint32_t target = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " Telephone Interconnect Voice Channel User - Target %d Timer %.1fs", target,
                (double)timer / 10.0);

    int identity_accepted = p25_lcw_accept_private_voice_user(ctx, target, 0, 1);
    if (ctx->allow_voice_start && !identity_accepted) {
        return;
    }
    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    DSD_SNPRINTF(notice, sizeof notice, "TELE Target: %d Timer: %.1fs; ", target, (double)timer / 10.0);
    p25_lcw_publish_activity(ctx->state, 0U, DSD_CALL_KIND_PRIVATE_VOICE, target, 0U, 0U, notice);
}

static void
p25_lcw_handle_format_47(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Telephone Interconnect Answer Request");
}

/* A full SUID (WACN, System ID, unit) naming the open Phase 1 call's talker (the Source ID Extension $49, Tait's
 * Subscriber FQ-SUID): the call takes the unit ID for display. A home of another system, or one not known yet, is no
 * local radio: the trunk state machine's record withdraws the source's verdict, even when the number is the one a local
 * voice user checked, so no alias is learned for it (dsd_call_state_source_learnable()). */
static void
p25_lcw_note_full_suid_source(const p25_lcw_ctx* ctx, uint32_t wacn, uint16_t sysid, uint32_t src) {
    dsd_call_snapshot call;
    if (src == 0U || !p25p1_current_call(ctx->state, &call)) {
        return;
    }
    const dsd_call_observation observation = {
        .protocol = call.protocol,
        .slot = 0U,
        .kind = call.kind,
        .ota_target_id = call.ota_target_id,
        .policy_target_id = call.policy_target_id,
        .ota_source_id = src,
        .channel = call.channel,
        .frequency_hz = call.frequency_hz,
        .service_options = call.service_options,
        .emergency = call.emergency,
        .priority = call.priority,
        .has_service_metadata = call.has_service_metadata,
    };
    (void)dsd_call_state_observe(ctx->state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
    if (!p25_sm_suid_home_is_local(ctx->state, wacn, sysid) && call.ota_target_id <= UINT32_MAX) {
        p25_sm_note_call_identity(ctx->state, 0, src, (uint32_t)call.ota_target_id,
                                  call.kind == DSD_CALL_KIND_GROUP_VOICE, 0U, 1U);
    }
    dsd_event_sync_slot(ctx->opts, ctx->state, 0U);
    dsd_signal_search_note_call(ctx->state, 0U);
}

static void
p25_lcw_handle_format_49(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Source ID Extension -");
    uint32_t wacn = (uint32_t)convert_bits_into_output(&ctx->bits[16], 20);
    uint16_t sysid = (uint16_t)convert_bits_into_output(&ctx->bits[36], 12);
    uint32_t src = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " Full SUID: WACN %05X SYSID %03X SRC %d", wacn, sysid, src);
    if (wacn != 0) {
        ctx->state->p25_src_nid = wacn;
    }
    p25_lcw_note_full_suid_source(ctx, wacn, sysid, src);
}

static void
p25_lcw_handle_format_4a(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Unit to Unit Voice Channel User %s Extended", dsd_unicode_or_ascii("–", "-"));
    uint32_t target = (uint32_t)convert_bits_into_output(&ctx->bits[24], 24);
    uint32_t src = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, "TGT: %d; SRC: %d; ", target, src);
    /* The Source ID is not a complete local address: S is always 1 (TIA-102.AABF-D §7.3.29). */
    (void)p25_lcw_accept_private_voice_user(ctx, target, src, 0);
    p25_lcw_note_checked_identity(ctx, target, src, 0U, 0);
}

/* Group Affiliation Query (TIA-102.AABF-D §7.3.9): octets 1-2 reserved, Target Address octets 3-5, Source Address
   octets 6-8. It asks the target to report its affiliation and names no group, so no affiliation is learned from it. */
static void
p25_lcw_handle_format_50(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Group Affiliation Query");
    uint32_t target = (uint32_t)convert_bits_into_output(&ctx->bits[24], 24);
    uint32_t source = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " - TGT %u SRC %u", target, source);
}

static void
p25_lcw_handle_format_51(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Unit Registration Command");
}

static void
p25_lcw_handle_format_52(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Unit Authentication Command - OBSOLETE");
}

static void
p25_lcw_handle_format_53(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Status Query");
}

static void
p25_lcw_handle_format_54(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Status Update");
}

/* Message Update (TIA-102.AABF-D §7.3.19) and Message Update - Source ID Extension Required (§7.3.31, LCO 27): the
   16-bit Message octets 1-2, Target Address octets 3-5, Source Address (Source ID) octets 6-8. The link control word
   reached here only after its FEC corrected it (p25p1_ldu1.c, p25p1_tdulc.c), so its message is a unit activity row.
   The extension form's Source ID is qualified by the Source ID Extension (LCO 9) that follows it: it is no local
   address, and with no home system yet the row keeps it nonlocal (never named from the local lists). */
static void
p25_lcw_print_message_update(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, uint8_t source_nonlocal) {
    uint16_t message = (uint16_t)convert_bits_into_output(&bits[8], 16);
    uint32_t target = (uint32_t)convert_bits_into_output(&bits[24], 24);
    uint32_t source = (uint32_t)convert_bits_into_output(&bits[48], 24);
    DSD_FPRINTF(stderr, " - MSG %04X TGT %u SRC %u", message, target, source);
    const p25_ua_origin origin = {opts, state, 0U, 0U, 1U};
    const p25_ua_unit reporter = {source, {0U, 0U, 0U, 0U}, 0U, source_nonlocal};
    const p25_ua_unit recipient = {target, {0U, 0U, 0U, 0U}, 0U, 0U};
    p25_ua_message_update(&origin, message, &recipient, &reporter);
}

static void
p25_lcw_handle_format_55(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Message Update");
    p25_lcw_print_message_update(ctx->opts, ctx->state, &ctx->bits[0], 0U);
}

static void
p25_lcw_handle_format_5b(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Message Update %s Source ID Extension Required", dsd_unicode_or_ascii("–", "-"));
    p25_lcw_print_message_update(ctx->opts, ctx->state, &ctx->bits[0], 1U);
}

static void
p25_lcw_handle_format_56(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Call Alert");
}

static void
p25_lcw_handle_format_57(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Extended Function Command");
}

static void
p25_lcw_handle_format_58(p25_lcw_ctx* ctx) {
    uint8_t iden = (uint8_t)convert_bits_into_output(&ctx->bits[8], 4);
    int bw = (int)convert_bits_into_output(&ctx->bits[12], 9);
    int sign = ctx->bits[21] & 1;
    int tx_raw = (int)convert_bits_into_output(&ctx->bits[22], 8);
    int chan_spac = (int)convert_bits_into_output(&ctx->bits[30], 10);
    uint32_t base = (uint32_t)convert_bits_into_output(&ctx->bits[40], 32);
    int trans_off = p25_lcw_signed_offset_units(sign, tx_raw);
    DSD_FPRINTF(stderr, " Channel Identifier Update; Iden: %X; BW: %X; TX Offset: %d; Spacing: %X; Base: %lld;", iden,
                bw, trans_off, chan_spac, (long long)base * 5LL);
    p25_lcw_store_fdma_iden(ctx->opts, ctx->state, iden, (long int)base, chan_spac, trans_off, 0, 0);
}

static void
p25_lcw_handle_format_59(p25_lcw_ctx* ctx) {
    uint8_t iden = (uint8_t)convert_bits_into_output(&ctx->bits[8], 4);
    uint8_t bw_vu = (uint8_t)convert_bits_into_output(&ctx->bits[12], 4);
    int sign = ctx->bits[16] & 1;
    int tx_raw = (int)convert_bits_into_output(&ctx->bits[17], 13);
    int chan_spac = (int)convert_bits_into_output(&ctx->bits[30], 10);
    uint32_t base = (uint32_t)convert_bits_into_output(&ctx->bits[40], 32);
    int trans_off = p25_lcw_signed_offset_units(sign, tx_raw);
    DSD_FPRINTF(stderr, " Channel Identifier Update VU; Iden: %X; BW: %X; TX Offset: %d; Spacing: %X; Base: %lld;",
                iden, bw_vu, trans_off, chan_spac, (long long)base * 5LL);
    p25_lcw_store_fdma_iden(ctx->opts, ctx->state, iden, (long int)base, chan_spac, trans_off, bw_vu,
                            P25_IDEN_F_OFF_SPACING);
}

static void
p25_lcw_handle_format_5a(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Status Update %s Source ID Extension Required", dsd_unicode_or_ascii("–", "-"));
}

static void
p25_lcw_handle_format_5c(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Extended Function Command %s Source ID Extension Required", dsd_unicode_or_ascii("–", "-"));
}

static void
p25_lcw_handle_format_60(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " System Service Broadcast");
    uint8_t request_priority = (uint8_t)convert_bits_into_output(&ctx->bits[20], 4);
    uint32_t available = (uint32_t)convert_bits_into_output(&ctx->bits[24], 24);
    uint32_t supported = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " RPL [%X] SSA [%06X] SSS [%06X]", request_priority, available, supported);
    p25_store_system_service_broadcast(ctx->state, available, supported, request_priority);
}

static void
p25_lcw_handle_format_61(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Secondary Control Channel Broadcast");
    uint8_t rfssid = (uint8_t)convert_bits_into_output(&ctx->bits[8], 8);
    uint8_t siteid = (uint8_t)convert_bits_into_output(&ctx->bits[16], 8);
    uint16_t channel_a = (uint16_t)convert_bits_into_output(&ctx->bits[24], 16);
    uint8_t ssc_a = (uint8_t)convert_bits_into_output(&ctx->bits[40], 8);
    uint16_t channel_b = (uint16_t)convert_bits_into_output(&ctx->bits[48], 16);
    uint8_t ssc_b = (uint8_t)convert_bits_into_output(&ctx->bits[64], 8);
    DSD_FPRINTF(stderr, " - RFSS %d Site %d CH A %04X SSC %02X CH B %04X SSC %02X", rfssid, siteid, channel_a, ssc_a,
                channel_b, ssc_b);
    (void)p25_announce_secondary_cc_channel(ctx->opts, ctx->state, channel_a, rfssid, siteid, ssc_a);
    if (channel_b != channel_a && channel_b != 0xFFFFU && ssc_b != 0U) {
        (void)p25_announce_secondary_cc_channel(ctx->opts, ctx->state, channel_b, rfssid, siteid, ssc_b);
    }
}

static void
p25_lcw_handle_format_62(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Adjacent Site Status Broadcast");
    uint8_t lra = (uint8_t)convert_bits_into_output(&ctx->bits[8], 8);
    uint8_t cfva = (uint8_t)convert_bits_into_output(&ctx->bits[16], 4);
    uint16_t sysid = (uint16_t)convert_bits_into_output(&ctx->bits[20], 12);
    uint8_t rfssid = (uint8_t)convert_bits_into_output(&ctx->bits[32], 8);
    uint8_t siteid = (uint8_t)convert_bits_into_output(&ctx->bits[40], 8);
    uint16_t channel = (uint16_t)convert_bits_into_output(&ctx->bits[48], 16);
    uint8_t ssc = (uint8_t)convert_bits_into_output(&ctx->bits[64], 8);
    DSD_FPRINTF(stderr, " - LRA %02X SYS %03X RFSS %d Site %d CH %04X SSC %02X", lra, sysid, rfssid, siteid, channel,
                ssc);
    const p25_neighbor_channel_announcement_t announcement = {
        .channel = channel,
        .sysid = sysid,
        .rfss = rfssid,
        .site = siteid,
        .lra = lra,
        .cfva = cfva,
        .lra_valid = 1U,
        .cfva_valid = 1U,
    };
    (void)p25_announce_neighbor_channel(ctx->opts, ctx->state, &announcement);
}

static void
p25_lcw_handle_format_63(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " RFSS Status Broadcast");
    uint8_t lra = (uint8_t)convert_bits_into_output(&ctx->bits[8], 8);
    int network_active = ctx->bits[19] ? 1 : 0;
    uint16_t sysid = (uint16_t)convert_bits_into_output(&ctx->bits[20], 12);
    uint8_t rfssid = (uint8_t)convert_bits_into_output(&ctx->bits[32], 8);
    uint8_t siteid = (uint8_t)convert_bits_into_output(&ctx->bits[40], 8);
    uint16_t channel = (uint16_t)convert_bits_into_output(&ctx->bits[48], 16);
    uint8_t ssc = (uint8_t)convert_bits_into_output(&ctx->bits[64], 8);
    DSD_FPRINTF(stderr, " - LRA %02X SYS %03X RFSS %d Site %d CH %04X SSC %02X A %d", lra, sysid, rfssid, siteid,
                channel, ssc, network_active);
    (void)process_channel_to_freq(ctx->opts, ctx->state, channel);
    p25_lcw_store_current_site_status(ctx, lra, 1, network_active, 1, rfssid, siteid);
}

static void
p25_lcw_handle_format_64(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Network Status Broadcast");
    uint32_t wacn = (uint32_t)convert_bits_into_output(&ctx->bits[16], 20);
    uint16_t sysid = (uint16_t)convert_bits_into_output(&ctx->bits[36], 12);
    uint16_t channel = (uint16_t)convert_bits_into_output(&ctx->bits[48], 16);
    uint8_t ssc = (uint8_t)convert_bits_into_output(&ctx->bits[64], 8);
    DSD_FPRINTF(stderr, " - WACN %05X SYS %03X CH %04X SSC %02X", wacn, sysid, channel, ssc);
    p25_lcw_update_primary_control_channel(ctx, wacn, sysid, channel, 1, "P25 LCW NET_STS");
}

static void
p25_lcw_handle_format_65(p25_lcw_ctx* ctx) {
    uint8_t algid = (uint8_t)convert_bits_into_output(&ctx->bits[24], 8);
    uint16_t kid = (uint16_t)convert_bits_into_output(&ctx->bits[32], 16);
    uint32_t target = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);

    const char* alg_name = p25_algid_name(algid);

    DSD_FPRINTF(stderr, " Protection Parameter Broadcast");
    DSD_FPRINTF(stderr, "\n  ALGID [%02X]", algid);
    if (alg_name) {
        DSD_FPRINTF(stderr, " (%s)", alg_name);
    }
    DSD_FPRINTF(stderr, " KID [%04X] Target [%d]", kid, target);

    ctx->state->p25_prot_algid = algid;
    ctx->state->p25_prot_kid = kid;
    ctx->state->p25_prot_valid = 1;
}

static void
p25_lcw_handle_format_66(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Secondary Control Channel Broadcast %s Explicit (LCSCBX)", dsd_unicode_or_ascii("–", "-"));
    uint8_t rfssid = (uint8_t)convert_bits_into_output(&ctx->bits[8], 8);
    uint8_t siteid = (uint8_t)convert_bits_into_output(&ctx->bits[16], 8);
    uint16_t channelt = (uint16_t)convert_bits_into_output(&ctx->bits[24], 16);
    uint16_t channelr = (uint16_t)convert_bits_into_output(&ctx->bits[40], 16);
    uint8_t ssc = (uint8_t)convert_bits_into_output(&ctx->bits[56], 8);
    DSD_FPRINTF(stderr, " - RFSS %d Site %d CH-T %04X CH-R %04X SSC %02X", rfssid, siteid, channelt, channelr, ssc);
    (void)process_channel_to_freq(ctx->opts, ctx->state, channelr);
    (void)p25_announce_secondary_cc_channel(ctx->opts, ctx->state, channelt, rfssid, siteid, ssc);
}

static void
p25_lcw_handle_format_67(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Adjacent Site Status (LCASBX)");
    uint8_t lra = (uint8_t)convert_bits_into_output(&ctx->bits[8], 8);
    uint16_t channelt = (uint16_t)convert_bits_into_output(&ctx->bits[16], 16);
    uint8_t rfssid = (uint8_t)convert_bits_into_output(&ctx->bits[32], 8);
    uint8_t siteid = (uint8_t)convert_bits_into_output(&ctx->bits[40], 8);
    uint16_t channelr = (uint16_t)convert_bits_into_output(&ctx->bits[48], 16);
    uint8_t ssc = (uint8_t)convert_bits_into_output(&ctx->bits[64], 8);
    DSD_FPRINTF(stderr, " - LRA %02X RFSS %d Site %d CH-T %04X CH-R %04X SSC %02X", lra, rfssid, siteid, channelt,
                channelr, ssc);
    (void)process_channel_to_freq(ctx->opts, ctx->state, channelr);
    uint16_t sysid = ctx->state ? (uint16_t)ctx->state->p2_sysid : 0U;
    const p25_neighbor_channel_announcement_t announcement = {
        .channel = channelt,
        .sysid = sysid,
        .rfss = rfssid,
        .site = siteid,
        .lra = lra,
        .lra_valid = 1U,
    };
    (void)p25_announce_neighbor_channel(ctx->opts, ctx->state, &announcement);
}

static void
p25_lcw_handle_format_68(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " RFSS Status Broadcast %s Explicit (LCRSBX)", dsd_unicode_or_ascii("–", "-"));
    uint8_t lra = (uint8_t)convert_bits_into_output(&ctx->bits[8], 8);
    uint16_t channelr = (uint16_t)convert_bits_into_output(&ctx->bits[16], 16);
    uint8_t rfssid = (uint8_t)convert_bits_into_output(&ctx->bits[32], 8);
    uint8_t siteid = (uint8_t)convert_bits_into_output(&ctx->bits[40], 8);
    uint16_t channelt = (uint16_t)convert_bits_into_output(&ctx->bits[48], 16);
    uint8_t ssc = (uint8_t)convert_bits_into_output(&ctx->bits[64], 8);
    DSD_FPRINTF(stderr, " - LRA %02X RFSS %d Site %d CH-T %04X CH-R %04X SSC %02X", lra, rfssid, siteid, channelt,
                channelr, ssc);
    (void)process_channel_to_freq(ctx->opts, ctx->state, channelt);
    (void)process_channel_to_freq(ctx->opts, ctx->state, channelr);
    p25_lcw_store_current_site_status(ctx, lra, 1, 0, 0, rfssid, siteid);
}

static void
p25_lcw_handle_format_69(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Network Status Broadcast %s Explicit (LCNSBX)", dsd_unicode_or_ascii("–", "-"));
    uint32_t wacn = (uint32_t)convert_bits_into_output(&ctx->bits[8], 20);
    uint16_t sysid = (uint16_t)convert_bits_into_output(&ctx->bits[28], 12);
    uint16_t channelt = (uint16_t)convert_bits_into_output(&ctx->bits[40], 16);
    uint16_t channelr = (uint16_t)convert_bits_into_output(&ctx->bits[56], 16);
    DSD_FPRINTF(stderr, " - WACN %05X SYS %03X CH-T %04X CH-R %04X", wacn, sysid, channelt, channelr);
    (void)process_channel_to_freq(ctx->opts, ctx->state, channelr);
    p25_lcw_update_primary_control_channel(ctx, wacn, sysid, channelt, 1, "P25 LCW NET_STS-EX");
}

static void
p25_lcw_handle_format_6a(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Conventional Fallback");
}

static void
p25_lcw_handle_call_termination(p25_lcw_ctx* ctx) {
    uint32_t tgt = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " Call Termination; TGT: %d;", tgt);
    DSD_MEMSET(ctx->state->dmr_pdu_sf[0], 0, sizeof(ctx->state->dmr_pdu_sf[0]));
    if (tgt == 0x000000U || tgt == 0xFFFFFDU || tgt == 0xFFFFFFU) {
        p25_sm_release(p25_sm_get_ctx(), ctx->opts, ctx->state, "lcw-network-release");
        return;
    }
    p25_sm_emit_end(ctx->opts, ctx->state, 0);
}

static int
p25_lcw_dispatch_standard_format(p25_lcw_ctx* ctx) {
    static const p25_lcw_handler_entry handlers[] = {
        {0x00, p25_lcw_handle_format_00}, {0x03, p25_lcw_handle_format_03}, {0x42, p25_lcw_handle_format_42},
        {0x44, p25_lcw_handle_format_44}, {0x45, p25_lcw_handle_format_45}, {0x46, p25_lcw_handle_format_46},
        {0x47, p25_lcw_handle_format_47}, {0x49, p25_lcw_handle_format_49}, {0x4A, p25_lcw_handle_format_4a},
        {0x50, p25_lcw_handle_format_50}, {0x51, p25_lcw_handle_format_51}, {0x52, p25_lcw_handle_format_52},
        {0x53, p25_lcw_handle_format_53}, {0x54, p25_lcw_handle_format_54}, {0x55, p25_lcw_handle_format_55},
        {0x56, p25_lcw_handle_format_56}, {0x57, p25_lcw_handle_format_57}, {0x58, p25_lcw_handle_format_58},
        {0x59, p25_lcw_handle_format_59}, {0x5A, p25_lcw_handle_format_5a}, {0x5B, p25_lcw_handle_format_5b},
        {0x5C, p25_lcw_handle_format_5c}, {0x60, p25_lcw_handle_format_60}, {0x61, p25_lcw_handle_format_61},
        {0x62, p25_lcw_handle_format_62}, {0x63, p25_lcw_handle_format_63}, {0x64, p25_lcw_handle_format_64},
        {0x65, p25_lcw_handle_format_65}, {0x66, p25_lcw_handle_format_66}, {0x67, p25_lcw_handle_format_67},
        {0x68, p25_lcw_handle_format_68}, {0x69, p25_lcw_handle_format_69}, {0x6A, p25_lcw_handle_format_6a},
    };

    for (size_t i = 0; i < (sizeof(handlers) / sizeof(handlers[0])); i++) {
        if (handlers[i].key == ctx->lc_format) {
            handlers[i].fn(ctx);
            return 1;
        }
    }

    if (ctx->lc_opcode == 0x0F) {
        p25_lcw_handle_call_termination(ctx);
        return 1;
    }

    return 0;
}

static void
p25_lcw_handle_unknown_standard(const p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Unknown Format %02X MFID %02X SVC %02X", ctx->lc_format, ctx->lc_mfid, ctx->lc_svcopt);
}

/* The call's source as the Part A pairing compares it: 0 when none fits 32 bits. */
static uint32_t
p25_lcw_gps_call_source(const p25_lcw_gps_call* call) {
    return call->view.source <= UINT32_MAX ? (uint32_t)call->view.source : 0U;
}

/* A GPS report's provenance (spec 2.2): the position is checked when its link control(s) passed a hard RS decode
 * (@p pos_verified); the source, target and group flag by the attribution rule from the evidence a hard-decoded
 * voice-user link control of this call gave (p25_lcw_note_checked_identity(), dsd_position_attribution_grant()), and
 * only while no other checked message of the call withdrew that talker: the trunk state machine's record of the call
 * (a repeated grant naming a special address or another source, for one) is the one record every P25 message updates,
 * so its withdrawal revokes this evidence too (p25_sm_call_source_withdrawn(), issue #800, fail closed). */
static void
p25_lcw_gps_report_ctx(const p25_lcw_ctx* ctx, const p25_lcw_gps_call* call, int pos_verified,
                       const dsd_position_origin* origin, dsd_gps_report_ctx* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->origin = origin;
    out->call_epoch = call->view.epoch;
    out->closing = call->closing;
    out->source_id = p25_lcw_gps_call_source(call);
    out->protocol = DSD_SYNC_IS_P25P1(ctx->state->lastsynctype) ? ctx->state->lastsynctype : DSD_SYNC_P25P1_POS;
    out->slot = 0U;
    out->pos_verified = pos_verified ? 1U : 0U;
    /* A TDULC's report bound to no call (p25_lcw_gps_call_get()) has no history row: falling back to the slot's
       ACTIVE call would put it on a call that TDULC did not end. */
    out->no_call_row = (uint8_t)(!ctx->allow_voice_start && !call->view.open);
    dsd_position_attribution granted;
    if (dsd_position_attribution_grant(ctx->state, &ctx->state->p25_p1_lc_attribution, &call->view, out->protocol,
                                       &granted)
        && !p25_sm_call_source_withdrawn(ctx->state, 0, call->view.epoch, granted.source)) {
        out->source_verified = 1U;
        out->target_id = granted.target;
        out->target_group = granted.group;
    }
}

static void
p25_lcw_handle_mfid90_opcode_06(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto)");
    p25_lcw_gps_call call;
    p25_lcw_gps_call_get(ctx, &call);
    dsd_gps_report_ctx report;
    p25_lcw_gps_report_ctx(ctx, &call, ctx->lc_hard, NULL, &report);
    (void)apx_embedded_gps(ctx->opts, ctx->state, ctx->bits, &report);
}

static void
p25_lcw_handle_mfid90_opcode_00(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) Group Regroup Channel User (LCGRGR)");
    uint32_t sg = (uint32_t)convert_bits_into_output(&ctx->bits[32], 16);
    uint32_t src = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " SG: %d; SRC: %d;", sg, src);
    if (ctx->bits[16] == 1) {
        DSD_FPRINTF(stderr, " Res;");
    }
    if (ctx->bits[17] == 1) {
        DSD_FPRINTF(stderr, " ENC;");
    }
    if (ctx->bits[31] == 1) {
        DSD_FPRINTF(stderr, " EXT;");
    }
    // The layout is the standard Group Voice Channel User's, with the supergroup for the group.
    p25_patch_update(ctx->state, (int)sg, 1, 1);
    (void)p25_lcw_accept_group_voice_user(ctx, (uint16_t)sg, src, ctx->bits[31] == 0U, 1);
    p25_lcw_note_checked_identity(ctx, sg, src, 1U, ctx->bits[31] == 0U);
}

static void
p25_lcw_handle_mfid90_opcode_01(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) Group Regroup Channel Update (LCGRGU)");
    uint32_t sg = (uint32_t)convert_bits_into_output(&ctx->bits[24], 16);
    uint32_t ch = (uint32_t)convert_bits_into_output(&ctx->bits[56], 16);
    DSD_FPRINTF(stderr, " SG: %d; CH: %04X;", sg, ch);
    if (ctx->bits[16] == 1) {
        DSD_FPRINTF(stderr, " Res;");
    }
    if (ctx->bits[17] == 1) {
        DSD_FPRINTF(stderr, " ENC;");
    }

    if (sg != 0U && ch != 0U) {
        char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
        DSD_SNPRINTF(notice, sizeof notice, "Regroup Ch: %04X SG: %u; ", (unsigned)ch, (unsigned)sg);
        p25_lcw_publish_activity(ctx->state, 0U, DSD_CALL_KIND_GROUP_VOICE, sg, 0U, (uint16_t)ch, notice);
        const uint16_t channels[1] = {(uint16_t)ch};
        const uint16_t groups[1] = {(uint16_t)sg};
        const int svcs[1] = {P25_SM_SVC_UNKNOWN};
        p25_lcw_offer_preemption(ctx, channels, groups, svcs, 1);
        if (ctx->lc_hard) {
            /* A hard-decoded update names its supergroup to an assignment it took (spec 2.20). */
            p25_sm_note_regroup_grant(p25_sm_get_ctx(), (int)sg);
        }
    } else {
        (void)dsd_recent_activity_clear(ctx->state, 0U);
    }
}

static void
p25_lcw_handle_mfid90_opcode_02(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) Failsoft");
    DSD_FPRINTF(stderr, " Data:");
    for (int bi = 16; bi + 8 <= 72; bi += 8) {
        uint8_t b = (uint8_t)convert_bits_into_output(&ctx->bits[bi], 8);
        DSD_FPRINTF(stderr, " %02X", b);
    }
}

static void
p25_lcw_handle_mfid90_opcode_03(p25_lcw_ctx* ctx) {
    uint32_t sg = (uint32_t)convert_bits_into_output(&ctx->bits[16], 16);
    uint32_t ga1 = (uint32_t)convert_bits_into_output(&ctx->bits[32], 16);
    uint32_t ga2 = (uint32_t)convert_bits_into_output(&ctx->bits[48], 16);
    DSD_FPRINTF(stderr, " MFID90 (Moto) Group Regroup Add");
    DSD_FPRINTF(stderr, " SG: %u;", (unsigned)sg);
    if (ga1 != 0 && ga1 != sg) {
        DSD_FPRINTF(stderr, " GA1: %u;", (unsigned)ga1);
        p25_patch_add_wgid(ctx->state, (int)sg, (int)ga1);
    }
    if (ga2 != 0 && ga2 != sg) {
        DSD_FPRINTF(stderr, " GA2: %u;", (unsigned)ga2);
        p25_patch_add_wgid(ctx->state, (int)sg, (int)ga2);
    }
}

static void
p25_lcw_handle_mfid90_opcode_04(p25_lcw_ctx* ctx) {
    uint32_t sg = (uint32_t)convert_bits_into_output(&ctx->bits[16], 16);
    uint32_t ga1 = (uint32_t)convert_bits_into_output(&ctx->bits[32], 16);
    uint32_t ga2 = (uint32_t)convert_bits_into_output(&ctx->bits[48], 16);
    DSD_FPRINTF(stderr, " MFID90 (Moto) Group Regroup Delete");
    DSD_FPRINTF(stderr, " SG: %u;", (unsigned)sg);
    // A Delete that lists the SG itself ends the whole patch (p25_patch_remove_wgid()).
    if (ga1 != 0) {
        DSD_FPRINTF(stderr, " GA1: %u;", (unsigned)ga1);
        p25_patch_remove_wgid(ctx->state, (int)sg, (int)ga1);
    }
    if (ga2 != 0) {
        DSD_FPRINTF(stderr, " GA2: %u;", (unsigned)ga2);
        p25_patch_remove_wgid(ctx->state, (int)sg, (int)ga2);
    }
}

static void
p25_lcw_handle_mfid90_opcode_05(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) System Information (BSI)");
    DSD_FPRINTF(stderr, " Data:");
    for (int bi = 16; bi + 8 <= 72; bi += 8) {
        uint8_t b = (uint8_t)convert_bits_into_output(&ctx->bits[bi], 8);
        DSD_FPRINTF(stderr, " %02X", b);
    }
    if (ctx->opts->frontend_display.show_p25_callsign_decode
        && (ctx->state->p2_wacn != 0 || ctx->state->p2_sysid != 0)) {
        char callsign[7];
        p25_wacn_sysid_to_callsign((uint32_t)ctx->state->p2_wacn, (uint16_t)ctx->state->p2_sysid, callsign);
        DSD_FPRINTF(stderr, " [%s]", callsign);
    }
}

/* The P25 Phase 1 call this link control speaks for, into @p call: from an LDU1, only the open call, since a new
 * transmission's first LDU1 decodes before its own call opens and the ended one is the previous transmission's; from a
 * TDULC, which ends its call before its link control is read, also the call it ended or whose end it repeats
 * (p25_lcw_from_tdulc()), never an older ended call the slot still holds. */
static int
p25_lcw_call(const p25_lcw_ctx* ctx, dsd_call_snapshot* call) {
    if (p25p1_current_call(ctx->state, call)) {
        return 1;
    }
    return !ctx->allow_voice_start && ctx->ended_epoch != 0U && p25p1_open_or_ended_call(ctx->state, call)
           && call->epoch == ctx->ended_epoch;
}

static void
p25_lcw_handle_mfid90_opcode_0f(p25_lcw_ctx* ctx) {
    uint32_t src = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " MFID90 (Moto) Talker EOT; SRC: %d;", src);
    DSD_MEMSET(ctx->state->dmr_pdu_sf[0], 0, sizeof(ctx->state->dmr_pdu_sf[0]));
    dsd_call_snapshot call;
    const int target = p25_lcw_call(ctx, &call) ? (int)call.ota_target_id : 0;
    p25_sm_emit_end_call_at(ctx->opts, ctx->state, 0, target, (int)src, dsd_decode_now_mono_s());
}

static void
p25_lcw_handle_mfid90_opcode_0a(p25_lcw_ctx* ctx) {
    uint16_t group = (uint16_t)convert_bits_into_output(&ctx->bits[32], 16);
    uint32_t source = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " MFID90 (Moto) Emergency Alarm Activation");
    DSD_FPRINTF(stderr, " Group: %u Source: %u;", group, source);
    DSD_FPRINTF(stderr, " %s** EMERGENCY **%s", KRED, KYEL);
}

static void
p25_lcw_handle_mfid90_opcode_15(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) Talker Alias Header");
    apx_embedded_alias_header_phase1(ctx->opts, ctx->state, 0, ctx->bits);
}

static void
p25_lcw_handle_mfid90_opcode_17(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) Talker Alias Blocks");
    apx_embedded_alias_blocks_phase1(ctx->opts, ctx->state, 0, ctx->bits);
}

static int
p25_lcw_dispatch_mfid90(p25_lcw_ctx* ctx) {
    static const p25_lcw_handler_entry handlers[] = {
        {0x00, p25_lcw_handle_mfid90_opcode_00}, {0x01, p25_lcw_handle_mfid90_opcode_01},
        {0x02, p25_lcw_handle_mfid90_opcode_02}, {0x03, p25_lcw_handle_mfid90_opcode_03},
        {0x04, p25_lcw_handle_mfid90_opcode_04}, {0x05, p25_lcw_handle_mfid90_opcode_05},
        {0x06, p25_lcw_handle_mfid90_opcode_06}, {0x0A, p25_lcw_handle_mfid90_opcode_0a},
        {0x0F, p25_lcw_handle_mfid90_opcode_0f}, {0x15, p25_lcw_handle_mfid90_opcode_15},
        {0x17, p25_lcw_handle_mfid90_opcode_17},
    };

    for (size_t i = 0; i < (sizeof(handlers) / sizeof(handlers[0])); i++) {
        if (handlers[i].key == ctx->lc_opcode) {
            handlers[i].fn(ctx);
            return 1;
        }
    }
    return 0;
}

/* Longest wait between an L3Harris GPS Part A and its Part B on the decode clock. The halves ride in successive link
 * controls (an LDU1 every 360 ms); the window allows a few lost between them. */
#define P25_HARRIS_GPS_PAIR_WINDOW_MS 2000U

static void
p25_lcw_harris_gps_part_a(p25_lcw_ctx* ctx, uint8_t even) {
    DSD_FPRINTF(stderr, " MFIDA4 (Harris) GPS Update Part A");
    p25_lcw_gps_call call;
    p25_lcw_gps_call_get(ctx, &call);
    p25_p1_harris_gps_part_a* a = &ctx->state->p25_p1_harris_part_a;
    DSD_MEMSET(a, 0, sizeof(*a));
    for (size_t i = 0; i < sizeof(a->octets); i++) {
        a->octets[i] = (uint8_t)convert_bits_into_output(&ctx->bits[16U + (8U * i)], 8);
    }
    a->even = even;
    a->hard = ctx->lc_hard;
    a->source = p25_lcw_gps_call_source(&call);
    a->epoch = call.view.epoch;
    a->stored_mono_ms = dsd_decode_now_mono_ms();
    dsd_position_origin_capture(ctx->opts, ctx->state, &a->origin);
    a->valid = 1U;
}

/* Why the pending Part A @p a cannot pair with a Part B of @p even parity heard on @p call; NULL when it can. */
static const char*
p25_lcw_harris_gps_unpaired(const dsd_state* state, const p25_p1_harris_gps_part_a* a, const p25_lcw_gps_call* call,
                            uint8_t even) {
    if (!a->valid) {
        return "Missing GPS Part A";
    }
    // Both halves must come from one transmission of one source (spec 2.2): with no call open, nothing binds them.
    if (a->epoch == 0U) {
        return "GPS Part A outside a call";
    }
    if (a->even != even) {
        return "GPS Part A of another update";
    }
    if (a->epoch != call->view.epoch || a->source != p25_lcw_gps_call_source(call)) {
        return "GPS Part A of another call";
    }
    /* Heard on another reception (a retune, another system or a change of P25 phase since, issue #800): a call left
       open across it is not shown to be the one the Part B rides on. */
    if (!dsd_position_origin_matches(state, &a->origin)) {
        return "GPS Part A of another reception";
    }
    const uint64_t now_ms = dsd_decode_now_mono_ms();
    if (now_ms < a->stored_mono_ms || now_ms - a->stored_mono_ms > P25_HARRIS_GPS_PAIR_WINDOW_MS) {
        return "GPS Part A expired";
    }
    return NULL;
}

static void
p25_lcw_harris_gps_part_b(p25_lcw_ctx* ctx, uint8_t even) {
    DSD_FPRINTF(stderr, " MFIDA4 (Harris) GPS Update Part B");
    // The pending Part A is used or dropped here, whatever the outcome.
    const p25_p1_harris_gps_part_a a = ctx->state->p25_p1_harris_part_a;
    DSD_MEMSET(&ctx->state->p25_p1_harris_part_a, 0, sizeof(ctx->state->p25_p1_harris_part_a));
    p25_lcw_gps_call call;
    p25_lcw_gps_call_get(ctx, &call);
    const char* unpaired = p25_lcw_harris_gps_unpaired(ctx->state, &a, &call, even);
    if (unpaired != NULL) {
        DSD_FPRINTF(stderr, " %s", unpaired);
        return;
    }
    uint8_t body[DSD_HARRIS_GPS_BODY_OCTETS];
    DSD_MEMCPY(body, a.octets, sizeof(a.octets));
    for (size_t i = 0; i < 7U; i++) {
        body[7U + i] = (uint8_t)convert_bits_into_output(&ctx->bits[16U + (8U * i)], 8);
    }
    dsd_gps_report_ctx report;
    p25_lcw_gps_report_ctx(ctx, &call, a.hard && ctx->lc_hard, &a.origin, &report);
    (void)nmea_harris(ctx->opts, ctx->state, body, &report);
}

static int
p25_lcw_dispatch_mfid_a4(p25_lcw_ctx* ctx) {
    if (ctx->lc_opcode > 0x31 && ctx->lc_opcode < 0x36) {
        DSD_FPRINTF(stderr, " MFIDA4 (Harris) Talker Alias Blocks");
        l3h_embedded_alias_blocks_phase1(ctx->opts, ctx->state, 0, ctx->bits);
        return 1;
    }

    // L3Harris GPS update (TIA-102.AABF-D-1 §7.3.35-36): LCO 42/43 for odd-numbered updates, 44/45 for even ones.
    if (ctx->lc_opcode == 0x2A || ctx->lc_opcode == 0x2C) {
        p25_lcw_harris_gps_part_a(ctx, (uint8_t)(ctx->lc_opcode == 0x2C));
        return 1;
    }

    if (ctx->lc_opcode == 0x2B || ctx->lc_opcode == 0x2D) {
        p25_lcw_harris_gps_part_b(ctx, (uint8_t)(ctx->lc_opcode == 0x2D));
        return 1;
    }

    if (ctx->lc_format == 0x0A) {
        uint32_t unk = (uint32_t)convert_bits_into_output(&ctx->bits[16], 8);
        uint32_t src = (uint32_t)convert_bits_into_output(&ctx->bits[24], 24);
        uint32_t tgt = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
        DSD_FPRINTF(stderr, " MFIDA4 (Harris) 0x0A Data/Return-to-Control Indication; UNK: %02X; SRC: %u; TGT: %u;",
                    (unsigned)unk, (unsigned)src, (unsigned)tgt);
        return 1;
    }

    return 0;
}

static int
p25_lcw_dispatch_mfid_d8(p25_lcw_ctx* ctx) {
    if (ctx->lc_format == 0x00) {
        DSD_FPRINTF(stderr, " MFIDD8 (Tait) Talker Alias: ");
        tait_iso7_embedded_alias_decode(ctx->opts, ctx->state, 0, 8, ctx->bits);
        return 1;
    }
    if (ctx->lc_format == 0x01) {
        uint32_t wacn = (uint32_t)convert_bits_into_output(&ctx->bits[16], 20);
        uint16_t sysid = (uint16_t)convert_bits_into_output(&ctx->bits[36], 12);
        uint32_t src = (uint32_t)convert_bits_into_output(&ctx->bits[48], 24);
        DSD_FPRINTF(stderr, " MFIDD8 (Tait) Subscriber FQ-SUID: %05X.%03X.%d", wacn, sysid, src);
        if (wacn != 0) {
            ctx->state->p25_src_nid = wacn;
        }
        p25_lcw_note_full_suid_source(ctx, wacn, sysid, src);
        return 1;
    }
    return 0;
}

static int
p25_lcw_dispatch_vendor_format(p25_lcw_ctx* ctx) {
    if (ctx->lc_mfid == 0x90) {
        return p25_lcw_dispatch_mfid90(ctx);
    }
    if (ctx->lc_mfid == 0xA4) {
        return p25_lcw_dispatch_mfid_a4(ctx);
    }
    if (ctx->lc_mfid == 0xD8) {
        return p25_lcw_dispatch_mfid_d8(ctx);
    }
    return 0;
}

static void
p25_lcw_handle_unknown_vendor(const p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Unknown Format %02X MFID %02X ", ctx->lc_format, ctx->lc_mfid);
    if (ctx->lc_mfid == 0x90) {
        DSD_FPRINTF(stderr, "(Moto)");
    } else if (ctx->lc_mfid == 0xA4) {
        DSD_FPRINTF(stderr, "(Harris)");
    } else if (ctx->lc_mfid == 0xD8) {
        DSD_FPRINTF(stderr, "(Tait)");
    }
}

static void
p25_lcw_decode(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors,
               uint8_t allow_voice_start, const dsd_event_closing_call* closing, uint64_t ended_epoch) {
    if (opts == NULL || state == NULL || lcw_bits == NULL) {
        return;
    }

    p25_lcw_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.opts = opts;
    ctx.state = state;
    ctx.bits = lcw_bits;
    ctx.lc_format = (uint8_t)convert_bits_into_output(&lcw_bits[0], 8);
    ctx.lc_opcode = (uint8_t)convert_bits_into_output(&lcw_bits[2], 6);
    ctx.lc_mfid = (uint8_t)convert_bits_into_output(&lcw_bits[8], 8);
    const int svcopt_offset = ctx.lc_format == 0x4A ? 8 : 16;
    ctx.lc_svcopt = (uint8_t)convert_bits_into_output(&lcw_bits[svcopt_offset], 8);
    ctx.lc_pf = lcw_bits[0];
    ctx.lc_sf = lcw_bits[1];
    ctx.allow_voice_start = allow_voice_start;
    /* Checked only from a hard RS decode that corrected every error: not a soft-erasure retry (spec 2.20), and not a
       caller's uncorrectable verdict (the CRC/FEC publication rule). */
    ctx.lc_hard = (uint8_t)(state->p25_p1_lcw_soft == 0U && irrecoverable_errors == 0U);
    ctx.closing = closing;
    ctx.ended_epoch = ended_epoch;
    ctx.is_standard_mfid = (ctx.lc_sf == 1) || ctx.lc_mfid == 0 || ctx.lc_mfid == 1;

    if (ctx.lc_pf == 1) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " LCW Protected ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (ctx.lc_pf == 0) {
        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, " LCW");
        }

        if (ctx.is_standard_mfid) {
            if (p25_lcw_format_has_service_options(ctx.lc_format)) {
                p25_lcw_print_service_options(&ctx);
            }
            if (!p25_lcw_dispatch_standard_format(&ctx)) {
                p25_lcw_handle_unknown_standard(&ctx);
            }
        } else if (!p25_lcw_dispatch_vendor_format(&ctx)) {
            p25_lcw_handle_unknown_vendor(&ctx);
        }
    }

    DSD_FPRINTF(stderr, "\n");
}

void
p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors) {
    p25_lcw_decode(opts, state, lcw_bits, irrecoverable_errors, 1, NULL, 0U);
}

void
p25_lcw_from_tdulc(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors,
                   const dsd_event_closing_call* closing, uint64_t ended_epoch) {
    p25_lcw_decode(opts, state, lcw_bits, irrecoverable_errors, 0, closing, ended_epoch);
}
