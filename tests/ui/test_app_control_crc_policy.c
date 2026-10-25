// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Relaxed CRC is one setting: -F, --strict-crc, the menu toggle (DSD_APP_CMD_AGGR_SYNC_TOGGLE) and a scan row's -F or
 * --strict-crc all move dsd_opts::aggressive_framesync and dsd_opts::dmr_crc_relaxed_default together. The toggle used
 * to flip only the first, so turned on from the default it left DMR's relaxed control-channel heartbeat off, and
 * turned off after -F it left DMR accepting data headers whose CRC failed (and the heartbeat on) while the menu said
 * off. The app-control view (crc_policy_view.h) reports the configured value, the one in force, and whether the scan
 * row on air decides it, in both directions and after the row leaves.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/crc_policy_view.h>
#include <dsd-neo/app_control/dmr_ras_view.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/app_control/commands_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int
expect_int(const char* what, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %lld, want %lld\n", what, got, want);
        return 1;
    }
    return 0;
}

static void
context_init(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    initOpts(opts);
    initState(state);
    dsd_app_frontend_runtime_start(opts, state);
}

static void
context_free(dsd_state* state) {
    dsd_app_frontend_runtime_stop();
    freeState(state);
}

/* Parse one CLI switch ("-F" or "--strict-crc") over the defaults, as a session starts. */
static int
parse_switch(dsd_opts* opts, dsd_state* state, const char* option) {
    char arg0[] = "dsd-neo";
    char arg1[32];
    DSD_SNPRINTF(arg1, sizeof arg1, "%s", option);
    char* argv[] = {arg0, arg1, NULL};
    int effective = 0;
    int exit_rc = 0;
    return dsd_parse_args(2, argv, opts, state, &effective, &exit_rc) == DSD_PARSE_CONTINUE ? 0 : 1;
}

