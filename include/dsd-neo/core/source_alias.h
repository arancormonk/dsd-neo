// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Source aliases: CSV grammar `id,name[,tags]`, decimal uint32_t ID or inclusive
 * `start-end` range. See docs/csv-formats.md, "Source ID List CSV".
 */
#ifndef DSD_NEO_CORE_SOURCE_ALIAS_H
#define DSD_NEO_CORE_SOURCE_ALIAS_H
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { DSD_SOURCE_ALIAS_NAME_MAX = 50 };
typedef struct dsd_source_alias_store dsd_source_alias_store;

typedef struct dsd_source_alias_entry {
    uint32_t id_start, id_end;
    uint8_t is_range;
    char name[DSD_SOURCE_ALIAS_NAME_MAX]; /* 49 bytes plus NUL; UTF-8 may be cut. */
    unsigned row;
} dsd_source_alias_entry;

/* Live stores are immutable once installed; access belongs to the decoder thread. */
dsd_source_alias_store* dsd_source_alias_store_create(void);
/* Returns 0 on success, 1 for an invalid entry, -1 on allocation failure. */
int dsd_source_alias_store_append(dsd_source_alias_store* store, const dsd_source_alias_entry* entry);
size_t dsd_source_alias_store_count(const dsd_source_alias_store* store);
/* NULL is accepted. */
void dsd_source_alias_store_free(dsd_source_alias_store* store);
/* Takes ownership, freeing the previous store. NULL removes it. */
void dsd_source_alias_install(dsd_state* state, dsd_source_alias_store* store);
int dsd_source_alias_clear(dsd_state* state);
/* Store presence, including a successfully imported empty list. */
int dsd_source_alias_loaded(const dsd_state* state);
size_t dsd_source_alias_count(const dsd_state* state);
/* Exact beats range, narrowest range wins, first row wins ties. Returns 1 on hit. */
int dsd_source_alias_lookup(const dsd_state* state, uint32_t id, char* name, size_t name_sz);
/* Alias then policy exact label; mode is only ever supplied by policy. */
int dsd_source_label_lookup(const dsd_state* state, uint32_t id, char* mode, size_t mode_sz, char* name,
                            size_t name_sz);
/* Deep copy or reuse matching clone. Failure clears dst without freeing shared src. */
int dsd_source_alias_copy_snapshot(dsd_state* dst, const dsd_state* src);
/* Fresh candidate; on open/read/allocation failure returns -1 and leaves *out untouched.
 * The header is always consumed. Blank lines are ignored, malformed rows skipped,
 * tags/extra columns ignored, and names trimmed and truncated to 49 bytes. Physical
 * lines over 998 content bytes (excluding LF/CRLF), or containing a NUL byte, are drained and
 * skipped once.
 * Zero accepted rows is success and produces a loaded, empty store.
 */
int dsd_source_alias_load(const char* path, dsd_source_alias_store** out);
#ifdef DSD_NEO_TEST_HOOKS
/* Fail after N successful operations; -1 disables the seam. */
void dsd_source_alias_test_alloc_fail_after(long successful_allocations);
void dsd_source_alias_test_alloc_reset(void);
void dsd_source_alias_test_read_fail_after_lines(long successful_lines);
#endif
#ifdef __cplusplus
}
#endif
#endif
