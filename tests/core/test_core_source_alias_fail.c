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
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* path = "source-alias-fail.csv";

static void
write_text(const char* text) {
    FILE* fp = dsd_fopen_private(path, "w");
    assert(fp);
    assert(fputs(text, fp) >= 0);
    assert(fclose(fp) == 0);
}

static void
expect_live(const dsd_state* state) {
    char name[50];
    assert(dsd_source_alias_lookup(state, 1201, name, sizeof(name)));
    assert(strcmp(name, "Live") == 0);
}

static void
test_failures(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    dsd_state* dst = calloc(1, sizeof(*dst));
    assert(state && dst);
    write_text("id,name\n1201,Live\n1202,Second\n");
    assert(csvSrcImportPath(path, state) == 0);
    dsd_source_alias_store* store = dsd_source_alias_store_create();
    assert(store);
    dsd_source_alias_entry e = {.id_start = 1, .id_end = 1, .name = "Name"};
    dsd_source_alias_test_alloc_fail_after(0);
    assert(dsd_source_alias_store_append(store, &e) == -1);
    dsd_source_alias_test_alloc_reset();
    for (unsigned i = 0; i < 16; ++i) {
        assert(dsd_source_alias_store_append(store, &e) == 0);
    }
    dsd_source_alias_test_alloc_fail_after(0);
    assert(dsd_source_alias_store_append(store, &e) == -1 && dsd_source_alias_store_count(store) == 16);
    dsd_source_alias_test_alloc_reset();
    assert(dsd_source_alias_store_append(store, &e) == 0 && dsd_source_alias_store_count(store) == 17);
    dsd_source_alias_store_free(store);
    for (long n = 0; n < 2; ++n) {
        store = (dsd_source_alias_store*)dsd_state_ext_get(state, DSD_STATE_EXT_CORE_SOURCE_ALIAS);
        dsd_source_alias_store* out = store;
        dsd_source_alias_test_alloc_fail_after(n);
        assert(dsd_source_alias_load(path, &out) == -1 && out == store);
        dsd_source_alias_test_alloc_fail_after(n);
        assert(csvSrcImportPath(path, state) == -1);
        expect_live(state);
        dsd_source_alias_test_alloc_reset();
    }
    store = (dsd_source_alias_store*)dsd_state_ext_get(state, DSD_STATE_EXT_CORE_SOURCE_ALIAS);
    dsd_source_alias_store* out = store;
    dsd_source_alias_test_read_fail_after_lines(2);
    assert(dsd_source_alias_load(path, &out) == -1 && out == store);
    dsd_source_alias_test_read_fail_after_lines(2);
    assert(csvSrcImportPath(path, state) == -1);
    expect_live(state);
    dsd_csv_validation counts = {9, 9, 9};
    dsd_source_alias_test_read_fail_after_lines(2);
    assert(dsd_csv_validate_src_file(path, &counts) == -1);
    assert(counts.accepted == 0 && counts.skipped == 0 && counts.total == 0);
    dsd_source_alias_test_read_fail_after_lines(-1);
    for (long n = 0; n < 2; ++n) {
        assert(dsd_source_alias_copy_snapshot(dst, state) == 0);
        /* A replacement source defeats the clone identity fast path. */
        assert(csvSrcImportPath(path, state) == 0);
        dsd_source_alias_test_alloc_fail_after(n);
        assert(dsd_source_alias_copy_snapshot(dst, state) == -1);
        expect_live(dst);
        expect_live(state);
        dsd_source_alias_test_alloc_reset();
        assert(dsd_source_alias_clear(dst) == 0);
        dst->state_ext[DSD_STATE_EXT_CORE_SOURCE_ALIAS] = state->state_ext[DSD_STATE_EXT_CORE_SOURCE_ALIAS];
        dst->state_ext_cleanup[DSD_STATE_EXT_CORE_SOURCE_ALIAS] =
            state->state_ext_cleanup[DSD_STATE_EXT_CORE_SOURCE_ALIAS];
        dsd_source_alias_test_alloc_fail_after(n);
        assert(dsd_source_alias_copy_snapshot(dst, state) == -1);
        assert(!dsd_source_alias_loaded(dst) && !dst->state_ext_cleanup[DSD_STATE_EXT_CORE_SOURCE_ALIAS]);
        expect_live(state);
        dsd_source_alias_test_alloc_reset();
    }
    dsd_state_ext_free_all(state);
    dsd_state_ext_free_all(dst);
    free(state);
    free(dst);
}

