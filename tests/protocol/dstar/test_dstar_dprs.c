// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #800 2.10: a D-STAR D-PRS (GPS-A) position comes only from a whole record, collected from the slow data's GPS
 * blocks across superframes, whose CRC-16/X.25 over the text after "$$CRCxxxx," (its CR included) equals the four
 * upper-case hex digits (JARL STD 6.0 6.8.1 and Ap2.1). Only that match is transmission evidence; the "$$CRC" literal
 * alone is not. Latitude and longitude are degrees, minutes and hundredths of a minute, read with or without the
 * /HHMMSSh or /DDHHMMz time stamp. The record is bounded and belongs to the call it began in. Driven through
 * processDSTAR_SD() with the real call state, history rows and position feed. Coordinates are synthetic.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <dsd-neo/protocol/dstar/dstar_header.h>
#include <dsd-neo/protocol/dstar/dstar_header_utils.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dstar_confirm.h"
#include "dstar_sd_state.h"

/* The published vector: CRC-16/X.25 of the text after the comma, CR included, is 9E3E. */
static const char k_vector[] = "$$CRC9E3E,N0CALL>API51,DSTAR*:!2230.00N/04500.00E>test\r";

static const uint8_t k_slow_data_scrambler[24] = {
    0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1,
};

static int g_failures;

static void
expect_int(const char* label, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %lld want %lld\n", label, got, want);
        g_failures++;
    }
}

static void
expect_str(const char* label, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", label, got, want);
        g_failures++;
    }
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    Event_History_I* history;
} rig;

static void
rig_open(rig* r) {
    r->opts = (dsd_opts*)calloc(1, sizeof(*r->opts));
    r->state = (dsd_state*)calloc(1, sizeof(*r->state));
    r->history = (Event_History_I*)calloc(2U, sizeof(*r->history));
    if (!r->opts || !r->state || !r->history) {
        DSD_FPRINTF(stderr, "alloc failed\n");
        exit(1);
    }
    for (uint8_t slot = 0U; slot < 2U; slot++) {
        init_event_history(&r->history[slot], 0, 255);
    }
    r->state->event_history_s = r->history;
    r->state->synctype = DSD_SYNC_DSTAR_VOICE_POS;
    expect_int("feed starts", dsd_position_feed_start(r->state), 0);
}

static void
rig_close(rig* r) {
    dsd_state_ext_free_all(r->state);
    free(r->history);
    free(r->state);
    free(r->opts);
}

/* Whether a D-PRS record is being collected (the D-STAR slow-data state extension, created empty on first use). */
static int
dprs_collecting(const rig* r) {
    const dstar_sd_state* sd = dstar_sd_st(r->state);
    return sd != NULL && sd->dprs.collecting != 0U;
}

/* Open a D-STAR voice call as a decoded RF header leaves it (dstar_header_decode_soft() hands its fields to
 * dstar_header_observe()): only a header whose CRC passed names a callsign, and it names the transmission's call. */
static void
open_call(rig* r, const char* mycall) {
    char my[13];
    DSD_SNPRINTF(my, sizeof(my), "%-12s", mycall);
    dstar_header_observe(r->state, DSD_SYNC_DSTAR_VOICE_POS, 0, "RPT2TST", "RPT1TST", "CQCQCQ", my);
    dsd_event_sync_slot(r->opts, r->state, 0U);
}

/* A late-entry call: the transmission confirmed with no header decoded, so the call opens with no identity. */
static void
open_anonymous_call(rig* r) {
    dsd_call_observation observation;
    DSD_MEMSET(&observation, 0, sizeof(observation));
    observation.protocol = DSD_SYNC_DSTAR_VOICE_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_VOICE;
    expect_int("anonymous call opens", dsd_call_state_observe(r->state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
    dsd_event_sync_slot(r->opts, r->state, 0U);
}

static int
feed_records(const rig* r, dsd_position_fix* out, size_t cap) {
    dsd_position_feed_meta meta;
    DSD_MEMSET(out, 0, cap * sizeof(*out));
    return dsd_position_feed_copy_since(r->state, 0U, out, cap, &meta);
}

static uint16_t
x25(const char* text, size_t len) {
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0U; i < len; i++) {
        crc ^= (uint8_t)text[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0x8408U) : (uint16_t)(crc >> 1);
        }
    }
    return (uint16_t)~crc;
}

/* A record for @p body (the text after the comma, its CR included) with its correct CRC. */
static void
record_for(char* out, size_t cap, const char* body) {
    DSD_SNPRINTF(out, cap, "$$CRC%04X,%s", (unsigned)x25(body, strlen(body)), body);
}

/* The published vector's position, sent by @p from (its APRS source, -SSID included when given), its CRC correct. */
static void
record_from(char* out, size_t cap, const char* from) {
    char body[96];
    DSD_SNPRINTF(body, sizeof(body), "%s>API51,DSTAR*:!2230.00N/04500.00E>test\r", from);
    record_for(out, cap, body);
}

/* The superframe's 60 bytes, scrambled and bit-ordered as processDSTAR_SD() reads them. */
static void
pack_slow_data(const uint8_t bytes[60], uint8_t bits[480]) {
    DSD_MEMSET(bits, 0, 480);
    for (int byte_idx = 0; byte_idx < 60; byte_idx++) {
        const uint8_t byte = bytes[59 - byte_idx];
        for (int bit = 0; bit < 8; bit++) {
            const int input_pos = 479 - ((byte_idx * 8) + bit);
            bits[input_pos] = (uint8_t)(((byte >> (7 - bit)) & 1U) ^ k_slow_data_scrambler[input_pos % 24]);
        }
    }
}

/* Slow data as a radio sends it: GPS blocks of up to five bytes, superframe after superframe, the last block short
 * and the rest of its superframe filler. @p skip_blocks filler blocks first, so the record starts mid-superframe. */
static void
send_gps(rig* r, const char* text, size_t len, int skip_blocks) {
    uint8_t bytes[60];
    uint8_t bits[480];
    size_t at = 0U;
    int block = skip_blocks;
    DSD_MEMSET(bytes, 0x66, sizeof(bytes));
    while (at < len) {
        const size_t n = (len - at) < 5U ? (len - at) : 5U;
        uint8_t* b = bytes + ((size_t)block * 6U);
        b[0] = (uint8_t)(0x30U | n);
        DSD_MEMCPY(b + 1, text + at, n);
        at += n;
        block++;
        if (block == 10 || at == len) {
            pack_slow_data(bytes, bits);
            processDSTAR_SD(r->opts, r->state, bits);
            DSD_MEMSET(bytes, 0x66, sizeof(bytes));
            block = 0;
        }
    }
}

