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
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_pdu.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
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

/* The receive time the parser reads an RMC date's two-digit year against: 2026-06-15 12:00:00 UTC. */
enum { RECEIVED_2026 = 1781524800 };

static nmea_fix
parse_body_at(const char* body, int64_t receive_utc_s) {
    char s[160];
    sentence(s, sizeof s, body, "");
    nmea_fix f;
    nmea_parse_fix(s, strlen(s), receive_utc_s, &f);
    return f;
}

static nmea_fix
parse_body(const char* body) {
    return parse_body_at(body, RECEIVED_2026);
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
    CHECK(nmea_next_sentence(bad_hex, sizeof bad_hex - 1U, &pos, &s) == 0);

    /* A sentence whose framing fails does not end the scan: the search resumes at the next line end or start. */
    char resync[256];
    DSD_SNPRINTF(resync, sizeof resync, "$GPGGA,120000*4\r\n%s$GPTXT,1\x01junk%s", a, c);
    pos = 0U;
    CHECK(nmea_next_sentence((const uint8_t*)resync, strlen(resync), &pos, &s) == 1
          && s.status == NMEA_SENTENCE_BAD_HEX);
    CHECK(nmea_next_sentence((const uint8_t*)resync, strlen(resync), &pos, &s) == 1 && s.status == NMEA_SENTENCE_OK);
    CHECK(s.len == strlen(a) - 2U);
    CHECK(nmea_next_sentence((const uint8_t*)resync, strlen(resync), &pos, &s) == 1
          && s.status == NMEA_SENTENCE_NO_STAR);
    CHECK(nmea_next_sentence((const uint8_t*)resync, strlen(resync), &pos, &s) == 1 && s.status == NMEA_SENTENCE_OK);
    CHECK(s.len == strlen(c) - 2U);
    CHECK(nmea_next_sentence((const uint8_t*)resync, strlen(resync), &pos, &s) == 0);
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
    /* '$P' starts a proprietary sentence, never an RMC or GGA. */
    CHECK(parse_body("PGRMC,A,2230.0,N,04500.0,E,,,150626,,,A").type == NMEA_TYPE_OTHER);
    CHECK(parse_body("PQGGA,120000,2230.0,N,04500.0,E,1,05,1.0,10.0,M,,M,,").type == NMEA_TYPE_OTHER);

    /* A mode field that is sent but empty reads as no mode field (pre-NMEA 2.3): status A alone. */
    CHECK(parse_body("GPRMC,120000,A,2230.0,N,04500.0,E,,,150626,,,").measured_fix);
    CHECK(parse_body("GPRMC,120000,A,2230.0,N,04500.0,E,,,150626,,,,V").measured_fix);

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

/* The two-digit year is read in the century that puts the date closest to its reception, so within 50 years of it. */
static void
test_rmc_century(void) {
    /* The canonical example sentence, checksum and all, is from 1994, not 2094. */
    nmea_fix f;
    static const char canonical[] = "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";
    nmea_parse_fix(canonical, strlen(canonical), RECEIVED_2026, &f);
    CHECK(f.has_position && f.fix_time_s == 764426119); /* 1994-03-23 12:35:19 UTC */

    CHECK(parse_body("GPRMC,000000,A,2230.0,N,04500.0,E,,,010170,,,A").fix_time_s == 3155760000LL); /* 2070 */
    CHECK(parse_body("GPRMC,000000,A,2230.0,N,04500.0,E,,,010180,,,A").fix_time_s == 315532800);    /* 1980 */
    CHECK(parse_body("GPRMC,000000,A,2230.0,N,04500.0,E,,,290200,,,A").fix_time_s == 951782400);    /* 2000, leap */
    /* Received in 2080: year 10 is 2110, 30 years on, not 2010, 70 years back. */
    CHECK(parse_body_at("GPRMC,000000,A,2230.0,N,04500.0,E,,,010110,,,A", 3484425600LL).fix_time_s == 4417977600LL);

    int64_t year = 0;
    int64_t days = 0;
    CHECK(dsd_gps_two_digit_year_date(94U, 3U, 23U, RECEIVED_2026, &year, &days) == 1 && year == 1994);
    /* 29 February 00 received in 2080 or 2144: 2100 and 2200 have no 29 February, and 2000 is more than 50 years away,
       so the date is no date at all; a fix time read from it is unknown. Received in 2049, 2000 is within the window. */
    CHECK(dsd_gps_two_digit_year_date(0U, 2U, 29U, 3471292800LL /* 2080-01-01 */, &year, &days) == 0);
    CHECK(dsd_gps_two_digit_year_date(0U, 2U, 29U, 5490892800LL /* 2144-01-01 */, &year, &days) == 0);
    CHECK(dsd_gps_two_digit_year_date(0U, 2U, 29U, 2493072000LL /* 2049-01-01 */, &year, &days) == 1 && year == 2000);
    CHECK(parse_body_at("GPRMC,000000,A,2230.0,N,04500.0,E,,,290200,,,A", 3471292800LL).fix_time_s == 0);
    CHECK(parse_body_at("GPRMC,000000,A,2230.0,N,04500.0,E,,,290200,,,A", 3471292800LL).has_position);
    /* The window's edge: 1 January 76 received on 1 January 2026 is 2076 (18262 days on), not 1976 (18263 back). */
    CHECK(dsd_gps_two_digit_year_date(76U, 1U, 1U, 1767225600LL /* 2026-01-01 */, &year, &days) == 1 && year == 2076);
    CHECK(dsd_gps_two_digit_year_date(100U, 1U, 1U, RECEIVED_2026, &year, &days) == 0);
    CHECK(dsd_gps_two_digit_year_date(26U, 2U, 30U, RECEIVED_2026, &year, &days) == 0);
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
    /* Any nonzero fraction digit past the boundary, those beyond the kept 1e-7 minute included (GGA as RMC). */
    CHECK(!parse_body("GPGGA,120000,9000.00000001,N,04500.0,E,1,05,1.0,10.0,M,,M,,").has_position);
    CHECK(!parse_body("GPRMC,120000,A,2230.0,N,18000.000000000009,W,,,150626,,,A").has_position);
    f = parse_body("GPGGA,120000,9000.000000000000,N,18000.00000000,E,1,05,1.0,10.0,M,,M,,");
    CHECK(f.has_position && f.lat_e7 == 900000000 && f.lon_e7 == 1800000000);
    /* Below the boundary, digits beyond the kept precision are dropped as before. */
    f = parse_body("GPRMC,120000,A,4500.12345678,N,04500.0,E,,,150626,,,A");
    CHECK(f.has_position && f.lat_e7 == 450020576); /* 45 + 0.1234567 / 60, rounded */
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
    /* The satellite count is a whole number: one written with a fraction, a sign or no digits stays unknown, and the
       otherwise valid position is kept. */
    static const char* const k_bad_sats[] = {"08.5", "08.", "8.0", "+8", "-8", "8a", "1234", ""};
    for (size_t i = 0U; i < sizeof k_bad_sats / sizeof k_bad_sats[0]; i++) {
        char body[96];
        DSD_SNPRINTF(body, sizeof body, "GPGGA,120000,2230.0,N,04500.0,E,1,%s,1.0,10.0,M,,M,,", k_bad_sats[i]);
        f = parse_body(body);
        CHECK(f.has_position && f.lat_e7 == 225000000);
        CHECK(f.sats == DSD_POSITION_SATS_NONE);
    }
    CHECK(parse_body("GPGGA,120000,2230.0,N,04500.0,E,1,012,1.0,10.0,M,,M,,").sats == 12U);
    /* Quality 1-5 is a measured fix; 0 (none), 6 (dead reckoning), 7 (manual) and 8 (simulator) are not. */
    for (int q = '0'; q <= '9'; q++) {
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

    /* A sentence with broken checksum digits does not hide the measured fix that follows it. */
    reset_fixture();
    char r[128];
    sentence(r, sizeof r, "GPRMC,120000,A,1000.0000,N,02000.0000,E,,,150626,,,A", "\r\n");
    DSD_SNPRINTF(buf, sizeof buf, "$GPGGA,120000,2230.0000,N,04500.0000,E,1,05,1.0,10.0,M,,M,,*4\r\n%s", r);
    CHECK(check_payload(buf, 4321U, 1) == 1U);
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].lat_e7 == 100000000 && recs[0].lon_e7 == 200000000);
    CHECK(strstr(notice_row()->text_message, "$GPGGA") == NULL);

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

    /* An active RMC at exactly 0, 0 is no candidate: the packet's GGA is the report, and the RMC's text is kept. One
       zero coordinate is a position. */
    reset_fixture();
    char z[128];
    sentence(z, sizeof z, "GPRMC,120000,A,0000.0000,N,00000.0000,E,001.0,045.0,150626,,,A", "\r\n");
    DSD_SNPRINTF(buf, sizeof buf, "%s%s", g, z);
    CHECK(check_payload(buf, 55U, 1) == 1U);
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].lat_e7 == 100000000 && recs[0].lon_e7 == 200000000 && recs[0].alt_dm == 100);
    CHECK(strstr(notice_row()->text_message, "$GPRMC,120000,A,0000.0000") != NULL);

    reset_fixture();
    sentence(z, sizeof z, "GPRMC,120000,A,0000.0000,N,02000.0000,E,001.0,045.0,150626,,,A", "\r\n");
    DSD_SNPRINTF(buf, sizeof buf, "%s%s", g, z);
    CHECK(check_payload(buf, 55U, 1) == 1U);
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].lat_e7 == 0 && recs[0].lon_e7 == 200000000 && recs[0].heading_ddeg == 450U);

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

