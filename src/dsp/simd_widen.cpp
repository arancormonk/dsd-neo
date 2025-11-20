// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SIMD widening and optional 90° IQ rotation with runtime CPU dispatch.
 *
 * Converts RTL-SDR unsigned 8-bit I/Q samples to signed 16-bit integers with
 * optional 90° rotation. Provides scalar and SIMD specializations (AVX2,
 * SSSE3/SSE2, NEON) selected at runtime via feature detection.
 */

#include <dsd-neo/dsp/simd_widen.h>
#include <pthread.h>
#include <string.h>

/* Optional SIMD intrinsics for USB byte->int16 widening */
#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#include <immintrin.h>
#include <tmmintrin.h>
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#include <arm_neon.h>
#endif

/* Runtime CPU feature detection headers */
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif
#if defined(__linux__)
#include <sys/auxv.h>
#ifndef AT_HWCAP
#define AT_HWCAP 16
#endif
#endif

/* Compiler-friendly restrict qualifier */
#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_RESTRICT __restrict__
#else
#define DSD_NEO_RESTRICT
#endif

/* SIMD target attribute macro */
#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_TARGET_ATTR(x) __attribute__((target(x)))
#else
#define DSD_NEO_TARGET_ATTR(x)
#endif

/* CPUID bits for feature detection (avoid redefining if provided by <cpuid.h>) */
#if defined(__x86_64__) || defined(__i386__)
#ifndef bit_SSE2
#define bit_SSE2 (1u << 26)
#endif
#ifndef bit_SSSE3
#define bit_SSSE3 (1u << 9)
#endif
#ifndef bit_AVX
#define bit_AVX (1u << 28)
#endif
#ifndef bit_AVX2
#define bit_AVX2 (1u << 5)
#endif
#ifndef bit_OSXSAVE
#define bit_OSXSAVE (1u << 27)
#endif
#endif

/* Forward declarations for runtime dispatch */
static void dsd_neo_init_runtime_dispatch_once(void);
static void dsd_neo_init_runtime_dispatch(void);

/* Runtime dispatch control */
static pthread_once_t dsd_neo_dispatch_once_control = PTHREAD_ONCE_INIT;
static dsd_neo_widen_fn g_widen_impl = NULL;
static dsd_neo_widen_rot_fn g_widen_rot_impl = NULL;

/**
 * @brief Widen u8 to s16 centered at 127 via runtime-dispatched implementation.
 *
 * Public wrapper that lazy-initializes runtime dispatch and widens u8 to s16
 * centered at 127.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination int16 buffer.
 * @param len Number of bytes in src to process.
 */
void
widen_u8_to_s16_bias127(const unsigned char* DSD_NEO_RESTRICT src, int16_t* DSD_NEO_RESTRICT dst, uint32_t len) {
    if (!g_widen_impl) {
        dsd_neo_init_runtime_dispatch();
    }
    g_widen_impl(src, dst, len);
}

/**
 * @brief Rotate 90° (IQ) and widen u8→s16 centered at 127 via runtime dispatch.
 *
 * Public wrapper that lazy-initializes runtime dispatch and performs 90° IQ
 * rotation combined with widen u8→s16 centered at 127.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination int16 buffer.
 * @param len Number of bytes in src to process.
 */
void
widen_rotate90_u8_to_s16_bias127(const unsigned char* DSD_NEO_RESTRICT src, int16_t* DSD_NEO_RESTRICT dst,
                                 uint32_t len) {
    if (!g_widen_rot_impl) {
        dsd_neo_init_runtime_dispatch();
    }
    g_widen_rot_impl(src, dst, len);
}

/**
 * @brief Widen u8 to s16 centered at 128 (for legacy pre-rotation negation).
 *
 * Scalar widening that subtracts 128 instead of 127.
 * Intended to pair with legacy byte-wise rotate_90(u8) which performs 255-x
 * negation so that overall effect equals correct centered negation (127-x).
 *
 * @param src Source buffer of unsigned bytes.
 * @param dst Destination int16 buffer.
 * @param len Number of bytes to process.
 */
