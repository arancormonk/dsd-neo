// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief CTCSS detector over the decimated sub-audible stream (issue #522).
 *
 * One complex correlator per supported tone, each with a phasor that runs continuously at its
 * table frequency. Every 50 ms sub-block closes into a ring of five (a 250 ms window, one hop
 * per sub-block). A hop, for every bin:
 *
 *   1. estimates the offset from the bin from the slope of the sub-block phases -- a coarse
 *      pulse-pair estimate, refined by a magnitude-weighted least-squares fit -- which is what
 *      separates 67.0 from 69.3 Hz inside 250 ms where plain Goertzel bins cannot,
 *   2. snaps the fine estimate to the table within +/-0.8 Hz (150.0 Hz is 1.4 Hz from 151.4
 *      and so never snaps), and
 *   3. measures rho, the share of the sub-audible band energy the tone explains, with the
 *      window made coherent at the fine estimate,
 *
 * and keeps the best qualifying bin. Qualifying means rho >= 0.35, a fine estimate within
 * 0.5 Hz of the table tone it snapped to and within 5 Hz of its bin (50 ms sub-blocks alias
 * beyond 10 Hz), a tone carrying at least 1e-5 (-50 dB) of the raw input's full-band power
 * (more than decimation can fold into the band from a voice-band tone), a phase fit that is
 * actually linear against the noise the band carries (reduced chi-square), and -- tested
 * last, on the winner only -- no harmonics phase-locked to it, which is what a voice
 * fundamental has and a tone has not.
 *
 * Hysteresis, in level and in frequency: a tone locks after two consecutive hops qualify it
 * with estimates within 0.5 Hz of each other and of the table value; it holds while its own
 * bin's estimate stays within the 0.8 Hz snap gate and the newest 100 ms still carry
 * rho >= 0.15 at the locked frequency (and the same -50 dB of the full band), and is lost
 * after four failing hops or at once on a reverse burst (the transmitter's end-of-message
 * phase flip). The frequency check on every held hop is what keeps an off-table tone that one
 * noisy pair of hops snapped to a neighbour from being reported as that neighbour for as long
 * as it lasts. Everything is measured in samples, so the bounds hold in sample time at any
 * input rate.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include "analog_rx_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Hysteresis and qualification thresholds. Ratios only: nothing here depends on scale. */
static const double k_acquire_rho = 0.35;
static const double k_hold_rho = 0.15;
/* The snap gate: a locked tone holds while its own bin's fine estimate stays this close to the
   table value. */
static const double k_snap_hz = 0.8;
/* Frequency hysteresis: a tone only locks from estimates this close to the table value. At
   0 dB in-band the 250 ms estimate scatters by about 0.19 Hz (RMS), so an off-table tone such
   as 68.2 Hz, 1.1 Hz from 69.3, reaches the 0.8 Hz snap gate on several percent of hops but
   this one on well under one in a thousand, while a table tone misses it on about 1%. */
static const double k_acquire_snap_hz = 0.5;
/* Weighted RMS phase residual (radians) above which the window is not one steady tone,
   whatever the noise: a tone at 0 dB in-band SNR fits to about 0.15 rad. */
static const double k_max_residual_rad = 0.5;
/* Reduced chi-square of the phase fit above which the phase wanders more than the noise in
   the band explains (see ctcss_fit_chi2). A tone in noise scores about 1; a voice
   fundamental that dominates the band while its pitch drifts, jitters or wobbles scores
   far higher. */
static const double k_max_chi2 = 6.0;
/* Floor on the per-sub-block phase variance (0.05 rad squared): a clean tone still shows a
   few hundredths of a radian from its own negative-frequency image in 50 ms sub-blocks. */
static const double k_min_phase_var = 0.0025;
/* A sub-block with no tone in it has a uniformly random phase: variance pi^2 / 3. */
static const double k_max_phase_var = M_PI * M_PI / 3.0;
/* Largest offset between a fine estimate and the bin it came from that is not an alias. */
static const double k_max_bin_offset_hz = 5.0;
/* Consecutive hops share 200 of their 250 ms, so a steady tone's two estimates agree well
   inside half a hertz even at 0 dB; a pitch that is still moving does not. */
