// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static inline void dsd_append(char* dst, size_t dstsz, const char* src);
static void processDSTAR_SD(const dsd_opts* opts, dsd_state* state, uint8_t* sd);

//simplified DSTAR
void
processDSTAR(dsd_opts* opts, dsd_state* state) {
    uint8_t sd[480];
    DSD_MEMSET(sd, 0, sizeof(sd));
    int i, j;
    char ambe_fr[4][24];
    DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));

    //20 voice and 19 slow data frames (20th is frame sync)
    for (j = 0; j < 21; j++) {

        DSD_MEMSET(ambe_fr, 0, sizeof(ambe_fr));
        const int* w = dstar_interleave_w;
        const int* x = dstar_interleave_x;

        for (i = 0; i < 72; i++) {
            int dibit = getDibit(opts, state);
            ambe_fr[*w][*x] = dibit & 1;
            w++;
            x++;
        }

        soft_mbe(opts, state, NULL, ambe_fr, NULL);

        if (j != 20) {
            for (i = 0; i < 24; i++) {
                //slow data
                sd[(j * 24) + i] = (uint8_t)getDibit(opts, state);
            }
        }

        if (j == 20) {
            processDSTAR_SD(opts, state, sd);
        }

        //since we are in a long loop, use this to improve response time in ncurses
        if (opts->use_ncurses_terminal == 1) {
            ui_publish_both_and_redraw(opts, state);
        }

        //slot 1
        watchdog_event_history(opts, state, 0);
        watchdog_event_current(opts, state, 0);
    }

    DSD_FPRINTF(stderr, "\n");
}

void
processDSTAR_HD(dsd_opts* opts, dsd_state* state) {

    int i;
    float soft_symbols[660];

    // Capture soft symbols for soft-decision decoding
    for (i = 0; i < 660; i++) {
        getDibitAndSoftSymbol(opts, state, &soft_symbols[i]);
    }

    dstar_header_decode_soft(state, soft_symbols);
    processDSTAR(opts, state);
}

//first 24-bits of the larger scramble table
static const uint8_t sd_d[48] = {0, 0, 0, 0,  //0
                                 1, 1, 1, 0,  //E
                                 1, 1, 1, 1,  //F
                                 0, 0, 1, 0,  //2
                                 1, 1, 0, 0,  //C
                                 1, 0, 0, 1}; //9

typedef struct dstar_sd_ctx_s {
    uint8_t sd_bytes[60];
    uint8_t hd_bytes[60];
    uint8_t payload[60];
    char str1[9];
    char str2[9];
    char str3[9];
    char str4[13];
    char strf[60];
    char strt[60];
    char type[7];
    uint16_t crc_ext;
    uint16_t crc_cmp;
    int len;
} dstar_sd_ctx;

static int
dstar_sd_is_printable(uint8_t value) {
    return (value > 0x19) && (value < 0x7F);
}

static void
dstar_sd_init_ctx(dstar_sd_ctx* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
}

static void
dstar_sd_apply_descrambler(uint8_t* sd) {
    int i;
    for (i = 0; i < 480; i++) {
        sd[i] ^= sd_d[i % 24];
    }
}

static void
dstar_sd_reverse_bits(uint8_t* sd) {
    int i;
    uint8_t rev[480];
    for (i = 0; i < 480; i++) {
        rev[i] = sd[479 - i];
    }
    for (i = 0; i < 480; i++) {
        sd[i] = rev[i];
    }
}

static void
dstar_sd_pack_bytes(const uint8_t* sd, uint8_t* sd_bytes) {
    int i;
    for (i = 0; i < 60; i++) {
        sd_bytes[59 - i] = (uint8_t)ConvertBitIntoBytes(&sd[i * 8 + 0], 8);
    }
}

static int
dstar_sd_payload_len(const uint8_t* sd_bytes) {
    int len = sd_bytes[0] & 0xF;
    return len + 1;
}

static void
dstar_sd_load_truncated_payload(const uint8_t* sd_bytes, uint8_t* hd_bytes, int len) {
    int i;
    int j;
    int marker;

    for (i = 0, j = 0; i < 50; i++) {
        j++;
        hd_bytes[i] = sd_bytes[j];
        for (marker = 1; marker <= 9; marker++) {
            if (j == len * marker - 1) {
                j++;
            }
        }
    }
}

