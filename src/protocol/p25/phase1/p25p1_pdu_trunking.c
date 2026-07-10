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
#include <dsd-neo/protocol/p25/p25_cc_activity.h>
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
    uint8_t direction;
    int is_outbound;
    uint8_t mfid;
    int blks;
    uint8_t opcode;
} p25p1_mbt_fields;

enum {
    P25_MBT_AMBTC_OPCODE_INDEX = 7,
    P25_MBT_UMBTC_OPCODE_INDEX = 12,
    P25_MBT_LEGACY_MAX_LEN = 48,
};

// NOLINTNEXTLINE(misc-use-internal-linkage)
int p25_decode_pdu_trunking_bounded(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len);

static int
p25_mbt_opcode_index(uint8_t fmt) {
    /*
     * Alternate MBTC (0x17) carries the opcode in the PDU header. Unconfirmed
     * MBTC (0x15) carries a data header at the start of block 0; in the
     * contiguous MPDU buffer that opcode is byte 12 and payload starts at 13.
     */
    return (fmt == 0x17) ? P25_MBT_AMBTC_OPCODE_INDEX : P25_MBT_UMBTC_OPCODE_INDEX;
}

static size_t
p25_mbt_declared_len(const uint8_t* mpdu_byte) {
    if (!mpdu_byte) {
        return 0U;
    }

    size_t declared = ((size_t)(mpdu_byte[6] & 0x7FU) + 1U) * 12U;
    return declared <= P25_MBT_LEGACY_MAX_LEN ? declared : P25_MBT_LEGACY_MAX_LEN;
}

static int
p25_parse_mbt_fields_checked(const uint8_t* mpdu_byte, size_t mpdu_len, p25p1_mbt_fields* fields) {
    if (!mpdu_byte || !fields || mpdu_len <= 6U) {
        return 0;
    }

    fields->fmt = mpdu_byte[0] & 0x1F;
    fields->direction = (uint8_t)((mpdu_byte[0] >> 5) & 0x01U);
    fields->is_outbound = fields->direction != 0U;
    fields->mfid = mpdu_byte[2];
    fields->blks = mpdu_byte[6] & 0x7F;

    int op_idx = p25_mbt_opcode_index(fields->fmt);
    if (op_idx < 0 || (size_t)op_idx >= mpdu_len) {
        return 0;
    }
    fields->opcode = mpdu_byte[op_idx] & 0x3F;
    return 1;
}

static void
p25_print_mbt_header(const p25p1_mbt_fields* fields) {
    if (fields->fmt == 0x15) {
        DSD_FPRINTF(stderr, " UNC");
    } else {
        DSD_FPRINTF(stderr, " ALT");
    }
    DSD_FPRINTF(stderr, " MBT");
    DSD_FPRINTF(stderr, fields->is_outbound ? " OSP" : " ISP");
    DSD_FPRINTF(stderr, " - OP: %02X", fields->opcode);
}

static void
p25_print_mbt_payload_hex(const uint8_t* mpdu_byte, int blks, size_t mpdu_len) {
    size_t limit = (size_t)((12 * (blks + 1)) % 37);
    if (limit > mpdu_len) {
        limit = mpdu_len;
    }
    for (size_t i = 0; i < limit; i++) {
        DSD_FPRINTF(stderr, "%02X", mpdu_byte[i]);
    }
}

static int
p25_mbt_require_len(const p25p1_mbt_fields* fields, size_t mpdu_len, size_t needed, const char* label) {
    if (mpdu_len >= needed) {
        return 1;
    }
    DSD_FPRINTF(stderr, " - %s short payload (fmt %02X op %02X need %zu got %zu)", label ? label : "MBT",
                fields ? fields->fmt : 0U, fields ? fields->opcode : 0U, needed, mpdu_len);
    return 0;
}

static uint16_t
p25_mbt_u16(const uint8_t* mpdu_byte, size_t off) {
    return (uint16_t)(((uint16_t)mpdu_byte[off] << 8) | (uint16_t)mpdu_byte[off + 1U]);
}

static uint32_t
p25_mbt_u24(const uint8_t* mpdu_byte, size_t off) {
    return ((uint32_t)mpdu_byte[off] << 16) | ((uint32_t)mpdu_byte[off + 1U] << 8) | (uint32_t)mpdu_byte[off + 2U];
}

static void
p25_mbt_decode_header_addr(const uint8_t* mpdu_byte, uint32_t* out_addr) {
    if (out_addr) {
        *out_addr = p25_mbt_u24(mpdu_byte, 3U);
    }
}

static void
p25_mbt_decode_source_fq_high16(const uint8_t* mpdu_byte, uint32_t* out_local, uint32_t* out_wacn, uint16_t* out_sysid,
                                uint32_t* out_id) {
    if (out_local) {
        *out_local = ((uint32_t)mpdu_byte[22] << 16) | ((uint32_t)mpdu_byte[23] << 8) | (uint32_t)mpdu_byte[24];
    }
    if (out_wacn) {
        *out_wacn = ((uint32_t)mpdu_byte[8] << 12) | ((uint32_t)mpdu_byte[9] << 4) | ((uint32_t)mpdu_byte[12] >> 4);
    }
    if (out_sysid) {
        *out_sysid = (uint16_t)(((uint16_t)(mpdu_byte[12] & 0x0F) << 8) | (uint16_t)mpdu_byte[13]);
    }
    if (out_id) {
        *out_id = p25_mbt_u24(mpdu_byte, 14U);
    }
}

