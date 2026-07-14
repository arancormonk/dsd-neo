// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <ctype.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

typedef struct {
    dsd_tg_policy_entry* entries;
    size_t count;
    size_t capacity;
    unsigned int generation;
} dsd_tg_policy_table;

const char*
dsd_tg_policy_block_reason_label(uint32_t block_reasons) {
    if (block_reasons & DSD_TG_POLICY_BLOCK_HOLD) {
        return "hold";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED) {
        return "private-disabled";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_GROUP_DISABLED) {
        return "group-disabled";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_DATA_DISABLED) {
        return "data-disabled";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED) {
        return "enc-disabled";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_ALLOWLIST) {
        return "allowlist";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_MODE) {
        return "mode";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_AUDIO) {
        return "audio";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_RECORD) {
        return "record";
    }
    if (block_reasons & DSD_TG_POLICY_BLOCK_STREAM) {
        return "stream";
    }
    return "policy";
}

typedef struct {
    uint8_t valid;
    uint32_t tg;
    uint32_t src;
    long freq_hz;
    int channel;
    int priority;
    int slot;
    double start_mono_s;
    double last_seen_mono_s;
    double last_preempt_mono_s;
} dsd_tg_policy_active_call;

typedef struct {
    dsd_tg_policy_active_call calls[2];
    double last_global_preempt_mono_s;
    unsigned int generation;
} dsd_tg_policy_active_table;

typedef struct {
    dsd_tg_policy_table table;
    dsd_tg_policy_active_table active;
    uint64_t context_id;
    uint64_t snapshot_source_context_id;
} dsd_tg_policy_context;

#ifdef DSD_NEO_TEST_HOOKS
static long s_alloc_fail_after = -1;
static long s_alloc_calls = 0;
void dsd_tg_policy_test_alloc_reset(void);
void dsd_tg_policy_test_alloc_fail_after(long fail_after);
#endif
static dsd_atomic_u64 s_next_context_id = {1u};

#ifdef DSD_NEO_TEST_HOOKS
static int
tg_policy_alloc_should_fail(void) {
    if (s_alloc_fail_after < 0) {
        return 0;
    }
    return s_alloc_calls >= s_alloc_fail_after;
}
#endif

static void* DSD_ATTR_USED
tg_policy_calloc(size_t n, size_t size) {
#ifdef DSD_NEO_TEST_HOOKS
    if (tg_policy_alloc_should_fail()) {
        return NULL;
    }
    s_alloc_calls++;
#endif
    return calloc(n, size);
}

