// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #800: a position report names the destination its checked carrier named (a UDT header, a data packet's
 * header, the checked full LC of a DMR call), with that carrier's group qualifier, so the same fix from one source to
 * two destinations within the feed's repeat window is two records. Driven through the real producers in dsd_gps.c and
 * the real admission and feed. All IDs and coordinates are made up.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/position_attribution.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

// Minimal stubs for the real dsd_gps.c.
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    (void)format;
    DSD_SNPRINTF(out, out_size, "%s", "");
    return 1;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)observation;
    (void)notice;
    return 0;
}

int
dsd_event_emit_data_notice_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                    const dsd_call_observation* observation, const char* notice, const char* gps) {
    (void)gps;
    return dsd_event_emit_data_notice(opts, state, slot, observation, notice);
}

void
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, const uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

static int g_failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                        \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

#define SOURCE  4321U
#define GROUP_A 77U
#define UNIT_B  88U

static dsd_opts g_opts;
static dsd_state g_state;
static uint64_t g_now_ns = 1000000000ULL;

static void
reset_fixture(uint8_t synctype) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    DSD_MEMSET(&g_state, 0, sizeof g_state);
    g_state.lastsynctype = synctype;
    g_state.synctype = synctype;
    if (dsd_position_feed_start(&g_state) != 0) {
        DSD_FPRINTF(stderr, "feed start failed\n");
        exit(2);
    }
}

/* One second later: well inside the feed's 30 s repeat window. */
static void
advance_1s(void) {
    g_now_ns += 1000000000ULL;
    dsd_decode_clock_use_test(g_now_ns);
}

static void
set_bits_msb(uint8_t* bits, size_t offset, unsigned width, uint32_t value) {
    for (unsigned i = 0; i < width; ++i) {
        bits[offset + i] = (uint8_t)((value >> (width - 1U - i)) & 1U);
    }
}

/* Two records: the first to GROUP_A as a group, the second to UNIT_B as a radio, both from SOURCE (or @p source). */
static void
expect_two_destinations(const char* tag, uint32_t source) {
    dsd_position_fix recs[4];
    dsd_position_feed_meta meta;
    const int n = dsd_position_feed_copy_since(&g_state, 0U, recs, 4, &meta);
    if (n != 2) {
        DSD_FPRINTF(stderr, "%s: %d record(s), want 2\n", tag, n);
        g_failures++;
        return;
    }
    if (!(recs[0].source_id == source && recs[0].target_id == GROUP_A
          && (recs[0].flags & DSD_POS_FLAG_TARGET_GROUP) != 0U)) {
        DSD_FPRINTF(stderr, "%s: first record src %u tgt %u flags %04X\n", tag, recs[0].source_id, recs[0].target_id,
                    recs[0].flags);
        g_failures++;
    }
    if (!(recs[1].source_id == source && recs[1].target_id == UNIT_B
          && (recs[1].flags & DSD_POS_FLAG_TARGET_GROUP) == 0U)) {
        DSD_FPRINTF(stderr, "%s: second record src %u tgt %u flags %04X\n", tag, recs[1].source_id, recs[1].target_id,
                    recs[1].flags);
        g_failures++;
    }
}

/* A LIP short report (22.5 S, 45 W) from a UDT or UDP datagram: its carrier's addressing. */
static void
test_lip(void) {
    uint8_t bits[76] = {0};
    set_bits_msb(bits, 4, 1, 1);
    set_bits_msb(bits, 5, 24, 0xC00000);
    set_bits_msb(bits, 29, 1, 1);
    set_bits_msb(bits, 30, 23, 0x600000);
    set_bits_msb(bits, 68, 8, 32);
    reset_fixture(DSD_SYNC_DMR_BS_DATA_POS);
    const dsd_gps_address to_group = {.source_id = SOURCE, .target_id = GROUP_A, .target_group = 1U};
    const dsd_gps_address to_unit = {.source_id = SOURCE, .target_id = UNIT_B, .target_group = 0U};
    lip_pdu_decoder(&g_opts, &g_state, bits, sizeof bits, &to_group);
    advance_1s();
    lip_pdu_decoder(&g_opts, &g_state, bits, sizeof bits, &to_unit);
    expect_two_destinations("LIP", SOURCE);
}

/* A binary UDT NMEA report (long form, 22 30 S, 45 W): the UDT header's addressing. */
static void
test_udt_nmea(void) {
    uint8_t nmea[192] = {0};
    set_bits_msb(nmea, 3, 1, 1);
    set_bits_msb(nmea, 11, 7, 22);
    set_bits_msb(nmea, 18, 6, 30);
    set_bits_msb(nmea, 38, 8, 45);
    set_bits_msb(nmea, 66, 5, 11);
    set_bits_msb(nmea, 71, 6, 22);
    set_bits_msb(nmea, 77, 6, 33);
    reset_fixture(DSD_SYNC_DMR_BS_DATA_POS);
    const dsd_gps_address to_group = {.source_id = SOURCE, .target_id = GROUP_A, .target_group = 1U};
    const dsd_gps_address to_unit = {.source_id = SOURCE, .target_id = UNIT_B, .target_group = 0U};
    nmea_iec_61162_1(&g_opts, &g_state, nmea, NMEA_UDT_LONG_BITS, &to_group, 2);
    advance_1s();
    nmea_iec_61162_1(&g_opts, &g_state, nmea, NMEA_UDT_LONG_BITS, &to_unit, 2);
    expect_two_destinations("UDT NMEA", SOURCE);
}

