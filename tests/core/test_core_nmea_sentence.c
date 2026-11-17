// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * ASCII NMEA 0183 position reports (issue #800, 2.4 and the RMC/GGA measured-fix rule in 2.2): each sentence is checked
 * against its own checksum and only checked sentences are kept; an RMC or GGA is a position only when its own status
 * says it is a measured fix; every field is range-checked before it is converted. Driven through the parser, the
 * producer (nmea_sentence_checker()) and the P25 SAP 48 packet path, with the real event layer, feed and -L writer.
 * All coordinates, IDs and dates are made up.
 */

#include <assert.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_pdu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nmea_sentence.h"
#include "test_support.h"

static int g_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* "$" + body + "*HH" with the body's own checksum, and @p tail after it. */
static void
sentence(char* out, size_t cap, const char* body, const char* tail) {
    uint8_t sum = 0U;
    for (const char* p = body; *p != '\0'; p++) {
        sum ^= (uint8_t)*p;
    }
    DSD_SNPRINTF(out, cap, "$%s*%02X%s", body, sum, tail);
}

static nmea_fix
parse_body(const char* body) {
    char s[160];
    sentence(s, sizeof s, body, "");
    nmea_fix f;
    nmea_parse_fix(s, strlen(s), &f);
    return f;
}

/* ---- Framing: one sentence at a time, each with its own checksum ---- */

static void
test_framing(void) {
    /* A literal sentence whose checksum was computed apart from this file's helper. */
    static const char lit[] = "$GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A*7F\r\n";
    size_t pos = 0U;
    nmea_sentence_span s;
    CHECK(nmea_next_sentence((const uint8_t*)lit, strlen(lit), &pos, &s) == 1);
    CHECK(s.status == NMEA_SENTENCE_OK && s.start == 0U && s.len == strlen(lit) - 2U);
    CHECK(nmea_next_sentence((const uint8_t*)lit, strlen(lit), &pos, &s) == 0);

    /* A good sentence, then one whose checksum fails (its coordinates would be out of range), then a good one. */
    char a[96];
    char b[96];
    char c[96];
    char buf[320];
    sentence(a, sizeof a, "GPGGA,120000,2230.0000,N,04500.0000,E,1,05,1.0,10.0,M,,M,,", "\r\n");
    DSD_SNPRINTF(b, sizeof b, "%s", "$GPRMC,120000,A,9930.0000,S,00000.0100,E,,,150626,,*00\r\n");
    sentence(c, sizeof c, "GPTXT,01,01,02,HELLO", "\r\n");
    DSD_SNPRINTF(buf, sizeof buf, "%s%s%s", a, b, c);
    pos = 0U;
    CHECK(nmea_next_sentence((const uint8_t*)buf, strlen(buf), &pos, &s) == 1 && s.status == NMEA_SENTENCE_OK);
    CHECK(nmea_next_sentence((const uint8_t*)buf, strlen(buf), &pos, &s) == 1 && s.status == NMEA_SENTENCE_CHECKSUM);
    CHECK(s.start == strlen(a) && s.received == 0x00U);
    CHECK(nmea_next_sentence((const uint8_t*)buf, strlen(buf), &pos, &s) == 1 && s.status == NMEA_SENTENCE_OK);
    CHECK(s.start == strlen(a) + strlen(b));
    CHECK(nmea_next_sentence((const uint8_t*)buf, strlen(buf), &pos, &s) == 0);

    /* No '*' before the line ends; non-hex checksum digits; NUL padding ends the payload; other bytes stop it. */
    static const uint8_t no_star[] = "$GPRMC,1\r\n$GPTXT*00";
    pos = 0U;
    CHECK(nmea_next_sentence(no_star, sizeof no_star - 1U, &pos, &s) == 1 && s.status == NMEA_SENTENCE_NO_STAR);
    CHECK(nmea_next_sentence(no_star, sizeof no_star - 1U, &pos, &s) == 1 && s.status == NMEA_SENTENCE_CHECKSUM);
    static const uint8_t bad_hex[] = "$GPTXT*G0";
    pos = 0U;
    CHECK(nmea_next_sentence(bad_hex, sizeof bad_hex - 1U, &pos, &s) == 1 && s.status == NMEA_SENTENCE_BAD_HEX);
    CHECK(nmea_next_sentence(bad_hex, sizeof bad_hex - 1U, &pos, &s) == -1);
    uint8_t padded[96];
    assert(strlen(lit) <= sizeof padded);
    DSD_MEMSET(padded, 0, sizeof padded);
    DSD_MEMCPY(padded, lit, strlen(lit));
    pos = 0U;
    CHECK(nmea_next_sentence(padded, sizeof padded, &pos, &s) == 1 && s.status == NMEA_SENTENCE_OK);
    CHECK(nmea_next_sentence(padded, sizeof padded, &pos, &s) == 0);
    static const uint8_t junk[] = "XYZ";
    pos = 0U;
    CHECK(nmea_next_sentence(junk, 3U, &pos, &s) == -1 && pos == 0U);
}

