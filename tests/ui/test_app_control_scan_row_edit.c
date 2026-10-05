// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * DSD_APP_CMD_SCAN_ROW_EDIT's handler (issue #518) over stubbed engine and receiver services: what an applied edit
 * leaves to app-control -- the RTL front end's width request, a rigctl peer's passband, the stream restart for a gain --
 * and the rollback of that field alone when it fails, at once or (the width request) where it lands, plus the payload
 * checks.
 */

#include <assert.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <dsd-neo/runtime/squelch.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "command_dispatch.h"
#include "scan_row_edit.h"
#include "services.h"

/* ---- stubs ---- */

static dsd_opts g_opts;
static dsd_state g_state;

static int g_engine_status = DSD_SCAN_ROW_EDIT_APPLIED;
static dsd_scan_row_edit_result g_engine_result;
static int g_engine_width_hz; /* the NFM width the edit leaves in force; 0: unchanged */
static int g_edit_calls;
static int g_restore_calls;
static int g_restore_locked_calls;
static dsd_scan_row_edit g_restored;
static uint32_t g_restored_fields;
static int g_restore_width_hz;  /* the NFM width a restore leaves in force; 0: unchanged */
static int g_restore_publishes; /* what a restore reports for publish_width */
static int g_publish_rc;
static int g_publish_calls;
static int g_not_run;
static int g_rigctl_rc = -1;
static int g_rigctl_calls;
static int g_restart_rc;
static int g_restart_calls;
static int g_recovery_calls;
static int g_recovery_stream_stopped;
static int g_guard_depth;
static int g_guard_held_through; /* every restart, restore and recovery of a gain ran inside one hold */

static int
stub_edit(dsd_scan_row_edit_result* out) {
    g_edit_calls++;
    if (out) {
        *out = g_engine_result;
    }
    if (g_engine_width_hz != 0) {
        g_opts.analog_nfm_bandwidth_hz = g_engine_width_hz;
    }
    return g_engine_status;
}

static int
stub_restore(uint32_t fields, const dsd_scan_row_edit* edit, dsd_scan_row_edit_result* out) {
    g_restore_calls++;
    g_restored = *edit;
    g_restored_fields = fields;
    if (out) {
        DSD_MEMSET(out, 0, sizeof(*out));
        out->publish_width = g_restore_publishes;
    }
    if (g_restore_width_hz != 0) {
        g_opts.analog_nfm_bandwidth_hz = g_restore_width_hz;
    }
    return DSD_SCAN_ROW_EDIT_APPLIED;
}

void
p25_sm_tick_guard_enter(void) {
    g_guard_depth++;
    assert(g_guard_depth == 1);
}

void
p25_sm_tick_guard_leave(void) {
    assert(g_guard_depth == 1);
    g_guard_depth--;
}

int
dsd_engine_trunk_scan_edit_target(dsd_opts* opts, dsd_state* state, uint32_t session, const char* target_id,
                                  uint32_t field, int action, const dsd_scan_row_edit_value* value,
                                  dsd_scan_row_edit_result* out) {
    (void)opts;
    (void)state;
    (void)session;
    (void)target_id;
    (void)field;
    (void)action;
    (void)value;
    return stub_edit(out);
}

int
dsd_engine_trunk_scan_restore_target_edit(dsd_opts* opts, dsd_state* state, uint32_t session, const char* target_id,
                                          uint32_t fields, const dsd_scan_row_edit* edit,
                                          dsd_scan_row_edit_result* out) {
    (void)opts;
    (void)state;
    (void)session;
    (void)target_id;
    assert(g_guard_depth == 0); /* it takes the guard itself */
    return stub_restore(fields, edit, out);
}

int
dsd_engine_trunk_scan_restore_target_edit_locked(dsd_opts* opts, dsd_state* state, uint32_t session,
                                                 const char* target_id, uint32_t fields, const dsd_scan_row_edit* edit,
                                                 dsd_scan_row_edit_result* out) {
    (void)opts;
    (void)state;
    (void)session;
    (void)target_id;
    g_restore_locked_calls++;
    g_guard_held_through &= g_guard_depth == 1;
    return stub_restore(fields, edit, out);
}