void
widen_u8_to_s16_bias128_scalar(const unsigned char* DSD_NEO_RESTRICT src, int16_t* DSD_NEO_RESTRICT dst, uint32_t len) {
    uint32_t i = 0;
    for (; i < len; i++) {
        int v = (int)src[i] - 128; /* range: [-128,127] */
        v <<= 8;                   /* scale to int16 domain */
        if (v > 32767) {
            v = 32767; /* saturate (theoretical no-hit here) */
        }
        if (v < -32768) {
            v = -32768;
        }
        dst[i] = (int16_t)v;
    }
}

/**
 * @brief Scalar fallback: widen u8 to s16 centered at 127.
 *
 * @param src Source buffer of unsigned bytes.
 * @param dst Destination int16 buffer.
 * @param len Number of bytes to process.
 */
static inline void
widen_u8_to_s16_bias127_scalar(const unsigned char* DSD_NEO_RESTRICT src, int16_t* DSD_NEO_RESTRICT dst, uint32_t len) {
    uint32_t i = 0;
    /* Scalar conversion with scaling: (u8 - 127) * 256 -> s16 (saturated) */
    for (; i < len; i++) {
        int v = (int)src[i] - 127; /* range: [-127,128] */
        /* Avoid 128*256 == 32768 overflow by pre-clamping */
        if (v > 127) {
            v = 127;
        }
        v <<= 8;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        dst[i] = (int16_t)v;
    }
}

/**
 * @brief Combined 90° rotation (1, j, -1, -j) + widen (u8→s16 centered at 127).
 *
 * Processes 4 IQ samples per iteration to avoid branches.
 *
 * @param src Source buffer of unsigned bytes (I/Q interleaved).
 * @param dst Destination int16 buffer.
 * @param len Number of bytes in src to process.
 */
