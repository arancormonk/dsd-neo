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

#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/nxdn/nxdn.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/protocol/nxdn/nxdn_lfsr.h>
#include <dsd-neo/protocol/nxdn/nxdn_voice.h>
#include <dsd-neo/runtime/colors.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifdef LIMAZULUTWEAKS
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include "dsd-neo/core/secret_redaction.h"
#endif

// #define NXDN_DEBUG_LICH         //print LICH debug info on err on payload == 1
// #define NXDN_LICH_OFFBITS_CHECK //optional strict filter for encoded LICH "off bits"
// NOTE:
// The offbits check was observed to reject otherwise-decodable NXDN frames on
// marginal signals (notably NXDN96 trunking). Keep it disabled by default and
// rely on parity/LICH-type validation below for robust sync handling.

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
        uint8_t rel = 255;
        ctx->lich_dibits[i] = ctx->dbuf[i] = (uint8_t)getDibitWithReliability(opts, state, &rel);
        ctx->dbuf_reliab[i] = rel;
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
    ctx->lich_bits_hex = (uint16_t)ConvertBitIntoBytes(ctx->lich_bits, 16);
}

#ifdef NXDN_LICH_OFFBITS_CHECK
static int
nxdn_validate_lich_offbits(const dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    uint8_t lich_off_hex = 0;
    for (int i = 0; i < 8; i++) {
        lich_off_hex += ctx->lich_bits[(i * 2) + 1];
    }
    if (lich_off_hex < 7) {
#ifdef NXDN_DEBUG_LICH
        if (opts->payload == 1) {
            DSD_FPRINTF(stderr, "  Lich Off Bit Fill Error: %d / 8; \n", lich_off_hex);
        }
#endif
        nxdn_mark_bad_sync(state);
        return 0;
    }
    return 1;
}
#endif

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
nxdn_validate_lich_parity(const dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->lich_parity_received == ctx->lich_parity_computed) {
        return 1;
    }
#ifdef NXDN_DEBUG_LICH
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "  Lich Parity Error %02X / %04X\n", ctx->lich_full, ctx->lich_bits_hex);
    }
#else
    UNUSED(opts);
#endif
    nxdn_mark_bad_sync(state);
    return 0;
}

static int
nxdn_validate_lich_direction(const dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if ((ctx->lich % 2) != 0 || opts->p25_trunk != 1) {
        return 1;
    }
#ifdef NXDN_DEBUG_LICH
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "  Simplex/Inbound NXDN lich on trunking system - type 0x%02X\n", ctx->lich);
    }
#endif
    nxdn_mark_bad_sync(state);
    return 0;
}

static int
nxdn_apply_lich_profile(const dsd_opts* opts, dsd_state* state, nxdn_frame_ctx* ctx) {
    for (size_t i = 0; i < (sizeof(k_nxdn_lich_profiles) / sizeof(k_nxdn_lich_profiles[0])); i++) {
        const nxdn_lich_profile* profile = &k_nxdn_lich_profiles[i];
        if (profile->lich != ctx->lich) {
            continue;
        }

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

#ifdef NXDN_DEBUG_LICH
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "  false sync or unsupported NXDN lich type L: %02X / LH: %04X\n", ctx->lich,
                    ctx->lich_bits_hex);
    }
#else
    UNUSED(opts);
#endif
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    nxdn_mark_bad_sync(state);
    return 0;
}

static void
nxdn_mark_carrier_sync_active(dsd_state* state) {
    state->carrier = 1;
    state->last_cc_sync_time = time(NULL);
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
}

static void
nxdn_print_lich_debug_payload(const dsd_opts* opts, const nxdn_frame_ctx* ctx) {
#ifdef NXDN_DEBUG_LICH
    if (opts->payload == 1) {
        DSD_FPRINTF(stderr, "L: %02X / LH: %04X; ", ctx->lich, ctx->lich_bits_hex);
    }
#else
    UNUSED(opts);
    UNUSED(ctx);
#endif
}

static void
nxdn_print_idas_sync_banner(const dsd_opts* opts, const dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (opts->frame_nxdn48 == 1) {
        printFrameSync(opts, state, "IDAS D ", 0, "-");
    }
    nxdn_print_lich_debug_payload(opts, ctx);
}

