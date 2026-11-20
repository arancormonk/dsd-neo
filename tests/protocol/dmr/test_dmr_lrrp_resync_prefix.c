// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * LRRP resync (issue #800 bug 2.18): a reading that only skipping leading bytes finds is a guess. The document's own
 * token order always wins over it, and a guessed position is never published: the summary (the data notice's text)
 * says a position was withheld and why, without coordinates.
 *
 * 1. A junk byte before the token stream that masquerades as a fixed-length token: only a resync finds the point, so
 *    it is reported as withheld.
 * 2. The issue's vector: request ID 51 60 00 00, then the point (22.5, 45). A resync inside the request ID reads a
 *    "circle" at about (67.5, 45); the document's own order gives the point and wins.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
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
lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count,
                const dsd_gps_address* src) {
    (void)bit_count;
    (void)src;
    (void)opts;
    (void)state;
    (void)bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
decode_cellocator(dsd_opts* opts, dsd_state* state, const uint8_t* input, int len) {
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
void dmr_lrrp(dsd_opts* opts, dsd_state* state, uint16_t len, uint32_t source, uint32_t dest, uint8_t* DMR_PDU,
              uint8_t pdu_crc_ok);

static int
expect_has_point(const char* s, double exp_lat, double exp_lon, const char* tag) {
    const char* p = strchr(s, '(');
    if (!p) {
        DSD_FPRINTF(stderr, "%s: missing '('\n", tag);
        return 1;
    }

    errno = 0;
    char* end = NULL;
    double lat = strtod(p + 1, &end);
    if (end == p + 1 || errno == ERANGE) {
        DSD_FPRINTF(stderr, "%s: failed to parse coordinates from '%s'\n", tag, s);
        return 1;
    }
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    if (*end != ',') {
        DSD_FPRINTF(stderr, "%s: missing coordinate separator in '%s'\n", tag, s);
        return 1;
    }
    end++;
    errno = 0;
    double lon = strtod(end, &end);
    if (errno == ERANGE) {
        DSD_FPRINTF(stderr, "%s: failed to parse coordinates from '%s'\n", tag, s);
        return 1;
    }

    double dlat = lat - exp_lat;
    if (dlat < 0.0) {
        dlat = -dlat;
    }
    double dlon = lon - exp_lon;
    if (dlon < 0.0) {
        dlon = -dlon;
    }
    if (dlat > 1e-5 || dlon > 1e-5) {
        DSD_FPRINTF(stderr, "%s: got (%.8lf, %.8lf) expected (%.8lf, %.8lf)\n", tag, lat, lon, exp_lat, exp_lon);
        return 1;
    }

    return 0;
}

int
main(void) {
    int rc = 0;

    static dsd_opts opts;
    static dsd_state st;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    opts.lrrp_file_output = 0;

    // Inject a 1-byte prefix before the token stream that looks like TRIGGER_PERIODIC (0x31).
    // The next byte is a valid token id (0x66), which would desync without resync.
    {
        const uint32_t lat_raw = 0x10000000u;
        const uint32_t lon_raw = 0x20000000u;
        uint8_t pdu[32];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        size_t i = 0;
        pdu[i++] = 0x07; // Immediate Location Response
        pdu[i++] = 10;   // payload length (bytes): prefix (1) + POINT_2D (9)
        pdu[i++] = 0x31; // prefix/junk byte (masquerades as a fixed-length token id)

        pdu[i++] = 0x66; // POINT_2D token id
        pdu[i++] = (lat_raw >> 24) & 0xFF;
        pdu[i++] = (lat_raw >> 16) & 0xFF;
        pdu[i++] = (lat_raw >> 8) & 0xFF;
        pdu[i++] = (lat_raw >> 0) & 0xFF;
        pdu[i++] = (lon_raw >> 24) & 0xFF;
        pdu[i++] = (lon_raw >> 16) & 0xFF;
        pdu[i++] = (lon_raw >> 8) & 0xFF;
        pdu[i++] = (lon_raw >> 0) & 0xFF;

        dmr_lrrp(&opts, &st, (uint16_t)i, /*src*/ 123, /*dst*/ 456, pdu, 1);
        if (strcmp(st.dmr_lrrp_gps[0], "LRRP SRC: 123; Position suppressed (layout not confirmed);") != 0) {
            DSD_FPRINTF(stderr, "resync prefix byte: got '%s'\n", st.dmr_lrrp_gps[0]);
            rc |= 1;
        }
    }

    // The issue's vector: the request ID's 0x51 must not start a circle.
    {
        static const uint8_t vec[] = {0x07, 0x0F, 0x22, 0x04, 0x51, 0x60, 0x00, 0x00, 0x66,
                                      0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00};
        uint8_t pdu[sizeof vec];
        DSD_MEMCPY(pdu, vec, sizeof vec);
        dmr_lrrp(&opts, &st, (uint16_t)sizeof pdu, /*src*/ 123, /*dst*/ 456, pdu, 1);
        rc |= expect_has_point(st.dmr_lrrp_gps[0], 22.5, 45.0, "request id is not a circle");
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
