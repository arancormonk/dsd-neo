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

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void dmr_embedded_gps(dsd_opts* opts, dsd_state* state, uint8_t lc_bits[]);
uint8_t nmea_sentence_checker(dsd_opts* opts, dsd_state* state, uint8_t* input, uint8_t slot, int len_bytes);

// Minimal stubs for direct link with dsd_gps.c
uint64_t
ConvertBitIntoBytes(uint8_t* BufferIn, uint32_t BitLength) {
    uint64_t out = 0;
    uint8_t* p = BufferIn;
    uint32_t n = BitLength;

    while (n--) {
        out = (out << 1) | (uint64_t)(*p++ & 1);
    }

    return out;
}

const char*
dsd_degrees_glyph(void) {
    return "";
}

char*
getTime(void) {
    return NULL;
}

char*
getDate(void) {
    return NULL;
}

void
getTime_buf(char out[7]) {
    snprintf(out, 7, "%s", "000000");
}

void
getTimeC_buf(char out[9]) {
    snprintf(out, 9, "%s", "00:00:00");
}

void
getDate_buf(char out[9]) {
    snprintf(out, 9, "%s", "00000000");
}

void
getDateS_buf(char out[11]) {
    snprintf(out, 11, "%s", "0000/00/00");
}

uint64_t
convert_bits_into_output(uint8_t* input, int len) {
    if (len <= 0) {
        return 0;
    }
    return ConvertBitIntoBytes(input, (uint32_t)len);
}

void
watchdog_event_datacall(dsd_opts* opts, dsd_state* state, uint32_t src, uint32_t dst, char* data_string, uint8_t slot) {
    (void)opts;
    (void)state;
    (void)src;
    (void)dst;
    (void)data_string;
    (void)slot;
}

void
utf8_to_text(dsd_state* state, uint8_t wr, uint16_t len, uint8_t* input) {
    (void)state;
    (void)wr;
    (void)len;
    (void)input;
}

static int
expect_has_substr(const char* buf, const char* needle, const char* tag) {
    if (!buf || !strstr(buf, needle)) {
        fprintf(stderr, "%s: missing '%s' in '%s'\n", tag, needle, buf ? buf : "(null)");
        return 1;
    }
    return 0;
}

static int
expect_u8(const char* tag, uint8_t got, uint8_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got %u want %u\n", tag, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_i(const char* tag, int got, int want) {
    if (got != want) {
        fprintf(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_u32(const char* tag, uint32_t got, uint32_t want) {
    if (got != want) {
        fprintf(stderr, "%s: got %u want %u\n", tag, got, want);
        return 1;
    }
    return 0;
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
    memset(out_bits, 0, (size_t)out_bits_len * sizeof(uint8_t));
    int n = nmea_len_bits(sentence);
    for (int i = 0; i < n; i++) {
        uint8_t c = (uint8_t)sentence[i];
        int base = i * 8;
        for (int b = 0; b < 8; b++) {
            out_bits[base + b] = (uint8_t)((c >> (7 - b)) & 1U);
        }
    }
}

int
main(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    memset(&opts, 0, sizeof opts);
    memset(&st, 0, sizeof st);
    opts.lrrp_file_output = 0;
    st.currentslot = 0;

    st.event_history_s = (Event_History_I*)calloc(2u, sizeof(Event_History_I));
    if (!st.event_history_s) {
        return 100;
    }

    uint8_t lc_bits[80];
    memset(lc_bits, 0, sizeof lc_bits);

    // pos_err == 5: less than 200km (200000m)
    {
        memset(st.dmr_embedded_gps[0], 0, sizeof st.dmr_embedded_gps[0]);
        set_pos_err(lc_bits, 5);
        dmr_embedded_gps(&opts, &st, lc_bits);
        rc |= expect_has_substr(st.dmr_embedded_gps[0], "Err: 200000m", "pos_err=5");
    }

    // pos_err == 6: more than 200km
    {
        memset(st.dmr_embedded_gps[0], 0, sizeof st.dmr_embedded_gps[0]);
        set_pos_err(lc_bits, 6);
        dmr_embedded_gps(&opts, &st, lc_bits);
        rc |= expect_has_substr(st.dmr_embedded_gps[0], "Err: >200km", "pos_err=6");
    }

    // pos_err == 7: unknown
    {
        memset(st.dmr_embedded_gps[0], 0, sizeof st.dmr_embedded_gps[0]);
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
            memset(st.event_history_s[0].Event_History_Items[0].text_message, 0,
                   sizeof(st.event_history_s[0].Event_History_Items[0].text_message));
        }
        uint8_t ok = nmea_sentence_checker(&opts, &st, bits, 0, len_bytes);
        rc |= expect_u8("nmea-valid", ok, 1U);
        if (st.event_history_s != NULL) {
            rc |= expect_has_substr(st.event_history_s[0].Event_History_Items[0].text_message, "$GPRMC,TEST*71",
                                    "nmea-valid-text");
        } else {
            fprintf(stderr, "%s\n", "nmea-valid-text: event_history_s is NULL");
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
            memset(st.event_history_s[0].Event_History_Items[0].text_message, 0,
                   sizeof(st.event_history_s[0].Event_History_Items[0].text_message));
        }
        uint8_t ok = nmea_sentence_checker(&opts, &st, bits, 0, len_bytes);
        rc |= expect_u8("nmea-invalid", ok, 0U);
        if (st.event_history_s != NULL) {
            rc |= expect_i("nmea-invalid-text-empty", st.event_history_s[0].Event_History_Items[0].text_message[0], 0);
        } else {
            fprintf(stderr, "%s\n", "nmea-invalid-text-empty: event_history_s is NULL");
            rc |= 1;
        }
        rc |= expect_u32("nmea-invalid-src-reset", st.dmr_lrrp_source[0], 0U);
        rc |= expect_u32("nmea-invalid-tgt-reset", st.dmr_lrrp_target[0], 0U);
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