static void
nxdn_print_dcr_sync_banner(const dsd_opts* opts, const dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (opts->frame_nxdn48 == 1) {
        printFrameSync(opts, state, "JPN DCR", 0, "-");
    }
    nxdn_print_lich_debug_payload(opts, ctx);
}

static void
nxdn_print_normal_sync_banner(const dsd_opts* opts, const dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (opts->frame_nxdn48 == 1) {
        printFrameSync(opts, state, "NXDN48 ", 0, "-");
    } else {
        printFrameSync(opts, state, "NXDN96 ", 0, "-");
    }
    nxdn_print_lich_debug_payload(opts, ctx);
}

static void
nxdn_print_sync_banner(const dsd_opts* opts, const dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->idas) {
        nxdn_print_idas_sync_banner(opts, state, ctx);
        return;
    }
    if (ctx->sacch2) {
        nxdn_print_dcr_sync_banner(opts, state, ctx);
        return;
    }
    if (ctx->voice || ctx->facch || ctx->sacch || ctx->facch2 || ctx->udch || ctx->cac) {
        nxdn_print_normal_sync_banner(opts, state, ctx);
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
nxdn_collect_payload_and_unpack(dsd_opts* opts, dsd_state* state, nxdn_frame_ctx* ctx) {
    for (int i = 0; i < 174; i++) {
        uint8_t rel = 255;
        ctx->dbuf[i + 8] = (uint8_t)getDibitWithReliability(opts, state, &rel);
        ctx->dbuf_reliab[i + 8] = rel;
    }

    nxdn_descramble_with_seed(ctx->dbuf, 182, state->nxdn_pn95_seed);
    nxdn_unpack_payload_fields(ctx);
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
    } else if (ctx->lich < 0x60) {
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
        limazulu = ComputeCrcCCITT16d(hash_bits, 24);
    }
    limazulu = limazulu & 0xFFFF;

    DSD_FPRINTF(stderr, "%s", KYEL);
    if (freq) {
        DSD_FPRINTF(stderr, "\n Freq: %ld - Freq Hash: %d", freq, limazulu);
    }
    if (state->rkey_array[limazulu] != 0) {
        char key_text[24];
        DSD_FPRINTF(
            stderr, " - Key Loaded: %s",
            dsd_secret_format_decimal(key_text, sizeof key_text, opts->show_keys, state->rkey_array[limazulu], 0U));
    }
    DSD_FPRINTF(stderr, "%s", KNRM);

    if (state->rkey_array[limazulu] != 0) {
        state->R = state->rkey_array[limazulu];
    }

    if (state->R != 0 && state->M == 1) {
        state->nxdn_cipher_type = 0x1;
    }

    state->last_cc_sync_time = time(NULL) + 2;
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
    if (state->nxdn_cipher_type == 0x1 && state->R != 0) {
        if (state->payload_miN == 0) {
            state->payload_miN = state->R;
        }
        nxdn_roll_voice_lfsr(state, 4);
    }

    if (state->nxdn_cipher_type == 0x2 || state->nxdn_cipher_type == 0x3) {
        state->bit_counterL += 49L * 4;
    }
}

static void
nxdn_apply_pre_voice_facch1_lfsr(dsd_state* state) {
    if (state->M == 1 && state->R != 0) {
        state->nxdn_cipher_type = 0x1;
    }
    if (state->nxdn_cipher_type == 0x1 && state->R != 0) {
        if (state->payload_miN == 0) {
            state->payload_miN = state->R;
        }
        nxdn_roll_voice_lfsr(state, 2);
    }
    if (state->nxdn_cipher_type == 0x2 || state->nxdn_cipher_type == 0x3) {
        state->bit_counterL += 49L * 2;
    }
}

static void
nxdn_print_voice_or_data_and_sync_lfsr(dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->voice == 0) {
        DSD_FPRINTF(stderr, "%s", KCYN);
        DSD_FPRINTF(stderr, "Data  ");
        DSD_FPRINTF(stderr, "%s", KNRM);
        nxdn_apply_data_frame_lfsr(state);
    } else if (!ctx->facch) {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, "Voice ");
        DSD_FPRINTF(stderr, "%s", KNRM);
    } else {
        DSD_FPRINTF(stderr, "%s", KGRN);
        DSD_FPRINTF(stderr, "V%d+F%d ", 3 - ctx->facch, ctx->facch);
        DSD_FPRINTF(stderr, "%s", KNRM);
    }

    if (ctx->voice && ctx->facch == 1) {
        nxdn_apply_pre_voice_facch1_lfsr(state);
    }
}

