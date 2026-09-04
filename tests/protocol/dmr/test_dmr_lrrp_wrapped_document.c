// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression (#453): some senders wrap a conformant LRRP document in a short
 * fixed envelope, a leading byte the classifier does not know, a counter and
 * a few constant bytes, then an ordinary document whose length byte runs
 * exactly to the end of the UDP payload. Read as a plain document the counter
 * lands in the length slot and the position behind it is lost, so the whole
 * message printed "Unknown Format". The inner document is found by that
 * exact-end property and decoded as itself.
 *
 * The six wrapped vectors are the synthetic ones the reporter built to mirror
 * that traffic byte for byte (issue #453, comment of 2026-09-02); the values
 * inside them are invented and nothing off air appears here.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/runtime/unicode.h>
#include <errno.h>
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

// Minimal stubs for direct link with dmr_pdu.c
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_unicode_supported(void) {
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
lip_protocol_decoder(dsd_opts* opts, dsd_state* state, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_cellocator(dsd_opts* opts, dsd_state* state, uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    (void)opts;
    (void)state;
    (void)observation->ota_source_id;
    (void)observation->ota_target_id;
    (void)notice;
    (void)slot;
    return 0;
}

// Deterministic system time (not used: file output disabled)
int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value = (format == DSD_LOCAL_DATETIME_DATE_SLASH) ? "1999/01/02" : "11:22:33";
    DSD_SNPRINTF(out, out_size, "%s", value);
    return 1;
}

// Under test
void dmr_lrrp(const dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest,
              const uint8_t* DMR_PDU, uint8_t pdu_crc_ok);

// 03 <counter lo> <counter hi> 3C 00 | 0D 18 <24 token bytes>: timestamp,
// position with radius, speed, heading. Byte 1 (the counter) is below the
// token count in every one, which is the property that loses the position.
static const uint8_t k_wrapped[6][31] = {
    {0x03, 0x05, 0x2A, 0x3C, 0x00, 0x0D, 0x18, 0x23, 0x55, 0x34, 0x1F, 0x90, 0x42, 0x00, 0x00, 0x51,
     0x11, 0x11, 0x11, 0x11, 0x18, 0x2D, 0x82, 0xD8, 0x03, 0xE8, 0x6C, 0x00, 0xFA, 0x56, 0x2D},
    {0x03, 0x06, 0x2A, 0x3C, 0x00, 0x0D, 0x18, 0x23, 0x55, 0x34, 0x1F, 0x90, 0x42, 0x00, 0x00, 0x51,
     0x11, 0x11, 0x3F, 0xAC, 0x18, 0x2D, 0x9A, 0x25, 0x05, 0xDC, 0x6C, 0x01, 0xF4, 0x56, 0x40},
    {0x03, 0x07, 0x2A, 0x3C, 0x00, 0x0D, 0x18, 0x23, 0x55, 0x34, 0x1F, 0x90, 0x42, 0x00, 0x00, 0x51,
     0x11, 0x11, 0x6E, 0x46, 0x18, 0x2D, 0xB1, 0x73, 0x07, 0xD0, 0x6C, 0x00, 0x64, 0x56, 0x10},
    {0x03, 0x0A, 0x3B, 0x3C, 0x00, 0x0D, 0x18, 0x23, 0x55, 0x34, 0x1F, 0x90, 0x42, 0x00, 0x00, 0x51,
     0x11, 0x11, 0x9C, 0xE0, 0x18, 0x2D, 0xC8, 0xC0, 0x04, 0xB0, 0x6C, 0x01, 0x2C, 0x56, 0x60},
    {0x03, 0x0B, 0x3B, 0x3C, 0x00, 0x0D, 0x18, 0x23, 0x55, 0x34, 0x1F, 0x90, 0x42, 0x00, 0x00, 0x51,
     0x11, 0x11, 0xCB, 0x7B, 0x18, 0x2D, 0xE0, 0x0D, 0x03, 0x20, 0x6C, 0x00, 0x32, 0x56, 0x08},
    {0x03, 0x0C, 0x3B, 0x3C, 0x00, 0x0D, 0x18, 0x23, 0x55, 0x34, 0x1F, 0x90, 0x42, 0x00, 0x00, 0x51,
     0x11, 0x11, 0xFA, 0x15, 0x18, 0x2D, 0xF7, 0x5A, 0x0B, 0xB8, 0x6C, 0x03, 0x84, 0x56, 0x74},
};

static const size_t k_prefix = 5u;

static int
expect_point(const char* s, double exp_lat, double exp_lon, const char* tag) {
    const char* p = strchr(s, '(');
    if (!p) {
        DSD_FPRINTF(stderr, "%s: no position in '%s'\n", tag, s);
        return 1;
    }
    errno = 0;
    char* end = NULL;
    double lat = strtod(p + 1, &end);
    if (end == p + 1 || errno == ERANGE || *end != ',') {
        DSD_FPRINTF(stderr, "%s: cannot read coordinates from '%s'\n", tag, s);
        return 1;
    }
    errno = 0;
    const char* lon_start = end + 1;
    double lon = strtod(lon_start, &end);
    if (end == lon_start || errno == ERANGE) {
        DSD_FPRINTF(stderr, "%s: cannot read coordinates from '%s'\n", tag, s);
        return 1;
    }
    double dlat = lat - exp_lat;
    double dlon = lon - exp_lon;
    if (dlat < 0.0) {
        dlat = -dlat;
    }
    if (dlon < 0.0) {
        dlon = -dlon;
    }
    if (dlat > 1e-5 || dlon > 1e-5) {
        DSD_FPRINTF(stderr, "%s: got (%.6lf, %.6lf) expected (%.6lf, %.6lf)\n", tag, lat, lon, exp_lat, exp_lon);
        return 1;
    }
    return 0;
}

static int
expect_exact(const char* s, const char* expect, const char* tag) {
    if (strcmp(s, expect) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' expected '%s'\n", tag, s, expect);
        return 1;
    }
    return 0;
}

static void
decode(dsd_opts* opts, dsd_state* st, const uint8_t* pdu, size_t len) {
    DSD_MEMSET(st->dmr_lrrp_gps[0], 0, sizeof st->dmr_lrrp_gps[0]);
    dmr_lrrp(opts, st, (uint16_t)len, /*src*/ 123, /*dst*/ 456, pdu, /*pdu_crc_ok*/ 1);
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    opts.lrrp_file_output = 0;

    int rc = 0;
    const size_t n = sizeof k_wrapped[0];

    // 1. Each wrapped vector decodes to the position, speed and heading of the
    //    document inside it, and prints exactly what that document prints on
    //    its own.
    for (size_t k = 0; k < 6u; k++) {
        char tag[32];
        DSD_SNPRINTF(tag, sizeof tag, "wrapped vector %u", (unsigned)(k + 1u));
        decode(&opts, &st, k_wrapped[k], n);
        rc |= expect_point(st.dmr_lrrp_gps[0], 12.0 + 0.0005 * (double)k, 34.0 + 0.0005 * (double)k, tag);
        if (!strstr(st.dmr_lrrp_gps[0], " km/h ")) {
            DSD_FPRINTF(stderr, "%s: no speed or heading in '%s'\n", tag, st.dmr_lrrp_gps[0]);
            rc = 1;
        }
        char wrapped[sizeof st.dmr_lrrp_gps[0]];
        DSD_SNPRINTF(wrapped, sizeof wrapped, "%s", st.dmr_lrrp_gps[0]);

        decode(&opts, &st, k_wrapped[k] + k_prefix, n - k_prefix);
        rc |= expect_exact(wrapped, st.dmr_lrrp_gps[0], tag);
    }

    // 2. The same envelope with no known document type inside stays unknown.
    {
        uint8_t pdu[31];
        DSD_MEMCPY(pdu, k_wrapped[0], n);
        pdu[k_prefix] = 0x00;
        decode(&opts, &st, pdu, n);
        rc |= expect_exact(st.dmr_lrrp_gps[0], "LRRP SRC: 123; Unknown Format 03; TGT: 456;", "no inner type");
    }

    // 3. A known type whose length byte does not land on the payload end is
    //    not taken as a wrapped document.
    {
        uint8_t pdu[31];
        DSD_MEMCPY(pdu, k_wrapped[0], n);
        pdu[k_prefix + 1u] = 0x17;
        decode(&opts, &st, pdu, n);
        rc |= expect_exact(st.dmr_lrrp_gps[0], "LRRP SRC: 123; Unknown Format 03; TGT: 456;", "inner length short");
        pdu[k_prefix + 1u] = 0x19;
        decode(&opts, &st, pdu, n);
        rc |= expect_exact(st.dmr_lrrp_gps[0], "LRRP SRC: 123; Unknown Format 03; TGT: 456;", "inner length long");
    }

    // 4. A known type with a zero length at the very end is two bytes, not a
    //    document: it must not relabel the message.
    {
        static const uint8_t pdu[] = {0x03, 0x01, 0x00, 0x3C, 0x00, 0x0D, 0x00};
        decode(&opts, &st, pdu, sizeof pdu);
        rc |= expect_exact(st.dmr_lrrp_gps[0], "LRRP SRC: 123; Unknown Format 03; TGT: 456;", "empty inner");
    }

    // 5. The prefix search is bounded: the document at offset 9 is out of reach.
    {
        uint8_t pdu[35];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        pdu[0] = 0x03;
        DSD_MEMCPY(pdu + 9, k_wrapped[0] + k_prefix, n - k_prefix);
        decode(&opts, &st, pdu, sizeof pdu);
        rc |= expect_exact(st.dmr_lrrp_gps[0], "LRRP SRC: 123; Unknown Format 03; TGT: 456;", "prefix too long");
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