static inline void
widen_rotate90_u8_to_s16_bias127_scalar(const unsigned char* DSD_NEO_RESTRICT src, int16_t* DSD_NEO_RESTRICT dst,
                                        uint32_t len) {
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) {
        int i0 = (int)src[i + 0] - 127;
        int q0 = (int)src[i + 1] - 127;
        if (i0 > 127) {
            i0 = 127;
        }
        if (q0 > 127) {
            q0 = 127;
        }
        i0 <<= 8;
        q0 <<= 8;
        if (i0 > 32767) {
            i0 = 32767;
        }
        if (i0 < -32768) {
            i0 = -32768;
        }
        if (q0 > 32767) {
            q0 = 32767;
        }
        if (q0 < -32768) {
            q0 = -32768;
        }
        dst[i + 0] = (int16_t)i0;
        dst[i + 1] = (int16_t)q0;

        int i1 = (int)src[i + 2] - 127;
        int q1 = (int)src[i + 3] - 127;
        int r20 = -q1, r21 = i1;
        if (r20 > 127) {
            r20 = 127;
        }
        if (r21 > 127) {
            r21 = 127;
        }
        if (r20 < -128) {
            r20 = -128;
        }
        if (r21 < -128) {
            r21 = -128;
        }
        r20 <<= 8;
        r21 <<= 8;
        if (r20 > 32767) {
            r20 = 32767;
        }
        if (r20 < -32768) {
            r20 = -32768;
        }
        if (r21 > 32767) {
            r21 = 32767;
        }
        if (r21 < -32768) {
            r21 = -32768;
        }
        dst[i + 2] = (int16_t)r20;
        dst[i + 3] = (int16_t)r21;

        int i2 = (int)src[i + 4] - 127;
        int q2 = (int)src[i + 5] - 127;
        int r40 = -i2, r41 = -q2;
        if (r40 > 127) {
            r40 = 127;
        }
        if (r41 > 127) {
            r41 = 127;
        }
        if (r40 < -128) {
            r40 = -128;
        }
        if (r41 < -128) {
            r41 = -128;
        }
        r40 <<= 8;
        r41 <<= 8;
        if (r40 > 32767) {
            r40 = 32767;
        }
        if (r40 < -32768) {
            r40 = -32768;
        }
        if (r41 > 32767) {
            r41 = 32767;
        }
        if (r41 < -32768) {
            r41 = -32768;
        }
        dst[i + 4] = (int16_t)r40;
        dst[i + 5] = (int16_t)r41;

        int i3 = (int)src[i + 6] - 127;
        int q3 = (int)src[i + 7] - 127;
        int r60 = q3, r61 = -i3;
        if (r60 > 127) {
            r60 = 127;
        }
        if (r61 > 127) {
            r61 = 127;
        }
        if (r60 < -128) {
            r60 = -128;
        }
        if (r61 < -128) {
            r61 = -128;
        }
        r60 <<= 8;
        r61 <<= 8;
        if (r60 > 32767) {
            r60 = 32767;
        }
        if (r60 < -32768) {
            r60 = -32768;
        }
        if (r61 > 32767) {
            r61 = 32767;
        }
        if (r61 < -32768) {
            r61 = -32768;
        }
        dst[i + 6] = (int16_t)r60;
        dst[i + 7] = (int16_t)r61;
    }
    /* Tail: apply rotation pattern for remaining up to 6 samples */
    if (i < len) {
        uint32_t base = i;
        uint32_t rem = len - base;
        if (rem >= 2) {
            int v0 = (int)src[base + 0] - 127;
            int v1 = (int)src[base + 1] - 127;
            if (v0 > 127) {
                v0 = 127;
            }
            if (v1 > 127) {
                v1 = 127;
            }
            v0 <<= 8;
            v1 <<= 8;
            if (v0 > 32767) {
                v0 = 32767;
            }
            if (v0 < -32768) {
                v0 = -32768;
            }
            if (v1 > 32767) {
                v1 = 32767;
            }
            if (v1 < -32768) {
                v1 = -32768;
            }
            dst[base + 0] = (int16_t)v0;
            dst[base + 1] = (int16_t)v1;
        }
        if (rem >= 4) {
            int i1 = (int)src[base + 2] - 127;
            int q1 = (int)src[base + 3] - 127;
            int r20 = -q1, r21 = i1;
            if (r20 > 127) {
                r20 = 127;
            }
            if (r21 > 127) {
                r21 = 127;
            }
            if (r20 < -128) {
                r20 = -128;
            }
            if (r21 < -128) {
                r21 = -128;
            }
            r20 <<= 8;
            r21 <<= 8;
            if (r20 > 32767) {
                r20 = 32767;
            }
            if (r20 < -32768) {
                r20 = -32768;
            }
            if (r21 > 32767) {
                r21 = 32767;
            }
            if (r21 < -32768) {
                r21 = -32768;
            }
            dst[base + 2] = (int16_t)r20;
            dst[base + 3] = (int16_t)r21;
        }
        if (rem >= 6) {
            int i2 = (int)src[base + 4] - 127;
            int q2 = (int)src[base + 5] - 127;
            int r40 = -i2, r41 = -q2;
            if (r40 > 127) {
                r40 = 127;
            }
            if (r41 > 127) {
                r41 = 127;
            }
            if (r40 < -128) {
                r40 = -128;
            }
            if (r41 < -128) {
                r41 = -128;
            }
            r40 <<= 8;
            r41 <<= 8;
            if (r40 > 32767) {
                r40 = 32767;
            }
            if (r40 < -32768) {
                r40 = -32768;
            }
            if (r41 > 32767) {
                r41 = 32767;
            }
            if (r41 < -32768) {
                r41 = -32768;
            }
            dst[base + 4] = (int16_t)r40;
            dst[base + 5] = (int16_t)r41;
        }
    }
}

#if defined(__x86_64__) || defined(__i386__)
/* AVX2 specializations */
/**
 * @brief AVX2: widen unsigned bytes to signed 16-bit centered at 127.
 *
 * @param src Source u8 buffer.
 * @param dst Destination s16 buffer.
 * @param len Number of bytes to process.
 */
static void
DSD_NEO_TARGET_ATTR("avx2") widen_u8_to_s16_bias127_avx2(const unsigned char* src, int16_t* dst, uint32_t len) {
    uint32_t i = 0;
    const __m256i bias256 = _mm256_set1_epi16(127);
    const __m256i max127 = _mm256_set1_epi16(127);
    for (; i + 32 <= len; i += 32) {
        __m128i b0 = _mm_loadu_si128((const __m128i*)(src + i));
        __m128i b1 = _mm_loadu_si128((const __m128i*)(src + i + 16));
        __m256i lo = _mm256_cvtepu8_epi16(b0);
        __m256i hi = _mm256_cvtepu8_epi16(b1);
        lo = _mm256_sub_epi16(lo, bias256);
        hi = _mm256_sub_epi16(hi, bias256);
        /* Pre-clamp 128 -> 127 to avoid 0x8000 after shift */
        lo = _mm256_min_epi16(lo, max127);
        hi = _mm256_min_epi16(hi, max127);
        lo = _mm256_slli_epi16(lo, 8);
        hi = _mm256_slli_epi16(hi, 8);
        _mm256_storeu_si256((__m256i*)(dst + i), lo);
        _mm256_storeu_si256((__m256i*)(dst + i + 16), hi);
    }
    for (; i < len; i++) {
        int v = (int)src[i] - 127;
        if (v > 127) {
            v = 127;
        }
        v <<= 8;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        dst[i] = (int16_t)v;
    }
}

