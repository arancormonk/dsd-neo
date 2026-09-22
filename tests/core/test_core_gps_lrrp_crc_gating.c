// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/* Issue #550: CRC-failed GPS fixes remain visible but never reach the location file. */

#include <dsd-neo/core/events.h>
#include <dsd-neo/core/gps.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
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
        if (!expect_rows(path, i == 1 ? 0 : 1, "00000218", "-22.500000", "-45.000000", 20, 0, "19990102",
                         lip_tags[i])) {
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
        } else if (memcmp(clean_lip, gps, sizeof clean_lip) != 0) {
            DSD_FPRINTF(stderr, "\n%s: decoded state changed: %s\n", lip_tags[i], gps);
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
        if (strncmp(state->dmr_embedded_gps[0], "GPS: (", 6) != 0) {
            DSD_FPRINTF(stderr, "\nUDT NMEA: missing decoded GPS state\n");
            return 8 + invalid * 2;
        }
    }

    uint8_t harris[192] = {0};
    // P25 phase 2 MAC shape: GPS starts at bit 40; explicit slot 2 clamps to index 1.
    set_bits_msb(harris, 0, 16, 0x80AA);
    set_bits_msb(harris, 40 + 16, 1, 1);
    set_bits_msb(harris, 40 + 17, 7, 30);
    set_bits_msb(harris, 40 + 24, 8, 22);
    set_bits_msb(harris, 40 + 48, 1, 1);
    set_bits_msb(harris, 40 + 56, 8, 45);
    state->event_crc_invalid[0] = 0;
    for (int invalid = 1; invalid >= 0; --invalid) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->event_crc_invalid[1] = invalid;
        state->dmr_embedded_gps[1][0] = '\0';
        nmea_harris(opts, state, harris, 234567, 2);
        if (!expect_rows(path, !invalid, "00234567", "-22.500000", "-45.000000", 0, 0, NULL, "Harris MAC slot")) {
            return 11 + (1 - invalid) * 2;
        }
        if (!strstr(state->dmr_embedded_gps[1], "(-22.500000, -45.000000)")) {
            DSD_FPRINTF(stderr, "\nHarris: missing decoded GPS state\n");
            return 12 + (1 - invalid) * 2;
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
        nxdn_gps_report(opts, state, nxdn, 345678);
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
        if (strcmp(state->dmr_embedded_gps[1], "GPS: 22.50000S 45.00000W Err: 20m") != 0) {
            DSD_FPRINTF(stderr, "\nDMR voice LC: unexpected decoded GPS state: %s\n", state->dmr_embedded_gps[1]);
            return 19 + (1 - invalid) * 2;
        }
    }

    uint8_t apx_lc[96] = {0};
    set_bits_msb(apx_lc, 1, 1, 1);
    set_bits_msb(apx_lc, 23, 1, 1);
    set_bits_msb(apx_lc, 24, 1, 1);
    set_bits_msb(apx_lc, 25, 23, 0x200000);
    set_bits_msb(apx_lc, 48, 1, 1);
    set_bits_msb(apx_lc, 49, 23, 0x300000);
    // APX voice LC has no production CRC scope: exercise the writer's gate directly.
    for (int invalid = 1; invalid >= 0; --invalid) {
        if (!truncate_output(path)) {
            return 100;
        }
        state->event_crc_invalid[1] = invalid;
        state->dmr_embedded_gps[1][0] = '\0';
        apx_embedded_gps(opts, state, apx_lc);
        if (!expect_rows(path, !invalid, "00000000", "-22.500003", "-112.499992", 0, 0, "19990102", "APX voice LC")) {
            return 22 + (1 - invalid) * 2;
        }
        if (strstr(state->dmr_embedded_gps[1], "(-22.500003, -112.499992) Last Fix") == NULL) {
            DSD_FPRINTF(stderr, "\nAPX voice LC: unexpected decoded GPS state: %s\n", state->dmr_embedded_gps[1]);
            return 23 + (1 - invalid) * 2;
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
    rc |= test_unsupported_lip(&opts, &state, path);
    rc |= run_cases(&opts, &state, path);
    remove(path);
    if (rc == 0) {
        DSD_FPRINTF(stdout, "CORE_GPS_LRRP_CRC_GATING: PASS\n");
    }
    return rc;
}
