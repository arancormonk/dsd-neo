// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/bit_packing.h>

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dstar_confirm.h"

//first 24-bits of the larger scramble table
enum { DSTAR_SLOW_DATA_SCRAMBLER_BITS = 24 };

static const uint8_t sd_d[DSTAR_SLOW_DATA_SCRAMBLER_BITS] = {0, 0, 0, 0,  //0
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
    uint16_t crc_ext;
    uint16_t crc_cmp;
    int len;
} dstar_sd_ctx;

/* Printable ASCII, space included: the only bytes slow data puts on the console or in a stored message. Control bytes
 * (ESC among them) never reach either; the raw bytes stay for the checks (the header CRC, the D-PRS record CRC). */
static int
dstar_sd_is_printable(uint8_t value) {
    return (value >= 0x20U) && (value < 0x7FU);
}

static void
dstar_sd_init_ctx(dstar_sd_ctx* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
}

static void
dstar_sd_apply_descrambler(uint8_t* sd) {
    int i;
    for (i = 0; i < 480; i++) {
        sd[i] ^= sd_d[i % DSTAR_SLOW_DATA_SCRAMBLER_BITS];
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
        sd_bytes[59 - i] = (uint8_t)convert_bits_into_output(&sd[i * 8 + 0], 8);
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
    /* Shown and put on the call: printable only. The CRC was taken over the raw header bytes. */
    dstar_text_make_printable(ctx->str1);
    dstar_text_make_printable(ctx->str2);
    dstar_text_make_printable(ctx->str3);
    dstar_text_make_printable(ctx->str4);
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
        if ((ctx->sd_bytes[1] & 0x80U) == 0U) {
            int protocol = DSD_SYNC_IS_DSTAR(state->synctype) ? state->synctype : DSD_SYNC_DSTAR_VOICE_POS;
            dsd_call_observation observation = {
                .protocol = protocol,
                .slot = 0U,
                .kind = DSD_CALL_KIND_VOICE,
            };
            DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", ctx->str4);
            DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", ctx->str3);
            DSD_SNPRINTF(observation.route_text[0], sizeof(observation.route_text[0]), "%s", ctx->str2);
            DSD_SNPRINTF(observation.route_text[1], sizeof(observation.route_text[1]), "%s", ctx->str1);
            (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
            /* The transmission's own callsign is on the call: a D-PRS record of it may name it again. A blank MY
               field names no one, so the callsign the call still carries is another transmission's. */
            state->dstar_dprs_source_unchecked = dstar_callsign_present(ctx->str4, 8U) ? 0U : 1U;
        }
        /* A CRC-16/X.25 over 39 octets of rebroadcast header: proof the transmission is
         * real, on its own (#421). Noted once the header's callsigns are on the call: a signal
         * search reads them from it when it hears the confirmation. */
        dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_STRONG);
        return;
    }
    DSD_FPRINTF(stderr, " SLOW DATA - HEADER FORMAT (CRC ERR)");
}

/* ---- D-PRS (GPS-A) records: JARL STD 6.0 section 6.8.1 and Ap2.1, issue #800 2.10 ---- */

enum {
    DSTAR_SD_BLOCK_BYTES = 6, /* a mini-header and up to five data bytes, over two voice frames */
    DSTAR_SD_BLOCKS = 10,     /* blocks in one superframe's 60 slow-data bytes */
    DSTAR_SD_BLOCK_DATA_MAX = 5,
    DSTAR_DPRS_TAG_LEN = 5,     /* "$$CRC" */
    DSTAR_DPRS_PREFIX_LEN = 10, /* "$$CRCxxxx," */
};

/* The mini-header type of a GPS block; its low nibble counts the block's data bytes. */
#define DSTAR_SD_TYPE_MASK 0xF0U
#define DSTAR_SD_TYPE_GPS  0x30U

/* An APRS position (APRS 1.0.1 chapter 6, as D-PRS fixes it): latitude ddmm.hh and longitude dddmm.hh, in degrees,
 * minutes and hundredths of a minute, and the optional time stamp. */
