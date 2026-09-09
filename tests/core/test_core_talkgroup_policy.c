// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/scan_profile.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static void
free_test_state(dsd_state* st) {
    if (st) {
        dsd_state_ext_free_all(st);
    }
    free(st);
}

static void
init_entry(dsd_tg_policy_entry* e, uint32_t id, const char* mode, const char* name, uint8_t source) {
    DSD_MEMSET(e, 0, sizeof(*e));
    e->id_start = id;
    e->id_end = id;
    DSD_SNPRINTF(e->mode, sizeof(e->mode), "%s", mode ? mode : "");
    DSD_SNPRINTF(e->name, sizeof(e->name), "%s", name ? name : "");
    e->source = source;
    e->audio = (strcmp(e->mode, "B") == 0 || strcmp(e->mode, "DE") == 0) ? 0 : 1;
    e->record = e->audio;
    e->stream = e->audio;
}

static unsigned int
policy_generation(const dsd_state* st) {
    unsigned int generation = 0;
    dsd_tg_policy_table_version(st, NULL, &generation);
    return generation;
}

static size_t
policy_count(const dsd_state* st) {
    return dsd_tg_policy_entry_count(st);
}

static int
test_block_reason_labels(void) {
    static const struct {
        uint32_t reason;
        const char* label;
    } cases[] = {
        {DSD_TG_POLICY_BLOCK_NONE, "policy"},
        {DSD_TG_POLICY_BLOCK_GROUP_DISABLED, "group-disabled"},
        {DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED, "private-disabled"},
        {DSD_TG_POLICY_BLOCK_DATA_DISABLED, "data-disabled"},
        {DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED, "enc-disabled"},
        {DSD_TG_POLICY_BLOCK_ALLOWLIST, "allowlist"},
        {DSD_TG_POLICY_BLOCK_MODE, "mode"},
        {DSD_TG_POLICY_BLOCK_HOLD, "hold"},
        {DSD_TG_POLICY_BLOCK_AUDIO, "audio"},
        {DSD_TG_POLICY_BLOCK_RECORD, "record"},
        {DSD_TG_POLICY_BLOCK_STREAM, "stream"},
        {DSD_TG_POLICY_BLOCK_HOLD | DSD_TG_POLICY_BLOCK_ALLOWLIST, "hold"},
    };

    int rc = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        rc |=
            expect_true(cases[i].label, strcmp(dsd_tg_policy_block_reason_label(cases[i].reason), cases[i].label) == 0);
    }
    return rc;
}

