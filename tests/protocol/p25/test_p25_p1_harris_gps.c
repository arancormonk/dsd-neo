// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 1 L3Harris GPS updates through the real link control dispatch, decoder, position admission, history
 * writer and location file (issue #800).
 *
 * TIA-102.AABF-D-1 §7.3.35-36: Part A (LCO 42, or 44 for even-numbered updates) carries the latitude and the longitude
 * fraction and minutes; Part B (LCO 43, or 45) the longitude degrees, a 17-bit time of day (the T bit is Part B octet 8
 * bit 7), a 12-bit heading in tenths of a degree, a 12-bit velocity in tenths of a knot, the emergency bit, a 2-bit
 * quality and the satellites in view. A report is checked only when both halves passed a hard RS decode, pair by parity
 * on one call epoch (never outside a call) and source within the pairing window, and carry quality %01 or %10; its
 * source only when a hard-decoded voice-user link control of the same ACTIVE call named it. All coordinates and IDs are
 * made up.
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_lcw.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int g_fail;

static void
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", tag);
        g_fail = 1;
    }
}

/* 2026-01-01T10:00:00Z on the TEST decode clock: the receive day starts at DAY_START_S. */
#define DAY_START_S INT64_C(1767225600)
#define T0_S        (DAY_START_S + INT64_C(36000))
#define NS_PER_S    UINT64_C(1000000000)
#define SRC_A       3001U
#define SRC_B       3002U
#define TG_A        100U
#define BODY_OCTETS DSD_HARRIS_GPS_BODY_OCTETS
#define PART_OCTETS 7
#define LCO_A_ODD   0x2A
#define LCO_B_ODD   0x2B
#define LCO_A_EVEN  0x2C
#define LCO_B_EVEN  0x2D
#define MFID_HARRIS 0xA4

typedef struct {
    dsd_opts* opts;
    dsd_state* st;
    char lrrp_path[DSD_TEST_PATH_MAX];
    uint64_t feed_seq;
} fixture;

static void
set_clock_s(int64_t s) {
    dsd_decode_clock_test_set_ns((uint64_t)s * NS_PER_S);
}

static void
set_clock_ms(int64_t s, uint64_t ms) {
    dsd_decode_clock_test_set_ns(((uint64_t)s * NS_PER_S) + (ms * UINT64_C(1000000)));
}

static void
fixture_open(fixture* f) {
    DSD_MEMSET(f, 0, sizeof(*f));
    dsd_decode_clock_use_test((uint64_t)T0_S * NS_PER_S);
    f->opts = (dsd_opts*)calloc(1, sizeof(*f->opts));
    f->st = (dsd_state*)calloc(1, sizeof(*f->st));
    Event_History_I* history = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    if (!f->opts || !f->st || !history) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        exit(2);
    }
    f->st->event_history_s = history;
    init_event_history(&history[0], 0, DSD_EVENT_HISTORY_LEN);
    init_event_history(&history[1], 0, DSD_EVENT_HISTORY_LEN);
    f->st->synctype = DSD_SYNC_P25P1_POS;
    f->st->lastsynctype = DSD_SYNC_P25P1_POS;
    const int fd = dsd_test_mkstemp(f->lrrp_path, sizeof f->lrrp_path, "harris_gps_lrrp");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "mkstemp failed\n");
        exit(2);
    }
    (void)dsd_close(fd);
    f->opts->lrrp_file_output = 1;
    DSD_SNPRINTF(f->opts->lrrp_out_file, sizeof f->opts->lrrp_out_file, "%s", f->lrrp_path);
    if (dsd_position_feed_start(f->st) != 0) {
        DSD_FPRINTF(stderr, "feed start failed\n");
        exit(2);
    }
}

static void
fixture_close(fixture* f) {
    dsd_state_ext_free_all(f->st);
    free(f->st->event_history_s);
    free(f->st);
    free(f->opts);
    (void)remove(f->lrrp_path);
}

/* Dispatch one link control (format, MFID, octets 2-8) as LDU1 does; @p soft marks a soft-erasure RS recovery. */
static void
send_lc(fixture* f, uint8_t format, uint8_t mfid, const uint8_t octets[PART_OCTETS], int soft) {
    uint8_t bytes[9] = {format, mfid, octets[0], octets[1], octets[2], octets[3], octets[4], octets[5], octets[6]};
    uint8_t lcw[72];
    for (int i = 0; i < 72; i++) {
        lcw[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1U);
    }
    f->st->p25_p1_lcw_soft = (uint8_t)(soft != 0);
    p25_lcw(f->opts, f->st, lcw, 0);
    f->st->p25_p1_lcw_soft = 0U;
}