int
dsd_engine_channel_scan_edit_row(dsd_opts* opts, dsd_state* state, uint32_t session, int row, uint32_t field,
                                 int action, const dsd_scan_row_edit_value* value, dsd_scan_row_edit_result* out) {
    (void)opts;
    (void)state;
    (void)session;
    (void)row;
    (void)field;
    (void)action;
    (void)value;
    return stub_edit(out);
}

int
dsd_engine_channel_scan_restore_row_edit(dsd_opts* opts, dsd_state* state, uint32_t session, int row, uint32_t fields,
                                         const dsd_scan_row_edit* edit, dsd_scan_row_edit_result* out) {
    (void)opts;
    (void)state;
    (void)session;
    (void)row;
    return stub_restore(fields, edit, out);
}

int
dsd_engine_scan_rigctl_apply_modulation(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_rigctl_calls++;
    return g_rigctl_rc;
}

int
svc_publish_row_analog_width(const dsd_opts* opts, const dsd_state* state, int kind) {
    (void)opts;
    (void)state;
    (void)kind;
    g_publish_calls++;
    return g_publish_rc;
}

int
svc_row_width_request_not_run(void) {
    return g_not_run;
}

void
svc_describe_analog_refusal(const dsd_opts* opts, int kind, int width_hz, char* why, size_t why_size) {
    (void)opts;
    (void)kind;
    DSD_SNPRINTF(why, why_size, "NFM %d Hz does not fit", width_hz);
}

int
svc_rtl_restart_locked(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_restart_calls++;
    g_guard_held_through &= g_guard_depth == 1;
    return g_restart_rc;
}

int
svc_rtl_restart_recovery_locked(dsd_opts* opts, dsd_state* state, int stream_stopped,
                                const dsd_input_failure* failure_before, int* out_capture_stopped) {
    (void)opts;
    (void)state;
    assert(failure_before != NULL);
    g_recovery_calls++;
    g_recovery_stream_stopped = stream_stopped;
    g_guard_held_through &= g_guard_depth == 1;
    if (out_capture_stopped) {
        *out_capture_stopped = 0;
    }
    return 0;
}

/* ---- helpers ---- */

static char g_stream_stand_in;

static void
reset(void) {
    g_engine_status = DSD_SCAN_ROW_EDIT_APPLIED;
    DSD_MEMSET(&g_engine_result, 0, sizeof g_engine_result);
    g_engine_width_hz = 0;
    g_edit_calls = g_restore_calls = g_restore_locked_calls = g_publish_calls = g_rigctl_calls = 0;
    g_restart_calls = g_recovery_calls = g_recovery_stream_stopped = 0;
    DSD_MEMSET(&g_restored, 0, sizeof g_restored);
    g_restored_fields = 0U;
    g_restore_width_hz = 0;
    g_restore_publishes = 0;
    g_publish_rc = 1;
    g_not_run = 0;
    g_rigctl_rc = -1;
    g_restart_rc = 0;
    g_guard_depth = 0;
    g_guard_held_through = 1;
    DSD_MEMSET(&g_opts, 0, sizeof g_opts);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    g_opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    g_opts.analog_nfm_bandwidth_hz = 12500;
    g_state.rtl_ctx = (struct RtlSdrContext*)(void*)&g_stream_stand_in;
}

/* A width edit on air that moves the NFM width in force from 12.5 kHz to @p width_hz, the row having run
   @p previous_hz under its previous edit. */
static void
width_edit(int width_hz, int previous_hz) {
    g_engine_result.publish_width = 1;
    g_engine_result.previous.set = DSD_SCAN_ROW_FIELD_WIDTH;
    g_engine_result.previous.value.width_hz = previous_hz;
    g_engine_width_hz = width_hz;
}

static struct dsd_app_command g_cmd;

