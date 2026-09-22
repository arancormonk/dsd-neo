// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */
#include <dsd-neo/core/bit_packing.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/unicode.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

static int
check(int ok, const char* expression, int line) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL line %d: %s\n", line, expression);
    }
    return !ok;
}

#define CHECK(expression) (failed |= check((expression), #expression, __LINE__))

static void
set_bits(uint8_t* bits, size_t offset, unsigned width, uint32_t value) {
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
    (void)fclose(file);
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
test_ras_call(dsd_opts* opts, dsd_state* state, int stereo) {
    int failed = 0;
    const uint8_t bytes[12] = {0, 0, 0, 0, 7, 0xD2, 0, 3, 0xE9, 0x96, 0x96, 0x96};
    const uint8_t reserved[3] = {0, 0, 1};
    uint8_t bits[96];
    uint8_t info[196];
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof *state);
    state->dmr_stereo = stereo;
    state->currentslot = 1;
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    opts->aggressive_framesync = 0;
    dsd_unpack_bytes_to_bits(bytes, sizeof bytes, bits, sizeof bits, sizeof bytes);
    dmr_test_encode_bptc_196x96(bits, reserved, info);
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "mono_ras") != 0) {
        return 1;
    }
    dmr_data_burst_handler(opts, state, info, 0x01, NULL);
    dsd_test_capture_stderr_end(&cap);
    char output[4096];
    if (dsd_test_capture_stderr_read(&cap, output, sizeof output) != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "T5%s burst: %s\n", stereo ? "b" : "a", output);
    CHECK(strstr(output, "-RAS") != NULL);
    CHECK(state->currentslot == stereo);
    dsd_call_snapshot call = {0};
    CHECK(dsd_call_state_get(state, (uint8_t)stereo, &call) > 0);
    CHECK(call.phase == DSD_CALL_PHASE_ACTIVE);
    CHECK(call.ota_source_id == 1001 && call.ota_target_id == 2002);
    CHECK(call.crc_invalid == 1);
    CHECK(dsd_call_state_get(state, (uint8_t)!stereo, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE);
    CHECK(state->event_crc_invalid[0] == 0 && state->event_crc_invalid[1] == 0);
    return failed;
}

static int
test_gps_scope(dsd_opts* opts, dsd_state* state, const char* path, uint8_t outer, uint8_t burst) {
    int failed = 0;
    dsd_state_ext_free_all(state);
    DSD_MEMSET(state, 0, sizeof *state);
    state->currentslot = 1;
    state->event_crc_invalid[0] = outer;
    state->event_crc_invalid[1] = burst;
    uint8_t bits[96] = {0};
    set_bits(bits, 2, 6, 0x08);
    set_bits(bits, 20, 3, 1);
    set_bits(bits, 23, 1, 1);
    set_bits(bits, 24, 24, 0xC00000);
    set_bits(bits, 48, 1, 1);
    set_bits(bits, 49, 23, 0x600000);
    uint32_t errors = 0;
    CHECK(truncate_output(path));
    dmr_flco(opts, state, bits, 1, &errors, 3);
    CHECK(expect_rows(path, outer || burst ? 0 : 1, "00000000", "-22.500000", "-45.000000", 0, 0,
                      outer ? "T5d outer verdict" : "T5c GPS seam"));
    char expected[256];
    DSD_SNPRINTF(expected, sizeof expected, "GPS: 22.50000%sS 45.00000%sW Err: 20m", dsd_degrees_glyph(),
                 dsd_degrees_glyph());
    CHECK(strcmp(state->dmr_embedded_gps[0], expected) == 0);
    CHECK(state->dmr_embedded_gps[1][0] == '\0');
    CHECK(state->event_crc_invalid[0] == outer && state->event_crc_invalid[1] == burst);
    return failed;
}

int
main(void) {
    InitAllFecFunction();
    if (!dmr_test_check_reference_burst()) {
        return 100;
    }
    static dsd_opts opts;
    static dsd_state state;
    opts.dmr_mono = 1;
    opts.lrrp_file_output = 1;
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "mono_crc_scope");
    if (fd < 0) {
        return 100;
    }
    dsd_close(fd);
    DSD_SNPRINTF(opts.lrrp_out_file, sizeof opts.lrrp_out_file, "%s", path);
    int failed = test_ras_call(&opts, &state, 0);
    failed |= test_ras_call(&opts, &state, 1);
    failed |= test_gps_scope(&opts, &state, path, 0, 1);
    failed |= test_gps_scope(&opts, &state, path, 0, 0);
    failed |= test_gps_scope(&opts, &state, path, 1, 0);
    dsd_state_ext_free_all(&state);
    (void)remove(path);
    return failed;
}