/* A Group Voice Channel User link control (hard decode) opens the call of @p src on @p tg. */
static uint64_t
start_call(fixture* f, uint16_t tg, uint32_t src) {
    const uint8_t user[PART_OCTETS] = {
        0x00, 0x00, (uint8_t)(tg >> 8), (uint8_t)tg, (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src};
    send_lc(f, 0x00, 0x00, user, 0);
    dsd_call_snapshot call;
    if (dsd_call_state_get(f->st, 0U, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE || call.ota_source_id != src) {
        DSD_FPRINTF(stderr, "call did not start\n");
        exit(2);
    }
    return call.epoch;
}

static void
end_call(fixture* f) {
    (void)dsd_call_state_end_ex(f->st, 0U, 1.0, DSD_CALL_END_TERMINATOR);
    dsd_event_sync_slot(f->opts, f->st, 0U);
}

/* Send both halves of @p body: Part A with @p lco_a, Part B with @p lco_b. */
static void
send_update(fixture* f, uint8_t lco_a, uint8_t lco_b, const uint8_t body[BODY_OCTETS]) {
    send_lc(f, lco_a, MFID_HARRIS, body, 0);
    send_lc(f, lco_b, MFID_HARRIS, body + PART_OCTETS, 0);
}

/* A body: latitude and longitude as degrees, minutes and ten-thousandths (negative degrees south/west), then the
 * Part B fields. */
typedef struct {
    int lat_deg, lat_min, lat_frac, lon_deg, lon_min, lon_frac;
    uint32_t time_s;
    uint16_t heading;
    uint16_t velocity;
    uint8_t emergency, quality, sats;
} body_fields;

static void
make_body(const body_fields* b, uint8_t out[BODY_OCTETS]) {
    const int lat_abs = b->lat_deg < 0 ? -b->lat_deg : b->lat_deg;
    const int lon_abs = b->lon_deg < 0 ? -b->lon_deg : b->lon_deg;
    out[0] = (uint8_t)(b->lat_frac >> 8);
    out[1] = (uint8_t)b->lat_frac;
    out[2] = (uint8_t)((b->lat_deg < 0 ? 0x80 : 0x00) | (b->lat_min & 0x7F));
    out[3] = (uint8_t)lat_abs;
    out[4] = (uint8_t)(b->lon_frac >> 8);
    out[5] = (uint8_t)b->lon_frac;
    out[6] = (uint8_t)((b->lon_deg < 0 ? 0x80 : 0x00) | (b->lon_min & 0x7F));
    out[7] = (uint8_t)lon_abs;
    out[8] = (uint8_t)(b->time_s >> 8);
    out[9] = (uint8_t)b->time_s;
    out[10] = (uint8_t)b->heading;
    out[11] = (uint8_t)b->velocity;
    out[12] = (uint8_t)(((b->velocity >> 8) & 0x0F) << 4 | ((b->heading >> 8) & 0x0F));
    out[13] = (uint8_t)(((b->time_s >> 16) & 1U) << 7 | (b->emergency & 1U) << 6 | (b->quality & 3U) << 4
                        | (b->sats & 0x0FU));
}

/* 22°30' N, 45° E, 02:00:00, heading 90.0, at rest, quality %01, 5 satellites. */
static body_fields
fields_default(void) {
    body_fields b = {22, 30, 0, 45, 0, 0, 7200U, 900U, 0U, 0U, 1U, 5U};
    return b;
}

/* Rows in the location file, and the last one's source, latitude, longitude, speed and azimuth columns. */
static int
lrrp_rows(const fixture* f, char last[256]) {
    FILE* fp = dsd_fopen_private(f->lrrp_path, "r");
    last[0] = '\0';
    if (fp == NULL) {
        return 0;
    }
    int rows = 0;
    char line[256];
    while (fgets(line, sizeof line, fp) != NULL) {
        rows++;
        DSD_SNPRINTF(last, 256, "%s", line);
    }
    fclose(fp);
    return rows;
}

static int
last_row_is(const fixture* f, const char* src, const char* lat, const char* lon, int speed, int azimuth) {
    char last[256];
    if (lrrp_rows(f, last) == 0) {
        return 0;
    }
    char want[160];
    DSD_SNPRINTF(want, sizeof want, "\t%s\t%s\t%s\t%d\t %d\t", src, lat, lon, speed, azimuth);
    const int ok = strstr(last, want) != NULL;
    if (!ok) {
        DSD_FPRINTF(stderr, "location row: %s (want ...%s...)\n", last, want);
    }
    return ok;
}

/* The newest admitted record the feed kept since the last call; 0 when it kept none. */
static int
feed_next(fixture* f, dsd_position_fix* out) {
    dsd_position_feed_meta meta;
    const int n = dsd_position_feed_copy_since(f->st, f->feed_seq, out, 1, &meta);
    if (n > 0) {
        f->feed_seq = meta.last_seq;
    }
    return n > 0;
}

static const Event_History*
row0(const fixture* f) {
    return &f->st->event_history_s[0].Event_History_Items[0];
}

/* The spec's own example (AABF-D-1 field map; coordinates made up): quality %00 is no fix; the same update with
 * quality %01 is 22.5, 45.0 at 02:00:00 of the receive day, heading 90.0 degrees. */
static void
test_spec_vector(void) {
    fixture f;
    fixture_open(&f);
    const uint64_t epoch = start_call(&f, TG_A, SRC_A);
    const uint8_t no_fix[BODY_OCTETS] = {0x00, 0x00, 0x1E, 0x16, 0x00, 0x00, 0x00,
                                         0x2D, 0x1C, 0x20, 0x84, 0x00, 0x03, 0x05};
    send_update(&f, LCO_A_ODD, LCO_B_ODD, no_fix);
    char last[256];
    dsd_position_fix fix;
    expect_true("quality %00: no location row", lrrp_rows(&f, last) == 0);
    expect_true("quality %00: no feed record", !feed_next(&f, &fix));
    expect_true("quality %00: no history position", row0(&f)->pos.valid == 0U && row0(&f)->gps_s[0] == '\0');

    uint8_t with_fix[BODY_OCTETS];
    DSD_MEMCPY(with_fix, no_fix, sizeof with_fix);
    with_fix[13] = 0x15;
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "harris_gps") != 0) {
        exit(2);
    }
    send_update(&f, LCO_A_ODD, LCO_B_ODD, with_fix);
    dsd_test_capture_stderr_end(&cap);
    char out[4096];
    if (dsd_test_capture_stderr_read(&cap, out, sizeof out) != 0) {
        exit(2);
    }
    expect_true("printed time of day", strstr(out, "TIME: 02:00:00 UTC") != NULL);
    expect_true("printed heading", strstr(out, "HEADING: 090.0") != NULL);
    expect_true("printed satellites", strstr(out, "SATS: 5") != NULL);
    expect_true("fix: location row", last_row_is(&f, "00003001", "22.500000", "45.000000", 0, 90));
    expect_true("fix: feed record", feed_next(&f, &fix));
    expect_true("fix: record fields", fix.kind == DSD_POS_P25_HARRIS && fix.lat_e7 == 225000000
                                          && fix.lon_e7 == 450000000 && fix.heading_ddeg == 900U && fix.speed_dkmh == 0U
                                          && fix.sats == 5U && fix.alt_dm == DSD_POSITION_ALT_UNKNOWN
                                          && fix.err_m == DSD_POSITION_ERR_UNKNOWN);
    expect_true("fix: receive-day time", fix.fix_time_s == DAY_START_S + 7200);
    expect_true("fix: checked identity", fix.source_id == SRC_A && fix.target_id == TG_A
                                             && (fix.flags & DSD_POS_FLAG_TARGET_GROUP) != 0U
                                             && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) == 0U);
    expect_true("fix: history row", row0(&f)->pos.valid == 1U && row0(&f)->pos.rev == fix.rev
                                        && strstr(row0(&f)->gps_s, "(22.500000") != NULL
                                        && strstr(row0(&f)->gps_s, "45.000000") != NULL
                                        && strstr(row0(&f)->gps_s, ") 090.0") != NULL);
    dsd_call_snapshot call;
    expect_true("fix: the call's row", dsd_call_state_get(f.st, 0U, &call) > 0 && call.epoch == epoch);
    fixture_close(&f);
}

/* D14: LCO 44/45 carry the even-numbered updates with the same layout. Pairs never cross parity. */
static void
test_parity(void) {
    fixture f;
    fixture_open(&f);
    (void)start_call(&f, TG_A, SRC_A);
    body_fields b = fields_default();
    uint8_t body[BODY_OCTETS];
    make_body(&b, body);
    char last[256];
    dsd_position_fix fix;

    send_update(&f, LCO_A_EVEN, LCO_B_EVEN, body);
    expect_true("44/45: location row", last_row_is(&f, "00003001", "22.500000", "45.000000", 0, 90));
    expect_true("44/45: feed record", feed_next(&f, &fix) && fix.lat_e7 == 225000000);

    send_update(&f, LCO_A_ODD, LCO_B_EVEN, body);
    expect_true("42 then 45: nothing", lrrp_rows(&f, last) == 1 && !feed_next(&f, &fix));
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
    expect_true("42 then 45: Part A dropped", lrrp_rows(&f, last) == 1);
    send_update(&f, LCO_A_EVEN, LCO_B_ODD, body);
    expect_true("44 then 43: nothing", lrrp_rows(&f, last) == 1 && !feed_next(&f, &fix));

    // A newer Part A replaces the pending one (a later time of day, so the report is no repeat of the first).
    b.time_s = 7201U;
    uint8_t newer[BODY_OCTETS];
    make_body(&b, newer);
    b.lat_deg = 10;
    b.lat_min = 0;
    uint8_t older[BODY_OCTETS];
    make_body(&b, older);
    send_lc(&f, LCO_A_ODD, MFID_HARRIS, older, 0);
    send_update(&f, LCO_A_ODD, LCO_B_ODD, newer);
    expect_true("newer Part A wins", lrrp_rows(&f, last) == 2 && feed_next(&f, &fix) && fix.lat_e7 == 225000000);
    fixture_close(&f);
}