static int
toggle(dsd_opts* opts, dsd_state* state) {
    int rc = expect_int("queue toggle", dsd_app_command_submit(DSD_APP_CMD_AGGR_SYNC_TOGGLE, NULL, 0U),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("toggle drained", dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

/* Whether a control PDU whose CRC failed keeps the control-channel timer warm (dmr_csbk.c's relaxed heartbeat). */
static int
heartbeat_runs(dsd_opts* opts, dsd_state* state) {
    uint8_t bits[196] = {0};
    uint8_t bytes[24] = {0};
    bytes[0] = 25; /* Aloha */
    for (int i = 0; i < 8; ++i) {
        bits[i] = (uint8_t)((bytes[0] >> (7 - i)) & 1);
    }
    state->last_cc_sync_time = 0;
    dmr_cspdu(opts, state, bits, bytes, /*CRCCorrect=*/0U, /*IrrecoverableErrors=*/0U);
    return state->last_cc_sync_time != 0;
}

/* Whether DMR takes a data header with no irrecoverable errors: dmr_block.c's CRC gate on the header. */
static int
data_header_accepted(dsd_opts* opts, dsd_state* state, uint32_t crc_correct) {
    uint8_t dheader[12] = {0};
    uint8_t bits[196] = {0};
    /* An unconfirmed (DPF 2) header with one block to follow. */
    dheader[0] = 0x02U;
    dheader[8] = 0x01U;
    for (int i = 0; i < 96; ++i) {
        bits[i] = (uint8_t)((dheader[i / 8] >> (7 - (i % 8))) & 1U);
    }
    state->currentslot = 0;
    state->data_header_valid[0] = 0;
    dmr_dheader(opts, state, dheader, bits, crc_correct, /*IrrecoverableErrors=*/0U);
    return state->data_header_valid[0] != 0;
}

static int
expect_strict(const char* when, dsd_opts* opts, dsd_state* state) {
    char what[160];
    int rc = 0;
    DSD_SNPRINTF(what, sizeof what, "%s: aggressive_framesync", when);
    rc |= expect_int(what, opts->aggressive_framesync, 1);
    DSD_SNPRINTF(what, sizeof what, "%s: dmr_crc_relaxed_default", when);
    rc |= expect_int(what, opts->dmr_crc_relaxed_default, 0);
    DSD_SNPRINTF(what, sizeof what, "%s: no relaxed DMR heartbeat", when);
    rc |= expect_int(what, heartbeat_runs(opts, state), 0);
    DSD_SNPRINTF(what, sizeof what, "%s: failed-CRC DMR data header refused", when);
    rc |= expect_int(what, data_header_accepted(opts, state, 0U), 0);
    DSD_SNPRINTF(what, sizeof what, "%s: CRC-good DMR data header taken", when);
    rc |= expect_int(what, data_header_accepted(opts, state, 1U), 1);
    return rc;
}

static int
expect_relaxed(const char* when, dsd_opts* opts, dsd_state* state) {
    char what[160];
    int rc = 0;
    DSD_SNPRINTF(what, sizeof what, "%s: aggressive_framesync", when);
    rc |= expect_int(what, opts->aggressive_framesync, 0);
    DSD_SNPRINTF(what, sizeof what, "%s: dmr_crc_relaxed_default", when);
    rc |= expect_int(what, opts->dmr_crc_relaxed_default, 1);
    DSD_SNPRINTF(what, sizeof what, "%s: relaxed DMR heartbeat", when);
    rc |= expect_int(what, heartbeat_runs(opts, state), 1);
    DSD_SNPRINTF(what, sizeof what, "%s: failed-CRC DMR data header taken", when);
    rc |= expect_int(what, data_header_accepted(opts, state, 0U), 1);
    return rc;
}

/* From the default (strict), the toggle turns everything -F relaxes on, and back off. */
static int
test_toggle_from_default(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    context_init(opts, state);
    int rc = expect_strict("default", opts, state);
    rc |= toggle(opts, state);
    rc |= expect_relaxed("default, toggled on", opts, state);
    rc |= toggle(opts, state);
    rc |= expect_strict("default, toggled on and off", opts, state);
    context_free(state);
    free(state);
    free(opts);
    return rc;
}

/* After -F (or --strict-crc), the toggle turns both fields the other way, and back. */
static int
test_toggle_from_cli(const char* option, int relaxed) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    context_init(opts, state);
    char when[64];
    int rc = expect_int("parse", parse_switch(opts, state, option), 0);
    DSD_SNPRINTF(when, sizeof when, "%s", option);
    rc |= relaxed ? expect_relaxed(when, opts, state) : expect_strict(when, opts, state);
    rc |= toggle(opts, state);
    DSD_SNPRINTF(when, sizeof when, "%s, toggled", option);
    rc |= relaxed ? expect_strict(when, opts, state) : expect_relaxed(when, opts, state);
    rc |= toggle(opts, state);
    DSD_SNPRINTF(when, sizeof when, "%s, toggled twice", option);
    rc |= relaxed ? expect_relaxed(when, opts, state) : expect_strict(when, opts, state);
    context_free(state);
    free(state);
    free(opts);
    return rc;
}

static int
expect_view(const char* when, const dsd_opts* opts, const dsd_state* state, int configured, int effective,
            int row_override) {
    dsd_app_crc_policy policy;
    DSD_MEMSET(&policy, 0xA5, sizeof policy);
    dsd_app_crc_policy_view(opts, state, &policy);
    char what[160];
    int rc = 0;
    DSD_SNPRINTF(what, sizeof what, "%s: view configured", when);
    rc |= expect_int(what, policy.configured, configured);
    DSD_SNPRINTF(what, sizeof what, "%s: view effective", when);
    rc |= expect_int(what, policy.effective, effective);
    DSD_SNPRINTF(what, sizeof what, "%s: view row_override", when);
    rc |= expect_int(what, policy.row_override, row_override);
    return rc;
}

/* The configured pair the scope saved, as a row's leave restores it. */
static int
expect_configured_pair(const char* when, const dsd_opts* opts, const dsd_state* state, int relaxed) {
    dsd_scan_settings configured;
    DSD_MEMSET(&configured, 0, sizeof configured);
    dsd_scan_mode_configured(opts, state, &configured);
    char what[160];
    int rc = 0;
    DSD_SNPRINTF(what, sizeof what, "%s: configured aggressive_framesync", when);
    rc |= expect_int(what, configured.aggressive_framesync, relaxed ? 0 : 1);
    DSD_SNPRINTF(what, sizeof what, "%s: configured dmr_crc_relaxed_default", when);
    rc |= expect_int(what, configured.dmr_crc_relaxed_default, relaxed ? 1 : 0);
    return rc;
}

static int
expect_toast(const char* when, const dsd_state* state, const char* want) {
    if (strcmp(state->ui_msg, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: toast \"%s\", want \"%s\"\n", when, state->ui_msg, want);
        return 1;
    }
    return 0;
}

static int
enter_dmr_row(dsd_opts* opts, dsd_state* state, const dsd_scan_option_values* row) {
    int rc = expect_int("enter row", dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR), 0);
    rc |= expect_int("row options", dsd_scan_mode_options(opts, state, row), 0);
    return rc;
}

static void
scan_context_init(dsd_opts* opts, dsd_state* state) {
    context_init(opts, state);
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 48000;
}

/* The same command with a 0/1 payload sets the value instead of flipping it (the Qt/Android Relaxed CRC switch): a
   frontend acting on a snapshot that is one command behind cannot turn it the wrong way. */
static int
set_relaxed(dsd_opts* opts, dsd_state* state, int32_t on) {
    int rc = expect_int("queue set", dsd_app_command_set_i32(DSD_APP_CMD_AGGR_SYNC_TOGGLE, on),
                        DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("set drained", dsd_app_drain_cmds(opts, state), 1);
    return rc;
}

static int
test_set_with_payload(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    scan_context_init(opts, state);
    int rc = set_relaxed(opts, state, 1);
    rc |= expect_relaxed("set on", opts, state);
    rc |= set_relaxed(opts, state, 1);
    rc |= expect_relaxed("set on twice", opts, state);
    rc |= set_relaxed(opts, state, 0);
    rc |= expect_strict("set off", opts, state);
    rc |= set_relaxed(opts, state, 0);
    rc |= expect_strict("set off twice", opts, state);

    /* Under a --strict-crc row the set edits the configured value, and the toast says the row decides. */
    const dsd_scan_option_values row = {.present = DSD_SCAN_OPT_CRC, .strict_crc = 1};
    rc |= enter_dmr_row(opts, state, &row);
    state->ui_msg[0] = '\0';
    rc |= set_relaxed(opts, state, 1);
    rc |= expect_strict("strict row, set on", opts, state);
    rc |= expect_view("strict row, set on", opts, state, 1, 0, 1);
    rc |= expect_toast("strict row, set on", state, "Default relaxed CRC -> On; this channel overrides it (Off)");
    rc |= set_relaxed(opts, state, 1);
    rc |= expect_view("strict row, set on twice", opts, state, 1, 0, 1);
    dsd_scan_mode_leave(opts, state);
    rc |= expect_relaxed("strict row left after set on", opts, state);
    context_free(state);
    free(state);
    free(opts);
    return rc;
}

/* A --strict-crc row over a relaxed (-F) baseline: the row decides until it leaves, and the toggle edits the
   baseline under it. */
static int
test_strict_row_over_relaxed_baseline(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    dsd_state* snapshot = (dsd_state*)calloc(1, sizeof(*snapshot));
    if (!opts || !state || !snapshot) {
        free(opts);
        free(state);
        free(snapshot);
        return 1;
    }
    scan_context_init(opts, state);
    int rc = expect_int("parse -F", parse_switch(opts, state, "-F"), 0);
    rc |= expect_view("-F, no scope", opts, state, 1, 1, 0);
    rc |= expect_view("-F, no state", opts, NULL, 1, 1, 0);
    const dsd_scan_option_values row = {.present = DSD_SCAN_OPT_CRC, .strict_crc = 1};
    rc |= enter_dmr_row(opts, state, &row);
    rc |= expect_strict("strict row over -F", opts, state);
    rc |= expect_view("strict row over -F", opts, state, 1, 0, 1);
    rc |= expect_configured_pair("strict row over -F", opts, state, 1);

    state->ui_msg[0] = '\0';
    rc |= toggle(opts, state);
    rc |= expect_strict("strict row, baseline toggled off", opts, state);
    rc |= expect_view("strict row, baseline toggled off", opts, state, 0, 0, 1);
    rc |= expect_configured_pair("strict row, baseline toggled off", opts, state, 0);
    rc |= expect_toast("strict row, baseline toggled off", state,
                       "Default relaxed CRC -> Off; this channel overrides it (Off)");
    rc |= toggle(opts, state);
    rc |= expect_strict("strict row, baseline toggled back on", opts, state);
    rc |= expect_view("strict row, baseline toggled back on", opts, state, 1, 0, 1);
    rc |= expect_configured_pair("strict row, baseline toggled back on", opts, state, 1);
    rc |= expect_toast("strict row, baseline toggled back on", state,
                       "Default relaxed CRC -> On; this channel overrides it (Off)");

    /* A frontend reads the same answer from its snapshot pair. */
    initState(snapshot);
    dsd_scan_mode_copy_snapshot(snapshot, state);
    rc |= expect_view("strict row, snapshot", opts, snapshot, 1, 0, 1);
    /* And so does a command between suspend and resume, while dsd_opts holds the configured pair. */
    rc |= expect_int("suspend", dsd_scan_mode_suspend(opts, state), 1);
    rc |= expect_view("strict row, suspended", opts, state, 1, 0, 1);
    (void)dsd_scan_mode_resume(opts, state);
    rc |= expect_view("strict row, resumed", opts, state, 1, 0, 1);

    dsd_scan_mode_leave(opts, state);
    rc |= expect_relaxed("strict row left", opts, state);
    rc |= expect_view("strict row left", opts, state, 1, 1, 0);
    freeState(snapshot);
    free(snapshot);
    context_free(state);
    free(state);
    free(opts);
    return rc;
}

/* An -F row over the strict default, then the row's options cleared and a toggle with no row override. */
static int
test_relaxed_row_over_strict_baseline(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    scan_context_init(opts, state);
    const dsd_scan_option_values row = {.present = DSD_SCAN_OPT_CRC, .strict_crc = 0};
    int rc = enter_dmr_row(opts, state, &row);
    rc |= expect_relaxed("-F row over default", opts, state);
    rc |= expect_view("-F row over default", opts, state, 0, 1, 1);
    rc |= expect_configured_pair("-F row over default", opts, state, 0);
    rc |= toggle(opts, state);
    rc |= expect_relaxed("-F row, baseline toggled on", opts, state);
    rc |= expect_view("-F row, baseline toggled on", opts, state, 1, 1, 1);
    rc |= expect_configured_pair("-F row, baseline toggled on", opts, state, 1);
    rc |= toggle(opts, state);
    rc |= expect_view("-F row, baseline toggled back off", opts, state, 0, 1, 1);
    rc |= expect_configured_pair("-F row, baseline toggled back off", opts, state, 0);

    /* The row's options cleared: the configured value is in force again, and the toggle edits it live. */
    rc |= expect_int("clear row options", dsd_scan_mode_options(opts, state, NULL), 0);
    rc |= expect_strict("row options cleared", opts, state);
    rc |= expect_view("row options cleared", opts, state, 0, 0, 0);
    state->ui_msg[0] = '\0';
    rc |= toggle(opts, state);
    rc |= expect_relaxed("no row override, toggled on", opts, state);
    rc |= expect_view("no row override, toggled on", opts, state, 1, 1, 0);
    rc |= expect_configured_pair("no row override, toggled on", opts, state, 1);
    rc |= expect_toast("no row override: no toast", state, "");

    dsd_scan_mode_leave(opts, state);
    rc |= expect_relaxed("row left", opts, state);
    rc |= expect_view("row left", opts, state, 1, 1, 0);
    context_free(state);
    free(state);
    free(opts);
    return rc;
}

static int
test_notice_and_null_arguments(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts) {
        return 1;
    }
    initOpts(opts);
    int rc = expect_view("NULL opts", NULL, NULL, 0, 0, 0);
    dsd_app_crc_policy_view(opts, NULL, NULL);
    char notice[DSD_APP_CRC_POLICY_NOTICE_SIZE];
    rc |= expect_int("notice without row", dsd_app_crc_policy_edit_notice(opts, NULL, notice, sizeof notice), 0);
    if (strcmp(notice, "Applied: relaxed CRC -> Off") != 0) {
        DSD_FPRINTF(stderr, "FAIL: notice without row: \"%s\"\n", notice);
        rc = 1;
    }
    rc |= expect_int("notice NULL out", dsd_app_crc_policy_edit_notice(opts, NULL, NULL, sizeof notice), -1);
    rc |= expect_int("notice zero size", dsd_app_crc_policy_edit_notice(opts, NULL, notice, 0U), -1);
    /* A pair set apart from the setter reads as on, and turning it off clears both. */
    opts->aggressive_framesync = 1;
    opts->dmr_crc_relaxed_default = 1;
    rc |= expect_int("half pair reads on", dsd_opts_relaxed_crc(opts), 1);
    dsd_opts_set_relaxed_crc(opts, 0);
    rc |= expect_int("off clears aggressive", opts->aggressive_framesync, 1);
    rc |= expect_int("off clears dmr", opts->dmr_crc_relaxed_default, 0);
    rc |= expect_int("off reads off", dsd_opts_relaxed_crc(opts), 0);
    dsd_opts_set_relaxed_crc(NULL, 1);
    rc |= expect_int("NULL opts reads off", dsd_opts_relaxed_crc(NULL), 0);
    free(opts);
    return rc;
}

/* A scope baseline holding the pair apart from the setter (aggressive_framesync and dmr_crc_relaxed_default both 1)
   reads configured on, as dsd_opts_relaxed_crc() reads the pair, under a strict row too. */
static int
test_half_pair_baseline(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    scan_context_init(opts, state);
    opts->aggressive_framesync = 1;
    opts->dmr_crc_relaxed_default = 1;
    const dsd_scan_option_values row = {.present = DSD_SCAN_OPT_CRC, .strict_crc = 1};
    int rc = enter_dmr_row(opts, state, &row);
    rc |= expect_int("half pair baseline: scope captured it", dsd_scan_mode_configured_view(state) != NULL, 1);
    rc |= expect_view("half pair baseline, strict row", opts, state, 1, 0, 1);
    dsd_scan_mode_leave(opts, state);
    context_free(state);
    free(state);
    free(opts);
    return rc;
}

static int
expect_ras(const char* when, const dsd_opts* opts, const dsd_state* state, int detected, int needs_relaxed,
           const char* label) {
    dsd_app_dmr_ras ras;
    dsd_app_dmr_ras_view(opts, state, &ras);
    char what[96];
    DSD_SNPRINTF(what, sizeof what, "%s: ras detected", when);
    int rc = expect_int(what, ras.detected, detected);
    DSD_SNPRINTF(what, sizeof what, "%s: ras needs relaxed", when);
    rc |= expect_int(what, ras.needs_relaxed, needs_relaxed);
    if (strcmp(dsd_app_dmr_ras_label(&ras), label) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: ras label \"%s\", want \"%s\"\n", when, dsd_app_dmr_ras_label(&ras), label);
        rc = 1;
    }
    return rc;
}

