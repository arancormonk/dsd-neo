// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/** Private ownership below the stable ENGINE_TRUNK_SCAN extension ID. */
#ifndef DSD_NEO_ENGINE_SCAN_OWNER_H
#define DSD_NEO_ENGINE_SCAN_OWNER_H
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdlib.h>

typedef struct {
    void* trunk;
    void* search;
    dsd_state_ext_cleanup_fn trunk_free, search_free;
} dsd_engine_scan_owner;

static inline void
dsd_engine_scan_owner_free(void* ptr) {
    dsd_engine_scan_owner* p = (dsd_engine_scan_owner*)ptr;
    if (p->trunk_free) {
        p->trunk_free(p->trunk);
    }
    if (p->search_free) {
        p->search_free(p->search);
    }
    free(p);
}

static inline void*
dsd_engine_scan_owned(const dsd_state* state, int search) {
    const dsd_engine_scan_owner* p =
        (const dsd_engine_scan_owner*)dsd_state_ext_get_const(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
    return p ? (search ? p->search : p->trunk) : NULL;
}

static inline int
dsd_engine_scan_own(dsd_state* state, int search, void* value, dsd_state_ext_cleanup_fn cleanup) {
    dsd_engine_scan_owner* p = DSD_STATE_EXT_GET_AS(dsd_engine_scan_owner, state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN);
    if (!p) {
        if (!value) {
            return 0;
        }
        p = (dsd_engine_scan_owner*)calloc(1, sizeof(*p));
        if (!p) {
            return -1;
        }
        if (dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_TRUNK_SCAN, p, dsd_engine_scan_owner_free)) {
            free(p);
            return -1;
        }
    }
    if (search) {
        if (p->search_free) {
            p->search_free(p->search);
        }
        p->search = value;
        p->search_free = cleanup;
    } else {
        if (p->trunk_free) {
            p->trunk_free(p->trunk);
        }
        p->trunk = value;
        p->trunk_free = cleanup;
    }
    return 0;
}
#endif