static void
nxdn_update_sacch_mode(dsd_state* state, uint8_t lich) {
    if (lich == 0x20 || lich == 0x21 || lich == 0x61 || lich == 0x40 || lich == 0x41) {
        state->nxdn_sacch_non_superframe = 1;
    } else {
        state->nxdn_sacch_non_superframe = 0;
    }
}

static void
nxdn_decode_control_channels(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->scch) {
        nxdn_deperm_scch_soft(opts, state, (uint8_t*)ctx->sacch_bits, (uint8_t*)ctx->sacch_reliab, ctx->direction);
    }

    if (ctx->udch2) {
        nxdn_deperm_facch3_udch2_soft(opts, state, (uint8_t*)ctx->facch3_bits, (uint8_t*)ctx->facch3_reliab, 0);
    }
    if (ctx->facch3) {
        nxdn_deperm_facch3_udch2_soft(opts, state, (uint8_t*)ctx->facch3_bits, (uint8_t*)ctx->facch3_reliab, 1);
    }

    if (ctx->sacch) {
        nxdn_deperm_sacch_soft(opts, state, (uint8_t*)ctx->sacch_bits, (uint8_t*)ctx->sacch_reliab);
    }
    if (ctx->cac) {
        nxdn_deperm_cac_soft(opts, state, (uint8_t*)ctx->cac_bits, (uint8_t*)ctx->cac_reliab);
    }

    if (ctx->udch) {
        nxdn_deperm_facch2_udch_soft(opts, state, (uint8_t*)ctx->facch2_bits, (uint8_t*)ctx->facch2_reliab, 0);
    }
    if (ctx->facch2) {
        nxdn_deperm_facch2_udch_soft(opts, state, (uint8_t*)ctx->facch2_bits, (uint8_t*)ctx->facch2_reliab, 1);
    }

    if (ctx->sacch2) {
        nxdn_deperm_sacch2_soft(opts, state, (uint8_t*)ctx->sacch_bits, (uint8_t*)ctx->sacch_reliab);
    }
    if (ctx->pich_tch & 1) {
        nxdn_deperm_pich_tch_soft(opts, state, (uint8_t*)ctx->facch_bits_a, (uint8_t*)ctx->facch_reliab_a, ctx->lich);
    }
    if (ctx->pich_tch & 2) {
        nxdn_deperm_pich_tch_soft(opts, state, (uint8_t*)ctx->facch_bits_b, (uint8_t*)ctx->facch_reliab_b, ctx->lich);
    }

    if (ctx->facch & 1) {
        nxdn_deperm_facch_soft(opts, state, (uint8_t*)ctx->facch_bits_a, (uint8_t*)ctx->facch_reliab_a,
                               ctx->facch == 3 ? 1U : 0U);
    }
    if (ctx->facch & 2) {
        nxdn_deperm_facch_soft(opts, state, (uint8_t*)ctx->facch_bits_b, (uint8_t*)ctx->facch_reliab_b,
                               ctx->facch == 3 ? 2U : 0U);
    }
}

static void
nxdn_process_voice_and_mbe(dsd_opts* opts, dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (ctx->voice) {
        if ((opts->mbe_out_dir[0] != 0) && (opts->mbe_out_f == NULL)) {
            openMbeOutFile(opts, state);
        }
        state->last_vc_sync_time = time(NULL);
        state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
        if (state->M == 1 && state->R != 0) {
            state->nxdn_cipher_type = 0x1;
        }
        nxdn_voice(opts, state, ctx->voice, (uint8_t*)ctx->dbuf, (uint8_t*)ctx->dbuf_reliab);
        return;
    }

    if (opts->mbe_out_f == NULL) {
        return;
    }
    if (opts->frame_nxdn96 == 1 && (time(NULL) - state->last_vc_sync_time) > 1) {
        closeMbeOutFile(opts, state);
    }
    if (opts->frame_nxdn48 == 1) {
        closeMbeOutFile(opts, state);
    }
}

