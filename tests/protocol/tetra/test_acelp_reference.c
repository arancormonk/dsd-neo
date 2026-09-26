// SPDX-License-Identifier: GPL-3.0-or-later
/* Decode telive's public "Hello Tetra" channel sample and compare every
 * recovered ACELP bit with the output of the ETSI reference decoder. */

#include <dsd-neo/protocol/tetra/tetra_acelp.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef TETRA_ACELP_SAMPLE
#error TETRA_ACELP_SAMPLE is required
#endif
#ifndef TETRA_ACELP_REFERENCE
#error TETRA_ACELP_REFERENCE is required
#endif

static int read_le16(FILE *fp, int16_t *value) {
    uint8_t b[2];
    if (fread(b, 1, 2, fp) != 2) return 0;
    *value = (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return 1;
}

int main(void) {
    int mismatches = 0;
    FILE *sample = fopen(TETRA_ACELP_SAMPLE, "rb");
    FILE *reference = fopen(TETRA_ACELP_REFERENCE, "rb");
    if (!sample || !reference) {
        fprintf(stderr, "failed to open ACELP fixtures\n");
        if (sample) fclose(sample);
        if (reference) fclose(reference);
        return 1;
    }

    for (int channel_frame = 0; channel_frame < 30; channel_frame++) {
        int16_t block[690];
        for (int i = 0; i < 690; i++) {
            if (!read_le16(sample, &block[i])) {
                fprintf(stderr, "short channel sample at frame %d\n", channel_frame);
                return 1;
            }
        }
        if ((uint16_t)block[0] != 0x6b21u) {
            fprintf(stderr, "bad channel-frame marker at frame %d\n", channel_frame);
            return 1;
        }

        int16_t received[432];
        memcpy(received,       block +   1, 114 * sizeof(int16_t));
        memcpy(received + 114, block + 116, 114 * sizeof(int16_t));
        memcpy(received + 228, block + 231, 114 * sizeof(int16_t));
        memcpy(received + 342, block + 346,  90 * sizeof(int16_t));

        uint16_t interleaved[432], deinterleaved[432];
        for (int i = 0; i < 432; i++)
            interleaved[i] = (int8_t)((uint16_t)received[i] & 0xffu) < 0
                                 ? 0xffffu : 0u;
        tetra_speech_deinterleave_soft(interleaved, deinterleaved);
        uint8_t type2[274] = {0};
        const int decoded = tetra_speech_channel_decode(deinterleaved, type2, 274);
        if (decoded != 274) {
            fprintf(stderr, "decoded %d bits at frame %d, expected 274\n",
                    decoded, channel_frame);
            return 1;
        }

        uint8_t codec[274] = {0};
        tetra_acelp_reorder(type2, codec, decoded);

        int ks_word = 461;
        uint16_t ks = (uint16_t)block[ks_word];
        for (int i = 0; i < 274; i++) {
            if (i && i % 4 == 0)
                ks = (uint16_t)block[++ks_word];
            codec[i] ^= (uint8_t)((ks & 0x0008u) != 0);
            ks <<= 1;
        }

        for (int speech_frame = 0; speech_frame < 2; speech_frame++) {
            int16_t bfi;
            if (!read_le16(reference, &bfi)) {
                fprintf(stderr, "short reference at speech frame %d\n",
                        channel_frame * 2 + speech_frame);
                return 1;
            }
            if (bfi != 0) {
                fprintf(stderr, "reference BFI set at speech frame %d\n",
                        channel_frame * 2 + speech_frame);
                return 1;
            }
            for (int bit = 0; bit < 137; bit++) {
                int16_t expected;
                if (!read_le16(reference, &expected)) return 1;
                const uint8_t actual = codec[speech_frame * 137 + bit];
                if (actual != (uint8_t)expected) {
                    if (mismatches < 20)
                        fprintf(stderr,
                                "ACELP mismatch at speech frame %d bit %d: %u != %d\n",
                                channel_frame * 2 + speech_frame, bit + 1,
                                actual, expected);
                    mismatches++;
                }
            }
        }
    }

    if (fgetc(sample) != EOF || fgetc(reference) != EOF) {
        fprintf(stderr, "unexpected trailing fixture data\n");
        return 1;
    }
    fclose(sample);
    fclose(reference);
    if (mismatches) {
        fprintf(stderr, "%d ACELP bit mismatches\n", mismatches);
        return 1;
    }
    puts("PASS test_acelp_reference: 30 channel frames / 60 ACELP frames");
    return 0;
}
