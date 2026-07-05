// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*-------------------------------------------------------------------------------
 * p25p1_pdu_trunking.c
 * P25p1 PDU Alt Format Trunking
 *
 * LWVMOBILE
 * 2025-03 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_vpdu.h>
#include <dsd-neo/protocol/p25/p25p1_pdu_trunking.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "../p25_cc_update.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/platform.h"

static inline int
p25_signed_offset_units(int sign_bit, int raw_offset) {
    return sign_bit ? raw_offset : -raw_offset;
}

static void DSD_ATTR_USED
p25p1_pdu_print_group_label(const dsd_state* state, uint32_t id) {
    char name[50];
    if (id != 0U && dsd_tg_policy_lookup_label(state, id, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, " [%s]", name);
    }
}

typedef struct {
    uint8_t fmt;
    uint8_t mfid;
    int blks;
    uint8_t opcode;
} p25p1_mbt_fields;

enum {
    P25_MBT_AMBTC_OPCODE_INDEX = 7,
    P25_MBT_UMBTC_OPCODE_INDEX = 12,
};

static int
p25_mbt_opcode_index(uint8_t fmt) {
    /*
     * Alternate MBTC (0x17) carries the opcode in the PDU header. Unconfirmed
     * MBTC (0x15) carries a data header at the start of block 0; in the
     * contiguous MPDU buffer that opcode is byte 12 and payload starts at 13.
     */
    return (fmt == 0x17) ? P25_MBT_AMBTC_OPCODE_INDEX : P25_MBT_UMBTC_OPCODE_INDEX;
}

static p25p1_mbt_fields
p25_parse_mbt_fields(const uint8_t* mpdu_byte) {
    p25p1_mbt_fields fields;
    fields.fmt = mpdu_byte[0] & 0x1F;
    fields.mfid = mpdu_byte[2];
    fields.blks = mpdu_byte[6] & 0x7F;
    fields.opcode = mpdu_byte[p25_mbt_opcode_index(fields.fmt)] & 0x3F;
    return fields;
}

static void
p25_print_mbt_header(const p25p1_mbt_fields* fields) {
    if (fields->fmt == 0x15) {
        DSD_FPRINTF(stderr, " UNC");
    } else {
        DSD_FPRINTF(stderr, " ALT");
    }
    DSD_FPRINTF(stderr, " MBT");
    DSD_FPRINTF(stderr, " - OP: %02X", fields->opcode);
}

static void
p25_print_mbt_payload_hex(const uint8_t* mpdu_byte, int blks) {
    int limit = (12 * (blks + 1) % 37);
    for (int i = 0; i < limit; i++) {
        DSD_FPRINTF(stderr, "%02X", mpdu_byte[i]);
    }
}

static int
p25_mbt_is_survey_broadcast(uint8_t opcode) {
    return opcode == 0x3A || opcode == 0x3B || opcode == 0x3C || opcode == 0x3E;
}

static int
p25_mbt_has_unsupported_survey_format(const p25p1_mbt_fields* fields) {
    return fields && p25_mbt_is_survey_broadcast(fields->opcode) && fields->fmt != 0x17;
}

static int
p25_mbt_is_bridgeable_iden_update(uint8_t opcode) {
    return opcode == 0x74 || opcode == 0x7D || opcode == 0x73 || opcode == 0xF3 || opcode == 0x34 || opcode == 0x3D;
}

static int
p25p1_pdu_can_tune_grant(const dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || !state || opts->p25_trunk != 1 || opts->p25_is_tuned != 0 || freq == 0) {
        return 0;
    }
    p25_sm_seed_cc_from_current_tuner_if_unknown(opts, state);
    return state->p25_cc_freq != 0;
}

