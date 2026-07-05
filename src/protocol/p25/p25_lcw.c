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

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/p25/p25_callsign.h>
#include <dsd-neo/protocol/p25/p25_cc_candidates.h>
#include <dsd-neo/protocol/p25/p25_frequency.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

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

static inline int
p25_lcw_signed_offset_units(int sign_bit, int raw_offset) {
    return sign_bit ? raw_offset : -raw_offset;
}

static void
p25_lcw_store_fdma_iden(dsd_opts* opts, dsd_state* state, int iden, long int base_freq, int chan_spac, int trans_off,
                        uint8_t bw_vu) {
    if (!state || iden < 0 || iden >= 16 || base_freq == 0 || chan_spac == 0) {
        return;
    }

    p25_invalidate_chan_map_for_iden(state, iden);

    p25_iden_entry_t* e = &state->p25_iden_fdma[iden];
    e->base_freq = base_freq;
    e->chan_type = 1;
    e->chan_spac = chan_spac;
    e->trans_off = trans_off;
    e->bw_vu = bw_vu;
    e->trust = (state->p25_cc_freq != 0 && opts && opts->p25_is_tuned == 0) ? 2 : 1;
    e->populated = 1;
    e->wacn = state->p2_wacn;
    e->sysid = state->p2_sysid;
    e->rfss = state->p2_rfssid;
    e->site = state->p2_siteid;
    state->p25_chan_tdma_explicit[iden] |= 1;
    p25_resolve_pending_announcements(opts, state);
}

/**
 * @brief Resolve a P25 Algorithm ID to a human-readable name.
 *
 * Common APCO P25 ALGIDs used by the voice/ESS paths:
 *   0x80 = unencrypted, 0x81 = DES-OFB, 0x84 = AES-256,
 *   0x89 = AES-128-OFB, 0x9F = DES-XL, 0xAA = ADP/RC4
 *
 * @param algid The 8-bit algorithm identifier.
 * @return Static string with algorithm name, or NULL if unrecognized.
 */
static const char*
p25_algid_name(uint8_t algid) {
    switch (algid) {
        case 0x80: return "UNENCRYPTED";
        case 0x81: return "DES-OFB";
        case 0x82: return "2-KEY 3DES";
        case 0x83: return "3-KEY 3DES";
        case 0x84: return "AES-256";
        case 0x85: return "AES-128";
        case 0x88: return "AES-CBC";
        case 0x89: return "AES-128-OFB";
        case 0x9F: return "DES-XL";
        case 0xAA: return "ADP/RC4";
        case 0xAF: return "AES-256-GCM";
        default: return NULL;
    }
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
    int is_standard_mfid;
} p25_lcw_ctx;

typedef void (*p25_lcw_handler_fn)(p25_lcw_ctx* ctx);

typedef struct p25_lcw_handler_entry {
    uint8_t key;
    p25_lcw_handler_fn fn;
} p25_lcw_handler_entry;

static void
p25_lcw_set_call_string_prefix(dsd_state* state, const char* prefix, uint8_t svcopt) {
    DSD_SNPRINTF(state->call_string[0], sizeof state->call_string[0], "%s", prefix);
    if (svcopt & 0x80) {
        dsd_append(state->call_string[0], sizeof state->call_string[0], " Emergency  ");
    } else if (svcopt & 0x40) {
        dsd_append(state->call_string[0], sizeof state->call_string[0], " Encrypted  ");
    } else {
        dsd_append(state->call_string[0], sizeof state->call_string[0], "            ");
    }
}

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
p25_lcw_handle_format_00(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Group Voice Channel User");
    uint8_t res = (uint8_t)ConvertBitIntoBytes(&ctx->bits[24], 7);
    uint8_t explicit_src = ctx->bits[24];
    uint16_t group = (uint16_t)ConvertBitIntoBytes(&ctx->bits[32], 16);
    uint32_t source = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " - Group %d Source %d", group, source);
    UNUSED2(res, explicit_src);

    ctx->state->gi[0] = 0;
    ctx->state->dmr_so = ctx->lc_svcopt;
    ctx->state->p25_service_options_valid[0] = 1;
    if (group != 0) {
        ctx->state->lasttg = group;
    }
    if (source != 0) {
        ctx->state->lastsrc = source;
    }
    ctx->state->generic_talker_alias[0][0] = '\0';
    ctx->state->generic_talker_alias_src[0] = 0;

    if (source != 0 && group != 0) {
        p25_ga_add(ctx->state, (uint32_t)source, (uint16_t)group);
    }

    p25_lcw_set_call_string_prefix(ctx->state, "   Group ", ctx->lc_svcopt);
}

