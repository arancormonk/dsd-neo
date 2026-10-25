// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frame sync helper APIs.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <time.h> // IWYU pragma: keep

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Stable indices stored in @c dsd_state::sps_hunt_idx. */
typedef enum {
    DSD_FRAME_SYNC_SPS_PROFILE_4800_4 = 0,
    DSD_FRAME_SYNC_SPS_PROFILE_2400_4 = 1,
    DSD_FRAME_SYNC_SPS_PROFILE_9600_2 = 2,
    DSD_FRAME_SYNC_SPS_PROFILE_6000_4 = 3,
    DSD_FRAME_SYNC_SPS_PROFILE_4800_2 = 4,
    DSD_FRAME_SYNC_SPS_PROFILE_18000_4 = 5, /**< TETRA pi/4-DQPSK; a candidate only when TETRA is the decode mode */
    DSD_FRAME_SYNC_SPS_PROFILE_COUNT = 6,
} dsd_frame_sync_sps_profile_index;

/**
 * @brief M17 evaluations a latched preamble candidate survives without a sync word behind it.
 *
 * A real preamble is a 192-symbol alternating run, and the matcher refreshes the candidate on
 * every window of it, so what this has to cover is only the gap between the last window that
 * still reads as a preamble and the first that holds the whole 8-symbol sync word: at most
 * eight evaluations, since the run's tail can survive about two symbols into the sync word.
 * Twice that leaves room for a noisy junction without leaving a candidate standing long enough
 * for an unrelated window to open a frame on it (issue #399).
 */
#define DSD_FRAME_SYNC_M17_CANDIDATE_TTL       8

/**
 * @brief Consecutive matching windows that make an alternating run an M17 preamble candidate.
 *
 * A single 8-symbol window within one error of the marker is not rare enough to act on -- noise
 * produces them, and each one used to open the whole M17 chain -- and neither is a run of eight,
 * which sign-sliced receiver noise produced a few times a second. A real preamble is 192 symbols,
 * so it offers some 180 consecutive matches and can spare 32 many times over; a transmission
 * landed in after its preamble joins on its sync word three frames running instead
 * (DSD_FRAME_SYNC_M17_FRAME_SYMBOLS).
 */
#define DSD_FRAME_SYNC_M17_PRE_RUN_SYMBOLS     32

/** @brief Symbols in every NXDN frame: 384 bits, 192 symbols at either rate (NXDN TS 1-A 4.4; 40 ms at 9600 bps). */
#define DSD_FRAME_SYNC_NXDN_FRAME_SYMBOLS      192U

/** @brief How far an NXDN96 FSW may sit from a frame's spacing and still count, in symbols (TS 1-A 5.3.1.1.2 allows
 * ±2 while the timing is held). */
#define DSD_FRAME_SYNC_NXDN96_TIMING_TOLERANCE 1U

/** @brief Frames after the last accepted NXDN96 FSW its lock still times the next one by: the receiver loses frame
 * sync after N7 = 5 FSWs missed in a row (TS 1-A 5.3.2, Table 5.9-3), so the sixth frame's FSW still counts. */
#define DSD_FRAME_SYNC_NXDN96_FLYWHEEL_FRAMES  6U

/** @brief Marks an NXDN96 FSW hit (dsd_nxdn96_sync::hits) whose four-level reading was within the specification's two
 * bit errors, as acquisition needs it; the plain sync type is a sign-only hit. */
#define DSD_FRAME_SYNC_NXDN96_CLEAR_HIT        0x80U

/** @brief Symbols in every M17 frame: a 16-bit sync burst and 368 bits of payload (M17 spec, Frame; 40 ms). */
#define DSD_FRAME_SYNC_M17_FRAME_SYMBOLS       192U

/**
 * @brief Non-zero when a symbol profile's level count admits only GFSK.
 *
 * Two-level slicing is frequency-shift keying; there is no two-level C4FM or
 * QPSK to choose. Stated once because three callers act on it and they have to
 * act the same way: the SPS hunt normalises @c dsd_state::rf_mod to GFSK on such
 * a profile, the SNR gate reads the GFSK estimator for it, and the UI's
 * modulation control has to stop offering the other two -- a control that lets a
 * two-level session be set to C4FM leaves @c dsd_opts::mod_c4fm saying one thing
 * while the demodulator the hunt rebuilt does another, and nothing ever
 * reconciles the pair.
 */
static inline int
dsd_frame_sync_profile_levels_force_gfsk(int levels) {
    return levels == 2;
}

/**
 * @brief The modulation @p requested_rf_mod becomes on a profile with @p levels levels.
 *
 * @param levels Slicer level count, 2 or 4.
 * @param requested_rf_mod Modulation asked for, as @c dsd_state::rf_mod
 *                         (0 C4FM, 1 QPSK, 2 GFSK). Pass 0 for "no opinion" to
 *                         get the profile's own default.
 * @return @p requested_rf_mod on a four-level profile, GFSK on a two-level one.
 */
