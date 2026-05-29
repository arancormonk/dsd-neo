// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/nonce.h>
#include <dsd-neo/platform/timing.h>
#include <stddef.h>
#include <stdint.h>

static dsd_atomic_u64 g_nonce_counter = {0x9E3779B97F4A7C15ULL};

static uint64_t
nonce_mix64(uint64_t x) {
    x ^= x >> 30U;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27U;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31U;
    return x;
}

void
dsd_nonce_fill(void* out, size_t size) {
    if (!out || size == 0) {
        return;
    }

    unsigned char* dst = (unsigned char*)out;
    uintptr_t addr = (uintptr_t)&dst;
    uint64_t seed = dsd_time_monotonic_ns() ^ ((uint64_t)addr << 1U);
    seed ^= dsd_atomic_u64_fetch_add_relaxed(&g_nonce_counter, 0x9E3779B97F4A7C15ULL);

    while (size > 0) {
        seed = nonce_mix64(seed + 0xD1B54A32D192ED03ULL);
        unsigned char block[sizeof(seed)];
        DSD_MEMCPY(block, &seed, sizeof(block));
        size_t n = (size < sizeof(block)) ? size : sizeof(block);
        DSD_MEMCPY(dst, block, n);
        dst += n;
        size -= n;
    }
}

uint16_t
dsd_nonce_u16(void) {
    uint16_t value = 0;
    dsd_nonce_fill(&value, sizeof(value));
    return value;
}