static void
p25_lcw_handle_format_03(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Unit to Unit Voice Channel User");
    uint32_t target = (uint32_t)ConvertBitIntoBytes(&ctx->bits[24], 24);
    uint32_t source = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " - Target %d Source %d", target, source);

    if (target != 0) {
        ctx->state->lasttg = target;
    }
    if (source != 0) {
        ctx->state->lastsrc = source;
    }
    ctx->state->generic_talker_alias[0][0] = '\0';
    ctx->state->generic_talker_alias_src[0] = 0;
    ctx->state->gi[0] = 1;
    ctx->state->dmr_so = ctx->lc_svcopt;
    ctx->state->p25_service_options_valid[0] = 1;

    p25_lcw_set_call_string_prefix(ctx->state, " Private ", ctx->lc_svcopt);
}

static void
p25_lcw_handle_format_42(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Group Voice Channel Update - ");
    uint16_t channel1 = (uint16_t)ConvertBitIntoBytes(&ctx->bits[8], 16);
    uint16_t group1 = (uint16_t)ConvertBitIntoBytes(&ctx->bits[24], 16);
    uint16_t channel2 = (uint16_t)ConvertBitIntoBytes(&ctx->bits[40], 16);
    uint16_t group2 = (uint16_t)ConvertBitIntoBytes(&ctx->bits[56], 16);

    if (channel1 && group1) {
        DSD_FPRINTF(stderr, "Ch: %04X TG: %d; ", channel1, group1);
        char suf[32];
        p25_format_chan_suffix(ctx->state, channel1, -1, suf, sizeof suf);
        DSD_SNPRINTF(ctx->state->active_channel[0], sizeof ctx->state->active_channel[0], "Active Ch: %04X%s TG: %d; ",
                     channel1, suf, group1);
        ctx->state->last_active_time = time(NULL);
    }

    if (channel2 && group2 && group1 != group2) {
        DSD_FPRINTF(stderr, "Ch: %04X TG: %d; ", channel2, group2);
        char suf[32];
        p25_format_chan_suffix(ctx->state, channel2, -1, suf, sizeof suf);
        DSD_SNPRINTF(ctx->state->active_channel[1], sizeof ctx->state->active_channel[1], "Active Ch: %04X%s TG: %d; ",
                     channel2, suf, group2);
        ctx->state->last_active_time = time(NULL);
    }
}

static int
p25_lcw_trunk_cc_ready_for_grant(p25_lcw_ctx* ctx) {
    if (ctx->opts->p25_trunk != 1) {
        return 0;
    }
    return ctx->state->p25_cc_freq != 0 || ctx->state->trunk_cc_freq > 0;
}

static int
p25_lcw_format_44_skip_grant(const p25_lcw_ctx* ctx, uint16_t group) {
    if (ctx->state->tg_hold != 0 && ctx->state->tg_hold != group) {
        return 1;
    }
    if ((ctx->lc_svcopt & 0x40) && ctx->opts->trunk_tune_enc_calls == 0
        && !p25_patch_tg_key_is_clear(ctx->state, group)) {
        return 1;
    }
    return 0;
}

static void
p25_lcw_warn_format_44_retune_disabled(p25_lcw_ctx* ctx) {
    if (ctx->state->p25_lcw_retune_disabled_warned != 0) {
        return;
    }
    ctx->state->p25_lcw_retune_disabled_warned = 1;
    DSD_FPRINTF(stderr,
                " [WARN: P25 LCW explicit retune is disabled; 0x44 grants may not be followed. Enable with -j or "
                "menu.] ");
}

static void
p25_lcw_handle_format_44_trunking(p25_lcw_ctx* ctx, uint16_t channel, uint16_t group) {
    if (!p25_lcw_trunk_cc_ready_for_grant(ctx)) {
        return;
    }
    if (ctx->opts->p25_lcw_retune == 0) {
        p25_lcw_warn_format_44_retune_disabled(ctx);
        return;
    }
    if (ctx->opts->p25_lcw_retune == 1 && ctx->opts->trunk_tune_group_calls == 1
        && !p25_lcw_format_44_skip_grant(ctx, group)) {
        p25_sm_on_group_grant(ctx->opts, ctx->state, channel, ctx->lc_svcopt, group, (int)ctx->state->lastsrc);
    }
}

