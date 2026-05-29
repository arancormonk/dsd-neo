// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief dsd-neo adapters for the mbelib-neo result-based API.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_MBE_API_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_MBE_API_H_

#include <mbelib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void dsd_mbe_init_result_from_errors(mbe_process_result* result, int errs, int errs2, unsigned flags);
void dsd_mbe_store_result(int* errs, int* errs2, char* err_str, size_t err_str_size, const mbe_process_result* result);

int dsd_mbe_decode_imbe7200_frame(int* errs, int* errs2, const char imbe_fr[8][23], char imbe_d[88],
                                  mbe_process_result* result);
int dsd_mbe_decode_imbe7100_frame(int* errs, int* errs2, const char imbe_fr[7][24], char imbe_d[88],
                                  mbe_process_result* result);
int dsd_mbe_decode_ambe2450_frame(int* errs, int* errs2, const char ambe_fr[4][24], char ambe_d[49],
                                  mbe_process_result* result);

int dsd_mbe_process_imbe4400_dataf(float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                                   const char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp,
                                   mbe_parms* prev_mp_enhanced, mbe_process_result* result);
int dsd_mbe_process_ambe2450_dataf(float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                                   const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                                   mbe_parms* prev_mp_enhanced, mbe_process_result* result);
int dsd_mbe_process_ambe2400_dataf(float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                                   const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                                   mbe_parms* prev_mp_enhanced, mbe_process_result* result);
int dsd_mbe_process_ambe3600x2400_framef(float* aout_buf, int* errs, int* errs2, char* err_str, size_t err_str_size,
                                         char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                                         mbe_parms* prev_mp_enhanced);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_MBE_API_H_ */
