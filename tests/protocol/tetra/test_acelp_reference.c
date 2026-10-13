// SPDX-License-Identifier: GPL-3.0-or-later
/* Decode telive's public "Hello Tetra" channel sample and compare every recovered speech frame bit with the output
 * of the ETSI reference decoder, whose bad-frame flag is clear for all of them: every burst's class-2 CRC
 * (EN 300 395-2 clause 5.5.1) must pass too. */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>

#include <stdint.h>
#include <stdio.h>

#ifndef TETRA_ACELP_SAMPLE
#error TETRA_ACELP_SAMPLE is required
#endif
#ifndef TETRA_ACELP_REFERENCE
#error TETRA_ACELP_REFERENCE is required
#endif

enum { CHANNEL_FRAMES = 30, BLOCK_WORDS = 690, FRAME_BITS = 137 };

typedef struct {
    int bit_mismatches;
    int crc_failures;
} reference_result;

static int
read_le16(FILE* fp, int16_t* value) {
    uint8_t b[2];
    if (fread(b, 1, 2, fp) != 2) {
        return 0;
    }
    *value = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return 1;
}

/* The 274 speech frame bits of one channel frame of the sample, after its keystream. */
static int
decode_channel_frame(FILE* sample, int channel_frame, uint8_t* codec, reference_result* result) {
    int16_t block[BLOCK_WORDS];
    for (int i = 0; i < BLOCK_WORDS; i++) {
        if (!read_le16(sample, &block[i])) {
            DSD_FPRINTF(stderr, "short channel sample at frame %d\n", channel_frame);
            return 0;
        }
    }
    if ((uint16_t)block[0] != 0x6b21u) {
        DSD_FPRINTF(stderr, "bad channel-frame marker at frame %d\n", channel_frame);
        return 0;
    }

    int16_t received[432];
    DSD_MEMCPY(received, block + 1, 114 * sizeof(int16_t));
    DSD_MEMCPY(received + 114, block + 116, 114 * sizeof(int16_t));
    DSD_MEMCPY(received + 228, block + 231, 114 * sizeof(int16_t));
    DSD_MEMCPY(received + 342, block + 346, 90 * sizeof(int16_t));

    uint16_t interleaved[432];
    uint16_t deinterleaved[432];
    for (int i = 0; i < 432; i++) {
        interleaved[i] = (int8_t)((uint16_t)received[i] & 0xffu) < 0 ? 0xffffu : 0u;
    }
    tetra_speech_deinterleave_soft(interleaved, deinterleaved);
    uint8_t type1[TETRA_SPEECH_FULL_BITS] = {0};
    int crc_ok = 0;
    int channel_mismatches = -1;
    if (tetra_speech_full_decode(deinterleaved, type1, &crc_ok, &channel_mismatches) != TETRA_SPEECH_FULL_BITS) {
        DSD_FPRINTF(stderr, "frame %d did not decode\n", channel_frame);
        return 0;
    }
    if (!crc_ok) {
        DSD_FPRINTF(stderr, "class-2 CRC failed at frame %d (%d channel-bit mismatches)\n", channel_frame,
                    channel_mismatches);
        result->crc_failures++;
    }
    tetra_voice_reorder_full(type1, codec);

    int ks_word = 461;
    uint16_t ks = (uint16_t)block[ks_word];
    for (int i = 0; i < TETRA_SPEECH_FULL_BITS; i++) {
        if (i && i % 4 == 0) {
            ks = (uint16_t)block[++ks_word];
        }
        codec[i] ^= (uint8_t)((ks & 0x0008u) != 0);
        ks <<= 1;
    }
    return 1;
}

static int
compare_with_reference(FILE* reference, int channel_frame, const uint8_t* codec, reference_result* result) {
    for (int speech_frame = 0; speech_frame < 2; speech_frame++) {
        const int number = (channel_frame * 2) + speech_frame;
        int16_t bfi = 0;
        if (!read_le16(reference, &bfi) || bfi != 0) {
            DSD_FPRINTF(stderr, "reference short or BFI set at speech frame %d\n", number);
            return 0;
        }
        for (int bit = 0; bit < FRAME_BITS; bit++) {
            int16_t expected = 0;
            if (!read_le16(reference, &expected)) {
                return 0;
            }
            const uint8_t actual = codec[(speech_frame * FRAME_BITS) + bit];
            if (actual != (uint8_t)expected) {
                if (result->bit_mismatches < 20) {
                    DSD_FPRINTF(stderr, "ACELP mismatch at speech frame %d bit %d: %u != %d\n", number, bit + 1, actual,
                                expected);
                }
                result->bit_mismatches++;
            }
        }
    }
    return 1;
}

int
main(void) {
    reference_result result = {0, 0};
    FILE* sample = fopen(TETRA_ACELP_SAMPLE, "rb");
    FILE* reference = fopen(TETRA_ACELP_REFERENCE, "rb");
    int ok = sample != NULL && reference != NULL;
    if (!ok) {
        DSD_FPRINTF(stderr, "failed to open ACELP fixtures\n");
    }
    for (int channel_frame = 0; ok && channel_frame < CHANNEL_FRAMES; channel_frame++) {
        uint8_t codec[TETRA_SPEECH_FULL_BITS] = {0};
        ok = decode_channel_frame(sample, channel_frame, codec, &result)
             && compare_with_reference(reference, channel_frame, codec, &result);
    }
    if (ok && (fgetc(sample) != EOF || fgetc(reference) != EOF)) {
        DSD_FPRINTF(stderr, "unexpected trailing fixture data\n");
        ok = 0;
    }
    if (sample) {
        (void)fclose(sample);
    }
    if (reference) {
        (void)fclose(reference);
    }
    if (!ok || result.bit_mismatches || result.crc_failures) {
        DSD_FPRINTF(stderr, "%d ACELP bit mismatches, %d CRC failures\n", result.bit_mismatches, result.crc_failures);
        return 1;
    }
    puts("PASS test_acelp_reference: 30 channel frames / 60 ACELP frames, every CRC passed");
    return 0;
}
