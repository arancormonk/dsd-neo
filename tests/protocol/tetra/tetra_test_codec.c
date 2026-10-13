// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A deterministic stand-in for a TETRA speech codec library (tetra_codec_abi.h), built as a shared library for the
 * voice tests. It makes no speech: every frame decodes to a tone whose samples say what the decoder was given, so a
 * test reads the output and knows which frame reached it and how.
 *
 *   pcm[i] = TETRA_TEST_CODEC_SAMPLE(frame, bad, bits[i % 137])
 *
 * where frame counts the instance's decodes from 1. The variants (one compile definition each) break the interface
 * the way a wrong library would: TETRA_TEST_CODEC_WRONG_ABI reports version 2, TETRA_TEST_CODEC_MISSING_SYMBOL exports
 * no destroy function, TETRA_TEST_CODEC_NO_CREATE cannot make a decoder, TETRA_TEST_CODEC_FAIL_DECODE fails every
 * decode.
 */

#define DSD_TETRA_CODEC_BUILD 1

#include <dsd-neo/protocol/tetra/tetra_codec_abi.h>
#include <stdint.h>
#include <stdlib.h>

#include "tetra_test_codec.h"

typedef struct {
    uint32_t frames;
} tetra_test_codec;

uint32_t
dsd_tetra_codec_abi_version(void) {
#ifdef TETRA_TEST_CODEC_WRONG_ABI
    return DSD_TETRA_CODEC_ABI_VERSION + 1U;
#else
    return DSD_TETRA_CODEC_ABI_VERSION;
#endif
}

void*
dsd_tetra_codec_create(void) {
#ifdef TETRA_TEST_CODEC_NO_CREATE
    return NULL;
#else
    return calloc(1, sizeof(tetra_test_codec));
#endif
}

int
dsd_tetra_codec_decode(void* codec, const uint8_t* bits, int bad_frame, int16_t* pcm) {
    tetra_test_codec* const c = (tetra_test_codec*)codec;
    if (!c || !bits || !pcm) {
        return -1;
    }
    c->frames++;
    for (int i = 0; i < DSD_TETRA_CODEC_FRAME_SAMPLES; i++) {
        pcm[i] = TETRA_TEST_CODEC_SAMPLE(c->frames, bad_frame, bits[i % DSD_TETRA_CODEC_FRAME_BITS]);
    }
#ifdef TETRA_TEST_CODEC_FAIL_DECODE
    return -1;
#else
    return 0;
#endif
}

#ifndef TETRA_TEST_CODEC_MISSING_SYMBOL
void
dsd_tetra_codec_destroy(void* codec) {
    free(codec);
}
#endif
