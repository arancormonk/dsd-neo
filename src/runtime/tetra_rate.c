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

int
dsd_tetra_dsp_rate_runs(int rate_hz) {
    return (rate_hz > 0 && rate_hz % DSD_TETRA_SYMBOL_RATE_HZ == 0) ? 1 : 0;
}

/* The fix for TETRA at a rate it cannot run, by what sets the rate: the RTL DSP bandwidth (in the spec form of the input
   at hand), a SoapySDR or Airspy device, or an I/Q replay's capture. The brief form fits a toast. */
static const char*
tetra_rate_fix(const dsd_opts* opts, int brief) {
    const char* dev = opts ? opts->audio_in_dev : NULL;
    switch (dsd_opts_analog_rate_source(opts)) {
        case DSD_ANALOG_RATE_CAPTURE:
            return brief ? ", and a replay keeps its capture's rate"
                         : ": an I/Q replay keeps its capture's rate; capture TETRA at a 72 kHz DSP bandwidth";
        case DSD_ANALOG_RATE_DEVICE:
            if (dsd_opts_audio_in_dev_is_airspy_spec(dev)) {
                return brief ? ", and no Airspy rate is one"
                             : ": no Airspy sample rate halves to one; use an RTL-SDR at a 72 kHz DSP bandwidth "
                               "(-i rtl:dev:freq:gain:ppm:72)";
            }
            return brief ? "; set the DSP bandwidth to 72 kHz"
                         : ": set the DSP bandwidth to 72 kHz (-i soapy:args:freq:gain:ppm:72), which needs a device "
                           "that delivers 1.152 MS/s";
        default:
            if (brief) {
                return "; set the DSP bandwidth to 72 kHz";
            }
            return dsd_opts_audio_in_dev_is_rtltcp_spec(dev)
                       ? ": set the DSP bandwidth to 72 kHz (-i rtltcp:host:port:freq:gain:ppm:72)"
                       : ": set the DSP bandwidth to 72 kHz (-i rtl:dev:freq:gain:ppm:72)";
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
    if (brief) {
        DSD_SNPRINTF(out, out_size, "DSP rate %s is no multiple of 18 kHz%s", rate_text, tetra_rate_fix(opts, 1));
    } else {
        DSD_SNPRINTF(out, out_size, "TETRA needs a DSP rate that is a multiple of 18 kHz, not %s%s", rate_text,
                     tetra_rate_fix(opts, 0));
    }
    return 1;
}