static inline int
dsd_frame_sync_profile_modulation(int levels, int requested_rf_mod) {
    return dsd_frame_sync_profile_levels_force_gfsk(levels) ? 2 : requested_rf_mod;
}

/** @brief NXDN air-interface variant selected by frame-sync profile matching. */
typedef enum {
    DSD_NXDN_VARIANT_NONE = 0,
    DSD_NXDN_VARIANT_48 = 48,
    DSD_NXDN_VARIANT_96 = 96,
} dsd_nxdn_variant;

/**
 * @brief Reset modulation auto-detect state used by frame sync.
 */
void dsd_frame_sync_reset_mod_state(void);
/** Reset per-acquisition evidence and slicer windows after a committed channel/mode change.
 * Set forget_p25_modulation for conventional rows; trunk targets own their learned modulation. */
void dsd_frame_sync_reset_acquisition(const dsd_opts* opts, dsd_state* state, int forget_p25_modulation);

/**
 * @brief Return the NXDN variant selected by the enabled mode and active SPS hunt profile.
 */
dsd_nxdn_variant dsd_frame_sync_active_nxdn_variant(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return non-zero when alternate-protocol sync should be suppressed during active P25 trunking.
 */
int dsd_frame_sync_suppress_p25_alt_sync(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return non-zero while an accepted dPMR FS2 frame owns the 2400/4 profile.
 *
 * dPMR and NXDN48 share that profile with matchers of very different strength, so the frame a
 * 12-symbol FS2 opened is not offered to the looser one (#374).
 */
int dsd_frame_sync_suppress_nxdn48_sync(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return non-zero while a P25p1 frame the C4FM chain accepted owns the 4800/4 profile.
 *
 * P25p1 shares that profile with NXDN96 and M17, whose matchers are far weaker than its own
 * exact 24-symbol sync, so the frame that sync opened is not offered to either of them (#388).
 */
int dsd_frame_sync_suppress_4800_4_for_p25p1_frame(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Record that a protocol's own content check passed on the profile now active.
 *
 * Called from the frame handlers, which are the only place that knows a check ran and passed.
 * Kept separate from the SPS hunt's dwell accounting on purpose: what the hunt does with a proof
 * and what the matchers on another profile may infer from one are different questions, and #445
 * measured that answering the first one for NXDN costs more than it wins.
 */
void dsd_frame_sync_note_profile_proof(dsd_state* state);

/**
 * @brief Return non-zero while a transmission that recently proved the 2400/4 profile may still
 *        be on air.
 *
 * The span above covers one frame; this covers the gap a hunt step opens. Under AUTO the hunt can
 * rotate off 2400/4 mid-transmission, and when it reaches 4800/4 the NXDN96 and M17 matchers there
 * are offered a 2400-baud signal read at twice its rate, which they accept (#445). While a proof of
 * 2400/4 is recent enough that its transmission may still be running, they are not.
 *
 * Armed only by a handler's own passing check, so a real NXDN96 or M17 signal cannot arm it against
 * itself: neither proves the 2400/4 profile. On symbol-file input, where the profile gate stands
 * open because stored symbols cannot be revisited after a hunt advance, no shipped configuration
 * reaches this -- the NXDN matcher is limited to an unambiguous variant there, and a build with
 * neither 2400/4 protocol enabled fails the gate -- and where it is reachable by hand, withholding
 * the weak matchers from a proved 2400/4 transmission is the same right answer as on live input.
 */
int dsd_frame_sync_suppress_4800_4_for_2400_4_transmission(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return non-zero when TCP no-signal diagnostics should stay off the console.
 */
int dsd_frame_sync_suppress_tcp_no_signal_console(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Return the number of no-sync buffer passes to dwell before SPS hunt advances.
 */
int dsd_frame_sync_sps_hunt_dwell_passes(const dsd_opts* opts, const dsd_state* state);

/**
 * @brief Give the active symbol profile a full dwell to prove itself on.
 *
 * For the sites that put the decoder somewhere new without going through the hunt -- a
 * trunking grant retuning to a voice channel, say. The profile that arrives has not spent
 * any of its own budget yet, and inheriting what the previous channel spent can retire it
 * within symbols of the tune (#392).
 *
 * Resets the whole budget: the counter, the measurement anchor and the refund snapshot.
 * Callers that assign dsd_state::sps_hunt_idx directly owe all three (#394); this is what
 * they should call rather than zeroing the counter alone.
 *
 * @param state Decoder state; NULL is a no-op.
 */
void dsd_frame_sync_sps_hunt_restart_dwell(dsd_state* state);

/**
 * @brief End a P25p1 chain comparison in flight (dsd_p25p1_mod_compare) because the receiver is about to be retuned.
 *
 * A trunked grant or control-channel tune inherits the decoder's modulation and the learned P25p1 one, and while the
 * comparison measures the other chain both name that chain. This puts both back on the chain leading so far -- the
 * incumbent, unless the other chain has read more -- and ends the comparison without a verdict, so the tune that
 * follows lands on it. A Phase 2 grant that has named its slot (dsd_state::p25_p2_active_slot) keeps the CQPSK it set:
 * only the learned modulation goes back. The front end is left to the tune. The engine's voice and control-channel tune
 * requests call it before they read the modulation, and put the comparison back when the tune fails; it does nothing
 * when no comparison runs.
 */
void dsd_frame_sync_p25p1_mod_compare_end_for_tune(dsd_state* state);

/**
 * @brief The P25p1 modulation the session has learned, as a snapshot of it should record it: while a chain comparison
 * runs, the chain leading so far rather than dsd_state::p25_p1_validated_rf_mod, which the other chain's frames
 * overwrite during its pass. -1 for nothing learned.
 */
int dsd_frame_sync_p25p1_learned_rf_mod(const dsd_state* state);

/**
 * @brief The P25p1 decision the decoder thread publishes for a tune made on another thread (the P25 watchdog's), read
 * whole in one atomic load (dsd_frame_sync_p25p1_read_decision()).
 */
typedef struct {
    int raw;     /* the published value, which a notice carries (dsd_frame_sync_note_off_thread_tune()) */
    int running; /* a chain comparison runs */
    int learned; /* the learned modulation: the leading chain while one runs; 0 C4FM, 1 CQPSK, -1 nothing learned */
} dsd_p25p1_decision;

/**
 * @brief Publish @p state's P25p1 chain comparison and learned modulation (dsd_frame_sync_p25p1_learned_rf_mod()).
 *
 * The decoder thread owns both, and every step of the comparison publishes; a caller that writes either itself (a
 * decoded NID's stamp, a tune's rollback, a trunk-scan target's restore) publishes after it.
 */
void dsd_frame_sync_p25p1_mod_compare_publish(const dsd_state* state);

/**
 * @brief Read the published decision, once. Safe from any thread. An off-thread tune reads it once and uses that read
 * both for the chain it tunes and for its notice.
 */
void dsd_frame_sync_p25p1_read_decision(dsd_p25p1_decision* out);

/**
 * @brief Note an accepted tune made off the decoder thread, with the decision it read (@p read) and tuned by. The
 * decoder thread reconciles it at its next frame sync step (dsd_frame_sync_apply_off_thread_tunes()). Safe from any
 * thread.
 */
void dsd_frame_sync_note_off_thread_tune(const dsd_p25p1_decision* read);

/** @brief Whether a tune off the decoder thread awaits dsd_frame_sync_apply_off_thread_tunes(). Decoder thread. */
int dsd_frame_sync_off_thread_tune_pending(void);

/**
 * @brief On the decoder thread, reconcile the tunes made off it since the last call, whatever the comparison did since:
 * forget the sync join histories, make the learned modulation the chain the latest tune set, end a comparison still
 * running there without a verdict, drop a verdict reached since on a comparison that tune captured, and put rf_mod and
 * the front end (@p opts) back on the chain the tune set if a step since moved them. Runs under the P25 SM tick guard
 * (taken when the caller does not hold it). 1 when one was reconciled.
 */
int dsd_frame_sync_apply_off_thread_tunes(const dsd_opts* opts, dsd_state* state);

/**
 * @brief A run boundary (a new session on @p state, or its end): seed the published decision from @p state (or with
 * nothing learned for NULL) and drop the notices posted so far, before the P25 watchdog starts. Decoder thread.
 */
void dsd_frame_sync_off_thread_tunes_reset(const dsd_state* state);

/**
 * @brief Return the symbol rate, in Hz, of the SPS hunt profile currently selected.
 *
 * The decoder's own timing authority on RTL-family FSK input: the front end applies
 * a hunt's profile request asynchronously, so its published symbol rate lags the
 * hunt and must not be what the slicer derives samples-per-symbol from.
 *
 * @param state Decoder state; NULL or an out-of-range index yields the 4800/4 default.
 */
int dsd_frame_sync_active_profile_symbol_rate_hz(const dsd_state* state);
/** Slicer levels of the active profile; NULL/invalid indices yield four levels. */
int dsd_frame_sync_active_profile_levels(const dsd_state* state);

/**
 * @brief Scan for a valid frame sync pattern and return its type.
 */
int getFrameSync(dsd_opts* opts, dsd_state* state);

/**
 * @brief Emit diagnostic information about detected frame sync.
 *
 * @param frametype Human-friendly frame type string.
 * @param offset Bit offset into the buffer where sync was found.
 * @param modulation Modulation label (e.g., C4FM, QPSK).
 */
void printFrameSync(const dsd_opts* opts, const dsd_state* state, const char* frametype, int offset,
                    const char* modulation);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_FRAME_SYNC_H_H */
