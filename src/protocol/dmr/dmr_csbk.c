// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * dmr_csbk.c
 * DMR Control Signal Data PDU (CSBK, MBC) Handler and Related Functions
 *
 * Portions of Connect+ code reworked from Boatbod OP25
 * Source: https://github.com/boatbod/op25/blob/master/op25/gr-op25_repeater/lib/dmr_slot.cc
 *
 * Portions of Capacity+ code reworked from Eric Cottrell
 * Source: https://github.com/LinuxSheeple-E/dsd/blob/Feature/DMRECC/dmr_csbk.c
 *
 * LWVMOBILE
 * 2023-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_channel.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/crc_provenance.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_csbk_parse.h>
#include <dsd-neo/protocol/dmr/dmr_csbk_tables.h>
#include <dsd-neo/protocol/dmr/dmr_t3_channels.h>
#include <dsd-neo/protocol/dmr/dmr_t3_grant.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dmr_abs_params.h"
#include "dmr_call_channel.h"
#include "dmr_confidence.h"
#include "dmr_crc_provenance.h"
#include "dmr_reason.h"
#include "dmr_t3_channels_internal.h"
#include "dmr_tiii_site.h"
#include "dmr_unit_activity.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* 1 while dmr_cspdu() handles the P_CLEAR the decoder synthesized itself (dmr_cspdu_release_channel()). Decoder thread
   only. */
static int s_dmr_cspdu_synthesized;

/* Whether the CSBK being handled passed its own CRC. dmr_cspdu() also handles one -F accepted by RAS and an MBC whose
   header CRC failed, which the burst handler flags for the slot, and a P_CLEAR the encryption lockout synthesized,
   which was never received: they keep decoding, following and releasing, but name no identity in the received
   identity record. */
static int
dmr_csbk_crc_checked(const dsd_state* state) {
    return !s_dmr_cspdu_synthesized && state->event_crc_invalid[state->currentslot & 1] == 0U;
}

/* The trunking brand a checked CSBK names, to the received identity record. */
static void
dmr_csbk_note_brand(dsd_state* state, const char* brand) {
    if (dmr_csbk_crc_checked(state)) {
        dsd_rxid_note_dmr_brand(state, brand);
    }
}

/* The CSBK being handled is one -F accepted as RAS, whose keyed CRC no one can check, with nothing else casting doubt
   on it: the slot's flags say RAS alone (a failed CRC never counts), RAS is confirmed on this carrier, and the burst
   was read whole from it, as dmr_ras_note_signature() takes a signature. Never the P_CLEAR the decoder synthesized. */
static int
dmr_csbk_ras_accepted(const dsd_opts* opts, const dsd_state* state) {
    return !s_dmr_cspdu_synthesized && opts->aggressive_framesync == 0
           && state->event_crc_invalid[state->currentslot & 1] == DSD_CRC_RAS_UNVERIFIED && dmr_ras_confirmed(state)
           && state->input_interrupted == 0
           && !(state->frame_dispatch_active && dsd_state_frame_read_lost(state, state->frame_dispatch_carrier_seq));
}

/* Record a frequency the CSBK being handled names for @p lpcn (dmr_t3_chan_learn()): SITE when its CRC checked,
   UNVERIFIED when -F accepted it as RAS, nothing otherwise; a neighbour site's (@p neighbour) is NEIGHBOUR either way.
   Returns whether the row or its origin changed. */
static int
dmr_csbk_learn_channel(dsd_opts* opts, dsd_state* state, uint16_t lpcn, long freq, int neighbour) {
    dmr_chan_origin_e origin = DMR_CHAN_ORIGIN_UNVERIFIED;
    if (dmr_csbk_crc_checked(state)) {
        origin = DMR_CHAN_ORIGIN_SITE;
    } else if (!dmr_csbk_ras_accepted(opts, state)) {
        return 0;
    }
    return dmr_t3_chan_learn(opts, state, lpcn, freq, neighbour ? DMR_CHAN_ORIGIN_NEIGHBOUR : origin);
}

/* A checked standard CSBK the site sent (a grant or a C_BCAST) names it Tier III (dmr_t3_note_tier3()). */
static void
dmr_csbk_note_tier3(dsd_state* state, int csbk_fid) {
    if (csbk_fid == 0 && !DSD_SYNC_IS_DMR_MS(state->synctype) && dmr_csbk_crc_checked(state)) {
        dmr_t3_note_tier3(state);
    }
}

/* The label of a grant's target as the call history names it (dsd_call_target_label_lookup(), issue #773 D4). */
static void
dmr_csbk_print_target_label(const dsd_state* state, dsd_call_kind kind, uint32_t id) {
    char name[DSD_SOURCE_ALIAS_NAME_MAX];
    if (id != 0U && dsd_call_target_label_lookup(state, kind, id, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, " [%s]", name);
    }
}

static void
dmr_csbk_publish_activity(dsd_state* state, uint8_t index, uint8_t slot, dsd_call_kind kind, uint64_t target,
                          uint64_t source, uint16_t channel, long int frequency, int emergency, const char* notice) {
    const dsd_call_observation observation = {
        .protocol = state->lastsynctype,
        .slot = slot & 1U,
        .kind = kind,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
        .channel = channel,
        .frequency_hz = frequency,
        .emergency = emergency != 0,
    };
    (void)dsd_recent_activity_publish(state, index, &observation, notice, 0U);
}

// Safe append helper: appends src to dst within dstsz, NUL-terminating
static inline void
dsd_append(char* dst, size_t dstsz, const char* src) {
    if (!dst || !src || dstsz == 0) {
        return;
    }
    size_t len = strlen(dst);
    if (len >= dstsz) {
        return;
    }
    DSD_SNPRINTF(dst + len, dstsz - len, "%s", src);
}

// Format a short suffix indicating TDMA slot for DMR UI, e.g., " (TDMA S1)".
static inline void
dmr_format_chan_suffix(int slot_index, char* out, size_t outsz) {
    if (!out || outsz == 0) {
        return;
    }
    out[0] = '\0';
    if (slot_index < 0) {
        return;
    }
    // Display slots as 1-based (S1/S2) to match UI conventions
    DSD_SNPRINTF(out, outsz, " (TDMA S%d)", (slot_index % 2) + 1);
}

static int
dmr_policy_tune_allowed(const dsd_opts* opts, const dsd_state* state, uint32_t target, uint32_t source,
                        int is_group_call, int data_call, dsd_tg_policy_decision* out_decision) {
    dsd_tg_policy_decision decision;
    int rc = 0;
    DSD_MEMSET(&decision, 0, sizeof(decision));

    if (is_group_call) {
        rc = dsd_tg_policy_evaluate_group_call(opts, state, target, source, 0, data_call, &decision);
    } else {
        rc = dsd_tg_policy_evaluate_private_call(opts, state, source, target, 0, data_call, &decision);
    }
    if (out_decision) {
        *out_decision = decision;
    }
    return rc == 0 && decision.tune_allowed;
}

static void
dmr_policy_log_block(const dsd_opts* opts, int is_group_call, uint32_t target, uint32_t source,
                     const dsd_tg_policy_decision* decision) {
    if (!opts || !decision || opts->verbose < 1) {
        return;
    }
    if (decision->block_reasons == DSD_TG_POLICY_BLOCK_NONE) {
        return;
    }
    DSD_FPRINTF(stderr, "\n DMR %s grant blocked (%s): target=%u source=%u;", is_group_call ? "group" : "private",
                dsd_tg_policy_block_reason_label(decision->block_reasons), target, source);
}

static void dmr_gateway_identifier(uint32_t source, uint32_t target);
static void dmr_decode_syscode(const dsd_opts* opts, dsd_state* state, const uint8_t* cs_pdu_bits, int csbk_fid,
                               int type);

/* A grant is judged by its own classes (dmr_t3_grant_classify()): a data grant needs data tuning, and a group grant
   group tuning or a private one private tuning, whatever its data class. */
static int
dmr_cspdu_pf0_should_skip_call(const dsd_opts* opts, const dmr_t3_grant_class* grant) {
    if (grant->data && opts->trunk_tune_data_calls != 1) {
        return 1;
    }
    return grant->group ? opts->trunk_tune_group_calls == 0 : opts->trunk_tune_private_calls == 0;
}

/* The call kind a grant's activity row and its call carry: data whatever its group class, else group or private
   voice. */
static dsd_call_kind
dmr_cspdu_pf0_grant_call_kind(const dmr_t3_grant_class* grant) {
    if (grant->data) {
        return DSD_CALL_KIND_DATA;
    }
    return grant->group ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE;
}

/* Why the PDU's absolute parameters are unusable: a reserved Cdeftype (its CdefParms printed raw), or no usable
   frequency. */
static void
dmr_cspdu_abs_params_print_unusable(const uint8_t cs_pdu_bits[], const dmr_abs_params* ap) {
    if (ap->cdeftype != 0U) {
        DSD_FPRINTF(stderr, "\n  MBC Absolute Parameters - Unknown CdefParms: %015llX",
                    (unsigned long long int)convert_bits_into_output(&cs_pdu_bits[118], 58));
    } else {
        DSD_FPRINTF(stderr, "\n  MBC Absolute Parameters missing or out of range;");
    }
}

/* The absolute parameters of the PDU being handled (dmr_abs_params_decode()), or 0 with the reason printed: a reserved
   Cdeftype, or no usable frequency (no single checked continuation, or one outside Table C.7). */
static int
dmr_cspdu_abs_params(const uint8_t cs_pdu_bits[], dmr_abs_params* ap) {
    if (dmr_abs_params_decode(cs_pdu_bits, dmr_cspdu_current_shape(), ap)) {
        return 1;
    }
    dmr_cspdu_abs_params_print_unusable(cs_pdu_bits, ap);
    return 0;
}

/* An absolute grant (LPCN 0xFFF): its frequency in @p freq and the LPCN its CdefParms define in @p chan (0 or 4095
   name no channel). 0 when its parameters are unusable: then it names no frequency and is neither shown nor
   followed. */
static int
dmr_cspdu_pf0_parse_absolute_grant(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], long int* freq,
                                   uint16_t* chan) {
    dmr_abs_params ap;
    if (!dmr_cspdu_abs_params(cs_pdu_bits, &ap)) {
        return 0;
    }
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "  RX APCN: %04d; RX INT: %ld; RX STEP: %ld;", ap.chan, ap.rx_hz / 1000000L,
                (ap.rx_hz % 1000000L) / 125L);
    *freq = ap.rx_hz;
    *chan = ap.chan;
    (void)dmr_csbk_learn_channel(opts, state, ap.chan, ap.rx_hz, 0);
    return 1;
}

static void
dmr_cspdu_pf0_publish_grant(dsd_state* state, uint8_t lcn, uint16_t channel, long int frequency,
                            const dmr_t3_grant_class* grant, int emergency) {
    char suf[24];
    dmr_format_chan_suffix(lcn, suf, sizeof suf);
    const dsd_call_kind call_kind = dmr_cspdu_pf0_grant_call_kind(grant);
    const char* kind = "Active Private Ch";
    if (call_kind == DSD_CALL_KIND_DATA) {
        kind = "Active Data Ch";
    } else if (call_kind == DSD_CALL_KIND_GROUP_VOICE) {
        kind = "Active Group Ch";
    }
    /* Tier III channel numbers read as decimal, as everywhere dsd-neo shows them (discussion #900 B11). */
    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    DSD_SNPRINTF(notice, sizeof notice, "%s: %u%s TG: %u; ", kind, (unsigned)channel, suf, (unsigned)grant->recipient);
    dmr_csbk_publish_activity(state, lcn, lcn, call_kind, grant->recipient, grant->caller, channel, frequency,
                              emergency, notice);
}

static void
dmr_cspdu_pf0_print_channel_kind(uint16_t lpchannum) {
    if (lpchannum == 0) {
        DSD_FPRINTF(stderr, " - Invalid Channel");
    } else if (lpchannum == 0xFFF) {
        DSD_FPRINTF(stderr, " - Absolute");
    } else {
        DSD_FPRINTF(stderr, " - Logical");
    }
}

/* The grant's frequency in @p freq, and for the absolute escape the LPCN its CdefParms define in @p mbc_lpchannum.
   0 for an absolute grant whose parameters are unusable. */
static int
dmr_cspdu_pf0_resolve_frequency(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint16_t lpchannum,
                                long int* freq, uint16_t* mbc_lpchannum) {
    if (lpchannum == 0xFFF) {
        return dmr_cspdu_pf0_parse_absolute_grant(opts, state, cs_pdu_bits, freq, mbc_lpchannum);
    }
    if (lpchannum != 0) {
        *freq = state->trunk_chan_map[lpchannum];
    }
    return 1;
}

/* The grant's call channel, the one value the trunk state machine, the call and the policy route all name: its LPCN, or
   for the absolute escape the LPCN its MBC's CdefParms define; 0 when that names no channel (TS 102 361-4
   §7.1.1.1.1.1.1, §7.2.42 Table 7.103). */
static uint16_t
dmr_cspdu_pf0_call_channel(uint16_t lpchannum, uint16_t mbc_lpchannum) {
    const uint16_t channel = lpchannum == 0xFFF ? mbc_lpchannum : lpchannum;
    return dmr_t3_lpcn_is_channel(channel) ? channel : 0U;
}

static void
dmr_cspdu_pf0_print_frequency(uint16_t lpchannum, long int freq) {
    if (freq != 0) {
        DSD_FPRINTF(stderr, "\n  Frequency: %.6lf MHz", (double)freq / 1000000.0);
    } else if (lpchannum != 0 && lpchannum != 0xFFF) {
        DSD_FPRINTF(stderr, "\n  Frequency Not Found in Channel Map;");
    }
}

static void
dmr_cspdu_pf0_publish_active_grant(dsd_state* state, uint8_t lcn, uint16_t lpchannum, uint16_t mbc_lpchannum,
                                   long int frequency, const dmr_t3_grant_class* grant, int emergency) {
    if (lpchannum != 0 && lpchannum != 0xFFF) {
        dmr_cspdu_pf0_publish_grant(state, lcn, lpchannum, frequency, grant, emergency);
    } else if (lpchannum == 0xFFF) {
        dmr_cspdu_pf0_publish_grant(state, lcn, mbc_lpchannum, frequency, grant, emergency);
    }
}

static int
dmr_cspdu_pf0_prepare_dispatch(const dsd_opts* opts, dsd_state* state, const dmr_t3_grant_class* grant, long int freq) {
    if (dmr_cspdu_pf0_should_skip_call(opts, grant)) {
        return 0;
    }
    // A Hold grant is admitted by its own decision (tg_hold_match), never by wiping the voice-sync stamp the
    // no-carrier return reads.
    if (opts->trunk_enable == 0 && freq != 0) {
        state->trunk_vc_freq[0] = freq;
        state->trunk_vc_freq[1] = freq;
    }
    return 1;
}

static void
dmr_cspdu_pf0_try_dispatch_grant(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[],
                                 const dmr_t3_grant_class* grant, uint16_t call_channel, long int freq) {
    if (state->trunk_cc_freq == 0 || opts->trunk_enable != 1 || freq == 0) {
        return;
    }

    const uint32_t target = grant->recipient;
    const uint32_t source = grant->caller;
    dsd_tg_policy_decision policy_decision;
    int policy_allowed =
        dmr_policy_tune_allowed(opts, state, target, source, grant->group, grant->data, &policy_decision);
    if (!policy_allowed) {
        dmr_policy_log_block(opts, grant->group, target, source, &policy_decision);
        return;
    }

    // The target's label from its own namespace: a talkgroup's for a group grant, a radio's for a private one.
    dmr_csbk_print_target_label(state, grant->group ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE, target);

    struct dmr_csbk_result res;
    if (dmr_csbk_parse(cs_pdu_bits, cs_pdu, &res) != 0) {
        return;
    }
    res.freq_hz = freq;
    res.lpcn = call_channel;
    res.policy_valid = 1;
    res.policy = policy_decision;
    dmr_csbk_handle(&res, opts, state);
}

/* A grant for channel @p lpcn whose row has no frequency goes to the missing-channel ledger
   (dmr_t3_missing_note_grant()) when it is a standard grant the site sent that passed its CRC, or that -F accepted as
   RAS (then unchecked); never one an MS sent, a manufacturer's, a failed one or the decoder's own. */
static void
dmr_cspdu_pf0_note_missing_channel(dsd_opts* opts, dsd_state* state, int csbk_fid, uint16_t lpcn,
                                   const dmr_t3_grant_class* grant, uint8_t slot) {
    if (csbk_fid != 0 || DSD_SYNC_IS_DMR_MS(state->synctype)) {
        return;
    }
    const int checked = dmr_csbk_crc_checked(state);
    if (!checked && !dmr_csbk_ras_accepted(opts, state)) {
        return;
    }
    dmr_t3_missing_note_grant(opts, state, lpcn, grant, slot, !checked);
}

/* A channel grant (TS 102 361-4 §7.1.1.1.1). Opcode 56 on MS-sourced data is BS_Dwn_Act, never a grant. The console
   line, the activity row, the policy and the trunk state machine all take the grant's classes and its called and
   calling parties from dmr_t3_grant_classify(). */
static void
dmr_cspdu_pf0_handle_grants(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o,
                            int csbk_fid) {
    dmr_t3_grant_class grant;
    if (!dmr_t3_grant_classify(csbk_o, cs_pdu_bits, state->synctype != DSD_SYNC_DMR_MS_DATA, &grant)) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    long int freq = 0;
    DSD_FPRINTF(stderr, " %s", dmr_csbk_grant_opcode_name((uint8_t)csbk_o));

    uint16_t lpchannum = (uint16_t)convert_bits_into_output(&cs_pdu_bits[16], 12);
    dmr_cspdu_pf0_print_channel_kind(lpchannum);

    uint16_t pluschannum = (uint16_t)convert_bits_into_output(&cs_pdu_bits[16], 13) + 1;
    uint8_t lcn = cs_pdu_bits[28];
    uint8_t st2 = cs_pdu_bits[30];

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "  LPCN: %04d; TS: %d; LPCN+TS: %04d; Target: %08u - Source: %08u ", lpchannum, lcn + 1,
                pluschannum, grant.recipient, grant.caller);
    if (st2) {
        DSD_FPRINTF(stderr, "Emergency; ");
    }
    dmr_gateway_identifier(grant.caller, grant.recipient);

    uint16_t mbc_lpchannum = 0;
    if (!dmr_cspdu_pf0_resolve_frequency(opts, state, cs_pdu_bits, lpchannum, &freq, &mbc_lpchannum)) {
        return;
    }
    dmr_cspdu_pf0_print_frequency(lpchannum, freq);
    dmr_csbk_note_tier3(state, csbk_fid);
    if (freq == 0 && dmr_t3_lpcn_is_channel(lpchannum)) {
        dmr_cspdu_pf0_note_missing_channel(opts, state, csbk_fid, lpchannum, &grant, lcn);
    }

    dmr_cspdu_pf0_publish_active_grant(state, lcn, lpchannum, mbc_lpchannum, freq, &grant, st2);

    if (!dmr_cspdu_pf0_prepare_dispatch(opts, state, &grant, freq)) {
        return;
    }

    dmr_cspdu_pf0_try_dispatch_grant(opts, state, cs_pdu_bits, cs_pdu, &grant,
                                     dmr_cspdu_pf0_call_channel(lpchannum, mbc_lpchannum), freq);
}

/* The TSCC an absolute C_MOVE's MV_AP or an absolute P_CLEAR's appended block names (CHAN @p chan, which may name no
   channel, on @p freq): the channel map learns it (dmr_csbk_learn_channel()) and the control channel hunt takes it as a
   current-site candidate, both only from a checked or RAS-accepted PDU. */
static void
dmr_cspdu_pf0_note_absolute_tscc(dsd_opts* opts, dsd_state* state, uint16_t chan, long int freq) {
    if (!dmr_csbk_crc_checked(state) && !dmr_csbk_ras_accepted(opts, state)) {
        return;
    }
    (void)dmr_csbk_learn_channel(opts, state, chan, freq, 0);
    if (freq != state->trunk_cc_freq) {
        (void)dsd_trunk_cc_candidates_add(state, freq, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    }
}

/* C_MOVE is Move TSCC (TS 102 361-4 §7.1.1.1.3, Table 7.17): the TSCC tells one MS (its individual address, bits 56-79)
   to move to another TSCC, Physical Channel Number bits 44-55, with Mask (25-29), Reg (35) and Backoff (36-39) for its
   access there. Informational: it names no call, so nothing is followed, published or debounced. The absolute escape
   0xFFF names the TSCC in an appended MV_AP block (Table 7.18), which teaches the channel map and the candidate list; a
   single-block C_MOVE naming 0xFFF is malformed and only printed. A call the site moves between payload carriers is
   picked up again from the TSCC's late-entry grants. */
static void
dmr_cspdu_pf0_handle_move(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (csbk_o != 57 || cs_pdu_bits == NULL) {
        return;
    }

    const uint8_t mask = (uint8_t)convert_bits_into_output(&cs_pdu_bits[25], 5);
    const uint8_t reg = cs_pdu_bits[35];
    const uint8_t backoff = (uint8_t)convert_bits_into_output(&cs_pdu_bits[36], 4);
    const uint16_t pcn = (uint16_t)convert_bits_into_output(&cs_pdu_bits[44], 12);
    const uint32_t ms = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);

    uint16_t chan = pcn;
    long int freq = 0;
    dmr_abs_params ap;
    int abs_ok = 0;
    if (pcn == 0xFFF) {
        abs_ok = dmr_abs_params_decode(cs_pdu_bits, dmr_cspdu_current_shape(), &ap);
        if (abs_ok) {
            chan = ap.chan;
            freq = ap.rx_hz;
        }
    } else if (dmr_t3_lpcn_is_channel(pcn)) {
        freq = state->trunk_chan_map[pcn];
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Move TSCC: MS %u to channel %u", ms, (unsigned)chan);
    if (freq > 0) {
        DSD_FPRINTF(stderr, " (%.6f MHz)", (double)freq / 1000000.0);
    }
    DSD_FPRINTF(stderr, "\n  Mask: %u; Reg: %u; Backoff: %u;", (unsigned)mask, (unsigned)reg, (unsigned)backoff);

    if (pcn != 0xFFF) {
        return;
    }
    if (!abs_ok) {
        dmr_cspdu_abs_params_print_unusable(cs_pdu_bits, &ap);
        return;
    }
    dmr_cspdu_pf0_note_absolute_tscc(opts, state, chan, freq);
}

static void
dmr_cspdu_pf0_handle_aloha(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_o != 25) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    dmr_decode_syscode(opts, state, cs_pdu_bits, csbk_fid, 0);

    dmr_sm_note_cc_activity(opts, state, 0);
    if (opts->trunk_is_tuned == 0) {
        rotate_symbol_out_file(opts, state);
    }
}

