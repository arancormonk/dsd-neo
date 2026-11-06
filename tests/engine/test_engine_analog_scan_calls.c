// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The scanners and the analog call tracker together (issue #667), the real engine on audio input with a rigctl peer
 * whose replies GNU ld --wrap stands in for. The channel and trunk scan tests stub the tracker's end; here it runs:
 *  - a -Y leave ends a call with its row and discards a candidate, which leaves none;
 *  - a trunk-scan advance whose preparation succeeds ends the call or discards the candidate before both snapshots,
 *    and one whose preparation fails leaves either running; a rollback after every retune failed, and a revisit of the
 *    target left, restore neither;
 *  - the carrier boundary a Signal search pause or failure runs discards a candidate;
 *  - D9: an analog call on audio input is heard on the frequency a scan confirmed it landed its rigctl peer on (a typed
 *    or untyped -Y row's commit, a trunk-scan conventional target's completed retune), and on none after a failed tune,
 *    a placeholder row, a map replacement, a manual takeover, a rollback, or a rigctl reconnect or disconnect
 *    (svc_rigctl_connect(), with Connect() stood in for too); an untyped list's manual Next and Previous (`C`, `J`)
 *    name the row they landed, and a refused one, or the operator's own tune (`=`), names none.
 * A reception is a call once it ran DSD_ANALOG_CALL_MIN_MS; a candidate shows by what follows it: one that was carried
 * on qualifies on blocks that alone are too short, and one discarded does not.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/channel_mode.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/engine/analog_call.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "analog_call_test_support.h"
#include "commands_internal.h"
#include "scan_landing.h"
#include "services.h"
#include "test_support.h"

static int g_failures = 0;

static void
expect(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

/* ---------------- The rigctl peer and the key preparation, stood in for ---------------- */

static int g_setfreq_ok = 1;
static long g_setfreq_freq = 0;
/* What the next rigctl connect returns. */
static dsd_socket_t g_connect_result = DSD_INVALID_SOCKET;
/* The key preparations to let through before one fails (-1: none fails). */
static int g_key_prepare_fail_after = -1;

// GNU ld --wrap requires these exact external symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
dsd_socket_t __wrap_Connect(char* hostname, int portno);
bool __wrap_SetFreq(dsd_socket_t sockfd, long int freq);
bool __wrap_SetModulation(dsd_socket_t sockfd, int bandwidth);
bool __wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth);
bool __wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth);
bool __wrap_RestoreScanModulation(dsd_socket_t sockfd, int kind, int bandwidth, bool session_passband);
void __wrap_RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind);
int __wrap_io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t freq_hz,
                                      uint64_t* out_pending_id);
int __real_dsd_scan_key_change_prepare(const dsd_state* state, const dsd_key_set* row, dsd_scan_key_change* change);
int __wrap_dsd_scan_key_change_prepare(const dsd_state* state, const dsd_key_set* row, dsd_scan_key_change* change);

dsd_socket_t
__wrap_Connect(char* hostname, int portno) {
    (void)hostname;
    (void)portno;
    return g_connect_result;
}

bool
__wrap_SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    g_setfreq_freq = freq;
    return g_setfreq_ok != 0;
}

/* The app-control tunes (`C`, `J`, `=`) reach the peer through io_control's rigctl leg, which answers as SetFreq does:
   0 when the peer took the frequency, -1 when it refused. */
int
__wrap_io_control_set_freq_tagged(dsd_opts* opts, const dsd_state* state, uint32_t freq_hz, uint64_t* out_pending_id) {
    (void)state;
    if (out_pending_id) {
        *out_pending_id = 0U;
    }
    if (!opts || freq_hz == 0U || opts->use_rigctl != 1) {
        return -1;
    }
    g_setfreq_freq = (long)freq_hz;
    if (!g_setfreq_ok) {
        return -1;
    }
    opts->rigctl_sent_freq_hz = freq_hz;
    opts->rtlsdr_center_freq = freq_hz;
    return 0;
}

bool
__wrap_SetModulation(dsd_socket_t sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return true;
}

bool
__wrap_SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    (void)kind;
    (void)bandwidth;
    return true;
}

bool
__wrap_SetScanRowModulation(dsd_socket_t sockfd, int kind, int bandwidth) {
    (void)sockfd;
    (void)kind;
    (void)bandwidth;
    return true;
}