/**
 * @brief AVX2: rotate (1,j,-1,-j) and widen u8→s16 centered at 127.
 *
 * Tail elements are handled by a scalar helper to preserve the rotation pattern.
 * @param src Source u8 buffer (I/Q interleaved).
 * @param dst Destination s16 buffer.
 * @param len Number of bytes to process.
 */
static void
DSD_NEO_TARGET_ATTR("avx2")
    widen_rotate90_u8_to_s16_bias127_avx2(const unsigned char* src, int16_t* dst, uint32_t len) {
    const __m256i shuffle = _mm256_setr_epi8(0, 1, 3, 2, 4, 5, 7, 6, 8, 9, 11, 10, 12, 13, 15, 14, 0, 1, 3, 2, 4, 5, 7,
                                             6, 8, 9, 11, 10, 12, 13, 15, 14);
    const __m256i mask_sel =
        _mm256_setr_epi16(0x0000, 0x0000, (short)0xFFFF, 0x0000, (short)0xFFFF, (short)0xFFFF, 0x0000, (short)0xFFFF,
                          0x0000, 0x0000, (short)0xFFFF, 0x0000, (short)0xFFFF, (short)0xFFFF, 0x0000, (short)0xFFFF);
    const __m256i c127 = _mm256_set1_epi16(127);
    const __m256i max127 = _mm256_set1_epi16(127);
    uint32_t i = 0;
    for (; i + 32 <= len; i += 32) {
        __m256i v8 = _mm256_loadu_si256((const __m256i*)(src + i));
        __m256i sh = _mm256_shuffle_epi8(v8, shuffle);
        __m128i sh_lo = _mm256_castsi256_si128(sh);
        __m128i sh_hi = _mm256_extracti128_si256(sh, 1);
        __m256i v16_lo = _mm256_cvtepu8_epi16(sh_lo);
        __m256i v16_hi = _mm256_cvtepu8_epi16(sh_hi);
        __m256i bs_lo = _mm256_sub_epi16(v16_lo, c127);
        __m256i bm_lo = _mm256_sub_epi16(c127, v16_lo);
        __m256i bs_hi = _mm256_sub_epi16(v16_hi, c127);
        __m256i bm_hi = _mm256_sub_epi16(c127, v16_hi);
        __m256i out_lo = _mm256_blendv_epi8(bs_lo, bm_lo, mask_sel);
        __m256i out_hi = _mm256_blendv_epi8(bs_hi, bm_hi, mask_sel);
        /* Pre-clamp 128 -> 127 to avoid 0x8000 */
        out_lo = _mm256_min_epi16(out_lo, max127);
        out_hi = _mm256_min_epi16(out_hi, max127);
        out_lo = _mm256_slli_epi16(out_lo, 8);
        out_hi = _mm256_slli_epi16(out_hi, 8);
        _mm256_storeu_si256((__m256i*)(dst + i), out_lo);
        _mm256_storeu_si256((__m256i*)(dst + i + 16), out_hi);
    }
    /* Tail: preserve rotation via scalar helper */
    if (i < len) {
        widen_rotate90_u8_to_s16_bias127_scalar(src + i, dst + i, len - i);
    }
}

/**
 * @brief SSE2: widen unsigned bytes to signed 16-bit centered at 127.
 */
