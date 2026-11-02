// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The RadioReference import's receiver verdict (issue #803), with no stream, engine or link seam, so it runs on every
 * platform: the verdict table by input, the facts as the session fills them from dsd_opts, the next-start rule over
 * a snapshot pair, and the frontends' pre-check over stand-in published snapshots (a SoapySDR device's rate is
 * unknown there and passes; an RTL-SDR off 72 kHz does not).
 */

#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/app_control/rr_apply_verdict.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

/* The published snapshots dsd_app_rr_apply_block_reason() reads: src/app_control/ui_opts_snapshot.c is not compiled
   here, so these stand in, file-static (a full dsd_opts or dsd_state on the stack is ~41 KB and more). */
static dsd_opts g_snap_opts;
static dsd_state g_snap_state;
static int g_opts_published = 0;
static int g_state_published = 0;

const dsd_opts*
dsd_app_get_latest_opts_snapshot(void) {
    return g_opts_published ? &g_snap_opts : NULL;
}

const dsd_state*
dsd_app_get_latest_snapshot(void) {
    return g_state_published ? &g_snap_state : NULL;
}

/* "Failed: RR import -> " plus a fixed reason fits an 80-column status line after the footer's "| ". */
#define RR_TOAST_PREFIX_LEN 21U
#define RR_STATUS_COLUMNS   78U

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", tag, got, want);
        return 1;
    }
    return 0;
}

static dsd_app_rr_receiver_facts
facts_for(int32_t mode, int input_kind) {
    dsd_app_rr_receiver_facts f;
    DSD_MEMSET(&f, 0, sizeof f);
    f.decode_mode = mode;
    f.input_kind = input_kind;
    f.rtl_dsp_bw_khz = 48;
    return f;
}

/* One verdict: refused or not, and the reason (NULL when it passes). A fixed reason fits the status line. */
static int
expect_verdict(const char* tag, const dsd_app_rr_receiver_facts* f, const char* want_why, int fixed_text) {
    char why[DSD_APP_RR_VERDICT_WHY_MAX];
    DSD_SNPRINTF(why, sizeof why, "%s", "stale");
    const int got = dsd_app_rr_receiver_verdict(f, why, sizeof why);
    int rc = expect_int(tag, got, want_why ? 1 : 0);
    rc |= expect_str(tag, why, want_why ? want_why : "");
    if (want_why && fixed_text && RR_TOAST_PREFIX_LEN + strlen(why) > RR_STATUS_COLUMNS) {
        DSD_FPRINTF(stderr, "%s: \"%s\" does not fit the status line\n", tag, why);
        rc = 1;
    }
    return rc;
}

static const char k_tetra_rtl_bw[] = "TETRA needs 72 kHz DSP bandwidth; set it and import again";