static void
p25_mbt_decode_target_fq_block1(const uint8_t* mpdu_byte, uint32_t* out_local, uint32_t* out_wacn, uint16_t* out_sysid,
                                uint32_t* out_id) {
    if (out_local) {
        *out_local = p25_mbt_u24(mpdu_byte, 3U);
    }
    if (out_wacn) {
        *out_wacn = ((uint32_t)mpdu_byte[25] << 12) | ((uint32_t)mpdu_byte[26] << 4) | ((uint32_t)mpdu_byte[27] >> 4);
    }
    if (out_sysid) {
        *out_sysid = (uint16_t)(((uint16_t)(mpdu_byte[27] & 0x0F) << 8) | (uint16_t)mpdu_byte[28]);
    }
    if (out_id) {
        *out_id = p25_mbt_u24(mpdu_byte, 29U);
    }
}

static void
p25_mbt_print_fq_radio(const char* prefix, uint32_t local, uint32_t wacn, uint16_t sysid, uint32_t id) {
    DSD_FPRINTF(stderr, " %s [%u] FULL [%05X.%03X.%06X]", prefix, local, wacn, sysid, id);
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
p25_mbt_mfid_is_standard(uint8_t mfid) {
    return mfid < 2U;
}

static int
p25_mbt_is_ambtc(const p25p1_mbt_fields* fields) {
    return fields && fields->fmt == 0x17;
}

static int
p25p1_pdu_can_tune_grant(const dsd_opts* opts, dsd_state* state, long int freq) {
    if (!opts || !state || opts->p25_trunk != 1 || opts->p25_is_tuned != 0 || freq == 0) {
        return 0;
    }
    p25_sm_seed_cc_from_current_tuner_if_unknown(opts, state);
    return state->p25_cc_freq != 0;
}

static void
p25p1_pdu_dispatch_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int group, int src,
                               long int freq) {
    // The trunk state machine owns group-grant policy so patched supergroup
    // grants can be evaluated against their member talkgroups.
    if (p25p1_pdu_can_tune_grant(opts, state, freq)) {
        p25_sm_on_group_grant(opts, state, channel, svc_bits, group, src);
    } else {
        p25_sm_apply_group_grant_policy(opts, state, channel, svc_bits, group, src);
    }
}