static void
boundary(size_t length, const char* suffix, int header, unsigned accepted, unsigned skipped) {
    FILE* fp = dsd_fopen_private(path, "wb");
    assert(fp);
    if (!header) {
        assert(fputs("id,name\n", fp) >= 0);
    }
    const char* prefix = "1201,Name,";
    assert(fputs(prefix, fp) >= 0);
    for (size_t i = strlen(prefix); i < length; ++i) {
        assert(fputc('x', fp) != EOF);
    }
    assert(fputs(suffix, fp) >= 0);
    assert(fclose(fp) == 0);
    dsd_csv_validation v;
    assert(dsd_csv_validate_src_file(path, &v) == 0);
    assert(v.accepted == accepted && v.skipped == skipped && v.total == accepted + skipped);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state);
    assert(csvSrcImportPath(path, state) == 0);
    assert(dsd_source_alias_count(state) == accepted);
    char name[50];
    if (accepted && !skipped) {
        assert(dsd_source_alias_lookup(state, 1201, name, sizeof(name)));
        assert(strcmp(name, "Name") == 0);
    }
    if (skipped) {
        assert(!dsd_source_alias_lookup(state, 1201, name, sizeof(name)));
        assert(!dsd_source_alias_lookup(state, 1202, name, sizeof(name)));
    }
    if (accepted && skipped) {
        assert(dsd_source_alias_lookup(state, 1203, name, sizeof(name)));
        assert(strcmp(name, "Next") == 0);
    }
    dsd_state_ext_free_all(state);
    free(state);
}

/* Raw bytes (fputs would stop at the NUL): a NUL anywhere makes that physical line one
 * skipped row, whether or not it is also overlong, and never leaks a continuation row. */
static void
raw_case(const unsigned char* data, size_t len, unsigned accepted, unsigned skipped, uint32_t present,
         uint32_t absent) {
    FILE* fp = dsd_fopen_private(path, "wb");
    assert(fp);
    assert(fwrite(data, 1, len, fp) == len);
    assert(fclose(fp) == 0);
    dsd_csv_validation v;
    assert(dsd_csv_validate_src_file(path, &v) == 0);
    assert(v.accepted == accepted && v.skipped == skipped && v.total == accepted + skipped);
    dsd_state* state = calloc(1, sizeof(*state));
    assert(state);
    assert(csvSrcImportPath(path, state) == 0);
    assert(dsd_source_alias_count(state) == accepted);
    char name[50];
    assert(dsd_source_alias_lookup(state, present, name, sizeof(name)) == (accepted != 0));
    assert(!dsd_source_alias_lookup(state, absent, name, sizeof(name)));
    dsd_state_ext_free_all(state);
    free(state);
}

static void
nul_cases(void) {
    static const unsigned char short_nul[] = "id,name\n12\0,Short\n789,Ok\n";
    raw_case(short_nul, sizeof(short_nul) - 1U, 1, 1, 789, 12);
    unsigned char padded[8 + 998 + 13];
    size_t n = 0;
    DSD_MEMCPY(padded, "id,name\n", 8);
    n += 8;
    DSD_MEMCPY(padded + n, "123,Invalid", 11);
    n += 11;
    padded[n++] = '\0';
    while (n < 8 + 998) {
        padded[n++] = 'x';
    }
    DSD_MEMCPY(padded + n, "456,Injected\n", 13);
    n += 13;
    raw_case(padded, n, 0, 1, 456, 123);
}

static void
test_validator_discards_partial_counts_on_allocation_failure(void) {
    FILE* fp = dsd_fopen_private(path, "w");
    assert(fp);
    assert(fputs("id,name\n", fp) >= 0);
    for (unsigned i = 0; i < 17; ++i) {
        assert(DSD_FPRINTF(fp, "%u,Unit\n", i) > 0);
    }
    assert(fclose(fp) == 0);
    dsd_csv_validation counts = {9, 9, 9};
    /* Store and first 16 entries succeed; growing for row 17 fails. */
    dsd_source_alias_test_alloc_fail_after(2);
    assert(dsd_csv_validate_src_file(path, &counts) == -1);
    assert(counts.accepted == 0 && counts.skipped == 0 && counts.total == 0);
    dsd_source_alias_test_alloc_reset();
}

int
main(void) {
    test_failures();
    test_validator_discards_partial_counts_on_allocation_failure();
    nul_cases();
    boundary(998, "", 0, 1, 0);
    boundary(998, "\n", 0, 1, 0);
    boundary(998, "\r\n", 0, 1, 0);
    boundary(997, "\r\n", 0, 1, 0);
    boundary(999, "\n1203,Next\n", 0, 1, 1);
    boundary(998, "1201,Name\n1203,Next\n", 0, 1, 1);
    boundary(999, "\n", 1, 0, 0);
    boundary(998, "\r1202,Injected\n1203,Next\n", 0, 1, 1);
    boundary(998, "\r", 0, 0, 1);
    assert(remove(path) == 0);
    return 0;
}