/* Through the producer: the decode clock is the receive time, so a capture from 1995 replayed now keeps its year. */
static void
test_producer_reads_the_century_from_the_decode_clock(void) {
    reset_fixture();
    dsd_decode_clock_use_test(801964800ULL * 1000000000ULL); /* 1995-06-01 */
    char s[128];
    sentence(s, sizeof s, "GPRMC,123519,A,2230.0000,N,04500.0000,E,,,230394,,,A", "\r\n");
    CHECK(check_payload(s, 1234U, 1) == 1U);
    dsd_position_fix recs[4];
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].fix_time_s == 764426119);
    dsd_decode_clock_use_system();
}

/* ---- P25 SAP 48 through the packet path ---- */

static void
p25_sap48(const char* text, int crc_invalid) {
    static uint8_t pdu[2400];
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

    /* Line ends before the first sentence are the parser's framing: the payload is NMEA. A checked sentence after them
       is the report; a failed one is never text (2.4). */
    char framed[160];
    DSD_SNPRINTF(framed, sizeof framed, "\r\n%s", s);
    reset_fixture();
    p25_sap48(framed, 0);
    CHECK(feed_records(recs, 4) == 1);
    CHECK(recs[0].source_id == 1234U && recs[0].lat_e7 == 225000000);
    CHECK(notice_row()->pos.valid == 1U);

    reset_fixture();
    p25_sap48("\n\r$GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A*00\r\n", 0);
    CHECK(feed_records(recs, 4) == 0);
    for (int i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        const Event_History* row = &g_history[0].Event_History_Items[i];
        CHECK(strstr(row->text_message, "GPRMC") == NULL && strstr(row->text_message, "2230") == NULL);
        CHECK(strstr(row->gps_s, "GPRMC") == NULL && strstr(row->gps_s, "2230") == NULL);
    }
    CHECK(strstr(dsd_event_staged_text(&g_state, 0), "GPRMC") == NULL);
    CHECK(strstr(dsd_event_staged_gps(&g_state, 0), "GPRMC") == NULL);

    /* Plain text is still text. */
    reset_fixture();
    p25_sap48("HELLO", 0);
    CHECK(feed_records(recs, 4) == 0);
    CHECK(strstr(notice_row()->text_message, "HELLO") != NULL || strstr(notice_row()->gps_s, "HELLO") != NULL);
}

