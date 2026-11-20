// SPDX-License-Identifier: GPL-3.0-or-later
//NXDN frame handler
//Reworked portions from Osmocom OP25 rx_sync.cc

/* -*- c++ -*- */
/*
 * NXDN Encoder/Decoder (C) Copyright 2019 Max H. Parke KA1RBI
 *
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/key_presence.h>
#include <dsd-neo/core/nxdn_channel_access.h>

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/receive.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol_anchor.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>

#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/nxdn/nxdn_voice.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_confirm.h"
#include "nxdn_internal.h"

#ifdef LIMAZULUTWEAKS
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include "dsd-neo/core/secret_redaction.h"
#endif

typedef struct {
    uint8_t dbuf[182];
    uint8_t dbuf_reliab[182];

    uint8_t lich;
    uint8_t lich_full;
    uint8_t lich_dibits[8];
    uint8_t lich_bits[16];
    uint16_t lich_bits_hex;

    int lich_parity_received;
    int lich_parity_computed;

    int voice;
    int facch;
    int facch2;
    int udch;
    int sacch;
    int cac;

    int idas;
    int scch;
    int facch3;
    int udch2;

    int sacch2;
    int pich_tch;

    /* The LICH read as a composite control channel's (RTCH_C) rather than a Type-D RTCH2's (nxdn_apply_lich_profile()). */
    int composite;
    /* Another channel's control traffic heard on the followed traffic channel (dsd_nxdn_control_heard_is_foreign()). */
    int foreign;

    uint8_t lich_rf;
    uint8_t direction;

    uint8_t sacch_bits[60];
    uint8_t sacch_reliab[60];
    uint8_t facch_bits_a[144];
    uint8_t facch_reliab_a[144];
    uint8_t facch_bits_b[144];
    uint8_t facch_reliab_b[144];
    uint8_t cac_bits[300];
    uint8_t cac_reliab[300];
    uint8_t facch2_bits[348];
    uint8_t facch2_reliab[348];
    uint8_t facch3_bits[288];
    uint8_t facch3_reliab[288];

    int nxdn_bit_buffer[364];
    uint8_t nxdn_reliab_buffer[364];

    /* The trunk-tuning generation and newest unresolved tune request when the frame's channels began to decode
       (nxdn_frame_left_carrier()). */
    uint64_t tune_generation;
    uint64_t tune_pending_request;

    /* What the frame's SCCH or superframe SACCH told the rest of the frame (nxdn_frame()'s own, frame-local). */
    nxdn_channel_result* result;
    /* Whether the transmission was confirmed before this frame began: the frame's spacing may confirm a check the frame
       before left pending (nxdn_confirm_note_frame_start()), which then starts the scanner's hold here. */
    int confirmed_before;
} nxdn_frame_ctx;

typedef struct {
    uint8_t lich;
    int voice;
    int facch;
    int facch2;
    int udch;
    int sacch;
    int cac;
    int idas;
    int scch;
    int facch3;
    int udch2;
    int sacch2;
    int pich_tch;
} nxdn_lich_profile;

