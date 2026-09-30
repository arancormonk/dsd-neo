// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <time.h>

#define DSD_DECODE_CLOCK_NS_PER_S       1000000000ULL
/* 2000-01-01T00:00:00Z */
#define DSD_DECODE_CLOCK_ANCHOR_FLOOR_S 946684800LL

static atomic_int g_source = DSD_DECODE_CLOCK_SYSTEM;
static dsd_atomic_u64 g_anchor_ns = {0U};
static dsd_atomic_u64 g_media_ns = {0U};
static dsd_atomic_u64 g_test_ns = {0U};

/* Value read by REPLAY and TEST sources for both mono and wall. */
static uint64_t
decode_virtual_ns(int source) {
    if (source == DSD_DECODE_CLOCK_TEST) {
        return dsd_atomic_u64_load_acquire(&g_test_ns);
    }
    return dsd_atomic_u64_load_acquire(&g_anchor_ns) + dsd_atomic_u64_load_acquire(&g_media_ns);
}

time_t
dsd_decode_time(void) {
    int s = atomic_load(&g_source);
    if (s == DSD_DECODE_CLOCK_SYSTEM) {
        return time(NULL);
    }
    return (time_t)(decode_virtual_ns(s) / DSD_DECODE_CLOCK_NS_PER_S);
}

uint64_t
dsd_decode_now_mono_ns(void) {
    int s = atomic_load(&g_source);
    if (s == DSD_DECODE_CLOCK_SYSTEM) {
        return dsd_time_monotonic_ns();
    }
    return decode_virtual_ns(s);
}

uint64_t
dsd_decode_now_mono_ms(void) {
    int s = atomic_load(&g_source);
    if (s == DSD_DECODE_CLOCK_SYSTEM) {
        return dsd_time_monotonic_ms();
    }
    return decode_virtual_ns(s) / 1000000ULL;
}

double
dsd_decode_now_mono_s(void) {
    return (double)dsd_decode_now_mono_ns() / 1e9;
}

double
dsd_decode_now_realtime_s(void) {
    int s = atomic_load(&g_source);
    if (s == DSD_DECODE_CLOCK_SYSTEM) {
        return (double)dsd_time_realtime_ns() / 1e9;
    }
    return (double)decode_virtual_ns(s) / 1e9;
}

uint64_t
dsd_realtime_mono_ms(void) {
    return dsd_time_monotonic_ms();
}

uint64_t
dsd_realtime_mono_ns(void) {
    return dsd_time_monotonic_ns();
}

double
dsd_realtime_mono_s(void) {
    return (double)dsd_time_monotonic_ns() / 1e9;
}

time_t
dsd_realtime_time(void) {
    return time(NULL);
}

double
dsd_realtime_now_s(void) {
    return (double)dsd_time_realtime_ns() / 1e9;
}

void
dsd_decode_clock_use_system(void) {
    atomic_store(&g_source, DSD_DECODE_CLOCK_SYSTEM);
}

void
dsd_decode_clock_use_replay(int64_t anchor_utc_s) {
    int64_t a = anchor_utc_s < DSD_DECODE_CLOCK_ANCHOR_FLOOR_S ? DSD_DECODE_CLOCK_ANCHOR_FLOOR_S : anchor_utc_s;
    /* Keep anchor_ns inside uint64 range (year ~2554) for absurd inputs. */
    if (a > 18000000000LL) {
        a = 18000000000LL;
    }
    dsd_atomic_u64_store_release(&g_media_ns, 0U);
    dsd_atomic_u64_store_release(&g_anchor_ns, (uint64_t)a * DSD_DECODE_CLOCK_NS_PER_S);
    atomic_store(&g_source, DSD_DECODE_CLOCK_REPLAY);
}

void
dsd_decode_clock_set_media_ns(uint64_t media_ns) {
    /* Single writer (decoder thread): load-compare-store is race free. */
    if (media_ns > dsd_atomic_u64_load_relaxed(&g_media_ns)) {
        dsd_atomic_u64_store_release(&g_media_ns, media_ns);
    }
}

void
dsd_decode_clock_use_test(uint64_t now_ns) {
    dsd_atomic_u64_store_release(&g_test_ns, now_ns);
    atomic_store(&g_source, DSD_DECODE_CLOCK_TEST);
}

void
dsd_decode_clock_test_set_ns(uint64_t now_ns) {
    dsd_atomic_u64_store_release(&g_test_ns, now_ns);
}

dsd_decode_clock_source_t
dsd_decode_clock_source(void) {
    return (dsd_decode_clock_source_t)atomic_load(&g_source);
}
