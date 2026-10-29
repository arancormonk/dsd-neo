// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Source aliases (the radio ID list): CSV grammar `id,name[,tags]`, or with policy columns
 * `id,name,mode,audio,record,stream,tags`. The id is a decimal uint32_t ID, an inclusive `start-end` range or a
 * digit pattern (<dsd-neo/core/id_pattern.h>). See docs/csv-formats.md, "Source ID List CSV".
 */
#ifndef DSD_NEO_CORE_SOURCE_ALIAS_H
#define DSD_NEO_CORE_SOURCE_ALIAS_H
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/id_pattern.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { DSD_SOURCE_ALIAS_NAME_MAX = 50 };
typedef struct dsd_source_alias_store dsd_source_alias_store;

/* One row. kind takes the group list's dsd_tg_policy_row_kind values: EXACT (id_start == id_end), RANGE
 * (id_start <= id_end) or PATTERN (id_start = id_end = 0, the pattern in `pattern`; the store keeps its canonical
 * text). A row without policy needs a name; a policy row (has_policy) may be unnamed and then decides policy only.
 * Policy fields are taken as given and normalised on append: `B`/`DE` mode forces audio, record and stream off, and
 * audio off forces record and stream off. A row without policy has its policy fields cleared. */
typedef struct dsd_source_alias_entry {
    uint32_t id_start, id_end;
    uint8_t kind;                          /* dsd_tg_policy_row_kind */
    char pattern[DSD_ID_PATTERN_TEXT_MAX]; /* canonical digit pattern when kind is PATTERN, else empty */
    char name[DSD_SOURCE_ALIAS_NAME_MAX];  /* 49 bytes plus NUL; UTF-8 may be cut. */
    uint8_t has_policy;                    /* the row came from a file with policy columns */
    char mode[8];                          /* `A`/empty: no restriction; `B`/`DE`: blocked */
    uint8_t audio, record, stream;
} dsd_source_alias_entry;

/* The radio list's policy row for one radio ID (dsd_source_policy_lookup()). */
typedef struct {
    int matched; /* 1 when the winning row exists and has policy */
    dsd_source_alias_entry entry;
} dsd_source_policy_match;

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
/* The identity of the installed store (unique per store, never 0), or 0 when none is installed. */
uint64_t dsd_source_alias_store_id(const dsd_state* state);
/* Whether the installed store holds any policy row (a file with policy columns). */
int dsd_source_policy_loaded(const dsd_state* state);
/* Row precedence, for names and policy alike: exact beats range beats pattern; the first exact row wins; the
 * narrowest range wins, the last row winning ties; the most specific pattern wins (more literal digits, then one
 * without `*`), the last row winning ties.
 * The name is the winning row among those with a non-empty name. Returns 1 on hit. */
int dsd_source_alias_lookup(const dsd_state* state, uint32_t id, char* name, size_t name_sz);
/* Policy of the winning row among all rows, named or not. out->matched is 0 (and the function returns 0) when the
 * store has no policy rows, no row matches, or the winning row has no policy. out may be NULL. */
int dsd_source_policy_lookup(const dsd_state* state, uint32_t id, dsd_source_policy_match* out);
/* Name: the radio list's name, else the group list's exact label. Mode: the winning radio row's mode when that row
 * has policy and a non-empty mode, else the group list's exact mode. Returns 1 only with a non-empty name; with no
 * name it returns 0 and leaves both outputs empty, so an unnamed row's mode is never shown as a label. */
int dsd_source_label_lookup(const dsd_state* state, uint32_t id, char* mode, size_t mode_sz, char* name,
                            size_t name_sz);
/* The label a call's target is named by (issue #773 D4): a group call's talkgroup by the row that decides it
 * (dsd_tg_policy_lookup_group_label(); a learned alias never labels a talkgroup), a private call's target as a radio
 * (dsd_source_label_lookup()), a call of any other kind (data, voice of unknown kind) by the group list's exact row.
 * Returns 1 only with a non-empty name; with no name it returns 0 and leaves both outputs empty. */
int dsd_call_target_label_lookup(const dsd_state* state, dsd_call_kind kind, uint32_t id, char* mode, size_t mode_sz,
                                 char* name, size_t name_sz);
/* Deep copy or reuse matching clone. Failure preserves an owned destination store;
 * a shallow alias of src is detached so destination cleanup cannot free src. */
int dsd_source_alias_copy_snapshot(dsd_state* dst, const dsd_state* src);
/* Fresh candidate; on open/read/allocation failure returns -1 and leaves *out untouched.
 * The header is always consumed. When its third column is `mode` (case-insensitive, trimmed) the file has policy
 * columns: the ordered prefix `mode,audio,record,stream,tags` from column 3, read up to the first header column
 * that breaks that order; its rows may have an empty name. Blank lines are ignored, malformed rows skipped,
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