/* N3: a talker alias link control between the halves leaves Part A alone. */
static void
test_alias_between_halves(void) {
    fixture f;
    fixture_open(&f);
    (void)start_call(&f, TG_A, SRC_A);
    body_fields b = fields_default();
    uint8_t body[BODY_OCTETS];
    make_body(&b, body);
    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
    const uint8_t alias[PART_OCTETS] = {0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42};
    send_lc(&f, 0x32, MFID_HARRIS, alias, 0);
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
    expect_true("alias between the halves: fix kept", last_row_is(&f, "00003001", "22.500000", "45.000000", 0, 90));
    fixture_close(&f);
}

/* N2: an update heard after the call ended belongs to no call: with nothing binding the halves to one transmission
 * of one source (spec 2.2) they never pair, and neither do halves split by the end. */
static void
test_ended_call(void) {
    fixture f;
    fixture_open(&f);
    (void)start_call(&f, TG_A, SRC_A);
    end_call(&f);
    body_fields b = fields_default();
    uint8_t body[BODY_OCTETS];
    make_body(&b, body);
    send_update(&f, LCO_A_ODD, LCO_B_ODD, body);
    dsd_position_fix fix;
    char last[256];
    expect_true("after the end: no location row", lrrp_rows(&f, last) == 0);
    expect_true("after the end: no feed record", !feed_next(&f, &fix));
    expect_true("after the end: the ended call's row untouched",
                f.st->event_history_s[0].Event_History_Items[1].gps_s[0] == '\0'
                    && f.st->event_history_s[0].Event_History_Items[1].pos.valid == 0U);

    (void)start_call(&f, TG_A, SRC_B);
    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
    end_call(&f);
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
    expect_true("Part A of an ended call: no fix", lrrp_rows(&f, last) == 0 && !feed_next(&f, &fix));
    fixture_close(&f);
}

/* Another protocol's call still open on the slot (a DMR call whose end was missed) binds no P25 GPS halves: the
 * L3Harris update has no P25 call to pair within, so it gives no position (issue #800). */
static void
test_other_protocol_call(void) {
    fixture f;
    fixture_open(&f);
    dsd_call_observation dmr = dsd_call_observation_data(DSD_SYNC_DMR_BS_VOICE_POS, 0U, SRC_A, TG_A);
    dmr.kind = DSD_CALL_KIND_GROUP_VOICE;
    expect_true("DMR call opens", dsd_call_state_observe(f.st, &dmr, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(f.opts, f.st, 0U);
    body_fields b = fields_default();
    uint8_t body[BODY_OCTETS];
    make_body(&b, body);
    send_update(&f, LCO_A_ODD, LCO_B_ODD, body);
    dsd_position_fix fix;
    char last[256];
    expect_true("DMR call: no location row", lrrp_rows(&f, last) == 0);
    expect_true("DMR call: no feed record", !feed_next(&f, &fix));
    expect_true("DMR call: its row untouched", row0(&f)->pos.valid == 0U && row0(&f)->gps_s[0] == '\0');
    fixture_close(&f);
}

/* Dispatch a link control as the TDULC that ended @p closing's call does (processTDULC() captures it first). */
static void
send_terminator_lc(fixture* f, uint8_t format, uint8_t mfid, const uint8_t octets[PART_OCTETS],
                   const dsd_event_closing_call* closing) {
    uint8_t bytes[9] = {format, mfid, octets[0], octets[1], octets[2], octets[3], octets[4], octets[5], octets[6]};
    uint8_t lcw[72];
    for (int i = 0; i < 72; i++) {
        lcw[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1U);
    }
    p25_lcw_from_tdulc(f->opts, f->st, lcw, 0, closing);
}

/* The call's committed row: the one that names @p src, wherever the end pushed it. */
static const Event_History*
row_of(const fixture* f, uint32_t src) {
    for (int i = 0; i < 4; i++) {
        const Event_History* item = &f->st->event_history_s[0].Event_History_Items[i];
        if (item->source_id == src) {
            return item;
        }
    }
    return NULL;
}

/* The event log's last line, without its line end; 0 when the log has none. */
static int
event_log_last_line(const char* path, char last[512]) {
    FILE* fp = dsd_fopen_private(path, "r");
    last[0] = '\0';
    if (fp == NULL) {
        return 0;
    }
    int lines = 0;
    char line[512];
    while (fgets(line, sizeof line, fp) != NULL) {
        lines++;
        line[strcspn(line, "\r\n")] = '\0';
        DSD_SNPRINTF(last, 512, "%s", line);
    }
    fclose(fp);
    return lines;
}

/* N2 terminator exception: the TDULC ends the call before its own link control is dispatched, and an L3Harris Part B
 * or a Motorola APX report in it belongs to that call: its ENDED row takes the position, -L keeps its checked source,
 * and the event log, which wrote the row's line at the end, gains the row's GPS line with the row's own stamp. Through
 * the real decoders, admission, history writer, event log and location file. */
static void
test_terminator(void) {
    for (int apx = 0; apx <= 1; apx++) {
        fixture f;
        fixture_open(&f);
        char event_path[DSD_TEST_PATH_MAX];
        const int event_fd = dsd_test_mkstemp(event_path, sizeof event_path, "harris_gps_events");
        if (event_fd < 0) {
            DSD_FPRINTF(stderr, "mkstemp failed\n");
            exit(2);
        }
        (void)dsd_close(event_fd);
        DSD_SNPRINTF(f.opts->event_out_file, sizeof f.opts->event_out_file, "%s", event_path);
        const uint64_t epoch = start_call(&f, TG_A, SRC_A);
        body_fields b = fields_default();
        uint8_t body[BODY_OCTETS];
        make_body(&b, body);
        if (!apx) {
            send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
        }
        dsd_event_closing_call closing;
        expect_true("terminator: call captured", dsd_event_capture_closing_call(f.st, 0U, &closing) == 1);
        end_call(&f);
        dsd_call_snapshot call;
        expect_true("terminator: call ended", dsd_call_state_get(f.st, 0U, &call) > 0
                                                  && call.phase == DSD_CALL_PHASE_ENDED && call.epoch == epoch);
        // APX: latitude magnitude 0x200000 (22.500003 N), longitude magnitude 0x300000 (67.500008 E).
        const uint8_t apx_lc[PART_OCTETS] = {0x00, 0x20, 0x00, 0x00, 0x30, 0x00, 0x00};
        if (apx) {
            send_terminator_lc(&f, 0x06, 0x90, apx_lc, &closing);
        } else {
            send_terminator_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, &closing);
        }
        const char* tag = apx ? "terminator APX" : "terminator Harris";
        const char* lat = apx ? "22.500003" : "22.500000";
        const char* lon = apx ? "67.500008" : "45.000000";
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "%s: -L keeps the closing call's source", tag);
        expect_true(label, last_row_is(&f, "00003001", lat, lon, 0, apx ? 0 : 90));
        dsd_position_fix fix;
        DSD_SNPRINTF(label, sizeof label, "%s: checked record", tag);
        expect_true(label, feed_next(&f, &fix) && fix.source_id == SRC_A && fix.target_id == TG_A
                               && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) == 0U);
        const Event_History* item = row_of(&f, SRC_A);
        DSD_SNPRINTF(label, sizeof label, "%s: the ended call's row takes it", tag);
        expect_true(label, item != NULL && item->pos.valid == 1U && item->pos.rev == fix.rev
                               && strstr(item->gps_s, lat) != NULL && strstr(item->gps_s, lon) != NULL);
        char last[512];
        char want[512];
        DSD_SNPRINTF(label, sizeof label, "%s: the event log gains the row's GPS line with its stamp", tag);
        if (item != NULL) {
            // One "GPS:" label: the APX text carries its own.
            DSD_SNPRINTF(want, sizeof want, "%.19s %s%s ", item->event_string, apx ? "" : "GPS: ", item->gps_s);
        } else {
            want[0] = '\0';
        }
        const int lines = event_log_last_line(event_path, last);
        expect_true(label, item != NULL && lines >= 2 && strcmp(last, want) == 0);
        if (strcmp(last, want) != 0) {
            DSD_FPRINTF(stderr, "event log: %s (want %s)\n", last, want);
        }
        (void)remove(event_path);
        fixture_close(&f);
    }
}

