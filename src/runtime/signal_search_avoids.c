// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The Signal search avoid lists (issue #828): one permanent list the app-control store writes and one session list the
   engine coordinator writes, each read whole under one lock by the search and the screens alike. */
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_avoids.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t frequencies[DSD_SEARCH_MAX_AVOIDS];
    size_t count;
    /* Written under avoids_mu, so a snapshot's list and revision always belong together; read without it by the
       revision checks, which then take a snapshot. */
    dsd_atomic_u64 revision;
} avoid_list;

static dsd_mutex_t avoids_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready, -1 = failed. */
static atomic_int avoids_mu_state;
static avoid_list permanent_list, session_list;
static uint64_t session_owner;

/* Whether the lock is usable. A lock that failed to initialise stays unusable: nothing is set through it and both
   lists read empty, as they start. */
static int
avoids_mu_ready(void) {
    int state = atomic_load(&avoids_mu_state);
    if (state == 2 || state == -1) {
        return state == 2;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&avoids_mu_state, &expected, 1)) {
        const int ready = dsd_mutex_init(&avoids_mu) == 0;
        atomic_store(&avoids_mu_state, ready ? 2 : -1);
        return ready;
    }
    while ((state = atomic_load(&avoids_mu_state)) == 1) {
        dsd_thread_yield();
    }
    return state == 2;
}

static int
avoids_lock(void) {
    return avoids_mu_ready() && dsd_mutex_lock(&avoids_mu) == 0;
}

enum {
    /* Open-addressed set of the frequencies kept so far, at most half full; 0, never kept, marks an empty slot. 2^11
       slots, indexed by an 11-bit hash. */
    AVOIDS_SEEN_SLOTS = 2 * DSD_SEARCH_MAX_AVOIDS
};

_Static_assert(AVOIDS_SEEN_SLOTS == 1 << 11, "the hash gives an 11-bit slot");

/* Add @p frequency to @p seen; 0 when it was already there. */
static int
avoids_seen_add(uint32_t* seen, uint32_t frequency) {
    /* Fibonacci hashing: the top 11 bits of the 32-bit product spread channel-step multiples over every slot. */
    size_t slot = (size_t)((((uint64_t)frequency * 2654435761U) & 0xFFFFFFFFU) >> 21);
    while (seen[slot] != 0) {
        if (seen[slot] == frequency) {
            return 0;
        }
        slot = (slot + 1U) % AVOIDS_SEEN_SLOTS;
    }
    seen[slot] = frequency;
    return 1;
}

/* @p list without 0 or repeats, in order, at most DSD_SEARCH_MAX_AVOIDS of it; returns the count. Exact Hz, as every
   avoid is matched. Linear in @p n: the engine publishes its session list at every change. */
static size_t
avoids_normalize(const uint32_t* list, size_t n, uint32_t* out) {
    uint32_t seen[AVOIDS_SEEN_SLOTS];
    DSD_MEMSET(seen, 0, sizeof(seen));
    size_t count = 0;
    for (size_t i = 0; list && i < n && count < DSD_SEARCH_MAX_AVOIDS; ++i) {
        if (list[i] != 0 && avoids_seen_add(seen, list[i])) {
            out[count++] = list[i];
        }
    }
    return count;
}

static int
avoids_same(const avoid_list* list, const uint32_t* frequencies, size_t count) {
    if (list->count != count) {
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (list->frequencies[i] != frequencies[i]) {
            return 0;
        }
    }
    return 1;
}

/* Under avoids_mu: replace @p list, moving its revision only on a change; 1 when it changed. */
static int
avoids_replace(avoid_list* list, const uint32_t* frequencies, size_t count) {
    if (avoids_same(list, frequencies, count)) {
        return 0;
    }
    if (count) {
        DSD_MEMCPY(list->frequencies, frequencies, count * sizeof(frequencies[0]));
    }
    list->count = count;
    return 1;
}

static void
avoids_bump(avoid_list* list) {
    dsd_atomic_u64_store_release(&list->revision, dsd_atomic_u64_load_relaxed(&list->revision) + 1U);
}

/* Under avoids_mu: copy up to @p cap of @p list to @p out; returns the number copied. */
static size_t
avoids_copy(const avoid_list* list, uint32_t* out, size_t cap) {
    const size_t n = out ? (list->count < cap ? list->count : cap) : 0;
    if (n) {
        DSD_MEMCPY(out, list->frequencies, n * sizeof(out[0]));
    }
    return n;
}

void
dsd_signal_search_avoids_permanent_set(const uint32_t* list, size_t n) {
    uint32_t next[DSD_SEARCH_MAX_AVOIDS];
    const size_t count = avoids_normalize(list, n, next);
    if (!avoids_lock()) {
        return;
    }
    if (avoids_replace(&permanent_list, next, count)) {
        avoids_bump(&permanent_list);
    }
    dsd_mutex_unlock(&avoids_mu);
}

size_t
dsd_signal_search_avoids_permanent_snapshot(uint32_t* out, size_t cap, uint64_t* revision) {
    size_t n = 0;
    uint64_t at = 0;
    if (avoids_lock()) {
        n = avoids_copy(&permanent_list, out, cap);
        at = dsd_atomic_u64_load_relaxed(&permanent_list.revision);
        dsd_mutex_unlock(&avoids_mu);
    }
    if (revision) {
        *revision = at;
    }
    return n;
}

uint64_t
dsd_signal_search_avoids_permanent_revision(void) {
    return dsd_atomic_u64_load_acquire(&permanent_list.revision);
}

void
dsd_signal_search_avoids_session_set(uint64_t session_id, const uint32_t* list, size_t n) {
    uint32_t next[DSD_SEARCH_MAX_AVOIDS];
    const size_t count = session_id ? avoids_normalize(list, n, next) : 0;
    if (!avoids_lock()) {
        return;
    }
    /* The list is replaced first, then the owner: either change moves the revision. */
    const int changed = avoids_replace(&session_list, next, count) || session_owner != session_id;
    session_owner = session_id;
    if (changed) {
        avoids_bump(&session_list);
    }
    dsd_mutex_unlock(&avoids_mu);
}

size_t
dsd_signal_search_avoids_session_get(uint64_t* session_id, uint32_t* out, size_t cap) {
    size_t n = 0;
    uint64_t owner = 0;
    if (avoids_lock()) {
        n = avoids_copy(&session_list, out, cap);
        owner = session_owner;
        dsd_mutex_unlock(&avoids_mu);
    }
    if (session_id) {
        *session_id = owner;
    }
    return n;
}

uint64_t
dsd_signal_search_avoids_session_revision(void) {
    return dsd_atomic_u64_load_acquire(&session_list.revision);
}