static void
dstar_sd_init_display_buffers(dstar_sd_ctx* ctx) {
    DSD_MEMSET(ctx->strf, 0x20, sizeof(ctx->strf));
    DSD_MEMSET(ctx->strt, 0x20, sizeof(ctx->strt));
}

static void
dstar_sd_capture_header_strings(dstar_sd_ctx* ctx) {
    DSD_MEMCPY(ctx->str1, ctx->hd_bytes + 3, 8);
    DSD_MEMCPY(ctx->str2, ctx->hd_bytes + 11, 8);
    DSD_MEMCPY(ctx->str3, ctx->hd_bytes + 19, 8);
    DSD_MEMCPY(ctx->str4, ctx->hd_bytes + 27, 12);
    ctx->str1[8] = '\0';
    ctx->str2[8] = '\0';
    ctx->str3[8] = '\0';
    ctx->str4[12] = '\0';
}

static void
dstar_sd_sanitize_bytes(uint8_t* bytes) {
    int i;
    for (i = 1; i < 60; i++) {
        if (bytes[i] < 0x20 || bytes[i] > 0x7E) {
            bytes[i] = 0x20;
        }
        if (i < 59 && bytes[i] == 0x66 && bytes[i + 1] == 0x66) {
            bytes[i] = 0x00;
        }
        if (i == 59 && bytes[i] == 0x66) {
            bytes[i] = 0x00;
        }
    }
}

static void
dstar_sd_capture_display_strings(dstar_sd_ctx* ctx) {
    for (int i = 0; i < 58; i++) {
        ctx->strf[i] = (char)ctx->sd_bytes[i + 1];
    }
    for (int i = 0; i < 48; i++) {
        ctx->strt[i] = (char)ctx->hd_bytes[i + 1];
    }
    ctx->strf[59] = '\0';
    ctx->strt[59] = '\0';
    DSD_MEMSET(ctx->type, 0, sizeof(ctx->type));
    for (int i = 0; i < 5; i++) {
        ctx->type[i] = (char)ctx->sd_bytes[i + 1];
    }
    ctx->type[6] = '\0';
}

static void
dstar_sd_print_header_flags(uint8_t flags) {
    if (flags & 0x80) {
        DSD_FPRINTF(stderr, " DATA");
    }
    if (flags & 0x40) {
        DSD_FPRINTF(stderr, " REPEATER");
    }
    if (flags & 0x20) {
        DSD_FPRINTF(stderr, " INTERRUPTED");
    }
    if (flags & 0x10) {
        DSD_FPRINTF(stderr, " CONTROL SIGNAL");
    }
    if (flags & 0x08) {
        DSD_FPRINTF(stderr, " URGENT");
    }
}

static void
dstar_sd_handle_header_format(dsd_state* state, const dstar_sd_ctx* ctx) {
    if (ctx->crc_cmp == ctx->crc_ext) {
        DSD_FPRINTF(stderr, " RPT 2: %s", ctx->str1);
        DSD_FPRINTF(stderr, " RPT 1: %s", ctx->str2);
        DSD_FPRINTF(stderr, " DST: %s", ctx->str3);
        DSD_FPRINTF(stderr, " SRC: %s", ctx->str4);
        dstar_sd_print_header_flags(ctx->sd_bytes[1]);
        DSD_MEMCPY(state->dstar_rpt2, ctx->str1, sizeof(ctx->str1));
        DSD_MEMCPY(state->dstar_rpt1, ctx->str2, sizeof(ctx->str2));
        DSD_MEMCPY(state->dstar_dst, ctx->str3, sizeof(ctx->str3));
        DSD_MEMCPY(state->dstar_src, ctx->str4, sizeof(ctx->str4));
        return;
    }
    DSD_FPRINTF(stderr, " SLOW DATA - HEADER FORMAT (CRC ERR)");
}

