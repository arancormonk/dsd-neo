// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SIMD FIR filter dispatch API with runtime CPU detection.
 *
 * Provides vectorized implementations of FIR filter hot paths, each streaming (its output does not depend on how its
 * input is cut into calls):
 * - Complex half-band decimator (exploits zero-tap sparsity + symmetry)
 * - Complex general symmetric FIR (exploits symmetry only)
 * - Real half-band decimator (exploits zero-tap sparsity + symmetry)
 *
 * The streaming contract the three share (issue #572):
 * - Units: simd_fir_complex_apply() counts complex samples, simd_hb_decim2_complex() floats (two a complex sample) and
 *   simd_hb_decim2_real() real samples.
 * - State: a history of the newest inputs and a pending count of inputs taken but not yet filtered past, which carries
 *   the look-ahead (and the half-band's decimation phase) from one call to the next. A zeroed history with pending = 0
 *   is the only reset: the next output is centred on the next input, and whatever was pending is dropped. The
 *   demodulator's one reset is dsd_demod_reset_filter_state() (<dsd-neo/dsp/demod_pipeline.h>).
 * - Latency: an output comes out only once the c = (taps_len - 1) / 2 inputs after its centre have arrived, so each
 *   filter holds back c inputs at its own rate. On the default 1.536 Msps RTL chain the five-pass cascade and the
 *   135-tap channel filter hold about 74 samples at 48 kHz, 1.54 ms. Nothing here flushes them; the pipeline drops
 *   them at a reset and at the end of an I/Q replay.
 * - Capacity: out_cap must hold every output a channel-filter call makes, up to N + c_old - c_new after a tap-count
 *   shrink; a half-band call makes at most (N + 1) / 2, so each cascade stage fits in its input's buffer.
 * - An invalid call returns -1 and touches nothing, and no call consumes part of its input. Scalar output is
 *   bit-exact across any split of the input; SSE2, AVX2, NEON and the dispatcher agree with their own whole-stream
 *   output within 1e-5.
 * - Only these filters stream. The demodulator stages after them still decide per block: channel power and the
 *   squelch decision, the squelch envelope, I/Q balance, the AM detector's warm start, CQPSK's adaptive Gardner gain,
 *   the rounding of a squelched block's zero-symbol count, the metrics, and the block a profile request is consumed
 *   on (docs/code_map.md, "Streaming linear front end").
 *
 * Runtime dispatch automatically selects the best available implementation:
 * - x86-64: AVX2+FMA > SSE2 > scalar
 * - ARM64: NEON (always available)
 * - Other: scalar fallback
 */

#ifndef DSD_NEO_SIMD_FIR_H
#define DSD_NEO_SIMD_FIR_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Complex interleaved general symmetric FIR filter (no decimation), streaming.
 * Used for channel_lpf_apply(). Exploits tap symmetry (fold pairs) but NOT zero-tap skipping.
 *
 * The output is centred on its input sample and does not depend on how the input is cut into calls: a call makes
 * only the outputs whose look-ahead it holds and keeps the rest pending for a later call. With c = (taps_len - 1) / 2
 * and N = n_in, a call makes max(0, pending + N - c) outputs, and output k is centred at logical index
 * hist_len - pending + k of [history | block]. Afterwards pending is pending + N - outputs and the history holds the
 * newest hist_len input samples, right-aligned. A zeroed history and pending = 0 start a stream: its first output is
 * centred on its first sample, with zeros before it. Taps may change between calls (pending reconciles a new tap
 * count), so a call can make up to N + c_old - c_new outputs. Units are complex samples.
 *
 * @param in       Input interleaved I/Q samples (may be NULL when n_in is 0).
 * @param n_in     Input complex samples (0 is valid).
 * @param out      Output interleaved I/Q samples (may be NULL when the call makes no output).
 * @param out_cap  Output capacity in complex samples.
 * @param hist_i   I history buffer (hist_len elements).
 * @param hist_q   Q history buffer (hist_len elements).
 * @param hist_len History length in complex samples, at least taps_len - 1.
 * @param pending  In/out: outputs held back for look-ahead, 0 <= pending and pending + c <= hist_len.
 * @param taps     Symmetric FIR taps (odd count).
 * @param taps_len Number of taps (odd, at least 3).
 * @return Outputs written (complex samples), or -1 with no state touched for an invalid call or an out_cap below
 *         the outputs the call makes; a call never consumes part of its input.
 */
int simd_fir_complex_apply(const float* in, int n_in, float* out, int out_cap, float* hist_i, float* hist_q,
                           int hist_len, int* pending, const float* taps, int taps_len);

/**
 * Complex half-band decimator by 2, streaming.
 * Exploits zero-valued odd taps in half-band filters AND tap symmetry.
 *
 * The output does not depend on how the input is cut into calls: `pending` carries both the look-ahead and the
 * decimation phase. With c = (taps_len - 1) / 2, H = taps_len - 1, N = in_len / 2 complex samples and lt = pending + N,
 * a call makes m = (lt - c + 1) / 2 outputs once lt reaches c + 1 (none before), output k centred at logical index
 * H - pending + 2k of [history | block], and leaves pending lt - 2m with the newest H inputs in the history. A zeroed
 * history and pending = 0 start a stream: its outputs are centred on its samples 0, 2, 4, ..., with zeros before it.
 * During a stream's warm-up pending takes any value in 0..c; once outputs flow it is c - 1 or c. A call makes at most
 * (N + 1) / 2 outputs and never looks past its block's last sample.
 *
 * @param in       Input interleaved I/Q samples (may be NULL when in_len is 0).
 * @param in_len   Input length in floats, even (2 per complex sample; 0 is valid).
 * @param out      Output interleaved I/Q samples (may be NULL when the call makes no output).
 * @param hist_i   I history buffer (taps_len - 1 elements).
 * @param hist_q   Q history buffer (taps_len - 1 elements).
 * @param pending  In/out: input samples taken but not yet decimated past, 0 <= pending <= c.
 * @param taps     Half-band taps (odd count, odd indices zero except center).
 * @param taps_len Number of taps (odd, at least 3; the pipeline runs 15 and 31).
 * @return Output length in floats, or -1 with no state touched for an invalid call (bad taps, a NULL buffer the call
 *         needs, a negative or odd in_len, or pending outside 0..c).
 */
int simd_hb_decim2_complex(const float* in, int in_len, float* out, float* hist_i, float* hist_q, int* pending,
                           const float* taps, int taps_len);

/**
 * Real half-band decimator by 2, streaming: the same contract as simd_hb_decim2_complex() on real samples.
 *
 * @param in       Real input samples (may be NULL when in_len is 0).
 * @param in_len   Number of input samples (0 is valid).
 * @param out      Decimated output (may be NULL when the call makes no output).
 * @param hist     History buffer (taps_len - 1 elements).
 * @param pending  In/out: input samples taken but not yet decimated past, 0 <= pending <= c.
 * @param taps     Half-band taps (odd count).
 * @param taps_len Number of taps (odd, at least 3).
 * @return Number of output samples, or -1 with no state touched for an invalid call.
 */
int simd_hb_decim2_real(const float* in, int in_len, float* out, float* hist, int* pending, const float* taps,
                        int taps_len);

/**
 * Query the active SIMD implementation name for debugging.
 * @return "scalar", "sse2", "avx2", or "neon".
 */
const char* simd_fir_get_impl_name(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SIMD_FIR_H */
