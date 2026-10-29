// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The environment override store (#782): layer order, hiding, set-if-unset, interned values, clearing, a full
 * table, and that the process environment is never written.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/env.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_env.h"

static int
streq(const char* a, const char* b) {
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static void
test_layer_order(void) {
    const char* name = "DSD_NEO_TEST_ENV_ORDER";
    assert(dsd_test_unsetenv(name) == 0);
    assert(dsd_env_get(name) == NULL);

    /* An exported variable is the base layer. */
    assert(dsd_test_setenv(name, "process", 1) == 0);
    assert(streq(dsd_env_get(name), "process"));

    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, "host") == 0);
    assert(streq(dsd_env_get(name), "host"));
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "session") == 0);
    assert(streq(dsd_env_get(name), "session"));
    /* The store never writes the process environment. */
    assert(streq(getenv(name), "process"));

    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(streq(dsd_env_get(name), "host"));
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    assert(streq(dsd_env_get(name), "process"));

    /* A session value over no host value. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "session") == 0);
    assert(streq(dsd_env_get(name), "session"));
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(dsd_test_unsetenv(name) == 0);
    assert(dsd_env_get(name) == NULL);
}

static void
test_hide(void) {
    const char* name = "DSD_NEO_TEST_ENV_HIDE";
    assert(dsd_test_setenv(name, "process", 1) == 0);
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, "host") == 0);

    /* A hidden name reads as unset over every lower layer and the process environment. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, NULL) == 0);
    assert(dsd_env_get(name) == NULL);
    assert(streq(getenv(name), "process"));

    /* A host hide over the process environment. */
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(streq(dsd_env_get(name), "host"));
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, NULL) == 0);
    assert(dsd_env_get(name) == NULL);
    /* A session value over a host hide. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "session") == 0);
    assert(streq(dsd_env_get(name), "session"));

    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    assert(streq(dsd_env_get(name), "process"));
    assert(dsd_test_unsetenv(name) == 0);
}

static void
test_set_if_unset(void) {
    const char* name = "DSD_NEO_TEST_ENV_IF_UNSET";
    assert(dsd_test_unsetenv(name) == 0);

    /* An exported variable wins, as setenv(..., 0) leaves it. */
    assert(dsd_test_setenv(name, "process", 1) == 0);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(dsd_env_get(name), "process"));
    assert(dsd_test_unsetenv(name) == 0);
    assert(dsd_env_get(name) == NULL); /* nothing was stored */

    /* Unset everywhere: written. */
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(dsd_env_get(name), "ini"));
    assert(getenv(name) == NULL);

    /* An override of either layer wins. */
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "other") == 0);
    assert(streq(dsd_env_get(name), "ini"));
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, "host") == 0);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(dsd_env_get(name), "host"));

    /* A hidden name reads as unset, so it is written. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, NULL) == 0);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(dsd_env_get(name), "ini"));

    /* An empty value is set, as for setenv(). */
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "") == 0);
    assert(streq(dsd_env_get(name), ""));
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(dsd_env_get(name), ""));

    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, NULL) == -1);
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
}

static void
test_interned_pointers_stay_valid(void) {
    const char* name = "DSD_NEO_TEST_ENV_INTERN";
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "0") == 0);
    const char* zero = dsd_env_get(name);
    assert(streq(zero, "0"));

    /* A reader's pointer outlives later sets and a clear, and a repeated value reuses its first copy. */
    char text[8];
    for (int i = 0; i < 100; i++) {
        DSD_SNPRINTF(text, sizeof text, "%d", i % 2);
        assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, text) == 0);
    }
    const char* one = dsd_env_get(name);
    assert(streq(one, "1"));
    assert(streq(zero, "0"));
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(streq(zero, "0"));
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, "0") == 0);
    assert(dsd_env_get(name) == zero);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "1") == 0);
    assert(dsd_env_get(name) == one);
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
}

static void
test_invalid_arguments(void) {
    assert(dsd_env_get(NULL) == NULL);
    assert(dsd_env_get("") == NULL);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, NULL, "x") == -1);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "", "x") == -1);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST=X", "x") == -1);
    /* DSD_ENV_LAYER_COUNT counts the layers; it is no layer. */
    assert(dsd_env_set(DSD_ENV_LAYER_COUNT, "DSD_NEO_TEST_ENV_BAD", "x") == -1);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_COUNT, "DSD_NEO_TEST_ENV_BAD", "x") == -1);
    assert(dsd_env_get("DSD_NEO_TEST_ENV_BAD") == NULL);
    dsd_env_clear_layer(DSD_ENV_LAYER_COUNT); /* ignored */
}

/* Last: the names it adds hold their slots for the rest of the process. */
static void
test_full_table(void) {
    char name[64];
    int stored = 0;
    int refused = 0;
    for (int i = 0; i < DSD_ENV_MAX_NAMES + 8; i++) {
        DSD_SNPRINTF(name, sizeof name, "DSD_NEO_TEST_ENV_FILL_%d", i);
        if (dsd_env_set(DSD_ENV_LAYER_SESSION, name, "v") == 0) {
            stored++;
            assert(refused == 0); /* once full, it stays full */
        } else {
            refused++;
            assert(dsd_env_get(name) == NULL);
        }
    }
    assert(stored > 0 && stored <= DSD_ENV_MAX_NAMES);
    assert(refused >= 8);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST_ENV_FILL_NEW", "v") == -1);

    /* A name that already has a slot still takes new values and hides. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST_ENV_FILL_0", "w") == 0);
    assert(streq(dsd_env_get("DSD_NEO_TEST_ENV_FILL_0"), "w"));
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, "DSD_NEO_TEST_ENV_ORDER", NULL) == 0);
    assert(dsd_env_get("DSD_NEO_TEST_ENV_ORDER") == NULL);
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    assert(dsd_env_get("DSD_NEO_TEST_ENV_FILL_0") == NULL);
}

int
main(void) {
    test_layer_order();
    test_hide();
    test_set_if_unset();
    test_interned_pointers_stay_valid();
    test_invalid_arguments();
    test_full_table();
    printf("PLATFORM_ENV_STORE: OK\n");
    return 0;
}
