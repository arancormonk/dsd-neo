// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 2 voice playout: per-slot queues played per burst pair, in both output formats (issue #651).
 *
 * Each slot's synthesized frames queue in order with the output verdict their burst had when it was decoded. After
 * slot 2's burst of each timeslot pair, the playout emits as many blocks as every open stream has ready, so the two
 * slots run concurrently at the air rate even though each slot's 4V/2V sequence starts at its own pair. A voice burst
 * an open stream misses plays as silence in place, sized from the slot's 2V position. Short (int16) and float (`-y`)
 * output go through the same queues and the same mixer.
 *
 * Every function runs on the decoder thread inside processFrame(), or in code holding the P25 SM tick guard.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_P25P2_PLAYOUT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_P25P2_PLAYOUT_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Frames a slot's queue holds: one superframe of carry plus a placeholder. */
#define DSD_P25P2_PLAYOUT_CAP 24

/** Kind of what a timeslot carried for its slot, as the frame decoder reports it to dsd_p25p2_playout_burst_done(). */
typedef enum dsd_p25p2_burst_kind {
    DSD_P25P2_BURST_4V = 0,    /**< Decoded 4V voice burst. */
    DSD_P25P2_BURST_2V = 1,    /**< Decoded 2V voice burst. */
    DSD_P25P2_BURST_SACCH = 2, /**< SACCH: carries no voice. */
    DSD_P25P2_BURST_OTHER = 3, /**< FACCH or LCCH in a voice position: the voice it replaced is missing. */
    DSD_P25P2_BURST_LOST = 4,  /**< Undecodable or skipped: what it carried is unknown. */
} dsd_p25p2_burst_kind;

/** A burst's output verdict, fixed when its first frame is queued. */
typedef struct dsd_p25p2_playout_verdict {
    uint8_t blocked;    /**< The talkgroup policy mutes the call (no hold). */
    uint8_t hold;       /**< A talkgroup hold matches the call: it plays even from a switched-off slot. */
    uint8_t crypto_ok;  /**< p25_crypto_audio_output_permitted(). */
    uint8_t recordable; /**< The static WAV may record the call. */
} dsd_p25p2_playout_verdict;

/** One queue entry. */
typedef struct dsd_p25p2_playout_entry {
    union {
        int16_t s16[160];
        float f32[160];
    } pcm;

    uint8_t kind;  /**< DSD_P25P2_PLAYOUT_ENTRY_* (playout-private values). */
    uint8_t fresh; /**< Decoded media the session's format plays (the #574 stamp). */
    uint8_t pair;  /**< Placeholders: the voice pair they stand for. */
    dsd_p25p2_playout_verdict verdict;
} dsd_p25p2_playout_entry;

/** One slot's queue and stream state. */
typedef struct dsd_p25p2_playout_slot {
    dsd_p25p2_playout_entry q[DSD_P25P2_PLAYOUT_CAP];
    uint8_t head;
    uint8_t count;
    uint8_t open;          /**< Mid-transmission: a voice burst it misses plays as silence. */
    uint64_t epoch;        /**< Canonical call epoch the open stream belongs to. */
    uint32_t burst_serial; /**< Burst the current verdict was taken for. */
    dsd_p25p2_playout_verdict verdict;
    int8_t cur_pair;            /**< Voice pair of the burst being processed, -1 between superframes. */
    uint8_t cur_pair_done;      /**< dsd_p25p2_playout_burst_done() already ran for cur_pair. */
    int8_t phase_2v;            /**< Pair carrying the slot's 2V, -1 unknown. */
    uint8_t phase_proven;       /**< phase_2v is proven (decoded 2V, or every other pair carried a 4V). */
    int8_t provisional;         /**< Provisional 2V pair while unproven, -1 none. */
    uint8_t seen_4v_mask;       /**< Voice pairs that carried a 4V in this transmission. */
    uint8_t missed;             /**< Consecutive voice bursts the open stream missed. */
    uint8_t sf_covered;         /**< The stream was open from pair 0 of the current superframe. */
    uint8_t sf_complete;        /**< ... and stayed open. */
    uint8_t sf_frames;          /**< Frames queued for the current superframe (decoded + fill, before debt). */
    uint8_t sf_placeholders;    /**< Placeholders queued in the current superframe. */
    uint8_t first_covered_done; /**< A covered superframe ended while the phase was unproven. */
    uint8_t partial_log_count;
    uint8_t partial_log_pair[4];
    uint8_t partial_log_size[4];
    int16_t fill_debt; /**< Fill frames emitted beyond the air time (positive) or short of it (negative). */
} dsd_p25p2_playout_slot;