static void
dmr_cspdu_pf0_handle_p_maint(dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    UNUSED(state);
    if (csbk_o != 42 || cs_pdu_bits == NULL) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " P_MAINT -");
    uint16_t pm_res1 = (uint16_t)convert_bits_into_output(&cs_pdu_bits[16], 12);
    uint8_t pm_kind = (uint8_t)convert_bits_into_output(&cs_pdu_bits[28], 3);
    uint8_t pm_res2 = cs_pdu_bits[31];
    uint32_t pm_target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[32], 24);
    uint32_t pm_source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);

    if (pm_kind == 0) {
        DSD_FPRINTF(stderr, "Disconnect; ");
    } else {
        DSD_FPRINTF(stderr, " Res Kind: %02X", pm_kind);
    }
    if (pm_res1) {
        DSD_FPRINTF(stderr, "Res A: %03X", pm_res1);
    }
    if (pm_res2) {
        DSD_FPRINTF(stderr, "Res B: 1");
    }
    DSD_FPRINTF(stderr, "Target: %d; Source: %d; ", pm_target, pm_source);
    dmr_gateway_identifier(pm_source, pm_target);
}

static void
dmr_cspdu_pf0_handle_ackvit(int csbk_o) {
    if (csbk_o != 30) {
        return;
    }
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " C_ACKVIT (Ackvitation/Authorization) ");
}

static void
dmr_cspdu_pf0_handle_acks(const dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (!(csbk_o == 32 || csbk_o == 33 || csbk_o == 34 || csbk_o == 35)) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    if (csbk_o == 32) {
        DSD_FPRINTF(stderr, " C_ACKD Outbound TSCC; ");
    } else if (csbk_o == 33) {
        DSD_FPRINTF(stderr, " C_ACKU Inbound TSCC; ");
    } else if (csbk_o == 34) {
        DSD_FPRINTF(stderr, " P_ACKD Outbound Payload; ");
    } else {
        DSD_FPRINTF(stderr, " P_ACKU Inbound Payload; ");
    }

    uint8_t response_info = (uint8_t)convert_bits_into_output(&cs_pdu_bits[16], 7);
    uint8_t reason_code = (uint8_t)convert_bits_into_output(&cs_pdu_bits[23], 8);
    uint8_t ack_res1 = cs_pdu_bits[31];
    uint32_t ack_target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[32], 24);
    uint32_t ack_source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);

    // The Tier III reason table (TS 102 361-4 Tables 7.42-7.45), not the Tier II NACK_Rsp one.
    const char* reason_text = dsd_ua_reason_text(DSD_UA_REASON_DMR_TIER3, reason_code);
    if (reason_text != NULL) {
        DSD_FPRINTF(stderr, "Response: %02X; Reason: %02X (%s); ", response_info, reason_code, reason_text);
    } else {
        DSD_FPRINTF(stderr, "Response: %02X; Reason: %02X; ", response_info, reason_code);
    }
    if (ack_res1) {
        DSD_FPRINTF(stderr, " Res: %d", ack_res1);
    }
    DSD_FPRINTF(stderr, "Target: %d; Source: %d; ", ack_target, ack_source);
    dmr_gateway_identifier(ack_source, ack_target);
    // Only the outbound C_ACKD is a TSCC answer a listener hears; only a checked one reaches the activity log.
    if (csbk_o == 32 && dmr_csbk_crc_checked(state)) {
        const dmr_ua_ackd_fields fields = {response_info, reason_code, ack_target, ack_source};
        dmr_ua_c_ackd(opts, state, &fields);
    }
}

static void
dmr_cspdu_pf0_handle_c_rand(int csbk_o) {
    if (csbk_o != 31) {
        return;
    }
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " C_RAND ");
}

static void
dmr_cspdu_pf0_print_tier2_target_source(const char* label, uint8_t cs_pdu_bits[]) {
    uint32_t target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[32], 24);
    uint32_t source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", label);
    DSD_FPRINTF(stderr, "Target [%d] - Source [%d] ", target, source);
}

// NACK_Rsp (TS 102 361-2 clause 7.1.2.4, Table 7.6): unlike UU_V_Req and UU_Ans_Rsp, the source address (bits 32-55,
// the Additional Information Field) comes before the target (56-79). Its Reason Code uses the Tier II table (7.13).
static void
dmr_cspdu_pf0_handle_nack_rsp(const uint8_t cs_pdu_bits[]) {
    const uint8_t source_type = cs_pdu_bits[17];
    const uint8_t service_type = (uint8_t)convert_bits_into_output(&cs_pdu_bits[18], 6);
    const uint8_t reason = (uint8_t)convert_bits_into_output(&cs_pdu_bits[24], 8);
    const uint32_t source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[32], 24);
    const uint32_t target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);
    const char* reason_text = dsd_ua_reason_text(DSD_UA_REASON_DMR_TIER2_NACK, reason);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Negative Acknowledgement Response (NACK_Rsp) - Source [%" PRIu32 "] - Target [%" PRIu32 "] ",
                source, target);
    DSD_FPRINTF(stderr, "- Source Type: %s; Service: %02X; ", source_type ? "MS" : "BS", service_type);
    if (reason_text != NULL) {
        DSD_FPRINTF(stderr, "Reason: %02X (%s); ", reason, reason_text);
    } else {
        DSD_FPRINTF(stderr, "Reason: %02X; ", reason);
    }
}

static void
dmr_cspdu_pf0_handle_tier2_simple(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (csbk_o == 4) {
        dmr_cspdu_pf0_print_tier2_target_source(" Unit to Unit Voice Service Request (UU_V_Req) - ", cs_pdu_bits);
        return;
    }
    if (csbk_o == 5) {
        dmr_cspdu_pf0_print_tier2_target_source(" Unit to Unit Voice Service Answer Response (UU_Ans_Rsp) - ",
                                                cs_pdu_bits);
        return;
    }
    if (csbk_o == 7) {
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, " Channel Timing CSBK (CT_CSBK) ");
        return;
    }
    if (csbk_o == 38) {
        dmr_cspdu_pf0_handle_nack_rsp(cs_pdu_bits);
        return;
    }
    if (csbk_o == 56 && !dmr_t3_grant_is_grant_opcode(csbk_o, state->synctype != DSD_SYNC_DMR_MS_DATA)) {
        dmr_cspdu_pf0_print_tier2_target_source(" BS Outbound Activation (BS_Dwn_Act) - ", cs_pdu_bits);
    }
    UNUSED2(opts, state);
}

static const char*
dmr_cspdu_pf0_ahoy_service_text(uint8_t svc_kind) {
    // Service_Kind (TS 102 361-4 Table 7.49).
    static const char* const k_text[16] = {
        "Voice Call ",
        "Voice Call ",
        "Packet Data Call ",
        "Packet Data Call ",
        "UDT Short Data Call ",
        "UDT Short Data Call ",
        "UDT Short Data Polling Service ",
        "Status Transport Service ",
        "Call Diversion Service ",
        "Call Answer Service ",
        "Full Duplex Voice Call ",
        "Full Duplex Packet Data Call ",
        "Reserved ",
        "Supplementary Service: ",
        "Registration/Authentication/Radio Check: ",
        "Cancel Call Service ",
    };
    return k_text[svc_kind & 0x0FU];
}

// What a kind 13 / 14 C_AHOY asks: a command or poll sent to the target, never its outcome.
static const char*
dmr_cspdu_pf0_ahoy_kind_text(int kind) {
    switch (kind) {
        case DMR_AHOY_STUN: return "Stun Command; ";
        case DMR_AHOY_REVIVE: return "Revive Command; ";
        case DMR_AHOY_KILL: return "Kill Command; ";
        case DMR_AHOY_RADIO_CHECK: return "Radio Check; ";
        case DMR_AHOY_AUTHENTICATION: return "Authentication Challenge; ";
        case DMR_AHOY_SUBSCRIPTION_REQ: return "Talkgroup Subscription List Request; ";
        default: return NULL;
    }
}

static void
dmr_cspdu_pf0_handle_c_ahoy(const dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_o != 28) {
        return;
    }

    dmr_ahoy_fields f;
    dmr_ahoy_fields_from_bits(cs_pdu_bits, &f);
    const uint16_t svc_opt = (uint16_t)convert_bits_into_output(&cs_pdu_bits[16], 7);
    const int kind = dmr_ahoy_classify(&f);
    const char* kind_text = dmr_cspdu_pf0_ahoy_kind_text(kind);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " C_AHOY - ");
    DSD_FPRINTF(stderr, f.group == 0 ? "Private " : "Group ");
    DSD_FPRINTF(stderr, "FID: %02X SVC: %02X ", csbk_fid, svc_opt);
    DSD_FPRINTF(stderr, "%s", dmr_cspdu_pf0_ahoy_service_text(f.service_kind));
    if (kind_text != NULL) {
        DSD_FPRINTF(stderr, "%s", kind_text);
        if (dmr_csbk_crc_checked(state)) {
            dmr_ua_c_ahoy(opts, state, kind, f.group, f.target, f.source);
        }
    }
    if (kind == DMR_AHOY_AUTHENTICATION) {
        // The source field carries the 24-bit challenge (Table 6.17), not an address.
        DSD_FPRINTF(stderr, "Target: %" PRIu32 "; Challenge: %06" PRIX32 "; ", f.target, f.source);
        dmr_gateway_identifier(0, f.target);
        return;
    }
    DSD_FPRINTF(stderr, "Target: %" PRIu32 "; Source: %" PRIu32 "; ", f.target, f.source);
    dmr_gateway_identifier(f.source, f.target);
}

static void
dmr_cspdu_pf0_handle_preamble(const dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (csbk_o != 61 || cs_pdu_bits == NULL) {
        return;
    }

    uint8_t content = cs_pdu_bits[16];
    uint8_t gi = cs_pdu_bits[17];
    uint8_t res = (uint8_t)convert_bits_into_output(&cs_pdu_bits[18], 6);
    uint8_t blocks = (uint8_t)convert_bits_into_output(&cs_pdu_bits[24], 8);
    uint32_t target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[32], 24);
    uint32_t source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Preamble CSBK - ");
    UNUSED2(res, blocks);
    DSD_FPRINTF(stderr, gi == 0 ? "Individual " : "Group ");
    DSD_FPRINTF(stderr, content == 0 ? "CSBK - " : "Data - ");

    if (strcmp(state->dmr_branding_sub, "XPT ") == 0) {
        target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[40], 16);
        source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[64], 16);
        if (gi == 0) {
            uint8_t target_hash[24];
            for (int i = 0; i < 16; i++) {
                target_hash[i] = cs_pdu_bits[40 + i];
            }
            uint8_t tg_hash = crc8(target_hash, 16);
            DSD_FPRINTF(stderr, "Source: %d - Target: %d - Hash: %d ", source, target, tg_hash);
        } else {
            DSD_FPRINTF(stderr, "Source: %d - Target: %d ", source, target);
        }
    } else if (strcmp(state->dmr_branding_sub, "Cap+ ") == 0) {
        if (gi == 0) {
            target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[40], 16);
        }
        source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[64], 16);
        int rest = (uint32_t)convert_bits_into_output(&cs_pdu_bits[60], 4);
        DSD_FPRINTF(stderr, "Source: %d - Target: %d - Rest LSN: %d", source, target, rest);
    } else {
        DSD_FPRINTF(stderr, "Source: %d - Target: %d ", source, target);
    }

    if (opts->trunk_enable == 1 && opts->trunk_tune_data_calls == 1) {
        if (state->currentslot == 0) {
            state->dmrburstL = 6;
        } else {
            state->dmrburstR = 6;
        }
    }
}

static void
dmr_cspdu_pf0_p_protect_mark_slot(dsd_state* state) {
    if (state->currentslot == 0) {
        state->dmrburstL = 1;
    } else {
        state->dmrburstR = 1;
    }
}

static const char*
dmr_cspdu_pf0_p_protect_kind_label(uint8_t p_kind) {
    switch (p_kind) {
        case 0: return " Disable Target PTT (DIS_PTT)";
        case 1: return " Enable Target PTT (EN_PTT)";
        case 2: return " Call Hangtime (ILLEGALLY_PARKED)";
        case 3: return " Enable Target MS PTT (EN_PTT_ONE_MS)";
        default: return "";
    }
}

static void
dmr_cspdu_pf0_handle_p_protect(const dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    if (csbk_o != 47 || cs_pdu_bits == NULL) {
        return;
    }

    uint16_t reserved = (uint16_t)convert_bits_into_output(&cs_pdu_bits[16], 12);
    uint8_t p_kind = (uint8_t)convert_bits_into_output(&cs_pdu_bits[28], 3);
    uint8_t gi = cs_pdu_bits[31];
    uint32_t target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[32], 24);
    uint32_t source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Protect (P_PROTECT) -");
    UNUSED(reserved);
    DSD_FPRINTF(stderr, gi ? " Group" : " Private");
    DSD_FPRINTF(stderr, "%s", dmr_cspdu_pf0_p_protect_kind_label(p_kind));
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "  Source: %08d; Target: %08d; ", source, target);
    dmr_gateway_identifier(source, target);

    if (opts->trunk_enable != 1) {
        return;
    }
    if (p_kind == 2) {
        /* The advisory holds the voice channel for admitted traffic only (issue #637). */
        if (state->trunk_cc_freq != 0 && opts->trunk_is_tuned == 1
            && dsd_receive_allows(opts, state, DSD_SYNC_DMR_BS_VOICE_POS, state->currentslot)) {
            state->last_vc_sync_time = dsd_decode_time();
            state->last_vc_sync_time_m = dsd_decode_now_mono_s();
            if (opts->verbose > 2) {
                DSD_FPRINTF(stderr, " Hold VC (hangtime advisory) ");
            }
        }
        return;
    }
    if (gi && opts->trunk_tune_group_calls == 1) {
        dmr_cspdu_pf0_p_protect_mark_slot(state);
    }
    if (!gi && opts->trunk_tune_private_calls == 1) {
        dmr_cspdu_pf0_p_protect_mark_slot(state);
    }
}

static int
dmr_cspdu_pf0_slot_call_active(const dsd_state* state, uint8_t slot) {
    dsd_call_snapshot call;
    return dsd_call_state_get(state, slot, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
}

// The companion slot is busy when its canonical call epoch is ACTIVE, not just
// when its burst hint shows voice: the hint records the last burst decoded, so
// a slot mid-call can sit on a TLC, data or stale hint between voice bursts,
// and misreading that as free would force-release the channel out from under
// the companion call. The hint values stay as corroboration for late entry,
// where bursts decode before any LC has opened a canonical epoch.
static int
dmr_cspdu_pf0_p_clear_from_voice(const dsd_state* state) {
    int clear = 0;
    if (state->currentslot == 0 && (state->dmrburstR != 16 && state->dmrburstR != 0 && state->dmrburstR != 1)
        && !dmr_cspdu_pf0_slot_call_active(state, 1U)) {
        clear = 2;
    }
    if (state->currentslot == 1 && (state->dmrburstL != 16 && state->dmrburstL != 0 && state->dmrburstL != 1)
        && !dmr_cspdu_pf0_slot_call_active(state, 0U)) {
        clear = 3;
    }
    return clear;
}

static int
dmr_cspdu_pf0_p_clear_from_data(const dsd_opts* opts, const dsd_state* state, int clear) {
    if (opts->trunk_tune_data_calls == 1) {
        if (state->currentslot == 0
            && (state->dmrburstR == 6 || state->dmrburstR == 7 || state->dmrburstR == 8 || state->dmrburstR == 10)) {
            clear = 21;
        }
        if (state->currentslot == 1
            && (state->dmrburstL == 6 || state->dmrburstL == 7 || state->dmrburstL == 8 || state->dmrburstL == 10)) {
            clear = 22;
        }
    }
    return clear;
}

static int
dmr_cspdu_pf0_p_clear_from_hold(const dsd_state* state, int clear) {
    dsd_call_snapshot call;
    if (state->tg_hold == 0 || state->currentslot < 0 || state->currentslot >= DSD_CALL_STATE_SLOT_COUNT
        || dsd_call_state_get(state, (uint8_t)state->currentslot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return clear;
    }
    const uint64_t target = call.policy_target_id != 0U ? call.policy_target_id : call.ota_target_id;
    if ((uint64_t)state->tg_hold == target) {
        clear = state->currentslot == 0 ? 4 : 5;
    }
    return clear;
}

static int
dmr_cspdu_pf0_p_clear_compute(const dsd_opts* opts, const dsd_state* state) {
    int clear = dmr_cspdu_pf0_p_clear_from_voice(state);
    clear = dmr_cspdu_pf0_p_clear_from_data(opts, state, clear);
    clear = dmr_cspdu_pf0_p_clear_from_hold(state, clear);
    return clear;
}

// Only the P_CLEAR's own slot goes idle. The companion's burst hint records
// what that slot last decoded, and this teardown is not an observation of the
// companion: stomping it mid-call would hand every hint consumer a false
// "idle" for a slot whose call is still running. A genuinely idle companion
// re-records 9 from its own idle bursts within a burst period.
static void
dmr_cspdu_pf0_p_clear_mark_slots_idle(dsd_state* state) {
    if (state->currentslot == 0) {
        state->dmrburstL = 9;
        (void)dsd_recent_activity_clear(state, 0U);
        return;
    }
    state->dmrburstR = 9;
    (void)dsd_recent_activity_clear(state, 1U);
}

static void
dmr_cspdu_pf0_p_clear_log_fid_special(const dsd_state* state, int clear, int csbk_fid, int pslot, int oslot,
                                      int* handled) {
    *handled = 1;
    switch (csbk_fid) {
        case 255:
            if (clear) {
                DSD_FPRINTF(stderr,
                            " Slot %d No Encrypted Call Trunking; Slot %d Free; Request return to CC; SM decides "
                            "(may defer by hangtime/activity); ",
                            pslot, oslot);
            } else {
                DSD_FPRINTF(stderr,
                            " Slot %d No Encrypted Call Trunking; Slot %d Busy; Suggest remain on VC; SM decides "
                            "(may defer by hangtime/activity);",
                            pslot, oslot);
            }
            return;
        case 254:
            if (clear) {
                DSD_FPRINTF(stderr,
                            " Cap+ Rest LSN Change: %d; Slot %d Free; Slot %d Free; Request return to Rest LSN; SM "
                            "decides (may defer by hangtime/activity);",
                            state->dmr_rest_channel, pslot, oslot);
            } else {
                DSD_FPRINTF(stderr,
                            " Cap+ Rest LSN Change: %d; Slot %d Free; Slot %d Busy; Suggest remain on LSN; SM "
                            "decides (may defer by hangtime/activity);",
                            state->dmr_rest_channel, pslot, oslot);
            }
            return;
        case 253:
            if (clear) {
                DSD_FPRINTF(stderr,
                            " Cap+ Rest LSN Change: %d; No CSBK Channel Activity; Request return to Rest LSN; SM "
                            "decides (may defer by hangtime/activity);",
                            state->dmr_rest_channel);
            } else {
                DSD_FPRINTF(stderr,
                            " Cap+ Rest LSN Change: %d; CSBK Channel Activity; Suggest remain on LSN; SM decides "
                            "(may defer by hangtime/activity);",
                            state->dmr_rest_channel);
            }
            return;
        case 12:
            if (clear) {
                DSD_FPRINTF(stderr,
                            " Con+ Slot %d Termination: Slot %d Clear or Control CSBK; SM decides (may defer by "
                            "hangtime/activity);",
                            pslot, oslot);
            } else {
                DSD_FPRINTF(stderr,
                            " Con+ Slot %d Termination: Slot %d Busy Voice or Data Call; SM decides (may defer by "
                            "hangtime/activity);",
                            pslot, oslot);
            }
            return;
        default: *handled = 0; return;
    }
}

static int
dmr_cspdu_pf0_p_clear_log_generic(const dsd_state* state, int clear, int pslot, int oslot) {
    if (!clear) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear; Slot %d Busy; Suggest remain on VC; SM decides (may defer by "
                    "hangtime/activity);",
                    pslot, oslot);
    } else if (clear == 1) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear; Slot %d Idle; Request return to CC; SM decides (may defer by "
                    "hangtime/activity);",
                    pslot, oslot);
    } else if (clear == 2 || clear == 3) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear; Slot %d Free; Request return to CC; SM decides (may defer by "
                    "hangtime/activity);",
                    pslot, oslot);
    } else if (clear == 4 || clear == 5) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear w/ TG Hold %d; Slot %d Activity Override; Force return to CC; SM "
                    "decides (honors force); ",
                    pslot, state->tg_hold, oslot);
    } else if (clear == 21 || clear == 22) {
        DSD_FPRINTF(stderr,
                    " Slot %d Clear; Slot %d Data; Suggest remain on DC; SM decides (may defer by "
                    "hangtime/activity);",
                    pslot, oslot);
        clear = 0;
    }
    return clear;
}

