// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression (#453, second item): some senders put a length byte on an LRRP
 * response that under-counts the tokens that follow. The length byte is still
 * trusted first; the bytes up to the UDP payload end are a second, penalised
 * candidate that wins only when it finds something the declared walk did not
 * (a position, or a timestamp plus speed plus heading). Junk past the declared
 * length never buys a position, and a conformant message decodes as before.
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

static int
expect_has_point(const char* s, double exp_lat, double exp_lon, const char* tag) {
    const char* p = strchr(s, '(');
    if (!p) {
        DSD_FPRINTF(stderr, "%s: no position in '%s'\n", tag, s);
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

static int
expect_exact(const char* s, const char* expect, const char* tag) {
    if (strcmp(s, expect) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' expected '%s'\n", tag, s, expect);
        return 1;
    }
    return 0;
}

static size_t
put_point_2d(uint8_t* pdu, size_t i, uint32_t lat_raw, uint32_t lon_raw) {
    pdu[i++] = 0x66; // POINT_2D token id
    pdu[i++] = (uint8_t)((lat_raw >> 24) & 0xFF);
    pdu[i++] = (uint8_t)((lat_raw >> 16) & 0xFF);
    pdu[i++] = (uint8_t)((lat_raw >> 8) & 0xFF);
    pdu[i++] = (uint8_t)(lat_raw & 0xFF);
    pdu[i++] = (uint8_t)((lon_raw >> 24) & 0xFF);
    pdu[i++] = (uint8_t)((lon_raw >> 16) & 0xFF);
    pdu[i++] = (uint8_t)((lon_raw >> 8) & 0xFF);
    pdu[i++] = (uint8_t)(lon_raw & 0xFF);
    return i;
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

    const uint32_t lat_raw = 0x10000000u;
    const uint32_t lon_raw = 0x20000000u;
    const double exp_lat = ((double)((int32_t)lat_raw) * 90.0) / 2147483648.0;
    const double exp_lon = ((double)((int32_t)lon_raw) * 180.0) / 2147483648.0;

    // 1. Length byte says 0, a POINT_2D follows anyway: the position is recovered.
    {
        uint8_t pdu[32];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        size_t i = 0;
        pdu[i++] = 0x0D; // Triggered Location report
        pdu[i++] = 0x00; // declared token length (wrong)
        i = put_point_2d(pdu, i, lat_raw, lon_raw);
        DSD_MEMSET(st.dmr_lrrp_gps[0], 0, sizeof st.dmr_lrrp_gps[0]);
        dmr_lrrp(&opts, &st, (uint16_t)i, 123, 456, pdu, 1);
        rc |= expect_has_point(st.dmr_lrrp_gps[0], exp_lat, exp_lon, "length 0, position follows");
    }

    // 2. Length byte covers only the identity token; the POINT_2D past it is recovered.
    {
        uint8_t pdu[32];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        size_t i = 0;
        pdu[i++] = 0x0D;
        pdu[i++] = 0x02; // declared: identity token only
        pdu[i++] = 0x22; // IDENTITY, zero-length request id
        pdu[i++] = 0x00;
        i = put_point_2d(pdu, i, lat_raw, lon_raw);
        DSD_MEMSET(st.dmr_lrrp_gps[0], 0, sizeof st.dmr_lrrp_gps[0]);
        dmr_lrrp(&opts, &st, (uint16_t)i, 123, 456, pdu, 1);
        rc |= expect_has_point(st.dmr_lrrp_gps[0], exp_lat, exp_lon, "length 2, position follows");
    }

    // 3. Length byte says 0 and only junk follows: no position is invented.
    {
        static const uint8_t pdu[] = {0x0D, 0x00, 0x00, 0x00, 0x00};
        DSD_MEMSET(st.dmr_lrrp_gps[0], 0, sizeof st.dmr_lrrp_gps[0]);
        dmr_lrrp(&opts, &st, (uint16_t)sizeof pdu, 123, 456, pdu, 1);
        rc |= expect_exact(st.dmr_lrrp_gps[0], "LRRP SRC: 123; Response to TGT: 456;", "length 0, junk follows");
    }

    // 4. Conformant message with two trailing bytes: decodes exactly as the declared walk does.
    {
        uint8_t pdu[32];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        size_t i = 0;
        pdu[i++] = 0x07; // Immediate Location Response
        pdu[i++] = 0x09; // declared: exactly the POINT_2D
        i = put_point_2d(pdu, i, lat_raw, lon_raw);
        pdu[i++] = 0x00;
        pdu[i++] = 0x00;
        DSD_MEMSET(st.dmr_lrrp_gps[0], 0, sizeof st.dmr_lrrp_gps[0]);
        dmr_lrrp(&opts, &st, (uint16_t)i, 123, 456, pdu, 1);
        rc |= expect_has_point(st.dmr_lrrp_gps[0], exp_lat, exp_lon, "conformant, trailing bytes");
    }

    // 5. A request type never gets the fallback: a POINT_2D after a zero length stays unread.
    {
        uint8_t pdu[32];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        size_t i = 0;
        pdu[i++] = 0x05; // Immediate Location Request
        pdu[i++] = 0x00;
        i = put_point_2d(pdu, i, lat_raw, lon_raw);
        DSD_MEMSET(st.dmr_lrrp_gps[0], 0, sizeof st.dmr_lrrp_gps[0]);
        dmr_lrrp(&opts, &st, (uint16_t)i, 123, 456, pdu, 1);
        rc |= expect_exact(st.dmr_lrrp_gps[0], "LRRP SRC: 123; Request from TGT: 456;", "request, no fallback");
    }

    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
