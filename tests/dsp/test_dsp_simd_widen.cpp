// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Focused unit test for SIMD u8->float widening and 90° rotate+widen. */

#include <dsd-neo/dsp/simd_widen.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"
#include "io/radio/rtl_capture_phase.h"

#if defined(__x86_64__) || defined(_M_X64)
extern "C" uint32_t widen_rotate90_u8_to_f32_bias127_phase_sse2(const unsigned char* src, float* dst, uint32_t len,
                                                                uint32_t phase);
#endif

#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
extern "C" uint32_t widen_rotate90_u8_to_f32_bias127_phase_neon(const unsigned char* src, float* dst, uint32_t len,
                                                                uint32_t phase);
#endif

extern "C" uint32_t dsd_test_widen_rotate90_u8_to_f32_bias127_phase_scalar(const unsigned char* src, float* dst,
                                                                           uint32_t len, uint32_t phase);

static int
arrays_close(const float* a, const float* b, int n, float tol) {
    for (int i = 0; i < n; i++) {
        if (fabsf(a[i] - b[i]) > tol) {
            return 0;
        }
    }
    return 1;
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

using widen_backend_fn = unsigned int (*)(const unsigned char*, float*, unsigned int, unsigned int);

static int
test_rotate_widen_backend(const char* name, widen_backend_fn fn) {
    const unsigned char src[10] = {127, 127, 130, 130, 255, 0, 0, 255, 64, 192};
    const float inv = 1.0f / 127.5f;
    float dst_full[10] = {0};
    float dst_split[10] = {0};
    float ref[10] = {0};

    unsigned int phase_ref = 3U;
    for (int pair = 0; pair < 5; pair++) {
        int idx = pair << 1;
        float in_i = ((float)src[idx + 0] - 127.5f) * inv;
        float in_q = ((float)src[idx + 1] - 127.5f) * inv;
        apply_j4_rotation_ref(in_i, in_q, phase_ref, &ref[idx + 0], &ref[idx + 1]);
        phase_ref = (phase_ref + 1U) & 3U;
    }

    unsigned int phase_full = fn(src, dst_full, 10U, 3U);
    unsigned int phase_split = 3U;
    phase_split = fn(src, dst_split, 4U, phase_split);
    phase_split = fn(src + 4, dst_split + 4, 6U, phase_split);

    if (phase_full != phase_ref || phase_split != phase_ref) {
        DSD_FPRINTF(stderr, "%s phase carry: mismatch\n", name);
        return 1;
    }
    if (!arrays_close(dst_full, ref, 10, 1e-6f) || !arrays_close(dst_split, ref, 10, 1e-6f)) {
        DSD_FPRINTF(stderr, "%s output: mismatch\n", name);
        return 1;
    }

    for (unsigned int start_phase = 0; start_phase < 4U; start_phase++) {
        const unsigned char phase_src[4] = {10, 40, 170, 230};
        float vector_dst[4] = {0};
        float scalar_dst[2] = {0};
        float vector_ref[4] = {0};
        float scalar_ref[2] = {0};
        unsigned int phase_local_ref = start_phase;

        for (int pair = 0; pair < 2; pair++) {
            int idx = pair << 1;
            float in_i = ((float)phase_src[idx + 0] - 127.5f) * inv;
            float in_q = ((float)phase_src[idx + 1] - 127.5f) * inv;
            apply_j4_rotation_ref(in_i, in_q, phase_local_ref, &vector_ref[idx + 0], &vector_ref[idx + 1]);
            phase_local_ref = (phase_local_ref + 1U) & 3U;
        }
        if (fn(phase_src, vector_dst, 4U, start_phase) != phase_local_ref
            || !arrays_close(vector_dst, vector_ref, 4, 1e-6f)) {
            DSD_FPRINTF(stderr, "%s vector phase %u output: mismatch\n", name, start_phase);
            return 1;
        }

        float in_i = ((float)phase_src[0] - 127.5f) * inv;
        float in_q = ((float)phase_src[1] - 127.5f) * inv;
        apply_j4_rotation_ref(in_i, in_q, start_phase, &scalar_ref[0], &scalar_ref[1]);
        if (fn(phase_src, scalar_dst, 2U, start_phase) != ((start_phase + 1U) & 3U)
            || !arrays_close(scalar_dst, scalar_ref, 2, 1e-6f)) {
            DSD_FPRINTF(stderr, "%s scalar phase %u output: mismatch\n", name, start_phase);
            return 1;
        }
    }

    float guard[2] = {12.0f, -34.0f};
    if (fn(NULL, guard, 2U, 5U) != 1U || fn(src, NULL, 2U, 6U) != 2U || fn(src, guard, 1U, 7U) != 3U
        || guard[0] != 12.0f || guard[1] != -34.0f) {
        DSD_FPRINTF(stderr, "%s guard: mismatch\n", name);
        return 1;
    }

    return 0;
}

int
main(void) {
    /*
     * Cover the widening helpers in the order used by capture replay:
     * bias conversion, full-buffer rotation, chunked phase carry, odd-byte carry,
     * and dropped-span phase advancement. Backend-specific tests then reuse the
     * same reference rotation logic.
     */

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
        DSD_FPRINTF(stderr, "SIMD widen: mismatch\n");
        return 1;
    }

    {
        float guard[2] = {12.0f, -34.0f};
        const float want_guard[2] = {12.0f, -34.0f};
        widen_u8_to_f32_bias127(NULL, guard, 2);
        widen_u8_to_f32_bias127(src, guard, 0);
        if (!arrays_close(guard, want_guard, 2, 0.0f)) {
            DSD_FPRINTF(stderr, "SIMD widen guard: mismatch\n");
            return 1;
        }
    }

    // rotate 90° with pattern per implementation: (I0,Q0),(I1,Q1)->(-Q1, I1),(I2,Q2)->(-I2,-Q2),(I3,Q3)->(Q3,-I3)
    for (int i = 0; i < 8; i++) {
        dst[i] = 0;
    }
    (void)widen_rotate90_u8_to_f32_bias127_phase(src, dst, 8, 0U);
    float i0 = ((float)src[0] - 127.5f) * inv, q0 = ((float)src[1] - 127.5f) * inv;
    float i1 = ((float)src[2] - 127.5f) * inv, q1 = ((float)src[3] - 127.5f) * inv;
    float i2 = ((float)src[4] - 127.5f) * inv, q2 = ((float)src[5] - 127.5f) * inv;
    float i3 = ((float)src[6] - 127.5f) * inv, q3 = ((float)src[7] - 127.5f) * inv;
    float ref_rot[8] = {i0, q0, -q1, i1, -i2, -q2, q3, -i3};
    if (!arrays_close(dst, ref_rot, 8, 1e-6f)) {
        DSD_FPRINTF(stderr, "SIMD rotate+widen: mismatch\n");
        return 1;
    }

    {
        // Split processing must match a single pass and return the same phase.
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
            DSD_FPRINTF(stderr, "SIMD rotate+widen phase carry: mismatch\n");
            return 1;
        }
        if (!arrays_close(dst_full, ref_phase, 10, 1e-6f) || !arrays_close(dst_split, ref_phase, 10, 1e-6f)) {
            DSD_FPRINTF(stderr, "SIMD rotate+widen phase carry output: mismatch\n");
            return 1;
        }
    }

    if (test_rotate_widen_backend("SIMD rotate+widen scalar", dsd_test_widen_rotate90_u8_to_f32_bias127_phase_scalar)
        != 0) {
        return 1;
    }

    {
        // Odd chunks carry one input byte across calls before emitting a pair.
        const unsigned char src_odd_split[10] = {127, 127, 130, 130, 255, 0, 0, 255, 64, 192};
        float dst_full[10] = {0};
        float dst_split[10] = {0};
        struct rtl_capture_u8_byte_carry carry = {};
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
            DSD_FPRINTF(stderr, "SIMD rotate+widen odd split carry: mismatch\n");
            return 1;
        }
    }

    {
        // Dropped muted spans still advance the quarter-cycle rotation phase.
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
            DSD_FPRINTF(stderr, "SIMD rotate+widen discard phase carry: mismatch\n");
            return 1;
        }
        if (!arrays_close(dst_gap, ref_gap, (int)ref_out, 1e-6f)) {
            DSD_FPRINTF(stderr, "SIMD rotate+widen discard phase carry output: mismatch\n");
            return 1;
        }
    }

#if defined(__x86_64__) || defined(_M_X64)
    if (test_rotate_widen_backend("SIMD rotate+widen SSE2", widen_rotate90_u8_to_f32_bias127_phase_sse2) != 0) {
        return 1;
    }
#endif

#if defined(__aarch64__) || defined(__arm64) || defined(_M_ARM64) || defined(_M_ARM64EC)
    if (test_rotate_widen_backend("SIMD rotate+widen NEON", widen_rotate90_u8_to_f32_bias127_phase_neon) != 0) {
        return 1;
    }
#endif

    return 0;
}