static void
nxdn_handle_post_voice_facch2_lfsr(dsd_state* state, const nxdn_frame_ctx* ctx) {
    if (!(ctx->voice && ctx->facch == 2)) {
        return;
    }

    if (state->nxdn_cipher_type == 0x1 && state->R != 0) {
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

void
nxdn_frame(dsd_opts* opts, dsd_state* state) {
    nxdn_frame_ctx ctx;

    nxdn_frame_ctx_init(&ctx);
    nxdn_collect_lich(opts, state, &ctx);

#ifdef NXDN_LICH_OFFBITS_CHECK
    if (!nxdn_validate_lich_offbits(opts, state, &ctx)) {
        goto END;
    }
#endif

    nxdn_prepare_lich_parity(&ctx);
    if (!nxdn_validate_lich_parity(opts, state, &ctx)) {
        goto END;
    }
    if (!nxdn_validate_lich_direction(opts, state, &ctx)) {
        goto END;
    }
    if (!nxdn_apply_lich_profile(opts, state, &ctx)) {
        goto END;
    }

    nxdn_mark_carrier_sync_active(state);
    nxdn_print_sync_banner(opts, state, &ctx);

    nxdn_collect_payload_and_unpack(opts, state, &ctx);

    ctx.lich_rf = (ctx.lich >> 5) & 0x3;
    ctx.direction = (ctx.lich % 2 == 0) ? 0 : 1;

    nxdn_print_rf_channel_type(&ctx);
    nxdn_apply_limazulu_voice_tweak(opts, state, &ctx);

    if (opts->scanner_mode == 1) {
        state->last_cc_sync_time = time(NULL) + 2;
    }

    nxdn_print_voice_or_data_and_sync_lfsr(state, &ctx);
    nxdn_update_sacch_mode(state, ctx.lich);
    nxdn_decode_control_channels(opts, state, &ctx);
    nxdn_process_voice_and_mbe(opts, state, &ctx);
    nxdn_handle_post_voice_facch2_lfsr(state, &ctx);

    if ((opts->payload == 1 && !ctx.voice) || opts->payload == 0) {
        DSD_FPRINTF(stderr, "\n");
    }

END:
    nxdn_finalize_sync_reject(state);
}

#ifdef DSD_NEO_TEST_HOOKS
int
dsd_neo_nxdn_test_route_decoded_lich(dsd_opts* opts, dsd_state* state, uint8_t lich, const uint8_t bits[364],
                                     const uint8_t reliab[364]) {
    if (opts == NULL || state == NULL || bits == NULL || reliab == NULL) {
        return -1;
    }

    nxdn_frame_ctx ctx;
    nxdn_frame_ctx_init(&ctx);
    ctx.lich = lich;

    if (!nxdn_validate_lich_direction(opts, state, &ctx)) {
        goto END;
    }
    if (!nxdn_apply_lich_profile(opts, state, &ctx)) {
        goto END;
    }

    nxdn_mark_carrier_sync_active(state);

    for (size_t i = 0U; i < 182U; i++) {
        const size_t idx = i * 2U;
        ctx.dbuf[i] = (uint8_t)(((bits[idx] & 1U) << 1) | (bits[idx + 1U] & 1U));
        ctx.dbuf_reliab[i] = reliab[idx];
    }
    nxdn_unpack_payload_fields(&ctx);

    ctx.lich_rf = (ctx.lich >> 5) & 0x3;
    ctx.direction = (ctx.lich % 2 == 0) ? 0 : 1;

    nxdn_apply_limazulu_voice_tweak(opts, state, &ctx);
    if (opts->scanner_mode == 1) {
        state->last_cc_sync_time = time(NULL) + 2;
    }

    nxdn_print_voice_or_data_and_sync_lfsr(state, &ctx);
    nxdn_update_sacch_mode(state, ctx.lich);
    nxdn_decode_control_channels(opts, state, &ctx);
    nxdn_process_voice_and_mbe(opts, state, &ctx);
    nxdn_handle_post_voice_facch2_lfsr(state, &ctx);
    return 1;

END:
    nxdn_finalize_sync_reject(state);
    return 0;
}
#endif