/* The table of issue #803 §3.2, row by row. */
static int
test_verdict_tetra_table(void) {
    const int32_t tetra = (int32_t)DSDCFG_MODE_TETRA;
    int rc = 0;
    dsd_app_rr_receiver_facts f = facts_for(tetra, DSD_APP_RR_INPUT_RTL);
    f.rtl_dsp_bw_khz = 72;
    rc |= expect_verdict("rtl at 72 kHz", &f, NULL, 0);
    f.rtl_dsp_bw_khz = 48;
    rc |= expect_verdict("rtl at 48 kHz", &f, k_tetra_rtl_bw, 1);
    /* 36 kHz is a TETRA rate (2 samples per symbol), but the import holds the RTL family to the 72 kHz it is set up
       for, as -fT sets it. */
    f.rtl_dsp_bw_khz = 36;
    rc |= expect_verdict("rtl at 36 kHz", &f, k_tetra_rtl_bw, 1);
    /* A committed rate does not stand in for the bandwidth of an RTL-SDR: the bandwidth is its rate. */
    f.actual_rate_hz = 72000U;
    rc |= expect_verdict("rtl at 36 kHz with a rate", &f, k_tetra_rtl_bw, 1);

    f = facts_for(tetra, DSD_APP_RR_INPUT_AIRSPY);
    rc |= expect_verdict("airspy", &f, "an Airspy cannot decode TETRA yet; use RTL-SDR or rtl_tcp", 1);

    f = facts_for(tetra, DSD_APP_RR_INPUT_SOAPY);
    rc |= expect_verdict("soapy, rate unknown (a frontend)", &f, NULL, 0);
    f.actual_rate_hz = 72000U;
    rc |= expect_verdict("soapy at 72 kHz", &f, NULL, 0);
    f.actual_rate_hz = 144000U;
    rc |= expect_verdict("soapy at 144 kHz", &f, NULL, 0);
    f.actual_rate_hz = 48000U;
    rc |= expect_verdict("soapy at 48 kHz", &f,
                         "DSP rate 48 kHz is no multiple of 18 kHz; set the DSP bandwidth to 72 kHz", 0);
    f.actual_rate_hz = 162000U;
    rc |= expect_verdict("soapy at 162 kHz", &f,
                         "DSP rate 162 kHz is outside 36 to 144 kHz; set the DSP bandwidth to 72 kHz", 0);

    f = facts_for(tetra, DSD_APP_RR_INPUT_IQ_REPLAY);
    rc |= expect_verdict("replay", &f, NULL, 0);
    f = facts_for(tetra, DSD_APP_RR_INPUT_SYMBOLS);
    rc |= expect_verdict("symbol capture", &f, NULL, 0);
    f = facts_for(tetra, DSD_APP_RR_INPUT_AUDIO);
    rc |= expect_verdict("audio", &f, "TETRA needs an IQ radio input or a symbol capture", 1);

    f = facts_for(tetra, DSD_APP_RR_INPUT_RTL);
    f.rtl_dsp_bw_khz = 72;
    f.tetra_uplink = 1;
    rc |= expect_verdict("uplink", &f, "TETRA uplink session; restart without --tetra-uplink", 1);
    f.input_kind = DSD_APP_RR_INPUT_SYMBOLS;
    rc |= expect_verdict("uplink symbol capture", &f, "TETRA uplink session; restart without --tetra-uplink", 1);
    /* Audio is refused for what it is, whatever the uplink flag says. */
    f.input_kind = DSD_APP_RR_INPUT_AUDIO;
    rc |= expect_verdict("uplink audio", &f, "TETRA needs an IQ radio input or a symbol capture", 1);
    return rc;
}

static int
test_verdict_other_modes(void) {
    int rc = 0;
    dsd_app_rr_receiver_facts f = facts_for((int32_t)DSDCFG_MODE_AM, DSD_APP_RR_INPUT_AUDIO);
    rc |= expect_verdict("am on audio", &f, "AM needs an IQ radio input", 1);
    f.input_kind = DSD_APP_RR_INPUT_SYMBOLS;
    rc |= expect_verdict("am on symbols", &f, "AM needs an IQ radio input", 1);
    const int iq_inputs[] = {
        DSD_APP_RR_INPUT_RTL,
        DSD_APP_RR_INPUT_SOAPY,
        DSD_APP_RR_INPUT_AIRSPY,
        DSD_APP_RR_INPUT_IQ_REPLAY,
    };
    for (size_t i = 0; i < sizeof iq_inputs / sizeof iq_inputs[0]; i++) {
        f.input_kind = iq_inputs[i];
        rc |= expect_verdict("am on an iq input", &f, NULL, 0);
    }
    /* Only AM and TETRA care what the input is, or its rate. */
    f = facts_for((int32_t)DSDCFG_MODE_DMR, DSD_APP_RR_INPUT_AUDIO);
    f.tetra_uplink = 1;
    rc |= expect_verdict("dmr on audio", &f, NULL, 0);
    f.input_kind = DSD_APP_RR_INPUT_AIRSPY;
    rc |= expect_verdict("dmr on airspy", &f, NULL, 0);

    /* A running trunk scan refuses every import first. */
    f = facts_for((int32_t)DSDCFG_MODE_TETRA, DSD_APP_RR_INPUT_AUDIO);
    f.trunk_scan_running = 1;
    rc |= expect_verdict("trunk scan", &f, "stop the trunk scan first; it keeps its own channel maps", 1);
    f.decode_mode = (int32_t)DSDCFG_MODE_P25P1;
    rc |= expect_verdict("trunk scan p25", &f, "stop the trunk scan first; it keeps its own channel maps", 1);

    rc |= expect_int("null facts refused", dsd_app_rr_receiver_verdict(NULL, NULL, 0U), 1);
    f = facts_for((int32_t)DSDCFG_MODE_AM, DSD_APP_RR_INPUT_AUDIO);
    rc |= expect_int("null why still refuses", dsd_app_rr_receiver_verdict(&f, NULL, 0U), 1);
    return rc;
}

