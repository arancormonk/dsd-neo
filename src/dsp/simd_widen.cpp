// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Scalar helpers to widen RTL u8 IQ into normalized float samples.
 *
 * Converts unsigned 8-bit I/Q into centered float in [-1.0, 1.0] with an
 * unbiased midpoint at 127.5. No clamping is applied so headroom is retained
 * for downstream float processing.
 */

#include <dsd-neo/dsp/simd_widen.h>

#include <stdint.h>

static inline void
apply_j4_rotation_f32(float in_i, float in_q, uint32_t phase, float* out_i, float* out_q) {
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

static inline void
apply_j4_rotation_u8(unsigned char in_i, unsigned char in_q, uint32_t phase, unsigned char* out_i,
                     unsigned char* out_q) {
    switch (phase & 3U) {
        case 0:
            *out_i = in_i;
            *out_q = in_q;
            break;
        case 1:
            *out_i = (unsigned char)(255U - (uint32_t)in_q);
            *out_q = in_i;
            break;
        case 2:
            *out_i = (unsigned char)(255U - (uint32_t)in_i);
            *out_q = (unsigned char)(255U - (uint32_t)in_q);
            break;
        default:
            *out_i = in_q;
            *out_q = (unsigned char)(255U - (uint32_t)in_i);
            break;
    }
}

void
widen_u8_to_f32_bias127(const unsigned char* src, float* dst, uint32_t len) {
    if (!src || !dst || len == 0) {
        return;
    }
    const float inv = 1.0f / 127.5f;
    for (uint32_t i = 0; i < len; i++) {
        float v = ((float)src[i] - 127.5f) * inv;
        dst[i] = v;
    }
}

void
widen_u8_to_f32_bias128_scalar(const unsigned char* src, float* dst, uint32_t len) {
    if (!src || !dst || len == 0) {
        return;
    }
    const float inv = 1.0f / 127.5f;
    for (uint32_t i = 0; i < len; i++) {
        float v = ((float)src[i] - 128.0f) * inv;
        dst[i] = v;
    }
}

uint32_t
widen_rotate90_u8_to_f32_bias127_phase(const unsigned char* src, float* dst, uint32_t len, uint32_t phase) {
    uint32_t cur_phase = phase & 3U;
    if (!src || !dst || len < 2) {
        return cur_phase;
    }

    const float inv = 1.0f / 127.5f;
    uint32_t pairs = len >> 1;
    for (uint32_t n = 0; n < pairs; n++) {
        uint32_t idx = n << 1;
        float i_raw = ((float)src[idx + 0] - 127.5f) * inv;
        float q_raw = ((float)src[idx + 1] - 127.5f) * inv;
        apply_j4_rotation_f32(i_raw, q_raw, cur_phase, &dst[idx + 0], &dst[idx + 1]);
        cur_phase = (cur_phase + 1U) & 3U;
    }

    return cur_phase;
}

void
widen_rotate90_u8_to_f32_bias127(const unsigned char* src, float* dst, uint32_t len) {
    (void)widen_rotate90_u8_to_f32_bias127_phase(src, dst, len, 0U);
}

uint32_t
rotate90_u8_inplace_phase(unsigned char* buf, uint32_t len, uint32_t phase) {
    uint32_t cur_phase = phase & 3U;
    if (!buf || len < 2) {
        return cur_phase;
    }

    uint32_t pairs = len >> 1;
    for (uint32_t n = 0; n < pairs; n++) {
        uint32_t idx = n << 1;
        unsigned char out_i = 0;
        unsigned char out_q = 0;
        apply_j4_rotation_u8(buf[idx + 0], buf[idx + 1], cur_phase, &out_i, &out_q);
        buf[idx + 0] = out_i;
        buf[idx + 1] = out_q;
        cur_phase = (cur_phase + 1U) & 3U;
    }

    return cur_phase;
}
