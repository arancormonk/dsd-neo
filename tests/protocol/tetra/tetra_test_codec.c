// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A deterministic stand-in for the TETRA speech decoder (tetra_voice_codec), installed with
 * tetra_voice_set_codec_for_test(): each sample it makes says which decode since the last reset made it, whether that
 * was a bad frame and one input bit (tetra_test_codec.h). The variants fail the ways the real decoder could.
 */

#include <stdint.h>
#include <stdlib.h>

#include "tetra_test_codec.h"
#include "tetra_voice_state.h"

typedef struct {
    uint32_t frames;
} tetra_test_decoder;

static void*
test_alloc(void) {
    return calloc(1, sizeof(tetra_test_decoder));
}

static void*
test_alloc_none(void) {
    return NULL;
}

static void
test_reset(void* decoder) {
    if (decoder) {
        ((tetra_test_decoder*)decoder)->frames = 0;
    }
}

static void
test_release(void* decoder) {
    free(decoder);
}

static int
test_decode(void* decoder, const uint8_t* bits, int bad_frame, int16_t* pcm) {
    tetra_test_decoder* const d = (tetra_test_decoder*)decoder;
    if (!d || !bits || !pcm) {
        return -1;
    }
    d->frames++;
    for (int i = 0; i < TETRA_VOICE_FRAME_SAMPLES; i++) {
        pcm[i] = TETRA_TEST_CODEC_SAMPLE(d->frames, bad_frame, bits[i % TETRA_VOICE_FRAME_BITS]);
    }
    return 0;
}

static int
test_decode_fail(void* decoder, const uint8_t* bits, int bad_frame, int16_t* pcm) {
    (void)test_decode(decoder, bits, bad_frame, pcm);
    return -1;
}

const tetra_voice_codec*
tetra_test_codec(tetra_test_codec_variant variant) {
    static const tetra_voice_codec good = {test_alloc, test_reset, test_release, test_decode};
    static const tetra_voice_codec no_decoder = {test_alloc_none, test_reset, test_release, test_decode};
    static const tetra_voice_codec fail_decode = {test_alloc, test_reset, test_release, test_decode_fail};
    switch (variant) {
        case TETRA_TEST_CODEC_NO_DECODER: return &no_decoder;
        case TETRA_TEST_CODEC_FAIL_DECODE: return &fail_decode;
        case TETRA_TEST_CODEC_GOOD:
        default: return &good;
    }
}

uint32_t
tetra_test_codec_decodes(const void* decoder) {
    return decoder ? ((const tetra_test_decoder*)decoder)->frames : 0U;
}
