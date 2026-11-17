// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/* Issue #550: CRC-failed GPS fixes remain visible but never reach the location file. */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/position.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <dsd-neo/protocol/pdu.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/call_state.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

// Minimal stubs for the real dsd_gps.c; use its public, const-correct interfaces.
const char*
dsd_degrees_glyph(void) {
    return "";
}

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    (void)timestamp;
    const char* value;
    switch (format) {
        case DSD_LOCAL_DATETIME_DATE_COMPACT: value = "19990102"; break;
        case DSD_LOCAL_DATETIME_TIME_COMPACT: value = "112233"; break;
        case DSD_LOCAL_DATETIME_DATE_SLASH: value = "1999/01/02"; break;
        case DSD_LOCAL_DATETIME_TIME_COLON: value = "11:22:33"; break;
        case DSD_LOCAL_DATETIME_DATE_HYPHEN: value = "1999-01-02"; break;
        default: value = ""; break;
    }
    DSD_SNPRINTF(out, out_size, "%s", value);
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

static void
set_bits_msb(uint8_t* bits, size_t offset, unsigned width, uint32_t value) {
    for (unsigned i = 0; i < width; ++i) {
        bits[offset + i] = (uint8_t)((value >> (width - i - 1U)) & 1U);
    }
}

static int
truncate_output(const char* path) {
    FILE* file = dsd_fopen_private(path, "w");
    return file != NULL && fclose(file) == 0;
}

static int
expect_rows(const char* path, int expected, const char* src, const char* lat, const char* lon, int speed, int azimuth,
            const char* date, const char* tag) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        DSD_FPRINTF(stderr, "\n%s: cannot read output\n", tag);
        return 0;
    }
    int rows = 0;
    char line[512];
    char last[512] = "";
    while (fgets(line, sizeof line, file) != NULL) {
        if (strchr(line, '\n') != NULL) {
            ++rows;
        }
        DSD_SNPRINTF(last, sizeof last, "%s", line);
    }
    int read_error = ferror(file);
    fclose(file);
    if (read_error || rows != expected || (expected == 0 && last[0] != '\0')) {
        DSD_FPRINTF(stderr, "\n%s: expected %d rows, got %d; last row: %s\n", tag, expected, rows, last);
        return 0;
    }
    if (expected == 0) {
        return 1;
    }
    char* fields[7];
    char* cursor = last;
    for (size_t i = 0; i < 7; ++i) {
        if (cursor == NULL) {
            DSD_FPRINTF(stderr, "\n%s: missing column %zu\n", tag, i);
            return 0;
        }
        fields[i] = cursor;
        char* tab = strchr(cursor, '\t');
        if (tab != NULL) {
            *tab = '\0';
            cursor = tab + 1;
        } else {
            cursor = NULL;
        }
    }
    if (strcmp(fields[2], src) != 0 || strcmp(fields[3], lat) != 0 || strcmp(fields[4], lon) != 0
        || strtol(fields[5], NULL, 10) != speed || strtol(fields[6], NULL, 10) != azimuth
        || (date != NULL && strcmp(fields[0], date) != 0)) {
        DSD_FPRINTF(stderr, "\n%s: unexpected src/lat/lon/speed/azimuth: %s / %s / %s / %s / %s\n", tag, fields[2],
                    fields[3], fields[4], fields[5], fields[6]);
        return 0;
    }
    return 1;
}

