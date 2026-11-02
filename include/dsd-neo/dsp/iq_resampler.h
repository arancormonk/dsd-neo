// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Streaming complex (I/Q) rational resampler for replayed captures (issue #701) and the live rate step (#806).
 *
 * A replayed capture whose rate cannot reach the mode's DSP rate through the half-band chain is resampled first, to a
 * rate R that can. Interleaved I/Q floats go in at in_hz and leave at out_hz = R, through L/M from
 * dsd_rate_converter_ratio() (exact where both reduced terms fit 4096, otherwise within 250 ppm of the true ratio).
 *
 * A live device that forces its own rate (an Airspy, say) leaves the half-band chain at a rate that is a whole number
 * of Hz over a power of two (2.5 MS/s over 2^6 is 39062.5 Hz). dsd_iq_resampler_configure_ratio() takes that rate as
 * the exact fraction, with an L/M the caller already worked out and the edges spelled out, so the rates and the delay
 * the resampler reports are the real ones. The design is the same: a sinc at the same edges relative to the rate gives
 * the same taps at any power-of-two scale of the rates.
 *
 * The filter is a Kaiser-windowed sinc, run as an L-phase polyphase bank. It is designed from explicit edges:
 *
 * - protect_hz (P) is the band the decoder needs, at the final DSP rate D after the half-band chain: 0.45 x D.
 * - The passband edge is Pe = min(P, 0.45 x in_hz). A capture holds nothing past its own Nyquist, so when upsampling a
 *   narrow capture (48254 Hz to 72000 Hz, say) the passband stops at 0.45 of the input rate.
 * - The stopband begins at S = min(in_hz - Pe, out_hz - P). Input content at out_hz - P or above would fold into +-P
 *   at the output rate, and the half-band chain passes +-P unchanged, so it would land in the protected band. The input's
 *   own images start at in_hz - Pe. Decimating, the first term is the larger one, so S = out_hz - P (1.1196 MHz for
 *   2.048 MS/s to 1.152 MS/s with D = 72 kHz). Upsampling to D, the first term is the smaller one, so S = in_hz - Pe,
 *   where the first image of the passband edge lands inside +-P (26.54 kHz for 48254 Hz to 72 kHz). Content between
 *   Pe and S, and aliases that fold outside +-P, are left to the half-band chain and the channel filter, which remove
 *   them as they do live.
 * - The stopband is at least DSD_IQ_RESAMPLER_STOPBAND_DB down and the passband ripple within
 *   DSD_IQ_RESAMPLER_RIPPLE_DB. The taps per phase come from the Kaiser estimate, rounded up to a multiple of 4.
 *
 * I and Q share one phase and one set of taps, so the two never mix. The output depends only on the samples fed since
 * the last clear, never on how they were cut into calls.
 *
 * One thread at a time; a resampler is plain storage the caller owns. Configuring one resampler never touches another,
 * so a caller can design a candidate next to the one it runs and swap them with dsd_iq_resampler_swap() when it
 * commits.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_DSP_IQ_RESAMPLER_H
#define DSD_NEO_INCLUDE_DSD_NEO_DSP_IQ_RESAMPLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The stopband attenuation the filter meets at least, in dB. */
#define DSD_IQ_RESAMPLER_STOPBAND_DB        60.0
/** The passband ripple, peak to peak, the filter stays within up to the passband edge, in dB. */
#define DSD_IQ_RESAMPLER_RIPPLE_DB          0.1
/** The passband edge never goes past this fraction of the input rate. */
#define DSD_IQ_RESAMPLER_MAX_PASS_FRACTION  0.45
/** The most taps per phase a design may need. */
#define DSD_IQ_RESAMPLER_MAX_TAPS_PER_PHASE 256
/** The largest bank, in taps (L x taps per phase): 4 MB of floats. */
#define DSD_IQ_RESAMPLER_MAX_BANK_TAPS      (4096 * 256)
/** The largest ratio error accepted, in ppm. */
#define DSD_IQ_RESAMPLER_MAX_PPM            250.0
/** The largest L or M dsd_iq_resampler_configure_ratio() takes. */
#define DSD_IQ_RESAMPLER_MAX_RATIO_TERM     4096
/** The largest power-of-two shift of the input rate dsd_iq_resampler_configure_ratio() takes. */
#define DSD_IQ_RESAMPLER_MAX_RATE_SHIFT     30
/** How far, in ppm, an approximated L/M may leave the output rate under the target. The live rate planner accepts a
    convergent this close when the exact terms pass DSD_IQ_RESAMPLER_MAX_RATIO_TERM, and configure_ratio() lets the
    output fall this far short of its stopband edge plus the protected band. */
#define DSD_IQ_RESAMPLER_MAX_APPROX_PPM     5.0