static const struct dsd_app_command*
command(int field, int action, const char* tone_list) {
    dsd_app_scan_row_edit_payload p;
    DSD_MEMSET(&p, 0, sizeof p);
    p.session = 3U;
    p.scanner = DSD_SCAN_ROW_SCANNER_TRUNK_SCAN;
    p.row = 1;
    p.mode = DSD_SCAN_MODE_NFM;
    p.field = field;
    p.action = action;
    p.squelch_db = -45;
    p.width_hz = 25000;
    p.gain_db = 20;
    p.tone_mode = 1;
    DSD_SNPRINTF(p.target_id, sizeof p.target_id, "%s", "fire");
    DSD_SNPRINTF(p.tone_list, sizeof p.tone_list, "%s", tone_list ? tone_list : "");
    DSD_MEMSET(&g_cmd, 0, sizeof g_cmd);
    g_cmd.id = DSD_APP_CMD_SCAN_ROW_EDIT;
    g_cmd.n = sizeof p;
    DSD_MEMCPY(g_cmd.data, &p, sizeof p);
    return &g_cmd;
}

static int
apply(const struct dsd_app_command* c, char* notice, size_t size, int* ttl) {
    return dsd_app_apply_scan_row_edit(&g_opts, &g_state, c, notice, size, ttl);
}

static void
expect_notice(const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "notice '%s', want '%s'\n", got, want);
        assert(0);
    }
}

/* ---- tests ---- */

static void
test_applied_and_stored(void) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    reset();
    assert(apply(command(DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    expect_notice(notice, "This channel (fire): squelch -45 dB for this session");
    assert(ttl == 3 && g_publish_calls == 0 && g_restart_calls == 0 && g_restore_calls == 0);
    g_engine_status = DSD_SCAN_ROW_EDIT_STORED;
    assert(apply(command(DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_INHERIT, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    expect_notice(notice, "This channel (fire): squelch follows the default from its next visit");
    g_engine_status = DSD_SCAN_ROW_EDIT_STALE;
    assert(apply(command(DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_REFUSED);
    expect_notice(notice, "Refused: the scan changed; nothing applied");
    assert(ttl == 5);
}

/* The RTL front end refuses the width request at once: the row's previous width goes back, and only the width. Accepted,
   it may still be refused where it lands: the settle then puts the width back too, the configured width untouched (the
   stubs have none to touch), and a refusal of anything but a row edit's request is not the settle's. An edit that
   leaves the width in force where it was (the row now sets the width the default ran) asks the RTL front end for
   nothing. */
static void
test_width_refusals(void) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    reset();
    width_edit(25000, 12500);
    g_publish_rc = -1;
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_REFUSED);
    assert(g_publish_calls == 1 && g_restore_calls == 1 && g_restored.value.width_hz == 12500);
    assert(g_restored_fields == DSD_SCAN_ROW_FIELD_WIDTH);
    expect_notice(notice, "Refused: this channel's NFM bandwidth: NFM 25000 Hz does not fit");

    reset();
    width_edit(25000, 12500);
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(g_publish_calls == 1 && g_restore_calls == 0);
    svc_monitor_refusal refusal;
    DSD_MEMSET(&refusal, 0, sizeof refusal);
    refusal.kind = DSD_ANALOG_DEMOD_FM;
    refusal.width_hz = 25000;
    refusal.kept_width_hz = 12500;
    assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 0);
    assert(g_restore_calls == 0);
    refusal.row_edit = 1;
    assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 1);
    assert(g_restore_calls == 1 && g_restored.value.width_hz == 12500 && (g_restored.set & DSD_SCAN_ROW_FIELD_WIDTH));
    assert(g_restored_fields == DSD_SCAN_ROW_FIELD_WIDTH);
    expect_notice(notice, "Refused: this channel's NFM bandwidth: NFM 25000 Hz does not fit");
    /* Settled once: a second refusal finds nothing to put back and is the ordinary settle's. */
    assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 0);
    assert(g_restore_calls == 1);

    reset();
    g_engine_result.publish_width = 1;
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(g_publish_calls == 0 && g_restore_calls == 0);
}

/* Width edits to @p widths in turn from 12.5 kHz, each request after the first made while @p not_run says the one
   before had not reached the front end, then a refusal of the last that kept @p kept_width_hz. Returns the width the
   row's width edit went back to. */
static int
refused_last_request(const int* widths, int n, int not_run, int kept_width_hz) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    reset();
    for (int i = 0; i < n; i++) {
        width_edit(widths[i], i == 0 ? 12500 : widths[i - 1]);
        g_not_run = i == 0 ? 0 : not_run;
        assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
               == DSD_APP_SCAN_ROW_EDIT_DONE);
    }
    assert(g_publish_calls == n && g_restore_calls == 0);
    svc_monitor_refusal refusal;
    DSD_MEMSET(&refusal, 0, sizeof refusal);
    refusal.kind = DSD_ANALOG_DEMOD_FM;
    refusal.width_hz = widths[n - 1];
    refusal.kept_width_hz = kept_width_hz;
    refusal.row_edit = 1;
    assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 1);
    assert(g_restore_calls == 1 && g_restored_fields == DSD_SCAN_ROW_FIELD_WIDTH);
    return g_restored.value.width_hz;
}

