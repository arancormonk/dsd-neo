// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Feature: rtl-tcp-lag-resilience, Property 2: Concealment state reset on good buffer */

/**
 * @file
 * @brief Exhaustive example-based tests for concealment state reset.
 *
 * Property 2: For any concealment state with repeat_count > 0 (after one or
 * more underruns), calling audio_conceal_on_good_buffer() with any valid
 * buffer SHALL reset repeat_count to 0, so that the next underrun starts
 * from repeat 1 (attenuation = 0.5^1, not compounded from the prior sequence).
 *
 * **Validates: Requirements 1.4**
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
expect_int(const char* label, int got, int want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_i16(const char* label, int16_t got, int16_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%d want=%d\n", label, (int)got, (int)want);
        return 1;
    }
    return 0;
}

/**
 * @brief Compute the expected attenuated sample for repeat k.
 */
static int16_t
expected_sample(int16_t original, int k) {
    float gain = 1.0f;
    for (int i = 0; i < k; i++) {
        gain *= AUDIO_CONCEAL_ATTEN_PER_REPEAT;
    }
    float sample = (float)original * gain;
    if (sample > 32767.0f) {
        sample = 32767.0f;
    }
    if (sample < -32768.0f) {
        sample = -32768.0f;
    }
    return (int16_t)sample;
}

/**
 * @brief After 1 underrun, call good_buffer, verify next underrun uses k=1.
 */
static int
test_reset_after_one_underrun(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    /* Set up a known good buffer (constant value 1000). */
    int16_t buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        buf[i] = 1000;
    }
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);

    /* Trigger 1 underrun (k=1). */
    int16_t out[SAMPLES];
    audio_conceal_on_underrun(&cs, out, FRAMES);
    rc |= expect_int("repeat_count after 1 underrun", cs.repeat_count, 1);

    /* Feed a new good buffer (constant value 2000). */
    int16_t new_buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        new_buf[i] = 2000;
    }
    audio_conceal_on_good_buffer(&cs, new_buf, FRAMES);

    rc |= expect_int("repeat_count after good_buffer", cs.repeat_count, 0);

    /* Next underrun should use k=1 attenuation on the NEW buffer. */
    memset(out, 0xAA, sizeof(out));
    audio_conceal_on_underrun(&cs, out, FRAMES);

    int16_t want = expected_sample(2000, 1);
    rc |= expect_i16("sample[0] after reset (k=1)", out[0], want);
    rc |= expect_i16("sample[127] after reset (k=1)", out[127], want);

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief After max_repeats underruns, call good_buffer, verify next underrun
 *        uses k=1 (not silence, not compounded).
 */
static int
test_reset_after_max_underruns(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    /* Set up a known good buffer. */
    int16_t buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        buf[i] = 5000;
    }
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);

    /* Exhaust all repeats. */
    int16_t out[SAMPLES];
    for (int k = 0; k < AUDIO_CONCEAL_MAX_REPEATS; k++) {
        audio_conceal_on_underrun(&cs, out, FRAMES);
    }
    rc |= expect_int("repeat_count at max", cs.repeat_count, AUDIO_CONCEAL_MAX_REPEATS);

    /* Feed a new good buffer (constant value 8000). */
    int16_t new_buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        new_buf[i] = 8000;
    }
    audio_conceal_on_good_buffer(&cs, new_buf, FRAMES);

    rc |= expect_int("repeat_count after good_buffer", cs.repeat_count, 0);

    /* Next underrun should use k=1 attenuation on the NEW buffer. */
    memset(out, 0xAA, sizeof(out));
    audio_conceal_on_underrun(&cs, out, FRAMES);

    int16_t want = expected_sample(8000, 1);
    rc |= expect_i16("sample[0] after max reset (k=1)", out[0], want);

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief Verify that good_buffer resets repeat_count to 0 directly.
 */
static int
test_repeat_count_is_zero_after_good_buffer(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        buf[i] = 100;
    }
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);

    /* Trigger 3 underruns. */
    int16_t out[SAMPLES];
    for (int k = 0; k < 3; k++) {
        audio_conceal_on_underrun(&cs, out, FRAMES);
    }
    rc |= expect_int("repeat_count after 3 underruns", cs.repeat_count, 3);

    /* Good buffer resets. */
    audio_conceal_on_good_buffer(&cs, buf, FRAMES);
    rc |= expect_int("repeat_count after good_buffer", cs.repeat_count, 0);

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief Multiple reset cycles: underrun → good → underrun → good.
 *        Each cycle should restart attenuation from k=1.
 */
static int
test_multiple_reset_cycles(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t out[SAMPLES];

    for (int cycle = 0; cycle < 5; cycle++) {
        /* Feed a good buffer with a distinct value per cycle. */
        int16_t buf[SAMPLES];
        int16_t val = (int16_t)(1000 * (cycle + 1));
        for (int i = 0; i < SAMPLES; i++) {
            buf[i] = val;
        }
        audio_conceal_on_good_buffer(&cs, buf, FRAMES);

        rc |= expect_int("repeat_count reset", cs.repeat_count, 0);

        /* Trigger 2 underruns. */
        audio_conceal_on_underrun(&cs, out, FRAMES);
        audio_conceal_on_underrun(&cs, out, FRAMES);

        /* Verify k=2 attenuation on this cycle's buffer. */
        int16_t want = expected_sample(val, 2);
        if (out[0] != want) {
            fprintf(stderr, "FAIL: cycle=%d k=2 sample[0]: got=%d want=%d\n", cycle, (int)out[0], (int)want);
            rc = 1;
        }
    }

    audio_conceal_destroy(&cs);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_reset_after_one_underrun();
    rc |= test_reset_after_max_underruns();
    rc |= test_repeat_count_is_zero_after_good_buffer();
    rc |= test_multiple_reset_cycles();

    return rc ? 1 : 0;
}