/* A SAP 48 packet whose CRC failed (-F) gives neither a report nor text, printed or stored: a damaged first octet makes
   an NMEA report read as text ($GPRMC arriving as %GPRMC), and the service's text can hold a position in any form. The
   packet keeps its notice, without the payload. */
static void
test_p25_sap48_failed_packet_no_text(void) {
    char s[128];
    sentence(s, sizeof s, "GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A", "\r\n");
    char damaged[128];
    DSD_SNPRINTF(damaged, sizeof damaged, "%s", s);
    damaged[0] = '%';
    const char* payloads[] = {damaged, s, "AT 2230.0000N 04500.0000E"};
    for (size_t i = 0; i < sizeof payloads / sizeof payloads[0]; i++) {
        reset_fixture();
        dsd_test_capture_stderr cap;
        char out[4096];
        out[0] = '\0';
        if (dsd_test_capture_stderr_begin(&cap, "sap48_failed") == 0) {
            p25_sap48(payloads[i], 1);
            dsd_test_capture_stderr_end(&cap);
            CHECK(dsd_test_capture_stderr_read(&cap, out, sizeof out) == 0);
        } else {
            CHECK(0);
        }
        CHECK(strstr(out, "2230") == NULL && strstr(out, "GPRMC") == NULL);
        CHECK(strstr(out, "payload withheld (CRC ERR)") != NULL);
        dsd_position_fix recs[4];
        CHECK(feed_records(recs, 4) == 0);
        int notices = 0;
        for (int r = 0; r < DSD_EVENT_HISTORY_LEN; r++) {
            const Event_History* row = &g_history[0].Event_History_Items[r];
            CHECK(strstr(row->text_message, "2230") == NULL && strstr(row->gps_s, "2230") == NULL);
            CHECK(strstr(row->event_string, "2230") == NULL);
            notices += strstr(row->event_string, "payload withheld") != NULL;
        }
        CHECK(notices == 1);
        CHECK(strstr(dsd_event_staged_text(&g_state, 0), "2230") == NULL);
        CHECK(strstr(dsd_event_staged_gps(&g_state, 0), "2230") == NULL);
        char lrrp[512];
        read_lrrp_file(lrrp, sizeof lrrp);
        CHECK(lrrp[0] == '\0');
    }
}