static int
dmr_cspdu_pf0_p_clear_log_status(const dsd_state* state, int clear, int csbk_fid, int pslot, int oslot) {
    int handled = 0;
    dmr_cspdu_pf0_p_clear_log_fid_special(state, clear, csbk_fid, pslot, oslot, &handled);
    if (!handled) {
        clear = dmr_cspdu_pf0_p_clear_log_generic(state, clear, pslot, oslot);
    }
    return clear;
}

/* The release a clear asks the machine for: of @p slot (-1: the carrier), forced for the reasons that force it. The
   latch only while the machine follows a call: on the control channel (a composite TSCC's payload slot, or an idle XPT
   status that marked the receiver tuned) it would outlive the clear and release the next followed grant on its first
   tick. */
static void
dmr_cspdu_pf0_p_clear_emit_release(dsd_opts* opts, dsd_state* state, int clear, int slot) {
    if (state->trunk_cc_freq != 0 && opts->trunk_is_tuned == 1) {
        if (dmr_sm_follows_call(dmr_sm_get_ctx())
            && (clear == 1 || clear == 2 || clear == 3 || clear == 4 || clear == 5)) {
            state->trunk_sm_force_release = 1;
        }
        watchdog_event_current(opts, state, 0);
        watchdog_event_current(opts, state, 1);
        dmr_sm_emit_release(opts, state, slot);
    }
}

/* ALLMSI, the totality of all individual MSs and talkgroups (TS 102 361-4 Table A.4), which most Tier III networks
   address to clear a payload channel whatever it carries (§7.1.1.3.2, with G/I 0), and ALLMSID, every MS in the
   system, which a clear to all MSs may name instead. */
#define DMR_CSBK_ALLMSI  0xFFFED4U
#define DMR_CSBK_ALLMSID 0xFFFFFFU

/* A received P_CLEAR's content (TS 102 361-4 Table 7.30). */
typedef struct {
    uint16_t lpcn;   // bits 16-27: the TSCC to move to; 0 the last confirmed one, 0xFFF the appended block's
    uint8_t gi;      // bit 31: 1 the target is a talkgroup, 0 an MS (or ALLMSI)
    uint32_t target; // bits 32-55
    uint32_t source; // bits 56-79
} dmr_cspdu_p_clear_fields;

static void
dmr_cspdu_pf0_p_clear_parse(const uint8_t cs_pdu_bits[], dmr_cspdu_p_clear_fields* f) {
    f->lpcn = (uint16_t)convert_bits_into_output(&cs_pdu_bits[16], 12);
    f->gi = cs_pdu_bits[31];
    f->target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[32], 24);
    f->source = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);
}

/* The TSCC a P_CLEAR sends its addressed parties to, printed: a logical channel the map knows, or the appended
   block's (dmr_cspdu_pf0_note_absolute_tscc()). Another TSCC than the control channel becomes a current-site hunt
   candidate when the clear was checked or RAS-accepted; where the release returns stays the machine's. */
static void
dmr_cspdu_pf0_p_clear_note_tscc(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[], uint16_t lpcn) {
    if (lpcn == 0U) {
        return;
    }
    if (lpcn == 0xFFFU) {
        dmr_abs_params ap;
        if (!dmr_cspdu_abs_params(cs_pdu_bits, &ap)) {
            return;
        }
        DSD_FPRINTF(stderr, " TSCC: %u (%.6f MHz);", (unsigned)ap.chan, (double)ap.rx_hz / 1000000.0);
        dmr_cspdu_pf0_note_absolute_tscc(opts, state, ap.chan, ap.rx_hz);
        return;
    }
    const long freq = dmr_t3_lpcn_is_channel(lpcn) ? state->trunk_chan_map[lpcn] : 0L;
    DSD_FPRINTF(stderr, " TSCC: %u", (unsigned)lpcn);
    if (freq <= 0) {
        DSD_FPRINTF(stderr, ";");
        return;
    }
    DSD_FPRINTF(stderr, " (%.6f MHz);", (double)freq / 1000000.0);
    if (freq != state->trunk_cc_freq && (dmr_csbk_crc_checked(state) || dmr_csbk_ras_accepted(opts, state))) {
        (void)dsd_trunk_cc_candidates_add(state, freq, 1, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    }
}

/* Whether a received standard P_CLEAR clears the call the machine follows: received on that call's slot and addressed
   to it (its talkgroup, either party of a private call, or every MS). */
static int
dmr_cspdu_pf0_p_clear_for_followed_call(const dsd_state* state, const dmr_cspdu_p_clear_fields* f) {
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    const int slot = state->currentslot & 1;
    if (f->gi == 0U && (f->target == DMR_CSBK_ALLMSI || f->target == DMR_CSBK_ALLMSID)) {
        return dmr_sm_follows_slot(ctx, slot);
    }
    return dmr_sm_names_followed_call(ctx, slot, f->gi, f->target);
}

/* P_CLEAR (TS 102 361-4 §7.1.1.3.2). The standard one (FID 0) is read as Table 7.30: it applies only on the followed
   call's slot and when its address names that call, otherwise nothing changes; applied, it releases only its own slot,
   so a busy companion slot keeps the carrier. The clear the encryption lockout synthesizes
   (dmr_cspdu_release_channel()) always applies, to the carrier, as does another feature set's opcode 46, whose content
   is the manufacturer's. */
static void
dmr_cspdu_pf0_handle_p_clear(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_o != 46) {
        return;
    }

    const int pslot = state->currentslot + 1;
    const int oslot = ((state->currentslot ^ 1) & 1) + 1;
    int clear = dmr_cspdu_pf0_p_clear_compute(opts, state);
    const int addressed = csbk_fid == 0 && !s_dmr_cspdu_synthesized;
    dmr_cspdu_p_clear_fields f;
    DSD_MEMSET(&f, 0, sizeof(f));
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Clear (P_CLEAR) ");
    if (addressed) {
        dmr_cspdu_pf0_p_clear_parse(cs_pdu_bits, &f);
        DSD_FPRINTF(stderr, "%s: %" PRIu32 "; Source: %" PRIu32 ";", f.gi ? "TG" : "Target", f.target, f.source);
        dmr_cspdu_pf0_p_clear_note_tscc(opts, state, cs_pdu_bits, f.lpcn);
    }
    if (opts->trunk_enable != 1) {
        return;
    }
    if (addressed && !dmr_cspdu_pf0_p_clear_for_followed_call(state, &f)) {
        DSD_FPRINTF(stderr, "%s",
                    dmr_sm_follows_call(dmr_sm_get_ctx()) ? " not for the followed call;" : " no followed call;");
        return;
    }
    dmr_cspdu_pf0_p_clear_mark_slots_idle(state);
    clear = dmr_cspdu_pf0_p_clear_log_status(state, clear, csbk_fid, pslot, oslot);
    dmr_cspdu_pf0_p_clear_emit_release(opts, state, clear, addressed ? (state->currentslot & 1) : -1);
}

typedef struct {
    uint8_t a_type;
    uint16_t bparms1;
    uint8_t bpbits1[14];
    uint8_t reg_req;
    uint8_t backoff;
    uint16_t syscode;
    uint8_t par;
    uint32_t bparms2;
    uint8_t bpbits2[24];
    uint8_t mbc_csbko;
    uint8_t mbc_res;
    uint8_t mbc_cc;
    uint8_t mbc_cdeftype;
    uint8_t mbc_res2;
    unsigned long long mbc_cdefparms;
    uint16_t a_channel;
} dmr_cspdu_pf0_c_bcast_fields;

typedef struct {
    uint16_t lpchannum;
    uint16_t abs_tx_int;
    uint16_t abs_tx_step;
    uint16_t abs_rx_int;
    uint16_t abs_rx_step;
    long freqt;
    long freqr;
} dmr_cspdu_pf0_c_bcast_abs_freqs;

static void
dmr_cspdu_pf0_c_bcast_parse(const uint8_t cs_pdu_bits[], dmr_cspdu_pf0_c_bcast_fields* f) {
    DSD_MEMSET(f, 0, sizeof(*f));
    f->a_type = (uint8_t)convert_bits_into_output(&cs_pdu_bits[16], 5);
    f->bparms1 = (uint16_t)convert_bits_into_output(&cs_pdu_bits[21], 14);
    for (int i = 0; i < 14; i++) {
        f->bpbits1[i] = cs_pdu_bits[21 + i];
    }

    f->reg_req = cs_pdu_bits[35];
    f->backoff = (uint8_t)convert_bits_into_output(&cs_pdu_bits[36], 4);
    f->syscode = (uint16_t)convert_bits_into_output(&cs_pdu_bits[40], 14);
    f->par = (uint8_t)convert_bits_into_output(&cs_pdu_bits[54], 2);
    f->bparms2 = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);
    for (int i = 0; i < 24; i++) {
        f->bpbits2[i] = cs_pdu_bits[56 + i];
    }

    f->a_channel = (uint16_t)convert_bits_into_output(&f->bpbits2[12], 12);
    if (dmr_cspdu_current_shape()->is_mbc == 0U) {
        return; // a single CSBK has no continuation (its fields stay 0)
    }
    f->mbc_csbko = (uint8_t)convert_bits_into_output(&cs_pdu_bits[98], 6);
    f->mbc_res = (uint8_t)convert_bits_into_output(&cs_pdu_bits[104], 4);
    f->mbc_cc = (uint8_t)convert_bits_into_output(&cs_pdu_bits[108], 4);
    f->mbc_cdeftype = (uint8_t)convert_bits_into_output(&cs_pdu_bits[112], 4);
    f->mbc_res2 = (uint8_t)convert_bits_into_output(&cs_pdu_bits[116], 2);
    f->mbc_cdefparms = (unsigned long long)convert_bits_into_output(&cs_pdu_bits[118], 58);
}

static void
dmr_cspdu_pf0_c_bcast_print_type(uint8_t a_type) {
    static const char* const k_labels[8] = {" Announce/Withdraw TSCC (Ann_WD_TSCC)",
                                            " Specify Call Timer Parameters (CallTimer_Parms)",
                                            " Vote Now Advice (Vote_Now)",
                                            " Broadcast Local Time (Local_Time)",
                                            " Mass Registration (MassReg)",
                                            " Announce Logical Channel/Frequency Relationship (Chan_Freq)",
                                            " Adjacent Site Information (Adjacent_Site)",
                                            " General Site Parameters (Gen_Site_Params)"};

    if (a_type < 8U) {
        DSD_FPRINTF(stderr, "%s", k_labels[a_type]);
        return;
    }
    if (a_type < 0x1EU) {
        DSD_FPRINTF(stderr, " Reserved: %02X", a_type);
        return;
    }
    DSD_FPRINTF(stderr, " Manufacturer Specific: %02X", a_type);
}

static void
dmr_cspdu_pf0_c_bcast_parse_abs_freqs(const uint8_t cs_pdu_bits[], dmr_cspdu_pf0_c_bcast_abs_freqs* abs_freqs) {
    abs_freqs->lpchannum = (uint16_t)convert_bits_into_output(&cs_pdu_bits[118], 12);
    abs_freqs->abs_tx_int = (uint16_t)convert_bits_into_output(&cs_pdu_bits[130], 10);
    abs_freqs->abs_tx_step = (uint16_t)convert_bits_into_output(&cs_pdu_bits[140], 13);
    abs_freqs->abs_rx_int = (uint16_t)convert_bits_into_output(&cs_pdu_bits[153], 10);
    abs_freqs->abs_rx_step = (uint16_t)convert_bits_into_output(&cs_pdu_bits[163], 13);
    abs_freqs->freqr = (abs_freqs->abs_rx_int * 1000000L) + (abs_freqs->abs_rx_step * 125L);
    abs_freqs->freqt = (abs_freqs->abs_tx_int * 1000000L) + (abs_freqs->abs_tx_step * 125L);
}

static void
dmr_cspdu_pf0_c_bcast_print_abs_freqs(const dmr_cspdu_pf0_c_bcast_abs_freqs* abs_freqs, int apcn) {
    DSD_FPRINTF(stderr, apcn ? "\n APCN: %04d;" : "\n LPCN: %04d;", abs_freqs->lpchannum);
    DSD_FPRINTF(stderr, " RX Base: %d; RX Step: %d; RX Freq: %ld;", abs_freqs->abs_rx_int * 1000000,
                abs_freqs->abs_rx_step * 125, abs_freqs->freqr);
    DSD_FPRINTF(stderr, "\n            ");
    DSD_FPRINTF(stderr, " TX Base: %d; TX Step: %d; TX Freq: %ld;", abs_freqs->abs_tx_int * 1000000,
                abs_freqs->abs_tx_step * 125, abs_freqs->freqt);
}

static void
dmr_cspdu_pf0_c_bcast_print_unknown_cdef(const dmr_cspdu_pf0_c_bcast_fields* f, int include_cc) {
    DSD_FPRINTF(stderr, "\n Unknown CDEFType: %X; CDEFParms: %015llX", f->mbc_cdeftype, f->mbc_cdefparms);
    DSD_FPRINTF(stderr, " MBC Op: %02X;", f->mbc_csbko);
    if (include_cc) {
        DSD_FPRINTF(stderr, " CC: %d;", f->mbc_cc);
    }
    DSD_FPRINTF(stderr, " RES1: %X;", f->mbc_res);
    DSD_FPRINTF(stderr, " RES2: %X;", f->mbc_res2);
}

/* The BC_AP absolute parameters of the C_BCAST being handled (dmr_abs_params_decode()), printed (@p apcn: as the APCN),
   in @p abs_freqs. 0 when unusable, with the reason printed (a reserved Cdeftype as before, @p include_cc its CC). */
static int
dmr_cspdu_pf0_c_bcast_abs(const uint8_t cs_pdu_bits[], const dmr_cspdu_pf0_c_bcast_fields* f, int include_cc, int apcn,
                          dmr_cspdu_pf0_c_bcast_abs_freqs* abs_freqs) {
    dmr_abs_params ap;
    if (!dmr_abs_params_decode(cs_pdu_bits, dmr_cspdu_current_shape(), &ap)) {
        if (ap.cdeftype != 0U) {
            dmr_cspdu_pf0_c_bcast_print_unknown_cdef(f, include_cc);
        } else {
            DSD_FPRINTF(stderr, " MBC Absolute Parameters missing or out of range;");
        }
        return 0;
    }
    dmr_cspdu_pf0_c_bcast_parse_abs_freqs(cs_pdu_bits, abs_freqs);
    abs_freqs->freqr = ap.rx_hz;
    abs_freqs->freqt = ap.tx_hz;
    dmr_cspdu_pf0_c_bcast_print_abs_freqs(abs_freqs, apcn);
    return 1;
}

/* A Chan_Freq names a logical channel's physical frequency (TS 102 361-4 §6.7.1.6), not a TSCC (Ann_WD_TSCC announces
   those, §6.7.1.1): it goes into the sparse channel map, never into the positional channel list (-Y, the channel
   cycle), whose rows are the user's. */
static void
dmr_cspdu_pf0_c_bcast_maybe_store_channel(dsd_opts* opts, dsd_state* state, uint16_t a_channel, long freqr) {
    if (a_channel == 0 || a_channel == 0xFFF || freqr == 0
        || !dmr_csbk_learn_channel(opts, state, a_channel, freqr, 0)) {
        return;
    }
    const long cand[1] = {freqr};
    dmr_sm_on_neighbor_update(opts, state, cand, 1);
}

static void
dmr_cspdu_pf0_c_bcast_print_add_remove(uint8_t flag) {
    if (flag == 0) {
        DSD_FPRINTF(stderr, " Add;");
    }
    if (flag == 1) {
        DSD_FPRINTF(stderr, " Remove;");
    }
}

static void
dmr_cspdu_pf0_c_bcast_try_switch_tscc(dsd_opts* opts, dsd_state* state, long f1, long f2, uint8_t ch1_flag,
                                      uint8_t ch2_flag) {
    if (opts->trunk_enable != 1 || opts->trunk_is_tuned != 0 || state->trunk_cc_freq == 0) {
        return;
    }

    long cur = state->trunk_cc_freq;
    long next = 0;
    if (cur == f1 && ch1_flag == 1 && ch2_flag == 0 && f2 > 0) {
        next = f2;
    } else if (cur == f2 && ch2_flag == 1 && ch1_flag == 0 && f1 > 0) {
        next = f1;
    }

    if (next > 0 && next != cur) {
        const dsd_trunk_tune_result result = dmr_sm_return_to_cc(dmr_sm_get_ctx(), opts, state, next);
        if (!dsd_trunk_tune_result_is_ok(result)) {
            return;
        }
        DSD_FPRINTF(stderr, "\n Switched to announced TSCC: %.6lf MHz\n", (double)next / 1000000.0);
    }
}

/* The frequency the channel map holds for a logical Ann_WD_TSCC channel: 0 for CHNULL, the escape or an unmapped
   one. */
static long
dmr_cspdu_pf0_c_bcast_ann_wd_freq(const dsd_state* state, uint16_t bcast_ch) {
    return dmr_t3_lpcn_is_channel(bcast_ch) ? state->trunk_chan_map[bcast_ch] : 0L;
}

/* The MBC form of Ann_WD_TSCC (TS 102 361-4 §7.2.19.1): a header and a BC_AP announcing or withdrawing one TSCC,
   whatever BCAST_CH1 holds. Only an announced TSCC (AW_FLAG1 0), from a checked or RAS-accepted PDU, teaches the
   channel map CHAN's frequency (SITE) and becomes a hunt candidate: a withdrawal may name a frequency the channel no
   longer has, so it writes nothing. A withdraw removes no candidate an earlier announcement added (a decided limit). */
static void
dmr_cspdu_pf0_c_bcast_ann_wd_absolute(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[],
                                      const dmr_cspdu_pf0_c_bcast_fields* f, uint8_t cc_ch1, uint8_t aw_flag1) {
    DSD_FPRINTF(stderr, " TSCC CC: %d;", cc_ch1);
    dmr_cspdu_pf0_c_bcast_print_add_remove(aw_flag1);
    dmr_cspdu_pf0_c_bcast_abs_freqs abs_freqs;
    if (!dmr_cspdu_pf0_c_bcast_abs(cs_pdu_bits, f, 0, 1, &abs_freqs)
        || (!dmr_csbk_crc_checked(state) && !dmr_csbk_ras_accepted(opts, state)) || aw_flag1 != 0U) {
        return;
    }
    (void)dmr_csbk_learn_channel(opts, state, abs_freqs.lpchannum, abs_freqs.freqr, 0);
    const long cand[1] = {abs_freqs.freqr};
    dmr_sm_on_neighbor_update(opts, state, cand, 1);
}

/* Ann_WD_TSCC (TS 102 361-4 §7.2.19.1, Table 7.69): up to two TSCCs as logical channels, BCAST_CH1/2 (Parms2 bits
   56-67, 68-79) with their colour codes (Parms1 25-28, 29-32) and AW_FLAG1/2 (33, 34: 0 add to the hunt list, 1
   withdraw), or one TSCC in the MBC form. Only a channel marked add becomes a hunt candidate, and only from a checked
   or RAS-accepted PDU; withdrawing the control channel the receiver is on moves it to the other one announced. */
