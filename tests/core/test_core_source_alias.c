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
#include <dsd-neo/core/csv_validate.h>
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
    dsd_source_alias_entry e = {.id_start = first,
                                .id_end = last,
                                .kind = (uint8_t)(first != last ? DSD_TG_POLICY_ROW_RANGE : DSD_TG_POLICY_ROW_EXACT)};
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
    e.kind = DSD_TG_POLICY_ROW_RANGE;
    assert(dsd_tg_policy_add_range_entry(state, &e) == 0);
    assert(!dsd_source_label_lookup(state, 150, mode, sizeof(mode), name, sizeof(name)));
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
}

static const char* csv_path = "source-alias-patterns.csv";

/* Import @p text as the radio list of @p state (replacing any list it has). */
static void
import_text(dsd_state* state, const char* text) {
    FILE* fp = dsd_fopen_private(csv_path, "w");
    assert(fp);
    assert(fputs(text, fp) >= 0);
    assert(fclose(fp) == 0);
    assert(csvSrcImportPath(csv_path, state) == 0);
    assert(remove(csv_path) == 0);
}

static dsd_csv_validation
validate_text(const char* text) {
    FILE* fp = dsd_fopen_private(csv_path, "w");
    assert(fp);
    assert(fputs(text, fp) >= 0);
    assert(fclose(fp) == 0);
    dsd_csv_validation v = {9, 9, 9};
    assert(dsd_csv_validate_src_file(csv_path, &v) == 0);
    assert(remove(csv_path) == 0);
    return v;
}

static dsd_source_policy_match
policy(const dsd_state* state, uint32_t id) {
    dsd_source_policy_match m;
    DSD_MEMSET(&m, 0xA5, sizeof(m));
    const int rc = dsd_source_policy_lookup(state, id, &m);
    assert(rc == m.matched);
    return m;
}

static void
expect_policy(const dsd_state* state, uint32_t id, const char* mode, int audio, int record, int stream) {
    dsd_source_policy_match m = policy(state, id);
    assert(m.matched == 1 && m.entry.has_policy == 1);
    assert(strcmp(m.entry.mode, mode) == 0);
    assert(m.entry.audio == audio && m.entry.record == record && m.entry.stream == stream);
}

static void
test_radio_patterns_name_radios(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state);
    import_text(state, "id,name\n2*,Fire\n2000123,Engine 3\n1xxx,Thousands\n*05,Ends 05\n*,Everyone\n");
    assert(dsd_source_alias_count(state) == 5);
    label(state, 2000124, "Fire");
    label(state, 2000123, "Engine 3"); /* exact beats the pattern */
    label(state, 1500, "Thousands");   /* 1 literal, no star, beats `*` */
    label(state, 1005, "Ends 05");     /* 1xxx and *05: two literals beat one, star or not */
    label(state, 305, "Ends 05");
    label(state, 7, "Everyone");
    label(state, 0, "Everyone");
    label(state, 4294967295u, "Everyone");
    /* A range beats any pattern; at equal rank the last pattern row wins. */
    import_text(state, "id,name\n12*,First\n12*,Second\n1x*,Wide\n1200-1299,Range\n");
    label(state, 1250, "Range");
    label(state, 1210000, "Second");
    label(state, 1310000, "Wide");
    /* More literals beat fewer; at equal literals a pattern without `*` beats one with it. */
    import_text(state, "id,name\n12xx,No star\n12*x,Star\n1xxx,One literal\n");
    label(state, 1234, "No star");
    label(state, 12345, "Star");
    label(state, 1934, "One literal");
    /* An invalid pattern is skipped like an invalid ID; the radio list stays labels-only. */
    dsd_csv_validation v = validate_text("id,name\n1xxx,Radios\n05x,Leading zero\n4294967296*,Too big\n1x-2x,Pair\n");
    assert(v.accepted == 1 && v.skipped == 3 && v.total == 4);
    assert(!dsd_source_policy_loaded(state));
    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_radio_policy_header(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    dsd_state* dst = calloc(1, sizeof(*dst));
    assert(state && dst);
    import_text(state, "id,name,mode,audio,record,stream,tags\n"
                       "100,Blocked,B\n"
                       "101,Blocked media on,DE,on,on,on\n"
                       "200,No record,A,on,off,on\n"
                       "300,Defaults,A\n"
                       "301,Empty mode,\n"
                       "400,Muted,A,off,on,on,Ops\n"
                       "500,Bad flag,A,maybe,no,1\n");
    assert(dsd_source_policy_loaded(state) && dsd_source_alias_count(state) == 7);
    expect_policy(state, 100, "B", 0, 0, 0);
    expect_policy(state, 101, "DE", 0, 0, 0);
    expect_policy(state, 200, "A", 1, 0, 1);
    expect_policy(state, 300, "A", 1, 1, 1);
    expect_policy(state, 301, "", 1, 1, 1);
    expect_policy(state, 400, "A", 0, 0, 0);
    expect_policy(state, 500, "A", 1, 0, 1); /* an invalid flag keeps its default */
    assert(policy(state, 600).matched == 0);
    label(state, 200, "No record");
    /* Snapshots carry kind, pattern and policy. */
    assert(dsd_source_alias_copy_snapshot(dst, state) == 0);
    assert(dsd_source_policy_loaded(dst));
    expect_policy(dst, 200, "A", 1, 0, 1);
    expect_policy(dst, 400, "A", 0, 0, 0);
    /* The header's column 3 opts in, case-insensitive and trimmed; a column out of order ends the policy prefix. */
    import_text(state, "ID,Name, MODE ,record,audio\n700,Order,A,off,off\n7xx,Pattern,B\n");
    assert(dsd_source_policy_loaded(state));
    expect_policy(state, 700, "A", 1, 1, 1);
    expect_policy(state, 701, "B", 0, 0, 0);
    import_text(state, "id,name,mode,audio,stream\n800,Prefix,A,on,off\n801,Prefix,A,off,on\n");
    expect_policy(state, 800, "A", 1, 1, 1); /* stream sits where record belongs: it is not read */
    expect_policy(state, 801, "A", 0, 0, 0); /* audio, before the break, is */
    /* Without `mode` in column 3 there is no policy: the tags column is ignored, as before. */
    import_text(state, "id,name,tags\n100,Labels,B\n");
    assert(!dsd_source_policy_loaded(state) && dsd_source_alias_count(state) == 1);
    assert(policy(state, 100).matched == 0);
    label(state, 100, "Labels");
    import_text(state, "id,name,audio,mode\n100,Labels,off,B\n");
    assert(!dsd_source_policy_loaded(state) && policy(state, 100).matched == 0);
    /* No store at all. */
    assert(dsd_source_alias_clear(state) == 0);
    assert(!dsd_source_policy_loaded(state) && policy(state, 100).matched == 0);
    assert(dsd_source_policy_lookup(state, 100, NULL) == 0);
    dsd_state_ext_free_all(state);
    dsd_state_ext_free_all(dst);
    free(state);
    free(dst);
}

