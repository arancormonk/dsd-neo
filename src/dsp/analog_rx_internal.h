// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Module-private analog receive core: the sub-audible front end and its detectors.
 *
 * The core is pure DSP over a block of raw monitor audio (no dsd_state, no clock), so the
 * detector tests drive it in sample time at any rate and any scale. src/dsp/analog_rx.c owns
 * the decoder-thread glue: the tap in the unsynced analog path, the resets, the publication
 * into dsd_state::analog_rx and the "Received tone:" log line.
 *
 * Signal path (issue #522):
 *   raw block @ fs -> stage 1: polyphase Blackman FIR, decimate by D = floor(fs / 2400)
 *                  -> stage 2: Blackman LPF at fs/D, 290 Hz cutoff, 60 Hz transition
 *                  -> DC blocker -> every detector in the core's table (CTCSS, then DCS)
 * Detectors also get the stage-1 output delayed to line up with stage 2 (the "wide" stream,
 * about 0-1 kHz): the CTCSS detector uses it to see a voice fundamental's harmonics, which a
 * tone does not have. And they get the "full" stream, aligned the same way: the raw input's
 * mean square over the span of each decimated sample, before any filter. Decimation folds a
 * small residue of everything above the band into it (stage 1 leaves it at least 58 dB down),
 * so a steady voice-band tone with nothing else below 290 Hz would otherwise read as a pure
 * sub-audible tone; the full stream is what tells that residue from a real one.
 * Every threshold a detector applies is a ratio against the energy of one of these streams,
 * so the RTL live (about +/-1/pi), replay (unscaled) and int16-scale PCM inputs all read the
 * same. The carrier test reads the raw block's mean square before the front end.
 */

#ifndef DSD_NEO_SRC_DSP_ANALOG_RX_INTERNAL_H_
#define DSD_NEO_SRC_DSP_ANALOG_RX_INTERNAL_H_

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Lowest input rate the front end accepts: below it there is no 2.4 kHz band to keep. */
enum { DSD_ANALOG_RX_MIN_RATE_HZ = 2400 };

/**
 * @brief Highest input rate the front end accepts.
 *
 * Stage 1 needs about fs / 325 taps, and DSD_ANALOG_RX_STAGE1_MAX_TAPS holds a design up to
 * about 333 kHz; the limit is a round figure below that, above every audio or RTL output rate
 * in use (192 kHz WAV included).
 */
enum { DSD_ANALOG_RX_MAX_RATE_HZ = 320000 };

/**
 * @brief Top of the sub-audible band: stage 2's cutoff, and the band every detector assumes
 * its noise fills.
 */
#define DSD_ANALOG_RX_BAND_HZ      290.0

/**
 * @brief Corner of the front end's DC blocker (a one-pole high-pass on the band).
 *
 * The DCS detector undoes it: at 134.4 bit/s a 10 Hz high-pass takes a third of a bit's level
 * away per bit of a run, so it moves the pole to DSD_ANALOG_DCS_DC_CORNER_HZ instead.
 */
#define DSD_ANALOG_RX_DC_CORNER_HZ 10.0

/** @brief Target decimated rate; the actual one is fs / floor(fs / 2400). */
enum { DSD_ANALOG_RX_TARGET_RATE_HZ = 2400 };

/** @brief Tap capacity of each front-end FIR: stage 1 needs about fs / 325 taps (see DSD_ANALOG_RX_MAX_RATE_HZ). */
enum { DSD_ANALOG_RX_STAGE1_MAX_TAPS = 1023, DSD_ANALOG_RX_STAGE2_MAX_TAPS = 511 };

/** @brief Decimated samples produced per detector hand-off. */
enum { DSD_ANALOG_RX_SCRATCH = 512 };

/** @brief CTCSS correlator geometry: 50 ms sub-blocks, a 5-sub-block (250 ms) window, 50 ms hop. */
enum { DSD_ANALOG_CTCSS_SUBBLOCK_MS = 50, DSD_ANALOG_CTCSS_WINDOW = 5 };

/**
 * @brief The late acquisition windows: the newest 12 sub-blocks (600 ms) and the newest 8
 * (400 ms), from the ring of 12.
 *
 * While no tone is locked, every hop also measures both, over no more than has closed since the
 * last reset or loss (the longer one is cut short until then, and neither runs before 8 have), so
 * a tone that has just been lost is never in them. The 250 ms window locks most tones; these lock
 * the ones its noise kept from it (see ctcss_step_late() in analog_ctcss.c).
 */
enum { DSD_ANALOG_CTCSS_LONG_WINDOW = 12, DSD_ANALOG_CTCSS_LONG_MIN = 8 };

/** @brief Consecutive agreeing hops needed to lock, and failing hops that lose the lock. */
enum { DSD_ANALOG_CTCSS_ACQUIRE_HOPS = 2, DSD_ANALOG_CTCSS_LOSE_HOPS = 4 };

/** @brief Carrier time without a lock after which the verdict is "no tone". */
enum { DSD_ANALOG_CTCSS_NO_TONE_MS = 500 };

/** @brief Wide-stream history the harmonic check correlates over: one late acquisition window at up to 4.8 kHz. */
enum { DSD_ANALOG_CTCSS_WIDE_MAX = (DSD_ANALOG_CTCSS_LONG_WINDOW * DSD_ANALOG_CTCSS_SUBBLOCK_MS * 4800) / 1000 };

/**
 * @brief What one detector currently reports.
 *
 * state is a dsd_analog_tone_state value limited to ACQUIRING, LOCKED and NONE: carrier and
 * activity are the core's business, not a detector's.
 */
typedef struct {
    int state;           /**< ACQUIRING, LOCKED or NONE (dsd_analog_tone_state) */
    int kind;            /**< dsd_analog_tone_kind; NONE unless LOCKED */
    int ctcss_tenths_hz; /**< locked CTCSS tone, tenths of a hertz */
    int dcs_code;        /**< locked DCS code as its value (023 octal = 19), canonical */
    int dcs_inverted;    /**< 1 when the canonical member of the locked DCS class is inverted */
} dsd_analog_rx_report;

/**
 * @brief Detector plug-in interface: every detector consumes the same decimated stream.
 *
 * configure() designs for a new decimated rate and starts from scratch; reset() starts from
 * scratch at the current rate; process() takes @p count decimated samples of the sub-audible
 * band and the time-aligned wide and full streams (see the file comment), and @p freeze says
 * they arrived while the carrier read closed inside its hangover: the detector keeps time
 * through them, but may not change its verdict on them alone (the CTCSS detector keeps it on
 * any hop whose newest 100 ms holds nothing else); report() says where it stands.
 */
typedef struct {
    const char* name;
    void (*configure)(void* ctx, double rate_hz);
    void (*reset)(void* ctx);
    void (*process)(void* ctx, const float* band, const float* wide, const float* full, int count, int freeze);
    void (*report)(const void* ctx, dsd_analog_rx_report* out);
} dsd_analog_rx_detector_ops;

/** @brief Per-hop diagnostics of the CTCSS evaluator, for tests and tuning. */
typedef struct {
    int evaluated;     /**< 1 once a full window has been evaluated */
    int best_index;    /**< table index of the strongest correlator bin */
    int snapped;       /**< table index the fine estimate snapped to, or -1 */
    double est_hz;     /**< fine frequency estimate */
    double rho;        /**< share of the sub-audible band energy the tone explains (0..1) */
    double residual;   /**< weighted RMS phase residual of the linear fit, radians */
    double chi2;       /**< reduced chi-square of that fit against the band's phase noise */
    double share;      /**< the tone's share of the raw input's full-band power (0..1) */
    double recent_rho; /**< rho over the newest 100 ms at the locked frequency, when locked */
    double harmonic;   /**< phase-locked (2f, 3f) power over the candidate's; computed only when needed */
} dsd_analog_ctcss_hop;

/** @brief A tone the acquisition tracks from hop to hop, one per acquisition window. */
typedef struct {
    int index; /**< table index the previous hop qualified, or -1 */
    double hz; /**< fine estimate of the previous qualifying hop */
    /** index's estimate from the qualifying hop before hz's, when the two agreed: the burst
        reference a lock on index starts from */
    double prev_hz;
    int run; /**< consecutive hops that qualified index */
} dsd_analog_ctcss_cand;

/**
 * @brief CTCSS detector working state (a correlator bin per supported tone).
 *
 * Each bin runs a phasor at its table frequency continuously across sub-blocks, so the
 * sub-block correlations keep absolute phase; the slope of that phase across the window is
 * the offset from the bin, which is how 67.0 and 69.3 Hz are told apart in 250 ms.
 */
typedef struct {
    double rate_hz; /**< decimated rate this detector is designed for; 0 = unconfigured */
    int sub_len;    /**< samples per sub-block */
    int sub_fill;
    int sub_open;  /**< 1 once a sample of the sub-block being filled arrived unfrozen (carrier open) */
    int prev_open; /**< the same for the newest sub-block in the ring */
    double sub_energy;
    double osc_re[DSD_CTCSS_TONE_COUNT];
    double osc_im[DSD_CTCSS_TONE_COUNT];
    double step_re[DSD_CTCSS_TONE_COUNT];
    double step_im[DSD_CTCSS_TONE_COUNT];
    double acc_re[DSD_CTCSS_TONE_COUNT];
    double acc_im[DSD_CTCSS_TONE_COUNT];
    double ring_re[DSD_ANALOG_CTCSS_LONG_WINDOW][DSD_CTCSS_TONE_COUNT];
    double ring_im[DSD_ANALOG_CTCSS_LONG_WINDOW][DSD_CTCSS_TONE_COUNT];
    double ring_energy[DSD_ANALOG_CTCSS_LONG_WINDOW];
    double sub_full;                                /**< full-stream energy of the sub-block being filled */
    double ring_full[DSD_ANALOG_CTCSS_LONG_WINDOW]; /**< full-stream energy of each sub-block in the ring */
    int ring_head;  /**< next ring slot to write; the oldest sub-block once the ring is full */
    int ring_count; /**< sub-blocks in the ring, up to DSD_ANALOG_CTCSS_LONG_WINDOW */
    int fresh;      /**< sub-blocks closed since the last reset or loss, up to DSD_ANALOG_CTCSS_LONG_WINDOW */
    int state;      /**< dsd_analog_tone_state: ACQUIRING, LOCKED or NONE */
    int locked;     /**< table index of the locked tone, or -1 */
    double locked_hz;
    /** The reverse burst check's reference: locked_hz as it stood two hops ago, or on the first
        hop after a lock the candidate's estimate from the hop before the lock (prev_hz).
        Either comes from a window that ends no later than the older sub-block of each pair the
        check compares. */
    double burst_ref_hz;
    dsd_analog_ctcss_cand cand;            /**< what the 250 ms window has been qualifying */
    dsd_analog_ctcss_cand long_cand;       /**< what the late acquisition window has been qualifying */
    int fail_run;                          /**< consecutive hops the locked tone failed its hold */
    int holdoff;                           /**< hops left before a tone may lock again (after a reverse burst) */
    int64_t open_samples;                  /**< unfrozen samples since the last reset, for the no-tone verdict */
    float wide[DSD_ANALOG_CTCSS_WIDE_MAX]; /**< the ring's wide-stream samples, circular */
    int wide_len;                          /**< ring length in samples (LONG_WINDOW * sub_len) */
    int wide_pos;                          /**< next write position; the oldest sample once the ring is full */
    dsd_analog_ctcss_hop last_hop;
} dsd_analog_ctcss;

extern const dsd_analog_rx_detector_ops dsd_analog_ctcss_ops;

/** @brief DCS bit rate, bit/s. */
#define DSD_ANALOG_DCS_BAUD         134.4

/** @brief Pole the DCS detector moves the front end's DC blocker to: about 0.3 s per 1/e. */
#define DSD_ANALOG_DCS_DC_CORNER_HZ 0.5

/**
 * @brief DCS detector geometry and verdict rules (see analog_dcs.c).
 *
 * - HYPOTHESES: slicers, one per model of the low-frequency droop a receiver's DC block puts
 *   on the bit levels (none, and three one-pole high-passes).
 * - RING: re-poled band samples kept for the bit integrals (a power of two). The bit clock's
 *   edge reaches back two bit integrals, 2 * box_len samples, and a bit read one sample more,
 *   for the interpolation; below 4800 Hz, above every decimated rate, a bit is at most 36
 *   samples, so 74 samples are enough.
 * - HISTORY_BITS: decisions each slicer keeps: two words.
 * - ACQUIRE_DISTANCE: bits the two words a lock is read from may differ by (one of them must be
 *   a supported code's word exactly).
 * - HOLD_DISTANCE: bits a held word may differ from the expected one.
 * - LOSE_BITS: bits without a held word that lose the lock, counting only bits whose window
 *   was read wholly with the carrier open (after a dropout the windows hold bits of it for a
 *   word, which no signal reads through).
 * - SPAN_BITS: bits since the lock last held, read with the carrier open or closed, that lose
 *   it whatever the windows held: the bound under dropouts and a flickering carrier.
 * - TURNOFF_BITS: bits the turn-off tone (134.4 Hz) is measured over; TURNOFF_RUN: consecutive
 *   bits it must stand out in before the first bit no slicer holds the expected word ends the
 *   lock, ahead of LOSE_BITS.
 */
enum {
    DSD_ANALOG_DCS_HYPOTHESES = 4,
    DSD_ANALOG_DCS_RING = 128,
    DSD_ANALOG_DCS_HISTORY_BITS = 46,
    DSD_ANALOG_DCS_ACQUIRE_DISTANCE = 1,
    DSD_ANALOG_DCS_HOLD_DISTANCE = 1,
    DSD_ANALOG_DCS_LOSE_BITS = 32,
    DSD_ANALOG_DCS_SPAN_BITS = 64,
    DSD_ANALOG_DCS_TURNOFF_BITS = 6,
    DSD_ANALOG_DCS_TURNOFF_RUN = 2,
};

/** @brief One droop hypothesis: a slicer with its own decisions. */
typedef struct {
    double droop;    /**< per-bit decay of the modelled high-pass, e^(-T/tau); 1 = none */
    double gain;     /**< a bit integral's share of its level under that droop, (1 - d) / -ln d */
    double baseline; /**< the level the high-pass has taken away, in bit-integral units */
    double amp;      /**< the level of a bit, in bit-integral units; 0 until the first bit */
    uint64_t bits;   /**< the newest HISTORY_BITS decisions, the newest in bit HISTORY_BITS - 1 */
    int count;       /**< decisions since the last reset, up to HISTORY_BITS */
} dsd_analog_dcs_slicer;

/**
 * @brief DCS detector working state.
 *
 * The band is re-poled (the front end's 10 Hz DC blocker undone, 0.5 Hz put in its place),
 * integrated over each bit at the instants a bit clock recovers from the signal's own edges,
 * and sliced by every droop hypothesis. A lock is a supported code's word read twice in a row
 * by one slicer; it holds while some slicer reads the expected word within one bit, or the
 * locked code exactly at another place in its word.
 */
typedef struct {
    double rate_hz;                /**< decimated rate this detector is designed for; 0 = unconfigured */
    double samples_per_bit;        /**< rate_hz / DSD_ANALOG_DCS_BAUD */
    int box_len;                   /**< bit integral length in samples: samples_per_bit rounded */
    double fe_pole;                /**< the front end's DC blocker pole, which the detector undoes */
    double slow_pole;              /**< the pole the detector puts in its place */
    double clock_alpha;            /**< per-sample weight of the bit clock's edge-energy average */
    double x1;                     /**< previous band sample */
    double u1;                     /**< previous re-poled sample */
    double u[DSD_ANALOG_DCS_RING]; /**< re-poled samples, sample k at k & (RING - 1) */
    int64_t n;                     /**< samples since the last reset */
    double next_bit;               /**< sample time the next bit ends at */
    /** The bit clock: the edge energy's component at the bit rate, averaged; its angle says
        where in the bit the edges fall. */
    double clock_re;
    double clock_im;
    int clock_heard; /**< 1 once the clock has averaged a sample with the carrier open */
    dsd_analog_dcs_slicer slicer[DSD_ANALOG_DCS_HYPOTHESES];
    /* A phasor at the bit rate, e^(-j 2 pi n / samples_per_bit): the bit clock's reference and
       the turn-off tone's correlator, which runs per bit over the newest TURNOFF_BITS bits. */
    double osc_re;
    double osc_im;
    double step_re;
    double step_im;
    double bit_re;
    double bit_im;
    double bit_energy;
    int bit_samples;
    int bit_open; /**< 1 once a sample of the bit being read arrived unfrozen (carrier open) */
    double ring_re[DSD_ANALOG_DCS_TURNOFF_BITS];
    double ring_im[DSD_ANALOG_DCS_TURNOFF_BITS];
    double ring_energy[DSD_ANALOG_DCS_TURNOFF_BITS];
    int ring_samples[DSD_ANALOG_DCS_TURNOFF_BITS];
    int ring_head;
    int ring_count;
    int turnoff_run; /**< consecutive bits the turn-off tone dominated */
    /* Verdict. */
    int state;            /**< dsd_analog_tone_state: ACQUIRING, LOCKED or NONE */
    int code;             /**< locked code (canonical), or -1 */
    int inverted;         /**< polarity of the canonical member of the locked class */
    uint32_t expected;    /**< the window the locked signal reads next, earliest bit in bit 0 */
    int fail_run;         /**< bits since the last hold whose window, read wholly with the carrier open, held nothing */
    int since_held;       /**< bits read since the lock last held (carrier open or closed), up to SPAN_BITS */
    int since_frozen;     /**< bits read with the carrier open since the last read with it closed, up to a word */
    int64_t open_samples; /**< unfrozen samples since the last reset, for the no-code verdict */
    int64_t bits_decided; /**< bits read since the last reset (tests) */
} dsd_analog_dcs;

extern const dsd_analog_rx_detector_ops dsd_analog_dcs_ops;

/** @brief The shared sub-audible front end: two decimating/low-pass stages and a DC blocker. */
typedef struct {
    int in_rate_hz; /**< design input rate; 0 = unconfigured */
    int active;     /**< 0 when the rate is outside the supported range or a design failed */
    int decim;
    double out_rate_hz;
    int n1;
    int n2;
    float taps1[DSD_ANALOG_RX_STAGE1_MAX_TAPS];
    float taps2[DSD_ANALOG_RX_STAGE2_MAX_TAPS];
    /* Doubled delay lines: each sample is written twice, so the newest n taps are always
       one contiguous run and the FIR never wraps. */
    float hist1[2 * DSD_ANALOG_RX_STAGE1_MAX_TAPS];
    float hist2[2 * DSD_ANALOG_RX_STAGE2_MAX_TAPS];
    int pos1;
    int pos2;
    int phase; /**< input samples since the last stage-1 output */
    /* Stage-1 output delayed by stage 2's group delay, so the wide stream lines up in time
       with the band it sits beside. */
    float wide_delay[DSD_ANALOG_RX_STAGE2_MAX_TAPS];
    int wide_delay_len;
    int wide_delay_pos;
    /* The raw input's energy since the last decimated output, and its mean square per output
       delayed by both stages' group delay, so the full stream lines up with the band too. */
    double full_acc;
    float full_delay[DSD_ANALOG_RX_STAGE2_MAX_TAPS];
    int full_delay_len;
    int full_delay_pos;
    double dc_alpha;
    double dc_x1;
    double dc_y1;
} dsd_analog_subaudible_fe;

/** @brief Pure analog receive core: front end, detectors, carrier and hangover. */
typedef struct {
    dsd_analog_subaudible_fe fe;
    dsd_analog_ctcss ctcss;
    dsd_analog_dcs dcs;
    int carrier_open;       /**< 1 from the first open block until the hangover expires */
    int64_t closed_samples; /**< input samples since the carrier last read open */
    uint32_t resets;        /**< bumped by every reset, published as the generation */
    /** 1 + the table index of the detector whose lock the publication shows, 0 while none is
        locked: the lock that came first keeps the publication until it is lost, so a later
        lock of the other kind (a voice's talk-off on a coded channel) never hides it. */
    int held_by;
    float scratch[DSD_ANALOG_RX_SCRATCH];
    float scratch_wide[DSD_ANALOG_RX_SCRATCH];
    float scratch_full[DSD_ANALOG_RX_SCRATCH];
} dsd_analog_rx_core;

/**
 * @brief Mean-square level below which a block is treated as no carrier.
 *
 * Absolute on purpose, and far below any real audio at any of the three input scales: it only
 * has to reject the zeroed blocks a closed demodulator squelch emits and digital silence.
 */
#define DSD_ANALOG_RX_FLOOR_MEAN_SQUARE 1e-9

/** @brief Zero the core; the first processed block designs the front end for its rate. */
void dsd_analog_rx_core_init(dsd_analog_rx_core* core);

/** @brief Drop every detector verdict and all filter history; keeps the rate design. */
void dsd_analog_rx_core_reset(dsd_analog_rx_core* core);

/**
 * @brief Feed one block of raw monitor audio.
 *
 * @param rate_hz      Input rate; a change redesigns the front end and resets every detector.
 * @param squelch_open 0 when the caller's squelch reads the block closed; the block is then
 *                     treated as no carrier whatever its level.
 * @return 1 when the front end is running at this rate, 0 when the rate is unusable.
 */
int dsd_analog_rx_core_process(dsd_analog_rx_core* core, const float* block, int count, int rate_hz, int squelch_open);

/**
 * @brief Where the core stands, in publication terms.
 *
 * Zeroes @p out, then fills carrier_open, tone_state (IDLE without carrier, else the merged
 * detector verdict), tone_kind, ctcss_tenths_hz or dcs_code and dcs_inverted, and generation.
 * gate stays OFF: detection never gates audio.
 */
void dsd_analog_rx_core_publish(const dsd_analog_rx_core* core, dsd_analog_rx_publication* out);

/**
 * @brief Design the front end for @p rate_hz.
 *
 * @return 1 when usable, 0 outside DSD_ANALOG_RX_MIN_RATE_HZ..DSD_ANALOG_RX_MAX_RATE_HZ.
 */
int dsd_analog_subaudible_fe_configure(dsd_analog_subaudible_fe* fe, int rate_hz);

/** @brief Clear the front end's filter history without redesigning it. */
void dsd_analog_subaudible_fe_clear(dsd_analog_subaudible_fe* fe);

/**
 * @brief Filter and decimate @p count input samples.
 *
 * Writes at most @p out_cap decimated samples of the sub-audible band to @p band, of the
 * aligned wide stream to @p wide and of the aligned full stream to @p full (either may be
 * NULL), and returns how many; the caller sizes its input so the output fits
 * (count / decim + 1 <= out_cap).
 */
int dsd_analog_subaudible_fe_process(dsd_analog_subaudible_fe* fe, const float* in, int count, float* band, float* wide,
                                     float* full, int out_cap);

/** @brief The CTCSS evaluator's last hop, for tests. */
const dsd_analog_ctcss_hop* dsd_analog_ctcss_last_hop(const dsd_analog_ctcss* det);

/**
 * @brief Test hook: replace the monotonic clock the tap reads to tell a paused live stream input.
 *
 * NULL restores dsd_time_monotonic_ms(). Defined only in the test-hook build of the DSP module
 * (dsd-neo_dsp_private_test_support, compiled with DSD_NEO_TEST_HOOKS); the shipped library
 * has no such symbol.
 */
void dsd_analog_rx_test_set_clock(uint64_t (*now_ms)(void));

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_DSP_ANALOG_RX_INTERNAL_H_ */
