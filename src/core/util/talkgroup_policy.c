// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/talkgroup_policy.h>

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

typedef struct {
    dsd_tg_policy_entry* entries;
    size_t count;
    size_t capacity;
    unsigned int generation;
} dsd_tg_policy_table;

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
} dsd_tg_policy_context;

static long s_alloc_fail_after = -1;
static long s_alloc_calls = 0;

static int
tg_policy_alloc_should_fail(void) {
    if (s_alloc_fail_after < 0) {
        return 0;
    }
    return s_alloc_calls >= s_alloc_fail_after;
}

static void*
tg_policy_calloc(size_t n, size_t size) {
    if (tg_policy_alloc_should_fail()) {
        return NULL;
    }
    s_alloc_calls++;
    return calloc(n, size);
}

static void*
tg_policy_realloc(void* ptr, size_t size) {
    if (tg_policy_alloc_should_fail()) {
        return NULL;
    }
    s_alloc_calls++;
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
    snprintf(dst, dst_sz, "%s", src);
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
    memset(dst, 0, sizeof(*dst));
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

static dsd_tg_policy_context*
tg_policy_ctx_get_mut(dsd_state* state, int create_if_missing) {
    dsd_tg_policy_context* ctx = NULL;
    if (!state) {
        return NULL;
    }

    ctx = (dsd_tg_policy_context*)dsd_state_ext_get(state, DSD_STATE_EXT_CORE_TG_POLICY);
    if (ctx || !create_if_missing) {
        return ctx;
    }

    ctx = (dsd_tg_policy_context*)tg_policy_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    if (dsd_state_ext_set(state, DSD_STATE_EXT_CORE_TG_POLICY, ctx, tg_policy_context_free) != 0) {
        tg_policy_context_free(ctx);
        return NULL;
    }
    return ctx;
}

static const dsd_tg_policy_context*
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
tg_policy_find_legacy_exact_idx(const dsd_state* state, uint32_t id) {
    if (!state) {
        return -1;
    }
    for (unsigned int i = 0; i < state->group_tally; i++) {
        if (state->group_array[i].groupNumber == (unsigned long)id) {
            return (int)i;
        }
    }
    return -1;
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

static int
tg_policy_lookup_exact_only(const dsd_state* state, uint32_t id, dsd_tg_policy_entry* out) {
    dsd_tg_policy_lookup lookup;
    if (!out) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (dsd_tg_policy_lookup_id(state, id, &lookup) != 0) {
        return 0;
    }
    if (lookup.match != DSD_TG_POLICY_MATCH_EXACT) {
        return 0;
    }
    *out = lookup.entry;
    return 1;
}

int
dsd_tg_policy_make_legacy_exact_entry(uint32_t id, const char* mode, const char* name,
                                      dsd_tg_policy_entry_source source, dsd_tg_policy_entry* out) {
    if (!out || !mode || !name) {
        return 1;
    }

    memset(out, 0, sizeof(*out));
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
    return 0;
}

int
dsd_tg_policy_append_legacy_exact(dsd_state* state, const dsd_tg_policy_entry* entry) {
    dsd_tg_policy_context* ctx = NULL;
    dsd_tg_policy_entry normalized;
    const size_t group_cap = state ? (sizeof(state->group_array) / sizeof(state->group_array[0])) : 0;
    if (!state || !entry) {
        return 1;
    }
    if (!tg_policy_entry_valid_exact(entry)) {
        return 1;
    }
    if (state->group_tally >= group_cap) {
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

    state->group_array[state->group_tally].groupNumber = (unsigned long)normalized.id_start;
    tg_policy_safe_copy(state->group_array[state->group_tally].groupMode,
                        sizeof(state->group_array[state->group_tally].groupMode), normalized.mode);
    tg_policy_safe_copy(state->group_array[state->group_tally].groupName,
                        sizeof(state->group_array[state->group_tally].groupName), normalized.name);
    state->group_tally++;
    return 0;
}

static void
tg_policy_apply_exact_to_legacy_row(dsd_state* state, int idx, const dsd_tg_policy_entry* entry) {
    if (!state || idx < 0 || !entry) {
        return;
    }
    state->group_array[idx].groupNumber = (unsigned long)entry->id_start;
    tg_policy_safe_copy(state->group_array[idx].groupMode, sizeof(state->group_array[idx].groupMode), entry->mode);
    tg_policy_safe_copy(state->group_array[idx].groupName, sizeof(state->group_array[idx].groupName), entry->name);
}

static void
tg_policy_build_legacy_mirror_entry(const dsd_state* state, int legacy_idx, dsd_tg_policy_entry* out) {
    if (!state || legacy_idx < 0 || !out) {
        return;
    }
    (void)dsd_tg_policy_make_legacy_exact_entry(
        (uint32_t)state->group_array[legacy_idx].groupNumber, state->group_array[legacy_idx].groupMode,
        state->group_array[legacy_idx].groupName, DSD_TG_POLICY_SOURCE_LEGACY_UNKNOWN, out);
    out->row = 0;
}

int
dsd_tg_policy_upsert_legacy_exact(dsd_state* state, const dsd_tg_policy_entry* entry, dsd_tg_policy_upsert_mode mode) {
    dsd_tg_policy_context* ctx = NULL;
    dsd_tg_policy_entry normalized;
    int legacy_idx = -1;
    int policy_idx = -1;
    const size_t group_cap = state ? (sizeof(state->group_array) / sizeof(state->group_array[0])) : 0;

    if (!state || !entry) {
        return 1;
    }
    if (!tg_policy_entry_valid_exact(entry)) {
        return 1;
    }
    tg_policy_copy_entry_normalized(&normalized, entry);
    normalized.is_range = 0;
    normalized.id_end = normalized.id_start;

    legacy_idx = tg_policy_find_legacy_exact_idx(state, normalized.id_start);

    if (mode == DSD_TG_POLICY_UPSERT_ADD_IF_MISSING) {
        if (legacy_idx >= 0) {
            return 0;
        }
        if (state->group_tally >= group_cap) {
            return 1;
        }
        return dsd_tg_policy_append_legacy_exact(state, &normalized);
    }

    ctx = tg_policy_ctx_get_mut(state, 1);
    if (!ctx) {
        return -1;
    }
    policy_idx = tg_policy_find_policy_exact_idx_first(ctx, normalized.id_start);

    if (mode == DSD_TG_POLICY_UPSERT_REPLACE_LEARNED_ONLY) {
        if (legacy_idx < 0 || policy_idx < 0) {
            return 0;
        }
        if (ctx->table.entries[policy_idx].source != DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS) {
            return 0;
        }
        tg_policy_apply_exact_to_legacy_row(state, legacy_idx, &normalized);
        ctx->table.entries[policy_idx] = normalized;
        return 0;
    }

    if (legacy_idx < 0) {
        if (state->group_tally >= group_cap) {
            return 1;
        }
        return dsd_tg_policy_append_legacy_exact(state, &normalized);
    }

    if (policy_idx < 0) {
        dsd_tg_policy_entry mirror;
        memset(&mirror, 0, sizeof(mirror));
        tg_policy_build_legacy_mirror_entry(state, legacy_idx, &mirror);
        if (tg_policy_table_reserve(ctx, ctx->table.count + 1) != 0) {
            return -1;
        }
        ctx->table.entries[ctx->table.count++] = mirror;
        policy_idx = tg_policy_find_policy_exact_idx_first(ctx, normalized.id_start);
    }

    tg_policy_apply_exact_to_legacy_row(state, legacy_idx, &normalized);
    if (policy_idx >= 0) {
        ctx->table.entries[policy_idx] = normalized;
    }
    return 0;
}

int
dsd_tg_policy_lookup_id(const dsd_state* state, uint32_t id, dsd_tg_policy_lookup* out) {
    const dsd_tg_policy_context* ctx = NULL;
    int best_idx = -1;
    uint64_t best_span = UINT64_MAX;
    if (!out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->match = DSD_TG_POLICY_MATCH_NONE;

    if (!state) {
        return 0;
    }

    ctx = tg_policy_ctx_get_const(state);
    if (ctx) {
        for (size_t i = 0; i < ctx->table.count; i++) {
            const dsd_tg_policy_entry* e = &ctx->table.entries[i];
            if (e->is_range == 0u && e->id_start == id && e->id_end == id) {
                out->match = DSD_TG_POLICY_MATCH_EXACT;
                out->entry = *e;
                return 0;
            }
        }
    }

    for (unsigned int i = 0; i < state->group_tally; i++) {
        if (state->group_array[i].groupNumber == (unsigned long)id) {
            dsd_tg_policy_entry synth;
            if (dsd_tg_policy_make_legacy_exact_entry(id, state->group_array[i].groupMode,
                                                      state->group_array[i].groupName,
                                                      DSD_TG_POLICY_SOURCE_LEGACY_UNKNOWN, &synth)
                == 0) {
                synth.row = 0;
                out->match = DSD_TG_POLICY_MATCH_EXACT;
                out->entry = synth;
            }
            return 0;
        }
    }

    if (!ctx) {
        return 0;
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
    if (best_idx >= 0) {
        out->match = DSD_TG_POLICY_MATCH_RANGE;
        out->entry = ctx->table.entries[best_idx];
    }
    return 0;
}

static void
tg_policy_init_decision(dsd_tg_policy_decision* out, uint32_t target_id, uint32_t source_id, int encrypted,
                        int data_call) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
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
tg_policy_apply_hold_overrides(dsd_tg_policy_decision* out, dsd_tg_policy_hold_behavior hold_behavior) {
    if (!out || !out->tg_hold_active || !out->tg_hold_match) {
        return;
    }
    if (hold_behavior == DSD_TG_POLICY_HOLD_FORCE_MEDIA_ONLY) {
        out->audio_allowed = 1;
        out->record_allowed = 1;
        out->stream_allowed = 1;
    } else if (hold_behavior == DSD_TG_POLICY_HOLD_FORCE_TUNE_AND_MEDIA) {
        out->tune_allowed = 1;
        out->audio_allowed = 1;
        out->record_allowed = 1;
        out->stream_allowed = 1;
        out->block_reasons &= ~(DSD_TG_POLICY_BLOCK_ALLOWLIST | DSD_TG_POLICY_BLOCK_MODE | DSD_TG_POLICY_BLOCK_HOLD
                                | DSD_TG_POLICY_BLOCK_GROUP_DISABLED | DSD_TG_POLICY_BLOCK_PRIVATE_DISABLED
                                | DSD_TG_POLICY_BLOCK_DATA_DISABLED | DSD_TG_POLICY_BLOCK_ENCRYPTED_DISABLED
                                | DSD_TG_POLICY_BLOCK_AUDIO | DSD_TG_POLICY_BLOCK_RECORD | DSD_TG_POLICY_BLOCK_STREAM);
    }
}

int
dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                  int encrypted, int data_call, dsd_tg_policy_hold_behavior hold_behavior,
                                  dsd_tg_policy_decision* out) {
    dsd_tg_policy_lookup lookup;
    int mode_blocking = 0;
    int explicit_audio_block = 0;
    int explicit_record_block = 0;
    int explicit_stream_block = 0;
    int hold_mismatch = 0;

    if (!out) {
        return -1;
    }
    tg_policy_init_decision(out, tg, src, encrypted, data_call);

    if (dsd_tg_policy_lookup_id(state, tg, &lookup) == 0 && lookup.match != DSD_TG_POLICY_MATCH_NONE) {
        out->match = lookup.match;
        out->priority = lookup.entry.priority;
        out->preempt_requested = lookup.entry.preempt ? 1 : 0;
        out->audio_allowed = lookup.entry.audio ? 1 : 0;
        out->record_allowed = lookup.entry.record ? 1 : 0;
        out->stream_allowed = lookup.entry.stream ? 1 : 0;
        tg_policy_safe_copy(out->mode, sizeof(out->mode), lookup.entry.mode);
        tg_policy_safe_copy(out->name, sizeof(out->name), lookup.entry.name);
        mode_blocking = tg_policy_mode_is_blocking(lookup.entry.mode);
        explicit_audio_block = (!mode_blocking && lookup.entry.audio == 0u);
        explicit_record_block = (!mode_blocking && lookup.entry.audio != 0u && lookup.entry.record == 0u);
        explicit_stream_block = (!mode_blocking && lookup.entry.audio != 0u && lookup.entry.stream == 0u);
    }

    out->tg_hold_active = (state && state->tg_hold != 0) ? 1 : 0;
    out->tg_hold_match = (out->tg_hold_active && state->tg_hold == tg) ? 1 : 0;
    hold_mismatch = out->tg_hold_active && !out->tg_hold_match;

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
    if (mode_blocking) {
        out->tune_allowed = 0;
        out->audio_allowed = 0;
        out->record_allowed = 0;
        out->stream_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_MODE;
    }
    if (hold_mismatch) {
        out->tune_allowed = 0;
        out->audio_allowed = 0;
        out->record_allowed = 0;
        out->stream_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_HOLD;
    }

    if (!mode_blocking && !hold_mismatch) {
        if (explicit_audio_block) {
            out->audio_allowed = 0;
            out->record_allowed = 0;
            out->stream_allowed = 0;
            out->block_reasons |= DSD_TG_POLICY_BLOCK_AUDIO;
        } else {
            if (explicit_record_block) {
                out->record_allowed = 0;
                out->block_reasons |= DSD_TG_POLICY_BLOCK_RECORD;
            }
            if (explicit_stream_block) {
                out->stream_allowed = 0;
                out->block_reasons |= DSD_TG_POLICY_BLOCK_STREAM;
            }
        }
    }

    tg_policy_apply_hold_overrides(out, hold_behavior);
    return 0;
}

int
dsd_tg_policy_evaluate_private_call(const dsd_opts* opts, const dsd_state* state, uint32_t src, uint32_t dst,
                                    int encrypted, int data_call, dsd_tg_policy_private_allowlist_mode allowlist_mode,
                                    dsd_tg_policy_hold_behavior hold_behavior, dsd_tg_policy_decision* out) {
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
    if (dst_match) {
        chosen = &dst_entry;
    } else if (src_match) {
        chosen = &src_entry;
    }
    if (chosen) {
        out->match = DSD_TG_POLICY_MATCH_EXACT;
        out->priority = chosen->priority;
        out->preempt_requested = chosen->preempt ? 1 : 0;
        out->audio_allowed = chosen->audio ? 1 : 0;
        out->record_allowed = chosen->record ? 1 : 0;
        out->stream_allowed = chosen->stream ? 1 : 0;
        tg_policy_safe_copy(out->mode, sizeof(out->mode), chosen->mode);
        tg_policy_safe_copy(out->name, sizeof(out->name), chosen->name);
    }

    mode_blocking = (src_match && tg_policy_mode_is_blocking(src_entry.mode))
                    || (dst_match && tg_policy_mode_is_blocking(dst_entry.mode));
    out->tg_hold_active = (state && state->tg_hold != 0) ? 1 : 0;
    out->tg_hold_match = (out->tg_hold_active && (state->tg_hold == src || state->tg_hold == dst)) ? 1 : 0;
    hold_mismatch = out->tg_hold_active && !out->tg_hold_match;

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
    if (opts && opts->trunk_use_allow_list == 1 && allowlist_mode == DSD_TG_POLICY_PRIVATE_ALLOWLIST_UNKNOWN_BLOCK
        && !src_match && !dst_match) {
        out->tune_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_ALLOWLIST;
    }
    if (mode_blocking) {
        out->tune_allowed = 0;
        out->audio_allowed = 0;
        out->record_allowed = 0;
        out->stream_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_MODE;
    }
    if (hold_mismatch) {
        out->tune_allowed = 0;
        out->audio_allowed = 0;
        out->record_allowed = 0;
        out->stream_allowed = 0;
        out->block_reasons |= DSD_TG_POLICY_BLOCK_HOLD;
    }
    if (!mode_blocking && !hold_mismatch && chosen) {
        if (chosen->audio == 0u) {
            out->audio_allowed = 0;
            out->record_allowed = 0;
            out->stream_allowed = 0;
            out->block_reasons |= DSD_TG_POLICY_BLOCK_AUDIO;
        } else {
            if (chosen->record == 0u) {
                out->record_allowed = 0;
                out->block_reasons |= DSD_TG_POLICY_BLOCK_RECORD;
            }
            if (chosen->stream == 0u) {
                out->stream_allowed = 0;
                out->block_reasons |= DSD_TG_POLICY_BLOCK_STREAM;
            }
        }
    }

    tg_policy_apply_hold_overrides(out, hold_behavior);
    return 0;
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
    snprintf(buf, sizeof(buf), "%s", header_line);
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

int
dsd_tg_policy_append_group_file_row(const dsd_opts* opts, const dsd_tg_policy_entry* entry,
                                    const char* legacy_metadata) {
    char first_line[512];
    char clean_mode[16];
    char clean_name[128];
    char clean_meta[256];
    int has_existing_header = 0;
    int existing_policy_header = 0;
    int file_missing_or_empty = 0;
    FILE* rf = NULL;
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
    tg_policy_csv_sanitize_copy(clean_meta, sizeof(clean_meta), legacy_metadata ? legacy_metadata : "");

    rf = fopen(path, "r");
    if (rf) {
        if (fgets(first_line, sizeof(first_line), rf) != NULL) {
            has_existing_header = 1;
            existing_policy_header = tg_policy_header_is_policy(first_line);
        } else {
            file_missing_or_empty = 1;
        }
        fclose(rf);
    } else {
        file_missing_or_empty = 1;
    }

    wf = fopen(path, "a");
    if (!wf) {
        return -1;
    }

    if (file_missing_or_empty) {
        if (clean_meta[0] != '\0') {
            if (fprintf(wf, "id,mode,name,metadata\n") < 0) {
                fclose(wf);
                return -1;
            }
        } else {
            if (fprintf(wf, "id,mode,name\n") < 0) {
                fclose(wf);
                return -1;
            }
        }
    } else if (!has_existing_header) {
        if (fprintf(wf, "id,mode,name\n") < 0) {
            fclose(wf);
            return -1;
        }
    }

    if (has_existing_header && existing_policy_header) {
        if (fprintf(wf, "%u,%s,%s,%d,%s,%s,%s,%s,%s\n", entry->id_start, clean_mode, clean_name, entry->priority,
                    entry->preempt ? "true" : "false", entry->audio ? "on" : "off", entry->record ? "on" : "off",
                    entry->stream ? "on" : "off", clean_meta)
            < 0) {
            fclose(wf);
            return -1;
        }
    } else if (clean_meta[0] != '\0') {
        if (fprintf(wf, "%u,%s,%s,%s\n", entry->id_start, clean_mode, clean_name, clean_meta) < 0) {
            fclose(wf);
            return -1;
        }
    } else {
        if (fprintf(wf, "%u,%s,%s\n", entry->id_start, clean_mode, clean_name) < 0) {
            fclose(wf);
            return -1;
        }
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

int
dsd_tg_policy_should_preempt(const dsd_opts* opts, const dsd_state* state,
                             const dsd_tg_policy_call_route* candidate_route, const dsd_tg_policy_decision* candidate,
                             double now_mono_s) {
    const dsd_tg_policy_context* ctx = tg_policy_ctx_get_const(state);
    int displaced[2] = {-1, -1};
    int displaced_count = 0;
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

    if (candidate_route->slot == -1) {
        for (int i = 0; i < 2; i++) {
            const dsd_tg_policy_active_call* c = &ctx->active.calls[i];
            if (!c->valid) {
                continue;
            }
            if (c->tg == candidate_route->target_id && c->channel == candidate_route->channel && c->slot == -1) {
                return 0;
            }
            displaced[displaced_count++] = i;
        }
    } else if (candidate_route->requires_tuner_retune) {
        for (int i = 0; i < 2; i++) {
            const dsd_tg_policy_active_call* c = &ctx->active.calls[i];
            if (!c->valid) {
                continue;
            }
            if (c->tg == candidate_route->target_id && c->channel == candidate_route->channel
                && c->slot == candidate_route->slot) {
                return 0;
            }
            displaced[displaced_count++] = i;
        }
    } else {
        const dsd_tg_policy_active_call* c = &ctx->active.calls[candidate_route->slot];
        if (c->valid) {
            if (c->tg == candidate_route->target_id && c->channel == candidate_route->channel
                && c->slot == candidate_route->slot) {
                return 0;
            }
            displaced[displaced_count++] = candidate_route->slot;
        }
    }

    if (displaced_count == 0) {
        return 0;
    }
    if (now_mono_s - ctx->active.last_global_preempt_mono_s < cooldown_s) {
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

static void
tg_policy_note_active_slot(dsd_tg_policy_context* ctx, int slot, const dsd_tg_policy_call_route* route,
                           const dsd_tg_policy_decision* decision, double now_mono_s) {
    dsd_tg_policy_active_call* c = NULL;
    if (!ctx || !route || !decision || slot < 0 || slot > 1) {
        return;
    }
    c = &ctx->active.calls[slot];
    if (c->valid && (c->tg != route->target_id || c->channel != route->channel || c->slot != route->slot)
        && decision->preempt_requested && decision->priority > c->priority) {
        c->last_preempt_mono_s = now_mono_s;
        ctx->active.last_global_preempt_mono_s = now_mono_s;
    }
    if (!c->valid || c->tg != route->target_id || c->channel != route->channel || c->slot != route->slot) {
        c->start_mono_s = now_mono_s;
    }
    c->valid = 1;
    c->tg = route->target_id;
    c->src = route->source_id;
    c->freq_hz = route->freq_hz;
    c->channel = route->channel;
    c->priority = decision->priority;
    c->slot = route->slot;
    c->last_seen_mono_s = now_mono_s;
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
        memset(&ctx->active.calls[0], 0, sizeof(ctx->active.calls));
        return 0;
    }
    if (slot > 1) {
        return 1;
    }
    memset(&ctx->active.calls[slot], 0, sizeof(ctx->active.calls[slot]));
    return 0;
}

int
dsd_tg_policy_clear_active_call_route(dsd_state* state, const dsd_tg_policy_call_route* route) {
    dsd_tg_policy_context* ctx = tg_policy_ctx_get_mut(state, 0);
    if (!ctx || !route) {
        return 0;
    }
    for (int i = 0; i < 2; i++) {
        dsd_tg_policy_active_call* c = &ctx->active.calls[i];
        if (!c->valid) {
            continue;
        }
        if (c->tg == route->target_id
            && ((route->channel > 0 && c->channel == route->channel)
                || (route->channel <= 0 && route->freq_hz > 0 && c->freq_hz == route->freq_hz))) {
            memset(c, 0, sizeof(*c));
        }
    }
    return 0;
}

static int
tg_policy_is_terminated_cstr(const char* s, size_t cap) {
    return (s != NULL) && (memchr(s, '\0', cap) != NULL);
}

static int
tg_policy_reload_validate_candidate(const dsd_state* imported) {
    dsd_tg_policy_lookup lookup;
    const dsd_tg_policy_context* ctx = NULL;
    const size_t group_cap = imported ? (sizeof(imported->group_array) / sizeof(imported->group_array[0])) : 0;

    if (!imported) {
        return -1;
    }
    if (imported->group_tally > group_cap) {
        return -1;
    }

    for (unsigned int i = 0; i < imported->group_tally; i++) {
        const uint32_t id = (uint32_t)imported->group_array[i].groupNumber;
        if (imported->group_array[i].groupNumber > UINT32_MAX) {
            return -1;
        }
        if (!tg_policy_is_terminated_cstr(imported->group_array[i].groupMode,
                                          sizeof(imported->group_array[i].groupMode))
            || !tg_policy_is_terminated_cstr(imported->group_array[i].groupName,
                                             sizeof(imported->group_array[i].groupName))) {
            return -1;
        }
        if (dsd_tg_policy_lookup_id(imported, id, &lookup) != 0 || lookup.match != DSD_TG_POLICY_MATCH_EXACT) {
            return -1;
        }
        if (strcmp(lookup.entry.mode, imported->group_array[i].groupMode) != 0
            || strcmp(lookup.entry.name, imported->group_array[i].groupName) != 0) {
            return -1;
        }
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
            if (tg_policy_find_legacy_exact_idx(imported, e->id_start) < 0) {
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
dsd_tg_policy_reload_group_file(dsd_opts* opts, dsd_state* state) {
    dsd_state imported;
    dsd_tg_policy_context* imported_ctx = NULL;
    const dsd_tg_policy_context* current_ctx = NULL;
    void* imported_policy = NULL;
    dsd_state_ext_cleanup_fn imported_cleanup = NULL;
    unsigned int next_table_generation = 1u;
    unsigned int next_active_generation = 1u;

    if (!opts || !state) {
        return -1;
    }

    memset(&imported, 0, sizeof(imported));
    if (csvGroupImportPath(opts->group_in_file, &imported) != 0) {
        dsd_state_ext_free_all(&imported);
        return -1;
    }
    if (tg_policy_reload_validate_candidate(&imported) != 0) {
        dsd_state_ext_free_all(&imported);
        return -1;
    }

    current_ctx = tg_policy_ctx_get_const(state);
    if (current_ctx) {
        next_table_generation = current_ctx->table.generation + 1u;
        next_active_generation = current_ctx->active.generation + 1u;
    }

    imported_policy = imported.state_ext[DSD_STATE_EXT_CORE_TG_POLICY];
    imported_cleanup = imported.state_ext_cleanup[DSD_STATE_EXT_CORE_TG_POLICY];
    imported.state_ext[DSD_STATE_EXT_CORE_TG_POLICY] = NULL;
    imported.state_ext_cleanup[DSD_STATE_EXT_CORE_TG_POLICY] = NULL;

    if (!imported_policy) {
        imported_ctx = (dsd_tg_policy_context*)tg_policy_calloc(1, sizeof(*imported_ctx));
        if (!imported_ctx) {
            dsd_state_ext_free_all(&imported);
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
        dsd_state_ext_free_all(&imported);
        return -1;
    }
    memcpy(state->group_array, imported.group_array, sizeof(state->group_array));
    state->group_tally = imported.group_tally;

    dsd_state_ext_free_all(&imported);
    return 0;
}