/* The published vector, split over two superframes inside a call: a matching CRC is the position, attributed to the
 * call's checked source, and proves the transmission. */
static void
test_vector_position(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0CALL  /TST");
    expect_int("not confirmed before the record", r.state->dstar_confirmed, 0);
    send_gps(&r, k_vector, strlen(k_vector), 0);
    expect_int("matching CRC confirms the transmission", r.state->dstar_confirmed, 1);

    dsd_position_fix f[2];
    expect_int("matching CRC admits one position", feed_records(&r, f, 2U), 1);
    expect_int("kind", f[0].kind, DSD_POS_DSTAR_DPRS);
    expect_int("protocol", f[0].protocol, DSD_SYNC_DSTAR_VOICE_POS);
    expect_int("latitude 22 30.00 N", f[0].lat_e7, 225000000);
    expect_int("longitude 045 00.00 E", f[0].lon_e7, 450000000);
    expect_str("source is the header's callsign", f[0].source_text, "N0CALL /TST");
    expect_int("source checked", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED, 0);
    expect_int("no time stamp", (long long)f[0].fix_time_s, 0);
    expect_str("display: hundredths of a minute", r.state->dstar_gps, "APRS - Lat: 22d 30.00m N Lon: 045d 00.00m E");

    const Event_History* row = &r.history[0].Event_History_Items[0];
    expect_int("call row takes the position", row->pos.valid, 1);
    expect_str("call row text", row->gps_s, r.state->dstar_gps);
    rig_close(&r);
}

/* Issue #800 (g62-r9): the slow-data assemblies live in the D-STAR protocol's state extension, not in public dsd_state:
 * a state that never heard D-STAR slow data holds none, the first superframe creates them, and the state's extension
 * cleanup frees them, so the next run's first superframe starts from empty assemblies. */
static void
test_assemblies_in_protocol_extension(void) {
    rig r;
    rig_open(&r);
    expect_int("no slow data, no extension", dsd_state_ext_get(r.state, DSD_STATE_EXT_PROTO_DSTAR_SLOW_DATA) != NULL,
               0);
    open_call(&r, "N0CALL");
    send_gps(&r, k_vector, 25U, 0);
    expect_int("slow data creates the extension",
               dsd_state_ext_get(r.state, DSD_STATE_EXT_PROTO_DSTAR_SLOW_DATA) != NULL, 1);
    expect_int("record collected there", dprs_collecting(&r), 1);
    dsd_state_ext_free_all(r.state);
    expect_int("extension freed with the state's extensions",
               dsd_state_ext_get(r.state, DSD_STATE_EXT_PROTO_DSTAR_SLOW_DATA) != NULL, 0);
    expect_int("feed restarts", dsd_position_feed_start(r.state), 0);
    send_gps(&r, k_vector + 25U, strlen(k_vector) - 25U, 0);
    dsd_position_fix f[2];
    expect_int("the last run's half record completes nothing", feed_records(&r, f, 2U), 0);
    rig_close(&r);
}

/* Issue #800 (g62-r9 review): N0OLD's call is ACTIVE and named (its terminator and the next transmission's RF header
 * missed, no slow-data header heard since). A CRC-valid record whose own APRS source is N0NEW is not N0OLD's: it
 * reaches the feed anonymous and no row. One whose source is N0OLD with an SSID is N0OLD's. */
