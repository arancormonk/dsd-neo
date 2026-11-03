// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The RadioReference import's receiver verdict (issue #803): whether the session can take an import and decode the
 * mode it selects. Pure: plain facts in, a verdict out, plus the two helpers that read those facts off a dsd_opts and
 * ask the next-start rule. No stream, engine or I/O, so the decoder thread's preflight and the frontends' pre-check
 * run the same code (rr_apply_verdict.h).
 */

#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/tetra_rate.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "next_start_check.h"
#include "rr_apply_verdict.h"

/* The reasons, as phrases a toast puts after "Failed: RR import -> " (and a frontend frames as it likes). Each fits an
   80-column status line with that prefix. */
static const char k_rr_why_trunk_scan[] = "stop the trunk scan first; it keeps its own channel maps";
static const char k_rr_why_am_audio[] = "AM needs an I/Q radio input";
static const char k_rr_why_tetra_audio[] = "TETRA needs an I/Q radio input or a symbol capture";
static const char k_rr_why_tetra_uplink[] = "TETRA uplink session; restart without --tetra-uplink";
static const char k_rr_why_tetra_rtl_bw[] = "TETRA needs 72 kHz DSP bandwidth; set it and import again";

static int
rr_refuse(char* why, size_t why_size, const char* text) {
    if (why && why_size > 0U) {
        DSD_SNPRINTF(why, why_size, "%s", text);
    }
    return 1;
}

/* Inputs that hand the decoder I/Q samples, which AM and TETRA need. */
static int
rr_input_is_iq(int input_kind) {
    return input_kind == DSD_APP_RR_INPUT_RTL || input_kind == DSD_APP_RR_INPUT_SOAPY
           || input_kind == DSD_APP_RR_INPUT_AIRSPY || input_kind == DSD_APP_RR_INPUT_IQ_REPLAY;
}

/* TETRA by input (the verdict table of issue #803). */
static int
rr_tetra_verdict(const dsd_app_rr_receiver_facts* f, char* why, size_t why_size) {
    if (!rr_input_is_iq(f->input_kind) && f->input_kind != DSD_APP_RR_INPUT_SYMBOLS) {
        return rr_refuse(why, why_size, k_rr_why_tetra_audio);
    }
    if (f->tetra_uplink) {
        return rr_refuse(why, why_size, k_rr_why_tetra_uplink);
    }
    switch (f->input_kind) {
        case DSD_APP_RR_INPUT_RTL:
        case DSD_APP_RR_INPUT_SOAPY:
        case DSD_APP_RR_INPUT_AIRSPY:
            /* The DSP bandwidth is the rate on every live radio (a SoapySDR or Airspy device's capture rate is stepped
               to it, issue #806), and 72 kHz the one TETRA is set up for (-fT). Changing it here would reopen the
               radio, which restarts an --iq-capture from its start, so the operator sets it. */
            return f->rtl_dsp_bw_khz == DSD_TETRA_DSP_BW_KHZ ? 0 : rr_refuse(why, why_size, k_rr_why_tetra_rtl_bw);
        default: return 0; /* an I/Q replay re-plans to a TETRA rate or refuses; a symbol capture has no rate */
    }
}

int
dsd_app_rr_receiver_verdict(const dsd_app_rr_receiver_facts* facts, char* why, size_t why_size) {
    if (why && why_size > 0U) {
        why[0] = '\0';
    }
    if (!facts) {
        return 1;
    }
    if (facts->trunk_scan_running) {
        /* A trunk scan gets its channel maps per target: refusing the whole import keeps a half-applied one off it. */
        return rr_refuse(why, why_size, k_rr_why_trunk_scan);
    }
    if (facts->decode_mode == (int32_t)DSDCFG_MODE_AM && !rr_input_is_iq(facts->input_kind)) {
        return rr_refuse(why, why_size, k_rr_why_am_audio);
    }
    if (facts->decode_mode == (int32_t)DSDCFG_MODE_TETRA) {
        return rr_tetra_verdict(facts, why, why_size);
    }
    return 0;
}

