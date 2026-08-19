// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference apply: plan -> payload mapping, and the queue-level contract
 * of DSD_APP_CMD_RR_APPLY_IMPORT / DSD_APP_CMD_RR_ACCOUNT_SET.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../src/app_control/commands_internal.h"

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
static int g_io_control_tune_result = RTL_STREAM_TUNE_OK;
static int g_io_control_tune_calls = 0;
static long int g_io_control_tune_freq = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq);

int
__wrap_io_control_set_freq(dsd_opts* opts, dsd_state* state, long int freq) {
    (void)opts;
    (void)state;
    g_io_control_tune_calls++;
    g_io_control_tune_freq = freq;
    return g_io_control_tune_result;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
reset_io_control_tune_stub(int result) {
    g_io_control_tune_result = result;
    g_io_control_tune_calls = 0;
    g_io_control_tune_freq = 0;
}
#endif

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

static void
plan_init(dsd_rr_import_plan* plan) {
    DSD_MEMSET(plan, 0, sizeof(*plan));
    plan->ok = 1;
}

static int
test_fill_payload_trunked_p25(void) {
    int rc = 0;
    dsd_rr_import_plan plan;
    dsd_app_rr_apply_payload payload;
    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_P25;
    plan.trunking = 1;
    plan.chan_need = 1;
    plan.simulcast = 1;
    plan.tune_hz = 851012500LL;
    DSD_SNPRINTF(plan.site_ids, sizeof plan.site_ids, "%s", "4321");

    rc |=
        expect_int("trunked p25 maps", dsd_app_rr_fill_apply_payload(&plan, "a chan.csv", "a group.csv", &payload), 0);
    rc |= expect_int("trunked p25 mode", payload.decode_mode, (int32_t)DSDCFG_MODE_TDMA);
    rc |= expect_int("trunked p25 prefers candidates", payload.p25_prefer_candidates, 1);
    rc |= expect_int("trunked p25 trunking", payload.trunking, 1);
    rc |= expect_int("trunked p25 not scanner", payload.scanner, 0);
    rc |= expect_int("trunked p25 simulcast", payload.simulcast_qpsk, 1);
    rc |= expect_int("trunked p25 not edacs ea", payload.edacs_ea, 0);
    rc |= expect_int("trunked p25 tune", (int)payload.tune_hz, 851012500);
    rc |= expect_int("trunked p25 has chan", payload.has_chan, 1);
    rc |= expect_int("trunked p25 has group", payload.has_group, 1);
    rc |= expect_str("trunked p25 chan path", payload.chan_path, "a chan.csv");
    rc |= expect_str("trunked p25 group path", payload.group_path, "a group.csv");
    return rc;
}

static int
test_fill_payload_conventional_dmr(void) {
    int rc = 0;
    dsd_rr_import_plan plan;
    dsd_app_rr_apply_payload payload;
    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_DMR_CONV;
    plan.conventional = 1;
    plan.scan_list = 1;
    plan.chan_need = 1;

    rc |= expect_int("conventional dmr maps", dsd_app_rr_fill_apply_payload(&plan, "c chan.csv", "", &payload), 0);
    rc |= expect_int("conventional dmr mode", payload.decode_mode, (int32_t)DSDCFG_MODE_DMR);
    rc |= expect_int("conventional dmr scanner", payload.scanner, 1);
    rc |= expect_int("conventional dmr not trunking", payload.trunking, 0);
    rc |= expect_int("conventional dmr no prefer candidates", payload.p25_prefer_candidates, 0);
    rc |= expect_int("conventional dmr has chan", payload.has_chan, 1);
    rc |= expect_int("conventional dmr no group", payload.has_group, 0);
    rc |= expect_int("conventional dmr no tune", (int)payload.tune_hz, 0);
    return rc;
}

static int
test_fill_payload_edacs_and_refusals(void) {
    int rc = 0;
    dsd_rr_import_plan plan;
    dsd_app_rr_apply_payload payload;
    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_EDACS_EA;
    plan.trunking = 1;
    plan.esk = 1;

    rc |= expect_int("edacs ea maps", dsd_app_rr_fill_apply_payload(&plan, "e chan.csv", "e group.csv", &payload), 0);
    rc |= expect_int("edacs ea mode", payload.decode_mode, (int32_t)DSDCFG_MODE_EDACS_PV);
    rc |= expect_int("edacs ea flag", payload.edacs_ea, 1);
    rc |= expect_int("edacs esk flag", payload.edacs_esk, 1);

    plan.protocol = DSD_RR_PROTO_EDACS_STD;
    rc |= expect_int("edacs std maps", dsd_app_rr_fill_apply_payload(&plan, "e chan.csv", "e group.csv", &payload), 0);
    rc |= expect_int("edacs std clears ea", payload.edacs_ea, 0);

    plan_init(&plan);
    plan.ok = 0;
    plan.protocol = DSD_RR_PROTO_P25;
    rc |= expect_int("not-ok plan refused", dsd_app_rr_fill_apply_payload(&plan, "", "", &payload), -1);

    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_UNSUPPORTED;
    rc |= expect_int("unsupported protocol refused", dsd_app_rr_fill_apply_payload(&plan, "", "", &payload), -1);

    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_P25;
    plan.tune_hz = 5000000000LL;
    rc |= expect_int("out-of-range tune refused", dsd_app_rr_fill_apply_payload(&plan, "", "", &payload), -1);

    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_P25;
    rc |= expect_int("null out refused", dsd_app_rr_fill_apply_payload(&plan, "", "", NULL), -1);
    rc |= expect_int("null plan refused", dsd_app_rr_fill_apply_payload(NULL, "", "", &payload), -1);
    return rc;
}

static int
write_text(const char* path, const char* text) {
    FILE* f = dsd_fopen_private(path, "wb");
    if (!f) {
        DSD_FPRINTF(stderr, "failed to create %s\n", path);
        return 1;
    }
    int rc = 0;
    const size_t n = strlen(text);
    if (n > 0U && fwrite(text, 1U, n, f) != n) {
        rc = 1;
    }
    if (fclose(f) != 0) {
        rc = 1;
    }
    return rc;
}

static void
init_test_context(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    initOpts(opts);
    initState(state);
    state->cli_argc_effective = 0;
    state->cli_argv = NULL;
}

static void
payload_files(dsd_app_rr_apply_payload* p, const char* chan, const char* group) {
    p->has_chan = 1U;
    p->has_group = 1U;
    DSD_SNPRINTF(p->chan_path, sizeof p->chan_path, "%s", chan);
    DSD_SNPRINTF(p->group_path, sizeof p->group_path, "%s", group);
}

static int
test_submit_null_guards(void) {
    int rc = 0;
    rc |= expect_int("null rr apply rejected", dsd_app_command_set_rr_apply(NULL), DSD_APP_COMMAND_SUBMIT_REJECTED);
    rc |= expect_int("null rr account rejected", dsd_app_command_set_rr_account(NULL), DSD_APP_COMMAND_SUBMIT_REJECTED);
    return rc;
}

static int
test_apply_dmr_trunked_with_files(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* chan_csv = "rr_apply_chan.csv";
    const char* group_csv = "rr_apply_group.csv";
    dsd_app_rr_apply_payload p;

    rc |= write_text(chan_csv, "ChannelNumber(dec),frequency(Hz),note\n"
                               "1,851012500,cc\n"
                               "2,851512500\n"
                               "3,852012500\n");
    rc |= write_text(group_csv, "id,mode,name\n"
                                "101,A,Dispatch\n"
                                "102,A,Fire\n");

    init_test_context(&opts, &state);
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_DMR;
    p.trunking = 1U;
    payload_files(&p, chan_csv, group_csv);

    rc |= expect_int("rr apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("rr apply drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("dmr framing on", opts.frame_dmr, 1);
    rc |= expect_int("trunking on", opts.trunk_enable, 1);
    rc |= expect_int("scanner off", opts.scanner_mode, 0);
    rc |= expect_str("chan path adopted", opts.chan_in_file, chan_csv);
    rc |= expect_str("group path adopted", opts.group_in_file, group_csv);
    rc |= expect_int("channel map populated", (int)(state.trunk_chan_map_used_count > 0U), 1);
    rc |= expect_int("group policy populated", dsd_tg_policy_has_entries(&state) != 0, 1);
    rc |= expect_int("lcn roll reset", state.lcn_freq_roll, 0);
    rc |= expect_int("cc sync time cleared", (int)state.last_cc_sync_time, 0);

    remove(chan_csv);
    remove(group_csv);
    freeState(&state);
    return rc;
}

static int
test_apply_edacs_variants(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;

    init_test_context(&opts, &state);
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_EDACS_PV;
    p.edacs_ea = 1U;
    p.edacs_esk = 1U;
    p.trunking = 1U;

    rc |= expect_int("edacs apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("edacs apply drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("provoice framing on", opts.frame_provoice, 1);
    rc |= expect_int("ea mode set after preset", state.ea_mode, 1);
    rc |= expect_int("esk mask set after preset", (int)state.esk_mask, 0xA0);

    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_EDACS_PV;
    p.trunking = 1U;
    rc |= expect_int("edacs plain queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("edacs plain drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("ea mode cleared", state.ea_mode, 0);
    rc |= expect_int("esk mask cleared", (int)state.esk_mask, 0);

    freeState(&state);
    return rc;
}

static int
test_apply_simulcast_forces_qpsk(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;

    init_test_context(&opts, &state);
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_TDMA;
    p.simulcast_qpsk = 1U;
    p.p25_prefer_candidates = 1U;
    p.trunking = 1U;

    rc |= expect_int("simulcast queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("simulcast drained", dsd_app_drain_cmds(&opts, &state), 1);
    /* The TDMA preset rewrites the modulation unconditionally, so the override
       has to land after it -- this is the ordering rr_generate.c refuses to
       express as a "-ft -mq" flag pair. */
    rc |= expect_int("p25p1 framing on", opts.frame_p25p1, 1);
    rc |= expect_int("p25p2 framing on", opts.frame_p25p2, 1);
    rc |= expect_int("qpsk forced", opts.mod_qpsk, 1);
    rc |= expect_int("c4fm cleared", opts.mod_c4fm, 0);
    rc |= expect_int("rf_mod qpsk", state.rf_mod, 1);
    rc |= expect_int("cli lock held for config save", opts.mod_cli_lock, 1);
    rc |= expect_int("prefer candidates applied", (int)opts.p25_prefer_candidates, 1);

    freeState(&state);
    return rc;
}

static int
test_apply_refusals(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    int32_t short_payload = 7;

    init_test_context(&opts, &state);
    opts.trunk_scan_enabled = 1;
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_DMR;
    p.trunking = 1U;
    payload_files(&p, "rr_apply_missing_chan.csv", "rr_apply_missing_group.csv");

    rc |= expect_int("trunk-scan apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("trunk-scan apply drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunk-scan refusal leaves trunking off", opts.trunk_enable, 0);
    rc |= expect_str("trunk-scan refusal leaves chan path", opts.chan_in_file, "");

    /* Missing CSVs are caught by the preflight, before any mutation. */
    opts.trunk_scan_enabled = 0;
    rc |= expect_int("missing csv apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("missing csv apply drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("missing csv leaves trunking off", opts.trunk_enable, 0);
    rc |= expect_str("missing csv leaves chan path", opts.chan_in_file, "");

    /* Short payload never reaches the handler: ui_cmd_payload_is_valid drops it,
       but the drain still counts it. */
    rc |= expect_int("short payload queued",
                     dsd_app_command_submit(DSD_APP_CMD_RR_APPLY_IMPORT, &short_payload, sizeof short_payload),
                     DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("short payload counted", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("short payload changed nothing", opts.trunk_enable, 0);

    freeState(&state);
    return rc;
}

static int
test_rr_account_set(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_account_payload account;

    init_test_context(&opts, &state);
    DSD_MEMSET(&account, 0, sizeof account);
    DSD_SNPRINTF(account.username, sizeof account.username, "%s", "operator");
    DSD_SNPRINTF(account.app_key, sizeof account.app_key, "%s", "not-a-real-token");

    rc |= expect_int("account queued", dsd_app_command_set_rr_account(&account), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("account drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("username mirrored", opts.rr_username, "operator");
    rc |= expect_str("app key mirrored", opts.rr_app_key, "not-a-real-token");

    freeState(&state);
    return rc;
}

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
static int
test_apply_requests_the_tune(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;

    init_test_context(&opts, &state);
    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_TDMA;
    p.trunking = 1U;
    p.tune_hz = 851012500U;

    rc |= expect_int("tune apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tune apply drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tune requested once", g_io_control_tune_calls, 1);
    rc |= expect_int("tune requested at plan frequency", (int)g_io_control_tune_freq, 851012500);

    /* A backend that owns no tuner answers -1; the apply still succeeds. */
    reset_io_control_tune_stub(-1);
    rc |= expect_int("untuned apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("untuned apply drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunking still applied", opts.trunk_enable, 1);

    freeState(&state);
    return rc;
}
#endif

int
main(void) {
    int rc = 0;
    rc |= test_fill_payload_trunked_p25();
    rc |= test_fill_payload_conventional_dmr();
    rc |= test_fill_payload_edacs_and_refusals();
    rc |= test_submit_null_guards();
    rc |= test_apply_dmr_trunked_with_files();
    rc |= test_apply_edacs_variants();
    rc |= test_apply_simulcast_forces_qpsk();
    rc |= test_apply_refusals();
    rc |= test_rr_account_set();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_apply_requests_the_tune();
#endif
    if (rc == 0) {
        printf("APP_CONTROL_RR_APPLY: OK\n");
    }
    return rc;
}