/* ---- RMC and GGA fields ---- */

static void
test_rmc_fields(void) {
    nmea_fix f = parse_body("GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A");
    CHECK(f.type == NMEA_TYPE_RMC && f.measured_fix && f.has_position);
    CHECK(f.lat_e7 == 225000000 && f.lon_e7 == 450000000);
    CHECK(f.speed_dkmh == 185U); /* 10 kn = 18.52 km/h */
    CHECK(f.heading_ddeg == 900U);
    CHECK(f.fix_time_s == 1781524800); /* 2026-06-15 12:00:00 UTC */
    CHECK(f.alt_dm == DSD_POSITION_ALT_UNKNOWN && f.sats == DSD_POSITION_SATS_NONE);
    CHECK(strcmp(f.refusal, "") == 0);

    /* South and west; fraction of a minute kept to 1e-7 degree. */
    f = parse_body("GNRMC,235959.50,A,2230.5000,S,04500.2500,W,,,290224,,");
    CHECK(f.has_position && f.lat_e7 == -225083333 && f.lon_e7 == -450041667);
    CHECK(f.speed_dkmh == DSD_POSITION_SPEED_UNKNOWN && f.heading_ddeg == DSD_POSITION_HEADING_UNKNOWN);
    CHECK(f.fix_time_s == 1709251199); /* 2024-02-29 23:59:59 UTC; no mode field (before NMEA 2.3) */

    /* Course: 359.9 kept, 360.0 and more unknown, never folded. */
    CHECK(parse_body("GPRMC,120000,A,2230.0,N,04500.0,E,0.0,359.9,150626,,,A").heading_ddeg == 3599U);
    CHECK(parse_body("GPRMC,120000,A,2230.0,N,04500.0,E,0.0,360.0,150626,,,A").heading_ddeg
          == DSD_POSITION_HEADING_UNKNOWN);

    /* Any two-letter talker. */
    CHECK(parse_body("GLRMC,120000,A,2230.0,N,04500.0,E,,,150626,,,D").has_position);
    CHECK(parse_body("BDRMC,120000,A,2230.0,N,04500.0,E,,,150626,,,F").has_position);
    CHECK(parse_body("GARMC,120000,A,2230.0,N,04500.0,E,,,150626,,,R").has_position);
    CHECK(parse_body("GBRMC,120000,A,2230.0,N,04500.0,E,,,150626,,,P").has_position);
    CHECK(parse_body("gpRMC,120000,A,2230.0,N,04500.0,E,,,150626,,,A").type == NMEA_TYPE_OTHER);
    CHECK(parse_body("GPRMB,A,0.66,L,003,004").type == NMEA_TYPE_OTHER);

    /* Measured-fix rule: status V, and modes N, E, M, S are no fix. */
    CHECK(!parse_body("GPRMC,120000,V,2230.0,N,04500.0,E,,,150626,,,A").measured_fix);
    static const char* refused_modes[] = {"N", "E", "M", "S", "X"};
    for (size_t i = 0; i < sizeof refused_modes / sizeof refused_modes[0]; i++) {
        char body[96];
        DSD_SNPRINTF(body, sizeof body, "GPRMC,120000,A,2230.0,N,04500.0,E,,,150626,,,%s", refused_modes[i]);
        f = parse_body(body);
        CHECK(!f.measured_fix && !f.has_position && f.refusal[0] != '\0');
    }

    /* An invalid date or time leaves the fix time unknown; the position stays. */
    f = parse_body("GPRMC,120000,A,2230.0,N,04500.0,E,,,310226,,,A"); /* 31 February */
    CHECK(f.has_position && f.fix_time_s == 0);
    CHECK(parse_body("GPRMC,240000,A,2230.0,N,04500.0,E,,,150626,,,A").fix_time_s == 0);
    CHECK(parse_body("GPRMC,126000,A,2230.0,N,04500.0,E,,,150626,,,A").fix_time_s == 0);
    CHECK(parse_body("GPRMC,120060,A,2230.0,N,04500.0,E,,,150626,,,A").fix_time_s == 0);
    CHECK(parse_body("GPRMC,120000,A,2230.0,N,04500.0,E,,,151326,,,A").fix_time_s == 0);
    CHECK(parse_body("GPRMC,120000,A,2230.0,N,04500.0,E,,,290225,,,A").fix_time_s == 0); /* not a leap year */
}