static const double k_stable_hz = 0.5;
/* The least share of the raw input's full-band power a tone must carry. Decimation folds a
   residue of everything above the band into it, and a steady voice-band tone with nothing else
   below 290 Hz -- a 2300 Hz test tone, say, which lands on 100 Hz at a 2400 Hz decimated rate --
   would otherwise read as a pure sub-audible tone, however faint that residue. Stage 1 leaves
   such a residue at least 58 dB below its source at 4.8-7.2 kHz input rates, 67 dB from
   7.2 kHz, 69 dB from 9.6 kHz and 74 dB from 20 kHz up, so no voice-band component reaches
   -50 dB. A real tone sits far above it: in white noise filling a 78 kHz input at 0 dB in-band
   tone-to-noise it carries about -21 dB of the full band, and no tone DSP_ANALOG_CTCSS locks,
   under speech included, reads below -26 dB. */
static const double k_min_full_share = 1e-5;
/* A voice fundamental comes with harmonics phase-locked to it; a CTCSS tone is one sinusoid
   (encoders stay under a few percent distortion). Phase-locked second and third harmonics
   together within 11 dB of the candidate make it a voice (see ctcss_harmonic_lock). */
static const double k_max_harmonic_ratio = 0.08;
/* A sub-block this dominated by the tone is trusted as a phase reference for the reverse
   burst check, and a jump of more than 100 degrees against it is a burst (120 and 180 degree
   variants are both in use). */
static const double k_burst_sub_rho = 0.35;
static const double k_burst_rad = 1.745329;

/** @brief Hops a reverse burst blocks re-acquisition for: one full window. */
enum { CTCSS_BURST_HOLDOFF_HOPS = DSD_ANALOG_CTCSS_WINDOW };

typedef struct {
    double re;
    double im;
} ctcss_cpx;

static ctcss_cpx
ctcss_ring_at(const dsd_analog_ctcss* det, int j, int bin) {
    /* j = 0 is the oldest sub-block in the window, j = WINDOW - 1 the newest. */
    const int slot = (det->ring_head + j) % DSD_ANALOG_CTCSS_WINDOW;
    ctcss_cpx c = {det->ring_re[slot][bin], det->ring_im[slot][bin]};
    return c;
}

static double
ctcss_ring_energy_at(const dsd_analog_ctcss* det, int j) {
    return det->ring_energy[(det->ring_head + j) % DSD_ANALOG_CTCSS_WINDOW];
}

static ctcss_cpx
ctcss_rotate(ctcss_cpx c, double angle) {
    const double cs = cos(angle);
    const double sn = sin(angle);
    ctcss_cpx out = {(c.re * cs) - (c.im * sn), (c.re * sn) + (c.im * cs)};
    return out;
}

static double
ctcss_mag2(ctcss_cpx c) {
    return (c.re * c.re) + (c.im * c.im);
}

static ctcss_cpx
ctcss_mul(ctcss_cpx x, ctcss_cpx y) {
    ctcss_cpx out = {(x.re * y.re) - (x.im * y.im), (x.re * y.im) + (x.im * y.re)};
    return out;
}

static double
ctcss_tone_hz(int index) {
    return (double)dsd_ctcss_tone_tenths(index) / 10.0;
}

static void
ctcss_reset(void* ctx) {
    dsd_analog_ctcss* det = (dsd_analog_ctcss*)ctx;
    if (!det) {
        return;
    }
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        det->osc_re[k] = 1.0;
        det->osc_im[k] = 0.0;
        det->acc_re[k] = 0.0;
        det->acc_im[k] = 0.0;
    }
    DSD_MEMSET(det->ring_re, 0, sizeof(det->ring_re));
    DSD_MEMSET(det->ring_im, 0, sizeof(det->ring_im));
    DSD_MEMSET(det->ring_energy, 0, sizeof(det->ring_energy));
    DSD_MEMSET(det->ring_full, 0, sizeof(det->ring_full));
    DSD_MEMSET(&det->last_hop, 0, sizeof(det->last_hop));
    det->ring_head = 0;
    det->ring_count = 0;
    det->sub_fill = 0;
    det->sub_energy = 0.0;
    det->sub_full = 0.0;
    det->state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    det->locked = -1;
    det->locked_hz = 0.0;
    det->burst_ref_hz = 0.0;
    det->cand = -1;
    det->cand_run = 0;
    det->fail_run = 0;
    det->holdoff = 0;
    det->open_samples = 0;
    DSD_MEMSET(det->wide, 0, sizeof(det->wide));
    det->wide_pos = 0;
    det->last_hop.best_index = -1;
    det->last_hop.snapped = -1;
}