typedef struct {
    uint8_t lat_deg;
    uint8_t lat_min;
    uint8_t lat_hund;
    char lat_hemi;
    uint8_t lon_deg;
    uint8_t lon_min;
    uint8_t lon_hund;
    char lon_hemi;
    char time_kind; /* 'z' DDHHMM, 'h' HHMMSS, 0 none or not readable */
    uint8_t time_fields[3];
} dstar_aprs_position;

static int
dstar_aprs_digits(const uint8_t* text, int count, unsigned* out) {
    unsigned value = 0U;
    for (int i = 0; i < count; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        value = (value * 10U) + (unsigned)(text[i] - '0');
    }
    *out = value;
    return 1;
}

/* One coordinate, @p deg_digits degree digits then mm.hh and the hemisphere; every part checked before it is used: at
 * most @p max_deg degrees, at most 59 minutes, and @p max_deg itself only with zero minutes. */
static int
dstar_aprs_coordinate(const uint8_t* text, int deg_digits, unsigned max_deg, const char* hemispheres, uint8_t* deg,
                      uint8_t* min, uint8_t* hund, char* hemi) {
    unsigned d = 0U;
    unsigned m = 0U;
    unsigned h = 0U;
    if (!dstar_aprs_digits(text, deg_digits, &d) || !dstar_aprs_digits(text + deg_digits, 2, &m)
        || text[deg_digits + 2] != '.' || !dstar_aprs_digits(text + deg_digits + 3, 2, &h)) {
        return 0;
    }
    const char c = (char)text[deg_digits + 5];
    if (c != hemispheres[0] && c != hemispheres[1]) {
        return 0;
    }
    if (d > max_deg || m > 59U || (d == max_deg && (m != 0U || h != 0U))) {
        return 0;
    }
    *deg = (uint8_t)d;
    *min = (uint8_t)m;
    *hund = (uint8_t)h;
    *hemi = c;
    return 1;
}

/* The time stamp after '/': DDHHMMz (day of month, hours, minutes) or HHMMSSh (JARL STD 6.0 6.8.1(4)). One that does
 * not read as a time, or a transmitter's "______z" for no time, leaves the time unknown; the position stays. */
static void
dstar_aprs_timestamp(const uint8_t* text, dstar_aprs_position* pos) {
    unsigned f[3];
    pos->time_kind = 0;
    for (size_t i = 0; i < 3U; i++) {
        if (!dstar_aprs_digits(text + (2U * i), 2, &f[i])) {
            return;
        }
    }
    if (text[6] == 'z' && f[0] >= 1U && f[0] <= 31U && f[1] <= 23U && f[2] <= 59U) {
        pos->time_kind = 'z';
    } else if (text[6] == 'h' && f[0] <= 23U && f[1] <= 59U && f[2] <= 59U) {
        pos->time_kind = 'h';
    } else {
        return;
    }
    for (int i = 0; i < 3; i++) {
        pos->time_fields[i] = (uint8_t)f[i];
    }
}

/* The position in @p body (the record's text after "$$CRCxxxx,", without its CR): the information field follows the
 * first ':'; '!' starts a position, '/' a 7-character time stamp and then the position. 1 when one was read. */
static int
dstar_aprs_parse(const uint8_t* body, size_t len, dstar_aprs_position* pos) {
    DSD_MEMSET(pos, 0, sizeof(*pos));
    const uint8_t* colon = (const uint8_t*)memchr(body, ':', len);
    if (colon == NULL) {
        return 0;
    }
    size_t at = (size_t)(colon - body) + 1U;
    if (at >= len) {
        return 0;
    }
    if (body[at] == '/') {
        if (len - at < 8U) {
            return 0;
        }
        dstar_aprs_timestamp(body + at + 1U, pos);
        at += 8U;
    } else if (body[at] == '!') {
        at += 1U;
    } else {
        return 0;
    }
    /* ddmm.hhN, the symbol table, dddmm.hhE, the symbol code. */
    if (len - at < 19U) {
        return 0;
    }
    return dstar_aprs_coordinate(body + at, 2, 90U, "NS", &pos->lat_deg, &pos->lat_min, &pos->lat_hund, &pos->lat_hemi)
           && dstar_aprs_coordinate(body + at + 9U, 3, 180U, "EW", &pos->lon_deg, &pos->lon_min, &pos->lon_hund,
                                    &pos->lon_hemi);
}

