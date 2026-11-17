// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * NXDN position reports through the data call dispatch (issue #800: 2.8, V2, V3, B6, D12): a checked data call header
 * on a FACCH2/UDCH, its data blocks, the CRC32 over the assembled packet, then the GPS report or the NMEA sentences,
 * with the real event layer, feed and -L writer. The degree/minute packing counts a minute once (2.8); a report too
 * short for its fields gives nothing; every component is checked before it is converted (V2); a heading out of range
 * is unknown (V3); the format is provisional (D12); and a payload is attributed only to the header that opened its
 * assembly with nothing between them (B6). All IDs, coordinates and dates are made up.
 */

#include <assert.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/nxdn/nxdn_deperm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "nxdn_crc.h"
#include "nxdn_internal.h"
#include "test_support.h"

static int g_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

enum {
    F2_ELEMENTS = 199 - 8 - 15, /* the elements a FACCH2/UDCH hands over (nxdn_handle_facch2_udch()) */
    BLOCK_BYTES = 20,           /* a Type-C UDCH data block */
    MAX_PAYLOAD = 16 * BLOCK_BYTES,
    GPS_MSG_BYTES = 38, /* the report as SDRTrunk lays it out: marker through the empty trailer */
};

static dsd_opts g_opts;
static dsd_state g_state;
static Event_History_I g_history[2];
static char g_lpath[DSD_TEST_PATH_MAX];
static uint64_t g_now_ns;
static uint8_t g_ran = 5U;

static void
put_bits(uint8_t* bits, size_t pos, uint64_t value, size_t count) {
    for (size_t i = 0; i < count; i++) {
        bits[pos + i] = (uint8_t)((value >> (count - 1U - i)) & 1U);
    }
}

static void
reset_fixture(void) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    DSD_MEMSET(&g_state, 0, sizeof g_state);
    DSD_MEMSET(g_history, 0, sizeof g_history);
    g_state.event_history_s = g_history;
    init_event_history(&g_history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&g_history[1], 0, DSD_EVENT_HISTORY_LEN);
    g_state.lastsynctype = DSD_SYNC_NXDN_POS;
    g_state.data_header_blocks[0] = 1;
    DSD_SNPRINTF(g_opts.lrrp_out_file, sizeof g_opts.lrrp_out_file, "%s", g_lpath);
    g_opts.lrrp_file_output = 1;
    FILE* f = dsd_fopen_private(g_lpath, "w");
    if (f != NULL) {
        fclose(f);
    }
    g_ran = 5U;
    g_now_ns += 60ULL * 1000000000ULL;
    dsd_decode_clock_test_set_ns(g_now_ns);
    assert(dsd_position_feed_start(&g_state) == 0);
}

static void
advance_ms(uint64_t ms) {
    g_now_ns += ms * 1000000ULL;
    dsd_decode_clock_test_set_ns(g_now_ns);
}

/* One FACCH2/UDCH carrying @p elements; @p crc_ok 0 makes its CRC fail. */
static void
udch(const uint8_t* elements, int crc_ok) {
    uint8_t trellis[208];
    uint8_t m_data[32];
    DSD_MEMSET(trellis, 0, sizeof trellis);
    DSD_MEMSET(m_data, 0, sizeof m_data);
    put_bits(trellis, 2, g_ran, 6);
    DSD_MEMCPY(trellis + 8, elements, F2_ELEMENTS);
    nxdn_handle_facch2_udch(&g_opts, &g_state, trellis, m_data, crc_ok ? 0U : 1U, 0U, 0U);
}

static void
header_elements(uint8_t* e, uint16_t src, uint16_t dst, uint8_t blocks, uint8_t pad) {
    DSD_MEMSET(e, 0, F2_ELEMENTS);
    put_bits(e, 2, 0x09U, 6);
    put_bits(e, 16, 1U, 3);
    put_bits(e, 24, src, 16);
    put_bits(e, 40, dst, 16);
    put_bits(e, 68, blocks, 4);
    put_bits(e, 72, pad, 5);
}