static void
DSD_NEO_TARGET_ATTR("sse2") widen_u8_to_s16_bias127_sse2(const unsigned char* src, int16_t* dst, uint32_t len) {
    uint32_t i = 0;
    const __m128i bias = _mm_set1_epi16(127);
    const __m128i zero = _mm_setzero_si128();
    const __m128i max127 = _mm_set1_epi16(127);
    const __m128i one = _mm_set1_epi16(1);
    for (; i + 16 <= len; i += 16) {
        __m128i b = _mm_loadu_si128((const __m128i*)(src + i));
        __m128i lo = _mm_unpacklo_epi8(b, zero);
        __m128i hi = _mm_unpackhi_epi8(b, zero);
        lo = _mm_sub_epi16(lo, bias);
        hi = _mm_sub_epi16(hi, bias);
        /* Shift left by 8 with saturating fix for 128 -> 127 */
        __m128i mask_lo = _mm_cmpeq_epi16(lo, _mm_set1_epi16(128));
        __m128i mask_hi = _mm_cmpeq_epi16(hi, _mm_set1_epi16(128));
        lo = _mm_min_epi16(lo, max127);
        hi = _mm_min_epi16(hi, max127);
        lo = _mm_slli_epi16(lo, 8);
        hi = _mm_slli_epi16(hi, 8);
        /* Correct lanes that were exactly 128 pre-clamp: subtract 1 to map 0x8000 -> 0x7FFF */
        lo = _mm_sub_epi16(lo, _mm_and_si128(mask_lo, one));
        hi = _mm_sub_epi16(hi, _mm_and_si128(mask_hi, one));
        _mm_storeu_si128((__m128i*)(dst + i), lo);
        _mm_storeu_si128((__m128i*)(dst + i + 8), hi);
    }
    for (; i < len; i++) {
        int v = (int)src[i] - 127;
        if (v > 127) {
            v = 127;
        }
        v <<= 8;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        dst[i] = (int16_t)v;
    }
}

/**
 * @brief SSE2: fallback rotate+widen via scalar since SSE2 lacks byte shuffle.
 */
static void
DSD_NEO_TARGET_ATTR("sse2")
    widen_rotate90_u8_to_s16_bias127_sse2(const unsigned char* src, int16_t* dst, uint32_t len) {
    /* Keep scalar logic for correctness without SSSE3 pshufb (SSE2 lacks byte shuffle). */
    widen_rotate90_u8_to_s16_bias127_scalar(src, dst, len);
}
#endif /* x86 */

#if defined(__x86_64__) || defined(__i386__)
/**
 * @brief SSSE3: rotate (1,j,-1,-j) and widen u8→s16 centered at 127.
 *
 * Tail elements are handled by a scalar helper to preserve the rotation pattern.
 */
static void
DSD_NEO_TARGET_ATTR("ssse3")
    widen_rotate90_u8_to_s16_bias127_ssse3(const unsigned char* src, int16_t* dst, uint32_t len) {
    uint32_t i = 0;
    const __m128i shuffle = _mm_setr_epi8(0, 1, 3, 2, 4, 5, 7, 6, 8, 9, 11, 10, 12, 13, 15, 14);
    const __m128i mask_sel =
        _mm_setr_epi16(0x0000, 0x0000, (short)0xFFFF, 0x0000, (short)0xFFFF, (short)0xFFFF, 0x0000, (short)0xFFFF);
    const __m128i c127 = _mm_set1_epi16(127);
    const __m128i zero = _mm_setzero_si128();
    const __m128i max127 = _mm_set1_epi16(127);
    const __m128i one = _mm_set1_epi16(1);
    for (; i + 16 <= len; i += 16) {
        __m128i v8 = _mm_loadu_si128((const __m128i*)(src + i));
        __m128i sh = _mm_shuffle_epi8(v8, shuffle);
        __m128i v16_lo = _mm_unpacklo_epi8(sh, zero);
        __m128i v16_hi = _mm_unpackhi_epi8(sh, zero);
        __m128i bs_lo = _mm_sub_epi16(v16_lo, c127);
        __m128i bm_lo = _mm_sub_epi16(c127, v16_lo);
        __m128i bs_hi = _mm_sub_epi16(v16_hi, c127);
        __m128i bm_hi = _mm_sub_epi16(c127, v16_hi);
        __m128i out_lo = _mm_or_si128(_mm_and_si128(bm_lo, mask_sel), _mm_andnot_si128(mask_sel, bs_lo));
        __m128i out_hi = _mm_or_si128(_mm_and_si128(bm_hi, mask_sel), _mm_andnot_si128(mask_sel, bs_hi));
        /* Identify exactly-128 lanes to correct after shift */
        __m128i mask128_lo = _mm_cmpeq_epi16(out_lo, _mm_set1_epi16(128));
        __m128i mask128_hi = _mm_cmpeq_epi16(out_hi, _mm_set1_epi16(128));
        out_lo = _mm_min_epi16(out_lo, max127);
        out_hi = _mm_min_epi16(out_hi, max127);
        out_lo = _mm_slli_epi16(out_lo, 8);
        out_hi = _mm_slli_epi16(out_hi, 8);
        out_lo = _mm_sub_epi16(out_lo, _mm_and_si128(mask128_lo, one));
        out_hi = _mm_sub_epi16(out_hi, _mm_and_si128(mask128_hi, one));
        _mm_storeu_si128((__m128i*)(dst + i), out_lo);
        _mm_storeu_si128((__m128i*)(dst + i + 8), out_hi);
    }
    if (i < len) {
        widen_rotate90_u8_to_s16_bias127_scalar(src + i, dst + i, len - i);
    }
}
#endif /* x86 */

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
/**
 * @brief NEON: widen unsigned bytes to signed 16-bit centered at 127.
 */
