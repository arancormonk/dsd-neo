// SPDX-License-Identifier: ISC
/*-------------------------------------------------------------------------------
 * dmr_flco.c
 * DMR Full Link Control, Short Link Control, TACT/CACH and related funtions
 *
 * Portions of link control/voice burst/vlc/tlc from LouisErigHerve
 * Source: https://github.com/LouisErigHerve/dsd/blob/master/src/dmr_sync.c
 *
 * LWVMOBILE
 * 2022-12 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/embedded_alias.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dmr_hytera.h"
#include "dmr_tiii_site.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/secret_redaction.h"
#include "dsd-neo/core/state_fwd.h"

static void dmr_slco(dsd_opts* opts, dsd_state* state, uint8_t slco_bits[]);
static inline int dmr_slot_is_known(const dsd_state* state);
static inline void dmr_print_slot_tag(const dsd_state* state);

static inline int
dmr_slot_is_known(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    return state->currentslot == 0 || state->currentslot == 1;
}

static inline void
dmr_print_slot_tag(const dsd_state* state) {
    if (dmr_slot_is_known(state)) {
        DSD_FPRINTF(stderr, " SLOT %d", ((state->currentslot & 1) + 1));
    } else {
        DSD_FPRINTF(stderr, " SLOT ?");
    }
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    uint8_t* lc_bits;
    uint32_t CRCCorrect;
    uint32_t* IrrecoverableErrors;
    uint8_t type;

    uint8_t pf;
    uint8_t reserved;
    uint8_t flco;
    uint8_t fid;
    uint8_t so;
    uint32_t target;
    uint32_t source;

    int restchannel;
    int is_cap_plus;
    int is_alias;
    int is_gps;
    int is_xpt;

    uint8_t xpt_hand;
    uint8_t xpt_free;
    uint8_t xpt_int;
    uint8_t target_hash[24];
    uint8_t tg_hash;

    uint8_t slot;
    uint8_t slot_idx;
    uint8_t unk;
    uint8_t is_kenwood_sc;
    int protected_lc;
} dmr_flco_ctx;

static int
dmr_flco_show_keys(const dmr_flco_ctx* ctx) {
    return (ctx != NULL && ctx->opts != NULL) ? ctx->opts->show_keys : 0;
}

static unsigned int
dmr_flco_hytera_key_segment_count(const dmr_flco_ctx* ctx) {
    const dsd_state* state = (ctx != NULL) ? ctx->state : NULL;
    if (state == NULL || (state->K1 == 0ULL && state->K2 == 0ULL && state->K3 == 0ULL && state->K4 == 0ULL)) {
        return 0U;
    }
    if (state->hytera_key_segments == 1U || state->hytera_key_segments == 2U || state->hytera_key_segments == 4U) {
        return state->hytera_key_segments;
    }
    if (state->K3 != 0ULL || state->K4 != 0ULL) {
        return 4U;
    }
    if (state->K2 != 0ULL) {
        return 2U;
    }
    return 1U;
}

static const char*
dmr_flco_format_hytera_basic_key(char* key_text, size_t key_text_size, const dmr_flco_ctx* ctx,
                                 unsigned int segment_count) {
    if (segment_count == 2U || segment_count == 4U) {
        const unsigned long long segments[4] = {ctx->state->K1, ctx->state->K2, ctx->state->K3, ctx->state->K4};
        return dsd_secret_format_u64_segments(key_text, key_text_size, dmr_flco_show_keys(ctx), segments,
                                              segment_count);
    }
    return dsd_secret_format_hex(key_text, key_text_size, dmr_flco_show_keys(ctx), ctx->state->K1 & 0xFFFFFFFFFFULL,
                                 10U, 0);
}

static void
dmr_flco_print_type_color(uint8_t type, const char* type1_color, const char* type2_color, const char* type3_color) {
    if (type == 1) {
        DSD_FPRINTF(stderr, "%s \n", type1_color);
    }
    if (type == 2) {
        DSD_FPRINTF(stderr, "%s \n", type2_color);
    }
    if (type == 3) {
        DSD_FPRINTF(stderr, "%s", type3_color);
    }
}

static void
dmr_flco_ctx_init(dmr_flco_ctx* ctx, dsd_opts* opts, dsd_state* state, uint8_t lc_bits[], uint32_t CRCCorrect,
                  uint32_t* IrrecoverableErrors, uint8_t type) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->opts = opts;
    ctx->state = state;
    ctx->lc_bits = lc_bits;
    ctx->CRCCorrect = CRCCorrect;
    ctx->IrrecoverableErrors = IrrecoverableErrors;
    ctx->type = type;
    ctx->restchannel = -1;

    ctx->slot = state->currentslot;
    ctx->slot_idx = (ctx->slot >= 2) ? 1 : ctx->slot;
    ctx->pf = lc_bits[0];
    ctx->reserved = lc_bits[1];
    ctx->flco = (uint8_t)convert_bits_into_output(&lc_bits[2], 6);
    ctx->fid = (uint8_t)convert_bits_into_output(&lc_bits[8], 8);
    ctx->so = (uint8_t)convert_bits_into_output(&lc_bits[16], 8);
    ctx->target = (uint32_t)convert_bits_into_output(&lc_bits[24], 24);
    ctx->source = (uint32_t)convert_bits_into_output(&lc_bits[48], 24);
}

static void
dmr_flco_detect_kenwood_sc(dmr_flco_ctx* ctx, int crc_ok) {
    if (crc_ok && ctx->pf == 1 && ctx->fid == 0x20 && (ctx->so & 0x40) == 0x40) {
        ctx->pf = 0;
        ctx->fid = 0;
        ctx->is_kenwood_sc = 1;
        if (ctx->state->ken_sc == 1) {
            ctx->so ^= 0x40;
        }
    }
}

static void
dmr_flco_detect_kirisun_le(dmr_flco_ctx* ctx, int crc_ok) {
    if (crc_ok && ctx->fid == 0x0A) {
        ctx->opts->dmr_le = ((ctx->so & 0x40U) != 0U) ? 3 : 0;
    }
}

static void
dmr_flco_detect_hytera_xpt(dmr_flco_ctx* ctx) {
    if (*ctx->IrrecoverableErrors == 0 && ctx->flco == 0x09 && ctx->fid == 0x68) {
        DSD_SNPRINTF(ctx->state->dmr_branding, sizeof(ctx->state->dmr_branding), "%s", "  Hytera");
        DSD_SNPRINTF(ctx->state->dmr_branding_sub, sizeof(ctx->state->dmr_branding_sub), "XPT ");
    }
}

static void
dmr_flco_detect_invalid_hytera_enhanced(dmr_flco_ctx* ctx) {
    if (ctx->fid == 0x68 && ctx->flco == 0x02 && *ctx->IrrecoverableErrors == 0) {
        *ctx->IrrecoverableErrors = 1;
    }
}

static void
dmr_flco_detect_special_modes(dmr_flco_ctx* ctx) {
    const int crc_ok = (*ctx->IrrecoverableErrors == 0 && ctx->CRCCorrect == 1);
    dmr_flco_detect_kenwood_sc(ctx, crc_ok);
    dmr_flco_detect_kirisun_le(ctx, crc_ok);
    dmr_flco_detect_hytera_xpt(ctx);
    dmr_flco_detect_invalid_hytera_enhanced(ctx);
    if (strcmp(ctx->state->dmr_branding_sub, "XPT ") == 0) {
        ctx->is_xpt = 1;
    }
}

static int
dmr_flco_is_protected(const dmr_flco_ctx* ctx) {
    int pf_overloaded_by_xpt = (ctx->fid == 0x68) && (ctx->flco == 0x09);
    return (ctx->pf == 1 && !pf_overloaded_by_xpt);
}

static void
dmr_flco_print_protected_lc(const dmr_flco_ctx* ctx) {
    if (!ctx->protected_lc) {
        return;
    }
    dmr_flco_print_type_color(ctx->type, KRED, KRED, KRED);
    dmr_print_slot_tag(ctx->state);
    DSD_FPRINTF(stderr, " Protected LC ");
}

static void
dmr_flco_store_flco_and_normalize(dmr_flco_ctx* ctx) {
    if (ctx->slot == 0) {
        ctx->state->dmr_flco = ctx->flco;
    } else {
        ctx->state->dmr_flcoR = ctx->flco;
    }

    if (ctx->fid == 0x10
        && (ctx->flco == 0x14 || ctx->flco == 0x15 || ctx->flco == 0x16 || ctx->flco == 0x17 || ctx->flco == 0x18)) {
        ctx->flco = (uint8_t)(ctx->flco - 0x10);
        ctx->fid = 0;
    }
}

static void
dmr_flco_handle_alias_header(dmr_flco_ctx* ctx) {
    if (!ctx->protected_lc && (ctx->fid == 0 || ctx->fid == 0x68) && ctx->type == 3 && ctx->flco == 0x04) {
        ctx->is_alias = 1;
        dmr_talker_alias_lc_header(ctx->opts, ctx->state, ctx->slot, ctx->lc_bits);
    }
}

static void
dmr_flco_handle_alias_blocks(dmr_flco_ctx* ctx) {
    if (!ctx->protected_lc && (ctx->fid == 0 || ctx->fid == 0x68) && ctx->type == 3 && ctx->flco > 0x04
        && ctx->flco < 0x08) {
        ctx->is_alias = 1;
        dmr_talker_alias_lc_blocks(ctx->opts, ctx->state, ctx->slot, ctx->flco - 5, ctx->lc_bits);
    }
}

static void
dmr_flco_handle_embedded_gps(dmr_flco_ctx* ctx) {
    if (!ctx->protected_lc && (ctx->fid == 0 || ctx->fid == 0x68) && ctx->type == 3 && ctx->flco == 0x08) {
        ctx->is_gps = 1;
        dmr_embedded_gps(ctx->opts, ctx->state, ctx->lc_bits);
    }
}

static void
dmr_flco_handle_cap_plus(dmr_flco_ctx* ctx) {
    if (ctx->type == 1 && ctx->fid == 0x10 && (ctx->flco == 0x04 || ctx->flco == 0x07)) {
        ctx->is_cap_plus = 1;
        (void)convert_bits_into_output(&ctx->lc_bits[48], 4);
        ctx->restchannel = (int)convert_bits_into_output(&ctx->lc_bits[52], 4);
        ctx->source = (uint32_t)convert_bits_into_output(&ctx->lc_bits[56], 16);
    }
}

static void
dmr_flco_handle_alias_gps_capplus(dmr_flco_ctx* ctx) {
    dmr_flco_handle_alias_header(ctx);
    dmr_flco_handle_alias_blocks(ctx);
    dmr_flco_handle_embedded_gps(ctx);
    dmr_flco_handle_cap_plus(ctx);
}

static int
dmr_flco_handle_motorola_or_tait(dmr_flco_ctx* ctx) {
    if (ctx->fid == 0x10 && (ctx->flco == 0x08 || ctx->flco == 0x28 || ctx->flco == 0x29)) {
        dmr_flco_print_type_color(ctx->type, KCYN, KCYN, KCYN);
        dmr_print_slot_tag(ctx->state);
        DSD_FPRINTF(stderr, " Motorola");
        ctx->unk = 1;
        return 1;
    }

    if (ctx->type == 2 && ctx->flco == 0x30) {
        DSD_FPRINTF(stderr, "%s \n", KRED);
        dmr_print_slot_tag(ctx->state);
        DSD_FPRINTF(stderr, " Data Terminator (TD_LC) ");
        DSD_FPRINTF(stderr, "%s", KNRM);

        ctx->state->data_header_format[ctx->slot] = 7;
        ctx->state->data_header_sap[ctx->slot] = 0;
        ctx->state->data_header_valid[ctx->slot] = 0;
        ctx->state->data_conf_data[ctx->slot] = 0;
        ctx->state->data_block_poc[ctx->slot] = 0;
        ctx->state->data_byte_ctr[ctx->slot] = 0;
        ctx->state->data_ks_start[ctx->slot] = 0;
        return 1;
    }

    if (ctx->fid == 0x58) {
        dmr_flco_print_type_color(ctx->type, KCYN, KCYN, KCYN);
        dmr_print_slot_tag(ctx->state);
        DSD_FPRINTF(stderr, " Tait");
        ctx->unk = 1;
        return 1;
    }

    return 0;
}

static void
dmr_flco_set_xpt_targets(dmr_flco_ctx* ctx) {
    ctx->target = (uint32_t)convert_bits_into_output(&ctx->lc_bits[32], 16);
    ctx->source = (uint32_t)convert_bits_into_output(&ctx->lc_bits[56], 16);
    for (int i = 0; i < 16; i++) {
        ctx->target_hash[i] = ctx->lc_bits[32 + i];
    }
    ctx->tg_hash = crc8(ctx->target_hash, 16);
}

static int
dmr_flco_handle_hytera_xpt_alert(dmr_flco_ctx* ctx) {
    if (!(ctx->fid == 0x68 && ctx->flco == 0x09)) {
        return 0;
    }

    ctx->xpt_int = (uint8_t)convert_bits_into_output(&ctx->lc_bits[16], 4);
    ctx->xpt_free = (uint8_t)convert_bits_into_output(&ctx->lc_bits[24], 4);
    ctx->xpt_hand = (uint8_t)convert_bits_into_output(&ctx->lc_bits[28], 4);
    dmr_flco_set_xpt_targets(ctx);
    (void)convert_bits_into_output(&ctx->lc_bits[20], 4);
    (void)convert_bits_into_output(&ctx->lc_bits[48], 8);

    DSD_FPRINTF(stderr, "%s \n", KGRN);
    dmr_print_slot_tag(ctx->state);
    DSD_FPRINTF(stderr, " ");
    if (ctx->opts->payload == 1) {
        DSD_FPRINTF(stderr, "FLCO=0x%02X FID=0x%02X ", ctx->flco, ctx->fid);
    }
    DSD_FPRINTF(stderr, "TGT=%u SRC=%u ", ctx->target, ctx->source);
    DSD_FPRINTF(stderr, "Hytera XPT ");
    if (ctx->reserved == 1) {
        DSD_FPRINTF(stderr, "Group ");
        if (ctx->target > 248 && ctx->target < 255) {
            DSD_FPRINTF(stderr, "Emergency ");
        }
        if (ctx->target == 255) {
            DSD_FPRINTF(stderr, "All ");
        }
    } else {
        DSD_FPRINTF(stderr, "Private ");
    }
    DSD_FPRINTF(stderr, "Call Alert ");

    if (ctx->opts->payload == 1) {
        DSD_FPRINTF(stderr, "\n  ");
        DSD_FPRINTF(stderr, "%s", KYEL);
    }

    if (ctx->reserved == 0 && ctx->opts->payload == 1) {
        DSD_FPRINTF(stderr, "TGT Hash=%d; ", ctx->tg_hash);
    }
    if (ctx->opts->payload == 1) {
        DSD_FPRINTF(stderr, "HSK=%X; ", ctx->xpt_hand);
        DSD_FPRINTF(stderr, "Handshake - ");
        if (ctx->xpt_hand == 0) {
            DSD_FPRINTF(stderr, "Ordinary; ");
        } else if (ctx->xpt_hand == 1) {
            DSD_FPRINTF(stderr, "Callback/Alarm Interrupt; ");
        } else if (ctx->xpt_hand == 2) {
            DSD_FPRINTF(stderr, "Release Channel Interrupt; ");
        } else {
            DSD_FPRINTF(stderr, "Reserved; ");
        }
        DSD_FPRINTF(stderr, "Call on LCN %d; ", ctx->xpt_int);
        DSD_FPRINTF(stderr, "Free LCN %d; ", ctx->xpt_free);
    }
    DSD_FPRINTF(stderr, "%s ", KNRM);
    DSD_SNPRINTF(ctx->state->dmr_site_parms, sizeof(ctx->state->dmr_site_parms), "Free LCN - %d ", ctx->xpt_free);
    return 1;
}

static int
dmr_flco_handle_hytera_unknown_or_fid(dmr_flco_ctx* ctx) {
    if (ctx->fid == 0x68 && (ctx->flco == 0x13 || ctx->flco == 0x31 || ctx->flco == 0x2E || ctx->flco == 0x2F)) {
        dmr_flco_print_type_color(ctx->type, KCYN, KCYN, KCYN);
        dmr_print_slot_tag(ctx->state);
        DSD_FPRINTF(stderr, " Hytera ");
        ctx->unk = 1;
        return 1;
    }

    if (ctx->fid != 0 && ctx->fid != 0x68 && ctx->fid != 0x10 && ctx->fid != 0x08 && ctx->is_kenwood_sc == 0) {
        dmr_flco_print_type_color(ctx->type, KYEL, KYEL, KYEL);
        dmr_print_slot_tag(ctx->state);
        DSD_FPRINTF(stderr, " Unknown LC ");
        ctx->unk = 1;
        return 1;
    }

    return 0;
}

static int
dmr_flco_handle_irrecoverable_hytera_enhanced(dmr_flco_ctx* ctx) {
    if (!(*ctx->IrrecoverableErrors != 0 && ctx->fid == 0x68 && ctx->flco == 0x02)) {
        return 0;
    }

    uint8_t checksum_bytes[8];
    uint8_t alg = (uint8_t)convert_bits_into_output(&ctx->lc_bits[0], 8);
    uint8_t key = (uint8_t)convert_bits_into_output(&ctx->lc_bits[16], 8);
    unsigned long long int mi = (unsigned long long int)convert_bits_into_output(&ctx->lc_bits[24], 40);
    DSD_FPRINTF(stderr, "%s", KYEL);
    if (dmr_slot_is_known(ctx->state)) {
        DSD_FPRINTF(stderr, " Slot %d Alg: %02X; KEY ID: %02X; MI(40): %010llX;", ctx->slot + 1, alg, key, mi);
    } else {
        DSD_FPRINTF(stderr, " Slot ? Alg: %02X; KEY ID: %02X; MI(40): %010llX;", alg, key, mi);
    }
    DSD_FPRINTF(stderr, " Hytera Enhanced; ");

    if (ctx->slot == 0 && ctx->state->R != 0) {
        char key_text[17];
        DSD_FPRINTF(stderr, "Key: %s; ",
                    dsd_secret_format_hex(key_text, sizeof key_text, dmr_flco_show_keys(ctx), ctx->state->R, 10U, 0));
    }
    if (ctx->slot == 1 && ctx->state->RR != 0) {
        char key_text[17];
        DSD_FPRINTF(stderr, "Key: %s; ",
                    dsd_secret_format_hex(key_text, sizeof key_text, dmr_flco_show_keys(ctx), ctx->state->RR, 10U, 0));
    }

    for (size_t i = 0; i < sizeof checksum_bytes; i++) {
        checksum_bytes[i] = (uint8_t)convert_bits_into_output(&ctx->lc_bits[i * 8U], 8);
    }

    if (dmr_hytera_checksum(checksum_bytes, sizeof checksum_bytes)
        == (uint8_t)convert_bits_into_output(&ctx->lc_bits[64], 8)) {
        if (ctx->slot == 0) {
            ctx->state->dmr_so |= 0x40;
            ctx->state->payload_algid = alg;
            ctx->state->payload_keyid = key;
            ctx->state->payload_mi = mi;
        } else {
            ctx->state->dmr_soR |= 0x40;
            ctx->state->payload_algidR = alg;
            ctx->state->payload_keyidR = key;
            ctx->state->payload_miR = mi;
        }
        ctx->opts->dmr_le = 2;
        *ctx->IrrecoverableErrors = 0;
    } else {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, " (Checksum Err);");
        DSD_FPRINTF(stderr, "\n");
    }

    DSD_FPRINTF(stderr, "%s ", KNRM);
    return 1;
}

static int
dmr_flco_handle_no_error_paths(dmr_flco_ctx* ctx) {
    dmr_flco_store_flco_and_normalize(ctx);
    dmr_flco_handle_alias_gps_capplus(ctx);
    if (dmr_flco_handle_motorola_or_tait(ctx)) {
        return 1;
    }
    if (ctx->is_xpt == 1) {
        dmr_flco_set_xpt_targets(ctx);
    }
    if (dmr_flco_handle_hytera_xpt_alert(ctx)) {
        return 1;
    }
    if (dmr_flco_handle_hytera_unknown_or_fid(ctx)) {
        return 1;
    }
    return 0;
}

static void
dmr_flco_reset_td_lc_slot0(dmr_flco_ctx* ctx) {
    ctx->state->dmr_fid = 0;
    ctx->state->dmr_so = 0;
    ctx->state->payload_algid = 0;
    ctx->state->payload_mi = 0;
    ctx->state->payload_keyid = 0;
    if (ctx->opts->floating_point == 1) {
        ctx->state->aout_gain = ctx->opts->audio_gain;
    }
    ctx->state->dmr_alias_block_len[0] = 0;
    ctx->state->dmr_alias_char_size[0] = 0;
    ctx->state->dmr_alias_format[0] = 0;
    DSD_SNPRINTF(ctx->state->generic_talker_alias[0], sizeof(ctx->state->generic_talker_alias[0]), "%s", "");
    DSD_MEMSET(ctx->state->dmr_pdu_sf[0], 0, sizeof(ctx->state->dmr_pdu_sf[0]));
    ctx->state->dmr_embedded_gps[0][0] = '\0';
    ctx->state->dmr_lrrp_gps[0][0] = '\0';
}

static void
dmr_flco_reset_td_lc_slot1(dmr_flco_ctx* ctx) {
    ctx->state->dmr_fidR = 0;
    ctx->state->dmr_soR = 0;
    ctx->state->payload_algidR = 0;
    ctx->state->payload_miR = 0;
    ctx->state->payload_keyidR = 0;
    if (ctx->opts->floating_point == 1) {
        ctx->state->aout_gainR = ctx->opts->audio_gain;
    }
    ctx->state->dmr_alias_block_len[1] = 0;
    ctx->state->dmr_alias_char_size[1] = 0;
    ctx->state->dmr_alias_format[1] = 0;
    DSD_SNPRINTF(ctx->state->generic_talker_alias[1], sizeof(ctx->state->generic_talker_alias[1]), "%s", "");
    DSD_MEMSET(ctx->state->dmr_pdu_sf[1], 0, sizeof(ctx->state->dmr_pdu_sf[1]));
    ctx->state->dmr_embedded_gps[1][0] = '\0';
    ctx->state->dmr_lrrp_gps[1][0] = '\0';
}

static void
dmr_flco_sync_active_call_state(dmr_flco_ctx* ctx) {
    if (ctx->state->currentslot == 0) {
        ctx->state->dmr_fid = ctx->fid;
        ctx->state->dmr_so = ctx->so;
    }
    if (ctx->state->currentslot == 1) {
        ctx->state->dmr_fidR = ctx->fid;
        ctx->state->dmr_soR = ctx->so;
    }
    if (ctx->opts->trunk_is_tuned == 1) {
        dsd_mark_vc_sync(ctx->state);
        dsd_mark_cc_sync(ctx->state);
    }
}

static dsd_call_kind
dmr_flco_call_kind(const dmr_flco_ctx* ctx) {
    if (ctx->flco == 0x03 || ctx->flco == 0x05 || ctx->flco == 0x07 || ctx->flco == 0x23) {
        return DSD_CALL_KIND_PRIVATE_VOICE;
    }
    return DSD_CALL_KIND_GROUP_VOICE;
}

static int
dmr_flco_voice_protocol(const dsd_state* state) {
    if (DSD_SYNC_IS_DMR(state->synctype)) {
        return state->synctype;
    }
    if (DSD_SYNC_IS_DMR(state->lastsynctype)) {
        return state->lastsynctype;
    }
    return DSD_SYNC_DMR_BS_VOICE_POS;
}

static void
dmr_flco_publish_crypto(const dmr_flco_ctx* ctx) {
    const int encrypted = (ctx->so & 0x40U) != 0U;
    const uint8_t algid = (uint8_t)(ctx->slot == 0U ? ctx->state->payload_algid : ctx->state->payload_algidR);
    const uint16_t kid = (uint16_t)(ctx->slot == 0U ? ctx->state->payload_keyid : ctx->state->payload_keyidR);
    const uint64_t mi = ctx->slot == 0U ? ctx->state->payload_mi : ctx->state->payload_miR;
    const int has_key = ctx->slot == 0U ? ctx->state->R != 0U : ctx->state->RR != 0U;
    const dsd_call_crypto_update crypto = {
        .classification = !encrypted ? DSD_CALL_CRYPTO_CLEAR
                          : has_key  ? DSD_CALL_CRYPTO_DECRYPTABLE
                                     : DSD_CALL_CRYPTO_ENCRYPTED_PENDING,
        .algid = algid,
        .kid = kid,
        .mi = mi,
        .audio_permitted = (uint8_t)(!encrypted || has_key),
    };
    (void)dsd_call_state_update_crypto(ctx->state, ctx->slot, &crypto);
}

static void
dmr_flco_publish_voice(dmr_flco_ctx* ctx) {
    if (ctx->slot >= DSD_CALL_STATE_SLOT_COUNT) {
        return;
    }
    const dsd_call_observation observation = {
        .protocol = dmr_flco_voice_protocol(ctx->state),
        .slot = ctx->slot,
        .kind = dmr_flco_call_kind(ctx),
        .ota_target_id = ctx->target,
        .policy_target_id = ctx->target,
        .ota_source_id = ctx->source,
        .channel = (uint32_t)(ctx->state->dmr_vc_lsn > 0 ? ctx->state->dmr_vc_lsn : ctx->state->dmr_vc_lcn),
        .frequency_hz = ctx->state->trunk_vc_freq[ctx->slot],
        .service_options = ctx->so,
        .emergency = (uint8_t)((ctx->so & 0x80U) != 0U),
        .priority = (uint8_t)(ctx->so & 0x03U),
    };
    const dsd_call_boundary boundary = ctx->type == 1U ? DSD_CALL_BOUNDARY_BEGIN : DSD_CALL_BOUNDARY_CONTINUE;
    (void)dsd_call_state_observe(ctx->state, &observation, boundary);
    dmr_flco_publish_crypto(ctx);
    dsd_event_sync_slot(ctx->opts, ctx->state, ctx->slot);
}

static int
dmr_flco_prepare_regular_state(dmr_flco_ctx* ctx) {
    if (*ctx->IrrecoverableErrors != 0 || ctx->is_alias != 0 || ctx->is_gps != 0) {
        return 0;
    }

    if (ctx->fid != 0) {
        ctx->state->dmr_mfid = ctx->fid;
    }

    if (ctx->protected_lc) {
        return -1;
    }

    if (ctx->type != 2) {
        dmr_flco_sync_active_call_state(ctx);
    }

    if (ctx->type == 2) {
        if (dsd_call_state_end(ctx->state, ctx->slot, 0.0) > 0) {
            dsd_event_sync_slot(ctx->opts, ctx->state, ctx->slot);
        }
        if (ctx->state->currentslot == 0) {
            dmr_flco_reset_td_lc_slot0(ctx);
        }
        if (ctx->state->currentslot == 1) {
            dmr_flco_reset_td_lc_slot1(ctx);
        }
    }

    if (ctx->restchannel != ctx->state->dmr_rest_channel && ctx->restchannel != -1) {
        ctx->state->dmr_rest_channel = ctx->restchannel;
    }

    return 1;
}

static void
dmr_flco_print_regular_header(dmr_flco_ctx* ctx) {
    dmr_flco_print_type_color(ctx->type, KGRN, KRED, KGRN);
    dmr_print_slot_tag(ctx->state);
    DSD_FPRINTF(stderr, " ");
    DSD_FPRINTF(stderr, "TGT=%u SRC=%u ", ctx->target, ctx->source);
    if (ctx->opts->payload == 1 && ctx->is_xpt == 1 && ctx->flco == 0x3) {
        DSD_FPRINTF(stderr, "HASH=%d ", ctx->tg_hash);
    }
    if (ctx->opts->payload == 1) {
        DSD_FPRINTF(stderr, "FLCO=0x%02X FID=0x%02X SVC=0x%02X ", ctx->flco, ctx->fid, ctx->so);
    }
}

static void
dmr_flco_print_call_class(const dmr_flco_ctx* ctx) {
    if (ctx->fid == 0x68) {
    } else if (ctx->flco == 0x4 || ctx->flco == 0x5 || ctx->flco == 0x7 || ctx->flco == 0x23) {
        DSD_FPRINTF(stderr, "Cap+ ");
        if (ctx->flco == 0x4) {
            DSD_FPRINTF(stderr, "Group ");
        } else {
            DSD_FPRINTF(stderr, "Private ");
        }
    } else if (ctx->flco == 0x3) {
        DSD_FPRINTF(stderr, "Private ");
    } else {
        DSD_FPRINTF(stderr, "Group ");
    }
}

static void
dmr_flco_print_emergency_flag(const dmr_flco_ctx* ctx) {
    if (ctx->so & 0x80) {
        DSD_FPRINTF(stderr, "%s", KRED);
        DSD_FPRINTF(stderr, "Emergency ");
    }
}

static void
dmr_flco_prepare_enc_lockout_labels(dmr_flco_ctx* ctx, unsigned int* lo, char* gm, size_t gm_sz, char* gn,
                                    size_t gn_sz) {
    dsd_tg_policy_entry lockout_entry;
    *lo = 0;
    if (ctx->target != 0 && dsd_tg_policy_lookup_label(ctx->state, ctx->target, gm, gm_sz, gn, gn_sz)) {
        *lo = 1;
    }
    if (*lo == 0) {
        if (dsd_tg_policy_make_exact_entry(ctx->target, "B", "ENC LO", DSD_TG_POLICY_SOURCE_ENC_LOCKOUT, &lockout_entry)
                == 0
            && dsd_tg_policy_upsert_exact(ctx->state, &lockout_entry, DSD_TG_POLICY_UPSERT_ADD_IF_MISSING) == 0) {
            DSD_SNPRINTF(gm, gm_sz, "%s", "B");
            DSD_SNPRINTF(gn, gn_sz, "%s", "ENC LO");
        } else {
            *lo = 1;
        }
    }
}

static void
dmr_flco_emit_enc_lockout_action(dmr_flco_ctx* ctx, const char* gm, const char* gn) {
    int eslot = ctx->state->currentslot & 1;
    int other = eslot ^ 1;
    int other_voice = (other == 0) ? (ctx->state->dmrburstL == 16) : (ctx->state->dmrburstR == 16);
    if (!other_voice) {
        uint8_t dummy[12];
        DSD_MEMSET(dummy, 0, sizeof(dummy));
        dummy[0] = 46;
        dummy[1] = 255;
        if ((strcmp(gm, "B") == 0) && (strcmp(gn, "ENC LO") == 0)) {
            uint8_t dbits_local[1] = {0};
            dmr_cspdu(ctx->opts, ctx->state, dbits_local, dummy, 1, 0);
        }
    } else if (ctx->opts->verbose > 0) {
        DSD_FPRINTF(stderr, " ENC lockout: other slot active with clear voice; stay on VC, mute enc slot. ");
    }
}

static void
dmr_flco_apply_enc_lockout(dmr_flco_ctx* ctx) {
    if (!(ctx->so & 0x40)) {
        return;
    }
    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, "Encrypted ");
    if (!(ctx->opts->trunk_enable == 1 && ctx->opts->trunk_tune_enc_calls == 0)) {
        return;
    }

    unsigned int lo = 0;
    char gm[8] = {0};
    char gn[50] = {0};
    dmr_flco_prepare_enc_lockout_labels(ctx, &lo, gm, sizeof(gm), gn, sizeof(gn));
    if (ctx->target != 0 && lo == 0) {
        DSD_SNPRINTF(ctx->state->event_history_s[ctx->slot].Event_History_Items[0].internal_str,
                     sizeof(ctx->state->event_history_s[ctx->slot].Event_History_Items[0].internal_str),
                     "Target: %d; has been locked out; Encryption Lock Out Enabled.", ctx->target);
        dsd_event_history_mark_dirty(&ctx->state->event_history_s[ctx->slot]);
        watchdog_event_current(ctx->opts, ctx->state, ctx->slot);
    }
    dmr_flco_emit_enc_lockout_action(ctx, gm, gn);
}

static void
dmr_flco_print_service_options(const dmr_flco_ctx* ctx) {
    if ((ctx->fid == 0x10) && (ctx->so & 0x20)) {
        DSD_FPRINTF(stderr, "TXI ");
    }
    if ((ctx->fid == 0x10) && (ctx->so & 0x10)) {
        DSD_FPRINTF(stderr, "RPT ");
    }
    if (ctx->so & 0x08) {
        DSD_FPRINTF(stderr, "Broadcast ");
    }
    if (ctx->so & 0x04) {
        DSD_FPRINTF(stderr, "OVCM ");
    }
    if (ctx->so & 0x03) {
        if ((ctx->so & 0x03) == 0x01) {
            DSD_FPRINTF(stderr, "Priority 1 ");
        } else if ((ctx->so & 0x03) == 0x02) {
            DSD_FPRINTF(stderr, "Priority 2 ");
        } else if ((ctx->so & 0x03) == 0x03) {
            DSD_FPRINTF(stderr, "Priority 3 ");
        } else {
            DSD_FPRINTF(stderr, "No Priority ");
        }
    }
}

static void
dmr_flco_print_branding(dmr_flco_ctx* ctx) {
    if (ctx->fid == 0x68) {
        DSD_FPRINTF(stderr, "Hytera ");
    }
    if (ctx->is_xpt) {
        DSD_FPRINTF(stderr, "XPT ");
    }
    if (ctx->fid == 0x68 && ctx->flco == 0x00) {
        DSD_FPRINTF(stderr, "Group ");
    }
    if (ctx->fid == 0x68 && ctx->flco == 0x03) {
        DSD_FPRINTF(stderr, "Private ");
    }
    if (ctx->is_kenwood_sc) {
        DSD_FPRINTF(stderr, "Kenwood Scrambler ");
    }
    DSD_FPRINTF(stderr, "Call ");
    if (ctx->is_cap_plus == 1 && ctx->restchannel != -1) {
        DSD_FPRINTF(stderr, "%s ", KYEL);
        DSD_FPRINTF(stderr, "Rest LSN: %d", ctx->restchannel);
    }
    DSD_FPRINTF(stderr, "%s ", KNRM);
}

static void
dmr_flco_print_tg_label(const dmr_flco_ctx* ctx) {
    char name[50];
    if (dsd_tg_policy_lookup_label(ctx->state, ctx->target, NULL, 0, name, sizeof(name))) {
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, "[%s] ", name);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

static void
dmr_flco_print_dmr_basic_keys(const dmr_flco_ctx* ctx) {
    if (ctx->state->K != 0 && ctx->fid == 0x10 && (ctx->so & 0x40) && ctx->slot == 0
        && ctx->state->payload_algid == 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[16];
        DSD_FPRINTF(stderr, "Key %s ",
                    dsd_secret_format_decimal(key_text, sizeof key_text, dmr_flco_show_keys(ctx), ctx->state->K, 0U));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
    if (ctx->state->K != 0 && ctx->fid == 0x10 && (ctx->so & 0x40) && ctx->slot == 1
        && ctx->state->payload_algidR == 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[16];
        DSD_FPRINTF(stderr, "Key %s ",
                    dsd_secret_format_decimal(key_text, sizeof key_text, dmr_flco_show_keys(ctx), ctx->state->K, 0U));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
dmr_flco_print_hytera_basic_key_slot0(const dmr_flco_ctx* ctx) {
    if (ctx->state->K1 != 0 && ctx->fid == 0x68 && (ctx->so & 0x40) && ctx->slot == 0
        && ctx->state->payload_algid == 0) {
        const unsigned int segment_count = dmr_flco_hytera_key_segment_count(ctx);
        if (segment_count >= 2U) {
            DSD_FPRINTF(stderr, "\n ");
        }
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[68];
        DSD_FPRINTF(stderr, "Key %s ", dmr_flco_format_hytera_basic_key(key_text, sizeof key_text, ctx, segment_count));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
dmr_flco_print_hytera_basic_key_slot1(const dmr_flco_ctx* ctx) {
    if (ctx->state->K1 != 0 && ctx->fid == 0x68 && (ctx->so & 0x40) && ctx->slot == 1
        && ctx->state->payload_algidR == 0) {
        const unsigned int segment_count = dmr_flco_hytera_key_segment_count(ctx);
        if (segment_count >= 2U) {
            DSD_FPRINTF(stderr, "\n ");
        }
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[68];
        DSD_FPRINTF(stderr, "Key %s ", dmr_flco_format_hytera_basic_key(key_text, sizeof key_text, ctx, segment_count));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
dmr_flco_print_alg21_keys(const dmr_flco_ctx* ctx) {
    if (ctx->slot == 0 && ctx->state->payload_algid == 0x21 && ctx->state->R != 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[17];
        DSD_FPRINTF(stderr, "Key %s ",
                    dsd_secret_format_hex(key_text, sizeof key_text, dmr_flco_show_keys(ctx), ctx->state->R, 10U, 0));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
    if (ctx->slot == 1 && ctx->state->payload_algidR == 0x21 && ctx->state->RR != 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[17];
        DSD_FPRINTF(stderr, "Key %s ",
                    dsd_secret_format_hex(key_text, sizeof key_text, dmr_flco_show_keys(ctx), ctx->state->RR, 10U, 0));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
dmr_flco_print_loaded_keys(const dmr_flco_ctx* ctx) {
    dmr_flco_print_dmr_basic_keys(ctx);
    dmr_flco_print_hytera_basic_key_slot0(ctx);
    dmr_flco_print_hytera_basic_key_slot1(ctx);
    dmr_flco_print_alg21_keys(ctx);
}

static void
dmr_flco_print_alg02_keys(const dmr_flco_ctx* ctx) {
    if (ctx->slot == 0 && ctx->state->payload_algid == 0x02 && ctx->state->R != 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[17];
        DSD_FPRINTF(stderr, "Key: %s ",
                    dsd_secret_format_hex(key_text, sizeof key_text, dmr_flco_show_keys(ctx), ctx->state->R, 10U, 0));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
    if (ctx->slot == 1 && ctx->state->payload_algidR == 0x02 && ctx->state->RR != 0) {
        DSD_FPRINTF(stderr, "%s", KYEL);
        char key_text[17];
        DSD_FPRINTF(stderr, "Key: %s ",
                    dsd_secret_format_hex(key_text, sizeof key_text, dmr_flco_show_keys(ctx), ctx->state->RR, 10U, 0));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
dmr_flco_print_aes_24_25_keys(const dmr_flco_ctx* ctx) {
    if (ctx->slot == 0 && (ctx->state->payload_algid == 0x25 || ctx->state->payload_algid == 0x24)
        && ctx->state->aes_key_loaded[0] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "%s", KYEL);
        const unsigned long long segments[4] = {ctx->state->A1[0], ctx->state->A2[0], ctx->state->A3[0],
                                                ctx->state->A4[0]};
        char key_text[68];
        DSD_FPRINTF(stderr, "Key: %s ",
                    dsd_secret_format_u64_segments(key_text, sizeof key_text, dmr_flco_show_keys(ctx), segments,
                                                   (ctx->state->payload_algid == 0x25) ? 4U : 2U));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
    if (ctx->slot == 1 && (ctx->state->payload_algidR == 0x25 || ctx->state->payload_algidR == 0x24)
        && ctx->state->aes_key_loaded[1] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "%s", KYEL);
        const unsigned long long segments[4] = {ctx->state->A1[1], ctx->state->A2[1], ctx->state->A3[1],
                                                ctx->state->A4[1]};
        char key_text[68];
        DSD_FPRINTF(stderr, "Key: %s ",
                    dsd_secret_format_u64_segments(key_text, sizeof key_text, dmr_flco_show_keys(ctx), segments,
                                                   (ctx->state->payload_algidR == 0x25) ? 4U : 2U));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
dmr_flco_print_aes_36_37_keys(const dmr_flco_ctx* ctx) {
    if (ctx->slot == 0 && (ctx->state->payload_algid == 0x36 || ctx->state->payload_algid == 0x37)
        && ctx->state->aes_key_loaded[0] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "%s", KYEL);
        const unsigned long long segments[4] = {ctx->state->A1[0], ctx->state->A2[0], ctx->state->A3[0],
                                                ctx->state->A4[0]};
        char key_text[68];
        DSD_FPRINTF(stderr, "Key: %s",
                    dsd_secret_format_u64_segments(key_text, sizeof key_text, dmr_flco_show_keys(ctx), segments, 4U));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
    if (ctx->slot == 1 && (ctx->state->payload_algidR == 0x36 || ctx->state->payload_algidR == 0x37)
        && ctx->state->aes_key_loaded[1] == 1) {
        DSD_FPRINTF(stderr, "\n ");
        DSD_FPRINTF(stderr, "%s", KYEL);
        const unsigned long long segments[4] = {ctx->state->A1[1], ctx->state->A2[1], ctx->state->A3[1],
                                                ctx->state->A4[1]};
        char key_text[68];
        DSD_FPRINTF(stderr, "Key: %s",
                    dsd_secret_format_u64_segments(key_text, sizeof key_text, dmr_flco_show_keys(ctx), segments, 4U));
        DSD_FPRINTF(stderr, "%s ", KNRM);
    }
}

static void
dmr_flco_print_extended_keys(const dmr_flco_ctx* ctx) {
    dmr_flco_print_alg02_keys(ctx);
    dmr_flco_print_aes_24_25_keys(ctx);
    dmr_flco_print_aes_36_37_keys(ctx);
}

static void
dmr_flco_finalize(dmr_flco_ctx* ctx) {
    if (ctx->unk == 1 || ctx->pf == 1) {
        DSD_FPRINTF(stderr, " FLCO=0x%02X FID=0x%02X ", ctx->flco, ctx->fid);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
    if (*ctx->IrrecoverableErrors != 0) {
        if (ctx->type != 3) {
            DSD_FPRINTF(stderr, "\n");
        }
        DSD_FPRINTF(stderr, "%s", KRED);
        dmr_print_slot_tag(ctx->state);
        DSD_FPRINTF(stderr, " FLCO FEC ERR ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    }
}

//combined flco handler (vlc, tlc, emb), minus the superfluous structs and strings
void
dmr_flco(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[], uint32_t CRCCorrect, uint32_t* IrrecoverableErrors,
         uint8_t type) {
    dmr_flco_ctx ctx;
    dmr_flco_ctx_init(&ctx, opts, state, lc_bits, CRCCorrect, IrrecoverableErrors, type);
    dmr_flco_detect_special_modes(&ctx);
    ctx.protected_lc = dmr_flco_is_protected(&ctx);
    dmr_flco_print_protected_lc(&ctx);

    if (*ctx.IrrecoverableErrors == 0) {
        if (dmr_flco_handle_no_error_paths(&ctx)) {
            dmr_flco_finalize(&ctx);
            return;
        }
    } else if (dmr_flco_handle_irrecoverable_hytera_enhanced(&ctx)) {
        dmr_flco_finalize(&ctx);
        return;
    }

    int regular_state = dmr_flco_prepare_regular_state(&ctx);
    if (regular_state > 0) {
        dmr_flco_print_regular_header(&ctx);
        dmr_flco_print_call_class(&ctx);
        const int is_private = dmr_flco_call_kind(&ctx) == DSD_CALL_KIND_PRIVATE_VOICE;
        dsd_trunk_scan_hook_dmr_conventional_activity(opts, state, ctx.target, ctx.source, is_private,
                                                      (ctx.so & 0x40U) != 0U, 0);
        dmr_flco_print_emergency_flag(&ctx);
        dmr_flco_apply_enc_lockout(&ctx);
        dmr_flco_print_service_options(&ctx);
        dmr_flco_print_branding(&ctx);
        dmr_flco_print_tg_label(&ctx);
        dmr_flco_print_loaded_keys(&ctx);
        dmr_flco_print_extended_keys(&ctx);
        if (ctx.type != 2U && ctx.CRCCorrect == 1U) {
            dmr_flco_publish_voice(&ctx);
        }
    }

    dmr_flco_finalize(&ctx);
}

static const char*
dmr_activity_type_label(uint8_t activity, char* fallback, size_t fallback_sz) {
    switch (activity) {
        case 0x0: return "Idle";
        case 0x2: return "Group CSBK";
        case 0x3: return "Ind CSBK";
        case 0x8: return "Group Voice";
        case 0x9: return "Ind Voice";
        case 0xA: return "Ind Data";
        case 0xB: return "Group Data";
        case 0xC: return "Group Emergency";
        case 0xD: return "Ind Emergency";
        default: DSD_SNPRINTF(fallback, fallback_sz, "Res %X", activity); return fallback;
    }
}

static const char*
dmr_model_label(uint8_t model) {
    switch (model) {
        case 0: return "Tiny";
        case 1: return "Small";
        case 2: return "Large";
        case 3: return "Huge";
        default: return "Unknown";
    }
}

static int
dmr_is_voice_payload_active(const dsd_opts* opts, const dsd_state* state) {
    return opts->payload == 1
           && ((state->dmrburstL == 16 && state->currentslot == 0)
               || (state->dmrburstR == 16 && state->currentslot == 1));
}

static void
dmr_cach_reset_fragments(dsd_state* state) {
    state->dmr_cach_counter = 0;
    DSD_MEMSET(state->dmr_cach_fragment, 1, sizeof(state->dmr_cach_fragment));
}

static void
dmr_cach_print_single_fragment(const dsd_state* state, uint8_t slco_bits[68]) {
    uint8_t slco = (uint8_t)convert_bits_into_output(&slco_bits[0], 4);

    DSD_FPRINTF(stderr, "\n%s", KYEL);
    dmr_print_slot_tag(state);
    if (slco == 0x0) {
        DSD_FPRINTF(stderr, " SLCO NULL (single) ");
    } else if (slco == 0x1) {
        uint8_t ts1_act = (uint8_t)convert_bits_into_output(&slco_bits[4], 4);
        uint8_t ts2_act = (uint8_t)convert_bits_into_output(&slco_bits[8], 4);
        char ts1_buf[16];
        char ts2_buf[16];
        const char* ts1_str = dmr_activity_type_label(ts1_act, ts1_buf, sizeof(ts1_buf));
        const char* ts2_str = dmr_activity_type_label(ts2_act, ts2_buf, sizeof(ts2_buf));
        DSD_FPRINTF(stderr, " SLC Activity (single) TS1: %s; TS2: %s;", ts1_str, ts2_str);
    } else if (slco == 0x2 || slco == 0x3) {
        uint8_t model = (uint8_t)convert_bits_into_output(&slco_bits[4], 2);
        if (slco == 0x2) {
            DSD_FPRINTF(stderr, " SLC C_SYS_PARMS (single) Model=%s", dmr_model_label(model));
        } else {
            DSD_FPRINTF(stderr, " SLC P_SYS_PARMS (single) Model=%s", dmr_model_label(model));
        }
    } else if (slco == 0x8) {
        DSD_FPRINTF(stderr, " SLCO Hytera XPT (single)");
    } else if (slco == 0x9) {
        DSD_FPRINTF(stderr, " SLCO Connect Plus Traffic (single)");
    } else if (slco == 0xA) {
        DSD_FPRINTF(stderr, " SLCO Connect Plus Control (single)");
    } else {
        DSD_FPRINTF(stderr, " SLC (single) OPC=0x%X ", slco);
    }
    DSD_FPRINTF(stderr, "%s", KNRM);
}

static uint8_t
dmr_cach_handle_single_fragment(dsd_state* state, uint8_t cach_bits[25], uint8_t slco_bits[68], uint8_t err) {
    for (int i = 0; i < 17; i++) {
        slco_bits[i] = cach_bits[i + 7];
    }

    if (!Hamming17123(slco_bits + 0)) {
        return 1;
    }

    int slot = state->currentslot;
    time_t now = time(NULL);
    if (state->slco_sfrag_last[slot] != 0 && (now - state->slco_sfrag_last[slot]) < 1) {
        return err;
    }
    state->slco_sfrag_last[slot] = now;

    dmr_cach_print_single_fragment(state, slco_bits);
    return err;
}

static void
dmr_cach_store_fragment(dsd_state* state, uint8_t cach_bits[25]) {
    for (int i = 0; i < 17; i++) {
        state->dmr_cach_fragment[state->dmr_cach_counter][i] = cach_bits[i + 7];
    }
}

static void
dmr_cach_log_crc_error(const dsd_opts* opts, const dsd_state* state) {
    if (!dmr_is_voice_payload_active(opts, state)) {
        DSD_FPRINTF(stderr, "\n");
    }
    DSD_FPRINTF(stderr, "%s", KRED);
    DSD_FPRINTF(stderr, " SLCO CRC ERR");
    DSD_FPRINTF(stderr, "%s", KNRM);
    if (dmr_is_voice_payload_active(opts, state)) {
        DSD_FPRINTF(stderr, "\n");
    }
}

static void
dmr_cach_process_final_fragment(dsd_opts* opts, dsd_state* state) {
    uint8_t slco_raw_bits[68];
    uint8_t slco_bits[68];

    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 17; i++) {
            slco_raw_bits[i + (17 * j)] = state->dmr_cach_fragment[j][i];
        }
    }

    int i = 0;
    for (; i < 67; i++) {
        int src = (i * 4) % 67;
        slco_bits[i] = slco_raw_bits[src];
    }
    slco_bits[i] = slco_raw_bits[i];

    bool h1 = Hamming17123(slco_bits + 0);
    bool h2 = Hamming17123(slco_bits + 17);
    bool h3 = Hamming17123(slco_bits + 34);

    for (int k = 17; k < 29; k++) {
        slco_bits[k - 5] = slco_bits[k];
    }
    for (int k = 34; k < 46; k++) {
        slco_bits[k - 10] = slco_bits[k];
    }
    for (int k = 36; k < 68; k++) {
        slco_bits[k] = 0;
    }

    if (h1 && h2 && h3 && crc8_ok(slco_bits, 36)) {
        dmr_slco(opts, state, slco_bits);
    } else {
        dmr_cach_log_crc_error(opts, state);
    }
}

//externalized dmr cach - tact and slco fragment handling
uint8_t
dmr_cach(dsd_opts* opts, dsd_state* state, uint8_t cach_bits[25]) {
    uint8_t err = 0;
    uint8_t tact_bits[7];
    uint8_t lcss = 0;
    int tact_valid = 0;

    for (int i = 0; i < 7; i++) {
        tact_bits[i] = cach_bits[i];
    }

    if (Hamming_7_4_decode(tact_bits)) {
        lcss = (tact_bits[2] << 1) | tact_bits[3];
        tact_valid = 1;
    } else {
        err = 1;
    }

    if (tact_valid && lcss == 0) {
        uint8_t slco_bits[68];
        dmr_cach_reset_fragments(state);
        return dmr_cach_handle_single_fragment(state, cach_bits, slco_bits, err);
    }

    if (lcss == 1) {
        dmr_cach_reset_fragments(state);
    } else if (lcss == 3) {
        state->dmr_cach_counter++;
    } else if (lcss == 2) {
        state->dmr_cach_counter = 3;
    }

    if (state->dmr_cach_counter > 3) {
        dmr_cach_reset_fragments(state);
        return 1;
    }

    dmr_cach_store_fragment(state, cach_bits);
    if (lcss == 2) {
        dmr_cach_process_final_fragment(opts, state);
    }

    return err;
}

typedef struct {
    uint8_t slco;
    uint8_t reg;
    uint16_t csc;
    uint16_t net;
    uint16_t site;
    uint16_t n;
    uint16_t sub_mask;
    char model_str[8];
    char ts1_str[25];
    char ts2_str[25];
    uint8_t ts1_hash;
    uint8_t ts2_hash;
    uint8_t con_netid;
    uint8_t con_siteid;
    uint8_t capsite;
    uint8_t restchannel;
    uint8_t cap_reserved;
    uint8_t xpt_free;
    uint8_t xpt_pri;
    uint8_t xpt_hash;
} dmr_slco_data;

static void
dmr_slco_tune_and_reset(dsd_opts* opts, dsd_state* state) {
    if (state->trunk_cc_freq == 0) {
        return;
    }
    dsd_trunk_tune_result tune_result = dsd_trunk_tuning_hook_tune_to_cc(opts, state, state->trunk_cc_freq, 0, NULL);
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        return;
    }
    opts->trunk_is_tuned = 0;
    state->p25_vc_freq[0] = state->p25_vc_freq[1] = 0;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    dmr_reset_blocks(opts, state);
}

static void
dmr_slco_fill_sys_fields(const dsd_opts* opts, uint8_t slco_bits[], dmr_slco_data* data) {
    uint8_t model = (uint8_t)convert_bits_into_output(&slco_bits[4], 2);
    uint16_t site_bits = 0;
    uint16_t default_n = dmr_tiii_model_default_split_n(model);
    DSD_SNPRINTF(data->model_str, sizeof(data->model_str), "%s", "");

    if (model == 0) {
        data->net = (uint16_t)convert_bits_into_output(&slco_bits[6], 9);
        data->site = (uint16_t)convert_bits_into_output(&slco_bits[15], 3);
        DSD_SNPRINTF(data->model_str, sizeof(data->model_str), "%s", "Tiny");
        site_bits = 3;
    } else if (model == 1) {
        data->net = (uint16_t)convert_bits_into_output(&slco_bits[6], 7);
        data->site = (uint16_t)convert_bits_into_output(&slco_bits[13], 5);
        DSD_SNPRINTF(data->model_str, sizeof(data->model_str), "%s", "Small");
        site_bits = 5;
    } else if (model == 2) {
        data->net = (uint16_t)convert_bits_into_output(&slco_bits[6], 4);
        data->site = (uint16_t)convert_bits_into_output(&slco_bits[10], 8);
        DSD_SNPRINTF(data->model_str, sizeof(data->model_str), "%s", "Large");
        site_bits = 8;
    } else if (model == 3) {
        data->net = (uint16_t)convert_bits_into_output(&slco_bits[6], 2);
        data->site = (uint16_t)convert_bits_into_output(&slco_bits[8], 10);
        DSD_SNPRINTF(data->model_str, sizeof(data->model_str), "%s", "Huge");
        site_bits = 10;
    }

    data->n = dmr_tiii_effective_split_n(default_n, opts->dmr_dmrla_is_set, opts->dmr_dmrla_n, site_bits);
    data->sub_mask = dmr_tiii_subsite_mask(data->n);
}

static void
dmr_slco_fill_activity_strings(uint8_t slco_bits[], dmr_slco_data* data) {
    uint8_t ts1_act = (uint8_t)convert_bits_into_output(&slco_bits[4], 4);
    uint8_t ts2_act = (uint8_t)convert_bits_into_output(&slco_bits[8], 4);
    char ts1_fb[16];
    const char* ts1 = dmr_activity_type_label(ts1_act, ts1_fb, sizeof(ts1_fb));
    DSD_SNPRINTF(data->ts1_str, sizeof(data->ts1_str), "%s", ts1);

    char ts2_fb[16];
    const char* ts2 = dmr_activity_type_label(ts2_act, ts2_fb, sizeof(ts2_fb));
    DSD_SNPRINTF(data->ts2_str, sizeof(data->ts2_str), "%s", ts2);

    data->ts1_hash = (uint8_t)convert_bits_into_output(&slco_bits[12], 8);
    data->ts2_hash = (uint8_t)convert_bits_into_output(&slco_bits[20], 8);
}

static void
dmr_slco_decode(uint8_t slco_bits[], const dsd_opts* opts, dmr_slco_data* data) {
    DSD_MEMSET(data, 0, sizeof(*data));
    data->slco = (uint8_t)convert_bits_into_output(&slco_bits[0], 4);
    data->reg = slco_bits[18];
    data->csc = (uint16_t)convert_bits_into_output(&slco_bits[19], 9);
    data->con_netid = (uint8_t)convert_bits_into_output(&slco_bits[8], 8);
    data->con_siteid = (uint8_t)convert_bits_into_output(&slco_bits[16], 8);
    data->capsite = (uint8_t)convert_bits_into_output(&slco_bits[22], 3);
    data->restchannel = (uint8_t)convert_bits_into_output(&slco_bits[16], 4);
    data->cap_reserved = (uint8_t)convert_bits_into_output(&slco_bits[20], 2);
    data->xpt_free = (uint8_t)convert_bits_into_output(&slco_bits[12], 4);
    data->xpt_pri = (uint8_t)convert_bits_into_output(&slco_bits[16], 4);
    data->xpt_hash = (uint8_t)convert_bits_into_output(&slco_bits[20], 8);
    dmr_slco_fill_activity_strings(slco_bits, data);
    if (data->slco == 0x2 || data->slco == 0x3) {
        dmr_slco_fill_sys_fields(opts, slco_bits, data);
    }
}

static void
dmr_slco_print_tiii_site_parms(dsd_state* state, const dmr_slco_data* data, uint16_t syscode) {
    if (data->n != 0) {
        uint16_t display_net = dmr_tiii_display_net(data->net, data->n);
        uint16_t display_site = dmr_tiii_display_site(data->site, data->n);
        uint16_t display_subsite = dmr_tiii_display_subsite(data->site, data->sub_mask, data->n);
        DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "TIII %s:%d-%d.%d;%04X; ", data->model_str,
                     display_net, display_site, display_subsite, syscode);
    } else {
        DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "TIII %s:%d-%d;%04X; ", data->model_str,
                     data->net, data->site, syscode);
    }
}

static void
dmr_slco_handle_c_sys_parms(const dsd_opts* opts, dsd_state* state, uint8_t slco_bits[], const dmr_slco_data* data) {
    uint16_t syscode = (uint16_t)convert_bits_into_output(&slco_bits[4], 14);
    if (data->n != 0) {
        uint16_t display_net = dmr_tiii_display_net(data->net, data->n);
        uint16_t display_site = dmr_tiii_display_site(data->site, data->n);
        uint16_t display_subsite = dmr_tiii_display_subsite(data->site, data->sub_mask, data->n);
        DSD_FPRINTF(stderr, " SLC_C_SYS_PARMS: %s; Net ID: %d; Site ID: %d.%d; Reg Req: %d; CSC: %d;", data->model_str,
                    display_net, display_site, display_subsite, data->reg, data->csc);
    } else {
        DSD_FPRINTF(stderr, " SLC_C_SYS_PARMS: %s; Net ID: %d; Site ID: %d; Reg Req: %d;", data->model_str, data->net,
                    data->site, data->reg);
    }
    DSD_FPRINTF(stderr, " SYS: %04X;", syscode);
    dmr_slco_print_tiii_site_parms(state, data, syscode);

    if (opts->use_rigctl == 1 && state->trunk_cc_freq == 0) {
        long int ccfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
        }
    }
}

static void
dmr_slco_handle_p_sys_parms(dsd_state* state, uint8_t slco_bits[], const dmr_slco_data* data) {
    uint16_t syscode = (uint16_t)convert_bits_into_output(&slco_bits[4], 14);
    if (data->n != 0) {
        uint16_t display_net = dmr_tiii_display_net(data->net, data->n);
        uint16_t display_site = dmr_tiii_display_site(data->site, data->n);
        uint16_t display_subsite = dmr_tiii_display_subsite(data->site, data->sub_mask, data->n);
        DSD_FPRINTF(stderr, " SLC_P_SYS_PARMS: %s; Net ID: %d; Site ID: %d.%d; Comp CC: %d;", data->model_str,
                    display_net, display_site, display_subsite, data->reg);
    } else {
        DSD_FPRINTF(stderr, " SLC_P_SYS_PARMS: %s; Net ID: %d; Site ID: %d;", data->model_str, data->net, data->site);
    }
    DSD_FPRINTF(stderr, " SYS: %04X;", syscode);
    dmr_slco_print_tiii_site_parms(state, data, syscode);
}

static int
dmr_slco_cap_plus_busy(const dsd_state* state) {
    return (state->dmrburstL == 16 || state->dmrburstL == 0 || state->dmrburstL == 1 || state->dmrburstL == 2)
           && (state->dmrburstR == 16 || state->dmrburstR == 0 || state->dmrburstR == 1 || state->dmrburstR == 2);
}

static int
dmr_slco_tg_hold_not_on_slot(const dsd_state* state) {
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        dsd_call_snapshot call;
        if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
            continue;
        }
        const uint64_t target = call.policy_target_id != 0U ? call.policy_target_id : call.ota_target_id;
        if (target == (uint64_t)state->tg_hold) {
            return 0;
        }
    }
    return 1;
}

static void
dmr_slco_handle_cap_plus(dsd_opts* opts, dsd_state* state, const dmr_slco_data* data) {
    DSD_FPRINTF(stderr, " SLCO Capacity Plus Site: %d - Rest LSN: %d - RS: %02X", data->capsite, data->restchannel,
                data->cap_reserved);

    if (state->tg_hold != 0 && opts->trunk_enable == 1 && dmr_slco_cap_plus_busy(state)
        && dmr_slco_tg_hold_not_on_slot(state)) {
        if (state->trunk_chan_map[data->restchannel] != 0) {
            state->trunk_cc_freq = state->trunk_chan_map[data->restchannel];
        }
        dmr_slco_tune_and_reset(opts, state);
    }
}

static uint8_t
dmr_slco_xpt_lcn_to_lsn(uint8_t lcn) {
    switch (lcn) {
        case 2: return 3;
        case 3: return 5;
        case 4: return 7;
        case 5: return 9;
        case 6: return 11;
        case 7: return 13;
        case 8: return 15;
        default: return lcn;
    }
}

static void
dmr_slco_handle_xpt(dsd_opts* opts, dsd_state* state, const dmr_slco_data* data) {
    DSD_FPRINTF(stderr, " SLCO Hytera XPT - Free LCN %d - PRI LCN %d - PRI HASH: %02X", data->xpt_free, data->xpt_pri,
                data->xpt_hash);
    DSD_SNPRINTF(state->dmr_branding_sub, sizeof(state->dmr_branding_sub), "XPT ");
    DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "Free LCN - %d ", data->xpt_free);

    if (state->tg_hold != 0 && opts->trunk_enable == 1 && state->dmrburstL == 16 && state->dmrburstR == 16
        && dmr_slco_tg_hold_not_on_slot(state)) {
        uint8_t xpt_lsn = dmr_slco_xpt_lcn_to_lsn(data->xpt_free);
        if (state->trunk_chan_map[xpt_lsn] != 0) {
            state->trunk_cc_freq = state->trunk_chan_map[xpt_lsn];
        }
        dmr_slco_tune_and_reset(opts, state);
    }
}

static void
dmr_slco_handle_con_plus_control(dsd_opts* opts, dsd_state* state, const dmr_slco_data* data) {
    DSD_FPRINTF(stderr, " SLCO Connect Plus Control Channel - Net ID: %d Site ID: %d", data->con_netid,
                data->con_siteid);
    DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "%d-%d ", data->con_netid, data->con_siteid);

    if (opts->use_rigctl == 1 && opts->trunk_is_tuned == 0) {
        long int ccfreq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
        }
    }
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->trunk_is_tuned == 0) {
        long int ccfreq = (long int)opts->rtlsdr_center_freq;
        if (ccfreq != 0) {
            state->trunk_cc_freq = ccfreq;
        }
    }
    if ((time(NULL) - state->last_vc_sync_time) > 2) {
        rotate_symbol_out_file(opts, state);
    }
}

static void
dmr_slco_print_completed_block(const dsd_opts* opts, uint8_t slco, const uint8_t slco_bytes[6]) {
    if (opts->payload == 1 && slco != 0) {
        DSD_FPRINTF(stderr, "\n SLCO Completed Block ");
        for (int i = 0; i < 5; i++) {
            DSD_FPRINTF(stderr, "[%02X]", slco_bytes[i]);
        }
        DSD_FPRINTF(stderr, "\n");
    }
}

static void
dmr_slco(dsd_opts* opts, dsd_state* state, uint8_t slco_bits[]) {
    uint8_t slco_bytes[6];
    dmr_slco_data data;

    for (int i = 0; i < 5; i++) {
        slco_bytes[i] = (uint8_t)convert_bits_into_output(&slco_bits[((size_t)i * 8)], 8);
    }
    slco_bytes[5] = (uint8_t)convert_bits_into_output(&slco_bits[32], 4);

    dmr_slco_decode(slco_bits, opts, &data);

    DSD_FPRINTF(stderr, "\n");
    DSD_FPRINTF(stderr, "%s", KYEL);

    if (data.slco == 0x2) {
        dmr_slco_handle_c_sys_parms(opts, state, slco_bits, &data);
    } else if (data.slco == 0x3) {
        dmr_slco_handle_p_sys_parms(state, slco_bits, &data);
    } else if (data.slco == 0x0) {
        DSD_FPRINTF(stderr, " SLCO NULL ");
    } else if (data.slco == 0x1) {
        DSD_FPRINTF(stderr, " Activity Update");
        DSD_FPRINTF(stderr, " TS1: %s; Hash: %d;", data.ts1_str, data.ts1_hash);
        DSD_FPRINTF(stderr, " TS2: %s; Hash: %d;", data.ts2_str, data.ts2_hash);
    } else if (data.slco == 0x9) {
        DSD_FPRINTF(stderr, " SLCO Connect Plus Traffic Channel - Net ID: %d Site ID: %d", data.con_netid,
                    data.con_siteid);
        DSD_SNPRINTF(state->dmr_site_parms, sizeof(state->dmr_site_parms), "%d-%d ", data.con_netid, data.con_siteid);
    } else if (data.slco == 0xA) {
        dmr_slco_handle_con_plus_control(opts, state, &data);
    } else if (data.slco == 0xF) {
        dmr_slco_handle_cap_plus(opts, state, &data);
    } else if (data.slco == 0x08) {
        dmr_slco_handle_xpt(opts, state, &data);
    } else {
        DSD_FPRINTF(stderr, " SLCO Unknown - %d ", data.slco);
    }

    dmr_slco_print_completed_block(opts, data.slco, slco_bytes);
    DSD_FPRINTF(stderr, "%s", KNRM);
}
