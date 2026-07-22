// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Parity: DMR embedded GPS Position Error (ETSI TS 102 361-2 7.2.15) matches SDRTrunk.
 * - 0..5: less than 2*10^n meters
 * - 6:    more than 200 kilometers
 * - 7:    unknown
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

void dmr_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[]);
void apx_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[]);
void lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input);
void nmea_iec_61162_1(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int type);
void nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot);
uint8_t nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes);
void nxdn_gps_report(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src);

static int g_watchdog_calls;
static uint32_t g_watchdog_src;
static uint32_t g_watchdog_dst;
static uint8_t g_watchdog_slot;
static char g_watchdog_data[128];
static char g_watchdog_gps[256];

// Minimal stubs for direct link with dsd_gps.c
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value;
    switch (format) {
        case DSD_LOCAL_DATETIME_TIME_COMPACT: value = "000000"; break;
        case DSD_LOCAL_DATETIME_TIME_COLON: value = "00:00:00"; break;
        case DSD_LOCAL_DATETIME_DATE_COMPACT: value = "00000000"; break;
        case DSD_LOCAL_DATETIME_DATE_SLASH: value = "0000/00/00"; break;
        case DSD_LOCAL_DATETIME_DATE_HYPHEN: value = "0000-00-00"; break;
        default: value = ""; break;
    }
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    g_watchdog_calls++;
    g_watchdog_src = observation->ota_source_id;
    g_watchdog_dst = observation->ota_target_id;
    g_watchdog_slot = slot;
    DSD_SNPRINTF(g_watchdog_data, sizeof g_watchdog_data, "%s", notice ? notice : "");
    if (state != NULL && state->event_history_s != NULL && slot < DSD_CALL_STATE_SLOT_COUNT) {
        DSD_SNPRINTF(g_watchdog_gps, sizeof g_watchdog_gps, "%s",
                     state->event_history_s[slot].Event_History_Items[0].gps_s);
    }
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