static void* DSD_ATTR_USED
tg_policy_realloc(void* ptr, size_t size) {
#ifdef DSD_NEO_TEST_HOOKS
    if (tg_policy_alloc_should_fail()) {
        return NULL;
    }
    s_alloc_calls++;
#endif
    return realloc(ptr, size);
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_tg_policy_test_alloc_reset(void) {
    s_alloc_calls = 0;
    s_alloc_fail_after = -1;
}

void
dsd_tg_policy_test_alloc_fail_after(long fail_after) {
    s_alloc_calls = 0;
    s_alloc_fail_after = fail_after;
}
#endif

static int
tg_policy_is_ascii_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static char*
tg_policy_trim_inplace(char* s) {
    size_t len = 0;
    size_t start = 0;
    if (!s) {
        return NULL;
    }
    len = strlen(s);
    while (start < len && tg_policy_is_ascii_space((unsigned char)s[start])) {
        start++;
    }
    while (len > start && tg_policy_is_ascii_space((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
    return s + start;
}

static void
tg_policy_safe_copy(char* dst, size_t dst_sz, const char* src) {
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    DSD_SNPRINTF(dst, dst_sz, "%s", src);
}

static int
tg_policy_mode_is_blocking(const char* mode) {
    if (!mode) {
        return 0;
    }
    return (strcmp(mode, "B") == 0 || strcmp(mode, "DE") == 0);
}

static void
tg_policy_defaults_from_mode(const char* mode, uint8_t* audio, uint8_t* record, uint8_t* stream) {
    uint8_t on = tg_policy_mode_is_blocking(mode) ? 0u : 1u;
    if (audio) {
        *audio = on;
    }
    if (record) {
        *record = on;
    }
    if (stream) {
        *stream = on;
    }
}

static void
tg_policy_normalize_entry(dsd_tg_policy_entry* e) {
    if (!e) {
        return;
    }
    e->preempt = (e->preempt != 0u) ? 1u : 0u;
    e->audio = (e->audio != 0u) ? 1u : 0u;
    e->record = (e->record != 0u) ? 1u : 0u;
    e->stream = (e->stream != 0u) ? 1u : 0u;
    if (e->priority < 0) {
        e->priority = 0;
    } else if (e->priority > 100) {
        e->priority = 100;
    }
    if (tg_policy_mode_is_blocking(e->mode)) {
        e->audio = 0;
        e->record = 0;
        e->stream = 0;
    } else if (!e->audio) {
        e->record = 0;
        e->stream = 0;
    }
}

static void
tg_policy_copy_entry_normalized(dsd_tg_policy_entry* dst, const dsd_tg_policy_entry* src) {
    if (!dst || !src) {
        return;
    }
    DSD_MEMSET(dst, 0, sizeof(*dst));
    dst->id_start = src->id_start;
    dst->id_end = src->id_end;
    tg_policy_safe_copy(dst->mode, sizeof(dst->mode), src->mode);
    tg_policy_safe_copy(dst->name, sizeof(dst->name), src->name);
    dst->priority = src->priority;
    dst->preempt = src->preempt;
    dst->audio = src->audio;
    dst->record = src->record;
    dst->stream = src->stream;
    dst->is_range = src->is_range ? 1u : 0u;
    dst->source = src->source;
    dst->row = src->row;
    tg_policy_normalize_entry(dst);
}

static int
tg_policy_entry_valid_exact(const dsd_tg_policy_entry* e) {
    if (!e) {
        return 0;
    }
    if (e->is_range != 0u) {
        return 0;
    }
    return e->id_start == e->id_end;
}

static int
tg_policy_entry_valid_range(const dsd_tg_policy_entry* e) {
    if (!e) {
        return 0;
    }
    if (e->is_range == 0u) {
        return 0;
    }
    if (e->id_start > e->id_end) {
        return 0;
    }
    return e->id_start != e->id_end;
}

static uint64_t
tg_policy_next_context_id(void) {
    uint64_t id = 0u;
    do {
        id = dsd_atomic_u64_fetch_add_relaxed(&s_next_context_id, 1u);
    } while (id == 0u);
    return id;
}

static dsd_tg_policy_context*
tg_policy_context_alloc(void) {
    dsd_tg_policy_context* ctx = (dsd_tg_policy_context*)tg_policy_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->context_id = tg_policy_next_context_id();
    ctx->snapshot_source_context_id = ctx->context_id;
    return ctx;
}

static void
tg_policy_context_free(void* ptr) {
    dsd_tg_policy_context* ctx = (dsd_tg_policy_context*)ptr;
    if (!ctx) {
        return;
    }
    free(ctx->table.entries);
    ctx->table.entries = NULL;
    ctx->table.count = 0;
    ctx->table.capacity = 0;
    free(ctx);
}

static dsd_tg_policy_context* DSD_ATTR_USED
tg_policy_ctx_get_mut(dsd_state* state, int create_if_missing) {
    dsd_tg_policy_context* ctx = NULL;
    if (!state) {
        return NULL;
    }

    ctx = (dsd_tg_policy_context*)dsd_state_ext_get(state, DSD_STATE_EXT_CORE_TG_POLICY);
    if (ctx || !create_if_missing) {
        return ctx;
    }

    ctx = tg_policy_context_alloc();
    if (!ctx) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_CORE_TG_POLICY, ctx, tg_policy_context_free) != 0) {
        tg_policy_context_free(ctx);
        return NULL;
    }
    return ctx;
}

static const dsd_tg_policy_context* DSD_ATTR_USED
tg_policy_ctx_get_const(const dsd_state* state) {
    if (!state) {
        return NULL;
    }
    return (const dsd_tg_policy_context*)dsd_state_ext_get_const(state, DSD_STATE_EXT_CORE_TG_POLICY);
}

static int
tg_policy_table_reserve(dsd_tg_policy_context* ctx, size_t needed) {
    size_t target = 0;
    dsd_tg_policy_entry* next = NULL;
    if (!ctx) {
        return -1;
    }
    if (ctx->table.capacity >= needed) {
        return 0;
    }

    target = (ctx->table.capacity > 0) ? ctx->table.capacity : 16u;
    while (target < needed) {
        target *= 2u;
    }

    next = (dsd_tg_policy_entry*)tg_policy_realloc(ctx->table.entries, target * sizeof(*next));
    if (!next) {
        return -1;
    }
    ctx->table.entries = next;
    ctx->table.capacity = target;
    return 0;
}

static int
tg_policy_find_policy_exact_idx_first(const dsd_tg_policy_context* ctx, uint32_t id) {
    if (!ctx) {
        return -1;
    }
    for (size_t i = 0; i < ctx->table.count; i++) {
        const dsd_tg_policy_entry* e = &ctx->table.entries[i];
        if (e->is_range == 0u && e->id_start == id && e->id_end == id) {
            return (int)i;
        }
    }
    return -1;
}

static int DSD_ATTR_USED
tg_policy_lookup_exact_only(const dsd_state* state, uint32_t id, dsd_tg_policy_entry* out) {
    dsd_tg_policy_lookup lookup;
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (dsd_tg_policy_lookup_id(state, id, &lookup) != 0) {
        return 0;
    }
    if (lookup.match != DSD_TG_POLICY_MATCH_EXACT) {
        return 0;
    }
    *out = lookup.entry;
    return 1;
}

static void
tg_policy_table_note_mutation(dsd_tg_policy_context* ctx) {
    if (!ctx) {
        return;
    }
    ctx->table.generation++;
    if (ctx->table.generation == 0u) {
        ctx->table.generation = 1u;
    }
}

int
dsd_tg_policy_make_exact_entry(uint32_t id, const char* mode, const char* name, dsd_tg_policy_entry_source source,
                               dsd_tg_policy_entry* out) {
    if (!out || !mode || !name) {
        return 1;
    }

    DSD_MEMSET(out, 0, sizeof(*out));
    out->id_start = id;
    out->id_end = id;
    tg_policy_safe_copy(out->mode, sizeof(out->mode), mode);
    tg_policy_safe_copy(out->name, sizeof(out->name), name);
    out->priority = 0;
    out->preempt = 0;
    tg_policy_defaults_from_mode(out->mode, &out->audio, &out->record, &out->stream);
    out->is_range = 0;
    out->source = (uint8_t)source;
    out->row = 0;
    return 0;
}

int
dsd_tg_policy_add_range_entry(dsd_state* state, const dsd_tg_policy_entry* entry) {
    dsd_tg_policy_context* ctx = NULL;
    dsd_tg_policy_entry normalized;
    if (!state || !entry) {
        return 1;
    }
    if (!tg_policy_entry_valid_range(entry)) {
        return 1;
    }

    tg_policy_copy_entry_normalized(&normalized, entry);

    ctx = tg_policy_ctx_get_mut(state, 1);
    if (!ctx) {
        return -1;
    }
    if (tg_policy_table_reserve(ctx, ctx->table.count + 1) != 0) {
        return -1;
    }

    ctx->table.entries[ctx->table.count++] = normalized;
    tg_policy_table_note_mutation(ctx);
    return 0;
}

int
dsd_tg_policy_append_exact(dsd_state* state, const dsd_tg_policy_entry* entry) {
    dsd_tg_policy_context* ctx = NULL;
    dsd_tg_policy_entry normalized;
    if (!state || !entry) {
        return 1;
    }
    if (!tg_policy_entry_valid_exact(entry)) {
        return 1;
    }

    tg_policy_copy_entry_normalized(&normalized, entry);
    normalized.is_range = 0;
    normalized.id_end = normalized.id_start;

    ctx = tg_policy_ctx_get_mut(state, 1);
    if (!ctx) {
        return -1;
    }
    if (tg_policy_table_reserve(ctx, ctx->table.count + 1) != 0) {
        return -1;
    }

    ctx->table.entries[ctx->table.count++] = normalized;
    tg_policy_table_note_mutation(ctx);
    return 0;
}

int
dsd_tg_policy_upsert_exact(dsd_state* state, const dsd_tg_policy_entry* entry, dsd_tg_policy_upsert_mode mode) {
    dsd_tg_policy_context* ctx = NULL;
    dsd_tg_policy_entry normalized;
    int policy_idx = -1;

    if (!state || !entry) {
        return 1;
    }
    if (!tg_policy_entry_valid_exact(entry)) {
        return 1;
    }
    tg_policy_copy_entry_normalized(&normalized, entry);
    normalized.is_range = 0;
    normalized.id_end = normalized.id_start;

    ctx = tg_policy_ctx_get_mut(state, mode != DSD_TG_POLICY_UPSERT_ADD_IF_MISSING);
    policy_idx = tg_policy_find_policy_exact_idx_first(ctx, normalized.id_start);

    if (mode == DSD_TG_POLICY_UPSERT_ADD_IF_MISSING) {
        if (policy_idx >= 0) {
            return 0;
        }
        return dsd_tg_policy_append_exact(state, &normalized);
    }

    if (!ctx) {
        return -1;
    }

    if (mode == DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY) {
        if (policy_idx < 0) {
            return 0;
        }
        if (ctx->table.entries[policy_idx].source != DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS) {
            return 0;
        }
        ctx->table.entries[policy_idx] = normalized;
        tg_policy_table_note_mutation(ctx);
        return 0;
    }

    if (policy_idx < 0) {
        return dsd_tg_policy_append_exact(state, &normalized);
    }
    ctx->table.entries[policy_idx] = normalized;
    tg_policy_table_note_mutation(ctx);
    return 0;
}

static int
tg_policy_lookup_exact_in_ctx(const dsd_tg_policy_context* ctx, uint32_t id, dsd_tg_policy_lookup* out) {
    if (!ctx || !out) {
        return 0;
    }
    for (size_t i = 0; i < ctx->table.count; i++) {
        const dsd_tg_policy_entry* e = &ctx->table.entries[i];
        if (e->is_range == 0u && e->id_start == id && e->id_end == id) {
            out->match = DSD_TG_POLICY_MATCH_EXACT;
            out->entry = *e;
            return 1;
        }
    }
    return 0;
}

static int
tg_policy_find_best_range_idx(const dsd_tg_policy_context* ctx, uint32_t id) {
    int best_idx = -1;
    uint64_t best_span = UINT64_MAX;

    if (!ctx) {
        return -1;
    }

    for (size_t i = 0; i < ctx->table.count; i++) {
        const dsd_tg_policy_entry* e = &ctx->table.entries[i];
        uint64_t span = 0;
        if (e->is_range == 0u) {
            continue;
        }
        if (id < e->id_start || id > e->id_end) {
            continue;
        }
        span = (uint64_t)e->id_end - (uint64_t)e->id_start;
        if (best_idx < 0 || span < best_span || (span == best_span && (int)i > best_idx)) {
            best_idx = (int)i;
            best_span = span;
        }
    }

    return best_idx;
}

int
dsd_tg_policy_lookup_id(const dsd_state* state, uint32_t id, dsd_tg_policy_lookup* out) {
    const dsd_tg_policy_context* ctx = NULL;
    int best_idx = -1;
    if (!out) {
        return -1;
    }

    DSD_MEMSET(out, 0, sizeof(*out));
    out->match = DSD_TG_POLICY_MATCH_NONE;

    if (!state) {
        return 0;
    }

    ctx = tg_policy_ctx_get_const(state);
    if (!ctx) {
        return 0;
    }

    if (tg_policy_lookup_exact_in_ctx(ctx, id, out)) {
        return 0;
    }

    best_idx = tg_policy_find_best_range_idx(ctx, id);
    if (best_idx >= 0) {
        out->match = DSD_TG_POLICY_MATCH_RANGE;
        out->entry = ctx->table.entries[best_idx];
    }
    return 0;
}

int
dsd_tg_policy_has_entries(const dsd_state* state) {
    const dsd_tg_policy_context* ctx = NULL;
    if (!state) {
        return 0;
    }
    ctx = tg_policy_ctx_get_const(state);
    if (ctx && ctx->table.count > 0) {
        return 1;
    }
    return 0;
}

int
dsd_tg_policy_lookup_label(const dsd_state* state, uint32_t id, char* mode, size_t mode_sz, char* name,
                           size_t name_sz) {
    dsd_tg_policy_lookup lookup;
    if (mode && mode_sz > 0) {
        mode[0] = '\0';
    }
    if (name && name_sz > 0) {
        name[0] = '\0';
    }
    if (dsd_tg_policy_lookup_id(state, id, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT) {
        return 0;
    }
    tg_policy_safe_copy(mode, mode_sz, lookup.entry.mode);
    tg_policy_safe_copy(name, name_sz, lookup.entry.name);
    return 1;
}

static int
tg_policy_context_clone(const dsd_tg_policy_context* src, dsd_tg_policy_context** out) {
    dsd_tg_policy_context* clone = NULL;
    if (!out) {
        return -1;
    }
    *out = NULL;
    if (!src) {
        return 0;
    }
    clone = tg_policy_context_alloc();
    if (!clone) {
        return -1;
    }
    clone->table.count = src->table.count;
    clone->table.capacity = src->table.count;
    clone->table.generation = src->table.generation;
    clone->active = src->active;
    clone->snapshot_source_context_id = src->context_id;
    if (src->table.count > 0) {
        clone->table.entries = (dsd_tg_policy_entry*)tg_policy_calloc(src->table.count, sizeof(*clone->table.entries));
        if (!clone->table.entries) {
            tg_policy_context_free(clone);
            return -1;
        }
        DSD_MEMCPY(clone->table.entries, src->table.entries, src->table.count * sizeof(*clone->table.entries));
    }
    *out = clone;
    return 0;
}

int
dsd_tg_policy_copy_snapshot(dsd_state* dst, const dsd_state* src) {
    const dsd_tg_policy_context* src_ctx = NULL;
    dsd_tg_policy_context* dst_ctx = NULL;
    dsd_tg_policy_context* clone = NULL;
    if (!dst || !src) {
        return -1;
    }
    if (dst == src) {
        return 0;
    }

    src_ctx = tg_policy_ctx_get_const(src);
    if (!src_ctx) {
        return dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_TG_POLICY, NULL, NULL);
    }

    dst_ctx = tg_policy_ctx_get_mut(dst, 0);
    if (dst_ctx && dst_ctx != src_ctx && src_ctx->context_id != 0u && dst_ctx->snapshot_source_context_id != 0u
        && dst_ctx->snapshot_source_context_id == src_ctx->context_id
        && dst_ctx->table.generation == src_ctx->table.generation && dst_ctx->table.count == src_ctx->table.count) {
        dst_ctx->active = src_ctx->active;
        return 0;
    }

    if (tg_policy_context_clone(src_ctx, &clone) != 0) {
        return -1;
    }
    if (dst_ctx == src_ctx) {
        dst->state_ext[DSD_STATE_EXT_CORE_TG_POLICY] = NULL;
        dst->state_ext_cleanup[DSD_STATE_EXT_CORE_TG_POLICY] = NULL;
    }
    if (dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_TG_POLICY, clone, tg_policy_context_free) != 0) {
        tg_policy_context_free(clone);
        return -1;
    }
    return 0;
}

static void
tg_policy_init_decision(dsd_tg_policy_decision* out, uint32_t target_id, uint32_t source_id, int encrypted,
                        int data_call) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->target_id = target_id;
    out->source_id = source_id;
    out->encrypted = encrypted ? 1 : 0;
    out->data_call = data_call ? 1 : 0;
    out->tune_allowed = 1;
    out->audio_allowed = 1;
    out->record_allowed = 1;
    out->stream_allowed = 1;
    out->priority = 0;
    out->preempt_requested = 0;
    out->block_reasons = DSD_TG_POLICY_BLOCK_NONE;
    out->match = DSD_TG_POLICY_MATCH_NONE;
}

