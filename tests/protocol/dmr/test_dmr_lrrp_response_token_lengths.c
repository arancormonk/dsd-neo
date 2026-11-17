// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression/parity: response token parsing must skip fixed-length tokens as single units.
 *
 * If we treat fixed-length token payload bytes as standalone tokens, we can desync when a payload
 * byte coincidentally matches a known token id (eg 0x66), producing an incorrect position decode.
 *
 * Issue #800 (bugs 2.16, 2.17, R6-1, V3, and 2.5 for the 0x34 date): the variable-length token encoding, checked on
 * the admitted position record and the `-L` row. Most vectors are real off-air reports (research R6: Capacity Max
 * 0x55 report with a 39-byte length fixed by its UDP header, 0x69 / 0x51 / 0x6C reports, 0x53 / 0x67 integer variants,
 * 0x6B integer speed, document type 0x13) with their latitude and longitude replaced by the synthetic 22.5, 45.0
 * (20000000 20000000); the rest are the issue's and the vendor example's vectors.
 */

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
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
#include "test_support.h"

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
lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count, uint32_t src) {
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

// The admitted record and `-L` row a report gives. alt_dm, err_m, speed_dkmh and heading_ddeg use the record's
// "unknown" values; row is the row's speed and heading columns ("" when no row is written).
typedef struct {
    const char* tag;
    const char* hex;
    int admitted;
    int32_t alt_dm;
    uint32_t err_m;
    uint16_t speed_dkmh;
    uint16_t heading_ddeg;
    int64_t fix_time_s;
    const char* row;
} encoding_case;

#define ALT_NONE DSD_POSITION_ALT_UNKNOWN
#define SPD_NONE DSD_POSITION_SPEED_UNKNOWN
#define HDG_NONE DSD_POSITION_HEADING_UNKNOWN
#define LL       "2000000020000000"

static const encoding_case k_encoding_cases[] = {
    // S20, Capacity Max circle-3d (0x55): radius 64.97 m, altitude 220.16 m, accuracy 74.71 m, speed 0.80 m/s,
    // heading 210, then 0x70 (vertical speed) ends the parse. Fixed widths misread every field.
    {"S20 circle-3d",
     "0D25220300000134"
     "1FA0491E4C"
     "55" LL "407C815C144A5B"
     "6C0067"
     "5669"
     "70404C"
     "653C",
     1, 2202, 65U, 29U, 2100U, 1704391032, "2.900\t 210"},
    // S03, point-3d (0x69): altitude 440.16 m (the 24-bit reading gave 85995.73 m).
    {"S03 point-3d",
     "0D1A22047FFFFFFF"
     "69" LL "833815"
     "6C0008"
     "56A2"
     "70400A",
     1, 4402, 0U, 2U, 3240U, 0, "0.200\t 324"},
    // S17, circle-3d integer variant (0x53): radius 2, altitude +68 (sintvar 80 44), accuracy 3.
    {"S17 circle-3d integer",
     "0D19220431323334341FA5FB1280"
     "53" LL "02"
     "8044"
     "03",
     1, 680, 2U, SPD_NONE, HDG_NONE, 1753809000, "0.000\t 0"},
    // S18, point-3d integer variant (0x67): altitude 66.
    {"S18 point-3d integer",
     "0711341FA5CA6D34"
     "67" LL "8042",
     1, 660, 0U, SPD_NONE, HDG_NONE, 1751698372, "0.000\t 0"},
    // S24, circle-2d (0x51): radius 5.66 m (rounds up to 6), speed 7.39 m/s, heading 216.
    {"S24 circle-2d",
     "0D15220300000151" LL "0554"
     "6C0732"
     "566C",
     1, ALT_NONE, 6U, 266U, 2160U, 0, "26.600\t 216"},
    // S14, unsolicited report (type 0x13) with integer speed (0x6B): 11 m/s, heading 64.
    {"S14 type 0x13",
     "1315232F341F4AD07B2E"
     "66" LL "6B0B"
     "5620",
     1, ALT_NONE, 0U, 396U, 640U, 1036741486, "39.600\t 64"},
    // S30, integer speed 20 m/s.
    {"S30 integer speed",
     "0D182203FFFFCC341F9F0CF921"
     "66" LL "6B14"
     "565E",
     1, ALT_NONE, 0U, 720U, 1880U, 1701876993, "72.000\t 188"},
    // The vendor example: radius 00 63 = 99/128 m (0.77), speed 00 06 = 6/128 m/s (0.04688).
    {"vendor example",
     "071A22042468ACE0341F4DBC7780"
     "51" LL "0063"
     "6C0006",
     1, ALT_NONE, 1U, 2U, HDG_NONE, 1056958200, "0.200\t 0"},
    // Bug 2.16: a point-3d with a short altitude (5.0 m) is no longer "truncated".
    {"2.16 short altitude", "070B69" LL "0500", 1, 50, 0U, SPD_NONE, HDG_NONE, 0, "0.000\t 0"},
    // Bug 2.17: 6C 0A 00 is 10 m/s, 36 km/h (not 41.2).
    {"2.17 speed", "070C66" LL "6C0A00", 1, ALT_NONE, 0U, 360U, HDG_NONE, 0, "36.000\t 0"},
    // V3: 179 steps is 358 degrees; 180 or more is no heading, and the position stays.
    {"heading 179", "070B66" LL "56B3", 1, ALT_NONE, 0U, SPD_NONE, 3580U, 0, "0.000\t 358"},
    {"heading 180", "070B66" LL "56B4", 1, ALT_NONE, 0U, SPD_NONE, HDG_NONE, 0, "0.000\t 0"},
    {"heading 255", "070B66" LL "56FF", 1, ALT_NONE, 0U, SPD_NONE, HDG_NONE, 0, "0.000\t 0"},
    // A token with no real sample (0x68) ends the parse: the position before it stays, the speed after it is not read.
    {"unsampled after", "070C66" LL "686B05", 1, ALT_NONE, 0U, SPD_NONE, HDG_NONE, 0, "0.000\t 0"},
    // ... and before the position, no position is published (a resync guess only).
    {"unsampled before",
     "070A54"
     "66" LL,
     0, ALT_NONE, 0U, SPD_NONE, HDG_NONE, 0, ""},
    // The document length is a uintvar (80 0D = 13).
    {"uintvar length",
     "07800D66" LL "6B05"
     "5610",
     1, ALT_NONE, 0U, 180U, 320U, 0, "18.000\t 32"},
    // Bug 2.5: 31 February 2024 is no date; 29 February 2024 is.
    {"31 February",
     "071034"
     "1FA0BE0000"
     "66" LL,
     1, ALT_NONE, 0U, SPD_NONE, HDG_NONE, 0, "0.000\t 0"},
    {"29 February",
     "071034"
     "1FA0BA0000"
     "66" LL,
     1, ALT_NONE, 0U, SPD_NONE, HDG_NONE, 1709164800, "0.000\t 0"},
    // A cut altitude (continuation bit on the last byte) is no position.
    {"cut altitude", "070B69" LL "8300", 0, ALT_NONE, 0U, SPD_NONE, HDG_NONE, 0, ""},
};

static uint8_t
hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'A' && c <= 'F') {
        return (uint8_t)(c - 'A' + 10);
    }
    return (uint8_t)(c - 'a' + 10);
}