/* An inbound SAP 48 packet whose LLID is a fixed-network ID (0xFFFFFD, 0xFFFFFF) names no radio: its checked report is
   kept anonymous, never as a track of the reserved ID. */
static void
test_p25_sap48_reserved_source(void) {
    static const uint32_t reserved[] = {0xFFFFFDU, 0xFFFFFFU};
    char s[128];
    sentence(s, sizeof s, "GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A", "\r\n");
    const size_t n = strlen(s);
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        reset_fixture();
        uint8_t pdu[256];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        pdu[0] = 0x10; /* fmt 16, io 0: the LLID is the source */
        pdu[1] = 48;
        pdu[3] = (uint8_t)(reserved[i] >> 16);
        pdu[4] = (uint8_t)(reserved[i] >> 8);
        pdu[5] = (uint8_t)reserved[i];
        pdu[6] = 0x05;
        DSD_MEMCPY(pdu + 12, s, n);
        p25_decode_pdu_data(&g_opts, &g_state, pdu, (int)(12U + n + 4U));
        dsd_position_fix recs[4];
        CHECK(feed_records(recs, 4) == 1);
        CHECK(recs[0].source_id == 0U && (recs[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) == 0U);
        CHECK(notice_row()->pos.valid == 1U && notice_row()->source_id == 0U);
    }
}

/* A SAP 48 packet longer than 1,024 octets is read whole: status sentences first, then the only RMC, after octet 1,024.
 * That RMC is the report, and it wins over a GGA that came first, as in a short packet. */
