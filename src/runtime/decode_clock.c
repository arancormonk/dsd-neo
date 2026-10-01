// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stdint.h>
#include <time.h>

#define DSD_DECODE_CLOCK_NS_PER_S 1000000000ULL

static atomic_int g_source = DSD_DECODE_CLOCK_SYSTEM;
static dsd_atomic_u64 g_anchor_ns = {0U};
static dsd_atomic_u64 g_media_ns = {0U};
static dsd_atomic_u64 g_test_ns = {0U};
/* Added to the platform monotonic clock under SYSTEM once a REPLAY source has been left, so decode-mono time goes on
   from the capture time the replay reached instead of jumping back to the platform clock's origin. Zero in a process
   that never left REPLAY, where SYSTEM reads exactly the platform clocks. */
static dsd_atomic_u64 g_system_mono_offset_ns = {0U};

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

/* The platform monotonic clock plus the offset a left replay set, saturating. */
static uint64_t
system_mono_ns(uint64_t offset_ns) {
    const uint64_t platform_ns = dsd_time_monotonic_ns();
    return offset_ns > UINT64_MAX - platform_ns ? UINT64_MAX : platform_ns + offset_ns;
}

uint64_t
dsd_decode_now_mono_ns(void) {
    int s = atomic_load(&g_source);
    if (s == DSD_DECODE_CLOCK_SYSTEM) {
        const uint64_t offset_ns = dsd_atomic_u64_load_acquire(&g_system_mono_offset_ns);
        return offset_ns == 0U ? dsd_time_monotonic_ns() : system_mono_ns(offset_ns);
    }
    return decode_virtual_ns(s);
}

uint64_t
dsd_decode_now_mono_ms(void) {
    int s = atomic_load(&g_source);
    if (s == DSD_DECODE_CLOCK_SYSTEM) {
        const uint64_t offset_ns = dsd_atomic_u64_load_acquire(&g_system_mono_offset_ns);
        return offset_ns == 0U ? dsd_time_monotonic_ms() : system_mono_ns(offset_ns) / 1000000ULL;
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
    /* Leaving REPLAY: SYSTEM mono goes on from where the replay's capture time stands. The offset is stored before the
       source, so a reader that sees SYSTEM sees it. Leaving TEST (or SYSTEM) sets nothing, so no test value reaches a
       later SYSTEM read. */
    if (atomic_load(&g_source) == DSD_DECODE_CLOCK_REPLAY) {
        const uint64_t replay_ns = decode_virtual_ns(DSD_DECODE_CLOCK_REPLAY);
        const uint64_t platform_ns = dsd_time_monotonic_ns();
        dsd_atomic_u64_store_release(&g_system_mono_offset_ns, replay_ns > platform_ns ? replay_ns - platform_ns : 0U);
    }
    atomic_store(&g_source, DSD_DECODE_CLOCK_SYSTEM);
}

void
dsd_decode_clock_use_replay(int64_t anchor_utc_s) {
    int64_t a = anchor_utc_s < DSD_DECODE_CLOCK_ANCHOR_FLOOR_S ? DSD_DECODE_CLOCK_ANCHOR_FLOOR_S : anchor_utc_s;
    /* Keep anchor_ns inside uint64 range for absurd inputs. */
    if (a > DSD_DECODE_CLOCK_ANCHOR_MAX_S) {
        a = DSD_DECODE_CLOCK_ANCHOR_MAX_S;
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

uint64_t
dsd_decode_clock_batch_media_ns(uint64_t start_ns, uint64_t duration_ns, uint32_t count, uint32_t index) {
    if (count == 0U) {
        return start_ns;
    }
    if (index > count) {
        index = count;
    }
    /* index * duration / count, split so no product leaves 64 bits: duration = q * count + r with r < count, so
       index * duration / count = index * q + (index * r) / count exactly, index * q <= duration (index <= count) and
       index * r < 2^32 * 2^32. */
    const uint64_t q = duration_ns / count;
    const uint64_t r = duration_ns % count;
    const uint64_t offset = q * index + (r * index) / count;
    return offset > UINT64_MAX - start_ns ? UINT64_MAX : start_ns + offset;
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