/* The attribution rule's reception (issue #800, dsd_position_attribution_grant()): a checked NID can change the NAC
 * without ending the call, and the voice-user identity named under the old NAC is not the sender of a GPS link control
 * heard under the new one, in the call (L3Harris pair, Motorola APX) or in its terminator. The position still counts,
 * anonymous; under an unchanged NAC the same sequence names the call's source. */
static void
test_reception_change(void) {
    for (int terminator = 0; terminator <= 1; terminator++) {
        for (int apx = 0; apx <= 1; apx++) {
            for (int change = 0; change <= 1; change++) {
                fixture f;
                fixture_open(&f);
                dsd_rxid_note_access(f.st, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
                (void)start_call(&f, TG_A, SRC_A);
                if (change) {
                    dsd_rxid_note_access(f.st, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x294U);
                }
                body_fields b = fields_default();
                uint8_t body[BODY_OCTETS];
                make_body(&b, body);
                const uint8_t apx_lc[PART_OCTETS] = {0x00, 0x20, 0x00, 0x00, 0x30, 0x00, 0x00};
                if (!apx) {
                    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
                }
                if (terminator) {
                    dsd_event_closing_call closing;
                    expect_true("reception: call captured", dsd_event_capture_closing_call(f.st, 0U, &closing) == 1);
                    end_call(&f);
                    if (apx) {
                        send_terminator_lc(&f, 0x06, 0x90, apx_lc, &closing);
                    } else {
                        send_terminator_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, &closing);
                    }
                } else if (apx) {
                    send_lc(&f, 0x06, 0x90, apx_lc, 0);
                } else {
                    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
                }
                char label[160];
                DSD_SNPRINTF(label, sizeof label, "%s %s, %s NAC", terminator ? "terminator" : "in-call",
                             apx ? "APX" : "Harris", change ? "another" : "the same");
                dsd_position_fix fix;
                const int kept = feed_next(&f, &fix);
                const uint32_t want = change ? 0U : SRC_A;
                if (!kept || fix.source_id != want || fix.target_id != (change ? 0U : TG_A)
                    || ((fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U) != (change != 0)) {
                    DSD_FPRINTF(stderr, "FAIL: %s: kept %d source %u target %u flags %04X\n", label, kept,
                                kept ? fix.source_id : 0U, kept ? fix.target_id : 0U, kept ? fix.flags : 0U);
                    g_fail = 1;
                }
                char last[256];
                expect_true(label,
                            lrrp_rows(&f, last) == 1 && strstr(last, change ? "\t00000000\t" : "\t00003001\t") != NULL);
                fixture_close(&f);
            }
        }
    }
}

/* Issue #800, fail closed: both P25 phases send one system's identity, but a transmission of the other phase is another
 * reception. A Phase 1 call's Part A, then Phase 2 frames on the carrier (the Phase 1 terminator missed, the call left
 * open), then Phase 1 again within the pairing window: a Part B pairs with no Part A heard before the change, so no
 * fix; and the voice user named before the change no longer names a later update's sender, which counts only
 * anonymous. Without the Phase 2 frames the same sequence gives radio 3001's fix. */
static void
test_phase_change(void) {
    for (int change = 0; change <= 1; change++) {
        fixture f;
        fixture_open(&f);
        dsd_rxid_note_sync(f.st, DSD_SYNC_P25P1_POS);
        dsd_rxid_note_access(f.st, DSD_RXID_FAMILY_P25, (uint8_t)DSD_ACCESS_CODE_NAC, 0x293U);
        (void)start_call(&f, TG_A, SRC_A);
        body_fields b = fields_default();
        uint8_t body[BODY_OCTETS];
        make_body(&b, body);
        send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
        if (change) {
            f.st->lastsynctype = DSD_SYNC_P25P2_POS;
            dsd_rxid_note_sync(f.st, DSD_SYNC_P25P2_POS);
            f.st->lastsynctype = DSD_SYNC_P25P1_POS;
            dsd_rxid_note_sync(f.st, DSD_SYNC_P25P1_POS);
        }
        set_clock_ms(T0_S, 720U);
        send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
        dsd_position_fix fix;
        const int kept = feed_next(&f, &fix);
        char last[256];
        const int rows = lrrp_rows(&f, last);
        if (change) {
            expect_true("phase: a Part A of the other side of a phase change pairs with nothing", !kept && rows == 0);
        } else {
            expect_true("phase: the same phase pairs", kept && fix.source_id == SRC_A && rows == 1);
        }
        /* A whole update after the change: its position counts, its sender only without the change. */
        set_clock_ms(T0_S + 40, 0U);
        b.lat_min = 31;
        make_body(&b, body);
        send_update(&f, LCO_A_EVEN, LCO_B_EVEN, body);
        const int later = feed_next(&f, &fix);
        if (change) {
            expect_true("phase: a later update is anonymous",
                        later && fix.source_id == 0U && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U);
        } else {
            expect_true("phase: a later update names the talker", later && fix.source_id == SRC_A);
        }
        fixture_close(&f);
    }
}

/* Occurrences of @p needle in @p hay. */
static int
count_of(const char* hay, const char* needle) {
    int n = 0;
    for (const char* at = strstr(hay, needle); at != NULL; at = strstr(at + 1, needle)) {
        n++;
    }
    return n;
}

/* A position sent during the call (Motorola APX, L3Harris) is on the call's row when the call ends: the event log's
 * GPS line for it, written with the row, carries exactly one "GPS:" label whatever the stored text starts with. */
static void
test_event_log_one_label(void) {
    for (int apx = 0; apx <= 1; apx++) {
        fixture f;
        fixture_open(&f);
        char event_path[DSD_TEST_PATH_MAX];
        const int event_fd = dsd_test_mkstemp(event_path, sizeof event_path, "harris_gps_label");
        if (event_fd < 0) {
            DSD_FPRINTF(stderr, "mkstemp failed\n");
            exit(2);
        }
        (void)dsd_close(event_fd);
        DSD_SNPRINTF(f.opts->event_out_file, sizeof f.opts->event_out_file, "%s", event_path);
        (void)start_call(&f, TG_A, SRC_A);
        if (apx) {
            const uint8_t apx_lc[PART_OCTETS] = {0x00, 0x20, 0x00, 0x00, 0x30, 0x00, 0x00};
            send_lc(&f, 0x06, 0x90, apx_lc, 0);
        } else {
            body_fields b = fields_default();
            uint8_t body[BODY_OCTETS];
            make_body(&b, body);
            send_update(&f, LCO_A_ODD, LCO_B_ODD, body);
        }
        end_call(&f);
        const char* tag = apx ? "APX" : "Harris";
        char label[96];
        const Event_History* item = row_of(&f, SRC_A);
        DSD_SNPRINTF(label, sizeof label, "%s in the call: its row takes it", tag);
        expect_true(label, item != NULL && item->pos.valid == 1U && item->gps_s[0] != '\0');
        FILE* fp = dsd_fopen_private(event_path, "r");
        int gps_lines = 0;
        int labels = 0;
        char line[512];
        while (fp != NULL && fgets(line, sizeof line, fp) != NULL) {
            if (strstr(line, apx ? "22.500003" : "22.500000") != NULL) {
                gps_lines++;
                labels += count_of(line, "GPS:");
            }
        }
        if (fp != NULL) {
            fclose(fp);
        }
        DSD_SNPRINTF(label, sizeof label, "%s in the call: one event log GPS line with one label", tag);
        expect_true(label, gps_lines == 1 && labels == 1);
        if (gps_lines != 1 || labels != 1) {
            DSD_FPRINTF(stderr, "%s: %d GPS line(s), %d label(s)\n", tag, gps_lines, labels);
        }
        (void)remove(event_path);
        fixture_close(&f);
    }
}

/* Only hard-decoded halves within the window; a source the link control did not name is unverified. */
static void
test_checks(void) {
    fixture f;
    fixture_open(&f);
    (void)start_call(&f, TG_A, SRC_A);
    body_fields b = fields_default();
    uint8_t body[BODY_OCTETS];
    make_body(&b, body);
    char last[256];
    dsd_position_fix fix;

    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 1);
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
    expect_true("soft Part A: no fix", lrrp_rows(&f, last) == 0 && !feed_next(&f, &fix));
    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 1);
    expect_true("soft Part B: no fix", lrrp_rows(&f, last) == 0 && !feed_next(&f, &fix));

    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
    set_clock_s(T0_S + 3);
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
    expect_true("Part A expired: no fix", lrrp_rows(&f, last) == 0 && !feed_next(&f, &fix));
    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
    set_clock_s(T0_S + 4);
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
    expect_true("within the window: fix", lrrp_rows(&f, last) == 1 && feed_next(&f, &fix));
    // The window's edge: 2000 ms pairs, 2001 ms does not (a repeat of the same report, so only -L counts it).
    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
    set_clock_ms(T0_S + 6, 0U);
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
    expect_true("2000 ms: fix", lrrp_rows(&f, last) == 2);
    send_lc(&f, LCO_A_ODD, MFID_HARRIS, body, 0);
    set_clock_ms(T0_S + 8, 1U);
    send_lc(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0);
    expect_true("2001 ms: expired", lrrp_rows(&f, last) == 2);

    // A call whose source no checked voice-user link control named.
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P1_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = TG_A,
        .policy_target_id = TG_A,
        .ota_source_id = SRC_B,
    };
    expect_true("unchecked call", dsd_call_state_observe(f.st, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0);
    dsd_event_sync_slot(f.opts, f.st, 0U);
    send_update(&f, LCO_A_ODD, LCO_B_ODD, body);
    expect_true("unchecked source: row without it", last_row_is(&f, "00000000", "22.500000", "45.000000", 0, 90));
    expect_true("unchecked source: unverified record", feed_next(&f, &fix) && fix.source_id == 0U);

    // A soft-decoded voice-user link control is no checked provenance either.
    const uint8_t user[PART_OCTETS] = {0x00, 0x00, 0x00, (uint8_t)TG_A, 0x00, 0x0B, 0xBB};
    send_lc(&f, 0x00, 0x00, user, 1);
    send_update(&f, LCO_A_ODD, LCO_B_ODD, body);
    expect_true("soft voice user: unverified record", feed_next(&f, &fix) && fix.source_id == 0U);
    fixture_close(&f);
}