static void
test_p25_sap48_long_payload(void) {
    static char payload[2200];
    char g[128];
    char r[128];
    char t[128];
    sentence(g, sizeof g, "GPGGA,120000,1000.0000,N,02000.0000,E,1,05,1.0,10.0,M,,M,,", "\r\n");
    sentence(r, sizeof r, "GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A", "\r\n");
    sentence(t, sizeof t, "GPTXT,01,01,02,RECEIVER STATUS REPORT FILLER", "\r\n");
    for (int with_gga = 0; with_gga <= 1; with_gga++) {
        size_t at = 0U;
        payload[0] = '\0';
        if (with_gga) {
            at += (size_t)DSD_SNPRINTF(payload + at, sizeof payload - at, "%s", g);
        }
        while (at <= 1100U) {
            at += (size_t)DSD_SNPRINTF(payload + at, sizeof payload - at, "%s", t);
        }
        (void)DSD_SNPRINTF(payload + at, sizeof payload - at, "%s", r);
        reset_fixture();
        p25_sap48(payload, 0);
        dsd_position_fix recs[4];
        CHECK(feed_records(recs, 4) == 1);
        CHECK(recs[0].lat_e7 == 225000000 && recs[0].lon_e7 == 450000000 && recs[0].heading_ddeg == 900U);
        CHECK(recs[0].source_id == 1234U);
    }
}

/* ---- P25: a report is admitted against the reception its MDPU's header was heard on (issue #800 2.3 step 3) ---- */

/* An LRRP POINT_2D response (lat 11.25, lon 45) in IPv4/UDP to port 4001, as a SAP 0 packet from LLID 1234. */
static void
p25_ip_lrrp(void) {
    static const uint8_t doc[] = {0x07, 11, 0x22, 0, 0x66, 0x10, 0, 0, 0, 0x20, 0, 0, 0};
    uint8_t pdu[128];
    DSD_MEMSET(pdu, 0, sizeof pdu);
    pdu[0] = 0x10; /* fmt 16, io 0: the LLID is the source */
    pdu[1] = 0;    /* SAP 0: IP packet data */
    pdu[4] = 0x04;
    pdu[5] = 0xD2; /* LLID 1234 */
    pdu[6] = 0x05;
    uint8_t* ip = pdu + 12;
    const uint16_t udp_len = (uint16_t)(8U + sizeof doc);
    const uint16_t ip_len = (uint16_t)(20U + udp_len);
    ip[0] = 0x45;
    ip[3] = (uint8_t)ip_len;
    ip[8] = 64;
    ip[9] = 17;
    ip[12] = 10;
    ip[15] = 111;
    ip[16] = 10;
    ip[19] = 222;
    ip[20] = ip[22] = 0x0F;
    ip[21] = ip[23] = 0xA1; /* UDP 4001 */
    ip[25] = (uint8_t)udp_len;
    DSD_MEMCPY(ip + 28, doc, sizeof doc);
    p25_decode_pdu_data(&g_opts, &g_state, pdu, (int)(12U + ip_len + 4U));
}

/* Issue #800, fail closed: a P25 SAP 0 packet hands the IPv4 decoder exactly its user data, never a pad or CRC octet.
 * The datagram (an LRRP POINT_2D response to UDP 4001 from LLID 1234) declares a total length one octet longer than
 * the user data, and that last octet is the first of seven pad octets: the datagram is incomplete and gives no
 * position. The same datagram whole in the user data gives one. */