static void
tg_policy_decision_apply_entry(dsd_tg_policy_decision* out, const dsd_tg_policy_entry* entry,
                               dsd_tg_policy_match_type match) {
    if (!out || !entry) {
        return;
    }
    out->match = match;
    out->priority = entry->priority;
    out->preempt_requested = entry->preempt ? 1 : 0;
    out->audio_allowed = entry->audio ? 1 : 0;
    out->record_allowed = entry->record ? 1 : 0;
    out->stream_allowed = entry->stream ? 1 : 0;
    tg_policy_safe_copy(out->mode, sizeof(out->mode), entry->mode);
    tg_policy_safe_copy(out->name, sizeof(out->name), entry->name);
}

static void
tg_policy_block_decision_tune_and_media(dsd_tg_policy_decision* out, dsd_tg_policy_block_reason reason) {
    if (!out) {
        return;
    }
    out->tune_allowed = 0;
    out->audio_allowed = 0;
    out->record_allowed = 0;
    out->stream_allowed = 0;
    out->block_reasons |= (uint32_t)reason;
}

static void
tg_policy_apply_explicit_media_blocks(dsd_tg_policy_decision* out, int explicit_audio_block, int explicit_record_block,
                                      int explicit_stream_block) {
    if (!out) {
        return;
    }
    if (explicit_audio_block) {
        out->audio_allowed = 0;
        out->record_allowed = 0;
        out->stream_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_AUDIO;
        return;
    }
    if (explicit_record_block) {
        out->record_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_RECORD;
    }
    if (explicit_stream_block) {
        out->stream_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_STREAM;
    }
}