static void
p25_lcw_handle_format_44(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Group Voice Channel Update %s Explicit", dsd_unicode_or_ascii("–", "-"));
    uint16_t group1 = (uint16_t)ConvertBitIntoBytes(&ctx->bits[24], 16);
    uint16_t channelt = (uint16_t)ConvertBitIntoBytes(&ctx->bits[40], 16);
    uint16_t channelr = (uint16_t)ConvertBitIntoBytes(&ctx->bits[56], 16);
    DSD_FPRINTF(stderr, "Ch: %04X TG: %d; ", channelt, group1);
    UNUSED(channelr);

    p25_lcw_handle_format_44_trunking(ctx, channelt, group1);

    char suf[32];
    p25_format_chan_suffix(ctx->state, channelt, -1, suf, sizeof suf);
    DSD_SNPRINTF(ctx->state->active_channel[0], sizeof ctx->state->active_channel[0], "Active Ch: %04X%s TG: %d; ",
                 channelt, suf, group1);
    ctx->state->last_active_time = time(NULL);
}

static void
p25_lcw_handle_format_45(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Unit to Unit Answer Request");
}

static void
p25_lcw_handle_format_46(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Telephone Interconnect Voice Channel User");
}

static void
p25_lcw_handle_format_47(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Telephone Interconnect Answer Request");
}

static void
p25_lcw_handle_format_49(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Source ID Extension -");
    uint32_t wacn = (uint32_t)ConvertBitIntoBytes(&ctx->bits[16], 20);
    uint16_t sysid = (uint16_t)ConvertBitIntoBytes(&ctx->bits[36], 12);
    uint32_t src = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " Full SUID: WACN %05X SYSID %03X SRC %d", wacn, sysid, src);
    if (wacn != 0) {
        ctx->state->p25_src_nid = wacn;
    }
    if (src != 0) {
        ctx->state->lastsrc = (int)src;
    }
}

static void
p25_lcw_handle_format_4a(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Unit to Unit Voice Channel User %s Extended", dsd_unicode_or_ascii("–", "-"));
    uint32_t target = (uint32_t)ConvertBitIntoBytes(&ctx->bits[16], 24);
    uint32_t src = (uint32_t)ConvertBitIntoBytes(&ctx->bits[40], 24);
    DSD_FPRINTF(stderr, "TGT: %d; SRC: %d; ", target, src);
    ctx->state->gi[0] = 1;
}

static void
p25_lcw_handle_format_50(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Group Affiliation Query");
    uint16_t group = (uint16_t)ConvertBitIntoBytes(&ctx->bits[32], 16);
    uint32_t source = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
    if (group) {
        DSD_FPRINTF(stderr, " - TG %u", group);
        ctx->state->lasttg = group;
    }
    if (source) {
        DSD_FPRINTF(stderr, " SRC %u", source);
        ctx->state->lastsrc = source;
    }
    if (group && source) {
        p25_ga_add(ctx->state, (uint32_t)source, (uint16_t)group);
    }
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

static void
p25_lcw_handle_format_55(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Status Update");
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
    uint8_t iden = (uint8_t)ConvertBitIntoBytes(&ctx->bits[8], 4);
    int bw = (int)ConvertBitIntoBytes(&ctx->bits[12], 9);
    int sign = ctx->bits[21] & 1;
    int tx_raw = (int)ConvertBitIntoBytes(&ctx->bits[22], 8);
    int chan_spac = (int)ConvertBitIntoBytes(&ctx->bits[30], 10);
    uint32_t base = (uint32_t)ConvertBitIntoBytes(&ctx->bits[40], 32);
    int trans_off = p25_lcw_signed_offset_units(sign, tx_raw);
    DSD_FPRINTF(stderr, " Channel Identifier Update; Iden: %X; BW: %X; TX Offset: %d; Spacing: %X; Base: %ld;", iden,
                bw, trans_off, chan_spac, (long)base * 5L);
    p25_lcw_store_fdma_iden(ctx->opts, ctx->state, iden, (long int)base, chan_spac, trans_off, 0);
}

static void
p25_lcw_handle_format_59(p25_lcw_ctx* ctx) {
    uint8_t iden = (uint8_t)ConvertBitIntoBytes(&ctx->bits[8], 4);
    uint8_t bw_vu = (uint8_t)ConvertBitIntoBytes(&ctx->bits[12], 4);
    int sign = ctx->bits[16] & 1;
    int tx_raw = (int)ConvertBitIntoBytes(&ctx->bits[17], 13);
    int chan_spac = (int)ConvertBitIntoBytes(&ctx->bits[30], 10);
    uint32_t base = (uint32_t)ConvertBitIntoBytes(&ctx->bits[40], 32);
    int trans_off = p25_lcw_signed_offset_units(sign, tx_raw);
    DSD_FPRINTF(stderr, " Channel Identifier Update VU; Iden: %X; BW: %X; TX Offset: %d; Spacing: %X; Base: %ld;", iden,
                bw_vu, trans_off, chan_spac, (long)base * 5L);
    p25_lcw_store_fdma_iden(ctx->opts, ctx->state, iden, (long int)base, chan_spac, trans_off, bw_vu);
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
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " System Service Broadcast");
}

