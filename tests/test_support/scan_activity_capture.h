// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A recording of the simplex digital families' trunk scan activity reports (issue #890,
 * dsd_trunk_scan_hook_digital_conventional_activity()), for the protocol tests that check each caller fires on checked
 * content and stays silent on the rest. Header only: one copy per test executable, which includes it once.
 *
 * A test that compiles protocol sources without the runtime defines the hook itself with
 * DSD_TEST_SCAN_ACTIVITY_DEFINE_HOOK(); one linked with the runtime installs the recorder instead
 * (dsd_test_scan_activity_install()).
 */

#ifndef DSD_NEO_TESTS_TEST_SUPPORT_SCAN_ACTIVITY_CAPTURE_H_
#define DSD_NEO_TESTS_TEST_SUPPORT_SCAN_ACTIVITY_CAPTURE_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>

typedef struct {
    int count;
    int protocol;
    int slot;
    dsd_trunk_scan_activity_identity last;
} dsd_test_scan_activity;

static dsd_test_scan_activity g_test_scan_activity;

/** Forget every report recorded. */
static inline void
dsd_test_scan_activity_reset(void) {
    DSD_MEMSET(&g_test_scan_activity, 0, sizeof(g_test_scan_activity));
}

static inline void
dsd_test_scan_activity_record(const dsd_opts* opts, const dsd_state* state, int protocol, int slot,
                              const dsd_trunk_scan_activity_identity* identity) {
    (void)opts;
    (void)state;
    g_test_scan_activity.count++;
    g_test_scan_activity.protocol = protocol;
    g_test_scan_activity.slot = slot;
    if (identity) {
        g_test_scan_activity.last = *identity;
    }
}

/** Install the recorder as the only trunk scan hook (a test linked with the runtime). */
static inline void
dsd_test_scan_activity_install(void) {
    dsd_trunk_scan_hooks hooks;
    DSD_MEMSET(&hooks, 0, sizeof(hooks));
    hooks.digital_conventional_activity = dsd_test_scan_activity_record;
    dsd_trunk_scan_hooks_set(&hooks);
    dsd_test_scan_activity_reset();
}

/** The hook itself, recording, for a test that compiles protocol sources without the runtime's trunk_scan_hooks.c; it
 * drops a NULL identity as dsd_trunk_scan_hook_digital_conventional_activity() does. */
#define DSD_TEST_SCAN_ACTIVITY_DEFINE_HOOK()                                                                           \
    void dsd_trunk_scan_hook_digital_conventional_activity(const dsd_opts* opts, const dsd_state* state, int protocol, \
                                                           int slot,                                                   \
                                                           const dsd_trunk_scan_activity_identity* identity) {         \
        if (identity != NULL) {                                                                                        \
            dsd_test_scan_activity_record(opts, state, protocol, slot, identity);                                      \
        }                                                                                                              \
    }

#endif /* DSD_NEO_TESTS_TEST_SUPPORT_SCAN_ACTIVITY_CAPTURE_H_ */
