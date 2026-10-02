// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_DEMOD_PIPELINE_TEST_SUPPORT_H_
#define DSD_NEO_SRC_DSP_DEMOD_PIPELINE_TEST_SUPPORT_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Test hook: while @p fail is nonzero, every allocation of the post-demod polyphase decimator fails, as when the
 * memory is not there, so full_demod() decimates through the fallback. 0 lets the next block allocate it again.
 *
 * Defined only in the test-hook build of the DSP module (dsd-neo_dsp_private_test_support, compiled with
 * DSD_NEO_TEST_HOOKS); the shipped library has no such symbol.
 */
void dsd_demod_test_fail_post_polydecim_alloc(int fail);

#ifdef __cplusplus
}
#endif

#endif
