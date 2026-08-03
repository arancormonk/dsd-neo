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

/*
 * Recording a descriptor is platform-uniform; opening from one is not.
 *
 * Keeping the slot itself uniform is deliberate — it keeps the engine's enumeration
 * bypass reachable from a host test. The open is where builds differ: without
 * rtlsdr_open_fd() nothing consumes the descriptor and the open falls through to
 * opening by index, with enumeration already bypassed. Hosts that can be built
 * either way have to ask before offering a descriptor at all.
 */
static int
test_support_query_describes_the_build(void) {
#ifdef __ANDROID__
    /* The descriptor path is Android-only; a host test cannot pin its other half. */
    return 0;
#else
    int rc = 0;
    rc |= expect_int("descriptor open unsupported off Android", rtl_device_preopened_fd_supported(), 0);

    /* The query describes the build, not the slot: setting one must not make an
     * unusable descriptor look usable. */
    rtl_device_set_preopened_fd(11);
    rc |= expect_int("support unchanged while set", rtl_device_preopened_fd_supported(), 0);
    rc |= expect_int("descriptor still recorded", rtl_device_preopened_fd_is_set(), 1);

    rtl_device_set_preopened_fd(-1);
    return rc;
#endif
}

/*
 * "Recorded" and "in use" are different questions, and the descriptor's owner needs
 * the second one.
 *
 * The engine takes the descriptor part way into a run and gives it back before the
 * run ends, so neither the slot being occupied nor the engine running brackets the
 * period during which closing the file would pull it out from under an in-flight USB
 * transfer. Only rtl_device_create() raises the in-use flag, so the slot alone must
 * never imply it -- an owner that conflated the two would close early.
 */
static int
test_in_use_is_not_implied_by_the_slot(void) {
    int rc = 0;

    rc |= expect_int("nothing in use at rest", rtl_device_preopened_fd_in_use(), 0);

    rtl_device_set_preopened_fd(5);
    rc |= expect_int("descriptor set", rtl_device_preopened_fd_is_set(), 1);
    /* Recording it hands it to nobody: no device has wrapped it yet. */
    rc |= expect_int("recorded is not in use", rtl_device_preopened_fd_in_use(), 0);

    /* Clearing the slot must not claim the descriptor came back either -- it only
     * affects the next open, by design, and a device already holding one keeps it. */
    rtl_device_set_preopened_fd(-1);
    rc |= expect_int("clearing does not report a release", rtl_device_preopened_fd_in_use(), 0);

    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_slot_starts_empty();
    rc |= test_set_and_clear_round_trip();
    rc |= test_zero_is_a_real_descriptor();
    rc |= test_any_negative_value_clears();
    rc |= test_support_query_describes_the_build();
    rc |= test_in_use_is_not_implied_by_the_slot();

    if (rc == 0) {
        DSD_FPRINTF(stderr, "IO_RTL_PREOPENED_FD: OK\n");
    }
    return rc;
}