static const nxdn_lich_profile k_nxdn_lich_profiles[] = {
    {0x01, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0}, {0x05, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0},

    {0x28, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0}, {0x29, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {0x49, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},

    {0x2E, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0x2F, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},
    {0x4E, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0x4F, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0},

    {0x32, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x33, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x52, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x53, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},

    {0x34, 1, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x35, 1, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x54, 1, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x55, 1, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},

    {0x36, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x37, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x56, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x57, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},

    {0x20, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x21, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x30, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x31, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x40, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x41, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x50, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x51, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},

    {0x38, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x39, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},

    {0x46, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0}, {0x08, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1},
    {0x48, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3}, {0x4A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0},

    {0x76, 3, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}, {0x77, 3, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},

    {0x75, 1, 2, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},

    {0x72, 2, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}, {0x73, 2, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},

    {0x70, 0, 3, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}, {0x71, 0, 3, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},

    {0x6E, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0}, {0x6F, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0},

    {0x68, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0}, {0x69, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0},

    {0x62, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}, {0x63, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},

    {0x60, 0, 3, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0}, {0x61, 0, 3, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0},
};

/*
 * A Type-C composite control channel's outbound frames (RTCH_C, TS 1-A Table 5.2-2), laid out as the RTCH ones with the
 * same low bits (TS 1-C §16.3.1): SACCH, FACCH1, FACCH2 and UDCH, never a Type-D SCCH. Every value but the idle one
 * (0x79) is also a Type-D RTCH2 outbound value above, and the LICH cannot tell the two apart: only the system the
 * session decodes can (independently, SDRTrunk's layer2/LICH.java keeps one table per system type). These are read
 * once Type-C trunking decoded here (dsd_nxdn_type_c()); until then the Type-D reading stands, as before.
 */
static const nxdn_lich_profile k_nxdn_composite_lich_profiles[] = {
    {0x77, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x75, 1, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x73, 2, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x71, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x61, 0, 3, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, {0x79, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {0x6F, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, {0x69, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static void
nxdn_mark_bad_sync(dsd_state* state) {
    state->lastsynctype = DSD_SYNC_NONE;
}

static void
nxdn_frame_ctx_init(nxdn_frame_ctx* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    DSD_MEMSET(ctx->dbuf_reliab, 255, sizeof(ctx->dbuf_reliab));
    DSD_MEMSET(ctx->sacch_reliab, 255, sizeof(ctx->sacch_reliab));
    DSD_MEMSET(ctx->facch_reliab_a, 255, sizeof(ctx->facch_reliab_a));
    DSD_MEMSET(ctx->facch_reliab_b, 255, sizeof(ctx->facch_reliab_b));
    DSD_MEMSET(ctx->cac_reliab, 255, sizeof(ctx->cac_reliab));
    DSD_MEMSET(ctx->facch2_reliab, 255, sizeof(ctx->facch2_reliab));
    DSD_MEMSET(ctx->facch3_reliab, 255, sizeof(ctx->facch3_reliab));
    DSD_MEMSET(ctx->nxdn_reliab_buffer, 255, sizeof(ctx->nxdn_reliab_buffer));
}

static void
nxdn_collect_lich(dsd_opts* opts, dsd_state* state, nxdn_frame_ctx* ctx) {
    for (int i = 0; i < 8; i++) {
        dsd_dibit_soft_t soft = {.reliability = 255};
        ctx->lich_dibits[i] = ctx->dbuf[i] = (uint8_t)getDibitSoft(opts, state, &soft);
        ctx->dbuf_reliab[i] = soft.reliability;
    }

    nxdn_descramble_with_seed(ctx->lich_dibits, 8, state->nxdn_pn95_seed);

    ctx->lich = 0;
    for (int i = 0; i < 8; i++) {
        ctx->lich |= (ctx->lich_dibits[i] >> 1) << (7 - i);
    }

    for (int i = 0; i < 8; i++) {
        ctx->lich_bits[(i * 2) + 0] = (ctx->lich_dibits[i] >> 1) & 1;
        ctx->lich_bits[(i * 2) + 1] = (ctx->lich_dibits[i] >> 0) & 1;
    }
    ctx->lich_bits_hex = (uint16_t)convert_bits_into_output(ctx->lich_bits, 16);
}

static void
nxdn_prepare_lich_parity(nxdn_frame_ctx* ctx) {
    ctx->lich_full = ctx->lich;
    ctx->lich_parity_received = ctx->lich & 1;
    ctx->lich_parity_computed =
        ((ctx->lich_full >> 7) + (ctx->lich_full >> 6) + (ctx->lich_full >> 5) + (ctx->lich_full >> 4)) & 1;
    ctx->lich = ctx->lich_full >> 1;

    if (ctx->lich == 0x08 || ctx->lich == 0x4A || ctx->lich == 0x48 || ctx->lich == 0x46) {
        ctx->lich_parity_computed =
            ((ctx->lich_full >> 7) + (ctx->lich_full >> 6) + (ctx->lich_full >> 5) + (ctx->lich_full >> 4)
             + (ctx->lich_full >> 3) + (ctx->lich_full >> 2) + (ctx->lich_full >> 1))
            & 1;
    }
}

static int
nxdn_validate_lich_parity(dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->lich_parity_received == ctx->lich_parity_computed) {
        return 1;
    }
    nxdn_mark_bad_sync(state);
    return 0;
}

static int
nxdn_validate_lich_direction(const dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if ((ctx->lich % 2) != 0 || opts->trunk_enable != 1) {
        return 1;
    }
    nxdn_mark_bad_sync(state);
    return 0;
}

static const nxdn_lich_profile*
nxdn_find_lich_profile(const nxdn_lich_profile* profiles, size_t count, uint8_t lich) {
    for (size_t i = 0; i < count; i++) {
        if (profiles[i].lich == lich) {
            return &profiles[i];
        }
    }
    return NULL;
}

static int
nxdn_apply_lich_profile(dsd_state* state, nxdn_frame_ctx* ctx) {
    const nxdn_lich_profile* profile = NULL;
    if (dsd_nxdn_type_c(state)) {
        profile = nxdn_find_lich_profile(
            k_nxdn_composite_lich_profiles,
            sizeof(k_nxdn_composite_lich_profiles) / sizeof(k_nxdn_composite_lich_profiles[0]), ctx->lich);
        ctx->composite = profile != NULL;
    }
    if (profile == NULL) {
        profile = nxdn_find_lich_profile(k_nxdn_lich_profiles,
                                         sizeof(k_nxdn_lich_profiles) / sizeof(k_nxdn_lich_profiles[0]), ctx->lich);
    }
    if (profile != NULL) {
        ctx->voice = profile->voice;
        ctx->facch = profile->facch;
        ctx->facch2 = profile->facch2;
        ctx->udch = profile->udch;
        ctx->sacch = profile->sacch;
        ctx->cac = profile->cac;
        ctx->idas = profile->idas;
        ctx->scch = profile->scch;
        ctx->facch3 = profile->facch3;
        ctx->udch2 = profile->udch2;
        ctx->sacch2 = profile->sacch2;
        ctx->pich_tch = profile->pich_tch;
        return 1;
    }

    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    nxdn_mark_bad_sync(state);
    return 0;
}

/**
 * @brief Refresh the sync clock a conventional scan and the trunking SMs read.
 *
 * Held behind nxdn_confirm_is_confirmed(): this timestamp is what stops a `-Y` scan on a
 * channel, and a sync word plus a LICH is not enough to believe there is a transmission
 * here (issue #398).
 */
static void
nxdn_mark_carrier_sync_active(dsd_state* state) {
    state->last_cc_sync_time = dsd_decode_time();
    state->last_cc_sync_time_m = dsd_decode_now_mono_s();
}

static void
nxdn_print_idas_sync_banner(const dsd_opts* opts, const dsd_state* state) {
    if (dsd_frame_sync_active_nxdn_variant(opts, state) == DSD_NXDN_VARIANT_48) {
        printFrameSync(opts, state, "IDAS D ", 0, "-");
    }
}

static void
nxdn_print_dcr_sync_banner(const dsd_opts* opts, const dsd_state* state) {
    if (dsd_frame_sync_active_nxdn_variant(opts, state) == DSD_NXDN_VARIANT_48) {
        printFrameSync(opts, state, "JPN DCR", 0, "-");
    }
}

static void
nxdn_print_normal_sync_banner(const dsd_opts* opts, const dsd_state* state) {
    if (dsd_frame_sync_active_nxdn_variant(opts, state) == DSD_NXDN_VARIANT_48) {
        printFrameSync(opts, state, "NXDN48 ", 0, "-");
    } else {
        printFrameSync(opts, state, "NXDN96 ", 0, "-");
    }
}

/* A frame sync accepted on a frame whose LICH failed prints nothing by default, since the banner waits for a LICH. The
 * payload log (-Z) shows it, so a sync taken on noise is visible even when the LICH turns it away. */
static void
nxdn_report_lich_reject(const dsd_opts* opts, const dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (opts->payload != 1) {
        return;
    }
    nxdn_print_normal_sync_banner(opts, state);
    DSD_FPRINTF(stderr, "LICH ERR %02X\n", ctx->lich_full);
}

static void
nxdn_print_sync_banner(const dsd_opts* opts, const dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->idas) {
        nxdn_print_idas_sync_banner(opts, state);
        return;
    }
    if (ctx->sacch2) {
        nxdn_print_dcr_sync_banner(opts, state);
        return;
    }
    if (ctx->voice || ctx->facch || ctx->sacch || ctx->facch2 || ctx->udch || ctx->cac) {
        nxdn_print_normal_sync_banner(opts, state);
    }
}

static void
nxdn_unpack_payload_fields(nxdn_frame_ctx* ctx) {
    for (size_t i = 0; i < 182; i++) {
        size_t idx = i * 2;
        ctx->nxdn_bit_buffer[idx] = ctx->dbuf[i] >> 1;
        ctx->nxdn_bit_buffer[idx + 1] = ctx->dbuf[i] & 1;
        ctx->nxdn_reliab_buffer[idx] = ctx->dbuf_reliab[i];
        ctx->nxdn_reliab_buffer[idx + 1] = ctx->dbuf_reliab[i];
    }

    for (int i = 0; i < 60; i++) {
        ctx->sacch_bits[i] = (uint8_t)ctx->nxdn_bit_buffer[i + 16];
        ctx->sacch_reliab[i] = ctx->nxdn_reliab_buffer[i + 16];
    }

    for (int i = 0; i < 144; i++) {
        ctx->facch_bits_a[i] = (uint8_t)ctx->nxdn_bit_buffer[i + 16 + 60];
        ctx->facch_reliab_a[i] = ctx->nxdn_reliab_buffer[i + 16 + 60];
        ctx->facch_bits_b[i] = (uint8_t)ctx->nxdn_bit_buffer[i + 16 + 60 + 144];
        ctx->facch_reliab_b[i] = ctx->nxdn_reliab_buffer[i + 16 + 60 + 144];
    }

    for (int i = 0; i < 300; i++) {
        ctx->cac_bits[i] = (uint8_t)ctx->nxdn_bit_buffer[i + 16];
        ctx->cac_reliab[i] = ctx->nxdn_reliab_buffer[i + 16];
    }

    for (int i = 0; i < 348; i++) {
        ctx->facch2_bits[i] = (uint8_t)ctx->nxdn_bit_buffer[i + 16];
        ctx->facch2_reliab[i] = ctx->nxdn_reliab_buffer[i + 16];
    }

    for (int i = 0; i < 288; i++) {
        ctx->facch3_bits[i] = (uint8_t)ctx->nxdn_bit_buffer[i + 16 + 60];
        ctx->facch3_reliab[i] = ctx->nxdn_reliab_buffer[i + 16 + 60];
    }
}

static void
nxdn_print_rf_channel_type(const nxdn_frame_ctx* ctx) {
    if (ctx->sacch2 != 0) {
        return;
    }

    if (ctx->lich_rf == 0) {
        DSD_FPRINTF(stderr, "RCCH ");
    } else if (ctx->lich_rf == 1) {
        DSD_FPRINTF(stderr, "RTCH ");
    } else if (ctx->lich_rf == 2) {
        DSD_FPRINTF(stderr, "RDCH ");
    } else if (ctx->composite) {
        DSD_FPRINTF(stderr, "RTCH_C ");
    } else {
        DSD_FPRINTF(stderr, "RTCH2 ");
    }
}

#ifdef LIMAZULUTWEAKS
static void
nxdn_apply_limazulu_voice_tweak(const dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (!ctx->voice) {
        return;
    }

    long int freq = 0;
    uint8_t hash_bits[24];
    uint16_t limazulu = 0;

    DSD_MEMSET(hash_bits, 0, sizeof(hash_bits));

    if (opts->use_rigctl == 1) {
        freq = dsd_rigctl_query_hook_get_current_freq_hz(opts);
    } else if (opts->audio_in_type == AUDIO_IN_RTL) {
        freq = (long int)opts->rtlsdr_center_freq;
    }

    for (int i = 0; i < 24; i++) {
        hash_bits[i] = ((freq << i) & 0x800000) >> 23;
    }

    if (freq) {
        limazulu = dsd_crc_ccitt16_bits(hash_bits, 24U);
    }
    limazulu = limazulu & 0xFFFF;

    DSD_FPRINTF(stderr, "%s", KYEL);
    if (freq) {
        DSD_FPRINTF(stderr, "\n Freq: %ld - Freq Hash: %d", freq, limazulu);
    }
    // An imported key is present by its import flag, a zero one included (keyring_kid_scalar_present()).
    if (keyring_load_scalar_key(state, 0, limazulu)) {
        char key_text[24];
        DSD_FPRINTF(stderr, " - Key Loaded: %s",
                    dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, state->R, 0U));
    }
    DSD_FPRINTF(stderr, "%s", KNRM);

    if (dsd_key_scalar_present(state, 0) && state->M == 1) {
        nxdn_cipher_force(state, 0x1);
    }

    state->last_cc_sync_time = dsd_decode_time() + 2;
}
#else
static void
nxdn_apply_limazulu_voice_tweak(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    UNUSED(opts);
    UNUSED(state);
    UNUSED(ctx);
}
#endif

static void
nxdn_roll_voice_lfsr(dsd_state* state, int rounds) {
    const char ambe_temp[49] = {0};
    char ambe_d[49] = {0};
    for (int i = 0; i < rounds; i++) {
        LFSRN(ambe_temp, ambe_d, state);
    }
}

static void
nxdn_apply_data_frame_lfsr(dsd_state* state) {
    if (state->nxdn_cipher_type == 0x1 && dsd_key_scalar_present(state, 0)) {
        if (state->nxdn_payload_mi == 0) {
            state->nxdn_payload_mi = state->R;
        }
        nxdn_roll_voice_lfsr(state, 4);
    }

    if (state->nxdn_cipher_type == 0x2 || state->nxdn_cipher_type == 0x3) {
        state->bit_counterL += 49L * 4;
    }
}

static void
nxdn_apply_pre_voice_facch1_lfsr(dsd_state* state) {
    if (state->nxdn_cipher_type == 0x1 && dsd_key_scalar_present(state, 0)) {
        if (state->nxdn_payload_mi == 0) {
            state->nxdn_payload_mi = state->R;
        }
        nxdn_roll_voice_lfsr(state, 2);
    }
    if (state->nxdn_cipher_type == 0x2 || state->nxdn_cipher_type == 0x3) {
        state->bit_counterL += 49L * 2;
    }
}

/* Whether the frame is a frame of the voice superframe that can carry its own position: a Type-D RTCH2 frame whose
   LICH names the superframe structure (USC 1x, NXDN TS 1-E Table 5.2-1) with its SCCH, or a Type-C frame with a
   superframe SACCH. Its cipher slots are stepped after its signalling, which may place the register
   (nxdn_step_cipher_after_signalling()). Every other frame steps them before its signalling, exactly as it always did:
   a DCR SB0/PICH header, a non-superframe SACCH's, a control channel's, and a standalone Type-D frame of the
   non-superframe structure (USC 0x: a single SCCH, whose structure 00 means "single", not 4/4, Table 6.3-1, places
   nothing). So a reset or an IV its signalling delivers (the DCR SACCH2's first block, a header's VCALL_IV) stands for
   the voice after it, and a VCALL it carries that selects Scramble takes none of its slots. */
static int
nxdn_frame_steps_after_signalling(const dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->scch) {
        return (ctx->lich & 0x10U) != 0U;
    }
    return ctx->sacch && state->nxdn_sacch_non_superframe == 0;
}

/* The cipher slots the frame's leading non-voice part takes (NXDN TS 1-D Fig 5.4-2, TS 1-F Fig 16.4-1: the PN sequence
   and the DES/AES output keep their places in a stolen slot): a data frame's four, a leading FACCH1's two. */
static void
nxdn_step_stolen_slots(dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->voice == 0) {
        nxdn_apply_data_frame_lfsr(state);
    } else if (ctx->facch == 1) {
        nxdn_apply_pre_voice_facch1_lfsr(state);
    }
}

static void
nxdn_print_voice_or_data_and_sync_lfsr(dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->voice == 0) {
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, "Data  ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    } else if (!ctx->facch) {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, "Voice ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    } else {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, "V%d+F%d ", 3 - ctx->facch, ctx->facch);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (ctx->voice && ctx->facch == 1 && state->M == 1 && dsd_key_scalar_present(state, 0)) {
        nxdn_cipher_force(state, 0x1);
    }
    if (!nxdn_frame_steps_after_signalling(state, ctx)) {
        nxdn_step_stolen_slots(state, ctx);
    }
}

