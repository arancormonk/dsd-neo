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

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/dstar/dstar.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Open a D-STAR voice call as a decoded header leaves it: only a header whose CRC passed names a callsign. */
static void
open_call(rig* r, const char* mycall) {
    dsd_call_observation observation;
    DSD_MEMSET(&observation, 0, sizeof(observation));
    observation.protocol = DSD_SYNC_DSTAR_VOICE_POS;
    observation.slot = 0U;
    observation.kind = DSD_CALL_KIND_VOICE;
    DSD_SNPRINTF(observation.source_text, sizeof(observation.source_text), "%s", mycall);
    DSD_SNPRINTF(observation.target_text, sizeof(observation.target_text), "%s", "CQCQCQ");
    expect_int("call opens", dsd_call_state_observe(r->state, &observation, DSD_CALL_BOUNDARY_BEGIN), 1);
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
        uint8_t* b = bytes + (block * 6);
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
    expect_int("record still being collected", r.state->dstar_dprs.collecting, 1);
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
    expect_int("over-long record dropped", r.state->dstar_dprs.collecting, 0);
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
    expect_int("record half collected", r.state->dstar_dprs.collecting, 1);
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
    expect_int("carrier end clears the record", r.state->dstar_dprs.collecting, 0);
    rig_close(&r);
}

int
main(void) {
    test_vector_position();
    test_crc_mismatch();
    test_literal_alone_is_no_evidence();
    test_time_stamps();
    test_out_of_range_components();
    test_bounded_record();
    test_record_bound_to_its_call();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DSTAR_DPRS: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "DSTAR_DPRS: OK\n");
    return 0;
}