static void
p25_lcw_handle_format_61(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Secondary Control Channel Broadcast");
}

static void
p25_lcw_handle_format_62(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Adjacent Site Status Broadcast");
}

static void
p25_lcw_handle_format_63(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " RFSS Status Broadcast");
}

static void
p25_lcw_handle_format_64(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Network Status Broadcast");
}

static void
p25_lcw_handle_format_65(p25_lcw_ctx* ctx) {
    uint8_t algid = (uint8_t)ConvertBitIntoBytes(&ctx->bits[24], 8);
    uint16_t kid = (uint16_t)ConvertBitIntoBytes(&ctx->bits[32], 16);
    uint32_t target = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);

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
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Secondary Control Channel Broadcast %s Explicit (LCSCBX)", dsd_unicode_or_ascii("–", "-"));
}

static void
p25_lcw_handle_format_67(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " Adjacent Site Status (LCASBX)");
    uint8_t lra = (uint8_t)ConvertBitIntoBytes(&ctx->bits[8], 8);
    uint16_t channelt = (uint16_t)ConvertBitIntoBytes(&ctx->bits[16], 16);
    uint8_t rfssid = (uint8_t)ConvertBitIntoBytes(&ctx->bits[32], 8);
    uint8_t siteid = (uint8_t)ConvertBitIntoBytes(&ctx->bits[40], 8);
    uint16_t channelr = (uint16_t)ConvertBitIntoBytes(&ctx->bits[48], 16);
    uint8_t cfva = (uint8_t)ConvertBitIntoBytes(&ctx->bits[64], 4);
    DSD_FPRINTF(stderr, " - RFSS %d Site %d CH %04X", rfssid, siteid, channelt);
    UNUSED(channelr);
    if (cfva & 0x1) {
        DSD_FPRINTF(stderr, " - Connection Active");
    }
    uint16_t sysid = ctx->state ? (uint16_t)ctx->state->p2_sysid : 0U;
    const p25_neighbor_channel_announcement_t announcement = {
        .channel = channelt,
        .sysid = sysid,
        .rfss = rfssid,
        .site = siteid,
        .lra = lra,
        .cfva = cfva,
        .lra_valid = 1U,
        .cfva_valid = 1U,
    };
    (void)p25_announce_neighbor_channel_ex(ctx->opts, ctx->state, &announcement);
}

static void
p25_lcw_handle_format_68(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " RFSS Status Broadcast %s Explicit (LCRSBX)", dsd_unicode_or_ascii("–", "-"));
}

static void
p25_lcw_handle_format_69(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Network Status Broadcast %s Explicit (LCNSBX)", dsd_unicode_or_ascii("–", "-"));
}

static void
p25_lcw_handle_format_6a(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Conventional Fallback");
}

static void
p25_lcw_handle_format_6b(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " Message Update %s Source ID Extension Required", dsd_unicode_or_ascii("–", "-"));
}