static void
widen_u8_to_s16_bias127_neon(const unsigned char* src, int16_t* dst, uint32_t len) {
    uint32_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8(src + i);
        uint8x8_t v_lo = vget_low_u8(v);
        uint8x8_t v_hi = vget_high_u8(v);
        int16x8_t v16_lo = vreinterpretq_s16_u16(vmovl_u8(v_lo));
        int16x8_t v16_hi = vreinterpretq_s16_u16(vmovl_u8(v_hi));
        int16x8_t lo = vsubq_s16(v16_lo, vdupq_n_s16(127));
        int16x8_t hi = vsubq_s16(v16_hi, vdupq_n_s16(127));
        /* Saturating left shift by 8 */
        lo = vqshlq_n_s16(lo, 8);
        hi = vqshlq_n_s16(hi, 8);
        vst1q_s16(dst + i, lo);
        vst1q_s16(dst + i + 8, hi);
    }
    for (; i < len; i++) {
        int v = (int)src[i] - 127;
        if (v > 127) {
            v = 127;
        }
        v <<= 8;
        if (v > 32767) {
            v = 32767;
        }
        if (v < -32768) {
            v = -32768;
        }
        dst[i] = (int16_t)v;
    }
}

/**
 * @brief NEON: rotate (1,j,-1,-j) and widen u8→s16 centered at 127.
 *
 * Uses table lookup on aarch64; on ARMv7 (no vqtbl1q_u8) falls back to scalar.
 */
static void
widen_rotate90_u8_to_s16_bias127_neon(const unsigned char* src, int16_t* dst, uint32_t len) {
#if defined(__aarch64__)
    const uint8x16_t tbl_idx = {0, 1, 3, 2, 4, 5, 7, 6, 8, 9, 11, 10, 12, 13, 15, 14};
    const int16x8_t c127 = vdupq_n_s16(127);
    const uint16_t mpat[8] = {0x0000, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF};
    const uint16x8_t msel = vld1q_u16(mpat);
    uint32_t i = 0;
    for (; i + 16 <= len; i += 16) {
        uint8x16_t v = vld1q_u8(src + i);
        uint8x16_t sh = vqtbl1q_u8(v, tbl_idx);
        uint8x8_t sh_lo8 = vget_low_u8(sh);
        uint8x8_t sh_hi8 = vget_high_u8(sh);
        int16x8_t v16_lo = vreinterpretq_s16_u16(vmovl_u8(sh_lo8));
        int16x8_t v16_hi = vreinterpretq_s16_u16(vmovl_u8(sh_hi8));
        int16x8_t bs_lo = vsubq_s16(v16_lo, c127);
        int16x8_t bm_lo = vsubq_s16(c127, v16_lo);
        int16x8_t bs_hi = vsubq_s16(v16_hi, c127);
        int16x8_t bm_hi = vsubq_s16(c127, v16_hi);
        int16x8_t out_lo = vbslq_s16(msel, bm_lo, bs_lo);
        int16x8_t out_hi = vbslq_s16(msel, bm_hi, bs_hi);
        /* Saturating left shift by 8 */
        out_lo = vqshlq_n_s16(out_lo, 8);
        out_hi = vqshlq_n_s16(out_hi, 8);
        vst1q_s16(dst + i, out_lo);
        vst1q_s16(dst + i + 8, out_hi);
    }
    /* Tail: preserve rotation via scalar helper */
    if (i < len) {
        widen_rotate90_u8_to_s16_bias127_scalar(src + i, dst + i, len - i);
    }
#else
    /* ARMv7 NEON lacks vqtbl1q_u8; use scalar fallback for rotate+widen */
    widen_rotate90_u8_to_s16_bias127_scalar(src, dst, len);
#endif
}
#endif

