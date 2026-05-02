// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/audio_concealment.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

static int
expect_float(const char* label, float got, float want) {
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got=%f want=%f\n", label, got, want);
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

static int
expect_nonnull(const char* label, const void* ptr) {
    if (!ptr) {
        fprintf(stderr, "FAIL: %s: got=NULL want=non-NULL\n", label);
        return 1;
    }
    return 0;
}

int
main(void) {
    int rc = 0;

    /* --- init with NULL cs returns -1 --- */
    rc |= expect_int("init(NULL, 256, 1)", audio_conceal_init(NULL, 256, 1), -1);

    /* --- init with 0 frames returns -1 --- */
    {
        struct audio_conceal_state cs;
        memset(&cs, 0, sizeof(cs));
        rc |= expect_int("init(&cs, 0, 1)", audio_conceal_init(&cs, 0, 1), -1);
    }

    /* --- init with 0 channels returns -1 --- */
    {
        struct audio_conceal_state cs;
        memset(&cs, 0, sizeof(cs));
        rc |= expect_int("init(&cs, 256, 0)", audio_conceal_init(&cs, 256, 0), -1);
    }

    /* --- init with valid params returns 0 and sets defaults --- */
    {
        struct audio_conceal_state cs;
        memset(&cs, 0, sizeof(cs));

        rc |= expect_int("init(&cs, 256, 1)", audio_conceal_init(&cs, 256, 1), 0);
        rc |= expect_nonnull("last_good_buffer after init", cs.last_good_buffer);
        rc |= expect_size("buffer_frames", cs.buffer_frames, 256U);
        rc |= expect_int("channels", cs.channels, 1);
        rc |= expect_int("max_repeats", cs.max_repeats, 4);
        rc |= expect_float("atten_per_repeat", cs.atten_per_repeat, 0.5f);
        rc |= expect_int("repeat_count", cs.repeat_count, 0);
        rc |= expect_u64("underrun_total", cs.underrun_total, 0U);

        audio_conceal_destroy(&cs);
    }

    /* --- destroy frees buffer, double-destroy is safe --- */
    {
        struct audio_conceal_state cs;
        memset(&cs, 0, sizeof(cs));

        rc |= expect_int("init for destroy test", audio_conceal_init(&cs, 256, 2), 0);
        rc |= expect_nonnull("buffer before destroy", cs.last_good_buffer);

        audio_conceal_destroy(&cs);
        rc |= expect_int("buffer NULL after destroy", cs.last_good_buffer == NULL, 1);

        /* Double-destroy must not crash. */
        audio_conceal_destroy(&cs);

        /* Destroy with NULL pointer must not crash. */
        audio_conceal_destroy(NULL);
    }

    return rc ? 1 : 0;
}
