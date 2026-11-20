// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression/parity: SDRTrunk selects the first Point2d-derived token after sorting LRRP tokens by TokenType.
 * That yields a deterministic precedence among position tokens:
 *   CIRCLE_2D (0x51) > CIRCLE_3D (0x55, integer variant 0x53) > POINT_2D (0x66) > POINT_3D (0x69, integer 0x67)
 *
 * Ensure we mirror that selection even when multiple position tokens are present.
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

// Deterministic system time stubs (not used: file output disabled)
int
// NOLINTNEXTLINE(misc-use-internal-linkage)
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

static void
expected_from_raw(uint32_t lat_raw, uint32_t lon_raw, double* out_lat, double* out_lon) {
    const double lat_unit = 180.0 / 4294967295.0;
    const double lon_unit = 360.0 / 4294967295.0;

    int lat_sign = 1;
    uint32_t lat_mag = lat_raw;
    if (lat_mag & 0x80000000u) {
        lat_mag &= 0x7FFFFFFFu;
        lat_sign = -1;
    }

    *out_lat = (double)lat_mag * lat_unit * (double)lat_sign;
    *out_lon = (double)((int32_t)lon_raw) * lon_unit;
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

    // Two distinct positions: POINT_3D first, then CIRCLE_2D.
    uint32_t lat_p3d = 0x10000000u;
    uint32_t lon_p3d = 0x20000000u;
    uint32_t lat_c2d = 0x30000000u;
    uint32_t lon_c2d = 0x40000000u;

    double exp_lat = 0.0;
    double exp_lon = 0.0;
    expected_from_raw(lat_c2d, lon_c2d, &exp_lat, &exp_lon);

    uint8_t pdu[64];
    DSD_MEMSET(pdu, 0, sizeof pdu);
    size_t i = 0;
    pdu[i++] = 0x07; // Immediate Location Response
    pdu[i++] = 22;   // payload length: POINT_3D (11) + CIRCLE_2D (11)

    // 0x69 POINT_3D (lat/lon + sfloatvar altitude)
    pdu[i++] = 0x69;
    pdu[i++] = (lat_p3d >> 24) & 0xFF;
    pdu[i++] = (lat_p3d >> 16) & 0xFF;
    pdu[i++] = (lat_p3d >> 8) & 0xFF;
    pdu[i++] = (lat_p3d >> 0) & 0xFF;
    pdu[i++] = (lon_p3d >> 24) & 0xFF;
    pdu[i++] = (lon_p3d >> 16) & 0xFF;
    pdu[i++] = (lon_p3d >> 8) & 0xFF;
    pdu[i++] = (lon_p3d >> 0) & 0xFF;
    pdu[i++] = 0x05; // altitude: integer 5 (sign clear)
    pdu[i++] = 0x00; // altitude: fraction 0/128

    // 0x51 CIRCLE_2D (lat/lon + ufloatvar radius)
    pdu[i++] = 0x51;
    pdu[i++] = (lat_c2d >> 24) & 0xFF;
    pdu[i++] = (lat_c2d >> 16) & 0xFF;
    pdu[i++] = (lat_c2d >> 8) & 0xFF;
    pdu[i++] = (lat_c2d >> 0) & 0xFF;
    pdu[i++] = (lon_c2d >> 24) & 0xFF;
    pdu[i++] = (lon_c2d >> 16) & 0xFF;
    pdu[i++] = (lon_c2d >> 8) & 0xFF;
    pdu[i++] = (lon_c2d >> 0) & 0xFF;
    pdu[i++] = 0x01; // radius: integer 1
    pdu[i++] = 0x00; // radius: fraction 0/128 -> 1.0 m

    dmr_lrrp(&opts, &st, (uint16_t)i, /*src*/ 123, /*dst*/ 456, pdu, 1);
    rc |= expect_has_point(st.dmr_lrrp_gps[0], exp_lat, exp_lon, "position precedence");

    // The integer variants rank with their float ones: CIRCLE_3D integer (0x53) wins over POINT_3D integer (0x67).
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    DSD_MEMSET(pdu, 0, sizeof pdu);
    i = 0;
    pdu[i++] = 0x07; // Immediate Location Response
    pdu[i++] = 23;   // payload length: POINT_3D integer (10) + CIRCLE_3D integer (13)
    pdu[i++] = 0x67;
    for (int b = 3; b >= 0; b--) {
        pdu[i++] = (uint8_t)(lat_p3d >> (8 * b));
    }
    for (int b = 3; b >= 0; b--) {
        pdu[i++] = (uint8_t)(lon_p3d >> (8 * b));
    }
    pdu[i++] = 0x02; // altitude sintvar: +2
    pdu[i++] = 0x53;
    for (int b = 3; b >= 0; b--) {
        pdu[i++] = (uint8_t)(lat_c2d >> (8 * b));
    }
    for (int b = 3; b >= 0; b--) {
        pdu[i++] = (uint8_t)(lon_c2d >> (8 * b));
    }
    pdu[i++] = 0x02; // radius uintvar: 2
    pdu[i++] = 0x80; // altitude sintvar: 0x80 0x44 = +68
    pdu[i++] = 0x44;
    pdu[i++] = 0x03; // altitude accuracy uintvar: 3

    expected_from_raw(lat_c2d, lon_c2d, &exp_lat, &exp_lon);
    dmr_lrrp(&opts, &st, (uint16_t)i, /*src*/ 123, /*dst*/ 456, pdu, 1);
    rc |= expect_has_point(st.dmr_lrrp_gps[0], exp_lat, exp_lon, "integer variant precedence");

    // A shape at exactly 0, 0 is no position: it does not outrank the document's other shape, whose position wins.
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    DSD_MEMSET(pdu, 0, sizeof pdu);
    i = 0;
    pdu[i++] = 0x07; // Immediate Location Response
    pdu[i++] = 20;   // payload length: CIRCLE_2D (11) + POINT_2D (9)
    pdu[i++] = 0x51; // CIRCLE_2D at 0, 0
    i += 8;
    pdu[i++] = 0x01; // radius: integer 1
    pdu[i++] = 0x00;
    pdu[i++] = 0x66; // POINT_2D
    for (int b = 3; b >= 0; b--) {
        pdu[i++] = (uint8_t)(lat_p3d >> (8 * b));
    }
    for (int b = 3; b >= 0; b--) {
        pdu[i++] = (uint8_t)(lon_p3d >> (8 * b));
    }
    expected_from_raw(lat_p3d, lon_p3d, &exp_lat, &exp_lon);
    dmr_lrrp(&opts, &st, (uint16_t)i, /*src*/ 123, /*dst*/ 456, pdu, 1);
    rc |= expect_has_point(st.dmr_lrrp_gps[0], exp_lat, exp_lon, "zero shape skipped");

    // Unknown LRRP message types still carry usable position payloads in the field.
    DSD_MEMSET(&st, 0, sizeof st);
    st.currentslot = 0;
    DSD_MEMSET(pdu, 0, sizeof pdu);
    i = 0;
    pdu[i++] = 0x99; // unknown LRRP type
    pdu[i++] = 9;    // POINT_2D token length
    pdu[i++] = 0x66; // POINT_2D token id
    pdu[i++] = (lat_p3d >> 24) & 0xFF;
    pdu[i++] = (lat_p3d >> 16) & 0xFF;
    pdu[i++] = (lat_p3d >> 8) & 0xFF;
    pdu[i++] = (lat_p3d >> 0) & 0xFF;
    pdu[i++] = (lon_p3d >> 24) & 0xFF;
    pdu[i++] = (lon_p3d >> 16) & 0xFF;
    pdu[i++] = (lon_p3d >> 8) & 0xFF;
    pdu[i++] = (lon_p3d >> 0) & 0xFF;

    expected_from_raw(lat_p3d, lon_p3d, &exp_lat, &exp_lon);
    dmr_lrrp(&opts, &st, (uint16_t)i, /*src*/ 321, /*dst*/ 654, pdu, 1);
    rc |= expect_has_point(st.dmr_lrrp_gps[0], exp_lat, exp_lon, "unknown type with point token");

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