/* Run @p body's update with its Part B soft (unchecked) or hard, and return what the console printed in @p out. */
static void
capture_update(fixture* f, const uint8_t body[BODY_OCTETS], int soft_b, char* out, size_t cap_size) {
    dsd_test_capture_stderr cap;
    out[0] = '\0';
    if (dsd_test_capture_stderr_begin(&cap, "harris_gps_unchecked") != 0) {
        exit(2);
    }
    send_lc(f, LCO_A_ODD, MFID_HARRIS, body, 0);
    send_lc(f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, soft_b);
    dsd_test_capture_stderr_end(&cap);
    if (dsd_test_capture_stderr_read(&cap, out, cap_size) != 0) {
        exit(2);
    }
}

/* Issue #800, fail closed: an unchecked report says nothing of its content by default. A hard Part A and a
 * soft-erasure Part B carrying quality %11 (GPS not enabled) print only that the update was unchecked, never the
 * quality; -Z reads it. A checked update prints its quality. */
static void
test_unchecked_report_prints_no_content(void) {
    for (int payload = 0; payload <= 1; payload++) {
        fixture f;
        fixture_open(&f);
        f.opts->payload = payload;
        (void)start_call(&f, TG_A, SRC_A);
        body_fields b = fields_default();
        b.quality = 3U;
        uint8_t body[BODY_OCTETS];
        make_body(&b, body);
        char out[4096];
        capture_update(&f, body, 1, out, sizeof out);
        expect_true(payload ? "unchecked Harris quality: -Z reads it" : "unchecked Harris quality: not read",
                    (strstr(out, "GPS Not Enabled") != NULL) == (payload == 1));
        expect_true("unchecked Harris update: says so", payload == 1 || strstr(out, "not used (unchecked)") != NULL);
        capture_update(&f, body, 0, out, sizeof out);
        expect_true("checked Harris quality: read", strstr(out, "GPS Not Enabled") != NULL);
        fixture_close(&f);
    }
}

/* Dispatch one link control with the RS verdict @p errors (irrecoverable errors), through p25_lcw() or, with @p tdulc,
 * p25_lcw_from_tdulc(); never a soft-erasure retry. */
