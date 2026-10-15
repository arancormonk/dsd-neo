// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A TETRA speech codec library for dsd-neo (--tetra-codec) over a locally obtained copy of the ETSI EN 300 395-2
 * reference source decoder. The ETSI sources are not part of dsd-neo; see README.md.
 *
 * The reference decoder keeps its state in file-scope variables, so this library serves one decoder at a time: a
 * second dsd_tetra_codec_create() before the first instance is destroyed returns NULL. dsd-neo holds one per decoding
 * session and destroys it before it makes the next.
 */

#define DSD_TETRA_CODEC_BUILD 1

#include <dsd-neo/protocol/tetra/tetra_codec_abi.h>
#include <stddef.h>
#include <stdint.h>

/* CMakeLists.txt names the header as the local tree spells it. */
#ifndef ETSI_SOURCE_HEADER
#define ETSI_SOURCE_HEADER "SOURCE.H"
#endif
#include ETSI_SOURCE_HEADER

/* The token dsd_tetra_codec_create() hands out; its address is the instance. */
static int g_decoder_in_use = 0;

uint32_t
dsd_tetra_codec_abi_version(void) {
    return DSD_TETRA_CODEC_ABI_VERSION;
}

void*
dsd_tetra_codec_create(void) {
    /* The reference code's Word16 must be the 16-bit PCM sample and bit type it exchanges. */
    if (sizeof(Word16) != sizeof(int16_t) || g_decoder_in_use) {
        return NULL;
    }
    Init_Decod_Tetra();
    g_decoder_in_use = 1;
    return &g_decoder_in_use;
}

int
dsd_tetra_codec_decode(void* codec, const uint8_t* bits, int bad_frame, int16_t* pcm) {
    /* Serial input of the source decoder (EN 300 395-2 clause 8): the bad frame indicator, then bits B1..B137. */
    Word16 serial[DSD_TETRA_CODEC_FRAME_BITS + 1];
    Word16 parameters[24];
    Word16 samples[DSD_TETRA_CODEC_FRAME_SAMPLES];
    if (codec != &g_decoder_in_use || !g_decoder_in_use || !bits || !pcm) {
        return -1;
    }
    serial[0] = (Word16)(bad_frame ? 1 : 0);
    for (int i = 0; i < DSD_TETRA_CODEC_FRAME_BITS; i++) {
        if (bits[i] > 1U) {
            return -1;
        }
        serial[i + 1] = (Word16)bits[i];
    }
    Bits2prm_Tetra(serial, parameters);
    Decod_Tetra(parameters, samples);
    Post_Process(samples, (Word16)DSD_TETRA_CODEC_FRAME_SAMPLES);
    for (int i = 0; i < DSD_TETRA_CODEC_FRAME_SAMPLES; i++) {
        pcm[i] = (int16_t)samples[i];
    }
    return 0;
}

void
dsd_tetra_codec_destroy(void* codec) {
    if (codec == &g_decoder_in_use) {
        g_decoder_in_use = 0;
    }
}