static void
test_coordinate_bounds(void) {
    /* Components are checked before conversion: minutes 60 is no 61st minute, it is invalid. */
    CHECK(!parse_body("GPRMC,120000,A,2260.0,N,04500.0,E,,,150626,,,A").has_position);
    CHECK(!parse_body("GPRMC,120000,A,2230.0,N,04560.0,E,,,150626,,,A").has_position);
    CHECK(!parse_body("GPRMC,120000,A,9100.0,N,04500.0,E,,,150626,,,A").has_position);
    CHECK(!parse_body("GPRMC,120000,A,2230.0,N,18100.0,E,,,150626,,,A").has_position);
    /* The boundary degree only with zero minutes and fraction. */
    nmea_fix f = parse_body("GPRMC,120000,A,9000.0000,S,18000.0000,W,,,150626,,,A");
    CHECK(f.has_position && f.lat_e7 == -900000000 && f.lon_e7 == -1800000000);
    CHECK(!parse_body("GPRMC,120000,A,9000.0001,N,04500.0,E,,,150626,,,A").has_position);
    CHECK(!parse_body("GPRMC,120000,A,2230.0,N,18000.0001,E,,,150626,,,A").has_position);
    /* Field shape: ddmm / dddmm, digits, a hemisphere letter. */
    CHECK(!parse_body("GPRMC,120000,A,230.0,N,04500.0,E,,,150626,,,A").has_position);
    CHECK(!parse_body("GPRMC,120000,A,2230.0,N,4500.0,E,,,150626,,,A").has_position);
    CHECK(!parse_body("GPRMC,120000,A,2230.0,X,04500.0,E,,,150626,,,A").has_position);
    CHECK(!parse_body("GPRMC,120000,A,22a0.0,N,04500.0,E,,,150626,,,A").has_position);
    CHECK(!parse_body("GPRMC,120000,A,,,,,,,150626,,,A").has_position);
}