static int
test_snapshot_reclones_recreated_context(void) {
    int rc = 0;
    dsd_state* live = (dsd_state*)calloc(1, sizeof(*live));
    dsd_state* snap = (dsd_state*)calloc(1, sizeof(*snap));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    if (!live || !snap) {
        free_test_state(live);
        free_test_state(snap);
        return 1;
    }

    init_entry(&e, 42, "A", "OLD", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("snapshot seed live row", dsd_tg_policy_append_exact(live, &e) == 0);
    rc |= expect_true("snapshot initial copy", dsd_tg_policy_copy_snapshot(snap, live) == 0);
    rc |= expect_true("snapshot initial lookup", dsd_tg_policy_lookup_id(snap, 42, &lookup) == 0);
    rc |= expect_true("snapshot initial label", strcmp(lookup.entry.name, "OLD") == 0);

    dsd_state_ext_free_all(live);
    init_entry(&e, 42, "B", "NEW", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("snapshot recreate live row", dsd_tg_policy_append_exact(live, &e) == 0);
    rc |= expect_true("snapshot recreated generation matches old", policy_generation(live) == 1u);
    rc |= expect_true("snapshot recreated count matches old", policy_count(live) == 1u);
    rc |= expect_true("snapshot recreated copy", dsd_tg_policy_copy_snapshot(snap, live) == 0);
    rc |= expect_true("snapshot recreated lookup", dsd_tg_policy_lookup_id(snap, 42, &lookup) == 0);
    rc |= expect_true("snapshot recloned label", strcmp(lookup.entry.name, "NEW") == 0);
    rc |= expect_true("snapshot recloned mode", strcmp(lookup.entry.mode, "B") == 0);

    free_test_state(live);
    free_test_state(snap);
    return rc;
}

static int
test_snapshot_reclones_recreated_empty_reload_context(void) {
    int rc = 0;
    dsd_state* live = (dsd_state*)calloc(1, sizeof(*live));
    dsd_state* snap = (dsd_state*)calloc(1, sizeof(*snap));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    char path_template[64];
    int fd = -1;
    if (!live || !snap || !opts) {
        free_test_state(live);
        free_test_state(snap);
        free(opts);
        return 1;
    }

    DSD_SNPRINTF(path_template, sizeof(path_template), "%s", "dsd-neo-test-tg-empty-reload-XXXXXX");
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free_test_state(live);
        free_test_state(snap);
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);

    rc |= expect_true("snapshot empty reload seed", dsd_tg_policy_reload_group_file(opts, live) == 0);
    init_entry(&e, 42, "A", "OLD", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("snapshot empty reload append old row", dsd_tg_policy_append_exact(live, &e) == 0);
    rc |= expect_true("snapshot empty reload old generation", policy_generation(live) == 2u);
    rc |= expect_true("snapshot empty reload old count", policy_count(live) == 1u);
    rc |= expect_true("snapshot empty reload initial copy", dsd_tg_policy_copy_snapshot(snap, live) == 0);
    rc |= expect_true("snapshot empty reload initial lookup", dsd_tg_policy_lookup_id(snap, 42, &lookup) == 0);
    rc |= expect_true("snapshot empty reload initial label", strcmp(lookup.entry.name, "OLD") == 0);

    dsd_state_ext_free_all(live);
    rc |= expect_true("snapshot empty reload recreate", dsd_tg_policy_reload_group_file(opts, live) == 0);
    init_entry(&e, 42, "B", "NEW", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("snapshot empty reload append new row", dsd_tg_policy_append_exact(live, &e) == 0);
    rc |= expect_true("snapshot empty reload recreated generation matches old", policy_generation(live) == 2u);
    rc |= expect_true("snapshot empty reload recreated count matches old", policy_count(live) == 1u);
    rc |= expect_true("snapshot empty reload recreated copy", dsd_tg_policy_copy_snapshot(snap, live) == 0);
    rc |= expect_true("snapshot empty reload recreated lookup", dsd_tg_policy_lookup_id(snap, 42, &lookup) == 0);
    rc |= expect_true("snapshot empty reload recloned label", strcmp(lookup.entry.name, "NEW") == 0);
    rc |= expect_true("snapshot empty reload recloned mode", strcmp(lookup.entry.mode, "B") == 0);

    (void)remove(path_template);
    free_test_state(live);
    free_test_state(snap);
    free(opts);
    return rc;
}

static int
test_lookup_and_precedence(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    if (!st) {
        return 1;
    }

    init_entry(&e, 1201, "A", "FIRST", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append exact first", dsd_tg_policy_append_exact(st, &e) == 0);
    init_entry(&e, 1201, "B", "SECOND", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append exact duplicate", dsd_tg_policy_append_exact(st, &e) == 0);

    init_entry(&e, 1200, "B", "RANGE", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 1299;
    e.is_range = 1;
    rc |= expect_true("append range", dsd_tg_policy_add_range_entry(st, &e) == 0);

    rc |= expect_true("lookup exact wins", dsd_tg_policy_lookup_id(st, 1201, &lookup) == 0);
    rc |= expect_true("exact match type", lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    rc |= expect_true("first duplicate wins", strcmp(lookup.entry.mode, "A") == 0);

    rc |= expect_true("lookup range", dsd_tg_policy_lookup_id(st, 1205, &lookup) == 0);
    rc |= expect_true("range match", lookup.match == DSD_TG_POLICY_MATCH_RANGE);
    rc |= expect_true("range mode", strcmp(lookup.entry.mode, "B") == 0);

    init_entry(&e, 1205, "A", "NARROW", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 1208;
    e.is_range = 1;
    rc |= expect_true("append narrow range", dsd_tg_policy_add_range_entry(st, &e) == 0);
    rc |= expect_true("lookup narrow", dsd_tg_policy_lookup_id(st, 1206, &lookup) == 0);
    rc |= expect_true("narrow wins", strcmp(lookup.entry.name, "NARROW") == 0);

    init_entry(&e, 1300, "A", "SPAN-1", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 1309;
    e.is_range = 1;
    rc |= expect_true("append span1", dsd_tg_policy_add_range_entry(st, &e) == 0);
    init_entry(&e, 1300, "B", "SPAN-2", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 1309;
    e.is_range = 1;
    rc |= expect_true("append span2", dsd_tg_policy_add_range_entry(st, &e) == 0);
    rc |= expect_true("lookup equal span", dsd_tg_policy_lookup_id(st, 1305, &lookup) == 0);
    rc |= expect_true("last inserted equal-span wins", strcmp(lookup.entry.name, "SPAN-2") == 0);

    free_test_state(st);
    return rc;
}

static int
test_policy_exact_lookup(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    if (!st) {
        return 1;
    }

    init_entry(&e, 1, "A", "RANGE", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 100;
    e.is_range = 1;
    rc |= expect_true("append range", dsd_tg_policy_add_range_entry(st, &e) == 0);
    init_entry(&e, 42, "DE", "EXACT", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append exact", dsd_tg_policy_append_exact(st, &e) == 0);

    rc |= expect_true("lookup exact", dsd_tg_policy_lookup_id(st, 42, &lookup) == 0);
    rc |= expect_true("exact wins over range", lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    rc |= expect_true("exact source", lookup.entry.source == DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("exact mode", strcmp(lookup.entry.mode, "DE") == 0);
    rc |= expect_true("lookup range other", dsd_tg_policy_lookup_id(st, 55, &lookup) == 0);
    rc |= expect_true("range for non-exact", lookup.match == DSD_TG_POLICY_MATCH_RANGE);

    init_entry(&e, 200, "A", "BAD-RANGE", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.is_range = 0;
    rc |= expect_true("reject exact in range helper", dsd_tg_policy_add_range_entry(st, &e) == 1);

    free_test_state(st);
    return rc;
}

static int
test_upsert_modes(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    if (!st) {
        return 1;
    }

    init_entry(&e, 500, "A", "IMPORTED", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append imported", dsd_tg_policy_append_exact(st, &e) == 0);

    init_entry(&e, 500, "B", "LOCKOUT", DSD_TG_POLICY_SOURCE_USER_LOCKOUT);
    rc |= expect_true("add-if-missing no-op",
                      dsd_tg_policy_upsert_exact(st, &e, DSD_TG_POLICY_UPSERT_ADD_IF_MISSING) == 0);
    rc |= expect_true("lookup imported unchanged", dsd_tg_policy_lookup_id(st, 500, &lookup) == 0);
    rc |= expect_true("imported protected", strcmp(lookup.entry.mode, "A") == 0);

    rc |= expect_true("replace-first updates",
                      dsd_tg_policy_upsert_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    rc |= expect_true("lookup replaced", dsd_tg_policy_lookup_id(st, 500, &lookup) == 0);
    rc |= expect_true("mode replaced", strcmp(lookup.entry.mode, "B") == 0);

    init_entry(&e, 600, "D", "LEARN-OLD", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("append learned", dsd_tg_policy_append_exact(st, &e) == 0);
    init_entry(&e, 600, "D", "LEARN-NEW", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("replace-learned updates learned",
                      dsd_tg_policy_upsert_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY) == 0);
    rc |= expect_true("learned refreshed", dsd_tg_policy_lookup_id(st, 600, &lookup) == 0);
    rc |= expect_true("learned name refreshed", strcmp(lookup.entry.name, "LEARN-NEW") == 0);

    init_entry(&e, 500, "D", "SHOULD-NOT-CHANGE", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("replace-learned protects imported/lockout",
                      dsd_tg_policy_upsert_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY) == 0);
    rc |= expect_true("protected row unchanged", dsd_tg_policy_lookup_id(st, 500, &lookup) == 0);
    rc |= expect_true("protected row still lockout", strcmp(lookup.entry.mode, "B") == 0);

    free_test_state(st);
    return rc;
}

static int
test_evaluator_behaviors(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_tg_policy_entry e;
    dsd_tg_policy_decision decision;
    if (!st || !opts) {
        free_test_state(st);
        free(opts);
        return 1;
    }

    /*
     * Keep all evaluator toggles enabled up front, then flip one switch at a
     * time. That makes each block-reason assertion isolate the intended gate.
     * Hold and private-call cases reuse the same policy table to cover overrides.
     */
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    opts->trunk_use_allow_list = 1;

    init_entry(&e, 700, "A", "ALLOW", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.record = 0;
    e.stream = 1;
    e.priority = 10;
    e.preempt = 1;
    rc |= expect_true("append eval row", dsd_tg_policy_append_exact(st, &e) == 0);

    rc |= expect_true("allowlist miss eval", dsd_tg_policy_evaluate_group_call(opts, st, 701, 0, 0, 0, &decision) == 0);
    rc |= expect_true("allowlist miss blocks tune", decision.tune_allowed == 0);
    rc |= expect_true("allowlist block bit", (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0);

    rc |= expect_true("known eval", dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 0, 0, &decision) == 0);
    rc |= expect_true("known tune allowed", decision.tune_allowed == 1);
    rc |= expect_true("known record blocked", decision.record_allowed == 0);
    rc |= expect_true("record block reason", (decision.block_reasons & DSD_TG_POLICY_BLOCK_RECORD) != 0);
    rc |= expect_true("priority preserved", decision.priority == 10 && decision.preempt_requested == 1);

    opts->trunk_tune_group_calls = 0;
    rc |= expect_true("group toggle block", dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 0, 0, &decision) == 0);
    rc |= expect_true("group disabled bit", (decision.block_reasons & DSD_TG_POLICY_BLOCK_GROUP_DISABLED) != 0);
    opts->trunk_tune_group_calls = 1;

    opts->trunk_tune_data_calls = 0;
    rc |= expect_true("data toggle block", dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 0, 1, &decision) == 0);
    rc |= expect_true("data disabled bit", (decision.block_reasons & DSD_TG_POLICY_BLOCK_DATA_DISABLED) != 0);
    opts->trunk_tune_data_calls = 1;

    opts->trunk_tune_enc_calls = 0;
    rc |= expect_true("enc toggle block", dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 1, 0, &decision) == 0);
    rc |= expect_true("enc disabled bit", (decision.block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) != 0);
    opts->trunk_tune_enc_calls = 1;

    init_entry(&e, 702, "DE", "ENC-LO", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append de row", dsd_tg_policy_append_exact(st, &e) == 0);
    st->tg_hold = 702;
    rc |= expect_true("matching hold keeps mode block",
                      dsd_tg_policy_evaluate_group_call(opts, st, 702, 0, 0, 0, &decision) == 0);
    rc |= expect_true("matching hold tune blocked", decision.tune_allowed == 0);
    rc |= expect_true("matching hold audio blocked", decision.audio_allowed == 0);

    rc |= expect_true("hold mismatch block", dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 0, 0, &decision) == 0);
    rc |= expect_true("hold mismatch reason", (decision.block_reasons & DSD_TG_POLICY_BLOCK_HOLD) != 0);
    rc |= expect_true("hold mismatch mutes media", decision.audio_allowed == 0);

    init_entry(&e, 900, "A", "SRC-ALIAS", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append private src", dsd_tg_policy_append_exact(st, &e) == 0);
    opts->trunk_tune_private_calls = 1;
    opts->trunk_use_allow_list = 1;
    st->tg_hold = 0;
    // Private allowlist behavior can match either endpoint before mode gates run.
    rc |=
        expect_true("private unknown block", dsd_tg_policy_evaluate_private_call(opts, st, 1, 2, 0, 0, &decision) == 0);
    rc |= expect_true("private allowlist blocked",
                      decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0);

    rc |= expect_true("private grant unknown eval",
                      dsd_tg_policy_evaluate_private_grant(opts, st, 1, 2, 0, 0, &decision) == 0);
    rc |= expect_true("private grant unknown allowed",
                      decision.tune_allowed == 1 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) == 0);

    rc |= expect_true("private with known src",
                      dsd_tg_policy_evaluate_private_call(opts, st, 900, 901, 0, 0, &decision) == 0);
    rc |= expect_true("private known src allowed", decision.tune_allowed == 1);

    init_entry(&e, 901, "B", "DST-BLOCK", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append private dst block", dsd_tg_policy_append_exact(st, &e) == 0);
    rc |= expect_true("private mode block",
                      dsd_tg_policy_evaluate_private_call(opts, st, 900, 901, 0, 0, &decision) == 0);
    rc |= expect_true("private mode block reason",
                      decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_MODE) != 0);
    rc |= expect_true("private grant still evaluates explicit blocks",
                      dsd_tg_policy_evaluate_private_grant(opts, st, 900, 901, 0, 0, &decision) == 0);
    rc |= expect_true("private grant explicit block reason",
                      decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_MODE) != 0);

    free_test_state(st);
    free(opts);
    return rc;
}

static int
read_file_lines(const char* path, char* l1, size_t l1sz, char* l2, size_t l2sz, char* l3, size_t l3sz) {
    FILE* fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (l1) {
        l1[0] = '\0';
    }
    if (l2) {
        l2[0] = '\0';
    }
    if (l3) {
        l3[0] = '\0';
    }
    if (l1 && fgets(l1, (int)l1sz, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    if (l2 && fgets(l2, (int)l2sz, fp) == NULL) {
        fclose(fp);
        return -1;
    }
    if (l3 && fgets(l3, (int)l3sz, fp) == NULL) {
        // The third line is optional, and a cast to void does not settle the
        // warn_unused_result that _FORTIFY_SOURCE puts on fgets.
        l3[0] = '\0';
    }
    fclose(fp);
    return 0;
}

static int
test_group_file_append_helper(void) {
    int rc = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_tg_policy_entry e;
    const char* path_seed = "dsd-neo-test-tg-policy-XXXXXX";
    char path_template[64];
    int fd = -1;
    char l1[256];
    char l2[256];
    char l3[256];
    if (!opts) {
        return 1;
    }

    /*
     * Each file shape models a user-maintained CSV encountered during append.
     * Missing and empty files need new headers; existing headers decide whether
     * metadata is appended in the short schema or full policy schema.
     */
    init_entry(&e, 100, "D", "Alias", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    e.priority = 7;
    e.preempt = 1;
    e.audio = 1;
    e.record = 0;
    e.stream = 1;

    DSD_SNPRINTF(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(path_template);

    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append to missing file", dsd_tg_policy_append_group_file_row(opts, &e, "") == 0);
    rc |= expect_true("read missing-created file",
                      read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("missing header", strcmp(l1, "id,mode,name\n") == 0);
    rc |= expect_true("missing row", strstr(l2, "100,D,Alias") == l2);
    (void)remove(path_template);

    DSD_SNPRINTF(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append to empty file", dsd_tg_policy_append_group_file_row(opts, &e, "ALG:01") == 0);
    rc |= expect_true("read empty-appended file",
                      read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("empty header metadata", strcmp(l1, "id,mode,name,metadata\n") == 0);
    rc |= expect_true("empty row metadata", strstr(l2, ",ALG:01") != NULL);
    (void)remove(path_template);

    DSD_SNPRINTF(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = dsd_fopen_private(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(opts);
            return 1;
        }
        DSD_FPRINTF(fp, "id,mode,name,tag\n");
        fclose(fp);
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append to basic header", dsd_tg_policy_append_group_file_row(opts, &e, "VEND") == 0);
    rc |= expect_true("read basic file", read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("basic row contains metadata", strstr(l2, ",VEND") != NULL);
    (void)remove(path_template);

    DSD_SNPRINTF(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = dsd_fopen_private(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(opts);
            return 1;
        }
        DSD_FPRINTF(fp, "id,mode,name,priority,preempt,audio,record,stream,tags\n");
        fclose(fp);
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append to policy header", dsd_tg_policy_append_group_file_row(opts, &e, "hello,\nworld") == 0);
    rc |= expect_true("read policy file", read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("policy row full schema", strstr(l2, "100,D,Alias,7,true,on,off,on,hello  world") == l2);
    (void)remove(path_template);

    // A policy-looking header with odd ordering still writes the full schema.
    DSD_SNPRINTF(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = dsd_fopen_private(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(opts);
            return 1;
        }
        DSD_FPRINTF(fp, "id,mode,name,PRIORITY,wrong_order\n");
        fclose(fp);
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append with policy-prefix header",
                      dsd_tg_policy_append_group_file_row(opts, &e, "TAG-UPPER") == 0);
    rc |= expect_true("read policy-prefix file",
                      read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |=
        expect_true("policy-prefix row uses policy schema", strstr(l2, "100,D,Alias,7,true,on,off,on,TAG-UPPER") == l2);
    (void)remove(path_template);

    DSD_SNPRINTF(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = dsd_fopen_private(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(opts);
            return 1;
        }
        DSD_FPRINTF(fp, "id,mode,name,something_else\n");
        fclose(fp);
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append unknown metadata header", dsd_tg_policy_append_group_file_row(opts, &e, "META") == 0);
    rc |= expect_true("read unknown metadata file",
                      read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3)) == 0);
    rc |= expect_true("unknown header unchanged", strcmp(l1, "id,mode,name,something_else\n") == 0);
    rc |= expect_true("unknown row basic format", strstr(l2, "100,D,Alias,META") == l2);
    (void)remove(path_template);

    free(opts);
    return rc;
}

static void
init_route(dsd_tg_policy_call_route* r, uint32_t target, uint32_t source, long freq_hz, int channel, int slot,
           int needs_retune) {
    DSD_MEMSET(r, 0, sizeof(*r));
    r->target_id = target;
    r->source_id = source;
    r->freq_hz = freq_hz;
    r->channel = channel;
    r->slot = slot;
    r->requires_tuner_retune = needs_retune;
}

static void
init_decision(dsd_tg_policy_decision* d, uint32_t target, uint32_t source, int priority, int preempt,
              int tune_allowed) {
    DSD_MEMSET(d, 0, sizeof(*d));
    d->target_id = target;
    d->source_id = source;
    d->priority = priority;
    d->preempt_requested = preempt;
    d->tune_allowed = tune_allowed;
}

static int
test_preemption_helpers(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_tg_policy_entry e;
    dsd_tg_policy_call_route active0;
    dsd_tg_policy_call_route active1;
    dsd_tg_policy_call_route cand;
    dsd_tg_policy_decision active_dec;
    dsd_tg_policy_decision cand_dec;
    if (!st || !opts) {
        free_test_state(st);
        free(opts);
        return 1;
    }

    /*
     * Preemption depends on active-route state, candidate policy, dwell time,
     * and cooldowns. This test walks those inputs in sequence so a failure points
     * to the exact condition that changed.
     */
    (void)dsd_setenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS", "500", 1);
    (void)dsd_setenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS", "1000", 1);
    opts->trunk_tune_group_calls = 1;
    opts->trunk_tune_private_calls = 1;
    opts->trunk_tune_data_calls = 1;
    opts->trunk_tune_enc_calls = 1;
    opts->trunk_use_allow_list = 1;

    init_route(&active0, 100, 1, 851000000L, 10, 0, 0);
    init_decision(&active_dec, 100, 1, 10, 0, 1);
    rc |= expect_true("note active slot0", dsd_tg_policy_note_active_call(st, &active0, &active_dec, 0.0) == 0);

    init_route(&cand, 200, 2, 851000000L, 10, 0, 0);
    init_decision(&cand_dec, 200, 2, 20, 1, 1);
    rc |= expect_true("higher prio preempts after dwell",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 1.2) == 1);

    cand_dec.preempt_requested = 0;
    rc |= expect_true("preempt flag required", dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 1.2) == 0);
    cand_dec.preempt_requested = 1;
    cand_dec.priority = 10;
    rc |= expect_true("equal prio no preempt", dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 1.2) == 0);
    cand_dec.priority = 20;
    cand_dec.tune_allowed = 0;
    rc |= expect_true("tune blocked no preempt", dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 1.2) == 0);
    cand_dec.tune_allowed = 1;

    rc |= expect_true("same tg/channel/slot refresh no preempt",
                      dsd_tg_policy_should_preempt(opts, st, &active0, &cand_dec, 1.2) == 0);

    rc |= expect_true("accept preempt note candidate", dsd_tg_policy_note_active_call(st, &cand, &cand_dec, 1.2) == 0);
    init_route(&cand, 300, 3, 851000000L, 10, 0, 0);
    init_decision(&cand_dec, 300, 3, 30, 1, 1);
    rc |= expect_true("global cooldown blocks repeat",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 1.6) == 0);
    rc |=
        expect_true("post-cooldown allows repeat", dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 2.4) == 1);

    rc |= expect_true("clear all active", dsd_tg_policy_clear_active_call(st, -1) == 0);
    init_route(&active0, 400, 4, 852000000L, 20, 0, 0);
    init_decision(&active_dec, 400, 4, 15, 0, 1);
    rc |= expect_true("note tdma slot0", dsd_tg_policy_note_active_call(st, &active0, &active_dec, 0.0) == 0);
    init_route(&cand, 401, 5, 852000000L, 20, 1, 0);
    init_decision(&cand_dec, 401, 5, 25, 1, 1);
    rc |= expect_true("opposite idle tdma slot no displacement",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 2.0) == 0);

    init_route(&active1, 402, 6, 852000000L, 20, 1, 0);
    init_decision(&active_dec, 402, 6, 35, 0, 1);
    rc |= expect_true("note tdma slot1", dsd_tg_policy_note_active_call(st, &active1, &active_dec, 0.0) == 0);
    init_route(&cand, 403, 7, 853000000L, 21, 0, 1);
    init_decision(&cand_dec, 403, 7, 30, 1, 1);
    rc |= expect_true("retune compares all displaced calls",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 2.0) == 0);
    cand_dec.priority = 40;
    rc |= expect_true("retune preempts when higher than all displaced",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 3.0) == 1);

    init_route(&cand, 0, 8, 0, 0, 0, 0);
    init_decision(&cand_dec, 0, 8, 50, 1, 1);
    rc |= expect_true("incomplete route rejected", dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 3.0) == 0);

    rc |= expect_true("clear active before hold-preempt cases", dsd_tg_policy_clear_active_call(st, -1) == 0);
    init_route(&active0, 5000, 9, 854000000L, 30, 0, 0);
    init_decision(&active_dec, 5000, 9, 10, 0, 1);
    rc |= expect_true("note active for hold-preempt cases",
                      dsd_tg_policy_note_active_call(st, &active0, &active_dec, 10.0) == 0);

    // Hold overrides can only preempt when they also make the candidate tunable.
    init_entry(&e, 5001, "B", "HOLD-CAND", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.priority = 90;
    e.preempt = 1;
    rc |= expect_true("seed hold candidate row", dsd_tg_policy_append_exact(st, &e) == 0);
    init_route(&cand, 5001, 10, 854000000L, 30, 0, 0);

    st->tg_hold = 5999;
    rc |= expect_true("hold mismatch blocks tune",
                      dsd_tg_policy_evaluate_group_call(opts, st, 5001, 10, 0, 0, &cand_dec) == 0
                          && cand_dec.tune_allowed == 0 && (cand_dec.block_reasons & DSD_TG_POLICY_BLOCK_HOLD) != 0);
    rc |= expect_true("hold mismatch blocks preempt",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 11.5) == 0);

    st->tg_hold = 5001;
    rc |= expect_true("matching hold keeps mode block",
                      dsd_tg_policy_evaluate_group_call(opts, st, 5001, 10, 0, 0, &cand_dec) == 0
                          && cand_dec.tune_allowed == 0 && (cand_dec.block_reasons & DSD_TG_POLICY_BLOCK_MODE) != 0);
    rc |= expect_true("matching hold blocks preempt",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 11.5) == 0);
    st->tg_hold = 0;

    init_entry(&e, 5002, "A", "ENC-CAND", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.priority = 95;
    e.preempt = 1;
    rc |= expect_true("seed encrypted candidate row", dsd_tg_policy_append_exact(st, &e) == 0);
    init_route(&cand, 5002, 11, 854000000L, 30, 0, 0);
    opts->trunk_tune_enc_calls = 0;
    rc |= expect_true("encrypted candidate tune blocked",
                      dsd_tg_policy_evaluate_group_call(opts, st, 5002, 11, 1, 0, &cand_dec) == 0
                          && cand_dec.tune_allowed == 0
                          && (cand_dec.block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) != 0);
    rc |= expect_true("encrypted candidate blocked preempt",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 12.0) == 0);
    opts->trunk_tune_enc_calls = 1;

    rc |= expect_true("clear slot0", dsd_tg_policy_clear_active_call(st, 0) == 0);
    rc |= expect_true("clear slot1", dsd_tg_policy_clear_active_call(st, 1) == 0);
    rc |= expect_true("reject invalid slot clear", dsd_tg_policy_clear_active_call(st, 2) == 1);

    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS");
    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS");
    free_test_state(st);
    free(opts);
    return rc;
}

static int
test_reload_group_file(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    unsigned int generation_before = 0;
    unsigned int generation_after = 0;
    char path_template[64];
    int fd = -1;
    if (!st || !opts) {
        free_test_state(st);
        free(opts);
        return 1;
    }

    init_entry(&e, 999, "A", "OLD", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("seed old row", dsd_tg_policy_append_exact(st, &e) == 0);
    rc |= expect_true("seed generation records mutation", policy_generation(st) == 1u);

    DSD_SNPRINTF(path_template, sizeof(path_template), "%s", "dsd-neo-test-tg-reload-XXXXXX");
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free_test_state(st);
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = dsd_fopen_private(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free_test_state(st);
            free(opts);
            return 1;
        }
        DSD_FPRINTF(fp, "id,mode,name,priority,preempt,audio,record,stream,tags\n");
        DSD_FPRINTF(fp, "100,A,ONE,5,true,on,on,on,\n");
        DSD_FPRINTF(fp, "200,B,TWO,1,false,off,off,off,\n");
        fclose(fp);
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    generation_before = policy_generation(st);
    rc |= expect_true("reload success", dsd_tg_policy_reload_group_file(opts, st) == 0);
    rc |= expect_true("reload replaced policy rows", policy_count(st) == 2u);
    generation_after = policy_generation(st);
    rc |= expect_true("reload success increments generation", generation_after == generation_before + 1u);
    rc |= expect_true("lookup row 100", dsd_tg_policy_lookup_id(st, 100, &lookup) == 0);
    rc |= expect_true("lookup row 100 exact", lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    rc |= expect_true("lookup row 100 fields", strcmp(lookup.entry.name, "ONE") == 0 && lookup.entry.priority == 5);
    rc |= expect_true("lookup row 200", dsd_tg_policy_lookup_id(st, 200, &lookup) == 0);
    rc |= expect_true("lookup row 200 blocked mode", strcmp(lookup.entry.mode, "B") == 0);

    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", "/tmp/dsd-neo-missing-group-file-nope.csv");
    generation_before = policy_generation(st);
    rc |= expect_true("reload missing file fails", dsd_tg_policy_reload_group_file(opts, st) != 0);
    rc |= expect_true("failed reload keeps current rows", policy_count(st) == 2u);
    rc |= expect_true("failed reload keeps policy", dsd_tg_policy_lookup_id(st, 100, &lookup) == 0);
    rc |= expect_true("failed reload preserved entry", strcmp(lookup.entry.name, "ONE") == 0);
    rc |= expect_true("failed reload keeps generation", policy_generation(st) == generation_before);

    {
        FILE* fp = dsd_fopen_private(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free_test_state(st);
            free(opts);
            return 1;
        }
        DSD_FPRINTF(fp, "id,mode,name\n");
        DSD_FPRINTF(fp, "300,A,THREE\n");
        fclose(fp);
    }
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    generation_before = policy_generation(st);
    rc |= expect_true("second reload success", dsd_tg_policy_reload_group_file(opts, st) == 0);
    rc |= expect_true("second reload replaced policy rows", policy_count(st) == 1u);
    rc |= expect_true("second reload increments generation", policy_generation(st) == generation_before + 1u);
    rc |= expect_true("second reload row applied", dsd_tg_policy_lookup_id(st, 300, &lookup) == 0);
    rc |= expect_true("second reload row exact", lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    rc |= expect_true("second reload row name", strcmp(lookup.entry.name, "THREE") == 0);

    (void)remove(path_template);
    free_test_state(st);
    free(opts);
    return rc;
}

static int
expect_zeroed_entry(const char* tag, const dsd_tg_policy_entry* entry) {
    int rc = expect_true(tag, entry->id_start == 0);
    rc |= expect_true(tag, entry->id_end == 0);
    rc |= expect_true(tag, entry->mode[0] == '\0');
    rc |= expect_true(tag, entry->name[0] == '\0');
    rc |= expect_true(tag, entry->tags[0] == '\0');
    rc |= expect_true(tag, entry->priority == 0);
    rc |= expect_true(tag, entry->preempt == 0);
    rc |= expect_true(tag, entry->audio == 0);
    rc |= expect_true(tag, entry->record == 0);
    rc |= expect_true(tag, entry->stream == 0);
    rc |= expect_true(tag, entry->is_range == 0);
    rc |= expect_true(tag, entry->source == 0);
    rc |= expect_true(tag, entry->row == 0);
    return rc;
}

static int
test_mode_edits_and_enumeration(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_state* snap = (dsd_state*)calloc(1, sizeof(*snap));
    dsd_tg_policy_entry e;
    dsd_tg_policy_entry row;
    uint64_t context = 1;
    uint64_t snapshot_context = 0;
    unsigned int generation = 1;
    unsigned int snapshot_generation = 0;
    if (!st || !snap) {
        free_test_state(st);
        free_test_state(snap);
        return 1;
    }

    dsd_tg_policy_table_version(st, &context, &generation);
    rc |= expect_true("absent table identity", context == 0 && generation == 0);
    rc |= expect_true("absent table count", dsd_tg_policy_entry_count(st) == 0);
    DSD_MEMSET(&row, 0xff, sizeof(row));
    rc |= expect_true("absent row", dsd_tg_policy_entry_at(st, 0, &row) == 0);
    rc |= expect_zeroed_entry("absent row zeroed", &row);
    rc |= expect_true("null row destination", dsd_tg_policy_entry_at(st, 0, NULL) == 0);

    init_entry(&e, 1001, "A", "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED);
    DSD_SNPRINTF(e.tags, sizeof(e.tags), "%s", "FIRE");
    e.priority = 23;
    e.preempt = 1;
    rc |= expect_true("seed editable row", dsd_tg_policy_append_exact(st, &e) == 0);
    rc |= expect_true("seed duplicate row", dsd_tg_policy_append_exact(st, &e) == 0);
    dsd_tg_policy_table_version(st, &context, &generation);
    rc |= expect_true("copy editable snapshot", dsd_tg_policy_copy_snapshot(snap, st) == 0);
    dsd_tg_policy_table_version(snap, &snapshot_context, &snapshot_generation);
    rc |= expect_true("snapshot shares source version",
                      context != 0 && snapshot_context == context && snapshot_generation == generation);
    rc |= expect_true("block existing row", dsd_tg_policy_set_mode(st, 1001, 1001, "B") == 0);
    rc |= expect_true("mode edit advances generation", policy_generation(st) > generation);
    rc |= expect_true("read blocked row", dsd_tg_policy_entry_at(st, 0, &row) == 1);
    rc |= expect_true("mode edit preserves metadata", strcmp(row.name, "Dispatch") == 0 && strcmp(row.tags, "FIRE") == 0
                                                          && row.priority == 23 && row.preempt == 1
                                                          && row.source == DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("blocking disables media",
                      strcmp(row.mode, "B") == 0 && row.audio == 0 && row.record == 0 && row.stream == 0);
    rc |= expect_true("read duplicate row", dsd_tg_policy_entry_at(st, 1, &row) == 1);
    rc |= expect_true("only first bounds match changes", strcmp(row.mode, "A") == 0);
    rc |= expect_true("read original snapshot", dsd_tg_policy_entry_at(snap, 0, &row) == 1);
    rc |= expect_true("snapshot keeps old row", strcmp(row.mode, "A") == 0 && strcmp(row.tags, "FIRE") == 0);
    rc |= expect_true("refresh edited snapshot", dsd_tg_policy_copy_snapshot(snap, st) == 0);
    rc |= expect_true("read refreshed snapshot", dsd_tg_policy_entry_at(snap, 0, &row) == 1);
    rc |= expect_true("snapshot receives edit", strcmp(row.mode, "B") == 0 && strcmp(row.tags, "FIRE") == 0);
    rc |= expect_true("listen to existing row", dsd_tg_policy_set_mode(st, 1001, 1001, "A") == 0);
    rc |= expect_true("read listening row", dsd_tg_policy_entry_at(st, 0, &row) == 1);
    rc |= expect_true("listening restores media",
                      strcmp(row.mode, "A") == 0 && row.audio == 1 && row.record == 1 && row.stream == 1);
    rc |= expect_true("listen preserves metadata", strcmp(row.name, "Dispatch") == 0 && strcmp(row.tags, "FIRE") == 0
                                                       && row.priority == 23 && row.preempt == 1
                                                       && row.source == DSD_TG_POLICY_SOURCE_IMPORTED);

    init_entry(&e, 1300, "A", "Regional", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 1399;
    e.is_range = 1;
    rc |= expect_true("append editable range", dsd_tg_policy_add_range_entry(st, &e) == 0);
    rc |= expect_true("edit range bounds", dsd_tg_policy_set_mode(st, 1300, 1399, "B") == 0);
    rc |= expect_true("append missing exact", dsd_tg_policy_set_mode(st, 900, 900, "B") == 0);
    rc |= expect_true("enumerated append count", dsd_tg_policy_entry_count(st) == 4);
    rc |= expect_true("enumerate range in append order", dsd_tg_policy_entry_at(st, 2, &row) == 1);
    rc |= expect_true("range edit retained bounds",
                      row.id_start == 1300 && row.id_end == 1399 && row.is_range && strcmp(row.mode, "B") == 0);
    rc |= expect_true("enumerate runtime row last", dsd_tg_policy_entry_at(st, 3, &row) == 1);
    rc |= expect_true("runtime row identity", row.id_start == 900 && row.id_end == 900 && row.name[0] == '\0'
                                                  && row.source == DSD_TG_POLICY_SOURCE_USER_LOCKOUT);
    generation = policy_generation(st);
    rc |= expect_true("index edit", dsd_tg_policy_set_mode_at(st, 2, "A") == 0);
    rc |= expect_true("index edit advances version", policy_generation(st) > generation);
    rc |= expect_true("read index edit", dsd_tg_policy_entry_at(st, 2, &row) == 1);
    rc |= expect_true("index edit restores range media", row.audio && row.record && row.stream);
    generation = policy_generation(st);
    rc |= expect_true("missing range refused", dsd_tg_policy_set_mode(st, 1400, 1499, "B") == 1);
    rc |= expect_true("reversed bounds refused", dsd_tg_policy_set_mode(st, 2, 1, "B") == 1);
    rc |= expect_true("null state refused", dsd_tg_policy_set_mode(NULL, 1, 1, "B") == 1);
    rc |= expect_true("null mode refused", dsd_tg_policy_set_mode(st, 1, 1, NULL) == 1);
    rc |= expect_true("empty mode refused", dsd_tg_policy_set_mode(st, 1, 1, "") == 1);
    rc |= expect_true("eight byte mode refused", dsd_tg_policy_set_mode(st, 1, 1, "12345678") == 1);
    rc |= expect_true("bad index refused", dsd_tg_policy_set_mode_at(st, 4, "A") == 1);
    rc |= expect_true("index empty mode refused", dsd_tg_policy_set_mode_at(st, 0, "") == 1);
    rc |= expect_true("index long mode refused", dsd_tg_policy_set_mode_at(st, 0, "12345678") == 1);
    rc |= expect_true("invalid edits leave table unchanged",
                      policy_generation(st) == generation && dsd_tg_policy_entry_count(st) == 4);
    rc |= expect_true("out of range enumeration", dsd_tg_policy_entry_at(st, 4, &row) == 0);
    rc |= expect_zeroed_entry("out of range row zeroed", &row);
    dsd_state_ext_free_all(st);
    rc |= expect_true("setter creates absent context", dsd_tg_policy_set_mode(st, 7, 7, "A") == 0);
    dsd_tg_policy_table_version(st, &snapshot_context, NULL);
    rc |= expect_true("recreated context differs", snapshot_context != 0 && snapshot_context != context);
    free_test_state(st);
    free_test_state(snap);
    return rc;
}

static int
test_group_file_rewrite(void) {
    int rc = 0;
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_state* loaded = (dsd_state*)calloc(1, sizeof(*loaded));
    dsd_tg_policy_entry e;
    dsd_tg_policy_entry row;
    char path[] = "dsd-neo-test-tg-rewrite-XXXXXX";
    char l1[256] = {0};
    char l2[256] = {0};
    char l3[256] = {0};
    if (!opts || !st || !loaded) {
        free(opts);
        free_test_state(st);
        free_test_state(loaded);
        return 1;
    }
    int fd = dsd_mkstemp(path);
    if (fd < 0) {
        free(opts);
        free_test_state(st);
        free_test_state(loaded);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(path);
    rc |= expect_true("empty path rewrite ignored", dsd_tg_policy_write_group_file(opts, st) == 0);
    rc |= expect_true("null options rewrite ignored", dsd_tg_policy_write_group_file(NULL, st) == 0);
    FILE* fp = fopen(path, "r");
    rc |= expect_true("empty path creates no file", fp == NULL);
    if (fp) {
        fclose(fp);
    }
    init_entry(&e, 1001, "B", "Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED);
    DSD_SNPRINTF(e.tags, sizeof(e.tags), "%s", "FIRE");
    rc |= expect_true("seed rewrite exact", dsd_tg_policy_append_exact(st, &e) == 0);
    init_entry(&e, 1300, "A", "Regional", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 1399;
    e.is_range = 1;
    rc |= expect_true("seed rewrite range", dsd_tg_policy_add_range_entry(st, &e) == 0);
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path);
    rc |= expect_true("rewrite tagged table", dsd_tg_policy_write_group_file(opts, st) == 0);
    rc |=
        expect_true("read rewritten table", read_file_lines(path, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3)) == 0);
    rc |= expect_true("tagged header", strcmp(l1, "id,mode,name,tags\n") == 0);
    rc |= expect_true("tagged exact row", strcmp(l2, "1001,B,Dispatch,FIRE\n") == 0);
    rc |= expect_true("range row with empty category", strcmp(l3, "1300-1399,A,Regional,\n") == 0);
    rc |= expect_true("reload tagged table", csvGroupImportPath(path, loaded) == 0);
    rc |= expect_true("roundtrip row count", dsd_tg_policy_entry_count(loaded) == 2);
    rc |= expect_true("roundtrip exact row", dsd_tg_policy_entry_at(loaded, 0, &row) == 1);
    rc |= expect_true("roundtrip exact fields", row.id_start == 1001 && row.id_end == 1001 && strcmp(row.mode, "B") == 0
                                                    && strcmp(row.name, "Dispatch") == 0
                                                    && strcmp(row.tags, "FIRE") == 0);
    rc |= expect_true("roundtrip range row", dsd_tg_policy_entry_at(loaded, 1, &row) == 1);
    rc |= expect_true("roundtrip range fields", row.id_start == 1300 && row.id_end == 1399 && row.is_range
                                                    && strcmp(row.mode, "A") == 0 && strcmp(row.name, "Regional") == 0
                                                    && row.tags[0] == '\0');

    fp = dsd_fopen_private(path, "w");
    if (!fp) {
        rc = 1;
        goto cleanup;
    }
    DSD_FPRINTF(fp, "id,mode,name\n");
    fclose(fp);
    rc |= expect_true("clear rewrite fixture", dsd_tg_policy_clear(st) == 0);
    init_entry(&e, 2001, "A", "EMS Dispatch", DSD_TG_POLICY_SOURCE_IMPORTED);
    DSD_SNPRINTF(e.tags, sizeof(e.tags), "%s", "EMS");
    e.priority = 17;
    e.preempt = 1;
    e.record = 0;
    rc |= expect_true("seed extended rewrite", dsd_tg_policy_append_exact(st, &e) == 0);
    rc |= expect_true("rewrite extended table", dsd_tg_policy_write_group_file(opts, st) == 0);
    rc |= expect_true("read extended table", read_file_lines(path, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("basic header promoted",
                      strcmp(l1, "id,mode,name,priority,preempt,audio,record,stream,tags\n") == 0);
    rc |= expect_true("extended row media", strcmp(l2, "2001,A,EMS Dispatch,17,true,on,off,on,EMS\n") == 0);
    dsd_state_ext_free_all(loaded);
    rc |= expect_true("reload extended table", csvGroupImportPath(path, loaded) == 0);
    rc |= expect_true("extended roundtrip count", dsd_tg_policy_entry_count(loaded) == 1);
    rc |= expect_true("extended roundtrip row", dsd_tg_policy_entry_at(loaded, 0, &row) == 1);
    rc |= expect_true("extended roundtrip policy", row.priority == 17 && row.preempt && row.audio && !row.record
                                                       && row.stream && strcmp(row.tags, "EMS") == 0);
    /* Each trigger alone promotes, even when an earlier row already has tags. */
    for (int trigger = 0; trigger < 2; ++trigger) {
        fp = dsd_fopen_private(path, "w");
        if (!fp) {
            rc = 1;
            goto cleanup;
        }
        DSD_FPRINTF(fp, "id,mode,name,tags\n");
        fclose(fp);
        dsd_tg_policy_clear(st);
        init_entry(&e, 1, "A", "Tagged", DSD_TG_POLICY_SOURCE_IMPORTED);
        strcpy(e.tags, "FIRE");
        dsd_tg_policy_append_exact(st, &e);
        init_entry(&e, 2, "A", "Priority", DSD_TG_POLICY_SOURCE_IMPORTED);
        e.priority = trigger ? 0 : 25;
        e.preempt = trigger ? 1 : 0;
        dsd_tg_policy_append_exact(st, &e);
        rc |= expect_true("single trigger rewrite", dsd_tg_policy_write_group_file(opts, st) == 0);
        rc |= expect_true("single trigger reload", dsd_tg_policy_reload_group_file(opts, loaded) == 0);
        dsd_tg_policy_entry_at(loaded, 1, &row);
        rc |= expect_true("single trigger survives promotion", row.priority == e.priority && row.preempt == e.preempt);
        e.priority = 0;
        e.preempt = 0;
        dsd_tg_policy_set_fields(st, 2, 2, &e, DSD_TG_POLICY_FIELD_PRIORITY | DSD_TG_POLICY_FIELD_PREEMPT);
        dsd_tg_policy_write_group_file(opts, st);
        read_file_lines(path, l1, sizeof(l1), NULL, 0, NULL, 0);
        rc |= expect_true("extended header stays extended",
                          strcmp(l1, "id,mode,name,priority,preempt,audio,record,stream,tags\n") == 0);
    }
    (void)remove(path);
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s/groups.csv", path);
    rc |= expect_true("missing directory rewrite fails", dsd_tg_policy_write_group_file(opts, st) == -1);
    DSD_SNPRINTF(opts->group_in_file, sizeof(opts->group_in_file), "%s", path);
    rc |= expect_true("clear untagged fixture", dsd_tg_policy_clear(st) == 0);
    rc |= expect_true("seed untagged runtime row", dsd_tg_policy_set_mode(st, 99, 99, "B") == 0);
    rc |= expect_true("rewrite untagged table", dsd_tg_policy_write_group_file(opts, st) == 0);
    rc |= expect_true("read untagged table", read_file_lines(path, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("untagged header", strcmp(l1, "id,mode,name\n") == 0);
    rc |= expect_true("untagged runtime row", strcmp(l2, "99,B,\n") == 0);
cleanup:
    (void)remove(path);
    free(opts);
    free_test_state(st);
    free_test_state(loaded);
    return rc;
}

static int
test_set_fields_remove_bounds(void) {
    int rc = 0;
    dsd_state* st = calloc(1, sizeof(*st));
    dsd_tg_policy_entry value = {0}, row;
    if (!st) {
        return 1;
    }
    strcpy(value.mode, "B");
    strcpy(value.name, "Dispatch");
    value.priority = 25;
    value.preempt = 1;
    rc |= expect_true("add fields", dsd_tg_policy_set_fields(st, 42, 42, &value,
                                                             DSD_TG_POLICY_FIELD_NAME | DSD_TG_POLICY_FIELD_PRIORITY
                                                                 | DSD_TG_POLICY_FIELD_PREEMPT)
                                        == 0);
    rc |= expect_true("read added fields", dsd_tg_policy_entry_at(st, 0, &row));
    rc |= expect_true("absent mode defaults allow", strcmp(row.mode, "A") == 0);
    rc |= expect_true("selected fields applied", row.priority == 25 && row.preempt && !strcmp(row.name, "Dispatch"));
    rc |= expect_true("listen only", dsd_tg_policy_set_fields(st, 42, 42, &value, DSD_TG_POLICY_FIELD_LISTEN) == 0);
    dsd_tg_policy_entry_at(st, 0, &row);
    rc |= expect_true("metadata preserved and media derived",
                      row.priority == 25 && row.preempt && !row.audio && !strcmp(row.mode, "B"));
    rc |= expect_true("add range", dsd_tg_policy_set_fields(st, 40, 49, &value, DSD_TG_POLICY_FIELD_LISTEN) == 0);
    rc |= expect_true("remove exact", dsd_tg_policy_remove_bounds(st, 42, 42) == 0);
    dsd_tg_policy_lookup found;
    dsd_tg_policy_lookup_id(st, 42, &found);
    rc |= expect_true("remove exposes range", found.match == DSD_TG_POLICY_MATCH_RANGE);
    rc |= expect_true("missing remove refused", dsd_tg_policy_remove_bounds(st, 42, 42) == 1);
    value.priority = 101;
    rc |= expect_true("invalid priority refused",
                      dsd_tg_policy_set_fields(st, 40, 49, &value, DSD_TG_POLICY_FIELD_PRIORITY) == 1);
    rc |= expect_true("invalid mask refused", dsd_tg_policy_set_fields(st, 1, 1, &value, 32) == 1);
    rc |= expect_true("reversed bounds refused",
                      dsd_tg_policy_set_fields(st, 2, 1, &value, DSD_TG_POLICY_FIELD_NAME) == 1);
    dsd_tg_policy_make_exact_entry(99, "D", "Alias", DSD_TG_POLICY_SOURCE_IMPORTED, &row);
    dsd_tg_policy_append_exact(st, &row);
    rc |=
        expect_true("alias edit refused", dsd_tg_policy_set_fields(st, 99, 99, &value, DSD_TG_POLICY_FIELD_NAME) == 1);
    rc |= expect_true("alias remove refused", dsd_tg_policy_remove_bounds(st, 99, 99) == 1);
    free_test_state(st);
    return rc;
}

static int
test_scan_row_policy_activity(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_scan_row_profile profile = {0};
    if (!st) {
        return 1;
    }
    rc |= expect_true("no scan policy active", !dsd_scan_groups_row_active(st));
    profile.values.present = DSD_SCAN_OPT_GROUP;
    rc |= expect_true("reserve scan groups", dsd_scan_groups_begin(st) == 0);
    dsd_scan_groups_enter(st, &profile);
    rc |= expect_true("row policy active", dsd_scan_groups_row_active(st));
    rc |= expect_true("suspend row policy", dsd_scan_groups_suspend(st) == 1);
    rc |= expect_true("suspended row policy inactive", !dsd_scan_groups_row_active(st));
    dsd_scan_groups_resume(st);
    rc |= expect_true("resumed row policy active", dsd_scan_groups_row_active(st));
    dsd_scan_groups_leave(st);
    rc |= expect_true("left row policy inactive", !dsd_scan_groups_row_active(st));
    free_test_state(st);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_set_fields_remove_bounds();
    rc |= test_block_reason_labels();
    rc |= test_snapshot_reclones_recreated_context();
    rc |= test_snapshot_reclones_recreated_empty_reload_context();
    rc |= test_lookup_and_precedence();
    rc |= test_policy_exact_lookup();
    rc |= test_upsert_modes();
    rc |= test_evaluator_behaviors();
    rc |= test_group_file_append_helper();
    rc |= test_preemption_helpers();
    rc |= test_reload_group_file();
    rc |= test_mode_edits_and_enumeration();
    rc |= test_group_file_rewrite();
    rc |= test_scan_row_policy_activity();
    return rc;
}