static void
set_input(dsd_opts* opts, int type, const char* dev) {
    opts->audio_in_type = type;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", dev);
}

static int
input_kind_of(dsd_opts* opts, int type, const char* dev) {
    dsd_app_rr_apply_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    set_input(opts, type, dev);
    dsd_app_rr_receiver_facts f;
    rr_apply_receiver_facts_fill(opts, &p, 0, 0, 0U, &f);
    return f.input_kind;
}

/* The input as the stream start classifies it, and the import's outputs. */
static int
test_facts_fill(void) {
    static dsd_opts opts;
    initOpts(&opts);
    int rc = 0;
    rc |= expect_int("pulse", input_kind_of(&opts, AUDIO_IN_PULSE, "pulse"), DSD_APP_RR_INPUT_AUDIO);
    rc |= expect_int("wav", input_kind_of(&opts, AUDIO_IN_WAV, "capture.wav"), DSD_APP_RR_INPUT_AUDIO);
    rc |= expect_int("rtl", input_kind_of(&opts, AUDIO_IN_RTL, "rtl:0:851.0125M"), DSD_APP_RR_INPUT_RTL);
    rc |= expect_int("rtl_tcp", input_kind_of(&opts, AUDIO_IN_RTL, "rtltcp:127.0.0.1:1234"), DSD_APP_RR_INPUT_RTL);
    rc |= expect_int("soapy", input_kind_of(&opts, AUDIO_IN_RTL, "soapy:driver=hackrf"), DSD_APP_RR_INPUT_SOAPY);
    rc |= expect_int("airspy", input_kind_of(&opts, AUDIO_IN_RTL, "airspy"), DSD_APP_RR_INPUT_AIRSPY);
    rc |= expect_int("replay", input_kind_of(&opts, AUDIO_IN_RTL, "iqreplay:cap.json"), DSD_APP_RR_INPUT_IQ_REPLAY);
    rc |= expect_int("symbols", input_kind_of(&opts, AUDIO_IN_SYMBOL_BIN, "cap.bin"), DSD_APP_RR_INPUT_SYMBOLS);
    rc |= expect_int("float symbols", input_kind_of(&opts, AUDIO_IN_SYMBOL_FLT, "cap.sym"), DSD_APP_RR_INPUT_SYMBOLS);
    /* A radio spec left behind by a switch to PCM is audio: the type decides. */
    rc |= expect_int("rtl spec on pulse", input_kind_of(&opts, AUDIO_IN_PULSE, "rtl:0"), DSD_APP_RR_INPUT_AUDIO);
    /* A replay flag a switch to an RTL-SDR left set (an input switch does not clear it, and a failed open does not
       reset it) is no replay: the spec in force decides, as every other import check asks it, so TETRA on that RTL-SDR
       at 48 kHz is refused rather than passed as a replay. */
    opts.iq_replay_active = 1;
    rc |= expect_int("stale replay flag on rtl", input_kind_of(&opts, AUDIO_IN_RTL, "rtl:0"), DSD_APP_RR_INPUT_RTL);
    {
        dsd_app_rr_apply_payload stale;
        DSD_MEMSET(&stale, 0, sizeof stale);
        stale.decode_mode = (int32_t)DSDCFG_MODE_TETRA;
        opts.rtl_dsp_bw_khz = 48;
        dsd_app_rr_receiver_facts stale_facts;
        rr_apply_receiver_facts_fill(&opts, &stale, 0, 0, 0U, &stale_facts);
        rc |= expect_verdict("stale replay flag: tetra at 48 kHz", &stale_facts, k_tetra_rtl_bw, 1);
    }
    rc |= expect_int("replay with the flag", input_kind_of(&opts, AUDIO_IN_RTL, "iqreplay:cap.json"),
                     DSD_APP_RR_INPUT_IQ_REPLAY);
    opts.iq_replay_active = 0;

    dsd_app_rr_apply_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_TETRA;
    p.trunking = 1U;
    p.scanner = 1U;
    set_input(&opts, AUDIO_IN_RTL, "rtl");
    opts.rtl_dsp_bw_khz = 72;
    opts.tetra_uplink = 1;
    opts.trunk_scan_enabled = 1;
    dsd_app_rr_receiver_facts f;
    rr_apply_receiver_facts_fill(&opts, &p, 1, 0, 72000U, &f);
    rc |= expect_int("mode", f.decode_mode, (int32_t)DSDCFG_MODE_TETRA);
    rc |= expect_int("bandwidth", f.rtl_dsp_bw_khz, 72);
    rc |= expect_int("rate", (int)f.actual_rate_hz, 72000);
    rc |= expect_int("uplink", f.tetra_uplink, 1);
    rc |= expect_int("trunk scan", f.trunk_scan_running, 1);
    rc |= expect_int("group from the caller", f.has_group, 1);
    rc |= expect_int("no chan from the caller", f.has_chan, 0);
    rc |= expect_int("trunking owns the tuner", f.trunking, 1);
    rc |= expect_int("so the scanner does not", f.scanner, 0);
    return rc;
}