bool
__wrap_RestoreScanModulation(dsd_socket_t sockfd, int kind, int bandwidth, bool session_passband) {
    (void)sockfd;
    (void)kind;
    (void)bandwidth;
    (void)session_passband;
    return true;
}

void
__wrap_RigctlMarkSessionPassband(dsd_socket_t sockfd, int kind) {
    (void)sockfd;
    (void)kind;
}

int
__wrap_dsd_scan_key_change_prepare(const dsd_state* state, const dsd_key_set* row, dsd_scan_key_change* change) {
    if (g_key_prepare_fail_after == 0) {
        g_key_prepare_fail_after = -1;
        return -1;
    }
    if (g_key_prepare_fail_after > 0) {
        g_key_prepare_fail_after--;
    }
    return __real_dsd_scan_key_change_prepare(state, row, change);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

/* ---------------- An analog session on audio input ---------------- */

enum { kBlock = 960 }; /* one 20 ms monitor block at 48 kHz */

enum { kCallBlocks = DSD_ANALOG_CALL_MIN_MS / 20 + 5 }; /* a reception long enough to be a call */

enum { kShortBlocks = (DSD_ANALOG_CALL_MIN_MS / 20) * 3 / 5 }; /* too short alone; two of them are long enough */

static int
session_new(dsd_opts** out_opts, dsd_state** out_state) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (!opts || !state) {
        free(opts);
        free(state);
        return -1;
    }
    initOpts(opts);
    initState(state);
    /* The FM monitor of a rigctl peer's audio (-fA -U), whose frequency the input names nowhere. */
    opts->audio_in_type = AUDIO_IN_WAV;
    opts->analog_only = 1;
    opts->monitor_input_audio = 1;
    opts->use_rigctl = 1;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->setmod_bw = 0;
    opts->trunk_hangtime = 1;
    /* A squelch the scanners hear no noise through, so they warn of none; on audio input it gates no analog call. */
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts->rtl_squelch_level = dsd_squelch_level_from_sql(-60.0);
    g_setfreq_ok = 1;
    g_connect_result = DSD_INVALID_SOCKET;
    g_key_prepare_fail_after = -1;
    /* engine.c keeps the last rigctl frequency it sent across cases. */
    dsd_engine_rigctl_tune_cache_forget();
    *out_opts = opts;
    *out_state = state;
    return 0;
}

static void
session_free(dsd_opts* opts, dsd_state* state) {
    dsd_engine_analog_call_end(opts, state);
    dsd_engine_trunk_scan_shutdown(opts, state);
    dsd_engine_channel_scan_leave(opts, state);
    freeState(state);
    free(state);
    free(opts);
}

/* @p blocks RECEPTION blocks on an open carrier with no tone. */
static void
feed(dsd_opts* opts, dsd_state* state, int blocks) {
    static float block[kBlock];
    for (int i = 0; i < kBlock; i++) {
        block[i] = 2000.0f; /* Monitor blocks carry signed-16 PCM units in floats. */
    }
    state->analog_rx.carrier_open = 1;
    for (int b = 0; b < blocks; b++) {
        dsd_test_analog_call_feed(opts, state, block, kBlock, 48000, DSD_ANALOG_CALL_RECEPTION);
    }
}

/* The analog history rows committed, and the newest one's frequency (-1 with none). */
static int
analog_rows(const dsd_state* state, int64_t* newest_hz) {
    int rows = 0;
    *newest_hz = -1;
    for (int i = DSD_EVENT_HISTORY_LEN - 1; i >= 1; i--) {
        const Event_History* row = &state->event_history_s[0].Event_History_Items[i];
        if (row->systype == DSD_SYNC_ANALOG) {
            rows++;
            *newest_hz = row->freq_hz;
        }
    }
    return rows;
}

/* A call heard now and ended: the frequency its row names. */
static int64_t
call_row_hz(dsd_opts* opts, dsd_state* state) {
    int64_t hz = -1;
    const int before = analog_rows(state, &hz);
    feed(opts, state, kCallBlocks);
    const int active = dsd_engine_analog_call_active(state);
    dsd_engine_analog_call_end(opts, state);
    return active && analog_rows(state, &hz) == before + 1 ? hz : -1;
}

/* One -Y step: -t has run out, so the next pass moves on. */
static void
scan_step(dsd_opts* opts, dsd_state* state) {
    state->last_cc_sync_time = time(NULL) - 11;
    noCarrier(opts, state);
}