/* The scrambler position and the cipher slots of a frame that can carry its own position
   (nxdn_frame_steps_after_signalling()), once its signalling decoded on this carrier (never after it retuned the
   receiver, never on another channel's frame): a forced scrambler (-4) is selected first (a fully stolen frame with no
   position included); a position (nxdn_channel_result::seed_part: a checked, valid SCCH's, or a
   checked superframe SACCH segment's, whose own placement came before the VCALL it completed chose Scramble or its key)
   places the register at the frame's start with the cipher and key the frame chose (nxdn_prepare_sacch_payload_seed());
   and the stolen slots step it after that, so the voice after a leading FACCH1 starts two slots in. */
static void
nxdn_step_cipher_after_signalling(dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (!nxdn_frame_steps_after_signalling(state, ctx)) {
        return;
    }
    /* A forced scrambler (-4) covers every frame of the voice superframe, a fully stolen one with no position included:
       its slots still run the register on. Only the absolute placement needs a checked position. */
    const uint8_t part = ctx->result->seed_part;
    if (state->M == 1 && dsd_key_scalar_present(state, 0)) {
        nxdn_cipher_force(state, 0x1);
    }
    if (part != 0U && state->nxdn_cipher_type == 0x1 && dsd_key_scalar_present(state, 0)) {
        nxdn_prepare_sacch_payload_seed(state, (int)part - 1);
    }
    nxdn_step_stolen_slots(state, ctx);
}