static void
p25_lcw_handle_call_termination(p25_lcw_ctx* ctx) {
    uint32_t tgt = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " Call Termination; TGT: %d;", tgt);
    DSD_MEMSET(ctx->state->dmr_pdu_sf[0], 0, sizeof(ctx->state->dmr_pdu_sf[0]));
    if (ctx->opts->p25_trunk == 1 && ctx->state->p25_cc_freq != 0 && ctx->opts->p25_is_tuned == 1) {
        ctx->state->p25_sm_force_release = 1;
        p25_sm_on_release(ctx->opts, ctx->state);
    }
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
        {0x59, p25_lcw_handle_format_59}, {0x5A, p25_lcw_handle_format_5a}, {0x5C, p25_lcw_handle_format_5c},
        {0x60, p25_lcw_handle_format_60}, {0x61, p25_lcw_handle_format_61}, {0x62, p25_lcw_handle_format_62},
        {0x63, p25_lcw_handle_format_63}, {0x64, p25_lcw_handle_format_64}, {0x65, p25_lcw_handle_format_65},
        {0x66, p25_lcw_handle_format_66}, {0x67, p25_lcw_handle_format_67}, {0x68, p25_lcw_handle_format_68},
        {0x69, p25_lcw_handle_format_69}, {0x6A, p25_lcw_handle_format_6a}, {0x6B, p25_lcw_handle_format_6b},
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

static void
p25_lcw_handle_mfid90_opcode_06(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto)");
    apx_embedded_gps(ctx->opts, ctx->state, ctx->bits);
}

static void
p25_lcw_handle_mfid90_opcode_00(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) Group Regroup Channel User (LCGRGR)");
    uint32_t sg = (uint32_t)ConvertBitIntoBytes(&ctx->bits[32], 16);
    uint32_t src = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
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
    ctx->state->lasttg = sg;
    if (src != 0) {
        ctx->state->lastsrc = src;
    }
    ctx->state->gi[0] = 0;
    p25_patch_update(ctx->state, (int)sg, 1, 1);
}

static void
p25_lcw_handle_mfid90_opcode_01(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) Group Regroup Channel Update (LCGRGU)");
    uint32_t sg = (uint32_t)ConvertBitIntoBytes(&ctx->bits[24], 16);
    uint32_t ch = (uint32_t)ConvertBitIntoBytes(&ctx->bits[56], 16);
    DSD_FPRINTF(stderr, " SG: %d; CH: %04X;", sg, ch);
    if (ctx->bits[16] == 1) {
        DSD_FPRINTF(stderr, " Res;");
    }
    if (ctx->bits[17] == 1) {
        DSD_FPRINTF(stderr, " ENC;");
    }
    ctx->state->gi[0] = 0;
}

static void
p25_lcw_handle_mfid90_opcode_03(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " MFID90 (Moto) Group Regroup Add");
}

static void
p25_lcw_handle_mfid90_opcode_04(p25_lcw_ctx* ctx) {
    UNUSED(ctx);
    DSD_FPRINTF(stderr, " MFID90 (Moto) Group Regroup Delete");
}

static void
p25_lcw_handle_mfid90_opcode_05(p25_lcw_ctx* ctx) {
    DSD_FPRINTF(stderr, " MFID90 (Moto) System Information (BSI)");
    DSD_FPRINTF(stderr, " Data:");
    for (int bi = 16; bi + 8 <= 72; bi += 8) {
        uint8_t b = (uint8_t)ConvertBitIntoBytes(&ctx->bits[bi], 8);
        DSD_FPRINTF(stderr, " %02X", b);
    }
    if (ctx->opts->frontend_display.show_p25_callsign_decode
        && (ctx->state->p2_wacn != 0 || ctx->state->p2_sysid != 0)) {
        char callsign[7];
        p25_wacn_sysid_to_callsign((uint32_t)ctx->state->p2_wacn, (uint16_t)ctx->state->p2_sysid, callsign);
        DSD_FPRINTF(stderr, " [%s]", callsign);
    }
}