static void
scan_list(dsd_opts* opts, dsd_state* state, long row0_hz, long row1_hz, int typed) {
    opts->scanner_mode = 1;
    /* A -Y list is one the user imported (issue #697): learned channels are no list to rotate through. */
    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", "scan.csv");
    state->trunk_lcn_freq[0] = row0_hz;
    state->trunk_lcn_freq[1] = row1_hz;
    state->lcn_freq_count = 2;
    state->lcn_freq_roll = 0;
    if (typed) {
        (void)dsd_channel_mode_set(state, 0, DSD_SCAN_MODE_NFM);
        (void)dsd_channel_mode_set(state, 1, DSD_SCAN_MODE_NFM);
    }
}

/* ---------------- D9: the frequency a -Y landing names ---------------- */

/* One step that lands: the frequency it tuned the peer to, which is the row it committed. */
static long
landing_step(dsd_opts* opts, dsd_state* state) {
    dsd_engine_rigctl_tune_cache_forget();
    g_setfreq_freq = 0;
    scan_step(opts, state);
    const int row = state->lcn_freq_roll - 1;
    return row >= 0 && g_setfreq_freq == state->trunk_lcn_freq[row] ? g_setfreq_freq : -1;
}

static void
test_scan_landing(int typed) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        expect("landing: a session", 0);
        return;
    }
    const long row0 = typed ? 961012500L : 962012500L;
    const long row1 = typed ? 961037500L : 962037500L;
    scan_list(opts, state, row0, row1, typed);
    expect("landing: no frequency before a landing", call_row_hz(opts, state) == 0);

    expect("landing: the first step lands row 0", landing_step(opts, state) == row0);
    expect(typed ? "landing: a typed row's call is heard on its frequency"
                 : "landing: an untyped row's call is heard on its frequency",
           call_row_hz(opts, state) == row0);

    /* A failed tune lands nothing: the receiver may be anywhere. */
    g_setfreq_ok = 0;
    scan_step(opts, state);
    g_setfreq_ok = 1;
    expect("landing: a failed tune names no frequency", call_row_hz(opts, state) == 0);

    /* A map replacement: the rows landed on are not the rows any more. */
    long landed = landing_step(opts, state);
    expect("landing: a step lands again", landed > 0 && call_row_hz(opts, state) == landed);
    state->trunk_chan_map_seq++;
    expect("landing: a map replacement names no frequency", call_row_hz(opts, state) == 0);

    /* A placeholder row (0 Hz) tunes nothing. */
    landed = landing_step(opts, state);
    expect("landing: a step lands after the map change", landed > 0 && call_row_hz(opts, state) == landed);
    const int next = state->lcn_freq_roll % state->lcn_freq_count;
    const long next_hz = state->trunk_lcn_freq[next];
    state->trunk_lcn_freq[next] = 0;
    scan_step(opts, state);
    expect("landing: the placeholder row is the row on air", state->lcn_freq_roll == next + 1);
    expect("landing: a placeholder row names no frequency", call_row_hz(opts, state) == 0);
    state->trunk_lcn_freq[next] = next_hz;

    /* A manual tune of the peer, and a manual takeover (the scan pauses for the operator). */
    landed = landing_step(opts, state);
    expect("landing: a step lands before the manual tune", landed > 0 && call_row_hz(opts, state) == landed);
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    expect("landing: a manual tune names no frequency", call_row_hz(opts, state) == 0);
    landed = landing_step(opts, state);
    expect("landing: a step lands before the takeover", landed > 0 && call_row_hz(opts, state) == landed);
    (void)dsd_engine_channel_scan_pause(opts, state, 1);
    expect("landing: a manual takeover names no frequency", call_row_hz(opts, state) == 0);
    session_free(opts, state);
}

/* A rigctl connection change: the peer the scan tuned is not the one heard now (a new one, or the same one restarted),
   or there is none. */
static void
test_rigctl_connection_change_forgets_landing(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        expect("rigctl change: a session", 0);
        return;
    }
    scan_list(opts, state, 965012500L, 965037500L, 0);
    expect("rigctl change: a step lands", landing_step(opts, state) == 965012500L);
    g_connect_result = (dsd_socket_t)77;
    expect("rigctl change: the reconnect succeeds", svc_rigctl_connect(opts, state, "localhost", 4532) == 0);
    expect("rigctl change: a reconnect names no frequency", call_row_hz(opts, state) == 0);

    const long landed = landing_step(opts, state);
    expect("rigctl change: a step lands on the new connection", landed > 0 && call_row_hz(opts, state) == landed);
    expect("rigctl change: the landing is in force", dsd_engine_scan_landing_hz(opts, state) == (uint32_t)landed);
    /* The connection dropped; a connect that fails leaves rigctl off. */
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    g_connect_result = DSD_INVALID_SOCKET;
    expect("rigctl change: the connect fails",
           svc_rigctl_connect(opts, state, "localhost", 4532) == -1 && opts->use_rigctl == 0);
    expect("rigctl change: a disconnect names no frequency", call_row_hz(opts, state) == 0);
    session_free(opts, state);
}