static void
dmr_cspdu_pf0_c_bcast_handle_ann_wd_tscc(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[],
                                         const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t ann_res = (uint8_t)convert_bits_into_output(&f->bpbits1[0], 4);
    uint8_t cc_ch1 = (uint8_t)convert_bits_into_output(&f->bpbits1[4], 4);
    uint8_t cc_ch2 = (uint8_t)convert_bits_into_output(&f->bpbits1[8], 4);
    uint8_t ch1_flag = f->bpbits1[12];
    uint8_t ch2_flag = f->bpbits1[13];
    uint16_t bcast_ch1 = (uint16_t)convert_bits_into_output(&f->bpbits2[0], 12);
    uint16_t bcast_ch2 = (uint16_t)convert_bits_into_output(&f->bpbits2[12], 12);

    DSD_FPRINTF(stderr, "\n");
    if (ann_res) {
        DSD_FPRINTF(stderr, " Res: %X;", ann_res);
    }
    if (dmr_cspdu_current_shape()->is_mbc) {
        dmr_cspdu_pf0_c_bcast_ann_wd_absolute(opts, state, cs_pdu_bits, f, cc_ch1, ch1_flag);
        return;
    }
    DSD_FPRINTF(stderr, " LPCN CH1: %d; CC: %d;", bcast_ch1, cc_ch1);
    dmr_cspdu_pf0_c_bcast_print_add_remove(ch1_flag);
    DSD_FPRINTF(stderr, " LPCN CH2: %d; CC: %d;", bcast_ch2, cc_ch2);
    dmr_cspdu_pf0_c_bcast_print_add_remove(ch2_flag);

    const long f1 = dmr_cspdu_pf0_c_bcast_ann_wd_freq(state, bcast_ch1);
    const long f2 = dmr_cspdu_pf0_c_bcast_ann_wd_freq(state, bcast_ch2);
    long cand[2];
    int ccount = 0;
    if (f1 > 0 && ch1_flag == 0U) {
        cand[ccount++] = f1;
    }
    if (f2 > 0 && ch2_flag == 0U && (ccount == 0 || f2 != cand[0])) {
        cand[ccount++] = f2;
    }
    if (ccount > 0 && (dmr_csbk_crc_checked(state) || dmr_csbk_ras_accepted(opts, state))) {
        dmr_sm_on_neighbor_update(opts, state, cand, ccount);
    }

    dmr_cspdu_pf0_c_bcast_try_switch_tscc(opts, state, f1, f2, ch1_flag, ch2_flag);
}

static void
dmr_cspdu_pf0_c_bcast_handle_call_timer(const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint16_t t_emerg_timer = (uint16_t)convert_bits_into_output(&f->bpbits1[0], 9);
    uint8_t t_packet_timer = (uint8_t)convert_bits_into_output(&f->bpbits1[9], 5);
    uint16_t t_msms_timer = (uint16_t)convert_bits_into_output(&f->bpbits2[0], 12);
    uint16_t t_msline_timer = (uint16_t)convert_bits_into_output(&f->bpbits2[12], 12);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Timers - Emergency: %d; Packet: %d; MS-MS: %d; Line: %d; ", t_emerg_timer, t_packet_timer,
                t_msms_timer, t_msline_timer);
}

static const char*
dmr_cspdu_pf0_c_bcast_weekday_label(uint8_t dofw) {
    static const char* const k_days[] = {"",          "Sunday",   "Monday", "Tuesday",
                                         "Wednesday", "Thursday", "Friday", "Saturday"};
    return (dofw < (sizeof(k_days) / sizeof(k_days[0]))) ? k_days[dofw] : NULL;
}

static int
dmr_cspdu_pf0_c_bcast_offset_minutes(uint8_t lt_off_fr) {
    switch (lt_off_fr) {
        case 1: return 15;
        case 2: return 30;
        case 3: return 45;
        default: return 0;
    }
}

static void
dmr_cspdu_pf0_c_bcast_handle_local_time(const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t lt_day = (uint8_t)convert_bits_into_output(&f->bpbits1[0], 5);
    uint8_t lt_mon = (uint8_t)convert_bits_into_output(&f->bpbits1[5], 4);
    uint8_t lt_off = (uint8_t)convert_bits_into_output(&f->bpbits1[9], 4);
    uint8_t lt_off_sign = f->bpbits1[13];
    uint8_t lt_hour = (uint8_t)convert_bits_into_output(&f->bpbits2[0], 5);
    uint8_t lt_mins = (uint8_t)convert_bits_into_output(&f->bpbits2[5], 6);
    uint8_t lt_secs = (uint8_t)convert_bits_into_output(&f->bpbits2[11], 6);
    uint8_t lt_dofw = (uint8_t)convert_bits_into_output(&f->bpbits2[17], 3);
    uint8_t lt_off_fr = (uint8_t)convert_bits_into_output(&f->bpbits2[20], 2);
    uint8_t lt_res = (uint8_t)convert_bits_into_output(&f->bpbits2[22], 2);

    int offset = lt_off_sign ? -(int)lt_off : (int)lt_off;
    int localhour = lt_off_sign ? (int)lt_hour - (int)lt_off : (int)lt_hour + (int)lt_off;
    int localmin = (int)lt_mins + dmr_cspdu_pf0_c_bcast_offset_minutes(lt_off_fr);

    DSD_FPRINTF(stderr, "\n");
    if (lt_mon != 0 && lt_day != 0) {
        DSD_FPRINTF(stderr, " Date: %d.%d;", lt_mon, lt_day);
    }
    const char* day = dmr_cspdu_pf0_c_bcast_weekday_label(lt_dofw);
    if (day && lt_dofw != 0) {
        DSD_FPRINTF(stderr, " %s;", day);
    }
    DSD_FPRINTF(stderr, " UTC Time: %02d:%02d:%02d;", lt_hour, lt_mins, lt_secs);
    if (lt_off != 15) {
        DSD_FPRINTF(stderr, " Local: %02d:%02d:%02d;", localhour, localmin, lt_secs);
        DSD_FPRINTF(stderr, " Offset: %d;", offset);
    }
    if (lt_res) {
        DSD_FPRINTF(stderr, " Res: %d;", lt_res);
    }
}

/* The LPCN a Chan_Freq announces: its header's CH when that names a channel, else its BC_AP's CHAN; 0 when neither
   names one. One whose two name different channels never gets here (dmr_cspdu_c_bcast_contradicts()). */
static uint16_t
dmr_cspdu_pf0_c_bcast_chan_freq_lpcn(uint16_t header_ch, uint16_t bcap_chan) {
    if (dmr_t3_lpcn_is_channel(header_ch)) {
        return header_ch;
    }
    return dmr_t3_lpcn_is_channel(bcap_chan) ? bcap_chan : 0U;
}

/* Chan_Freq (TS 102 361-4 §7.2.19.6, Table 7.78) is always an MBC header and a BC_AP: CH (header bits 68-79) names
   the channel, or 0xFFF leaves it to the BC_AP's CHAN. The pair is learned (SITE, or UNVERIFIED when RAS-accepted) and
   its frequency joins the hunt list and the control channel candidates. */
static void
dmr_cspdu_pf0_c_bcast_handle_chan_freq(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[],
                                       const dmr_cspdu_pf0_c_bcast_fields* f) {
    if (f->a_channel == 0) {
        DSD_FPRINTF(stderr, " LPCN: Null;");
        return;
    }
    dmr_cspdu_pf0_c_bcast_abs_freqs abs_freqs;
    if (!dmr_cspdu_pf0_c_bcast_abs(cs_pdu_bits, f, 0, f->a_channel == 0xFFF, &abs_freqs)) {
        return;
    }
    const uint16_t lpcn = dmr_cspdu_pf0_c_bcast_chan_freq_lpcn(f->a_channel, abs_freqs.lpchannum);
    if (lpcn != 0U) {
        dmr_cspdu_pf0_c_bcast_maybe_store_channel(opts, state, lpcn, abs_freqs.freqr);
    }
}

/* Vote_Now (TS 102 361-4 §7.2.19.3, Tables 7.71/7.72) and Adjacent_Site (§7.2.19.7, Table 7.79) share their Parms:
   the assessed TSCC's system code (Parms1), AC-info-available (56), Active_connection (57), the confirmed and adjacent
   channel priorities (58-60, 61-63), 4 reserved bits and the channel (68-79). Vote_Now's 0xFFF appends a VN_AP; the
   spec defines Adjacent_Site with a logical channel only, and its absolute form (a BC_AP after 0xFFF) is read as
   SDRTrunk reads it. An absolute entry is a neighbour's frequency: NEIGHBOUR, filling empty rows only, and a hunt
   candidate, both only from a checked or RAS-accepted PDU. */
static void
dmr_cspdu_pf0_c_bcast_handle_vote_or_adjacent(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[],
                                              int csbk_fid, const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t active_ava = f->bpbits2[0];
    uint8_t active_con = f->bpbits2[1];
    uint8_t c_chan_pri = (uint8_t)convert_bits_into_output(&f->bpbits2[2], 3);
    uint8_t a_chan_pri = (uint8_t)convert_bits_into_output(&f->bpbits2[5], 3);
    uint8_t a_reserved = (uint8_t)convert_bits_into_output(&f->bpbits2[8], 4);

    DSD_FPRINTF(stderr, "\n");
    dmr_decode_syscode(opts, state, cs_pdu_bits, csbk_fid, 1);
    /* Without Active_connection information only that bit means nothing (§6.7.1.3): the rest is decoded as ever. */
    if (active_ava != 1) {
        DSD_FPRINTF(stderr, " Active Connection Information Not Available;");
    } else {
        DSD_FPRINTF(stderr, active_con == 1 ? " Online;" : " Offline;");
    }
    DSD_FPRINTF(stderr, " CC Pri: %d;", c_chan_pri);
    DSD_FPRINTF(stderr, " AC Pri: %d;", a_chan_pri);
    if (a_reserved) {
        DSD_FPRINTF(stderr, " Res: %X;", a_reserved);
    }
    if (f->a_channel != 0xFFF && f->a_channel != 0) {
        DSD_FPRINTF(stderr, " LPCN: %d;", f->a_channel);
    } else if (f->a_channel == 0) {
        DSD_FPRINTF(stderr, " LPCN: Null;");
    }

    if (f->a_channel != 0xFFF) {
        return;
    }

    dmr_cspdu_pf0_c_bcast_abs_freqs abs_freqs;
    if (!dmr_cspdu_pf0_c_bcast_abs(cs_pdu_bits, f, f->a_type == 2, 1, &abs_freqs)) {
        return;
    }
    if (!dmr_csbk_crc_checked(state) && !dmr_csbk_ras_accepted(opts, state)) {
        return;
    }
    (void)dmr_csbk_learn_channel(opts, state, abs_freqs.lpchannum, abs_freqs.freqr, 1);
    const long cand[1] = {abs_freqs.freqr};
    dmr_sm_on_neighbor_update(opts, state, cand, 1);
}

// Broadcast General Site Parameters (TS 102 361-4 Table 7.80): BParms1 is reserved; BParms2 is Current Site
// Information (8: reserved, Hibernating_Flag, 6 reserved; Table 7.101), 8 reserved bits and Network Information (8: Reg
// and TG Subscription, 7 reserved; Table 7.102). The Reg and TG Subscription flag says whether registering radios send
// their talkgroup list; whether they must register at all is the C_BCAST Reg bit.
static void
dmr_cspdu_pf0_c_bcast_handle_gen_site_params(const dmr_cspdu_pf0_c_bcast_fields* f) {
    const uint8_t csi = (uint8_t)convert_bits_into_output(&f->bpbits2[0], 8);
    const uint8_t nin = (uint8_t)convert_bits_into_output(&f->bpbits2[16], 8);
    const uint8_t hibernate_flag = f->bpbits2[1];
    const uint8_t reg_tg_sub = f->bpbits2[16];

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Hibernating: %d; Reg TG Subscription: %d (%s); Current Site Info: %02X; Network Info: %02X;",
                hibernate_flag, reg_tg_sub, reg_tg_sub ? "register without TG list" : "register with TG list", csi,
                nin);
    if (f->bparms1 != 0U) {
        DSD_FPRINTF(stderr, " BParms1 (reserved): %04X;", f->bparms1);
    }
}

static void
dmr_cspdu_pf0_c_bcast_handle_mass_reg(const dmr_cspdu_pf0_c_bcast_fields* f) {
    uint8_t reg_window = (uint8_t)convert_bits_into_output(&f->bpbits1[5], 4);
    uint8_t aloha_mask = (uint8_t)convert_bits_into_output(&f->bpbits1[9], 5);
    /* ETSI TS 102 361-4 Table 7.76 assigns the entire 24-bit BParms2 field to the address. */
    const uint32_t reg_address = f->bparms2;

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Reg Window: %X; Aloha Mask: %02X; Target: %" PRIu32 "; ", reg_window, aloha_mask,
                reg_address);
    dmr_gateway_identifier(0, reg_address);
}

static void
dmr_cspdu_pf0_c_bcast_print_payload(const dsd_opts* opts, const dmr_cspdu_pf0_c_bcast_fields* f) {
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n ");
    // The 14 MODEL + NET + SITE bits of the 16-bit System Identity Code, as everywhere else; its two PAR bits apart.
    DSD_FPRINTF(stderr, " SYS: %04X;", f->syscode);
    DSD_FPRINTF(stderr, " PAR: %d;", f->par);
    DSD_FPRINTF(stderr, " Reg: %d;", f->reg_req);
    DSD_FPRINTF(stderr, " Backoff: %X;", f->backoff);
    DSD_FPRINTF(stderr, " BParms1: %04X;", f->bparms1);
    DSD_FPRINTF(stderr, " BParms2: %06X;", f->bparms2);
    if (f->mbc_cdefparms == 0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n ");
    DSD_FPRINTF(stderr, " MBC Op: %02X;", f->mbc_csbko);
    if (f->a_type == 2) {
        DSD_FPRINTF(stderr, " CC: %d;", f->mbc_cc);
    }
    DSD_FPRINTF(stderr, " RES1: %X;", f->mbc_res);
    DSD_FPRINTF(stderr, " RES2: %X;", f->mbc_res2);
    DSD_FPRINTF(stderr, " CDEFTYPE: %X;", f->mbc_cdeftype);
    DSD_FPRINTF(stderr, " CDEFPARMS: %015llX;", f->mbc_cdefparms);
}

static void
dmr_cspdu_pf0_c_bcast_dispatch(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu_bits[], int csbk_fid,
                               const dmr_cspdu_pf0_c_bcast_fields* f) {
    switch (f->a_type) {
        case 0: dmr_cspdu_pf0_c_bcast_handle_ann_wd_tscc(opts, state, cs_pdu_bits, f); return;
        case 1: dmr_cspdu_pf0_c_bcast_handle_call_timer(f); return;
        case 2:
        case 6: dmr_cspdu_pf0_c_bcast_handle_vote_or_adjacent(opts, state, cs_pdu_bits, csbk_fid, f); return;
        case 3: dmr_cspdu_pf0_c_bcast_handle_local_time(f); return;
        case 4: dmr_cspdu_pf0_c_bcast_handle_mass_reg(f); return;
        case 5: dmr_cspdu_pf0_c_bcast_handle_chan_freq(opts, state, cs_pdu_bits, f); return;
        case 7: dmr_cspdu_pf0_c_bcast_handle_gen_site_params(f); return;
        default: return;
    }
}

static void
dmr_cspdu_pf0_handle_c_bcast(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_o != 40) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Announcements (C_BCAST)");

    dmr_csbk_note_tier3(state, csbk_fid);
    dmr_cspdu_pf0_c_bcast_fields f;
    dmr_cspdu_pf0_c_bcast_parse(cs_pdu_bits, &f);
    dmr_cspdu_pf0_c_bcast_print_type(f.a_type);
    /* A standard C_BCAST's System Identity Code (bits 40-55, Table 7.20) is the transmitting site's, whatever it
       announces (Vote_Now's and Adjacent_Site's BParms1 is the one that names another site): checked, its MODEL and NET
       are the network baseline before anything the announcement teaches is learned under it. */
    if (csbk_fid == 0 && dmr_csbk_crc_checked(state)) {
        dmr_t3_chan_note_checked_syscode(state, f.syscode);
    }
    dmr_cspdu_pf0_c_bcast_dispatch(opts, state, cs_pdu_bits, csbk_fid, &f);
    dmr_cspdu_pf0_c_bcast_print_payload(opts, &f);
}

// Opcodes whose meaning only the standard feature set (FID 0x00) gives (every one of their tables says "FID shall be set
// to 0000 0000"): vendors reuse the numbers for their own PDUs (TS 102 361-2 / -4).
static int
dmr_cspdu_pf0_is_standard_only(const dsd_state* state, int csbk_o) {
    switch (csbk_o) {
        case 4:  // UU_V_Req
        case 5:  // UU_Ans_Rsp
        case 7:  // CT_CSBK
        case 28: // C_AHOY
        case 30: // C_ACKVIT
        case 31: // C_RAND
        case 32: // C_ACKD
        case 33: // C_ACKU
        case 34: // P_ACKD
        case 35: // P_ACKU
        case 38: // NACK_Rsp
        case 42: // P_MAINT
            return 1;
        case 56: // BS_Dwn_Act on MS data; elsewhere a grant opcode, which runs for any FID
            return state->synctype == DSD_SYNC_DMR_MS_DATA;
        default: return 0;
    }
}

// The standard-only handlers run for FID 0x00. Under another FID the opcode is the manufacturer's and prints as a
// manufacturer CSBK. No vendor dispatcher below decodes any of these opcodes; a vendor dispatcher that starts decoding
// one must also take its opcode out of dmr_cspdu_pf0_is_standard_only(), or both lines print.
static void
dmr_cspdu_pf0_handle_standard_only(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if (csbk_fid != 0) {
        if (dmr_cspdu_pf0_is_standard_only(state, csbk_o)) {
            DSD_FPRINTF(stderr, "\n");
            DSD_FPRINTF(stderr, " Manufacturer CSBK: FID %02X; Opcode %02X;", csbk_fid, csbk_o);
        }
        return;
    }
    dmr_cspdu_pf0_handle_c_ahoy(opts, state, cs_pdu_bits, csbk_o, csbk_fid);
    dmr_cspdu_pf0_handle_p_maint(state, cs_pdu_bits, csbk_o);
    dmr_cspdu_pf0_handle_ackvit(csbk_o);
    dmr_cspdu_pf0_handle_acks(opts, state, cs_pdu_bits, csbk_o);
    dmr_cspdu_pf0_handle_c_rand(csbk_o);
    dmr_cspdu_pf0_handle_tier2_simple(opts, state, cs_pdu_bits, csbk_o);
}

