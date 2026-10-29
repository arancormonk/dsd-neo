// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief OP25-compatible CQPSK signal chain interface.
 *
 * Direct port of OP25's CQPSK demodulator signal chain:
 *   AGC -> Gardner (timing) -> diff_phasor -> Costas (carrier)
 *
 * From OP25's p25_demodulator_dev.py line 486:
 *   self.connect(self.if_out, self.agc, self.fll, self.clock, self.diffdec, self.costas, ...)
 *
 * Where:
 *   - clock = op25_repeater.gardner_cc (timing recovery only)
 *   - diffdec = digital.diff_phasor_cc (differential decoding at symbol rate)
 *   - costas = op25_repeater.costas_loop_cc (carrier tracking at symbol rate)
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_COSTAS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_COSTAS_H_

#ifdef __cplusplus
extern "C" {
#endif

struct demod_state; /* fwd decl */

typedef struct {
    float phase;
    float freq;
    float max_freq;
    float min_freq;
    float damping;
    float loop_bw;
    float alpha;
    float beta;
    float error;
    float error_smooth;
    int initialized;
} dsd_costas_loop_state_t;

/**
 * @brief OP25-compatible FLL band-edge filter state.
 *
 * Direct port of GNU Radio's digital.fll_band_edge_cc used in OP25:
 *   self.fll = digital.fll_band_edge_cc(sps, excess_bw, 2*sps+1, TWO_PI/sps/350)
 *
 * The FLL uses band-edge filters to estimate frequency error before timing recovery.
 * This is critical for initial frequency acquisition on channel retunes.
 *
 * Filter size = 2*sps+1. At 48 kHz DSP bandwidth:
 *   - P25P1: SPS=10 -> 21 taps
 *   - P25P2: SPS=8  -> 17 taps
 *   - NXDN:  SPS=20 -> 41 taps
 * Size 48 supports SPS up to 23 (e.g., future 2400 sym/s modes at higher rates).
 */
#define FLL_BAND_EDGE_MAX_TAPS    48

/** @brief The TETRA FLL's lock window, in transfer sub-blocks (dsd_fll_band_edge_tetra_boundary()). */
#define DSD_TETRA_FLL_LOCK_WINDOW 16

typedef struct {
    float phase;    /* NCO phase accumulator (radians) */
    float freq;     /* NCO frequency (rad/sample) */
    float max_freq; /* Max frequency limit (rad/sample) */
    float min_freq; /* Min frequency limit (rad/sample) */
    float loop_bw;  /* Loop bandwidth */
    float alpha;    /* Loop filter gain (phase/proportional) - from control_loop */
    float beta;     /* Loop filter gain (frequency/integral) - from control_loop */

    /* Band-edge filter taps (upper and lower) */
    float taps_lower_r[FLL_BAND_EDGE_MAX_TAPS];
    float taps_lower_i[FLL_BAND_EDGE_MAX_TAPS];
    float taps_upper_r[FLL_BAND_EDGE_MAX_TAPS];
    float taps_upper_i[FLL_BAND_EDGE_MAX_TAPS];
    int n_taps;

    /* Filter delay line, mirrored once to make reverse history reads wrap-free. */
    float delay_r[FLL_BAND_EDGE_MAX_TAPS * 2];
    float delay_i[FLL_BAND_EDGE_MAX_TAPS * 2];
    int delay_idx;

    int sps;             /* Samples per symbol (for reinit detection) */
    float rolloff;       /* Band-edge rolloff the taps were designed for (for reinit detection) */
    float loop_bw_scale; /* Loop-bandwidth scale the gains were set for (for reinit detection) */
    int initialized;

    /* TETRA tracking mixer ahead of the channel filter (dsd_tetra_premix_block()). The carrier estimate is the sum
     * pre_freq + freq (dsd_fll_band_edge_total_freq()); freq alone is the FLL's residual, which keeps its own
     * +-max_freq clamp. pre_freq is 0 off the TETRA chain. (pre_re, pre_im) is the mixer's running rotation, (1, 0)
     * after a reset; a zeroed phasor reads as (1, 0). */
    float pre_freq; /* Mixer frequency (rad/sample) */
    float pre_re;   /* Mixer phasor, real part */
    float pre_im;   /* Mixer phasor, imaginary part */

    /* The TETRA chain's two loop gears (dsd_fll_band_edge_tetra_boundary()), read and written only at its transfer
     * sub-block boundaries: the design's wide loop while acquiring, the narrow tracking loop once locked. Every design
     * and every initialization starts them over, acquiring. */
    int tetra_track;       /* the tracking gear: 0 acquiring, 1 to 4 the loop bandwidth halved that many times */
    int tetra_have_ref;    /* tetra_ref_total holds the estimate at the last boundary */
    float tetra_ref_total; /* pre_freq + freq at the last boundary */
    float tetra_err[DSD_TETRA_FLL_LOCK_WINDOW]; /* each recent sub-block's band-edge error sum, per sample per symbol */
    int tetra_err_idx;                          /* next slot of tetra_err */
    int tetra_err_count;                        /* filled slots of tetra_err */
    int tetra_lock_run; /* boundaries in a row the acquiring loop met the lock test, or since the last step down */
} dsd_fll_band_edge_state_t;

/** @brief Bound on the TETRA mixer frequency, in Hz either side of the tuned centre. */
#define DSD_TETRA_PREMIX_LIMIT_HZ 8000

/**
 * @brief Per-chain band-edge FLL design parameters.
 *
 * The filter size is 2*sps+1 and the loop bandwidth loop_bw_scale * 2*pi/sps/350 on every chain.
 */
typedef struct {
    float rolloff;       /* Band-edge excess bandwidth of the transmitted pulse */
    float loop_bw_scale; /* Multiplier on OP25's loop bandwidth 2*pi/sps/350 */
} dsd_fll_band_edge_params;

/* OP25-compatible defaults for CQPSK carrier recovery.
 *
 * OP25 uses loop_bw=0.008, damping=sqrt(2)/2, computed alpha/beta:
 *   denom = 1.0 + 2.0 * damping * loop_bw + loop_bw * loop_bw
 *   alpha = (4 * damping * loop_bw) / denom  ≈ 0.0223
 *   beta  = (4 * loop_bw * loop_bw) / denom  ≈ 0.000253
 *
 * From p25_demodulator_dev.py:
 *   costas_alpha = 0.008 (this is loop_bw, NOT alpha)
 *   costas = op25_repeater.costas_loop_cc(costas_alpha, 4, TWO_PI/4)
 *
 * Frequency limits: ±1.0 rad/sample
 * Phase limits: ±π/2 (clamped, not wrapped)
 */

/** @brief OP25 Costas loop bandwidth. */
static inline float
dsd_neo_costas_default_loop_bw_op25(void) {
    return 0.008f;
}

/** @brief Default Costas loop alpha (phase gain) - computed from OP25 loop_bw/damping. */
static inline float
dsd_neo_costas_default_alpha(void) {
    /* loop_bw=0.008, damping=sqrt(2)/2
       denom = 1.0 + 2.0 * 0.7071 * 0.008 + 0.008^2 ≈ 1.01137
       alpha = (4 * 0.7071 * 0.008) / 1.01137 ≈ 0.0223 */
    return 0.0223f;
}

/** @brief Default Costas loop beta (frequency gain) - computed from OP25 loop_bw/damping. */
static inline float
dsd_neo_costas_default_beta(void) {
    /* beta = (4 * 0.008^2) / 1.01137 ≈ 0.000253 */
    return 0.000253f;
}

/** @brief Default max frequency (rad/sample) - OP25: ±1.0 rad/sample. */
static inline float
dsd_neo_costas_default_max_freq(void) {
    return 1.0f;
}

/** @brief Default Costas loop bandwidth for non-CQPSK modes. */
static inline float
dsd_neo_costas_default_loop_bw(void) {
    return 6.28318530717958647692f / 100.0f;
}

/** @brief Default Costas loop damping factor (sqrt(2)/2). */
static inline float
dsd_neo_costas_default_damping(void) {
    return 0.70710678118654752440f; /* sqrt(2)/2 */
}

/**
 * @brief OP25-compatible Gardner timing recovery block.
 *
 * Direct port of OP25's gardner_cc_impl::general_work() from:
 *   op25/gr-op25_repeater/lib/gardner_cc_impl.cc
 *
 * This is PURE timing recovery - NO carrier tracking, NO NCO rotation.
 * The carrier is tracked separately by the downstream Costas loop.
 *
 * Signal flow:
 *   Input: AGC'd complex samples at sample rate
 *   Processing:
 *     1. Push samples to circular delay line
 *     2. When mu accumulates past 1.0, interpolate symbol and mid-symbol
 *     3. Compute Gardner error: (last - current) * mid
 *     4. Update omega and mu
 *     5. Update lock detector (Yair Linn method)
 *   Output: Symbol-rate complex samples (timing corrected, NOT carrier corrected)
 *
 * Key OP25 parameters (from p25_demodulator_dev.py and gardner_cc_impl.cc):
 *   - gain_mu = 0.025
 *   - gain_omega = 0.1 * gain_mu^2 = 0.0000625
 *   - omega_rel = 0.002 absolute samples/symbol clamp
 *
 * @param d Demodulator state. Input: lowpassed (sample-rate IQ after AGC).
 *          Output: lowpassed (symbol-rate samples).
 */
void op25_gardner_cc(struct demod_state* d);

/**
 * @brief External differential phasor decoder.
 *
 * Computes y[n] = x[n] * conj(x[n-1]) to produce differential phase output.
 *
 * From OP25's p25_demodulator_dev.py line 408:
 *   self.diffdec = digital.diff_phasor_cc()
 *
 * This is applied AFTER Gardner timing recovery, producing raw differential
 * phase symbols for the Costas loop. Costas handles magnitude normalization and
 * reliability weighting before updating the carrier loop.
 *
 * @param d Demodulator state. Modifies lowpassed in-place to differential phasors.
 */
void op25_diff_phasor_cc(struct demod_state* d);

/**
 * @brief OP25-compatible Costas loop at symbol rate.
 *
 * Direct port of OP25's costas_loop_cc_impl::work() from:
 *   op25/gr-op25_repeater/lib/costas_loop_cc_impl.cc
 *
 * This operates on DIFFERENTIALLY DECODED symbols (after diff_phasor_cc).
 * The phase detector expects OP25 differential QPSK symbols at diagonal
 * positions. Reliable phasors are normalized before phase detection, and raw
 * phasor magnitude is used as a confidence weight so fades do not fully train
 * the loop.
 *
 * Signal flow:
 *   Input: Symbol-rate differential phasors from diff_phasor_cc
 *   Processing:
 *     1. NCO rotation: out = in * exp(-j*phase)
 *     2. Phase error detection (QPSK detector)
 *     3. Loop filter update (PI controller)
 *     4. Phase limiting to ±π/2
 *   Output: Carrier-corrected differential phasors
 *
 * Key OP25 parameters (from p25_demodulator_dev.py and costas_loop_cc_impl.cc):
 *   - loop_bw = 0.008 (called "costas_alpha" in p25_demodulator_dev.py)
 *   - damping = sqrt(2)/2 (critically damped)
 *   - max_phase = π/2
 *   - Computed: alpha ≈ 0.0223, beta ≈ 0.000253
 *
 * @param d Demodulator state. Modifies lowpassed in-place with carrier correction.
 */
void op25_costas_loop_cc(struct demod_state* d);

/**
 * @brief The samples per symbol the CQPSK timing chain runs at.
 *
 * The one rule the Gardner TED, the band-edge FLL and the TETRA chain's transitions share: demod_state::ted_sps, or 5
 * (P25 Phase 1 at 24 kHz) before one is set. A TED override reaches it only when a rate refresh applies it.
 *
 * @param d Demodulator state; NULL reads as unset.
 * @return ted_sps when above 0, else 5.
 */
int dsd_cqpsk_timing_sps(const struct demod_state* d);

/**
 * @brief Initialize FLL band-edge filter for a given samples-per-symbol, with the P25 parameters.
 *
 * Designs the band-edge filters and sets loop parameters with dsd_fll_band_edge_params_for(0): OP25's rolloff 0.2 and
 * loop bandwidth 2*pi/sps/350, never the TETRA chain's (dsd_fll_band_edge_init_params()). Call this during
 * cold start or retune initialization to ensure the FLL is ready before
 * processing samples. This avoids the lazy initialization that can cause
 * poor acquisition on the first few sample blocks.
 *
 * @param f   FLL state to initialize.
 * @param sps Samples per symbol (e.g., 5 for P25p1, 4 for P25p2).
 */
void dsd_fll_band_edge_init(dsd_fll_band_edge_state_t* f, int sps);

/**
 * @brief Band-edge FLL parameters for a receive chain.
 *
 * @param tetra_chain demod_state::tetra_chain: nonzero for the TETRA receive chain.
 * @return {0.35, 4} for the TETRA chain (its pulse's rolloff; a 4x loop bandwidth, since the band edges carry less
 *         energy behind the TETRA matched filter), {0.2, 1} (OP25's P25 values) for every other chain.
 */
dsd_fll_band_edge_params dsd_fll_band_edge_params_for(int tetra_chain);

/**
 * @brief Initialize the band-edge FLL for a samples-per-symbol and design parameters.
 *
 * Designs the filters and loop gains on first init, or when sps, rolloff or loop-bandwidth scale differs from the
 * design the state holds; otherwise keeps them. Either way the phase and delay line are reset and the frequency
 * estimate is kept as the caller seeded it. dsd_fll_band_edge_init() is this with dsd_fll_band_edge_params_for(0).
 * Parameters outside rolloff (0, 1] and scale > 0 leave the state untouched.
 *
 * @param f   FLL state to initialize.
 * @param sps Samples per symbol.
 * @param p   Design parameters, normally dsd_fll_band_edge_params_for(demod_state::tetra_chain).
 */
void dsd_fll_band_edge_init_params(dsd_fll_band_edge_state_t* f, int sps, dsd_fll_band_edge_params p);

/**
 * @brief The combined carrier-frequency estimate: the TETRA mixer's frequency plus the FLL's residual.
 *
 * Pure: it reads the state and changes nothing. Every reader that means the receiver's carrier offset (metrics, the
 * retune seed cache, logs) uses this; only the FLL's own loop and its saturation test read freq alone. Off the TETRA
 * chain pre_freq is 0 and this is freq exactly.
 *
 * @param f FLL state (NULL reads 0).
 * @return pre_freq + freq, rad/sample.
 */
float dsd_fll_band_edge_total_freq(const dsd_fll_band_edge_state_t* f);

/**
 * @brief Set the combined estimate, split between the mixer and the FLL.
 *
 * pre_freq = clamp(total, +-pre_limit), then freq = clamp(total - pre_freq, min_freq, max_freq): whatever exceeds
 * both bounds is dropped. A pre_limit of 0 (the TETRA chain inactive) puts the whole value in freq. A non-finite
 * total clears both. The phasor is left as it is.
 *
 * @param f         FLL state.
 * @param total     Combined estimate, rad/sample.
 * @param pre_limit Mixer bound, rad/sample (dsd_tetra_pre_limit_rad()), or 0.
 */
void dsd_fll_band_edge_split_total(dsd_fll_band_edge_state_t* f, float total, float pre_limit);

/**
 * @brief Fold the mixer back into the FLL: freq = clamp(freq + pre_freq, min_freq, max_freq), pre_freq = 0, and the
 * phasor to (1, 0). Used when the TETRA chain stops.
 *
 * @param f FLL state.
 */
void dsd_fll_band_edge_fold(dsd_fll_band_edge_state_t* f);

/**
 * @brief Move the FLL's residual into the mixer, up to its bound, keeping the combined estimate.
 *
 * want = clamp(pre_freq + freq, +-pre_limit); freq -= want - pre_freq; pre_freq = want. Once per block after the
 * FLL. What exceeds the bound stays in freq.
 *
 * @param f         FLL state.
 * @param pre_limit Mixer bound, rad/sample (dsd_tetra_pre_limit_rad()).
 */
void dsd_fll_band_edge_transfer(dsd_fll_band_edge_state_t* f, float pre_limit);

/**
 * @brief The TETRA chain's FLL gear, decided at a transfer sub-block boundary.
 *
 * The step the combined estimate took over the sub-block, divided by the loop's frequency gain (beta) and by the
 * sps, is that sub-block's band-edge error sum per symbol, whatever gear ran it. Over the last
 * DSD_TETRA_FLL_LOCK_WINDOW sub-blocks the acquiring (wide) loop locks once the summed magnitude stays below 8 for 8
 * boundaries in a row, and the loop then narrows an octave every 4 sub-blocks down to a quarter of OP25's bandwidth;
 * at any tracking gear, a sum above 15 drops it back to acquiring. Locked carriers measured at most 4 to 7 at 4x and
 * at 1x OP25's bandwidth, pull-ins of a few kHz 13 and up. Only the loop gains change: the filters, delay line, phase
 * and frequency carry on. The first boundary after a design or an initialization only takes its reference.
 *
 * @param f FLL state (designed; anything else is left alone).
 */
void dsd_fll_band_edge_tetra_boundary(dsd_fll_band_edge_state_t* f);

/**
 * @brief Rotate a block by the TETRA mixer: x[n] * p, with the phasor p advanced by exp(+j * pre_freq) per sample.
 *
 * The sign is the FLL NCO's: like freq, pre_freq is minus the carrier offset, so a carrier at -pre_freq comes out at
 * DC and pre_freq + freq is one estimate.
 *
 * Two complex multiplies per sample and no per-sample sin/cos; the phasor's magnitude is renormalized once per
 * block, so the rotation is phase continuous across any partition of the stream into blocks.
 *
 * @param f     FLL state holding pre_freq and the phasor.
 * @param iq    Interleaved I/Q, rotated in place.
 * @param pairs Number of complex samples.
 */
void dsd_tetra_premix_block(dsd_fll_band_edge_state_t* f, float* iq, int pairs);

/**
 * @brief Advance the TETRA mixer's phasor over @p pairs samples at pre_freq, as dsd_tetra_premix_block() would, without
 * rotating any: the phasor ends rotated by pairs * pre_freq from where it was (renormalized).
 *
 * @param f     FLL state holding pre_freq and the phasor.
 * @param pairs Number of complex samples.
 */
void dsd_tetra_premix_skip(dsd_fll_band_edge_state_t* f, int pairs);

/**
 * @brief The TETRA mixer bound at a DSP rate: 2*pi*DSD_TETRA_PREMIX_LIMIT_HZ/rate_out rad/sample, 0 for a rate that
 * is not positive.
 */
float dsd_tetra_pre_limit_rad(int rate_out);

/**
 * @brief OP25-compatible FLL band-edge frequency lock loop.
 *
 * Direct port of GNU Radio's digital.fll_band_edge_cc as used in OP25:
 *   self.fll = digital.fll_band_edge_cc(sps, excess_bw, 2*sps+1, TWO_PI/sps/350)
 *
 * This FLL uses band-edge filters to detect and correct frequency offset
 * before timing recovery. The error signal is derived from the difference
 * in power between upper and lower band-edge filter outputs.
 *
 * Signal flow:
 *   Input: AGC'd complex samples at sample rate
 *   Processing:
 *     1. NCO rotation: out = in * exp(+j*phase); freq converges to minus the carrier offset
 *     2. Band-edge filtering (upper and lower)
 *     3. Error computation: |x_lower|^2 - |x_upper|^2
 *     4. Loop filter update (frequency only, no phase term)
 *   Output: Frequency-corrected complex samples
 *
 * Key OP25 parameters (from p25_demodulator_dev.py line 403):
 *   - sps = samples per symbol (5 for P25p1, 4 for P25p2)
 *   - excess_bw = 0.2 (rolloff factor)
 *   - filter_size = 2*sps+1
 *   - loop_bw = TWO_PI/sps/350
 *
 * @param d Demodulator state. Modifies lowpassed in-place with frequency correction.
 */
void op25_fll_band_edge_cc(struct demod_state* d);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_COSTAS_H_ */