static void
ctcss_configure(void* ctx, double rate_hz) {
    dsd_analog_ctcss* det = (dsd_analog_ctcss*)ctx;
    if (!det) {
        return;
    }
    det->rate_hz = rate_hz;
    det->sub_len = (int)lround(rate_hz * (double)DSD_ANALOG_CTCSS_SUBBLOCK_MS / 1000.0);
    if (det->sub_len < 1) {
        det->sub_len = 1;
    }
    det->wide_len = DSD_ANALOG_CTCSS_WINDOW * det->sub_len;
    if (det->wide_len > DSD_ANALOG_CTCSS_WIDE_MAX) {
        det->wide_len = DSD_ANALOG_CTCSS_WIDE_MAX;
    }
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        const double w = 2.0 * M_PI * ctcss_tone_hz(k) / rate_hz;
        det->step_re[k] = cos(w);
        det->step_im[k] = -sin(w);
    }
    ctcss_reset(det);
}

/** @brief Nearest table tone within the snap gate, or -1. */
static int
ctcss_snap(double hz) {
    int best = -1;
    double best_err = k_snap_hz;
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        const double err = fabs(hz - ctcss_tone_hz(k));
        if (err <= best_err) {
            best_err = err;
            best = k;
        }
    }
    return best;
}

/** @brief Coarse per-sub-block phase advance: the magnitude-weighted pulse-pair estimate. */
static double
ctcss_coarse_advance(const dsd_analog_ctcss* det, int bin) {
    double re = 0.0;
    double im = 0.0;
    for (int j = 1; j < DSD_ANALOG_CTCSS_WINDOW; j++) {
        const ctcss_cpx a = ctcss_ring_at(det, j - 1, bin);
        const ctcss_cpx b = ctcss_ring_at(det, j, bin);
        re += (b.re * a.re) + (b.im * a.im);
        im += (b.im * a.re) - (b.re * a.im);
    }
    return atan2(im, re);
}

/** @brief A linear phase fit across the window: the offset and how well a line explains it. */
typedef struct {
    double advance;                      /**< per-sub-block phase advance, radians */
    double residual;                     /**< magnitude-weighted RMS residual, radians */
    double err[DSD_ANALOG_CTCSS_WINDOW]; /**< per-sub-block residual, radians */
} ctcss_fit;

/**
 * @brief Refine the coarse advance by weighted least squares on the de-rotated phases.
 *
 * After de-rotation by the coarse advance the sub-block phases sit near one value, so they
 * need no unwrapping; the fit's slope is the correction and its weighted RMS residual says
 * whether the window is one steady tone at all.
 */
static void
ctcss_refine_advance(const dsd_analog_ctcss* det, int bin, double coarse, ctcss_fit* fit) {
    ctcss_cpx d[DSD_ANALOG_CTCSS_WINDOW];
    ctcss_cpx sum = {0.0, 0.0};
    for (int j = 0; j < DSD_ANALOG_CTCSS_WINDOW; j++) {
        d[j] = ctcss_rotate(ctcss_ring_at(det, j, bin), -coarse * (double)j);
        sum.re += d[j].re;
        sum.im += d[j].im;
    }
    const double ref = atan2(sum.im, sum.re);
    double r[DSD_ANALOG_CTCSS_WINDOW];
    double w[DSD_ANALOG_CTCSS_WINDOW];
    double sw = 0.0;
    double swj = 0.0;
    double swr = 0.0;
    for (int j = 0; j < DSD_ANALOG_CTCSS_WINDOW; j++) {
        const double phase = atan2(d[j].im, d[j].re) - ref;
        r[j] = atan2(sin(phase), cos(phase));
        w[j] = ctcss_mag2(d[j]);
        sw += w[j];
        swj += w[j] * (double)j;
        swr += w[j] * r[j];
    }
    fit->advance = coarse;
    fit->residual = M_PI;
    if (!(sw > 0.0)) {
        /* An empty bin has nothing to fit: every sub-block counts as fully unexplained. */
        for (int j = 0; j < DSD_ANALOG_CTCSS_WINDOW; j++) {
            fit->err[j] = M_PI;
        }
        return;
    }
    const double jm = swj / sw;
    const double rm = swr / sw;
    double sxy = 0.0;
    double sxx = 0.0;
    for (int j = 0; j < DSD_ANALOG_CTCSS_WINDOW; j++) {
        sxy += w[j] * ((double)j - jm) * (r[j] - rm);
        sxx += w[j] * ((double)j - jm) * ((double)j - jm);
    }
    const double slope = (sxx > 0.0) ? sxy / sxx : 0.0;
    double se = 0.0;
    for (int j = 0; j < DSD_ANALOG_CTCSS_WINDOW; j++) {
        fit->err[j] = r[j] - (rm + (slope * ((double)j - jm)));
        se += w[j] * fit->err[j] * fit->err[j];
    }
    fit->residual = sqrt(se / sw);
    fit->advance = coarse + slope;
}