static void
dmr_cspdu_handle_pf0(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_pf, int csbk_o,
                     int csbk_fid) {
    if (csbk_pf == 0) //okay to run
    {

        //set overarching manufacturer in use when non-standard feature id set is up
        if (csbk_fid != 0) {
            state->dmr_mfid = csbk_fid;
            if (dmr_csbk_crc_checked(state)) {
                dsd_rxid_note_dmr_vendor(state, (uint8_t)csbk_fid);
            }
        }

        DSD_FPRINTF(stderr, "%s", KYEL);

        dmr_cspdu_pf0_handle_grants(opts, state, cs_pdu_bits, cs_pdu, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_move(opts, state, cs_pdu_bits, csbk_o);
        dmr_cspdu_pf0_handle_aloha(opts, state, cs_pdu_bits, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_p_clear(opts, state, cs_pdu_bits, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_p_protect(opts, state, cs_pdu_bits, csbk_o);

        dmr_cspdu_pf0_handle_c_bcast(opts, state, cs_pdu_bits, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_standard_only(opts, state, cs_pdu_bits, csbk_o, csbk_fid);

        dmr_cspdu_pf0_handle_preamble(opts, state, cs_pdu_bits, csbk_o);
        //end tier 2 csbks
    }
}

static void
dmr_cspdu_cap_plus_handle_3a(int csbk_o) {
    if (csbk_o != 0x3A) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Capacity Plus CSBK 0x3A ");
}

static void
dmr_cspdu_cap_plus_handle_3b(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    uint8_t nl[6];
    uint8_t nr[6];
    long cand[6];
    int ccount = 0;

    if (csbk_o != 0x3B) {
        return;
    }

    /* Its own site, bits 25..28, as SDRTrunk (CapacityPlusNeighbors) and OP25 (tk_trbo.py op 59) read it. */
    const uint8_t site = (uint8_t)convert_bits_into_output(&cs_pdu_bits[25], 4);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Capacity Plus Site: %d - Adjacent Sites\n  ", site);
    if (dmr_csbk_crc_checked(state)) {
        dmr_cap_plus_note_site(state, site);
        dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    }
    DSD_MEMSET(nl, 0, sizeof(nl));
    DSD_MEMSET(nr, 0, sizeof(nr));

    for (int i = 0; i < 6; i++) {
        nl[i] = (uint8_t)convert_bits_into_output(&cs_pdu_bits[32 + (i * 8)], 4);
        nr[i] = (uint8_t)convert_bits_into_output(&cs_pdu_bits[36 + (i * 8)], 4);
        if (nl[i]) {
            DSD_FPRINTF(stderr, "Site: %d Rest: %d; ", nl[i], nr[i]);
        }
    }
    for (int i = 0; i < 6; i++) {
        if (nr[i] != 0) {
            long f = state->trunk_chan_map[nr[i]];
            if (f != 0) {
                cand[ccount++] = f;
            }
        }
    }
    if (ccount > 0) {
        dmr_sm_on_neighbor_update(opts, state, cand, ccount);
    }
}

typedef struct {
    uint8_t fl;
    uint8_t ts;
    uint8_t rest_channel;
    uint8_t active_group_count;
    uint8_t bank_one;
    uint8_t bank_two;
    uint8_t ch[24];
    uint8_t pch[24];
    uint16_t group_tg[16];   // per LSN, as the status carries it: one byte for each active group LSN
    uint16_t private_tg[16]; // per LSN: one 16-bit ID for each active private/data LSN
    uint16_t decoded_targets[24];
    uint16_t t_tg[24];
    int start;
    int end;
} dmr_cap_plus_3e_ctx;

// A Cap+ rest LSN names one of the 16 logical slots; anything else announces none.
static int
dmr_cap_plus_rest_lsn_valid(int lsn) {
    return lsn >= 1 && lsn <= 16;
}

static void
dmr_cspdu_cap_plus_3e_init_ctx(dmr_cap_plus_3e_ctx* ctx, const uint8_t cs_pdu_bits[]) {
    ctx->fl = (uint8_t)convert_bits_into_output(&cs_pdu_bits[16], 2);
    ctx->ts = cs_pdu_bits[18];
    // Five bits (19..23), as the public Cap+ decoders read it: four would never reach LSN 16.
    ctx->rest_channel = (uint8_t)convert_bits_into_output(&cs_pdu_bits[19], 5);
    ctx->active_group_count = 0;
    ctx->bank_one = 0;
    ctx->bank_two = 0;
    ctx->start = 0;
    ctx->end = 16;
    DSD_MEMSET(ctx->ch, 0, sizeof(ctx->ch));
    DSD_MEMSET(ctx->pch, 0, sizeof(ctx->pch));
    DSD_MEMSET(ctx->group_tg, 0, sizeof(ctx->group_tg));
    DSD_MEMSET(ctx->private_tg, 0, sizeof(ctx->private_tg));
    DSD_MEMSET(ctx->decoded_targets, 0, sizeof(ctx->decoded_targets));
    DSD_MEMSET(ctx->t_tg, 0, sizeof(ctx->t_tg));
}

static uint8_t
dmr_cspdu_cap_plus_3e_get_block_num(dsd_state* state, uint8_t ts) {
    uint8_t block_num = state->cap_plus_block_num[ts];
    if (block_num > 6) {
        state->cap_plus_block_num[ts] = 6;
        block_num = 6;
    }
    return block_num;
}

static uint8_t
dmr_cspdu_cap_plus_3e_update_multiblock(dsd_state* state, const uint8_t cs_pdu_bits[], const dmr_cap_plus_3e_ctx* ctx,
                                        uint8_t block_num) {
    // The assembled status is only as trustworthy as its weakest fragment, so its CRC flags accumulate with it.
    const uint8_t burst_crc_invalid = state->event_crc_invalid[state->currentslot & 1];
    if (ctx->fl == 2 || ctx->fl == 3) {
        DSD_MEMSET(state->cap_plus_csbk_bits[ctx->ts], 0, sizeof(state->cap_plus_csbk_bits[ctx->ts]));
        for (int i = 0; i < 80; i++) {
            state->cap_plus_csbk_bits[ctx->ts][i] = cs_pdu_bits[i];
        }
        state->cap_plus_block_num[ctx->ts] = 0;
        state->cap_plus_csbk_crc_invalid[ctx->ts] = burst_crc_invalid;
        return 0;
    }

    for (int i = 0; i < 56; i++) {
        state->cap_plus_csbk_bits[ctx->ts][i + 80 + (56 * block_num)] = cs_pdu_bits[i + 24];
    }
    state->cap_plus_block_num[ctx->ts]++;
    state->cap_plus_csbk_crc_invalid[ctx->ts] |= burst_crc_invalid;
    return (uint8_t)(block_num + 1);
}

static void
dmr_cspdu_cap_plus_3e_sync_rest(const dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx) {
    if (!dmr_cap_plus_rest_lsn_valid(ctx->rest_channel)) {
        return;
    }
    // Before the change test: an unchanged repeat proves the rest channel again.
    if (dmr_csbk_crc_checked(state)) {
        dsd_rxid_note_dmr_rest_lsn(state, ctx->rest_channel);
        dsd_rxid_note_dmr_brand(state, "Capacity Plus");
    }
    if (ctx->rest_channel != state->dmr_rest_channel) {
        state->dmr_rest_channel = ctx->rest_channel;
    }
    if (state->trunk_chan_map[ctx->rest_channel] != 0) {
        dmr_sm_note_cc_activity(opts, state, state->trunk_chan_map[ctx->rest_channel]);
    }
}

static void
dmr_cspdu_cap_plus_3e_print_header(const dmr_cap_plus_3e_ctx* ctx) {
    DSD_FPRINTF(stderr, " Capacity Plus Channel Status - FL: %d TS: %d - Rest LSN: %d", ctx->fl, ctx->ts,
                ctx->rest_channel);
    if (ctx->fl == 0) {
        DSD_FPRINTF(stderr, " - Appended Block");
    } else if (ctx->fl == 1) {
        DSD_FPRINTF(stderr, " - Final Block");
    } else if (ctx->fl == 2) {
        DSD_FPRINTF(stderr, " - Initial Block");
    } else if (ctx->fl == 3) {
        DSD_FPRINTF(stderr, " - Single Block");
    }
}

static void
dmr_cspdu_cap_plus_3e_parse_group_banks(dsd_state* state, dmr_cap_plus_3e_ctx* ctx) {
    uint8_t b2_start = 0;

    ctx->bank_one = (uint8_t)convert_bits_into_output(&state->cap_plus_csbk_bits[ctx->ts][24], 8);
    for (int i = 0; i < 8; i++) {
        ctx->ch[i] = state->cap_plus_csbk_bits[ctx->ts][i + 24];
        if (ctx->ch[i] == 1) {
            ctx->group_tg[i] = (uint16_t)convert_bits_into_output(
                &state->cap_plus_csbk_bits[ctx->ts][32 + (ctx->active_group_count * 8)], 8);
            ctx->active_group_count++;
        }
    }

    ctx->bank_two =
        (uint8_t)convert_bits_into_output(&state->cap_plus_csbk_bits[ctx->ts][32 + (ctx->active_group_count * 8)], 8);
    b2_start = ctx->active_group_count;
    if (ctx->bank_two == 0) {
        return;
    }

    for (int i = 0; i < 8; i++) {
        ctx->ch[i + 8] = state->cap_plus_csbk_bits[ctx->ts][i + 32 + (b2_start * 8)];
        if (ctx->ch[i + 8] == 1) {
            // Bank two's bitmap byte sits between the two banks' talkgroups.
            ctx->group_tg[i + 8] = (uint16_t)convert_bits_into_output(
                &state->cap_plus_csbk_bits[ctx->ts][40 + (ctx->active_group_count * 8)], 8);
            ctx->active_group_count++;
        }
    }
}

static int
dmr_cspdu_cap_plus_3e_parse_private_bank_one(dsd_state* state, dmr_cap_plus_3e_ctx* ctx) {
    int k = 0;
    uint8_t pdflag =
        (uint8_t)convert_bits_into_output(&state->cap_plus_csbk_bits[ctx->ts][40 + (ctx->active_group_count * 8)], 8);
    if (pdflag == 0) {
        return 0;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Bank One F%X Private or Data Call(s) - ", pdflag);
    for (int i = 0; i < 8; i++) {
        ctx->pch[i] = state->cap_plus_csbk_bits[ctx->ts][i + 48 + (ctx->active_group_count * 8)];
        if (ctx->pch[i] != 1) {
            continue;
        }
        DSD_FPRINTF(stderr, " LSN %02d:", i + 1);
        uint16_t private_target = (uint16_t)convert_bits_into_output(
            &state->cap_plus_csbk_bits[ctx->ts][56 + (k * 16) + (ctx->active_group_count * 8)], 16);
        ctx->private_tg[i] = private_target;
        DSD_FPRINTF(stderr, " TGT %d;", private_target);
        k++;
        if (ctx->bank_one == 0) {
            ctx->bank_one = 0xFF;
        }
    }
    return k;
}

static void
dmr_cspdu_cap_plus_3e_parse_private_bank_two(dsd_state* state, dmr_cap_plus_3e_ctx* ctx, int pd_b2) {
    uint8_t pdflag2 = (uint8_t)convert_bits_into_output(
        &state->cap_plus_csbk_bits[ctx->ts][56 + (ctx->active_group_count * 8) + (pd_b2 * 16)], 8);
    int k = 0;

    if (pdflag2 == 0) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " Bank Two F%02X Private or Data Call(s) - ", pdflag2);
    for (int i = 0; i < 8; i++) {
        ctx->pch[i + 8] = state->cap_plus_csbk_bits[ctx->ts][i + 64 + (ctx->active_group_count * 8) + (pd_b2 * 16)];
        if (ctx->pch[i + 8] != 1) {
            continue;
        }
        DSD_FPRINTF(stderr, " LSN %02d:", i + 9);
        // Laid out as bank one's: the flag byte, the bitmap byte, then one 16-bit ID per active LSN.
        uint16_t private_target = (uint16_t)convert_bits_into_output(
            &state->cap_plus_csbk_bits[ctx->ts][72 + (k * 16) + (ctx->active_group_count * 8) + (pd_b2 * 16)], 16);
        ctx->private_tg[i + 8] = private_target;
        DSD_FPRINTF(stderr, " TGT %d;", private_target);
        k++;
        if (ctx->bank_two == 0) {
            ctx->bank_two = 0xFF;
        }
    }
}

static int
dmr_cspdu_cap_plus_3e_calc_start(const dmr_cap_plus_3e_ctx* ctx) {
    if ((ctx->bank_one & 0xF0) != 0 || ctx->rest_channel < 5) {
        return 0;
    }
    if ((ctx->bank_one & 0x0F) != 0 || ctx->rest_channel < 9) {
        return 4;
    }
    if ((ctx->bank_two & 0xF0) != 0 || ctx->rest_channel < 13) {
        return 8;
    }
    return 12;
}

static int
dmr_cspdu_cap_plus_3e_calc_end(const dmr_cap_plus_3e_ctx* ctx) {
    if ((ctx->bank_two & 0x0F) != 0 || ctx->rest_channel > 12) {
        return 16;
    }
    if ((ctx->bank_two & 0xF0) != 0 || ctx->rest_channel > 9) {
        return 12;
    }
    if ((ctx->bank_one & 0x0F) != 0 || (ctx->rest_channel > 4 && ctx->rest_channel < 9)) {
        return 8;
    }
    if ((ctx->bank_one & 0xF0) != 0 || ctx->rest_channel < 5) {
        return 4;
    }
    return 16;
}

static void
dmr_cspdu_cap_plus_3e_calc_window(dmr_cap_plus_3e_ctx* ctx) {
    const int b1_hi = (ctx->bank_one & 0xF0) != 0;
    const int b1_lo = (ctx->bank_one & 0x0F) != 0;
    const int b2_hi = (ctx->bank_two & 0xF0) != 0;
    const int b2_lo = (ctx->bank_two & 0x0F) != 0;

    if (b1_hi || b1_lo || b2_hi || b2_lo) {
        ctx->start = dmr_cspdu_cap_plus_3e_calc_start(ctx);
        ctx->end = dmr_cspdu_cap_plus_3e_calc_end(ctx);
        return;
    }
    ctx->start = dmr_cspdu_cap_plus_3e_calc_start(ctx);
    ctx->end = dmr_cspdu_cap_plus_3e_calc_end(ctx);
}

static void
dmr_cspdu_cap_plus_3e_emit_row_breaks(int start, int i) {
    if (start < 1 && i == 4) {
        DSD_FPRINTF(stderr, "\n  ");
    }
    if (start < 5 && i == 8) {
        DSD_FPRINTF(stderr, "\n  ");
    }
    if (start < 9 && i == 12) {
        DSD_FPRINTF(stderr, "\n  ");
    }
}

static void
dmr_cspdu_cap_plus_3e_render_slot(const dsd_opts* opts, dmr_cap_plus_3e_ctx* ctx, int i,
                                  char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE]) {
    char cap_active[20];
    // Read from the parsed banks rather than counted here: which LSNs the window shows must not move the offsets.
    if (ctx->ch[i] == 1) {
        uint16_t tg = ctx->group_tg[i];
        DSD_FPRINTF(stderr, tg ? "%5d;  " : "Group;  ", tg);
        ctx->decoded_targets[i] = tg;
        if (opts->trunk_tune_group_calls == 1) {
            ctx->t_tg[i] = tg;
        }
        DSD_SNPRINTF(cap_active, sizeof(cap_active), "LSN:%d TG:%d; ", i + 1, tg);
        dsd_append(notice, DSD_RECENT_ACTIVITY_TEXT_SIZE, cap_active);
        return;
    }

    if (ctx->pch[i] == 1) {
        uint16_t tg = ctx->private_tg[i];
        DSD_FPRINTF(stderr, tg ? "%5d;  " : " P||D;  ", tg);
        ctx->decoded_targets[i] = tg;
        if (opts->trunk_tune_private_calls == 1) {
            ctx->t_tg[i] = tg;
        }
        if (opts->trunk_tune_private_calls == 1) {
            DSD_SNPRINTF(cap_active, sizeof(cap_active), "LSN:%d PC:%d; ", i + 1, tg);
            dsd_append(notice, DSD_RECENT_ACTIVITY_TEXT_SIZE, cap_active);
        }
        return;
    }

    DSD_FPRINTF(stderr, i + 1 == ctx->rest_channel ? " Rest;  " : " Idle;  ");
}

static void
dmr_cspdu_cap_plus_3e_render_activity(const dsd_opts* opts, dsd_state* state, dmr_cap_plus_3e_ctx* ctx) {
    char notices[17][DSD_RECENT_ACTIVITY_TEXT_SIZE] = {{0}};

    DSD_FPRINTF(stderr, "\n  ");
    (void)dsd_recent_activity_clear_all(state);
    DSD_SNPRINTF(notices[0], sizeof notices[0], "Cap+ ");
    dmr_csbk_publish_activity(state, 0U, 0U, DSD_CALL_KIND_DATA, 0U, 0U, 0U, 0, 0, notices[0]);
    dmr_cspdu_cap_plus_3e_calc_window(ctx);

    for (int i = ctx->start; i < ctx->end; i++) {
        dmr_cspdu_cap_plus_3e_emit_row_breaks(ctx->start, i);
        DSD_FPRINTF(stderr, "LSN %02d: ", i + 1);
        dmr_cspdu_cap_plus_3e_render_slot(opts, ctx, i, notices[i + 1]);
    }
    for (int i = 1; i <= 16; i++) {
        if (notices[i][0] != '\0') {
            const dsd_call_kind kind = ctx->pch[i - 1] == 1 ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE;
            const uint8_t slot = (uint8_t)((i - 1) & 1);
            dmr_csbk_publish_activity(state, (uint8_t)i, slot, kind, ctx->decoded_targets[i - 1], 0U, (uint16_t)i,
                                      state->trunk_chan_map[i], 0, notices[i]);
        }
    }
}

static void
dmr_cspdu_cap_plus_3e_set_branding(dsd_state* state) {
    state->dmr_mfid = 0x10;
    DSD_SNPRINTF(state->dmr_branding, sizeof(state->dmr_branding), "%s", "Motorola");
    DSD_SNPRINTF(state->dmr_branding_sub, sizeof(state->dmr_branding_sub), "%s", "Cap+ ");
    DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "%s", "");
}

// Hand the trunk SM the group each slot of the followed carrier is carrying, as this status names it. Cap+ follows a
// call already in progress, so its voice LC header has usually gone by; this is what names the call when no LC on
// the voice channel decodes. Group calls only: a private ID's position past bank one has no public layout.
static void
dmr_cspdu_cap_plus_3e_seed_carrier(const dsd_opts* opts, const dsd_state* state, const dmr_cap_plus_3e_ctx* ctx,
                                   long freq, uint64_t generation_before, int only_unarmed) {
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    for (int slot = 0; slot < 2; slot++) {
        if (only_unarmed && sm->capplus_seed[slot].armed) {
            continue;
        }
        uint32_t tg = 0U;
        int lsn = 0;
        for (int j = slot; j < 16; j += 2) {
            if (state->trunk_chan_map[j + 1] == freq && ctx->ch[j] == 1 && ctx->group_tg[j] != 0U) {
                tg = ctx->group_tg[j];
                lsn = j + 1;
                break;
            }
        }
        dmr_sm_capplus_seed_arm(sm, state, slot, tg, lsn, freq, opts->rtlsdr_center_freq,
                                state->cap_plus_csbk_crc_invalid[ctx->ts], generation_before);
    }
}

/* The LSN of @p slot of the carrier the machine follows: each Cap+ frequency carries two LSNs, LSN n on slot (n - 1) & 1,
   so the channel map names it whatever the slot's identity is now (one that expired with its call keeps none). Else
   the LSN the slot's record was named on. 0: none. */
static int
dmr_cspdu_cap_plus_slot_lsn(const dsd_state* state, const dmr_sm_ctx_t* sm, int slot) {
    if (sm->state == DMR_SM_TUNED && sm->vc_freq_hz > 0) {
        for (int j = slot; j < 16; j += 2) {
            if (state->trunk_chan_map[j + 1] == sm->vc_freq_hz) {
                return j + 1;
            }
        }
    }
    return sm->slots[slot].channel;
}

// A later status that shows a seeded LSN idle or carrying another call means the call the seed named is gone.
static void
dmr_cspdu_cap_plus_3e_reconcile_seeds(const dsd_state* state, const dmr_cap_plus_3e_ctx* ctx) {
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    for (int slot = 0; slot < 2; slot++) {
        const int lsn = sm->capplus_seed[slot].lsn;
        if (!sm->capplus_seed[slot].armed || lsn < 1 || lsn > 16) {
            continue;
        }
        const uint32_t tg = (ctx->ch[lsn - 1] == 1) ? ctx->group_tg[lsn - 1] : 0U;
        dmr_sm_capplus_seed_reconcile(sm, slot, lsn, tg);
    }
    /* The followed slots' identities a seed gave (not a checked LC naming the call still on the slot) take the group the
       status now names on their LSN, so a preemption candidate is compared with the call actually on the slot; a slot
       whose identity expired with its call takes the group the status names for its next one. */
    for (int slot = 0; slot < 2; slot++) {
        const int lsn = dmr_cspdu_cap_plus_slot_lsn(state, sm, slot);
        if (lsn >= 1 && lsn <= 16) {
            /* A private or data call on the LSN is a call of unknown identity, not an idle LSN. */
            const int group = ctx->ch[lsn - 1] == 1;
            dmr_sm_status_identity_reconcile(sm, state, slot, lsn, group ? ctx->group_tg[lsn - 1] : 0U,
                                             group || ctx->pch[lsn - 1] == 1,
                                             state->cap_plus_csbk_crc_invalid[ctx->ts]);
        }
    }
}

/* Whether a grant this module tunes itself may go to @p freq_hz: not to a voice channel whose verified colour code the
   receive policy rejects (issue #637), as the trunk state machine's own grants (dmr_sm_resolve_tunable_grant()). */
static int
dmr_csbk_receive_carrier_allowed(const dsd_opts* opts, long freq_hz) {
    const dmr_sm_ctx_t* ctx = dmr_sm_get_ctx();
    return !ctx || !dsd_receive_carriers_rejects(&ctx->receive_carriers, opts, freq_hz);
}

/* Whether status candidate @p a outranks @p b: Hold first, then priority. The caller walks the channels in order and
   keeps the first of equals, so with no priorities configured the first eligible channel is followed, as before. */
static int
dmr_status_candidate_outranks(const dsd_tg_policy_decision* a, const dsd_tg_policy_decision* b) {
    if (a->tg_hold_match != b->tg_hold_match) {
        return a->tg_hold_match > b->tg_hold_match;
    }
    return a->priority > b->priority;
}

/* Follow the status's LSN @p j (one tune per status): a carrier the machine already follows is only reseeded. */
static void
dmr_cspdu_cap_plus_3e_follow(dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx, int j,
                             const dsd_tg_policy_decision* decision, dmr_sm_displace_e how) {
    const long int grant_freq = state->trunk_chan_map[j + 1];
    if (dmr_sm_follows(dmr_sm_get_ctx(), grant_freq)) {
        /* The carrier followed already satisfies this status: no retune, and its deadlines stand. A slot whose seed
           reconciliation dropped (another group now) is named anew, for the carrier as it is now tuned. */
        dmr_cspdu_cap_plus_3e_seed_carrier(opts, state, ctx, grant_freq, dsd_trunk_tuning_generation() - 1U, 1);
        return;
    }
    const uint32_t old_rtl_center_freq = opts->rtlsdr_center_freq;
    const uint64_t generation_before = dsd_trunk_tuning_generation();
    const long receiver_before = dmr_sm_receiver_frequency(opts); // whether the follow leaves the carrier it is on
    uint64_t request_id = 0U;
    dsd_trunk_tune_result tune_result = dmr_sm_tune_to_freq(opts, state, grant_freq, &request_id);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        return;
    }
    dmr_sm_note_status_follow(opts, state, grant_freq, j & 1, receiver_before, tune_result, request_id);
    dmr_cspdu_cap_plus_3e_seed_carrier(opts, state, ctx, grant_freq, generation_before, 0);
    const dsd_tg_policy_call_route route = {
        .target_id = ctx->t_tg[j],
        .source_id = 0U,
        .freq_hz = grant_freq,
        .channel = j + 1,
        .slot = j & 1,
        .requires_tuner_retune = 0,
    };
    dmr_sm_policy_note_follow(state, &route, decision, how == DMR_SM_DISPLACE_PREEMPT, dsd_decode_now_mono_s());
    if (old_rtl_center_freq != (uint32_t)grant_freq) {
        dmr_reset_blocks(opts, state);
    }
}

/* The receiver's held calls as a status sees them (@p view: the state machine's busy view while trunking, else none),
   and whether it holds a call: that view's, or the status's own two-second voice window (@p caller_busy). */
static int
dmr_csbk_status_busy(const dsd_opts* opts, dsd_state* state, int trunking, int caller_busy, double now_m,
                     dmr_sm_busy_view_t* view) {
    DSD_MEMSET(view, 0, sizeof(*view));
    if (trunking) {
        (void)dmr_sm_describe_held_calls(dmr_sm_get_ctx(), opts, state, now_m, view);
    }
    return view->held || caller_busy;
}

/* The carrier of the status's LSN @p j when it may be followed at all (a named group, trunking, the receive filters and
   the talkgroup policy, whose verdict lands in @p decision), else 0. */
static long int
dmr_cspdu_cap_plus_3e_lsn_freq(const dsd_opts* opts, const dsd_state* state, const dmr_cap_plus_3e_ctx* ctx, int j,
                               int trunking, dsd_tg_policy_decision* decision) {
    const int is_group_call = (ctx->pch[j] == 1) ? 0 : 1;
    int policy_allowed = 0;

    dmr_csbk_print_target_label(state, is_group_call ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE,
                                (uint32_t)ctx->t_tg[j]);
    policy_allowed = dmr_policy_tune_allowed(opts, state, ctx->t_tg[j], 0, is_group_call, 0, decision);
    if (ctx->t_tg[j] == 0 || !trunking || !dsd_receive_grant_allows(opts, DSD_SYNC_DMR_BS_VOICE_POS, j & 1)) {
        return 0;
    }
    if (!policy_allowed) {
        dmr_policy_log_block(opts, is_group_call, ctx->t_tg[j], 0, decision);
        return 0;
    }
    const long int grant_freq = state->trunk_chan_map[j + 1];
    if (grant_freq == 0 || !dmr_csbk_receive_carrier_allowed(opts, grant_freq)) {
        return 0;
    }
    return grant_freq;
}

/* Pick the one LSN this status follows. While the receiver holds a call (the state machine's busy view, or the
   status's own two-second voice window), an LSN on another carrier is eligible only by Hold over a non-held call or by
   preemption, and one on the held carrier is never a displacement; otherwise every allowed LSN is. Among the eligible,
   Hold, then priority, then channel order decides. */
static void
dmr_cspdu_cap_plus_3e_try_tune_grants(dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx) {
    const int trunking = state->trunk_cc_freq != 0 && opts->trunk_enable == 1;
    const double now_m = dsd_decode_now_mono_s();
    const int caller_busy = (dsd_decode_time() - state->last_vc_sync_time) <= 2;
    dmr_sm_busy_view_t view;
    const int busy = dmr_csbk_status_busy(opts, state, trunking, caller_busy, now_m, &view);

    int best = -1;
    int own = -1;
    dsd_tg_policy_decision best_decision;
    dsd_tg_policy_decision own_decision;
    dmr_sm_displace_e best_how = DMR_SM_DISPLACE_NONE;
    DSD_MEMSET(&best_decision, 0, sizeof(best_decision));
    DSD_MEMSET(&own_decision, 0, sizeof(own_decision));
    for (int j = ctx->start; j < ctx->end; j++) {
        dsd_tg_policy_decision policy_decision;
        const long int grant_freq = dmr_cspdu_cap_plus_3e_lsn_freq(opts, state, ctx, j, trunking, &policy_decision);
        if (grant_freq == 0) {
            continue;
        }
        dmr_sm_displace_e how = DMR_SM_DISPLACE_NONE;
        if (busy && grant_freq == view.carrier_hz) {
            if (own < 0) {
                own = j;
                own_decision = policy_decision;
            }
            continue;
        }
        if (busy) {
            const dsd_tg_policy_call_route candidate = {
                .target_id = ctx->t_tg[j],
                .source_id = 0U,
                .freq_hz = grant_freq,
                .channel = j + 1,
                .slot = j & 1,
                .requires_tuner_retune = 1,
            };
            how = dmr_sm_may_displace(&view, opts, state, &candidate, &policy_decision, 1, 1, now_m);
            if (how == DMR_SM_DISPLACE_NONE) {
                continue;
            }
        }
        if (best < 0 || dmr_status_candidate_outranks(&policy_decision, &best_decision)) {
            best = j;
            best_decision = policy_decision;
            best_how = how;
        }
    }
    if (best >= 0) {
        dmr_cspdu_cap_plus_3e_follow(opts, state, ctx, best, &best_decision, best_how);
        return;
    }
    /* Nothing displaces what is held. Outside the status's voice window (which leaves the carrier alone, as it always
       did) a status naming the held carrier reseeds a followed one, and a call heard where the receiver is parked is
       followed in place, as a tune to that frequency did. */
    if (own >= 0 && !caller_busy) {
        dmr_cspdu_cap_plus_3e_follow(opts, state, ctx, own, &own_decision, DMR_SM_DISPLACE_NONE);
    }
}

static void
dmr_cspdu_cap_plus_3e_dump_payload(const dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx,
                                   uint8_t block_num) {
    if (ctx->fl != 1 || opts->payload != 1) {
        return;
    }
    DSD_FPRINTF(stderr, "%s\n", KYEL);
    DSD_FPRINTF(stderr, " CAP+ Multi Block PDU \n  ");
    for (int i = 0; i < (10 + (block_num * 7)); i++) {
        uint8_t fl_bytes = (uint8_t)convert_bits_into_output(&state->cap_plus_csbk_bits[ctx->ts][((size_t)i * 8)], 8);
        DSD_FPRINTF(stderr, "[%02X]", fl_bytes);
        if (i == 17 || i == 35) {
            DSD_FPRINTF(stderr, "\n  ");
        }
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
dmr_cspdu_cap_plus_3e_try_return_to_rest(dsd_opts* opts, dsd_state* state, const dmr_cap_plus_3e_ctx* ctx) {
    if (!dmr_cap_plus_rest_lsn_valid(ctx->rest_channel)) {
        return;
    }
    const long rest = state->trunk_chan_map[ctx->rest_channel];
    if (opts->trunk_enable != 1 || rest <= 0) {
        return;
    }
    if (opts->trunk_is_tuned == 1) {
        /* Save the announced return destination without cutting short voice
         * hangtime when the other slot reports empty busy banks. */
        state->trunk_cc_freq = rest;
        return;
    }
    if (dmr_sm_receiver_on(opts, rest)) {
        return; // the receiver is there, or a tracked tune is taking it there; the saved alias alone does not say so
    }
    /* Busy status can announce a new rest channel even when every advertised
     * call is blocked or unmapped. Follow it unless a call owns the tuner. */
    (void)dmr_sm_return_to_cc(dmr_sm_get_ctx(), opts, state, rest);
}

static void
dmr_cspdu_cap_plus_handle_3e(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    dmr_cap_plus_3e_ctx ctx;
    uint8_t block_num;

    if (csbk_o != 0x3E) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);

    dmr_cspdu_cap_plus_3e_init_ctx(&ctx, cs_pdu_bits);
    block_num = dmr_cspdu_cap_plus_3e_get_block_num(state, ctx.ts);
    block_num = dmr_cspdu_cap_plus_3e_update_multiblock(state, cs_pdu_bits, &ctx, block_num);
    dmr_cspdu_cap_plus_3e_sync_rest(opts, state, &ctx);
    dmr_cspdu_cap_plus_3e_print_header(&ctx);
    dmr_cspdu_cap_plus_3e_parse_group_banks(state, &ctx);

    if (!(ctx.fl == 1 || ctx.fl == 3)) {
        return;
    }

    int pd_b2 = dmr_cspdu_cap_plus_3e_parse_private_bank_one(state, &ctx);
    dmr_cspdu_cap_plus_3e_parse_private_bank_two(state, &ctx, pd_b2);
    dmr_cspdu_cap_plus_3e_render_activity(opts, state, &ctx);
    dmr_cspdu_cap_plus_3e_reconcile_seeds(state, &ctx);

    dmr_cspdu_cap_plus_3e_set_branding(state);
    DSD_FPRINTF(stderr, "%s", KNRM);

    /* An allow-list or a Hold is admission, never a reason to wipe the voice-sync stamp: a listed or held group on
       another carrier takes the receiver only by Hold or preemption (dmr_cspdu_cap_plus_3e_try_tune_grants()). */
    if ((dsd_decode_time() - state->last_vc_sync_time) > 2) {
        rotate_symbol_out_file(opts, state);
    }

    dmr_cspdu_cap_plus_3e_try_tune_grants(opts, state, &ctx);
    dmr_cspdu_cap_plus_3e_dump_payload(opts, state, &ctx, block_num);
    DSD_MEMSET(state->cap_plus_csbk_bits[ctx.ts], 0, sizeof(state->cap_plus_csbk_bits[ctx.ts]));
    state->cap_plus_block_num[ctx.ts] = 0;
    state->cap_plus_csbk_crc_invalid[ctx.ts] = 0U;
    dmr_cspdu_cap_plus_3e_try_return_to_rest(opts, state, &ctx);
}

static void
dmr_cspdu_handle_cap_plus(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o,
                          int csbk_fid) {
    UNUSED(cs_pdu);
    if (csbk_fid != 0x10) {
        return;
    }

    dmr_cspdu_cap_plus_handle_3a(csbk_o);
    dmr_cspdu_cap_plus_handle_3b(opts, state, cs_pdu_bits, csbk_o);
    dmr_cspdu_cap_plus_handle_3e(opts, state, cs_pdu_bits, csbk_o);
}

static void
dmr_cspdu_con_plus_set_branding(dsd_state* state) {
    state->dmr_mfid = 0x06;
    DSD_SNPRINTF(state->dmr_branding, sizeof(state->dmr_branding), "%s", "Motorola");
    DSD_SNPRINTF(state->dmr_branding_sub, sizeof(state->dmr_branding_sub), "Con+ ");
    dmr_csbk_note_brand(state, "Connect Plus");
}

static void
dmr_cspdu_con_plus_handle_adjacent(dsd_state* state, const uint8_t cs_pdu[], int csbk_o) {
    uint8_t nb[5];

    if (csbk_o != 0x01) {
        return;
    }

    nb[0] = cs_pdu[2] & 0x3F;
    nb[1] = cs_pdu[3] & 0x3F;
    nb[2] = cs_pdu[4] & 0x3F;
    nb[3] = cs_pdu[5] & 0x3F;
    nb[4] = cs_pdu[6] & 0x3F;

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Connect Plus Adjacent Sites:");
    for (int i = 0; i < 5; i++) {
        if (nb[i] != 0) {
            DSD_FPRINTF(stderr, " %d;", nb[i]);
        }
    }
    if (nb[0] == 0) {
        DSD_FPRINTF(stderr, " None Listed;");
    }
    dmr_cspdu_con_plus_set_branding(state);
}

typedef struct {
    uint32_t src_addr;
    uint32_t grp_addr;
    uint8_t lcn;
    uint8_t tslot;
    uint8_t opt;
} dmr_con_plus_voice_grant;

static void
dmr_cspdu_con_plus_print_voice_grant(const dmr_con_plus_voice_grant* g) {
    DSD_FPRINTF(stderr, "%s", KYEL);
    if (g->opt == 2) {
        DSD_FPRINTF(stderr, " Connect Plus Group Voice Channel Grant;");
    } else if (g->opt == 3) {
        DSD_FPRINTF(stderr, " Connect Plus Private Voice Channel Grant;");
    } else {
        DSD_FPRINTF(stderr, " Connect Plus Unknown %02X Channel Grant;", g->opt);
    }
    DSD_FPRINTF(stderr, " Target: %d; Source: %d; LCN: %d; TS: %d;", g->grp_addr, g->src_addr, g->lcn, g->tslot + 1);
}

static int
dmr_cspdu_con_plus_skip_voice_tune(const dsd_opts* opts, uint8_t opt) {
    if (opts->trunk_tune_group_calls == 0 && opt == 2) {
        return 1;
    }
    if (opts->trunk_tune_private_calls == 0 && opt == 3) {
        return 1;
    }
    return opts->trunk_tune_group_calls == 0 && opt != 2 && opt != 3;
}

/* A Connect Plus voice grant. Its policy is judged before the voice window, which still refuses an ordinary grant while
   voice was heard within 2 s (4 s with data calls followed), as it always did; a Hold or preempt grant goes to the trunk
   state machine with that window as its busy hint, where it is followed only by Hold over a non-held call or by
   preemption over every call the receiver holds. Group tuning off still refuses every grant, private ones included. */
static void
dmr_cspdu_con_plus_try_tune_voice(dsd_opts* opts, dsd_state* state, const dmr_con_plus_voice_grant* g) {
    dsd_tg_policy_decision policy_decision;
    int policy_allowed;
    long f;

    if (opts->trunk_tune_group_calls != 1) {
        return;
    }
    const int is_group = (g->opt == 3) ? 0 : 1;
    policy_allowed = dmr_policy_tune_allowed(opts, state, g->grp_addr, g->src_addr, is_group, 0, &policy_decision);
    if (state->trunk_cc_freq == 0 || opts->trunk_enable != 1) {
        return;
    }
    if (!policy_allowed) {
        dmr_policy_log_block(opts, is_group, g->grp_addr, g->src_addr, &policy_decision);
        return;
    }
    const int window = dsd_decode_time() - state->last_vc_sync_time <= (opts->trunk_tune_data_calls == 1 ? 4 : 2);
    if (window && !policy_decision.tg_hold_match && !policy_decision.preempt_requested) {
        return;
    }

    f = state->trunk_chan_map[g->lcn];
    if (f == 0) {
        return;
    }

    dmr_sm_event_t ev = is_group ? dmr_sm_ev_group_grant(f, g->lcn, (int)g->grp_addr, (int)g->src_addr)
                                 : dmr_sm_ev_indiv_grant(f, g->lcn, (int)g->grp_addr, (int)g->src_addr);
    ev.slot = g->tslot;
    ev.channel_kind = g->lcn != 0 ? (uint8_t)DSD_CALL_CHANNEL_LCN : (uint8_t)DSD_CALL_CHANNEL_NONE;
    ev.origin = DMR_SM_ORIGIN_GRANT;
    ev.caller_busy = window;
    ev.policy_valid = 1;
    ev.policy = policy_decision;
    const dmr_sm_grant_result_e result = dmr_sm_submit_grant(opts, state, &ev);
    if (result == DMR_SM_GRANT_FOLLOWED || result == DMR_SM_GRANT_IN_PLACE) {
        state->is_con_plus = 1;
    }
}

static void
dmr_cspdu_con_plus_handle_voice(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu[], int csbk_o) {
    dmr_con_plus_voice_grant g;
    char suf[24];

    if (csbk_o != 0x03) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    g.src_addr = ((cs_pdu[2] << 16) + (cs_pdu[3] << 8) + cs_pdu[4]);
    g.grp_addr = ((cs_pdu[5] << 16) + (cs_pdu[6] << 8) + cs_pdu[7]);
    g.lcn = ((cs_pdu[8] & 0xF0) >> 4);
    g.tslot = ((cs_pdu[8] & 0x08) >> 3) & 1;
    g.opt = cs_pdu[9];
    dmr_cspdu_con_plus_print_voice_grant(&g);
    dmr_cspdu_con_plus_set_branding(state);

    dmr_format_chan_suffix(g.tslot, suf, sizeof suf);
    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    DSD_SNPRINTF(notice, sizeof notice, "Active Ch: %04X%s TG: %d; ", g.lcn, suf, g.grp_addr);
    const dsd_call_kind kind = g.opt == 3 ? DSD_CALL_KIND_PRIVATE_VOICE : DSD_CALL_KIND_GROUP_VOICE;
    dmr_csbk_publish_activity(state, g.tslot, g.tslot, kind, (uint64_t)g.grp_addr, (uint64_t)g.src_addr,
                              (uint16_t)g.lcn, state->trunk_chan_map[g.lcn], 0, notice);

    if (opts->trunk_enable == 0 && state->trunk_chan_map[g.lcn] != 0) {
        state->trunk_vc_freq[0] = state->trunk_chan_map[g.lcn];
        state->trunk_vc_freq[1] = state->trunk_chan_map[g.lcn];
    }

    dmr_csbk_print_target_label(state, kind, g.grp_addr);
    if (dmr_cspdu_con_plus_skip_voice_tune(opts, g.opt)) {
        return;
    }
    dmr_cspdu_con_plus_try_tune_voice(opts, state, &g);
}

/* Follow a Connect Plus data grant the talkgroup policy allows, unless the receive policy rejects its slot or the
   colour code verified on its channel (issue #637). Returns 0 only when the tune was refused. */
static int
dmr_cspdu_con_plus_tune_data(dsd_opts* opts, dsd_state* state, uint8_t lcn, uint8_t tslot) {
    const long freq = state->trunk_chan_map[lcn];
    if (freq == 0 || !dsd_receive_grant_allows(opts, DSD_SYNC_DMR_BS_DATA_POS, tslot)
        || !dmr_csbk_receive_carrier_allowed(opts, freq)) {
        return 1;
    }
    const long receiver = dmr_sm_receiver_frequency(opts);
    uint64_t request_id = 0U;
    const dsd_trunk_tune_result tune_result = dmr_sm_tune_to_freq(opts, state, freq, &request_id);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        return 0;
    }
    /* A tune still in flight is judged once it lands, and one that fails takes nothing from the calls it left. */
    dmr_sm_note_tracked_carrier_tune(dmr_sm_get_ctx(), state, tune_result, request_id, receiver, freq);
    state->is_con_plus = 1;
    dmr_reset_blocks(opts, state);
    return 1;
}

static void
dmr_cspdu_con_plus_handle_data(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu[], int csbk_o) {
    uint32_t dtarget;
    uint8_t lcn;
    uint8_t tslot;
    dsd_tg_policy_decision policy_decision;
    int policy_allowed;
    char suf[24];
    dsd_recent_activity_transaction activity_transaction;
    time_t now;

    if (csbk_o != 0x06) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    dtarget = ((cs_pdu[2] << 16) + (cs_pdu[3] << 8) + cs_pdu[4]);
    lcn = ((cs_pdu[5] & 0xF0) >> 4);
    tslot = ((cs_pdu[5] & 0x08) >> 3) & 1;
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Connect Plus Data Channel Grant;");
    DSD_FPRINTF(stderr, " Target: %d; LCN: %d; TS: %d;", dtarget, lcn, tslot + 1);
    dmr_cspdu_con_plus_set_branding(state);

    dmr_format_chan_suffix(tslot, suf, sizeof suf);
    (void)dsd_recent_activity_save(state, tslot, &activity_transaction);
    now = dsd_decode_time();
    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE];
    DSD_SNPRINTF(notice, sizeof notice, "Active Ch: %04X%s TG: %d; ", lcn, suf, dtarget);
    dmr_csbk_publish_activity(state, tslot, tslot, DSD_CALL_KIND_DATA, dtarget, 0U, lcn, state->trunk_chan_map[lcn], 0,
                              notice);
    if (opts->trunk_tune_data_calls == 0) {
        return;
    }
    if (opts->trunk_enable == 0 && state->trunk_chan_map[lcn] != 0) {
        state->trunk_vc_freq[0] = state->trunk_chan_map[lcn];
        state->trunk_vc_freq[1] = state->trunk_chan_map[lcn];
    }

    dmr_csbk_print_target_label(state, DSD_CALL_KIND_DATA, dtarget);
    if (opts->trunk_tune_data_calls != 1 || (now - state->last_vc_sync_time <= 2)) {
        return;
    }

    policy_allowed = dmr_policy_tune_allowed(opts, state, dtarget, 0, 0, 1, &policy_decision);
    if (state->trunk_cc_freq != 0 && opts->trunk_enable == 1 && policy_allowed) {
        if (!dmr_cspdu_con_plus_tune_data(opts, state, lcn, tslot)) {
            (void)dsd_recent_activity_restore(state, &activity_transaction);
        }
    } else if (state->trunk_cc_freq != 0 && opts->trunk_enable == 1) {
        dmr_policy_log_block(opts, 0, dtarget, 0, &policy_decision);
    }
}