static void
send_lc_verdict(fixture* f, uint8_t format, uint8_t mfid, const uint8_t octets[PART_OCTETS], uint8_t errors,
                int tdulc) {
    uint8_t bytes[9] = {format, mfid, octets[0], octets[1], octets[2], octets[3], octets[4], octets[5], octets[6]};
    uint8_t lcw[72];
    for (int i = 0; i < 72; i++) {
        lcw[i] = (uint8_t)((bytes[i / 8] >> (7 - (i % 8))) & 1U);
    }
    if (tdulc) {
        p25_lcw_from_tdulc(f->opts, f->st, lcw, errors, NULL);
    } else {
        p25_lcw(f->opts, f->st, lcw, errors);
    }
}

/* A link control whose RS decode left irrecoverable errors is no checked provenance, whichever entry point it comes
 * through: neither a GPS half nor a voice user's identity counts, even with no soft-erasure retry. */
static void
test_uncorrectable_verdict(void) {
    for (int tdulc = 0; tdulc < 2; tdulc++) {
        char label[160];
        fixture f;
        fixture_open(&f);
        (void)start_call(&f, TG_A, SRC_A);
        body_fields b = fields_default();
        uint8_t body[BODY_OCTETS];
        make_body(&b, body);
        char last[256];
        dsd_position_fix fix;

        send_lc_verdict(&f, LCO_A_ODD, MFID_HARRIS, body, 1U, tdulc);
        send_lc_verdict(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 0U, tdulc);
        DSD_SNPRINTF(label, sizeof label, "uncorrectable Part A (tdulc %d): no fix", tdulc);
        expect_true(label, lrrp_rows(&f, last) == 0 && !feed_next(&f, &fix));
        send_lc_verdict(&f, LCO_A_ODD, MFID_HARRIS, body, 0U, tdulc);
        send_lc_verdict(&f, LCO_B_ODD, MFID_HARRIS, body + PART_OCTETS, 1U, tdulc);
        DSD_SNPRINTF(label, sizeof label, "uncorrectable Part B (tdulc %d): no fix", tdulc);
        expect_true(label, lrrp_rows(&f, last) == 0 && !feed_next(&f, &fix));
        fixture_close(&f);
    }

    // A voice user with irrecoverable errors names the call, but not as checked provenance.
    fixture f;
    fixture_open(&f);
    const uint8_t user[PART_OCTETS] = {
        0x00, 0x00, 0x00, (uint8_t)TG_A, (uint8_t)(SRC_A >> 16), (uint8_t)(SRC_A >> 8), (uint8_t)SRC_A};
    send_lc_verdict(&f, 0x00, 0x00, user, 1U, 0);
    body_fields b = fields_default();
    uint8_t body[BODY_OCTETS];
    make_body(&b, body);
    send_update(&f, LCO_A_ODD, LCO_B_ODD, body);
    dsd_position_fix fix;
    expect_true("uncorrectable voice user: unverified record",
                feed_next(&f, &fix) && fix.source_id == 0U && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U);
    fixture_close(&f);
}

/* A hard-decoded voice user whose source field holds a fixed-network ID (0xFFFFFD, 0xFFFFFF) names no subscriber: a
 * report during that call is kept, anonymous, never a track of the reserved ID. */
static void
test_reserved_source(void) {
    static const uint32_t reserved[] = {0xFFFFFDU, 0xFFFFFFU};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        fixture f;
        fixture_open(&f);
        const uint32_t src = reserved[i];
        const uint8_t user[PART_OCTETS] = {
            0x00, 0x00, 0x00, (uint8_t)TG_A, (uint8_t)(src >> 16), (uint8_t)(src >> 8), (uint8_t)src};
        send_lc(&f, 0x00, 0x00, user, 0);
        body_fields b = fields_default();
        uint8_t body[BODY_OCTETS];
        make_body(&b, body);
        send_update(&f, LCO_A_ODD, LCO_B_ODD, body);
        dsd_position_fix fix;
        char label[160];
        DSD_SNPRINTF(label, sizeof label, "reserved source %06X: anonymous record", (unsigned)src);
        expect_true(label, feed_next(&f, &fix) && fix.source_id == 0U && fix.target_id == 0U
                               && (fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U);
        DSD_SNPRINTF(label, sizeof label, "reserved source %06X: row without it", (unsigned)src);
        expect_true(label, last_row_is(&f, "00000000", "22.500000", "45.000000", 0, 90));
        fixture_close(&f);
    }
}

/* A voice-user link control of the call (hard decode unless @p soft) from @p src: Group Voice Channel User (LCO $00,
 * with the Source ID Extension Required flag @p ext), Unit to Unit Voice Channel User (LCO $03, target TG_A), its
 * Extended form (LCO $4A) or the Motorola LCGRGR (MFID $90 LCO $00, supergroup TG_A). */
static void
send_voice_user(fixture* f, uint8_t format, uint8_t mfid, uint32_t src, int ext, int soft) {
    const uint8_t s0 = (uint8_t)(src >> 16);
    const uint8_t s1 = (uint8_t)(src >> 8);
    const uint8_t s2 = (uint8_t)src;
    /* LCO $00 and LCGRGR: service options, reserved + S, group, source. LCO $03 and $4A: service options, target,
       source. */
    const uint8_t group[PART_OCTETS] = {0x00, (uint8_t)(ext ? 0x01 : 0x00), 0x00, (uint8_t)TG_A, s0, s1, s2};
    const uint8_t unit[PART_OCTETS] = {0x00, 0x00, 0x00, (uint8_t)TG_A, s0, s1, s2};
    send_lc(f, format, mfid, (format == 0x03 || format == 0x4A) ? unit : group, soft);
}

/* The GPS link control after the voice user: an L3Harris pair or a Motorola APX report; the record it gave names @p
 * src (0: anonymous). */
static void
expect_gps_names(fixture* f, int apx, uint32_t src, const char* tag) {
    /* Each report 31 s after the last, past the feed's 30 s repeat window, so an equal report is kept again. */
    static int64_t at_s = T0_S;
    at_s += 31;
    set_clock_s(at_s);
    body_fields b = fields_default();
    uint8_t body[BODY_OCTETS];
    make_body(&b, body);
    const uint8_t apx_lc[PART_OCTETS] = {0x00, 0x20, 0x00, 0x00, 0x30, 0x00, 0x00};
    if (apx) {
        send_lc(f, 0x06, 0x90, apx_lc, 0);
    } else {
        send_update(f, LCO_A_ODD, LCO_B_ODD, body);
    }
    dsd_position_fix fix;
    const int kept = feed_next(f, &fix);
    if (!kept || fix.source_id != src || ((fix.flags & DSD_POS_FLAG_SOURCE_UNVERIFIED) != 0U) != (src == 0U)) {
        DSD_FPRINTF(stderr, "FAIL: %s (%s): kept %d source %u flags %04X (want %u)\n", tag, apx ? "APX" : "Harris",
                    kept, kept ? fix.source_id : 0U, kept ? fix.flags : 0U, src);
        g_fail = 1;
    }
    char want[32];
    char last[256];
    DSD_SNPRINTF(want, sizeof want, "\t%08u\t", (unsigned)src);
    expect_true(tag, lrrp_rows(f, last) > 0 && strstr(last, want) != NULL);
}

