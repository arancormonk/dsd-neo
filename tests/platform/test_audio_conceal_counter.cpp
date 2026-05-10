// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Feature: rtl-tcp-lag-resilience, Property 3: Underrun counter monotonicity */

/**
 * @file
 * @brief Exhaustive example-based tests for underrun counter monotonicity.
 *
 * Property 3: For any interleaved sequence of N good-buffer events and M
 * underrun events (in any order), the cumulative underrun_total SHALL equal M.
 * The counter SHALL never decrease and SHALL increment by exactly 1 per
 * underrun call regardless of repeat_count or whether the output is attenuated
 * or silence.
 *
 * **Validates: Requirements 1.5**
 */

#include <dsd-neo/platform/audio_concealment.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FRAMES   256
#define CHANNELS 1
#define SAMPLES  (FRAMES * CHANNELS)

/*============================================================================
 * Helpers
 *============================================================================*/

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
        return 1;
    }
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

/**
 * @brief 5 underruns in a row → underrun_total == 5.
 */
static int
test_consecutive_underruns(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t buf[SAMPLES];
    memset(buf, 0, sizeof(buf));
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);

    int16_t out[SAMPLES];
    for (int i = 0; i < 5; i++) {
        audio_conceal_on_underrun(&cs, out, FRAMES);
    }

    rc |= expect_u64("5 consecutive underruns", cs.underrun_total, 5U);

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief 3 underruns, 1 good, 2 underruns → underrun_total == 5.
 */
static int
test_interleaved_good_underrun(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        buf[i] = 1000;
    }
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);

    int16_t out[SAMPLES];

    /* 3 underruns. */
    for (int i = 0; i < 3; i++) {
        audio_conceal_on_underrun(&cs, out, FRAMES);
    }
    rc |= expect_u64("after 3 underruns", cs.underrun_total, 3U);

    /* 1 good buffer. */
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);
    rc |= expect_u64("good_buffer does not change total", cs.underrun_total, 3U);

    /* 2 more underruns. */
    for (int i = 0; i < 2; i++) {
        audio_conceal_on_underrun(&cs, out, FRAMES);
    }
    rc |= expect_u64("3 + 2 underruns", cs.underrun_total, 5U);

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief 10 good buffers, 0 underruns → underrun_total == 0.
 */
static int
test_only_good_buffers(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        buf[i] = 500;
    }

    for (int i = 0; i < 10; i++) {
        audio_conceal_on_good_buffer(&cs, buf, FRAMES);
    }

    rc |= expect_u64("10 good buffers, 0 underruns", cs.underrun_total, 0U);

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief Alternating good/underrun × 10 → underrun_total == 10.
 */
static int
test_alternating(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        buf[i] = 2000;
    }
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);

    int16_t out[SAMPLES];

    for (int i = 0; i < 10; i++) {
        /* Each cycle: 1 underrun then 1 good buffer. */
        audio_conceal_on_underrun(&cs, out, FRAMES);
        audio_conceal_on_good_buffer(&cs, buf, FRAMES);
    }

    rc |= expect_u64("alternating × 10", cs.underrun_total, 10U);

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief max_repeats+2 underruns (some past max) → underrun_total == max_repeats+2.
 *
 * Verifies that underruns past max_repeats (which produce silence) still
 * increment the counter.
 */
static int
test_past_max_repeats(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        buf[i] = 3000;
    }
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);

    int16_t out[SAMPLES];
    int total = AUDIO_CONCEAL_MAX_REPEATS + 2;

    for (int i = 0; i < total; i++) {
        uint64_t before = cs.underrun_total;
        audio_conceal_on_underrun(&cs, out, FRAMES);

        /* Counter must increment by exactly 1 each time. */
        if (cs.underrun_total != before + 1) {
            fprintf(stderr, "FAIL: underrun %d: total went from %llu to %llu (expected +1)\n", i,
                    (unsigned long long)before, (unsigned long long)cs.underrun_total);
            rc = 1;
        }
    }

    rc |= expect_u64("max_repeats+2 underruns", cs.underrun_total, (uint64_t)total);

    audio_conceal_destroy(&cs);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_consecutive_underruns();
    rc |= test_interleaved_good_underrun();
    rc |= test_only_good_buffers();
    rc |= test_alternating();
    rc |= test_past_max_repeats();

    return rc ? 1 : 0;
}