static void
dstar_sd_emit_truncated_ascii(const uint8_t* sd_bytes, char* out, int capture) {
    int i;
    for (i = 1; i < 59; i++) {
        if (i > 1 && (i % 6 == 0 || i % 6 == 1)) {
            continue;
        }
        if (dstar_sd_is_printable(sd_bytes[i])) {
            DSD_FPRINTF(stderr, "%c", sd_bytes[i]);
            if (capture && out != NULL) {
                out[i] = (char)sd_bytes[i];
            }
        }
    }
    if (capture && out != NULL) {
        out[59] = '\0';
    }
}

static void
dstar_sd_collect_aprs_bytes(const uint8_t* sd_bytes, uint8_t* aprs) {
    int i;
    int k;
    for (i = 1, k = 0; i < 59; i++) {
        if (i > 1 && (i % 6 == 0 || i % 6 == 1)) {
            continue;
        }
        aprs[k] = sd_bytes[i];
        if ((aprs[k] > 0x19) && (aprs[k] < 0x7F)) {
            aprs[i] = 0x20;
        }
        k++;
    }
}

static int
dstar_sd_find_aprs_start(const uint8_t* aprs) {
    int i;
    for (i = 30; i < 40; i++) {
        if (aprs[i] == 0x21) {
            return i + 1;
        }
    }
    return -1;
}

static void
dstar_sd_print_aprs_lat(dsd_state* state, const uint8_t* sd_bytes, const uint8_t* aprs, int* start, char* temp) {
    DSD_MEMCPY(temp, aprs + *start, 2);
    *start += 2;
    DSD_FPRINTF(stderr, "Lat: %sd ", temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, "Lat: ");
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, "d ");

    DSD_MEMCPY(temp, aprs + *start, 2);
    *start += 3;
    DSD_FPRINTF(stderr, "%sm ", temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, "m ");

    DSD_MEMCPY(temp, aprs + *start, 2);
    *start += 1;
    DSD_FPRINTF(stderr, "%ss ", temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, "s ");

    DSD_FPRINTF(stderr, "%c", aprs[*start]);
    if (sd_bytes[*start] == 0x4E) {
        dsd_append(state->dstar_gps, sizeof state->dstar_gps, "N ");
    } else if (sd_bytes[*start] == 0x53) {
        dsd_append(state->dstar_gps, sizeof state->dstar_gps, "S ");
    }
    DSD_FPRINTF(stderr, "; ");
}

static void
dstar_sd_print_aprs_lon(dsd_state* state, const uint8_t* aprs, int* start, char* temp, char* tempa) {
    *start += 3;
    DSD_MEMCPY(tempa, aprs + *start, 3);
    *start += 3;
    DSD_FPRINTF(stderr, "Lon: %sd ", tempa);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, "Lon: ");
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, tempa);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, "d ");

    DSD_MEMCPY(temp, aprs + *start, 2);
    *start += 3;
    DSD_FPRINTF(stderr, "%sm ", temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, "m ");

    DSD_MEMCPY(temp, aprs + *start, 2);
    *start += 2;
    DSD_FPRINTF(stderr, "%ss ", temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, temp);
    dsd_append(state->dstar_gps, sizeof state->dstar_gps, "s ");

    DSD_FPRINTF(stderr, "%c", aprs[*start]);
    if (aprs[*start] == 0x45) {
        dsd_append(state->dstar_gps, sizeof state->dstar_gps, "E ");
    } else if (aprs[*start] == 0x57) {
        dsd_append(state->dstar_gps, sizeof state->dstar_gps, "W ");
    }
    DSD_FPRINTF(stderr, "; ");
}

static void
dstar_sd_handle_aprs(dsd_state* state, const uint8_t* sd_bytes) {
    uint8_t aprs[60];
    char temp[8];
    char tempa[8];
    int start;

    DSD_FPRINTF(stderr, "\n APRS - ");
    DSD_SPRINTF(state->dstar_gps, "APRS - ");
    DSD_MEMSET(aprs, 0, sizeof(aprs));
    DSD_MEMSET(temp, 0, sizeof(temp));
    DSD_MEMSET(tempa, 0, sizeof(tempa));

    dstar_sd_collect_aprs_bytes(sd_bytes, aprs);
    start = dstar_sd_find_aprs_start(aprs);
    if (start == -1) {
        DSD_SPRINTF(state->event_history_s[0].Event_History_Items[0].gps_s, "%s", state->dstar_gps);
        return;
    }

    dstar_sd_print_aprs_lat(state, sd_bytes, aprs, &start, temp);
    dstar_sd_print_aprs_lon(state, aprs, &start, temp, tempa);
    DSD_SPRINTF(state->event_history_s[0].Event_History_Items[0].gps_s, "%s", state->dstar_gps);
}

