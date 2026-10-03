// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_DSP_SIMD_WIDEN_INTERNAL_H_
#define DSD_NEO_SRC_DSP_SIMD_WIDEN_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include "dsd-neo/core/safe_api.h"

/*
 * The bounded cf32 copy (bound_cf32_to_f32() and bound_rotate90_cf32_to_f32_phase() in <dsd-neo/dsp/simd_widen.h>),
 * shared by every backend: each supplies only a kernel for one fs/4 period, four complex samples (32 bytes).
 *
 * External cf32 samples (a SoapySDR driver's CF32 buffer, a cf32 I/Q replay) are floats nothing has checked, and one
 * Inf or NaN among them poisons every IIR stage downstream until the next retune. So each component enters as 0 when
 * it is Inf, NaN or at least 2^60 in magnitude, the bound the Costas and Gardner loops apply to their input
 * (sample_bits_in_range() in costas.cpp). The global fast-math option folds std::isfinite() to true, and Clang also
 * folds a bit test of a float it computed or took as an argument, so the test compares the bits just loaded from the
 * source as integers. A failing component is replaced, and the fs/4 rotation applied, on those bits: a rotation by a
 * multiple of 90 degrees only swaps the components and flips their signs, so no Inf or NaN reaches a floating-point
 * operation, and the result is bit-identical to rotating the bounded floats.
 *
 * Everything here has internal linkage, so each backend's unit, built with its own instruction set, keeps its own copy.
 */
constexpr uint32_t kCf32MagnitudeMask = 0x7FFFFFFFu;
constexpr uint32_t kCf32SignBit = 0x80000000u;
constexpr uint32_t kCf32MagnitudeLimitBits = 0x5D800000u; /* 2^60 */
/* The largest magnitude that enters, for the SIMD kernels' signed compares (a magnitude is never negative). */
constexpr uint32_t kCf32MagnitudeLargestBits = kCf32MagnitudeLimitBits - 1U;
/* The sign flips of one fs/4 period, phases 0 to 3: (i0, q0), (-q1, i1), (-i2, -q2), (q3, -i3). */
constexpr uint32_t kCf32PeriodSigns[8] = {0U, 0U, kCf32SignBit, 0U, kCf32SignBit, kCf32SignBit, 0U, kCf32SignBit};

/* The component's bits, or 0 (+0.0f) when they are Inf, NaN or of magnitude 2^60 or more. */
static inline uint32_t
cf32_bound_bits(uint32_t bits) {
    return ((bits & kCf32MagnitudeMask) < kCf32MagnitudeLimitBits) ? bits : 0U;
}

/* One complex sample, bounded and rotated at fs/4 phase @p phase: phase 1 gives (-q, i), 2 (-i, -q), 3 (q, -i). @p src
   is the sample's 8 bytes at any alignment, and may be @p dst itself. */
static inline void
cf32_copy_sample_bounded(float* dst, const void* src, uint32_t phase) {
    uint32_t in[2];
    DSD_MEMCPY(in, src, sizeof(in));
    in[0] = cf32_bound_bits(in[0]);
    in[1] = cf32_bound_bits(in[1]);
    uint32_t out[2] = {in[0], in[1]};
    switch (phase & 3U) {
        case 0: break;
        case 1:
            out[0] = in[1] ^ kCf32SignBit;
            out[1] = in[0];
            break;
        case 2:
            out[0] = in[0] ^ kCf32SignBit;
            out[1] = in[1] ^ kCf32SignBit;
            break;
        default:
            out[0] = in[1];
            out[1] = in[0] ^ kCf32SignBit;
            break;
    }
    DSD_MEMCPY(dst, out, sizeof(out));
}

/* For the helpers cf32_copy_bounded() calls with the pointers it has already checked: neither may be null. */
#if defined(__GNUC__) || defined(__clang__)
#define DSD_NEO_CF32_NONNULL __attribute__((nonnull))
#else
#define DSD_NEO_CF32_NONNULL
#endif

/* How a kernel copies the periods of an unrotated copy (its period_copy type); see cf32_copy_bounded(). */
struct cf32_periods_flagged {};

struct cf32_periods_bounded {};

/* The unrotated copy from sample @p i to @p pairs: whole periods through a flagged kernel, then single samples, then
   the in-place bounding of the periods if the kernel reported any component out of range. */