static void
nxdn_update_sacch_mode(dsd_state* state, uint8_t lich) {
    if (lich == 0x20 || lich == 0x21 || lich == 0x61 || lich == 0x40 || lich == 0x41) {
        state->nxdn_sacch_non_superframe = 1;
    } else {
        state->nxdn_sacch_non_superframe = 0;
    }
}

/* The channel the frame's LICH names, which the element parsers read while its channels decode: a CAC is the Type-C
 * control channel, an RTCH_C profile a composite control channel, an RTCH2 profile (the IDAS column) a Type-D traffic
 * channel, anything else a traffic or data channel. */
static uint8_t
nxdn_frame_channel_of(const nxdn_frame_ctx* ctx) {
    if (ctx->cac) {
        return (uint8_t)NXDN_FRAME_CHANNEL_CONTROL;
    }
    if (ctx->composite) {
        return (uint8_t)NXDN_FRAME_CHANNEL_COMPOSITE;
    }
    if (ctx->idas) {
        return (uint8_t)NXDN_FRAME_CHANNEL_TYPE_D;
    }
    return (uint8_t)NXDN_FRAME_CHANNEL_TRAFFIC;
}

/* Note where the receiver is before the frame's channels decode: the completed trunk-tuning generation and the newest
   unresolved tune request (a PENDING retune counts at once), as P25 Phase 2's retune token does. */
