// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_TESTS_TEST_SUPPORT_P25_TEST_LCW_SHIM_H_
#define DSD_NEO_TESTS_TEST_SUPPORT_P25_TEST_LCW_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

void p25_test_invoke_lcw(const unsigned char* lcw_bits, int len, int enable_retune, long cc_freq, long lastsrc,
                         long tuner_freq);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TESTS_TEST_SUPPORT_P25_TEST_LCW_SHIM_H_ */
