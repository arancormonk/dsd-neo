// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference apply: plan -> payload mapping, and the queue-level contract
 * of DSD_APP_CMD_RR_APPLY_IMPORT / DSD_APP_CMD_RR_ACCOUNT_SET.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/rr_import_apply.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/app_control/commands_internal.h"
#include "../../src/app_control/services.h"
#include "../../src/protocol/tetra/tetra_state.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "dsd-neo/runtime/radioreference_generate.h"
#include "dsd-neo/runtime/radioreference_import.h"

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
static int g_io_control_tune_result = RTL_STREAM_TUNE_OK;
static int g_io_control_tune_calls = 0;
static uint32_t g_io_control_tune_freq = 0U;
static int g_manual_tune_supersedes = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t freq_hz,
                                      uint64_t* out_pending_id);

int
__wrap_io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t freq_hz, uint64_t* out_pending_id) {
    (void)opts;
    (void)state;
    if (out_pending_id) {
        *out_pending_id = 0U;
    }
    g_io_control_tune_calls++;
    g_io_control_tune_freq = freq_hz;
    return g_io_control_tune_result;
}

/* The import's tune supersedes a pending manual tune (issue #671): counted, and superseded for real. */
void __real_dsd_engine_manual_tune_supersede(dsd_state* state);
void __wrap_dsd_engine_manual_tune_supersede(dsd_state* state);