/* Degrees, minutes and hundredths of a minute to degrees x 1e7, rounded. */
static int32_t
dstar_aprs_e7(uint8_t deg, uint8_t min, uint8_t hund, int negative) {
    const int64_t hundredths = ((int64_t)min * 100) + (int64_t)hund; /* of a minute: 6000 per degree */
    const int64_t e7 = ((int64_t)deg * 10000000) + (((hundredths * 10000000) + 3000) / 6000);
    return (int32_t)(negative ? -e7 : e7);
}

static void
dstar_aprs_text(const dstar_aprs_position* pos, char* out, size_t cap) {
    const int n =
        DSD_SNPRINTF(out, cap, "APRS - Lat: %02ud %02u.%02um %c Lon: %03ud %02u.%02um %c", pos->lat_deg, pos->lat_min,
                     pos->lat_hund, pos->lat_hemi, pos->lon_deg, pos->lon_min, pos->lon_hund, pos->lon_hemi);
    if (n <= 0 || (size_t)n >= cap) {
        return;
    }
    if (pos->time_kind == 'h') {
        (void)DSD_SNPRINTF(out + n, cap - (size_t)n, " %02u:%02u:%02uZ", pos->time_fields[0], pos->time_fields[1],
                           pos->time_fields[2]);
    } else if (pos->time_kind == 'z') {
        (void)DSD_SNPRINTF(out + n, cap - (size_t)n, " D%02u %02u:%02uZ", pos->time_fields[0], pos->time_fields[1],
                           pos->time_fields[2]);
    }
}

static int
dstar_dprs_active_call(const dsd_state* state, dsd_call_snapshot* call) {
    return dsd_call_state_get(state, 0U, call) > 0 && call->phase == DSD_CALL_PHASE_ACTIVE
           && DSD_SYNC_IS_DSTAR(call->protocol);
}

/* The slot 0 call a record is bound to: the ACTIVE D-STAR call's epoch, 0 when there is none. */
static uint64_t
dstar_dprs_call_epoch(const dsd_state* state) {
    dsd_call_snapshot call;
    return dstar_dprs_active_call(state, &call) ? call.epoch : 0U;
}

/*
 * An admitted D-PRS position: its check is the record's own CRC; its source is the callsign of the call it was heard
 * in, which only a header whose CRC-16/X.25 passed names (dstar_header.c and the slow-data header above), and only
 * when the whole record arrived within that call. A record begun outside an active call has no checked source, and
 * neither has one completed after an RF header (another transmission began, maybe another radio's) until a checked
 * header names the transmission's MY callsign (dsd_state::dstar_dprs_source_unchecked).
 */
static void
dstar_dprs_publish(const dsd_opts* opts, dsd_state* state, const dstar_aprs_position* pos) {
    const dstar_dprs_assembly* a = &state->dstar_dprs;
    dsd_position_fix fix;
    dsd_position_fix_init(&fix);
    fix.kind = (uint8_t)DSD_POS_DSTAR_DPRS;
    fix.protocol = (uint8_t)(DSD_SYNC_IS_DSTAR(state->synctype) ? state->synctype : DSD_SYNC_DSTAR_VOICE_POS);
    fix.slot = 0U;
    fix.lat_e7 = dstar_aprs_e7(pos->lat_deg, pos->lat_min, pos->lat_hund, pos->lat_hemi == 'S');
    fix.lon_e7 = dstar_aprs_e7(pos->lon_deg, pos->lon_min, pos->lon_hund, pos->lon_hemi == 'W');
    dsd_call_snapshot call;
    int source_verified = 0;
    if (a->epoch != 0U && state->dstar_dprs_source_unchecked == 0U && dstar_dprs_active_call(state, &call)
        && call.epoch == a->epoch && call.source_text[0] != '\0') {
        DSD_SNPRINTF(fix.source_text, sizeof(fix.source_text), "%s", call.source_text);
        source_verified = 1;
    }
    if (dsd_position_admit(opts, state, &fix, 1, source_verified, &a->origin) == DSD_POSITION_REFUSED) {
        return;
    }
    dstar_aprs_text(pos, state->dstar_gps, sizeof(state->dstar_gps));
    DSD_FPRINTF(stderr, "\n %s;", state->dstar_gps);
    // The display keeps the position either way; the history row takes it only through an open call.
    (void)dsd_event_set_open_call_position(state, 0U, &fix, state->dstar_gps);
}

