// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * TETRA DMO scan rows (issue #890): whether one can be visited, and the TETRA state's lifetime across a scan session's
 * landings. See scan_tetra_internal.h.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/scan_entry.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/tetra/tetra.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/tetra_rate.h>
#include <stddef.h>

#include "scan_analog_internal.h"
#include "scan_tetra_internal.h"

/* The DSP's TED rule for the TETRA chain (dsd_demod_tetra_chain_wanted(), the landing predicate
   dsd_trunk_scan_landing_ted_agrees()): the effective samples per symbol (an override, else the TED's own) and the
   TED's own must both be the rate's; 0 for none, judged by what there is. */
static int
scan_tetra_ted_agrees(int rate_hz, int override_sps, int applied_sps) {
    const int sps = rate_hz / DSD_TETRA_SYMBOL_RATE_HZ;
    const int effective = override_sps > 0 ? override_sps : applied_sps;
    return (effective <= 0 || effective == sps) && (applied_sps <= 0 || applied_sps == sps);
}

dsd_engine_tetra_row
dsd_engine_scan_tetra_row_check(const dsd_opts* opts, const dsd_state* state, int rate_hz, int landed) {
    /* A radio DSD-neo tunes delivers the I/Q TETRA's chain runs on; audio input (a rigctl peer's included) never does,
       and a replay or a stream another program sends cannot be tuned to the row. */
    if (!dsd_opts_radio_can_tune(opts)) {
        return DSD_ENGINE_TETRA_ROW_INPUT;
    }
    if (opts->tetra_uplink) {
        return DSD_ENGINE_TETRA_ROW_UPLINK;
    }
    if (dsd_engine_scan_channel_lpf_forced_off()) {
        return DSD_ENGINE_TETRA_ROW_LPF;
    }
    if (rate_hz <= 0) {
        return DSD_ENGINE_TETRA_ROW_RATE_UNKNOWN;
    }
    if (!dsd_tetra_dsp_rate_runs(rate_hz)) {
        return DSD_ENGINE_TETRA_ROW_RATE;
    }
    if (landed) {
        if (dsd_engine_scan_tetra_chain_settled_other(opts, state)) {
            return DSD_ENGINE_TETRA_ROW_CHAIN;
        }
        int override_sps = 0;
        int applied_sps = 0;
        dsd_engine_scan_tetra_ted_sps(opts, state, &override_sps, &applied_sps);
        if (!scan_tetra_ted_agrees(rate_hz, override_sps, applied_sps)) {
            return DSD_ENGINE_TETRA_ROW_TED;
        }
    }
    return DSD_ENGINE_TETRA_ROW_OK;
}

dsd_scan_entry_reason
dsd_engine_scan_tetra_entry_reason(dsd_engine_tetra_row why) {
    switch (why) {
        case DSD_ENGINE_TETRA_ROW_OK: return DSD_SCAN_ENTRY_OK;
        case DSD_ENGINE_TETRA_ROW_INPUT: return DSD_SCAN_ENTRY_TETRA_INPUT;
        case DSD_ENGINE_TETRA_ROW_RATE_UNKNOWN: return DSD_SCAN_ENTRY_TETRA_WAIT;
        case DSD_ENGINE_TETRA_ROW_RATE: return DSD_SCAN_ENTRY_TETRA_RATE;
        case DSD_ENGINE_TETRA_ROW_LPF:
        case DSD_ENGINE_TETRA_ROW_TED:
        case DSD_ENGINE_TETRA_ROW_UPLINK:
        case DSD_ENGINE_TETRA_ROW_CHAIN: return DSD_SCAN_ENTRY_TETRA_CHAIN;
    }
    return DSD_SCAN_ENTRY_TETRA_CHAIN;
}