static void
dstar_sd_handle_text_message(dsd_state* state, dstar_sd_ctx* ctx) {
    DSD_MEMSET(ctx->strt, 0x20, sizeof(ctx->strt));
    DSD_FPRINTF(stderr, " TEXT: ");
    dstar_sd_emit_truncated_ascii(ctx->sd_bytes, ctx->strt, 1);
    DSD_MEMCPY(state->dstar_txt, ctx->strt, sizeof(ctx->strt));
    DSD_SPRINTF(state->event_history_s[0].Event_History_Items[0].text_message, "%s", state->dstar_txt);
}

static void
dstar_sd_handle_fixed_form(dsd_state* state, dstar_sd_ctx* ctx) {
    if (strcmp(ctx->type, "$$CRC") == 0) {
        DSD_MEMSET(ctx->strt, 0x20, sizeof(ctx->strt));
        DSD_FPRINTF(stderr, " DATA: ");
        dstar_sd_emit_truncated_ascii(ctx->sd_bytes, ctx->strt, 0);
    }

    if (strcmp(ctx->type, "$$CRC") == 0) {
        dstar_sd_handle_aprs(state, ctx->sd_bytes);
    } else {
        dstar_sd_handle_text_message(state, ctx);
    }
}

static void
dstar_sd_handle_non_header(dsd_state* state, dstar_sd_ctx* ctx) {
    if (ctx->sd_bytes[0] == 0x35) {
        dstar_sd_handle_fixed_form(state, ctx);
    } else if (ctx->sd_bytes[0] == 0x40) {
        dstar_sd_handle_text_message(state, ctx);
    } else {
        DSD_FPRINTF(stderr, " _UNK:");
        DSD_FPRINTF(stderr, " %s", ctx->strf);
    }
}

static void
dstar_sd_print_payload(const dsd_opts* opts, const dstar_sd_ctx* ctx) {
    int i;
    if (opts->payload != 1) {
        return;
    }

    DSD_FPRINTF(stderr, "\n SD: ");
    for (i = 0; i < 60; i++) {
        DSD_FPRINTF(stderr, "[%02X]", ctx->payload[i]);
        if (i == 29) {
            DSD_FPRINTF(stderr, "\n     ");
        }
    }

    if (ctx->crc_cmp != ctx->crc_ext) {
        DSD_FPRINTF(stderr, "CRC - EXT: %04X CMP: %04X", ctx->crc_ext, ctx->crc_cmp);
    }
}

//no so simplified Slow Data
static void
processDSTAR_SD(const dsd_opts* opts, dsd_state* state, uint8_t* sd) {
    dstar_sd_ctx ctx;
    dstar_sd_init_ctx(&ctx);

    dstar_sd_apply_descrambler(sd);
    dstar_sd_reverse_bits(sd);
    dstar_sd_pack_bytes(sd, ctx.sd_bytes);
    DSD_MEMCPY(ctx.payload, ctx.sd_bytes, sizeof(ctx.payload));

    ctx.len = dstar_sd_payload_len(ctx.sd_bytes);
    dstar_sd_load_truncated_payload(ctx.sd_bytes, ctx.hd_bytes, ctx.len);
    ctx.crc_ext = (uint16_t)((ctx.hd_bytes[39] << 8) + ctx.hd_bytes[40]);
    ctx.crc_cmp = dstar_crc16(ctx.hd_bytes, 39);

    dstar_sd_init_display_buffers(&ctx);
    dstar_sd_capture_header_strings(&ctx);
    dstar_sd_sanitize_bytes(ctx.sd_bytes);
    dstar_sd_sanitize_bytes(ctx.hd_bytes);
    dstar_sd_capture_display_strings(&ctx);

    if (ctx.sd_bytes[0] == 0x55) {
        dstar_sd_handle_header_format(state, &ctx);
    } else {
        dstar_sd_handle_non_header(state, &ctx);
    }

    dstar_sd_print_payload(opts, &ctx);
}

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
