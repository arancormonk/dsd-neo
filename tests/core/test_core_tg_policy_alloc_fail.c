// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

void dsd_tg_policy_test_alloc_reset(void);
void dsd_tg_policy_test_alloc_fail_after(long fail_after);
void dsd_tg_policy_test_set_generation(dsd_state* state, unsigned int generation, unsigned int structural_generation);
void dsd_tg_policy_test_fail_next_append_close(void);
int dsd_tg_policy_test_keys_consistent(const dsd_state* state);

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static void
init_entry(dsd_tg_policy_entry* e, uint32_t id, const char* mode, const char* name, uint8_t source) {
    DSD_MEMSET(e, 0, sizeof(*e));
    e->id_start = id;
    e->id_end = id;
    DSD_SNPRINTF(e->mode, sizeof(e->mode), "%s", mode);
    DSD_SNPRINTF(e->name, sizeof(e->name), "%s", name);
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
    DSD_MEMSET(r, 0, sizeof(*r));
    r->target_id = target;
    r->source_id = source;
    r->freq_hz = freq_hz;
    r->channel = channel;
    r->slot = slot;
}

static void
init_decision(dsd_tg_policy_decision* d, uint32_t target, uint32_t source, int priority, int preempt) {
    DSD_MEMSET(d, 0, sizeof(*d));
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

static int
test_session_avoid_allocation_failure(void) {
    int rc = 0;
    for (long failure = 0; failure < 2; ++failure) {
        dsd_state* state = calloc(1, sizeof(*state));
        if (!state) {
            return 1;
        }
        dsd_tg_policy_test_alloc_fail_after(failure);
        rc |= expect_true("avoid allocation failure", dsd_tg_policy_session_avoid_add(state, 42) == -1);
        uint64_t context = 1;
        dsd_tg_policy_table_version(state, &context, NULL);
        rc |= expect_true("failed avoid leaves empty context intact", context == 0);
        dsd_tg_policy_test_alloc_reset();
        free_test_state(state);
    }
    dsd_state* state = calloc(1, sizeof(*state));
    dsd_state* snapshot = calloc(1, sizeof(*snapshot));
    if (!state || !snapshot) {
        free_test_state(state);
        free_test_state(snapshot);
        return 1;
    }
    for (uint32_t id = 1; id <= 16; ++id) {
        rc |= expect_true("seed avoids", dsd_tg_policy_session_avoid_add(state, id) == 0);
    }
    rc |= expect_true("seed snapshot", dsd_tg_policy_copy_snapshot(snapshot, state) == 0);
    dsd_tg_policy_test_alloc_fail_after(0);
    rc |= expect_true("growth failure", dsd_tg_policy_session_avoid_add(state, 17) == -1);
    rc |= expect_true("growth failure keeps avoids", dsd_tg_policy_session_avoid_count(state, 0, UINT32_MAX) == 16
                                                         && !dsd_tg_policy_session_avoid_contains(state, 17));
    rc |= expect_true("duplicate does not allocate", dsd_tg_policy_session_avoid_add(state, 1) == 0);
    rc |= expect_true("unchanged snapshot does not allocate", dsd_tg_policy_copy_snapshot(snapshot, state) == 0);
    dsd_tg_policy_test_alloc_reset();
    rc |= expect_true("successful growth", dsd_tg_policy_session_avoid_add(state, 17) == 0);
    for (long failure = 0; failure < 2; ++failure) {
        dsd_tg_policy_test_alloc_fail_after(failure);
        rc |= expect_true("snapshot allocation failure", dsd_tg_policy_copy_snapshot(snapshot, state) == -1);
        rc |= expect_true("failed copy keeps old snapshot", !dsd_tg_policy_session_avoid_contains(snapshot, 17)
                                                                && dsd_tg_policy_session_avoid_contains(snapshot, 16));
    }
    dsd_tg_policy_test_alloc_reset();
    rc |= expect_true("snapshot retry", dsd_tg_policy_copy_snapshot(snapshot, state) == 0
                                            && dsd_tg_policy_session_avoid_contains(snapshot, 17));
    free_test_state(state);
    free_test_state(snapshot);
    return rc;
}

/* A generation wrap is structural: an edit captured before it goes stale, so the tolerated interval stays bounded. */
static int
test_edit_version_wrap(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_tg_policy_entry e;
    uint64_t context = 0;
    unsigned int generation = 0;
    if (!st) {
        return 1;
    }
    init_entry(&e, 100, "A", "Fire", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("seed row", dsd_tg_policy_append_exact(st, &e) == 0);
    dsd_tg_policy_test_set_generation(st, UINT_MAX - 1u, UINT_MAX - 2u);
    dsd_tg_policy_table_version(st, &context, &generation);
    init_entry(&e, 777001, "D", "Radio A", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("alias at the top", dsd_tg_policy_learn_exact(st, &e) == DSD_TG_POLICY_LEARN_ADDED);
    rc |= expect_true("capture valid before wrap", dsd_tg_policy_edit_version_valid(st, context, generation) == 1);
    init_entry(&e, 777002, "D", "Radio B", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("alias wraps", dsd_tg_policy_learn_exact(st, &e) == DSD_TG_POLICY_LEARN_ADDED);
    rc |= expect_true("capture stale after wrap", dsd_tg_policy_edit_version_valid(st, context, generation) == 0);
    dsd_tg_policy_table_version(st, &context, &generation);
    rc |= expect_true("generation skips zero", generation == 1u);
    rc |= expect_true("fresh capture valid", dsd_tg_policy_edit_version_valid(st, context, generation) == 1);
    free_test_state(st);
    return rc;
}

/* A group-file append whose final close fails (the buffered row never reached the disk) reports the failure. */
static int
test_append_reports_close_failure(void) {
    int rc = 0;
    char path[] = "dsd-neo-test-tg-append-close-XXXXXX";
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_tg_policy_entry e;
    int fd = dsd_mkstemp(path);
    if (!opts || fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path);
    init_entry(&e, 100, "D", "Alias", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    dsd_tg_policy_test_fail_next_append_close();
    rc |= expect_true("append reports close failure", dsd_tg_policy_append_group_file_row(opts, &e, "") == -1);
    rc |= expect_true("append succeeds once close works", dsd_tg_policy_append_group_file_row(opts, &e, "") == 0);
    (void)remove(path);
    free(opts);
    return rc;
}

/* The talkgroup lookups scan a compact key per row: every kind of table mutation keeps the keys in step with the rows,
 * and the lookups answer from them as from the rows. */
static int
keys_ok(const char* tag, const dsd_state* st) {
    return expect_true(tag, dsd_tg_policy_test_keys_consistent(st) == 1);
}

static dsd_tg_policy_match_type
group_match_name(const dsd_state* st, uint32_t id, const char* name) {
    dsd_tg_policy_lookup lookup;
    if (dsd_tg_policy_lookup_group(st, id, &lookup) != 0) {
        return DSD_TG_POLICY_MATCH_NONE;
    }
    return (name == NULL || strcmp(lookup.entry.name, name) == 0) ? lookup.match : DSD_TG_POLICY_MATCH_NONE;
}

static int
test_scan_keys_follow_every_mutation(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_state* snap = (dsd_state*)calloc(1, sizeof(*snap));
    static dsd_opts opts;
    dsd_tg_policy_entry e;
    if (!st || !snap) {
        free_test_state(st);
        free_test_state(snap);
        return 1;
    }
    dsd_tg_policy_test_alloc_reset();
    rc |= keys_ok("empty table", st);
    for (uint32_t i = 0; i < 40; i++) { /* past two growths of the block */
        init_entry(&e, 1000U + i, "A", "ROW", DSD_TG_POLICY_SOURCE_IMPORTED);
        rc |= expect_true("append", dsd_tg_policy_append_exact(st, &e) == 0);
        rc |= keys_ok("append keeps keys", st);
    }
    init_entry(&e, 2000, "A", "RANGE", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 2099;
    e.kind = DSD_TG_POLICY_ROW_RANGE;
    rc |= expect_true("range", dsd_tg_policy_add_range_entry(st, &e) == 0);
    init_entry(&e, 0, "A", "PATTERN", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.kind = DSD_TG_POLICY_ROW_PATTERN;
    DSD_SNPRINTF(e.pattern, sizeof(e.pattern), "%s", "3*");
    rc |= expect_true("pattern", dsd_tg_policy_add_pattern_entry(st, &e) == 0);
    rc |= keys_ok("range and pattern", st);
    rc |= expect_true("lookups by key", group_match_name(st, 1005, "ROW") == DSD_TG_POLICY_MATCH_EXACT
                                            && group_match_name(st, 2050, "RANGE") == DSD_TG_POLICY_MATCH_RANGE
                                            && group_match_name(st, 345, "PATTERN") == DSD_TG_POLICY_MATCH_PATTERN);

    /* A growth that fails leaves the keys of the rows already held. */
    dsd_tg_policy_test_alloc_reset();
    for (uint32_t i = 0; i < 64; i++) {
        init_entry(&e, 5000U + i, "A", "GROW", DSD_TG_POLICY_SOURCE_IMPORTED);
        (void)dsd_tg_policy_append_exact(st, &e);
    }
    dsd_tg_policy_test_alloc_fail_after(0);
    for (uint32_t i = 0; i < 128; i++) {
        init_entry(&e, 6000U + i, "A", "FAIL", DSD_TG_POLICY_SOURCE_IMPORTED);
        if (dsd_tg_policy_append_exact(st, &e) != 0) {
            break;
        }
    }
    dsd_tg_policy_test_alloc_reset();
    rc |= keys_ok("failed growth", st);
    rc |= expect_true("failed growth keeps lookups", group_match_name(st, 5063, "GROW") == DSD_TG_POLICY_MATCH_EXACT);

    /* Learned aliases: added, then replaced; an alias never decides a talkgroup. */
    init_entry(&e, 7000, "D", "ALIAS", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("learn add", dsd_tg_policy_learn_exact(st, &e) == DSD_TG_POLICY_LEARN_ADDED);
    rc |= keys_ok("learn add", st);
    DSD_SNPRINTF(e.name, sizeof(e.name), "%s", "RENAMED");
    rc |= expect_true("learn replace", dsd_tg_policy_learn_exact(st, &e) == DSD_TG_POLICY_LEARN_REPLACED);
    rc |= keys_ok("learn replace", st);
    rc |= expect_true("alias decides no talkgroup", group_match_name(st, 7000, NULL) == DSD_TG_POLICY_MATCH_NONE);

    /* Mode edits take the alias row over: its key stops being an alias. */
    rc |= expect_true("set_mode takes the alias over", dsd_tg_policy_set_mode(st, 7000, 7000, "B") == 0);
    rc |= keys_ok("set_mode", st);
    rc |= expect_true("taken-over row decides", group_match_name(st, 7000, NULL) == DSD_TG_POLICY_MATCH_EXACT);
    rc |= expect_true("set_mode_at", dsd_tg_policy_set_mode_at(st, 0, "B") == 0);
    rc |= keys_ok("set_mode_at", st);

    init_entry(&e, 1001, "B", "UPSERT", DSD_TG_POLICY_SOURCE_USER_LOCKOUT);
    rc |= expect_true("upsert replace", dsd_tg_policy_upsert_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    rc |= keys_ok("upsert replace", st);

    dsd_tg_policy_entry values;
    DSD_MEMSET(&values, 0, sizeof(values));
    DSD_SNPRINTF(values.name, sizeof(values.name), "%s", "EDITED");
    rc |= expect_true("set_fields edit",
                      dsd_tg_policy_set_fields(st, 1002, 1002, &values, DSD_TG_POLICY_FIELD_NAME) == 0);
    rc |= expect_true("set_fields append",
                      dsd_tg_policy_set_fields(st, 8000, 8000, &values, DSD_TG_POLICY_FIELD_NAME) == 0);
    rc |= keys_ok("set_fields", st);
    uint64_t context = 0;
    unsigned int generation = 0;
    dsd_tg_policy_table_version(st, &context, &generation);
    rc |= expect_true("set_fields_key",
                      dsd_tg_policy_set_fields_key(st, context, generation,
                                                   &(dsd_tg_policy_row_key){1, DSD_TG_POLICY_ROW_EXACT, 1001, 1001, ""},
                                                   &values, DSD_TG_POLICY_FIELD_NAME)
                          == 0);
    rc |= keys_ok("set_fields_key", st);

    /* A captured selection rebuilds the table. */
    dsd_tg_policy_table_version(st, &context, &generation);
    const dsd_tg_policy_selection selection[] = {{3, 1003, 1003, DSD_TG_POLICY_ROW_EXACT, ""},
                                                 {-1, 9000, 9000, DSD_TG_POLICY_ROW_EXACT, ""}};
    rc |=
        expect_true("selection", dsd_tg_policy_set_listening_selection(st, context, generation, selection, 2, 0) == 0);
    rc |= keys_ok("selection", st);
    rc |= expect_true("selection row decides", group_match_name(st, 9000, NULL) == DSD_TG_POLICY_MATCH_EXACT);
    init_entry(&e, 9100, "A", "AFTER-SELECTION", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append after selection", dsd_tg_policy_append_exact(st, &e) == 0);
    rc |= keys_ok("append after selection", st);

    /* Removals shift the rows after them. */
    rc |= expect_true("remove_bounds", dsd_tg_policy_remove_bounds(st, 1000, 1000) == 0);
    rc |= keys_ok("remove_bounds", st);
    dsd_tg_policy_table_version(st, &context, &generation);
    rc |= expect_true("remove_key",
                      dsd_tg_policy_remove_key(st, context, generation,
                                               &(dsd_tg_policy_row_key){-1, DSD_TG_POLICY_ROW_RANGE, 2000, 2099, ""})
                          == 0);
    rc |= keys_ok("remove_key", st);
    rc |=
        expect_true("removed rows match nothing", group_match_name(st, 1000, NULL) == DSD_TG_POLICY_MATCH_NONE
                                                      && group_match_name(st, 2050, NULL) == DSD_TG_POLICY_MATCH_NONE);
    rc |= expect_true("later rows still match",
                      group_match_name(st, 9100, "AFTER-SELECTION") == DSD_TG_POLICY_MATCH_EXACT);

    /* Snapshot clones carry the keys; a failed reload keeps the live ones; a clear empties them. */
    rc |= expect_true("snapshot", dsd_tg_policy_copy_snapshot(snap, st) == 0);
    rc |= keys_ok("snapshot", snap);
    rc |= expect_true("snapshot lookups", group_match_name(snap, 9100, "AFTER-SELECTION") == DSD_TG_POLICY_MATCH_EXACT
                                              && group_match_name(snap, 345, "PATTERN") == DSD_TG_POLICY_MATCH_PATTERN);
    DSD_SNPRINTF(opts.group_in_file, sizeof(opts.group_in_file), "%s", "missing.csv");
    (void)dsd_tg_policy_reload_group_file(&opts, st);
    rc |= keys_ok("reload", st);
    rc |= expect_true("clear", dsd_tg_policy_clear(st) == 0);
    rc |= keys_ok("clear", st);
    init_entry(&e, 1234, "A", "AFTER-CLEAR", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append after clear", dsd_tg_policy_append_exact(st, &e) == 0);
    rc |= keys_ok("append after clear", st);
    rc |= expect_true("append after clear matches",
                      group_match_name(st, 1234, "AFTER-CLEAR") == DSD_TG_POLICY_MATCH_EXACT);
    dsd_tg_policy_test_alloc_reset();
    free_test_state(st);
    free_test_state(snap);
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
    rc |= test_edit_version_wrap();

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
    rc |= test_session_avoid_allocation_failure();
    rc |= test_append_reports_close_failure();
    rc |= test_scan_keys_follow_every_mutation();

    dsd_tg_policy_test_alloc_reset();
    free_test_state(st);
    return rc;
}
