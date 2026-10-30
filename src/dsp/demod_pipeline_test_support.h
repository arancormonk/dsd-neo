// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_DEMOD_PIPELINE_TEST_SUPPORT_H_
#define DSD_NEO_SRC_DSP_DEMOD_PIPELINE_TEST_SUPPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

struct demod_state;

/**
 * @brief Test hook: while @p fail is nonzero, every allocation of the post-demod polyphase decimator fails, as when the
 * memory is not there, so full_demod() decimates through the fallback. 0 lets the next block allocate it again.
 *
 * Defined only in the test-hook build of the DSP module (dsd-neo_dsp_private_test_support, compiled with
 * DSD_NEO_TEST_HOOKS); the shipped library has no such symbol.
 */
void dsd_demod_test_fail_post_polydecim_alloc(int fail);

/**
 * @brief Test hook: the linear front end of the TETRA chain alone, on d->lowpassed as full_demod() runs it after the
 * half-band decimation: the mixer at the FLL state's pre_freq, the channel LPF, then the matched filter. Nothing
 * adaptive runs (no AGC, FLL, transfer or timing), so the output depends on the block partition only through the
 * mixer phasor's per-block renormalization. Expects demod_state::tetra_chain set; leaves d->lowpassed / d->lp_len at
 * the matched filter's output.
 *
 * Defined only in the test-hook build of the DSP module, like the hook above.
 */
void dsd_demod_test_tetra_linear_front_end(struct demod_state* d);

/**
 * @brief Test hook: one run of the CQPSK RMS AGC (OP25 rms_agc) on d->lowpassed, as full_demod() runs it, with the
 * averaging weight demod_state::tetra_chain selects (dsd_cqpsk_agc_alpha()).
 *
 * Defined only in the test-hook build of the DSP module, like the hooks above.
 */
void dsd_demod_test_cqpsk_rms_agc(struct demod_state* d);

#ifdef __cplusplus
}
#endif

#endif
