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
/* The same with each sample's auto squelch flag (DSD_SQUELCH_FLAG_CLOSED; 0 open), as the RTL stream reads them. */
unsigned int dsd_symbol_test_finalize_unsynced_analog_block_flags(dsd_opts* opts, dsd_state* state, const float* input,
                                                                  const uint8_t* flags, unsigned int count);
/* Hand the unsynced analog path one input sample, as getSymbol() does for each sample it reads
   while there is no sync: the sample joins the block being assembled, which is finalized when
   full. */
void dsd_symbol_test_push_unsynced_analog_sample(dsd_opts* opts, dsd_state* state, float sample);
/* The same with the sample's auto squelch flag, as the RTL stream reads it (DSD_SQUELCH_FLAG_CLOSED; 0 open). */
void dsd_symbol_test_push_unsynced_analog_sample_flag(dsd_opts* opts, dsd_state* state, float sample, uint8_t flag);
/* The same on RTL input, with @p rtl_generation the stream generation the sample came off, which getSymbol() refreshes
   every symbol: a test can move the stream's rate and generation part-way through a block (issue #633). */
void dsd_symbol_test_push_unsynced_analog_sample_rtl(dsd_opts* opts, dsd_state* state, float sample,
                                                     uint32_t rtl_generation);
/* Fail the analog sinks' state allocation while @p fail is set (issue #633): a block to convert is then muted. */
void dsd_analog_sink_test_fail_alloc(int fail);
/* Call @p sync in place of the unsynced raw WAV's sync to disk after each block: a slow disk's fsync,
   without the disk. NULL restores sf_write_sync(). */
void dsd_symbol_test_set_raw_wav_sync(void (*sync)(void));
#ifdef USE_RADIO
int dsd_symbol_test_rtl_cache_and_center_contract(int out_values[10]);
int dsd_symbol_test_auto_center_step_direction(int e_ema, int deadband, int* run_dir, int* run_len, int* dir_out);
#endif

#ifdef __cplusplus
}
#endif

#endif