static void
nxdn_frame_note_tuning(nxdn_frame_ctx* ctx) {
    ctx->tune_generation = dsd_trunk_tuning_generation();
    ctx->tune_pending_request = dsd_trunk_tuning_pending_request();
}

/* Whether a channel of this frame retuned the receiver (a grant or busy repeater message followed, a Type-D
   termination's return to the control channel): what is left of the frame was read on the carrier left, so none of it
   may name, reopen or play a call where the receiver went. */
static int
nxdn_frame_left_carrier(const nxdn_frame_ctx* ctx) {
    return dsd_trunk_tuning_generation() != ctx->tune_generation
           || dsd_trunk_tuning_pending_request() != ctx->tune_pending_request;
}

static void
nxdn_decode_scch_channels(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->scch) {
        *ctx->result =
            nxdn_deperm_scch_soft(opts, state, (uint8_t*)ctx->sacch_bits, (uint8_t*)ctx->sacch_reliab, ctx->direction);
        if (nxdn_frame_left_carrier(ctx)) {
            return;
        }
    }
    if (ctx->udch2) {
        nxdn_deperm_facch3_udch2_soft(opts, state, (uint8_t*)ctx->facch3_bits, (uint8_t*)ctx->facch3_reliab, 0);
    }
    if (ctx->facch3) {
        nxdn_deperm_facch3_udch2_soft(opts, state, (uint8_t*)ctx->facch3_bits, (uint8_t*)ctx->facch3_reliab, 1);
    }
}

static void
nxdn_decode_sacch_cac_channels(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->sacch) {
        *ctx->result = nxdn_deperm_sacch_soft(opts, state, (uint8_t*)ctx->sacch_bits, (uint8_t*)ctx->sacch_reliab);
    }
    if (ctx->cac) {
        nxdn_deperm_cac_soft(opts, state, (uint8_t*)ctx->cac_bits, (uint8_t*)ctx->cac_reliab);
    }
}

static void
nxdn_decode_facch2_channels(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->udch) {
        nxdn_deperm_facch2_udch_soft(opts, state, (uint8_t*)ctx->facch2_bits, (uint8_t*)ctx->facch2_reliab, 0);
    }
    if (ctx->facch2) {
        nxdn_deperm_facch2_udch_soft(opts, state, (uint8_t*)ctx->facch2_bits, (uint8_t*)ctx->facch2_reliab, 1);
    }
}

static void
// cppcheck-suppress constParameterCallback -- one step of the nxdn_channel_step table, which shares one signature.
nxdn_decode_sacch2_channels(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->sacch2) {
        nxdn_deperm_sacch2_soft(opts, state, (uint8_t*)ctx->sacch_bits, (uint8_t*)ctx->sacch_reliab);
    }
    if ((ctx->pich_tch & 1) && !nxdn_frame_left_carrier(ctx)) {
        nxdn_deperm_pich_tch_soft(opts, state, (uint8_t*)ctx->facch_bits_a, (uint8_t*)ctx->facch_reliab_a, ctx->lich);
    }
    if ((ctx->pich_tch & 2) && !nxdn_frame_left_carrier(ctx)) {
        nxdn_deperm_pich_tch_soft(opts, state, (uint8_t*)ctx->facch_bits_b, (uint8_t*)ctx->facch_reliab_b, ctx->lich);
    }
}