static void DSD_ATTR_USED
p25_mbt_try_bridge_iden_updates(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len,
                                const p25p1_mbt_fields* fields) {
    if (!fields || !fields->is_outbound
        || !(p25_mbt_is_bridgeable_iden_update(fields->opcode) && p25_mbt_mfid_is_standard(fields->mfid))) {
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
    size_t total_len = 12U * (size_t)(fields->blks + 1);
    if (total_len > mpdu_len) {
        total_len = mpdu_len;
    }
    unsigned long long int MAC[24] = {0};

    uint8_t mac_opcode = fields->opcode;
    if ((mac_opcode & 0xC0) == 0x00) {
        mac_opcode |= 0x40;
    }
    MAC[1] = mac_opcode;

    int mac_i = 2;
    for (size_t i = (size_t)payload_off; i < total_len && mac_i < 24; i++, mac_i++) {
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
    int svc_bits = mpdu_byte[8];
    int channelt = (mpdu_byte[14] << 8) | mpdu_byte[15];
    int channelr = (mpdu_byte[16] << 8) | mpdu_byte[17];
    long int src = (mpdu_byte[3] << 16) | (mpdu_byte[4] << 8) | mpdu_byte[5];
    int group = (mpdu_byte[18] << 8) | mpdu_byte[19];
    long int freq1 = 0;
    long int freq2 = 0;
    UNUSED(freq2);

    DSD_FPRINTF(stderr, "%s\n ", KYEL);
    p25_print_voice_svc_common(opts, state, svc_bits);

    DSD_FPRINTF(stderr, " Group Voice Channel Grant Update - Extended");
    DSD_FPRINTF(stderr, "\n  SVC [%02X] CHAN-T [%04X] CHAN-R [%04X] Group [%d][%04X]", svc_bits, channelt, channelr,
                group, group);

    freq1 = process_channel_to_freq(opts, state, channelt);
    (void)process_channel_to_freq(opts, state, channelr);

    char suf1[32];
    p25_format_chan_suffix(state, channelt, -1, suf1, sizeof(suf1));
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active Ch: %04X%s TG: %d; ", channelt,
                 suf1, group);
    state->last_active_time = time(NULL);

    p25p1_pdu_print_group_label(state, (uint32_t)group);

    p25p1_pdu_dispatch_group_grant(opts, state, channelt, svc_bits, group, (int)src, freq1);
}

static void DSD_ATTR_USED
p25_handle_mbt_unit_to_unit_voice_grant(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len,
                                        uint8_t opcode) {
    const int has_block1 = mpdu_len >= 32U;
    uint8_t svc = mpdu_byte[8];
    uint16_t channelt = (uint16_t)(((uint16_t)mpdu_byte[22] << 8) | (uint16_t)mpdu_byte[23]);
    uint16_t channelr = has_block1 ? (uint16_t)(((uint16_t)mpdu_byte[24] << 8) | (uint16_t)mpdu_byte[25]) : 0xFFFFU;
    uint32_t source = ((uint32_t)mpdu_byte[3] << 16) | ((uint32_t)mpdu_byte[4] << 8) | (uint32_t)mpdu_byte[5];
    uint32_t target = ((uint32_t)mpdu_byte[19] << 16) | ((uint32_t)mpdu_byte[20] << 8) | (uint32_t)mpdu_byte[21];
    uint32_t src_wacn =
        ((uint32_t)mpdu_byte[12] << 12) | ((uint32_t)mpdu_byte[13] << 4) | ((uint32_t)mpdu_byte[14] >> 4);
    uint16_t src_sys = (uint16_t)(((uint16_t)(mpdu_byte[14] & 0x0F) << 8) | (uint16_t)mpdu_byte[15]);
    uint32_t src_id = ((uint32_t)mpdu_byte[16] << 16) | ((uint32_t)mpdu_byte[17] << 8) | (uint32_t)mpdu_byte[18];
    uint32_t tgt_wacn = 0;
    uint16_t tgt_sys = 0;
    uint32_t tgt_id = target;
    long int freq1 = 0;

    if (has_block1) {
        tgt_wacn = ((uint32_t)mpdu_byte[9] << 12) | ((uint32_t)mpdu_byte[26] << 4) | ((uint32_t)mpdu_byte[27] >> 4);
        tgt_sys = (uint16_t)(((uint16_t)(mpdu_byte[27] & 0x0F) << 8) | (uint16_t)mpdu_byte[28]);
        tgt_id = ((uint32_t)mpdu_byte[29] << 16) | ((uint32_t)mpdu_byte[30] << 8) | (uint32_t)mpdu_byte[31];
    }

    DSD_FPRINTF(stderr, "%s\n ", KYEL);
    p25_print_voice_svc_common(opts, state, svc);

    /*
     * AMBTC 0x04 (grant) and 0x06 (grant update) share the extended UU
     * layout used by SDRTrunk/Trunk Recorder: service and target-WACN high
     * bits in the header, source FQ + local target + downlink in block 0, and
     * uplink + target FQ in block 1.
     */
    DSD_FPRINTF(stderr, " Unit to Unit Voice Channel Grant");
    if (opcode == 0x06) {
        DSD_FPRINTF(stderr, " Update");
    }
    DSD_FPRINTF(stderr, " - Extended");
    DSD_FPRINTF(stderr, "\n  SVC [%02X] CHAN-T [%04X]", svc, channelt);
    if (has_block1) {
        DSD_FPRINTF(stderr, " CHAN-R [%04X]", channelr);
    } else {
        DSD_FPRINTF(stderr, " CHAN-R [----]");
    }
    DSD_FPRINTF(stderr, " SRC [%u] TGT [%u] FULL SRC [%05X.%03X.%06X]", source, target, src_wacn, src_sys, src_id);
    if (has_block1) {
        DSD_FPRINTF(stderr, " FULL TGT [%05X.%03X.%06X]", tgt_wacn, tgt_sys, tgt_id);
    } else {
        DSD_FPRINTF(stderr, " FULL TGT [unavailable]");
    }

    freq1 = process_channel_to_freq(opts, state, channelt);
    if (has_block1) {
        (void)process_channel_to_freq(opts, state, channelr);
    }

    char suf2[32];
    p25_format_chan_suffix(state, channelt, -1, suf2, sizeof(suf2));
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "Active UU Ch: %04X%s SRC: %u TGT: %u; ",
                 channelt, suf2, source, target);
    state->last_active_time = time(NULL);

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

static void DSD_ATTR_USED
p25_handle_mbt_unit_to_unit_answer_request(const dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    int svc = mpdu_byte[8];
    uint32_t target = p25_mbt_u24(mpdu_byte, 3U);
    uint32_t src_wacn =
        ((uint32_t)mpdu_byte[13] << 12) | ((uint32_t)mpdu_byte[14] << 4) | ((uint32_t)mpdu_byte[15] >> 4);
    uint16_t src_sys = (uint16_t)(((uint16_t)(mpdu_byte[15] & 0x0F) << 8) | (uint16_t)mpdu_byte[16]);
    uint32_t src_id = p25_mbt_u24(mpdu_byte, 17U);

    DSD_FPRINTF(stderr, "%s\n ", KYEL);
    p25_print_voice_svc_common(opts, state, svc);
    DSD_FPRINTF(stderr, " Unit to Unit Answer Request MBT - Extended");
    DSD_FPRINTF(stderr, "\n  SVC [%02X] TO [%u] FULL SRC [%05X.%03X.%06X]", svc, target, src_wacn, src_sys, src_id);
}

static void DSD_ATTR_USED
p25_handle_mbt_extended_command_metadata(const uint8_t* mpdu_byte, uint8_t opcode) {
    const char* label = "Command";
    uint32_t src_local = 0;
    uint32_t src_wacn = 0;
    uint16_t src_sys = 0;
    uint32_t src_id = 0;
    uint32_t tgt_local = 0;
    uint32_t tgt_wacn = 0;
    uint16_t tgt_sys = 0;
    uint32_t tgt_id = 0;

    p25_mbt_decode_source_fq_high16(mpdu_byte, &src_local, &src_wacn, &src_sys, &src_id);
    p25_mbt_decode_target_fq_block1(mpdu_byte, &tgt_local, &tgt_wacn, &tgt_sys, &tgt_id);

    if (opcode == 0x18) {
        label = "Status Update";
    } else if (opcode == 0x1A) {
        label = "Status Query";
    } else if (opcode == 0x1C) {
        label = "Message Update";
    } else if (opcode == 0x1F) {
        label = "Call Alert";
    }

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n %s MBT - Extended", label);
    p25_mbt_print_fq_radio("FM", src_local, src_wacn, src_sys, src_id);
    p25_mbt_print_fq_radio("TO", tgt_local, tgt_wacn, tgt_sys, tgt_id);
    if (opcode == 0x18) {
        DSD_FPRINTF(stderr, " UNIT STATUS [%02X] USER STATUS [%02X]", mpdu_byte[17], mpdu_byte[18]);
    } else if (opcode == 0x1C) {
        DSD_FPRINTF(stderr, " SHORT DATA [%04X]", p25_mbt_u16(mpdu_byte, 17U));
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_group_affiliation_query(const uint8_t* mpdu_byte) {
    uint32_t target = 0;
    uint32_t src_wacn = 0;
    uint16_t src_sys = 0;
    uint32_t src_id = 0;

    p25_mbt_decode_header_addr(mpdu_byte, &target);
    src_wacn = ((uint32_t)mpdu_byte[8] << 12) | ((uint32_t)mpdu_byte[9] << 4) | ((uint32_t)mpdu_byte[12] >> 4);
    src_sys = (uint16_t)(((uint16_t)(mpdu_byte[12] & 0x0F) << 8) | (uint16_t)mpdu_byte[13]);
    src_id = p25_mbt_u24(mpdu_byte, 14U);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Group Affiliation Query MBT - Extended");
    DSD_FPRINTF(stderr, " TO [%u] FULL SRC [%05X.%03X.%06X]", target, src_wacn, src_sys, src_id);
}

static void DSD_ATTR_USED
p25_handle_mbt_roaming_address(const uint8_t* mpdu_byte, uint8_t opcode) {
    uint32_t target = p25_mbt_u24(mpdu_byte, 3U);
    int final = (mpdu_byte[8] & 0x80) ? 1 : 0;
    int msn = mpdu_byte[8] & 0x0F;
    uint32_t wacn_a = ((uint32_t)mpdu_byte[9] << 12) | ((uint32_t)mpdu_byte[12] << 4) | ((uint32_t)mpdu_byte[13] >> 4);
    uint16_t sys_a = (uint16_t)(((uint16_t)(mpdu_byte[13] & 0x0F) << 8) | (uint16_t)mpdu_byte[14]);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Roaming Address %s MBT - Extended", (opcode == 0x36) ? "Command" : "Update");
    DSD_FPRINTF(stderr, " TO [%u] MSN [%d] FINAL [%d] ADDR-A [%05X.%03X]", target, msn, final, wacn_a, sys_a);
}

static void DSD_ATTR_USED
p25_handle_mbt_individual_data_channel_grant(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte,
                                             size_t mpdu_len) {
    int has_uplink = mpdu_len >= 26U;
    uint8_t svc = mpdu_byte[8];
    uint32_t source = p25_mbt_u24(mpdu_byte, 3U);
    uint32_t src_wacn =
        ((uint32_t)mpdu_byte[12] << 12) | ((uint32_t)mpdu_byte[13] << 4) | ((uint32_t)mpdu_byte[14] >> 4);
    uint16_t src_sys = (uint16_t)(((uint16_t)(mpdu_byte[14] & 0x0F) << 8) | (uint16_t)mpdu_byte[15]);
    uint32_t src_id = p25_mbt_u24(mpdu_byte, 16U);
    uint32_t target = p25_mbt_u24(mpdu_byte, 19U);
    uint16_t channelt = p25_mbt_u16(mpdu_byte, 22U);
    uint16_t channelr = has_uplink ? p25_mbt_u16(mpdu_byte, 24U) : 0xFFFFU;
    long int freq = 0;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Individual Data Channel Grant MBT - Obsolete");
    DSD_FPRINTF(stderr, " SVC [%02X] SRC [%u] FULL SRC [%05X.%03X.%06X] TO [%u] CHAN-T [%04X]", svc, source, src_wacn,
                src_sys, src_id, target, channelt);
    if (has_uplink) {
        DSD_FPRINTF(stderr, " CHAN-R [%04X]", channelr);
    }

    freq = process_channel_to_freq(opts, state, channelt);
    if (has_uplink) {
        (void)process_channel_to_freq(opts, state, channelr);
    }

    dsd_tg_policy_decision decision;
    if (dsd_tg_policy_evaluate_private_call(opts, state, source, target, (svc & 0x40) ? 1 : 0, 1,
                                            DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                            DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
            != 0
        || !decision.tune_allowed) {
        return;
    }

    if (p25p1_pdu_can_tune_grant(opts, state, freq)) {
        p25_sm_on_indiv_data_grant(opts, state, channelt, svc, (int)target, (int)source);
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_group_data_channel_grant(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    uint8_t svc = mpdu_byte[8];
    uint32_t source = p25_mbt_u24(mpdu_byte, 3U);
    uint16_t channelt = p25_mbt_u16(mpdu_byte, 14U);
    uint16_t channelr = p25_mbt_u16(mpdu_byte, 16U);
    uint16_t group = p25_mbt_u16(mpdu_byte, 18U);
    long int freq = 0;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Group Data Channel Grant MBT - Obsolete");
    DSD_FPRINTF(stderr, " SVC [%02X] SRC [%u] CHAN-T [%04X] CHAN-R [%04X] Group [%d][%04X]", svc, source, channelt,
                channelr, group, group);

    freq = process_channel_to_freq(opts, state, channelt);
    (void)process_channel_to_freq(opts, state, channelr);

    if (p25p1_pdu_can_tune_grant(opts, state, freq)) {
        p25_sm_on_group_data_grant(opts, state, channelt, svc, (int)group, (int)source);
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
p25_handle_mbt_mfid_a4(const uint8_t* mpdu_byte, int blks, uint8_t opcode, size_t mpdu_len) {
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n MFID A4 (Harris); Opcode: %02X; ", opcode);
    p25_print_mbt_payload_hex(mpdu_byte, blks, mpdu_len);
    DSD_FPRINTF(stderr, " %s", KNRM);
}

static void DSD_ATTR_USED
p25_handle_mbt_mfid90_group_regroup(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    int svc_bits = mpdu_byte[8];
    int channelt = (mpdu_byte[12] << 8) | mpdu_byte[13];
    int channelr = (mpdu_byte[14] << 8) | mpdu_byte[15];
    long int src = (mpdu_byte[3] << 16) | (mpdu_byte[4] << 8) | mpdu_byte[5];
    int group = (mpdu_byte[16] << 8) | mpdu_byte[17];
    long int freq1 = 0;
    long int freq2 = 0;
    UNUSED(freq2);

    DSD_FPRINTF(stderr, "%s\n ", KYEL);
    if (svc_bits & 0x40) {
        DSD_FPRINTF(stderr, " Encrypted");
    }

    DSD_FPRINTF(stderr, " MFID90 Group Regroup Channel Grant - Explicit");
    DSD_FPRINTF(stderr, "\n  RES/P [%02X] CHAN-T [%04X] CHAN-R [%04X] SG [%d][%04X]", svc_bits, channelt, channelr,
                group, group);

    freq1 = process_channel_to_freq(opts, state, channelt);
    (void)process_channel_to_freq(opts, state, channelr);

    char suf3[32];
    p25_format_chan_suffix(state, channelt, -1, suf3, sizeof(suf3));
    DSD_SNPRINTF(state->active_channel[0], sizeof(state->active_channel[0]), "MFID90 Ch: %04X%s SG: %d ", channelt,
                 suf3, group);
    state->last_active_time = time(NULL);

    p25p1_pdu_print_group_label(state, (uint32_t)group);

    p25p1_pdu_dispatch_group_grant(opts, state, channelt, svc_bits, group, (int)src, freq1);
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
p25_handle_mbt_unit_registration_response(dsd_state* state, const uint8_t* mpdu_byte) {
    uint32_t src = ((uint32_t)mpdu_byte[3] << 16) | ((uint32_t)mpdu_byte[4] << 8) | (uint32_t)mpdu_byte[5];
    int mfid = mpdu_byte[2];
    int wacn = (mpdu_byte[8] << 12) | (mpdu_byte[9] << 4) | ((mpdu_byte[12] & 0xF0) >> 4);
    int sysid = ((mpdu_byte[12] & 0x0F) << 8) | mpdu_byte[13];
    uint32_t sid = ((uint32_t)mpdu_byte[14] << 16) | ((uint32_t)mpdu_byte[15] << 8) | (uint32_t)mpdu_byte[16];
    int res = (mpdu_byte[17] >> 2) & 0x3F;
    int rv = mpdu_byte[17] & 0x03;

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Unit Registration Response MBT - Extended");
    DSD_FPRINTF(stderr, "\n  MFID [%02X] WACN [%05X] SYSID [%03X] SRC_ID [%06X] SRC [%u]", mfid, wacn, sysid, sid, src);
    if (res != 0) {
        DSD_FPRINTF(stderr, " RES [%02X]", res);
    }
    if (rv == 0) {
        DSD_FPRINTF(stderr, " REG_ACCEPT");
        p25_aff_register(state, src);
    } else if (rv == 1) {
        DSD_FPRINTF(stderr, " REG_FAIL");
    } else if (rv == 2) {
        DSD_FPRINTF(stderr, " REG_DENY");
    } else {
        DSD_FPRINTF(stderr, " REG_REFUSED");
    }
}

static void DSD_ATTR_USED
p25_handle_mbt_mfid90_unknown(const uint8_t* mpdu_byte, int blks, uint8_t opcode, size_t mpdu_len) {
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n MFID 90 (Moto); Opcode: %02X; ", opcode);
    p25_print_mbt_payload_hex(mpdu_byte, blks, mpdu_len);
    DSD_FPRINTF(stderr, " %s", KNRM);
}

static void DSD_ATTR_USED
p25_handle_mbt_unknown_mfid(const uint8_t* mpdu_byte, int blks, uint8_t mfid, uint8_t opcode, size_t mpdu_len) {
    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n MFID %02X (Unknown); Opcode: %02X; ", mfid, opcode);
    p25_print_mbt_payload_hex(mpdu_byte, blks, mpdu_len);
    DSD_FPRINTF(stderr, " %s", KNRM);
}

static void
p25_handle_mbt_inbound_two_party(const uint8_t* mpdu_byte, const char* label, int has_response) {
    uint8_t svc = mpdu_byte[8];
    uint32_t target = p25_mbt_u24(mpdu_byte, 3U);
    uint32_t source = p25_mbt_u24(mpdu_byte, 14U);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n %s MBT - Inbound", label);
    DSD_FPRINTF(stderr, " SVC [%02X] FM [%u] TO [%u]", svc, source, target);
    if (has_response) {
        uint8_t response = mpdu_byte[9];
        DSD_FPRINTF(stderr, " RESPONSE [%02X]", response);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25_handle_mbt_inbound_telephone(const uint8_t* mpdu_byte, const char* label) {
    uint8_t svc = mpdu_byte[8];
    uint32_t source = p25_mbt_u24(mpdu_byte, 14U);
    uint32_t target = p25_mbt_u24(mpdu_byte, 3U);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n %s MBT - Inbound", label);
    DSD_FPRINTF(stderr, " SVC [%02X] FM [%u] TO [%u]", svc, source, target);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25_handle_mbt_inbound_identifier_request(const uint8_t* mpdu_byte) {
    uint32_t wacn = ((uint32_t)mpdu_byte[3] << 12) | ((uint32_t)mpdu_byte[4] << 4) | ((uint32_t)mpdu_byte[5] >> 4);
    uint16_t sysid = (uint16_t)(((uint16_t)(mpdu_byte[5] & 0x0F) << 8) | (uint16_t)mpdu_byte[6]);
    uint32_t source = p25_mbt_u24(mpdu_byte, 14U);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Identifier/Frequency Band Update Request MBT - Inbound");
    DSD_FPRINTF(stderr, " FM [%u] WACN [%05X] SYSID [%03X]", source, wacn, sysid);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25_handle_mbt_inbound_umbtc_explicit_dial(const uint8_t* mpdu_byte, size_t mpdu_len) {
    static const char hex[] = "0123456789ABCDEF";
    char digits[33];
    size_t d = 0U;
    size_t digits_end = (mpdu_len >= 18U) ? 15U : mpdu_len;
    uint32_t source = (mpdu_len >= 18U) ? p25_mbt_u24(mpdu_byte, 15U) : 0U;

    for (size_t i = 13U; i < digits_end && d + 2U < sizeof(digits); i++) {
        digits[d++] = hex[(mpdu_byte[i] >> 4) & 0x0F];
        digits[d++] = hex[mpdu_byte[i] & 0x0F];
    }
    digits[d] = '\0';

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n Telephone Interconnect Explicit Dial Request UMBTC - Inbound");
    if (source != 0U) {
        DSD_FPRINTF(stderr, " FM [%u]", source);
    }
    if (digits[0] != '\0') {
        DSD_FPRINTF(stderr, " DIGITS [%s]", digits);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25_handle_mbt_inbound_mfid90_group_regroup_request(const uint8_t* mpdu_byte) {
    uint8_t svc = mpdu_byte[8];
    uint16_t sg = p25_mbt_u16(mpdu_byte, 16U);
    uint32_t source = p25_mbt_u24(mpdu_byte, 3U);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Group Regroup Voice Request MBT - Inbound");
    DSD_FPRINTF(stderr, " SVC [%02X] SG [%u][%04X] FM [%u]", svc, sg, sg, source);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static void
p25_handle_mbt_inbound_mfid90_extended_function_response(const uint8_t* mpdu_byte) {
    uint16_t function = p25_mbt_u16(mpdu_byte, 8U);
    uint32_t argument = p25_mbt_u24(mpdu_byte, 12U);
    uint32_t source = p25_mbt_u24(mpdu_byte, 3U);

    DSD_FPRINTF(stderr, "%s", KYEL);
    DSD_FPRINTF(stderr, "\n MFID90 (Moto) Extended Function Response MBT - Inbound");
    DSD_FPRINTF(stderr, " FM [%u] FUNC [%04X] ARG [%06X]", source, function, argument);
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static int
p25_handle_mbt_inbound_standard_opcode(const uint8_t* mpdu_byte, size_t mpdu_len, const p25p1_mbt_fields* fields) {
    switch (fields->opcode) {
        case 0x04:
            if (!p25_mbt_require_len(fields, mpdu_len, 17U, "Unit-to-Unit Voice Service Request")) {
                return 1;
            }
            p25_handle_mbt_inbound_two_party(mpdu_byte, "Unit-to-Unit Voice Service Request", 0);
            return 1;
        case 0x08:
            if (fields->fmt == 0x15) {
                if (!p25_mbt_require_len(fields, mpdu_len, 13U, "Telephone Interconnect Explicit Dial Request")) {
                    return 1;
                }
                p25_handle_mbt_inbound_umbtc_explicit_dial(mpdu_byte, mpdu_len);
                return 1;
            }
            return 0;
        case 0x09:
            if (!p25_mbt_require_len(fields, mpdu_len, 17U, "Telephone Interconnect PSTN Request")) {
                return 1;
            }
            p25_handle_mbt_inbound_telephone(mpdu_byte, "Telephone Interconnect PSTN Request");
            return 1;
        case 0x0A:
            if (!p25_mbt_require_len(fields, mpdu_len, 17U, "Telephone Interconnect Answer Response")) {
                return 1;
            }
            p25_handle_mbt_inbound_two_party(mpdu_byte, "Telephone Interconnect Answer Response", 1);
            return 1;
        case 0x32:
            if (!p25_mbt_require_len(fields, mpdu_len, 17U, "Identifier/Frequency Band Update Request")) {
                return 1;
            }
            p25_handle_mbt_inbound_identifier_request(mpdu_byte);
            return 1;
        default: return 0;
    }
}

static int
p25_handle_mbt_inbound_mfid90_opcode(const uint8_t* mpdu_byte, size_t mpdu_len, const p25p1_mbt_fields* fields) {
    switch (fields->opcode) {
        case 0x00:
            if (!p25_mbt_require_len(fields, mpdu_len, 18U, "MFID90 Group Regroup Voice Request")) {
                return 1;
            }
            p25_handle_mbt_inbound_mfid90_group_regroup_request(mpdu_byte);
            return 1;
        case 0x01:
            if (!p25_mbt_require_len(fields, mpdu_len, 15U, "MFID90 Extended Function Response")) {
                return 1;
            }
            p25_handle_mbt_inbound_mfid90_extended_function_response(mpdu_byte);
            return 1;
        default: return 0;
    }
}

static int
p25_handle_mbt_inbound_opcode(const uint8_t* mpdu_byte, size_t mpdu_len, const p25p1_mbt_fields* fields) {
    if (!fields) {
        return 0;
    }
    if (p25_mbt_mfid_is_standard(fields->mfid) && p25_handle_mbt_inbound_standard_opcode(mpdu_byte, mpdu_len, fields)) {
        return 1;
    }
    if (fields->mfid == 0x90 && p25_handle_mbt_inbound_mfid90_opcode(mpdu_byte, mpdu_len, fields)) {
        return 1;
    }

    DSD_FPRINTF(stderr, "%s", KCYN);
    DSD_FPRINTF(stderr, "\n Inbound MBT metadata only - MFID %02X OP %02X", fields->mfid, fields->opcode);
    DSD_FPRINTF(stderr, "%s", KNRM);
    return 1;
}

static int
p25_handle_mbt_site_status_opcode(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len,
                                  const p25p1_mbt_fields* fields) {
    switch (fields->opcode) {
        case 0x3B:
            if (!p25_mbt_require_len(fields, mpdu_len, 20U, "Network Status Broadcast")) {
                return 1;
            }
            p25_handle_mbt_net_sts_broadcast(opts, state, mpdu_byte);
            return 1;
        case 0x3A:
            if (!p25_mbt_require_len(fields, mpdu_len, 19U, "RFSS Status Broadcast")) {
                return 1;
            }
            p25_handle_mbt_rfss_status_broadcast(opts, state, mpdu_byte);
            return 1;
        case 0x3C:
            if (!p25_mbt_require_len(fields, mpdu_len, 14U, "Adjacent Status Broadcast")) {
                return 1;
            }
            p25_handle_mbt_adjacent_status_broadcast(opts, state, mpdu_byte);
            return 1;
        case 0x3E:
            /*
             * Keep AMBTC 0x3E as Protection Parameter Broadcast. Older AABC-B
             * tables conflict with newer SDRTrunk/observed behavior and with
             * bridged P1 TSBK 0x3E adjacency handling; change only with current
             * AABC-E text or captures.
             */
            if (!p25_mbt_require_len(fields, mpdu_len, 10U, "Protection Parameter Broadcast")) {
                return 1;
            }
            p25_handle_mbt_protection_parameter_broadcast(state, mpdu_byte);
            return 1;
        case 0x33:
            if (!p25_mbt_require_len(fields, mpdu_len, 19U, "TDMA Identifier Update")) {
                return 1;
            }
            p25_handle_mbt_tdma_iden_foreign_system(mpdu_byte);
            return 1;
        default: return 0;
    }
}

static int
p25_handle_mbt_voice_service_opcode(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len,
                                    const p25p1_mbt_fields* fields) {
    switch (fields->opcode) {
        case 0x00:
            if (!p25_mbt_require_len(fields, mpdu_len, 20U, "Group Voice Channel Grant")) {
                return 1;
            }
            p25_handle_mbt_group_voice_grant(opts, state, mpdu_byte);
            return 1;
        case 0x04:
        case 0x06:
            if (!p25_mbt_require_len(fields, mpdu_len, 24U, "Unit-to-Unit Voice Channel Grant")) {
                return 1;
            }
            p25_handle_mbt_unit_to_unit_voice_grant(opts, state, mpdu_byte, mpdu_len, fields->opcode);
            return 1;
        case 0x05:
            if (!p25_mbt_require_len(fields, mpdu_len, 20U, "Unit-to-Unit Answer Request")) {
                return 1;
            }
            p25_handle_mbt_unit_to_unit_answer_request(opts, state, mpdu_byte);
            return 1;
        case 0x08:
        case 0x09:
            if (fields->mfid >= 2) {
                return 0;
            }
            if (!p25_mbt_require_len(fields, mpdu_len, 18U, "Telephone Interconnect Voice Channel Grant")) {
                return 1;
            }
            p25_handle_mbt_telephone_interconnect_grant(opts, state, mpdu_byte, fields->opcode);
            return 1;
        default: return 0;
    }
}

static int
p25_handle_mbt_data_service_opcode(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len,
                                   const p25p1_mbt_fields* fields) {
    switch (fields->opcode) {
        case 0x10:
            if (!p25_mbt_require_len(fields, mpdu_len, 24U, "Individual Data Channel Grant")) {
                return 1;
            }
            p25_handle_mbt_individual_data_channel_grant(opts, state, mpdu_byte, mpdu_len);
            return 1;
        case 0x11:
            if (!p25_mbt_require_len(fields, mpdu_len, 20U, "Group Data Channel Grant")) {
                return 1;
            }
            p25_handle_mbt_group_data_channel_grant(opts, state, mpdu_byte);
            return 1;
        default: return 0;
    }
}

static int
p25_handle_mbt_command_metadata_opcode(const uint8_t* mpdu_byte, size_t mpdu_len, const p25p1_mbt_fields* fields) {
    switch (fields->opcode) {
        case 0x18:
        case 0x1A:
        case 0x1C:
        case 0x1F:
            if (!p25_mbt_require_len(fields, mpdu_len, 32U, "AMBTC command/status metadata")) {
                return 1;
            }
            p25_handle_mbt_extended_command_metadata(mpdu_byte, fields->opcode);
            return 1;
        default: return 0;
    }
}

static int
p25_handle_mbt_affiliation_roaming_opcode(dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len,
                                          const p25p1_mbt_fields* fields) {
    switch (fields->opcode) {
        case 0x28:
            if (fields->fmt == 0x17) {
                if (!p25_mbt_require_len(fields, mpdu_len, 21U, "Group Affiliation Response")) {
                    return 1;
                }
                p25_handle_mbt_group_affiliation_response(state, mpdu_byte);
            } else {
                DSD_FPRINTF(stderr, " - group affiliation response format %02X not handled", fields->fmt);
            }
            return 1;
        case 0x2A:
            if (!p25_mbt_require_len(fields, mpdu_len, 17U, "Group Affiliation Query")) {
                return 1;
            }
            p25_handle_mbt_group_affiliation_query(mpdu_byte);
            return 1;
        case 0x2C:
            if (fields->fmt == 0x17) {
                if (!p25_mbt_require_len(fields, mpdu_len, 18U, "Unit Registration Response")) {
                    return 1;
                }
                p25_handle_mbt_unit_registration_response(state, mpdu_byte);
            } else {
                DSD_FPRINTF(stderr, " - unit registration response format %02X not handled", fields->fmt);
            }
            return 1;
        case 0x36:
        case 0x37:
            if (!p25_mbt_require_len(fields, mpdu_len, 15U, "Roaming Address")) {
                return 1;
            }
            p25_handle_mbt_roaming_address(mpdu_byte, fields->opcode);
            return 1;
        default: return 0;
    }
}

static int
p25_handle_mbt_standard_opcode(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len,
                               const p25p1_mbt_fields* fields) {
    if (!p25_mbt_is_ambtc(fields)) {
        return 0;
    }

    if (p25_handle_mbt_site_status_opcode(opts, state, mpdu_byte, mpdu_len, fields)) {
        return 1;
    }
    if (p25_handle_mbt_voice_service_opcode(opts, state, mpdu_byte, mpdu_len, fields)) {
        return 1;
    }
    if (p25_handle_mbt_data_service_opcode(opts, state, mpdu_byte, mpdu_len, fields)) {
        return 1;
    }
    if (p25_handle_mbt_command_metadata_opcode(mpdu_byte, mpdu_len, fields)) {
        return 1;
    }
    return p25_handle_mbt_affiliation_roaming_opcode(state, mpdu_byte, mpdu_len, fields);
}

//trunking data delivered via PDU format
void
p25_decode_pdu_trunking(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte) {
    (void)p25_decode_pdu_trunking_bounded(opts, state, mpdu_byte, p25_mbt_declared_len(mpdu_byte));
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
p25_decode_pdu_trunking_bounded(dsd_opts* opts, dsd_state* state, const uint8_t* mpdu_byte, size_t mpdu_len) {
    p25p1_mbt_fields fields;
    if (!p25_parse_mbt_fields_checked(mpdu_byte, mpdu_len, &fields)) {
        DSD_FPRINTF(stderr, " ALT MBT - short payload (got %zu)", mpdu_len);
        return -1;
    }

    p25_print_mbt_header(&fields);
    p25_mbt_try_bridge_iden_updates(opts, state, mpdu_byte, mpdu_len, &fields);

    if (!fields.is_outbound) {
        (void)p25_handle_mbt_inbound_opcode(mpdu_byte, mpdu_len, &fields);
        return 0;
    }

    p25_sm_note_cc_activity(opts, state, "p25p1-mbt");

    if (p25_mbt_has_unsupported_survey_format(&fields)) {
        DSD_FPRINTF(stderr, " - broadcast format %02X not handled", fields.fmt);
        return 0;
    }

    if (p25_mbt_mfid_is_standard(fields.mfid)
        && p25_handle_mbt_standard_opcode(opts, state, mpdu_byte, mpdu_len, &fields)) {
        return 0;
    }

    if (!p25_mbt_is_ambtc(&fields) && p25_mbt_mfid_is_standard(fields.mfid)) {
        DSD_FPRINTF(stderr, " - UMBTC standard opcode %02X not handled as AMBTC", fields.opcode);
        return 0;
    }

    if (fields.mfid == 0xA4) {
        p25_handle_mbt_mfid_a4(mpdu_byte, fields.blks, fields.opcode, mpdu_len);
    } else if (fields.mfid == 0x90) {
        if (p25_mbt_is_ambtc(&fields) && fields.opcode == 0x02) {
            if (!p25_mbt_require_len(&fields, mpdu_len, 18U, "MFID90 Group Regroup Channel Grant")) {
                return 0;
            }
            p25_handle_mbt_mfid90_group_regroup(opts, state, mpdu_byte);
        } else {
            p25_handle_mbt_mfid90_unknown(mpdu_byte, fields.blks, fields.opcode, mpdu_len);
        }
    } else {
        p25_handle_mbt_unknown_mfid(mpdu_byte, fields.blks, fields.mfid, fields.opcode, mpdu_len);
    }
    return 0;
}