/**
 * @brief Goodness of the linear fit against the phase noise each sub-block should carry.
 *
 * What the tone does not explain in a sub-block is treated as noise across the sub-audible
 * band; its share in the tone's correlator bin sets how far that sub-block's phase may
 * wander, from 1 / (2 SNR) for a strong tone up to a uniformly random phase for none. A tone
 * in noise scores about 1 (a reduced chi-square with three degrees of freedom), whatever the
 * SNR. A voice fundamental that dominates the band but drifts, jitters or wobbles in pitch
 * scores far higher: its phase wanders more than the little energy it leaves unexplained can
 * account for.
 */
static double
ctcss_fit_chi2(const dsd_analog_ctcss* det, int bin, const ctcss_fit* fit) {
    /* The noise is assumed to fill the sub-audible band up to the front end's cutoff. */
    const double noise_bin_gain = det->rate_hz / (2.0 * DSD_ANALOG_RX_BAND_HZ);
    double chi2 = 0.0;
    for (int j = 0; j < DSD_ANALOG_CTCSS_WINDOW; j++) {
        const double bin_power = ctcss_mag2(ctcss_ring_at(det, j, bin));
        const double tone_energy = 2.0 * bin_power / (double)det->sub_len;
        double noise_energy = ctcss_ring_energy_at(det, j) - tone_energy;
        if (!(noise_energy > 0.0)) {
            noise_energy = 0.0;
        }
        /* Noise alone puts about noise_energy * gain into the bin, so the tone's own SNR there
           is what the bin holds beyond that. The phase variance runs from 1 / (2 SNR) for a
           strong tone to pi^2 / 3 -- a uniformly random phase -- for a sub-block with none. */
        const double noise_in_bin = noise_energy * noise_bin_gain;
        double snr = 1e9;
        if (noise_in_bin > 0.0) {
            snr = (bin_power / noise_in_bin) - 1.0;
        }
        if (snr < 0.0) {
            snr = 0.0;
        }
        double var = k_max_phase_var / (1.0 + (2.0 * k_max_phase_var * snr));
        if (var < k_min_phase_var) {
            var = k_min_phase_var;
        }
        chi2 += fit->err[j] * fit->err[j] / var;
    }
    return chi2 / (double)(DSD_ANALOG_CTCSS_WINDOW - 2);
}

/**
 * @brief rho over sub-blocks [first, first + count) at @p bin, made coherent at @p advance.
 *
 * 2|sum|^2 / (N * energy): a pure tone reads 1, a tone at 0 dB in-band SNR about 0.5.
 */
static double
ctcss_rho(const dsd_analog_ctcss* det, int bin, double advance, int first, int count) {
    ctcss_cpx sum = {0.0, 0.0};
    double energy = 0.0;
    for (int j = first; j < first + count; j++) {
        const ctcss_cpx c = ctcss_rotate(ctcss_ring_at(det, j, bin), -advance * (double)j);
        sum.re += c.re;
        sum.im += c.im;
        energy += ctcss_ring_energy_at(det, j);
    }
    const double denom = (double)det->sub_len * (double)count * energy;
    if (!(denom > 0.0)) {
        return 0.0;
    }
    return 2.0 * ctcss_mag2(sum) / denom;
}

/**
 * @brief The share of the raw input's full-band power that @p rho of the band over sub-blocks
 * [first, first + count) stands for: rho times the band energy, over the full stream's energy.
 */
static double
ctcss_full_share(const dsd_analog_ctcss* det, double rho, int first, int count) {
    double band = 0.0;
    double full = 0.0;
    for (int j = first; j < first + count; j++) {
        band += ctcss_ring_energy_at(det, j);
        full += det->ring_full[(det->ring_head + j) % DSD_ANALOG_CTCSS_WINDOW];
    }
    if (!(full > 0.0)) {
        return 0.0;
    }
    return rho * band / full;
}

/** @brief Per-sub-block phase advance of a tone at @p hz relative to @p bin. */
static double
ctcss_advance_for(const dsd_analog_ctcss* det, int bin, double hz) {
    return 2.0 * M_PI * (hz - ctcss_tone_hz(bin)) * (double)det->sub_len / det->rate_hz;
}