static void
dmr_cspdu_con_plus_handle_termination(dsd_opts* opts, dsd_state* state, const uint8_t cs_pdu[], int csbk_o) {
    uint32_t ttarget;

    if (csbk_o != 0x0C) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    ttarget = ((cs_pdu[2] << 16) + (cs_pdu[3] << 8) + cs_pdu[4]);
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, " Connect Plus Slot Termination;");
    DSD_FPRINTF(stderr, " Target: %d;", ttarget);
    dmr_sm_emit_release(opts, state, -1);
    dmr_cspdu_con_plus_set_branding(state);
}

static void
dmr_cspdu_handle_con_plus(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o,
                          int csbk_fid) {
    UNUSED(cs_pdu_bits);
    if (csbk_fid != 0x06) {
        return;
    }

    dmr_cspdu_con_plus_handle_adjacent(state, cs_pdu, csbk_o);
    dmr_cspdu_con_plus_handle_voice(opts, state, cs_pdu, csbk_o);
    dmr_cspdu_con_plus_handle_data(opts, state, cs_pdu, csbk_o);
    dmr_cspdu_con_plus_handle_termination(opts, state, cs_pdu, csbk_o);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static const char*
dmr_cspdu_xpt_status_label(uint8_t status) {
    if (status == 3) {
        return " Null; ";
    }
    if (status == 2) {
        return " Priv; ";
    }
    if (status == 1) {
        return " Unk;  ";
    }
    return " Idle; ";
}

static int
dmr_cspdu_xpt_lcn_start(uint8_t seq) {
    if (seq == 1) {
        return 4;
    }
    if (seq == 2) {
        return 7;
    }
    return 1;
}

static void
dmr_cspdu_xpt_update_cc_from_input(dsd_opts* opts, dsd_state* state) {
    if (opts->use_rigctl == 1) {
        long int ccfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
            opts->trunk_is_tuned = 1;
        }
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        long int ccfreq = (long int)opts->rtlsdr_center_freq;
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
            opts->trunk_is_tuned = 1;
        }
    }
}