static size_t
hex_to_bytes(const char* hex, uint8_t* out, size_t cap) {
    size_t n = 0;
    for (size_t i = 0; hex[i] != '\0' && hex[i + 1] != '\0' && n < cap; i += 2) {
        out[n++] = (uint8_t)((hex_nibble(hex[i]) << 4) | hex_nibble(hex[i + 1]));
    }
    return n;
}

static int
check_record(const encoding_case* c, const dsd_position_fix* fix) {
    if (fix->rev == 0U || fix->lat_e7 != 225000000 || fix->lon_e7 != 450000000 || fix->alt_dm != c->alt_dm
        || fix->err_m != c->err_m || fix->speed_dkmh != c->speed_dkmh || fix->heading_ddeg != c->heading_ddeg
        || fix->fix_time_s != c->fix_time_s) {
        DSD_FPRINTF(stderr,
                    "%s: rev %llu (%d, %d) alt %d err %u speed %u heading %u time %lld; want alt %d err %u speed %u "
                    "heading %u time %lld\n",
                    c->tag, (unsigned long long)fix->rev, (int)fix->lat_e7, (int)fix->lon_e7, (int)fix->alt_dm,
                    (unsigned)fix->err_m, (unsigned)fix->speed_dkmh, (unsigned)fix->heading_ddeg,
                    (long long)fix->fix_time_s, (int)c->alt_dm, (unsigned)c->err_m, (unsigned)c->speed_dkmh,
                    (unsigned)c->heading_ddeg, (long long)c->fix_time_s);
        return 1;
    }
    return 0;
}