/** @brief Fine estimate, fit and rho for one correlator bin. */
static void
ctcss_measure_bin(const dsd_analog_ctcss* det, int bin, dsd_analog_ctcss_hop* out) {
    ctcss_fit fit;
    ctcss_refine_advance(det, bin, ctcss_coarse_advance(det, bin), &fit);
    out->evaluated = 1;
    out->best_index = bin;
    out->residual = fit.residual;
    out->chi2 = ctcss_fit_chi2(det, bin, &fit);
    out->est_hz = ctcss_tone_hz(bin) + (fit.advance * det->rate_hz / (2.0 * M_PI * (double)det->sub_len));
    out->rho = ctcss_rho(det, bin, fit.advance, 0, DSD_ANALOG_CTCSS_WINDOW);
    out->share = ctcss_full_share(det, out->rho, 0, DSD_ANALOG_CTCSS_WINDOW);
    out->snapped = ctcss_snap(out->est_hz);
    out->recent_rho = 0.0;
    out->harmonic = 0.0;
}

static int
ctcss_hop_qualifies(const dsd_analog_ctcss_hop* hop) {
    /* A 50 ms sub-block only resolves offsets up to 10 Hz from its bin: a signal 10.7 Hz
       below a bin reads as 9.3 Hz above it. The table is dense enough that every supported
       tone is within 4.1 Hz of its nearest bin, so an estimate further than 5 Hz from the bin
       that produced it is an alias, never a tone. */
    return hop->snapped >= 0 && fabs(hop->est_hz - ctcss_tone_hz(hop->snapped)) <= k_acquire_snap_hz
           && fabs(hop->est_hz - ctcss_tone_hz(hop->best_index)) <= k_max_bin_offset_hz && hop->rho >= k_acquire_rho
           && hop->share >= k_min_full_share && hop->residual <= k_max_residual_rad && hop->chi2 <= k_max_chi2;
}

/**
 * @brief Measure every bin and keep the strongest candidate.
 *
 * Every bin rather than only the loudest one: under voice, energy the transmitter's voice
 * filter let through just below 300 Hz can outweigh a tone at the bottom of the table, and
 * the bin that sees the tone best is the one whose window the tone explains best. A bin
 * whose fine estimate snaps to the table and passes the fit wins over one that does not;
 * among equals the larger rho wins.
 */
static void
ctcss_measure(const dsd_analog_ctcss* det, dsd_analog_ctcss_hop* hop) {
    int have_qualified = 0;
    hop->rho = -1.0;
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        dsd_analog_ctcss_hop trial;
        ctcss_measure_bin(det, k, &trial);
        const int qualified = ctcss_hop_qualifies(&trial);
        if (qualified < have_qualified || (qualified == have_qualified && !(trial.rho > hop->rho))) {
            continue;
        }
        *hop = trial;
        have_qualified = qualified;
    }
}

/** @brief Single-sub-block rho at @p bin: how much the tone dominates that sub-block. */
static double
ctcss_sub_rho(const dsd_analog_ctcss* det, int bin, int j) {
    const double denom = (double)det->sub_len * ctcss_ring_energy_at(det, j);
    if (!(denom > 0.0)) {
        return 0.0;
    }
    return 2.0 * ctcss_mag2(ctcss_ring_at(det, j, bin)) / denom;
}

/** @brief Phase jump between sub-blocks @p a and @p b beyond what the locked offset explains. */
static int
ctcss_pair_jumped(const dsd_analog_ctcss* det, int bin, double advance, int a, int b) {
    if (ctcss_sub_rho(det, bin, a) < k_burst_sub_rho || ctcss_sub_rho(det, bin, b) < k_burst_sub_rho) {
        return 0;
    }
    const ctcss_cpx ca = ctcss_ring_at(det, a, bin);
    const ctcss_cpx cb = ctcss_rotate(ctcss_ring_at(det, b, bin), -advance * (double)(b - a));
    const double jump = atan2((cb.im * ca.re) - (cb.re * ca.im), (cb.re * ca.re) + (cb.im * ca.im));
    return fabs(jump) >= k_burst_rad;
}

/**
 * @brief The locked tone's phase flipped: a reverse burst, the end of the transmission.
 *
 * The newest sub-block is compared with the one before it (a flip on a sub-block boundary)
 * and with the one before that (a flip inside the previous sub-block, which then cancels
 * itself and cannot serve as a reference).
 *
 * The phase each pair should have advanced by comes from @p ref_hz, the locked frequency as it
 * stood two hops ago, never from a newer estimate. A flip inside a sub-block that stays strong
 * -- a 120 or 240 degree burst early or late in it -- puts part of the step into that
 * sub-block's phase, and a window that ends on it fits the step as a steeper slope. Measured
 * against that slope, the whole step across the next pair can read short of the 100 degree
 * threshold: on a clean tone about one such burst in fourteen was missed that way. The window
 * two hops back ends no later than the older sub-block of every pair compared here, so no step
 * after that sub-block can have bent it.
 */
