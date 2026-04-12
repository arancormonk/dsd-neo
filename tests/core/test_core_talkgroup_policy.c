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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

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
    snprintf(e->mode, sizeof(e->mode), "%s", mode ? mode : "");
    snprintf(e->name, sizeof(e->name), "%s", name ? name : "");
    e->source = source;
    e->audio = (strcmp(e->mode, "B") == 0 || strcmp(e->mode, "DE") == 0) ? 0 : 1;
    e->record = e->audio;
    e->stream = e->audio;
}

typedef struct {
    dsd_tg_policy_entry* entries;
    size_t count;
    size_t capacity;
    unsigned int generation;
} test_tg_policy_table_view;

typedef struct {
    test_tg_policy_table_view table;
} test_tg_policy_context_view;

static unsigned int
policy_generation(const dsd_state* st) {
    const test_tg_policy_context_view* ctx =
        (const test_tg_policy_context_view*)dsd_state_ext_get_const(st, DSD_STATE_EXT_CORE_TG_POLICY);
    if (!ctx) {
        return 0u;
    }
    return ctx->table.generation;
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
    rc |= expect_true("append exact first", dsd_tg_policy_append_legacy_exact(st, &e) == 0);
    init_entry(&e, 1201, "B", "SECOND", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append exact duplicate", dsd_tg_policy_append_legacy_exact(st, &e) == 0);

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

    free(st);
    return rc;
}

static int
test_legacy_exact_fallback(void) {
    int rc = 0;
    dsd_state* st = (dsd_state*)calloc(1, sizeof(*st));
    dsd_tg_policy_entry e;
    dsd_tg_policy_lookup lookup;
    if (!st) {
        return 1;
    }
    st->group_array[0].groupNumber = 42;
    snprintf(st->group_array[0].groupMode, sizeof(st->group_array[0].groupMode), "%s", "DE");
    snprintf(st->group_array[0].groupName, sizeof(st->group_array[0].groupName), "%s", "LEGACY");
    st->group_tally = 1;

    init_entry(&e, 1, "A", "RANGE", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.id_end = 100;
    e.is_range = 1;
    rc |= expect_true("append range fallback", dsd_tg_policy_add_range_entry(st, &e) == 0);

    rc |= expect_true("lookup fallback exact", dsd_tg_policy_lookup_id(st, 42, &lookup) == 0);
    rc |= expect_true("fallback is exact", lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    rc |= expect_true("fallback source", lookup.entry.source == DSD_TG_POLICY_SOURCE_LEGACY_UNKNOWN);
    rc |= expect_true("fallback mode", strcmp(lookup.entry.mode, "DE") == 0);
    rc |= expect_true("lookup range other", dsd_tg_policy_lookup_id(st, 55, &lookup) == 0);
    rc |= expect_true("range for non-exact", lookup.match == DSD_TG_POLICY_MATCH_RANGE);

    init_entry(&e, 200, "A", "BAD-RANGE", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.is_range = 0;
    rc |= expect_true("reject exact in range helper", dsd_tg_policy_add_range_entry(st, &e) == 1);

    free(st);
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
    rc |= expect_true("append imported", dsd_tg_policy_append_legacy_exact(st, &e) == 0);

    init_entry(&e, 500, "B", "LOCKOUT", DSD_TG_POLICY_SOURCE_USER_LOCKOUT);
    rc |= expect_true("add-if-missing no-op",
                      dsd_tg_policy_upsert_legacy_exact(st, &e, DSD_TG_POLICY_UPSERT_ADD_IF_MISSING) == 0);
    rc |= expect_true("lookup imported unchanged", dsd_tg_policy_lookup_id(st, 500, &lookup) == 0);
    rc |= expect_true("imported protected", strcmp(lookup.entry.mode, "A") == 0);

    rc |= expect_true("replace-first updates",
                      dsd_tg_policy_upsert_legacy_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0);
    rc |= expect_true("lookup replaced", dsd_tg_policy_lookup_id(st, 500, &lookup) == 0);
    rc |= expect_true("mode replaced", strcmp(lookup.entry.mode, "B") == 0);

    init_entry(&e, 600, "D", "LEARN-OLD", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("append learned", dsd_tg_policy_append_legacy_exact(st, &e) == 0);
    init_entry(&e, 600, "D", "LEARN-NEW", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("replace-learned updates learned",
                      dsd_tg_policy_upsert_legacy_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY) == 0);
    rc |= expect_true("learned refreshed", dsd_tg_policy_lookup_id(st, 600, &lookup) == 0);
    rc |= expect_true("learned name refreshed", strcmp(lookup.entry.name, "LEARN-NEW") == 0);

    init_entry(&e, 500, "D", "SHOULD-NOT-CHANGE", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("replace-learned protects imported/lockout",
                      dsd_tg_policy_upsert_legacy_exact(st, &e, DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY) == 0);
    rc |= expect_true("protected row unchanged", dsd_tg_policy_lookup_id(st, 500, &lookup) == 0);
    rc |= expect_true("protected row still lockout", strcmp(lookup.entry.mode, "B") == 0);

    free(st);
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
        free(st);
        free(opts);
        return 1;
    }

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
    rc |= expect_true("append eval row", dsd_tg_policy_append_legacy_exact(st, &e) == 0);

    rc |= expect_true(
        "allowlist miss eval",
        dsd_tg_policy_evaluate_group_call(opts, st, 701, 0, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0);
    rc |= expect_true("allowlist miss blocks tune", decision.tune_allowed == 0);
    rc |= expect_true("allowlist block bit", (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0);

    rc |= expect_true(
        "known eval",
        dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0);
    rc |= expect_true("known tune allowed", decision.tune_allowed == 1);
    rc |= expect_true("known record blocked", decision.record_allowed == 0);
    rc |= expect_true("record block reason", (decision.block_reasons & DSD_TG_POLICY_BLOCK_RECORD) != 0);
    rc |= expect_true("priority preserved", decision.priority == 10 && decision.preempt_requested == 1);

    opts->trunk_tune_group_calls = 0;
    rc |= expect_true(
        "group toggle block",
        dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0);
    rc |= expect_true("group disabled bit", (decision.block_reasons & DSD_TG_POLICY_BLOCK_GROUP_DISABLED) != 0);
    opts->trunk_tune_group_calls = 1;

    opts->trunk_tune_data_calls = 0;
    rc |= expect_true(
        "data toggle block",
        dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 0, 1, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0);
    rc |= expect_true("data disabled bit", (decision.block_reasons & DSD_TG_POLICY_BLOCK_DATA_DISABLED) != 0);
    opts->trunk_tune_data_calls = 1;

    opts->trunk_tune_enc_calls = 0;
    rc |= expect_true(
        "enc toggle block",
        dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 1, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0);
    rc |= expect_true("enc disabled bit", (decision.block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) != 0);
    opts->trunk_tune_enc_calls = 1;

    init_entry(&e, 702, "DE", "ENC-LO", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append de row", dsd_tg_policy_append_legacy_exact(st, &e) == 0);
    st->tg_hold = 702;
    rc |= expect_true(
        "hold compat keeps mode block",
        dsd_tg_policy_evaluate_group_call(opts, st, 702, 0, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0);
    rc |= expect_true("hold compat tune blocked", decision.tune_allowed == 0);
    rc |= expect_true("hold compat audio blocked", decision.audio_allowed == 0);

    rc |= expect_true(
        "hold media-only force",
        dsd_tg_policy_evaluate_group_call(opts, st, 702, 0, 0, 0, DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY, &decision) == 0);
    rc |= expect_true("hold media-only keeps tune blocked", decision.tune_allowed == 0);
    rc |= expect_true("hold media-only unblocks media",
                      decision.audio_allowed == 1 && decision.record_allowed == 1 && decision.stream_allowed == 1);
    rc |= expect_true(
        "hold force tune+media",
        dsd_tg_policy_evaluate_group_call(opts, st, 702, 0, 0, 0, DSD_TG_POLICY_HOLD_FORCE_TUNE_AND_MEDIA, &decision)
            == 0);
    rc |= expect_true("hold force tune+media allows all", decision.tune_allowed == 1 && decision.audio_allowed == 1
                                                              && decision.record_allowed == 1
                                                              && decision.stream_allowed == 1);
    rc |= expect_true("hold force tune+media clears mode/hold bits",
                      (decision.block_reasons & (DSD_TG_POLICY_BLOCK_MODE | DSD_TG_POLICY_BLOCK_HOLD)) == 0);

    rc |= expect_true(
        "hold mismatch block",
        dsd_tg_policy_evaluate_group_call(opts, st, 700, 0, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision) == 0);
    rc |= expect_true("hold mismatch reason", (decision.block_reasons & DSD_TG_POLICY_BLOCK_HOLD) != 0);
    rc |= expect_true("hold mismatch mutes media", decision.audio_allowed == 0);

    init_entry(&e, 900, "A", "SRC-ALIAS", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append private src", dsd_tg_policy_append_legacy_exact(st, &e) == 0);
    opts->trunk_tune_private_calls = 1;
    opts->trunk_use_allow_list = 1;
    st->tg_hold = 0;
    rc |= expect_true("private unknown block", dsd_tg_policy_evaluate_private_call(
                                                   opts, st, 1, 2, 0, 0, DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                   DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                                                   == 0);
    rc |= expect_true("private allowlist blocked",
                      decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0);

    rc |= expect_true("private with known src",
                      dsd_tg_policy_evaluate_private_call(opts, st, 900, 901, 0, 0,
                                                          DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                          DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                          == 0);
    rc |= expect_true("private known src allowed", decision.tune_allowed == 1);

    init_entry(&e, 901, "B", "DST-BLOCK", DSD_TG_POLICY_SOURCE_IMPORTED);
    rc |= expect_true("append private dst block", dsd_tg_policy_append_legacy_exact(st, &e) == 0);
    rc |= expect_true("private mode block", dsd_tg_policy_evaluate_private_call(
                                                opts, st, 900, 901, 0, 0, DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_ALLOW,
                                                DSD_TG_POLICY_HOLD_COMPAT_GRANT, &decision)
                                                == 0);
    rc |= expect_true("private mode block reason",
                      decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_MODE) != 0);

    st->tg_hold = 0;
    rc |= expect_true("private unknown blocked (force tune/media)",
                      dsd_tg_policy_evaluate_private_call(opts, st, 1000, 1001, 0, 0,
                                                          DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                          DSD_TG_POLICY_HOLD_FORCE_TUNE_AND_MEDIA, &decision)
                          == 0);
    rc |= expect_true("private unknown blocked reason (force tune/media)",
                      decision.tune_allowed == 0 && (decision.block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) != 0);
    st->tg_hold = 1001;
    rc |= expect_true("private hold override (force tune/media)",
                      dsd_tg_policy_evaluate_private_call(opts, st, 1000, 1001, 0, 0,
                                                          DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK,
                                                          DSD_TG_POLICY_HOLD_FORCE_TUNE_AND_MEDIA, &decision)
                          == 0);
    rc |= expect_true("private hold override enables tune/media",
                      decision.tune_allowed == 1 && decision.audio_allowed == 1 && decision.record_allowed == 1
                          && decision.stream_allowed == 1);

    free(st);
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
    if (l3) {
        (void)fgets(l3, (int)l3sz, fp);
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

    init_entry(&e, 100, "D", "Alias", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    e.priority = 7;
    e.preempt = 1;
    e.audio = 1;
    e.record = 0;
    e.stream = 1;

    snprintf(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    (void)remove(path_template);

    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append to missing file", dsd_tg_policy_append_group_file_row(opts, &e, "") == 0);
    rc |= expect_true("read missing-created file",
                      read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("missing header", strcmp(l1, "id,mode,name\n") == 0);
    rc |= expect_true("missing row", strstr(l2, "100,D,Alias") == l2);
    (void)remove(path_template);

    snprintf(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append to empty file", dsd_tg_policy_append_group_file_row(opts, &e, "ALG:01") == 0);
    rc |= expect_true("read empty-appended file",
                      read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("empty header metadata", strcmp(l1, "id,mode,name,metadata\n") == 0);
    rc |= expect_true("empty row metadata", strstr(l2, ",ALG:01") != NULL);
    (void)remove(path_template);

    snprintf(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = fopen(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(opts);
            return 1;
        }
        fprintf(fp, "id,mode,name,tag\n");
        fclose(fp);
    }
    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append to legacy header", dsd_tg_policy_append_group_file_row(opts, &e, "VEND") == 0);
    rc |= expect_true("read legacy file", read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("legacy row contains metadata", strstr(l2, ",VEND") != NULL);
    (void)remove(path_template);

    snprintf(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = fopen(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(opts);
            return 1;
        }
        fprintf(fp, "id,mode,name,priority,preempt,audio,record,stream,tags\n");
        fclose(fp);
    }
    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append to policy header", dsd_tg_policy_append_group_file_row(opts, &e, "hello,\nworld") == 0);
    rc |= expect_true("read policy file", read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |= expect_true("policy row full schema", strstr(l2, "100,D,Alias,7,true,on,off,on,hello  world") == l2);
    (void)remove(path_template);

    snprintf(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = fopen(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(opts);
            return 1;
        }
        fprintf(fp, "id,mode,name,PRIORITY,wrong_order\n");
        fclose(fp);
    }
    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append with policy-prefix header",
                      dsd_tg_policy_append_group_file_row(opts, &e, "TAG-UPPER") == 0);
    rc |= expect_true("read policy-prefix file",
                      read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), NULL, 0) == 0);
    rc |=
        expect_true("policy-prefix row uses policy schema", strstr(l2, "100,D,Alias,7,true,on,off,on,TAG-UPPER") == l2);
    (void)remove(path_template);

    snprintf(path_template, sizeof(path_template), "%s", path_seed);
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = fopen(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(opts);
            return 1;
        }
        fprintf(fp, "id,mode,name,something_else\n");
        fclose(fp);
    }
    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    rc |= expect_true("append unknown legacy header", dsd_tg_policy_append_group_file_row(opts, &e, "META") == 0);
    rc |= expect_true("read unknown legacy file",
                      read_file_lines(path_template, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3)) == 0);
    rc |= expect_true("unknown header unchanged", strcmp(l1, "id,mode,name,something_else\n") == 0);
    rc |= expect_true("unknown row legacy format", strstr(l2, "100,D,Alias,META") == l2);
    (void)remove(path_template);

    free(opts);
    return rc;
}

static void
init_route(dsd_tg_policy_call_route* r, uint32_t target, uint32_t source, long freq_hz, int channel, int slot,
           int needs_retune) {
    memset(r, 0, sizeof(*r));
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
    memset(d, 0, sizeof(*d));
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
        free(st);
        free(opts);
        return 1;
    }

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

    init_entry(&e, 5001, "B", "HOLD-CAND", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.priority = 90;
    e.preempt = 1;
    rc |= expect_true("seed hold candidate row", dsd_tg_policy_append_legacy_exact(st, &e) == 0);
    init_route(&cand, 5001, 10, 854000000L, 30, 0, 0);

    st->tg_hold = 5999;
    rc |= expect_true(
        "hold mismatch blocks tune",
        dsd_tg_policy_evaluate_group_call(opts, st, 5001, 10, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &cand_dec) == 0
            && cand_dec.tune_allowed == 0 && (cand_dec.block_reasons & DSD_TG_POLICY_BLOCK_HOLD) != 0);
    rc |= expect_true("hold mismatch blocks preempt",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 11.5) == 0);

    st->tg_hold = 5001;
    rc |= expect_true(
        "hold compat keeps mode block",
        dsd_tg_policy_evaluate_group_call(opts, st, 5001, 10, 0, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &cand_dec) == 0
            && cand_dec.tune_allowed == 0 && (cand_dec.block_reasons & DSD_TG_POLICY_BLOCK_MODE) != 0);
    rc |=
        expect_true("hold compat blocks preempt", dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 11.5) == 0);

    rc |= expect_true(
        "hold force tune/media allows candidate",
        dsd_tg_policy_evaluate_group_call(opts, st, 5001, 10, 0, 0, DSD_TG_POLICY_HOLD_FORCE_TUNE_AND_MEDIA, &cand_dec)
                == 0
            && cand_dec.tune_allowed == 1 && cand_dec.preempt_requested == 1);
    rc |= expect_true("hold force tune/media allows preempt",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 11.5) == 1);
    st->tg_hold = 0;

    init_entry(&e, 5002, "A", "ENC-CAND", DSD_TG_POLICY_SOURCE_IMPORTED);
    e.priority = 95;
    e.preempt = 1;
    rc |= expect_true("seed encrypted candidate row", dsd_tg_policy_append_legacy_exact(st, &e) == 0);
    init_route(&cand, 5002, 11, 854000000L, 30, 0, 0);
    opts->trunk_tune_enc_calls = 0;
    rc |= expect_true(
        "encrypted candidate tune blocked",
        dsd_tg_policy_evaluate_group_call(opts, st, 5002, 11, 1, 0, DSD_TG_POLICY_HOLD_COMPAT_GRANT, &cand_dec) == 0
            && cand_dec.tune_allowed == 0 && (cand_dec.block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) != 0);
    rc |= expect_true("encrypted candidate blocked preempt",
                      dsd_tg_policy_should_preempt(opts, st, &cand, &cand_dec, 12.0) == 0);
    opts->trunk_tune_enc_calls = 1;

    rc |= expect_true("clear route by tg/channel", dsd_tg_policy_clear_active_call_route(st, &active0) == 0);
    rc |= expect_true("clear slot0", dsd_tg_policy_clear_active_call(st, 0) == 0);
    rc |= expect_true("clear slot1", dsd_tg_policy_clear_active_call(st, 1) == 0);
    rc |= expect_true("reject invalid slot clear", dsd_tg_policy_clear_active_call(st, 2) == 1);

    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS");
    (void)dsd_unsetenv("DSD_NEO_TG_PREEMPT_COOLDOWN_MS");
    free(st);
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
        free(st);
        free(opts);
        return 1;
    }

    init_entry(&e, 999, "A", "OLD", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS);
    rc |= expect_true("seed old row", dsd_tg_policy_append_legacy_exact(st, &e) == 0);
    rc |= expect_true("seed generation starts at zero", policy_generation(st) == 0u);

    snprintf(path_template, sizeof(path_template), "%s", "dsd-neo-test-tg-reload-XXXXXX");
    fd = dsd_mkstemp(path_template);
    if (fd < 0) {
        free(st);
        free(opts);
        return 1;
    }
    (void)dsd_close(fd);
    {
        FILE* fp = fopen(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(st);
            free(opts);
            return 1;
        }
        fprintf(fp, "id,mode,name,priority,preempt,audio,record,stream,tags\n");
        fprintf(fp, "100,A,ONE,5,true,on,on,on,\n");
        fprintf(fp, "200,B,TWO,1,false,off,off,off,\n");
        fclose(fp);
    }
    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    generation_before = policy_generation(st);
    rc |= expect_true("reload success", dsd_tg_policy_reload_group_file(opts, st) == 0);
    rc |= expect_true("reload replaced legacy rows", st->group_tally == 2);
    generation_after = policy_generation(st);
    rc |= expect_true("reload success increments generation", generation_after == generation_before + 1u);
    rc |= expect_true("lookup row 100", dsd_tg_policy_lookup_id(st, 100, &lookup) == 0);
    rc |= expect_true("lookup row 100 exact", lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    rc |= expect_true("lookup row 100 fields", strcmp(lookup.entry.name, "ONE") == 0 && lookup.entry.priority == 5);
    rc |= expect_true("lookup row 200", dsd_tg_policy_lookup_id(st, 200, &lookup) == 0);
    rc |= expect_true("lookup row 200 blocked mode", strcmp(lookup.entry.mode, "B") == 0);

    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", "/tmp/dsd-neo-missing-group-file-nope.csv");
    generation_before = policy_generation(st);
    rc |= expect_true("reload missing file fails", dsd_tg_policy_reload_group_file(opts, st) != 0);
    rc |= expect_true("failed reload keeps current rows", st->group_tally == 2);
    rc |= expect_true("failed reload keeps policy", dsd_tg_policy_lookup_id(st, 100, &lookup) == 0);
    rc |= expect_true("failed reload preserved entry", strcmp(lookup.entry.name, "ONE") == 0);
    rc |= expect_true("failed reload keeps generation", policy_generation(st) == generation_before);

    {
        FILE* fp = fopen(path_template, "w");
        if (!fp) {
            (void)remove(path_template);
            free(st);
            free(opts);
            return 1;
        }
        fprintf(fp, "id,mode,name\n");
        fprintf(fp, "300,A,THREE\n");
        fclose(fp);
    }
    snprintf(opts->group_in_file, sizeof(opts->group_in_file), "%s", path_template);
    generation_before = policy_generation(st);
    rc |= expect_true("second reload success", dsd_tg_policy_reload_group_file(opts, st) == 0);
    rc |= expect_true("second reload replaced legacy rows", st->group_tally == 1);
    rc |= expect_true("second reload increments generation", policy_generation(st) == generation_before + 1u);
    rc |= expect_true("second reload row applied", dsd_tg_policy_lookup_id(st, 300, &lookup) == 0);
    rc |= expect_true("second reload row exact", lookup.match == DSD_TG_POLICY_MATCH_EXACT);
    rc |= expect_true("second reload row name", strcmp(lookup.entry.name, "THREE") == 0);

    (void)remove(path_template);
    free(st);
    free(opts);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_lookup_and_precedence();
    rc |= test_legacy_exact_fallback();
    rc |= test_upsert_modes();
    rc |= test_evaluator_behaviors();
    rc |= test_group_file_append_helper();
    rc |= test_preemption_helpers();
    rc |= test_reload_group_file();
    return rc;
}
