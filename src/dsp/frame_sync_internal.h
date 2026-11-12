// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DSD_NEO_SRC_DSP_FRAME_SYNC_INTERNAL_H_
#define DSD_NEO_SRC_DSP_FRAME_SYNC_INTERNAL_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#include <time.h>

struct dsd_p25_sync_context;

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

/** Unlocked RTL trunk-scan P25 acquisition needs both demodulators within one visit. */
int frame_sync_trunk_scan_p25p1_trial(const dsd_opts* opts, const dsd_state* state);

/** The unlocked SPS hunt's next profile from @p current while a P25-driven control-channel probe is in flight
 * (dsd_frame_sync_hook_p25_probe_in_flight()): only the P25 profiles the options enable, 4800/4 -> 6000/4 -> 4800/4,
 * the sequence a P25-only rotation runs, so a probe steps from 4800/4 to 6000/4 as there. From any other
 * profile it returns to the first of them. -1 when the options enable neither P25 phase: the ordinary rotation runs. */
int frame_sync_sps_hunt_p25_probe_next_index(const dsd_opts* opts, int current);

/** The unlocked SPS hunt's next profile from @p current while a Signal search probe identifies an Auto detection
 * (dsd_state::sps_hunt_search_order, issue #804): 4800/4 -> 6000/4 -> 2400/4 -> 9600/2 -> 4800/2 (-> 18000/4, which no
 * search enables), skipping profiles the options give no candidate, and back to the first. P25 Phase 2 then comes right
 * after the 4800/4 protocols instead of fourth. @p current itself when it is the only candidate; -1 when no profile
 * has one: the ordinary rotation runs. */
int frame_sync_sps_hunt_search_next_index(const dsd_opts* opts, int current);

/** Dwell passes on 6000/4 while a Signal search probe identifies an Auto detection (issue #804), 300 ms: a P25 Phase 2
 * carrier keeps the profile for as long as its bursts keep coming (each frame read is refunded to the dwell), so the
 * pass only has to see the first one.
 * Chosen by replaying the I/Q fixtures and real Phase 2 voice through the search order
 * (tests/engine/search_schedule_replay.c): two passes identified nothing sooner and put 2400/4 300 ms later, and a
 * shorter 4800/4 dwell lost P25 Phase 1 voice whose first frame comes late. */
#define DSD_FRAME_SYNC_SEARCH_6000_4_PASSES 1

/** Run the P25p1 chain comparison (dsd_p25p1_mod_compare) on what the last frame validated: start one on an
 * acquisition's first NID-decoded frame on 4800/4, add up what validated frames read in each chain's pass, try the
 * other chain only when the incumbent let a frame go by unread, and keep the chain that read more.
 * getFrameSync() calls it on entry, before the hunt re-anchors dsd_state::sps_hunt_symbolcnt_mark. */
void frame_sync_p25p1_mod_compare_note(const dsd_opts* opts, dsd_state* state);

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

/**
 * @brief Symbols a proof of the 2400/4 profile keeps the weaker 4800/4 matchers at bay.
 *
 * One full rotation of the hunt at the default dwell: DSD_FRAME_SYNC_SPS_PROFILE_COUNT profiles
 * of dsd_frame_sync_sps_hunt_dwell_passes() x DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS. That is what
 * the span has to cover to do its job at all, and it is tight rather than padded: from the proof,
 * the hunt spends at most one dwell finishing on 2400/4 -- the proof restarted that counter -- one
 * each traversing 9600/2, 6000/4 and 4800/2, and the guard must still be standing through the
 * whole of the first 4800/4 dwell, where the claimants are. Builds without some of those
 * candidates simply arrive sooner.
 *
 * The other bound is what it costs a signal that is really there: a transmission that has genuinely
 * ended frees 4800/4 for real NXDN96 and M17 within one rotation of its last proof, and a rotation
 * is mostly spent on profiles neither of them uses. Where the dwell is longer -- an untuned P25
 * trunk takes five passes -- the rotation outruns this span, so the guard thins out rather than
 * over-reaching, and the profile hold that PROFILE_PROVEN itself buys stays the primary defence
 * (see dsd_frame_sync_suppress_4800_4_for_2400_4_transmission()).
 */