static void
test_gga_fields(void) {
    nmea_fix f = parse_body("GNGGA,120000.00,2230.0000,S,04500.0000,W,1,08,0.9,123.4,M,0.0,M,,");
    CHECK(f.type == NMEA_TYPE_GGA && f.measured_fix && f.has_position);
    CHECK(f.lat_e7 == -225000000 && f.lon_e7 == -450000000);
    CHECK(f.alt_dm == 1234 && f.sats == 8U);
    CHECK(f.fix_time_s == 0); /* GGA has no date */
    CHECK(f.speed_dkmh == DSD_POSITION_SPEED_UNKNOWN && f.heading_ddeg == DSD_POSITION_HEADING_UNKNOWN);
    CHECK(parse_body("GPGGA,120000,2230.0,N,04500.0,E,2,05,1.0,-12.5,M,,M,,").alt_dm == -125);
    /* Quality 1-5 is a measured fix; 0 (none), 6 (dead reckoning), 7 (manual) and 8 (simulator) are not. */
    for (char q = '0'; q <= '9'; q++) {
        char body[96];
        DSD_SNPRINTF(body, sizeof body, "GPGGA,120000,2230.0,N,04500.0,E,%c,05,1.0,10.0,M,,M,,", q);
        f = parse_body(body);
        CHECK(f.measured_fix == (q >= '1' && q <= '5'));
        CHECK(f.has_position == (q >= '1' && q <= '5'));
    }
}

/* ---- The producer with the real event layer, feed and -L ---- */

static dsd_opts g_opts;
static dsd_state g_state;
static Event_History_I g_history[2];
static char g_lpath[DSD_TEST_PATH_MAX];

static void
reset_fixture(void) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    DSD_MEMSET(&g_state, 0, sizeof g_state);
    DSD_MEMSET(g_history, 0, sizeof g_history);
    g_state.event_history_s = g_history;
    init_event_history(&g_history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&g_history[1], 0, DSD_EVENT_HISTORY_LEN);
    g_state.lastsynctype = DSD_SYNC_P25P1_POS;
    DSD_SNPRINTF(g_opts.lrrp_out_file, sizeof g_opts.lrrp_out_file, "%s", g_lpath);
    g_opts.lrrp_file_output = 1;
    FILE* f = dsd_fopen_private(g_lpath, "w");
    if (f != NULL) {
        fclose(f);
    }
    assert(dsd_position_feed_start(&g_state) == 0);
}

static void
read_lrrp_file(char* out, size_t cap) {
    out[0] = '\0';
    FILE* f = dsd_fopen_private(g_lpath, "r");
    if (f == NULL) {
        return;
    }
    const size_t n = fread(out, 1, cap - 1U, f);
    out[n] = '\0';
    fclose(f);
}

static int
feed_records(dsd_position_fix* out, size_t cap) {
    dsd_position_feed_meta meta;
    return dsd_position_feed_copy_since(&g_state, 0U, out, cap, &meta);
}

/* Bits (one per byte) of @p text. */
static size_t
to_bits(const char* text, uint8_t* bits, size_t cap_bits) {
    const size_t n = strlen(text);
    assert(n * 8U <= cap_bits);
    for (size_t i = 0; i < n; i++) {
        for (size_t b = 0; b < 8U; b++) {
            bits[(i * 8U) + b] = (uint8_t)(((uint8_t)text[i] >> (7U - b)) & 1U);
        }
    }
    return n;
}

static uint8_t
check_payload(const char* text, uint32_t source, int verified) {
    static uint8_t bits[8 * 512];
    const size_t n = to_bits(text, bits, sizeof bits);
    g_state.dmr_lrrp_source[0] = source;
    g_state.dmr_lrrp_target[0] = 77U;
    return nmea_sentence_checker(&g_opts, &g_state, bits, 0U, (int)n, verified, NULL);
}

static const Event_History*
notice_row(void) {
    return &g_history[0].Event_History_Items[1];
}