/* A record through its CR: only a CRC-16/X.25 over the text after "$$CRCxxxx," (CR included) that equals the four
 * upper-case hex digits proves it, and only then is it evidence or a position. */
static void
dstar_dprs_complete(const dsd_opts* opts, dsd_state* state) {
    const dstar_dprs_assembly* a = &state->dstar_dprs;
    if (a->len <= DSTAR_DPRS_PREFIX_LEN || a->text[DSTAR_DPRS_PREFIX_LEN - 1] != ',') {
        DSD_FPRINTF(stderr, " D-PRS (FORMAT ERR)");
        return;
    }
    /* dstar_crc16() returns the CRC low byte first, as the header carries it. */
    const uint16_t wire = dstar_crc16(a->text + DSTAR_DPRS_PREFIX_LEN, (size_t)a->len - DSTAR_DPRS_PREFIX_LEN);
    const uint16_t crc = (uint16_t)((wire << 8) | (wire >> 8));
    char hex[5];
    DSD_SNPRINTF(hex, sizeof(hex), "%04X", (unsigned)crc);
    if (memcmp(hex, a->text + DSTAR_DPRS_TAG_LEN, 4U) != 0) {
        DSD_FPRINTF(stderr, " D-PRS (CRC ERR)");
        return;
    }
    dstar_confirm_note_evidence(state, DSTAR_EVIDENCE_STRONG);
    dstar_aprs_position pos;
    if (!dstar_aprs_parse(a->text + DSTAR_DPRS_PREFIX_LEN, (size_t)a->len - DSTAR_DPRS_PREFIX_LEN - 1U, &pos)) {
        DSD_FPRINTF(stderr, " D-PRS (NO POSITION)");
        return;
    }
    dstar_dprs_publish(opts, state, &pos);
}

static void
dstar_dprs_reset(dstar_dprs_assembly* a) {
    DSD_MEMSET(a, 0, sizeof(*a));
}

/* A record starts at "$$CRC": bound to the call active now and to the reception it was heard on. */
static void
dstar_dprs_begin(const dsd_opts* opts, dsd_state* state) {
    dstar_dprs_assembly* a = &state->dstar_dprs;
    DSD_MEMCPY(a->text, "$$CRC", DSTAR_DPRS_TAG_LEN);
    a->len = DSTAR_DPRS_TAG_LEN;
    a->collecting = 1U;
    a->epoch = dstar_dprs_call_epoch(state);
    dsd_position_origin_capture(opts, state, &a->origin);
}

/* One GPS data byte while no record is being collected: the last five bytes are kept to find "$$CRC". */
static void
dstar_dprs_hunt(const dsd_opts* opts, dsd_state* state, uint8_t byte) {
    dstar_dprs_assembly* a = &state->dstar_dprs;
    if (a->len == DSTAR_DPRS_TAG_LEN) {
        for (int i = 1; i < DSTAR_DPRS_TAG_LEN; i++) {
            a->text[i - 1] = a->text[i];
        }
        a->len--;
    }
    a->text[a->len++] = byte;
    if (a->len == DSTAR_DPRS_TAG_LEN && memcmp(a->text, "$$CRC", DSTAR_DPRS_TAG_LEN) == 0) {
        dstar_dprs_begin(opts, state);
    }
}