static int
ctcss_reverse_burst(const dsd_analog_ctcss* det, double ref_hz) {
    const int bin = det->locked;
    const double advance = ctcss_advance_for(det, bin, ref_hz);
    const int newest = DSD_ANALOG_CTCSS_WINDOW - 1;
    return ctcss_pair_jumped(det, bin, advance, newest - 1, newest)
           || ctcss_pair_jumped(det, bin, advance, newest - 2, newest);
}

static void
ctcss_lock(dsd_analog_ctcss* det, int index, double hz) {
    det->state = DSD_ANALOG_TONE_STATE_LOCKED;
    det->locked = index;
    det->locked_hz = hz;
    det->burst_ref_hz = hz;
    det->fail_run = 0;
}

static void
ctcss_unlock(dsd_analog_ctcss* det) {
    det->state = DSD_ANALOG_TONE_STATE_NONE;
    det->locked = -1;
    det->locked_hz = 0.0;
    det->burst_ref_hz = 0.0;
    det->fail_run = 0;
}

/** @brief Most pieces the harmonic check splits the window into (about 25 ms each). */
enum { CTCSS_HARMONIC_PIECES = 12 };

/**
 * @brief Per-piece correlations of the wide stream at hz, 2hz and 3hz, phase-locked to each other.
 *
 * Each piece spans a whole number of the candidate's cycles, close to 25 ms, so the three
 * kernels are orthogonal over it: noise at f is independent of noise at 2f and 3f, and the
 * candidate's own energy does not leak into its harmonic bins. Pieces are taken from the
 * newest end of the window. Returns the number of pieces filled.
 */
static int
ctcss_wide_correlate(const dsd_analog_ctcss* det, double hz, ctcss_cpx a[CTCSS_HARMONIC_PIECES],
                     ctcss_cpx b[CTCSS_HARMONIC_PIECES], ctcss_cpx c[CTCSS_HARMONIC_PIECES]) {
    const double cycles = fmax(1.0, round(hz * 0.025));
    const int piece = (int)lround(cycles * det->rate_hz / hz);
    if (piece <= 0) {
        return 0;
    }
    int pieces = det->wide_len / piece;
    if (pieces > CTCSS_HARMONIC_PIECES) {
        pieces = CTCSS_HARMONIC_PIECES;
    }
    const int first = det->wide_len - (pieces * piece);
    const double w = 2.0 * M_PI * hz / det->rate_hz;
    const ctcss_cpx step = {cos(w), -sin(w)};
    ctcss_cpx p1 = {1.0, 0.0};
    for (int j = 0; j < pieces; j++) {
        a[j].re = a[j].im = b[j].re = b[j].im = c[j].re = c[j].im = 0.0;
        for (int n = first + (j * piece); n < first + ((j + 1) * piece); n++) {
            const double x = (double)det->wide[(det->wide_pos + n) % det->wide_len];
            const ctcss_cpx p2 = ctcss_mul(p1, p1);
            const ctcss_cpx p3 = ctcss_mul(p2, p1);
            a[j].re += x * p1.re;
            a[j].im += x * p1.im;
            b[j].re += x * p2.re;
            b[j].im += x * p2.im;
            c[j].re += x * p3.re;
            c[j].im += x * p3.im;
            p1 = ctcss_mul(p1, step);
        }
        /* Renormalise so the phasor's gain never drifts across the window. */
        const double mag = sqrt(ctcss_mag2(p1));
        p1.re /= mag;
        p1.im /= mag;
    }
    return pieces;
}

/** @brief Cross-piece sums for one harmonic. */
typedef struct {
    ctcss_cpx sum; /**< sum of the harmonic rotated onto the fundamental's phase */
    double self;   /**< sum of |t|^2: the part a lock-free harmonic contributes on its own */
} ctcss_coupling;

static void
ctcss_coupling_add(ctcss_coupling* acc, ctcss_cpx t) {
    acc->sum.re += t.re;
    acc->sum.im += t.im;
    acc->self += ctcss_mag2(t);
}

/**
 * @brief Voice test: harmonic power phase-locked to the candidate, relative to its own.
 *
 * A voice's second and third harmonics stay phase-locked to its fundamental however the pitch
 * wanders, so rotating each piece's harmonic by the fundamental's phase (twice or three times
 * over) lines them up across the window. Only cross terms between pieces are kept, so energy
 * that merely sits near 2f or 3f -- noise, or voice under a genuine tone -- averages to zero
 * instead of adding a floor that grows as the SNR falls. A voice reads its real harmonic
 * balance; a tone reads about zero.
 */
