// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Source aliases (the radio ID list): the immutable row store, its lookups and its snapshot copy. Rows are
 * exact IDs, ranges or digit patterns, with optional policy. See docs/csv-formats.md, "Source ID List CSV".
 */
#include <dsd-neo/core/id_pattern.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "talkgroup_policy_internal.h"

struct dsd_source_alias_store {
    dsd_source_alias_entry* entries;
    size_t count, capacity;
    /* Pattern rows (0 skips the pattern pass and the ID formatting) and policy rows (0 skips policy lookups). */
    size_t pattern_count, policy_count;
    uint64_t generation, store_id, snapshot_source_id;
};

static dsd_atomic_u64 next_store_id = {1u};
#ifdef DSD_NEO_TEST_HOOKS
static long alloc_remaining = -1;

void
dsd_source_alias_test_alloc_fail_after(long successful_allocations) {
    alloc_remaining = successful_allocations;
}

void
dsd_source_alias_test_alloc_reset(void) {
    alloc_remaining = -1;
}
#endif
static void*
alias_alloc(void* ptr, size_t size) {
#ifdef DSD_NEO_TEST_HOOKS
    if (alloc_remaining == 0) {
        return NULL;
    }
    if (alloc_remaining > 0) {
        --alloc_remaining;
    }
#endif
    return ptr ? realloc(ptr, size) : calloc(1, size);
}

static const dsd_source_alias_store*
alias_get(const dsd_state* state) {
    return (const dsd_source_alias_store*)dsd_state_ext_get_const(state, DSD_STATE_EXT_CORE_SOURCE_ALIAS);
}

dsd_source_alias_store*
dsd_source_alias_store_create(void) {
    dsd_source_alias_store* s = alias_alloc(NULL, sizeof(*s));
    if (!s) {
        return NULL;
    }
    do {
        s->store_id = dsd_atomic_u64_fetch_add_relaxed(&next_store_id, 1u);
    } while (!s->store_id);
    return s;
}

/* A row as the store keeps it: kind and bounds checked, pattern text canonical, policy fields normalised (cleared on a
 * row without policy), strings terminated. Returns 0 when the row is valid. */
static int
alias_entry_normalize(const dsd_source_alias_entry* in, dsd_source_alias_entry* out) {
    *out = *in;
    out->name[sizeof(out->name) - 1U] = '\0';
    out->has_policy = in->has_policy ? 1u : 0u;
    switch (in->kind) {
        case DSD_TG_POLICY_ROW_EXACT:
        case DSD_TG_POLICY_ROW_RANGE:
            if (in->id_start > in->id_end || (in->kind == DSD_TG_POLICY_ROW_EXACT && in->id_start != in->id_end)) {
                return 1;
            }
            DSD_MEMSET(out->pattern, 0, sizeof(out->pattern));
            break;
        case DSD_TG_POLICY_ROW_PATTERN: {
            dsd_id_pattern parsed;
            if (in->id_start != 0u || in->id_end != 0u || !memchr(in->pattern, '\0', sizeof(in->pattern))
                || dsd_id_pattern_parse(in->pattern, &parsed) != 0) {
                return 1;
            }
            DSD_SNPRINTF(out->pattern, sizeof(out->pattern), "%s", parsed.text);
            break;
        }
        default: return 1;
    }
    if (!out->has_policy) {
        if (!out->name[0]) {
            return 1;
        }
        DSD_MEMSET(out->mode, 0, sizeof(out->mode));
        out->audio = out->record = out->stream = 0u;
        return 0;
    }
    out->mode[sizeof(out->mode) - 1U] = '\0';
    dsd_tg_policy_media_normalize(out->mode, &out->audio, &out->record, &out->stream);
    return 0;
}

int
dsd_source_alias_store_append(dsd_source_alias_store* store, const dsd_source_alias_entry* entry) {
    dsd_source_alias_entry row;
    if (!store || !entry || alias_entry_normalize(entry, &row) != 0) {
        return 1;
    }
    if (store->count == store->capacity) {
        if (store->capacity > SIZE_MAX / 2) {
            return -1;
        }
        size_t cap = store->capacity ? store->capacity * 2 : 16;
        if (cap > SIZE_MAX / sizeof(*store->entries)) {
            return -1;
        }
        void* entries = alias_alloc(store->entries, cap * sizeof(*store->entries));
        if (!entries) {
            return -1;
        }
        store->entries = entries;
        store->capacity = cap;
    }
    store->entries[store->count++] = row;
    store->pattern_count += row.kind == DSD_TG_POLICY_ROW_PATTERN ? 1u : 0u;
    store->policy_count += row.has_policy ? 1u : 0u;
    if (++store->generation == 0) {
        ++store->generation;
    }
    return 0;
}

