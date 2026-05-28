// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <mbelib.h>
#include <stdio.h>
#include <string.h>

#include "mbe_result_context.h"

static int
expect_eq_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_flag(const char* tag, unsigned flags, unsigned flag, int want_set) {
    int got_set = (flags & flag) != 0u;
    if (got_set != want_set) {
        DSD_FPRINTF(stderr, "%s: flags 0x%X flag 0x%X got %d want %d\n", tag, flags, flag, got_set, want_set);
        return 1;
    }
    return 0;
}

static void
set_bits_zero(char bits[49]) {
    for (int i = 0; i < 49; i++) {
        bits[i] = 0;
    }
}

static void
copy_ambe49(char dst[49], const char src[49]) {
    for (int i = 0; i < 49; i++) {
        dst[i] = src[i];
    }
}

static void
copy_imbe88(char dst[88], const char src[88]) {
    for (int i = 0; i < 88; i++) {
        dst[i] = src[i];
    }
}

static void
set_ambe2450_b0(char ambe_d[49], int b0) {
    ambe_d[0] = (char)((b0 >> 6) & 1);
    ambe_d[1] = (char)((b0 >> 5) & 1);
    ambe_d[2] = (char)((b0 >> 4) & 1);
    ambe_d[3] = (char)((b0 >> 3) & 1);
    ambe_d[37] = (char)((b0 >> 2) & 1);
    ambe_d[38] = (char)((b0 >> 1) & 1);
    ambe_d[39] = (char)((b0 >> 0) & 1);
}

static void
init_result(mbe_process_result* result, int total_errors, int c0_errors, unsigned flags) {
    mbe_initProcessResult(result);
    result->total_errors = total_errors;
    result->flags = flags;
    result->c0_errors = c0_errors;
    result->protected_errors = total_errors - c0_errors;
}

static void
init_imbe_result(mbe_process_result* result, int total_errors, int c0_errors, int c4_errors, unsigned flags) {
    mbe_initProcessResult(result);
    result->total_errors = total_errors;
    result->flags = flags;
    result->c0_errors = c0_errors;
    result->c4_errors = c4_errors;
    result->protected_errors = total_errors - c0_errors;
}

static int
result_has_marker(const mbe_process_result* result, char marker) {
    char status[96];
    mbe_formatProcessResult(status, sizeof(status), result);
    return strchr(status, marker) != NULL;
}

