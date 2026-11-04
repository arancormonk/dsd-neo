// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The DSP rates TETRA decodes at, and the advice for a rate it cannot; see <dsd-neo/runtime/tetra_rate.h>.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/tetra_rate.h>
#include <stddef.h>
#include "dsd-neo/core/opts_fwd.h"

/* The input's own spec form, which a full live-radio advice names the fix in. */
static const char*
tetra_rate_spec_example(const dsd_opts* opts) {
    const char* dev = opts ? opts->audio_in_dev : NULL;
    if (dsd_opts_audio_in_dev_is_airspy_spec(dev)) {
        return "-i airspy:freq:72";
    }
    if (dsd_opts_audio_in_dev_is_soapy_spec(dev)) {
        return "-i soapy:args:freq:gain:ppm:72";
    }
    return dsd_opts_audio_in_dev_is_rtltcp_spec(dev) ? "-i rtltcp:host:port:freq:gain:ppm:72"
                                                     : "-i rtl:dev:freq:gain:ppm:72";
}

/* An I/Q replay re-plans its rate to TETRA's when TETRA comes on (issue #701), so only a capture no plan resamples to a
   TETRA rate reaches its text, which names the rate and the range. The brief form fits a toast. */
static void
tetra_rate_replay_advice(int rate_hz, const char* rate_text, int brief, char* out, size_t out_size) {
    if (brief) {
        /* Say which way the rate is wrong: a multiple of 18 kHz can still be out of range (18 or 162 kHz). */
        DSD_SNPRINTF(out, out_size,
                     rate_hz % DSD_TETRA_SYMBOL_RATE_HZ != 0
                         ? "DSP rate %s is no multiple of 18 kHz, and this replay cannot be resampled to one"
                         : "DSP rate %s is outside 36 to 144 kHz, and this replay cannot be resampled to one",
                     rate_text);
    } else {
        DSD_SNPRINTF(out, out_size,
                     "TETRA needs a DSP rate that is a multiple of 18 kHz from 36 to 144 kHz, not %s: this replay's "
                     "capture cannot be resampled to a TETRA rate",
                     rate_text);
    }
}

/* A live radio runs at its DSP bandwidth (an RTL-SDR's cascade lands on it, a SoapySDR or Airspy device is stepped to
   it, issue #806), so the fix is the 72 kHz bandwidth in the input's spec form. TETRA switched on at another one stays
   on but decodes nothing, and the bandwidth is the operator's to change: dsd-neo never moves it on its own, which the
   text says, so the operator does not wait for it. */
static void
tetra_rate_live_advice(const dsd_opts* opts, const char* rate_text, int brief, char* out, size_t out_size) {
    if (brief) {
        DSD_SNPRINTF(out, out_size, "needs a 72 kHz DSP bandwidth; it stays %s until you set it", rate_text);
    } else {
        DSD_SNPRINTF(out, out_size,
                     "TETRA needs a 72 kHz DSP bandwidth, not %s; dsd-neo does not change it on its own, so set it to "
                     "72 kHz (%s)",
                     rate_text, tetra_rate_spec_example(opts));
    }
}

int
dsd_tetra_dsp_rate_advice(const dsd_opts* opts, int rate_hz, int brief, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (rate_hz <= 0 || dsd_tetra_dsp_rate_runs(rate_hz)) {
        return 0;
    }
    char rate_text[DSD_ANALOG_WIDTH_TEXT_MAX];
    (void)dsd_analog_width_format(rate_hz, rate_text, sizeof rate_text);
    if (dsd_input_dsp_rate_policy(opts).source == DSD_ANALOG_RATE_CAPTURE) {
        tetra_rate_replay_advice(rate_hz, rate_text, brief, out, out_size);
    } else {
        tetra_rate_live_advice(opts, rate_text, brief, out, out_size);
    }
    return 1;
}