static void DSD_ATTR_USED
tg_policy_group_apply_lookup(const dsd_state* state, uint32_t tg, dsd_tg_policy_decision* out, int* mode_blocking,
                             int* explicit_audio_block, int* explicit_record_block, int* explicit_stream_block) {
    dsd_tg_policy_lookup lookup;
    int local_mode_blocking = 0;
    int local_audio_block = 0;
    int local_record_block = 0;
    int local_stream_block = 0;

    if (dsd_tg_policy_lookup_id(state, tg, &lookup) == 0 && lookup.match != DSD_TG_POLICY_MATCH_NONE) {
        tg_policy_decision_apply_entry(out, &lookup.entry, lookup.match);
        local_mode_blocking = tg_policy_mode_is_blocking(lookup.entry.mode);
        local_audio_block = (!local_mode_blocking && lookup.entry.audio == 0u);
        local_record_block = (!local_mode_blocking && lookup.entry.audio != 0u && lookup.entry.record == 0u);
        local_stream_block = (!local_mode_blocking && lookup.entry.audio != 0u && lookup.entry.stream == 0u);
    }

    if (mode_blocking) {
        *mode_blocking = local_mode_blocking;
    }
    if (explicit_audio_block) {
        *explicit_audio_block = local_audio_block;
    }
    if (explicit_record_block) {
        *explicit_record_block = local_record_block;
    }
    if (explicit_stream_block) {
        *explicit_stream_block = local_stream_block;
    }
}

static void DSD_ATTR_USED
tg_policy_group_set_hold_state(const dsd_state* state, uint32_t tg, dsd_tg_policy_decision* out, int* hold_mismatch) {
    int local_hold_mismatch = 0;

    if (!out) {
        if (hold_mismatch) {
            *hold_mismatch = 0;
        }
        return;
    }

    out->tg_hold_active = (state && state->tg_hold != 0) ? 1 : 0;
    out->tg_hold_match = (out->tg_hold_active && state->tg_hold == tg) ? 1 : 0;
    local_hold_mismatch = out->tg_hold_active && !out->tg_hold_match;

    if (hold_mismatch) {
        *hold_mismatch = local_hold_mismatch;
    }
}

static void DSD_ATTR_USED
tg_policy_private_set_hold_state(const dsd_state* state, uint32_t src, uint32_t dst, dsd_tg_policy_decision* out,
                                 int* hold_mismatch) {
    int local_hold_mismatch = 0;

    if (!out) {
        if (hold_mismatch) {
            *hold_mismatch = 0;
        }
        return;
    }

    out->tg_hold_active = (state && state->tg_hold != 0) ? 1 : 0;
    out->tg_hold_match = (out->tg_hold_active && (state->tg_hold == src || state->tg_hold == dst)) ? 1 : 0;
    local_hold_mismatch = out->tg_hold_active && !out->tg_hold_match;

    if (hold_mismatch) {
        *hold_mismatch = local_hold_mismatch;
    }
}