static void
test_p25_ip_exact_payload(void) {
    static const uint8_t doc[] = {0x07, 11, 0x22, 0, 0x66, 0x10, 0, 0, 0, 0x20, 0, 0, 0};
    for (int cut = 0; cut <= 1; cut++) {
        reset_fixture();
        uint8_t pdu[128];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        const uint16_t udp_len = (uint16_t)(8U + sizeof doc);
        const uint16_t ip_len = (uint16_t)(20U + udp_len);
        const uint8_t pad = cut ? 7U : 0U;
        const size_t user = (size_t)ip_len - (size_t)cut;
        pdu[0] = 0x10; /* fmt 16, io 0: the LLID is the source */
        pdu[1] = 0;    /* SAP 0: IP packet data */
        pdu[4] = 0x04;
        pdu[5] = 0xD2; /* LLID 1234 */
        pdu[6] = 0x05;
        pdu[7] = pad;
        uint8_t* ip = pdu + 12;
        ip[0] = 0x45;
        ip[3] = (uint8_t)ip_len;
        ip[8] = 64;
        ip[9] = 17;
        ip[12] = 10;
        ip[15] = 111;
        ip[16] = 10;
        ip[19] = 222;
        ip[20] = ip[22] = 0x0F;
        ip[21] = ip[23] = 0xA1; /* UDP 4001 */
        ip[25] = (uint8_t)udp_len;
        DSD_MEMCPY(ip + 28, doc, sizeof doc); /* the last octet (0) is the first pad octet when cut */
        p25_decode_pdu_data(&g_opts, &g_state, pdu, (int)(12U + user + pad + 4U));
        dsd_position_fix recs[4];
        CHECK(feed_records(recs, 4) == (cut ? 0 : 1));
        CHECK(!cut || notice_row()->pos.valid == 0U);
    }
}

/* Issue #800, fail closed: an MDPU's header and its blocks pass separate checks, and neither shows they were heard on
 * one reception. A packet whose header was heard before a retune or a change of P25 phase gives nothing of its
 * location payload under the reception after it, in any form: no NMEA or LRRP report, and no location text (a
 * plain-text location payload, which reads as a position in any form) in a notice, the text staged for one or on
 * stderr. Heard with no change, each gives its report or text. */
static void
test_p25_origin(void) {
    char s[128];
    sentence(s, sizeof s, "GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A", "\r\n");
    dsd_position_fix recs[4];
    for (int kind = 0; kind <= 2; kind++) {           /* 0 NMEA, 1 LRRP over IP, 2 plain location text */
        for (int change = 0; change <= 2; change++) { /* 0 none, 1 a retune, 2 a change of P25 phase */
            reset_fixture();
            dsd_rxid_note_sync(&g_state, DSD_SYNC_P25P1_POS);
            dsd_position_origin_capture(&g_opts, &g_state, &g_state.p25_mdpu_origin);
            if (change == 1) {
                dsd_trunk_tuning_generation_advance();
            } else if (change == 2) {
                dsd_rxid_note_sync(&g_state, DSD_SYNC_P25P2_POS);
                dsd_rxid_note_sync(&g_state, DSD_SYNC_P25P1_POS);
            }
            dsd_test_capture_stderr cap;
            char out[4096];
            out[0] = '\0';
            if (dsd_test_capture_stderr_begin(&cap, "sap48_origin") == 0) {
                if (kind == 1) {
                    p25_ip_lrrp();
                } else {
                    p25_sap48(kind == 0 ? s : "AT 2230.0000N 04500.0000E", 0);
                }
                dsd_test_capture_stderr_end(&cap);
                CHECK(dsd_test_capture_stderr_read(&cap, out, sizeof out) == 0);
            } else {
                CHECK(0);
            }
            const int kept = feed_records(recs, 4);
            CHECK(kept == (change != 0 || kind == 2 ? 0 : 1));
            CHECK(kept != 1 || recs[0].kind == (kind == 1 ? DSD_POS_DMR_LRRP : DSD_POS_NMEA));
            CHECK(kept != 1 || recs[0].protocol == DSD_SYNC_P25P1_POS);
            if (kind == 1) {
                continue;
            }
            int text_rows = 0;
            for (int i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
                const Event_History* row = &g_history[0].Event_History_Items[i];
                text_rows += strstr(row->text_message, "2230") != NULL || strstr(row->gps_s, "2230") != NULL
                             || strstr(row->event_string, "2230") != NULL;
                CHECK(change == 0 || strstr(row->event_string, "NMEA SRC") == NULL);
            }
            if (change != 0) {
                /* Nothing of a packet that began on another reception goes in under this one: no sentence or text,
                   no notice, nothing staged for the next notice. */
                CHECK(text_rows == 0 && strstr(out, "2230") == NULL);
                CHECK(strstr(dsd_event_staged_text(&g_state, 0), "2230") == NULL);
                CHECK(strstr(dsd_event_staged_gps(&g_state, 0), "2230") == NULL);
            } else {
                CHECK(text_rows > 0);
            }
        }
    }
}

