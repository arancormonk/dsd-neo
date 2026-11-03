// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_LIVE_IQ_STEP_TEST_SUPPORT_H_
#define DSD_NEO_SRC_DSP_LIVE_IQ_STEP_TEST_SUPPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Test hook: while @p fail is nonzero, every design dsd_live_iq_step_prepare() makes fails as when its memory
 * is not there (DSD_LIVE_IQ_STEP_ERR_DESIGN, design_status DSD_IQ_RESAMPLER_ERR_ALLOC). A plan that needs no design
 * (one that does not step, or the active one) still prepares. 0 lets the next prepare design again.
 *
 * Defined only in the test-hook build of the DSP module (dsd-neo_dsp_private_test_support, compiled with
 * DSD_NEO_TEST_HOOKS); the shipped library has no such symbol.
 */
void dsd_live_iq_step_test_fail_design(int fail);

#ifdef __cplusplus
}
#endif

#endif