static void
p25_lcw_handle_mfid90_opcode_0f(p25_lcw_ctx* ctx) {
    uint32_t src = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
    DSD_FPRINTF(stderr, " MFID90 (Moto) Talker EOT; SRC: %d;", src);
    DSD_MEMSET(ctx->state->dmr_pdu_sf[0], 0, sizeof(ctx->state->dmr_pdu_sf[0]));
    if (ctx->opts->p25_trunk == 1 && ctx->state->p25_cc_freq != 0 && ctx->opts->p25_is_tuned == 1) {
        ctx->state->p25_sm_force_release = 1;
        p25_sm_on_release(ctx->opts, ctx->state);
    }
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
        {0x03, p25_lcw_handle_mfid90_opcode_03}, {0x04, p25_lcw_handle_mfid90_opcode_04},
        {0x05, p25_lcw_handle_mfid90_opcode_05}, {0x06, p25_lcw_handle_mfid90_opcode_06},
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

static int
p25_lcw_dispatch_mfid_a4(p25_lcw_ctx* ctx) {
    if (ctx->lc_opcode > 0x31 && ctx->lc_opcode < 0x36) {
        DSD_FPRINTF(stderr, " MFIDA4 (Harris) Talker Alias Blocks");
        l3h_embedded_alias_blocks_phase1(ctx->opts, ctx->state, 0, ctx->bits);
        return 1;
    }

    if (ctx->lc_opcode == 0x2A) {
        DSD_FPRINTF(stderr, " MFIDA4 (Harris) GPS Block 1");
        DSD_MEMSET(ctx->state->dmr_pdu_sf[0], 0, sizeof(ctx->state->dmr_pdu_sf[0]));
        DSD_MEMCPY(ctx->state->dmr_pdu_sf[0], ctx->bits, 16 * sizeof(uint8_t));
        DSD_MEMCPY(ctx->state->dmr_pdu_sf[0] + 40, ctx->bits + 16, 56 * sizeof(uint8_t));
        return 1;
    }

    if (ctx->lc_opcode == 0x2B) {
        DSD_FPRINTF(stderr, " MFIDA4 (Harris) GPS Block 2");
        DSD_MEMCPY(ctx->state->dmr_pdu_sf[0] + 40 + 56, ctx->bits + 16, 56 * sizeof(uint8_t));
        uint16_t check = (uint16_t)ConvertBitIntoBytes(&ctx->state->dmr_pdu_sf[0][0], 16);
        if (check == 0x2AA4) {
            nmea_harris(ctx->opts, ctx->state, ctx->state->dmr_pdu_sf[0], (uint32_t)ctx->state->lastsrc, 0);
        } else {
            DSD_FPRINTF(stderr, " Missing GPS Block 1");
        }
        DSD_MEMSET(ctx->state->dmr_pdu_sf[0], 0, sizeof(ctx->state->dmr_pdu_sf[0]));
        return 1;
    }

    if (ctx->lc_format == 0x0A) {
        uint32_t src = (uint32_t)ConvertBitIntoBytes(&ctx->bits[24], 24);
        uint32_t tgt = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
        DSD_FPRINTF(stderr, " MFIDA4 (Harris) Data Channel; SRC: %d; TGT: %d;", src, tgt);
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
        uint32_t wacn = (uint32_t)ConvertBitIntoBytes(&ctx->bits[16], 20);
        uint16_t sysid = (uint16_t)ConvertBitIntoBytes(&ctx->bits[36], 12);
        uint32_t src = (uint32_t)ConvertBitIntoBytes(&ctx->bits[48], 24);
        DSD_FPRINTF(stderr, " MFIDD8 (Tait) Subscriber FQ-SUID: %05X.%03X.%d", wacn, sysid, src);
        if (wacn != 0) {
            ctx->state->p25_src_nid = wacn;
        }
        if (src != 0) {
            ctx->state->lastsrc = (int)src;
        }
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

void
p25_lcw(dsd_opts* opts, dsd_state* state, uint8_t lcw_bits[], uint8_t irrecoverable_errors) {
    UNUSED(irrecoverable_errors);
    if (opts == NULL || state == NULL || lcw_bits == NULL) {
        return;
    }

    p25_lcw_ctx ctx;
    DSD_MEMSET(&ctx, 0, sizeof(ctx));
    ctx.opts = opts;
    ctx.state = state;
    ctx.bits = lcw_bits;
    ctx.lc_format = (uint8_t)ConvertBitIntoBytes(&lcw_bits[0], 8);
    ctx.lc_opcode = (uint8_t)ConvertBitIntoBytes(&lcw_bits[2], 6);
    ctx.lc_mfid = (uint8_t)ConvertBitIntoBytes(&lcw_bits[8], 8);
    ctx.lc_svcopt = (uint8_t)ConvertBitIntoBytes(&lcw_bits[16], 8);
    ctx.lc_pf = lcw_bits[0];
    ctx.lc_sf = lcw_bits[1];
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
