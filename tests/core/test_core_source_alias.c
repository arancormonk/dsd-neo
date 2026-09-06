// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Source aliases: CSV grammar `id,name[,tags]`, decimal uint32_t ID or inclusive
 * `start-end` range. See docs/csv-formats.md, "Source ID List CSV".
 */
#include <assert.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
add(dsd_source_alias_store* store, uint32_t first, uint32_t last, const char* name) {
    dsd_source_alias_entry e = {.id_start = first, .id_end = last, .is_range = (first != last)};
    DSD_SNPRINTF(e.name, sizeof(e.name), "%s", name);
    assert(dsd_source_alias_store_append(store, &e) == 0);
}

static void
label(const dsd_state* state, uint32_t id, const char* expected) {
    char name[50];
    assert(dsd_source_alias_lookup(state, id, name, sizeof(name)) == (expected != NULL));
    assert(strcmp(name, expected ? expected : "") == 0);
}

static void
test_store(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    dsd_state* dst = calloc(1, sizeof(*dst));
    assert(state && dst);
    assert(!dsd_source_alias_loaded(state) && !dsd_source_alias_count(state));
    dsd_source_alias_store* store = dsd_source_alias_store_create();
    assert(store);
    add(store, 1000, 2000, "Wide");
    add(store, 1100, 1300, "Narrow first");
    add(store, 1150, 1350, "Equal later");
    add(store, 1201, 1201, "Unit 1201");
    add(store, 1201, 1201, "Duplicate later");
    dsd_source_alias_entry bad = {.id_start = 1, .id_end = 1};
    assert(dsd_source_alias_store_append(store, &bad) == 1);
    DSD_MEMSET(bad.name, 'x', sizeof(bad.name));
    assert(dsd_source_alias_store_append(store, &bad) == 0);
    dsd_source_alias_install(state, store);
    assert(dsd_source_alias_loaded(state) && dsd_source_alias_count(state) == 6);
    label(state, 1201, "Unit 1201");
    label(state, 1200, "Equal later");
    label(state, 1120, "Narrow first");
    label(state, 1900, "Wide");
    label(state, 0, NULL);
    char name[50];
    assert(dsd_source_alias_lookup(state, 1, name, sizeof(name)));
    assert(strlen(name) == 49);
    assert(dsd_source_alias_copy_snapshot(dst, state) == 0);
    const void* clone = dsd_state_ext_get_const(dst, DSD_STATE_EXT_CORE_SOURCE_ALIAS);
    assert(clone != store);
    assert(dsd_source_alias_copy_snapshot(dst, state) == 0);
    assert(clone == dsd_state_ext_get_const(dst, DSD_STATE_EXT_CORE_SOURCE_ALIAS));
    label(dst, 1201, "Unit 1201");
    store = dsd_source_alias_store_create();
    assert(store);
    for (int i = 0; i < 6; ++i) {
        add(store, 1201, 1201, "Replacement");
    }
    dsd_source_alias_install(state, store);
    assert(dsd_source_alias_copy_snapshot(dst, state) == 0);
    label(dst, 1201, "Replacement");
    for (int i = 0; i < 2; ++i) {
        store = dsd_source_alias_store_create();
        assert(store);
        dsd_source_alias_install(state, store);
        clone = dsd_state_ext_get_const(dst, DSD_STATE_EXT_CORE_SOURCE_ALIAS);
        assert(dsd_source_alias_copy_snapshot(dst, state) == 0);
        assert(clone != dsd_state_ext_get_const(dst, DSD_STATE_EXT_CORE_SOURCE_ALIAS));
        assert(dsd_source_alias_loaded(dst) && dsd_source_alias_count(dst) == 0);
    }
    assert(dsd_source_alias_clear(state) == 0);
    assert(dsd_source_alias_copy_snapshot(dst, state) == 0 && !dsd_source_alias_loaded(dst));
    dsd_state_ext_free_all(state);
    dsd_state_ext_free_all(dst);
    free(state);
    free(dst);
    dsd_source_alias_store_free(NULL);
}