/* ASCII NMEA (P25 SAP 48, NXDN 0x06): the checked data header's source, destination and group qualifier. */
static void
test_ascii_nmea(void) {
    static const char text[] = "$GPRMC,120000,A,2230.0000,N,04500.0000,E,010.0,090.0,150626,,,A*7F\r\n";
    uint8_t bits[sizeof text * 8U];
    const size_t n = strlen(text);
    for (size_t i = 0; i < n; i++) {
        set_bits_msb(bits, i * 8U, 8U, (uint8_t)text[i]);
    }
    reset_fixture(DSD_SYNC_NXDN_POS);
    g_state.dmr_lrrp_source[0] = SOURCE;
    g_state.dmr_lrrp_target[0] = GROUP_A;
    g_state.dmr_data_checked_group[0] = 1U;
    CHECK(nmea_sentence_checker(&g_opts, &g_state, bits, 0U, (int)n, 1, NULL) == 1U);
    advance_1s();
    g_state.dmr_lrrp_source[0] = SOURCE;
    g_state.dmr_lrrp_target[0] = UNIT_B;
    g_state.dmr_data_checked_group[0] = 0U;
    CHECK(nmea_sentence_checker(&g_opts, &g_state, bits, 0U, (int)n, 1, NULL) == 1U);
    expect_two_destinations("ASCII NMEA", SOURCE);
}

/* The NXDN GPS report: the bound data call header's destination and group qualifier. */
static void
test_nxdn_gps(void) {
    uint8_t bits[DSD_NXDN_GPS_REPORT_BITS] = {0};
    set_bits_msb(bits, 168U - 16U, 16, 2230U); /* longitude DDDMM after the marker */
    set_bits_msb(bits, 200U - 16U, 16, 1000U); /* latitude DDMM after the marker */
    set_bits_msb(bits, 152U - 16U, 7, 25U);    /* year - 2000 */
    set_bits_msb(bits, 159U - 16U, 4, 6U);     /* month */
    set_bits_msb(bits, 163U - 16U, 5, 20U);    /* day - 1 */
    reset_fixture(DSD_SYNC_NXDN_POS);
    g_state.dmr_lrrp_target[0] = GROUP_A;
    g_state.dmr_data_checked_group[0] = 1U;
    nxdn_gps_report(&g_opts, &g_state, bits, sizeof bits, SOURCE, NULL);
    advance_1s();
    g_state.dmr_lrrp_target[0] = UNIT_B;
    g_state.dmr_data_checked_group[0] = 0U;
    nxdn_gps_report(&g_opts, &g_state, bits, sizeof bits, SOURCE, NULL);
    expect_two_destinations("NXDN GPS", SOURCE);
}

/* The DMR embedded GPS LC: the destination the checked full LC of the call named, while the call still has it. */
static void
embedded_on_call(uint32_t target, int group, const uint8_t* lc) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 0U,
        .kind = group ? DSD_CALL_KIND_GROUP_VOICE : DSD_CALL_KIND_PRIVATE_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = SOURCE,
    };
    CHECK(dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN) >= 0);
    dsd_call_snapshot call;
    CHECK(dsd_call_state_get(&g_state, 0U, &call) > 0);
    dsd_position_attribution_call view;
    CHECK(dsd_position_attribution_call_from_slot(&g_state, 0U, &view) == 1);
    CHECK(dsd_position_attribution_learn(&g_state, &view, SOURCE, target, group,
                                         DSD_POSITION_ATTRIBUTION_BIND_TRANSMISSION, &g_state.dmr_lc_attribution[0])
          == 1);
    dmr_embedded_gps(&g_opts, &g_state, lc);
}

static void
test_dmr_embedded(void) {
    uint8_t lc[96] = {0};
    set_bits_msb(lc, 20, 3, 1);
    set_bits_msb(lc, 23, 25, 0x1000000U); /* west */
    set_bits_msb(lc, 48, 24, 0x200000U);  /* 22.5 N */
    reset_fixture(DSD_SYNC_DMR_BS_VOICE_POS);
    Event_History_I* history = (Event_History_I*)calloc(2U, sizeof(Event_History_I));
    if (history == NULL) {
        exit(2);
    }
    g_state.event_history_s = history;
    embedded_on_call(GROUP_A, 1, lc);
    advance_1s();
    embedded_on_call(UNIT_B, 0, lc);
    expect_two_destinations("DMR embedded GPS", SOURCE);

    // The checked LC named another destination than the call now has: no destination is named.
    reset_fixture(DSD_SYNC_DMR_BS_VOICE_POS);
    g_state.event_history_s = history;
    embedded_on_call(GROUP_A, 1, lc);
    dsd_position_fix recs[2];
    dsd_position_feed_meta meta;
    CHECK(dsd_position_feed_copy_since(&g_state, 0U, recs, 2, &meta) == 1 && recs[0].target_id == GROUP_A);
    g_state.dmr_lc_attribution[0].target = UNIT_B;
    advance_1s();
    set_bits_msb(lc, 20, 3, 2); /* another report */
    dmr_embedded_gps(&g_opts, &g_state, lc);
    CHECK(dsd_position_feed_copy_since(&g_state, 0U, recs, 2, &meta) == 2 && recs[1].source_id == SOURCE
          && recs[1].target_id == 0U && (recs[1].flags & DSD_POS_FLAG_TARGET_GROUP) == 0U);
    g_state.event_history_s = NULL;
    free(history);
}

int
main(void) {
    dsd_decode_clock_use_test(g_now_ns);
    test_lip();
    test_udt_nmea();
    test_ascii_nmea();
    test_nxdn_gps();
    test_dmr_embedded();
    dsd_state_ext_free_all(&g_state);
    dsd_decode_clock_use_system();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "CORE_GPS_DESTINATION: %d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stdout, "CORE_GPS_DESTINATION: PASS\n");
    return 0;
}