/* The DMR RAS indicator: detected from the received identity's DSD_RXID_DMR_RAS of a DMR record only, labelled "RAS"
   while relaxed CRC is in force and "RAS (-F decodes)" while it is not, a scan row's own policy included, except
   "RAS (channel sets strict CRC)" while a row's own --strict-crc runs over a configured -F. */
static int
test_dmr_ras_view(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return 1;
    }
    scan_context_init(opts, state);
    int rc = expect_ras("nothing received", opts, state, 0, 0, "");
    dsd_rxid_note_access(state, DSD_RXID_FAMILY_DMR, DSD_ACCESS_CODE_COLOR_CODE, 1U);
    rc |= expect_ras("a colour code alone", opts, state, 0, 0, "");
    dsd_rxid_note_dmr_ras(state);
    rc |= expect_ras("ras, strict CRC", opts, state, 1, 1, "RAS (-F decodes)");
    dsd_opts_set_relaxed_crc(opts, 1);
    rc |= expect_ras("ras, relaxed CRC", opts, state, 1, 0, "RAS");
    const dsd_scan_option_values row = {.present = DSD_SCAN_OPT_CRC, .strict_crc = 1};
    rc |= enter_dmr_row(opts, state, &row);
    dsd_rxid_note_dmr_ras(state);
    /* -F is on already: pressing F would turn it off and change nothing on this channel. */
    rc |= expect_ras("ras, a strict row over -F", opts, state, 1, 1, "RAS (channel sets strict CRC)");
    dsd_scan_mode_leave(opts, state);
    dsd_rxid_note_dmr_ras(state);
    rc |= expect_ras("ras, the row left", opts, state, 1, 0, "RAS");
    /* Over the strict default, -F still decodes the channels after this one. */
    dsd_opts_set_relaxed_crc(opts, 0);
    rc |= enter_dmr_row(opts, state, &row);
    dsd_rxid_note_dmr_ras(state);
    rc |= expect_ras("ras, a strict row over the default", opts, state, 1, 1, "RAS (-F decodes)");
    dsd_scan_mode_leave(opts, state);
    dsd_rxid_forget_dmr_ras(state);
    rc |= expect_ras("ras forgotten", opts, state, 0, 0, "");
    rc |= expect_ras("NULL state", opts, NULL, 0, 0, "");
    dsd_app_dmr_ras_view(opts, state, NULL);
    rc |= expect_int("NULL view label", dsd_app_dmr_ras_label(NULL)[0], '\0');
    context_free(state);
    free(state);
    free(opts);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_toggle_from_default();
    rc |= test_toggle_from_cli("-F", 1);
    rc |= test_toggle_from_cli("--strict-crc", 0);
    rc |= test_set_with_payload();
    rc |= test_strict_row_over_relaxed_baseline();
    rc |= test_relaxed_row_over_strict_baseline();
    rc |= test_notice_and_null_arguments();
    rc |= test_half_pair_baseline();
    rc |= test_dmr_ras_view();
    if (rc == 0) {
        printf("APP_CONTROL_CRC_POLICY: OK\n");
    }
    return rc;
}
