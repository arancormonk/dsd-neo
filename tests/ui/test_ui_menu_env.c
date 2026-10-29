// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsd-neo/platform/env.h>

#include "menu_env.h"
#include "test_env.h"

/* The terminal's environment edits are session overrides (#782): they read back through the store, win over an
   exported value and never write the process environment, which the decoder thread reads concurrently. */
static void
test_int_env_defaults_and_validation(void) {
    (void)dsd_test_unsetenv("DSD_NEO_TEST_MENU_INT");
    assert(env_get_int("DSD_NEO_TEST_MENU_INT", 17) == 17);

    env_set_int("DSD_NEO_TEST_MENU_INT", -42);
    assert(env_get_int("DSD_NEO_TEST_MENU_INT", 17) == -42);
    assert(getenv("DSD_NEO_TEST_MENU_INT") == NULL);

    /* An exported value is read once no override covers it, and validated like any other. */
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    (void)dsd_test_setenv("DSD_NEO_TEST_MENU_INT", "12junk", 1);
    assert(env_get_int("DSD_NEO_TEST_MENU_INT", 17) == 0);

    /* An edit wins over it and leaves the exported text alone. */
    env_set_int("DSD_NEO_TEST_MENU_INT", 5);
    assert(env_get_int("DSD_NEO_TEST_MENU_INT", 17) == 5);
    const char* exported = getenv("DSD_NEO_TEST_MENU_INT");
    assert(exported != NULL && strcmp(exported, "12junk") == 0);

    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    (void)dsd_test_unsetenv("DSD_NEO_TEST_MENU_INT");
}

static void
test_double_env_defaults_and_validation(void) {
    (void)dsd_test_unsetenv("DSD_NEO_TEST_MENU_DOUBLE");
    assert(fabs(env_get_double("DSD_NEO_TEST_MENU_DOUBLE", 1.25) - 1.25) < 0.000001);

    env_set_double("DSD_NEO_TEST_MENU_DOUBLE", -3.5);
    assert(fabs(env_get_double("DSD_NEO_TEST_MENU_DOUBLE", 1.25) - -3.5) < 0.000001);
    assert(getenv("DSD_NEO_TEST_MENU_DOUBLE") == NULL);

    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    (void)dsd_test_setenv("DSD_NEO_TEST_MENU_DOUBLE", "3.25x", 1);
    assert(fabs(env_get_double("DSD_NEO_TEST_MENU_DOUBLE", 1.25)) < 0.000001);

    (void)dsd_test_unsetenv("DSD_NEO_TEST_MENU_DOUBLE");
}

int
main(void) {
    test_int_env_defaults_and_validation();
    test_double_env_defaults_and_validation();
    printf("UI_MENU_ENV: OK\n");
    return 0;
}
