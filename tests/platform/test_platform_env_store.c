// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The environment override store (#782): layer order, hiding, set-if-unset, copies, clearing, a full table that
 * frees its places, and that the process environment is never written.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/platform/platform.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_env.h"

/* The value through dsd_env_read(), in one of a few rotating buffers so two reads can be compared. */
static const char*
get(const char* name) {
    static char bufs[4][256];
    static unsigned next;
    char* buf = bufs[next++ % 4U];
    return dsd_env_read(name, buf, sizeof bufs[0]);
}

static int
streq(const char* a, const char* b) {
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

static void
test_layer_order(void) {
    const char* name = "DSD_NEO_TEST_ENV_ORDER";
    assert(dsd_test_unsetenv(name) == 0);
    assert(get(name) == NULL);

    /* An exported variable is the base layer. */
    assert(dsd_test_setenv(name, "process", 1) == 0);
    assert(streq(get(name), "process"));

    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, "host") == 0);
    assert(streq(get(name), "host"));
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "session") == 0);
    assert(streq(get(name), "session"));
    /* The store never writes the process environment. */
    assert(streq(getenv(name), "process"));

    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(streq(get(name), "host"));
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    assert(streq(get(name), "process"));

    /* A session value over no host value. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "session") == 0);
    assert(streq(get(name), "session"));
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(dsd_test_unsetenv(name) == 0);
    assert(get(name) == NULL);
}

static void
test_hide(void) {
    const char* name = "DSD_NEO_TEST_ENV_HIDE";
    assert(dsd_test_setenv(name, "process", 1) == 0);
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, "host") == 0);

    /* A hidden name reads as unset over every lower layer and the process environment. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, NULL) == 0);
    assert(get(name) == NULL);
    assert(streq(getenv(name), "process"));

    /* A host hide over the process environment. */
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(streq(get(name), "host"));
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, NULL) == 0);
    assert(get(name) == NULL);
    /* A session value over a host hide. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "session") == 0);
    assert(streq(get(name), "session"));

    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    assert(streq(get(name), "process"));
    assert(dsd_test_unsetenv(name) == 0);
}

static void
test_set_if_unset(void) {
    const char* name = "DSD_NEO_TEST_ENV_IF_UNSET";
    assert(dsd_test_unsetenv(name) == 0);

    /* An exported variable wins, as setenv(..., 0) leaves it. */
    assert(dsd_test_setenv(name, "process", 1) == 0);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(get(name), "process"));
    assert(dsd_test_unsetenv(name) == 0);
    assert(get(name) == NULL); /* nothing was stored */

    /* Unset everywhere: written. */
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(get(name), "ini"));
    assert(getenv(name) == NULL);

    /* An override of either layer wins. */
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "other") == 0);
    assert(streq(get(name), "ini"));
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, name, "host") == 0);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(get(name), "host"));

    /* A hidden name reads as unset, so it is written. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, NULL) == 0);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(get(name), "ini"));

    /* An empty value is set, as for setenv(). */
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "") == 0);
    assert(streq(get(name), ""));
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, "ini") == 0);
    assert(streq(get(name), ""));

    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, name, NULL) == -1);
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
}

static void
test_copies(void) {
    const char* name = "DSD_NEO_TEST_ENV_COPY";
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "0123456789") == 0);

    /* A copy belongs to the reader: later sets and a clear leave it as it was. */
    char held[32];
    assert(streq(dsd_env_read(name, held, sizeof held), "0123456789"));
    for (int i = 0; i < 100; i++) {
        char text[8];
        DSD_SNPRINTF(text, sizeof text, "%d", i % 2);
        assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, text) == 0);
    }
    assert(streq(get(name), "1"));
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(streq(held, "0123456789"));

    /* dsd_env_copy() sizes and cuts like snprintf(); dsd_env_read() refuses a value that does not fit. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "0123456789") == 0);
    assert(dsd_env_copy(name, NULL, 0) == 11U);
    char small[4];
    assert(dsd_env_copy(name, small, sizeof small) == 11U);
    assert(streq(small, "012"));
    errno = 0;
    assert(dsd_env_read(name, small, sizeof small) == NULL);
    assert(errno == ERANGE);
    assert(small[0] == '\0');
    /* Unset: 0, and an empty buffer. */
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    char unset[4] = "abc";
    assert(dsd_env_copy(name, unset, sizeof unset) == 0U);
    assert(unset[0] == '\0');
}

