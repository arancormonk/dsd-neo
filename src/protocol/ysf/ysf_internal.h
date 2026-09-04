// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief Private YSF decode helpers shared by the protocol implementation and focused tests.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_YSF_YSF_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_YSF_YSF_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ysf_dch_decode(dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
                    const uint8_t input[160]);
void ysf_dch_decode2(dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
                     const uint8_t input[80]);
uint16_t ysf_crc16(const uint8_t* bits, int len);
int ysf_conv_fich(const uint8_t input[100], uint8_t dest[32], uint32_t* v_error_out);
int ysf_conv_dch(const dsd_opts* opts, dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
                 uint8_t input[180]);
int ysf_conv_dch2(const dsd_opts* opts, dsd_state* state, uint8_t bn, uint8_t bt, uint8_t fn, uint8_t ft, uint8_t cm,
                  uint8_t input[100]);
void ysf_build_type2_ambe(const uint8_t vech_bits[104], uint8_t temp[512], char ambe_d[49]);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_YSF_YSF_INTERNAL_H_ */
