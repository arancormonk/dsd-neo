// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Feature: rtl-tcp-lag-resilience, Property 1: Concealment attenuation curve */

/**
 * @file
 * @brief Exhaustive example-based tests for the concealment attenuation curve.
 *
 * Property 1: For any audio buffer of 256 int16 frames and for any repeat
 * count k (1 ≤ k ≤ max_repeats), the output of the k-th consecutive underrun
 * SHALL be the original last-good buffer with each sample multiplied by
 * (atten_per_repeat)^k.  When k > max_repeats, the output SHALL be all zeros.
 *
 * **Validates: Requirements 1.1, 1.2, 1.3, 1.6**
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
 * @brief Fill a buffer with a specific pattern.
 *
 * @param buf     Output buffer.
 * @param pattern One of: 'Z' (zeros), 'P' (max positive), 'N' (max negative),
 *                'A' (alternating +/−), 'R' (ramp 0..SAMPLES−1).
 */
static void
fill_pattern(int16_t* buf, char pattern) {
    for (int i = 0; i < SAMPLES; i++) {
        switch (pattern) {
            case 'Z': buf[i] = 0; break;
            case 'P': buf[i] = 32767; break;
            case 'N': buf[i] = -32768; break;
            case 'A': buf[i] = (i & 1) ? (int16_t)32767 : (int16_t)-32768; break;
            case 'R': buf[i] = (int16_t)(i - SAMPLES / 2); break;
            default: buf[i] = 0; break;
        }
    }
}

/**
 * @brief Compute the expected attenuated sample value.
 *
 * Mirrors the implementation: (float)original × gain, then truncate to int16.
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
 * @brief Test the attenuation curve for a given buffer pattern.
 *
 * Feeds the pattern as a good buffer, then triggers underruns for
 * k = 1 .. max_repeats and verifies each sample matches the expected
 * attenuated value.  Then triggers one more underrun (k > max_repeats)
 * and verifies silence.
 *
 * @return 0 on success, 1 on any failure.
 */
static int
test_pattern(const char* name, char pattern) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t original[SAMPLES];
    fill_pattern(original, pattern);

    /* Feed the pattern as the last-good buffer. */
    audio_conceal_on_good_buffer(&cs, original, FRAMES);

    int16_t out[SAMPLES];

    /* Verify attenuation for each repeat k = 1 .. max_repeats. */
    for (int k = 1; k <= AUDIO_CONCEAL_MAX_REPEATS; k++) {
        memset(out, 0xAA, sizeof(out));
        size_t written = audio_conceal_on_underrun(&cs, out, FRAMES);
        rc |= expect_int("underrun frames", (int)written, FRAMES);

        for (int i = 0; i < SAMPLES; i++) {
            int16_t want = expected_sample(original[i], k);
            if (out[i] != want) {
                fprintf(stderr, "FAIL: pattern=%s k=%d sample[%d]: got=%d want=%d\n", name, k, i, (int)out[i],
                        (int)want);
                rc = 1;
                /* Report first failure per k, then move on. */
                break;
            }
        }
    }

    /* k > max_repeats: output must be silence (all zeros). */
    memset(out, 0xAA, sizeof(out));
    size_t written = audio_conceal_on_underrun(&cs, out, FRAMES);
    rc |= expect_int("silence frames", (int)written, FRAMES);

    for (int i = 0; i < SAMPLES; i++) {
        if (out[i] != 0) {
            fprintf(stderr, "FAIL: pattern=%s k=%d (silence) sample[%d]: got=%d want=0\n", name,
                    AUDIO_CONCEAL_MAX_REPEATS + 1, i, (int)out[i]);
            rc = 1;
            break;
        }
    }

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief Test that re-feeding a good buffer and triggering underruns again
 *        restarts the attenuation curve from k=1.
 */
static int
test_reset_and_re_attenuate(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    /* Use a ramp pattern. */
    int16_t original[SAMPLES];
    fill_pattern(original, 'R');
    audio_conceal_on_good_buffer(&cs, original, FRAMES);

    /* Trigger 2 underruns (k=1, k=2). */
    int16_t out[SAMPLES];
    audio_conceal_on_underrun(&cs, out, FRAMES);
    audio_conceal_on_underrun(&cs, out, FRAMES);

    /* Feed a new good buffer (max positive). */
    int16_t new_buf[SAMPLES];
    fill_pattern(new_buf, 'P');
    audio_conceal_on_good_buffer(&cs, new_buf, FRAMES);

    /* Next underrun should use k=1 attenuation on the NEW buffer. */
    memset(out, 0xAA, sizeof(out));
    audio_conceal_on_underrun(&cs, out, FRAMES);

    for (int i = 0; i < SAMPLES; i++) {
        int16_t want = expected_sample(new_buf[i], 1);
        if (out[i] != want) {
            fprintf(stderr, "FAIL: reset_re_attenuate sample[%d]: got=%d want=%d\n", i, (int)out[i], (int)want);
            rc = 1;
            break;
        }
    }

    audio_conceal_destroy(&cs);
    return rc;
}

/**
 * @brief Test that multiple consecutive silence outputs (past max_repeats)
 *        remain silent and don't produce stale data.
 */
static int
test_extended_silence(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));

    rc |= expect_int("init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    int16_t original[SAMPLES];
    fill_pattern(original, 'P');
    audio_conceal_on_good_buffer(&cs, original, FRAMES);

    int16_t out[SAMPLES];

    /* Exhaust all repeats. */
    for (int k = 0; k < AUDIO_CONCEAL_MAX_REPEATS; k++) {
        audio_conceal_on_underrun(&cs, out, FRAMES);
    }

    /* Three more underruns past max — all must be silence. */
    for (int extra = 0; extra < 3; extra++) {
        memset(out, 0xAA, sizeof(out));
        audio_conceal_on_underrun(&cs, out, FRAMES);

        for (int i = 0; i < SAMPLES; i++) {
            if (out[i] != 0) {
                fprintf(stderr, "FAIL: extended_silence extra=%d sample[%d]: got=%d want=0\n", extra, i, (int)out[i]);
                rc = 1;
                break;
            }
        }
    }

    audio_conceal_destroy(&cs);
    return rc;
}

int
main(void) {
    int rc = 0;

    /* Test each buffer pattern through the full attenuation curve. */
    rc |= test_pattern("zeros", 'Z');
    rc |= test_pattern("max_pos", 'P');
    rc |= test_pattern("max_neg", 'N');
    rc |= test_pattern("alternating", 'A');
    rc |= test_pattern("ramp", 'R');

    /* Test reset-and-re-attenuate behaviour. */
    rc |= test_reset_and_re_attenuate();

    /* Test extended silence past max_repeats. */
    rc |= test_extended_silence();

    return rc ? 1 : 0;
}
