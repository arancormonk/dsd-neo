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
#include <dsd-neo/protocol/dstar/dstar_header.h>
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
    char strt[60];
    uint16_t crc_ext;
    uint16_t crc_cmp;
    uint8_t
        header_flag1; /* the header's first flag byte as received: the header CRC covers it, never its display copy */
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
    for (int i = 0; i < 48; i++) {
        ctx->strt[i] = (char)ctx->hd_bytes[i + 1];
    }
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

/* A rebroadcast header whose CRC passed. Every field it acts on -- the callsigns and the DATA flag that decides whether it
 * names a voice call -- is read from the bytes the CRC was taken over, never from the display copy (whose clean-up turns
 * a flag byte of 0x80 into a space). */
static void
dstar_sd_handle_header_format(dsd_state* state, const dstar_sd_ctx* ctx) {
    if (ctx->crc_cmp == ctx->crc_ext) {
        DSD_FPRINTF(stderr, " RPT 2: %s", ctx->str1);
        DSD_FPRINTF(stderr, " RPT 1: %s", ctx->str2);
        DSD_FPRINTF(stderr, " DST: %s", ctx->str3);
        DSD_FPRINTF(stderr, " SRC: %s", ctx->str4);
        dstar_sd_print_header_flags(ctx->header_flag1);
        if ((ctx->header_flag1 & 0x80U) == 0U) {
            const int protocol = DSD_SYNC_IS_DSTAR(state->synctype) ? state->synctype : DSD_SYNC_DSTAR_VOICE_POS;
            dstar_header_observe(state, protocol, 0, ctx->str1, ctx->str2, ctx->str3, ctx->str4);
        } else {
            /* A data transmission's header names no call and opens none. Whatever a header heard before named -- a
               voice call left ACTIVE after its terminator and this transmission's RF header were missed among them --
               is not this transmission's: that naming goes, with the slow data assembled under it (issue #800). */
            dstar_tx_unname(state);
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

/* ---- What slow data is bound to (state.h dstar_sd_binding), and the only call it reaches ---- */

/* The slot 0 call a checked header of the current transmission named (dsd_state::dstar_tx_call_epoch), or 0. The one
 * rule every use of the header's naming goes through: the name holds only on the reception the header was heard on
 * (dsd_state::dstar_tx_call_origin). After a retune, a trunk-scan switch or another system's frame noted on the carrier
 * (the received identity's epoch moved), the header names no call: a call left ACTIVE across that change is not proven
 * to be the one the slow data heard now belongs to. */
static uint64_t
dstar_tx_named_call(const dsd_state* state) {
    if (state->dstar_tx_call_epoch == 0U || !dsd_position_origin_matches(state, &state->dstar_tx_call_origin)) {
        return 0U;
    }
    return state->dstar_tx_call_epoch;
}

/* Capture what an assembly's first byte or block is heard under. */
static void
dstar_sd_bind(const dsd_opts* opts, const dsd_state* state, dstar_sd_binding* b) {
    DSD_MEMSET(b, 0, sizeof(*b));
    b->tx = state->dstar_tx_seq;
    b->call_epoch = dstar_tx_named_call(state);
    b->confirmed = (uint8_t)dstar_confirm_is_confirmed(state);
    dsd_position_origin_capture(opts, state, &b->origin);
}

/* 1 while everything @p b was captured under still holds: the same transmission, the same call named by its checked
 * header on this reception (dstar_tx_named_call(), or still none), the same confirmation and the same reception
 * generation. Anything else, the reset. */
static int
dstar_sd_binding_holds(const dsd_state* state, const dstar_sd_binding* b) {
    return b->tx == state->dstar_tx_seq && b->call_epoch == dstar_tx_named_call(state)
           && b->confirmed == (uint8_t)dstar_confirm_is_confirmed(state)
           && dsd_position_origin_matches(state, &b->origin);
}

/* The call slow data bound by @p b may reach: the slot 0 call a checked header of its transmission named on the
 * reception the slow data is heard on (dstar_tx_named_call()), still ACTIVE and D-STAR's. No other call -- one the
 * transmission's header did not name, one left ACTIVE by the transmission before (its terminator missed) or across a
 * reception change, an anonymous one -- is the transmission's. */
static int
dstar_sd_bound_call(const dsd_state* state, const dstar_sd_binding* b, dsd_call_snapshot* call) {
    return b->call_epoch != 0U && b->call_epoch == dstar_tx_named_call(state) && dsd_call_state_get(state, 0U, call) > 0
           && call->phase == DSD_CALL_PHASE_ACTIVE && DSD_SYNC_IS_DSTAR(call->protocol) && call->epoch == b->call_epoch;
}

/*
 * An admitted D-PRS position: its check is the record's own CRC; its source is the callsign of the call a checked
 * header of the record's transmission named before the record's first byte (dstar_header_observe()), and it reaches
 * only that call's row. A record bound to no named call (late entry, an RF header that failed its CRC, a blank MY
 * field) has no checked source and reaches no row; the feed still takes it, anonymous.
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
    const int on_call = dstar_sd_bound_call(state, &a->bind, &call) && call.source_text[0] != '\0';
    if (on_call) {
        DSD_SNPRINTF(fix.source_text, sizeof(fix.source_text), "%s", call.source_text);
    }
    if (dsd_position_admit(opts, state, &fix, 1, on_call, &a->bind.origin) == DSD_POSITION_REFUSED) {
        return;
    }
    dstar_aprs_text(pos, state->dstar_gps, sizeof(state->dstar_gps));
    DSD_FPRINTF(stderr, "\n %s;", state->dstar_gps);
    // The display keeps the position either way; the history row takes it only through the transmission's call.
    if (on_call) {
        (void)dsd_event_set_open_call_position(state, 0U, &fix, state->dstar_gps);
    }
}

/* A record through its CR: only a CRC-16/X.25 over the text after "$$CRCxxxx," (CR included) that equals the four
 * upper-case hex digits proves it, and only then is it evidence or a position. */
static void
dstar_dprs_complete(const dsd_opts* opts, dsd_state* state) {
    const dstar_dprs_assembly* a = &state->dstar_dprs;
    /* Checked before the record counts as anything, evidence included: one assembled under another transmission,
       call, confirmation or reception proves nothing about this carrier. */
    if (!dstar_sd_binding_holds(state, &a->bind)) {
        return;
    }
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

/* A record starts at "$$CRC", under the binding its first byte (the hunt's) was captured with. */
static void
dstar_dprs_begin(dsd_state* state) {
    dstar_dprs_assembly* a = &state->dstar_dprs;
    DSD_MEMCPY(a->text, "$$CRC", DSTAR_DPRS_TAG_LEN);
    a->len = DSTAR_DPRS_TAG_LEN;
    a->collecting = 1U;
}

/* One GPS data byte while no record is being collected: the last five bytes are kept to find "$$CRC". */
static void
dstar_dprs_hunt(const dsd_opts* opts, dsd_state* state, uint8_t byte) {
    dstar_dprs_assembly* a = &state->dstar_dprs;
    if (a->len == 0U) {
        dstar_sd_bind(opts, state, &a->bind);
    }
    if (a->len == DSTAR_DPRS_TAG_LEN) {
        for (int i = 1; i < DSTAR_DPRS_TAG_LEN; i++) {
            a->text[i - 1] = a->text[i];
        }
        a->len--;
    }
    a->text[a->len++] = byte;
    if (a->len == DSTAR_DPRS_TAG_LEN && memcmp(a->text, "$$CRC", DSTAR_DPRS_TAG_LEN) == 0) {
        dstar_dprs_begin(state);
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
        dstar_dprs_begin(state);
    }
}

/* Feed the superframe's GPS blocks (as received, before any display clean-up) to the record. What was assembled under
 * a binding that no longer holds (another transmission, named call, confirmation or reception) is dropped first. */
static void
dstar_sd_collect_gps(const dsd_opts* opts, dsd_state* state, const uint8_t* raw) {
    dstar_dprs_assembly* a = &state->dstar_dprs;
    if (a->len != 0U && !dstar_sd_binding_holds(state, &a->bind)) {
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
 * place in the one being shown. A message assembled under a binding that no longer holds is dropped before this
 * superframe's blocks: none of it is this message's. The places of one message come in order after its block 0, so a
 * block whose place is at or before one already heard since then starts the next message, its block 0 missed: the
 * message so far is dropped too. The first block of a fresh assembly (a new transmission's or message's, its block 0
 * missed among them) starts from an empty message, so no character of the last one is shown with it. Returns 1 when
 * the superframe held a text block. */
static int
dstar_sd_collect_text(const dsd_opts* opts, dsd_state* state, const uint8_t* raw) {
    char* const msg = state->dstar_txt;
    dstar_text_assembly* t = &state->dstar_text;
    int any = 0;
    for (int b = 0; b < DSTAR_SD_BLOCKS; b++) {
        unsigned place = 0U;
        if (!dstar_sd_text_block(raw, b, &place)) {
            continue;
        }
        if (t->places != 0U
            && (!dstar_sd_binding_holds(state, &t->bind) || (place != 0U && (t->places >> place) != 0U))) {
            t->places = 0U;
            msg[0] = '\0';
        }
        if (t->places == 0U) {
            /* A fresh assembly: nothing shown so far is this message's, whichever block of it came first. */
            dstar_sd_bind(opts, state, &t->bind);
            msg[0] = '\0';
        }
        size_t len = strnlen(msg, DSTAR_SD_TEXT_CHARS);
        if (place == 0U) {
            len = 0U;
            t->places = 0U;
        }
        t->places |= (uint8_t)(1U << place);
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
    // The display keeps the text either way. The text carries no check of its own: the history row takes it only when
    // every block of it, its first (place 0) among them, was heard while the transmission was confirmed
    // (dstar_confirm.h) under one binding, and only through the call a checked header of that transmission named.
    const dstar_text_assembly* t = &state->dstar_text;
    dsd_call_snapshot call;
    if (t->bind.confirmed != 0U && (t->places & 1U) != 0U && dstar_sd_binding_holds(state, &t->bind)
        && dstar_sd_bound_call(state, &t->bind, &call)) {
        const int protocol = DSD_SYNC_IS_DSTAR(state->synctype) ? state->synctype : DSD_SYNC_DSTAR_VOICE_POS;
        (void)dsd_event_set_open_call_detail(state, 0U, protocol, DSD_EVENT_DETAIL_TEXT, state->dstar_txt);
    }
}

/* The GPS blocks' data (a D-PRS record or NMEA sentences) of every GPS block, wherever it sits in the superframe,
 * shown as it arrived: a raw dump, like every payload dump, only with the payload option (-Z). The "$$CRC" literal checks nothing, and a block is shown before its record is whole:
 * a D-PRS record is evidence and a position only once its CRC matches and admission admitted it (dstar_dprs_publish()
 * prints that one's position), so without -Z no part of a record reaches the output before then (issue #800, fail
 * closed). The "GPS DATA:" label names what the blocks are whatever part of a record they hold, so the diagnostics log
 * can remove them (D10). */
static void
dstar_sd_handle_gps_data(const dsd_opts* opts, const uint8_t* raw) {
    if (opts->payload != 1) {
        return;
    }
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

/* What a slow-data block is, by its own mini-header: GPS data (0x3n), a text block (0x40-0x43), filler (0x66), or
 * anything else. */
enum { DSTAR_SD_BLOCK_OTHER = 0, DSTAR_SD_BLOCK_GPS, DSTAR_SD_BLOCK_TEXT, DSTAR_SD_BLOCK_FILLER };

/* The blocks a rebroadcast header fills at the start of its superframe: its 41 octets, five per block. */
enum { DSTAR_SD_HEADER_BLOCKS = 9 };

#define DSTAR_SD_FILLER 0x66U

static int
dstar_sd_block_kind(const uint8_t* raw, int b) {
    const uint8_t mini = raw[(size_t)b * (size_t)DSTAR_SD_BLOCK_BYTES];
    unsigned place = 0U;
    if ((mini & DSTAR_SD_TYPE_MASK) == DSTAR_SD_TYPE_GPS) {
        return DSTAR_SD_BLOCK_GPS;
    }
    if (dstar_sd_text_block(raw, b, &place)) {
        return DSTAR_SD_BLOCK_TEXT;
    }
    return mini == DSTAR_SD_FILLER ? DSTAR_SD_BLOCK_FILLER : DSTAR_SD_BLOCK_OTHER;
}

/* The superframe's blocks of no known kind from block @p first on (a header's superframe: those after the header),
 * shown as "_UNK:", printable bytes only; GPS, text and filler blocks are never part of it, so GPS data reaches the
 * output only through dstar_sd_handle_gps_data() (-Z) or an admitted position. */
static void
dstar_sd_print_unknown(const uint8_t* raw, int first) {
    DSD_FPRINTF(stderr, " _UNK: ");
    for (int b = first; b < DSTAR_SD_BLOCKS; b++) {
        if (dstar_sd_block_kind(raw, b) != DSTAR_SD_BLOCK_OTHER) {
            continue;
        }
        const uint8_t* block = raw + ((size_t)b * (size_t)DSTAR_SD_BLOCK_BYTES);
        for (int i = 0; i < DSTAR_SD_BLOCK_BYTES; i++) {
            DSD_FPRINTF(stderr, "%c", dstar_sd_is_printable(block[i]) ? (char)block[i] : ' ');
        }
    }
}

/* Every superframe, a rebroadcast header's after the header itself (@p header: its first nine blocks are the header's):
 * each block classified by its own mini-header, wherever it sits. Text blocks make the message (the block a header
 * leaves after its nine among them); GPS blocks, any of them, the -Z "GPS DATA:" dump; the blocks of no known kind
 * outside the header, when no text block came, the "_UNK:" dump. */
static void
dstar_sd_handle_typed_blocks(const dsd_opts* opts, dsd_state* state, const dstar_sd_ctx* ctx, int header) {
    const int first = header ? DSTAR_SD_HEADER_BLOCKS : 0;
    const int text = dstar_sd_collect_text(opts, state, ctx->payload);
    if (text) {
        dstar_sd_handle_text_message(state);
    }
    int gps = 0;
    int other = 0;
    for (int b = 0; b < DSTAR_SD_BLOCKS; b++) {
        const int kind = dstar_sd_block_kind(ctx->payload, b);
        gps |= kind == DSTAR_SD_BLOCK_GPS;
        other |= b >= first && kind == DSTAR_SD_BLOCK_OTHER;
    }
    if (gps) {
        dstar_sd_handle_gps_data(opts, ctx->payload);
    }
    if (!text && other) {
        dstar_sd_print_unknown(ctx->payload, first);
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
    ctx.header_flag1 = ctx.hd_bytes[0];

    dstar_sd_init_display_buffers(&ctx);
    dstar_sd_capture_header_strings(&ctx);
    dstar_sd_sanitize_bytes(ctx.sd_bytes);
    dstar_sd_sanitize_bytes(ctx.hd_bytes);
    dstar_sd_capture_display_strings(&ctx);

    /* A header first: it may name the call the superframe's other blocks belong to. */
    const int header = ctx.payload[0] == 0x55;
    if (header) {
        dstar_sd_handle_header_format(state, &ctx);
    }
    dstar_sd_handle_typed_blocks(opts, state, &ctx, header);
    dstar_sd_collect_gps(opts, state, ctx.payload);

    dstar_sd_print_payload(opts, &ctx);
}
