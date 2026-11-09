// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#ifndef DSD_NEO_SRC_CORE_UTIL_TALKGROUP_POLICY_INTERNAL_H_
#define DSD_NEO_SRC_CORE_UTIL_TALKGROUP_POLICY_INTERNAL_H_

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <stdint.h>

/* Standalone CSV imports need only a policy store, not a decoder state. The created
 * reference is released with dsd_tg_policy_release(). Append has the public exact/range
 * insertion return convention: 0 stored, 1 invalid, -1 allocation failure. */
dsd_tg_policy_store* dsd_tg_policy_store_create(void);
int dsd_tg_policy_store_append(dsd_tg_policy_store* store, const dsd_tg_policy_entry* entry);
/* Mark a list loaded incomplete (dsd_tg_policy_load_report_incomplete()): a detached store, or the state's effective
 * one, created empty when the load stored no row at all. The mark lasts as long as the store; only a complete load
 * replaces it. The state form returns -1 when no store could be created to carry the mark, else 0. */
void dsd_tg_policy_store_mark_incomplete(dsd_tg_policy_store* store);
int dsd_tg_policy_mark_incomplete(dsd_state* state);
/* The state's effective store, created empty in place (as a decoder write creates it) when there is none; NULL only
 * when it cannot be allocated. No reference is taken. */
dsd_tg_policy_store* dsd_tg_policy_ensure(dsd_state* state);

/* A group file's rows hash: 64-bit FNV-1a over the rows the importer accepts from it, in file order, each in the
 * canonical form a store keeps it (kind, bounds or pattern, mode, name, tags, priority, preempt, media). Line endings,
 * unmodelled columns and malformed rows do not enter it. A list's baseline is that hash for the file it was last read
 * from or written to, so a rewrite can tell whether the file still holds what dsd-neo last saw there. */
#define DSD_TG_POLICY_ROWS_HASH_INIT 0xcbf29ce484222325ULL
/* Fold one row into @p hash as a store would keep it: 0 folded, 1 a row a store refuses (nothing folded). */
int dsd_tg_policy_rows_hash_fold(uint64_t* hash, const dsd_tg_policy_entry* entry);
/* Record that @p path held the rows @p rows_hash describes: on a detached store, or on the state's store (created when
 * missing; -1 when it cannot be). */
void dsd_tg_policy_store_set_baseline(dsd_tg_policy_store* store, const char* path, uint64_t rows_hash);
int dsd_tg_policy_set_baseline(dsd_state* state, const char* path, uint64_t rows_hash);

/* The importer's side (dsd_import.c). A group file's first line as the importer reads it, parsed once for the
 * lines below it. */
typedef struct {
    int policy_active;
    unsigned int prefix_len;
    int invalid_order;
    int basic_tags;
} dsd_tg_policy_group_header;

void dsd_tg_policy_group_header_parse(const char* header_line, dsd_tg_policy_group_header* out);
/* Fold into @p hash the row the importer would store from data line @p line (trimmed in place; no row warning) under
 * @p header. Returns 1 folded, 0 the importer would skip the line. */
int dsd_tg_policy_rows_hash_line(uint64_t* hash, const dsd_tg_policy_group_header* header, char* line);
/* Set @p hash to the rows hash of the file at @p path, read as a load reads it, without storing a row. Returns 0 read
 * to the end, -1 not (@p report, which may be NULL, says why: not_found or unreadable). */
int dsd_tg_policy_rows_hash_file(const char* path, uint64_t* hash, dsd_tg_policy_load_report* report);

/* Mode rules shared by the group list and the radio list's policy columns. Mode `B` or `DE` blocks: no tuning and
 * every medium off. */
int dsd_tg_policy_mode_blocks(const char* mode);
/* Make media flags consistent with the mode: each flag 0/1, a blocking mode forces all three off, and audio off forces
 * record and stream off. Does nothing when a flag pointer is NULL. */
void dsd_tg_policy_media_normalize(const char* mode, uint8_t* audio, uint8_t* record, uint8_t* stream);

#endif