static void
nxdn_decode_facch_channels(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->facch & 1) {
        nxdn_deperm_facch_soft(opts, state, (uint8_t*)ctx->facch_bits_a, (uint8_t*)ctx->facch_reliab_a,
                               ctx->facch == 3 ? 1U : 0U);
    }
    if ((ctx->facch & 2) && !nxdn_frame_left_carrier(ctx)) {
        nxdn_deperm_facch_soft(opts, state, (uint8_t*)ctx->facch_bits_b, (uint8_t*)ctx->facch_reliab_b,
                               ctx->facch == 3 ? 2U : 0U);
    }
}

/* Decode the frame's signalling channels in their order. Returns 1 when one of them retuned the receiver
   (nxdn_frame_left_carrier()): no channel after it is decoded (the steps also stop between their own channels). */
static int
nxdn_decode_control_channels(dsd_opts* opts, dsd_state* state, nxdn_frame_ctx* ctx) {
    typedef void (*nxdn_channel_step)(dsd_opts*, dsd_state*, const nxdn_frame_ctx*);
    static const nxdn_channel_step k_steps[] = {
        nxdn_decode_scch_channels,   nxdn_decode_sacch_cac_channels, nxdn_decode_facch2_channels,
        nxdn_decode_sacch2_channels, nxdn_decode_facch_channels,
    };
    nxdn_frame_note_tuning(ctx);
    for (size_t i = 0; i < sizeof(k_steps) / sizeof(k_steps[0]); i++) {
        k_steps[i](opts, state, ctx);
        if (nxdn_frame_left_carrier(ctx)) {
            return 1;
        }
    }
    return 0;
}

/* Decode the frame's signalling channels (nxdn_decode_control_channels()). The "Type-D" layout an SCCH names
   (nxdn_scch_prepare_type_d()) is read by the frame's other channels whatever the frame proves, but it outlives the
   frame only on a confirmed transmission: an SCCH's 7-bit CRC passes noise one time in 128, and a category one left
   behind on another protocol's carrier would read the next Type-C VCALL with Type-D's 11-bit IDs. */
static int
nxdn_decode_frame_channels(dsd_opts* opts, dsd_state* state, nxdn_frame_ctx* ctx) {
    char category[sizeof(state->nxdn_location_category)];
    DSD_MEMCPY(category, state->nxdn_location_category, sizeof(category));
    const uint8_t type_c_before = state->nxdn_site.type_c;
    const int left_carrier = nxdn_decode_control_channels(opts, state, ctx);
    /* An SCCH whose content TS 1-E does not define (nxdn_scch_content_valid()) names the layout for the frame's other
       channels only: it never outlives the frame, whatever confirmed the transmission. The rollback undoes only the
       SCCH's provisional layout: a Type-C site a later channel of the frame verified (dsd_nxdn_note_type_c()) never
       reads "Type-D" again. */
    if (!nxdn_confirm_is_confirmed(state) || ctx->result->scch_invalid != 0U) {
        DSD_MEMCPY(state->nxdn_location_category, category, sizeof(category));
        if (state->nxdn_site.type_c != type_c_before && strcmp(category, "Type-D") == 0) {
            DSD_SNPRINTF(state->nxdn_location_category, sizeof(state->nxdn_location_category), "%s", " ");
        }
    }
    return left_carrier;
}

/* Voice withheld before the transmission is confirmed still takes its places in the PN sequence (NXDN TS 1-F Fig
   16.4-1): the Scramble register runs on across the withheld words as the vocoder would have run it
   (mbe_apply_nxdn_cipher1()), so the next frame's voice starts where it belongs. The audio gate is unchanged: nothing
   plays. */
static void
nxdn_step_withheld_voice(dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (!ctx->voice || !dsd_key_scalar_present(state, 0) || (state->nxdn_cipher_type != 0x1 && state->M != 1)) {
        return;
    }
    if (state->nxdn_payload_mi == 0) {
        state->nxdn_payload_mi = state->R;
    }
    nxdn_roll_voice_lfsr(state, ctx->voice == 3 ? 4 : 2);
}

static void
nxdn_process_voice_and_mbe(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    /* The LICH alone decides whether a frame claims to carry voice, and noise clears the
     * LICH often enough that acting on that claim is what put fictional speech on a dead
     * channel. Synthesize only once the frame body has passed a CRC; a real call confirms
     * on its first FACCH, or within two frames of SACCH (issue #398). */
    if (ctx->voice && nxdn_confirm_is_confirmed(state)) {
        if (dsd_receive_allows(opts, state, DSD_SYNC_NXDN_POS, 0)) {
            state->last_vc_sync_time = dsd_decode_time();
            state->last_vc_sync_time_m = dsd_decode_now_mono_s();
        }
        if (state->M == 1 && dsd_key_scalar_present(state, 0)) {
            nxdn_cipher_force(state, 0x1);
        }
        nxdn_voice(opts, state, ctx->voice, (uint8_t*)ctx->dbuf, (uint8_t*)ctx->dbuf_reliab);
        return;
    }
    nxdn_step_withheld_voice(state, ctx);

    if (opts->mbe_out_f == NULL) {
        return;
    }
    const dsd_nxdn_variant variant = dsd_frame_sync_active_nxdn_variant(opts, state);
    /* NXDN's own capture only (issue #661), named as NXDN: a failed CAC run can have reset state->synctype by now. */
    if (variant == DSD_NXDN_VARIANT_96 && (dsd_decode_time() - state->last_vc_sync_time) > 1) {
        dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_NXDN_POS);
    }
    if (variant == DSD_NXDN_VARIANT_48) {
        dsd_mbe_out_close_protocol(opts, state, DSD_MBE_OUT_SLOT1, DSD_SYNC_NXDN_POS);
    }
}

