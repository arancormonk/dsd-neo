// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief Source aliases: CSV grammar `id,name[,tags]`, decimal uint32_t ID or inclusive
 * `start-end` range. See docs/csv-formats.md, "Source ID List CSV".
 */
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/source_alias.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <stdint.h>
#include <stdlib.h>

struct dsd_source_alias_store {
    dsd_source_alias_entry* entries;
    size_t count, capacity;
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
    DSD_MEMSET(s, 0, sizeof(*s));
    do {
        s->store_id = dsd_atomic_u64_fetch_add_relaxed(&next_store_id, 1u);
    } while (!s->store_id);
    return s;
}

int
dsd_source_alias_store_append(dsd_source_alias_store* store, const dsd_source_alias_entry* entry) {
    if (!store || !entry || !entry->name[0] || entry->id_start > entry->id_end
        || (!entry->is_range && entry->id_start != entry->id_end)) {
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
    store->entries[store->count] = *entry;
    store->entries[store->count++].name[DSD_SOURCE_ALIAS_NAME_MAX - 1] = '\0';
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

int
dsd_source_alias_lookup(const dsd_state* state, uint32_t id, char* name, size_t name_sz) {
    const dsd_source_alias_store* s = alias_get(state);
    const dsd_source_alias_entry* best = NULL;
    if (name && name_sz) {
        name[0] = '\0';
    }
    if (!s) {
        return 0;
    }
    for (size_t i = 0; i < s->count; ++i) {
        const dsd_source_alias_entry* e = &s->entries[i];
        if (id < e->id_start || id > e->id_end) {
            continue;
        }
        if (!best || (best->is_range && (!e->is_range || e->id_end - e->id_start < best->id_end - best->id_start))) {
            best = e;
        }
        if (!best->is_range) {
            break;
        }
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
dsd_source_label_lookup(const dsd_state* state, uint32_t id, char* mode, size_t mode_sz, char* name, size_t name_sz) {
    if (mode && mode_sz) {
        mode[0] = '\0';
    }
    if (name && name_sz) {
        name[0] = '\0';
    }
    if (dsd_source_alias_lookup(state, id, name, name_sz)) {
        (void)dsd_tg_policy_lookup_label(state, id, mode, mode_sz, NULL, 0);
        return 1;
    }
    return dsd_tg_policy_lookup_label(state, id, mode, mode_sz, name, name_sz);
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
        (void)dsd_source_alias_clear(dst);
        return -1;
    }
    clone->count = clone->capacity = s->count;
    clone->generation = s->generation;
    clone->snapshot_source_id = s->store_id;
    dsd_source_alias_install(dst, clone);
    return 0;
}