/* ---- The report's destination (issue #800): a P25 packet's target LLID or IP destination, never a group ---- */

/* SAP 48 outbound (io 1): the LLID is the destination; the packet names no source. */
static void
p25_sap48_to(const char* text, uint32_t llid) {
    uint8_t pdu[256];
    DSD_MEMSET(pdu, 0, sizeof pdu);
    pdu[0] = 0x30; /* fmt 16, io 1: the LLID is the destination */
    pdu[1] = 48;
    pdu[3] = (uint8_t)(llid >> 16);
    pdu[4] = (uint8_t)(llid >> 8);
    pdu[5] = (uint8_t)llid;
    pdu[6] = 0x05;
    const size_t n = strlen(text);
    DSD_MEMCPY(pdu + 12, text, n);
    p25_decode_pdu_data(&g_opts, &g_state, pdu, (int)(12U + n + 4U));
}

/* A LIP short report (22.5 S, 45 W) in IPv4/UDP to port 5017 from 10.0.4.210 to 10.0.0.@p dst_host. */
static void
p25_ip_lip(uint8_t dst_host) {
    uint8_t lip[10];
    DSD_MEMSET(lip, 0, sizeof lip);
    uint8_t bits[80];
    DSD_MEMSET(bits, 0, sizeof bits);

    const struct {
        size_t off;
        unsigned width;
        uint32_t value;
    } fields[] = {{4, 1, 1}, {5, 24, 0xC00000}, {29, 1, 1}, {30, 23, 0x600000}, {68, 8, 32}};

    for (size_t f = 0; f < sizeof fields / sizeof fields[0]; f++) {
        for (unsigned i = 0; i < fields[f].width; i++) {
            bits[fields[f].off + i] = (uint8_t)((fields[f].value >> (fields[f].width - 1U - i)) & 1U);
        }
    }
    for (size_t i = 0; i < sizeof lip; i++) {
        for (size_t b = 0; b < 8U; b++) {
            lip[i] = (uint8_t)((lip[i] << 1) | bits[(i * 8U) + b]);
        }
    }
    uint8_t pdu[128];
    DSD_MEMSET(pdu, 0, sizeof pdu);
    pdu[0] = 0x10;
    pdu[4] = 0x04;
    pdu[5] = 0xD2; /* LLID 1234 */
    pdu[6] = 0x05;
    uint8_t* ip = pdu + 12;
    const uint16_t udp_len = (uint16_t)(8U + sizeof lip);
    const uint16_t ip_len = (uint16_t)(20U + udp_len);
    ip[0] = 0x45;
    ip[3] = (uint8_t)ip_len;
    ip[8] = 64;
    ip[9] = 17;
    ip[12] = 10;
    ip[14] = 0x04;
    ip[15] = 0xD2;
    ip[16] = 10;
    ip[19] = dst_host;
    ip[20] = ip[22] = 0x13;
    ip[21] = ip[23] = 0x99; /* UDP 5017 */
    ip[25] = (uint8_t)udp_len;
    DSD_MEMCPY(ip + 28, lip, sizeof lip);
    p25_decode_pdu_data(&g_opts, &g_state, pdu, (int)(12U + ip_len + 4U));
}