static void
test_invalid_arguments(void) {
    assert(get(NULL) == NULL);
    assert(get("") == NULL);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, NULL, "x") == -1);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "", "x") == -1);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST=X", "x") == -1);
    /* DSD_ENV_LAYER_COUNT counts the layers; it is no layer. */
    assert(dsd_env_set(DSD_ENV_LAYER_COUNT, "DSD_NEO_TEST_ENV_BAD", "x") == -1);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_COUNT, "DSD_NEO_TEST_ENV_BAD", "x") == -1);
    assert(get("DSD_NEO_TEST_ENV_BAD") == NULL);
    dsd_env_clear_layer(DSD_ENV_LAYER_COUNT); /* ignored */
}

/* The table counts names that hold an override now: a name whose overrides are all cleared frees its place. */
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
            assert(get(name) == NULL);
        }
    }
    /* Every earlier test cleared its overrides, so the whole table was free. */
    assert(stored == DSD_ENV_MAX_NAMES);
    assert(refused == 8);
    assert(dsd_env_set_if_unset(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST_ENV_FILL_NEW", "v") == -1);

    /* A name that already has a slot still takes new values and hides. */
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST_ENV_FILL_0", "w") == 0);
    assert(streq(get("DSD_NEO_TEST_ENV_FILL_0"), "w"));
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, "DSD_NEO_TEST_ENV_FILL_1", NULL) == 0);
    assert(streq(get("DSD_NEO_TEST_ENV_FILL_1"), "v")); /* the session value is above the host's hide */
    errno = 0;
    assert(dsd_env_set(DSD_ENV_LAYER_HOST, "DSD_NEO_TEST_ENV_FILL_NEW", "v") == -1);
    assert(errno == ENOMEM);

    /* Clearing the session keeps the name that still has a host override and frees the rest. */
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(get("DSD_NEO_TEST_ENV_FILL_0") == NULL);
    assert(get("DSD_NEO_TEST_ENV_FILL_1") == NULL); /* still hidden by the host */
    for (int i = 0; i < DSD_ENV_MAX_NAMES - 1; i++) {
        DSD_SNPRINTF(name, sizeof name, "DSD_NEO_TEST_ENV_REFILL_%d", i);
        assert(dsd_env_set(DSD_ENV_LAYER_SESSION, name, "v") == 0);
    }
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST_ENV_REFILL_LAST", "v") == -1);
    dsd_env_clear_layer(DSD_ENV_LAYER_HOST);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST_ENV_REFILL_LAST", "v") == 0);
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(get("DSD_NEO_TEST_ENV_REFILL_0") == NULL);
}

/* Names are matched as the platform's environment matches them: on Windows, case-insensitively (an override set as
 * dsd_neo_auto_ppm is the DSD_NEO_AUTO_PPM a reader asks for, as an exported variable is there); elsewhere exactly. */
static void
test_name_case(void) {
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "dsd_neo_test_env_case", "lower") == 0);
#if DSD_PLATFORM_WIN_NATIVE
    assert(streq(get("DSD_NEO_TEST_ENV_CASE"), "lower"));
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "DSD_NEO_TEST_ENV_CASE", "upper") == 0);
    assert(streq(get("dsd_neo_test_env_case"), "upper"));
#else
    assert(get("DSD_NEO_TEST_ENV_CASE") == NULL);
    assert(streq(get("dsd_neo_test_env_case"), "lower"));
#endif
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
}

int
main(void) {
    test_layer_order();
    test_hide();
    test_set_if_unset();
    test_copies();
    test_invalid_arguments();
    test_name_case();
    test_full_table();
    printf("PLATFORM_ENV_STORE: OK\n");
    return 0;
}