size_t
dsd_source_alias_store_count(const dsd_source_alias_store* store) {
    return store ? store->count : 0;
}

void
dsd_source_alias_store_free(dsd_source_alias_store* store) {
    if (store) {
        free(store->entries);
        free(store);
    }
}

static void
alias_cleanup(void* s) {
    dsd_source_alias_store_free(s);
}

void
dsd_source_alias_install(dsd_state* state, dsd_source_alias_store* store) {
    if (dsd_state_ext_set(state, DSD_STATE_EXT_CORE_SOURCE_ALIAS, store, store ? alias_cleanup : NULL) != 0) {
        dsd_source_alias_store_free(store);
    }
}

int
dsd_source_alias_clear(dsd_state* state) {
    return dsd_state_ext_set(state, DSD_STATE_EXT_CORE_SOURCE_ALIAS, NULL, NULL);
}

int
dsd_source_alias_loaded(const dsd_state* state) {
    return alias_get(state) != NULL;
}

size_t
dsd_source_alias_count(const dsd_state* state) {
    return dsd_source_alias_store_count(alias_get(state));
}

uint64_t
dsd_source_alias_store_id(const dsd_state* state) {
    const dsd_source_alias_store* s = alias_get(state);
    return s ? s->store_id : 0u;
}

int
dsd_source_policy_loaded(const dsd_state* state) {
    const dsd_source_alias_store* s = alias_get(state);
    return s && s->policy_count > 0u;
}

/* The winning row for one ID among a set of rows, built one matching row at a time: the first exact row; else the
 * narrowest range, the last row winning ties; else the most specific pattern, the last row winning ties. */
typedef struct {
    const dsd_source_alias_entry* exact;
    const dsd_source_alias_entry* range;
    const dsd_source_alias_entry* pattern;
    dsd_id_pattern rank; /* the pattern row's rank */
} alias_pick;

static void
alias_pick_offer(alias_pick* p, const dsd_source_alias_entry* e, const dsd_id_pattern* rank) {
    if (e->kind == DSD_TG_POLICY_ROW_EXACT) {
        if (!p->exact) {
            p->exact = e;
        }
    } else if (e->kind == DSD_TG_POLICY_ROW_RANGE) {
        if (!p->range || e->id_end - e->id_start <= p->range->id_end - p->range->id_start) {
            p->range = e;
        }
    } else if (rank && (!p->pattern || !dsd_id_pattern_more_specific(&p->rank, rank))) {
        p->pattern = e;
        p->rank = *rank;
    }
}

static const dsd_source_alias_entry*
alias_pick_winner(const alias_pick* p) {
    return p->exact ? p->exact : (p->range ? p->range : p->pattern);
}

/* One pass over the rows: the winner among all rows (@p any) and among named rows (@p named); either may be NULL when
 * not wanted. The ID is written in decimal once, and only when the store has pattern rows; a pattern row is matched
 * only while an exact or range row has not already beaten every pattern. */
static void
alias_find(const dsd_source_alias_store* s, uint32_t id, const dsd_source_alias_entry** any,
           const dsd_source_alias_entry** named) {
    alias_pick picks[2];
    DSD_MEMSET(picks, 0, sizeof(picks));
    char digits[DSD_ID_DECIMAL_MAX] = {0};
    const size_t len = s->pattern_count ? dsd_id_decimal(id, digits, sizeof(digits)) : 0u;
    for (size_t i = 0; i < s->count; ++i) {
        const int open_any = any && !picks[0].exact;
        const int open_named = named && !picks[1].exact;
        if (!open_any && !open_named) {
            break; /* every wanted pick has its exact row: nothing later can win */
        }
        const dsd_source_alias_entry* e = &s->entries[i];
        const int want_any = open_any;
        const int want_named = open_named && e->name[0];
        dsd_id_pattern rank;
        const dsd_id_pattern* rank_ptr = NULL;
        if (e->kind == DSD_TG_POLICY_ROW_PATTERN) {
            /* A pattern only wins where no exact or range row matched. */
            if ((!want_any || picks[0].range) && (!want_named || picks[1].range)) {
                continue;
            }
            dsd_id_pattern_from_canonical(e->pattern, &rank);
            if (!dsd_id_pattern_match_decimal(&rank, digits, len)) {
                continue;
            }
            rank_ptr = &rank;
        } else if (id < e->id_start || id > e->id_end) {
            continue;
        }
        if (want_any) {
            alias_pick_offer(&picks[0], e, rank_ptr);
        }
        if (want_named) {
            alias_pick_offer(&picks[1], e, rank_ptr);
        }
    }
    if (any) {
        *any = alias_pick_winner(&picks[0]);
    }
    if (named) {
        *named = alias_pick_winner(&picks[1]);
    }
}