static uint8_t
blocks_for(size_t total) {
    return (uint8_t)(((total + BLOCK_BYTES - 1U) / BLOCK_BYTES) - 1U);
}

static void
send_header(uint16_t src, size_t total, int crc_ok) {
    uint8_t e[F2_ELEMENTS];
    const uint8_t blocks = blocks_for(total);
    header_elements(e, src, 900U, blocks, (uint8_t)(((size_t)blocks + 1U) * BLOCK_BYTES - total));
    udch(e, crc_ok);
}

/* The data blocks of @p payload (its CRC32 already in place), highest block number first. */
static void
send_blocks(const uint8_t* payload, size_t total) {
    const uint8_t blocks = blocks_for(total);
    for (int k = blocks; k >= 0; k--) {
        uint8_t e[F2_ELEMENTS];
        DSD_MEMSET(e, 0, sizeof e);
        put_bits(e, 2, 0x0BU, 6);
        put_bits(e, 8, (uint64_t)k, 4);
        put_bits(e, 12, (uint64_t)k, 4);
        const size_t first = (size_t)(blocks - k) * BLOCK_BYTES;
        for (size_t i = 0; i < BLOCK_BYTES && first + i < total; i++) {
            put_bits(e, 16U + (i * 8U), payload[first + i], 8);
        }
        udch(e, 1);
    }
}

/* The packet's CRC32 over every octet before it, in its last four octets. */
static void
seal(uint8_t* payload, size_t total) {
    uint8_t bits[MAX_PAYLOAD * 8];
    const size_t n = total - 4U;
    for (size_t i = 0; i < n; i++) {
        put_bits(bits, i * 8U, payload[i], 8);
    }
    const uint32_t crc = nxdn_crc32_bits(bits, n * 8U);
    payload[n] = (uint8_t)(crc >> 24);
    payload[n + 1U] = (uint8_t)(crc >> 16);
    payload[n + 2U] = (uint8_t)(crc >> 8);
    payload[n + 3U] = (uint8_t)crc;
}

typedef struct {
    uint16_t lat_degmin, lat_frac, lon_degmin;
    uint8_t lat_south, lon_west;
    uint16_t speed, heading;
    uint8_t year, month, day_raw, hour, minute, second;
} gps_in;

static gps_in
gps_default(void) {
    gps_in g = {
        .lat_degmin = 1000U,
        .lon_degmin = 2230U,
        .speed = 123U,
        .heading = 2700U,
        .year = 25U,
        .month = 6U,
        .day_raw = 21U,
        .hour = 14U,
        .minute = 45U,
        .second = 30U,
    };
    return g;
}

/* The report as it travels: 0xFFFC and the fields at SDRTrunk's offsets, cut to @p len octets, byte-reversed after
 * four leading zero octets, then the CRC32 (the payload nxdn_dcall_handle_reverse_gps() reverses). */
static size_t
gps_payload(uint8_t* payload, const gps_in* g, size_t len) {
    uint8_t bits[GPS_MSG_BYTES * 8];
    DSD_MEMSET(bits, 0, sizeof bits);
    put_bits(bits, 0, 0xFFFCU, 16);
    put_bits(bits, 90, g->speed, 14);
    put_bits(bits, 108, g->heading, 12);
    put_bits(bits, 152, g->year, 7);
    put_bits(bits, 159, g->month, 4);
    put_bits(bits, 163, g->day_raw, 5);
    put_bits(bits, 168, g->lon_degmin, 16);
    put_bits(bits, 199, g->lon_west, 1);
    put_bits(bits, 200, g->lat_degmin, 16);
    put_bits(bits, 216, g->lat_frac, 15);
    put_bits(bits, 231, g->lat_south, 1);
    put_bits(bits, 263, g->hour, 5);
    put_bits(bits, 268, g->minute, 6);
    put_bits(bits, 274, g->second, 6);
    const size_t total = len + 8U;
    DSD_MEMSET(payload, 0, total);
    for (size_t i = 0; i < len; i++) {
        uint8_t octet = 0U;
        for (size_t b = 0; b < 8U; b++) {
            octet = (uint8_t)((octet << 1) | bits[(i * 8U) + b]);
        }
        payload[total - 5U - i] = octet;
    }
    seal(payload, total);
    return total;
}

