// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief CTCSS detector over the decimated sub-audible stream (issue #522).
 *
 * One complex correlator per supported tone, each with a phasor that runs continuously at its
 * table frequency. Every 50 ms sub-block closes into a ring of twelve; the newest five are the
 * 250 ms window, with one hop per sub-block. A hop, for every bin:
 *
 *   1. estimates the offset from the bin from the slope of the sub-block phases -- a coarse
 *      pulse-pair estimate, refined by a magnitude-weighted least-squares fit -- which is what
 *      separates 67.0 from 69.3 Hz inside 250 ms where plain Goertzel bins cannot,
 *   2. snaps the fine estimate to the table within the tone's gate: +/-0.8 Hz, or 0.5 % of the
 *      tone where that is more (from 160 Hz up), or half the distance to its nearest neighbour
 *      where that is less (150.0 and 151.4 Hz, 1.4 Hz apart, get 0.7 Hz each), so no estimate is
 *      ever within the gates of two tones, and
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
 * with estimates within 0.5 Hz of each other and of the table value -- three for 150.0 or
 * 151.4 Hz while the newest estimate leans more than 0.3 Hz toward the other (k_pair_lean_hz),
 * where noise most often carries an estimate of one into the other's gate; it holds while its own
 * bin's estimate stays within the tone's snap gate and the newest 100 ms still carry
 * rho >= 0.15 at the locked frequency (and the same -50 dB of the full band), and is lost
 * after four failing hops or at once on a reverse burst (the transmitter's end-of-message
 * phase flip). The frequency check on every held hop is what keeps an off-table tone that one
 * noisy pair of hops snapped to a neighbour from being reported as that neighbour for as long
 * as it lasts. Everything is measured in samples, so the bounds hold in sample time at any
 * input rate.
 *
 * Late acquisition bounds the lock time in noise. While no tone is locked every hop also measures
 * two longer windows from the ring, its newest 600 and 400 ms, over no more of them than has
 * closed since the last reset or loss: the same tests with rho down to 0.25, and the newest 250 ms
 * still carrying the tone. The longer windows average away the noise that now and then keeps
 * every pair of 250 ms hops from qualifying a tone at 0 dB well past half a second (see
 * ctcss_step_late()).
 *
 * Samples from inside the carrier hangover keep the correlators' time, but a hop whose newest
 * 100 ms holds nothing else keeps the verdict, so a dropout's silence alone never ends a lock or
 * makes one. A carrier that keeps dropping out still cannot keep a lock the tone has left:
 * every opening makes the two hops that read it count.
 *
 * Transmitter tone error (issue #643). The rules above -- the on-value rules -- confirm a tone only
 * from estimates within 0.5 Hz of its value, but encoders are specified to 0.5 % of it: a radio
 * 0.4 % high on 150.0 Hz sends 150.6 Hz, which they never confirm. Where they qualify nothing, a hop
 * may also qualify a tone from an estimate further off, up to 0.5 % of the tone (never past its
 * gate), as far as the estimate's own precision allows (ctcss_tone_error_gate_hz()); only a window
 * that carries the tone throughout is weighed that way. The on-value rules stay first at every
 * step: a hop they qualify wins over one that rests on tone error; a lock that rests on tone error
 * alone (not main_confirmed) needs the late windows, may not pre-empt a tone they are acquiring,
 * yields to any tone they confirm, and is reported as off-value, which the tone policy never
 * rejects on before its window ends (dsd_analog_rx_report::off_value); a lock is only handed to a
 * candidate they would lock. The hold grows with the offset the tone locked at, and never rejects
 * what acquisition would take. A lock that rests on tone error alone is none for the on-value
 * rules: what they keep -- their candidates, late windows and holdoff, and the frequency a lock of
 * theirs is measured from -- is left as it would be without it. Where nothing rests on tone error,
 * the detector does exactly what the on-value rules do.
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
/* rho the late acquisition windows (ctcss_step_late) qualify a tone at. Over 400 ms noise alone
   puts about 1/116 of the band into a bin (1/174 over 600 ms), so noise reaches 0.25 at a bin on
   well under one hop in 10^12, while a tone at 0 dB in-band reads about 0.5. */
static const double k_late_acquire_rho = 0.25;
static const double k_hold_rho = 0.15;
/* The snap gate's floor: a locked tone holds while its own bin's fine estimate stays this close to the
   table value, or k_tone_error_frac of it where that is more. A tone whose nearest neighbour is closer
   than twice that gets half that distance instead (ctcss_tables_build()). */
static const double k_snap_hz = 0.8;
/* Estimates this close are the same distance from two tones (snap ties). */
static const double k_snap_tie_hz = 1e-9;
/* A candidate for one tone of a close pair (150.0 and 151.4 Hz, 1.4 Hz apart; dsd_analog_ctcss_tables::close_neighbour)
   whose newest estimate sits more than this toward the other tone needs one more agreeing hop to lock
   (DSD_ANALOG_CTCSS_PAIR_LEAN_HOPS; issue #623). The pair's acquisition gates are 0.4 Hz apart, so a noisy
   estimate of either tone can reach the other's: at 0 dB in-band the 250 ms estimate scatters by about 0.19 Hz, and
   by more while the window still holds the noise before a tone's onset. A tone set 0.2 Hz toward the other was then
   named as the other at about one start in 850. Noise that carries an estimate from the true tone into the other's
   gate leaves it leaning toward the true tone: nine in ten of the candidates that named the other tone leaned more
   than this. A third agreeing hop, whose newest sub-block brings fresh noise, stops most of them, and only leaning
   candidates wait for it: a tone near its value leans this far on few hops, so the wait costs little lock time
   (docs/testing.md has both). */
static const double k_pair_lean_hz = 0.3;
/* Frequency hysteresis: a tone only locks from estimates this close to the table value. At
   0 dB in-band the 250 ms estimate scatters by about 0.19 Hz (RMS), so an off-table tone such
   as 68.2 Hz, 1.1 Hz from 69.3, reaches the 0.8 Hz snap gate on several percent of hops but
   this one on well under one in a thousand, while a table tone misses it on about 1%. */
static const double k_acquire_snap_hz = 0.5;
/* Transmitter tone error (issue #643): CTCSS encoders are specified to 0.5 % (the CML MX315A datasheet: "a tone
   accuracy within 0.5%"), and radio decoders accept 1-2 Hz either side of a tone (Tait TN-1031). A radio 0.4 % high on
   150.0 Hz sends 150.6 Hz, which the 0.5 Hz gate above never confirms. So a tone may also be confirmed from estimates
   up to this share of its value off it -- never past its gate (ctcss_tables_build()) -- as far as the estimate is
   precise enough to say so: the gate is shortened by k_name_sigmas times the estimate's own standard deviation
   (ctcss_estimate_variance()), and never shorter than k_acquire_snap_hz. Only a window that carries the tone in every
   sub-block (ctcss_window_stationary()) is measured that way; tones below 100 Hz, where 0.5 % is less than 0.5 Hz,
   never are. */
static const double k_tone_error_frac = 0.005;
static const double k_name_sigmas = 3.0;
/* The least standard deviation an estimate is credited with: what the tone's own negative-frequency image can bend the
   fitted slope by (a few hundredths of a hertz at most above 100 Hz), so a clean tone just short of a gate's edge is
   never named on that bias alone. */
static const double k_min_est_sd_hz = 0.01;
/* The variance reported for a window with no usable fit: far wider than any gate. */
static const double k_no_est_var_hz2 = 1e6;
/* A sub-block below this share of the window's median coherent amplitude at the bin means the window does not carry the
   tone throughout -- an onset, a fade or a dropout -- where the fit leans toward its own bin (issue #623) and its
   variance cannot be trusted. */
static const double k_stationary_frac = 0.5;
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

/* A window is the newest @p span sub-blocks of the ring: DSD_ANALOG_CTCSS_WINDOW of them, or up to
   DSD_ANALOG_CTCSS_LONG_WINDOW for late acquisition. j = 0 is its oldest sub-block, j = span - 1
   the newest. */
static int
ctcss_slot(const dsd_analog_ctcss* det, int span, int j) {
    return (det->ring_head + DSD_ANALOG_CTCSS_LONG_WINDOW - span + j) % DSD_ANALOG_CTCSS_LONG_WINDOW;
}

static ctcss_cpx
ctcss_ring_at(const dsd_analog_ctcss* det, int span, int j, int bin) {
    const int slot = ctcss_slot(det, span, j);
    ctcss_cpx c = {det->ring_re[slot][bin], det->ring_im[slot][bin]};
    return c;
}

static double
ctcss_ring_energy_at(const dsd_analog_ctcss* det, int span, int j) {
    return det->ring_energy[ctcss_slot(det, span, j)];
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
ctcss_cand_clear(dsd_analog_ctcss_cand* cand) {
    cand->index = -1;
    cand->hz = 0.0;
    cand->prev_hz = 0.0;
    cand->run = 0;
    cand->narrow_run = 0;
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
    det->fresh = 0;
    det->main_fresh = 0;
    det->sub_fill = 0;
    det->sub_open = 0;
    det->prev_open = 0;
    det->sub_energy = 0.0;
    det->sub_full = 0.0;
    det->state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    det->locked = -1;
    det->locked_hz = 0.0;
    det->burst_ref_hz = 0.0;
    ctcss_cand_clear(&det->cand);
    ctcss_cand_clear(&det->long_cand);
    det->fail_run = 0;
    det->lock_offset_hz = 0.0;
    det->main_confirmed = 0;
    det->main_fail_run = 0;
    det->main_lost = 0;
    det->main_none = 0;
    det->holdoff = 0;
    det->wide_holdoff = 0;
    det->open_samples = 0;
    DSD_MEMSET(det->wide, 0, sizeof(det->wide));
    det->wide_pos = 0;
    det->last_hop.best_index = -1;
    det->last_hop.snapped = -1;
}

static void ctcss_tables_build(dsd_analog_ctcss_tables* t);

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
    det->wide_len = DSD_ANALOG_CTCSS_LONG_WINDOW * det->sub_len;
    if (det->wide_len > DSD_ANALOG_CTCSS_WIDE_MAX) {
        det->wide_len = DSD_ANALOG_CTCSS_WIDE_MAX;
    }
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        const double w = 2.0 * M_PI * ctcss_tone_hz(k) / rate_hz;
        det->step_re[k] = cos(w);
        det->step_im[k] = -sin(w);
    }
    ctcss_tables_build(&det->tables);
    ctcss_reset(det);
}