static int
test_usbd_lip(const dsd_opts* opts, dsd_state* state, const char* path) {
    int rc = 0;
    uint8_t lip[96] = {0};
    set_bits_msb(lip, 4, 2, 1);
    set_bits_msb(lip, 6, 1, 1);
    set_bits_msb(lip, 7, 24, 0xC00000);
    set_bits_msb(lip, 31, 1, 1);
    set_bits_msb(lip, 32, 23, 0x600000);
    set_bits_msb(lip, 55, 3, 1);
    set_bits_msb(lip, 58, 7, 20);
    set_bits_msb(lip, 72, 8, 0xDA);
    // Service, direction, reason and spare bits are zero. Bit 80 belongs to the CRC, never the hash.
    char clean_lip[sizeof state->dmr_embedded_gps[1]] = "";
    const char* lip_tags[] = {"LIP clean", "LIP CRC-invalid", "LIP slot isolation", "LIP CRC bit 80"};
    for (int i = 0; i < 4; ++i) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->currentslot = 1;
        state->event_crc_invalid[0] = (i == 2);
        state->event_crc_invalid[1] = (i == 1);
        DSD_MEMSET(state->dmr_embedded_gps[1], 0, sizeof state->dmr_embedded_gps[1]);
        lip[80] = (uint8_t)(i == 3);
        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "lip_reason") != 0) {
            return 100;
        }
        lip_protocol_decoder(opts, state, lip);
        dsd_test_capture_stderr_end(&cap);
        char output[2048];
        if (dsd_test_capture_stderr_read(&cap, output, sizeof output) != 0) {
            return 100;
        }
        if (!strstr(output, "Response to an immediate location request")) {
            DSD_FPRINTF(stderr, "T1 reason: missing Response to an immediate location request in %s\n", output);
            rc = 1;
        }
        // The hash is no radio ID: a USBD reply never reaches the location file (decision D8).
        if (!expect_rows(path, 0, NULL, NULL, NULL, 0, 0, NULL, lip_tags[i])) {
            rc = 1;
        }
        const char* gps = state->dmr_embedded_gps[1];
        if (i == 0) {
            if (!strstr(gps, "218; LIP:") || !strstr(gps, "22.50000S") || !strstr(gps, "45.00000W")
                || !strstr(gps, "Err: 20m")) {
                DSD_FPRINTF(stderr, "\nLIP clean: unexpected state: %s\n", gps);
                rc = 1;
            }
            DSD_MEMCPY(clean_lip, gps, sizeof clean_lip);
        } else if (i == 1 ? gps[0] != '\0' : memcmp(clean_lip, gps, sizeof clean_lip) != 0) {
            // A CRC-failed reply leaves no position text (spec 2.11); the other slot's scope and the CRC bit do not count.
            DSD_FPRINTF(stderr, "\n%s: unexpected decoded state: %s\n", lip_tags[i], gps);
            rc = 1;
        }
    }

    return rc;
}

static int
capture_lip_pdu(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t count, char output[2048]) {
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "short_lip") != 0) {
        return 1;
    }
    lip_pdu_decoder(opts, state, bits, count, 123456);
    dsd_test_capture_stderr_end(&cap);
    return dsd_test_capture_stderr_read(&cap, output, 2048) != 0;
}

static int
test_short_lip(dsd_opts* opts, dsd_state* state, const char* path) {
    uint8_t bits[76] = {0};
    set_bits_msb(bits, 2, 2, 1);
    set_bits_msb(bits, 4, 1, 1);
    set_bits_msb(bits, 5, 24, 0xC00000);
    set_bits_msb(bits, 29, 1, 1);
    set_bits_msb(bits, 30, 23, 0x600000);
    set_bits_msb(bits, 53, 3, 1);
    set_bits_msb(bits, 56, 7, 20);
    state->currentslot = 1;
    opts->lrrp_file_output = 1;
    const char* expected = "LIP: 22.50000S 45.00000W; Err: 20m; Spd: 20 km/h; Dir: 0";
    const char* labels[] = {"Response to an immediate location request", "Response to an immediate location request",
                            "User data: 0xA5", "Emergency condition is detected"};
    int rc = 0;
    for (int i = 0; i < 4; ++i) {
        state->event_crc_invalid[1] = (uint8_t)(i == 1);
        set_bits_msb(bits, 67, 1, (uint32_t)(i == 2));
        const uint32_t reasons[] = {32, 32, 0xA5, 2};
        set_bits_msb(bits, 68, 8, reasons[i]);
        if (!truncate_output(path)) {
            return 100;
        }
        char output[2048];
        if (capture_lip_pdu(opts, state, bits, sizeof bits, output)) {
            return 100;
        }
        if (!expect_rows(path, i == 1 ? 0 : 1, "00123456", "-22.500000", "-45.000000", 20, 0, "19990102",
                         "T7 short LIP")
            || strcmp(state->dmr_embedded_gps[1], expected) != 0 || !strstr(output, labels[i])
            || (i == 2 && strstr(output, "Reason:"))) {
            DSD_FPRINTF(stderr, "T7 short LIP variant %d: state=%s; console=%s\n", i, state->dmr_embedded_gps[1],
                        output);
            rc = 1;
        }
    }
    return rc;
}