template <typename Kernel>
DSD_NEO_CF32_NONNULL static inline void
cf32_copy_unrotated_from(float* dst, const unsigned char* in, size_t i, size_t pairs, cf32_periods_flagged /*mode*/) {
    const size_t body = i;
    const size_t body_pairs = ((pairs - i) / 4U) * 4U;
    const bool body_out_of_range = Kernel::copy_periods_flagged(dst + (i * 2U), in + (i * 8U), body_pairs / 4U);
    i += body_pairs;
    for (; i < pairs; i++) {
        cf32_copy_sample_bounded(dst + (i * 2U), in + (i * 8U), 0U);
    }
    if (body_out_of_range) {
        for (size_t k = body; k < body + body_pairs; k++) {
            cf32_copy_sample_bounded(dst + (k * 2U), dst + (k * 2U), 0U);
        }
    }
}

/* The same through a kernel that bounds the periods as it copies them: nothing is left to bound afterwards. */
template <typename Kernel>
DSD_NEO_CF32_NONNULL static inline void
cf32_copy_unrotated_from(float* dst, const unsigned char* in, size_t i, size_t pairs, cf32_periods_bounded /*mode*/) {
    const size_t body_pairs = ((pairs - i) / 4U) * 4U;
    Kernel::copy_periods(dst + (i * 2U), in + (i * 8U), body_pairs / 4U);
    i += body_pairs;
    for (; i < pairs; i++) {
        cf32_copy_sample_bounded(dst + (i * 2U), in + (i * 8U), 0U);
    }
}

/*
 * Copies @p pairs complex samples (I then Q, at any alignment) from @p src to @p dst, bounded, and with @p rotate
 * rotated at fs/4 phases @p phase, phase + 1, ...; returns the phase after the last sample (@p phase & 3 without
 * @p rotate). The kernel handles whole fs/4 periods; single samples run before and after them.
 *
 * Rotating, single samples run up to the first phase-0 sample, then Kernel::copy<true>(dst, src) one period at a time
 * (two a pass: with one, GCC's SSE2 loop took up to half as long again).
 *
 * Not rotating, the copy has to match a plain memcpy, which the check would slow by a few percent if every store
 * waited for it. So a SIMD kernel (Kernel::period_copy is cf32_periods_flagged) supplies
 * Kernel::copy_periods_flagged(dst, src, periods), which copies the periods as they are, its stores fed by the loads
 * alone, and only reports whether any component was out of range; in that rare case the components it copied are
 * bounded afterwards, in place. The scalar kernel (cf32_periods_bounded) gains nothing from checking apart, so its
 * Kernel::copy_periods(dst, src, periods) bounds the periods as it copies them and leaves nothing to report. Single
 * samples, bounded as they are copied, run first up to a 32-byte store boundary (where dst holds whole samples, so
 * that it can reach one: a vector store split across cache lines costs the copy a fifth of its speed) and after the
 * periods.
 */
template <typename Kernel>
static inline uint32_t
cf32_copy_bounded(float* dst, const void* src, size_t pairs, bool rotate, uint32_t phase) {
    uint32_t p = phase & 3U;
    if (!dst || !src) {
        return p;
    }
    const unsigned char* in = static_cast<const unsigned char*>(src);
    size_t i = 0U;
    if (rotate) {
        for (; i < pairs && p != 0U; i++) {
            cf32_copy_sample_bounded(dst + (i * 2U), in + (i * 8U), p);
            p = (p + 1U) & 3U;
        }
        for (; i + 8U <= pairs; i += 8U) {
            Kernel::template copy<true>(dst + (i * 2U), in + (i * 8U));
            Kernel::template copy<true>(dst + (i * 2U) + 8U, in + (i * 8U) + 32U);
        }
        for (; i + 4U <= pairs; i += 4U) {
            Kernel::template copy<true>(dst + (i * 2U), in + (i * 8U));
        }
        for (; i < pairs; i++) {
            cf32_copy_sample_bounded(dst + (i * 2U), in + (i * 8U), p);
            p = (p + 1U) & 3U;
        }
        return p;
    }
    const uintptr_t dst_addr = reinterpret_cast<uintptr_t>(dst);
    const size_t align_samples = ((dst_addr & 7U) == 0U) ? (((32U - (dst_addr & 31U)) & 31U) / 8U) : 0U;
    for (; i < pairs && i < align_samples; i++) {
        cf32_copy_sample_bounded(dst + (i * 2U), in + (i * 8U), 0U);
    }
    cf32_copy_unrotated_from<Kernel>(dst, in, i, pairs, typename Kernel::period_copy{});
    return p;
}

#endif /* DSD_NEO_SRC_DSP_SIMD_WIDEN_INTERNAL_H_ */