/** @brief Half the distance from table tone @p k to its nearest neighbour. */
static double
ctcss_half_spacing_hz(int k) {
    double half = 1e9; /* no neighbour on that side: no limit (finite, for fast-math builds) */
    if (k > 0) {
        half = fmin(half, 0.5 * (ctcss_tone_hz(k) - ctcss_tone_hz(k - 1)));
    }
    if (k + 1 < DSD_CTCSS_TONE_COUNT) {
        half = fmin(half, 0.5 * (ctcss_tone_hz(k + 1) - ctcss_tone_hz(k)));
    }
    return half;
}

/**
 * @brief The per-tone gates (issue #643), computed once per configuration: ctcss_snap() runs over every tone for
 * every bin of every window, and computing them there cost about a third of the detector's time.
 *
 * - tolerance: k_snap_hz, or k_tone_error_frac of the tone where that is more (from 160 Hz up);
 * - gate, the snap and hold gate: the tolerance, or half the distance to the nearest neighbour where that is less, so
 *   the gates of two tones never overlap. Only 150.0 and 151.4 Hz, 1.4 Hz apart, get less than their tolerance: 0.7 Hz
 *   each (the next closest pair, 67.0 and 69.3 Hz, is 2.3 Hz apart);
 * - on-value gate, what the gate was before tone error was allowed for: k_snap_hz, or half the distance to the nearest
 *   neighbour where that is less. The on-value hold (main_confirmed) is judged against it;
 * - close neighbour: the neighbour that narrowed the gate below the tolerance, or -1;
 * - noise shape: an FM discriminator's noise density rises as f^2, so at the tone it is 3 (f / B)^2 times the band's
 *   average, B = DSD_ANALOG_RX_BAND_HZ, and never taken below the average.
 */
static void
ctcss_tables_build(dsd_analog_ctcss_tables* t) {
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        const double hz = ctcss_tone_hz(k);
        const double half = ctcss_half_spacing_hz(k);
        t->tolerance_hz[k] = fmax(k_snap_hz, k_tone_error_frac * hz);
        t->gate_hz[k] = fmin(t->tolerance_hz[k], half);
        t->on_value_gate_hz[k] = fmin(k_snap_hz, half);
        const double f = hz / DSD_ANALOG_RX_BAND_HZ;
        t->noise_shape[k] = fmax(1.0, 3.0 * f * f);
        t->close_neighbour[k] = -1;
        if (t->gate_hz[k] < t->tolerance_hz[k] - k_snap_tie_hz) {
            if (k > 0 && fabs((hz - ctcss_tone_hz(k - 1)) - (2.0 * t->gate_hz[k])) <= k_snap_tie_hz) {
                t->close_neighbour[k] = k - 1;
            } else if (k + 1 < DSD_CTCSS_TONE_COUNT) {
                t->close_neighbour[k] = k + 1;
            }
        }
    }
}

/** @brief Nearest table tone whose gate holds @p hz, or -1: also -1 for an estimate the same distance
 *  from two tones (the midpoint where 150.0 and 151.4 Hz's gates meet), which says neither. A gate's
 *  edge counts as inside within k_snap_tie_hz, so where two gates meet both tones are compared, and
 *  the tie found, whatever the rounding of their distances and gates. */
static int
ctcss_snap(const dsd_analog_ctcss_tables* t, double hz) {
    int best = -1;
    double best_err = 0.0;
    int tied = 0;
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        const double err = fabs(hz - ctcss_tone_hz(k));
        if (err > t->gate_hz[k] + k_snap_tie_hz) {
            continue;
        }
        if (best < 0 || err < best_err - k_snap_tie_hz) {
            best_err = err;
            best = k;
            tied = 0;
        } else if (fabs(err - best_err) <= k_snap_tie_hz) {
            tied = 1;
        }
    }
    return tied ? -1 : best;
}

int
dsd_analog_ctcss_snap_index(double hz) {
    dsd_analog_ctcss_tables t;
    ctcss_tables_build(&t);
    return ctcss_snap(&t, hz);
}

/** @brief Coarse per-sub-block phase advance: the magnitude-weighted pulse-pair estimate. */
static double
ctcss_coarse_advance(const dsd_analog_ctcss* det, int span, int bin) {
    double re = 0.0;
    double im = 0.0;
    for (int j = 1; j < span; j++) {
        const ctcss_cpx a = ctcss_ring_at(det, span, j - 1, bin);
        const ctcss_cpx b = ctcss_ring_at(det, span, j, bin);
        re += (b.re * a.re) + (b.im * a.im);
        im += (b.im * a.re) - (b.re * a.im);
    }
    return atan2(im, re);
}

/** @brief A linear phase fit across the window: the offset and how well a line explains it. */
typedef struct {
    double advance;                           /**< per-sub-block phase advance, radians */
    double residual;                          /**< magnitude-weighted RMS residual, radians */
    double err[DSD_ANALOG_CTCSS_LONG_WINDOW]; /**< per-sub-block residual, radians */
    double jm;                                /**< weighted mean sub-block index */
    double sxx;                               /**< weighted sum of squared index deviations; 0 = no usable fit */
} ctcss_fit;

/**
 * @brief Refine the coarse advance by weighted least squares on the de-rotated phases.
 *
 * After de-rotation by the coarse advance the sub-block phases sit near one value, so they
 * need no unwrapping; the fit's slope is the correction and its weighted RMS residual says
 * whether the window is one steady tone at all.
 */
