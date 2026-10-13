// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The DSP rates TETRA decodes at, and the advice for a rate it cannot.
 *
 * TETRA is pi/4-DQPSK at 36 kbit/s, 18 ksym/s (EN 300 392-2 clause 5.3). The CQPSK timing loop its symbols come from
 * counts whole samples per symbol, so TETRA runs only where the DSP (demodulator) rate is a multiple of 18 kHz: the
 * 72 kHz RTL DSP bandwidth (4 samples per symbol) on a live radio. The stream start warns about any other rate, and so
 * does a switch to TETRA at run time, with the fix for the input in force.
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

/** @brief Return 1 when TETRA's symbols can be timed at a DSP rate of @p rate_hz: a positive multiple of 18 kHz. */
int dsd_tetra_dsp_rate_runs(int rate_hz);

/**
 * @brief Say what to change when TETRA runs at a DSP rate it cannot time its symbols at.
 *
 * The text names the rate and the fix for the input @p opts names (dsd_opts_analog_rate_source()): an RTL-SDR or
 * rtl_tcp input's 72 kHz DSP bandwidth in its spec form, a SoapySDR device's 72 kHz DSP bandwidth on a device that
 * delivers 1.152 MS/s, that no Airspy sample rate halves to a multiple of 18 kHz, or that an I/Q replay keeps its
 * capture's rate. The full text is for the log; the brief one ("DSP rate 48 kHz is no multiple of 18 kHz; set the DSP
 * bandwidth to 72 kHz") fits a toast.
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