static void DSD_ATTR_USED
p25_mbt_try_bridge_iden_updates(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte,
                                const p25p1_mbt_fields* fields) {
    if (!fields || !(p25_mbt_is_bridgeable_iden_update(fields->opcode) && fields->mfid < 2)) {
        return;
    }

    /*
     * Keep UMBTC identifier updates bridgeable. Some paths provide a decoded
     * non-extended MBTC buffer directly to this decoder; for those frames the
     * opcode is byte 12 and the IDEN payload begins at byte 13. The survey
     * broadcast handlers below remain Extended Format only.
     */
    int op_idx = p25_mbt_opcode_index(fields->fmt);
    int payload_off = op_idx + 1;
    int total_len = (12 * (fields->blks + 1));
    unsigned long long int MAC[24] = {0};

    uint8_t mac_opcode = fields->opcode;
    if ((mac_opcode & 0xC0) == 0x00) {
        mac_opcode |= 0x40;
    }
    MAC[1] = mac_opcode;

    int mac_i = 2;
    for (int i = payload_off; i < total_len && mac_i < 24; i++, mac_i++) {
        MAC[mac_i] = mpdu_byte[i];
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Identifier Update (MBT bridged) OP:%02X -> MAC decode", fields->opcode);
    process_MAC_VPDU(opts, state, 0, MAC);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void DSD_ATTR_USED
p25_print_voice_svc_common(const dsd_opts* opts, dsd_state* state, int svc) {
    state->dmr_so = (uint16_t)svc;
    state->p25_service_options_valid[0] = 1;

    if (svc & 0x80) {
        DSD_FPRINTF(stderr, " Emergency");
        state->p25_call_emergency[0] = 1;
    } else {
        state->p25_call_emergency[0] = 0;
    }

    if (svc & 0x40) {
        DSD_FPRINTF(stderr, " Encrypted");
    }

    if (opts->payload == 1) {
        if (svc & 0x20) {
            DSD_FPRINTF(stderr, " Duplex");
        }
        if (svc & 0x10) {
            DSD_FPRINTF(stderr, " Packet");
        } else {
            DSD_FPRINTF(stderr, " Circuit");
        }
        if (svc & 0x8) {
            DSD_FPRINTF(stderr, " R");
        }
        DSD_FPRINTF(stderr, " Priority %d", svc & 0x7);
        state->p25_call_priority[0] = (uint8_t)(svc & 0x7);
    } else {
        state->p25_call_priority[0] = 0;
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_net_sts_broadcast(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    int lra = mpdu_byte[3];
    int sysid = ((mpdu_byte[4] & 0xF) << 8) | mpdu_byte[5];
    int res_a = mpdu_byte[8];
    int res_b = mpdu_byte[9];
    long int wacn = (mpdu_byte[12] << 12) | (mpdu_byte[13] << 4) | (mpdu_byte[14] >> 4);
    int channelt = (mpdu_byte[15] << 8) | mpdu_byte[16];
    int channelr = (mpdu_byte[17] << 8) | mpdu_byte[18];
    int ssc = mpdu_byte[19];
    UNUSED3(res_a, res_b, ssc);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Network Status Broadcast MBT - Extended \n");
    DSD_FPRINTF(stderr, "  LRA [%02X] WACN [%05lX] SYSID [%03X] NAC [%03llX]\n", lra, wacn, sysid, state->p2_cc);
    DSD_FPRINTF(stderr, "  CHAN-T [%04X] CHAN-R [%04X]", channelt, channelr);

    long int ct_freq = process_channel_to_freq(opts, state, channelt);
    long int cr_freq = process_channel_to_freq(opts, state, channelr);
    UNUSED(cr_freq);

    int accepted_cc = p25_cc_update_primary_from_network_status(opts, state, ct_freq);
    const int cc_metadata_allowed = accepted_cc || !p25_cc_update_is_voice_tuned(opts);
    if (cc_metadata_allowed) {
        if (state->p2_hardset == 0) {
            (void)p25_update_system_identity(state, (unsigned long long)wacn, (unsigned long long)sysid);
        }

        p25_store_site_lra(state, (uint8_t)lra);
        state->p25_cc_is_tdma = 0;
    }

    if (accepted_cc) {
        if (state->trunk_lcn_freq[0] == 0 || state->trunk_lcn_freq[0] != state->p25_cc_freq) {
            state->trunk_lcn_freq[0] = state->p25_cc_freq;
        }

        const long neigh[1] = {ct_freq};
        p25_sm_on_neighbor_update(opts, state, neigh, 1);
        p25_confirm_idens_for_current_site(state);
    } else if (ct_freq > 0) {
        DSD_FPRINTF(stderr, "\n  P25 MBT NET_STS: ignoring CC update while voice-tuned (freq=%ld)", ct_freq);
    } else {
        DSD_FPRINTF(stderr, "\n  P25 MBT NET_STS: ignoring invalid channel->freq (CHAN-T=%04X)", channelt);
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_rfss_status_broadcast(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    int lra = mpdu_byte[3];
    int lsysid = ((mpdu_byte[4] & 0xF) << 8) | mpdu_byte[5];
    int rfssid = mpdu_byte[12];
    int siteid = mpdu_byte[13];
    int channelt = (mpdu_byte[14] << 8) | mpdu_byte[15];
    int channelr = (mpdu_byte[16] << 8) | mpdu_byte[17];
    int sysclass = mpdu_byte[18];

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n RFSS Status Broadcast MBF - Extended \n");
    DSD_FPRINTF(stderr,
                "  LRA [%02X] SYSID [%03X] RFSS ID [%03d] SITE ID [%03d]\n  CHAN-T [%04X] CHAN-R [%02X] SSC [%02X] ",
                lra, lsysid, rfssid, siteid, channelt, channelr, sysclass);

    long int f1 = process_channel_to_freq(opts, state, channelt);
    (void)process_channel_to_freq(opts, state, channelr);

    const long neigh2[1] = {f1};
    p25_sm_on_neighbor_update(opts, state, neigh2, 1);

    if (state->p2_sysid != 0 && (uint16_t)lsysid != state->p2_sysid) {
        return;
    }

    state->p2_siteid = siteid;
    state->p2_rfssid = rfssid;
    p25_confirm_idens_for_current_site(state);
}

static void DSD_ATTR_USED
p25_handle_mbt_adjacent_status_broadcast(const dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    int lra = mpdu_byte[3];
    int cfva = mpdu_byte[4] >> 4;
    int lsysid = ((mpdu_byte[4] & 0xF) << 8) | mpdu_byte[5];
    int rfssid = mpdu_byte[8];
    int siteid = mpdu_byte[9];
    int channelt = (mpdu_byte[12] << 8) | mpdu_byte[13];

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Adjacent Status Broadcast - Extended\n");
    DSD_FPRINTF(stderr, "  LRA [%02X] SYSID [%03X] RFSS[%03d] SITE [%03d] CHAN-T [%04X]\n  ", lra, lsysid, rfssid,
                siteid, channelt);
    char cfva_buf[96];
    if (p25_format_adjacent_cfva((uint8_t)cfva, cfva_buf, sizeof cfva_buf) > 0U) {
        DSD_FPRINTF(stderr, "%s", cfva_buf);
    }

    const p25_neighbor_channel_announcement_t announcement = {
        .channel = (uint16_t)channelt,
        .sysid = (uint16_t)lsysid,
        .rfss = (uint8_t)rfssid,
        .site = (uint8_t)siteid,
        .lra = (uint8_t)lra,
        .cfva = (uint8_t)cfva,
        .lra_valid = 1U,
        .cfva_valid = 1U,
    };
    (void)p25_announce_neighbor_channel_ex(opts, state, &announcement);
}

static void DSD_ATTR_USED
p25_handle_mbt_protection_parameter_broadcast(dsd_state* state, const uint8_t* mpdu_byte) {
    uint8_t algid = mpdu_byte ? mpdu_byte[9] : 0;
    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Protection Parameter Broadcast MBT - protected CC ALGID [%02X]", algid);
    p25_store_protected_control_channel(state, algid);
}

static void DSD_ATTR_USED
p25_handle_mbt_tdma_iden_foreign_system(const uint8_t* mpdu_byte) {
    int iden = (mpdu_byte[3] >> 4) & 0x0F;
    int chan_type = mpdu_byte[3] & 0x0F;
    long int lwacn = ((long)mpdu_byte[4] << 12) | ((long)mpdu_byte[5] << 4) | ((mpdu_byte[8] & 0xF0) >> 4);
    int lsysid = ((mpdu_byte[8] & 0x0F) << 8) | mpdu_byte[9];
    long int base_freq =
        ((long)mpdu_byte[12] << 24) | ((long)mpdu_byte[13] << 16) | ((long)mpdu_byte[14] << 8) | (long)mpdu_byte[15];
    int tx_off_sign = (mpdu_byte[16] >> 7) & 1;
    int tx_off_raw = ((mpdu_byte[16] & 0x7F) << 6) | (mpdu_byte[17] >> 2);
    int chan_spac = ((mpdu_byte[17] & 0x3) << 8) | mpdu_byte[18];
    int trans_off = p25_signed_offset_units(tx_off_sign, tx_off_raw);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n TDMA Identifier Update MBT - foreign system, not applied\n");
    DSD_FPRINTF(stderr, "  IDEN [%X] Type [%X] Base Freq [%ld] (%ld Hz) TX Offset [%d] Spacing [%d]", iden, chan_type,
                base_freq, base_freq * 5, trans_off, chan_spac);
    DSD_FPRINTF(stderr, "\n  Foreign WACN [%05lX] SYSID [%03X] - ignored for current IDEN tables", lwacn, lsysid);
}

static void DSD_ATTR_USED
p25_handle_mbt_group_voice_grant(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    int svc = mpdu_byte[8];
    int channelt = (mpdu_byte[14] << 8) | mpdu_byte[15];
    int channelr = (mpdu_byte[16] << 8) | mpdu_byte[17];
    long int source = (mpdu_byte[3] << 16) | (mpdu_byte[4] << 8) | mpdu_byte[5];
    int group = (mpdu_byte[18] << 8) | mpdu_byte[19];
    long int freq1 = 0;
    long int freq2 = 0;
    UNUSED(freq2);

    DSD_FPRINTF(stderr, "%s\n ", KYEL);
    p25_print_voice_svc_common(opts, state, svc);

    DSD_FPRINTF(stderr, " Group Voice Channel Grant Update - Extended");
    DSD_FPRINTF(stderr, "\n  SVC [%02X] CHAN-T [%04X] CHAN-R [%04X] Group [%d][%04X]", svc, channelt, channelr, group,
                group);

    freq1 = process_channel_to_freq(opts, state, channelt);
    (void)process_channel_to_freq(opts, state, channelr);

    char suf1[32];
    p25_format_chan_suffix(state, channelt, -1, suf1, sizeof(suf1));
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Ch: %04X%s TG: %d; ", channelt,
                 suf1, group);
    state->last_active_time = time(NULL);

    p25p1_pdu_print_group_label(state, (uint32_t)group);

    dsd_tg_policy_decision decision;
    int enc_for_policy = (svc & 0x40) ? 1 : 0;
    if (enc_for_policy && opts->trunk_tune_enc_calls == 0
        && (p25_patch_tg_key_is_clear(state, group) || p25_patch_sg_key_is_clear(state, group))) {
        enc_for_policy = 0;
    }

    if (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)group, (uint32_t)source, enc_for_policy,
                                          (svc & 0x10) ? 1 : 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
            != 0
        || !decision.tune_allowed) {
        if (decision.block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) {
            p25_emit_enc_lockout_once(opts, state, 0, group, svc);
        }
        return;
    }

    if (p25p1_pdu_can_tune_grant(opts, state, freq1)) {
        p25_sm_on_group_grant(opts, state, channelt, svc, group, (int)source);
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_unit_to_unit_voice_grant(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    int svc = mpdu_byte[8];
    int channelt = (mpdu_byte[22] << 8) | mpdu_byte[23];
    int channelr = (mpdu_byte[24] << 8) | mpdu_byte[25];
    long int source = (mpdu_byte[3] << 16) | (mpdu_byte[4] << 8) | mpdu_byte[5];
    long int target = (mpdu_byte[19] << 16) | (mpdu_byte[20] << 8) | mpdu_byte[21];
    long int src_nid = (mpdu_byte[12] << 24) | (mpdu_byte[13] << 16) | (mpdu_byte[14] << 8) | mpdu_byte[15];
    long int src_sid = (mpdu_byte[16] << 16) | (mpdu_byte[17] << 8) | mpdu_byte[18];
    long int tgt_nid = (mpdu_byte[26] << 16) | (mpdu_byte[27] << 8) | mpdu_byte[28];
    long int tgt_sid = (mpdu_byte[29] << 16) | (mpdu_byte[30] << 8) | mpdu_byte[31];
    long int freq1 = 0;
    long int freq2 = 0;
    UNUSED(freq2);

    DSD_FPRINTF(stderr, "%s\n ", KYEL);
    p25_print_voice_svc_common(opts, state, svc);

    DSD_FPRINTF(stderr, " Unit to Unit Voice Channel Grant Update - Extended");
    DSD_FPRINTF(stderr,
                "\n  SVC: %02X; CHAN-T: %04X; CHAN-R: %04X; SRC: %ld; TGT: %ld; FULL SRC: %08lX-%08ld; FULL TGT: "
                "%08lX-%08ld;",
                svc, channelt, channelr, source, target, src_nid, src_sid, tgt_nid, tgt_sid);

    freq1 = process_channel_to_freq(opts, state, channelt);
    (void)process_channel_to_freq(opts, state, channelr);

    char suf2[32];
    p25_format_chan_suffix(state, channelt, -1, suf2, sizeof(suf2));
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Ch: %04X%s TGT: %u; ", channelt,
                 suf2, (uint32_t)target);

    p25p1_pdu_print_group_label(state, (uint32_t)target);

    dsd_tg_policy_decision decision;
    if (dsd_tg_policy_evaluate_private_call(opts, state, (uint32_t)source, (uint32_t)target, (svc & 0x40) ? 1 : 0,
                                            (svc & 0x10) ? 1 : 0, DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                            DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
            != 0
        || !decision.tune_allowed) {
        return;
    }

    if (p25p1_pdu_can_tune_grant(opts, state, freq1)) {
        p25_sm_on_indiv_grant(opts, state, channelt, svc, (int)target, (int)source);
    }
}

static int DSD_ATTR_USED
p25_telephone_call_policy_allows(const dsd_opts* opts, const dsd_state* state, uint32_t target, int svc) {
    dsd_tg_policy_decision decision;
    return dsd_tg_policy_evaluate_private_call(opts, state, 0, target, (svc & 0x40) ? 1 : 0, (svc & 0x10) ? 1 : 0,
                                               DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                               DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
               == 0
           && decision.tune_allowed;
}

static void DSD_ATTR_USED
p25_telephone_update_nontrunk_vc_freq(const dsd_opts* opts, dsd_state* state, uint32_t target, long int freq) {
    if (opts->p25_trunk != 0) {
        return;
    }

    if ((int)target != state->lasttg && (int)target != state->lasttgR) {
        return;
    }

    if (DSD_SYNC_IS_P25P1(state->synctype)) {
        state->p25_vc_freq[0] = freq;
    } else {
        state->p25_vc_freq[0] = state->p25_vc_freq[1] = freq;
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_telephone_interconnect_grant(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte,
                                            uint8_t opcode) {
    int svc = mpdu_byte[8];
    int channel = (mpdu_byte[12] << 8) | mpdu_byte[13];
    int timer = (mpdu_byte[16] << 8) | mpdu_byte[17];
    uint32_t target = (uint32_t)((mpdu_byte[3] << 16) | (mpdu_byte[4] << 8) | mpdu_byte[5]);
    long int freq = 0;

    DSD_FPRINTF(stderr, "\n");
    p25_print_voice_svc_common(opts, state, svc);

    DSD_FPRINTF(stderr, " Telephone Interconnect Voice Channel Grant");
    if (opcode & 1) {
        DSD_FPRINTF(stderr, " Update");
    }
    DSD_FPRINTF(stderr, " Extended");
    DSD_FPRINTF(stderr, "\n  CHAN: %04X; Timer: %f Seconds; Target: %u;", channel, (float)timer * 0.1f, target);

    freq = process_channel_to_freq(opts, state, channel);

    if (channel != 0 && channel != 0xFFFF) {
        DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Tele Ch: %04X TGT: %u; ",
                     channel, target);
    }
    state->last_active_time = time(NULL);

    p25p1_pdu_print_group_label(state, target);

    if (!p25_telephone_call_policy_allows(opts, state, target, svc)) {
        return;
    }

    if (p25p1_pdu_can_tune_grant(opts, state, freq)) {
        p25_sm_on_indiv_grant(opts, state, channel, svc, (int)target, 0);
    }

    p25_telephone_update_nontrunk_vc_freq(opts, state, target, freq);
}

static void DSD_ATTR_USED
p25_handle_mbt_mfid_a4(const uint8_t* mpdu_byte, int blks, uint8_t opcode) {
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n MFID A4 (Harris); Opcode: %02X; ", opcode);
    p25_print_mbt_payload_hex(mpdu_byte, blks);
    DSD_FPRINTF(stderr, " %s", KNRM);
}

static void DSD_ATTR_USED
p25_handle_mbt_mfid90_group_regroup(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    int svc = mpdu_byte[8];
    int channelt = (mpdu_byte[12] << 8) | mpdu_byte[13];
    int channelr = (mpdu_byte[14] << 8) | mpdu_byte[15];
    long int source = (mpdu_byte[3] << 16) | (mpdu_byte[4] << 8) | mpdu_byte[5];
    int group = (mpdu_byte[16] << 8) | mpdu_byte[17];
    long int freq1 = 0;
    long int freq2 = 0;
    UNUSED(freq2);

    DSD_FPRINTF(stderr, "%s\n ", KYEL);
    if (svc & 0x40) {
        DSD_FPRINTF(stderr, " Encrypted");
    }

    DSD_FPRINTF(stderr, " MFID90 Group Regroup Channel Grant - Explicit");
    DSD_FPRINTF(stderr, "\n  RES/P [%02X] CHAN-T [%04X] CHAN-R [%04X] SG [%d][%04X]", svc, channelt, channelr, group,
                group);

    freq1 = process_channel_to_freq(opts, state, channelt);
    (void)process_channel_to_freq(opts, state, channelr);

    char suf3[32];
    p25_format_chan_suffix(state, channelt, -1, suf3, sizeof(suf3));
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MFID90 Ch: %04X%s SG: %d ", channelt,
                 suf3, group);
    state->last_active_time = time(NULL);

    p25p1_pdu_print_group_label(state, (uint32_t)group);

    dsd_tg_policy_decision decision;
    int enc_for_policy = (svc & 0x40) ? 1 : 0;
    if (enc_for_policy && opts->trunk_tune_enc_calls == 0 && p25_patch_sg_key_is_clear(state, group)) {
        enc_for_policy = 0;
    }

    if (dsd_tg_policy_evaluate_group_call(opts, state, (uint32_t)group, (uint32_t)source, enc_for_policy,
                                          (svc & 0x10) ? 1 : 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
            != 0
        || !decision.tune_allowed) {
        if (decision.block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) {
            p25_emit_enc_lockout_once(opts, state, 0, group, svc);
        }
        return;
    }

    if (p25p1_pdu_can_tune_grant(opts, state, freq1)) {
        p25_sm_on_group_grant(opts, state, channelt, svc, group, (int)source);
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_group_affiliation_response(dsd_state* state, const uint8_t* mpdu_byte) {
    uint32_t ta = ((uint32_t)mpdu_byte[3] << 16) | ((uint32_t)mpdu_byte[4] << 8) | (uint32_t)mpdu_byte[5];
    int mfid = mpdu_byte[2];
    int wacn = (mpdu_byte[8] << 12) | (mpdu_byte[9] << 4) | ((mpdu_byte[12] & 0xF0) >> 4);
    int sysid = ((mpdu_byte[12] & 0x0F) << 8) | mpdu_byte[13];
    int gid = (mpdu_byte[14] << 8) | mpdu_byte[15];
    int aga = (mpdu_byte[16] << 8) | mpdu_byte[17];
    int ga = (mpdu_byte[18] << 8) | mpdu_byte[19];
    int lg = (mpdu_byte[20] >> 7) & 0x01;
    int gav = mpdu_byte[20] & 0x03;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Group Affiliation Response MBT - Extended");
    DSD_FPRINTF(stderr, "\n  MFID [%02X] WACN [%05X] SYSID [%03X] GID [%04X] LG [%d] GAV [%d] AGA [%d] GA [%d] TA [%u]",
                mfid, wacn, sysid, gid, lg, gav, aga, ga, ta);

    if (gav == 0) {
        p25_aff_register(state, ta);
        p25_ga_add(state, ta, (uint16_t)ga);
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_mfid90_unknown(const uint8_t* mpdu_byte, int blks) {
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n MFID 90 (Moto); Opcode: %02X; ", mpdu_byte[0] & 0x3F);
    p25_print_mbt_payload_hex(mpdu_byte, blks);
    DSD_FPRINTF(stderr, " %s", KNRM);
}

static void DSD_ATTR_USED
p25_handle_mbt_unknown_mfid(const uint8_t* mpdu_byte, int blks, uint8_t mfid, uint8_t opcode) {
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n MFID %02X (Unknown); Opcode: %02X; ", mfid, opcode);
    p25_print_mbt_payload_hex(mpdu_byte, blks);
    DSD_FPRINTF(stderr, " %s", KNRM);
}

//trunking data delivered via PDU format
void
p25_decode_pdu_trunking(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    p25p1_mbt_fields fields = p25_parse_mbt_fields(mpdu_byte);

    p25_print_mbt_header(&fields);
    p25_mbt_try_bridge_iden_updates(opts, state, mpdu_byte, &fields);

    if (p25_mbt_has_unsupported_survey_format(&fields)) {
        DSD_FPRINTF(stderr, " - broadcast format %02X not handled", fields.fmt);
        return;
    }

    if (fields.opcode == 0x3B) {
        p25_handle_mbt_net_sts_broadcast(opts, state, mpdu_byte);
    } else if (fields.opcode == 0x3A) {
        p25_handle_mbt_rfss_status_broadcast(opts, state, mpdu_byte);
    } else if (fields.opcode == 0x3C) {
        p25_handle_mbt_adjacent_status_broadcast(opts, state, mpdu_byte);
    } else if (fields.opcode == 0x3E) {
        p25_handle_mbt_protection_parameter_broadcast(state, mpdu_byte);
    } else if (fields.opcode == 0x33) {
        p25_handle_mbt_tdma_iden_foreign_system(mpdu_byte);
    } else if (fields.opcode == 0x0) {
        p25_handle_mbt_group_voice_grant(opts, state, mpdu_byte);
    } else if (fields.opcode == 0x6) {
        p25_handle_mbt_unit_to_unit_voice_grant(opts, state, mpdu_byte);
    } else if ((fields.opcode == 0x8 || fields.opcode == 0x9) && fields.mfid < 2) {
        p25_handle_mbt_telephone_interconnect_grant(opts, state, mpdu_byte, fields.opcode);
    } else if (fields.opcode == 0x28) {
        if (fields.fmt == 0x17) {
            p25_handle_mbt_group_affiliation_response(state, mpdu_byte);
        } else {
            DSD_FPRINTF(stderr, " - group affiliation response format %02X not handled", fields.fmt);
        }
    } else if (fields.mfid == 0xA4) {
        p25_handle_mbt_mfid_a4(mpdu_byte, fields.blks, fields.opcode);
    } else if (fields.mfid == 0x90) {
        if (fields.opcode == 0x02) {
            p25_handle_mbt_mfid90_group_regroup(opts, state, mpdu_byte);
        } else {
            p25_handle_mbt_mfid90_unknown(mpdu_byte, fields.blks);
        }
    } else {
        p25_handle_mbt_unknown_mfid(mpdu_byte, fields.blks, fields.mfid, mpdu_byte[0] & 0x3F);
    }
}