void
__wrap_dsd_engine_manual_tune_supersede(dsd_state* state) {
    g_manual_tune_supersedes++;
    __real_dsd_engine_manual_tune_supersede(state);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static void
reset_io_control_tune_stub(int result) {
    g_io_control_tune_result = result;
    g_io_control_tune_calls = 0;
    g_io_control_tune_freq = 0U;
    g_manual_tune_supersedes = 0;
}
#endif

#ifdef DSD_NEO_TEST_RR_APPLY_WRAP
/* Issue #803: the running-replay and file-load steps an import can fail at, each passed to the real one unless a test
   scripts it. A channel map that fails to load after the preflight read it stands for one gone in between. */
static int g_chan_import_fails = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_svc_import_channel_map(dsd_opts* opts, dsd_state* state, const char* path);
int __wrap_svc_import_channel_map(dsd_opts* opts, dsd_state* state, const char* path);

int
__wrap_svc_import_channel_map(dsd_opts* opts, dsd_state* state, const char* path) {
    return g_chan_import_fails ? -1 : __real_svc_import_channel_map(opts, state, path);
}

#ifdef USE_RADIO
#define RR_SCRIPT_REAL (-100) /* call the real function */
static int g_request_rate_hz = RR_SCRIPT_REAL;
static int g_replan_check_verdict = RR_SCRIPT_REAL;
static int g_replan_check_calls = 0;
static int g_replan_result = RR_SCRIPT_REAL;
static int g_replan_calls = 0;
static int g_recovery_calls = 0;

int __real_rtl_stream_get_request_rate_hz(void);
int __wrap_rtl_stream_get_request_rate_hz(void);
int __real_rtl_stream_replay_replan_check_request(const dsd_opts* opts, const rtl_stream_replay_request* req,
                                                  uint32_t* out_dsp_rate_hz, int* out_resampled, char* why,
                                                  size_t why_size);
int __wrap_rtl_stream_replay_replan_check_request(const dsd_opts* opts, const rtl_stream_replay_request* req,
                                                  uint32_t* out_dsp_rate_hz, int* out_resampled, char* why,
                                                  size_t why_size);
int __real_svc_rtl_replay_replan_locked(dsd_opts* opts, dsd_state* state, const char* reason,
                                        svc_replay_profile_restore_fn restore, const void* restore_ctx,
                                        uint32_t* out_dsp_rate_hz, int* out_resampled, char* why, size_t why_size);
int __wrap_svc_rtl_replay_replan_locked(dsd_opts* opts, dsd_state* state, const char* reason,
                                        svc_replay_profile_restore_fn restore, const void* restore_ctx,
                                        uint32_t* out_dsp_rate_hz, int* out_resampled, char* why, size_t why_size);
int __real_svc_rtl_restart_recovery_locked(dsd_opts* opts, dsd_state* state, int stream_stopped,
                                           const dsd_input_failure* failure_before, int* out_capture_stopped);
int __wrap_svc_rtl_restart_recovery_locked(dsd_opts* opts, dsd_state* state, int stream_stopped,
                                           const dsd_input_failure* failure_before, int* out_capture_stopped);

int
__wrap_rtl_stream_get_request_rate_hz(void) {
    return g_request_rate_hz == RR_SCRIPT_REAL ? __real_rtl_stream_get_request_rate_hz() : g_request_rate_hz;
}

/* The dry re-plan an import's preflight asks (and an analog width check of a running replay). */
int
__wrap_rtl_stream_replay_replan_check_request(const dsd_opts* opts, const rtl_stream_replay_request* req,
                                              uint32_t* out_dsp_rate_hz, int* out_resampled, char* why,
                                              size_t why_size) {
    if (g_replan_check_verdict == RR_SCRIPT_REAL) {
        return __real_rtl_stream_replay_replan_check_request(opts, req, out_dsp_rate_hz, out_resampled, why, why_size);
    }
    g_replan_check_calls++;
    if (why && why_size > 0U) {
        DSD_SNPRINTF(why, why_size, "%s", "no rate chain this capture can run");
    }
    return g_replan_check_verdict;
}

/* The mode change's own re-plan and reopen. A refusal puts the caller's profile back, as the real one does; a failure
   leaves no stream running (svc_rtl_stop_locked()). */
int
__wrap_svc_rtl_replay_replan_locked(dsd_opts* opts, dsd_state* state, const char* reason,
                                    svc_replay_profile_restore_fn restore, const void* restore_ctx,
                                    uint32_t* out_dsp_rate_hz, int* out_resampled, char* why, size_t why_size) {
    if (g_replan_result == RR_SCRIPT_REAL) {
        return __real_svc_rtl_replay_replan_locked(opts, state, reason, restore, restore_ctx, out_dsp_rate_hz,
                                                   out_resampled, why, why_size);
    }
    g_replan_calls++;
    if (g_replan_result == SVC_REPLAY_REPLAN_REFUSED) {
        restore(opts, state, restore_ctx);
        DSD_SNPRINTF(why, why_size, "%s", "the replay did not start at the new rate (see log)");
    } else if (g_replan_result == SVC_REPLAY_REPLAN_REOPENED) {
        if (out_dsp_rate_hz) {
            *out_dsp_rate_hz = 72000U;
        }
        if (out_resampled) {
            *out_resampled = 1;
        }
    } else if (g_replan_result == SVC_REPLAY_REPLAN_FAILED) {
        restore(opts, state, restore_ctx);
        state->rtl_ctx = NULL;
        why[0] = '\0';
    }
    return g_replan_result;
}

int
__wrap_svc_rtl_restart_recovery_locked(dsd_opts* opts, dsd_state* state, int stream_stopped,
                                       const dsd_input_failure* failure_before, int* out_capture_stopped) {
    if (g_replan_result == RR_SCRIPT_REAL) {
        return __real_svc_rtl_restart_recovery_locked(opts, state, stream_stopped, failure_before, out_capture_stopped);
    }
    (void)opts;
    (void)state;
    (void)stream_stopped;
    (void)failure_before;
    g_recovery_calls++;
    if (out_capture_stopped) {
        *out_capture_stopped = 0;
    }
    return 0;
}

static void
reset_replay_script(void) {
    g_request_rate_hz = RR_SCRIPT_REAL;
    g_replan_check_verdict = RR_SCRIPT_REAL;
    g_replan_check_calls = 0;
    g_replan_result = RR_SCRIPT_REAL;
    g_replan_calls = 0;
    g_recovery_calls = 0;
}
#endif
// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

#ifdef DSD_NEO_TEST_AUDIO_ENSURE_WRAP
/* The sink helpers an import's decode-mode change calls, recorded instead of run. */
static int g_ensure_analog_calls = 0;
static int g_ensure_digital_calls = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __wrap_dsd_audio_ensure_analog_output(dsd_opts* opts);
int __wrap_dsd_audio_ensure_digital_output(dsd_opts* opts, const dsd_state* state);

int
__wrap_dsd_audio_ensure_analog_output(dsd_opts* opts) {
    (void)opts;
    g_ensure_analog_calls++;
    return 0;
}

int
__wrap_dsd_audio_ensure_digital_output(dsd_opts* opts, const dsd_state* state) {
    (void)state;
    (void)opts;
    g_ensure_digital_calls++;
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
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

/*
 * dsd_rr_site_is_simulcast() keys off the site description and fires for ANY
 * protocol, which is why dsd_rr_decode_flag() only lets the answer select an
 * alternate for the family it belongs to. The payload has to make the same
 * distinction: forcing QPSK on a DMR/NXDN/EDACS site merely DESCRIBED as
 * "Simulcast" publishes a QPSK symbol profile for a C4FM protocol, decodes
 * nothing, and is then stored in the .rr recipe and re-applied forever.
 */
static int
test_fill_payload_simulcast_is_p25_only(void) {
    int rc = 0;
    dsd_rr_import_plan plan;
    dsd_app_rr_apply_payload payload;

    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_DMR_CAPPLUS;
    plan.trunking = 1;
    plan.simulcast = 1;
    plan.tune_hz = 851012500LL;
    rc |= expect_int("dmr simulcast maps", dsd_app_rr_fill_apply_payload(&plan, "d chan.csv", "", &payload), 0);
    rc |= expect_int("dmr simulcast does not force qpsk", payload.simulcast_qpsk, 0);

    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_EDACS_STD;
    plan.trunking = 1;
    plan.simulcast = 1;
    plan.tune_hz = 851012500LL;
    rc |= expect_int("edacs simulcast maps", dsd_app_rr_fill_apply_payload(&plan, "", "e group.csv", &payload), 0);
    rc |= expect_int("edacs simulcast does not force qpsk", payload.simulcast_qpsk, 0);

    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_NXDN48;
    plan.trunking = 1;
    plan.simulcast = 1;
    plan.tune_hz = 851012500LL;
    rc |= expect_int("nxdn simulcast maps", dsd_app_rr_fill_apply_payload(&plan, "", "n group.csv", &payload), 0);
    rc |= expect_int("nxdn simulcast does not force qpsk", payload.simulcast_qpsk, 0);

    /* Conventional P25 does carry the alternate, so it must still force QPSK. */
    plan_init(&plan);
    plan.protocol = DSD_RR_PROTO_P25_CONV;
    plan.conventional = 1;
    plan.scan_list = 1;
    plan.simulcast = 1;
    plan.tune_hz = 851012500LL;
    rc |= expect_int("p25 conv simulcast maps", dsd_app_rr_fill_apply_payload(&plan, "p chan.csv", "", &payload), 0);
    rc |= expect_int("p25 conv simulcast forces qpsk", payload.simulcast_qpsk, 1);
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
seed_carrier_codes(dsd_state* state) {
    state->dmr_color_code = 5U;
    state->nxdn_last_ran = 9U;
    state->nxdn_last_ran_stand_in = 1U;
    state->dpmr_color_code = 33;
    state->p2_cc = 0x293U;
    state->p2_cc_verified = 1U;
}

static int
expect_carrier_codes(const char* what, const dsd_state* state, int forgotten) {
    char label[96];
    int rc = 0;
    DSD_SNPRINTF(label, sizeof label, "%s: DMR colour code", what);
    rc |= expect_int(label, (int)state->dmr_color_code, forgotten ? 16 : 5);
    DSD_SNPRINTF(label, sizeof label, "%s: NXDN RAN", what);
    rc |= expect_int(label, (int)state->nxdn_last_ran, forgotten ? -1 : 9);
    DSD_SNPRINTF(label, sizeof label, "%s: NXDN stand-in mark", what);
    rc |= expect_int(label, (int)state->nxdn_last_ran_stand_in, forgotten ? 0 : 1);
    DSD_SNPRINTF(label, sizeof label, "%s: dPMR colour code", what);
    rc |= expect_int(label, state->dpmr_color_code, forgotten ? -1 : 33);
    DSD_SNPRINTF(label, sizeof label, "%s: Phase 2 seed proof", what);
    rc |= expect_int(label, (int)state->p2_cc_verified, forgotten ? 0 : 1);
    /* The seed itself stays: a later carrier proves it again before it names a NAC. */
    DSD_SNPRINTF(label, sizeof label, "%s: Phase 2 seed", what);
    rc |= expect_int(label, (int)state->p2_cc, 0x293);
    return rc;
}

static void
init_test_context(dsd_opts* opts, dsd_state* state) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    initOpts(opts);
    /* An import's decode-mode change opens the new mode's sink when the session plays to a local audio device, which
       is what initOpts() selects; the tests play to the null output so none of them opens a host audio stream. */
    opts->audio_out_type = 9;
    initState(state);
    dsd_app_frontend_runtime_start(opts, state);
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
    state.trunk_recovery_protocol = DSD_TRUNK_RECOVERY_NXDN; // the old system was an NXDN (here IDAS Type-D) site

#ifdef DSD_NEO_TEST_AUDIO_ENSURE_WRAP
    g_ensure_analog_calls = g_ensure_digital_calls = 0;
#endif
    rc |= expect_int("rr apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("rr apply drained", dsd_app_drain_cmds(&opts, &state), 1);
#ifdef DSD_NEO_TEST_AUDIO_ENSURE_WRAP
    rc |= expect_int("rr import ensures the digital sink", g_ensure_digital_calls, 1);
    rc |= expect_int("rr import leaves the raw sink", g_ensure_analog_calls, 0);
#endif
    rc |= expect_int("dmr framing on", opts.frame_dmr, 1);
    rc |= expect_int("trunking on", opts.trunk_enable, 1);
    rc |= expect_int("scanner off", opts.scanner_mode, 0);
    rc |= expect_str("chan path adopted", opts.chan_in_file, chan_csv);
    rc |= expect_str("group path adopted", opts.group_in_file, group_csv);
    rc |= expect_int("channel map populated", (int)(state.trunk_chan_map_used_count > 0U), 1);
    rc |= expect_int("group policy populated", dsd_tg_policy_has_entries(&state) != 0, 1);
    rc |= expect_int("lcn roll reset", state.lcn_freq_roll, 0);
    rc |= expect_int("cc sync time cleared", (int)state.last_cc_sync_time, 0);
    rc |=
        expect_int("an NXDN owner goes with the old system", state.trunk_recovery_protocol, DSD_TRUNK_RECOVERY_UNKNOWN);

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

/*
 * Issue #524: the import's decode-mode bound runs through TETRA, the last preset. AM on a radio input applies, and
 * TETRA at the 72 kHz DSP bandwidth; the value past TETRA is refused, as is AM on a PCM input, whose audio arrives
 * already demodulated (dsd_decode_mode_runs_on_input(), as DECODE_MODE_SET refuses it), with that reason (issue #803).
 * A refusal changes nothing.
 */
static int
test_apply_am_bounds(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;

    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL; /* a radio input, no stream running */
    opts.rtl_dsp_bw_khz = 72;          /* the bandwidth TETRA is set up for (issue #803) */
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_TETRA + 1;
    state.ui_msg[0] = '\0';
    rc |= expect_int("past tetra queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("past tetra drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("past tetra refused", state.ui_msg, "Failed: RR import -> decode mode");
    rc |= expect_int("past tetra leaves the mode", opts.analog_only, 0);
    rc |= expect_int("past tetra leaves the TETRA decoder off", opts.frame_tetra, 0);

    p.decode_mode = (int32_t)DSDCFG_MODE_TETRA;
    state.ui_msg[0] = '\0';
    rc |= expect_int("tetra queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tetra drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tetra applied", dsd_infer_decode_mode_preset(&opts), (int)DSDCFG_MODE_TETRA);
    rc |= expect_str("tetra applied toast", state.ui_msg, "Applied: RR import (TETRA); save via Config menu");

    p.decode_mode = (int32_t)DSDCFG_MODE_AM;
    state.ui_msg[0] = '\0';
    rc |= expect_int("am on radio queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("am on radio drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("am on radio applied", dsd_infer_decode_mode_preset(&opts), (int)DSDCFG_MODE_AM);
    rc |= expect_int("am on radio runs the AM detector", opts.analog_demod, DSD_ANALOG_DEMOD_AM);
    freeState(&state);

    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    state.ui_msg[0] = '\0';
    rc |= expect_int("am on pcm queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("am on pcm drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("am on pcm refused", state.ui_msg, "Failed: RR import -> AM needs an IQ radio input");
    rc |= expect_int("am on pcm leaves the mode", opts.analog_only, 0);
    rc |= expect_int("am on pcm leaves the kind", opts.analog_demod, DSD_ANALOG_DEMOD_FM);
    freeState(&state);
    return rc;
}

/* ---- Issue #803: the receiver verdict and the commit point -------------------------------------------------------- */

static int
expect_contains(const char* tag, const char* got, const char* want) {
    if (strstr(got, want) == NULL) {
        DSD_FPRINTF(stderr, "%s: \"%s\" does not contain \"%s\"\n", tag, got, want);
        return 1;
    }
    return 0;
}

/* The INI Config -> Save would write now (dsd_snapshot_opts_to_user_config()), into @p path. */
static int
render_saved_config(const dsd_opts* opts, const dsd_state* state, const char* path) {
    dsdneoUserConfig* cfg = (dsdneoUserConfig*)calloc(1, sizeof *cfg);
    FILE* f = dsd_fopen_private(path, "wb");
    int rc = (cfg && f) ? 0 : 1;
    if (rc == 0) {
        dsd_snapshot_opts_to_user_config(opts, state, cfg);
        dsd_user_config_render_ini(cfg, f);
    }
    if (f && fclose(f) != 0) {
        rc = 1;
    }
    free(cfg);
    return rc;
}

static int
files_equal(const char* a, const char* b) {
    FILE* fa = dsd_fopen_private(a, "rb");
    FILE* fb = dsd_fopen_private(b, "rb");
    int same = fa && fb;
    while (same) {
        const int ca = fgetc(fa);
        const int cb = fgetc(fb);
        same = ca == cb;
        if (ca == EOF || cb == EOF) {
            break;
        }
    }
    if (fa) {
        (void)fclose(fa);
    }
    if (fb) {
        (void)fclose(fb);
    }
    return same;
}

static const char k_saved_before[] = "rr_apply_saved_before.ini";
static const char k_saved_after[] = "rr_apply_saved_after.ini";
static const char k_tetra_group_csv[] = "rr_apply_tetra_group.csv";

/* A session an import that is refused must leave as it was: a -Y list mid-scan, a call followed onto a voice channel,
   the carrier's codes, and the files it already uses. */
static void
seed_live_session(dsd_opts* opts, dsd_state* state) {
    opts->scanner_mode = 1;
    state->lcn_freq_count = 3;
    state->trunk_lcn_freq[0] = 851375000;
    state->trunk_lcn_freq[1] = 852375000;
    state->trunk_lcn_freq[2] = 853375000;
    state->lcn_freq_roll = 2;
    state->trunk_vc_freq[0] = 852375000;
    state->payload_keyid = 7;
    seed_carrier_codes(state);
    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", "earlier_chan.csv");
    DSD_SNPRINTF(opts->group_in_file, sizeof opts->group_in_file, "%s", "earlier_group.csv");
}

static int
expect_session_kept(const char* what, const dsd_opts* opts, const dsd_state* state, int mode_before) {
    char label[128];
    int rc = 0;
    DSD_SNPRINTF(label, sizeof label, "%s: mode", what);
    rc |= expect_int(label, dsd_infer_decode_mode_preset(opts), mode_before);
    DSD_SNPRINTF(label, sizeof label, "%s: -Y scan still running", what);
    rc |= expect_int(label, opts->scanner_mode == 1 && state->lcn_freq_count == 3 && state->lcn_freq_roll == 2, 1);
    DSD_SNPRINTF(label, sizeof label, "%s: trunking off", what);
    rc |= expect_int(label, opts->trunk_enable, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: the call goes on", what);
    rc |= expect_int(label, state->trunk_vc_freq[0] == 852375000 && state->payload_keyid == 7, 1);
    rc |= expect_carrier_codes(what, state, 0);
    DSD_SNPRINTF(label, sizeof label, "%s: channel map", what);
    rc |= expect_str(label, opts->chan_in_file, "earlier_chan.csv");
    DSD_SNPRINTF(label, sizeof label, "%s: group list", what);
    rc |= expect_str(label, opts->group_in_file, "earlier_group.csv");
    return rc;
}

/* Queue @p p on a session seeded as live, drain it, and expect it refused with @p toast and nothing changed: the
   session, and what Config -> Save would write. */
static int
expect_refused_import(const char* what, dsd_opts* opts, dsd_state* state, const dsd_app_rr_apply_payload* p,
                      const char* toast) {
    char label[128];
    int rc = 0;
    seed_live_session(opts, state);
    const int mode_before = dsd_infer_decode_mode_preset(opts);
    rc |= render_saved_config(opts, state, k_saved_before);
    state->ui_msg[0] = '\0';
    DSD_SNPRINTF(label, sizeof label, "%s: queued", what);
    rc |= expect_int(label, dsd_app_command_set_rr_apply(p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    DSD_SNPRINTF(label, sizeof label, "%s: drained", what);
    rc |= expect_int(label, dsd_app_drain_cmds(opts, state), 1);
    DSD_SNPRINTF(label, sizeof label, "%s: toast", what);
    rc |= expect_str(label, state->ui_msg, toast);
    rc |= expect_session_kept(what, opts, state, mode_before);
    rc |= render_saved_config(opts, state, k_saved_after);
    DSD_SNPRINTF(label, sizeof label, "%s: saved config unchanged", what);
    rc |= expect_int(label, files_equal(k_saved_before, k_saved_after), 1);
    (void)remove(k_saved_before);
    (void)remove(k_saved_after);
    return rc;
}

static void
tetra_payload(dsd_app_rr_apply_payload* p) {
    DSD_MEMSET(p, 0, sizeof *p);
    p->decode_mode = (int32_t)DSDCFG_MODE_TETRA;
    p->trunking = 1U;
    p->tune_hz = 390012500U;
    p->has_group = 1U;
    DSD_SNPRINTF(p->group_path, sizeof p->group_path, "%s", k_tetra_group_csv);
}

/*
 * Main-failing regression (T1, T3): a TETRA import the receiver cannot decode is refused with its reason before it
 * changes anything. On main an RTL-SDR at 48 kHz, an Airspy and a --tetra-uplink session took it ("Applied: RR import
 * (TETRA)", nothing decoded), and PCM was refused as "decode mode".
 */
static int
test_apply_tetra_receiver_refusals(void) {
    int rc = write_text(k_tetra_group_csv, "id,mode,name\n1001,A,Ops\n");
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    tetra_payload(&p);

    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL; /* an RTL-SDR at the default 48 kHz, no stream running */
    rc |= expect_int("rtl default bandwidth", opts.rtl_dsp_bw_khz, 48);
    rc |= expect_refused_import("tetra on rtl at 48 kHz", &opts, &state, &p,
                                "Failed: RR import -> TETRA needs 72 kHz DSP bandwidth; set it and import again");
    freeState(&state);

    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy");
    rc |= expect_refused_import("tetra on airspy", &opts, &state, &p,
                                "Failed: RR import -> an Airspy cannot decode TETRA yet; use RTL-SDR or rtl_tcp");
    freeState(&state);

    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 72;
    opts.tetra_uplink = 1;
    rc |= expect_refused_import("tetra uplink session", &opts, &state, &p,
                                "Failed: RR import -> TETRA uplink session; restart without --tetra-uplink");
    freeState(&state);

    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_PULSE;
    rc |= expect_refused_import("tetra on pcm", &opts, &state, &p,
                                "Failed: RR import -> TETRA needs an IQ radio input or a symbol capture");
    freeState(&state);
    (void)remove(k_tetra_group_csv);
    return rc;
}

#ifdef USE_RADIO
/*
 * Main-failing regression (spec §10, preflight completeness): an AM import whose width the front end cannot take is
 * refused before the commit point. On main the carrier boundary and the -Y leave ran first, ending the call and the
 * scan, and only then did the mode change refuse.
 */
static int
test_apply_am_profile_refused_before_commit(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dsp_bw_khz = 8;
    opts.analog_am_bandwidth_hz = 10000;
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_AM;
    p.tune_hz = 127850000U;
    /* The width check's own reason, as an AM switch names it. */
    rc |= expect_refused_import("am too wide", &opts, &state, &p,
                                "Failed: RR import -> AM 10 kHz does not fit the 8 kHz DSP rate (max 6 kHz); use a 16, "
                                "24, 48 or 72 kHz DSP bandwidth");
    freeState(&state);
    return rc;
}
#endif

/* New behaviour (issue #803, and the carried Task 4 check): a TETRA import an RTL-SDR at 72 kHz decodes is taken,
   leaves the DSP bandwidth alone, forgets the main carriers the TETRA machine moved to for the old system, and saves
   the mode and the group list it loaded. */
static int
test_apply_tetra_accepted(void) {
    int rc = write_text(k_tetra_group_csv, "id,mode,name\n1001,A,Ops\n");
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "rtl:0");
    dsd_opts_set_rtl_dsp_bw(&opts, 72, DSD_RTL_DSP_BW_SOURCE_EXPLICIT);

    /* The session already decodes TETRA (an earlier import), and its machine has moved to a main carrier. */
    tetra_payload(&p);
    p.has_group = 0U;
    p.group_path[0] = '\0';
    rc |= expect_int("first tetra import queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("first tetra import drained", dsd_app_drain_cmds(&opts, &state), 1);
    tetra_state_t* ts = tetra_st(&state);
    rc |= expect_int("tetra state", ts != NULL, 1);
    if (ts) {
        ts->sm.acq.moves[0].hz = 390112500L;
        ts->sm.acq.moves[0].consumed = 1U;
        ts->sm.acq.staged_hz = 390212500L;
        ts->sm.acq.full_noted = 1U;
    }

    tetra_payload(&p);
    state.ui_msg[0] = '\0';
    rc |= expect_int("tetra import queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tetra import drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("tetra import toast", state.ui_msg, "Applied: RR import (TETRA) +trunking; save via Config menu");
    rc |= expect_int("tetra mode", dsd_infer_decode_mode_preset(&opts), DSDCFG_MODE_TETRA);
    rc |= expect_int("trunking on", opts.trunk_enable, 1);
    rc |= expect_str("group list loaded", opts.group_in_file, k_tetra_group_csv);
    rc |= expect_int("bandwidth kept",
                     opts.rtl_dsp_bw_khz == 72 && opts.rtl_dsp_bw_source == DSD_RTL_DSP_BW_SOURCE_EXPLICIT, 1);
    ts = tetra_st_existing(&state);
    rc |= expect_int("the TETRA state stays across a TETRA import", ts != NULL, 1);
    if (ts) {
        rc |= expect_int("the main-carrier moves are forgotten",
                         ts->sm.acq.moves[0].hz == 0L && ts->sm.acq.moves[0].consumed == 0U
                             && ts->sm.acq.staged_hz == 0L && ts->sm.acq.full_noted == 0U,
                         1);
    }

    dsdneoUserConfig* cfg = (dsdneoUserConfig*)calloc(1, sizeof *cfg);
    rc |= expect_int("config", cfg != NULL, 1);
    if (cfg) {
        dsd_snapshot_opts_to_user_config(&opts, &state, cfg);
        rc |= expect_int("saved mode", cfg->decode_mode, DSDCFG_MODE_TETRA);
        rc |= expect_str("saved group list", cfg->trunk_group_csv, k_tetra_group_csv);
        rc |= expect_int("saved bandwidth", cfg->rtl_bw_khz, 72);
        free(cfg);
    }
    freeState(&state);
    (void)remove(k_tetra_group_csv);
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
    rc |= expect_int("the tune supersedes a pending manual tune", g_manual_tune_supersedes, 1);

    /* A deferred tune never reached the tuner: a manual tune still pending keeps it. */
    reset_io_control_tune_stub(RTL_STREAM_TUNE_DEFERRED);
    rc |= expect_int("deferred apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("deferred apply drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("a deferred tune supersedes nothing", g_manual_tune_supersedes, 0);

    /* A backend that owns no tuner answers -1; the apply still succeeds. */
    reset_io_control_tune_stub(-1);
    rc |= expect_int("untuned apply queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("untuned apply drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("trunking still applied", opts.trunk_enable, 1);

    freeState(&state);
    return rc;
}

/* Issue #575: an import that moves the receiver to its system's channel -- it carries a frequency, and the session owns
   a tuner -- is a carrier boundary, which runs before the import ends anything, so the codes the old carrier decoded go
   whatever the tuner then answers. An import with no frequency, or on a session that owns no tuner, keeps them. */
static int
test_apply_tune_forgets_the_carrier_codes(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;

    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_TDMA;
    p.trunking = 1U;
    p.tune_hz = 851012500U;

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    seed_carrier_codes(&state);
    rc |= expect_int("tuned import queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tuned import drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_carrier_codes("tuned import", &state, 1);

    reset_io_control_tune_stub(RTL_STREAM_TUNE_TIMEOUT);
    seed_carrier_codes(&state);
    rc |= expect_int("pending import queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("pending import drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_carrier_codes("pending import", &state, 1);

    reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
    p.tune_hz = 0U;
    seed_carrier_codes(&state);
    rc |= expect_int("tuneless import queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("tuneless import drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("tuneless import asks no tune", g_io_control_tune_calls, 0);
    rc |= expect_int("tuneless import supersedes nothing", g_manual_tune_supersedes, 0);
    rc |= expect_carrier_codes("tuneless import", &state, 0);
    freeState(&state);

    /* A session with no tuner: the tune is asked, the backend answers -1, and the receiver stayed. */
    init_test_context(&opts, &state);
    p.tune_hz = 851012500U;
    reset_io_control_tune_stub(-1);
    seed_carrier_codes(&state);
    rc |= expect_int("untuned import queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("untuned import drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_carrier_codes("untuned import", &state, 0);

    freeState(&state);
    return rc;
}
#endif

#ifdef DSD_NEO_TEST_RR_APPLY_WRAP
/* Main-failing regression (spec §3.3): a channel map that does not load after the commit point is a partial apply that
   says what took, never "Applied" and never the bare "CSV import" main reported. */
static int
test_apply_partial_files(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    const char* chan_csv = "rr_apply_partial_chan.csv";
    const char* group_csv = "rr_apply_partial_group.csv";
    rc |= write_text(chan_csv, "ChannelNumber(dec),frequency(Hz),note\n1,851012500,cc\n");
    rc |= write_text(group_csv, "id,mode,name\n101,A,Dispatch\n");
    dsd_app_rr_apply_payload p;
    init_test_context(&opts, &state);
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_DMR;
    p.trunking = 1U;
    payload_files(&p, chan_csv, group_csv);

    g_chan_import_fails = 1;
    rc |= expect_int("partial queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("partial drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_str("partial toast", state.ui_msg,
                     "Failed: RR import -> channel map not loaded; mode and talkgroups applied");
    rc |= expect_int("partial mode applied", opts.frame_dmr, 1);
    rc |= expect_str("partial group list applied", opts.group_in_file, group_csv);

    p.has_group = 0U;
    p.group_path[0] = '\0';
    rc |= expect_int("partial no group queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("partial no group drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |=
        expect_str("partial no group toast", state.ui_msg, "Failed: RR import -> channel map not loaded; mode applied");
    g_chan_import_fails = 0;

    (void)remove(chan_csv);
    (void)remove(group_csv);
    freeState(&state);
    return rc;
}
#endif

#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
/* Main-failing regression (spec §10): an import on an I/Q replay never tunes, rigctl peer or not. The capture decides
   the frequency; on main the import crossed a receiver-move boundary (the carrier's codes went) and asked the tuner,
   which with a rigctl peer connected moves a radio the replay does not hear. */
static int
test_apply_replay_never_tunes(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    for (int with_rigctl = 0; with_rigctl <= 1; with_rigctl++) {
        init_test_context(&opts, &state);
        opts.audio_in_type = AUDIO_IN_RTL;
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "iqreplay:rr_apply_capture.json");
        if (with_rigctl) {
            /* A connected peer as dsd_opts_rigctl_live() sees one; nothing is sent to it. */
            opts.use_rigctl = 1;
            opts.rigctl_sockfd = (dsd_socket_t)4242;
        }
        DSD_MEMSET(&p, 0, sizeof p);
        p.decode_mode = (int32_t)DSDCFG_MODE_TDMA;
        p.trunking = 1U;
        p.tune_hz = 851012500U;
        reset_io_control_tune_stub(RTL_STREAM_TUNE_OK);
        seed_carrier_codes(&state);
        state.ui_msg[0] = '\0';
        const char* what = with_rigctl ? "replay with rigctl" : "replay";
        rc |= expect_int(what, dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int(what, dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int(with_rigctl ? "replay with rigctl: no tune" : "replay: no tune", g_io_control_tune_calls, 0);
        rc |= expect_int(with_rigctl ? "replay with rigctl: no pending record" : "replay: no pending record",
                         g_manual_tune_supersedes, 0);
        rc |= expect_carrier_codes(with_rigctl ? "replay with rigctl: no boundary" : "replay: no boundary", &state, 0);
        rc |= expect_contains(what, state.ui_msg, "Applied: RR import");
        rc |= expect_int(with_rigctl ? "replay with rigctl: no tune message" : "replay: no tune message",
                         strstr(state.ui_msg, "tune") == NULL, 1);
        rc |= expect_int(what, opts.trunk_enable, 1);
        opts.use_rigctl = 0;
        opts.rigctl_sockfd = DSD_INVALID_SOCKET;
        freeState(&state);
    }
    return rc;
}
#endif

#if defined(DSD_NEO_TEST_RR_APPLY_WRAP) && defined(USE_RADIO)
/* A running replay as the apply sees one: the stream handle only marks it running. Its storage is zeroed and pointer
   sized, so a call that reads the context's stream finds none (rtl_stream_c.cpp checks ctx->stream), and the calls that
   would act on a stream are scripted. */
static void* g_fake_stream[4];

static void
replay_session(dsd_opts* opts, dsd_state* state) {
    init_test_context(opts, state);
    opts->audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "iqreplay:rr_apply_capture.json");
    state->rtl_ctx = (struct RtlSdrContext*)(void*)g_fake_stream;
    reset_replay_script();
}

/* Main-failing regression (spec §3.3): a replay that cannot run the import's mode is refused by the dry re-plan before
   the commit point, with TETRA's rate advice. On main the -Y scan was left and the call ended, and the mode change's
   refusal was then overwritten by "decode mode". */
static int
test_apply_replay_replan_refused_before_commit(void) {
    int rc = write_text(k_tetra_group_csv, "id,mode,name\n1001,A,Ops\n");
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    tetra_payload(&p);
    p.tune_hz = 0U;

    replay_session(&opts, &state);
    g_request_rate_hz = 48000;
    g_replan_check_verdict = RTL_STREAM_REPLAN_REFUSED;
    g_replan_result = SVC_REPLAY_REPLAN_REFUSED; /* what the mode change would meet, had the import got that far */
    rc |= expect_refused_import(
        "tetra on a 48 kHz replay", &opts, &state, &p,
        "Failed: RR import -> DSP rate 48 kHz is no multiple of 18 kHz, and this replay cannot be resampled to one");
    rc |= expect_int("dry re-plan asked", g_replan_check_calls > 0, 1);
    rc |= expect_int("no reopen tried", g_replan_calls, 0);
    state.rtl_ctx = NULL;
    freeState(&state);

    replay_session(&opts, &state);
    g_request_rate_hz = 72000;
    g_replan_check_verdict = RTL_STREAM_REPLAN_REFUSED;
    g_replan_result = SVC_REPLAY_REPLAN_REFUSED;
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_P25P1;
    p.trunking = 1U;
    rc |= expect_refused_import("p25 on a TETRA-rate replay", &opts, &state, &p,
                                "Failed: RR import -> no rate chain this capture can run");
    rc |= expect_int("p25: no reopen tried", g_replan_calls, 0);
    state.rtl_ctx = NULL;
    freeState(&state);
    reset_replay_script();
    (void)remove(k_tetra_group_csv);
    return rc;
}

/* Main-failing regression (T3): a mode change that fails after the commit point keeps its own toast (the replay
   reopen's reason, or the rollback's), which main overwrote with "Failed: RR import -> decode mode"; a reopen that
   left no stream runs the recovery restart once. */
static int
test_apply_mode_change_failure_keeps_its_reason(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_P25P1;
    p.trunking = 1U;

    replay_session(&opts, &state);
    g_replan_check_verdict = RTL_STREAM_REPLAN_DIFFERS;
    g_replan_result = SVC_REPLAY_REPLAN_REFUSED;
    const int mode_before = dsd_infer_decode_mode_preset(&opts);
    state.ui_msg[0] = '\0';
    rc |= expect_int("reopen refused queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("reopen refused drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("reopen tried", g_replan_calls, 1);
    rc |= expect_str("reopen refused keeps its reason", state.ui_msg,
                     "Failed: P25 Phase 1 -> the replay did not start at the new rate (see log)");
    rc |= expect_int("reopen refused keeps the mode", dsd_infer_decode_mode_preset(&opts), mode_before);
    rc |= expect_int("reopen refused: trunking not applied", opts.trunk_enable, 0);
    state.rtl_ctx = NULL;
    freeState(&state);

    replay_session(&opts, &state);
    g_replan_check_verdict = RTL_STREAM_REPLAN_DIFFERS;
    g_replan_result = SVC_REPLAY_REPLAN_FAILED;
    state.ui_msg[0] = '\0';
    rc |= expect_int("reopen failed queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("reopen failed drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("recovery restart ran once", g_recovery_calls, 1);
    rc |= expect_contains("reopen failed keeps the rollback toast", state.ui_msg, "Failed: ");
    rc |= expect_int("reopen failed is not the bare decode-mode text",
                     strcmp(state.ui_msg, "Failed: RR import -> decode mode") != 0, 1);
    rc |= expect_int("reopen failed keeps the mode", dsd_infer_decode_mode_preset(&opts), mode_before);
    state.rtl_ctx = NULL;
    freeState(&state);
    reset_replay_script();
    return rc;
}

/* Main-failing regression (T3): what the mode change had to say on success (here, the rate the replay was reopened
   and resampled at) is carried by the import's own toast, which on main replaced it with the reminder to save. */
static int
test_apply_success_keeps_the_mode_note(void) {
    int rc = write_text(k_tetra_group_csv, "id,mode,name\n1001,A,Ops\n");
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    tetra_payload(&p);
    p.tune_hz = 0U;
    replay_session(&opts, &state);
    g_replan_check_verdict = RTL_STREAM_REPLAN_DIFFERS;
    g_replan_result = SVC_REPLAY_REPLAN_REOPENED;
    state.ui_msg[0] = '\0';
    rc |= expect_int("reopened queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("reopened drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |=
        expect_str("reopened toast", state.ui_msg, "Applied: RR import (TETRA) +trunking; replay resampled to 72 kHz");
    rc |= expect_int("reopened mode", dsd_infer_decode_mode_preset(&opts), DSDCFG_MODE_TETRA);
    state.rtl_ctx = NULL;
    reset_replay_script();
    freeState(&state);
    (void)remove(k_tetra_group_csv);
    return rc;
}

/* New behaviour (D3): the decoder thread checks a SoapySDR device at the rate the running stream committed, which a
   frontend cannot see. */
static int
test_apply_tetra_soapy_rate(void) {
    int rc = write_text(k_tetra_group_csv, "id,mode,name\n1001,A,Ops\n");
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    tetra_payload(&p);
    p.tune_hz = 0U;
    init_test_context(&opts, &state);
    opts.audio_in_type = AUDIO_IN_RTL;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "soapy:driver=hackrf");
    state.rtl_ctx = (struct RtlSdrContext*)(void*)g_fake_stream;
    reset_replay_script();
    g_request_rate_hz = 48000;
    rc |= expect_refused_import(
        "tetra on a 48 kHz soapy device", &opts, &state, &p,
        "Failed: RR import -> DSP rate 48 kHz is no multiple of 18 kHz; set the DSP bandwidth to 72 kHz");
    state.rtl_ctx = NULL;
    reset_replay_script();
    freeState(&state);
    (void)remove(k_tetra_group_csv);
    return rc;
}
#endif

/* An import drops a recovery owner its decode mode no longer decodes (P25 or DMR before an IDAS import), so the new
   system's startup hunt can run; an owner the new mode still decodes stays. */
static int
test_apply_drops_an_owner_the_mode_cannot_decode(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    dsd_app_rr_apply_payload p;
    const int owners[] = {DSD_TRUNK_RECOVERY_P25, DSD_TRUNK_RECOVERY_DMR};
    for (size_t i = 0; i < sizeof owners / sizeof owners[0]; i++) {
        init_test_context(&opts, &state);
        DSD_MEMSET(&p, 0, sizeof p);
        p.decode_mode = (int32_t)DSDCFG_MODE_NXDN48;
        p.trunking = 1U;
        state.trunk_recovery_protocol = owners[i];
        rc |= expect_int("nxdn import queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
        rc |= expect_int("nxdn import drained", dsd_app_drain_cmds(&opts, &state), 1);
        rc |= expect_int("an owner NXDN mode cannot decode is dropped", state.trunk_recovery_protocol,
                         DSD_TRUNK_RECOVERY_UNKNOWN);
        freeState(&state);
    }
    init_test_context(&opts, &state);
    DSD_MEMSET(&p, 0, sizeof p);
    p.decode_mode = (int32_t)DSDCFG_MODE_DMR;
    p.trunking = 1U;
    state.trunk_recovery_protocol = DSD_TRUNK_RECOVERY_DMR;
    rc |= expect_int("dmr import queued", dsd_app_command_set_rr_apply(&p), DSD_APP_COMMAND_SUBMIT_QUEUED);
    rc |= expect_int("dmr import drained", dsd_app_drain_cmds(&opts, &state), 1);
    rc |= expect_int("an owner the mode decodes stays", state.trunk_recovery_protocol, DSD_TRUNK_RECOVERY_DMR);
    freeState(&state);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_fill_payload_trunked_p25();
    rc |= test_fill_payload_conventional_dmr();
    rc |= test_fill_payload_simulcast_is_p25_only();
    rc |= test_fill_payload_edacs_and_refusals();
    rc |= test_submit_null_guards();
    rc |= test_apply_dmr_trunked_with_files();
    rc |= test_apply_edacs_variants();
    rc |= test_apply_simulcast_forces_qpsk();
    rc |= test_apply_refusals();
    rc |= test_apply_am_bounds();
    rc |= test_apply_tetra_receiver_refusals();
#ifdef USE_RADIO
    rc |= test_apply_am_profile_refused_before_commit(); /* a radio-off build holds no width to a DSP rate */
#endif
    rc |= test_rr_account_set();
    rc |= test_apply_drops_an_owner_the_mode_cannot_decode();
    rc |= test_apply_tetra_accepted();
#ifdef DSD_NEO_TEST_IO_CONTROL_WRAP
    rc |= test_apply_requests_the_tune();
    rc |= test_apply_tune_forgets_the_carrier_codes();
    rc |= test_apply_replay_never_tunes();
#endif
#ifdef DSD_NEO_TEST_RR_APPLY_WRAP
    rc |= test_apply_partial_files();
#ifdef USE_RADIO
    rc |= test_apply_replay_replan_refused_before_commit();
    rc |= test_apply_mode_change_failure_keeps_its_reason();
    rc |= test_apply_success_keeps_the_mode_note();
    rc |= test_apply_tetra_soapy_rate();
#endif
#endif
    if (rc == 0) {
        printf("APP_CONTROL_RR_APPLY: OK\n");
    }
    return rc;
}
