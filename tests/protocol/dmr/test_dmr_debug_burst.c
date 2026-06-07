// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static void
set_no_cach_nibble(dsd_state* state, size_t no_cach_nibble_index, uint8_t value) {
    const size_t dibit_pair_index = 6U + no_cach_nibble_index;
    const size_t payload_index = dibit_pair_index * 2U;
    state->dmr_stereo_payload[payload_index] = (value >> 2U) & 0x03;
    state->dmr_stereo_payload[payload_index + 1U] = value & 0x03;
}

static void
append_expected_byte(char* out, size_t out_size, uint8_t value) {
    const size_t len = strlen(out);
    if (len >= out_size) {
        return;
    }
    DSD_SNPRINTF(out + len, out_size - len, "[%02X]", (unsigned int)value);
}

static void
build_expected(char* expected, size_t expected_size) {
    DSD_SNPRINTF(expected, expected_size, "Debug Demod +Sync slot=2 type=0x0A: ");
    for (uint8_t byte = 0; byte < 33U; byte++) {
        append_expected_byte(expected, expected_size, byte);
    }
}

static int
expect_debug_burst_line(const char* label, const char* actual, const char* expected) {
    if (strcmp(actual, expected) != 0) {
        DSD_FPRINTF(stderr, "unexpected %s debug burst\nexpected: %s\nactual:   %s\n", label, expected, actual);
        return 1;
    }
    return 0;
}

static int
test_state_formatter(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));

    char expected[192];
    build_expected(expected, sizeof(expected));
    for (uint8_t byte = 0; byte < 33U; byte++) {
        set_no_cach_nibble(&state, (size_t)byte * 2U, (uint8_t)(byte >> 4U));
        set_no_cach_nibble(&state, ((size_t)byte * 2U) + 1U, (uint8_t)(byte & 0x0FU));
    }

    char actual[192];
    size_t n = dmr_debug_format_burst(actual, sizeof(actual), &state, 1, 0x0A);
    if (n == 0U) {
        DSD_FPRINTF(stderr, "dmr_debug_format_burst returned zero\n");
        return 1;
    }
    return expect_debug_burst_line("state", actual, expected);
}

static int
test_payload_formatter(void) {
    int payload[144];
    DSD_MEMSET(payload, 0, sizeof(payload));

    char expected[192];
    build_expected(expected, sizeof(expected));
    for (uint8_t byte = 0; byte < 33U; byte++) {
        const size_t hi_index = (6U + ((size_t)byte * 2U)) * 2U;
        const size_t lo_index = (6U + (((size_t)byte * 2U) + 1U)) * 2U;
        payload[hi_index] = (byte >> 6U) & 0x03;
        payload[hi_index + 1U] = (byte >> 4U) & 0x03;
        payload[lo_index] = (byte >> 2U) & 0x03;
        payload[lo_index + 1U] = byte & 0x03;
    }

    char actual[192];
    size_t n = dmr_debug_format_burst_payload(actual, sizeof(actual), payload, 1, 0x0A);
    if (n == 0U) {
        DSD_FPRINTF(stderr, "dmr_debug_format_burst_payload returned zero\n");
        return 1;
    }
    return expect_debug_burst_line("payload", actual, expected);
}

int
main(void) {
    int rc = 0;
    rc |= test_state_formatter();
    rc |= test_payload_formatter();
    return rc;
}
