// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/posix_compat.h>

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

static void
free_test_state(dsd_state* st) {
    if (st) {
        dsd_state_ext_free_all(st);
    }
    free(st);
}

static void
init_route(dsd_tg_policy_call_route* r, uint32_t target, uint32_t source, long freq_hz, int channel, int slot) {
    memset(r, 0, sizeof(*r));
    r->target_id = target;
    r->source_id = source;
    r->freq_hz = freq_hz;
    r->channel = channel;
    r->slot = slot;
}

static void
init_decision(dsd_tg_policy_decision* d, uint32_t target, uint32_t source, int priority, int preempt) {
    memset(d, 0, sizeof(*d));
    d->target_id = target;
    d->source_id = source;
    d->priority = priority;
    d->preempt_requested = preempt;
    d->tune_allowed = 1;
}

static int
test_snapshot_reuses_unchanged_policy_table(void) {
    int rc = 0;
    dsd_state* src = (dsd_state*)calloc(1, sizeof(*src));
    dsd_state* dst = (dsd_state*)calloc(1, sizeof(*dst));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    dsd_tg_policy_call_route active_route;
    dsd_tg_policy_call_route candidate_route;
    dsd_tg_policy_decision active_decision;
    dsd_tg_policy_decision candidate_decision;
    if (!src || !dst) {
        free_test_state(src);
        free_test_state(dst);
        return 1;
    }

    dsd_tg_policy_test_alloc_reset();
    init_entry(&e, 100, "A", "SNAPSHOT", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("seed source policy", dsd_tg_policy_append_exact(src, &e) == 0);
    rc |= expect_true("initial snapshot clone", dsd_tg_policy_copy_snapshot(dst, src) == 0);

    init_route(&active_route, 100, 1, 851000000L, 10, 0);
    init_decision(&active_decision, 100, 1, 10, 0);
    rc |= expect_true("note source active call",
                      dsd_tg_policy_note_active_call(src, &active_route, &active_decision, 0.0) == 0);

    dsd_tg_policy_test_alloc_reset();
    dsd_tg_policy_test_alloc_fail_after(0);
    rc |= expect_true("unchanged snapshot copy avoids allocation", dsd_tg_policy_copy_snapshot(dst, src) == 0);
    rc |= expect_true("snapshot still has policy", dsd_tg_policy_lookup_id(dst, 100, &lookup) == 0
                                                       && lookup.match == DSD_TG_POLICY_MATCH_EXACT
                                                       && strcmp(lookup.entry.name, "SNAPSHOT") == 0);

    init_route(&candidate_route, 200, 2, 851000000L, 10, 0);
    init_decision(&candidate_decision, 200, 2, 20, 1);
    (void)dsd_setenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS", "500", 1);
    (void)dsd_setenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS", "1000", 1);
    rc |= expect_true("snapshot active state refreshed without policy clone",
                      dsd_tg_policy_should_preempt(NULL, dst, &candidate_route, &candidate_decision, 1.0) == 1);
    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS");
    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS");

    dsd_tg_policy_test_alloc_reset();
    free_test_state(dst);
    free_test_state(src);
    return rc;
}

int
main(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    if (!st) {
        return 1;
    }

    init_entry(&e, 100, "A", "ALLOC-FAIL", DSD_TG_POLICY_SOURCE_IMPORTED);
    dsd_tg_policy_test_alloc_reset();
    dsd_tg_policy_test_alloc_fail_after(0);
    rc |= expect_true("append alloc fail", dsd_tg_policy_append_exact(st, &e) == -1);
    rc |= expect_true("append leaves lookup empty",
                      dsd_tg_policy_lookup_id(st, 100, &lookup) == 0 && lookup.match == DSD_TG_POLICY_MATCH_NONE);

    init_entry(&e, 123, "B", "LOCKOUT", DSD_TG_POLICY_SOURCE_USER_LOCKOUT);
    dsd_tg_policy_test_alloc_reset();
    dsd_tg_policy_test_alloc_fail_after(0);
    rc |= expect_true("replace-first reserve fail",
                      dsd_tg_policy_upsert_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == -1);
    rc |= expect_true("replace-first leaves lookup empty",
                      dsd_tg_policy_lookup_id(st, 123, &lookup) == 0 && lookup.match == DSD_TG_POLICY_MATCH_NONE);

    rc |= test_snapshot_reuses_unchanged_policy_table();

    dsd_tg_policy_test_alloc_reset();
    free_test_state(st);
    return rc;
}