static void
test_record_from_another_station_is_not_the_calls(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0OLD");
    char record[128];
    record_from(record, sizeof(record), "N0NEW");
    send_gps(&r, record, strlen(record), 0);
    dsd_position_fix f[2];
    expect_int("another station's record admitted", feed_records(&r, f, 2U), 1);
    expect_str("another station's record names nobody", f[0].source_text, "");
    expect_int("another station's record is unverified", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    expect_int("N0OLD's row takes no position", r.history[0].Event_History_Items[0].pos.valid, 0);
    rig_close(&r);

    rig_open(&r);
    open_call(&r, "N0OLD");
    record_from(record, sizeof(record), "N0OLD-7");
    send_gps(&r, record, strlen(record), 0);
    expect_int("the caller's record admitted", feed_records(&r, f, 2U), 1);
    expect_str("the caller's record (SSID aside) is N0OLD's", f[0].source_text, "N0OLD");
    expect_int("N0OLD's row takes the position", r.history[0].Event_History_Items[0].pos.valid, 1);
    rig_close(&r);
}

/* One changed character, or a CRC written without the CR: no position, no evidence. */
static void
test_crc_mismatch(void) {
    char bad[sizeof(k_vector)];
    DSD_MEMCPY(bad, k_vector, sizeof(k_vector));
    bad[sizeof(k_vector) - 3U] = 'T'; /* "test" -> "tesT" */
    /* 28B0 is the CRC of the text without its CR. */
    static const char no_cr[] = "$$CRC28B0,N0CALL>API51,DSTAR*:!2230.00N/04500.00E>test\r";
    const char* records[2] = {bad, no_cr};
    for (int i = 0; i < 2; i++) {
        rig r;
        rig_open(&r);
        open_call(&r, "N0CALL");
        const uint64_t rev = dsd_position_last_rev();
        send_gps(&r, records[i], strlen(records[i]), 3);
        expect_int("mismatch is no evidence", r.state->dstar_confirmed, 0);
        expect_int("mismatch admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
        expect_int("mismatch writes no row position", r.history[0].Event_History_Items[0].pos.valid, 0);
        rig_close(&r);
    }
}

/* The "$$CRC" literal heading a superframe is no evidence by itself (it was, before issue #800). */
static void
test_literal_alone_is_no_evidence(void) {
    rig r;
    rig_open(&r);
    send_gps(&r, "$$CRC1234,N0CALL>API51,DSTAR*:!22", 33U, 0);
    expect_int("literal alone does not confirm", r.state->dstar_confirmed, 0);
    expect_int("record still being collected", dprs_collecting(&r), 1);
    rig_close(&r);
}

/* Both time stamp forms; south and west. */
static void
test_time_stamps(void) {
    static const struct {
        const char* body;
        int32_t lat_e7;
        int32_t lon_e7;
        const char* display;
    } cases[] = {
        {"N0CALL>API51,DSTAR*:/072130h2230.00N/04500.00E>test\r", 225000000, 450000000,
         "APRS - Lat: 22d 30.00m N Lon: 045d 00.00m E 07:21:30Z"},
        {"N0CALL>API51,DSTAR*:/010710z1000.50S/02000.25W>test\r", -100083333, -200041667,
         "APRS - Lat: 10d 00.50m S Lon: 020d 00.25m W D01 07:10Z"},
        /* No time from the receiver: the transmitter fills the digits with '_'; the position stays. */
        {"N0CALL>API51,DSTAR*:/______z1000.00N/02000.00E>test\r", 100000000, 200000000,
         "APRS - Lat: 10d 00.00m N Lon: 020d 00.00m E"},
        /* Hour 24 is no time: unknown, never wrapped. */
        {"N0CALL>API51,DSTAR*:/240000h1000.00N/02000.00E>test\r", 100000000, 200000000,
         "APRS - Lat: 10d 00.00m N Lon: 020d 00.00m E"},
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rig r;
        rig_open(&r);
        open_call(&r, "N0CALL");
        char record[128];
        record_for(record, sizeof(record), cases[i].body);
        send_gps(&r, record, strlen(record), 1);
        dsd_position_fix f[2];
        expect_int("time stamp case admitted", feed_records(&r, f, 2U), 1);
        expect_int("time stamp case latitude", f[0].lat_e7, cases[i].lat_e7);
        expect_int("time stamp case longitude", f[0].lon_e7, cases[i].lon_e7);
        /* No month or year (DDHHMMz) or no date at all (HHMMSSh): the fix time stays unknown (report decision 7). */
        expect_int("time stamp case has no full fix time", (long long)f[0].fix_time_s, 0);
        expect_str("time stamp case display", r.state->dstar_gps, cases[i].display);
        rig_close(&r);
    }
}

/* Components outside their range are no position (the CRC still proves the transmission). */
static void
test_out_of_range_components(void) {
    static const char* bodies[] = {
        "N0CALL>API51,DSTAR*:!2260.00N/04500.00E>test\r", /* 60 minutes */
        "N0CALL>API51,DSTAR*:!9000.01N/04500.00E>test\r", /* past 90 degrees */
        "N0CALL>API51,DSTAR*:!2230.00N/18030.00E>test\r", /* past 180 degrees */
        "N0CALL>API51,DSTAR*:!2230.00X/04500.00E>test\r", /* no hemisphere */
        "N0CALL>API51,DSTAR*:!22 0.  N/045 0.  E>test\r", /* ambiguity spaces: not read */
        "N0CALL>API51,DSTAR*:>status text only\r",        /* no position at all */
    };
    for (size_t i = 0U; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        rig r;
        rig_open(&r);
        open_call(&r, "N0CALL");
        char record[128];
        record_for(record, sizeof(record), bodies[i]);
        const uint64_t rev = dsd_position_last_rev();
        send_gps(&r, record, strlen(record), 0);
        expect_int("out-of-range record still proves the transmission", r.state->dstar_confirmed, 1);
        expect_int("out-of-range record admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
        rig_close(&r);
    }
    /* The boundary values themselves are positions. */
    rig r;
    rig_open(&r);
    open_call(&r, "N0CALL");
    char record[128];
    record_for(record, sizeof(record), "N0CALL>API51,DSTAR*:!9000.00S/18000.00W>test\r");
    send_gps(&r, record, strlen(record), 0);
    dsd_position_fix f[2];
    expect_int("boundary record admitted", feed_records(&r, f, 2U), 1);
    expect_int("boundary latitude", f[0].lat_e7, -900000000);
    expect_int("boundary longitude", f[0].lon_e7, -1800000000);
    rig_close(&r);
}

/* A record that never ends is dropped at the bound; the next one decodes. */
static void
test_bounded_record(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0CALL");
    char longrec[400];
    DSD_MEMSET(longrec, 'x', sizeof(longrec));
    DSD_MEMCPY(longrec, "$$CRC0000,", 10U);
    send_gps(&r, longrec, sizeof(longrec), 0);
    expect_int("over-long record dropped", dprs_collecting(&r), 0);
    send_gps(&r, k_vector, strlen(k_vector), 0);
    dsd_position_fix f[2];
    expect_int("next record decodes", feed_records(&r, f, 2U), 1);
    rig_close(&r);
}

/* A record belongs to the call it began in: one cut by a new call gives nothing; one heard with no call open is a
 * position with no checked source, which no named call's row takes. */
static void
test_record_bound_to_its_call(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0CALL");
    const size_t half = 25U;
    send_gps(&r, k_vector, half, 0);
    expect_int("record half collected", dprs_collecting(&r), 1);
    open_call(&r, "N1CALL");
    const uint64_t rev = dsd_position_last_rev();
    send_gps(&r, k_vector + half, strlen(k_vector) - half, 0);
    expect_int("record cut by a new call admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
    rig_close(&r);

    rig_open(&r);
    send_gps(&r, k_vector, strlen(k_vector), 0);
    dsd_position_fix f[2];
    expect_int("record with no call admitted", feed_records(&r, f, 2U), 1);
    expect_int("record with no call is unverified", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    expect_str("record with no call names nobody", f[0].source_text, "");
    rig_close(&r);

    /* The carrier's end forgets a half-collected record. */
    rig_open(&r);
    open_call(&r, "N0CALL");
    send_gps(&r, k_vector, half, 0);
    dstar_confirm_reset(r.state);
    expect_int("carrier end clears the record", dprs_collecting(&r), 0);
    rig_close(&r);
}

/* One superframe of 60 slow-data bytes as given. */
static void
send_superframe(rig* r, const uint8_t bytes[60]) {
    uint8_t bits[480];
    pack_slow_data(bytes, bits);
    processDSTAR_SD(r->opts, r->state, bits);
}

/* Issue #800 (report item 10): a superframe that opens with a full GPS block (type 3) that is no "$$CRC" -- an NMEA
 * sentence or a D-PRS record's continuation -- is GPS data, never the transmission's text message (type 4): neither
 * the display text nor the history row's text takes its unchecked position text. */
static void
test_gps_block_is_never_a_text_message(void) {
    static const char* firsts[] = {"$GPGGA,092725.00,2230.00000,N,04500.00000,E,1", "N/04500.00E>test continuation"};
    for (size_t i = 0U; i < sizeof(firsts) / sizeof(firsts[0]); i++) {
        rig r;
        rig_open(&r);
        open_call(&r, "N0CALL");
        DSD_SNPRINTF(r.state->dstar_txt, sizeof(r.state->dstar_txt), "%s", "EARLIER TEXT");
        const uint64_t revision = r.history[0].revision;
        uint8_t bytes[60];
        DSD_MEMSET(bytes, 0x66, sizeof(bytes));
        const char* text = firsts[i];
        const size_t len = strlen(text);
        for (size_t b = 0U; b < 10U && (b * 5U) < len; b++) {
            const size_t n = (len - (b * 5U)) < 5U ? (len - (b * 5U)) : 5U;
            bytes[b * 6U] = (uint8_t)(0x30U | n);
            DSD_MEMCPY(bytes + (b * 6U) + 1U, text + (b * 5U), n);
        }
        send_superframe(&r, bytes);
        expect_str("GPS block leaves the display text", r.state->dstar_txt, "EARLIER TEXT");
        expect_str("GPS block is no row text", r.history[0].Event_History_Items[0].text_message, "");
        expect_int("GPS block does not touch the row", (long long)(r.history[0].revision - revision), 0);
        rig_close(&r);
    }
}

/* "$$CRC" straddling two GPS blocks is found; the record decodes. */
static void
test_tag_straddles_blocks(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0CALL");
    char text[128];
    DSD_SNPRINTF(text, sizeof(text), "ab%s", k_vector);
    send_gps(&r, text, strlen(text), 2);
    dsd_position_fix f[2];
    expect_int("straddling tag: record decodes", feed_records(&r, f, 2U), 1);
    rig_close(&r);
}

/* What the record's own format refuses before its position is read: a block that claims more than five bytes (the
 * record cannot be whole), no comma after the four digits, and lower-case hex. */
static void
test_malformed_records(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0CALL");
    const size_t half = 25U;
    send_gps(&r, k_vector, half, 0);
    uint8_t bytes[60];
    DSD_MEMSET(bytes, 0x66, sizeof(bytes));
    bytes[0] = 0x36U; /* six data bytes: no such block */
    send_superframe(&r, bytes);
    expect_int("oversized block drops the record", dprs_collecting(&r), 0);
    uint64_t rev = dsd_position_last_rev();
    send_gps(&r, k_vector + half, strlen(k_vector) - half, 0);
    expect_int("dropped record admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
    rig_close(&r);

    static const char no_comma[] = "$$CRC9E3E;N0CALL>API51,DSTAR*:!2230.00N/04500.00E>test\r";
    static const char lower_hex[] = "$$CRC9e3e,N0CALL>API51,DSTAR*:!2230.00N/04500.00E>test\r";
    const char* records[2] = {no_comma, lower_hex};
    for (int i = 0; i < 2; i++) {
        rig_open(&r);
        open_call(&r, "N0CALL");
        rev = dsd_position_last_rev();
        send_gps(&r, records[i], strlen(records[i]), 0);
        expect_int("malformed record is no evidence", r.state->dstar_confirmed, 0);
        expect_int("malformed record admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
        rig_close(&r);
    }
}

/* A record whose CR was lost gives way to the next "$$CRC": only the lost one is gone. */
static void
test_lost_cr_costs_only_its_record(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0CALL");
    char text[160];
    const size_t without_cr = sizeof(k_vector) - 2U;
    DSD_MEMCPY(text, k_vector, without_cr);
    DSD_MEMCPY(text + without_cr, k_vector, sizeof(k_vector));
    send_gps(&r, text, without_cr + sizeof(k_vector) - 1U, 0);
    dsd_position_fix f[2];
    expect_int("record after a lost CR decodes", feed_records(&r, f, 2U), 1);
    expect_int("record after a lost CR is the transmission's", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED, 0);
    rig_close(&r);
}

/* A record that lost its CR runs into the bound while the next record's "$$CRC" is arriving: whatever part of the tag
 * the long record already held, or none, the next record still decodes; only the long one is lost. */
static void
test_tag_across_the_bound(void) {
    for (size_t at = DSTAR_DPRS_RECORD_MAX - 4U; at <= DSTAR_DPRS_RECORD_MAX; at++) {
        rig r;
        rig_open(&r);
        open_call(&r, "N0CALL");
        char text[DSTAR_DPRS_RECORD_MAX + sizeof(k_vector)];
        DSD_MEMSET(text, 'x', sizeof(text));
        DSD_MEMCPY(text, "$$CRC0000,", 10U);
        DSD_MEMCPY(text + at, k_vector, sizeof(k_vector) - 1U);
        send_gps(&r, text, at + sizeof(k_vector) - 1U, 0);
        dsd_position_fix f[2];
        if (feed_records(&r, f, 2U) != 1) {
            DSD_FPRINTF(stderr, "tag at %zu across the bound: no record\n", at);
            g_failures++;
        }
        rig_close(&r);
    }
}

/* Late entry: a record begun before any call was active, whose (anonymous) call opens mid-record, is kept. Its CRC
 * proves the transmission; its position names no checked source and reaches no row: no checked header of its
 * transmission named the call. A header that names the call mid-record changes what the record is bound to: the record
 * is dropped, never credited to the call it did not begin in. */
static void
test_late_entry_record_kept_unverified(void) {
    rig r;
    rig_open(&r);
    const size_t half = 25U;
    send_gps(&r, k_vector, half, 0);
    open_anonymous_call(&r);
    send_gps(&r, k_vector + half, strlen(k_vector) - half, 0);
    expect_int("late-entry record proves the transmission", r.state->dstar_confirmed, 1);
    dsd_position_fix f[2];
    expect_int("late-entry record admitted", feed_records(&r, f, 2U), 1);
    expect_int("late-entry record is unverified", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    expect_int("late-entry record not on the anonymous row", r.history[0].Event_History_Items[0].pos.valid, 0);
    rig_close(&r);

    rig_open(&r);
    send_gps(&r, k_vector, half, 0);
    open_call(&r, "N0CALL");
    const uint64_t rev = dsd_position_last_rev();
    send_gps(&r, k_vector + half, strlen(k_vector) - half, 0);
    expect_int("record cut by the naming header proves nothing", r.state->dstar_confirmed, 0);
    expect_int("record cut by the naming header admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
    rig_close(&r);
}

/* A slow-data superframe carrying the header naming @p mycall with the first flag byte @p flag1, its CRC-16/X.25
 * correct: the header as it is repeated through a transmission (nine blocks, each a mini-header 0x55 and five bytes),
 * the tenth block filler. */
static void
build_slow_data_header(uint8_t bytes[60], const char* mycall, uint8_t flag1) {
    uint8_t header[45];
    DSD_MEMSET(header, 0x20, sizeof(header));
    header[0] = flag1;
    header[1] = 0x00U;
    header[2] = 0x00U;
    DSD_MEMCPY(header + 3, "RPT2TST ", 8);
    DSD_MEMCPY(header + 11, "RPT1TST ", 8);
    DSD_MEMCPY(header + 19, "CQCQCQ  ", 8);
    char source[13];
    DSD_SNPRINTF(source, sizeof(source), "%-12s", mycall);
    DSD_MEMCPY(header + 27, source, 12);
    const uint16_t crc = dstar_crc16(header, 39U);
    header[39] = (uint8_t)(crc >> 8);
    header[40] = (uint8_t)(crc & 0xFFU);
    DSD_MEMSET(bytes, 0x66, 60U);
    size_t at = 0U;
    for (size_t i = 0U; i < 54U; i++) {
        bytes[i] = (i % 6U == 0U) ? 0x55U : header[at++];
    }
}

/* The voice header naming @p mycall, as build_slow_data_header() lays it out. */
static void
send_slow_data_header(rig* r, const char* mycall) {
    uint8_t bytes[60];
    build_slow_data_header(bytes, mycall, 0x00U);
    send_superframe(r, bytes);
}

/* N0ONE's call is ACTIVE (its terminator missed) when another transmission's RF header fails its CRC
 * (processDSTAR_HD() begins a transmission, dstar_tx_begin(): DSTAR_PROCESS). A whole, CRC-valid D-PRS record heard
 * after it is not N0ONE's: it names no one and reaches no row, until a header whose CRC passed names the transmission's
 * MY callsign (here the slow-data header of N0TWO's), whose next record is N0TWO's. A checked header with a blank MY
 * field names no one and leaves the records anonymous. */
static void
test_failed_header_leaves_records_anonymous(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0ONE");
    dstar_tx_begin(r.state);
    send_gps(&r, k_vector, strlen(k_vector), 0);
    dsd_position_fix f[3];
    expect_int("record after a failed header admitted", feed_records(&r, f, 3U), 1);
    expect_str("record after a failed header names nobody", f[0].source_text, "");
    expect_int("record after a failed header is unverified", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    expect_int("record after a failed header is not on N0ONE's row", r.history[0].Event_History_Items[0].pos.valid, 0);
    /* A checked header whose MY field is blank names no one: the call keeps N0ONE's callsign, which is not this
       transmission's, so the next record still names nobody. */
    send_slow_data_header(&r, "");
    expect_int("a blank checked header names no one", (long long)r.state->dstar_tx_call_epoch, 0);
    send_gps(&r, k_vector, strlen(k_vector), 0);
    expect_int("record after a blank header admitted", feed_records(&r, f, 3U), 2);
    expect_str("record after a blank header names nobody", f[1].source_text, "");
    expect_int("record after a blank header is unverified", f[1].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    send_slow_data_header(&r, "N0TWO");
    expect_int("a checked header names the transmission", r.state->dstar_tx_call_epoch != 0U, 1);
    char two[128];
    record_from(two, sizeof(two), "N0TWO");
    send_gps(&r, two, strlen(two), 0);
    expect_int("record after the checked header admitted", feed_records(&r, f, 3U), 3);
    expect_str("record after the checked header is N0TWO's", f[2].source_text, "N0TWO");
    expect_int("record after the checked header is checked", f[2].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED, 0);
    rig_close(&r);
}

/* A superframe holding text blocks: @p count blocks, block i at place @p first + i with five characters of @p text
 * (spaces past its end). */
static void
send_text(rig* r, unsigned first, const char* text, size_t count) {
    uint8_t bytes[60];
    DSD_MEMSET(bytes, 0x66, sizeof(bytes));
    const size_t len = strlen(text);
    for (size_t i = 0U; i < count; i++) {
        bytes[i * 6U] = (uint8_t)(0x40U | (first + i));
        DSD_MEMSET(bytes + (i * 6U) + 1U, ' ', 5U);
        const size_t at = i * 5U;
        if (at < len) {
            DSD_MEMCPY(bytes + (i * 6U) + 1U, text + at, len - at < 5U ? len - at : 5U);
        }
    }
    send_superframe(r, bytes);
}

static const Event_History*
open_row(const rig* r) {
    return &r->history[0].Event_History_Items[0];
}

/* Issue #800 (g62-r5): a record whose bytes all arrived before a retune but whose CR arrived after it was assembled
 * across two receptions. Its CRC passes, but it is checked against the reception it began on before it counts as
 * anything: it neither confirms the new carrier nor reaches the feed. */
static void
test_reception_change_before_cr_proves_nothing(void) {
    rig r;
    rig_open(&r);
    open_call(&r, "N0CALL");
    send_gps(&r, k_vector, strlen(k_vector) - 1U, 0);
    expect_int("record waits for its CR", dprs_collecting(&r), 1);
    dsd_trunk_tuning_generation_advance();
    const uint64_t rev = dsd_position_last_rev();
    send_gps(&r, "\r", 1U, 0);
    expect_int("record across a retune does not confirm the carrier", r.state->dstar_confirmed, 0);
    expect_int("record across a retune admits nothing", (long long)(dsd_position_last_rev() - rev), 0);
    /* The next whole record, heard on the new reception, decodes. */
    send_gps(&r, k_vector, strlen(k_vector), 0);
    expect_int("next record confirms", r.state->dstar_confirmed, 1);
    expect_int("next record admitted", (long long)(dsd_position_last_rev() - rev), 1);
    rig_close(&r);
}

/* Issue #800 (g62-r5): text blocks heard before the transmission was confirmed, or before its header named the call,
 * are no part of the confirmed call's message: an unconfirmed block 0 ("OLDER") followed by a checked header naming
 * N0NEWS and block 1 ("NEWER") puts nothing on N0NEWS's row (the message there lacks its first block). The next whole
 * message heard under the header does. */
static void
test_unconfirmed_text_stays_out_of_confirmed_messages(void) {
    rig r;
    rig_open(&r);
    send_text(&r, 0U, "OLDER", 1U);
    expect_int("not confirmed", r.state->dstar_confirmed, 0);
    send_slow_data_header(&r, "N0NEWS");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    expect_int("header confirms", r.state->dstar_confirmed, 1);
    send_text(&r, 1U, "NEWER", 1U);
    expect_str("message missing its own first block reaches no row", open_row(&r)->text_message, "");
    send_text(&r, 0U, "HELLOWORLD", 2U);
    expect_str("whole message heard under the header", open_row(&r)->text_message, "HELLOWORLD");
    expect_str("row is N0NEWS's", open_row(&r)->src_str, "N0NEWS");
    rig_close(&r);
}

/* Issue #800 (g62-r5): N0OLD's confirmed call is still ACTIVE (its terminator missed) when the next transmission's RF
 * header fails its CRC (processDSTAR_HD() begins a transmission). A fresh, whole text message heard next is that
 * transmission's, and no checked header has named its call: N0OLD's row does not take it. */
static void
test_text_withheld_from_the_previous_transmitters_call(void) {
    rig r;
    rig_open(&r);
    send_slow_data_header(&r, "N0OLD");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    send_text(&r, 0U, "OLD MESSAGE TEXT    ", 4U);
    expect_str("N0OLD's own message", open_row(&r)->text_message, "OLD MESSAGE TEXT");
    dstar_tx_begin(r.state);
    expect_int("still confirmed", r.state->dstar_confirmed, 1);
    send_text(&r, 0U, "NEXT!", 1U);
    expect_str("next transmission's text is not N0OLD's", open_row(&r)->text_message, "OLD MESSAGE TEXT");
    rig_close(&r);
}

/* Issue #800 (g62-r5): a checked slow-data header with a blank MY field and the suffix TEST names no caller: the call
 * stays anonymous and the suffix never becomes its source; the record that follows is anonymous too. */
static void
test_slow_data_suffix_alone_is_no_caller(void) {
    rig r;
    rig_open(&r);
    send_slow_data_header(&r, "        TEST");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    expect_int("header opens a call", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_str("suffix alone is no source", call.source_text, "");
    expect_int("transmission names no call", (long long)r.state->dstar_tx_call_epoch, 0);
    send_gps(&r, k_vector, strlen(k_vector), 0);
    dsd_position_fix f[2];
    expect_int("record admitted", feed_records(&r, f, 2U), 1);
    expect_str("record names nobody", f[0].source_text, "");
    expect_str("row names nobody", open_row(&r)->src_str, "");
    rig_close(&r);
}

/* Another system's frame noted on the carrier between a D-STAR header and its slow data: a P25 control frame's checked
 * NAC (the received identity's epoch moves), then D-STAR sync again. */
static void
note_other_system_frame(rig* r) {
    dsd_rxid_note_sync(r->state, DSD_SYNC_P25P1_POS);
    dsd_rxid_note_access(r->state, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
    dsd_rxid_note_sync(r->state, DSD_SYNC_DSTAR_VOICE_POS);
}

/* Issue #800 (g62-r6): N0OLD's checked header names the call, which stays ACTIVE across another system's frame (a
 * reception change). A whole, CRC-valid D-PRS record and a fresh text message heard after it, with no new header, are
 * not proven N0OLD's: the record reaches the feed anonymous and no row, and the text stays off N0OLD's row. A header
 * heard on the new reception names the call again. */
static void
test_header_bound_to_its_reception(void) {
    rig r;
    rig_open(&r);
    send_slow_data_header(&r, "N0OLD");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    send_text(&r, 0U, "OLD MESSAGE TEXT    ", 4U);
    expect_str("N0OLD's own message", open_row(&r)->text_message, "OLD MESSAGE TEXT");
    const uint64_t named = r.state->dstar_tx_call_epoch;
    expect_int("header names the call", named != 0U, 1);
    note_other_system_frame(&r);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    expect_int("call still open", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_int("call still ACTIVE", call.phase, DSD_CALL_PHASE_ACTIVE);
    expect_int("header naming bound to the old reception",
               dsd_position_origin_matches(r.state, &r.state->dstar_tx_call_origin), 0);

    send_text(&r, 0U, "FRESH MESSAGE TEXT  ", 4U);
    expect_str("text after the reception change is not N0OLD's", open_row(&r)->text_message, "OLD MESSAGE TEXT");

    const uint64_t rev = dsd_position_last_rev();
    send_gps(&r, k_vector, strlen(k_vector), 0);
    expect_int("record admitted", (long long)(dsd_position_last_rev() - rev), 1);
    dsd_position_fix f[2];
    expect_int("one feed record", feed_records(&r, f, 2U), 1);
    expect_str("record after the reception change names nobody", f[0].source_text, "");
    expect_int("record after the reception change is unverified", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    expect_int("N0OLD's row takes no position", open_row(&r)->pos.valid, 0);

    /* A checked header heard on the new reception names its call again. */
    send_slow_data_header(&r, "N0OLD");
    expect_int("header on the new reception names the call",
               dsd_position_origin_matches(r.state, &r.state->dstar_tx_call_origin), 1);
    send_text(&r, 0U, "NEW MESSAGE TEXT    ", 4U);
    expect_str("text under the new header reaches the row", open_row(&r)->text_message, "NEW MESSAGE TEXT");
    rig_close(&r);
}

/* Issue #800 (g62-r7): in Auto mode N0OLD's terminator is missed and its call stays ACTIVE while an M17 BERT frame
 * intervenes. Neither D-STAR nor BERT notes any identity, yet the M17 frame is another reception: when D-STAR resumes
 * with no new header, a whole, CRC-valid D-PRS record is anonymous (feed only, no row) and a fresh text message stays
 * off N0OLD's row. */
static void
test_header_bound_across_a_protocol_switch(void) {
    rig r;
    rig_open(&r);
    dsd_rxid_note_sync(r.state, DSD_SYNC_DSTAR_VOICE_POS);
    send_slow_data_header(&r, "N0OLD");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    send_text(&r, 0U, "OLD MESSAGE TEXT    ", 4U);
    expect_str("N0OLD's own message", open_row(&r)->text_message, "OLD MESSAGE TEXT");
    expect_int("header names the call", r.state->dstar_tx_call_epoch != 0U, 1);

    dsd_rxid_note_sync(r.state, DSD_SYNC_M17_BRT_POS);
    dsd_rxid_note_sync(r.state, DSD_SYNC_DSTAR_VOICE_POS);
    expect_int("no identity noted across the switch", r.state->rx_id.family, DSD_RXID_FAMILY_NONE);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    expect_int("call still open", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_int("call still ACTIVE", call.phase, DSD_CALL_PHASE_ACTIVE);
    expect_int("header naming bound to the protocol it was heard under",
               dsd_position_origin_matches(r.state, &r.state->dstar_tx_call_origin), 0);

    send_text(&r, 0U, "FRESH MESSAGE TEXT  ", 4U);
    expect_str("text after the protocol switch is not N0OLD's", open_row(&r)->text_message, "OLD MESSAGE TEXT");
    send_gps(&r, k_vector, strlen(k_vector), 0);
    dsd_position_fix f[2];
    expect_int("record admitted", feed_records(&r, f, 2U), 1);
    expect_str("record after the protocol switch names nobody", f[0].source_text, "");
    expect_int("record after the protocol switch is unverified", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    expect_int("N0OLD's row takes no position", open_row(&r)->pos.valid, 0);
    rig_close(&r);
}

/* Issue #800 (g62-r7): a new transmission whose first text block was missed starts its message empty. N0ONE's
 * "ALICE MOBI" is shown; the next transmission's block 1 ("NEWER") shows alone at its place, never with the previous
 * caller's characters around it ("ALICENEWER"), and reaches no row (its first block was not heard). */
static void
test_new_message_starts_empty(void) {
    rig r;
    rig_open(&r);
    send_slow_data_header(&r, "N0ONE");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    send_text(&r, 0U, "ALICE MOBI", 2U);
    expect_str("first caller's message shown", r.state->dstar_txt, "ALICE MOBI");
    dstar_tx_begin(r.state);
    send_text(&r, 1U, "NEWER", 1U);
    expect_str("new transmission's block alone", r.state->dstar_txt, "     NEWER");
    expect_str("row keeps the first caller's message", open_row(&r)->text_message, "ALICE MOBI");
    rig_close(&r);
}

/* Issue #800 (g62-r8): a CRC-valid slow-data header whose first flag byte has the DATA bit set (0x80) is a data
 * transmission's: it names no voice call. The DATA bit is read from the bytes the header CRC covers -- the display
 * copy shows that byte as a space. A late-entry voice call open on the slot keeps no source and its row no position:
 * the D-PRS record heard after the header reaches the feed anonymous. */
static void
test_data_header_names_no_voice_call(void) {
    rig r;
    rig_open(&r);
    open_anonymous_call(&r);
    uint8_t bytes[60];
    build_slow_data_header(bytes, "N0DATA", 0x80U);
    send_superframe(&r, bytes);
    expect_int("data header names no call", (long long)r.state->dstar_tx_call_epoch, 0);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    expect_int("call still open", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_str("data header is no voice call's source", call.source_text, "");
    send_gps(&r, k_vector, strlen(k_vector), 0);
    dsd_position_fix f[2];
    expect_int("record admitted", feed_records(&r, f, 2U), 1);
    expect_str("record after a data header names nobody", f[0].source_text, "");
    expect_int("record after a data header is unverified", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    expect_int("voice row takes no position", open_row(&r)->pos.valid, 0);
    rig_close(&r);
}

/* Issue #800 (g62-r9): N0OLD's voice call is ACTIVE and named (its terminator and the next RF header missed) when a
 * CRC-valid slow-data header with the DATA bit set names N0DATA. That header is no voice call's, so it revokes
 * N0OLD's naming without making a new one: a D-PRS record and a whole text message heard after it reach the feed
 * anonymous and no row, and the text message begun under N0OLD's naming before it never completes on N0OLD's row. */
static void
test_data_header_revokes_the_voice_naming(void) {
    rig r;
    rig_open(&r);
    send_slow_data_header(&r, "N0OLD");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    expect_int("voice header names the call", r.state->dstar_tx_call_epoch != 0U, 1);
    send_text(&r, 0U, "OLD MESSAGE TEXT    ", 4U);
    expect_str("N0OLD's own message", open_row(&r)->text_message, "OLD MESSAGE TEXT");
    send_text(&r, 0U, "PART1", 1U);
    expect_str("first block of N0OLD's next message", open_row(&r)->text_message, "PART1");

    uint8_t bytes[60];
    build_slow_data_header(bytes, "N0DATA", 0x80U);
    send_superframe(&r, bytes);
    expect_int("data header revokes the voice naming", (long long)r.state->dstar_tx_call_epoch, 0);
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    expect_int("call still open", dsd_call_state_get(r.state, 0U, &call), 1);
    expect_str("data header changes no voice call's source", call.source_text, "N0OLD");

    send_text(&r, 1U, " PART2 PART3", 3U);
    expect_str("message begun before the data header completes on no row", open_row(&r)->text_message, "PART1");
    send_text(&r, 0U, "NEW DATA MESSAGE    ", 4U);
    expect_str("message after the data header stays off the row", open_row(&r)->text_message, "PART1");

    send_gps(&r, k_vector, strlen(k_vector), 0);
    dsd_position_fix f[2];
    expect_int("record admitted", feed_records(&r, f, 2U), 1);
    expect_str("record after the data header names nobody", f[0].source_text, "");
    expect_int("record after the data header is unverified", f[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED,
               DSD_POS_FLAG_SOURCE_UNVERIFIED);
    expect_int("N0OLD's row takes no position", open_row(&r)->pos.valid, 0);
    rig_close(&r);
}

/* Issue #800 (g62-r9 review): a checked DATA header takes back only what an earlier header named. In a data-flagged
 * transmission nothing is named, so an anonymous D-PRS record whose bytes straddle the next DATA header superframe
 * completes and reaches the feed (anonymous), as it does across a blank-MY voice header. */
static void
test_data_header_keeps_an_anonymous_record(void) {
    rig r;
    rig_open(&r);
    uint8_t bytes[60];
    build_slow_data_header(bytes, "N0DATA", 0x80U);
    send_superframe(&r, bytes);
    expect_int("data header confirms", r.state->dstar_confirmed, 1);
    const size_t half = 25U;
    send_gps(&r, k_vector, half, 0);
    send_superframe(&r, bytes);
    send_gps(&r, k_vector + half, strlen(k_vector) - half, 0);
    dsd_position_fix f[2];
    expect_int("record across the data header admitted", feed_records(&r, f, 2U), 1);
    expect_str("record across the data header names nobody", f[0].source_text, "");
    rig_close(&r);
}

/* Issue #800 (g62-r8): a rebroadcast header fills nine of a superframe's ten blocks; the tenth may be a text block.
 * It is read like any other text block: the message's first block ("HELLO"), heard after the header that names
 * N0TEXT, starts the message, and with the rest of it the whole message reaches N0TEXT's row. */
static void
test_text_block_after_a_header(void) {
    rig r;
    rig_open(&r);
    send_slow_data_header(&r, "N0TEXT");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    uint8_t bytes[60];
    build_slow_data_header(bytes, "N0TEXT", 0x00U);
    bytes[54] = 0x40U;
    DSD_MEMCPY(bytes + 55, "HELLO", 5U);
    send_superframe(&r, bytes);
    expect_str("text block after the header shown", r.state->dstar_txt, "HELLO");
    send_text(&r, 1U, " WORLD TEST MSG", 3U);
    expect_str("whole message shown", r.state->dstar_txt, "HELLO WORLD TEST MSG");
    expect_str("whole message on the row", open_row(&r)->text_message, "HELLO WORLD TEST MSG");
    rig_close(&r);
}

/* Issue #800 (g62-r9): the header CRC covers a rebroadcast header's octets, not its nine blocks' mini-headers. A header
 * block whose mini-header arrives as a text block's (0x41) or a GPS block's (0x35) in an otherwise valid header still
 * holds header octets: they are never text or GPS data. N0TEXT's message "HELLO" stays "HELLO" on the row (no repeater
 * callsign appended), and a D-PRS record whose bytes straddle the header superframe completes and is N0TEXT's. */
static void
test_header_blocks_are_never_text_or_gps(void) {
    rig r;
    rig_open(&r);
    send_slow_data_header(&r, "N0TEXT");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    send_text(&r, 0U, "HELLO", 1U);
    expect_str("first block on the row", open_row(&r)->text_message, "HELLO");
    uint8_t bytes[60];
    build_slow_data_header(bytes, "N0TEXT", 0x00U);
    bytes[18] = 0x41U; /* block 3's mini-header */
    send_superframe(&r, bytes);
    expect_str("header octets are no text", r.state->dstar_txt, "HELLO");
    expect_str("row keeps its message", open_row(&r)->text_message, "HELLO");

    const size_t half = 25U;
    char record[128];
    record_from(record, sizeof(record), "N0TEXT");
    send_gps(&r, record, half, 0);
    build_slow_data_header(bytes, "N0TEXT", 0x00U);
    bytes[24] = 0x35U; /* block 4's mini-header */
    send_superframe(&r, bytes);
    send_gps(&r, record + half, strlen(record) - half, 0);
    dsd_position_fix f[2];
    expect_int("record across the header superframe admitted", feed_records(&r, f, 2U), 1);
    expect_str("record is N0TEXT's", f[0].source_text, "N0TEXT");
    expect_int("N0TEXT's row takes the position", open_row(&r)->pos.valid, 1);
    rig_close(&r);
}

/* Issue #800 (g62-r8): the next message's first block is missed within the same transmission. Its later blocks start
 * a new message (a place at or before one already heard since the last block 0): none of the previous message's
 * characters is shown with them, and the row keeps the previous whole message. */
static void
test_missed_first_block_starts_a_new_message(void) {
    rig r;
    rig_open(&r);
    send_slow_data_header(&r, "N0TEXT");
    dsd_event_sync_slot(r.opts, r.state, 0U);
    send_text(&r, 0U, "FIRST MESSAGE TEXT  ", 4U);
    expect_str("first message on the row", open_row(&r)->text_message, "FIRST MESSAGE TEXT");
    send_text(&r, 1U, "D MESSAGE TEXT ", 3U);
    expect_str("next message's blocks alone", r.state->dstar_txt, "     D MESSAGE TEXT");
    expect_str("row keeps the whole first message", open_row(&r)->text_message, "FIRST MESSAGE TEXT");
    /* Out of order too: block 2 after block 3 of one message is the next message's, and its row text stays. */
    send_text(&r, 0U, "THIRD", 1U);
    send_text(&r, 3U, "LATER", 1U);
    char before[sizeof(open_row(&r)->text_message)];
    DSD_SNPRINTF(before, sizeof(before), "%s", open_row(&r)->text_message);
    send_text(&r, 2U, "AGAIN", 1U);
    expect_str("a place before one heard starts afresh", r.state->dstar_txt, "          AGAIN");
    expect_str("the fresh block reaches no row", open_row(&r)->text_message, before);
    rig_close(&r);
}

int
main(void) {
    test_vector_position();
    test_assemblies_in_protocol_extension();
    test_record_from_another_station_is_not_the_calls();
    test_crc_mismatch();
    test_literal_alone_is_no_evidence();
    test_time_stamps();
    test_out_of_range_components();
    test_bounded_record();
    test_record_bound_to_its_call();
    test_gps_block_is_never_a_text_message();
    test_tag_straddles_blocks();
    test_malformed_records();
    test_lost_cr_costs_only_its_record();
    test_tag_across_the_bound();
    test_late_entry_record_kept_unverified();
    test_failed_header_leaves_records_anonymous();
    test_reception_change_before_cr_proves_nothing();
    test_unconfirmed_text_stays_out_of_confirmed_messages();
    test_text_withheld_from_the_previous_transmitters_call();
    test_slow_data_suffix_alone_is_no_caller();
    test_header_bound_to_its_reception();
    test_header_bound_across_a_protocol_switch();
    test_new_message_starts_empty();
    test_data_header_names_no_voice_call();
    test_data_header_revokes_the_voice_naming();
    test_data_header_keeps_an_anonymous_record();
    test_text_block_after_a_header();
    test_missed_first_block_starts_a_new_message();
    test_header_blocks_are_never_text_or_gps();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DSTAR_DPRS: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "DSTAR_DPRS: OK\n");
    return 0;
}
