// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_FRAME_SYNC_INTERNAL_H_
#define DSD_NEO_SRC_DSP_FRAME_SYNC_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Symbols one no-sync pass is worth.
 *
 * The no-sync timeout fires after this many consecutive matchless symbols, and the SPS
 * hunt's dwell is a whole number of these (see dsd_frame_sync_sps_hunt_dwell_passes()).
 */
#define DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS 1800

/**
 * @brief Symbols a frame handler must consume on one sync for that sync to count as a frame.
 *
 * The SPS hunt credits a profile for symbols its handlers take (see
 * frame_sync_no_sync_sps_hunt()), and this is what separates decoding from recognising a
 * marker and bailing. It sits between the two costs it has to tell apart: above the 8
 * dibits dispatch_m17.c skips on a bare preamble -- the only path in src/engine/dispatch
 * that returns without reading a frame -- and below the 28 dibits of P25p1's TDU
 * (processTDU(), the shortest frame any protocol here decodes).
 */
#define DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS    16

/** @brief One entry of the SPS hunt's rate/level table, indexed by dsd_state::sps_hunt_idx. */
typedef struct {
    int symbol_rate_hz;
    int levels;
} frame_sync_sps_profile;

/** @brief The hunt profile at @p index; an out-of-range index yields the 4800/4 default. */
const frame_sync_sps_profile* frame_sync_sps_profile_for_index(int index);

void frame_sync_maybe_tick_p25_trunk_sm(dsd_opts* opts, dsd_state* state, time_t now);
void frame_sync_maybe_auto_switch_modulation(const dsd_opts* opts, dsd_state* state, int t_max, int* lastt);
int frame_sync_active_profile_modulation(const dsd_opts* opts, const dsd_state* state);
int frame_sync_should_skip_snr_or_power_gate(const dsd_opts* opts, const dsd_state* state);
int frame_sync_hamming_distance_pattern(const char* symbols, const char* pattern, int len);
int frame_sync_best_ham_for_patterns(const char* symbols, const char* const patterns[], int pattern_count,
                                     int pattern_len, int best_start);
int frame_sync_best_nxdn_scaled_ham(const char* symbols10, int best_start);
int frame_sync_sps_hunt_next_index(const dsd_opts* opts, const dsd_state* state);
void frame_sync_apply_sps_hunt_profile(const dsd_opts* opts, dsd_state* state, int next_idx, int preserve_modulation);
void frame_sync_ensure_enabled_sps_profile(const dsd_opts* opts, dsd_state* state);
/**
 * @brief Step the SPS hunt if the active profile has spent its symbol budget.
 *
 * @return 1 when the step changed the profile index or the modulation -- the caller's sync
 *         window is then stale and must be rebuilt -- and 0 when the profile still owes
 *         symbols, or when spending the budget left the timing and modulation untouched.
 */
int frame_sync_no_sync_sps_hunt(const dsd_opts* opts, dsd_state* state);
double frame_sync_elapsed_seconds(double nowm, time_t now, double mono_stamp, time_t wall_stamp);
void frame_sync_p25_slot_activity(const dsd_opts* opts, const dsd_state* state, time_t now, double nowm,
                                  double mac_hold, double ring_hold, double dt, int* left_active, int* right_active);
#ifdef USE_RADIO
double frame_sync_active_profile_snr_db(const dsd_opts* opts, const dsd_state* state);
#endif

#ifdef __cplusplus
}
#endif

#endif
