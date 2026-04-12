// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/state_fwd.h"

/* Test hooks provided by talkgroup_policy.c when DSD_NEO_TEST_HOOKS is defined. */
extern void dsd_tg_policy_test_alloc_reset(void);
extern void dsd_tg_policy_test_alloc_fail_after(long fail_after);

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        fprintf(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static void
init_entry(dsd_tg_policy_entry* e, uint32_t id, const char* mode, const char* name, uint8_t source) {
    memset(e, 0, sizeof(*e));
    e->id_start = id;
    e->id_end = id;
    snprintf(e->mode, sizeof(e->mode), "%s", mode);
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->source = source;
    e->audio = (strcmp(mode, "B") == 0 || strcmp(mode, "DE") == 0) ? 0 : 1;
    e->record = e->audio;
    e->stream = e->audio;
}

int
main(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_tg_policy_entry e;
    if (!st) {
        return 1;
    }

    init_entry(&e, 100, "A", "ALLOC-FAIL", DSD_TG_POLICY_SOURCE_IMPORTED);
    dsd_tg_policy_test_alloc_reset();
    dsd_tg_policy_test_alloc_fail_after(0);
    rc |= expect_true("append alloc fail", dsd_tg_policy_append_legacy_exact(st, &e) == -1);
    rc |= expect_true("append leaves tally unchanged", st->group_tally == 0);

    st->group_tally = 1;
    st->group_array[0].groupNumber = 123;
    snprintf(st->group_array[0].groupMode, sizeof(st->group_array[0].groupMode), "%s", "A");
    snprintf(st->group_array[0].groupName, sizeof(st->group_array[0].groupName), "%s", "LEGACY");

    init_entry(&e, 123, "B", "LOCKOUT", DSD_TG_POLICY_SOURCE_USER_LOCKOUT);
    dsd_tg_policy_test_alloc_reset();
    dsd_tg_policy_test_alloc_fail_after(0);
    rc |= expect_true("replace-first reserve fail",
                      dsd_tg_policy_upsert_legacy_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == -1);
    rc |= expect_true("legacy row unchanged on reserve fail", strcmp(st->group_array[0].groupMode, "A") == 0);

    free(st);
    return rc;
}