/* Spec 2.3 (TS 100 392-18-1 Tables 6.51 and 6.45): velocity 126 is "more than 1043 km/h", 127 "not known"; direction
   15 is "337.5 or not known". None is printed or written as an exact value: the location file takes the bound for 126
   and its placeholder 0 for an unknown speed or an ambiguous heading. Position error 7 (D11) is still written. */
static int
test_short_lip_codes(dsd_opts* opts, dsd_state* state, const char* path) {
    static const struct {
        uint8_t velocity;
        uint8_t direction;
        uint8_t pos_err;
        int speed_column;
        int azimuth_column;
        const char* text;    // the row's and the display's text
        const char* console; // printed
    } cases[] = {
        // Table 6.51 gives 125 as 1 043 km/h: rounded, not truncated. A half-degree heading keeps its decimal.
        {125, 14, 1, 1043, 315, "Spd: 1043 km/h; Dir: 315", "Spd: 1043 km/h; Dir: 315"},
        {126, 1, 1, 1043, 22, "Spd: >1043 km/h; Dir: 22.5", "Spd: >1043 km/h; Dir: 22.5"},
        {30, 3, 1, 30, 67, "Spd: 30 km/h; Dir: 67.5", "Spd: 30 km/h; Dir: 67.5"},
        {127, 0, 1, 0, 0, "Spd: unknown; Dir: 0", "Spd: unknown; Dir: 0"},
        {28, 15, 1, 28, 0, "Spd: 28 km/h; Dir: 337.5 or unknown", "Spd: 28 km/h; Dir: 337.5 or unknown"},
        {29, 2, 7, 29, 45, "Unknown Pos Err (fix may be old or invalid); Spd: 29 km/h; Dir: 45",
         "Position Error: Unknown or Invalid"},
    };

    int rc = 0;
    state->currentslot = 1;
    state->event_crc_invalid[1] = 0;
    opts->lrrp_file_output = 1;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t bits[76] = {0};
        set_bits_msb(bits, 4, 1, 1);
        set_bits_msb(bits, 5, 24, 0xC00000);
        set_bits_msb(bits, 29, 1, 1);
        set_bits_msb(bits, 30, 23, 0x600000);
        set_bits_msb(bits, 53, 3, cases[i].pos_err);
        set_bits_msb(bits, 56, 7, cases[i].velocity);
        set_bits_msb(bits, 63, 4, cases[i].direction);
        if (!truncate_output(path)) {
            return 100;
        }
        char output[2048];
        if (capture_lip_pdu(opts, state, bits, sizeof bits, output)) {
            return 100;
        }
        if (!expect_rows(path, 1, "00123456", "-22.500000", "-45.000000", cases[i].speed_column,
                         cases[i].azimuth_column, NULL, "2.3 LIP codes")
            || !strstr(state->dmr_embedded_gps[1], cases[i].text) || !strstr(output, cases[i].console)) {
            DSD_FPRINTF(stderr, "2.3 LIP code case %zu: state=%s; console=%s\n", i, state->dmr_embedded_gps[1], output);
            rc = 1;
        }
    }
    // A report admission refuses ((0, 0) here) prints no coordinates.
    {
        uint8_t zero[76] = {0};
        char output[2048];
        if (!truncate_output(path) || capture_lip_pdu(opts, state, zero, sizeof zero, output)) {
            return 100;
        }
        if (!strstr(output, "Position: (refused)") || strstr(output, "Lat:") != NULL
            || !expect_rows(path, 0, NULL, NULL, NULL, 0, 0, NULL, "refused LIP")) {
            DSD_FPRINTF(stderr, "refused LIP console: %s\n", output);
            rc = 1;
        }
    }
    lip_short_report r;
    uint8_t bits[76] = {0};
    set_bits_msb(bits, 56, 7, 127);
    set_bits_msb(bits, 63, 4, 15);
    set_bits_msb(bits, 53, 3, 7);
    if (lip_parse_short_report(bits, sizeof bits, &r) != 1 || r.speed_dkmh != 0xFFFFU || r.heading_ddeg != 3375U
        || r.flags != (DSD_POS_FLAG_HEADING_AMBIGUOUS | DSD_POS_FLAG_VALIDITY_UNKNOWN) || r.err_m != 0U
        || lip_parse_short_report(bits, 75U, &r) != 0) {
        DSD_FPRINTF(stderr, "2.3 LIP parse: unexpected speed %u heading %u flags %u\n", r.speed_dkmh, r.heading_ddeg,
                    r.flags);
        rc = 1;
    }
    return rc;
}

