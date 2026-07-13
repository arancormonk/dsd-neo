// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(performance-no-int-to-ptr)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <atomic>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/input_ring.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "dsd-neo/core/safe_api.h"

extern "C" volatile uint8_t exitflag;

extern "C" int
dsd_rtl_stream_should_exit(void) {
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_size(const char* label, size_t got, size_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%zu want=%zu\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_float(const char* label, float got, float want) {
    float diff = got - want;
    if (diff < -1.0e-6f || diff > 1.0e-6f) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%f want=%f\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
test_guard_and_wrap_contracts(void) {
    int rc = 0;
    float out[8] = {0.0f};
    float* p1 = reinterpret_cast<float*>(static_cast<uintptr_t>(1U));
    float* p2 = reinterpret_cast<float*>(static_cast<uintptr_t>(2U));
    size_t n1 = 9U;
    size_t n2 = 10U;

    input_ring_destroy(NULL);
    input_ring_enable_space_notify(NULL, 1);

    struct input_ring_state ring;
    DSD_MEMSET(&ring, 0, sizeof(ring));
    input_ring_read_commit(NULL, 1U);
    input_ring_read_commit(&ring, 1U);

    rc |= expect_int("init guard ring", input_ring_init(&ring, 8U), 0);
    input_ring_enable_space_notify(&ring, 2);
    rc |= expect_int("space notify enabled", ring.space_notify_enabled.load(), 1);
    input_ring_enable_space_notify(&ring, 0);
    rc |= expect_int("space notify disabled", ring.space_notify_enabled.load(), 0);

    input_ring_commit(&ring, 0U);
    rc |= expect_size("commit zero keeps empty", input_ring_used(&ring), 0U);
    ring.head.store(6U);
    ring.tail.store(4U);
    rc |= expect_int("reserve wrap", input_ring_reserve(&ring, 4U, &p1, &n1, &p2, &n2), 4);
    rc |= expect_int("reserve wrap p1", p1 == ring.buffer + 6U, 1);
    rc |= expect_size("reserve wrap n1", n1, 2U);
    rc |= expect_int("reserve wrap p2", p2 == ring.buffer, 1);
    rc |= expect_size("reserve wrap n2", n2, 2U);

    ring.tail.store(0U);
    ring.head.store(0U);
    ring.head.store(6U);
    ring.tail.store(5U);
    p1 = NULL;
    p2 = NULL;
    n1 = 0U;
    n2 = 0U;
    rc |= expect_int("reserve exact end", input_ring_reserve(&ring, 2U, &p1, &n1, &p2, &n2), 2);
    if (p1 && n1 == 2U) {
        p1[0] = 30.0f;
        p1[1] = 31.0f;
    }
    input_ring_commit(&ring, 2U);
    rc |= expect_size("commit exact end wraps head", ring.head.load(), 0U);

    ring.tail.store(0U);
    ring.head.store(0U);
    ring.buffer[6] = 40.0f;
    ring.buffer[7] = 41.0f;
    ring.head.store(0U);
    ring.tail.store(6U);
    rc |= expect_int("read block exact end", input_ring_read_block(&ring, out, 2U), 2);
    rc |= expect_float("read block exact end out[0]", out[0], 40.0f);
    rc |= expect_float("read block exact end out[1]", out[1], 41.0f);
    rc |= expect_size("read block exact end wraps tail", ring.tail.load(), 0U);

    ring.tail.store(0U);
    ring.head.store(0U);
    rc |= expect_int("read block zero", input_ring_read_block(&ring, out, 0U), 0);
    exitflag = 1U;
    rc |= expect_int("read block exit", input_ring_read_block(&ring, out, 1U), -1);
    exitflag = 0U;
    rc |= expect_int("read reserve null ring", input_ring_read_reserve(NULL, 1U, &p1, &n1, &p2, &n2), -1);
    rc |= expect_int("read reserve null p1", input_ring_read_reserve(&ring, 1U, NULL, &n1, &p2, &n2), -1);
    rc |= expect_int("read reserve null n1", input_ring_read_reserve(&ring, 1U, &p1, NULL, &p2, &n2), -1);
    rc |= expect_int("read reserve null p2", input_ring_read_reserve(&ring, 1U, &p1, &n1, NULL, &n2), -1);
    rc |= expect_int("read reserve null n2", input_ring_read_reserve(&ring, 1U, &p1, &n1, &p2, NULL), -1);
    p1 = reinterpret_cast<float*>(static_cast<uintptr_t>(1U));
    p2 = reinterpret_cast<float*>(static_cast<uintptr_t>(2U));
    n1 = 9U;
    n2 = 10U;
    rc |= expect_int("read reserve zero", input_ring_read_reserve(&ring, 0U, &p1, &n1, &p2, &n2), 0);
    rc |= expect_int("read reserve zero p1", p1 == NULL, 1);
    rc |= expect_size("read reserve zero n1", n1, 0U);
    rc |= expect_int("read reserve zero p2", p2 == NULL, 1);
    rc |= expect_size("read reserve zero n2", n2, 0U);

    float write_full[7] = {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};
    p1 = NULL;
    p2 = NULL;
    n1 = 0U;
    n2 = 0U;
    rc |= expect_int("reserve full ring", input_ring_reserve(&ring, 7U, &p1, &n1, &p2, &n2), 7);
    if (p1 && n1 == 7U) {
        DSD_MEMCPY(p1, write_full, sizeof write_full);
    }
    input_ring_commit(&ring, 7U);
    rc |= expect_size("full ring used", input_ring_used(&ring), 7U);
    rc |= expect_int("full ring grants no write span", input_ring_reserve(&ring, 1U, &p1, &n1, &p2, &n2), 0);

    input_ring_read_commit(&ring, 99U);
    rc |= expect_size("overconsume clamps to available", input_ring_used(&ring), 0U);

    input_ring_enable_space_notify(&ring, 1);
    rc |= expect_int("reserve two", input_ring_reserve(&ring, 2U, &p1, &n1, &p2, &n2), 2);
    if (p1 && n1 > 0U) {
        DSD_MEMCPY(p1, write_full, n1 * sizeof(float));
    }
    if (p2 && n2 > 0U) {
        DSD_MEMCPY(p2, write_full + n1, n2 * sizeof(float));
    }
    input_ring_commit(&ring, 2U);
    rc |= expect_int("read block two", input_ring_read_block(&ring, out, 2U), 2);
    rc |= expect_float("read block out[0]", out[0], 0.0f);
    rc |= expect_float("read block out[1]", out[1], 1.0f);
    rc |= expect_size("read block consumed", input_ring_used(&ring), 0U);

    input_ring_destroy(&ring);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_guard_and_wrap_contracts();

    rc |= expect_int("init(NULL, 8)", input_ring_init(NULL, 8), -1);

    struct input_ring_state ring;
    DSD_MEMSET(&ring, 0, sizeof(ring));

    rc |= expect_int("init(&ring, 0)", input_ring_init(&ring, 0), -1);

    rc |= expect_int("init(&ring, 8)", input_ring_init(&ring, 8), 0);
    rc |= expect_size("used after init", input_ring_used(&ring), 0U);
    rc |= expect_size("free after init", input_ring_free(&ring), 7U);
    input_ring_destroy(&ring);

    rc |= expect_int("re-init(&ring, 8)", input_ring_init(&ring, 8), 0);
    rc |= expect_size("used after re-init", input_ring_used(&ring), 0U);
    rc |= expect_size("free after re-init", input_ring_free(&ring), 7U);
    uint64_t discard_generation = input_ring_discard_generation(&ring);
    rc |= expect_int("initial discard generation", discard_generation == 0U, 1);
    input_ring_request_discard(&ring);
    rc |= expect_int("discard generation advanced", input_ring_discard_generation(&ring) == discard_generation + 1U, 1);
    rc |=
        expect_int("old discard generation stale", input_ring_discard_generation_matches(&ring, discard_generation), 0);

    float* p1 = NULL;
    float* p2 = NULL;
    size_t n1 = 0;
    size_t n2 = 0;
    discard_generation = input_ring_discard_generation(&ring);
    rc |= expect_int("reserve four", input_ring_reserve(&ring, 4U, &p1, &n1, &p2, &n2), 4);
    if (p1 && n1 >= 4U) {
        p1[0] = 1.0f;
        p1[1] = 2.0f;
        p1[2] = 3.0f;
        p1[3] = 4.0f;
    }
    input_ring_request_discard(&ring);
    if (input_ring_discard_generation_matches(&ring, discard_generation)) {
        input_ring_commit(&ring, 4U);
    }
    rc |= expect_size("stale reservation not committed", input_ring_used(&ring), 0U);

    float write_a[6] = {10.0f, 11.0f, 12.0f, 13.0f, 14.0f, 15.0f};
    rc |= expect_int("reserve initial six", input_ring_reserve(&ring, 6U, &p1, &n1, &p2, &n2), 6);
    if (p1 && n1 == 6U) {
        DSD_MEMCPY(p1, write_a, sizeof write_a);
    }
    input_ring_commit(&ring, 6U);
    float* rp1 = NULL;
    float* rp2 = NULL;
    size_t rn1 = 0;
    size_t rn2 = 0;
    rc |= expect_int("read reserve four", input_ring_read_reserve(&ring, 4U, &rp1, &rn1, &rp2, &rn2), 4);
    rc |= expect_size("read reserve four n1", rn1, 4U);
    rc |= expect_size("read reserve four n2", rn2, 0U);
    if (rp1 && rn1 >= 4U) {
        rc |= expect_float("read reserve p1[0]", rp1[0], 10.0f);
        rc |= expect_float("read reserve p1[3]", rp1[3], 13.0f);
    }
    rc |= expect_size("used before read commit", input_ring_used(&ring), 6U);
    input_ring_read_commit(&ring, 4U);
    rc |= expect_size("used after read commit", input_ring_used(&ring), 2U);

    float write_b[5] = {20.0f, 21.0f, 22.0f, 23.0f, 24.0f};
    p1 = NULL;
    p2 = NULL;
    n1 = 0U;
    n2 = 0U;
    rc |= expect_int("reserve wrapped five", input_ring_reserve(&ring, 5U, &p1, &n1, &p2, &n2), 5);
    if (p1 && n1 > 0U) {
        DSD_MEMCPY(p1, write_b, n1 * sizeof(float));
    }
    if (p2 && n2 > 0U) {
        DSD_MEMCPY(p2, write_b + n1, n2 * sizeof(float));
    }
    input_ring_commit(&ring, 5U);
    rp1 = NULL;
    rp2 = NULL;
    rn1 = 0;
    rn2 = 0;
    rc |= expect_int("read reserve wrapped", input_ring_read_reserve(&ring, 7U, &rp1, &rn1, &rp2, &rn2), 7);
    rc |= expect_size("read reserve wrapped n1", rn1, 4U);
    rc |= expect_size("read reserve wrapped n2", rn2, 3U);
    if (rp1 && rn1 >= 4U && rp2 && rn2 >= 3U) {
        rc |= expect_float("read reserve wrapped p1[0]", rp1[0], 14.0f);
        rc |= expect_float("read reserve wrapped p1[1]", rp1[1], 15.0f);
        rc |= expect_float("read reserve wrapped p1[2]", rp1[2], 20.0f);
        rc |= expect_float("read reserve wrapped p1[3]", rp1[3], 21.0f);
        rc |= expect_float("read reserve wrapped p2[0]", rp2[0], 22.0f);
        rc |= expect_float("read reserve wrapped p2[2]", rp2[2], 24.0f);
    }
    input_ring_read_commit(&ring, 7U);
    rc |= expect_size("used after wrapped commit", input_ring_used(&ring), 0U);

    dsd_mutex_lock(&ring.ready_m);
    uint64_t wait_start_ns = dsd_time_monotonic_ns();
    uint64_t deadline_ns = wait_start_ns + 20ULL * 1000000ULL;
    int wait_rc = dsd_cond_timedwait_monotonic(&ring.space, &ring.ready_m, deadline_ns);
    uint64_t waited_ns = dsd_time_monotonic_ns() - wait_start_ns;
    dsd_mutex_unlock(&ring.ready_m);

    rc |= expect_int("space monotonic timedwait timeout", wait_rc, ETIMEDOUT);
    rc |= expect_int("space monotonic wait not immediate", waited_ns >= 5ULL * 1000000ULL, 1);

    input_ring_destroy(&ring);

    return rc ? 1 : 0;
}

// NOLINTEND(performance-no-int-to-ptr)
