// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Integration test for the full audio concealment callback sequence.
 *
 * Simulates a complete player_callback cycle:
 *   1. Init concealment with 256 frames, 1 channel
 *   2. Feed 3 good buffers (known data) → verify repeat_count stays 0
 *   3. Trigger 4 underruns → verify progressive attenuation (0.5, 0.25, 0.125, 0.0625)
 *   4. Trigger 1 more underrun (past max) → verify silence output
 *   5. Feed 1 good buffer → verify repeat_count resets to 0
 *   6. Trigger 1 underrun → verify attenuation starts fresh at 0.5
 *   7. Verify underrun_total == 6 (4 + 1 + 1)
 *   8. Destroy and verify cleanup
 *
 * **Validates: Requirements 1.1, 1.2, 1.3, 1.4, 1.5, 1.6**
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

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
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
 * @brief Verify that every sample in a buffer matches an expected value.
 */
static int
verify_buffer_constant(const char* label, const int16_t* buf, int16_t want) {
    for (int i = 0; i < SAMPLES; i++) {
        if (buf[i] != want) {
            fprintf(stderr, "FAIL: %s sample[%d]: got=%d want=%d\n", label, i, (int)buf[i], (int)want);
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Verify that every sample in a buffer is zero (silence).
 */
static int
verify_silence(const char* label, const int16_t* buf) {
    return verify_buffer_constant(label, buf, 0);
}

int
main(void) {
    int rc = 0;

    /* --- Step 1: Init concealment --- */
    struct audio_conceal_state cs;
    memset(&cs, 0, sizeof(cs));
    rc |= expect_int("step1: init", audio_conceal_init(&cs, FRAMES, CHANNELS), 0);

    /* Use a constant value for easy verification. */
    const int16_t GOOD_VAL = 10000;
    int16_t good_buf[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        good_buf[i] = GOOD_VAL;
    }

    int16_t out[SAMPLES];

    /* --- Step 2: Feed 3 good buffers → repeat_count stays 0 --- */
    for (int g = 0; g < 3; g++) {
        audio_conceal_on_good_buffer(&cs, good_buf, FRAMES);
        rc |= expect_int("step2: repeat_count after good", cs.repeat_count, 0);
    }

    /* --- Step 3: Trigger 4 underruns → progressive attenuation --- */
    /* Expected gains: k=1 → 0.5, k=2 → 0.25, k=3 → 0.125, k=4 → 0.0625 */
    float expected_gains[4] = {0.5f, 0.25f, 0.125f, 0.0625f};

    for (int k = 1; k <= 4; k++) {
        memset(out, 0xAA, sizeof(out));
        audio_conceal_on_underrun(&cs, out, FRAMES);

        int16_t want = expected_sample(GOOD_VAL, k);
        char label[64];
        snprintf(label, sizeof(label), "step3: underrun k=%d sample[0]", k);
        rc |= expect_i16(label, out[0], want);

        /* Spot-check a middle sample too. */
        snprintf(label, sizeof(label), "step3: underrun k=%d sample[128]", k);
        rc |= expect_i16(label, out[128], want);
    }

    rc |= expect_int("step3: repeat_count after 4 underruns", cs.repeat_count, 4);

    /* --- Step 4: 1 more underrun (past max) → silence --- */
    memset(out, 0xAA, sizeof(out));
    audio_conceal_on_underrun(&cs, out, FRAMES);
    rc |= verify_silence("step4: silence after max", out);

    /* --- Step 5: Feed 1 good buffer → repeat_count resets to 0 --- */
    /* Use a different value to distinguish from the original good buffer. */
    const int16_t GOOD_VAL2 = 20000;
    int16_t good_buf2[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        good_buf2[i] = GOOD_VAL2;
    }

    audio_conceal_on_good_buffer(&cs, good_buf2, FRAMES);
    rc |= expect_int("step5: repeat_count after good_buffer", cs.repeat_count, 0);

    /* --- Step 6: 1 underrun → fresh attenuation at 0.5 on new buffer --- */
    memset(out, 0xAA, sizeof(out));
    audio_conceal_on_underrun(&cs, out, FRAMES);

    int16_t want_fresh = expected_sample(GOOD_VAL2, 1);
    rc |= expect_i16("step6: fresh k=1 sample[0]", out[0], want_fresh);
    rc |= expect_i16("step6: fresh k=1 sample[255]", out[255], want_fresh);

    /* --- Step 7: Verify underrun_total == 6 (4 + 1 + 1) --- */
    rc |= expect_u64("step7: underrun_total", cs.underrun_total, 6U);

    /* --- Step 8: Destroy and verify cleanup --- */
    audio_conceal_destroy(&cs);
    rc |= expect_int("step8: buffer NULL after destroy", cs.last_good_buffer == NULL, 1);

    /* Double-destroy must not crash. */
    audio_conceal_destroy(&cs);

    return rc ? 1 : 0;
}