static int
expect_has_substr(const char* buf, const char* needle, const char* tag) {
    if (!buf || !strstr(buf, needle)) {
        DSD_FPRINTF(stderr, "%s: missing '%s' in '%s'\n", tag, needle, buf ? buf : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_i(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* tag, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
reset_watchdog_capture(void) {
    g_watchdog_calls = 0;
    g_watchdog_src = 0;
    g_watchdog_dst = 0;
    g_watchdog_slot = 0xFFU;
    DSD_MEMSET(g_watchdog_data, 0, sizeof g_watchdog_data);
    DSD_MEMSET(g_watchdog_gps, 0, sizeof g_watchdog_gps);
}

static void
seed_active_call(dsd_state* state, uint8_t slot, uint32_t source_id) {
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1201U,
        .policy_target_id = 1201U,
        .ota_source_id = source_id,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) < 0) {
        abort();
    }
}

static void
set_pos_err(uint8_t* lc_bits, uint8_t pos_err) {
    // pos_err is 3 bits at positions 20..22, MSB-first.
    lc_bits[20] = (pos_err >> 2) & 1;
    lc_bits[21] = (pos_err >> 1) & 1;
    lc_bits[22] = (pos_err >> 0) & 1;
}

static int
nmea_len_bits(const char* sentence) {
    int n = 0;
    while (sentence[n] != '\0') {
        n++;
    }
    return n;
}

static void
nmea_ascii_to_bits(uint8_t* out_bits, int out_bits_len, const char* sentence) {
    DSD_MEMSET(out_bits, 0, (size_t)out_bits_len * sizeof(uint8_t));
    int n = nmea_len_bits(sentence);
    for (int i = 0; i < n; i++) {
        uint8_t c = (uint8_t)sentence[i];
        int base = i * 8;
        for (int b = 0; b < 8; b++) {
            out_bits[base + b] = (uint8_t)((c >> (7 - b)) & 1U);
        }
    }
}

static void
set_bits_msb(uint8_t* bits_out, int out_bits_len, int bit_offset, uint32_t value, int bit_count) {
    if (bit_offset < 0 || bit_count < 0 || bit_offset + bit_count > out_bits_len) {
        DSD_FPRINTF(stderr, "set_bits_msb: need=%d have=%d\n", bit_offset + bit_count, out_bits_len);
        exit(2);
    }

    for (int b = 0; b < bit_count; b++) {
        bits_out[bit_offset + b] = (uint8_t)((value >> (bit_count - 1 - b)) & 1U);
    }
}

static int
test_packed_nmea_formats(dsd_opts* opts, dsd_state* st) {
    int rc = 0;

    {
        uint8_t bits[128];
        DSD_MEMSET(bits, 0, sizeof bits);
        bits[1] = 1U; // north
        bits[2] = 1U; // east
        bits[3] = 1U; // fix valid
        set_bits_msb(bits, (int)sizeof bits, 4, 10U, 7);
        set_bits_msb(bits, (int)sizeof bits, 11, 41U, 7);
        set_bits_msb(bits, (int)sizeof bits, 18, 30U, 6);
        set_bits_msb(bits, (int)sizeof bits, 24, 0U, 14);
        set_bits_msb(bits, (int)sizeof bits, 38, 87U, 8);
        set_bits_msb(bits, (int)sizeof bits, 46, 15U, 6);
        set_bits_msb(bits, (int)sizeof bits, 52, 0U, 14);
        set_bits_msb(bits, (int)sizeof bits, 66, 12U, 5);
        set_bits_msb(bits, (int)sizeof bits, 71, 34U, 6);
        set_bits_msb(bits, (int)sizeof bits, 77, 56U, 6);
        set_bits_msb(bits, (int)sizeof bits, 103, 123U, 9);

        st->currentslot = 0;
        DSD_MEMSET(st->dmr_embedded_gps[0], 0, sizeof st->dmr_embedded_gps[0]);
        DSD_MEMSET(st->event_history_s[0].Event_History_Items[0].gps_s, 0,
                   sizeof st->event_history_s[0].Event_History_Items[0].gps_s);
        const uint64_t revision = st->event_history_s[0].revision;
        nmea_iec_61162_1(opts, st, bits, 900001U, 2);

        rc |= expect_has_substr(st->dmr_embedded_gps[0], "41.500000", "nmea-iec-lat");
        rc |= expect_has_substr(st->dmr_embedded_gps[0], "87.250000", "nmea-iec-lon");
        rc |= expect_has_substr(st->event_history_s[0].Event_History_Items[0].gps_s, "41.500000", "nmea-iec-event-lat");
        rc |= expect_i("nmea-iec-history-revision", st->event_history_s[0].revision == revision + 1U, 1);
    }

    {
        uint8_t bits[160];
        DSD_MEMSET(bits, 0, sizeof bits);
        set_bits_msb(bits, (int)sizeof bits, 0, 0x2AA4U, 16);
        set_bits_msb(bits, (int)sizeof bits, 40, 5000U, 16);
        bits[56] = 1U; // negative latitude
        set_bits_msb(bits, (int)sizeof bits, 57, 30U, 7);
        set_bits_msb(bits, (int)sizeof bits, 64, 41U, 8);
        set_bits_msb(bits, (int)sizeof bits, 72, 2500U, 16);
        bits[88] = 1U; // negative longitude
        set_bits_msb(bits, (int)sizeof bits, 89, 15U, 7);
        set_bits_msb(bits, (int)sizeof bits, 96, 87U, 8);
        set_bits_msb(bits, (int)sizeof bits, 104, 3661U, 16);
        set_bits_msb(bits, (int)sizeof bits, 135, 270U, 9);

        DSD_MEMSET(st->dmr_embedded_gps[1], 0, sizeof st->dmr_embedded_gps[1]);
        DSD_MEMSET(st->event_history_s[1].Event_History_Items[0].gps_s, 0,
                   sizeof st->event_history_s[1].Event_History_Items[0].gps_s);
        st->event_history_s[1].Event_History_Items[0].source_id = 900002U;
        nmea_harris(opts, st, bits, 900002U, 2);

        rc |= expect_has_substr(st->dmr_embedded_gps[1], "-41.508333", "harris-nmea-lat");
        rc |= expect_has_substr(st->dmr_embedded_gps[1], "-87.254167", "harris-nmea-lon");
        rc |= expect_has_substr(st->dmr_embedded_gps[1], "270", "harris-nmea-heading");
        rc |= expect_has_substr(st->event_history_s[1].Event_History_Items[0].gps_s, "-87.254167",
                                "harris-nmea-event-lon");
    }

    return rc;
}

static int
test_lip_and_vendor_gps(dsd_opts* opts, dsd_state* st) {
    int rc = 0;

    {
        uint8_t bits[96];
        DSD_MEMSET(bits, 0, sizeof bits);
        set_bits_msb(bits, (int)sizeof bits, 6, 2U, 2); // time elapsed
        bits[8] = 1U;                                   // west
        set_bits_msb(bits, (int)sizeof bits, 9, 0x010000U, 24);
        bits[33] = 1U; // south
        set_bits_msb(bits, (int)sizeof bits, 34, 0x020000U, 23);
        set_bits_msb(bits, (int)sizeof bits, 57, 3U, 2);
        set_bits_msb(bits, (int)sizeof bits, 59, 40U, 7);
        set_bits_msb(bits, (int)sizeof bits, 66, 6U, 4);
        set_bits_msb(bits, (int)sizeof bits, 70, 5U, 3);
        set_bits_msb(bits, (int)sizeof bits, 73, 0x5AU, 8);

        st->currentslot = 1;
        DSD_MEMSET(st->dmr_embedded_gps[1], 0, sizeof st->dmr_embedded_gps[1]);
        DSD_MEMSET(st->event_history_s[1].Event_History_Items[0].gps_s, 0,
                   sizeof st->event_history_s[1].Event_History_Items[0].gps_s);
        lip_protocol_decoder(opts, st, bits);

        rc |= expect_has_substr(st->dmr_embedded_gps[1], "090; LIP:", "lip-slot1-prefix");
        rc |= expect_has_substr(st->dmr_embedded_gps[1], "S", "lip-south");
        rc |= expect_has_substr(st->dmr_embedded_gps[1], "W", "lip-west");
        rc |= expect_has_substr(st->dmr_embedded_gps[1], "Err: 2000m", "lip-position-error");
        rc |= expect_has_substr(st->event_history_s[1].Event_History_Items[0].gps_s, "090; LIP:", "lip-event");
    }

    {
        uint8_t bits[96];
        DSD_MEMSET(bits, 0, sizeof bits);
        st->currentslot = 1;
        seed_active_call(st, 1U, 0x102030U);
        st->event_history_s[1].Event_History_Items[0].source_id = 0x102030;
        DSD_MEMSET(st->dmr_embedded_gps[1], 0, sizeof st->dmr_embedded_gps[1]);
        DSD_MEMSET(st->event_history_s[1].Event_History_Items[0].gps_s, 0,
                   sizeof st->event_history_s[1].Event_History_Items[0].gps_s);
        bits[1] = 1U;  // res_a
        bits[23] = 1U; // expired/last fix
        bits[24] = 1U; // negative latitude
        set_bits_msb(bits, (int)sizeof bits, 25, 0x200000U, 23);
        bits[48] = 1U; // west longitude encoding
        set_bits_msb(bits, (int)sizeof bits, 49, 0x300000U, 23);

        apx_embedded_gps(opts, st, bits);

        rc |= expect_has_substr(st->dmr_embedded_gps[1], "GPS:", "apx-gps-string");
        rc |= expect_has_substr(st->dmr_embedded_gps[1], "Last Fix", "apx-expired");
        rc |= expect_has_substr(st->event_history_s[1].Event_History_Items[0].gps_s, "Last Fix", "apx-event");
    }

    return rc;
}

static int
test_nxdn_gps_report_paths(dsd_opts* opts, dsd_state* st) {
    int rc = 0;
    uint8_t bits[280];

    seed_active_call(st, 0U, 900003U);
    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits_msb(bits, (int)sizeof bits, 16, 2500U, 15);
    set_bits_msb(bits, (int)sizeof bits, 56, 123U, 16);
    set_bits_msb(bits, (int)sizeof bits, 74, 321U, 14);
    set_bits_msb(bits, (int)sizeof bits, 92, 2700U, 12);
    set_bits_msb(bits, (int)sizeof bits, 136, 25U, 7);
    set_bits_msb(bits, (int)sizeof bits, 143, 6U, 4);
    set_bits_msb(bits, (int)sizeof bits, 147, 21U, 5);
    set_bits_msb(bits, (int)sizeof bits, 152, 8715U, 16);
    set_bits_msb(bits, (int)sizeof bits, 184, 4130U, 16);
    set_bits_msb(bits, (int)sizeof bits, 200, 5000U, 15);
    set_bits_msb(bits, (int)sizeof bits, 247, 14U, 5);
    set_bits_msb(bits, (int)sizeof bits, 252, 45U, 6);
    st->dmr_lrrp_source[0] = 1234U;
    st->dmr_lrrp_target[0] = 5678U;
    DSD_MEMSET(st->event_history_s[0].Event_History_Items[0].gps_s, 0,
               sizeof st->event_history_s[0].Event_History_Items[0].gps_s);
    reset_watchdog_capture();

    nxdn_gps_report(opts, st, bits, 900003U);

    rc |= expect_has_substr(st->event_history_s[0].Event_History_Items[0].gps_s, "41.", "nxdn-gps-lat");
    rc |= expect_has_substr(st->event_history_s[0].Event_History_Items[0].gps_s, "87.", "nxdn-gps-lon");
    rc |= expect_i("nxdn-watchdog-calls", g_watchdog_calls, 1);
    rc |= expect_u32("nxdn-watchdog-src", g_watchdog_src, 1234U);
    rc |= expect_u32("nxdn-watchdog-dst", g_watchdog_dst, 5678U);
    rc |= expect_u32("nxdn-source-reset", st->dmr_lrrp_source[0], 0U);
    rc |= expect_u32("nxdn-target-reset", st->dmr_lrrp_target[0], 0U);

    (void)dsd_call_state_end(st, 0U, 0.0);
    DSD_MEMSET(st->event_history_s[0].Event_History_Items[0].gps_s, 0,
               sizeof st->event_history_s[0].Event_History_Items[0].gps_s);
    st->dmr_lrrp_source[0] = 1234U;
    st->dmr_lrrp_target[0] = 5678U;
    reset_watchdog_capture();
    nxdn_gps_report(opts, st, bits, 900003U);
    rc |= expect_has_substr(g_watchdog_gps, "41.", "standalone-nxdn-data-event-lat");
    rc |= expect_has_substr(g_watchdog_gps, "87.", "standalone-nxdn-data-event-lon");

    DSD_MEMSET(bits, 0, sizeof bits);
    set_bits_msb(bits, (int)sizeof bits, 184, 9900U, 16);
    st->dmr_lrrp_source[0] = 4444U;
    st->dmr_lrrp_target[0] = 5555U;
    nxdn_gps_report(opts, st, bits, 0U);
    rc |= expect_u32("nxdn-invalid-source-reset", st->dmr_lrrp_source[0], 0U);
    rc |= expect_u32("nxdn-invalid-target-reset", st->dmr_lrrp_target[0], 0U);

    return rc;
}

int
main(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    opts.lrrp_file_output = 0;
    st.currentslot = 0;

    st.event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    if (!st.event_history_s) {
        return 100;
    }
    seed_active_call(&st, 0U, 900001U);
    seed_active_call(&st, 1U, 900002U);
    rc |= test_packed_nmea_formats(&opts, &st);
    rc |= test_lip_and_vendor_gps(&opts, &st);
    rc |= test_nxdn_gps_report_paths(&opts, &st);

    uint8_t lc_bits[80];
    DSD_MEMSET(lc_bits, 0, sizeof lc_bits);
    st.currentslot = 0;

    // pos_err == 5: less than 200km (200000m)
    {
        DSD_MEMSET(st.dmr_embedded_gps[0], 0, sizeof st.dmr_embedded_gps[0]);
        set_pos_err(lc_bits, 5);
        dmr_embedded_gps(&opts, &st, lc_bits);
        rc |= expect_has_substr(st.dmr_embedded_gps[0], "Err: 200000m", "pos_err=5");
    }

    // pos_err == 6: more than 200km
    {
        DSD_MEMSET(st.dmr_embedded_gps[0], 0, sizeof st.dmr_embedded_gps[0]);
        set_pos_err(lc_bits, 6);
        dmr_embedded_gps(&opts, &st, lc_bits);
        rc |= expect_has_substr(st.dmr_embedded_gps[0], "Err: >200km", "pos_err=6");
    }

    // pos_err == 7: unknown
    {
        DSD_MEMSET(st.dmr_embedded_gps[0], 0, sizeof st.dmr_embedded_gps[0]);
        set_pos_err(lc_bits, 7);
        dmr_embedded_gps(&opts, &st, lc_bits);
        rc |= expect_has_substr(st.dmr_embedded_gps[0], "Unknown Pos Err", "pos_err=7");
    }

    // NMEA sentence checker: valid sentence
    {
        static const char nmea_ok[] = "$GPRMC,TEST*71\r\n";
        uint8_t bits[8 * (int)(sizeof(nmea_ok) - 1)];
        int len_bytes = nmea_len_bits(nmea_ok);
        nmea_ascii_to_bits(bits, (int)sizeof(bits), nmea_ok);
        st.dmr_lrrp_source[0] = 111U;
        st.dmr_lrrp_target[0] = 222U;
        if (st.event_history_s != NULL) {
            DSD_MEMSET(st.event_history_s[0].Event_History_Items[0].text_message, 0,
                       sizeof(st.event_history_s[0].Event_History_Items[0].text_message));
        }
        uint8_t ok = nmea_sentence_checker(&opts, &st, bits, 0, len_bytes);
        rc |= expect_u8("nmea-valid", ok, 1U);
        if (st.event_history_s != NULL) {
            rc |= expect_has_substr(st.event_history_s[0].Event_History_Items[0].text_message, "$GPRMC,TEST*71",
                                    "nmea-valid-text");
        } else {
            DSD_FPRINTF(stderr, "%s\n", "nmea-valid-text: event_history_s is NULL");
            rc |= 1;
        }
        rc |= expect_u32("nmea-valid-src-reset", st.dmr_lrrp_source[0], 0U);
        rc |= expect_u32("nmea-valid-tgt-reset", st.dmr_lrrp_target[0], 0U);
    }

    // NMEA sentence checker: checksum error
    {
        static const char nmea_bad[] = "$GPRMC,TEST*72\r\n";
        uint8_t bits[8 * (int)(sizeof(nmea_bad) - 1)];
        int len_bytes = nmea_len_bits(nmea_bad);
        nmea_ascii_to_bits(bits, (int)sizeof(bits), nmea_bad);
        st.dmr_lrrp_source[0] = 333U;
        st.dmr_lrrp_target[0] = 444U;
        if (st.event_history_s != NULL) {
            DSD_MEMSET(st.event_history_s[0].Event_History_Items[0].text_message, 0,
                       sizeof(st.event_history_s[0].Event_History_Items[0].text_message));
        }
        uint8_t ok = nmea_sentence_checker(&opts, &st, bits, 0, len_bytes);
        rc |= expect_u8("nmea-invalid", ok, 0U);
        if (st.event_history_s != NULL) {
            rc |= expect_i("nmea-invalid-text-empty", st.event_history_s[0].Event_History_Items[0].text_message[0], 0);
        } else {
            DSD_FPRINTF(stderr, "%s\n", "nmea-invalid-text-empty: event_history_s is NULL");
            rc |= 1;
        }
        rc |= expect_u32("nmea-invalid-src-reset", st.dmr_lrrp_source[0], 0U);
        rc |= expect_u32("nmea-invalid-tgt-reset", st.dmr_lrrp_target[0], 0U);
    }

    // NMEA sentence checker: missing checksum delimiter still clears LRRP state without emitting an event.
    {
        static const char nmea_missing_star[] = "$GPRMC,TEST\r\n";
        uint8_t bits[8 * (int)(sizeof(nmea_missing_star) - 1)];
        int len_bytes = nmea_len_bits(nmea_missing_star);
        nmea_ascii_to_bits(bits, (int)sizeof(bits), nmea_missing_star);
        st.dmr_lrrp_source[0] = 555U;
        st.dmr_lrrp_target[0] = 666U;
        reset_watchdog_capture();
        if (st.event_history_s != NULL) {
            DSD_MEMSET(st.event_history_s[0].Event_History_Items[0].text_message, 0,
                       sizeof(st.event_history_s[0].Event_History_Items[0].text_message));
        }
        uint8_t ok = nmea_sentence_checker(&opts, &st, bits, 0, len_bytes);
        rc |= expect_u8("nmea-missing-star", ok, 0U);
        rc |= expect_i("nmea-missing-star-watchdog", g_watchdog_calls, 0);
        if (st.event_history_s != NULL) {
            rc |= expect_i("nmea-missing-star-text-empty", st.event_history_s[0].Event_History_Items[0].text_message[0],
                           0);
        } else {
            DSD_FPRINTF(stderr, "%s\n", "nmea-missing-star-text-empty: event_history_s is NULL");
            rc |= 1;
        }
        rc |= expect_u32("nmea-missing-star-src-reset", st.dmr_lrrp_source[0], 0U);
        rc |= expect_u32("nmea-missing-star-tgt-reset", st.dmr_lrrp_target[0], 0U);
    }

    // NMEA checker should still validate even if event history is unavailable.
    {
        static const char nmea_ok[] = "$GPRMC,TEST*71\r\n";
        uint8_t bits[8 * (int)(sizeof(nmea_ok) - 1)];
        int len_bytes = nmea_len_bits(nmea_ok);
        nmea_ascii_to_bits(bits, (int)sizeof(bits), nmea_ok);
        Event_History_I* saved_events = st.event_history_s;
        st.event_history_s = NULL;
        st.dmr_lrrp_source[0] = 777U;
        st.dmr_lrrp_target[0] = 888U;
        uint8_t ok = nmea_sentence_checker(&opts, &st, bits, 0, len_bytes);
        rc |= expect_u8("nmea-no-events-valid", ok, 1U);
        rc |= expect_u32("nmea-no-events-src-reset", st.dmr_lrrp_source[0], 0U);
        rc |= expect_u32("nmea-no-events-tgt-reset", st.dmr_lrrp_target[0], 0U);
        st.event_history_s = saved_events;
    }

    free(st.event_history_s);
    st.event_history_s = NULL;

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