static void
send_gps(uint16_t src, const gps_in* g, size_t len) {
    uint8_t payload[MAX_PAYLOAD];
    const size_t total = gps_payload(payload, g, len);
    send_header(src, total, 1);
    send_blocks(payload, total);
}

static int
feed(dsd_position_fix* out, size_t cap) {
    dsd_position_feed_meta meta;
    return dsd_position_feed_copy_since(&g_state, 0U, out, cap, &meta);
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

/* Nothing anywhere: no feed record, no -L row, no history row naming @p src or carrying a position. */
static void
expect_nothing(uint16_t src) {
    dsd_position_fix recs[4];
    CHECK(feed(recs, 4) == 0);
    char lfile[512];
    read_lrrp_file(lfile, sizeof lfile);
    CHECK(lfile[0] == '\0');
    for (int i = 0; i < DSD_EVENT_HISTORY_LEN; i++) {
        const Event_History* row = &g_history[0].Event_History_Items[i];
        CHECK(row->pos.valid == 0U && row->gps_s[0] == '\0');
        CHECK(row->write == 0U || row->source_id != src);
    }
}

static void
expect_one(uint16_t src, int32_t lat_e7, int32_t lon_e7, dsd_position_fix* out) {
    dsd_position_fix recs[4];
    CHECK(feed(recs, 4) == 1);
    *out = recs[0];
    CHECK(recs[0].kind == DSD_POS_NXDN_GPS && recs[0].source_id == src);
    CHECK(recs[0].lat_e7 == lat_e7 && recs[0].lon_e7 == lon_e7);
    CHECK((recs[0].flags & DSD_POS_FLAG_FORMAT_PROVISIONAL) != 0U);
    CHECK((recs[0].flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) == 0U);
}

/* 2.8: DDMM 2230 is 22 degrees 30 minutes = 22.5, not 22.30 + 0.5. */
static void
test_degree_minute_packing(void) {
    reset_fixture();
    gps_in g = gps_default();
    send_gps(100U, &g, GPS_MSG_BYTES);
    dsd_position_fix r;
    expect_one(100U, 100000000, 225000000, &r);
    CHECK(r.speed_dkmh == 123U && r.heading_ddeg == 2700U && r.fix_time_s == 1750603530);
    CHECK(r.alt_dm == DSD_POSITION_ALT_UNKNOWN);
    const Event_History* row = &g_history[0].Event_History_Items[1];
    CHECK(row->source_id == 100U && row->pos.valid == 1U && row->pos.lon_e7 == 225000000);
    CHECK(strstr(row->gps_s, "22.500000") != NULL);
    char lfile[512];
    read_lrrp_file(lfile, sizeof lfile);
    CHECK(strstr(lfile, "00000100\t10.000000\t22.500000\t12\t 270\t") != NULL);

    /* South and west, with the latitude's fraction of a minute. */
    reset_fixture();
    g.lat_degmin = 2230U;
    g.lat_frac = 5000U;
    g.lat_south = 1U;
    g.lon_degmin = 4500U;
    g.lon_west = 1U;
    send_gps(100U, &g, GPS_MSG_BYTES);
    expect_one(100U, -225083333, -450000000, &r);
}

/* 2.8: a report shorter than its fields gives no position (here a 2-octet and a 34-octet report). */
static void
test_short_report(void) {
    const gps_in g = gps_default();
    reset_fixture();
    send_gps(100U, &g, 2U);
    expect_nothing(100U);
    reset_fixture();
    send_gps(100U, &g, 34U);
    expect_nothing(100U);
    reset_fixture();
    send_gps(100U, &g, 35U); /* through the UTC seconds: enough */
    dsd_position_fix r;
    expect_one(100U, 100000000, 225000000, &r);
}

/* V2: components are checked before they are converted; a boundary degree only with zero minutes and fraction. */
static void
test_component_bounds(void) {
    static const struct {
        uint16_t lat, frac, lon;
        int valid;
        int32_t lat_e7, lon_e7;
    } cases[] = {
        {2260U, 0U, 2230U, 0, 0, 0},
        {2230U, 10000U, 2230U, 0, 0, 0},
        {9100U, 0U, 2230U, 0, 0, 0},
        {9000U, 1U, 2230U, 0, 0, 0},
        {1000U, 0U, 2260U, 0, 0, 0},
        {1000U, 0U, 18001U, 0, 0, 0},
        {1000U, 0U, 18100U, 0, 0, 0},
        {9000U, 0U, 18000U, 1, 900000000, 1800000000},
        {1000U, 9999U, 2230U, 1, 100166650, 225000000},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_fixture();
        gps_in g = gps_default();
        g.lat_degmin = cases[i].lat;
        g.lat_frac = cases[i].frac;
        g.lon_degmin = cases[i].lon;
        send_gps(100U, &g, GPS_MSG_BYTES);
        if (cases[i].valid) {
            dsd_position_fix r;
            expect_one(100U, cases[i].lat_e7, cases[i].lon_e7, &r);
        } else {
            expect_nothing(100U);
        }
    }
}

/* V2: an invalid date or time leaves the fix time unknown; the position stays. */
static void
test_date_time_bounds(void) {
    static const struct {
        uint8_t year, month, day_raw, hour, minute, second;
        int64_t fix_time_s;
    } cases[] = {
        {25U, 6U, 21U, 14U, 45U, 30U, 1750603530}, {26U, 1U, 0U, 0U, 0U, 0U, 1767225600},
        {25U, 13U, 21U, 14U, 45U, 30U, 0},         {25U, 0U, 21U, 14U, 45U, 30U, 0},
        {25U, 2U, 30U, 14U, 45U, 30U, 0},          {25U, 2U, 28U, 14U, 45U, 30U, 0}, /* 29 February 2025 */
        {25U, 6U, 21U, 24U, 45U, 30U, 0},          {25U, 6U, 21U, 14U, 60U, 30U, 0},
        {25U, 6U, 21U, 14U, 45U, 60U, 0},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_fixture();
        gps_in g = gps_default();
        g.year = cases[i].year;
        g.month = cases[i].month;
        g.day_raw = cases[i].day_raw;
        g.hour = cases[i].hour;
        g.minute = cases[i].minute;
        g.second = cases[i].second;
        send_gps(100U, &g, GPS_MSG_BYTES);
        dsd_position_fix r;
        expect_one(100U, 100000000, 225000000, &r);
        CHECK(r.fix_time_s == cases[i].fix_time_s);
    }
}

/* V3: a heading above 359.9 degrees is unknown (raw 4095 is "none"), never folded; -L keeps its 0 placeholder. */
static void
test_heading_bounds(void) {
    static const struct {
        uint16_t raw;
        uint16_t want;
        const char* row;
        const char* printed;
    } cases[] = {
        {3599U, 3599U, "\t12\t 359\t", "COG: 359.9;"},
        {3600U, DSD_POSITION_HEADING_UNKNOWN, "\t12\t 0\t", "COG: unknown;"},
        {4095U, DSD_POSITION_HEADING_UNKNOWN, "\t12\t 0\t", "COG: unknown;"},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        reset_fixture();
        gps_in g = gps_default();
        g.heading = cases[i].raw;
        dsd_test_capture_stderr cap;
        const int captured = dsd_test_capture_stderr_begin(&cap, "nxdn_gps_heading") == 0;
        send_gps(100U, &g, GPS_MSG_BYTES);
        static char printed[16384];
        printed[0] = '\0';
        if (captured) {
            (void)dsd_test_capture_stderr_end(&cap);
            (void)dsd_test_capture_stderr_read(&cap, printed, sizeof printed);
        }
        CHECK(captured && strstr(printed, cases[i].printed) != NULL);
        dsd_position_fix r;
        expect_one(100U, 100000000, 225000000, &r);
        CHECK(r.heading_ddeg == cases[i].want);
        char lfile[512];
        read_lrrp_file(lfile, sizeof lfile);
        CHECK(strstr(lfile, cases[i].row) != NULL);
    }
}

/* B6: radio 100's header is heard and its payload lost; radio 200's header fails its CRC; radio 200's whole payload
 * passes its CRC32. Nothing of it may be published as radio 100's. */
static void
test_binding_failed_replacement_header(void) {
    reset_fixture();
    const gps_in g = gps_default();
    uint8_t payload[MAX_PAYLOAD];
    const size_t total = gps_payload(payload, &g, GPS_MSG_BYTES);
    send_header(100U, total, 1);
    send_header(200U, total, 0);
    send_blocks(payload, total);
    expect_nothing(100U);

    /* With radio 200's header heard, the same payload is radio 200's. */
    send_header(200U, total, 1);
    send_blocks(payload, total);
    dsd_position_fix r;
    expect_one(200U, 100000000, 225000000, &r);
}

/* B6: a rejected (too short) replacement header and a failed FACCH3/UDCH2 drop the binding too. */
static void
test_binding_rejected_header_and_udch2(void) {
    const gps_in g = gps_default();
    uint8_t payload[MAX_PAYLOAD];
    const size_t total = gps_payload(payload, &g, GPS_MSG_BYTES);

    reset_fixture();
    send_header(100U, total, 1);
    uint8_t short_header[80];
    DSD_MEMSET(short_header, 0, sizeof short_header);
    put_bits(short_header, 2, 0x09U, 6);
    NXDN_Elements_Content_decode(&g_opts, &g_state, short_header, sizeof short_header);
    send_blocks(payload, total);
    expect_nothing(100U);

    /* A rejected SD call header (0x38 shorter than 79 bits) replaces the bound one too. */
    reset_fixture();
    send_header(100U, total, 1);
    uint8_t short_sd_header[64];
    DSD_MEMSET(short_sd_header, 0, sizeof short_sd_header);
    put_bits(short_sd_header, 2, 0x38U, 6);
    NXDN_Elements_Content_decode(&g_opts, &g_state, short_sd_header, sizeof short_sd_header);
    CHECK(!nxdn_data_call_header_bound(&g_state));
    send_blocks(payload, total);
    expect_nothing(100U);

    reset_fixture();
    send_header(100U, total, 1);
    struct nxdn_facch3_udch2_message failed;
    DSD_MEMSET(&failed, 0, sizeof failed);
    failed.crc[0] = 1U;
    nxdn_handle_facch3_udch2_soft(&g_opts, &g_state, &failed, 0U);
    send_blocks(payload, total);
    expect_nothing(100U);
}

/* B6: another RAN, another carrier, or the hold time on the decode clock ends the binding. */
static void
test_binding_reception_and_expiry(void) {
    const gps_in g = gps_default();
    uint8_t payload[MAX_PAYLOAD];
    const size_t total = gps_payload(payload, &g, GPS_MSG_BYTES);

    reset_fixture();
    send_header(100U, total, 1);
    g_ran = 6U;
    send_blocks(payload, total);
    expect_nothing(100U);

    reset_fixture();
    send_header(100U, total, 1);
    g_state.carrier_seq++;
    send_blocks(payload, total);
    expect_nothing(100U);

    reset_fixture();
    send_header(100U, total, 1);
    advance_ms(NXDN_DATA_HEADER_HOLD_MS + 1U);
    send_blocks(payload, total);
    expect_nothing(100U);

    /* Within the hold time on the same reception and carrier: the header's own payload. */
    reset_fixture();
    send_header(100U, total, 1);
    advance_ms(NXDN_DATA_HEADER_HOLD_MS - 500U);
    send_blocks(payload, total);
    dsd_position_fix r;
    expect_one(100U, 100000000, 225000000, &r);
}

/* Every channel that carries a RAN notes it in the received identity, whose epoch a different RAN moves: a SACCH
 * (non-superframe and superframe), a CAC and a FACCH2/UDCH (above) all end the binding with another RAN, and keep it
 * with the same one. */
static void
deliver_ran(int path, uint8_t ran) {
    uint8_t trellis[208];
    uint8_t m_data[32];
    DSD_MEMSET(trellis, 0, sizeof trellis);
    DSD_MEMSET(m_data, 0, sizeof m_data);
    put_bits(trellis, 2, ran, 6);
    if (path == 0) {
        g_state.nxdn_sacch_non_superframe = 1;
        nxdn_handle_sacch(&g_opts, &g_state, trellis, m_data, 0U, 0U);
    } else if (path == 1) {
        g_state.nxdn_sacch_non_superframe = 0;
        put_bits(trellis, 0, 3U, 2); /* the first of four segments */
        nxdn_handle_sacch(&g_opts, &g_state, trellis, m_data, 0U, 0U);
    } else {
        nxdn_handle_cac(&g_opts, &g_state, trellis, m_data, 0U);
    }
}

static void
test_binding_ran_from_every_channel(void) {
    static const char* names[] = {"SACCH non-superframe", "SACCH superframe", "CAC"};
    for (int path = 0; path < 3; path++) {
        for (int other = 0; other <= 1; other++) {
            reset_fixture();
            g_state.nxdn_confirmed = 1U; /* a SACCH's RAN counts once the transmission is confirmed */
            send_header(100U, 60U, 1);
            CHECK(nxdn_data_call_header_bound(&g_state));
            deliver_ran(path, other ? 6U : g_ran);
            if (nxdn_data_call_header_bound(&g_state) != !other) {
                DSD_FPRINTF(stderr, "FAIL: %s RAN %s: binding %s\n", names[path], other ? "change" : "repeat",
                            other ? "kept" : "lost");
                g_failures++;
            }
        }
    }
}

/* NMEA in a data call (format 0x06): the sentences end where the CRC32 begins; the bound header names the source. */
static void
test_nmea_data_call(void) {
    reset_fixture();
    static const char body[] = "GPRMC,120000,A,1000.0000,N,02000.0000,E,,,150626,,,A";
    char text[96];
    uint8_t sum = 0U;
    for (const char* p = body; *p != '\0'; p++) {
        sum ^= (uint8_t)*p;
    }
    DSD_SNPRINTF(text, sizeof text, "$%s*%02X\r\n", body, sum);
    uint8_t payload[MAX_PAYLOAD];
    DSD_MEMSET(payload, 0, sizeof payload);
    payload[0] = 0x06U;
    const size_t n = strlen(text);
    DSD_MEMCPY(payload + 1, text, n);
    const size_t total = 1U + n + 4U;
    seal(payload, total);
    send_header(300U, total, 1);
    send_blocks(payload, total);
    dsd_position_fix recs[4];
    CHECK(feed(recs, 4) == 1);
    CHECK(recs[0].kind == DSD_POS_NMEA && recs[0].source_id == 300U);
    CHECK(recs[0].lat_e7 == 100000000 && recs[0].lon_e7 == 200000000);
    CHECK((recs[0].flags & DSD_POS_FLAG_FORMAT_PROVISIONAL) == 0U);
    CHECK(strncmp(g_history[0].Event_History_Items[1].text_message, text, n - 2U) == 0);
    CHECK(strlen(g_history[0].Event_History_Items[1].text_message) == n - 2U);
}

int
main(void) {
    int fd = dsd_test_mkstemp(g_lpath, sizeof g_lpath, "nxdn_gps_dispatch");
    if (fd < 0) {
        return 101;
    }
    (void)dsd_close(fd);
    g_now_ns = 1000000000ULL;
    dsd_decode_clock_use_test(g_now_ns);

    test_degree_minute_packing();
    test_short_report();
    test_component_bounds();
    test_date_time_bounds();
    test_heading_bounds();
    test_binding_failed_replacement_header();
    test_binding_rejected_header_and_udch2();
    test_binding_reception_and_expiry();
    test_binding_ran_from_every_channel();
    test_nmea_data_call();

    dsd_state_ext_free_all(&g_state);
    (void)remove(g_lpath);
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "NXDN_GPS_DISPATCH: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "NXDN_GPS_DISPATCH: PASS\n");
    return 0;
}
