// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/secret_redaction.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/cli.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

static int
expect_str(const char* label, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: redaction mismatch\n", label);
        return 1;
    }
    return 0;
}

static int
test_redacted_default(void) {
    char buf[64];
    uint8_t bytes[2] = {0x12U, 0xABU};
    unsigned long long segments[2] = {0x1ULL, 0x2ULL};
    int rc = 0;
    rc |=
        expect_str("redacted decimal", dsd_secret_format_decimal(buf, sizeof buf, 0, 123ULL, 5U), DSD_SECRET_REDACTED);
    rc |= expect_str("redacted hex", dsd_secret_format_hex(buf, sizeof buf, 0, 0xABULL, 4U, 1), DSD_SECRET_REDACTED);
    rc |= expect_str("redacted segments", dsd_secret_format_u64_segments(buf, sizeof buf, 0, segments, 2U),
                     DSD_SECRET_REDACTED);
    rc |= expect_str("redacted bytes", dsd_secret_format_byte_hex(buf, sizeof buf, 0, bytes, sizeof bytes),
                     DSD_SECRET_REDACTED);
    rc |= expect_str("redacted string", dsd_secret_format_string(buf, sizeof buf, 0, "ABCDEF"), DSD_SECRET_REDACTED);
    return rc;
}

static int
test_revealed_decimal_and_hex(void) {
    char buf[64];
    int rc = 0;
    rc |= expect_str("decimal width", dsd_secret_format_decimal(buf, sizeof buf, 1, 42ULL, 5U), "00042");
    rc |= expect_str("decimal no width", dsd_secret_format_decimal(buf, sizeof buf, 1, 42ULL, 0U), "42");
    rc |= expect_str("hex width", dsd_secret_format_hex(buf, sizeof buf, 1, 0x1AULL, 4U, 0), "001A");
    rc |= expect_str("hex prefix", dsd_secret_format_hex(buf, sizeof buf, 1, 0x1AULL, 4U, 1), "0x001A");
    return rc;
}

static int
test_revealed_segments_and_bytes(void) {
    char buf[128];
    unsigned long long segments[2] = {0x1122ULL, 0xAABBULL};
    uint8_t bytes[3] = {0x12U, 0xABU, 0x00U};
    int rc = 0;
    rc |= expect_str("segments", dsd_secret_format_u64_segments(buf, sizeof buf, 1, segments, 2U),
                     "0000000000001122 000000000000AABB");
    rc |= expect_str("bytes", dsd_secret_format_byte_hex(buf, sizeof buf, 1, bytes, sizeof bytes), "12AB00");
    rc |= expect_str("string", dsd_secret_format_string(buf, sizeof buf, 1, "ABCDEF"), "ABCDEF");
    return rc;
}

static int
test_cli_key_errors(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    initOpts(opts);
    initState(state);
    int failed = 0;
    const char* flags[] = {"-b", "-R", "-H", "-1", "-_", "-2"};
    for (size_t i = 0; i < sizeof flags / sizeof flags[0]; ++i) {
        char malformed[40];
        memset(malformed, '7', sizeof malformed - 2);
        malformed[sizeof malformed - 2] = 'z';
        malformed[sizeof malformed - 1] = 0;
        char name[] = "diagnostics-test";
        char option[3];
        memcpy(option, flags[i], sizeof option);
        char* argv[] = {name, option, malformed, NULL};
        dsd_test_capture_stderr capture;
        if (dsd_test_capture_stderr_begin(&capture, "secret_errors") != 0) {
            failed = 1;
            break;
        }
        int effective = 0, exit_rc = 0;
        const int result = dsd_parse_args(3, argv, opts, state, &effective, &exit_rc);
        if (dsd_test_capture_stderr_end(&capture) != 0) {
            failed = 1;
        }
        char output[8192] = {0};
        FILE* file = fopen(capture.path, "rb");
        if (!file) {
            failed = 1;
        } else {
            size_t n = fread(output, 1, sizeof output - 1, file);
            output[n] = 0;
            fclose(file);
        }
        remove(capture.path);
        if (result != DSD_PARSE_ERROR || strstr(output, malformed) != NULL) {
            failed = 1;
        }
        DSD_SECURE_ZERO(malformed, sizeof malformed);
        DSD_SECURE_ZERO(output, sizeof output);
    }
    freeState(state);
    DSD_SECURE_ZERO(state, sizeof(*state));
    free(state);
    free(opts);
    return failed;
}

int
main(void) {
    int rc = 0;
    rc |= test_cli_key_errors();
    rc |= test_redacted_default();
    rc |= test_revealed_decimal_and_hex();
    rc |= test_revealed_segments_and_bytes();
    return rc;
}