/** Why dsd_iq_resampler_configure() or dsd_iq_resampler_configure_ratio() refused. */
typedef enum {
    DSD_IQ_RESAMPLER_OK = 0,
    /** A NULL resampler, a rate or protected band of 0 or less, or a max_in of 0; for configure_ratio(), also an L or M
        outside 1..DSD_IQ_RESAMPLER_MAX_RATIO_TERM or not in lowest terms, a stopband edge of 0 or less, or a rate
        numerator past INT32_MAX or shift past DSD_IQ_RESAMPLER_MAX_RATE_SHIFT. */
    DSD_IQ_RESAMPLER_ERR_ARGS = -1,
    /** dsd_rate_converter_ratio() has no ratio for the rates, or its error is past DSD_IQ_RESAMPLER_MAX_PPM. */
    DSD_IQ_RESAMPLER_ERR_RATIO = -2,
    /** The protected band leaves no transition band: the stopband edge is at or below the passband edge. For
        configure_ratio(), also a protected band past DSD_IQ_RESAMPLER_MAX_PASS_FRACTION of the input rate, or a
        stopband edge past the input rate less the protected band, or past the output rate less the protected band by
        more than DSD_IQ_RESAMPLER_MAX_APPROX_PPM. */
    DSD_IQ_RESAMPLER_ERR_BAND = -3,
    /** The transition band is too narrow: the design needs more than DSD_IQ_RESAMPLER_MAX_TAPS_PER_PHASE taps per
        phase, or more than DSD_IQ_RESAMPLER_MAX_BANK_TAPS in all. */
    DSD_IQ_RESAMPLER_ERR_TAPS = -4,
    /** The bank or the history could not be allocated. */
    DSD_IQ_RESAMPLER_ERR_ALLOC = -5,
} dsd_iq_resampler_status;

/** A streaming I/Q resampler: initialise it with dsd_iq_resampler_init(), release it with dsd_iq_resampler_free(). */
typedef struct dsd_iq_resampler {
    int configured;
    int in_hz;  /**< The input rate; 0 after configure_ratio() when it is not a whole number of Hz. */
    int out_hz; /**< The output rate asked for; after configure_ratio(), in_hz x L / M, or 0 when that is not whole. */
    int L;
    int M;
    int taps_per_phase;
    int64_t in_rate_num;    /**< The exact input rate is in_rate_num / 2^in_rate_shift Hz, kept in lowest terms. */
    unsigned in_rate_shift; /**< 0 after configure(). */
    int keyed;      /**< 1 after configure_ratio(): L, M, the input rate and the two edges below are its design key. */
    int protect_hz; /**< configure_ratio()'s protected band edge; 0 otherwise. */
    int stop_hz;    /**< configure_ratio()'s stopband edge; 0 otherwise. */
    double passband_hz; /**< Pe, at the input */
    double stopband_hz; /**< S, at the input */
    size_t max_in;      /**< Complex inputs filtered per pass; longer calls are cut into passes. */
    int phase;          /**< The next output's offset in the L-times-upsampled stream, from the newest input. */
    float* bank;        /**< L phases of taps_per_phase taps, each phase in reverse so it lines up with the history. */
    float* work;        /**< Interleaved I/Q: taps_per_phase - 1 inputs of history, then up to max_in new ones. */
} dsd_iq_resampler;

/** Initialise @p r, unconfigured. Safe on any storage. */
void dsd_iq_resampler_init(dsd_iq_resampler* r);

/** Release @p r's bank and history; it is unconfigured again. */
void dsd_iq_resampler_free(dsd_iq_resampler* r);

/**
 * @brief Design @p r to resample from @p in_hz to @p out_hz, protecting +-@p protect_hz.
 *
 * Any earlier design is released first, so a refused configure leaves @p r unconfigured. A designed resampler starts
 * cleared.
 *
 * @param max_in_complex The complex inputs filtered per pass, which sizes the history buffer. Calls may pass more.
 * @return DSD_IQ_RESAMPLER_OK, or a negative dsd_iq_resampler_status.
 */
int dsd_iq_resampler_configure(dsd_iq_resampler* r, int in_hz, int out_hz, int protect_hz, size_t max_in_complex);