static void
test_producer_publishes_checked_rmc(void) {
    reset_fixture();
    char s[128];
    sentence(s, sizeof s, "GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A", "\r\n");
    CHECK(check_payload(s, 1234U, 1) == 1U);

    dsd_position_fix recs[4];
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].kind == DSD_POS_NMEA && recs[0].source_id == 1234U);
    CHECK(recs[0].lat_e7 == 225000000 && recs[0].lon_e7 == 450000000);
    CHECK(recs[0].speed_dkmh == 185U && recs[0].heading_ddeg == 900U && recs[0].fix_time_s == 1781524800);
    CHECK(recs[0].err_m == DSD_POSITION_ERR_UNKNOWN);
    CHECK((recs[0].flags & (DSD_POS_FLAG_SOURCE_UNVERIFIED | DSD_POS_FLAG_FORMAT_PROVISIONAL)) == 0U);

    const Event_History* row = notice_row();
    CHECK(row->source_id == 1234U && row->pos.valid == 1U);
    CHECK(row->pos.lat_e7 == 225000000 && row->pos.lon_e7 == 450000000 && row->pos.rev == recs[0].rev);
    CHECK(strstr(row->gps_s, "22.500000") != NULL && strstr(row->gps_s, "45.000000") != NULL);
    CHECK(strncmp(row->text_message, s, strlen(s) - 2U) == 0);

    char lfile[512];
    read_lrrp_file(lfile, sizeof lfile);
    CHECK(strstr(lfile, "00001234\t22.500000\t45.000000\t18\t 90\t") != NULL);
}

/* A packet whose own check failed: no position anywhere, and its sentences' text is withheld. */
static void
test_producer_withholds_failed_packet(void) {
    reset_fixture();
    char s[128];
    sentence(s, sizeof s, "GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A", "\r\n");
    CHECK(check_payload(s, 1234U, 0) == 1U);
    dsd_position_fix recs[4];
    CHECK(feed_records(recs, 4) == 0);
    CHECK(notice_row()->pos.valid == 0U && notice_row()->gps_s[0] == '\0');
    CHECK(strstr(notice_row()->text_message, "2230") == NULL);
    char lfile[512];
    read_lrrp_file(lfile, sizeof lfile);
    CHECK(lfile[0] == '\0');
}

/* Per-sentence checksum and copy: a checked sentence followed by one that fails keeps only the first. */
static void
test_producer_per_sentence(void) {
    reset_fixture();
    char a[128];
    char buf[256];
    sentence(a, sizeof a, "GPGGA,120000,2230.0000,N,04500.0000,E,1,05,1.0,10.0,M,,M,,", "\r\n");
    DSD_SNPRINTF(buf, sizeof buf, "%s%s", a, "$GPRMC,120000,A,9930.0000,S,00000.0100,E,,,150626,,*00\r\n");
    CHECK(check_payload(buf, 4321U, 1) == 1U);
    CHECK(strstr(notice_row()->text_message, "9930") == NULL);
    CHECK(strstr(notice_row()->text_message, "$GPGGA") != NULL);
    dsd_position_fix recs[4];
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].lat_e7 == 225000000 && recs[0].lon_e7 == 450000000 && recs[0].alt_dm == 100);

    /* Every sentence failing: nothing is checked, no notice, and nothing is kept as text. */
    reset_fixture();
    CHECK(check_payload("$GPRMC,120000,A,2230.0000,N,04500.0000,E,,,150626,,,A*00\r\n", 4321U, 1) == 0U);
    CHECK(feed_records(recs, 4) == 0);
    CHECK(notice_row()->write == 0U);
}

/* RMC wins over GGA in one packet (date, speed, course); a refused RMC leaves the GGA. */
static void
test_producer_sentence_choice(void) {
    reset_fixture();
    char g[128];
    char r[128];
    char buf[300];
    sentence(g, sizeof g, "GPGGA,120000,1000.0000,N,02000.0000,E,1,05,1.0,10.0,M,,M,,", "\r\n");
    sentence(r, sizeof r, "GPRMC,120000,A,1000.0000,N,02000.0000,E,001.0,045.0,150626,,,A", "\r\n");
    DSD_SNPRINTF(buf, sizeof buf, "%s%s", g, r);
    CHECK(check_payload(buf, 55U, 1) == 1U);
    dsd_position_fix recs[4];
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].heading_ddeg == 450U && recs[0].fix_time_s == 1781524800);

    reset_fixture();
    sentence(r, sizeof r, "GPRMC,120000,V,1000.0000,N,02000.0000,E,001.0,045.0,150626,,,N", "\r\n");
    DSD_SNPRINTF(buf, sizeof buf, "%s%s", r, g);
    CHECK(check_payload(buf, 55U, 1) == 1U);
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].heading_ddeg == DSD_POSITION_HEADING_UNKNOWN && recs[0].alt_dm == 100);

    /* No measured fix at all: a notice with the text, no position, no -L row. */
    reset_fixture();
    DSD_SNPRINTF(buf, sizeof buf, "%s", r);
    CHECK(check_payload(buf, 55U, 1) == 1U);
    CHECK(feed_records(recs, 4) == 0);
    CHECK(notice_row()->write == 1U && notice_row()->pos.valid == 0U);
    char lfile[512];
    read_lrrp_file(lfile, sizeof lfile);
    CHECK(lfile[0] == '\0');
}

