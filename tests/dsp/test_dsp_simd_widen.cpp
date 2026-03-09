// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for SIMD u8->float widening and 90° rotate+widen. */

#include <dsd-neo/dsp/simd_widen.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "io/radio/rtl_capture_phase.h"

static int
arrays_close(const float* a, const float* b, int n, float tol) {
    for (int i = 0; i < n; i++) {
        if (fabsf(a[i] - b[i]) > tol) {
            return 0;
        }
    }
    return 1;
}

static int
bytes_equal(const unsigned char* a, const unsigned char* b, int n) {
    return memcmp(a, b, (size_t)n) == 0;
}

static void
apply_j4_rotation_ref(float in_i, float in_q, unsigned int phase, float* out_i, float* out_q) {
    switch (phase & 3U) {
        case 0:
            *out_i = in_i;
            *out_q = in_q;
            break;
        case 1:
            *out_i = -in_q;
            *out_q = in_i;
            break;
        case 2:
            *out_i = -in_i;
            *out_q = -in_q;
            break;
        default:
            *out_i = in_q;
            *out_q = -in_i;
            break;
    }
}

static void
apply_j4_rotation_u8_ref(unsigned char in_i, unsigned char in_q, unsigned int phase, unsigned char* out_i,
                         unsigned char* out_q) {
    switch (phase & 3U) {
        case 0:
            *out_i = in_i;
            *out_q = in_q;
            break;
        case 1:
            *out_i = (unsigned char)(255U - (unsigned int)in_q);
            *out_q = in_i;
            break;
        case 2:
            *out_i = (unsigned char)(255U - (unsigned int)in_i);
            *out_q = (unsigned char)(255U - (unsigned int)in_q);
            break;
        default:
            *out_i = in_q;
            *out_q = (unsigned char)(255U - (unsigned int)in_i);
            break;
    }
}

static unsigned int
process_rot_widen_chunk_with_carry(const unsigned char* src, size_t len, float* dst, unsigned int phase,
                                   struct rtl_capture_u8_byte_carry* carry, size_t* written) {
    if ((!src && len != 0U) || !dst || !carry) {
        if (written) {
            *written = 0;
        }
        return phase;
    }

    size_t out = 0;
    unsigned char pair[2] = {0};
    size_t prefix = rtl_capture_u8_byte_carry_consume_prefix(src, len, carry, pair);
    if (prefix != 0U) {
        phase = widen_rotate90_u8_to_f32_bias127_phase(pair, dst, 2U, phase);
        src += prefix;
        len -= prefix;
        dst += 2;
        out += 2U;
    }

    size_t body = len & ~((size_t)1U);
    if (body != 0U) {
        phase = widen_rotate90_u8_to_f32_bias127_phase(src, dst, (uint32_t)body, phase);
        src += body;
        len -= body;
        out += body;
    }

    if (len != 0U) {
        rtl_capture_u8_byte_carry_save(carry, src[0]);
    }
    if (written) {
        *written = out;
    }
    return phase;
}

static unsigned int
process_legacy_rotate_chunk_with_carry(const unsigned char* src, size_t len, unsigned char* dst, unsigned int phase,
                                       struct rtl_capture_u8_byte_carry* carry, size_t* written) {
    if ((!src && len != 0U) || !dst || !carry) {
        if (written) {
            *written = 0;
        }
        return phase;
    }

    size_t out = 0;
    unsigned char pair[2] = {0};
    size_t prefix = rtl_capture_u8_byte_carry_consume_prefix(src, len, carry, pair);
    if (prefix != 0U) {
        phase = rotate90_u8_inplace_phase(pair, 2U, phase);
        memcpy(dst, pair, 2U);
        src += prefix;
        len -= prefix;
        dst += 2;
        out += 2U;
    }

    size_t body = len & ~((size_t)1U);
    if (body != 0U) {
        memcpy(dst, src, body);
        phase = rotate90_u8_inplace_phase(dst, (uint32_t)body, phase);
        src += body;
        len -= body;
        out += body;
    }

    if (len != 0U) {
        rtl_capture_u8_byte_carry_save(carry, src[0]);
    }
    if (written) {
        *written = out;
    }
    return phase;
}