static int
test_unsupported_lip(const dsd_opts* opts, dsd_state* state, const char* path) {
    const struct {
        unsigned type;
        unsigned extension;
        size_t length;
        const char* label;
    } cases[] = {
        {1, 3, 76, "Long location report"},
        {1, 1, 76, "Immediate location report request"},
        {0, 0, 75, "truncated"},
        {2, 0, 76, "reserved PDU type"},
        {0, 0, 1, "truncated"},
        {1, 0, 5, "truncated"},
        {3, 0, 76, "reserved PDU type"},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t bits[76] = {0};
        set_bits_msb(bits, 0, 2, cases[i].type);
        set_bits_msb(bits, 2, 4, cases[i].extension);
        DSD_SNPRINTF(state->dmr_embedded_gps[1], sizeof state->dmr_embedded_gps[1], "%s", "retained fix");
        if (!truncate_output(path)) {
            return 100;
        }
        char output[2048];
        if (capture_lip_pdu(opts, state, bits, cases[i].length, output)) {
            return 100;
        }
        if (!expect_rows(path, 0, NULL, NULL, NULL, 0, 0, NULL, "T7 unsupported LIP")
            || strcmp(state->dmr_embedded_gps[1], "retained fix") != 0 || !strstr(output, cases[i].label)) {
            DSD_FPRINTF(stderr, "T7 unsupported LIP variant %zu: state=%s; console=%s\n", i, state->dmr_embedded_gps[1],
                        output);
            rc = 1;
        }
    }
    return rc;
}

/* One long or short UDT NMEA report through the decoder: Table B.7 bounds (spec 2.5, V3, V4). */
typedef struct {
    int type;
    uint8_t q;
    uint8_t speed;
    uint8_t ndeg, nmin;
    uint16_t nminf;
    uint8_t edeg, emin;
    uint16_t eminf;
    uint8_t hh, mm, ss;
    uint16_t cog;
    int rows;      // 1 when the position reaches -L
    int speed_col; // -L speed column (km/h)
    int az_col;    // -L azimuth column (0 = unknown)
    const char* lat;
    const char* lon;
    const char* console; // must appear on the console
    const char* tag;
} udt_nmea_case;

static int
run_udt_nmea_case(const dsd_opts* opts, dsd_state* state, const char* path, const udt_nmea_case* c) {
    uint8_t nmea[192] = {0};
    set_bits_msb(nmea, 1, 1, 1); // north
    set_bits_msb(nmea, 2, 1, 0); // west
    set_bits_msb(nmea, 3, 1, c->q);
    set_bits_msb(nmea, 4, 7, c->speed);
    set_bits_msb(nmea, 11, 7, c->ndeg);
    set_bits_msb(nmea, 18, 6, c->nmin);
    set_bits_msb(nmea, 24, 14, c->nminf);
    set_bits_msb(nmea, 38, 8, c->edeg);
    set_bits_msb(nmea, 46, 6, c->emin);
    set_bits_msb(nmea, 52, 14, c->eminf);
    set_bits_msb(nmea, 66, 5, c->hh);
    set_bits_msb(nmea, 71, 6, c->mm);
    set_bits_msb(nmea, 77, c->type == 1 ? 3U : 6U, c->ss);
    set_bits_msb(nmea, 103, 9, c->cog);
    if (!truncate_output(path)) {
        return 0;
    }
    state->dmr_embedded_gps[0][0] = '\0';
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "udt_nmea") != 0) {
        return 0;
    }
    nmea_iec_61162_1(opts, state, nmea, 123456, c->type);
    dsd_test_capture_stderr_end(&cap);
    char output[2048];
    if (dsd_test_capture_stderr_read(&cap, output, sizeof output) != 0) {
        return 0;
    }
    const int text_ok = c->rows ? state->dmr_embedded_gps[0][0] != '\0' : state->dmr_embedded_gps[0][0] == '\0';
    if (!expect_rows(path, c->rows, "00123456", c->lat, c->lon, c->speed_col, c->az_col, NULL, c->tag) || !text_ok
        || strstr(output, c->console) == NULL) {
        DSD_FPRINTF(stderr, "%s: state=%s; console=%s\n", c->tag, state->dmr_embedded_gps[0], output);
        return 0;
    }
    return 1;
}

