// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_SYMBOL_TEST_SUPPORT_H_
#define DSD_NEO_SRC_DSP_SYMBOL_TEST_SUPPORT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void dsd_symbol_test_select_window(int rf_mod, int synctype, int lastsynctype, int freeze_window, int* l_edge,
                                   int* r_edge);
int dsd_symbol_test_adjust_timing_index(int samples_per_symbol, int symbol_center, int rf_mod, int jitter,
                                        int have_sync, int symbol_span, int start_i, int* jitter_after);
float dsd_symbol_test_apply_matched_filter(const dsd_opts* opts, const dsd_state* state, float sample,
                                           int rtl_symbol_rate_output, int cqpsk_symbol_rate);
unsigned int dsd_symbol_test_convert_analog_block_to_i16(const float* input, short* output, unsigned int count);
/* Fill the unsynced analog block with @p input and run the whole finalize step on it: input
   power, raw WAV, the received-tone tap, the voice filters and the monitor output. */
unsigned int dsd_symbol_test_finalize_unsynced_analog_block(dsd_opts* opts, dsd_state* state, const float* input,
                                                            unsigned int count);
/* Replace the monotonic clock the received-tone tap reads to tell a paused live stream input
   (src/dsp/analog_rx.c); NULL restores dsd_time_monotonic_ms(). */
void dsd_analog_rx_test_set_clock(uint64_t (*now_ms)(void));
#ifdef USE_RADIO
int dsd_symbol_test_rtl_cache_and_center_contract(int out_values[10]);
int dsd_symbol_test_auto_center_step_direction(int e_ema, int deadband, int* run_dir, int* run_len, int* dir_out);
#endif

#ifdef __cplusplus
}
#endif

#endif