static void
ctcss_refine_advance(const dsd_analog_ctcss* det, int span, int bin, double coarse, ctcss_fit* fit) {
    ctcss_cpx d[DSD_ANALOG_CTCSS_LONG_WINDOW];
    ctcss_cpx sum = {0.0, 0.0};
    for (int j = 0; j < span; j++) {
        d[j] = ctcss_rotate(ctcss_ring_at(det, span, j, bin), -coarse * (double)j);
        sum.re += d[j].re;
        sum.im += d[j].im;
    }
    const double ref = atan2(sum.im, sum.re);
    double r[DSD_ANALOG_CTCSS_LONG_WINDOW];
    double w[DSD_ANALOG_CTCSS_LONG_WINDOW];
    double sw = 0.0;
    double swj = 0.0;
    double swr = 0.0;
    for (int j = 0; j < span; j++) {
        const double phase = atan2(d[j].im, d[j].re) - ref;
        r[j] = atan2(sin(phase), cos(phase));
        w[j] = ctcss_mag2(d[j]);
        sw += w[j];
        swj += w[j] * (double)j;
        swr += w[j] * r[j];
    }
    fit->advance = coarse;
    fit->residual = M_PI;
    fit->jm = 0.0;
    fit->sxx = 0.0;
    if (!(sw > 0.0)) {
        /* An empty bin has nothing to fit: every sub-block counts as fully unexplained. */
        for (int j = 0; j < span; j++) {
            fit->err[j] = M_PI;
        }
        return;
    }
    const double jm = swj / sw;
    const double rm = swr / sw;
    double sxy = 0.0;
    double sxx = 0.0;
    for (int j = 0; j < span; j++) {
        sxy += w[j] * ((double)j - jm) * (r[j] - rm);
        sxx += w[j] * ((double)j - jm) * ((double)j - jm);
    }
    const double slope = (sxx > 0.0) ? sxy / sxx : 0.0;
    fit->jm = jm;
    fit->sxx = sxx;
    double se = 0.0;
    for (int j = 0; j < span; j++) {
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
ctcss_fit_chi2(const dsd_analog_ctcss* det, int span, int bin, const ctcss_fit* fit) {
    /* The noise is assumed to fill the sub-audible band up to the front end's cutoff. */
    const double noise_bin_gain = det->rate_hz / (2.0 * DSD_ANALOG_RX_BAND_HZ);
    double chi2 = 0.0;
    for (int j = 0; j < span; j++) {
        const double bin_power = ctcss_mag2(ctcss_ring_at(det, span, j, bin));
        const double tone_energy = 2.0 * bin_power / (double)det->sub_len;
        double noise_energy = ctcss_ring_energy_at(det, span, j) - tone_energy;
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
    return chi2 / (double)(span - 2);
}

/**
 * @brief Variance of a window's fine estimate at @p bin, Hz^2 (issue #643): how far off its table value the estimate
 * may be trusted to say a tone is.
 *
 * The fit weights each sub-block's phase by its bin power w_j, which is inverse to the phase noise the sub-block
 * carries, so the slope's variance is sum (w_j (j - jm))^2 v_j / sxx^2, with v_j the phase variance the noise model of
 * ctcss_fit_chi2() gives -- here without its floor, and with the noise density at the tone (the discriminator's f^2
 * rise, dsd_analog_ctcss_tables::noise_shape). For a window that carries the tone throughout, in white noise, that is
 * the Cramer-Rao bound: 0.19 Hz at 0 dB in-band over 250 ms. Then it is scaled up by the fit's own reduced chi-square
 * against the same variances, floored at k_min_phase_var as the qualification's is (the floor that absorbs the tone's
 * own negative-frequency image), wherever the phase wanders more than the model allows: coloured noise, a voice's
 * pitch, a phase disturbance. Computed only for a bin the tone-error gate is weighing.
 */
static double
ctcss_estimate_variance(const dsd_analog_ctcss* det, int span, int bin) {
    ctcss_fit fit;
    ctcss_refine_advance(det, span, bin, ctcss_coarse_advance(det, span, bin), &fit);
    if (!(fit.sxx > 0.0) || span <= 2) {
        return k_no_est_var_hz2;
    }
    const double noise_bin_gain = det->rate_hz / (2.0 * DSD_ANALOG_RX_BAND_HZ) * det->tables.noise_shape[bin];
    double lever = 0.0;
    double chi2 = 0.0;
    for (int j = 0; j < span; j++) {
        const double bin_power = ctcss_mag2(ctcss_ring_at(det, span, j, bin));
        const double tone_energy = 2.0 * bin_power / (double)det->sub_len;
        double noise_energy = ctcss_ring_energy_at(det, span, j) - tone_energy;
        if (!(noise_energy > 0.0)) {
            noise_energy = 0.0;
        }
        const double noise_in_bin = noise_energy * noise_bin_gain;
        double snr = 1e9;
        if (noise_in_bin > 0.0) {
            snr = (bin_power / noise_in_bin) - 1.0;
        }
        if (snr < 0.0) {
            snr = 0.0;
        }
        const double var = k_max_phase_var / (1.0 + (2.0 * k_max_phase_var * snr));
        chi2 += fit.err[j] * fit.err[j] / fmax(var, k_min_phase_var);
        const double lw = bin_power * ((double)j - fit.jm);
        lever += lw * lw * var;
    }
    const double inflate = fmax(1.0, chi2 / (double)(span - 2));
    const double conv = det->rate_hz / (2.0 * M_PI * (double)det->sub_len);
    const double var_slope = lever / (fit.sxx * fit.sxx) * inflate;
    return (var_slope * conv * conv) + (k_min_est_sd_hz * k_min_est_sd_hz);
}

/**
 * @brief rho over sub-blocks [first, first + count) at @p bin, made coherent at @p advance.
 *
 * 2|sum|^2 / (N * energy): a pure tone reads 1, a tone at 0 dB in-band SNR about 0.5.
 */
static double
ctcss_rho(const dsd_analog_ctcss* det, int span, int bin, double advance, int first, int count) {
    ctcss_cpx sum = {0.0, 0.0};
    double energy = 0.0;
    for (int j = first; j < first + count; j++) {
        const ctcss_cpx c = ctcss_rotate(ctcss_ring_at(det, span, j, bin), -advance * (double)j);
        sum.re += c.re;
        sum.im += c.im;
        energy += ctcss_ring_energy_at(det, span, j);
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
ctcss_full_share(const dsd_analog_ctcss* det, int span, double rho, int first, int count) {
    double band = 0.0;
    double full = 0.0;
    for (int j = first; j < first + count; j++) {
        band += ctcss_ring_energy_at(det, span, j);
        full += det->ring_full[ctcss_slot(det, span, j)];
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

/** @brief Fine estimate, fit and rho for one correlator bin over the newest @p span sub-blocks. */
static void
ctcss_measure_bin(const dsd_analog_ctcss* det, int span, int bin, dsd_analog_ctcss_hop* out) {
    ctcss_fit fit;
    ctcss_refine_advance(det, span, bin, ctcss_coarse_advance(det, span, bin), &fit);
    out->evaluated = 1;
    out->best_index = bin;
    out->residual = fit.residual;
    out->chi2 = ctcss_fit_chi2(det, span, bin, &fit);
    out->est_hz = ctcss_tone_hz(bin) + (fit.advance * det->rate_hz / (2.0 * M_PI * (double)det->sub_len));
    out->rho = ctcss_rho(det, span, bin, fit.advance, 0, span);
    out->share = ctcss_full_share(det, span, out->rho, 0, span);
    out->snapped = ctcss_snap(&det->tables, out->est_hz);
    out->recent_rho = 0.0;
    out->harmonic = 0.0;
    out->span = span;
    out->est_var_hz2 = -1.0;
    out->stationary = -1;
}

/* What a qualifying hop passes whatever its frequency: an estimate that snaps to the table, from its own bin,
   explaining enough of the band and of the full input, with a phase fit that is one steady tone. */
static int
ctcss_hop_fit_ok(const dsd_analog_ctcss_hop* hop, double acquire_rho) {
    /* A 50 ms sub-block only resolves offsets up to 10 Hz from its bin: a signal 10.7 Hz
       below a bin reads as 9.3 Hz above it. The table is dense enough that every supported
       tone is within 4.1 Hz of its nearest bin, so an estimate further than 5 Hz from the bin
       that produced it is an alias, never a tone. */
    return hop->snapped >= 0 && fabs(hop->est_hz - ctcss_tone_hz(hop->best_index)) <= k_max_bin_offset_hz
           && hop->rho >= acquire_rho && hop->share >= k_min_full_share && hop->residual <= k_max_residual_rad
           && hop->chi2 <= k_max_chi2;
}

/** @brief How far @p hop's estimate sits off the table tone it snapped to. */
static double
ctcss_hop_offset_hz(const dsd_analog_ctcss_hop* hop) {
    return fabs(hop->est_hz - ctcss_tone_hz(hop->snapped));
}

/* On its value: within k_acquire_snap_hz of the table tone, the frequency hysteresis that keeps an off-table tone such
   as 68.2 Hz off 69.3. These on-value rules are the detector as it stood before tone error was allowed for (issue
   #643), and they are applied first at every step: a hop they qualify is taken whatever else qualifies, and tone error
   is weighed only where they find nothing. */
static int
ctcss_hop_qualifies_on_value(const dsd_analog_ctcss_hop* hop, double acquire_rho) {
    return ctcss_hop_fit_ok(hop, acquire_rho) && ctcss_hop_offset_hz(hop) <= k_acquire_snap_hz;
}

/**
 * @brief Whether every sub-block of the newest @p span carries @p bin's tone: none below k_stationary_frac of the
 * window's median coherent amplitude. An onset, a fade or a dropout inside the window fails it.
 */
static int
ctcss_window_stationary(const dsd_analog_ctcss* det, int span, int bin) {
    if (span < 1 || span > DSD_ANALOG_CTCSS_LONG_WINDOW) {
        return 0;
    }
    double mag[DSD_ANALOG_CTCSS_LONG_WINDOW] = {0.0};
    double sorted[DSD_ANALOG_CTCSS_LONG_WINDOW] = {0.0};
    for (int j = 0; j < span; j++) {
        mag[j] = sqrt(ctcss_mag2(ctcss_ring_at(det, span, j, bin)));
        /* Insertion sort: at most twelve values. */
        int i = j;
        while (i > 0 && sorted[i - 1] > mag[j]) {
            sorted[i] = sorted[i - 1];
            i--;
        }
        sorted[i] = mag[j];
    }
    const double median = sorted[span / 2];
    if (!(median > 0.0)) {
        return 0;
    }
    for (int j = 0; j < span; j++) {
        if (!(mag[j] >= k_stationary_frac * median)) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief The tone-error gate of @p hop's tone (issue #643): k_tone_error_frac of the tone, never past its gate,
 * shortened by k_name_sigmas times the estimate's standard deviation, and never less than k_acquire_snap_hz -- which is
 * what it is for a tone below 100 Hz and for a window that does not carry the tone throughout. Computes, and keeps in
 * @p hop, the window's stationarity and the estimate's variance, each only when the gate needs it.
 */
static double
ctcss_tone_error_gate_hz(const dsd_analog_ctcss* det, dsd_analog_ctcss_hop* hop) {
    const int k = hop->snapped;
    const double reach = fmin(k_tone_error_frac * ctcss_tone_hz(k), det->tables.gate_hz[k]);
    if (!(reach > k_acquire_snap_hz)) {
        return k_acquire_snap_hz;
    }
    if (hop->stationary < 0) {
        hop->stationary = ctcss_window_stationary(det, hop->span, hop->best_index);
    }
    if (!hop->stationary) {
        return k_acquire_snap_hz;
    }
    if (hop->est_var_hz2 < 0.0) {
        hop->est_var_hz2 = ctcss_estimate_variance(det, hop->span, hop->best_index);
    }
    return fmax(k_acquire_snap_hz, reach - (k_name_sigmas * sqrt(hop->est_var_hz2)));
}

/* Off its value by transmitter tone error: every other test passed, the estimate more than k_acquire_snap_hz off the
   table tone but within its tone-error gate. */
static int
ctcss_hop_qualifies_tone_error(const dsd_analog_ctcss* det, dsd_analog_ctcss_hop* hop, double acquire_rho) {
    if (!ctcss_hop_fit_ok(hop, acquire_rho)) {
        return 0;
    }
    const double off = ctcss_hop_offset_hz(hop);
    return off > k_acquire_snap_hz && off <= ctcss_tone_error_gate_hz(det, hop);
}

/* Either way. */
static int
ctcss_hop_qualifies(const dsd_analog_ctcss* det, dsd_analog_ctcss_hop* hop, double acquire_rho) {
    return ctcss_hop_qualifies_on_value(hop, acquire_rho) || ctcss_hop_qualifies_tone_error(det, hop, acquire_rho);
}

/**
 * @brief Measure every bin and keep the strongest candidate.
 *
 * Every bin rather than only the loudest one: under voice, energy the transmitter's voice
 * filter let through just below 300 Hz can outweigh a tone at the bottom of the table, and
 * the bin that sees the tone best is the one whose window the tone explains best. A bin
 * the on-value rules qualify wins over one that qualifies by tone error alone (issue #643), which wins over one that
 * does not qualify; among equals the larger rho wins. So the winner is the on-value rules' own whenever they qualify
 * anything. @p alt, when not NULL, gets the best tone-error qualifier (alt->evaluated 0 when there is none): what an
 * on-value winner the harmonic test then rejects hands over to.
 */
static void
ctcss_measure(const dsd_analog_ctcss* det, int span, double acquire_rho, dsd_analog_ctcss_hop* hop,
              dsd_analog_ctcss_hop* alt) {
    int best_rank = 0;
    hop->rho = -1.0;
    if (alt) {
        DSD_MEMSET(alt, 0, sizeof(*alt));
        alt->rho = -1.0;
    }
    for (int k = 0; k < DSD_CTCSS_TONE_COUNT; k++) {
        dsd_analog_ctcss_hop trial;
        ctcss_measure_bin(det, span, k, &trial);
        int rank = 0;
        if (ctcss_hop_qualifies_on_value(&trial, acquire_rho)) {
            rank = 2;
        } else if (ctcss_hop_qualifies_tone_error(det, &trial, acquire_rho)) {
            rank = 1;
            if (alt && trial.rho > alt->rho) {
                *alt = trial;
            }
        }
        if (rank < best_rank || (rank == best_rank && !(trial.rho > hop->rho))) {
            continue;
        }
        *hop = trial;
        best_rank = rank;
    }
}

/** @brief Single-sub-block rho at @p bin: how much the tone dominates that sub-block. */
static double
ctcss_sub_rho(const dsd_analog_ctcss* det, int bin, int j) {
    const double denom = (double)det->sub_len * ctcss_ring_energy_at(det, DSD_ANALOG_CTCSS_WINDOW, j);
    if (!(denom > 0.0)) {
        return 0.0;
    }
    return 2.0 * ctcss_mag2(ctcss_ring_at(det, DSD_ANALOG_CTCSS_WINDOW, j, bin)) / denom;
}

/** @brief Phase jump between sub-blocks @p a and @p b beyond what the locked offset explains. */
static int
ctcss_pair_jumped(const dsd_analog_ctcss* det, int bin, double advance, int a, int b) {
    if (ctcss_sub_rho(det, bin, a) < k_burst_sub_rho || ctcss_sub_rho(det, bin, b) < k_burst_sub_rho) {
        return 0;
    }
    const ctcss_cpx ca = ctcss_ring_at(det, DSD_ANALOG_CTCSS_WINDOW, a, bin);
    const ctcss_cpx cb = ctcss_rotate(ctcss_ring_at(det, DSD_ANALOG_CTCSS_WINDOW, b, bin), -advance * (double)(b - a));
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
 * stood two hops ago, never from a newer estimate; on the first hop after a lock, the
 * candidate's estimate from the hop before the lock (ctcss_lock()). A flip inside a sub-block
 * that stays strong -- a 120 or 240 degree burst early or late in it -- puts part of the step
 * into that sub-block's phase, and a window that ends on it fits the step as a steeper slope.
 * Measured against that slope, the whole step across the next pair can read short of the 100
 * degree threshold: on a clean tone about one such burst in fourteen was missed that way. Either
 * reference comes from a window that ends no later than the older sub-block of every pair
 * compared here, so no step after that sub-block can have bent it.
 */
static int
ctcss_reverse_burst(const dsd_analog_ctcss* det, double ref_hz) {
    const int bin = det->locked;
    const double advance = ctcss_advance_for(det, bin, ref_hz);
    const int newest = DSD_ANALOG_CTCSS_WINDOW - 1;
    return ctcss_pair_jumped(det, bin, advance, newest - 1, newest)
           || ctcss_pair_jumped(det, bin, advance, newest - 2, newest);
}

/* A lock needs the candidate's estimate from the hop before it (dsd_analog_ctcss_cand::prev_hz). */
_Static_assert(DSD_ANALOG_CTCSS_ACQUIRE_HOPS >= 2, "a lock takes its burst reference from an earlier qualifying hop");

/*
 * Lock @p index at @p hz, this hop's estimate. The reverse burst check on the next hop compares
 * the sub-block two back, the newest of this hop's window, and a window that ends on it may
 * already carry part of a step made inside it. So the check starts one hop further back, from
 * @p burst_ref_hz: the same candidate's estimate from the qualifying hop before this one.
 */
static void
ctcss_lock(dsd_analog_ctcss* det, int index, double hz, double burst_ref_hz, int main_confirmed) {
    det->state = DSD_ANALOG_TONE_STATE_LOCKED;
    det->locked = index;
    det->locked_hz = hz;
    det->burst_ref_hz = burst_ref_hz;
    det->fail_run = 0;
    det->lock_offset_hz = fabs(hz - ctcss_tone_hz(index));
    det->main_confirmed = main_confirmed;
    det->main_fail_run = 0;
    det->main_lost = 0;
}

static void
ctcss_unlock(dsd_analog_ctcss* det) {
    /* What the ring holds from before the loss may still carry the tone that was lost: the late
       acquisition window starts over from here -- for the on-value rules only when the lock was theirs, since a lock
       that rested on tone error alone never existed for them (issue #643). Their verdict is then NONE if they lost this
       lock or an earlier one, or have read no tone for 500 ms of carrier, and still ACQUIRING otherwise. */
    const int theirs = det->main_confirmed || det->main_lost;
    det->fresh = 0;
    if (det->main_confirmed) {
        det->main_fresh = 0;
    }
    if (theirs) {
        det->main_none = 1;
    }
    const int64_t no_tone_samples = (int64_t)llround(det->rate_hz * (double)DSD_ANALOG_CTCSS_NO_TONE_MS / 1000.0);
    det->state = det->main_none || det->open_samples >= no_tone_samples ? DSD_ANALOG_TONE_STATE_NONE
                                                                        : DSD_ANALOG_TONE_STATE_ACQUIRING;
    det->locked = -1;
    det->locked_hz = 0.0;
    det->burst_ref_hz = 0.0;
    det->fail_run = 0;
    det->lock_offset_hz = 0.0;
    det->main_confirmed = 0;
    det->main_fail_run = 0;
    det->main_lost = 0;
}

/** @brief Most pieces the harmonic check splits a 250 ms window into (about 25 ms each), and
    the most any window gets: as many per sub-block, over the late acquisition window. */
enum {
    CTCSS_HARMONIC_PIECES = 12,
    CTCSS_HARMONIC_PIECES_MAX = (CTCSS_HARMONIC_PIECES * DSD_ANALOG_CTCSS_LONG_WINDOW) / DSD_ANALOG_CTCSS_WINDOW,
};

/**
 * @brief Per-piece correlations of the wide stream at hz, 2hz and 3hz, phase-locked to each other.
 *
 * Each piece spans a whole number of the candidate's cycles, close to 25 ms, so the three
 * kernels are orthogonal over it: noise at f is independent of noise at 2f and 3f, and the
 * candidate's own energy does not leak into its harmonic bins. Pieces are taken from the
 * newest end of the window, the newest @p span sub-blocks. Returns the number of pieces filled.
 */
static int
ctcss_wide_correlate(const dsd_analog_ctcss* det, int span, double hz, ctcss_cpx a[CTCSS_HARMONIC_PIECES_MAX],
                     ctcss_cpx b[CTCSS_HARMONIC_PIECES_MAX], ctcss_cpx c[CTCSS_HARMONIC_PIECES_MAX]) {
    const double cycles = fmax(1.0, round(hz * 0.025));
    const int piece = (int)lround(cycles * det->rate_hz / hz);
    const int span_len = span * det->sub_len;
    if (piece <= 0 || span_len > det->wide_len) {
        return 0;
    }
    const int max_pieces = (CTCSS_HARMONIC_PIECES * span) / DSD_ANALOG_CTCSS_WINDOW;
    int pieces = span_len / piece;
    if (pieces > max_pieces) {
        pieces = max_pieces;
    }
    /* The window's oldest sample sits span_len samples behind the newest. */
    const int first = (det->wide_len - span_len) + (span_len - (pieces * piece));
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
ctcss_harmonic_lock(const dsd_analog_ctcss* det, int span, double hz) {
    ctcss_cpx a[CTCSS_HARMONIC_PIECES_MAX];
    ctcss_cpx b[CTCSS_HARMONIC_PIECES_MAX];
    ctcss_cpx c[CTCSS_HARMONIC_PIECES_MAX];
    const int pieces = ctcss_wide_correlate(det, span, hz, a, b, c);
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

/* The last qualifying test, on the winner only: no harmonics phase-locked to it. Last, because it
   is the one test that costs a pass over the window. */
static int
ctcss_passes_harmonic(const dsd_analog_ctcss* det, int span, dsd_analog_ctcss_hop* hop, double max_ratio) {
    hop->harmonic = ctcss_harmonic_lock(det, span, hop->est_hz);
    return hop->harmonic < max_ratio;
}

/* Feed a hop into @p cand: the run of consecutive hops that qualified the same table tone with
   estimates that agree, and the part of it the on-value rules alone qualified (narrow_run). */
static void
ctcss_track(dsd_analog_ctcss_cand* cand, int qualified, const dsd_analog_ctcss_hop* hop) {
    if (!qualified) {
        ctcss_cand_clear(cand);
        return;
    }
    const int stable = hop->snapped == cand->index && fabs(hop->est_hz - cand->hz) <= k_stable_hz;
    const int on_value = ctcss_hop_offset_hz(hop) <= k_acquire_snap_hz;
    cand->run = stable ? cand->run + 1 : 1;
    cand->narrow_run = on_value ? (stable ? cand->narrow_run + 1 : 1) : 0;
    cand->prev_hz = stable ? cand->hz : hop->est_hz;
    cand->index = hop->snapped;
    cand->hz = hop->est_hz;
}

/* The agreeing hops @p cand needs to lock: DSD_ANALOG_CTCSS_ACQUIRE_HOPS, or DSD_ANALOG_CTCSS_PAIR_LEAN_HOPS for a tone
   of a close pair while its newest estimate leans more than k_pair_lean_hz toward the other tone. */
static int
ctcss_cand_hops_needed(const dsd_analog_ctcss* det, const dsd_analog_ctcss_cand* cand) {
    const int other = det->tables.close_neighbour[cand->index];
    if (other >= 0) {
        const double toward = ctcss_tone_hz(other) > ctcss_tone_hz(cand->index) ? 1.0 : -1.0;
        if ((cand->hz - ctcss_tone_hz(cand->index)) * toward > k_pair_lean_hz) {
            return DSD_ANALOG_CTCSS_PAIR_LEAN_HOPS;
        }
    }
    return DSD_ANALOG_CTCSS_ACQUIRE_HOPS;
}

/* Whether @p cand has agreed on enough hops to lock. One test for both acquisition windows and for a lock handed from
   another tone. */
static int
ctcss_cand_ready(const dsd_analog_ctcss* det, const dsd_analog_ctcss_cand* cand) {
    return cand->index >= 0 && cand->run >= ctcss_cand_hops_needed(det, cand);
}

/* Whether the on-value rules alone would lock @p cand now: its agreeing run qualified on its value throughout. */
static int
ctcss_cand_main_ready(const dsd_analog_ctcss* det, const dsd_analog_ctcss_cand* cand) {
    return cand->index >= 0 && cand->narrow_run >= ctcss_cand_hops_needed(det, cand);
}

/* What the on-value rules hold of @p cand: the agreeing hops at the end of its run that they qualified, as they count
   them (issue #643). A run that ends on a hop qualified by tone error is none of theirs. */
static void
ctcss_cand_narrow(dsd_analog_ctcss_cand* cand) {
    if (cand->narrow_run < 1) {
        ctcss_cand_clear(cand);
        return;
    }
    if (cand->narrow_run == 1) {
        /* The one hop they qualified starts their run: the burst reference it would give is its own estimate. */
        cand->prev_hz = cand->hz;
    }
    cand->run = cand->narrow_run;
}

static void
ctcss_lock_cand(dsd_analog_ctcss* det, const dsd_analog_ctcss_cand* cand) {
    const int theirs = ctcss_cand_main_ready(det, cand);
    ctcss_lock(det, cand->index, cand->hz, cand->prev_hz, theirs);
    if (theirs) {
        ctcss_cand_clear(&det->long_cand);
    } else {
        /* A lock the on-value rules do not take leaves their late run where it was. */
        ctcss_cand_narrow(&det->long_cand);
    }
}

/*
 * Whether the newest 250 ms still carry what a late window qualified: the tone's own bin there
 * reads at least the late rho, with an estimate inside @p gate_hz -- the on-value rules' gate for
 * what they qualified, the tone's snap gate for what tone error did (issue #643). A tone in noise
 * always does (that window's rho is about 0.5 at 0 dB); a voice that held a pitch near a table tone
 * for most of a late window and has since moved on does not.
 */
static int
ctcss_late_still_present(const dsd_analog_ctcss* det, const dsd_analog_ctcss_hop* hop, const double* gate_hz) {
    dsd_analog_ctcss_hop newest;
    ctcss_measure_bin(det, DSD_ANALOG_CTCSS_WINDOW, hop->snapped, &newest);
    return newest.rho >= k_late_acquire_rho
           && fabs(newest.est_hz - ctcss_tone_hz(hop->snapped)) <= gate_hz[hop->snapped];
}

/* The late windows to try over the newest @p fresh sub-blocks (dsd_analog_ctcss::fresh, or main_fresh for the on-value
   rules), the longer first: the newest available sub-blocks, then DSD_ANALOG_CTCSS_LONG_MIN when that is shorter.
   Returns how many. */
static int
ctcss_late_spans(int fresh, int spans[2]) {
    const int avail = fresh < DSD_ANALOG_CTCSS_LONG_WINDOW ? fresh : DSD_ANALOG_CTCSS_LONG_WINDOW;
    if (avail < DSD_ANALOG_CTCSS_LONG_MIN) {
        return 0;
    }
    spans[0] = avail;
    spans[1] = DSD_ANALOG_CTCSS_LONG_MIN;
    return avail > DSD_ANALOG_CTCSS_LONG_MIN ? 2 : 1;
}

/* Whether a late window's on-value winner @p hop of span @p span qualifies a tone other than @p except (-1 for any). */
static int
ctcss_late_on_value(const dsd_analog_ctcss* det, int span, dsd_analog_ctcss_hop* hop, int except) {
    return ctcss_hop_qualifies_on_value(hop, k_late_acquire_rho) && hop->snapped != except
           && ctcss_late_still_present(det, hop, det->tables.on_value_gate_hz)
           && ctcss_passes_harmonic(det, span, hop, k_max_harmonic_ratio);
}

/* Whether the late windows hold a tone other than @p index that the on-value rules qualify (issue #643). */
static int
ctcss_on_value_rival(const dsd_analog_ctcss* det, int index) {
    int spans[2];
    const int n = ctcss_late_spans(det->main_fresh, spans);
    for (int i = 0; i < n; i++) {
        dsd_analog_ctcss_hop hop;
        DSD_MEMSET(&hop, 0, sizeof(hop));
        ctcss_measure(det, spans[i], k_late_acquire_rho, &hop, NULL);
        if (ctcss_late_on_value(det, spans[i], &hop, index)) {
            return 1;
        }
    }
    return 0;
}

/*
 * Whether @p cand may lock now (issue #643). A run the on-value rules alone qualified always may. One that rests on
 * tone error may only fill a gap they leave: once the late windows exist (DSD_ANALOG_CTCSS_LONG_MIN sub-blocks since
 * the last reset or loss), with no candidate of another tone on its value in either acquisition window and no such
 * tone qualifying in the late windows -- a tone the on-value rules are still acquiring is never pre-empted.
 */
static int
ctcss_lock_allowed(const dsd_analog_ctcss* det, const dsd_analog_ctcss_cand* cand) {
    if (ctcss_cand_main_ready(det, cand)) {
        return 1;
    }
    if (det->fresh < DSD_ANALOG_CTCSS_LONG_MIN || det->main_fresh < DSD_ANALOG_CTCSS_LONG_MIN) {
        return 0;
    }
    const dsd_analog_ctcss_cand* both[2] = {&det->cand, &det->long_cand};
    for (int i = 0; i < 2; i++) {
        if (both[i]->index >= 0 && both[i]->index != cand->index && both[i]->narrow_run >= 1) {
            return 0;
        }
    }
    return !ctcss_on_value_rival(det, cand->index);
}

/* The late windows the on-value rules measured this hop: each one's span, winner and best tone-error qualifier. */
typedef struct {
    int spans[2];
    int count;
    dsd_analog_ctcss_hop wins[2];
    dsd_analog_ctcss_hop alts[2];
} ctcss_late_set;

/* The on-value rules over their own late windows (main_fresh), the longer first. Returns 1 with the qualifying hop in
   @p hop when one qualifies. */
static int
ctcss_late_on_value_pass(const dsd_analog_ctcss* det, ctcss_late_set* set, dsd_analog_ctcss_hop* hop) {
    const int n = ctcss_late_spans(det->main_fresh, set->spans);
    set->count = 0;
    for (int i = 0; i < n; i++) {
        DSD_MEMSET(&set->wins[i], 0, sizeof(set->wins[i]));
        ctcss_measure(det, set->spans[i], k_late_acquire_rho, &set->wins[i], &set->alts[i]);
        set->count++;
        if (ctcss_late_on_value(det, set->spans[i], &set->wins[i], -1)) {
            *hop = set->wins[i];
            return 1;
        }
    }
    return 0;
}

/* Whether the newest @p span sub-blocks qualify a tone by tone error, from the window the on-value pass measured when
   it measured that span. The window's best tone-error qualifier is its winner, unless the on-value rules qualified that
   (and something after them rejected it), when the best one behind it. */
static int
ctcss_late_tone_error_window(const dsd_analog_ctcss* det, const ctcss_late_set* set, int span,
                             dsd_analog_ctcss_hop* hop) {
    dsd_analog_ctcss_hop win;
    dsd_analog_ctcss_hop alt;
    int j = 0;
    while (j < set->count && set->spans[j] != span) {
        j++;
    }
    if (j < set->count) {
        win = set->wins[j];
        alt = set->alts[j];
    } else {
        DSD_MEMSET(&win, 0, sizeof(win));
        ctcss_measure(det, span, k_late_acquire_rho, &win, &alt);
    }
    dsd_analog_ctcss_hop* c = ctcss_hop_qualifies_on_value(&win, k_late_acquire_rho) ? &alt : &win;
    if (c->evaluated && ctcss_hop_qualifies_tone_error(det, c, k_late_acquire_rho)
        && ctcss_late_still_present(det, c, det->tables.gate_hz)
        && ctcss_passes_harmonic(det, span, c, k_max_harmonic_ratio)) {
        *hop = *c;
        return 1;
    }
    return 0;
}

/* Tone error over the late windows since the last loss of any lock (fresh). Returns 1 with the qualifying hop in @p hop
   when one qualifies. */
static int
ctcss_late_tone_error_pass(const dsd_analog_ctcss* det, const ctcss_late_set* set, dsd_analog_ctcss_hop* hop) {
    int spans[2];
    const int n = ctcss_late_spans(det->fresh, spans);
    for (int i = 0; i < n; i++) {
        if (ctcss_late_tone_error_window(det, set, spans[i], hop)) {
            return 1;
        }
    }
    return 0;
}

/*
 * Late acquisition. At 0 dB in-band the 250 ms window's rho, estimate and harmonic reading scatter
 * enough that now and then a tone's noise keeps every pair of hops from qualifying it for well
 * over half a second. Two longer windows average that scatter down: the newest
 * DSD_ANALOG_CTCSS_LONG_WINDOW sub-blocks (600 ms) and the newest DSD_ANALOG_CTCSS_LONG_MIN
 * (400 ms), which fills with a new tone sooner. A hop qualifies a tone when either window does --
 * the longer one tried first -- with the same tests as the 250 ms window, except that rho may be
 * as low as k_late_acquire_rho, and with one more: the newest 250 ms must still carry the tone
 * (ctcss_late_still_present()). Two consecutive qualifying hops with agreeing estimates lock it,
 * as for the 250 ms window. The on-value rules go first across both windows; tone error (issue #643)
 * is weighed only when they qualify nothing in either, on the same measurements.
 *
 * Neither window reaches back past the last reset or loss (dsd_analog_ctcss::fresh): until 600 ms
 * have closed since then the longer one covers only what has, and until 400 ms have there is no
 * late acquisition at all, so a tone that has just been lost cannot lock again from what the ring
 * still holds of it. The on-value rules count from their own losses (main_fresh), so a lock that
 * rested on tone error alone, which they never had, does not start them over. A reverse burst's
 * holdoff blocks it as it blocks the 250 ms window, and it never runs while a tone is locked --
 * except while a lock rests on tone error alone (not main_confirmed), when the on-value rules,
 * for which nothing is locked, keep listening and lock what they confirm: the locked tone itself,
 * which they then hold, or another, which takes the lock.
 */
static void
ctcss_step_late(dsd_analog_ctcss* det) {
    const int locked = det->state == DSD_ANALOG_TONE_STATE_LOCKED;
    const int yield = locked && !det->main_confirmed;
    if ((locked && !yield) || det->holdoff > 0) {
        ctcss_cand_clear(&det->long_cand);
        return;
    }
    ctcss_late_set set;
    dsd_analog_ctcss_hop hop;
    DSD_MEMSET(&hop, 0, sizeof(hop));
    int qualified = ctcss_late_on_value_pass(det, &set, &hop);
    /* Tone error never while yielding, and never in a burst's holdoff of its own. */
    if (!qualified && !yield && det->wide_holdoff == 0) {
        qualified = ctcss_late_tone_error_pass(det, &set, &hop);
    }
    ctcss_track(&det->long_cand, qualified, &hop);
    if (yield) {
        /* The on-value rules, for which nothing is locked, lock what they confirm: the locked tone itself, which they
           then hold, or another, which takes the lock. */
        if (ctcss_cand_main_ready(det, &det->long_cand)) {
            ctcss_lock_cand(det, &det->long_cand);
        }
        return;
    }
    if (ctcss_cand_ready(det, &det->long_cand) && ctcss_lock_allowed(det, &det->long_cand)) {
        ctcss_lock_cand(det, &det->long_cand);
    }
}

static void
ctcss_step_unlocked(dsd_analog_ctcss* det) {
    if (ctcss_cand_ready(det, &det->cand) && ctcss_lock_allowed(det, &det->cand)) {
        ctcss_lock_cand(det, &det->cand);
        return;
    }
    const int64_t no_tone_samples = (int64_t)llround(det->rate_hz * (double)DSD_ANALOG_CTCSS_NO_TONE_MS / 1000.0);
    if (det->state == DSD_ANALOG_TONE_STATE_ACQUIRING && det->open_samples >= no_tone_samples) {
        det->state = DSD_ANALOG_TONE_STATE_NONE;
        det->main_none = 1;
    }
}

/*
 * The late same-tone hold (issue #643): a locked tone whose 250 ms window is one steady tone but sits outside its hold
 * on frequency alone -- it moved within its tolerance, as when a second radio keys inside the carrier hangover, by more
 * than the 250 ms estimate can yet confirm -- holds while a late window over the locked bin qualifies it with every
 * acquisition test. Returns the late estimate's offset, or a negative value when none qualifies.
 */
static double
ctcss_late_same_tone(const dsd_analog_ctcss* det) {
    int spans[2];
    const int n = ctcss_late_spans(det->fresh, spans);
    for (int i = 0; i < n; i++) {
        dsd_analog_ctcss_hop late;
        ctcss_measure_bin(det, spans[i], det->locked, &late);
        if (late.snapped == det->locked && ctcss_hop_qualifies(det, &late, k_late_acquire_rho)
            && ctcss_late_still_present(det, &late, det->tables.gate_hz)
            && ctcss_passes_harmonic(det, spans[i], &late, k_max_harmonic_ratio)) {
            return ctcss_hop_offset_hz(&late);
        }
    }
    return -1.0;
}

/*
 * What ends a lock, or hands it to another tone, before its hold is judged. Returns 1 when it did. A lock that rests on
 * tone error alone is none for the on-value rules (issue #643): they lock what they confirm, as from no lock -- the
 * locked tone itself, which they then hold, or another, which takes the lock -- before anything else of this one is
 * judged. Then the reverse burst, and a tone the on-value rules would lock in its place.
 */
static int
ctcss_locked_preempted(dsd_analog_ctcss* det) {
    if (det->locked < 0 || det->locked >= DSD_CTCSS_TONE_COUNT) {
        /* Not reachable: a lock always names a table tone. */
        ctcss_unlock(det);
        return 1;
    }
    if (!det->main_confirmed && ctcss_cand_main_ready(det, &det->cand)) {
        ctcss_lock_cand(det, &det->cand);
        return 1;
    }
    /* The burst reference moves on one hop behind locked_hz: this hop checks against the
       frequency from two hops ago, and the next one against the frequency this hop starts from. */
    const double burst_ref_hz = det->burst_ref_hz;
    det->burst_ref_hz = det->locked_hz;
    if (ctcss_reverse_burst(det, burst_ref_hz)) {
        /* The holdoff and the candidate's loss are the on-value rules' for a lock of theirs; after a lock that was not,
           only the tone-error rules hold off. */
        const int theirs = det->main_confirmed;
        ctcss_unlock(det);
        if (theirs) {
            det->holdoff = CTCSS_BURST_HOLDOFF_HOPS;
            ctcss_cand_clear(&det->cand);
        } else {
            det->wide_holdoff = CTCSS_BURST_HOLDOFF_HOPS;
        }
        return 1;
    }
    if (det->cand.index >= 0 && det->cand.index != det->locked && ctcss_cand_main_ready(det, &det->cand)) {
        ctcss_lock_cand(det, &det->cand);
        return 1;
    }
    return 0;
}

/* The late same-tone hold (issue #643) for a hop the hold's frequency test failed: while the locked bin's 250 ms
   window @p own is one steady tone that misses on frequency alone, a late window over it that passes every late
   acquisition test holds it. Returns 1 when one does. */
static int
ctcss_late_same_tone_holds(dsd_analog_ctcss* det, dsd_analog_ctcss_hop* own, double own_off) {
    const int k = det->locked;
    if (!(own_off <= det->tables.gate_hz[k] && own->rho >= k_acquire_rho && own->residual <= k_max_residual_rad
          && own->chi2 <= k_max_chi2 && own->share >= k_min_full_share)) {
        return 0;
    }
    if (own->stationary < 0) {
        own->stationary = ctcss_window_stationary(det, DSD_ANALOG_CTCSS_WINDOW, k);
    }
    if (!own->stationary) {
        return 0;
    }
    const double late_off = ctcss_late_same_tone(det);
    if (!(late_off >= 0.0)) {
        return 0;
    }
    det->lock_offset_hz = fmax(det->lock_offset_hz, late_off);
    return 1;
}

/*
 * The hold's frequency test (issue #643): a tone that locked on its value keeps the 0.8 Hz hold (half the distance to
 * its neighbour where that is less) as before; one that locked further off gets as much more as the 0.5 Hz acquisition
 * gate gets from the 0.8 Hz hold, never past its gate. And the hold never rejects what acquisition would take: the same
 * tone from this window under the tone-error gate, or, while this window is one steady tone that misses on frequency
 * alone and no other tone qualified, from a late window.
 */
static int
ctcss_hold_on_tone(dsd_analog_ctcss* det, dsd_analog_ctcss_hop* own, double own_off, int other, int present) {
    const int k = det->locked;
    const double hold_hz =
        fmin(det->tables.gate_hz[k], fmax(k_snap_hz, det->lock_offset_hz + (k_snap_hz - k_acquire_snap_hz)));
    if (own_off <= hold_hz || (own->snapped == k && own_off <= ctcss_tone_error_gate_hz(det, own))) {
        return 1;
    }
    return !other && present && ctcss_late_same_tone_holds(det, own, own_off);
}

/* Whether the on-value rules would still hold this lock: their own gate, the same presence, and no rival. Once they
   would have lost it, the lock rests on tone error alone again, and yields to any tone they confirm. */
static void
ctcss_track_main_hold(dsd_analog_ctcss* det, double own_off, int present, int rival) {
    const int main_holds = own_off <= det->tables.on_value_gate_hz[det->locked] && present && !rival;
    det->main_fail_run = main_holds ? 0 : det->main_fail_run + 1;
    if (det->main_confirmed && det->main_fail_run >= DSD_ANALOG_CTCSS_LOSE_HOPS) {
        /* Where the on-value rules lose it, as a loss of theirs: their late windows start over. */
        det->main_confirmed = 0;
        det->main_lost = 1;
        det->main_none = 1;
        det->main_fresh = 0;
    }
}

static void
ctcss_step_locked(dsd_analog_ctcss* det, dsd_analog_ctcss_hop* hop) {
    if (ctcss_locked_preempted(det)) {
        return;
    }
    const int k = det->locked;
    const int other = det->cand.index >= 0 && det->cand.index != k;
    /* A rival is another tone this hop qualified on its value, as the on-value rules count it. One that rests on tone
       error alone neither fails the hold nor takes the lock: a lock is only ever handed to a candidate the on-value
       rules would lock (issue #643). */
    const int rival = other && det->cand.narrow_run >= 1;
    if (det->cand.index == k && ctcss_cand_ready(det, &det->cand)) {
        det->lock_offset_hz = fmax(det->lock_offset_hz, fabs(det->cand.hz - ctcss_tone_hz(k)));
    }
    /* The locked bin's own estimate, every hop: once locked, a tone is only held while it still
       sits on the table value it locked to. Without this an off-table tone that locked on one
       noisy pair of hops would keep reporting its neighbour for as long as it stayed coherent. */
    dsd_analog_ctcss_hop own;
    ctcss_measure_bin(det, DSD_ANALOG_CTCSS_WINDOW, k, &own);
    const double own_off = fabs(own.est_hz - ctcss_tone_hz(k));
    const double advance = ctcss_advance_for(det, k, det->locked_hz);
    hop->recent_rho = ctcss_rho(det, DSD_ANALOG_CTCSS_WINDOW, k, advance, DSD_ANALOG_CTCSS_WINDOW - 2, 2);
    const int present =
        hop->recent_rho >= k_hold_rho
        && ctcss_full_share(det, DSD_ANALOG_CTCSS_WINDOW, hop->recent_rho, DSD_ANALOG_CTCSS_WINDOW - 2, 2)
               >= k_min_full_share;
    const int on_tone = ctcss_hold_on_tone(det, &own, own_off, other, present);
    ctcss_track_main_hold(det, own_off, present, rival);
    if (on_tone && present && !rival) {
        det->fail_run = 0;
        /* The frequency the reverse-burst check and the hold's presence measure from follows the estimate as the
           on-value rules move it: a hop that only the wider hold keeps leaves it where they left it, while the lock
           is theirs. */
        if (own_off <= det->tables.on_value_gate_hz[k] || !det->main_confirmed) {
            det->locked_hz = own.est_hz;
        }
        return;
    }
    if (++det->fail_run >= DSD_ANALOG_CTCSS_LOSE_HOPS) {
        ctcss_unlock(det);
    }
}

/* Whether the 250 ms window qualifies a tone this hop: the on-value winner if the harmonic test passes it, else the
   window's best tone-error qualifier if that passes (issue #643). Leaves in @p hop the hop that qualified, or the
   window's winner when none did. */
static int
ctcss_window_qualifies(const dsd_analog_ctcss* det, dsd_analog_ctcss_hop* hop, dsd_analog_ctcss_hop* alt) {
    if (det->holdoff > 0) {
        return 0;
    }
    if (ctcss_hop_qualifies_on_value(hop, k_acquire_rho)) {
        if (ctcss_passes_harmonic(det, DSD_ANALOG_CTCSS_WINDOW, hop, k_max_harmonic_ratio)) {
            return 1;
        }
        if (det->wide_holdoff == 0 && alt->evaluated
            && ctcss_passes_harmonic(det, DSD_ANALOG_CTCSS_WINDOW, alt, k_max_harmonic_ratio)) {
            *hop = *alt;
            return 1;
        }
        return 0;
    }
    return det->wide_holdoff == 0 && ctcss_hop_qualifies_tone_error(det, hop, k_acquire_rho)
           && ctcss_passes_harmonic(det, DSD_ANALOG_CTCSS_WINDOW, hop, k_max_harmonic_ratio);
}

static void
ctcss_evaluate_hop(dsd_analog_ctcss* det, int freeze) {
    dsd_analog_ctcss_hop hop;
    dsd_analog_ctcss_hop alt;
    DSD_MEMSET(&hop, 0, sizeof(hop));
    ctcss_measure(det, DSD_ANALOG_CTCSS_WINDOW, k_acquire_rho, &hop, &alt);
    if (!freeze) {
        const int qualified = ctcss_window_qualifies(det, &hop, &alt);
        ctcss_track(&det->cand, qualified, &hop);
        if (det->state == DSD_ANALOG_TONE_STATE_LOCKED) {
            ctcss_step_locked(det, &hop);
        } else {
            ctcss_step_unlocked(det);
        }
        ctcss_step_late(det);
        if (det->holdoff > 0) {
            det->holdoff--;
        }
        if (det->wide_holdoff > 0) {
            det->wide_holdoff--;
        }
    }
    det->last_hop = hop;
}

/* The hop this sub-block closes may change the verdict unless everything its hold test reads --
   the newest 100 ms, two sub-blocks -- arrived inside the carrier hangover. Judged by the sample
   that closes the hop instead, a carrier that keeps dropping out for less than the hangover would
   hold a verdict for good once its openings missed every hop's end. A dropout the hangover allows
   (under 200 ms) leaves at most three hops in a row with nothing else in their newest 100 ms, so
   the verdict is re-examined at least every fourth hop. */
static void
ctcss_close_subblock(dsd_analog_ctcss* det) {
    const int freeze = !det->sub_open && !det->prev_open;
    det->prev_open = det->sub_open;
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
    det->sub_open = 0;
    det->ring_head = (det->ring_head + 1) % DSD_ANALOG_CTCSS_LONG_WINDOW;
    if (det->ring_count < DSD_ANALOG_CTCSS_LONG_WINDOW) {
        det->ring_count++;
    }
    if (det->fresh < DSD_ANALOG_CTCSS_LONG_WINDOW) {
        det->fresh++;
    }
    if (det->main_fresh < DSD_ANALOG_CTCSS_LONG_WINDOW) {
        det->main_fresh++;
    }
    if (det->ring_count >= DSD_ANALOG_CTCSS_WINDOW) {
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
            det->sub_open = 1;
        }
        if (++det->sub_fill >= det->sub_len) {
            ctcss_close_subblock(det);
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
        out->off_value = det->main_confirmed ? 0 : 1;
    }
}

const dsd_analog_rx_detector_ops dsd_analog_ctcss_ops = {
    "ctcss", ctcss_configure, ctcss_reset, ctcss_process, ctcss_report,
};

const dsd_analog_ctcss_hop*
dsd_analog_ctcss_last_hop(const dsd_analog_ctcss* det) {
    return det ? &det->last_hop : NULL;
}

void
dsd_analog_ctcss_measure_span(const dsd_analog_ctcss* det, int span, int bin, dsd_analog_ctcss_hop* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->best_index = -1;
    out->snapped = -1;
    out->est_var_hz2 = -1.0;
    out->stationary = -1;
    if (!det || det->sub_len <= 0 || span < 3 || span > DSD_ANALOG_CTCSS_LONG_WINDOW || span > det->ring_count
        || bin < 0 || bin >= DSD_CTCSS_TONE_COUNT) {
        return;
    }
    ctcss_measure_bin(det, span, bin, out);
    out->stationary = ctcss_window_stationary(det, span, bin);
    out->est_var_hz2 = ctcss_estimate_variance(det, span, bin);
}
