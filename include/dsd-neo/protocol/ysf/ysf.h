// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Yaesu System Fusion (YSF) protocol decode entrypoints.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_YSF_YSF_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_YSF_YSF_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef DSD_NEO_TEST_HOOKS
#include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void processYSF(dsd_opts* opts, dsd_state* state);

#ifdef DSD_NEO_TEST_HOOKS
void dsd_neo_ysf_test_decode_dch(dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
                                 const uint8_t input[160]);
void dsd_neo_ysf_test_decode_dch2(dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
                                  const uint8_t input[80]);
uint16_t dsd_neo_ysf_test_crc16(const uint8_t* bits, int len);
int dsd_neo_ysf_test_conv_fich(const uint8_t input[100], uint8_t dest[32], uint32_t* v_error_out);
int dsd_neo_ysf_test_conv_dch(const dsd_opts* opts, dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft,
                              uint8_t cm, uint8_t input[180]);
int dsd_neo_ysf_test_conv_dch2(const dsd_opts* opts, dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft,
                               uint8_t cm, uint8_t input[100]);
void dsd_neo_ysf_test_build_type2_ambe(const uint8_t vech_bits[104], uint8_t temp[512], char ambe_d[49]);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_YSF_YSF_H_ */