/* Width edits whose requests the front end has not all run: a refusal puts the row's width back to what the front end
   kept -- the edit from before the newest request of the run that ran that width, which a request the front end ran
   before a later one replaced the next is among, else the edit from before the first, since a request replaced while
   still queued ran neither. A first request the front end ran starts the run again. */
static void
test_consecutive_width_requests(void) {
    static const int two[] = {20000, 25000};
    assert(refused_last_request(two, 2, 1, 12500) == 12500);
    assert(refused_last_request(two, 2, 1, 20000) == 20000);
    assert(refused_last_request(two, 2, 0, 12500) == 20000);
    /* 16 kHz ran, 20 kHz was replaced by 25 kHz, which was refused: the front end runs 16 kHz. */
    static const int three[] = {16000, 20000, 25000};
    assert(refused_last_request(three, 3, 1, 16000) == 16000);
    assert(refused_last_request(three, 3, 1, 12500) == 12500);
    assert(refused_last_request(three, 3, 1, 11000) == 12500);
    /* A run longer than the record keeps its oldest and its newest. */
    static const int long_run[] = {13000, 14000, 15000, 16000, 17000, 18000, 19000, 20000, 21000, 22000};
    assert(refused_last_request(long_run, 10, 1, 12500) == 12500);
    assert(refused_last_request(long_run, 10, 1, 21000) == 21000);
}

/* The width put back follows the default, which changed while the request was out: the restore leaves 12.5 kHz in
   force while the front end kept 16 kHz, so the settle asks for 12.5 kHz once; a refusal of that request is left as it
   stands, never chased again. */
static void
test_settle_asks_for_the_width_put_back(void) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    reset();
    g_opts.analog_nfm_bandwidth_hz = 16000;
    g_engine_result.publish_width = 1;
    g_engine_result.previous.inherit = DSD_SCAN_ROW_FIELD_WIDTH;
    g_engine_width_hz = 25000;
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(g_publish_calls == 1);
    g_restore_width_hz = 12500;
    g_restore_publishes = 1;
    svc_monitor_refusal refusal;
    DSD_MEMSET(&refusal, 0, sizeof refusal);
    refusal.kind = DSD_ANALOG_DEMOD_FM;
    refusal.width_hz = 25000;
    refusal.kept_width_hz = 16000;
    refusal.row_edit = 1;
    assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 1);
    assert(g_restore_calls == 1 && (g_restored.inherit & DSD_SCAN_ROW_FIELD_WIDTH));
    assert(g_publish_calls == 2 && g_opts.analog_nfm_bandwidth_hz == 12500);
    refusal.width_hz = 12500;
    assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 1);
    assert(g_restore_calls == 2 && (g_restored.inherit & DSD_SCAN_ROW_FIELD_WIDTH) && g_publish_calls == 2);
    /* Settled: nothing left to chase. */
    assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 0);
}

/* A width edit of the row stands without a request of its own -- stored for its next visit after the scan moved on,
   applied without moving the width in force, or applied with the front end queuing nothing for it -- while an earlier
   request for that row is still out: a late refusal
   of that request reports, but puts nothing back over the newer edit. An edit of another field in between leaves the
   rollback to the width. */
