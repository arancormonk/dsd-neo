// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief Private EDACS frame, trunking, and analog-audio helpers.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_EDACS_EDACS_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_EDACS_EDACS_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

void edacs_process_valid_frame(dsd_opts* opts, dsd_state* state, unsigned long long msg_1, unsigned long long msg_2);
const char* edacs_lcn_status_string(int lcn);
short edacs_apply_input_volume(const dsd_opts* opts, short sample);
unsigned long long edacs_vote_frames(unsigned long long fr_1_4, unsigned long long fr_2_5, unsigned long long fr_3_6);
int edacs_update_squelch_count(double pwr, double sql, int count);
int edacs_should_release_voice(unsigned long long sr, int sql_disabled, time_t start_time, double no_sql_watchdog_s);
void edacs_update_lcn_count(dsd_state* state, int lcn);
void edacs_build_raw_frames(const int* edacs_bit, unsigned long long* fr_1, unsigned long long* fr_2,
                            unsigned long long* fr_3, unsigned long long* fr_4, unsigned long long* fr_5,
                            unsigned long long* fr_6);
unsigned long long edacs_build_symbol_register(const dsd_opts* opts, dsd_state* state, const short* analog1);
void edacs_reset_digitize_overflow(dsd_state* state);
int edacs_collect_analog_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3,
                                 double* pwr);
void edacs_emit_analog_audio(dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                             const short* analog3);
int edacs_build_static_wav_block(const short* src, short* out, size_t out_count);
double edacs_no_sql_watchdog_window(double trunk_hangtime);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_EDACS_EDACS_INTERNAL_H_ */