/* One LSN of an XPT site status: its status kind (3 group, 2 private, 1 unknown, 0 idle) and 8-bit value. */
typedef struct {
    uint8_t kind;
    uint8_t id8;
} dmr_xpt_entry;

enum { DMR_XPT_ENTRY_COUNT = 18, DMR_XPT_KIND_PRIVATE = 2, DMR_XPT_KIND_GROUP = 3 };

static void
dmr_cspdu_xpt_print_and_collect(dsd_state* state, uint8_t cs_pdu_bits[], uint8_t xpt_seq, uint8_t xpt_bank,
                                const uint8_t xpt_ch[6], dmr_xpt_entry entries[DMR_XPT_ENTRY_COUNT]) {
    char xpt_active[20];
    char notice[DSD_RECENT_ACTIVITY_TEXT_SIZE] = {0};
    int xpt_lcn = dmr_cspdu_xpt_lcn_start(xpt_seq);

    if (xpt_seq == 0) {
        DSD_SNPRINTF(notice, sizeof notice, "XPT ");
    }

    for (int i = 0; i < 6; i++) {
        int slot_idx = i + xpt_bank;
        if (slot_idx >= DMR_XPT_ENTRY_COUNT) {
            continue;
        }
        uint16_t tg = (uint16_t)convert_bits_into_output(&cs_pdu_bits[i * 8 + 32], 8);
        entries[slot_idx].kind = xpt_ch[i];
        entries[slot_idx].id8 = (uint8_t)tg;

        if (i == 0 || i == 2 || i == 4) {
            DSD_FPRINTF(stderr, "\n LCN %d - ", xpt_lcn);
            xpt_lcn++;
        }
        DSD_FPRINTF(stderr, "LSN %02d: ", slot_idx + 1);
        DSD_FPRINTF(stderr, "ST-%X", xpt_ch[i]);
        if (tg != 0) {
            DSD_FPRINTF(stderr, " %03d;  ", tg);
            if (xpt_ch[i] == 3) {
                DSD_SNPRINTF(xpt_active, sizeof(xpt_active), "LSN:%d TG:%d; ", slot_idx + 1, tg);
            } else if (xpt_ch[i] == 2) {
                DSD_SNPRINTF(xpt_active, sizeof(xpt_active), "LSN:%d PC:%d; ", slot_idx + 1, tg);
            } else {
                DSD_SNPRINTF(xpt_active, sizeof(xpt_active), "LSN:%d UK:%d; ", slot_idx + 1, tg);
            }
            dsd_append(notice, sizeof notice, xpt_active);
            continue;
        }

        DSD_FPRINTF(stderr, "%s", dmr_cspdu_xpt_status_label(xpt_ch[i]));
    }
    dmr_csbk_publish_activity(state, xpt_seq, xpt_seq & 1U, DSD_CALL_KIND_DATA, 0U, 0U, 0U, 0, 0, notice);
}

/* An XPT private status names no radio: its 8-bit value is a hash or a placeholder (the 16-bit target only comes with
   the voice LC). Its decision is built without any endpoint lookup, so no row (an ID-0 row or a hash collision
   included) can admit or rank it: private tuning off, the allow-list and an active Hold each block it, and it has
   priority 0 and never preempts. */
static void
dmr_xpt_unknown_private_decision(const dsd_opts* opts, const dsd_state* state, dsd_tg_policy_decision* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    out->private_call = 1;
    out->match = DSD_TG_POLICY_MATCH_NONE;
    if (opts->trunk_tune_private_calls != 1) {
        out->block_reasons |= DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED;
    }
    if (opts->trunk_use_allow_list == 1) {
        out->block_reasons |= DSD_TG_POLICY_BLOCK_ALLOWLIST;
    }
    if (state->tg_hold != 0) {
        out->tg_hold_active = 1;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_HOLD;
    }
    out->tune_allowed = out->block_reasons == DSD_TG_POLICY_BLOCK_NONE;
    out->audio_allowed = out->tune_allowed;
    out->record_allowed = out->tune_allowed;
    out->stream_allowed = out->tune_allowed;
    // No radio is named, so the radio part allows everything and the talkgroup part is the whole answer.
    out->tg_tune_allowed = out->tune_allowed;
    out->tg_audio_allowed = out->tune_allowed;
    out->tg_record_allowed = out->tune_allowed;
    out->tg_stream_allowed = out->tune_allowed;
    out->radio_audio_allowed = 1;
    out->radio_record_allowed = 1;
    out->radio_stream_allowed = 1;
}

/* The candidate an XPT entry is: a group status with a talkgroup (XPT talkgroups are 8-bit, so the value is the real
   ID), or a private status with private tuning on, judged as an unknown identity. Unknown (1) and idle (0) statuses are
   never followed. Returns 0 when the entry is no candidate. */
static int
dmr_cspdu_xpt_entry_decision(const dsd_opts* opts, const dsd_state* state, const dmr_xpt_entry* e,
                             dsd_tg_policy_decision* out, int* allowed) {
    if (e->kind == DMR_XPT_KIND_GROUP && e->id8 != 0U) {
        dmr_csbk_print_target_label(state, DSD_CALL_KIND_GROUP_VOICE, e->id8);
        *allowed = dmr_policy_tune_allowed(opts, state, e->id8, 0, 1, 0, out);
        return 1;
    }
    if (e->kind == DMR_XPT_KIND_PRIVATE && opts->trunk_tune_private_calls == 1) {
        dmr_xpt_unknown_private_decision(opts, state, out);
        *allowed = out->tune_allowed;
        return 1;
    }
    return 0;
}

/* The carrier of XPT entry @p slot_idx when it may be followed at all (trunking, the receive filters, a candidate the
   talkgroup policy allows, whose verdict lands in @p decision, and a tunable input), else 0. A candidate the receive
   policy rejects (its slot, or its channel's verified colour code) is passed over for the next one, as on Cap+ (issue
   #637): the state machine would refuse it and the bank's loop stop at it. */
static long
dmr_cspdu_xpt_entry_freq(const dsd_opts* opts, const dsd_state* state, const dmr_xpt_entry* e, int slot_idx,
                         int trunking, dsd_tg_policy_decision* decision) {
    int policy_allowed = 0;

    if (!trunking || !dsd_receive_grant_allows(opts, DSD_SYNC_DMR_BS_VOICE_POS, slot_idx & 1)) {
        return 0;
    }
    if (!dmr_cspdu_xpt_entry_decision(opts, state, e, decision, &policy_allowed)) {
        return 0;
    }
    const int identity_known = e->kind == DMR_XPT_KIND_GROUP;
    if (!policy_allowed) {
        dmr_policy_log_block(opts, identity_known, identity_known ? e->id8 : 0U, 0, decision);
        return 0;
    }
    const long freq = state->trunk_chan_map[slot_idx + 1];
    if (freq == 0 || !dmr_csbk_receive_carrier_allowed(opts, freq)) {
        return 0;
    }
    if (!(opts->use_rigctl == 1 || opts->audio_in_type == AUDIO_IN_RTL)) {
        return 0;
    }
    return freq;
}

/* Hand the bank's chosen entry @p e (LSN @p best) to the trunk state machine as a status grant. Its channel is the LSN,
   as the ranking described the candidate: the machine's own preemption check must judge the same route. */
static void
dmr_cspdu_xpt_submit(dsd_opts* opts, dsd_state* state, const dmr_xpt_entry* e, int best,
                     const dsd_tg_policy_decision* decision, int caller_busy) {
    const long freq = state->trunk_chan_map[best + 1];
    const int lsn = best + 1;
    dmr_sm_event_t ev;
    if (e->kind == DMR_XPT_KIND_GROUP) {
        DSD_FPRINTF(stderr, "\n LSN/TG to tune to: %d - %d", lsn, e->id8);
        ev = dmr_sm_ev_group_grant(freq, lsn, e->id8, 0);
    } else {
        DSD_FPRINTF(stderr, "\n LSN to tune to: %d - private call", lsn);
        ev = dmr_sm_ev_indiv_grant(freq, lsn, 0, 0);
        ev.identity_known = 0; // the 8-bit value is no radio ID: the voice LC names the call
    }
    ev.slot = best & 1;
    ev.channel_kind = (uint8_t)DSD_CALL_CHANNEL_LSN;
    ev.origin = DMR_SM_ORIGIN_STATUS;
    ev.caller_busy = caller_busy;
    ev.policy_valid = 1;
    ev.policy = *decision;
    (void)dmr_sm_submit_grant(opts, state, &ev);
}

/* The followed carrier's slots as this status bank names them, before any busy view or ranking reads them (as Capacity
   Plus reconciles its seeds): a group entry names its slot's call, a private one leaves it of unknown identity, so a
   group identity the call no longer carries never ranks it against a candidate. Idle and unknown entries change
   nothing. */
static void
dmr_cspdu_xpt_reconcile_followed(const dsd_state* state, const dmr_xpt_entry entries[DMR_XPT_ENTRY_COUNT],
                                 uint8_t xpt_bank) {
    dmr_sm_ctx_t* sm = dmr_sm_get_ctx();
    for (int j = 0; j < 6; j++) {
        const int slot_idx = j + xpt_bank;
        if (slot_idx >= DMR_XPT_ENTRY_COUNT) {
            continue;
        }
        const dmr_xpt_entry* e = &entries[slot_idx];
        const int group = e->kind == DMR_XPT_KIND_GROUP && e->id8 != 0U;
        if (group || e->kind == DMR_XPT_KIND_PRIVATE) {
            dmr_sm_status_slot_reconcile(sm, state, slot_idx & 1, slot_idx + 1, state->trunk_chan_map[slot_idx + 1],
                                         group ? e->id8 : 0U, 1);
        }
    }
}

/* One XPT status bank: as for Capacity Plus (dmr_cspdu_cap_plus_3e_try_tune_grants()), at most one entry is handed to
   the trunk state machine, the best by Hold, priority and channel order among those eligible, and while the receiver
   holds a call only Hold over a non-held call or a preemption is eligible. An unknown private identity never is, nor,
   inside the status's voice window, an entry on the carrier the receiver holds or is parked on. */
static void
dmr_cspdu_xpt_try_tune(dsd_opts* opts, dsd_state* state, const dmr_xpt_entry entries[DMR_XPT_ENTRY_COUNT],
                       uint8_t xpt_bank) {
    const int trunking = state->trunk_cc_freq != 0 && opts->trunk_enable == 1;
    const double now_m = dsd_decode_now_mono_s();
    const int caller_busy = (dsd_decode_time() - state->last_vc_sync_time) <= 2;
    dmr_sm_busy_view_t view;
    const int busy = dmr_csbk_status_busy(opts, state, trunking, caller_busy, now_m, &view);

    int best = -1;
    dsd_tg_policy_decision best_decision;
    DSD_MEMSET(&best_decision, 0, sizeof(best_decision));
    for (int j = 0; j < 6; j++) {
        const int slot_idx = j + xpt_bank;
        if (slot_idx >= DMR_XPT_ENTRY_COUNT) {
            continue;
        }
        const dmr_xpt_entry* e = &entries[slot_idx];
        dsd_tg_policy_decision policy_decision;
        const long freq = dmr_cspdu_xpt_entry_freq(opts, state, e, slot_idx, trunking, &policy_decision);
        if (freq == 0) {
            continue;
        }
        if (caller_busy && freq == view.carrier_hz) {
            /* Inside the voice window a call on the carrier the receiver holds or is parked on is left alone: a submit
               there from the control channel would retune, reset the blocks and clear the live slot's activity. */
            continue;
        }
        const int identity_known = e->kind == DMR_XPT_KIND_GROUP;
        if (busy && freq != view.carrier_hz) {
            const dsd_tg_policy_call_route candidate = {
                .target_id = identity_known ? e->id8 : 0U,
                .source_id = 0U,
                .freq_hz = freq,
                .channel = slot_idx + 1,
                .slot = slot_idx & 1,
                .requires_tuner_retune = 1,
            };
            if (dmr_sm_may_displace(&view, opts, state, &candidate, &policy_decision, 1, identity_known, now_m)
                == DMR_SM_DISPLACE_NONE) {
                continue;
            }
        }
        if (best < 0 || dmr_status_candidate_outranks(&policy_decision, &best_decision)) {
            best = slot_idx;
            best_decision = policy_decision;
        }
    }
    if (best < 0) {
        return;
    }
    dmr_cspdu_xpt_submit(opts, state, &entries[best], best, &best_decision, caller_busy);
}

static void
dmr_cspdu_xpt_handle_site_status(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], int csbk_o) {
    uint8_t xpt_ch[6];
    dmr_xpt_entry entries[DMR_XPT_ENTRY_COUNT];
    uint8_t xpt_seq;
    uint8_t xpt_free;
    uint8_t xpt_bank;

    if (csbk_o != 0x0A) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_MEMSET(entries, 0, sizeof(entries));

    xpt_seq = (uint8_t)convert_bits_into_output(&cs_pdu_bits[0], 2);
    xpt_free = (uint8_t)convert_bits_into_output(&cs_pdu_bits[16], 4);
    xpt_bank = (xpt_seq <= 2 && xpt_seq != 0) ? (uint8_t)(xpt_seq * 6) : 0;
    for (int i = 0; i < 6; i++) {
        xpt_ch[i] = (uint8_t)convert_bits_into_output(&cs_pdu_bits[20 + (i * 2)], 2);
    }

    DSD_FPRINTF(stderr, " Hytera XPT Site Status - Free LCN: %d SN: %d", xpt_free, xpt_seq);
    // The free value is a repeater (LCN); the map is keyed by LSN: its first slot.
    const uint8_t xpt_free_lsn = dmr_xpt_lcn_to_lsn(xpt_free);
    if (xpt_free_lsn != 0) {
        long f = state->trunk_chan_map[xpt_free_lsn];
        if (f != 0) {
            const long candx[1] = {f};
            dmr_sm_on_neighbor_update(opts, state, candx, 1);
        }
    }

    dmr_cspdu_xpt_print_and_collect(state, cs_pdu_bits, xpt_seq, xpt_bank, xpt_ch, entries);
    DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "Free LCN - %d ", xpt_free);
    if (dmr_csbk_crc_checked(state)) {
        dsd_rxid_note_dmr_xpt_free_lcn(state, xpt_free);
    }
    dmr_cspdu_xpt_update_cc_from_input(opts, state);
    dmr_cspdu_xpt_reconcile_followed(state, entries, xpt_bank);

    /* Group and private statuses are admitted each by their own decision, so private calls are followed with group
       tuning off; an allow-list or a Hold never wipes the voice-sync stamp (dmr_cspdu_xpt_try_tune()). */
    if (opts->trunk_tune_group_calls == 1 || opts->trunk_tune_private_calls == 1) {
        if ((dsd_decode_time() - state->last_vc_sync_time) > 2) {
            rotate_symbol_out_file(opts, state);
        }
        dmr_cspdu_xpt_try_tune(opts, state, entries, xpt_bank);
    }

    DSD_SNPRINTF(state->dmr_branding_sub, sizeof(state->dmr_branding_sub), "XPT ");
    dmr_csbk_note_brand(state, "Hytera XPT");
}

static void
dmr_cspdu_xpt_handle_adjacent(uint8_t cs_pdu_bits[], dsd_state* state, int csbk_o) {
    uint8_t xpt_site_id[4];
    uint8_t xpt_site_rp[4];
    uint8_t xpt_sn;

    if (csbk_o != 0x0B) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);

    xpt_sn = (uint8_t)convert_bits_into_output(&cs_pdu_bits[0], 2);
    for (int i = 0; i < 4; i++) {
        xpt_site_id[i] = (uint8_t)convert_bits_into_output(&cs_pdu_bits[16 + (i * 16)], 5);
        xpt_site_rp[i] = (uint8_t)convert_bits_into_output(&cs_pdu_bits[24 + (i * 16)], 4);
    }

    DSD_FPRINTF(stderr, " Hytera XPT CSBK 0x0B - SN: %d", xpt_sn);
    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, " XPT Adjacent ");
    for (int i = 0; i < 4; i++) {
        if (xpt_site_id[i] != 0) {
            DSD_FPRINTF(stderr, "Site:%d Free:%d; ", xpt_site_id[i], xpt_site_rp[i]);
        }
    }
    DSD_SNPRINTF(state->dmr_branding_sub, sizeof(state->dmr_branding_sub), "XPT ");
    dmr_csbk_note_brand(state, "Hytera XPT");
}

static void
dmr_cspdu_handle_xpt(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], int csbk_o,
                     int csbk_fid) {
    UNUSED(cs_pdu);
    if (csbk_fid != 0x68) {
        return;
    }
    if (csbk_o == 0x0A || csbk_o == 0x0B) {
        dmr_sm_note_xpt_site(opts, state); // a CRC-valid XPT site status or adjacent-site list: no control channel
    }

    dmr_cspdu_xpt_handle_site_status(opts, state, cs_pdu_bits, csbk_o);
    dmr_cspdu_xpt_handle_adjacent(cs_pdu_bits, state, csbk_o);
}

static void
dmr_cspdu_handle_moto_unknown(uint8_t cs_pdu[], int csbk_o, int csbk_fid) {
    if (csbk_o == 41 && csbk_fid == 0x10) {
        //initial line break
        DSD_FPRINTF(stderr, "\n");
        DSD_FPRINTF(stderr, "%s", KYEL);
        DSD_FPRINTF(stderr, " Moto Data Channel: %02X; ", csbk_o);
        for (int i = 2; i < 10; i++) {
            DSD_FPRINTF(stderr, "%02X ", cs_pdu[i]);
        }

        //SDRTrunk suggest this could be a data channel revert announcement
        //I'm not even sure what a revert data channel is
        //Moto Unknown Data Opcode: 29; 00 00 00 39 04 FC 00 00
    }
}

static int
dmr_cspdu_apply_protect_flag_checks(uint32_t IrrecoverableErrors, int csbk_o, int csbk_fid, int csbk_pf) {
    if (IrrecoverableErrors == 0) {
        //Hytera XPT CSBK Check -- if bits 0 and 1 are used as lcss, gi, ts, then the pf bit may be set on
        if (csbk_fid == 0x68 && (csbk_o == 0x0A || csbk_o == 0x0B)) {
            csbk_pf = 0;
        }
        if (csbk_pf == 1) //check the protect flag, don't run if set
        {
            DSD_FPRINTF(stderr, "%s", KRED);
            DSD_FPRINTF(stderr, "\n Protected Control Signalling Block(s)");
            DSD_FPRINTF(stderr, "%s", KNRM);
        }
    }
    return csbk_pf;
}

static void
dmr_cspdu_init_cc_anchor(const dsd_opts* opts, dsd_state* state) {
    // If trunking is enabled and we don't yet know the CC frequency, set it
    // from the current tuner so return-to-CC and SM logic have an anchor.
    if (opts->trunk_enable == 1 && opts->trunk_is_tuned == 0 && state->trunk_cc_freq == 0) {
        long int ccfreq = 0;
        if (opts->use_rigctl == 1) {
            ccfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        } else if (opts->audio_in_type == AUDIO_IN_RTL) {
            ccfreq = (long int)opts->rtlsdr_center_freq;
        }
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
        }
    }
}

/* Opcodes whose MBC form appends absolute parameters (CG_AP, MV_AP, BC_AP: TS 102 361-4 Tables 7.16, 7.18, 7.21):
   C_BCAST, the standard P_CLEAR, the channel grants and C_MOVE. Their handlers run whatever the feature set, except
   P_CLEAR: another feature set's opcode 46 is the manufacturer's, its content opaque (dmr_cspdu_pf0_handle_p_clear()). */
static int
dmr_cspdu_appends_abs_params(int csbk_o, int csbk_fid) {
    return csbk_o == 40 || (csbk_o == 46 && csbk_fid == 0) || (csbk_o >= 48 && csbk_o <= 57);
}

/* The channel field of a PDU that names its channel by one, or -1: bits 16-27 of a channel grant or a standard
   P_CLEAR (Table 7.30), the Physical Channel Number (bits 44-55) of a C_MOVE. Each is a logical channel sent as a
   single CSBK, or 0xFFF heading an MBC (§7.1.1.1.1, §7.1.1.1.3, §7.1.1.3.2). Opcode 56 on MS-sourced data is
   BS_Dwn_Act, not a grant. */
static int
dmr_cspdu_channel_field(const dsd_state* state, const uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid) {
    if ((csbk_o == 46 && csbk_fid == 0)
        || dmr_t3_grant_is_grant_opcode(csbk_o, state->synctype != DSD_SYNC_DMR_MS_DATA)) {
        return (int)convert_bits_into_output(&cs_pdu_bits[16], 12);
    }
    if (csbk_o == 57) {
        return (int)convert_bits_into_output(&cs_pdu_bits[44], 12);
    }
    return -1;
}

/* 1 when the C_BCAST names a channel by the absolute parameters it appends (TS 102 361-4 §7.2.19): Ann_WD_TSCC in its
   MBC form, Chan_Freq always (Table 7.78), Vote_Now (VN_AP) and Adjacent_Site (BC_AP) on channel 0xFFF (bits 68-79). */