/* One command through the app-control queue, drained on this (the decoder) thread: 1 when it was applied. */
static int
command(dsd_opts* opts, dsd_state* state, int cmd) {
    dsd_engine_rigctl_tune_cache_forget();
    g_setfreq_freq = 0;
    return dsd_app_command_action(cmd) == DSD_APP_COMMAND_SUBMIT_QUEUED && dsd_app_drain_cmds(opts, state) == 1;
}

/* The untyped -Y list's manual steps: Next (`C`) and Previous (`J`) to a row of the list are the scan's own steps, so
   the row they land names the call's frequency, as the scan's automatic step does; a refused step, and the operator's
   own tune (`=`, which takes the tuner from the scan), name none. */
static void
test_manual_cycle_landing(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        expect("manual cycle: a session", 0);
        return;
    }
    opts->scanner_mode = 1;
    DSD_SNPRINTF(opts->chan_in_file, sizeof opts->chan_in_file, "%s", "scan.csv"); /* an imported list (issue #697) */
    state->trunk_lcn_freq[0] = 966012500L;
    state->trunk_lcn_freq[1] = 966037500L;
    state->trunk_lcn_freq[2] = 966062500L;
    state->lcn_freq_count = 3;
    state->lcn_freq_roll = 0;
    expect("manual cycle: the scan lands row 0", landing_step(opts, state) == 966012500L);
    dsd_app_command_session_set_open(1);

    expect("manual cycle: Next tunes row 1",
           command(opts, state, DSD_APP_CMD_CHANNEL_CYCLE) && g_setfreq_freq == 966037500L);
    expect("manual cycle: the scan still runs", opts->scanner_mode == 1);
    expect("manual cycle: a call after Next names row 1", call_row_hz(opts, state) == 966037500L);

    expect("manual cycle: Previous tunes row 0",
           command(opts, state, DSD_APP_CMD_CHANNEL_CYCLE_PREV) && g_setfreq_freq == 966012500L);
    expect("manual cycle: a call after Previous names row 0", call_row_hz(opts, state) == 966012500L);

    /* The peer refuses Next: the receiver may be anywhere. */
    g_setfreq_ok = 0;
    (void)command(opts, state, DSD_APP_CMD_CHANNEL_CYCLE);
    g_setfreq_ok = 1;
    expect("manual cycle: the refused Next asked the peer for row 1", g_setfreq_freq == 966037500L);
    expect("manual cycle: a call after a refused Next names no frequency", call_row_hz(opts, state) == 0);

    /* The operator's own tune takes the tuner from the scan (issue #671). */
    expect("manual cycle: Next lands again",
           command(opts, state, DSD_APP_CMD_CHANNEL_CYCLE) && call_row_hz(opts, state) > 0);
    dsd_engine_rigctl_tune_cache_forget();
    expect("manual cycle: the operator's tune is applied",
           dsd_app_command_set_u32(DSD_APP_CMD_RTL_SET_FREQ, 967012500U) == DSD_APP_COMMAND_SUBMIT_QUEUED
               && dsd_app_drain_cmds(opts, state) == 1 && g_setfreq_freq == 967012500L);
    expect("manual cycle: the operator's tune pauses the scan", opts->scanner_mode == 0);
    expect("manual cycle: a call after the operator's tune names no frequency", call_row_hz(opts, state) == 0);
    dsd_app_command_session_set_open(0);
    session_free(opts, state);
}

/* A landing is taken at the reception's first block: one that began before it keeps the frequency it began with. */
static void
test_landing_taken_once(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        expect("landing once: a session", 0);
        return;
    }
    feed(opts, state, kShortBlocks);
    dsd_engine_scan_landing_note_row(state, 0, 963012500L);
    opts->scanner_mode = 1;
    feed(opts, state, kShortBlocks);
    expect("landing once: the reception qualified", dsd_engine_analog_call_active(state));
    dsd_engine_analog_call_end(opts, state);
    int64_t hz = -1;
    expect("landing once: a later landing does not rewrite it", analog_rows(state, &hz) == 1 && hz == 0);
    session_free(opts, state);
}

