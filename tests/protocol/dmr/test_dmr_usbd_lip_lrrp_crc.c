// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
/*
 * Issue #550: real USBD -> LIP -> location-file CRC gating, including relaxed RAS.
 * The test encoder reverses src/fec/bptc.c: place reserved bits and 96 payload bits
 * in a 13x15 matrix, add systematic Hamming(15,11) row and Hamming(13,9) column
 * parity using the literal check matrices from src/fec/fec.c, then interleave.
 * Compare against an independent existing burst before using generated codewords.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/fec/bptc.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static void
unpack(const uint8_t* bytes, uint8_t* bits, size_t count) {
    for (size_t i = 0; i < count * 8U; ++i) {
        bits[i] = (uint8_t)((bytes[i / 8U] >> (7U - i % 8U)) & 1U);
    }
}

static int
truncate_output(const char* path) {
    FILE* file = dsd_fopen_private(path, "w");
    return file != NULL && fclose(file) == 0;
}

static int
expect_rows(const char* path, int expected, const char* src, const char* lat, const char* lon, int speed, int azimuth,
            const char* tag) {
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
        || strtol(fields[5], NULL, 10) != speed || strtol(fields[6], NULL, 10) != azimuth) {
        DSD_FPRINTF(stderr, "\n%s: unexpected src/lat/lon/speed/azimuth: %s / %s / %s / %s / %s\n", tag, fields[2],
                    fields[3], fields[4], fields[5], fields[6]);
        return 0;
    }
    return 1;
}

static int
run_cases(dsd_opts* opts, dsd_state* state, const char* path) {
    // Same LIP vector as CORE_GPS_LRRP_CRC_GATING: 22.5 S, 45 W, hash 218, speed 20.
    static const uint8_t lip_bytes[10] = {0x07, 0x80, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x4A, 0x00, 0xDA};
    uint8_t good[96] = {0};
    unpack(lip_bytes, good, sizeof lip_bytes);
    uint16_t crc = (uint16_t)(ComputeCrcCCITT(good) ^ 0x3333U);
    for (unsigned i = 0; i < 16; ++i) {
        good[80 + i] = (uint8_t)((crc >> (15U - i)) & 1U);
    }

    static const struct {
        int aggressive;
        int relaxed;
        int corrupt;
        uint8_t reserved[3];
        int rows;
        const char* tag;
    } cases[] = {
        {1, 0, 0, {0, 0, 0}, 1, "Strict clean USBD"},
        {1, 0, 1, {0, 0, 0}, 0, "Strict CRC-failed USBD"},
        {0, 1, 1, {0, 0, 1}, 0, "Relaxed RAS CRC-failed USBD"},
        {0, 1, 0, {0, 0, 0}, 1, "Relaxed clean USBD"},
    };

    char clean_gps[sizeof state->dmr_embedded_gps[1]];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        uint8_t payload[96];
        uint8_t info[196];
        DSD_MEMCPY(payload, good, sizeof payload);
        // Flip the Time Elapsed LSB before encoding: FEC stays valid, the CRC fails, no asserted field changes.
        payload[5] ^= (uint8_t)cases[i].corrupt;
        dmr_test_encode_bptc_196x96(payload, cases[i].reserved, info);
        uint8_t deint[196];
        uint8_t decoded[96];
        uint8_t reserved[3];
        BPTCDeInterleaveDMRData(info, deint);
        if (BPTC_196x96_Extract_Data(deint, decoded, reserved) != 0 || memcmp(decoded, payload, sizeof payload) != 0
            || memcmp(reserved, cases[i].reserved, sizeof reserved) != 0) {
            DSD_FPRINTF(stderr, "%s: BPTC round-trip failed\n", cases[i].tag);
            return 2;
        }
        if (!truncate_output(path)) {
            return 100;
        }
        opts->aggressive_framesync = cases[i].aggressive;
        // Mirror -F; the USBD burst path consults aggressive_framesync alone.
        opts->dmr_crc_relaxed_default = cases[i].relaxed;
        state->currentslot = 1;
        state->dmr_color_code = 16;
        state->data_p_head[1] = 1;
        state->data_conf_data[1] = 0;
        state->data_header_format[1] = 1;
        DSD_MEMSET(state->dmr_embedded_gps[1], 0, sizeof state->dmr_embedded_gps[1]);
        dmr_data_burst_handler(opts, state, info, 0x0B, NULL);
        if (state->event_crc_invalid[1] != 0) {
            DSD_FPRINTF(stderr, "\n%s: CRC flag was not restored\n", cases[i].tag);
            return 10 + (int)i * 3;
        }
        if (!expect_rows(path, cases[i].rows, "00000218", "-22.500000", "-45.000000", 20, 0, cases[i].tag)) {
            return 11 + (int)i * 3;
        }
        const char* gps = state->dmr_embedded_gps[1];
        if (i == 0) {
            if (!strstr(gps, "218; LIP:") || !strstr(gps, "22.50000") || !strstr(gps, "S") || !strstr(gps, "45.00000")
                || !strstr(gps, "W") || !strstr(gps, "Err: 20m")) {
                DSD_FPRINTF(stderr, "\n%s: unexpected GPS state: %s\n", cases[i].tag, gps);
                return 12;
            }
            DSD_MEMCPY(clean_gps, gps, sizeof clean_gps);
        } else if (memcmp(gps, clean_gps, sizeof clean_gps) != 0) {
            DSD_FPRINTF(stderr, "\n%s: decoded GPS state changed: %s\n", cases[i].tag, gps);
            return 12 + (int)i * 3;
        }
    }
    return 0;
}

int
main(void) {
    InitAllFecFunction();
    if (!dmr_test_check_reference_burst()) {
        return 1;
    }
    dsd_opts* opts = calloc(1, sizeof *opts);
    dsd_state* state = calloc(1, sizeof *state);
    if (opts == NULL || state == NULL) {
        free(opts);
        free(state);
        return 101;
    }
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dmr_usbd_lip_crc");
    if (fd < 0) {
        free(opts);
        free(state);
        return 102;
    }
    if (dsd_close(fd) != 0) {
        remove(path);
        free(opts);
        free(state);
        return 103;
    }
    DSD_SNPRINTF(opts->lrrp_out_file, sizeof opts->lrrp_out_file, "%s", path);
    opts->lrrp_file_output = 1;
    opts->use_dsp_output = 0;
    opts->payload = 0;
    int rc = run_cases(opts, state, path);
    remove(path);
    free(opts);
    free(state);
    if (rc == 0) {
        DSD_FPRINTF(stdout, "DMR_USBD_LIP_LRRP_CRC: PASS\n");
    }
    return rc;
}