/** Per-slot playout state, embedded in dsd_state. Decoder-thread private; snapshots skip it. */
typedef struct dsd_p25p2_playout {
    dsd_p25p2_playout_slot slot[2];
} dsd_p25p2_playout;

/**
 * @brief Queue the frame the vocoder just synthesized for @p slot.
 *
 * Copies the slot's short frame (s_l/s_r, zeros when dsd_state::mbe_short_silenced says the vocoder left it out) or
 * its float frame (audio_out_temp_buf(R), through agf()), whichever the session's format plays. The first frame of
 * a burst (@p burst_serial) fixes the burst's verdict: @p burst_verdict when the decode gate already evaluated the
 * policy for this burst, else one evaluation here. A frame whose slot's canonical call epoch differs from the open
 * stream's starts a new stream; the old stream's queued frames still play.
 */
void dsd_p25p2_playout_stage(dsd_opts* opts, dsd_state* state, int slot, int pair, uint32_t burst_serial,
                             const dsd_p25p2_playout_verdict* burst_verdict);

/** @brief The slot decoded a frame its gate muted: its stream closes. */
void dsd_p25p2_playout_note_muted(dsd_state* state, int slot);

/**
 * @brief Report what the slot's timeslot at superframe pair @p pair carried.
 *
 * Learns the slot's 2V position from decoded bursts, and queues silence for a voice burst an open stream missed.
 * Pair 5 (SACCH) ends the superframe.
 */
void dsd_p25p2_playout_burst_done(dsd_opts* opts, dsd_state* state, int slot, int pair, dsd_p25p2_burst_kind kind);

/** @brief Slot 2's burst of a pair was processed: emit what both open streams have ready. */
void dsd_p25p2_playout_pair_done(dsd_opts* opts, dsd_state* state);

/** @brief Close @p slot's stream (END, IDLE, HANGTIME, MAC Release). Never emits or discards. */
void dsd_p25p2_playout_close(dsd_state* state, int slot);

/** @brief Emit everything queued (the receiver leaves the carrier or the call), then close both streams. */
void dsd_p25p2_playout_drain(dsd_opts* opts, dsd_state* state);

/** @brief Discard @p slot's queue and stream state, or both slots' for a negative @p slot. */
void dsd_p25p2_playout_reset(dsd_state* state, int slot);

/** @brief Frames ready to play in @p slot's queue (ahead of any unresolved placeholder). */
int dsd_p25p2_playout_level(const dsd_state* state, int slot);

/**
 * @brief The talkgroup-policy part of @p slot's output verdict (blocked, hold, recordable), from one decision on the
 * slot's active call. crypto_ok is left 0 for the caller. Without an active call nothing is blocked or held and the
 * call is recordable, as the mixers' gates answer then.
 */
void dsd_p25p2_playout_policy_verdict(const dsd_opts* opts, const dsd_state* state, int slot,
                                      dsd_p25p2_playout_verdict* out);

/**
 * @brief dsd_p25p2_decode_audio_allowed() that also hands back the policy part of the output verdict its decision
 * gives, so the playout need not evaluate the policy again for the burst. @p verdict_valid is set to 1 when a decision
 * was evaluated, else 0 and @p verdict is untouched.
 */
int dsd_p25p2_decode_audio_allowed_verdict(const dsd_opts* opts, const dsd_state* state, int slot, int alg,
                                           dsd_p25p2_playout_verdict* verdict, int* verdict_valid);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_P25P2_PLAYOUT_H_H */