/* ---------------- The -Y leave ---------------- */

static void
test_scan_leave_ends_reception(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        expect("leave: a session", 0);
        return;
    }
    scan_list(opts, state, 964012500L, 964037500L, 1);
    scan_step(opts, state);
    int64_t hz = -1;

    /* A candidate: discarded, so the reception after the leave starts over. */
    feed(opts, state, kShortBlocks);
    (void)dsd_engine_channel_scan_leave(opts, state);
    feed(opts, state, kShortBlocks);
    expect("leave: a candidate is discarded", !dsd_engine_analog_call_active(state) && analog_rows(state, &hz) == 0);
    dsd_engine_analog_call_end(opts, state);

    /* A call: it ends with its row, on the row's frequency. */
    scan_list(opts, state, 964012500L, 964037500L, 1);
    dsd_engine_rigctl_tune_cache_forget();
    scan_step(opts, state);
    feed(opts, state, kCallBlocks);
    expect("leave: the call is on", dsd_engine_analog_call_active(state));
    (void)dsd_engine_channel_scan_leave(opts, state);
    expect("leave: the call ends with its row, on its row's frequency",
           !dsd_engine_analog_call_active(state) && analog_rows(state, &hz) == 1 && hz == 964012500L);
    session_free(opts, state);
}

/* ---------------- The carrier boundary a Signal search pause or failure runs ---------------- */

static void
test_search_boundary_discards_candidate(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (session_new(&opts, &state) != 0) {
        expect("search: a session", 0);
        return;
    }
    feed(opts, state, kShortBlocks);
    /* What search_end_reception() runs at a pause or a failure. */
    dsd_engine_carrier_boundary(opts, state, DSD_CARRIER_BOUNDARY_TUNE, 0);
    feed(opts, state, kShortBlocks);
    int64_t hz = -1;
    expect("search: the candidate is discarded", !dsd_engine_analog_call_active(state));
    dsd_engine_analog_call_end(opts, state);
    expect("search: no row", analog_rows(state, &hz) == 0);
    session_free(opts, state);
}

/* ---------------- The trunk scan ---------------- */

static char g_targets[DSD_TEST_PATH_MAX];

static int
trunk_scan_start(dsd_opts* opts, dsd_state* state) {
    const int fd = dsd_test_mkstemp(g_targets, sizeof g_targets, "analog-scan-calls-");
    if (fd < 0) {
        return -1;
    }
    dsd_close(fd);
    FILE* fp = dsd_fopen_private(g_targets, "w");
    if (!fp) {
        return -1;
    }
    DSD_FPRINTF(fp, "id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n"
                    "alpha,nfm-conventional,154430000,,250,250,,\n"
                    "bravo,nfm-conventional,154455000,,250,250,,\n");
    (void)fclose(fp);
    dsd_trunk_tuning_requests_reset();
    opts->trunk_scan_enabled = 1;
    DSD_SNPRINTF(opts->trunk_scan_lists[0].path, sizeof opts->trunk_scan_lists[0].path, "%s", g_targets);
    opts->trunk_scan_list_count = 1;
    char err[256] = {0};
    if (dsd_engine_trunk_scan_init(opts, state, err, sizeof err) != 0) {
        DSD_FPRINTF(stderr, "trunk scan init: %s\n", err);
        return -1;
    }
    return 0;
}

static void
trunk_scan_stop(dsd_opts* opts, dsd_state* state) {
    dsd_engine_trunk_scan_shutdown(opts, state);
    (void)remove(g_targets);
    dsd_trunk_tuning_requests_reset();
}

static int
advance(dsd_opts* opts, dsd_state* state) {
    return dsd_engine_trunk_scan_control(opts, state, DSD_TRUNK_SCAN_CONTROL_ADVANCE);
}

/* The preparation fails: the receiver never left, and the candidate (@p call 0) or the call (1) goes on. */
static void
trunk_scan_preparation_fails(dsd_opts* opts, dsd_state* state, int call) {
    int64_t hz = -1;
    feed(opts, state, call ? kCallBlocks : kShortBlocks);
    g_key_prepare_fail_after = 1; /* the advance's rollback keys pass, the switch's fail */
    (void)advance(opts, state);
    expect("trunk scan: a failed preparation leaves the receiver and the reception",
           strcmp(state->trunk_scan_active_id, "alpha") == 0 && analog_rows(state, &hz) == 0);
    feed(opts, state, kShortBlocks);
    expect(call ? "trunk scan: the call goes on" : "trunk scan: the candidate goes on and qualifies",
           dsd_engine_analog_call_active(state));
    dsd_engine_analog_call_end(opts, state);
    expect("trunk scan: its row names the target's frequency", analog_rows(state, &hz) == 1 && hz == 154430000L);
}