static int
check_row(const char* path, const encoding_case* c) {
    char file[256] = "";
    FILE* f = fopen(path, "rb");
    if (f != NULL) {
        const size_t n = fread(file, 1, sizeof file - 1U, f);
        file[n] = '\0';
        fclose(f);
    }
    char want[160] = "";
    if (c->row[0] != '\0') {
        DSD_SNPRINTF(want, sizeof want, "1999/01/02\t11:22:33\t00000123\t22.50000\t45.00000\t%s\t\n", c->row);
    }
    if (strcmp(file, want) != 0) {
        DSD_FPRINTF(stderr, "%s: -L '%s', want '%s'\n", c->tag, file, want);
        return 1;
    }
    return 0;
}

static int
run_encoding_cases(void) {
    static dsd_opts opts;
    static dsd_state st;
    st.lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
    st.event_history_s = calloc(2, sizeof *st.event_history_s);
    if (st.event_history_s == NULL) {
        return 1;
    }
    int fd = dsd_test_mkstemp(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "dmr_lrrp_token_lengths");
    if (fd < 0) {
        free(st.event_history_s);
        return 1;
    }
    (void)dsd_close(fd);
    opts.lrrp_file_output = 1;

    int rc = 0;
    for (size_t i = 0; i < sizeof k_encoding_cases / sizeof k_encoding_cases[0]; i++) {
        const encoding_case* c = &k_encoding_cases[i];
        uint8_t pdu[64];
        const size_t len = hex_to_bytes(c->hex, pdu, sizeof pdu);
        FILE* trunc = dsd_fopen_private(opts.lrrp_out_file, "w");
        if (trunc == NULL || fclose(trunc) != 0) {
            rc = 1;
            break;
        }
        dsd_event_stage_clear(&st, 0);
        dmr_lrrp(&opts, &st, (uint16_t)len, /*src*/ 123, /*dst*/ 456, pdu, 1);
        const dsd_position_fix* fix = dsd_event_staged_position(&st, 0);
        if (c->admitted) {
            rc |= check_record(c, fix);
        } else if (fix->rev != 0U) {
            DSD_FPRINTF(stderr, "%s: a position was published\n", c->tag);
            rc |= 1;
        }
        rc |= check_row(opts.lrrp_out_file, c);
    }
    remove(opts.lrrp_out_file);
    free(st.event_history_s);
    return rc;
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

    uint32_t lat_raw = 0x10000000u;
    uint32_t lon_raw = 0x20000000u;
    double exp_lat = 0.0;
    double exp_lon = 0.0;
    expected_from_raw(lat_raw, lon_raw, &exp_lat, &exp_lon);

    // 0x31 TRIGGER_PERIODIC is a fixed-length token (2 bytes total). Make its payload byte look like a 0x66 token id.
    {
        uint8_t pdu[64];
        DSD_MEMSET(pdu, 0, sizeof pdu);
        size_t i = 0;
        pdu[i++] = 0x07; // Immediate Location Response
        pdu[i++] = 11;   // payload length: TRIGGER_PERIODIC (2) + POINT_2D (9)

        pdu[i++] = 0x31; // TRIGGER_PERIODIC
        pdu[i++] = 0x66; // payload byte (should NOT be treated as a token)

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
        rc |= expect_has_point(st.dmr_lrrp_gps[0], exp_lat, exp_lon, "trigger token length skip");
    }

    rc |= run_encoding_cases();
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
