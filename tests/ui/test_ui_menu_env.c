// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/platform/env.h>
#include <dsd-neo/runtime/config.h>

#include "menu_env.h"
#include "test_env.h"

static int g_actions[4];
static int g_action_count;
static int g_action_saw_new_config;

/* The decoder side of a re-parse (issue #672, B10): what the menu asked the decoder to do, and whether the config it
   re-parsed was already published when it asked. */
int
dsd_app_command_action(int cmd_id) {
    if (g_action_count < (int)(sizeof g_actions / sizeof g_actions[0])) {
        g_actions[g_action_count] = cmd_id;
    }
    g_action_count++;
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    g_action_saw_new_config = cfg && cfg->dmr_t3_heur_is_set && cfg->dmr_t3_heur_enable;
    return DSD_APP_COMMAND_SUBMIT_QUEUED;
}

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

/* Issue #672 (B10): a re-parse publishes the runtime config on the menu's thread and then asks the decoder to apply it
   to the options it owns. It takes no options to write (the decoder's are never touched from the UI thread). */
static void
test_reparse_hands_the_config_to_the_decoder(void) {
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, "DSD_NEO_DMR_T3_HEUR", "1") == 0);
    g_action_count = 0;
    g_action_saw_new_config = 0;
    env_reparse_runtime_cfg();
    assert(g_action_count == 1);
    assert(g_actions[0] == DSD_APP_CMD_RUNTIME_CONFIG_APPLY);
    assert(g_action_saw_new_config == 1);

    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    dsd_neo_config_init();
}

int
main(void) {
    test_int_env_defaults_and_validation();
    test_double_env_defaults_and_validation();
    test_reparse_hands_the_config_to_the_decoder();
    printf("UI_MENU_ENV: OK\n");
    return 0;
}