/**
 * @brief Design @p r for a ratio @p L / @p M the caller chose, at an input rate of @p in_rate_num / 2^@p in_rate_shift
 *        Hz, protecting +-@p protect_hz with the stopband from @p stop_hz.
 *
 * The filter is designed as configure()'s is, from these edges as given: the passband edge is @p protect_hz and the
 * stopband edge @p stop_hz. The edges must leave a transition band, the protected band must stay within
 * DSD_IQ_RESAMPLER_MAX_PASS_FRACTION of the input rate, and @p stop_hz + @p protect_hz must not pass the input rate
 * (the input's first image) or the output rate (where content folds into the protected band), except that the output
 * may fall short by DSD_IQ_RESAMPLER_MAX_APPROX_PPM, so an approximated L/M a few ppm under the target still designs:
 * the stopband edge then folds a fraction of a Hz into the protected band, still 60 dB down. The live step passes
 * 0.45 x D and D - 0.45 x D for a DSP rate D, the same edges configure(in, D, 0.45 x D) picks when decimating, so at
 * a whole input rate the two designs match bit for bit, and at a fractional one the design matches configure() at the
 * rates scaled by 2^@p in_rate_shift.
 *
 * Any earlier design of @p r is released first, so a refused configure leaves @p r unconfigured; no other resampler is
 * touched. A designed resampler starts cleared.
 *
 * @param L, M The ratio in lowest terms, each 1..DSD_IQ_RESAMPLER_MAX_RATIO_TERM.
 * @param in_rate_num The input rate's numerator in Hz, 1..INT32_MAX.
 * @param in_rate_shift The power of two the numerator is divided by, 0..DSD_IQ_RESAMPLER_MAX_RATE_SHIFT.
 * @param max_in_complex As for configure().
 * @return DSD_IQ_RESAMPLER_OK, or a negative dsd_iq_resampler_status.
 */
int dsd_iq_resampler_configure_ratio(dsd_iq_resampler* r, int L, int M, int64_t in_rate_num, unsigned in_rate_shift,
                                     int protect_hz, int stop_hz, size_t max_in_complex);

/**
 * @brief Whether @p r holds the configure_ratio() design these arguments would make.
 *
 * The input rate compares as a value, so 78125 / 2^0 matches 156250 / 2^1. max_in is not part of the design. A
 * resampler that configure() designed, or none, matches nothing.
 *
 * @return 1 on a match, else 0.
 */
int dsd_iq_resampler_matches(const dsd_iq_resampler* r, int L, int M, int64_t in_rate_num, unsigned in_rate_shift,
                             int protect_hz, int stop_hz);

/** Exchange @p a and @p b whole: design, history and phase. Nothing is allocated or freed. Either NULL does nothing. */
void dsd_iq_resampler_swap(dsd_iq_resampler* a, dsd_iq_resampler* b);

/** A short reason for a dsd_iq_resampler_status, for a log line. */
const char* dsd_iq_resampler_status_text(int status);

/** The most complex outputs @p in_complex inputs make, from any phase: ceil(in_complex x L / M). 0 unconfigured. */
size_t dsd_iq_resampler_max_out(const dsd_iq_resampler* r, size_t in_complex);

/**
 * @brief Resample @p in_complex interleaved I/Q pairs from @p in into @p out.
 *
 * Every input is consumed. @p in and @p out must not overlap.
 *
 * @param out_cap_complex The room in @p out, in complex samples: at least dsd_iq_resampler_max_out(@p in_complex).
 * @return The complex samples written, or -1 (unconfigured, a NULL buffer, too little room, or more outputs than an int
 *         holds), with nothing consumed.
 */
int dsd_iq_resampler_process(dsd_iq_resampler* r, const float* in, size_t in_complex, float* out,
                             size_t out_cap_complex);

/** Start over from silence: zero the history and the phase. A cleared resampler matches a freshly configured one. */
void dsd_iq_resampler_clear(dsd_iq_resampler* r);

/** How far L/M is from out_hz/in_hz: ((L / M) / (out_hz / in_hz) - 1) x 1e6. 0 unconfigured, and 0 after
    configure_ratio(), whose L/M is the ratio asked for. */
double dsd_iq_resampler_ratio_error_ppm(const dsd_iq_resampler* r);

/** The exact input rate, in Hz: in_rate_num / 2^in_rate_shift. 0 unconfigured. */
double dsd_iq_resampler_in_rate_hz(const dsd_iq_resampler* r);

/** The filter's group delay, in input samples: (L x taps_per_phase - 1) / (2 x L). 0 unconfigured. */
double dsd_iq_resampler_group_delay_inputs(const dsd_iq_resampler* r);

/** The filter's group delay, in seconds: the delay in input samples over the exact input rate. 0 unconfigured. */
double dsd_iq_resampler_group_delay_seconds(const dsd_iq_resampler* r);

/** How long the filter rings after its last input, in input samples: taps_per_phase - 1, the history it keeps. Feeding
    that many more inputs (silence, say) flushes every output the real input contributes to. 0 unconfigured. */
size_t dsd_iq_resampler_tail_inputs(const dsd_iq_resampler* r);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_DSP_IQ_RESAMPLER_H */