int
main(void) {
    // 4 complex samples (8 bytes)
    const unsigned char src[8] = {127, 127, 130, 130, 255, 0, 0, 255};
    float dst[8] = {0};
    float ref[8] = {0};

    // widen center 127 → normalized float around zero (center at 127.5)
    widen_u8_to_f32_bias127(src, dst, 8);
    const float inv = 1.0f / 127.5f;
    for (int i = 0; i < 8; i++) {
        ref[i] = ((float)src[i] - 127.5f) * inv;
    }
    if (!arrays_close(dst, ref, 8, 1e-6f)) {
        fprintf(stderr, "SIMD widen: mismatch\n");
        return 1;
    }

    // rotate 90° with pattern per implementation: (I0,Q0),(I1,Q1)->(-Q1, I1),(I2,Q2)->(-I2,-Q2),(I3,Q3)->(Q3,-I3)
    for (int i = 0; i < 8; i++) {
        dst[i] = 0;
    }
    widen_rotate90_u8_to_f32_bias127(src, dst, 8);
    float i0 = ((float)src[0] - 127.5f) * inv, q0 = ((float)src[1] - 127.5f) * inv;
    float i1 = ((float)src[2] - 127.5f) * inv, q1 = ((float)src[3] - 127.5f) * inv;
    float i2 = ((float)src[4] - 127.5f) * inv, q2 = ((float)src[5] - 127.5f) * inv;
    float i3 = ((float)src[6] - 127.5f) * inv, q3 = ((float)src[7] - 127.5f) * inv;
    float ref_rot[8] = {i0, q0, -q1, i1, -i2, -q2, q3, -i3};
    if (!arrays_close(dst, ref_rot, 8, 1e-6f)) {
        fprintf(stderr, "SIMD rotate+widen: mismatch\n");
        return 1;
    }

    {
        const unsigned char src_phase[10] = {127, 127, 130, 130, 255, 0, 0, 255, 64, 192};
        float dst_full[10] = {0};
        float dst_split[10] = {0};
        float ref_phase[10] = {0};
        unsigned int phase_full = widen_rotate90_u8_to_f32_bias127_phase(src_phase, dst_full, 10, 2U);
        unsigned int phase_split = 2U;
        unsigned int phase_ref = 2U;

        phase_split = widen_rotate90_u8_to_f32_bias127_phase(src_phase, dst_split, 6, phase_split);
        phase_split = widen_rotate90_u8_to_f32_bias127_phase(src_phase + 6, dst_split + 6, 4, phase_split);

        for (int pair = 0; pair < 5; pair++) {
            int idx = pair << 1;
            float in_i = ((float)src_phase[idx + 0] - 127.5f) * inv;
            float in_q = ((float)src_phase[idx + 1] - 127.5f) * inv;
            apply_j4_rotation_ref(in_i, in_q, phase_ref, &ref_phase[idx + 0], &ref_phase[idx + 1]);
            phase_ref = (phase_ref + 1U) & 3U;
        }

        if (phase_full != phase_split || phase_full != phase_ref) {
            fprintf(stderr, "SIMD rotate+widen phase carry: mismatch\n");
            return 1;
        }
        if (!arrays_close(dst_full, ref_phase, 10, 1e-6f) || !arrays_close(dst_split, ref_phase, 10, 1e-6f)) {
            fprintf(stderr, "SIMD rotate+widen phase carry output: mismatch\n");
            return 1;
        }
    }

    {
        unsigned char legacy[8] = {10, 11, 20, 21, 30, 31, 40, 41};
        unsigned char legacy_ref[8] = {0};
        float legacy_wide[8] = {0};
        float legacy_ref_wide[8] = {0};
        unsigned int phase = 0U;

        for (int pair = 0; pair < 4; pair++) {
            int idx = pair << 1;
            apply_j4_rotation_u8_ref(legacy[idx + 0], legacy[idx + 1], phase, &legacy_ref[idx + 0],
                                     &legacy_ref[idx + 1]);
            phase = (phase + 1U) & 3U;
        }

        if (rotate90_u8_inplace_phase(legacy, 8, 0U) != 0U || !bytes_equal(legacy, legacy_ref, 8)) {
            fprintf(stderr, "Legacy byte rotate: mismatch\n");
            return 1;
        }

        widen_u8_to_f32_bias128_scalar(legacy, legacy_wide, 8);
        widen_u8_to_f32_bias128_scalar(legacy_ref, legacy_ref_wide, 8);
        if (!arrays_close(legacy_wide, legacy_ref_wide, 8, 1e-6f)) {
            fprintf(stderr, "Legacy byte rotate + bias128 widen: mismatch\n");
            return 1;
        }
    }

    {
        const unsigned char src_split[10] = {10, 11, 20, 21, 30, 31, 40, 41, 50, 51};
        unsigned char whole[10];
        unsigned char split[10];
        memcpy(whole, src_split, sizeof(whole));
        memcpy(split, src_split, sizeof(split));

        unsigned int phase_whole = rotate90_u8_inplace_phase(whole, 10, 1U);
        unsigned int phase_split = 1U;
        phase_split = rotate90_u8_inplace_phase(split, 6, phase_split);
        phase_split = rotate90_u8_inplace_phase(split + 6, 4, phase_split);

        if (phase_whole != phase_split || !bytes_equal(whole, split, 10)) {
            fprintf(stderr, "Legacy byte rotate phase carry: mismatch\n");
            return 1;
        }
    }

    {
        const unsigned char src_odd_split[10] = {127, 127, 130, 130, 255, 0, 0, 255, 64, 192};
        float dst_full[10] = {0};
        float dst_split[10] = {0};
        struct rtl_capture_u8_byte_carry carry = {0};
        unsigned int phase_full = widen_rotate90_u8_to_f32_bias127_phase(src_odd_split, dst_full, 10, 3U);
        unsigned int phase_split = 3U;
        size_t out = 0;
        size_t wrote = 0;

        phase_split =
            process_rot_widen_chunk_with_carry(src_odd_split, 5, dst_split + out, phase_split, &carry, &wrote);
        out += wrote;
        phase_split =
            process_rot_widen_chunk_with_carry(src_odd_split + 5, 5, dst_split + out, phase_split, &carry, &wrote);
        out += wrote;

        if (carry.valid != 0U || out != 10U || phase_split != phase_full
            || !arrays_close(dst_split, dst_full, 10, 1e-6f)) {
            fprintf(stderr, "SIMD rotate+widen odd split carry: mismatch\n");
            return 1;
        }
    }

    {
        const unsigned char src_odd_split[10] = {10, 11, 20, 21, 30, 31, 40, 41, 50, 51};
        unsigned char whole[10];
        unsigned char split[10] = {0};
        struct rtl_capture_u8_byte_carry carry = {0};
        unsigned int phase_full = 0U;
        unsigned int phase_split = 0U;
        size_t out = 0;
        size_t wrote = 0;

        memcpy(whole, src_odd_split, sizeof(whole));
        phase_full = rotate90_u8_inplace_phase(whole, 10, 0U);

        phase_split =
            process_legacy_rotate_chunk_with_carry(src_odd_split, 5, split + out, phase_split, &carry, &wrote);
        out += wrote;
        phase_split =
            process_legacy_rotate_chunk_with_carry(src_odd_split + 5, 5, split + out, phase_split, &carry, &wrote);
        out += wrote;

        if (carry.valid != 0U || out != 10U || phase_split != phase_full || !bytes_equal(split, whole, 10)) {
            fprintf(stderr, "Legacy byte rotate odd split carry: mismatch\n");
            return 1;
        }
    }

    {
        const unsigned char src_gap[14] = {127, 127, 130, 130, 255, 0, 0, 255, 64, 192, 10, 240, 200, 40};
        const unsigned int start_phase = 1U;
        const size_t lead_bytes = 4;
        const size_t dropped_bytes = 6;
        const size_t tail_offset = lead_bytes + dropped_bytes;
        const size_t tail_bytes = sizeof(src_gap) - tail_offset;
        float dst_gap[8] = {0};
        float ref_gap[8] = {0};
        unsigned int phase_gap = start_phase;
        unsigned int phase_ref = start_phase;
        size_t ref_out = 0;

        phase_gap = widen_rotate90_u8_to_f32_bias127_phase(src_gap, dst_gap, (uint32_t)lead_bytes, phase_gap);
        phase_gap = (unsigned int)rtl_capture_phase_advance_u8_bytes((int)phase_gap, dropped_bytes);
        phase_gap = widen_rotate90_u8_to_f32_bias127_phase(src_gap + tail_offset, dst_gap + lead_bytes,
                                                           (uint32_t)tail_bytes, phase_gap);

        for (size_t pair = 0; pair < (sizeof(src_gap) / 2); pair++) {
            size_t idx = pair << 1;
            float in_i = ((float)src_gap[idx + 0] - 127.5f) * inv;
            float in_q = ((float)src_gap[idx + 1] - 127.5f) * inv;
            float out_i = 0.0f;
            float out_q = 0.0f;
            apply_j4_rotation_ref(in_i, in_q, phase_ref, &out_i, &out_q);
            if (idx < lead_bytes || idx >= tail_offset) {
                ref_gap[ref_out + 0] = out_i;
                ref_gap[ref_out + 1] = out_q;
                ref_out += 2;
            }
            phase_ref = (phase_ref + 1U) & 3U;
        }

        if (phase_gap != phase_ref) {
            fprintf(stderr, "SIMD rotate+widen discard phase carry: mismatch\n");
            return 1;
        }
        if (!arrays_close(dst_gap, ref_gap, (int)ref_out, 1e-6f)) {
            fprintf(stderr, "SIMD rotate+widen discard phase carry output: mismatch\n");
            return 1;
        }
    }

    {
        const unsigned char src_gap[14] = {10, 11, 20, 21, 30, 31, 40, 41, 50, 51, 60, 61, 70, 71};
        const unsigned int start_phase = 2U;
        const size_t lead_bytes = 4;
        const size_t dropped_bytes = 4;
        const size_t tail_offset = lead_bytes + dropped_bytes;
        const size_t tail_bytes = sizeof(src_gap) - tail_offset;
        unsigned char rotated[10] = {0};
        unsigned char ref_rotated[10] = {0};
        unsigned int phase_gap = start_phase;
        unsigned int phase_ref = start_phase;
        size_t ref_out = 0;

        memcpy(rotated, src_gap, lead_bytes);
        memcpy(rotated + lead_bytes, src_gap + tail_offset, tail_bytes);

        phase_gap = rotate90_u8_inplace_phase(rotated, (uint32_t)lead_bytes, phase_gap);
        phase_gap = (unsigned int)rtl_capture_phase_advance_u8_bytes((int)phase_gap, dropped_bytes);
        phase_gap = rotate90_u8_inplace_phase(rotated + lead_bytes, (uint32_t)tail_bytes, phase_gap);

        for (size_t pair = 0; pair < (sizeof(src_gap) / 2); pair++) {
            size_t idx = pair << 1;
            unsigned char out_i = 0;
            unsigned char out_q = 0;
            apply_j4_rotation_u8_ref(src_gap[idx + 0], src_gap[idx + 1], phase_ref, &out_i, &out_q);
            if (idx < lead_bytes || idx >= tail_offset) {
                ref_rotated[ref_out + 0] = out_i;
                ref_rotated[ref_out + 1] = out_q;
                ref_out += 2;
            }
            phase_ref = (phase_ref + 1U) & 3U;
        }

        if (phase_gap != phase_ref) {
            fprintf(stderr, "Legacy byte rotate discard phase carry: mismatch\n");
            return 1;
        }
        if (!bytes_equal(rotated, ref_rotated, (int)ref_out)) {
            fprintf(stderr, "Legacy byte rotate discard phase carry output: mismatch\n");
            return 1;
        }
    }

    return 0;
}
