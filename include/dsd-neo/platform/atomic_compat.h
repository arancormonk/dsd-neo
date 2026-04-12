// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#pragma once

/**
 * @file
 * @brief Cross-platform atomics wrapper for C sources.
 *
 * MSVC's <stdatomic.h> may hard-error when C11 atomics are unavailable
 * (vcruntime_c11_stdatomic.h: "C atomic support is not enabled"). Provide a
 * small fallback implementation for the atomics used by dsd-neo.
 */

#include <dsd-neo/platform/platform.h>
#include <stdint.h>

#if DSD_COMPILER_MSVC && !DSD_COMPILER_CLANG && defined(__STDC_NO_ATOMICS__)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

typedef volatile LONG atomic_int;

static inline int
atomic_load(const atomic_int* obj) {
    return (int)InterlockedCompareExchange((volatile LONG*)obj, 0, 0);
}

static inline void
atomic_store(atomic_int* obj, int desired) {
    (void)InterlockedExchange((volatile LONG*)obj, (LONG)desired);
}

static inline int
atomic_exchange(atomic_int* obj, int desired) {
    return (int)InterlockedExchange((volatile LONG*)obj, (LONG)desired);
}

static inline int
atomic_fetch_add(atomic_int* obj, int arg) {
    return (int)InterlockedExchangeAdd((volatile LONG*)obj, (LONG)arg);
}

static inline int
atomic_compare_exchange_strong(atomic_int* obj, int* expected, int desired) {
    LONG old = InterlockedCompareExchange((volatile LONG*)obj, (LONG)desired, (LONG)*expected);
    if (old == (LONG)*expected) {
        return 1;
    }
    *expected = (int)old;
    return 0;
}

typedef struct dsd_atomic_u64 {
    volatile LONG64 v;
} dsd_atomic_u64;

static inline void
dsd_atomic_u64_init(dsd_atomic_u64* a, uint64_t initial) {
    if (!a) {
        return;
    }
    (void)InterlockedExchange64(&a->v, (LONG64)initial);
}

static inline uint64_t
dsd_atomic_u64_load_relaxed(const dsd_atomic_u64* a) {
    if (!a) {
        return 0;
    }
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64*)&a->v, 0, 0);
}

static inline void
dsd_atomic_u64_store_relaxed(dsd_atomic_u64* a, uint64_t v) {
    if (!a) {
        return;
    }
    (void)InterlockedExchange64(&a->v, (LONG64)v);
}

static inline uint64_t
dsd_atomic_u64_fetch_add_relaxed(dsd_atomic_u64* a, uint64_t delta) {
    if (!a) {
        return 0;
    }
    return (uint64_t)InterlockedExchangeAdd64(&a->v, (LONG64)delta);
}

static inline uint64_t
dsd_atomic_u64_load_acquire(const dsd_atomic_u64* a) {
    return dsd_atomic_u64_load_relaxed(a);
}

static inline void
dsd_atomic_u64_store_release(dsd_atomic_u64* a, uint64_t v) {
    dsd_atomic_u64_store_relaxed(a, v);
}

static inline uint64_t
dsd_atomic_u64_fetch_add_release(dsd_atomic_u64* a, uint64_t delta) {
    return dsd_atomic_u64_fetch_add_relaxed(a, delta);
}

#else

#include <stdatomic.h> // IWYU pragma: export

typedef struct dsd_atomic_u64 {
    _Atomic(uint64_t) v;
} dsd_atomic_u64;

static inline void
dsd_atomic_u64_init(dsd_atomic_u64* a, uint64_t initial) {
    if (!a) {
        return;
    }
    atomic_init(&a->v, initial);
}

static inline uint64_t
dsd_atomic_u64_load_relaxed(const dsd_atomic_u64* a) {
    if (!a) {
        return 0;
    }
    return atomic_load_explicit(&a->v, memory_order_relaxed);
}

static inline void
dsd_atomic_u64_store_relaxed(dsd_atomic_u64* a, uint64_t v) {
    if (!a) {
        return;
    }
    atomic_store_explicit(&a->v, v, memory_order_relaxed);
}

static inline uint64_t
dsd_atomic_u64_fetch_add_relaxed(dsd_atomic_u64* a, uint64_t delta) {
    if (!a) {
        return 0;
    }
    return atomic_fetch_add_explicit(&a->v, delta, memory_order_relaxed);
}

static inline uint64_t
dsd_atomic_u64_load_acquire(const dsd_atomic_u64* a) {
    if (!a) {
        return 0;
    }
    return atomic_load_explicit(&a->v, memory_order_acquire);
}

static inline void
dsd_atomic_u64_store_release(dsd_atomic_u64* a, uint64_t v) {
    if (!a) {
        return;
    }
    atomic_store_explicit(&a->v, v, memory_order_release);
}

static inline uint64_t
dsd_atomic_u64_fetch_add_release(dsd_atomic_u64* a, uint64_t delta) {
    if (!a) {
        return 0;
    }
    return atomic_fetch_add_explicit(&a->v, delta, memory_order_release);
}

#endif