static void DSD_ATTR_USED
tg_policy_apply_group_tune_blocks(const dsd_opts* opts, int encrypted, int data_call, dsd_tg_policy_decision* out) {
    if (!out) {
        return;
    }
    if (opts && opts->trunk_tune_group_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_GROUP_DISABLED;
    }
    if (data_call && opts && opts->trunk_tune_data_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_DATA_DISABLED;
    }
    if (encrypted && opts && opts->trunk_tune_enc_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED;
    }
    if (opts && opts->trunk_use_allow_list == 1 && out->match == DSD_TG_POLICY_MATCH_NONE) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_ALLOWLIST;
    }
}

static void DSD_ATTR_USED
tg_policy_apply_private_tune_blocks(const dsd_opts* opts, int encrypted, int data_call, int src_match, int dst_match,
                                    int allow_unlisted, dsd_tg_policy_decision* out) {
    if (!out) {
        return;
    }
    if (opts && opts->trunk_tune_private_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED;
    }
    if (data_call && opts && opts->trunk_tune_data_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_DATA_DISABLED;
    }
    if (encrypted && opts && opts->trunk_tune_enc_calls == 0) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED;
    }
    if (opts && opts->trunk_use_allow_list == 1 && !allow_unlisted && !src_match && !dst_match) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_ALLOWLIST;
    }
}

static const dsd_tg_policy_entry*
tg_policy_choose_private_entry(const dsd_tg_policy_entry* src_entry, int src_match,
                               const dsd_tg_policy_entry* dst_entry, int dst_match) {
    if (dst_match) {
        return dst_entry;
    }
    if (src_match) {
        return src_entry;
    }
    return NULL;
}

static int
tg_policy_private_mode_is_blocking(int src_match, const dsd_tg_policy_entry* src_entry, int dst_match,
                                   const dsd_tg_policy_entry* dst_entry) {
    return (src_match && tg_policy_mode_is_blocking(src_entry->mode))
           || (dst_match && tg_policy_mode_is_blocking(dst_entry->mode));
}

int
dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                  int encrypted, int data_call, dsd_tg_policy_decision* out) {
    int mode_blocking = 0;
    int explicit_audio_block = 0;
    int explicit_record_block = 0;
    int explicit_stream_block = 0;
    int hold_mismatch = 0;

    if (!out) {
        return -1;
    }
    tg_policy_init_decision(out, tg, src, encrypted, data_call);

    tg_policy_group_apply_lookup(state, tg, out, &mode_blocking, &explicit_audio_block, &explicit_record_block,
                                 &explicit_stream_block);
    tg_policy_group_set_hold_state(state, tg, out, &hold_mismatch);
    tg_policy_apply_group_tune_blocks(opts, encrypted, data_call, out);

    if (mode_blocking) {
        tg_policy_block_decision_tune_and_media(out, DSD_TG_POLICY_BLOCK_MODE);
    }
    if (hold_mismatch) {
        tg_policy_block_decision_tune_and_media(out, DSD_TG_POLICY_BLOCK_HOLD);
    }

    if (!mode_blocking && !hold_mismatch) {
        tg_policy_apply_explicit_media_blocks(out, explicit_audio_block, explicit_record_block, explicit_stream_block);
    }

    return 0;
}

static int
tg_policy_evaluate_private_call(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst, int encrypted,
                                int data_call, int allow_unlisted, dsd_tg_policy_decision* out) {
    dsd_tg_policy_entry src_entry;
    dsd_tg_policy_entry dst_entry;
    const dsd_tg_policy_entry* chosen = NULL;
    int src_match = 0;
    int dst_match = 0;
    int mode_blocking = 0;
    int hold_mismatch = 0;

    if (!out) {
        return -1;
    }
    tg_policy_init_decision(out, dst, src, encrypted, data_call);

    src_match = tg_policy_lookup_exact_only(state, src, &src_entry);
    dst_match = tg_policy_lookup_exact_only(state, dst, &dst_entry);
    chosen = tg_policy_choose_private_entry(&src_entry, src_match, &dst_entry, dst_match);
    if (chosen) {
        tg_policy_decision_apply_entry(out, chosen, DSD_TG_POLICY_MATCH_EXACT);
    }

    mode_blocking = tg_policy_private_mode_is_blocking(src_match, &src_entry, dst_match, &dst_entry);
    tg_policy_private_set_hold_state(state, src, dst, out, &hold_mismatch);
    tg_policy_apply_private_tune_blocks(opts, encrypted, data_call, src_match, dst_match, allow_unlisted, out);
    if (mode_blocking) {
        tg_policy_block_decision_tune_and_media(out, DSD_TG_POLICY_BLOCK_MODE);
    }
    if (hold_mismatch) {
        tg_policy_block_decision_tune_and_media(out, DSD_TG_POLICY_BLOCK_HOLD);
    }
    if (!mode_blocking && !hold_mismatch && chosen) {
        tg_policy_apply_explicit_media_blocks(out, chosen->audio == 0u, chosen->audio != 0u && chosen->record == 0u,
                                              chosen->audio != 0u && chosen->stream == 0u);
    }

    return 0;
}

int
dsd_tg_policy_evaluate_private_call(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                    int encrypted, int data_call, dsd_tg_policy_decision* out) {
    return tg_policy_evaluate_private_call(opts, state, src, dst, encrypted, data_call, 0, out);
}

int
dsd_tg_policy_evaluate_private_grant(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                     int encrypted, int data_call, dsd_tg_policy_decision* out) {
    return tg_policy_evaluate_private_call(opts, state, src, dst, encrypted, data_call, 1, out);
}

static void
tg_policy_csv_sanitize_copy(char* dst, size_t dst_sz, const char* src) {
    size_t wi = 0;
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (size_t i = 0; src[i] != '\0' && wi + 1 < dst_sz; i++) {
        char c = src[i];
        if (c == ',' || c == '\n' || c == '\r') {
            c = ' ';
        }
        dst[wi++] = c;
    }
    dst[wi] = '\0';
}

