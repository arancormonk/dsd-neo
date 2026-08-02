// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Pre-opened USB descriptor slot used by the Android USB-OTG path.
 *
 * The setter is the only thing standing between "the app chose the device" and the
 * engine's normal enumeration, so its clear semantics are worth pinning: fd 0 is a
 * legal descriptor and only a negative value clears the slot.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <stdio.h>

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s failed got=%d want=%d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
test_slot_starts_empty(void) {
    return expect_int("slot starts empty", rtl_device_preopened_fd_is_set(), 0);
}

static int
test_set_and_clear_round_trip(void) {
    int rc = 0;

    rtl_device_set_preopened_fd(7);
    rc |= expect_int("descriptor set", rtl_device_preopened_fd_is_set(), 1);

    rtl_device_set_preopened_fd(-1);
    rc |= expect_int("negative clears", rtl_device_preopened_fd_is_set(), 0);

    return rc;
}

static int
test_zero_is_a_real_descriptor(void) {
    int rc = 0;

    /* Nothing hands out fd 0 in this app, but treating it as "unset" would be a
     * silent fall-back to enumeration, which cannot work with discovery off. */
    rtl_device_set_preopened_fd(0);
    rc |= expect_int("fd 0 counts as set", rtl_device_preopened_fd_is_set(), 1);

    rtl_device_set_preopened_fd(-1);
    rc |= expect_int("cleared again", rtl_device_preopened_fd_is_set(), 0);

    return rc;
}

static int
test_any_negative_value_clears(void) {
    int rc = 0;

    rtl_device_set_preopened_fd(9);
    rc |= expect_int("descriptor set", rtl_device_preopened_fd_is_set(), 1);

    /* Java hands back -1 on failure, but an errno-shaped negative must not read as
     * a descriptor either. */
    rtl_device_set_preopened_fd(-22);
    rc |= expect_int("errno-shaped negative clears", rtl_device_preopened_fd_is_set(), 0);

    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_slot_starts_empty();
    rc |= test_set_and_clear_round_trip();
    rc |= test_zero_is_a_real_descriptor();
    rc |= test_any_negative_value_clears();

    if (rc == 0) {
        DSD_FPRINTF(stderr, "IO_RTL_PREOPENED_FD: OK\n");
    }
    return rc;
}
