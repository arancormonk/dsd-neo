// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* What the test codec library (tetra_test_codec.c) puts in each sample, for the tests that read its output back. */
#ifndef DSD_NEO_TESTS_TETRA_TEST_CODEC_H
#define DSD_NEO_TESTS_TETRA_TEST_CODEC_H

#include <stdint.h>

/* Sample i of decode number @p frame (from 1): negative for a bad frame, its magnitude 2 + 16 * frame + bit, where bit
   is input bit i mod 137. Distinct from silence for every frame below 2047. */
#define TETRA_TEST_CODEC_SAMPLE(frame, bad, bit)                                                                       \
    ((int16_t)(((bad) ? -1 : 1) * (int)(2U + (16U * (uint32_t)(frame)) + ((uint32_t)(bit) & 1U))))

typedef struct {
    int valid; /* 0 for a sample the codec cannot have made (silence) */
    int bad;
    uint32_t frame;
    int bit;
} tetra_test_codec_sample;

static inline tetra_test_codec_sample
tetra_test_codec_read(int16_t sample) {
    tetra_test_codec_sample s = {0, 0, 0, 0};
    const int magnitude = sample < 0 ? -(int)sample : (int)sample;
    if (magnitude < 18) {
        return s;
    }
    s.valid = 1;
    s.bad = sample < 0;
    s.frame = (uint32_t)(magnitude - 2) / 16U;
    s.bit = (magnitude - 2) % 16;
    return s;
}

#endif /* DSD_NEO_TESTS_TETRA_TEST_CODEC_H */