static int
test_udt_nmea_bounds(const dsd_opts* opts, dsd_state* state, const char* path) {
    static const udt_nmea_case cases[] = {
        {2, 1, 20, 22, 30, 0, 45, 0, 0, 11, 22, 33, 90, 1, 37, 90, "22.500000", "-45.000000", "11:22:33 UTC", "valid"},
        {2, 0, 20, 22, 30, 0, 45, 0, 0, 11, 22, 33, 90, 0, 0, 0, NULL, NULL, "no fix", "Q=0 no fix"},
        {2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, "GPS: (refused);", "refused (0, 0)"},
        {2, 1, 0, 89, 59, 9999, 179, 59, 9999, 0, 0, 0, 0, 1, 0, 0, "89.999998", "-179.999998", "COG: 0", "max"},
        {2, 1, 0, 90, 0, 0, 45, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, "invalid position", "NDEG 90"},
        {2, 1, 0, 22, 60, 0, 45, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, "invalid position", "NMIN 60"},
        {2, 1, 0, 22, 0, 10000, 45, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, "invalid position", "NMINF 10000"},
        {2, 1, 0, 22, 0, 0, 180, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, "invalid position", "EDEG 180"},
        {2, 1, 0, 22, 0, 0, 45, 60, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, "invalid position", "EMIN 60"},
        {2, 1, 0, 22, 0, 0, 45, 0, 10000, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, "invalid position", "EMINF 10000"},
        {2, 1, 127, 22, 30, 0, 45, 0, 0, 0, 0, 0, 359, 1, 233, 359, "22.500000", "-45.000000", "SPD > 126", "speed"},
        {2, 1, 0, 22, 30, 0, 45, 0, 0, 0, 0, 0, 360, 1, 0, 0, "22.500000", "-45.000000", "COG: unknown", "COG 360"},
        {2, 1, 0, 22, 30, 0, 45, 0, 0, 23, 59, 59, 0, 1, 0, 0, "22.500000", "-45.000000", "23:59:59 UTC", "time max"},
        {2, 1, 0, 22, 30, 0, 45, 0, 0, 24, 0, 0, 0, 1, 0, 0, "22.500000", "-45.000000", "time unknown", "hour 24"},
        {2, 1, 0, 22, 30, 0, 45, 0, 0, 0, 60, 0, 0, 1, 0, 0, "22.500000", "-45.000000", "time unknown", "minute 60"},
        {2, 1, 0, 22, 30, 0, 45, 0, 0, 0, 0, 60, 0, 1, 0, 0, "22.500000", "-45.000000", "time unknown", "second 60"},
        {1,
         1,
         0,
         22,
         30,
         0,
         45,
         0,
         0,
         31,
         63,
         7,
         0,
         1,
         0,
         0,
         "22.500000",
         "-45.000000",
         "time unknown",
         "short 31:63:7"},
        {1, 1, 0, 22, 30, 0, 45, 0, 0, 12, 0, 5, 0, 1, 0, 0, "22.500000", "-45.000000", "12:00:50 UTC", "short code 5"},
        {1, 1, 0, 22, 30, 0, 45, 0, 0, 12, 0, 6, 0, 1, 0, 0, "22.500000", "-45.000000", "time unknown", "short code 6"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        if (!run_udt_nmea_case(opts, state, path, &cases[i])) {
            return 30 + (int)i;
        }
    }
    return 0;
}

/* Spec 2.2: the embedded GPS LC's source is the slot's ACTIVE call's only while a checked full LC of that call (voice
   header or late entry, not RAS: dmr_flco) named it in this transmission; otherwise the fix is written unattributed. */
static int
test_embedded_attribution(const dsd_opts* opts, dsd_state* state, const char* path, const uint8_t* dmr_lc) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = 1U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 1201U,
        .policy_target_id = 1201U,
        .ota_source_id = 777U,
    };
    if (dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) < 0) {
        return 100;
    }
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, 1U, &call) <= 0) {
        return 100;
    }

    static const struct {
        int epoch_matches;
        uint32_t checked_source;
        const char* src;
        const char* tag;
    } cases[] = {
        {1, 777U, "00000777", "checked LC named the call's source"},
        {1, 778U, "00000000", "checked LC named another source"},
        {0, 777U, "00000000", "checked LC of an earlier transmission"},
    };

    state->currentslot = 1;
    state->event_crc_invalid[1] = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        state->dmr_lc_checked_transmission[1] =
            cases[i].epoch_matches ? dsd_call_snapshot_transmission(&call) : dsd_call_snapshot_transmission(&call) + 1U;
        state->dmr_lc_checked_source[1] = cases[i].checked_source;
        if (!truncate_output(path)) {
            return 100;
        }
        dmr_embedded_gps(opts, state, dmr_lc);
        if (!expect_rows(path, 1, cases[i].src, "-22.500000", "-45.000000", 0, 0, NULL, cases[i].tag)) {
            return 26 + (int)i;
        }
    }
    // A fix admission refuses ((0, 0) here) prints no coordinates and leaves no text and no -L row.
    {
        uint8_t zero_lc[96] = {0};
        set_bits_msb(zero_lc, 20, 3, 1);
        state->dmr_embedded_gps[1][0] = '\0';
        if (!truncate_output(path)) {
            return 100;
        }
        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "embedded_refused") != 0) {
            return 100;
        }
        dmr_embedded_gps(opts, state, zero_lc);
        dsd_test_capture_stderr_end(&cap);
        char output[2048];
        if (dsd_test_capture_stderr_read(&cap, output, sizeof output) != 0) {
            return 100;
        }
        if (!strstr(output, "Position: (refused)") || strstr(output, "Lat:") != NULL
            || state->dmr_embedded_gps[1][0] != '\0'
            || !expect_rows(path, 0, NULL, NULL, NULL, 0, 0, NULL, "refused embedded GPS")) {
            DSD_FPRINTF(stderr, "refused embedded GPS: state=%s; console=%s\n", state->dmr_embedded_gps[1], output);
            return 29;
        }
    }
    (void)dsd_call_state_end(state, 1U, 0.0);
    state->dmr_lc_checked_transmission[1] = 0U;
    state->dmr_lc_checked_source[1] = 0U;
    return 0;
}