/* A record grew past DSTAR_DPRS_RECORD_MAX: it is dropped, but its last bytes may be the start of the next record's
 * "$$CRC" (its CR was lost), so they and @p byte go on to the hunt for it. */
static void
dstar_dprs_overflow(const dsd_opts* opts, dsd_state* state, uint8_t byte) {
    dstar_dprs_assembly* a = &state->dstar_dprs;
    uint8_t tail[DSTAR_DPRS_TAG_LEN - 1];
    DSD_MEMCPY(tail, a->text + a->len - sizeof(tail), sizeof(tail));
    dstar_dprs_reset(a);
    for (size_t i = 0U; i < sizeof(tail); i++) {
        dstar_dprs_hunt(opts, state, tail[i]);
    }
    dstar_dprs_hunt(opts, state, byte);
}

/* One GPS data byte: until "$$CRC" is seen, the last five bytes are kept to find it; then the record grows through its
 * CR, or is dropped once longer than DSTAR_DPRS_RECORD_MAX. A "$$CRC" inside a record means the one collected lost
 * its CR: the new record starts there, so a lost CR costs only its own record, at the bound too. */
static void
dstar_dprs_push(const dsd_opts* opts, dsd_state* state, uint8_t byte) {
    dstar_dprs_assembly* a = &state->dstar_dprs;
    if (!a->collecting) {
        dstar_dprs_hunt(opts, state, byte);
        return;
    }
    if (a->len >= DSTAR_DPRS_RECORD_MAX) {
        dstar_dprs_overflow(opts, state, byte);
        return;
    }
    a->text[a->len++] = byte;
    if (byte == 0x0DU) {
        dstar_dprs_complete(opts, state);
        dstar_dprs_reset(a);
    } else if (a->len >= 2U * DSTAR_DPRS_TAG_LEN
               && memcmp(a->text + a->len - DSTAR_DPRS_TAG_LEN, "$$CRC", DSTAR_DPRS_TAG_LEN) == 0) {
        DSD_FPRINTF(stderr, " D-PRS (NO CR)");
        dstar_dprs_begin(opts, state);
    }
}

/* Feed the superframe's GPS blocks (as received, before any display clean-up) to the record. A record begun in another
 * call is not this one's. One begun before any call was active (late entry: the call opens once the transmission is
 * confirmed) is kept: it names no checked source, and is published as unverified. */
static void
dstar_sd_collect_gps(const dsd_opts* opts, dsd_state* state, const uint8_t* raw) {
    dstar_dprs_assembly* a = &state->dstar_dprs;
    if (a->collecting && a->epoch != 0U && a->epoch != dstar_dprs_call_epoch(state)) {
        dstar_dprs_reset(a);
    }
    for (int b = 0; b < DSTAR_SD_BLOCKS; b++) {
        const uint8_t* block = raw + ((size_t)b * (size_t)DSTAR_SD_BLOCK_BYTES);
        if ((block[0] & DSTAR_SD_TYPE_MASK) != DSTAR_SD_TYPE_GPS) {
            continue;
        }
        const int count = block[0] & 0x0F;
        if (count > DSTAR_SD_BLOCK_DATA_MAX) {
            dstar_dprs_reset(a); /* no such block: the record cannot be whole */
            continue;
        }
        for (int i = 1; i <= count; i++) {
            dstar_dprs_push(opts, state, block[i]);
        }
    }
}

/* The 20-character text message (JARL STD 6.0 6.8.1(3)): four blocks of five characters, each block's mini-header 0x40
 * + its place in the message (0-3). */
enum { DSTAR_SD_TEXT_CHARS = 20, DSTAR_SD_TEXT_BLOCK_CHARS = 5 };

#define DSTAR_SD_TYPE_TEXT 0x40U

/* Whether block @p b of the raw slow data @p raw is a text block, and its place in the message. */
static int
dstar_sd_text_block(const uint8_t* raw, int b, unsigned* place) {
    const uint8_t mini = raw[(size_t)b * (size_t)DSTAR_SD_BLOCK_BYTES];
    if ((mini & DSTAR_SD_TYPE_MASK) != DSTAR_SD_TYPE_TEXT || (mini & 0x0FU) > 3U) {
        return 0;
    }
    *place = mini & 0x0FU;
    return 1;
}

