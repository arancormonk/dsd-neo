// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <dsd-neo/platform/posix_compat.h>

#include "menu_env.h"

static void
test_int_env_defaults_and_validation(void) {
    (void)dsd_unsetenv("DSD_NEO_TEST_MENU_INT");
    assert(env_get_int("DSD_NEO_TEST_MENU_INT", 17) == 17);

    env_set_int("DSD_NEO_TEST_MENU_INT", -42);
    assert(env_get_int("DSD_NEO_TEST_MENU_INT", 17) == -42);

    (void)dsd_setenv("DSD_NEO_TEST_MENU_INT", "12junk", 1);
    assert(env_get_int("DSD_NEO_TEST_MENU_INT", 17) == 0);

    (void)dsd_unsetenv("DSD_NEO_TEST_MENU_INT");
}

static void
test_double_env_defaults_and_validation(void) {
    (void)dsd_unsetenv("DSD_NEO_TEST_MENU_DOUBLE");
    assert(env_get_double("DSD_NEO_TEST_MENU_DOUBLE", 1.25) == 1.25);

    env_set_double("DSD_NEO_TEST_MENU_DOUBLE", -3.5);
    assert(fabs(env_get_double("DSD_NEO_TEST_MENU_DOUBLE", 1.25) - -3.5) < 0.000001);

    (void)dsd_setenv("DSD_NEO_TEST_MENU_DOUBLE", "3.25x", 1);
    assert(env_get_double("DSD_NEO_TEST_MENU_DOUBLE", 1.25) == 0.0);

    (void)dsd_unsetenv("DSD_NEO_TEST_MENU_DOUBLE");
}

int
main(void) {
    test_int_env_defaults_and_validation();
    test_double_env_defaults_and_validation();
    printf("UI_MENU_ENV: OK\n");
    return 0;
}