/* A trunk scan waiting for the next start (issue #671) refuses an import that leaves a channel map or the -Y scanner,
   whatever the session runs now; one that leaves neither passes. */
static int
test_next_start(void) {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    int rc = 0;
    dsd_app_rr_receiver_facts f = facts_for((int32_t)DSDCFG_MODE_DMR, DSD_APP_RR_INPUT_AUDIO);
    char why[256];
    f.has_chan = 1;
    rc |= expect_int("no scan waiting: channel map passes",
                     rr_apply_next_start_refusal(&opts, &state, &f, NULL, why, sizeof why), 0);
    f.has_chan = 0;
    opts.next_start.pending = DSD_NEXT_START_TRUNK_SCAN | DSD_NEXT_START_TRUNK_SCAN_TARGETS;
    opts.next_start.trunk_scan_enabled = 1;
    DSD_SNPRINTF(opts.next_start.trunk_scan_targets_csv, sizeof opts.next_start.trunk_scan_targets_csv, "%s",
                 "targets.csv");
    f.trunking = 1;
    rc |=
        expect_int("trunked, no map passes", rr_apply_next_start_refusal(&opts, &state, &f, NULL, why, sizeof why), 0);
    f.has_chan = 1;
    rc |= expect_int("a map before its file exists is refused",
                     rr_apply_next_start_refusal(&opts, &state, &f, NULL, why, sizeof why), -1);
    rc |= expect_str("the map rule", why,
                     "trunking.chan_csv cannot be combined with trunk_scan.enabled; use per-target chan_csv values");
    rc |= expect_int("a written map is refused",
                     rr_apply_next_start_refusal(&opts, &state, &f, "/tmp/chan.csv", why, sizeof why), -1);
    f.has_chan = 0;
    f.trunking = 0;
    f.scanner = 1;
    rc |=
        expect_int("the scanner is refused", rr_apply_next_start_refusal(&opts, &state, &f, NULL, why, sizeof why), -1);
    rc |= expect_int("the scanner rule", strncmp(why, "trunking.scanner cannot be combined", 35), 0);
    rc |= expect_int("null state", rr_apply_next_start_refusal(&opts, NULL, &f, NULL, why, sizeof why), -1);
    freeState(&state);
    return rc;
}

