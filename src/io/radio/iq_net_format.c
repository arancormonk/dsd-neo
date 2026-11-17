// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Network I/Q wire formats and their conversion to the decoder's interleaved floats.
 */

#include <dsd-neo/dsp/simd_widen.h>
#include <dsd-neo/io/iq_net.h>
#include <dsd-neo/io/iq_net_spec.h>
#include <dsd-neo/io/signal_measure.h>
#include <stddef.h>
#include <stdint.h>

#include "iq_net_internal.h"

/* cu8's rails convert to +-1.0 and the codes next to them to +-0.992 (rtl_device_signal_clip_level()'s single-pass
   level). cs8's rails convert to -1.0 and 127/128 = 0.9921875, the codes inside them to at most 0.984 in magnitude;
   a magnitude test at 127/128 also counts -127, the positive rail's mirror. */
#define IQ_NET_CU8_CLIP_LEVEL 0.996f
#define IQ_NET_CS8_CLIP_LEVEL (127.0f / 128.0f)

float
dsd_iq_net_format_clip_level(dsd_iq_net_format format) {
    switch (format) {
        case DSD_IQ_NET_FORMAT_CU8: return IQ_NET_CU8_CLIP_LEVEL;
        case DSD_IQ_NET_FORMAT_CS8: return IQ_NET_CS8_CLIP_LEVEL;
        default: return DSD_SIGNAL_CLIP_LEVEL_DEFAULT;
    }
}

static void
iq_net_cs8_to_f32(const uint8_t* src, size_t n, float* dst) {
    const float inv = 1.0f / 128.0f;
    for (size_t i = 0; i < n; i++) {
        const int32_t v = (int32_t)src[i] - ((src[i] & 0x80U) ? 256 : 0);
        dst[i] = (float)v * inv;
    }
}

static void
iq_net_cs16_to_f32(const uint8_t* src, size_t n, float* dst) {
    const float inv = 1.0f / 32768.0f;
    for (size_t i = 0; i < n; i++) {
        const uint32_t u = (uint32_t)src[2U * i] | ((uint32_t)src[2U * i + 1U] << 8);
        const int32_t v = (int32_t)u - ((u & 0x8000U) ? 65536 : 0);
        dst[i] = (float)v * inv;
    }
}

static void
iq_net_cs32_to_f32(const uint8_t* src, size_t n, float* dst) {
    /* The WAV s32 rule: x / 2^31 after the (rounding) conversion to float. */
    const float inv = 1.0f / 2147483648.0f;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* p = src + 4U * i;
        const uint32_t u = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        const int64_t v = (int64_t)u - ((u & 0x80000000U) ? (int64_t)4294967296LL : 0);
        dst[i] = (float)v * inv;
    }
}

size_t
dsd_iq_net_convert(dsd_iq_net_format format, const uint8_t* src, size_t samples, float* dst) {
    if (!src || !dst) {
        return 0U;
    }
    const size_t components = 2U * samples;
    switch (format) {
        case DSD_IQ_NET_FORMAT_CU8:
            /* The widen RTL USB and rtl_tcp use, (x - 127.5) / 127.5; a reader call carries at most 64 KiB. */
            widen_u8_to_f32_bias127(src, dst, (uint32_t)components);
            break;
        case DSD_IQ_NET_FORMAT_CS8: iq_net_cs8_to_f32(src, components, dst); break;
        case DSD_IQ_NET_FORMAT_CS16: iq_net_cs16_to_f32(src, components, dst); break;
        case DSD_IQ_NET_FORMAT_CS32: iq_net_cs32_to_f32(src, components, dst); break;
        case DSD_IQ_NET_FORMAT_CF32:
            /* Host-order floats: every host dsd-neo builds for is little-endian, as the senders are. */
            bound_cf32_to_f32(src, dst, samples);
            break;
        default: return 0U;
    }
    return samples;
}

size_t
iq_net_carry_restore(const iq_net_carry* carry, uint8_t* buf) {
    const size_t len = carry->len;
    for (size_t i = 0; i < len; i++) {
        buf[i] = carry->bytes[i];
    }
    return len;
}

size_t
iq_net_carry_keep(iq_net_carry* carry, const uint8_t* buf, size_t total, size_t sample_bytes) {
    const size_t whole = total / sample_bytes;
    const size_t rest = total - whole * sample_bytes;
    for (size_t i = 0; i < rest; i++) {
        carry->bytes[i] = buf[whole * sample_bytes + i];
    }
    carry->len = rest;
    return whole;
}
