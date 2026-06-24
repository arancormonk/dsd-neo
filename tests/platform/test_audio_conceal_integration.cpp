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
#include "dsd-neo/core/safe_api.h"

#define FRAMES   256
#define CHANNELS 1
#define SAMPLES  (FRAMES * CHANNELS)

/*============================================================================
 * Helpers
 *============================================================================*/

static int
expect_int(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_i16(const char* label, int16_t got, int16_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%d want=%d\n", label, (int)got, (int)want);
        return 1;
    }
    return 0;
}

static int
expect_u64(const char* label, uint64_t got, uint64_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%llu want=%llu\n", label, (unsigned long long)got, (unsigned long long)want);
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
            DSD_FPRINTF(stderr, "FAIL: %s sample[%d]: got=%d want=%d\n", label, i, (int)buf[i], (int)want);
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

static int
test_guard_paths_and_no_last_good_silence(void) {
    int rc = 0;
    int16_t out[8];
    for (int i = 0; i < 8; i++) {
        out[i] = (int16_t)(100 + i);
    }

    rc |= expect_int("guard: null state writes zero frames", (int)audio_conceal_on_underrun(NULL, out, 4), 0);
    rc |= expect_int("guard: null output writes zero frames", (int)audio_conceal_on_underrun(NULL, NULL, 4), 0);

    struct audio_conceal_state cs;
    DSD_MEMSET(&cs, 0, sizeof(cs));
    cs.buffer_frames = 4;
    cs.channels = 2;
    cs.underrun_total = 41;

    rc |= expect_int("guard: zero frames writes zero frames", (int)audio_conceal_on_underrun(&cs, out, 0), 0);
    rc |= expect_i16("guard: zero frames leaves sample", out[0], 100);

    for (int i = 0; i < 8; i++) {
        out[i] = (int16_t)(200 + i);
    }
    rc |= expect_int("no last-good returns requested bounded frames", (int)audio_conceal_on_underrun(&cs, out, 3), 3);
    for (int i = 0; i < 6; i++) {
        rc |= expect_i16("no last-good zero-fills written sample", out[i], 0);
    }
    rc |= expect_i16("no last-good leaves unwritten tail", out[6], 206);
    rc |= expect_i16("no last-good leaves final tail", out[7], 207);
    rc |= expect_u64("no last-good increments underrun total", cs.underrun_total, 42U);
    return rc;
}

static int
test_short_good_buffer_tail_padding_and_custom_gain_clamps(void) {
    int rc = 0;
    struct audio_conceal_state cs;
    DSD_MEMSET(&cs, 0, sizeof(cs));
    rc |= expect_int("tail/clamp: init", audio_conceal_init(&cs, 4, 2), 0);

    int16_t good[4] = {1000, -1000, 32767, -32768};
    audio_conceal_on_good_buffer(&cs, good, 2);

    rc |= expect_i16("tail/clamp: copied first sample", cs.last_good_buffer[0], 1000);
    rc |= expect_i16("tail/clamp: copied second sample", cs.last_good_buffer[1], -1000);
    rc |= expect_i16("tail/clamp: copied positive clamp seed", cs.last_good_buffer[2], 32767);
    rc |= expect_i16("tail/clamp: copied negative clamp seed", cs.last_good_buffer[3], -32768);
    for (int i = 4; i < 8; i++) {
        rc |= expect_i16("tail/clamp: short good buffer zero-pads tail", cs.last_good_buffer[i], 0);
    }

    cs.repeat_count = 3;
    audio_conceal_on_good_buffer(&cs, NULL, 2);
    rc |= expect_int("tail/clamp: null good buffer does not reset repeat count", cs.repeat_count, 3);
    audio_conceal_on_good_buffer(&cs, good, 0);
    rc |= expect_int("tail/clamp: zero-frame good buffer does not reset repeat count", cs.repeat_count, 3);
    cs.repeat_count = 0;
    cs.atten_per_repeat = 2.0f;

    int16_t out[8];
    DSD_MEMSET(out, 0x7F, sizeof(out));
    rc |= expect_int("tail/clamp: underrun writes full frame count", (int)audio_conceal_on_underrun(&cs, out, 4), 4);
    rc |= expect_i16("tail/clamp: custom gain scales positive", out[0], 2000);
    rc |= expect_i16("tail/clamp: custom gain scales negative", out[1], -2000);
    rc |= expect_i16("tail/clamp: positive sample clamps", out[2], 32767);
    rc |= expect_i16("tail/clamp: negative sample clamps", out[3], -32768);
    for (int i = 4; i < 8; i++) {
        rc |= expect_i16("tail/clamp: padded tail stays silent", out[i], 0);
    }
    rc |= expect_int("tail/clamp: underrun increments repeat", cs.repeat_count, 1);

    audio_conceal_destroy(&cs);
    return rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_guard_paths_and_no_last_good_silence();
    rc |= test_short_good_buffer_tail_padding_and_custom_gain_clamps();

    /* --- Step 1: Init concealment --- */
    struct audio_conceal_state cs;
    DSD_MEMSET(&cs, 0, sizeof(cs));
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
    for (int k = 1; k <= 4; k++) {
        DSD_MEMSET(out, 0xAA, sizeof(out));
        audio_conceal_on_underrun(&cs, out, FRAMES);

        int16_t want = expected_sample(GOOD_VAL, k);
        char label[64];
        DSD_SNPRINTF(label, sizeof(label), "step3: underrun k=%d sample[0]", k);
        rc |= expect_i16(label, out[0], want);

        /* Spot-check a middle sample too. */
        DSD_SNPRINTF(label, sizeof(label), "step3: underrun k=%d sample[128]", k);
        rc |= expect_i16(label, out[128], want);
    }

    rc |= expect_int("step3: repeat_count after 4 underruns", cs.repeat_count, 4);

    /* --- Step 4: 1 more underrun (past max) → silence --- */
    DSD_MEMSET(out, 0xAA, sizeof(out));
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
    DSD_MEMSET(out, 0xAA, sizeof(out));
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