static int
dmr_cspdu_c_bcast_uses_abs_params(const uint8_t cs_pdu_bits[], const dmr_cspdu_shape* shape) {
    switch (convert_bits_into_output(&cs_pdu_bits[16], 5)) {
        case 0: return shape->is_mbc != 0U;
        case 5: return 1;
        case 2:
        case 6: return convert_bits_into_output(&cs_pdu_bits[68], 12) == 0xFFFU;
        default: return 0;
    }
}

/* 1 when a C_BCAST says something no handler may act on: a single-block Ann_WD_TSCC naming the absolute escape as
   BCAST_CH1 or BCAST_CH2 (bits 56-67, 68-79), whose frequency only an MBC carries (§7.2.19.1); a Vote_Now or
   Adjacent_Site MBC whose channel (bits 68-79) is not the escape, since a logical channel is a single CSBK
   (§7.2.19.3.0(b), Table 7.71); or a Chan_Freq MBC whose header CH (bits 68-79) and BC_AP CHAN (bits 118-129) both
   name channels and differ (§7.2.19.6). No continuation bit is read before the shape says there is one. */
static int
dmr_cspdu_c_bcast_contradicts(const uint8_t cs_pdu_bits[], const dmr_cspdu_shape* shape) {
    const uint32_t ch_a = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 12);
    const uint32_t ch_b = (uint32_t)convert_bits_into_output(&cs_pdu_bits[68], 12);
    switch (convert_bits_into_output(&cs_pdu_bits[16], 5)) {
        case 0: return shape->is_mbc == 0U && (ch_a == 0xFFFU || ch_b == 0xFFFU);
        case 2:
        case 6: return shape->is_mbc != 0U && ch_b != 0xFFFU;
        case 5: {
            if (shape->is_mbc == 0U) {
                return 0; // no BC_AP: dmr_cspdu_malformed() refuses it on its parameters
            }
            const uint32_t bcap_chan = (uint32_t)convert_bits_into_output(&cs_pdu_bits[118], 12);
            return dmr_t3_lpcn_is_channel(ch_b) && dmr_t3_lpcn_is_channel(bcap_chan) && ch_b != bcap_chan;
        }
        default: return 0;
    }
}

/* 1 when an MBC's form is wrong whatever its content: more continuations than TS 102 361-1 §7.4.0 allows, or none;
   for an absolute-parameter opcode, a continuation that is not exactly one last block carrying its header's CSBKO. */
static int
dmr_cspdu_mbc_shape_malformed(int csbk_o, int csbk_fid, const dmr_cspdu_shape* shape) {
    if (shape->continuations < 1U || shape->continuations > 3U) {
        return 1;
    }
    return dmr_cspdu_appends_abs_params(csbk_o, csbk_fid) && !dmr_abs_params_shape_ok(shape);
}

/* 1 when a standard C_BCAST single block (data type 3) does not carry LB 1, which Table 7.20 requires: it is no
   complete PDU. A manufacturer's flags are its own. */
static int
dmr_cspdu_c_bcast_single_unfinished(const uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid,
                                    const dmr_cspdu_shape* shape) {
    return csbk_o == 40 && csbk_fid == 0 && shape->is_mbc == 0U && cs_pdu_bits[0] == 0U;
}

/* A PDU no handler may act on, refused before any of its effects (scan activity, the control channel heartbeat and
   anchor included): an MBC of the wrong shape (dmr_cspdu_mbc_shape_malformed()); a standard C_BCAST single block
   without LB; a grant, standard P_CLEAR or C_MOVE naming anything but the absolute escape as an MBC (a logical channel,
   or a P_CLEAR's 0, the last confirmed TSCC, is a single CSBK); a C_BCAST that contradicts itself
   (dmr_cspdu_c_bcast_contradicts()); or an unprotected PDU that names its channel by absolute parameters without
   usable ones (dmr_abs_params_decode(): that one continuation, Cdeftype 0 and a receive frequency inside Table C.7), a
   single CSBK on 0xFFF among them. */
static int
dmr_cspdu_malformed(const dsd_state* state, const uint8_t cs_pdu_bits[], int csbk_o, int csbk_fid, int csbk_pf,
                    const dmr_cspdu_shape* shape) {
    if ((shape->is_mbc && dmr_cspdu_mbc_shape_malformed(csbk_o, csbk_fid, shape))
        || dmr_cspdu_c_bcast_single_unfinished(cs_pdu_bits, csbk_o, csbk_fid, shape)) {
        return 1;
    }
    if (csbk_pf != 0) {
        return 0;
    }
    const int chan = dmr_cspdu_channel_field(state, cs_pdu_bits, csbk_o, csbk_fid);
    if (shape->is_mbc && chan >= 0 && chan != 0xFFF) {
        return 1;
    }
    if (csbk_o == 40 && dmr_cspdu_c_bcast_contradicts(cs_pdu_bits, shape)) {
        return 1;
    }
    if (chan != 0xFFF && !(csbk_o == 40 && dmr_cspdu_c_bcast_uses_abs_params(cs_pdu_bits, shape))) {
        return 0;
    }
    dmr_abs_params ap;
    return !dmr_abs_params_decode(cs_pdu_bits, shape, &ap);
}

static const dmr_cspdu_shape k_dmr_cspdu_single_shape = {0U, 0U, 0U, 0U, 0U, 0U};

/* The shape of the PDU dmr_cspdu_ex() is dispatching; NULL outside one. Decoder thread only. */
static const dmr_cspdu_shape* s_dmr_cspdu_shape;

const dmr_cspdu_shape*
dmr_cspdu_current_shape(void) {
    return s_dmr_cspdu_shape != NULL ? s_dmr_cspdu_shape : &k_dmr_cspdu_single_shape;
}

static void
dmr_cspdu_dispatch(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
                   uint32_t IrrecoverableErrors) {
    int csbk_pf = ((cs_pdu[0] & 0x40) >> 6);
    int csbk_o = cs_pdu[0] & 0x3F;
    int csbk_fid = cs_pdu[1]; //feature set id

    csbk_pf = dmr_cspdu_apply_protect_flag_checks(IrrecoverableErrors, csbk_o, csbk_fid, csbk_pf);

    const dmr_cspdu_shape* const shape = dmr_cspdu_current_shape();
    if (dmr_cspdu_malformed(state, cs_pdu_bits, csbk_o, csbk_fid, csbk_pf, shape)) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "\n Malformed Control Signalling Block (CSBKO %02X); ignored", csbk_o);
        DSD_FPRINTF(stderr, "%s", KNRM);
        return;
    }
    /* Activity for a scan quiet check (issue #834), from the bursts that proved it, now the PDU is not refused: with
       the colour-code gate's evidence those bursts deferred (dmr_confidence_note_control_burst()). */
    if (shape->proves_content) {
        dsd_frame_sync_hook_content_evidence(opts, state, DSD_CONTENT_EVIDENCE_PROVEN, 0U);
        dmr_confidence_note_control_accepted(state);
    }

    if (IrrecoverableErrors == 0 && CRCCorrect == 1) {
        dsd_trunk_recovery_note_protocol(state, DSD_TRUNK_RECOVERY_DMR);
        //clear stale Active Channel messages here
        (void)dsd_recent_activity_expire(state, 0U, DSD_RECENT_ACTIVITY_TTL_MS);

        //update time to prevent random 'Control Channel Signal Lost' hopping
        //in the middle of voice call on current Control Channel (con+ and t3)
        dsd_mark_cc_sync(state);
        dmr_sm_note_cc_heartbeat(opts, state);

        dmr_cspdu_init_cc_anchor(opts, state);

        dmr_cspdu_handle_pf0(opts, state, cs_pdu_bits, cs_pdu, csbk_pf, csbk_o, csbk_fid);
        dmr_cspdu_handle_cap_plus(opts, state, cs_pdu_bits, cs_pdu, csbk_o, csbk_fid);
        dmr_cspdu_handle_con_plus(opts, state, cs_pdu_bits, cs_pdu, csbk_o, csbk_fid);
        dmr_cspdu_handle_xpt(opts, state, cs_pdu_bits, cs_pdu, csbk_o, csbk_fid);
        dmr_cspdu_handle_moto_unknown(cs_pdu, csbk_o, csbk_fid);
    }
    // Relaxed CC heartbeat: when CRC fails (e.g., RAS/vendor variants), allow
    // a last_cc_sync_time refresh to prevent premature CC hunts if configured.
    // This does not process the PDU further — it only keeps the CC timer warm.
    else if (opts->dmr_crc_relaxed_default) {
        dmr_sm_note_cc_heartbeat(opts, state);
        state->last_cc_sync_time = dsd_decode_time();
        state->last_cc_sync_time_m = dsd_decode_now_mono_s();
    }

    DSD_FPRINTF(stderr, "%s", KNRM);
}

//function for handling Control Signalling PDUs (CSBK, MBC) messages
void
dmr_cspdu_ex(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t crc_correct,
             uint32_t irrecoverable, const dmr_cspdu_shape* shape) {
    if (opts == NULL || state == NULL || cs_pdu_bits == NULL || cs_pdu == NULL) {
        return;
    }
    const dmr_cspdu_shape* const outer = s_dmr_cspdu_shape;
    s_dmr_cspdu_shape = shape != NULL ? shape : &k_dmr_cspdu_single_shape;
    dmr_cspdu_dispatch(opts, state, cs_pdu_bits, cs_pdu, crc_correct, irrecoverable);
    s_dmr_cspdu_shape = outer;
}

void
dmr_cspdu(dsd_opts* opts, dsd_state* state, uint8_t cs_pdu_bits[], uint8_t cs_pdu[], uint32_t CRCCorrect,
          uint32_t IrrecoverableErrors) {
    dmr_cspdu_ex(opts, state, cs_pdu_bits, cs_pdu, CRCCorrect, IrrecoverableErrors, NULL);
}

void
dmr_cspdu_release_channel(dsd_opts* opts, dsd_state* state) {
    // The bit buffer must span the full 12-octet CSPDU: handlers may read bits ahead of their opcode check.
    uint8_t pdu[12];
    uint8_t pdu_bits[96];
    DSD_MEMSET(pdu, 0, sizeof(pdu));
    DSD_MEMSET(pdu_bits, 0, sizeof(pdu_bits));
    pdu[0] = 46; // P_CLEAR
    pdu[1] = 255;
    s_dmr_cspdu_synthesized = 1;
    dmr_cspdu(opts, state, pdu_bits, pdu, 1, 0);
    s_dmr_cspdu_synthesized = 0;
}

//translate special gateway identifier addresses
typedef struct {
    uint32_t id;
    const char* label;
} dmr_gateway_id_entry;

static const dmr_gateway_id_entry k_dmr_gateway_ids[] = {
    {0xFFFEC0U, "PSTNI; "},    {0xFFFEC1U, "PABXI; "},    {0xFFFEC2U, "LINEI; "},   {0xFFFEC3U, "IPI; "},
    {0xFFFEC4U, "SUPLI; "},    {0xFFFEC5U, "SDMI; "},     {0xFFFEC6U, "REGI; "},    {0xFFFEC7U, "MSI; "},
    {0xFFFEC8U, "RESERVED; "}, {0xFFFEC9U, "DIVERTI; "},  {0xFFFECAU, "TSI; "},     {0xFFFECBU, "DISPATI; "},
    {0xFFFECCU, "STUNI; "},    {0xFFFECDU, "AUTHI; "},    {0xFFFECEU, "GPI; "},     {0xFFFECFU, "KILLI; "},
    {0xFFFED0U, "PSTNDI; "},   {0xFFFED1U, "PABXDI; "},   {0xFFFED2U, "LINEDI; "},  {0xFFFED3U, "DISPATDI; "},
    {0xFFFED4U, "ALLMSI; "},   {0xFFFED5U, "IPDI; "},     {0xFFFED6U, "DGNAI; "},   {0xFFFED7U, "TATTSI; "},
    {0xFFFFFDU, "ALLMSIDL; "}, {0xFFFFFEU, "ALLMSIDZ; "}, {0xFFFFFFU, "ALLMSID; "},
};

static const char*
dmr_gateway_label_for_id(uint32_t id) {
    size_t count = sizeof(k_dmr_gateway_ids) / sizeof(k_dmr_gateway_ids[0]);
    for (size_t i = 0; i < count; i++) {
        if (k_dmr_gateway_ids[i].id == id) {
            return k_dmr_gateway_ids[i].label;
        }
    }
    return NULL;
}

static void
dmr_gateway_identifier(uint32_t source, uint32_t target) {
    const uint32_t ids[2] = {source, target};

    for (size_t i = 0; i < 2; i++) {
        const char* label = dmr_gateway_label_for_id(ids[i]);
        if (label) {
            DSD_FPRINTF(stderr, "%s", label);
        }
    }

    //NOTE: Observed address values of 64250, or 0xFAFA have been observed
    //on some Moto Tier 2 and Cap+ Systems, and 0xFAFAFA has been observed
    //on some Moto Tier 3 (CapMax) Systems, unsure if these are unique to that
    //manufacturer, or not, usually associated with Data Headers and PDU Messages
}

static void
dmr_syscode_set_partition_label(uint8_t par, char* par_str, size_t par_str_sz) {
    if (!par_str || par_str_sz == 0) {
        return;
    }
    DSD_SNPRINTF(par_str, par_str_sz, "%s", "Res");
    if (par == 1) {
        DSD_SNPRINTF(par_str, par_str_sz, "%s", "A");
    } else if (par == 2) {
        DSD_SNPRINTF(par_str, par_str_sz, "%s", "B");
    } else if (par == 3) {
        DSD_SNPRINTF(par_str, par_str_sz, "%s", "AB");
    }
}

/* Whether a system code CSBK of feature set @p csbk_fid is Motorola's (FID 0x10), which brands the carrier Motorola. A
   C_ALOHA of it recognises its network as Capacity Max only when dmr_aloha_recognises_capmax() holds. */
static uint8_t
dmr_syscode_note_capmax(dsd_state* state, int csbk_fid) {
    if (csbk_fid != 0x10) {
        return 0U;
    }
    DSD_SNPRINTF(state->dmr_branding, sizeof(state->dmr_branding), "%s", "Motorola");
    return 1U;
}

/* Whether the Motorola C_ALOHA being handled may recognise its network as Capacity Max (dmr_tiii_site.h): a single
   CSBK (last-block bit set, never an MBC header that happens to carry the opcode) that passed its CRC, or one -F
   accepted as RAS, whose keyed CRC no one can check, when nothing else casts doubt on it. The slot's flags must say RAS
   alone (a failed CRC never counts), RAS must be confirmed on this carrier, and the burst must be read whole from it,
   as dmr_ras_note_signature() takes a signature. dmr_cspdu() handles a C_ALOHA only with no irrecoverable errors and the
   protect flag clear, and the P_CLEAR it synthesizes is never one. */
static int
dmr_aloha_recognises_capmax(const dsd_opts* opts, const dsd_state* state, const uint8_t* cs_pdu_bits) {
    if (s_dmr_cspdu_synthesized || cs_pdu_bits[0] != 1U) {
        return 0;
    }
    return dmr_csbk_crc_checked(state) || dmr_csbk_ras_accepted(opts, state);
}

/* The C_ALOHA line. It names Capacity Max only on a recognised network (@p capmax), whose numbers it shows raw: a
   Motorola C_ALOHA read before recognition shows the standard numbers and so does not name it. */
static void
dmr_syscode_print_type0(const dsd_opts* opts, const uint8_t* cs_pdu_bits, const char* model_str, uint16_t net,
                        const char* site_label, const char* par_str, uint16_t syscode, int capmax) {
    uint8_t reserved = cs_pdu_bits[16];
    uint8_t tsccas = cs_pdu_bits[17];
    uint8_t sync = cs_pdu_bits[18];
    uint8_t version = (uint8_t)convert_bits_into_output(&cs_pdu_bits[19], 3);
    uint8_t offset = cs_pdu_bits[22];
    uint8_t active = cs_pdu_bits[23];
    uint8_t mask = (uint8_t)convert_bits_into_output(&cs_pdu_bits[24], 5);
    uint8_t sf = (uint8_t)convert_bits_into_output(&cs_pdu_bits[29], 2);
    uint8_t nrandwait = (uint8_t)convert_bits_into_output(&cs_pdu_bits[31], 4);
    uint8_t regreq = cs_pdu_bits[35];
    uint8_t backoff = (uint8_t)convert_bits_into_output(&cs_pdu_bits[36], 4);
    uint32_t target = (uint32_t)convert_bits_into_output(&cs_pdu_bits[56], 24);

    DSD_FPRINTF(stderr, " C_ALOHA_SYS_PARMS: %s; Net ID: %u; Site ID: %s; Cat: %s;", model_str, (unsigned)net,
                site_label, par_str);
    DSD_FPRINTF(stderr, " SYS: %04X;", syscode);
    if (capmax) {
        DSD_FPRINTF(stderr, " " DSD_RXID_T3_CAPMAX_TOKEN);
    }
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n");
    if (reserved) {
        DSD_FPRINTF(stderr, " Res: %04X;", reserved);
    }
    if (tsccas) {
        DSD_FPRINTF(stderr, " TSCCAS;");
    }
    if (sync) {
        DSD_FPRINTF(stderr, " Sync;");
    }
    DSD_FPRINTF(stderr, " Ver: %d;", version);
    if (offset) {
        DSD_FPRINTF(stderr, " Offset;");
    }
    if (active) {
        DSD_FPRINTF(stderr, " Active Connection;");
    }
    DSD_FPRINTF(stderr, " SF: %d;", sf);
    DSD_FPRINTF(stderr, " NR: %X;", nrandwait);
    if (regreq) {
        DSD_FPRINTF(stderr, " Reg Required;");
    }
    DSD_FPRINTF(stderr, " Backoff: %X;", backoff);
    if (mask) {
        DSD_FPRINTF(stderr, " Mask: %02X;", mask);
    }
    if (target) {
        DSD_FPRINTF(stderr, " MS: %d; ", target);
    }
    dmr_gateway_identifier(0, target);
}

static void
dmr_syscode_print_type1(const char* model_str, uint16_t net, const char* site_label, uint16_t syscode) {
    DSD_FPRINTF(stderr, " %s; Net ID: %u; Site ID: %s;", model_str, (unsigned)net, site_label);
    DSD_FPRINTF(stderr, " SYS: %04X;", syscode);
}

/* A system code CSBK: type 0 is C_ALOHA, whose system code (bits 40..53) is this TSCC's; type 1 is C_BCAST vote or
   adjacent, whose BParms1 (bits 21..34) names another site. Numbers show as the network's vendor numbers them
   (dmr_tiii_site.h): the code's own network is looked up, so a neighbour of a recognised Capacity Max network reads raw
   and one of any other network reads standard. */
static void
dmr_decode_syscode(const dsd_opts* opts, dsd_state* state, const uint8_t* cs_pdu_bits, int csbk_fid, int type) {
    const uint16_t syscode = (uint16_t)convert_bits_into_output(&cs_pdu_bits[type == 0 ? 40 : 21], 14);
    const dmr_tiii_code code = dmr_tiii_decode_syscode(syscode);
    const uint16_t dmrla = dmr_tiii_dmrla(opts->dmr_dmrla_is_set, opts->dmr_dmrla_n);
    char site_label[16];

    const uint8_t is_motorola = dmr_syscode_note_capmax(state, csbk_fid);
    if (type == 0) {
        state->dmr_t3_syscode = syscode;
        if (is_motorola && dmr_aloha_recognises_capmax(opts, state, cs_pdu_bits)) {
            dsd_rxid_note_dmr_t3_capmax(state, code.model, code.net);
        }
    }
    const int capmax = dsd_rxid_dmr_t3_is_capmax(state, code.model, code.net);
    const uint16_t net = dmr_tiii_display_net(code.net, capmax);
    dmr_tiii_site_label(site_label, sizeof(site_label), code.site, code.site_bits, dmrla, capmax);

    if (type == 0) {
        char par_str[8];
        dmr_syscode_set_partition_label((uint8_t)convert_bits_into_output(&cs_pdu_bits[54], 2), par_str,
                                        sizeof(par_str));
        dmr_syscode_print_type0(opts, cs_pdu_bits, code.model_name, net, site_label, par_str, syscode, capmax);
        DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "TIII %s:%u-%s;%04X; ", code.model_name,
                     (unsigned)net, site_label, syscode);
        /* A C_ALOHA names this site; a C_BCAST names another and is no note. dmr_cspdu()'s gate also passes a burst -F
           accepted by RAS, or an MBC whose header CRC failed: the dispatcher flags those for the slot, and only a CRC
           that matched names the site. */
        if (dmr_csbk_crc_checked(state)) {
            /* Another checked code than the last is another network: what the air taught goes. */
            dmr_t3_chan_note_checked_syscode(state, syscode);
            dmr_tiii_note_search_site(state, syscode, dmrla);
            /* A C_ALOHA comes from a Tier III control channel: this carrier, where the receiver is tuned. */
            dsd_rxid_note_cc_freq(state, DSD_RXID_FAMILY_DMR, dsd_opts_tuned_freq_hz(opts));
            dmr_t3_cc_note_evidence(opts, state);
        }
    } else if (type == 1) {
        dmr_syscode_print_type1(code.model_name, net, site_label, syscode);
    }
}