static void
test_policy_row_may_be_unnamed(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state);
    dsd_csv_validation v = validate_text("id,name,mode\n1234,,B\n1235, ,\n1236\n");
    assert(v.accepted == 2 && v.skipped == 1 && v.total == 3);
    import_text(state, "id,name,mode\n1234,,B\n");
    assert(dsd_source_alias_count(state) == 1);
    expect_policy(state, 1234, "B", 0, 0, 0);
    label(state, 1234, NULL);
    v = validate_text("id,name\n1234,,B\n1235,Named\n");
    assert(v.accepted == 1 && v.skipped == 1 && v.total == 2);
    import_text(state, "id,name\n1234,,B\n");
    assert(dsd_source_alias_count(state) == 0 && !dsd_source_policy_loaded(state));
    /* The store API applies the same rule. */
    dsd_source_alias_store* store = dsd_source_alias_store_create();
    assert(store);
    dsd_source_alias_entry e = {.id_start = 9, .id_end = 9};
    assert(dsd_source_alias_store_append(store, &e) == 1);
    e.has_policy = 1;
    DSD_SNPRINTF(e.mode, sizeof(e.mode), "%s", "B");
    e.audio = e.record = e.stream = 1;
    assert(dsd_source_alias_store_append(store, &e) == 0);
    dsd_source_alias_install(state, store);
    expect_policy(state, 9, "B", 0, 0, 0);
    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_store_validates_patterns(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state);
    dsd_source_alias_store* store = dsd_source_alias_store_create();
    assert(store);
    dsd_source_alias_entry e = {.kind = DSD_TG_POLICY_ROW_PATTERN, .name = "Pattern"};
    assert(dsd_source_alias_store_append(store, &e) == 1); /* empty pattern */
    DSD_SNPRINTF(e.pattern, sizeof(e.pattern), "%s", "05x");
    assert(dsd_source_alias_store_append(store, &e) == 1);
    DSD_SNPRINTF(e.pattern, sizeof(e.pattern), "%s", "1XX**");
    e.id_end = 5;
    assert(dsd_source_alias_store_append(store, &e) == 1); /* a pattern row is never a span */
    e.id_end = 0;
    e.kind = 3;
    assert(dsd_source_alias_store_append(store, &e) == 1);
    e.kind = DSD_TG_POLICY_ROW_PATTERN;
    e.has_policy = 0;
    DSD_SNPRINTF(e.mode, sizeof(e.mode), "%s", "B");
    assert(dsd_source_alias_store_append(store, &e) == 0);
    dsd_source_alias_entry exact = {.id_start = 2, .id_end = 3, .kind = DSD_TG_POLICY_ROW_EXACT, .name = "Bad"};
    assert(dsd_source_alias_store_append(store, &exact) == 1);
    dsd_source_alias_install(state, store);
    label(state, 1234, "Pattern"); /* stored canonical (1xx*), so the matcher reads it */
    label(state, 123, "Pattern");
    label(state, 99, NULL);
    /* A row without policy keeps none: its mode is cleared. */
    assert(!dsd_source_policy_loaded(state) && policy(state, 1234).matched == 0);
    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_label_uses_most_specific_named_row(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state);
    import_text(state, "id,name,mode\n2*,Fire,A\n2001234,,B\n");
    label(state, 2001234, "Fire");
    expect_policy(state, 2001234, "B", 0, 0, 0);
    expect_policy(state, 2001235, "A", 1, 1, 1);
    char name[50], mode[8];
    assert(dsd_source_label_lookup(state, 2001234, mode, sizeof(mode), name, sizeof(name)));
    assert(strcmp(name, "Fire") == 0 && strcmp(mode, "B") == 0);
    /* Policy follows the winning row even when a broader row is the one that names the radio. */
    import_text(state, "id,name,mode\n2001234,Engine,B\n2*,,A\n2001234,Later,A\n");
    label(state, 2001234, "Engine");
    expect_policy(state, 2001234, "B", 0, 0, 0);
    import_text(state, "id,name,mode\n2000000-2999999,,B\n2xxxxxx,Pattern name,A\n");
    label(state, 2001234, "Pattern name");
    expect_policy(state, 2001234, "B", 0, 0, 0);
    /* The first exact row decides policy; the first named exact row names the radio. */
    import_text(state, "id,name,mode\n1201,,B\n1201,Named,A\n1201,Later,DE\n");
    assert(dsd_source_label_lookup(state, 1201, mode, sizeof(mode), name, sizeof(name)));
    assert(strcmp(name, "Named") == 0 && strcmp(mode, "B") == 0);
    expect_policy(state, 1201, "B", 0, 0, 0);
    /* An unnamed pattern decides nothing where a range matches. */
    import_text(state, "id,name,mode\n1xxx,,B\n1000-1999,Range,A\n");
    label(state, 1500, "Range");
    expect_policy(state, 1500, "A", 1, 1, 1);
    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_source_mode_from_radio_policy(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state);
    dsd_tg_policy_entry e;
    assert(dsd_tg_policy_make_exact_entry(1201, "A", "Group row", DSD_TG_POLICY_SOURCE_IMPORTED, &e) == 0);
    assert(dsd_tg_policy_append_exact(state, &e) == 0);
    char name[50], mode[8];
    /* A labels-only radio list leaves the mode to the group list. */
    import_text(state, "id,name\n1201,Radio\n");
    assert(dsd_source_label_lookup(state, 1201, mode, sizeof(mode), name, sizeof(name)));
    assert(strcmp(name, "Radio") == 0 && strcmp(mode, "A") == 0);
    /* A policy row reports its own mode. */
    import_text(state, "id,name,mode\n1201,Radio,B\n12xx,,DE\n");
    assert(dsd_source_label_lookup(state, 1201, mode, sizeof(mode), name, sizeof(name)));
    assert(strcmp(name, "Radio") == 0 && strcmp(mode, "B") == 0);
    /* An unnamed policy row: the name falls back to the group list, the mode is the radio row's. */
    import_text(state, "id,name,mode\n1201,,B\n");
    assert(dsd_source_label_lookup(state, 1201, mode, sizeof(mode), name, sizeof(name)));
    assert(strcmp(name, "Group row") == 0 && strcmp(mode, "B") == 0);
    assert(dsd_source_label_lookup(state, 1299, mode, sizeof(mode), name, sizeof(name)) == 0);
    import_text(state, "id,name,mode\n12xx,,DE\n");
    assert(dsd_source_label_lookup(state, 1299, mode, sizeof(mode), name, sizeof(name)));
    assert(strcmp(name, "") == 0 && strcmp(mode, "DE") == 0);
    /* A policy row with an empty mode states no mode: the group list's stands. */
    import_text(state, "id,name,mode\n1201,Radio,\n");
    assert(dsd_source_label_lookup(state, 1201, mode, sizeof(mode), name, sizeof(name)));
    assert(strcmp(name, "Radio") == 0 && strcmp(mode, "A") == 0);
    dsd_state_ext_free_all(state);
    free(state);
}

static void
test_store_id(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state);
    assert(dsd_source_alias_store_id(state) == 0);
    import_text(state, "id,name\n1,One\n");
    const uint64_t first = dsd_source_alias_store_id(state);
    assert(first != 0);
    assert(dsd_source_alias_store_id(state) == first);
    import_text(state, "id,name\n1,One\n");
    assert(dsd_source_alias_store_id(state) != 0 && dsd_source_alias_store_id(state) != first);
    assert(dsd_source_alias_clear(state) == 0 && dsd_source_alias_store_id(state) == 0);
    dsd_state_ext_free_all(state);
    free(state);
}

int
main(void) {
    test_store();
    test_policy();
    test_radio_patterns_name_radios();
    test_radio_policy_header();
    test_policy_row_may_be_unnamed();
    test_store_validates_patterns();
    test_label_uses_most_specific_named_row();
    test_source_mode_from_radio_policy();
    test_store_id();
    return 0;
}