/* The frontends' pre-check, over the published snapshots only. */
static int
test_block_reason(void) {
    int rc = 0;
    char why[DSD_APP_RR_VERDICT_WHY_MAX];
    dsd_app_rr_apply_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_TETRA;
    p.trunking = 1U;

    g_opts_published = 0;
    g_state_published = 0;
    rc |= expect_int("nothing published passes", dsd_app_rr_apply_block_reason(&p, 1, 0, why, sizeof why), 0);
    rc |= expect_str("nothing published: no reason", why, "");
    rc |= expect_int("null payload passes", dsd_app_rr_apply_block_reason(NULL, 1, 0, why, sizeof why), 0);

    initOpts(&g_snap_opts);
    initState(&g_snap_state);
    g_opts_published = 1;
    set_input(&g_snap_opts, AUDIO_IN_RTL, "rtl");
    g_snap_opts.rtl_dsp_bw_khz = 48;
    rc |= expect_int("rtl at 48 kHz blocked", dsd_app_rr_apply_block_reason(&p, 1, 0, why, sizeof why), 1);
    rc |= expect_str("rtl at 48 kHz reason", why, k_tetra_rtl_bw);
    g_snap_opts.rtl_dsp_bw_khz = 72;
    rc |= expect_int("rtl at 72 kHz passes", dsd_app_rr_apply_block_reason(&p, 1, 0, why, sizeof why), 0);

    /* No measured rate on the frontend side: a SoapySDR device passes, whatever the DSP bandwidth reads. */
    set_input(&g_snap_opts, AUDIO_IN_RTL, "soapy:driver=hackrf");
    g_snap_opts.rtl_dsp_bw_khz = 48;
    rc |= expect_int("soapy passes to the decoder", dsd_app_rr_apply_block_reason(&p, 1, 0, why, sizeof why), 0);
    set_input(&g_snap_opts, AUDIO_IN_RTL, "airspy");
    rc |= expect_int("airspy blocked", dsd_app_rr_apply_block_reason(&p, 1, 0, why, sizeof why), 1);
    set_input(&g_snap_opts, AUDIO_IN_PULSE, "pulse");
    rc |= expect_int("pulse blocked", dsd_app_rr_apply_block_reason(&p, 1, 0, why, sizeof why), 1);
    rc |= expect_str("pulse reason", why, "TETRA needs an IQ radio input or a symbol capture");

    /* The next-start rule needs the state snapshot as well, and takes the plan's outputs before any file exists. */
    p.decode_mode = (int32_t)DSDCFG_MODE_P25P1;
    g_snap_opts.next_start.pending = DSD_NEXT_START_TRUNK_SCAN | DSD_NEXT_START_TRUNK_SCAN_TARGETS;
    g_snap_opts.next_start.trunk_scan_enabled = 1;
    DSD_SNPRINTF(g_snap_opts.next_start.trunk_scan_targets_csv, sizeof g_snap_opts.next_start.trunk_scan_targets_csv,
                 "%s", "targets.csv");
    rc |= expect_int("no state snapshot: the rule is not asked",
                     dsd_app_rr_apply_block_reason(&p, 1, 1, why, sizeof why), 0);
    g_state_published = 1;
    rc |= expect_int("prospective map blocked", dsd_app_rr_apply_block_reason(&p, 1, 1, why, sizeof why), 1);
    rc |=
        expect_str("the rule without its advice", why, "trunking.chan_csv cannot be combined with trunk_scan.enabled");
    rc |= expect_int("no map passes", dsd_app_rr_apply_block_reason(&p, 1, 0, why, sizeof why), 0);
    rc |= expect_int("null why", dsd_app_rr_apply_block_reason(&p, 1, 1, NULL, 0U), 1);

    g_snap_opts.trunk_scan_enabled = 1;
    rc |= expect_int("running trunk scan blocked", dsd_app_rr_apply_block_reason(&p, 1, 0, why, sizeof why), 1);
    rc |= expect_str("running trunk scan reason", why, "stop the trunk scan first; it keeps its own channel maps");
    freeState(&g_snap_state);
    g_opts_published = 0;
    g_state_published = 0;
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_verdict_tetra_table();
    rc |= test_verdict_other_modes();
    rc |= test_facts_fill();
    rc |= test_next_start();
    rc |= test_block_reason();
    if (rc == 0) {
        printf("APP_CONTROL_RR_VERDICT: OK\n");
    }
    return rc;
}