static int
test_unchanged_preserves_c0_context(void) {
    int rc = 0;
    char before[49] = {0};
    char after[49] = {0};
    mbe_process_result result;

    set_ambe2450_b0(before, 42);
    copy_ambe49(after, before);
    init_result(&result, 5, 1, MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_SOFT_INPUT | MBE_PROCESS_FLAG_TONE);

    rc |= expect_eq_int("unchanged-return", dsd_mbe_strip_ambe_context_if_changed(before, after, &result), 0);
    rc |= expect_flag("unchanged-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 1);
    rc |= expect_flag("unchanged-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_flag("unchanged-tone", result.flags, MBE_PROCESS_FLAG_TONE, 1);
    rc |= expect_eq_int("unchanged-c0-errors", result.c0_errors, 1);
    rc |= expect_eq_int("unchanged-protected", result.protected_errors, 4);
    rc |= expect_eq_int("unchanged-total", result.total_errors, 5);

    return rc;
}

static int
test_changed_strips_only_c0_context(void) {
    int rc = 0;
    char before[49] = {0};
    char after[49] = {0};
    mbe_process_result result;

    set_ambe2450_b0(before, 7);
    copy_ambe49(after, before);
    after[48] ^= 1;
    init_result(&result, 6, 2, MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_SOFT_INPUT | MBE_PROCESS_FLAG_TONE);

    rc |= expect_eq_int("changed-return", dsd_mbe_strip_ambe_context_if_changed(before, after, &result), 1);
    rc |= expect_flag("changed-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 0);
    rc |= expect_flag("changed-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_flag("changed-tone", result.flags, MBE_PROCESS_FLAG_TONE, 1);
    rc |= expect_eq_int("changed-c0-errors", result.c0_errors, 0);
    rc |= expect_eq_int("changed-protected", result.protected_errors, 6);
    rc |= expect_eq_int("changed-total", result.total_errors, 6);

    return rc;
}

static int
test_changed_without_c0_context_keeps_legacy_shape(void) {
    int rc = 0;
    char before[49] = {0};
    char after[49] = {0};
    mbe_process_result result;

    copy_ambe49(after, before);
    after[0] = 1;
    init_result(&result, 7, 3, MBE_PROCESS_FLAG_SOFT_INPUT);

    rc |= expect_eq_int("legacy-return", dsd_mbe_strip_ambe_context_if_changed(before, after, &result), 1);
    rc |= expect_flag("legacy-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 0);
    rc |= expect_flag("legacy-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_eq_int("legacy-c0-errors", result.c0_errors, 0);
    rc |= expect_eq_int("legacy-protected", result.protected_errors, 7);
    rc |= expect_eq_int("legacy-total", result.total_errors, 7);

    return rc;
}

static int
test_null_result_is_safe(void) {
    char before[49] = {0};
    char after[49] = {0};
    after[1] = 1;
    return expect_eq_int("null-ambe-result", dsd_mbe_strip_ambe_context_if_changed(before, after, NULL), 0)
           | expect_eq_int("null-imbe-result", dsd_mbe_strip_imbe_context_if_changed(NULL, NULL, NULL), 0);
}

static int
test_imbe_unchanged_preserves_c0_c4_context(void) {
    int rc = 0;
    char before[88] = {0};
    char after[88] = {0};
    mbe_process_result result;

    before[3] = 1;
    before[47] = 1;
    before[86] = 1;
    copy_imbe88(after, before);
    init_imbe_result(&result, 9, 2, 3,
                     MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_C4_VALID | MBE_PROCESS_FLAG_SOFT_INPUT
                         | MBE_PROCESS_FLAG_REPEAT);

    rc |= expect_eq_int("imbe-unchanged-return", dsd_mbe_strip_imbe_context_if_changed(before, after, &result), 0);
    rc |= expect_flag("imbe-unchanged-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 1);
    rc |= expect_flag("imbe-unchanged-c4", result.flags, MBE_PROCESS_FLAG_C4_VALID, 1);
    rc |= expect_flag("imbe-unchanged-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_flag("imbe-unchanged-repeat", result.flags, MBE_PROCESS_FLAG_REPEAT, 1);
    rc |= expect_eq_int("imbe-unchanged-c0-errors", result.c0_errors, 2);
    rc |= expect_eq_int("imbe-unchanged-c4-errors", result.c4_errors, 3);
    rc |= expect_eq_int("imbe-unchanged-protected", result.protected_errors, 7);
    rc |= expect_eq_int("imbe-unchanged-total", result.total_errors, 9);

    return rc;
}

static int
test_imbe_changed_strips_c0_c4_context(void) {
    int rc = 0;
    char before[88] = {0};
    char after[88] = {0};
    mbe_process_result result;

    before[5] = 1;
    before[40] = 1;
    copy_imbe88(after, before);
    after[87] ^= 1;
    init_imbe_result(&result, 11, 4, 2,
                     MBE_PROCESS_FLAG_C0_VALID | MBE_PROCESS_FLAG_C4_VALID | MBE_PROCESS_FLAG_SOFT_INPUT
                         | MBE_PROCESS_FLAG_TONE | MBE_PROCESS_FLAG_MUTE);

    rc |= expect_eq_int("imbe-changed-return", dsd_mbe_strip_imbe_context_if_changed(before, after, &result), 1);
    rc |= expect_flag("imbe-changed-c0", result.flags, MBE_PROCESS_FLAG_C0_VALID, 0);
    rc |= expect_flag("imbe-changed-c4", result.flags, MBE_PROCESS_FLAG_C4_VALID, 0);
    rc |= expect_flag("imbe-changed-soft", result.flags, MBE_PROCESS_FLAG_SOFT_INPUT, 1);
    rc |= expect_flag("imbe-changed-tone", result.flags, MBE_PROCESS_FLAG_TONE, 1);
    rc |= expect_flag("imbe-changed-mute", result.flags, MBE_PROCESS_FLAG_MUTE, 1);
    rc |= expect_eq_int("imbe-changed-c0-errors", result.c0_errors, 0);
    rc |= expect_eq_int("imbe-changed-c4-errors", result.c4_errors, 0);
    rc |= expect_eq_int("imbe-changed-protected", result.protected_errors, 11);
    rc |= expect_eq_int("imbe-changed-total", result.total_errors, 11);

    return rc;
}

static int
process_ambe2450(char ambe_d[49], mbe_process_result* result) {
    float out[160] = {0};
    mbe_parms cur = {0};
    mbe_parms prev = {0};
    mbe_parms prev_enhanced = {0};

    mbe_initMbeParms(&cur, &prev, &prev_enhanced);
    return mbe_processAmbe2450Dataf(out, result, ambe_d, &cur, &prev, &prev_enhanced, 8);
}

static int
test_changed_frame_restores_total_error_repeat_fallback(void) {
    int rc = 0;
    char before[49];
    char after[49];
    mbe_process_result c0_result;
    mbe_process_result fallback_result;

    set_bits_zero(before);
    set_ambe2450_b0(before, 0);
    copy_ambe49(after, before);
    after[4] ^= 1;

    init_result(&c0_result, 4, 0, MBE_PROCESS_FLAG_C0_VALID);
    rc |= expect_eq_int("c0-process", process_ambe2450(after, &c0_result) < 0, 0);
    rc |= expect_eq_int("c0-no-repeat", result_has_marker(&c0_result, 'R'), 0);

    init_result(&fallback_result, 4, 0, MBE_PROCESS_FLAG_C0_VALID);
    rc |= expect_eq_int("fallback-strip", dsd_mbe_strip_ambe_context_if_changed(before, after, &fallback_result), 1);
    rc |= expect_eq_int("fallback-process", process_ambe2450(after, &fallback_result) < 0, 0);
    rc |= expect_eq_int("fallback-repeat", result_has_marker(&fallback_result, 'R'), 1);

    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_unchanged_preserves_c0_context();
    rc |= test_changed_strips_only_c0_context();
    rc |= test_changed_without_c0_context_keeps_legacy_shape();
    rc |= test_null_result_is_safe();
    rc |= test_imbe_unchanged_preserves_c0_c4_context();
    rc |= test_imbe_changed_strips_c0_c4_context();
    rc |= test_changed_frame_restores_total_error_repeat_fallback();

    if (rc == 0) {
        printf("CORE_MBE_TRANSFORM_CONTEXT: OK\n");
    }
    return rc;
}