static double
ctcss_harmonic_lock(const dsd_analog_ctcss* det, double hz) {
    ctcss_cpx a[CTCSS_HARMONIC_PIECES];
    ctcss_cpx b[CTCSS_HARMONIC_PIECES];
    ctcss_cpx c[CTCSS_HARMONIC_PIECES];
    const int pieces = ctcss_wide_correlate(det, hz, a, b, c);
    ctcss_coupling h2 = {{0.0, 0.0}, 0.0};
    ctcss_coupling h3 = {{0.0, 0.0}, 0.0};
    double sum_mag = 0.0;
    double sum_pow = 0.0;
    for (int j = 0; j < pieces; j++) {
        const double mag = sqrt(ctcss_mag2(a[j]));
        if (!(mag > 0.0)) {
            continue;
        }
        const ctcss_cpx u = {a[j].re / mag, -a[j].im / mag};
        const ctcss_cpx u2 = ctcss_mul(u, u);
        ctcss_coupling_add(&h2, ctcss_mul(b[j], u2));
        ctcss_coupling_add(&h3, ctcss_mul(c[j], ctcss_mul(u2, u)));
        sum_mag += mag;
        sum_pow += mag * mag;
    }
    const double cross = (ctcss_mag2(h2.sum) - h2.self) + (ctcss_mag2(h3.sum) - h3.self);
    const double fundamental = (sum_mag * sum_mag) - sum_pow;
    return (fundamental > 0.0) ? cross / fundamental : 1.0;
}

static void
ctcss_track_candidate(dsd_analog_ctcss* det, dsd_analog_ctcss_hop* hop) {
    int qualified = det->holdoff == 0 && ctcss_hop_qualifies(hop);
    if (qualified) {
        /* Last, because it is the one test that costs a pass over the window. */
        hop->harmonic = ctcss_harmonic_lock(det, hop->est_hz);
        qualified = hop->harmonic < k_max_harmonic_ratio;
    }
    if (!qualified) {
        det->cand = -1;
        det->cand_run = 0;
        return;
    }
    const int stable = hop->snapped == det->cand && fabs(hop->est_hz - det->cand_hz) <= k_stable_hz;
    det->cand_run = stable ? det->cand_run + 1 : 1;
    det->cand = hop->snapped;
    det->cand_hz = hop->est_hz;
}

static void
ctcss_step_unlocked(dsd_analog_ctcss* det, const dsd_analog_ctcss_hop* hop) {
    if (det->cand >= 0 && det->cand_run >= DSD_ANALOG_CTCSS_ACQUIRE_HOPS) {
        ctcss_lock(det, det->cand, hop->est_hz);
        return;
    }
    const int64_t no_tone_samples = (int64_t)llround(det->rate_hz * (double)DSD_ANALOG_CTCSS_NO_TONE_MS / 1000.0);
    if (det->state == DSD_ANALOG_TONE_STATE_ACQUIRING && det->open_samples >= no_tone_samples) {
        det->state = DSD_ANALOG_TONE_STATE_NONE;
    }
}

static void
ctcss_step_locked(dsd_analog_ctcss* det, dsd_analog_ctcss_hop* hop) {
    /* The burst reference moves on one hop behind locked_hz: this hop checks against the
       frequency from two hops ago, and the next one against the frequency this hop starts from. */
    const double burst_ref_hz = det->burst_ref_hz;
    det->burst_ref_hz = det->locked_hz;
    if (ctcss_reverse_burst(det, burst_ref_hz)) {
        ctcss_unlock(det);
        det->holdoff = CTCSS_BURST_HOLDOFF_HOPS;
        det->cand = -1;
        det->cand_run = 0;
        return;
    }
    const int other = det->cand >= 0 && det->cand != det->locked;
    if (other && det->cand_run >= DSD_ANALOG_CTCSS_ACQUIRE_HOPS) {
        ctcss_lock(det, det->cand, hop->est_hz);
        return;
    }
    /* The locked bin's own estimate, every hop: once locked, a tone is only held while it still
       sits on the table value it locked to. Without this an off-table tone that locked on one
       noisy pair of hops would keep reporting its neighbour for as long as it stayed coherent. */
    dsd_analog_ctcss_hop own;
    ctcss_measure_bin(det, det->locked, &own);
    const int on_tone = fabs(own.est_hz - ctcss_tone_hz(det->locked)) <= k_snap_hz;
    const double advance = ctcss_advance_for(det, det->locked, det->locked_hz);
    hop->recent_rho = ctcss_rho(det, det->locked, advance, DSD_ANALOG_CTCSS_WINDOW - 2, 2);
    const int above_residue =
        ctcss_full_share(det, hop->recent_rho, DSD_ANALOG_CTCSS_WINDOW - 2, 2) >= k_min_full_share;
    if (on_tone && hop->recent_rho >= k_hold_rho && above_residue && !other) {
        det->fail_run = 0;
        det->locked_hz = own.est_hz;
        return;
    }
    if (++det->fail_run >= DSD_ANALOG_CTCSS_LOSE_HOPS) {
        ctcss_unlock(det);
    }
}