static void
nxdn_handle_post_voice_facch2_lfsr(dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (!(ctx->voice && ctx->facch == 2)) {
        return;
    }

    if (state->nxdn_cipher_type == 0x1 && dsd_key_scalar_present(state, 0)) {
        nxdn_roll_voice_lfsr(state, 2);
    }

    if (state->nxdn_cipher_type == 0x2 || state->nxdn_cipher_type == 0x3) {
        state->bit_counterL += 49L * 2;
    }
}

static void
nxdn_finalize_sync_reject(dsd_state* state) {
    if (state->lastsynctype == DSD_SYNC_NONE) {
        state->carrier = 0;
        state->synctype = DSD_SYNC_NONE;
    }
}

/* A frame whose channels decoded on the carrier it was read on: a confirmed transmission keeps the carrier active, and
   its voice plays. Another channel's control traffic on the followed traffic channel does neither: the call followed is
   not on it, and its voice is another call's. */
static void
nxdn_finish_decoded_frame(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->foreign) {
        DSD_FPRINTF(stderr, "\n");
        return;
    }
    nxdn_step_cipher_after_signalling(state, ctx);
    /* A data frame whose only checked content was an SCCH TS 1-E does not define (nxdn_channel_result::scch_invalid)
       shows nothing more about a carrier already confirmed: it neither stamps it active nor extends the scanner's hold.
       A transmission this frame's spacing confirmed (a check the frame before left pending) starts its hold here. */
    const int diagnostic_only_frame = ctx->confirmed_before && !ctx->voice && ctx->result->scch_invalid != 0U
                                      && state->nxdn_confirm_frame_evidence == 0U;
    /* On a conventional -Y scan the hold needs this frame's own content proven, as the quiet check hears it (issue #834):
       a transmission confirmed once lets every later frame whose LICH parity passes through, receiver noise after the
       call included, and a hold refreshed by those, or by a lone short CRC one of them passes by chance, would keep the
       row. Trunked receivers and a plain conventional one keep the transmission's. */
    const int y_conventional = opts->scanner_mode == 1 && opts->trunk_enable != 1;
    if (nxdn_confirm_is_confirmed(state) && !diagnostic_only_frame
        && (!y_conventional || nxdn_confirm_frame_content_proven(state))) {
        nxdn_mark_carrier_sync_active(state);
        if (opts->scanner_mode == 1) {
            /* Hold the scanner a little past this frame so the gap to the next one does
             * not read as loss of signal. */
            state->last_cc_sync_time = dsd_decode_time() + 2;
        }
    }

    nxdn_process_voice_and_mbe(opts, state, ctx);
    nxdn_handle_post_voice_facch2_lfsr(state, ctx);

    if ((opts->payload == 1 && !ctx->voice) || opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }
}

/* The FSW the hunt just accepted places the symbol grid for the frame behind it, as in ysf.c: the hunt's crossing loop
   leaves the grid where the word's crossings put it, which its two inner symbols pull off the symbol boundaries. A
   frame read whole armed a recording at its end, so the recording starts on the FSW's first sample; the word is named
   from its second symbol, which leaves the measurement its sample of margin before the first centre. */
static void
nxdn_note_fsw_anchor(const dsd_state* state) {
    const char* fsw = state->synctype == DSD_SYNC_NXDN_NEG ? INV_NXDN_FSW_DIBITS : NXDN_FSW_DIBITS;
    dsd_symbol_note_sync_anchor(state, fsw + 1, 9);
}

/* Open the frame's evidence accounting for a frame that began at @p frame_symbol. NXDN96's accept needs a hit one
   frame before; NXDN48's takes two at any distance, so an NXDN48 frame runs on only from a frame read one frame
   before it, and its strong checks hold at once only when its sync word followed another one frame before it
   (nxdn_confirm_note_frame_start()). */
static void
nxdn_frame_begin_evidence(const dsd_opts* opts, dsd_state* state, uint32_t frame_symbol) {
    const int nxdn48 = dsd_frame_sync_active_nxdn_variant(opts, state) == DSD_NXDN_VARIANT_48;
    nxdn_confirm_note_frame_start(state, frame_symbol, !nxdn48, nxdn48 && state->nxdn48_accept_corroborated);
    nxdn_confirm_begin_frame(state);
}

/* A frame whose channel retuned the receiver: its remaining channels and voice were read on the carrier left and are
   dropped, and nothing of it stamps the carrier reached as active or steps its cipher. */
static void
nxdn_frame_left(void) {
    DSD_FPRINTF(stderr, "\n");
}

/* What the frame does before its channels decode: the SACCH mode its LICH names, the voice/data banner and the cipher
   slots of a frame that carries no position (nxdn_print_voice_or_data_and_sync_lfsr()), the channel the element
   parsers read, and whether it is another channel's control traffic. */
static void
nxdn_frame_before_channels(const dsd_opts* opts, dsd_state* state, nxdn_frame_ctx* ctx) {
    nxdn_update_sacch_mode(state, ctx->lich);
    nxdn_print_voice_or_data_and_sync_lfsr(state, ctx);
    state->nxdn_frame_channel = nxdn_frame_channel_of(ctx);
    /* Decided before any channel of the frame decodes: none of its checks may confirm the transmission, and none of its
       messages may name, follow or play anything here. */
    ctx->foreign = (ctx->cac || ctx->composite) && dsd_nxdn_control_heard_is_foreign(opts, state);
    state->nxdn_frame_foreign = (uint8_t)ctx->foreign;
}