#define DSD_FRAME_SYNC_PROVEN_2400_4_HOLD_SYMBOLS   27000U

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
 * @return FRAME_SYNC_HUNT_NO_SYNC_EXIT (1) when the caller must take a no-sync exit: either the
 *         step changed the profile index or the modulation -- the caller's sync window is then
 *         stale and must be rebuilt -- or the budget expired on a trunked voice channel, where the
 *         profile is deliberately held but the no-sync accounting is still owed (#392, #393).
 *         FRAME_SYNC_HUNT_CHAIN_HANDBACK (2) when a silent CQPSK trial handed the rest of its dwell
 *         back to C4FM: the window is stale, but the dwell goes on, so getFrameSync() rebuilds it in
 *         place and keeps hunting, owing no no-sync accounting and returning nothing the engine
 *         would answer with noCarrier(). 0 when the profile still owes symbols, or when spending the
 *         budget left the timing and modulation untouched.
 */
#define FRAME_SYNC_HUNT_NO_SYNC_EXIT   1
#define FRAME_SYNC_HUNT_CHAIN_HANDBACK 2
int frame_sync_no_sync_sps_hunt(const dsd_opts* opts, dsd_state* state);
/** @brief Arm (non-zero) or disarm the 2 s CQPSK dwell the 4800/4 profile holds a CQPSK chain for. */
void frame_sync_qpsk_dwell_set(int armed);
/** Put the 4800/4 profile, the decoder's modulation and the front end on P25p1 chain @p chain (0 C4FM, 1 CQPSK),
 * arming the CQPSK dwell with it. A chain step: the caller holds the P25 SM tick guard. */
void frame_sync_p25p1_set_chain(const dsd_opts* opts, dsd_state* state, int chain);
/* The P25p1 chain comparison's shared steps (frame_sync_p25p1_compare.c). */
/** @brief End a comparison (dsd_p25p1_mod_compare): no phase, and the trial flag that kept the votes off is down. */
void frame_sync_p25p1_mod_compare_end(dsd_state* state);
/** @brief The chain the comparison would keep if it ended now: the incumbent, unless the other chain has read more. */
int frame_sync_p25p1_mod_compare_leader(const dsd_state* state);
/* What the P25 Phase 1 and Phase 2 sync anchors share (frame_sync_p25_sync_context.c). */
/** Bit errors between the first @p dibits of @p window, read under CQPSK dibit map @p map_idx, and @p pattern: each
 * dibit counts the bits it differs by (P25's dibits are Gray coded over the four levels), a byte outside '0'..'3' 2.
 * 2 * @p dibits for a NULL or short window. */
int frame_sync_p25_sync_bit_distance(const char* window, const char* pattern, int dibits, uint8_t map_idx);
/** The stream and receiver setup as they stand, for a sync word whose first dibit is @p sync_clock (polarity 0). */
void frame_sync_p25_sync_context_now(const dsd_state* state, uint64_t sync_clock, struct dsd_p25_sync_context* out);
/** Whether @p ctx still describes the stream: nothing that ends the history carry has happened since
 * (dsd_frame_sync_history_carry), nor a profile or modulation change. A pending off-thread tune is the caller's to
 * check (dsd_frame_sync_off_thread_tune_pending()). */
int frame_sync_p25_sync_context_holds(const dsd_state* state, const struct dsd_p25_sync_context* ctx);
/** The slicer warm start of a P25p1 sync the C4FM chain read: the levels from the word's outer symbols, and whether
 * they were set (dsd_state::p25_p1_c4fm_sync_levels), which decides whether use_symbol() holds them through the
 * frame. */
void frame_sync_p25p1_c4fm_warm_start(const dsd_opts* opts, dsd_state* state);
/* The P25p1 sync anchor's frame-sync side (frame_sync_p25p1_anchor.c, dsd_p25p1_sync_anchor). */
/** Capture the sync-time context of the P25p1 sync just accepted (@p synctype), the window ending on the latest
 * symbol: @p kind how it was accepted (dsd_p25p1_sync_kind), @p anchor_nac the anchor's NAC for an anchored accept (-1
 * otherwise), @p errors what the word was off by (bits on the CQPSK chain, signs on the C4FM chain). No context is
 * captured while an off-thread tune is pending. */
void frame_sync_p25p1_anchor_note_sync(dsd_state* state, int synctype, int kind, int anchor_nac, int errors);
/** Hold what the accept of a tolerant P25p1 sync word on the C4FM chain is about to change (dsd_p25p1_sync_candidate),
 * for dsd_frame_sync_p25p1_candidate_rejected() to put back if its NID fails, with the hunt's pass position
 * @p pass_symbols at the word's last symbol. */
void frame_sync_p25p1_candidate_hold(dsd_state* state, int pass_symbols);
/** Start the hunt after a dropped tolerant P25p1 candidate from the last @p symbols symbols the word and its NID took
 * (the C4FM hunt's sign slicing of dsd_symbol_history_get_back()), as a history a no-sync exit carries
 * (dsd_frame_sync_history_carry), at pass position @p pass_symbols: the next getFrameSync() call resumes it when
 * nothing came between. */
void frame_sync_history_carry_symbols(dsd_state* state, int symbols, int pass_symbols);
/** A sync of any protocol was accepted (@p synctype): the anchor is spent, and another protocol's leaves no P25p1
 * frame context. */
void frame_sync_p25p1_anchor_note_accept(dsd_state* state, int synctype);
/** The anchor's map, polarity and NAC when the window ending on the latest symbol starts at its expected position on the
 * 4800/4 profile and its context still holds. The first window at or past that position spends it, whatever it holds.
 * 0 otherwise. */
int frame_sync_p25p1_anchor_take(dsd_state* state, uint8_t* out_map_idx, int* out_inverted, int* out_nac);
/* The P25p2 sync anchor's frame-sync side (frame_sync_p25p2_anchor.c, dsd_p25p2_sync_anchor). */
/** Capture the sync-time context of the P25p2 sync just accepted (@p synctype), the window ending on the latest
 * symbol. @p tolerant_start is the superframe timeslot the anchor predicts for a tolerant accept and -1 for an exact
 * one, @p bit_errors the bits the word was off by. No context is captured while an off-thread tune is pending
 * (@p tune_pending). */
void frame_sync_p25p2_anchor_note_sync(dsd_state* state, int synctype, int tolerant_start, int bit_errors,
                                       int tune_pending);
/** A sync of any protocol was accepted (@p synctype): the anchor is spent, and another protocol's leaves no P25p2
 * window context. */
void frame_sync_p25p2_anchor_note_accept(dsd_state* state, int synctype);
/** The anchor's map, polarity and predicted window start when the window ending on the latest symbol starts at its
 * expected position on the 6000/4 profile and its context still holds (a pending off-thread tune is the caller's to
 * check). The first window at or past that position spends it, whatever it holds. 0 otherwise. */
int frame_sync_p25p2_anchor_take(dsd_state* state, uint8_t* out_map_idx, int* out_inverted, int* out_start);
double frame_sync_elapsed_seconds(double nowm, time_t now, double mono_stamp, time_t wall_stamp);
void frame_sync_p25_slot_activity(const dsd_opts* opts, const dsd_state* state, time_t now, double nowm,
                                  double mac_hold, double dt, int* left_active, int* right_active);
#ifdef USE_RADIO
double frame_sync_active_profile_snr_db(const dsd_opts* opts, const dsd_state* state);
#endif

#ifdef __cplusplus
}
#endif

#endif