/* Each block by its own mini-header: a text block's five characters go to their place in the message, and nothing of
 * another block (GPS data among them) ever does. A block 0 starts a new message; a later block of the message fills its
 * place in the one being shown. Returns 1 when the superframe held a text block. */
static int
dstar_sd_collect_text(dsd_state* state, const uint8_t* raw) {
    char* const msg = state->dstar_txt;
    int any = 0;
    for (int b = 0; b < DSTAR_SD_BLOCKS; b++) {
        unsigned place = 0U;
        if (!dstar_sd_text_block(raw, b, &place)) {
            continue;
        }
        size_t len = strnlen(msg, DSTAR_SD_TEXT_CHARS);
        if (place == 0U) {
            len = 0U;
        }
        for (; len < DSTAR_SD_TEXT_CHARS; len++) {
            msg[len] = ' ';
        }
        msg[DSTAR_SD_TEXT_CHARS] = '\0';
        const uint8_t* data = raw + ((size_t)b * (size_t)DSTAR_SD_BLOCK_BYTES) + 1U;
        for (unsigned i = 0U; i < DSTAR_SD_TEXT_BLOCK_CHARS; i++) {
            msg[(place * DSTAR_SD_TEXT_BLOCK_CHARS) + i] = dstar_sd_is_printable(data[i]) ? (char)data[i] : ' ';
        }
        any = 1;
    }
    if (any) {
        size_t len = strnlen(msg, DSTAR_SD_TEXT_CHARS);
        while (len > 0U && msg[len - 1U] == ' ') {
            msg[--len] = '\0';
        }
    }
    return any;
}

static void
dstar_sd_handle_text_message(dsd_state* state) {
    DSD_FPRINTF(stderr, " TEXT: %s", state->dstar_txt);
    // The display keeps the text either way; the history row takes it only through an open call.
    (void)dsd_event_set_open_call_detail(state, 0U, DSD_EVENT_DETAIL_TEXT, state->dstar_txt);
}

/* The GPS blocks' data (a D-PRS record or NMEA sentences), shown as it arrived. The "$$CRC" literal checks nothing: a
 * D-PRS record is evidence and a position only once it is whole and its CRC matches (dstar_sd_collect_gps()). The
 * "GPS DATA:" label names what the blocks are whatever part of a record they hold, so the diagnostics log can remove
 * them (issue #800, D10). */
static void
dstar_sd_handle_gps_data(const uint8_t* raw) {
    DSD_FPRINTF(stderr, " GPS DATA: ");
    for (int b = 0; b < DSTAR_SD_BLOCKS; b++) {
        const uint8_t* block = raw + ((size_t)b * (size_t)DSTAR_SD_BLOCK_BYTES);
        if ((block[0] & DSTAR_SD_TYPE_MASK) != DSTAR_SD_TYPE_GPS) {
            continue;
        }
        const int count = block[0] & 0x0F;
        for (int i = 1; i <= count && i <= DSTAR_SD_BLOCK_DATA_MAX; i++) {
            if (dstar_sd_is_printable(block[i])) {
                DSD_FPRINTF(stderr, "%c", block[i]);
            }
        }
    }
}

/* A superframe that is no header: its text blocks and its GPS blocks, each chosen by its own mini-header. */
static void
dstar_sd_handle_non_header(dsd_state* state, const dstar_sd_ctx* ctx) {
    const int text = dstar_sd_collect_text(state, ctx->payload);
    if (text) {
        dstar_sd_handle_text_message(state);
    }
    if ((ctx->payload[0] & DSTAR_SD_TYPE_MASK) == DSTAR_SD_TYPE_GPS) {
        dstar_sd_handle_gps_data(ctx->payload);
    } else if (!text) {
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
void
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
    dstar_sd_collect_gps(opts, state, ctx.payload);

    dstar_sd_print_payload(opts, &ctx);
}
