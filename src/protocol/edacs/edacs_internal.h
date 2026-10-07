// SPDX-License-Identifier: ISC
/**
 * @file
 * @brief Private EDACS frame, trunking, and analog-audio helpers.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_EDACS_EDACS_INTERNAL_H_
#define DSD_NEO_SRC_PROTOCOL_EDACS_EDACS_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>
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
int edacs_frame_bch_verdict(const int* edacs_bit, unsigned long long* msg_1_ec_out, unsigned long long* msg_2_ec_out);
unsigned long long edacs_build_symbol_register(const dsd_opts* opts, dsd_state* state, const short* analog1);
void edacs_reset_digitize_overflow(dsd_state* state);
int edacs_collect_analog_triplet(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3,
                                 double* pwr);

/* EDACS analog voice reads three 960-sample blocks at a time. */
enum { EDACS_ANALOG_BLOCK_SAMPLES = 960, EDACS_ANALOG_TRIPLET_SAMPLES = 3 * EDACS_ANALOG_BLOCK_SAMPLES };

/* How an analog call's squelch is decided (issue #625): not at all (the fallback watchdog and the release marker end
   it), on the level squelch's power test, or on the dynamic squelch's per-sample gate. */
enum { EDACS_ANALOG_SQL_NONE = 0, EDACS_ANALOG_SQL_LEVEL = 1, EDACS_ANALOG_SQL_GATE = 2 };

/* edacs_collect_analog_triplet() with each sample's squelch flag (DSD_SQUELCH_FLAG_CLOSED) into @p flags, 2880 of them
   or NULL: an RTL stream's own, every flag 0 (open) on audio input. */
int edacs_collect_analog_triplet_flags(dsd_opts* opts, dsd_state* state, short* analog1, short* analog2, short* analog3,
                                       uint8_t* flags, double* pwr);

/* The squelch an analog call runs (EDACS_ANALOG_SQL_*): the gate under a dynamic setting on a radio input whose stream
   runs it (@p gate_running: its status active with a valid plan), the level under a LEVEL setting above 0, else none.
   A dynamic setting on audio input runs none. */
int edacs_analog_sql_kind(const dsd_opts* opts, int gate_running);

/* The run of consecutive closed samples after @p count more flags, carried from @p run: any open sample restarts it. */
size_t edacs_gate_closed_run(size_t run, const uint8_t* flags, size_t count);

/* The shortest closed run that releases a call: two of the tracker's 40 ms classification windows, so neither the
   window a call's first decision takes nor one window read closed inside a call ends it. */
enum { EDACS_ANALOG_GATE_MIN_HOLD_MS = 80 };

/* The closed run that releases a call at @p rate_hz: four triplets, less the dynamic squelch's closing delay
   (DSD_SQUELCH_CLOSE_DELAY_MS), never under EDACS_ANALOG_GATE_MIN_HOLD_MS. The level squelch releases on its fifth
   closed reading, taken at each triplet's end, so it releases a carrier that drops inside a triplet four whole triplets
   after that one ends: this run reaches that end for any drop and any closing delay up to the bound. Above about
   70 kHz (an unresampled high-rate replay; EDACS's RTL output runs 24 or 48 kHz) four triplets are too short for the
   delay and the floor sets the run, so a release there can come up to the delay, the floor and the triplet the run is
   read at (EDACS decides once per triplet) after the drop. */
size_t edacs_gate_hold_samples(int rate_hz);

/* The level path's count (5 down to 1, 0 when released) a closed run of @p run samples matches against @p hold. */
int edacs_gate_count(size_t run, size_t hold);
void edacs_emit_analog_audio(dsd_opts* opts, dsd_state* state, const short* analog1, const short* analog2,
                             const short* analog3);
int edacs_build_static_wav_block(const short* src, short* out, size_t out_count);
void edacs_write_analog_wav(dsd_opts* opts, const dsd_state* state, const short* analog1, const short* analog2,
                            const short* analog3);
double edacs_no_sql_watchdog_window(double trunk_hangtime);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_EDACS_EDACS_INTERNAL_H_ */