/* The preparation succeeds: the departure ends the call with its row, or discards the candidate, before the
   snapshots. */
static void
trunk_scan_departs(dsd_opts* opts, dsd_state* state, int call) {
    int64_t hz = -1;
    const int rows = analog_rows(state, &hz);
    feed(opts, state, call ? kCallBlocks : kShortBlocks);
    expect("trunk scan: the advance lands the next target", advance(opts, state) == 0);
    expect("trunk scan: the departure ends the reception",
           !dsd_engine_analog_call_active(state) && analog_rows(state, &hz) == rows + call);
    expect("trunk scan: a call's row names the target left", !call || hz == 154430000L);
    expect("trunk scan: the next target is landed", dsd_engine_scan_landing_hz(opts, state) == 154455000U);
    feed(opts, state, kShortBlocks);
    expect("trunk scan: nothing of the reception carries on", !dsd_engine_analog_call_active(state));
    dsd_engine_analog_call_end(opts, state);
}

/* A revisit, and a rollback after every retune failed, restore neither a call nor a candidate. */
static void
trunk_scan_revisit_and_rollback(dsd_opts* opts, dsd_state* state, int call) {
    int64_t hz = -1;
    int rows = analog_rows(state, &hz);
    expect("trunk scan: the advance revisits the first target", advance(opts, state) == 0);
    feed(opts, state, kShortBlocks);
    expect("trunk scan: a revisit restores no reception",
           !dsd_engine_analog_call_active(state) && analog_rows(state, &hz) == rows);
    dsd_engine_analog_call_end(opts, state);

    /* The rollback parks on the target left, whose frequency it names no longer. */
    feed(opts, state, call ? kCallBlocks : kShortBlocks);
    rows = analog_rows(state, &hz);
    g_setfreq_ok = 0;
    (void)advance(opts, state);
    g_setfreq_ok = 1;
    expect("trunk scan: the rollback parks on the target left", strcmp(state->trunk_scan_active_id, "alpha") == 0);
    expect("trunk scan: the rollback names no frequency", dsd_engine_scan_landing_hz(opts, state) == 0U);
    feed(opts, state, kShortBlocks);
    expect("trunk scan: the rollback restores no reception",
           !dsd_engine_analog_call_active(state) && analog_rows(state, &hz) == rows + call);
}

/* A candidate (@p call 0) or a call (1) on the first target when the operator advances. */
static void
test_trunk_scan_departure(int call) {
    static const uint64_t kT0Ns = 5000ULL * 1000000000ULL;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    dsd_decode_clock_use_test(kT0Ns);
    if (session_new(&opts, &state) != 0 || trunk_scan_start(opts, state) != 0) {
        expect("trunk scan: a session", 0);
        if (opts) {
            session_free(opts, state);
        }
        dsd_decode_clock_use_system();
        return;
    }
    expect("trunk scan: the first target is landed",
           g_setfreq_freq == 154430000L && dsd_engine_scan_landing_hz(opts, state) == 154430000U);
    trunk_scan_preparation_fails(opts, state, call);
    /* A target whose switch could not be prepared cools down as a failed retune does (issue #697): the departure
       below waits it out. */
    dsd_decode_clock_test_set_ns(kT0Ns + 3ULL * 1000000000ULL);
    trunk_scan_departs(opts, state, call);
    trunk_scan_revisit_and_rollback(opts, state, call);
    trunk_scan_stop(opts, state);
    session_free(opts, state);
    dsd_decode_clock_use_system();
}

int
main(void) {
    test_scan_landing(1);
    test_scan_landing(0);
    test_landing_taken_once();
    test_rigctl_connection_change_forgets_landing();
    test_manual_cycle_landing();
    test_scan_leave_ends_reception();
    test_search_boundary_discards_candidate();
    test_trunk_scan_departure(0);
    test_trunk_scan_departure(1);
    if (g_failures == 0) {
        printf("ENGINE_ANALOG_SCAN_CALLS: OK\n");
    }
    return g_failures == 0 ? 0 : 1;
}