/* The status-line or log text of every reason but the rate's, which the rate advice words. */
static const char*
scan_tetra_row_reason_text(dsd_engine_tetra_row why, int brief) {
    switch (why) {
        case DSD_ENGINE_TETRA_ROW_INPUT:
            return brief ? "TETRA DMO needs a radio I/Q input"
                         : "TETRA DMO needs a radio I/Q input dsd-neo tunes (RTL-SDR, rtl_tcp, SoapySDR or Airspy); "
                           "audio input, a rigctl peer, an I/Q replay or an I/Q stream cannot run it";
        case DSD_ENGINE_TETRA_ROW_RATE_UNKNOWN:
            return brief ? "TETRA DMO waits for the radio's DSP rate"
                         : "TETRA DMO waits for the radio to publish its DSP rate (the stream is opening)";
        case DSD_ENGINE_TETRA_ROW_LPF:
            return brief ? "TETRA DMO needs the filter DSD_NEO_CHANNEL_LPF=0 turns off"
                         : "TETRA DMO needs the channel filter, which DSD_NEO_CHANNEL_LPF=0 turns off";
        case DSD_ENGINE_TETRA_ROW_TED:
            return brief ? "TETRA DMO's TED timing does not fit the DSP rate"
                         : "TETRA DMO needs the TED timed at the DSP rate over 18000 symbols/s, and the stream runs "
                           "another timing";
        case DSD_ENGINE_TETRA_ROW_UPLINK:
            return brief ? "TETRA DMO does not run with --tetra-uplink"
                         : "TETRA DMO decodes the downlink chain, which --tetra-uplink turns off";
        case DSD_ENGINE_TETRA_ROW_CHAIN:
            return brief ? "TETRA DMO's receive chain is not running"
                         : "TETRA DMO needs the CQPSK chain with the TETRA channel filter, and the stream runs another "
                           "receive profile";
        case DSD_ENGINE_TETRA_ROW_OK:
        case DSD_ENGINE_TETRA_ROW_RATE: break;
    }
    return "";
}

void
dsd_engine_scan_tetra_row_text(const dsd_opts* opts, dsd_engine_tetra_row why, int rate_hz, int brief, char* out,
                               size_t out_size) {
    if (!out || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    if (why != DSD_ENGINE_TETRA_ROW_RATE) {
        DSD_SNPRINTF(out, out_size, "%s", scan_tetra_row_reason_text(why, brief));
        return;
    }
    if (brief) {
        /* Short enough for the status line after the row's name: "TETRA DMO needs a 72 kHz DSP bandwidth, not 48 kHz";
           the log line beside it names the fix in the input's spec form. */
        char rate[DSD_ANALOG_WIDTH_TEXT_MAX];
        (void)dsd_analog_width_format(rate_hz, rate, sizeof rate);
        DSD_SNPRINTF(out, out_size, "TETRA DMO needs a %d kHz DSP bandwidth, not %s", DSD_TETRA_DSP_BW_KHZ, rate);
        return;
    }
    char advice[DSD_TETRA_RATE_ADVICE_MAX];
    if (dsd_tetra_dsp_rate_advice(opts, rate_hz, 0, advice, sizeof advice) != 1) {
        DSD_SNPRINTF(out, out_size, "%s", "TETRA DMO needs a TETRA DSP rate");
        return;
    }
    DSD_SNPRINTF(out, out_size, "%s", advice);
}

void
dsd_engine_scan_tetra_landing(dsd_opts* opts, dsd_state* state, int session_has_tetra) {
    if (!opts || !state) {
        return;
    }
    if (session_has_tetra) {
        state->tetra_ext_scan_held = 1U;
    }
    if (state->tetra_ext_scan_held) {
        tetra_scan_visit_reset(opts, state);
    }
}

void
dsd_engine_scan_tetra_release(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || !state->tetra_ext_scan_held) {
        return;
    }
    tetra_scan_visit_reset(opts, state);
    state->tetra_ext_scan_held = 0U;
    if (opts->frame_tetra != 1) {
        dsd_decode_mode_leave_tetra(opts, state);
    }
}
