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
#define DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS         1800

/* How long a P25p1 frame that decoded its NID keeps the modulation heuristics off the
 * demodulator that carried it, in symbols at 4800 sym/s (two seconds). Long enough to span the
 * gaps between frames on a control channel, short enough that a modulation which has gone
 * quiet is argued about again. */
#define DSD_FRAME_SYNC_P25P1_VALIDATED_HOLD_SYMBOLS 9600U

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
#define DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS            16

/**
 * @brief Symbols one dPMR FS2 frame occupies, sync word included.
 *
 * processdPMRvoice() reads 372 dibits behind the 12-symbol FS2 sync: two 36-dibit CCH halves,
 * two groups of four 36-dibit AMBE frames, and the 12-dibit channel code. That span is the
 * frame FS2 opened, and it is how long the weaker NXDN48 matcher stays off the 2400/4 profile
 * (see dsd_frame_sync_suppress_nxdn48_sync()).
 */
#define DSD_FRAME_SYNC_DPMR_FS2_FRAME_SYMBOLS       384U

/**
 * @brief Symbols an accepted P25p1 sync owns on the 4800/4 profile, sync word included.
 *
 * P25p1's longest frame is an LDU at 864 symbols, and the status symbols interleaved through it
 * add about 25 more. Rounding up to 900 covers the longest frame the sync can have opened while
 * staying short enough that a channel which has gone quiet releases the profile inside a fifth of
 * a second. Shorter frames re-arm the span on their own next sync -- a control channel's TSDUs
 * arrive every ~360 symbols -- so continuous traffic stays covered without the span having to
 * name a length it cannot know at sync time (see
 * dsd_frame_sync_suppress_4800_4_for_p25p1_frame()).
 */
#define DSD_FRAME_SYNC_P25P1_FRAME_SYMBOLS          900U

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
 * @return 1 when the caller must take a no-sync exit: either the step changed the profile
 *         index or the modulation -- the caller's sync window is then stale and must be
 *         rebuilt -- or the budget expired on a trunked voice channel, where the profile is
 *         deliberately held but the no-sync accounting is still owed (#392, #393). 0 when
 *         the profile still owes symbols, or when spending the budget left the timing and
 *         modulation untouched.
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