int
dsd_source_alias_lookup(const dsd_state* state, uint32_t id, char* name, size_t name_sz) {
    const dsd_source_alias_store* s = alias_get(state);
    if (name && name_sz) {
        name[0] = '\0';
    }
    const dsd_source_alias_entry* best = NULL;
    if (s) {
        alias_find(s, id, NULL, &best);
    }
    if (!best) {
        return 0;
    }
    if (name && name_sz) {
        DSD_SNPRINTF(name, name_sz, "%s", best->name);
    }
    return 1;
}

int
dsd_source_policy_lookup(const dsd_state* state, uint32_t id, dsd_source_policy_match* out) {
    const dsd_source_alias_store* s = alias_get(state);
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (!s || s->policy_count == 0u) {
        return 0;
    }
    const dsd_source_alias_entry* row = NULL;
    alias_find(s, id, &row, NULL);
    if (!row || !row->has_policy) {
        return 0;
    }
    if (out) {
        out->matched = 1;
        out->entry = *row;
    }
    return 1;
}

int
dsd_source_label_lookup(const dsd_state* state, uint32_t id, char* mode, size_t mode_sz, char* name, size_t name_sz) {
    if (mode && mode_sz) {
        mode[0] = '\0';
    }
    if (name && name_sz) {
        name[0] = '\0';
    }
    const dsd_source_alias_store* s = alias_get(state);
    const dsd_source_alias_entry* winner = NULL;
    const dsd_source_alias_entry* named = NULL;
    if (s) {
        alias_find(s, id, s->policy_count ? &winner : NULL, &named);
    }
    const int radio_mode = winner && winner->has_policy && winner->mode[0] != '\0';
    /* The group list's exact row supplies whatever the radio list did not: the name, the mode, or both. */
    char group_mode[sizeof(winner->mode)] = {0};
    char group_name[DSD_SOURCE_ALIAS_NAME_MAX] = {0};
    (void)dsd_tg_policy_lookup_label(state, id, group_mode, sizeof(group_mode), group_name, sizeof(group_name));
    const char* label = named ? named->name : group_name;
    if (label[0] == '\0') {
        return 0; /* no name, no label: an unnamed row's mode is never shown on its own */
    }
    if (name && name_sz) {
        DSD_SNPRINTF(name, name_sz, "%s", label);
    }
    if (mode && mode_sz) {
        DSD_SNPRINTF(mode, mode_sz, "%s", radio_mode ? winner->mode : group_mode);
    }
    return 1;
}

int
dsd_source_alias_copy_snapshot(dsd_state* dst, const dsd_state* src) {
    if (!dst || !src) {
        return -1;
    }
    if (dst == src) {
        return 0;
    }
    const dsd_source_alias_store* s = alias_get(src);
    const dsd_source_alias_store* old = alias_get(dst);
    if (!s) {
        return dsd_source_alias_clear(dst);
    }
    if (old && old != s && old->snapshot_source_id == s->store_id && old->generation == s->generation
        && old->count == s->count) {
        return 0;
    }
    dsd_source_alias_store* clone = dsd_source_alias_store_create();
    if (clone && s->count) {
        clone->entries = alias_alloc(NULL, s->count * sizeof(*s->entries));
        if (!clone->entries) {
            dsd_source_alias_store_free(clone);
            clone = NULL;
        } else {
            DSD_MEMCPY(clone->entries, s->entries, s->count * sizeof(*s->entries));
        }
    }
    if (old == s) {
        dst->state_ext[DSD_STATE_EXT_CORE_SOURCE_ALIAS] = NULL;
        dst->state_ext_cleanup[DSD_STATE_EXT_CORE_SOURCE_ALIAS] = NULL;
    }
    if (!clone) {
        return -1;
    }
    clone->count = clone->capacity = s->count;
    clone->pattern_count = s->pattern_count;
    clone->policy_count = s->policy_count;
    clone->generation = s->generation;
    clone->snapshot_source_id = s->store_id;
    dsd_source_alias_install(dst, clone);
    return 0;
}