/* The input as the decode-mode input check sees it (dsd_decode_mode_runs_on_input()): a radio input is I/Q, a symbol
   capture is symbols, anything else is audio. A replay is the replay spec in force (dsd_opts_input_is_iq_replay()), as
   every other import check asks it, never dsd_opts::iq_replay_active, which an input switch does not clear; among the
   other radio specs the device sets the rate (SoapySDR, Airspy), else the RTL DSP bandwidth does, as the stream start
   classifies them (detect_radio_source()). */
static int
rr_input_kind(const dsd_opts* opts) {
    if (opts->audio_in_type == AUDIO_IN_SYMBOL_BIN || opts->audio_in_type == AUDIO_IN_SYMBOL_FLT) {
        return DSD_APP_RR_INPUT_SYMBOLS;
    }
    if (!dsd_opts_input_is_radio(opts)) {
        return DSD_APP_RR_INPUT_AUDIO;
    }
    if (dsd_opts_input_is_iq_replay(opts)) {
        return DSD_APP_RR_INPUT_IQ_REPLAY;
    }
    if (dsd_opts_audio_in_dev_is_airspy_spec(opts->audio_in_dev)) {
        return DSD_APP_RR_INPUT_AIRSPY;
    }
    return dsd_opts_audio_in_dev_is_soapy_spec(opts->audio_in_dev) ? DSD_APP_RR_INPUT_SOAPY : DSD_APP_RR_INPUT_RTL;
}

void
rr_apply_receiver_facts_fill(const dsd_opts* opts, const dsd_app_rr_apply_payload* p, int has_group, int has_chan,
                             uint32_t actual_rate_hz, dsd_app_rr_receiver_facts* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    if (!opts || !p) {
        return;
    }
    out->decode_mode = p->decode_mode;
    out->input_kind = rr_input_kind(opts);
    out->rtl_dsp_bw_khz = opts->rtl_dsp_bw_khz;
    out->actual_rate_hz = actual_rate_hz;
    out->tetra_uplink = opts->tetra_uplink ? 1 : 0;
    out->trunk_scan_running = opts->trunk_scan_enabled == 1 ? 1 : 0;
    out->has_group = has_group ? 1 : 0;
    out->has_chan = has_chan ? 1 : 0;
    /* Exactly one tuner owner, as the apply sets it (rr_apply_tuner_owner()). */
    out->trunking = p->trunking ? 1 : 0;
    out->scanner = (!p->trunking && p->scanner) ? 1 : 0;
}

/* What the import leaves (rr_apply_next_start_refusal()): the facts and the channel map it loads. */
typedef struct {
    const dsd_app_rr_receiver_facts* facts;
    const char* chan_path;
} rr_next_start_change;

/* The tuner owner the import sets and the channel map it loads or clears (rr_apply_files()). A frontend asks before
   the file exists; the rules only ask whether there is one. */
static void
rr_next_start_apply(dsdneoUserConfig* save, const void* ctx) {
    const rr_next_start_change* change = (const rr_next_start_change*)ctx;
    save->trunk_enabled = change->facts->trunking;
    save->trunk_scanner = change->facts->scanner;
    const char* chan =
        (change->chan_path && change->chan_path[0] != '\0') ? change->chan_path : "radioreference import";
    DSD_SNPRINTF(save->trunk_chan_csv, sizeof save->trunk_chan_csv, "%s", change->facts->has_chan ? chan : "");
}

int
rr_apply_next_start_refusal(const dsd_opts* opts, const dsd_state* state, const dsd_app_rr_receiver_facts* facts,
                            const char* chan_path, char* why, size_t why_size) {
    if (!facts) {
        if (why && why_size > 0U) {
            why[0] = '\0';
        }
        return -1;
    }
    const rr_next_start_change change = {facts, chan_path};
    return app_check_next_start(opts, state, rr_next_start_apply, &change, why, why_size);
}