int
nxdn_frame(dsd_opts* opts, dsd_state* state) {
    nxdn_frame_ctx ctx;
    /* Declared out here so every goto END path below carries the "proved nothing" answer:
     * those bail before the frame body is read, so there is no CRC of this frame's own to
     * report however much the transmission has proved before now (#445). */
    int frame_proved = 0;
    int frame_read = 0; // the body reached the confirmation gate

    nxdn_channel_result result = {0U, 0U};

    nxdn_note_fsw_anchor(state);
    nxdn_frame_ctx_init(&ctx);
    ctx.result = &result;
    const uint32_t carrier_seq = state->carrier_seq;
    const uint32_t frame_symbol = (uint32_t)state->symbolcnt;
    nxdn_collect_lich(opts, state, &ctx);

    nxdn_prepare_lich_parity(&ctx);
    if (!nxdn_validate_lich_parity(state, &ctx)) {
        dsd_nxdn96_sync_note_lich(&state->nxdn96_sync, (uint32_t)state->symbolcnt, DSD_NXDN96_LICH_PARITY_ERROR);
        nxdn_report_lich_reject(opts, state, &ctx);
        goto END;
    }
    if (!nxdn_validate_lich_direction(opts, state, &ctx) || !nxdn_apply_lich_profile(state, &ctx)) {
        dsd_nxdn96_sync_note_lich(&state->nxdn96_sync, (uint32_t)state->symbolcnt, DSD_NXDN96_LICH_WRONG);
        nxdn_report_lich_reject(opts, state, &ctx);
        goto END;
    }
    dsd_nxdn96_sync_note_lich(&state->nxdn96_sync, (uint32_t)state->symbolcnt, DSD_NXDN96_LICH_GOOD);

    state->carrier = 1;
    nxdn_print_sync_banner(opts, state, &ctx);

    for (int i = 0; i < 174; i++) {
        dsd_dibit_soft_t soft = {.reliability = 255};
        ctx.dbuf[i + 8] = (uint8_t)getDibitSoft(opts, state, &soft);
        ctx.dbuf_reliab[i + 8] = soft.reliability;
    }
    /* The frame was read whole: the next one's FSW starts at the next symbol (nxdn_note_fsw_anchor()). */
    dsd_symbol_arm_hunt_sync_anchor(state);
    if (dsd_state_frame_read_lost(state, carrier_seq)) {
        /* The carrier boundary moved the carrier count while the frame was read (issue #575). Everything after the
           LICH decodes only now, so a SACCH segment read whole before the move, possibly the one completing a
           superframe, would decode after the boundary ended that carrier's calls: the frame is dropped, with the
           SACCH segments gathered, as on a sync loss. So is a frame an input interruption cut, whose rest is
           erasures the reader stood in. */
        DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
        DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
        nxdn_mark_bad_sync(state);
        dsd_nxdn96_sync_clear(&state->nxdn96_sync);
        DSD_FPRINTF(stderr, "\n");
        goto END;
    }

    nxdn_descramble_with_seed(ctx.dbuf, 182, state->nxdn_pn95_seed);
    nxdn_unpack_payload_fields(&ctx);

    ctx.lich_rf = (ctx.lich >> 5) & 0x3;
    ctx.direction = (ctx.lich % 2 == 0) ? 0 : 1;

    nxdn_print_rf_channel_type(&ctx);
    nxdn_apply_limazulu_voice_tweak(opts, state, &ctx);

    ctx.confirmed_before = nxdn_confirm_is_confirmed(state);
    nxdn_frame_begin_evidence(opts, state, frame_symbol);
    nxdn_frame_before_channels(opts, state, &ctx);
    const int left_carrier = nxdn_decode_frame_channels(opts, state, &ctx);
    state->nxdn_frame_channel = (uint8_t)NXDN_FRAME_CHANNEL_NONE;
    state->nxdn_frame_foreign = 0U;
    nxdn_confirm_end_frame(opts, state);
    frame_read = 1;
    frame_proved = nxdn_confirm_frame_proved(state);
    if (left_carrier) {
        /* A channel of the frame retuned the receiver: the rest was read on the carrier left (nxdn_frame_left()). */
        nxdn_frame_left();
        goto END;
    }
    nxdn_finish_decoded_frame(opts, state, &ctx);

END:
    if (!frame_read) {
        nxdn_confirm_frame_rejected(state);
        state->nxdn_frame_channel = (uint8_t)NXDN_FRAME_CHANNEL_NONE;
        state->nxdn_frame_foreign = 0U;
    }
    nxdn_finalize_sync_reject(state);
    /* The sticky flag, not this frame's evidence: a confirmed transmission whose current
     * frame happens to carry no CRC still decoded, and reporting it unproductive would let
     * the SPS hunt rotate off a live call.
     *
     * This frame's own evidence is reported alongside it as the 2, because the caller needs
     * both answers: the sticky one says the call is live, and the per-frame one is what proves
     * the profile the frame was read on -- a standing that the syncs between transmissions must
     * not inherit, however confirmed the last call was (#445). */
    return frame_proved ? 2 : nxdn_confirm_is_confirmed(state);
}
