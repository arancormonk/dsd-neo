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
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_utils_api.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dmr_bptc_test_encoder.h"
#include "dmr_rs_test_encoder.h"
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

static int
validate_encoder(void) {
    const uint8_t data[9] = {3, 20, 37, 54, 71, 88, 105, 122, 139};
    const uint8_t expected[3] = {208, 63, 250};
    uint8_t parity[3];
    dmr_test_encode_rs_12_9(data, parity);
    DSD_FPRINTF(stderr, "T9 RS encoder validation: parity {%u, %u, %u}, expected {208, 63, 250}: %s\n", parity[0],
                parity[1], parity[2], memcmp(parity, expected, sizeof parity) == 0 ? "PASS" : "FAIL");
    return memcmp(parity, expected, sizeof parity) == 0;
}

static void
make_word(uint8_t word[12], int variant) {
    const uint8_t data[9] = {0, 0, 0, 0, 7, 0xD2, 0, 3, 0xE9};
    DSD_MEMCPY(word, data, sizeof data);
    dmr_test_encode_rs_12_9(data, word + 9);
    for (size_t i = 9; i < 12; ++i) {
        word[i] ^= 0x96;
    }
    if (variant == 1) {
        word[4] ^= 0x55;
    }
    if (variant == 2) {
        word[9] = word[10] = word[11] = 0;
    }
    if (variant == 3) {
        word[9] = 0x05;
        word[10] = 0x83;
        word[11] = 0x3D;
    }
    if (variant == 4) {
        word[3] ^= 0xB4;
        word[4] ^= 0x53;
    }
}

static int
test_wrapper(void) {
    int failed = 0;
    uint8_t clean[12];
    make_word(clean, 0);
    for (int variant = 0; variant < 5; ++variant) {
        uint8_t word[12];
        make_word(word, variant);
        uint32_t crc = 0;
        uint32_t verdict = ComputeAndCorrectFullLinkControlCrc(word, &crc, 0x969696);
        if (verdict != (uint32_t)(variant < 2)) {
            DSD_FPRINTF(stderr, "T9 LC wrapper variant %d: expected verdict %d, got %u\n", variant, variant < 2,
                        verdict);
            failed = 1;
        }
        if (variant < 2) {
            CHECK(memcmp(word, clean, sizeof word) == 0);
        }
    }
    return failed;
}

static int
test_burst(int variant) {
    int failed = 0;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof state);
    opts.aggressive_framesync = 1;
    state.dmr_stereo = 1;
    state.lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
    uint8_t word[12];
    uint8_t bits[96];
    uint8_t info[196];
    const uint8_t reserved[3] = {0};
    make_word(word, variant);
    dsd_unpack_bytes_to_bits(word, sizeof word, bits, sizeof bits, sizeof word);
    dmr_test_encode_bptc_196x96(bits, reserved, info);
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "rs_burst") != 0) {
        return 1;
    }
    dmr_data_burst_handler(&opts, &state, info, 0x01, NULL);
    dsd_test_capture_stderr_end(&cap);
    char output[4096];
    if (dsd_test_capture_stderr_read(&cap, output, sizeof output) != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "T9 burst variant %d: %s\n", variant, output);
    dsd_call_snapshot call = {0};
    int has_call = dsd_call_state_get(&state, 0, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE;
    if (variant < 2) {
        CHECK(has_call);
        CHECK(call.ota_source_id == 1001 && call.ota_target_id == 2002);
        CHECK(call.crc_invalid == 0);
    } else {
        CHECK(!has_call);
        CHECK(strstr(output, "(CRC ERR)") != NULL);
    }
    dsd_state_ext_free_all(&state);
    return failed;
}

int
main(void) {
    InitAllFecFunction();
    if (!validate_encoder() || !dmr_test_check_reference_burst()) {
        return 100;
    }
    int failed = test_wrapper();
    for (int variant = 0; variant < 5; ++variant) {
        failed |= test_burst(variant);
    }
    return failed;
}
