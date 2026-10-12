// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Comparator for tests/iq_float_output_check.cmake (issue #652): the same replay run once with 16-bit output and once
 * with float output (-y), each writing the digital voice stream to stdout (-o -) and a static WAV (-w).
 *
 *   dsd-neo_test_float_output_compare <channels> [flags]
 *
 * Run in the test's work directory. Only the four fixed regular files s16.raw, f32.raw, s16.wav and f32.wav are read.
 *
 * Passes when the float stream is the 16-bit stream written as f32 (every sample s16 / 32768, bit for bit, so none is
 * NaN or past full scale), the stream carries audio, and the two static WAVs are byte-identical and hold audio. With
 * --both-ears the stereo stream must carry the same sample in both channels throughout, and audio in them. With
 * --silent neither stream may hold a sample (both formats mute the same frames, or nothing plays: -o null), and with
 * --silent-wav the static WAVs must match and contain no nonzero samples; a fully muted call may leave only a header.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"

enum { WAV_HEADER_BYTES = 44 };

static unsigned char*
read_all(const char* path, size_t* len) {
    *len = 0;
    FILE* f = dsd_fopen_existing_regular_file(path, "rb");
    if (!f) {
        DSD_FPRINTF(stderr, "float output compare: cannot open %s\n", path);
        return NULL;
    }
    size_t cap = 1U << 16;
    unsigned char* buf = malloc(cap);
    while (buf) {
        const size_t got = fread(buf + *len, 1, cap - *len, f);
        *len += got;
        if (*len < cap) {
            break;
        }
        unsigned char* grown = realloc(buf, cap * 2U);
        if (!grown) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = grown;
        cap *= 2U;
    }
    if (!buf || ferror(f)) {
        DSD_FPRINTF(stderr, "float output compare: cannot read %s\n", path);
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

static int
check_streams(const unsigned char* s16, size_t s16_len, const unsigned char* f32, size_t f32_len, int channels,
              int both_ears) {
    if (s16_len == 0U || s16_len % (sizeof(int16_t) * (size_t)channels) != 0U) {
        DSD_FPRINTF(stderr, "float output compare: 16-bit stream holds %zu bytes, no whole %d-channel frames\n",
                    s16_len, channels);
        return 1;
    }
    if (f32_len != s16_len * 2U) {
        DSD_FPRINTF(stderr, "float output compare: float stream holds %zu bytes, want %zu (the 16-bit stream's x2)\n",
                    f32_len, s16_len * 2U);
        return 1;
    }
    const size_t samples = s16_len / sizeof(int16_t);
    size_t nonzero = 0;
    size_t first_mismatch = samples;
    size_t ear_mismatch = samples;
    for (size_t i = 0; i < samples; i++) {
        int16_t s = 0;
        DSD_MEMCPY(&s, s16 + (i * sizeof(int16_t)), sizeof(s));
        // Bit for bit: s16 / 32768 is exact in f32, so the two encodings must match exactly.
        const float want = (float)s * (1.0f / 32768.0f);
        uint32_t want_bits = 0;
        uint32_t got_bits = 0;
        DSD_MEMCPY(&want_bits, &want, sizeof(want_bits));
        DSD_MEMCPY(&got_bits, f32 + (i * sizeof(float)), sizeof(got_bits));
        if (got_bits != want_bits && first_mismatch == samples) {
            first_mismatch = i;
        }
        nonzero += (s != 0) ? 1U : 0U;
        if (both_ears && channels == 2 && (i & 1U) == 1U) {
            int16_t left = 0;
            DSD_MEMCPY(&left, s16 + ((i - 1U) * sizeof(int16_t)), sizeof(left));
            if (left != s && ear_mismatch == samples) {
                ear_mismatch = i / 2U;
            }
        }
    }
    int rc = 0;
    if (first_mismatch != samples) {
        DSD_FPRINTF(stderr, "float output compare: float sample %zu is not the 16-bit sample / 32768\n",
                    first_mismatch);
        rc = 1;
    }
    if (nonzero == 0U) {
        DSD_FPRINTF(stderr, "float output compare: the stream carries no audio\n");
        rc = 1;
    }
    if (both_ears && (channels != 2 || ear_mismatch != samples)) {
        DSD_FPRINTF(stderr, "float output compare: stereo frame %zu differs between the ears\n", ear_mismatch);
        rc = 1;
    }
    DSD_FPRINTF(stderr, "float output compare: %zu samples, %zu non-zero\n", samples, nonzero);
    return rc;
}

static int
check_wavs(const unsigned char* a, size_t a_len, const unsigned char* b, size_t b_len, int silent_ok) {
    size_t nonzero = 0;
    for (size_t i = WAV_HEADER_BYTES; i < a_len; i++) {
        nonzero += (a[i] != 0U) ? 1U : 0U;
    }
    if (a_len < WAV_HEADER_BYTES || (silent_ok ? nonzero != 0U : nonzero == 0U)) {
        DSD_FPRINTF(stderr, "float output compare: unexpected static WAV content (%zu bytes, silent expected %d)\n",
                    a_len, silent_ok);
        return 1;
    }
    if (a_len != b_len || memcmp(a, b, a_len) != 0) {
        DSD_FPRINTF(stderr, "float output compare: static WAVs differ (%zu bytes 16-bit, %zu bytes -y)\n", a_len,
                    b_len);
        return 1;
    }
    DSD_FPRINTF(stderr, "float output compare: static WAVs identical, %zu bytes\n", a_len);
    return 0;
}

int
main(int argc, char** argv) {
    if (argc < 2 || (strcmp(argv[1], "1") != 0 && strcmp(argv[1], "2") != 0)) {
        DSD_FPRINTF(stderr,
                    "usage: %s <channels: 1|2> [--both-ears] [--silent] "
                    "[--silent-wav]\n",
                    argv[0]);
        return 2;
    }
    const int channels = argv[1][0] == '2' ? 2 : 1;
    int both_ears = 0;
    int silent = 0;
    int silent_wav = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--both-ears") == 0) {
            both_ears = 1;
        } else if (strcmp(argv[i], "--silent") == 0) {
            silent = 1;
        } else if (strcmp(argv[i], "--silent-wav") == 0) {
            silent_wav = 1;
        } else {
            DSD_FPRINTF(stderr, "float output compare: unknown flag %s\n", argv[i]);
            return 2;
        }
    }
    size_t lens[4] = {0, 0, 0, 0};
    unsigned char* data[4] = {NULL, NULL, NULL, NULL};
    static const char* const paths[4] = {"s16.raw", "f32.raw", "s16.wav", "f32.wav"};
    int rc = 0;
    for (int i = 0; i < 4; i++) {
        data[i] = read_all(paths[i], &lens[i]);
        if (!data[i]) {
            rc = 1;
        }
    }
    if (rc == 0) {
        if (silent) {
            if (lens[0] != 0U || lens[1] != 0U) {
                DSD_FPRINTF(stderr, "float output compare: want both streams silent, got %zu and %zu bytes\n", lens[0],
                            lens[1]);
                rc = 1;
            }
        } else {
            rc |= check_streams(data[0], lens[0], data[1], lens[1], channels, both_ears);
        }
        rc |= check_wavs(data[2], lens[2], data[3], lens[3], silent_wav);
    }
    for (int i = 0; i < 4; i++) {
        free(data[i]);
    }
    return rc;
}
