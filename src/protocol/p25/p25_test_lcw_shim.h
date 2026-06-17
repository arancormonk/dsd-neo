// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_TEST_LCW_SHIM_H_
#define DSD_NEO_SRC_PROTOCOL_P25_P25_TEST_LCW_SHIM_H_

#ifdef __cplusplus
extern "C" {
#endif

void p25_test_invoke_lcw(const unsigned char* lcw_bits, int len, int enable_retune, long cc_freq);
void p25_test_invoke_lcw_with_lastsrc(const unsigned char* lcw_bits, int len, int enable_retune, long cc_freq,
                                      long lastsrc);
void p25_test_invoke_lcw_with_tuner(const unsigned char* lcw_bits, int len, int enable_retune, long cc_freq,
                                    long tuner_freq);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_TEST_LCW_SHIM_H_ */