static void
ctcss_evaluate_hop(dsd_analog_ctcss* det, int freeze) {
    dsd_analog_ctcss_hop hop;
    DSD_MEMSET(&hop, 0, sizeof(hop));
    ctcss_measure(det, &hop);
    if (!freeze) {
        ctcss_track_candidate(det, &hop);
        if (det->state == DSD_ANALOG_TONE_STATE_LOCKED) {
            ctcss_step_locked(det, &hop);
        } else {
            ctcss_step_unlocked(det, &hop);
        }
        if (det->holdoff > 0) {
            det->holdoff--;
        }
    }
    det->last_hop = hop;
}

static void
ctcss_close_subblock(dsd_analog_ctcss* det, int freeze) {
    const int slot = det->ring_head;
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        det->ring_re[slot][k] = det->acc_re[k];
        det->ring_im[slot][k] = det->acc_im[k];
        det->acc_re[k] = 0.0;
        det->acc_im[k] = 0.0;
        /* Renormalise the phasors once per sub-block so rounding never grows their gain. */
        const double mag = sqrt((det->osc_re[k] * det->osc_re[k]) + (det->osc_im[k] * det->osc_im[k]));
        if (mag > 0.0) {
            det->osc_re[k] /= mag;
            det->osc_im[k] /= mag;
        }
    }
    det->ring_energy[slot] = det->sub_energy;
    det->sub_energy = 0.0;
    det->ring_full[slot] = det->sub_full;
    det->sub_full = 0.0;
    det->sub_fill = 0;
    det->ring_head = (det->ring_head + 1) % DSD_ANALOG_CTCSS_WINDOW;
    if (det->ring_count < DSD_ANALOG_CTCSS_WINDOW) {
        det->ring_count++;
    }
    if (det->ring_count == DSD_ANALOG_CTCSS_WINDOW) {
        ctcss_evaluate_hop(det, freeze);
    }
}

static void
ctcss_accumulate(dsd_analog_ctcss* det, double x) {
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        const double ore = det->osc_re[k];
        const double oim = det->osc_im[k];
        det->acc_re[k] += x * ore;
        det->acc_im[k] += x * oim;
        det->osc_re[k] = (ore * det->step_re[k]) - (oim * det->step_im[k]);
        det->osc_im[k] = (ore * det->step_im[k]) + (oim * det->step_re[k]);
    }
    det->sub_energy += x * x;
}

static void
ctcss_process(void* ctx, const float* band, const float* wide, const float* full, int count, int freeze) {
    dsd_analog_ctcss* det = (dsd_analog_ctcss*)ctx;
    if (!det || !band || count <= 0 || det->sub_len <= 0 || det->wide_len <= 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        ctcss_accumulate(det, (double)band[i]);
        /* Without a full stream the band stands in for it, and the share is rho itself. */
        det->sub_full += full ? (double)full[i] : (double)band[i] * (double)band[i];
        det->wide[det->wide_pos] = wide ? wide[i] : band[i];
        det->wide_pos = (det->wide_pos + 1) % det->wide_len;
        /* Counted per sample, before the hop it may close, so the no-tone verdict lands on the
           same hop however the input was cut into blocks. */
        if (!freeze) {
            det->open_samples++;
        }
        if (++det->sub_fill >= det->sub_len) {
            ctcss_close_subblock(det, freeze);
        }
    }
}

static void
ctcss_report(const void* ctx, dsd_analog_rx_report* out) {
    const dsd_analog_ctcss* det = (const dsd_analog_ctcss*)ctx;
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    out->kind = DSD_ANALOG_TONE_KIND_NONE;
    if (!det) {
        return;
    }
    out->state = det->state;
    if (det->state == DSD_ANALOG_TONE_STATE_LOCKED && det->locked >= 0) {
        out->kind = DSD_ANALOG_TONE_KIND_CTCSS;
        out->ctcss_tenths_hz = dsd_ctcss_tone_tenths(det->locked);
    }
}

const dsd_analog_rx_detector_ops dsd_analog_ctcss_ops = {
    "ctcss", ctcss_configure, ctcss_reset, ctcss_process, ctcss_report,
};

const dsd_analog_ctcss_hop*
dsd_analog_ctcss_last_hop(const dsd_analog_ctcss* det) {
    return det ? &det->last_hop : NULL;
}