static void
test_newer_width_edit_survives_a_late_refusal(void) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    svc_monitor_refusal refusal;
    DSD_MEMSET(&refusal, 0, sizeof refusal);
    refusal.kind = DSD_ANALOG_DEMOD_FM;
    refusal.width_hz = 25000;
    refusal.kept_width_hz = 12500;
    refusal.row_edit = 1;
    /* 0: stored for the next visit; 1: applied without moving the width; 2: applied, moving it, with the front end
       queuing nothing for it (CQPSK asked for). */
    for (int how = 0; how < 3; how++) {
        reset();
        width_edit(25000, 12500);
        assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
               == DSD_APP_SCAN_ROW_EDIT_DONE);
        assert(g_publish_calls == 1);
        DSD_MEMSET(&g_engine_result, 0, sizeof g_engine_result);
        g_engine_width_hz = 0;
        g_engine_status = how == 0 ? DSD_SCAN_ROW_EDIT_STORED : DSD_SCAN_ROW_EDIT_APPLIED;
        if (how == 2) {
            width_edit(20000, 25000);
            g_publish_rc = 0;
        }
        assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
               == DSD_APP_SCAN_ROW_EDIT_DONE);
        assert(g_publish_calls == (how == 2 ? 2 : 1));
        assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 1);
        assert(g_restore_calls == 0 && g_publish_calls == (how == 2 ? 2 : 1));
        expect_notice(notice, "Refused: this channel's NFM bandwidth: NFM 25000 Hz does not fit");
    }
    reset();
    width_edit(25000, 12500);
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    DSD_MEMSET(&g_engine_result, 0, sizeof g_engine_result);
    g_engine_width_hz = 0;
    assert(apply(command(DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(dsd_app_scan_row_edit_settle_refusal(&g_opts, &g_state, &refusal, notice, sizeof notice) == 1);
    assert(g_restore_calls == 1 && g_restored_fields == DSD_SCAN_ROW_FIELD_WIDTH && g_restored.value.width_hz == 12500);
}

/* On audio input the width goes to a rigctl peer that demodulates it, and a peer that refuses it gets the previous
   width's passband back. The peer is asked even when the width the decoder runs stays where it was (a row that now
   sets the width the default ran asks for its own passband rather than -B); with no peer there is nothing to ask. */
static void
test_rigctl_width(void) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    reset();
    g_opts.audio_in_type = AUDIO_IN_WAV;
    width_edit(25000, 12500);
    g_rigctl_rc = 0;
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_REFUSED);
    assert(g_rigctl_calls == 2 && g_restore_calls == 1 && g_publish_calls == 0);
    assert(g_restored_fields == DSD_SCAN_ROW_FIELD_WIDTH && g_restored.value.width_hz == 12500);
    expect_notice(notice, "Refused: this channel's NFM bandwidth: the rigctl peer refused the passband");
    g_restore_calls = g_rigctl_calls = 0;
    g_rigctl_rc = 1;
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(g_rigctl_calls == 1 && g_restore_calls == 0);
    g_rigctl_calls = 0;
    g_engine_width_hz = 0;
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(g_rigctl_calls == 1);
    g_rigctl_rc = -1;
    assert(apply(command(DSD_SCAN_ROW_FIELD_WIDTH, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
}

/* A gain edit on air reopens the stream. A reopen that fails puts the previous gain back, and only the gain, and starts
   the input that ran again through the recovery path, which keeps an I/Q capture from being reopened over its
   recording; the restart, the rollback and the recovery run inside one hold of the P25 SM tick guard. */
static void
test_gain_restart(void) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    reset();
    g_engine_result.restart_gain = 1;
    assert(apply(command(DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(g_restart_calls == 1 && g_restore_calls == 0 && g_recovery_calls == 0 && g_opts.rtl_needs_restart == 1);
    assert(g_guard_depth == 0 && g_guard_held_through);
    expect_notice(notice, "This channel (fire): RTL gain 20 dB for this session");
    reset();
    g_engine_result.restart_gain = 1;
    g_engine_result.previous.set = DSD_SCAN_ROW_FIELD_GAIN;
    g_engine_result.previous.value.gain_db = 10;
    g_restart_rc = -1;
    assert(apply(command(DSD_SCAN_ROW_FIELD_GAIN, DSD_SCAN_ROW_EDIT_SET, NULL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_REFUSED);
    assert(g_restart_calls == 1 && g_restore_locked_calls == 1 && g_restore_calls == 1);
    assert(g_restored.value.gain_db == 10 && g_restored_fields == DSD_SCAN_ROW_FIELD_GAIN);
    assert(g_recovery_calls == 1 && g_recovery_stream_stopped == 1);
    assert(g_guard_depth == 0 && g_guard_held_through);
    expect_notice(notice, "Failed: this channel's RTL gain did not apply; kept the previous gain");
}

static void
test_payload_checks(void) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    reset();
    assert(apply(command(DSD_SCAN_ROW_FIELD_TONE, DSD_SCAN_ROW_EDIT_SET, "1234.5"), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_REFUSED);
    assert(g_edit_calls == 0 && strncmp(notice, "Refused: this channel's tone filter: ", 37) == 0);
    struct dsd_app_command* c = (struct dsd_app_command*)command(DSD_SCAN_ROW_FIELD_SQUELCH, 9, NULL);
    assert(apply(c, notice, sizeof notice, &ttl) == DSD_APP_SCAN_ROW_EDIT_BAD_PAYLOAD);
    c = (struct dsd_app_command*)command(DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, NULL);
    c->n--;
    assert(apply(c, notice, sizeof notice, &ttl) == DSD_APP_SCAN_ROW_EDIT_BAD_PAYLOAD);
    c = (struct dsd_app_command*)command(DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, NULL);
    dsd_app_scan_row_edit_payload p;
    DSD_MEMCPY(&p, c->data, sizeof p);
    DSD_MEMSET(p.target_id, 'x', sizeof p.target_id);
    DSD_MEMCPY(c->data, &p, sizeof p);
    assert(apply(c, notice, sizeof notice, &ttl) == DSD_APP_SCAN_ROW_EDIT_BAD_PAYLOAD);
    assert(dsd_app_apply_scan_row_edit(&g_opts, &g_state, command(DSD_SCAN_ROW_FIELD_SQUELCH, 1, NULL), notice,
                                       sizeof notice, NULL)
           == DSD_APP_SCAN_ROW_EDIT_BAD_PAYLOAD);
    assert(g_edit_calls == 0);
}

/* A row edit of @p mode (dsd_squelch_mode) with a 10 dB margin. */
static struct dsd_app_command*
squelch_mode_command(int mode) {
    struct dsd_app_command* c =
        (struct dsd_app_command*)command(DSD_SCAN_ROW_FIELD_SQUELCH, DSD_SCAN_ROW_EDIT_SET, NULL);
    dsd_app_scan_row_edit_payload p;
    DSD_MEMCPY(&p, c->data, sizeof p);
    p.squelch_mode = mode;
    p.squelch_margin_db = 10;
    DSD_MEMCPY(c->data, &p, sizeof p);
    return c;
}

/* "This channel" on audio input (issue #628): the auto squelch has no channel power to learn a floor from there, so a
   row edit to it is refused and says why, as the default's editor refuses it; noise and a level go through, and a radio
   input takes auto. */
static void
test_auto_refused_on_audio_input(void) {
    char notice[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    int ttl = 0;
    reset();
    g_opts.audio_in_type = AUDIO_IN_UDP;
    assert(apply(squelch_mode_command(DSD_SQUELCH_MODE_AUTO), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_REFUSED);
    assert(g_edit_calls == 0 && ttl == 5);
    assert(strstr(notice, "the auto squelch needs a radio input; audio input takes a level or noise") != NULL);
    assert(apply(squelch_mode_command(DSD_SQUELCH_MODE_NOISE), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(apply(squelch_mode_command(DSD_SQUELCH_MODE_LEVEL), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(g_edit_calls == 2);
    g_opts.audio_in_type = AUDIO_IN_RTL;
    assert(apply(squelch_mode_command(DSD_SQUELCH_MODE_AUTO), notice, sizeof notice, &ttl)
           == DSD_APP_SCAN_ROW_EDIT_DONE);
    assert(g_edit_calls == 3);
}

int
main(void) {
    test_applied_and_stored();
    test_width_refusals();
    test_consecutive_width_requests();
    test_settle_asks_for_the_width_put_back();
    test_newer_width_edit_survives_a_late_refusal();
    test_rigctl_width();
    test_gain_restart();
    test_payload_checks();
    test_auto_refused_on_audio_input();
    printf("APP_CONTROL_SCAN_ROW_EDIT: OK\n");
    return 0;
}