/**
 * @brief Runtime CPU feature detection and dispatch binding for widening/rotation.
 *
 * Detects available SIMD features (AVX2/SSSE3/SSE2/NEON) and binds the
 * function pointers `g_widen_impl` and `g_widen_rot_impl` accordingly.
 * Safe to call concurrently; guarded by pthread_once in the public wrapper.
 */
static void
dsd_neo_init_runtime_dispatch_once(void) {

#if defined(__x86_64__) || defined(__i386__)
    int use_avx2 = 0;
    int use_sse2 = 0;
    int use_ssse3 = 0;
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        use_sse2 = (edx & bit_SSE2) ? 1 : 0;
        use_ssse3 = (ecx & bit_SSSE3) ? 1 : 0;
        int osxsave = (ecx & bit_OSXSAVE) ? 1 : 0;
        int avx = (ecx & bit_AVX) ? 1 : 0;
        if (osxsave && avx) {
            uint32_t xcr0_lo = 0, xcr0_hi = 0;
            /* xgetbv ecx=0 is non-privileged on userland */
            __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
            uint64_t xcr0 = ((uint64_t)xcr0_hi << 32) | xcr0_lo;
            if ((xcr0 & 0x6) == 0x6) {
                unsigned int eax7 = 0, ebx7 = 0, ecx7 = 0, edx7 = 0;
                if (__get_cpuid_count(7, 0, &eax7, &ebx7, &ecx7, &edx7)) {
                    use_avx2 = (ebx7 & bit_AVX2) ? 1 : 0;
                }
            }
        }
    }
#if defined(__x86_64__)
    if (!use_sse2) {
        use_sse2 = 1; /* mandatory on x86_64 */
    }
#endif
#endif

#if defined(__aarch64__)
    int use_neon = 1; /* ASIMD mandatory */
#elif defined(__arm__)
    int use_neon = 0;
#if defined(__linux__)
    unsigned long hw = getauxval(AT_HWCAP);
    /* HWCAP_NEON may be undefined on some headers; fallback to known value 4096 */
#ifndef HWCAP_NEON
#define HWCAP_NEON 4096
#endif
    use_neon = (hw & HWCAP_NEON) ? 1 : 0;
#endif
#if !defined(__ARM_NEON) && !defined(__ARM_NEON__)
    (void)use_neon;
#endif
#endif

    /* Fallbacks */
    g_widen_impl = &widen_u8_to_s16_bias127_scalar;
    g_widen_rot_impl = &widen_rotate90_u8_to_s16_bias127_scalar;

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
    if (use_neon) {
        g_widen_impl = &widen_u8_to_s16_bias127_neon;
        g_widen_rot_impl = &widen_rotate90_u8_to_s16_bias127_neon;
        return;
    }
#endif

#if defined(__x86_64__) || defined(__i386__)
    if (use_avx2) {
        g_widen_impl = &widen_u8_to_s16_bias127_avx2;
        g_widen_rot_impl = &widen_rotate90_u8_to_s16_bias127_avx2;
        return;
    }
    if (use_ssse3 || use_sse2) {
        if (use_sse2) {
            g_widen_impl = &widen_u8_to_s16_bias127_sse2;
        }
        if (use_ssse3) {
            g_widen_rot_impl = &widen_rotate90_u8_to_s16_bias127_ssse3;
        } else {
            g_widen_rot_impl = &widen_rotate90_u8_to_s16_bias127_sse2;
        }
        return;
    }
#endif
}

/**
 * @brief Ensure SIMD dispatch is initialized (thread-safe, idempotent).
 */
static void
dsd_neo_init_runtime_dispatch(void) {
    /* Thread-safe, idempotent initialization */
    pthread_once(&dsd_neo_dispatch_once_control, dsd_neo_init_runtime_dispatch_once);
}
