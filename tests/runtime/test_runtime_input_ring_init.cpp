// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/input_ring.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern "C" int
dsd_rtl_stream_should_exit(void) {
    return 0;
}

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_size(const char* label, size_t got, size_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%zu want=%zu\n", label, got, want);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    rc |= expect_int("init(NULL, 8)", input_ring_init(NULL, 8), -1);

    struct input_ring_state ring;
    memset(&ring, 0, sizeof(ring));

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
