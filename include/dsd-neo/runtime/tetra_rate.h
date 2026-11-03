// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The DSP rates TETRA decodes at, and the advice for a rate it cannot.
 *
 * TETRA is pi/4-DQPSK at 36 kbit/s, 18 ksym/s (EN 300 392-2 clause 5.3). The CQPSK timing loop its symbols come from
 * counts whole samples per symbol, so TETRA runs only where the DSP (demodulator) rate is a multiple of 18 kHz from
 * 36 to 144 kHz: the 72 kHz DSP bandwidth (4 samples per symbol) on a live radio, an Airspy included (issue #806). The
 * stream start warns about any other rate, and so does a switch to TETRA at run time, with the fix for the input in
 * force.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TETRA_RATE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TETRA_RATE_H_

#include <dsd-neo/core/opts_fwd.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief TETRA's symbol rate in Hz. */
#define DSD_TETRA_SYMBOL_RATE_HZ        18000
/** @brief The DSP bandwidth in kHz TETRA takes when nothing else names one (-fT, a Qt TETRA system): 4 samples per
 *  symbol. */
#define DSD_TETRA_DSP_BW_KHZ            72
/** @brief Buffer size that holds every dsd_tetra_dsp_rate_advice() text untruncated. */
#define DSD_TETRA_RATE_ADVICE_MAX       224
/** @brief Buffer size that holds every brief dsd_tetra_dsp_rate_advice() text, which a toast leaves room for. */
#define DSD_TETRA_RATE_ADVICE_BRIEF_MAX 96

/** @brief The lowest DSP rate in Hz TETRA runs at: 2 samples per symbol. */
#define DSD_TETRA_DSP_RATE_MIN_HZ       (2 * DSD_TETRA_SYMBOL_RATE_HZ)
/** @brief The highest DSP rate in Hz TETRA runs at: 8 samples per symbol. */
#define DSD_TETRA_DSP_RATE_MAX_HZ       (8 * DSD_TETRA_SYMBOL_RATE_HZ)

/**
 * @brief Return 1 when TETRA's symbols can be timed at a DSP rate of @p rate_hz: a multiple of 18 kHz from 36 to
 * 144 kHz (DSD_TETRA_DSP_RATE_MIN_HZ to DSD_TETRA_DSP_RATE_MAX_HZ).
 *
 * Inline so the I/Q replay planner, which sits below the runtime library, applies the same domain.
 */
static inline int
dsd_tetra_dsp_rate_runs(int rate_hz) {
    return (rate_hz >= DSD_TETRA_DSP_RATE_MIN_HZ && rate_hz <= DSD_TETRA_DSP_RATE_MAX_HZ
            && rate_hz % DSD_TETRA_SYMBOL_RATE_HZ == 0)
               ? 1
               : 0;
}

/**
 * @brief Say what to change when TETRA runs at a DSP rate it cannot time its symbols at.
 *
 * On a live radio (an RTL-SDR, rtl_tcp, SoapySDR or Airspy input, dsd_input_dsp_rate_policy()) the DSP rate is the DSP
 * bandwidth, so the fix is the 72 kHz bandwidth, which dsd-neo never sets on its own: the full text, for the log, says
 * so and names it in the input's spec form ("TETRA needs a 72 kHz DSP bandwidth, not 48 kHz; dsd-neo does not change it
 * on its own, so set it to 72 kHz (-i airspy:freq:72)"); the brief one fits a toast after "Decoding TETRA: " ("needs a
 * 72 kHz DSP bandwidth; it stays 48 kHz until you set it"). An I/Q replay re-plans to a TETRA rate when it can (issue
 * #701), so its text says its capture cannot be resampled to one, and the brief one which way the rate is wrong ("DSP
 * rate 162 kHz is outside 36 to 144 kHz, and this replay cannot be resampled to one").
 *
 * @param opts     Decoder options naming the input; NULL reads as an RTL-SDR.
 * @param rate_hz  The DSP rate TETRA runs at, in Hz; 0 or less is not known yet, which no advice is written for.
 * @param brief    Nonzero for the brief text, which DSD_TETRA_RATE_ADVICE_BRIEF_MAX holds.
 * @param out      Receives the advice, or an empty string when the rate runs TETRA or is not known.
 * @param out_size Size of @p out; DSD_TETRA_RATE_ADVICE_MAX holds every text.
 * @return 1 when advice was written, 0 when there is none, -1 on a NULL or empty buffer.
 */
int dsd_tetra_dsp_rate_advice(const dsd_opts* opts, int rate_hz, int brief, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TETRA_RATE_H_ */