static dsd_trunk_tune_result
tune_ok(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)state;
    (void)ted_sps;
    (void)request_id;
    if (freq <= 0) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }
    opts->trunk_is_tuned = 1;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_ok(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)state;
    (void)request_id;
    opts->trunk_is_tuned = 0;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

/* Trunking on, following a checked FDMA grant by SRC_A of TG_A (@p private_call: to unit TG_A): the voice-user link
 * controls that follow are the assignment's voice starts. */
static void
follow_fdma_grant(fixture* f, int private_call) {
    enum { CHANNEL = 0x1234 };

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune_ok,
        .tune_to_cc_request = tune_ok,
        .return_to_cc_request = return_ok,
    });
    f->opts->trunk_enable = 1;
    f->opts->trunk_hangtime = 2.0f;
    f->opts->trunk_tune_group_calls = 1;
    f->opts->trunk_tune_private_calls = 1;
    f->st->p25_cc_freq = 851000000L;
    f->st->trunk_chan_map[CHANNEL] = 851500000L;
    f->st->p25_chan_tdma_explicit[1] = 1;
    p25_sm_init_ctx(p25_sm_get_ctx(), f->opts, f->st);
    p25_sm_event_t grant = private_call ? p25_sm_ev_indiv_grant(CHANNEL, 851500000L, (int)TG_A, (int)SRC_A, 0x00)
                                        : p25_sm_ev_group_grant(CHANNEL, 851500000L, (int)TG_A, (int)SRC_A, 0x00);
    grant.identity_checked = P25_SM_IDENTITY_CHECKED_ALL;
    p25_sm_event(p25_sm_get_ctx(), f->opts, f->st, &grant);
    if (p25_sm_get_ctx()->state != P25_SM_TUNED) {
        DSD_FPRINTF(stderr, "grant not followed\n");
        exit(2);
    }
}

/* Issue #800, fail closed: the evidence a hard-decoded voice user gave (p25_p1_lc_attribution) names the sender of a
 * later GPS link control only while no later voice user of the call says otherwise. After SRC_A's checked voice user, a
 * voice user of the same call naming a special address ($FFFFFC-$FFFFFF), a source that is not a complete local
 * address (LCO $00 or LCGRGR with Source ID Extension Required, LCO $4A), or another radio by a soft decode takes it
 * away, whether or not the call keeps SRC_A for display (a trunked assignment does): the next L3Harris or APX report
 * is anonymous until a checked voice user names a radio again; the trunk state machine's assignment holds that source
 * unchecked too. A soft-decoded repeat of SRC_A, or a source of 0 (an
 * omitted field, as the trunk state machine reads it), changes nothing. The private cases open the call with a Unit to
 * Unit Voice Channel User. */
static void
test_voice_user_withdraws_evidence(void) {
    static const struct {
        const char* tag;
        uint8_t format, mfid;
        uint32_t src;
        int ext, soft, keeps;
    } cases[] = {
        {"special FFFFFC", 0x00, 0x00, 0xFFFFFCU, 0, 0, 0},
        {"special FFFFFD", 0x00, 0x00, 0xFFFFFDU, 0, 0, 0},
        {"special FFFFFF", 0x00, 0x00, 0xFFFFFFU, 0, 0, 0},
        {"special FFFFFD soft", 0x00, 0x00, 0xFFFFFDU, 0, 1, 0},
        {"regroup special FFFFFD", 0x00, 0x90, 0xFFFFFDU, 0, 0, 0},
        {"private special FFFFFD", 0x03, 0x00, 0xFFFFFDU, 0, 0, 0},
        {"source id extension required", 0x00, 0x00, SRC_A, 1, 0, 0},
        {"regroup source id extension required", 0x00, 0x90, SRC_A, 1, 0, 0},
        {"private extended", 0x4A, 0x00, SRC_A, 0, 0, 0},
        {"other radio, soft", 0x00, 0x00, SRC_B, 0, 1, 0},
        {"no source", 0x00, 0x00, 0U, 0, 0, 1},
        {"same radio, soft", 0x00, 0x00, SRC_A, 0, 1, 1},
        {"same radio, hard", 0x00, 0x00, SRC_A, 0, 0, 1},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (int k = 0; k < 4; k++) {
            const int apx = k & 1;
            const int trunked = (k & 2) != 0;
            const uint8_t opener = (cases[i].format == 0x03 || cases[i].format == 0x4A) ? 0x03 : 0x00;
            char tag[128];
            DSD_SNPRINTF(tag, sizeof tag, "%s, %s", cases[i].tag, trunked ? "trunked" : "conventional");
            fixture f;
            fixture_open(&f);
            if (trunked) {
                follow_fdma_grant(&f, opener == 0x03);
            }
            send_voice_user(&f, opener, 0x00, SRC_A, 0, 0);
            expect_gps_names(&f, apx, SRC_A, "withdraw: before");
            send_voice_user(&f, cases[i].format, cases[i].mfid, cases[i].src, cases[i].ext, cases[i].soft);
            if (trunked && !cases[i].keeps) {
                /* The trunk state machine reads the same voice user the same way: its source is not checked. */
                char sm_tag[160];
                DSD_SNPRINTF(sm_tag, sizeof sm_tag, "%s: assignment source unchecked", tag);
                expect_true(sm_tag,
                            (p25_sm_get_ctx()->slots[0].identity_checked & P25_SM_IDENTITY_CHECKED_SOURCE) == 0U);
            }
            expect_gps_names(&f, apx, cases[i].keeps ? SRC_A : 0U, tag);
            send_voice_user(&f, opener, 0x00, SRC_A, 0, 0);
            expect_gps_names(&f, apx, SRC_A, "withdraw: named again");
            p25_sm_init_ctx(p25_sm_get_ctx(), f.opts, f.st);
            fixture_close(&f);
        }
    }
}

/* Issue #800, fail closed: on a trunked FDMA call, a checked repeat of the call's grant (same channel and talkgroup)
 * whose source field names a special address ($FFFFFC-$FFFFFF) or a source that is no local address takes the talker
 * away in the trunk state machine's record, though the call keeps SRC_A for display: the voice-user LC evidence of
 * SRC_A goes with it, so the next L3Harris or APX report is anonymous until a checked voice user names a radio again. A
 * repeat naming SRC_A itself changes nothing. */