/* ---- P25 SAP 48 through the packet path ---- */

static void
p25_sap48(const char* text, int crc_invalid) {
    uint8_t pdu[256];
    DSD_MEMSET(pdu, 0, sizeof pdu);
    pdu[0] = 0x10; /* fmt 16, io 0: the LLID is the source */
    pdu[1] = 48;   /* SAP 48 location service */
    pdu[3] = 0x00;
    pdu[4] = 0x04;
    pdu[5] = 0xD2; /* LLID 1234 */
    pdu[6] = 0x05;
    const size_t n = strlen(text);
    assert(12U + n + 4U <= sizeof pdu);
    DSD_MEMCPY(pdu + 12, text, n);
    /* As the MDPU decodes a packet: inside its CRC scope on slot 0. */
    const uint8_t saved = g_state.event_crc_invalid[0];
    g_state.event_crc_invalid[0] = (uint8_t)crc_invalid;
    p25_decode_pdu_data(&g_opts, &g_state, pdu, (int)(12U + n + 4U));
    g_state.event_crc_invalid[0] = saved;
}

static void
test_p25_sap48(void) {
    char s[128];
    sentence(s, sizeof s, "GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A", "\r\n");
    reset_fixture();
    p25_sap48(s, 0);
    dsd_position_fix recs[4];
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].source_id == 1234U && recs[0].protocol == DSD_SYNC_P25P1_POS);
    CHECK(notice_row()->pos.valid == 1U && notice_row()->source_id == 1234U);

    reset_fixture();
    p25_sap48(s, 1);
    CHECK(feed_records(recs, 4) == 0);
    CHECK(notice_row()->pos.valid == 0U);

    /* A payload shaped as NMEA whose sentence fails its checksum is never kept as text or GPS (2.4). */
    reset_fixture();
    p25_sap48("$GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A*00\r\n", 0);
    CHECK(feed_records(recs, 4) == 0);
    for (int i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        const Event_History* row = &g_history[0].Event_History_Items[i];
        CHECK(strstr(row->text_message, "GPRMC") == NULL);
        CHECK(strstr(row->gps_s, "GPRMC") == NULL);
    }
    CHECK(strstr(dsd_event_staged_text(&g_state, 0), "GPRMC") == NULL);
    CHECK(strstr(dsd_event_staged_gps(&g_state, 0), "GPRMC") == NULL);
}

int
main(void) {
    int fd = dsd_test_mkstemp(g_lpath, sizeof g_lpath, "core_nmea_sentence");
    if (fd < 0) {
        return 101;
    }
    (void)dsd_close(fd);

    test_framing();
    test_rmc_fields();
    test_coordinate_bounds();
    test_gga_fields();
    test_producer_publishes_checked_rmc();
    test_producer_withholds_failed_packet();
    test_producer_per_sentence();
    test_producer_sentence_choice();
    test_p25_sap48();

    dsd_state_ext_free_all(&g_state);
    (void)remove(g_lpath);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "CORE_NMEA_SENTENCE: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "CORE_NMEA_SENTENCE: PASS\n");
    return 0;
}