static size_t
tg_policy_split_csv_preserve_empty(char* line, char** fields, size_t max_fields) {
    size_t count = 0;
    char* p = line;
    if (!line || !fields || max_fields == 0) {
        return 0;
    }
    fields[count++] = p;
    while (*p != '\0') {
        if (*p == ',') {
            *p = '\0';
            if (count < max_fields) {
                fields[count++] = p + 1;
            }
        }
        p++;
    }
    return count;
}

static int
tg_policy_ascii_casecmp(const char* a, const char* b) {
    if (!a || !b) {
        return (a == b) ? 0 : 1;
    }
    while (*a && *b) {
        unsigned char ca = (unsigned char)tolower((unsigned char)*a);
        unsigned char cb = (unsigned char)tolower((unsigned char)*b);
        if (ca != cb) {
            return (int)ca - (int)cb;
        }
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int
tg_policy_header_is_policy(const char* header_line) {
    char buf[512];
    char* fields[16];
    static const char* expected[] = {"preempt", "audio", "record", "stream", "tags"};
    size_t n = 0;
    size_t idx = 0;

    if (!header_line) {
        return 0;
    }
    DSD_SNPRINTF(buf, sizeof(buf), "%s", header_line);
    n = tg_policy_split_csv_preserve_empty(buf, fields, sizeof(fields) / sizeof(fields[0]));
    if (n < 4) {
        return 0;
    }
    if (tg_policy_ascii_casecmp(tg_policy_trim_inplace(fields[3]), "priority") != 0) {
        return 0;
    }
    for (size_t i = 4; i < n && idx < (sizeof(expected) / sizeof(expected[0])); i++) {
        if (tg_policy_ascii_casecmp(tg_policy_trim_inplace(fields[i]), expected[idx]) != 0) {
            break;
        }
        idx++;
    }
    return 1;
}

static void
tg_policy_probe_group_file(const char* path, int* has_existing_header, int* existing_policy_header,
                           int* file_missing_or_empty) {
    char first_line[512];
    FILE* rf = NULL;

    if (has_existing_header) {
        *has_existing_header = 0;
    }
    if (existing_policy_header) {
        *existing_policy_header = 0;
    }
    if (file_missing_or_empty) {
        *file_missing_or_empty = 0;
    }
    if (!path || !file_missing_or_empty) {
        return;
    }

    rf = dsd_fopen_existing_regular_file(path, "r");
    if (!rf) {
        *file_missing_or_empty = 1;
        return;
    }

    if (fgets(first_line, sizeof(first_line), rf) != NULL) {
        if (has_existing_header) {
            *has_existing_header = 1;
        }
        if (existing_policy_header) {
            *existing_policy_header = tg_policy_header_is_policy(first_line);
        }
    } else {
        *file_missing_or_empty = 1;
    }
    fclose(rf);
}

static int
tg_policy_write_group_file_header(FILE* wf, int file_missing_or_empty, int has_existing_header,
                                  const char* clean_meta) {
    if (!wf) {
        return -1;
    }
    if (file_missing_or_empty) {
        if (clean_meta && clean_meta[0] != '\0') {
            if (DSD_FPRINTF(wf, "id,mode,name,metadata\n") < 0) {
                return -1;
            }
        } else {
            if (DSD_FPRINTF(wf, "id,mode,name\n") < 0) {
                return -1;
            }
        }
    } else if (!has_existing_header) {
        if (DSD_FPRINTF(wf, "id,mode,name\n") < 0) {
            return -1;
        }
    }
    return 0;
}

static const char*
tg_policy_bool_true_false(uint8_t v) {
    return v ? "true" : "false";
}

static const char*
tg_policy_bool_on_off(uint8_t v) {
    return v ? "on" : "off";
}

static int
tg_policy_write_group_file_entry(FILE* wf, const dsd_tg_policy_entry* entry, int has_existing_header,
                                 int existing_policy_header, const char* clean_mode, const char* clean_name,
                                 const char* clean_meta) {
    if (!wf || !entry || !clean_mode || !clean_name || !clean_meta) {
        return -1;
    }
    if (has_existing_header && existing_policy_header) {
        if (DSD_FPRINTF(wf, "%u,%s,%s,%d,%s,%s,%s,%s,%s\n", entry->id_start, clean_mode, clean_name, entry->priority,
                        tg_policy_bool_true_false(entry->preempt), tg_policy_bool_on_off(entry->audio),
                        tg_policy_bool_on_off(entry->record), tg_policy_bool_on_off(entry->stream), clean_meta)
            < 0) {
            return -1;
        }
    } else if (clean_meta[0] != '\0') {
        if (DSD_FPRINTF(wf, "%u,%s,%s,%s\n", entry->id_start, clean_mode, clean_name, clean_meta) < 0) {
            return -1;
        }
    } else {
        if (DSD_FPRINTF(wf, "%u,%s,%s\n", entry->id_start, clean_mode, clean_name) < 0) {
            return -1;
        }
    }
    return 0;
}

int
dsd_tg_policy_append_group_file_row(const dsd_opts* opts, const dsd_tg_policy_entry* entry, const char* metadata) {
    char clean_mode[16];
    char clean_name[128];
    char clean_meta[256];
    int has_existing_header = 0;
    int existing_policy_header = 0;
    int file_missing_or_empty = 0;
    FILE* wf = NULL;
    const char* path = NULL;

    if (!opts || !entry) {
        return 0;
    }
    path = opts->group_in_file;
    if (!path || path[0] == '\0') {
        return 0;
    }
    if (!tg_policy_entry_valid_exact(entry)) {
        return 1;
    }

    tg_policy_csv_sanitize_copy(clean_mode, sizeof(clean_mode), entry->mode);
    tg_policy_csv_sanitize_copy(clean_name, sizeof(clean_name), entry->name);
    tg_policy_csv_sanitize_copy(clean_meta, sizeof(clean_meta), metadata ? metadata : "");

    tg_policy_probe_group_file(path, &has_existing_header, &existing_policy_header, &file_missing_or_empty);

    wf = dsd_fopen_private(path, "a");
    if (!wf) {
        return -1;
    }

    if (tg_policy_write_group_file_header(wf, file_missing_or_empty, has_existing_header, clean_meta) != 0) {
        fclose(wf);
        return -1;
    }
    if (tg_policy_write_group_file_entry(wf, entry, has_existing_header, existing_policy_header, clean_mode, clean_name,
                                         clean_meta)
        != 0) {
        fclose(wf);
        return -1;
    }

    fclose(wf);
    return 0;
}

static double
tg_policy_get_env_ms_default(const char* name, double fallback_ms) {
    const char* s = getenv(name);
    char* end = NULL;
    double v = 0.0;
    if (!s || s[0] == '\0') {
        return fallback_ms;
    }
    errno = 0;
    v = strtod(s, &end);
    if (errno != 0 || end == s || v < 0.0) {
        return fallback_ms;
    }
    return v;
}

static int
tg_policy_route_is_complete(const dsd_tg_policy_call_route* route) {
    if (!route) {
        return 0;
    }
    if (route->slot != -1 && route->slot != 0 && route->slot != 1) {
        return 0;
    }
    if (route->channel <= 0 && route->freq_hz <= 0) {
        return 0;
    }
    return 1;
}

static int
tg_policy_active_call_matches_route_slot(const dsd_tg_policy_active_call* c, const dsd_tg_policy_call_route* route,
                                         int slot) {
    if (!c || !route) {
        return 0;
    }
    return c->valid && c->tg == route->target_id && c->channel == route->channel && c->slot == slot;
}

static int
tg_policy_collect_displaced_all_slots(const dsd_tg_policy_context* ctx, const dsd_tg_policy_call_route* route,
                                      int slot_match, int displaced[2], int* displaced_count) {
    if (!ctx || !route || !displaced || !displaced_count) {
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        const dsd_tg_policy_active_call* c = &ctx->active.calls[i];
        if (!c->valid) {
            continue;
        }
        if (tg_policy_active_call_matches_route_slot(c, route, slot_match)) {
            return 1;
        }
        displaced[(*displaced_count)++] = i;
    }
    return 0;
}

static int
tg_policy_collect_displaced_slots(const dsd_tg_policy_context* ctx, const dsd_tg_policy_call_route* route,
                                  int displaced[2], int* displaced_count) {
    const dsd_tg_policy_active_call* c = NULL;

    if (!ctx || !route || !displaced || !displaced_count) {
        return -1;
    }
    *displaced_count = 0;

    if (route->slot == -1) {
        return tg_policy_collect_displaced_all_slots(ctx, route, -1, displaced, displaced_count);
    }
    if (route->requires_tuner_retune) {
        return tg_policy_collect_displaced_all_slots(ctx, route, route->slot, displaced, displaced_count);
    }

    c = &ctx->active.calls[route->slot];
    if (!c->valid) {
        return 0;
    }
    if (tg_policy_active_call_matches_route_slot(c, route, route->slot)) {
        return 1;
    }
    displaced[(*displaced_count)++] = route->slot;
    return 0;
}

static int
tg_policy_preempt_passes_displaced_checks(const dsd_tg_policy_context* ctx, const int displaced[2], int displaced_count,
                                          const dsd_tg_policy_decision* candidate, double now_mono_s,
                                          double min_dwell_s, double cooldown_s) {
    if (!ctx || !displaced || !candidate) {
        return 0;
    }
    for (int i = 0; i < displaced_count; i++) {
        const dsd_tg_policy_active_call* c = &ctx->active.calls[displaced[i]];
        if (!c->valid) {
            continue;
        }
        if (candidate->priority <= c->priority) {
            return 0;
        }
        if (now_mono_s - c->start_mono_s < min_dwell_s) {
            return 0;
        }
        if (now_mono_s - c->last_preempt_mono_s < cooldown_s) {
            return 0;
        }
    }
    return 1;
}

int
dsd_tg_policy_should_preempt(const dsd_opts* opts, const dsd_state* state,
                             const dsd_tg_policy_call_route* candidate_route, const dsd_tg_policy_decision* candidate,
                             double now_mono_s) {
    const dsd_tg_policy_context* ctx = tg_policy_ctx_get_const(state);
    int displaced[2] = {-1, -1};
    int displaced_count = 0;
    int collect_result = 0;
    double min_dwell_s = tg_policy_get_env_ms_default("DSD_NEO_TG_PREEMPT_MIN_DWELL_MS", 750.0) / 1000.0;
    double cooldown_s = tg_policy_get_env_ms_default("DSD_NEO_TG_PREEMPT_COOLDOWN_MS", 1000.0) / 1000.0;

    (void)opts;
    if (!ctx || !candidate_route || !candidate) {
        return 0;
    }
    if (!candidate->tune_allowed || !candidate->preempt_requested) {
        return 0;
    }
    if (!tg_policy_route_is_complete(candidate_route)) {
        return 0;
    }

    collect_result = tg_policy_collect_displaced_slots(ctx, candidate_route, displaced, &displaced_count);
    if (collect_result != 0) {
        return 0;
    }

    if (displaced_count == 0) {
        return 0;
    }
    if (now_mono_s - ctx->active.last_global_preempt_mono_s < cooldown_s) {
        return 0;
    }
    if (!tg_policy_preempt_passes_displaced_checks(ctx, displaced, displaced_count, candidate, now_mono_s, min_dwell_s,
                                                   cooldown_s)) {
        return 0;
    }

    return 1;
}

static void
tg_policy_active_call_assign_from_route(dsd_tg_policy_active_call* c, const dsd_tg_policy_call_route* route,
                                        const dsd_tg_policy_decision* decision, double now_mono_s) {
    c->valid = 1;
    c->tg = route->target_id;
    c->src = route->source_id;
    c->freq_hz = route->freq_hz;
    c->channel = route->channel;
    c->priority = decision->priority;
    c->slot = route->slot;
    c->last_seen_mono_s = now_mono_s;
}

static int
tg_policy_active_call_matches_route(const dsd_tg_policy_active_call* c, const dsd_tg_policy_call_route* route) {
    if (!c || !route) {
        return 0;
    }
    return c->valid && c->tg == route->target_id && c->channel == route->channel && c->slot == route->slot;
}

static int
tg_policy_should_note_preempt(const dsd_tg_policy_active_call* c, const dsd_tg_policy_call_route* route,
                              const dsd_tg_policy_decision* decision) {
    if (!c || !route || !decision) {
        return 0;
    }
    if (tg_policy_active_call_matches_route(c, route)) {
        return 0;
    }
    return decision->preempt_requested && decision->priority > c->priority;
}

static void
tg_policy_note_active_slot(dsd_tg_policy_context* ctx, int slot, const dsd_tg_policy_call_route* route,
                           const dsd_tg_policy_decision* decision, double now_mono_s) {
    dsd_tg_policy_active_call* c = NULL;
    if (!ctx || !route || !decision || slot < 0 || slot > 1) {
        return;
    }
    c = &ctx->active.calls[slot];
    if (tg_policy_should_note_preempt(c, route, decision)) {
        c->last_preempt_mono_s = now_mono_s;
        ctx->active.last_global_preempt_mono_s = now_mono_s;
    }
    if (!tg_policy_active_call_matches_route(c, route)) {
        c->start_mono_s = now_mono_s;
    }
    tg_policy_active_call_assign_from_route(c, route, decision, now_mono_s);
}

int
dsd_tg_policy_note_active_call(dsd_state* state, const dsd_tg_policy_call_route* route,
                               const dsd_tg_policy_decision* decision, double now_mono_s) {
    dsd_tg_policy_context* ctx = tg_policy_ctx_get_mut(state, 1);
    if (!ctx || !route || !decision) {
        return 1;
    }
    if (!tg_policy_route_is_complete(route)) {
        return 1;
    }
    if (route->slot == -1) {
        tg_policy_note_active_slot(ctx, 0, route, decision, now_mono_s);
        tg_policy_note_active_slot(ctx, 1, route, decision, now_mono_s);
    } else {
        tg_policy_note_active_slot(ctx, route->slot, route, decision, now_mono_s);
    }
    return 0;
}

int
dsd_tg_policy_clear_active_call(dsd_state* state, int slot) {
    dsd_tg_policy_context* ctx = tg_policy_ctx_get_mut(state, 0);
    if (!ctx) {
        return 0;
    }
    if (slot < 0) {
        DSD_MEMSET(&ctx->active.calls[0], 0, sizeof(ctx->active.calls));
        return 0;
    }
    if (slot > 1) {
        return 1;
    }
    DSD_MEMSET(&ctx->active.calls[slot], 0, sizeof(ctx->active.calls[slot]));
    return 0;
}

static int
tg_policy_is_terminated_cstr(const char* s, size_t cap) {
    return (s != NULL) && (memchr(s, '\0', cap) != NULL);
}

static int DSD_ATTR_USED
tg_policy_reload_validate_candidate(const dsd_state* imported) {
    const dsd_tg_policy_context* ctx = NULL;

    if (!imported) {
        return -1;
    }

    ctx = tg_policy_ctx_get_const(imported);
    if (!ctx) {
        return 0;
    }
    for (size_t i = 0; i < ctx->table.count; i++) {
        const dsd_tg_policy_entry* e = &ctx->table.entries[i];
        if (!tg_policy_is_terminated_cstr(e->mode, sizeof(e->mode))
            || !tg_policy_is_terminated_cstr(e->name, sizeof(e->name))) {
            return -1;
        }
        if (e->is_range == 0u) {
            if (!tg_policy_entry_valid_exact(e)) {
                return -1;
            }
        } else {
            if (!tg_policy_entry_valid_range(e)) {
                return -1;
            }
        }
    }

    return 0;
}

int
dsd_tg_policy_reload_group_file(const dsd_opts* opts, dsd_state* state) {
    dsd_state* imported = NULL;
    dsd_tg_policy_context* imported_ctx = NULL;
    const dsd_tg_policy_context* current_ctx = NULL;
    void* imported_policy = NULL;
    dsd_state_ext_cleanup_fn imported_cleanup = NULL;
    unsigned int next_table_generation = 1u;
    unsigned int next_active_generation = 1u;

    if (!opts || !state) {
        return -1;
    }

    imported = (dsd_state*)calloc(1, sizeof(*imported));
    if (!imported) {
        return -1;
    }

    if (csvGroupImportPath(opts->group_in_file, imported) != 0) {
        dsd_state_ext_free_all(imported);
        free(imported);
        return -1;
    }
    if (tg_policy_reload_validate_candidate(imported) != 0) {
        dsd_state_ext_free_all(imported);
        free(imported);
        return -1;
    }

    current_ctx = tg_policy_ctx_get_const(state);
    if (current_ctx) {
        next_table_generation = current_ctx->table.generation + 1u;
        next_active_generation = current_ctx->active.generation + 1u;
    }

    imported_policy = imported->state_ext[DSD_STATE_EXT_CORE_TG_POLICY];
    imported_cleanup = imported->state_ext_cleanup[DSD_STATE_EXT_CORE_TG_POLICY];
    imported->state_ext[DSD_STATE_EXT_CORE_TG_POLICY] = NULL;
    imported->state_ext_cleanup[DSD_STATE_EXT_CORE_TG_POLICY] = NULL;

    if (!imported_policy) {
        imported_ctx = tg_policy_context_alloc();
        if (!imported_ctx) {
            dsd_state_ext_free_all(imported);
            free(imported);
            return -1;
        }
        imported_policy = imported_ctx;
        imported_cleanup = tg_policy_context_free;
    }

    imported_ctx = (dsd_tg_policy_context*)imported_policy;
    imported_ctx->table.generation = next_table_generation;
    imported_ctx->active.generation = next_active_generation;

    if (dsd_state_ext_set(state, DSD_STATE_EXT_CORE_TG_POLICY, imported_policy, imported_cleanup) != 0) {
        if (imported_cleanup) {
            imported_cleanup(imported_policy);
        }
        dsd_state_ext_free_all(imported);
        free(imported);
        return -1;
    }
    dsd_state_ext_free_all(imported);
    free(imported);
    return 0;
}
