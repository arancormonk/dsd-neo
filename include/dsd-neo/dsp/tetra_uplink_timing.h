// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief Burst timing and carrier phase for a TETRA uplink session, acquired from each burst's training sequence.
 *
 * Uplink bursts come from different radios, each with a clock of its own, and the Gardner TED and the Costas loop
 * free-run on noise between them. A control uplink burst is 103 symbols and a normal uplink burst 231, too short for
 * either loop to pull in from where a burst finds it: from half a symbol off the TED hangs for most of a burst, and the
 * Costas loop, clamped to a quarter turn either way, can meet a burst near 45 degrees from its lock and stay there.
 * Both carry a known training sequence (EN 300 392-2 clause 9.4.4.2.1, 9.4.4.2.4): the control burst the extended
 * training sequence, symbols 44 to 58 of 103 (clause 9.4.4.3.3), the normal burst normal training sequence 1 or 2,
 * symbols 110 to 120 of 231 (clause 9.4.4.3.2).
 *
 * The Gardner input of an uplink session passes through a look-ahead of DSD_TETRA_UL_LOOKAHEAD_SYMBOLS. On the way in
 * each sample's differential phasor x[n] conj(x[n - sps]) is correlated against both training sequences, on every
 * sample, so at every sampling phase; the sum is normalized by its Cauchy-Schwarz bound, the larger of the sequence and
 * its complex conjugate (a reversed spectrum; NTS2 is NTS1 conjugated) counts, and a carrier offset turns every term
 * alike, so it cancels. A peak at or above DSD_TETRA_UL_TRAINING_SCORE gives the training's last symbol centre to a
 * fraction of a sample, and with it the burst's symbol grid from its first symbol on. The training of a normal burst
 * is also part of the control burst's extended training sequence, two symbols before its end, so a normal burst is
 * decided only once those two symbols showed no extended training sequence ends there. Bursts do not overlap: one
 * whose span overlaps a burst found before it is kept only when its training scored higher, and then replaces it.
 *
 * When the Gardner's next symbol strobe reaches a burst's grid, two symbols before its first symbol, the strobe moves
 * by less than half a symbol onto the grid, so the whole burst is sampled at the phase its training measured, its
 * first block included. The Costas loop takes the rotation the training measured on the phasors (within a quarter
 * turn, the frame sync's 90 degree steps being its own) from the symbol after, with its frequency and error smoothing
 * started over. Both loops track from there as before.
 *
 * The Gardner runs over a whole demod block before the Costas loop takes any of its symbols, and a block can span
 * many bursts, so each burst's rotation waits in a queue keyed by the symbol it applies at until the Costas loop
 * reaches that symbol: every burst gets its own, however the input is cut into blocks.
 */

#ifndef DSD_NEO_DSP_TETRA_UPLINK_TIMING_H
#define DSD_NEO_DSP_TETRA_UPLINK_TIMING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Look-ahead in symbols: a normal uplink burst's training ends 121 symbols after its first symbol, and the decision
 * between a normal and a control burst, the strobe margin and the peak's finishing take a few more. */
#define DSD_TETRA_UL_LOOKAHEAD_SYMBOLS 128
/* Samples per symbol the look-ahead runs at: 2 to 8 (the TETRA rate domain, 36 to 144 kHz). */
#define DSD_TETRA_UL_MIN_SPS           2
#define DSD_TETRA_UL_MAX_SPS           8
/* Complex samples the look-ahead ring holds: a power of two above the look-ahead at the largest sps. */
#define DSD_TETRA_UL_RING              2048
/* Differential phasors kept: a power of two above the extended training sequence's span at the largest sps. */
#define DSD_TETRA_UL_DIFF_RING         128
/* Bursts found and not yet passed: the one the Gardner is heading for and those behind it. */
#define DSD_TETRA_UL_BURSTS            4
/* Burst rotations waiting for the carrier loop (dsd_tetra_ul_timing_align()): the Gardner places every burst of a
 * demod block before the carrier loop takes the block's first symbol. The largest block (MAXIMUM_BUF_LENGTH / 2
 * complex samples and the channel filter's growth, demod_state.h) at 2 samples a symbol holds 638 control uplink
 * bursts of 103 symbols; one more waits from the block before. costas.cpp asserts the bound. */
#define DSD_TETRA_UL_CARRIERS          640
/* Symbols between the uplink Gardner's gain decisions (op25_gardner_cc()), counted from the reset: the 5.33 ms a
 * replay's block, one live transfer, lasts at every TETRA rate, at the symbol count rather than where a block begins,
 * so a stream gives the same symbols however it is cut. */
#define DSD_TETRA_UL_GAIN_SYMBOLS      96
/* Normalized training score a burst needs (1 is an exact, noiseless match). The trainings of the uplink fixture score
 * 0.95 to 0.99 at their symbol centres and far less two samples either side; payload stays below 0.83 and noise mostly
 * below the threshold (up to 0.91 on the normal training sequence), and a burst found on noise is replaced by the real
 * one, which scores higher. */
#define DSD_TETRA_UL_TRAINING_SCORE    0.85f

/** A burst's training peak while it is being followed, and the score of the sample before. */
typedef struct {
    float prev;       /**< score of the previous sample */
    int active;       /**< 1 while a peak at or above the threshold is being followed */
    uint64_t best_n;  /**< sample index of the best score so far */
    float best;       /**< best score */
    float best_phase; /**< rotation of the differential phasors at best_n, radians */
    int best_conj;    /**< 1 when the conjugated sequence matched there */
    float before;     /**< score of the sample before best_n */
    float after;      /**< score of the sample after best_n */
} dsd_tetra_ul_peak;

/* Symbols of the longest training sequence searched: the extended training sequence. */
#define DSD_TETRA_UL_TRAINING_MAX 15

/** A training sequence as unit phase steps, in the order sent. */
typedef struct {
    float re[DSD_TETRA_UL_TRAINING_MAX];
    float im[DSD_TETRA_UL_TRAINING_MAX];
    int len;
} dsd_tetra_ul_training;

/** A burst the training placed, in Gardner input samples (the look-ahead included). */
typedef struct {
    uint64_t anchor; /**< centre of the training's last symbol, whole samples */
    float frac;      /**< and its fraction of a sample, -0.5 to 0.5 */
    uint64_t start;  /**< centre of its first symbol, whole samples */
    uint64_t end;    /**< one symbol past its last symbol */
    float score;     /**< its training score */
    float carrier;   /**< the carrier loop's phase for it: the phasors' rotation within a quarter turn */
    int pending;     /**< 1 until the Gardner's strobe reached it */
} dsd_tetra_ul_burst;

/** A burst's rotation waiting for the carrier loop. */
typedef struct {
    uint64_t at; /**< the symbol (symbols_out count) the carrier loop takes it from */
    float phase; /**< the carrier loop's phase from there */
} dsd_tetra_ul_carrier;

/** The look-ahead and its training search (demod_state::tetra_ul_timing). */
typedef struct {
    int sps;         /**< samples per symbol it was reset for; 0 = not set up */
    int delay;       /**< look-ahead in samples */
    uint64_t pushed; /**< samples pushed since the reset */
    float ring[DSD_TETRA_UL_RING * 2];
    float diff[DSD_TETRA_UL_DIFF_RING * 2];
    float diff_pow[DSD_TETRA_UL_DIFF_RING];
    dsd_tetra_ul_training ets_seq; /**< the extended training sequence (a control uplink burst) */
    dsd_tetra_ul_training nts_seq; /**< normal training sequence 1 (a normal uplink burst) */
    dsd_tetra_ul_peak ets;         /**< extended training sequence: a control uplink burst */
    dsd_tetra_ul_peak nts;         /**< normal training sequence: a normal uplink burst */
    int nub_held;                  /**< 1 while a normal burst waits for its decision */
    uint64_t nub_decide;           /**< sample index from which it is a normal burst */
    dsd_tetra_ul_burst nub;        /**< that normal burst */
    dsd_tetra_ul_burst bursts[DSD_TETRA_UL_BURSTS];
    uint64_t symbols_out;  /**< symbols the Gardner emitted since the reset */
    uint64_t carrier_seen; /**< symbols the carrier loop took since the reset */
    /** Burst rotations waiting for the carrier loop, oldest first from carrier_head, in the order of their symbols. */
    dsd_tetra_ul_carrier carriers[DSD_TETRA_UL_CARRIERS];
    int carrier_head;        /**< index of the oldest waiting rotation */
    int carrier_count;       /**< rotations waiting */
    uint64_t aligned;        /**< bursts the Gardner's strobe moved onto since the reset */
    uint64_t carriers_taken; /**< burst rotations the carrier loop took since the reset */
    /** The Gardner's gain for an uplink session, decided every DSD_TETRA_UL_GAIN_SYMBOLS symbols it emits; 0 until
     * the first decision. */
    float gain_mu;
    /** The pushed count at which every non-zero sample pushed so far has come out (dsd_tetra_ul_timing_held()). */
    uint64_t held_until;
} dsd_tetra_ul_timing;

/** Start over at @p sps (DSD_TETRA_UL_MIN_SPS to DSD_TETRA_UL_MAX_SPS): an empty look-ahead, no burst. */
void dsd_tetra_ul_timing_reset(dsd_tetra_ul_timing* t, int sps);

/**
 * @brief Push one Gardner input sample and search the trainings that end on it.
 *
 * Writes the sample the look-ahead returns, DSD_TETRA_UL_LOOKAHEAD_SYMBOLS symbols older (0 while it fills), to
 * @p out_r / @p out_j. The Gardner takes that one.
 */
void dsd_tetra_ul_timing_push(dsd_tetra_ul_timing* t, float in_r, float in_j, float* out_r, float* out_j);

/**
 * @brief The strobe correction for the Gardner's next symbol.
 *
 * @p next_strobe is where the next symbol will be read, in samples relative to the next sample the Gardner will take
 * (0 is that sample, -1 the newest it took). Returns 1 once for each burst, when the strobe has reached two symbols
 * before its first symbol and not yet passed its training, with @p move the samples to add to the Gardner's mu that
 * put the strobe on the burst's grid, within half of @p omega either way; the burst's carrier phase then waits in the
 * queue for the symbol after the one the Gardner emits now (dsd_tetra_ul_timing_carrier()). Returns 0 with @p move 0
 * otherwise. The Gardner counts each symbol it emits in symbols_out.
 *
 * Should the queue be full (DSD_TETRA_UL_CARRIERS, more than the largest demod block holds of bursts that do not
 * overlap), the new phase replaces the newest waiting one: that burst's span is the one this burst cuts short.
 */
int dsd_tetra_ul_timing_align(dsd_tetra_ul_timing* t, double next_strobe, float omega, float* move);

/**
 * @brief Called by the carrier loop for each symbol it takes, in the Gardner's order.
 *
 * Returns 1 with @p phase, the phase the loop takes from this symbol on, at the symbol a burst's grid begins with
 * (dsd_tetra_ul_timing_align()); else 0. A waiting phase whose symbol already went by (never, while the carrier loop
 * takes every symbol the Gardner emits) is dropped.
 */
int dsd_tetra_ul_timing_carrier(dsd_tetra_ul_timing* t, float* phase);

/**
 * @brief Zero samples to push before every non-zero sample pushed so far has come out of the look-ahead.
 *
 * 0 when the look-ahead holds only zeros (or is not set up). Whenever the Gardner's input stops (a squelched block, the
 * end of a capture) the stage pushes zeros through until this and the Gardner's own delay are out
 * (dsd_tetra_ul_timing_held_through()), so a burst's last symbols reach the decoder then, not after whatever comes
 * next: full_demod() on a squelched block, dsd_demod_tetra_uplink_flush() at the end of a replay.
 */
size_t dsd_tetra_ul_timing_held(const dsd_tetra_ul_timing* t);

/**
 * @brief Zero samples to push before every non-zero sample pushed so far has come out of the look-ahead and @p tail
 * samples more have followed it.
 *
 * The stage behind the look-ahead (the Gardner TED's delay line) holds a sample @p tail more pushes before the last
 * symbol that reads it is out. 0 when no non-zero sample was pushed since the reset (or it is not set up);
 * dsd_tetra_ul_timing_held() is this with @p tail 0.
 */
size_t dsd_tetra_ul_timing_held_through(const dsd_tetra_ul_timing* t, size_t tail);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_DSP_TETRA_UPLINK_TIMING_H */