static int
run_cases(dsd_opts* opts, dsd_state* state, const char* path) {
    uint8_t nmea[192] = {0};
    // Long UDT NMEA: valid fix, 22 degrees 30 minutes S, 45 degrees W, stationary.
    set_bits_msb(nmea, 3, 1, 1);
    set_bits_msb(nmea, 11, 7, 22);
    set_bits_msb(nmea, 18, 6, 30);
    set_bits_msb(nmea, 38, 8, 45);
    set_bits_msb(nmea, 66, 5, 11);
    set_bits_msb(nmea, 71, 6, 22);
    set_bits_msb(nmea, 77, 6, 33);
    state->currentslot = 0;
    state->event_crc_invalid[1] = 0;
    for (int invalid = 0; invalid <= 1; ++invalid) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->event_crc_invalid[0] = invalid;
        state->dmr_embedded_gps[0][0] = '\0';
        nmea_iec_61162_1(opts, state, nmea, 123456, 2);
        if (!expect_rows(path, !invalid, "00123456", "-22.500000", "-45.000000", 0, 0, NULL, "UDT NMEA")) {
            return 7 + invalid * 2;
        }
        // A CRC-failed report leaves no position text (spec 2.11).
        if (invalid ? state->dmr_embedded_gps[0][0] != '\0' : strncmp(state->dmr_embedded_gps[0], "GPS: (", 6) != 0) {
            DSD_FPRINTF(stderr, "\nUDT NMEA: unexpected decoded GPS state: %s\n", state->dmr_embedded_gps[0]);
            return 8 + invalid * 2;
        }
    }
    state->event_crc_invalid[0] = 0;
    int udt_rc = test_udt_nmea_bounds(opts, state, path);
    if (udt_rc != 0) {
        return udt_rc;
    }

    // L3Harris GPS update body (TIA-102.AABF-D-1 §7.3.35-36): 22 degrees 30 minutes S, 45 degrees W, quality %01 (fix).
    // Phase 2 MAC on slot 1; only a message whose own check passed (pos_verified) reaches the location file.
    const uint8_t harris[DSD_HARRIS_GPS_BODY_OCTETS] = {0x00, 0x00, 0x9E, 22,   0x00, 0x00, 0x80,
                                                        45,   0x00, 0x00, 0x00, 0x00, 0x00, 0x10};
    state->event_crc_invalid[0] = 0;
    state->event_crc_invalid[1] = 0;
    for (int verified = 0; verified <= 1; ++verified) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->dmr_embedded_gps[1][0] = '\0';
        const dsd_gps_report_ctx ctx = {
            .protocol = DSD_SYNC_P25P2_POS,
            .slot = 1U,
            .source_id = 234567U,
            .source_verified = 1U,
            .pos_verified = (uint8_t)verified,
        };
        const int admitted = nmea_harris(opts, state, harris, &ctx);
        if ((admitted != 0) != verified
            || !expect_rows(path, verified, "00234567", "-22.500000", "-45.000000", 0, 0, NULL, "Harris MAC slot")) {
            return 11 + verified * 2;
        }
        if ((strstr(state->dmr_embedded_gps[1], "(-22.500000, -45.000000)") != NULL) != verified) {
            DSD_FPRINTF(stderr, "\nHarris: unexpected decoded GPS state: %s\n", state->dmr_embedded_gps[1]);
            return 12 + verified * 2;
        }
    }

    uint8_t nxdn[272] = {0};
    set_bits_msb(nxdn, 74, 14, 200);
    set_bits_msb(nxdn, 136, 7, 26);
    set_bits_msb(nxdn, 143, 4, 1);
    set_bits_msb(nxdn, 152, 16, 4500);
    set_bits_msb(nxdn, 183, 1, 1);
    set_bits_msb(nxdn, 184, 16, 2200);
    set_bits_msb(nxdn, 215, 1, 1);
    // NXDN never sets this flag in production: cover the slash/colon writer's gate, not an NXDN CRC path.
    state->currentslot = 1;
    state->event_crc_invalid[1] = 0;
    for (int invalid = 1; invalid >= 0; --invalid) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->event_crc_invalid[0] = invalid;
        state->dmr_lrrp_source[0] = 345678;
        nxdn_gps_report(opts, state, nxdn, sizeof(nxdn), 345678, NULL);
        if (!expect_rows(path, !invalid, "00345678", "-22.000000", "-45.000000", 20, 0, "1999/01/02", "NXDN")) {
            return 15 + (1 - invalid);
        }
    }

    uint8_t dmr_lc[96] = {0};
    // Clear voice LC: position error 20m, 22.5 S, 45 W in two's complement.
    set_bits_msb(dmr_lc, 20, 3, 1);
    set_bits_msb(dmr_lc, 23, 1, 1);
    set_bits_msb(dmr_lc, 24, 24, 0xC00000);
    set_bits_msb(dmr_lc, 48, 1, 1);
    set_bits_msb(dmr_lc, 49, 23, 0x600000);
    state->currentslot = 1;
    state->event_crc_invalid[0] = 0;
    for (int invalid = 1; invalid >= 0; --invalid) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->event_crc_invalid[1] = invalid;
        state->dmr_embedded_gps[1][0] = '\0';
        dmr_embedded_gps(opts, state, dmr_lc);
        if (!expect_rows(path, !invalid, "00000000", "-22.500000", "-45.000000", 0, 0, "19990102", "DMR voice LC")) {
            return 18 + (1 - invalid) * 2;
        }
        if (strcmp(state->dmr_embedded_gps[1], invalid ? "" : "GPS: 22.50000S 45.00000W Err: 20m") != 0) {
            DSD_FPRINTF(stderr, "\nDMR voice LC: unexpected decoded GPS state: %s\n", state->dmr_embedded_gps[1]);
            return 19 + (1 - invalid) * 2;
        }
    }
    int embedded_rc = test_embedded_attribution(opts, state, path, dmr_lc);
    if (embedded_rc != 0) {
        return embedded_rc;
    }

    uint8_t apx_lc[96] = {0};
    set_bits_msb(apx_lc, 1, 1, 1);
    set_bits_msb(apx_lc, 23, 1, 1);
    set_bits_msb(apx_lc, 24, 1, 1);
    set_bits_msb(apx_lc, 25, 23, 0x200000);
    set_bits_msb(apx_lc, 48, 1, 1);
    set_bits_msb(apx_lc, 49, 23, 0x300000);
    // APX voice LC: the location file takes it only after a hard RS decode (pos_verified). Bit 23 has no documented
    // meaning and is not read as "last fix".
    state->event_crc_invalid[1] = 0;
    state->currentslot = 1;
    for (int verified = 0; verified <= 1; ++verified) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->dmr_embedded_gps[1][0] = '\0';
        const dsd_gps_report_ctx ctx = {.protocol = DSD_SYNC_P25P1_POS, .slot = 1U, .pos_verified = (uint8_t)verified};
        (void)apx_embedded_gps(opts, state, apx_lc, &ctx);
        if (!expect_rows(path, verified, "00000000", "-22.500003", "-112.499992", 0, 0, "19990102", "APX voice LC")) {
            return 22 + verified * 2;
        }
        const char* apx_state = state->dmr_embedded_gps[1];
        if ((strstr(apx_state, "(-22.500003, -112.499992)") != NULL) != verified || strstr(apx_state, "Last Fix")) {
            DSD_FPRINTF(stderr, "\nAPX voice LC: unexpected decoded GPS state: %s\n", apx_state);
            return 23 + verified * 2;
        }
    }

    if (!truncate_output(path)) {
        return 100;
    }
    opts->lrrp_file_output = 0;
    state->event_crc_invalid[0] = 0;
    state->event_crc_invalid[1] = 0;
    const uint8_t lip[96] = {0};
    lip_protocol_decoder(opts, state, lip);
    if (!expect_rows(path, 0, NULL, NULL, NULL, 0, 0, NULL, "Output disabled")) {
        return 17;
    }
    return 0;
}