static void
test_repeat_grant_withdraws_evidence(void) {
    static const struct {
        const char* tag;
        uint32_t src;
        int nonlocal;
        int keeps;
    } cases[] = {
        {"repeat grant, special FFFFFD", 0xFFFFFDU, 0, 0},
        {"repeat grant, special FFFFFC", 0xFFFFFCU, 0, 0},
        {"repeat grant, non-local source", SRC_A, 1, 0},
        {"repeat grant, same talker", SRC_A, 0, 1},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (int apx = 0; apx <= 1; apx++) {
            fixture f;
            fixture_open(&f);
            follow_fdma_grant(&f, 0);
            send_voice_user(&f, 0x00, 0x00, SRC_A, 0, 0);
            expect_gps_names(&f, apx, SRC_A, "repeat grant: before");
            p25_sm_event_t repeat = p25_sm_ev_group_grant(0x1234, 851500000L, (int)TG_A, (int)cases[i].src, 0x00);
            repeat.identity_checked = P25_SM_IDENTITY_CHECKED_ALL;
            repeat.source_nonlocal = (uint8_t)cases[i].nonlocal;
            p25_sm_event(p25_sm_get_ctx(), f.opts, f.st, &repeat);
            dsd_call_snapshot call;
            char tag[128];
            DSD_SNPRINTF(tag, sizeof tag, "%s: call kept with its talker", cases[i].tag);
            expect_true(tag, dsd_call_state_get(f.st, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
                                 && call.ota_source_id == SRC_A);
            expect_gps_names(&f, apx, cases[i].keeps ? SRC_A : 0U, cases[i].tag);
            send_voice_user(&f, 0x00, 0x00, SRC_A, 0, 0);
            expect_gps_names(&f, apx, SRC_A, "repeat grant: named again");
            p25_sm_init_ctx(p25_sm_get_ctx(), f.opts, f.st);
            fixture_close(&f);
        }
    }
}

/* V1, V3, the time field, quality, emergency and speed, through the decoder with a checked context. */
static int
decode(fixture* f, const body_fields* b, dsd_position_fix* fix) {
    uint8_t body[BODY_OCTETS];
    make_body(b, body);
    const dsd_gps_report_ctx ctx = {.protocol = DSD_SYNC_P25P1_POS, .pos_verified = 1U};
    const int admitted = nmea_harris(f->opts, f->st, body, &ctx);
    const int kept = feed_next(f, fix);
    return admitted != 0 && kept;
}

static void
test_fields(void) {
    fixture f;
    fixture_open(&f);
    dsd_position_fix fix;
    body_fields b = fields_default();

    b.lat_min = 60;
    expect_true("V1: 22 deg 60 min refused", !decode(&f, &b, &fix));
    b = fields_default();
    b.lat_frac = 10000;
    expect_true("V1: fraction 10000 refused", !decode(&f, &b, &fix));
    b = fields_default();
    b.lat_deg = 91;
    expect_true("V1: latitude 91 refused", !decode(&f, &b, &fix));
    b = fields_default();
    b.lat_deg = -90;
    b.lat_min = 0;
    b.lat_frac = 1;
    expect_true("V1: 90 deg S plus a fraction refused", !decode(&f, &b, &fix));
    b.lat_frac = 0;
    expect_true("V1: 90 deg S", decode(&f, &b, &fix) && fix.lat_e7 == -900000000);
    b = fields_default();
    b.lon_deg = -180;
    b.lon_min = 1;
    expect_true("V1: 180 deg 1 min W refused", !decode(&f, &b, &fix));
    b.lon_min = 0;
    expect_true("V1: 180 deg W", decode(&f, &b, &fix) && fix.lon_e7 == -1800000000);
    b = fields_default();
    b.lat_deg = -10;
    b.lat_min = 7;
    b.lat_frac = 5000;
    expect_true("minutes convert exactly", decode(&f, &b, &fix) && fix.lat_e7 == -101250000);
    b.lat_min = 0;
    b.lat_frac = 1;
    expect_true("0.0001 min = 16.67 units rounds up", decode(&f, &b, &fix) && fix.lat_e7 == -100000017);
    b.lat_frac = 2;
    expect_true("0.0002 min = 33.33 units rounds down", decode(&f, &b, &fix) && fix.lat_e7 == -100000033);

    b = fields_default();
    b.heading = 3599U;
    expect_true("V3: heading 359.9", decode(&f, &b, &fix) && fix.heading_ddeg == 3599U);
    expect_true("V3: -L azimuth 359", last_row_is(&f, "00000000", "22.500000", "45.000000", 0, 359));
    b.heading = 3600U;
    expect_true("V3: heading 360.0 unknown", decode(&f, &b, &fix) && fix.heading_ddeg == DSD_POSITION_HEADING_UNKNOWN);
    expect_true("V3: -L azimuth placeholder", last_row_is(&f, "00000000", "22.500000", "45.000000", 0, 0));
    b.heading = 4095U;
    expect_true("V3: heading 409.5 unknown", decode(&f, &b, &fix) && fix.heading_ddeg == DSD_POSITION_HEADING_UNKNOWN);

    b = fields_default();
    b.time_s = 0x1FFFFU;
    expect_true("time $1FFFF unknown", decode(&f, &b, &fix) && fix.fix_time_s == 0);
    b.time_s = 86400U;
    expect_true("time 86400 invalid", decode(&f, &b, &fix) && fix.fix_time_s == 0);
    b.time_s = 86399U;
    expect_true("time 86399 uses the T bit", decode(&f, &b, &fix) && fix.fix_time_s == DAY_START_S + 86399 - 86400);
    set_clock_s(DAY_START_S + 30);
    b.time_s = 86390U;
    expect_true("midnight: yesterday's 23:59:50", decode(&f, &b, &fix) && fix.fix_time_s == DAY_START_S - 10);
    set_clock_s(DAY_START_S + 86395);
    b.time_s = 5U;
    expect_true("midnight: tomorrow's 00:00:05", decode(&f, &b, &fix) && fix.fix_time_s == DAY_START_S + 86405);
    set_clock_s(T0_S);

    b = fields_default();
    b.quality = 0U;
    expect_true("quality %00 refused", !decode(&f, &b, &fix));
    b.quality = 3U;
    expect_true("quality %11 refused", !decode(&f, &b, &fix));
    b.quality = 2U;
    b.emergency = 1U;
    b.velocity = 291U;
    b.sats = 12U;
    expect_true("quality %10 (DGPS) with emergency",
                decode(&f, &b, &fix) && (fix.flags & DSD_POS_FLAG_EMERGENCY) != 0U && fix.sats == 12U);
    expect_true("29.1 kn = 53.9 km/h", fix.speed_dkmh == 539U);
    expect_true("-L speed in km/h", last_row_is(&f, "00000000", "22.500000", "45.000000", 54, 90));

    const dsd_gps_report_ctx unchecked = {.protocol = DSD_SYNC_P25P1_POS};
    uint8_t body[BODY_OCTETS];
    b = fields_default();
    make_body(&b, body);
    char last[256];
    const int rows = lrrp_rows(&f, last);
    expect_true("unchecked message refused", nmea_harris(f.opts, f.st, body, &unchecked) == 0);
    expect_true("unchecked message: no row", lrrp_rows(&f, last) == rows && !feed_next(&f, &fix));
    fixture_close(&f);
}

int
main(void) {
    test_other_protocol_call();
    test_spec_vector();
    test_parity();
    test_alias_between_halves();
    test_ended_call();
    test_terminator();
    test_reception_change();
    test_phase_change();
    test_event_log_one_label();
    test_checks();
    test_uncorrectable_verdict();
    test_unchecked_report_prints_no_content();
    test_reserved_source();
    test_voice_user_withdraws_evidence();
    test_repeat_grant_withdraws_evidence();
    test_fields();
    dsd_decode_clock_use_system();
    if (g_fail) {
        return 1;
    }
    DSD_FPRINTF(stderr, "P25_P1_HARRIS_GPS: OK\n");
    return 0;
}