static void
same_decision(const dsd_tg_policy_decision* a, const dsd_tg_policy_decision* b) {
    assert(a->tune_allowed == b->tune_allowed && a->audio_allowed == b->audio_allowed);
    assert(a->record_allowed == b->record_allowed && a->stream_allowed == b->stream_allowed);
    assert(a->priority == b->priority && a->preempt_requested == b->preempt_requested);
    assert(a->block_reasons == b->block_reasons && a->match == b->match);
    assert(a->target_id == b->target_id && a->source_id == b->source_id);
    assert(a->encrypted == b->encrypted && a->data_call == b->data_call);
    assert(a->tg_hold_active == b->tg_hold_active && a->tg_hold_match == b->tg_hold_match);
    assert(strcmp(a->mode, b->mode) == 0 && strcmp(a->name, b->name) == 0);
}

static void
test_policy(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    dsd_opts* opts = calloc(1, sizeof(*opts));
    assert(state && opts);
    dsd_tg_policy_entry e;
    for (int blocked = 0; blocked < 2; ++blocked) {
        if (blocked) {
            assert(dsd_tg_policy_make_exact_entry(1201, "B", "Talkgroup 1201", DSD_TG_POLICY_SOURCE_IMPORTED, &e) == 0);
            assert(dsd_tg_policy_append_exact(state, &e) == 0);
        }
        dsd_tg_policy_decision g1, g2, p1, p2;
        int entries = dsd_tg_policy_has_entries(state);
        int gr = dsd_tg_policy_evaluate_group_call(opts, state, 1201, 1201, 0, 0, &g1);
        int pr = dsd_tg_policy_evaluate_private_call(opts, state, 1201, 999, 0, 0, &p1);
        FILE* fp = dsd_fopen_private("source-alias-policy.csv", "w");
        assert(fp);
        assert(fputs("id,name\n1201,Unit 1201\n", fp) >= 0);
        assert(fclose(fp) == 0);
        assert(csvSrcImportPath("source-alias-policy.csv", state) == 0);
        assert(remove("source-alias-policy.csv") == 0);
        assert(dsd_tg_policy_has_entries(state) == entries);
        assert(dsd_tg_policy_evaluate_group_call(opts, state, 1201, 1201, 0, 0, &g2) == gr);
        assert(dsd_tg_policy_evaluate_private_call(opts, state, 1201, 999, 0, 0, &p2) == pr);
        same_decision(&g1, &g2);
        same_decision(&p1, &p2);
        if (blocked) {
            assert(!g2.tune_allowed && !p2.tune_allowed);
        }
        char name[50], mode[8];
        assert(dsd_source_label_lookup(state, 1201, mode, sizeof(mode), name, sizeof(name)));
        assert(strcmp(name, "Unit 1201") == 0 && strcmp(mode, blocked ? "B" : "") == 0);
        if (blocked) {
            assert(dsd_tg_policy_lookup_label(state, 1201, NULL, 0, name, sizeof(name)));
            assert(strcmp(name, "Talkgroup 1201") == 0);
        }
        assert(dsd_source_alias_clear(state) == 0);
    }
    assert(dsd_tg_policy_make_exact_entry(42, "A", "OTA", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &e) == 0);
    assert(dsd_tg_policy_append_exact(state, &e) == 0);
    char name[50], mode[8];
    assert(dsd_source_label_lookup(state, 42, mode, sizeof(mode), name, sizeof(name)) && strcmp(name, "OTA") == 0);
    dsd_source_alias_store* store = dsd_source_alias_store_create();
    assert(store);
    add(store, 42, 42, "CSV");
    dsd_source_alias_install(state, store);
    assert(dsd_source_label_lookup(state, 42, mode, sizeof(mode), name, sizeof(name)) && strcmp(name, "CSV") == 0
           && strcmp(mode, "A") == 0);
    e.id_start = 100;
    e.id_end = 200;
    e.is_range = 1;
    assert(dsd_tg_policy_add_range_entry(state, &e) == 0);
    assert(!dsd_source_label_lookup(state, 150, mode, sizeof(mode), name, sizeof(name)));
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
}

int
main(void) {
    test_store();
    test_policy();
    return 0;
}