// UDP 231 (Cellocator) payloads reach decode_cellocator() with their own length; the decoder reads a
// four-octet signature and a type octet, so anything shorter is reported, not read past (issue #746).
static int
test_cellocator_short_payloads(dsd_opts* opts, dsd_state* state) {
    int rc = 0;
    for (int len = 0; len <= 5; len++) {
        uint8_t* payload = (uint8_t*)malloc(len > 0 ? (size_t)len : 1U);
        if (payload == NULL) {
            return 1;
        }
        DSD_MEMSET(payload, 0x04, len > 0 ? (size_t)len : 1U);
        char output[2048] = {0};
        dsd_test_capture_stderr cap;
        if (dsd_test_capture_stderr_begin(&cap, "cellocator") != 0) {
            free(payload);
            return 1;
        }
        decode_cellocator(opts, state, payload, len);
        dsd_test_capture_stderr_end(&cap);
        free(payload);
        if (dsd_test_capture_stderr_read(&cap, output, sizeof output) != 0) {
            return 1;
        }
        const char* want = (len < 5) ? "Cellocator: Truncated;" : "Time and Location Data;";
        if (strstr(output, want) == NULL) {
            DSD_FPRINTF(stderr, "cellocator len %d: missing '%s' in '%s'\n", len, want, output);
            rc |= 1;
        }
    }
    return rc;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "core_gps_lrrp_crc");
    if (fd < 0) {
        return 101;
    }
    if (dsd_close(fd) != 0) {
        remove(path);
        return 102;
    }
    DSD_SNPRINTF(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "%s", path);
    opts.lrrp_file_output = 1;
    int rc = test_usbd_lip(&opts, &state, path);
    rc |= test_short_lip(&opts, &state, path);
    rc |= test_short_lip_codes(&opts, &state, path);
    rc |= test_unsupported_lip(&opts, &state, path);
    rc |= run_cases(&opts, &state, path);
    rc |= test_cellocator_short_payloads(&opts, &state);
    remove(path);
    if (rc == 0) {
        DSD_FPRINTF(stdout, "CORE_GPS_LRRP_CRC_GATING: PASS\n");
    }
    return rc;
}