static void
test_p25_destination(void) {
    char s[128];
    sentence(s, sizeof s, "GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A", "\r\n");
    dsd_position_fix recs[4];
    reset_fixture();
    p25_sap48_to(s, 77U);
    p25_sap48_to(s, 88U);
    CHECK(feed_records(recs, 4) == 2);
    CHECK(recs[0].target_id == 77U && (recs[0].flags & DSD_POS_FLAG_TARGET_GROUP) == 0U);
    CHECK(recs[1].target_id == 88U && (recs[1].flags & DSD_POS_FLAG_TARGET_GROUP) == 0U);

    reset_fixture();
    p25_ip_lip(77U);
    p25_ip_lip(88U);
    CHECK(feed_records(recs, 4) == 2);
    CHECK(recs[0].kind == DSD_POS_LIP && recs[0].source_id == 1234U && recs[0].target_id == 77U
          && (recs[0].flags & DSD_POS_FLAG_TARGET_GROUP) == 0U);
    CHECK(recs[1].kind == DSD_POS_LIP && recs[1].source_id == 1234U && recs[1].target_id == 88U);
}

/* Issue #800 (g62-r6): the sentences of a checked packet are shown (console and History) only after admission admitted
 * its report. A packet with no measured fix (RMC status V, an active RMC at exactly 0, 0) carries coordinates the
 * decoder rejected: neither the console nor the notice shows them. An admitted packet's sentences are shown. */
static void
test_producer_withholds_unadmitted_sentences(void) {
    static const char* const bodies[2] = {
        "GPRMC,120000,V,1000.0000,N,02000.0000,E,001.0,045.0,150626,,,N",
        "GPRMC,120000,A,0000.0000,N,00000.0000,E,001.0,045.0,150626,,,A",
    };
    for (size_t i = 0U; i < 2U; i++) {
        reset_fixture();
        char r[128];
        sentence(r, sizeof r, bodies[i], "\r\n");
        dsd_test_capture_stderr cap;
        char out[2048];
        out[0] = '\0';
        const int captured = dsd_test_capture_stderr_begin(&cap, "nmea_unadmitted") == 0;
        CHECK(check_payload(r, 55U, 1) == 1U);
        if (captured) {
            (void)dsd_test_capture_stderr_end(&cap);
            (void)dsd_test_capture_stderr_read(&cap, out, sizeof out);
            CHECK(strstr(out, "GPRMC") == NULL);
            CHECK(strstr(out, "NMEA Withheld (no admitted position);") != NULL);
        }
        dsd_position_fix recs[2];
        CHECK(feed_records(recs, 2) == 0);
        CHECK(notice_row()->write == 1U && notice_row()->pos.valid == 0U);
        CHECK(strstr(notice_row()->text_message, "GPRMC") == NULL);
        CHECK(strstr(notice_row()->text_message, "NMEA withheld (no admitted position)") != NULL);
    }

    reset_fixture();
    char a[128];
    sentence(a, sizeof a, "GPRMC,120000,A,1000.0000,N,02000.0000,E,001.0,045.0,150626,,,A", "\r\n");
    dsd_test_capture_stderr cap;
    char out[2048];
    out[0] = '\0';
    const int captured = dsd_test_capture_stderr_begin(&cap, "nmea_admitted") == 0;
    CHECK(check_payload(a, 55U, 1) == 1U);
    if (captured) {
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof out);
        CHECK(strstr(out, "NMEA: $GPRMC,120000,A,1000.0000") != NULL);
    }
    CHECK(strstr(notice_row()->text_message, "$GPRMC,120000,A,1000.0000") != NULL);
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
    test_rmc_century();
    test_coordinate_bounds();
    test_gga_fields();
    test_producer_publishes_checked_rmc();
    test_producer_withholds_failed_packet();
    test_producer_per_sentence();
    test_producer_sentence_choice();
    test_producer_withholds_unadmitted_sentences();
    test_p25_sap48();
    test_p25_sap48_failed_packet_no_text();
    test_p25_sap48_reserved_source();
    test_p25_sap48_long_payload();
    test_p25_origin();
    test_p25_ip_exact_payload();
    test_p25_destination();
    test_producer_reads_the_century_from_the_decode_clock();

    dsd_state_ext_free_all(&g_state);
    (void)remove(g_lpath);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "CORE_NMEA_SENTENCE: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "CORE_NMEA_SENTENCE: PASS\n");
    return 0;
}
