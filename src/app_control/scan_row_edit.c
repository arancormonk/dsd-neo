// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

/*
 * DSD_APP_CMD_SCAN_ROW_EDIT: a session edit of the scan row on air ("this channel", issue #518). The engine owns the
 * edit and its scope; app-control does what only it can after an edit lands on air -- the front end's width request,
 * the stream restart a tuner gain needs -- and undoes the edit when that fails, so the row never runs settings the
 * receiver does not.
 */

#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/engine/channel_scan.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#ifdef USE_RADIO
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/input_failure.h>
#endif
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "command_dispatch.h"
#include "scan_row_edit.h"
#include "services.h"

static int
scan_row_edit_terminated(const char* text, size_t size) {
    return memchr(text, '\0', size) != NULL;
}

/* Apply @p p to the engine: the edit (@p restore NULL) or, after a failed follow-up, @p p's field of the previous
   edit, which keeps any edit of another field made since. */
static int
scan_row_edit_engine(dsd_opts* opts, dsd_state* state, const dsd_app_scan_row_edit_payload* p,
                     const dsd_scan_row_edit_value* value, const dsd_scan_row_edit* restore,
                     dsd_scan_row_edit_result* result) {
    /* A scanner neither is answers nothing: the result reads as no change. */
    DSD_MEMSET(result, 0, sizeof(*result));
    const uint32_t field = (uint32_t)p->field;
    if (p->scanner == DSD_SCAN_ROW_SCANNER_TRUNK_SCAN) {
        return restore ? dsd_engine_trunk_scan_restore_target_edit(opts, state, p->session, p->target_id, field,
                                                                   restore, result)
                       : dsd_engine_trunk_scan_edit_target(opts, state, p->session, p->target_id, field, p->action,
                                                           value, result);
    }
    if (p->scanner == DSD_SCAN_ROW_SCANNER_CHANNEL_SCAN) {
        return restore
                   ? dsd_engine_channel_scan_restore_row_edit(opts, state, p->session, p->row, field, restore, result)
                   : dsd_engine_channel_scan_edit_row(opts, state, p->session, p->row, field, p->action, value, result);
    }
    return DSD_SCAN_ROW_EDIT_UNAVAILABLE;
}

/* The value a SET carries, the tone list parsed. Returns 0, or -1 with why in @p why. */
static int
scan_row_edit_value(const dsd_app_scan_row_edit_payload* p, dsd_scan_row_edit_value* value, char* why,
                    size_t why_size) {
    DSD_MEMSET(value, 0, sizeof(*value));
    value->squelch_db = p->squelch_db;
    value->squelch_mode = p->squelch_mode == DSD_SQUELCH_MODE_AUTO ? DSD_SQUELCH_MODE_AUTO : DSD_SQUELCH_MODE_LEVEL;
    value->squelch_margin_db = p->squelch_margin_db;
    value->width_hz = p->width_hz;
    value->gain_db = p->gain_db;
    value->tone_filter = p->tone_mode;
    if (p->action == DSD_SCAN_ROW_EDIT_SET && p->field == (int32_t)DSD_SCAN_ROW_FIELD_TONE
        && dsd_tone_filter_check(p->tone_mode, p->tone_list, &value->tone_set, why, why_size) != 0) {
        return -1;
    }
    return 0;
}

/* A row's width edit and the width the decoder ran under it (dsd_opts_analog_width_hz()). */
typedef struct {
    dsd_scan_row_edit edit;
    int width_hz;
} scan_row_width_baseline;

/* Width requests a run can hold baselines for; a longer run keeps its oldest. */
enum { SCAN_ROW_WIDTH_BASELINES = 8 };

/* The width edits applied on air whose requests the RTL front end has not all run, and the row they went to: the edit
   and width from before each request of the run, oldest first. A refusal where the last request lands puts the row's
   width back (dsd_app_scan_row_edit_settle_refusal()) to the newest of them that ran the width the front end kept --
   a request it ran before a later one replaced the next is among them -- else to the oldest, as the configured width's
   first baseline does (symbol_profile_configured_width_run()). reconcile marks a request the settle made itself, to
   bring the front end to the width it restored; that request's own refusal is not chased again. Decoder thread only,
   as the monitor request record is. */
static struct {
    int pending;
    int reconcile;
    dsd_app_scan_row_edit_payload row;
    scan_row_width_baseline baselines[SCAN_ROW_WIDTH_BASELINES];
    int count;
    unsigned int records; /* requests recorded, ever: tells whether a command made one */
} g_width_request;

/* Whether @p a and @p b name the same row of the same scan. */
static int
scan_row_edit_same_row(const dsd_app_scan_row_edit_payload* a, const dsd_app_scan_row_edit_payload* b) {
    return a->scanner == b->scanner && a->session == b->session && a->row == b->row
           && strcmp(a->target_id, b->target_id) == 0;
}

/* Whether a front end that kept @p kept_hz runs @p width_hz of analog @p kind: the monitor publishes an unset width the
   DSP rate limits as the kind's default. */
static int
scan_row_edit_runs_width(int kind, int width_hz, int kept_hz) {
    return kept_hz == width_hz || (width_hz == 0 && kept_hz == dsd_analog_width_default_hz(kind));
}

/* Record the request just made for @p p's row, which ran @p width_before under the edit @p previous.
   @p earlier_not_run (svc_row_width_request_not_run(), read before the request) says the last one has not reached the
   front end either: for the same row the run goes on. @p reconcile: the settle's own request. */
static void
scan_row_edit_record_width_request(const dsd_app_scan_row_edit_payload* p, const dsd_scan_row_edit* previous,
                                   int width_before, int earlier_not_run, int reconcile) {
    if (!(g_width_request.pending && earlier_not_run && scan_row_edit_same_row(&g_width_request.row, p))) {
        g_width_request.count = 0;
    }
    if (g_width_request.count == SCAN_ROW_WIDTH_BASELINES) {
        DSD_MEMMOVE(&g_width_request.baselines[1], &g_width_request.baselines[2],
                    sizeof(g_width_request.baselines[0]) * (SCAN_ROW_WIDTH_BASELINES - 2));
        g_width_request.count--;
    }
    g_width_request.baselines[g_width_request.count].edit = *previous;
    g_width_request.baselines[g_width_request.count].width_hz = width_before;
    g_width_request.count++;
    g_width_request.row = *p;
    g_width_request.pending = 1;
    g_width_request.reconcile = reconcile;
    g_width_request.records++;
}

/* A width edit of the row a request is still out for, which stands (applied or stored for the row's next visit)
   without having made a request of its own: the row's width is no longer the one that request carried, so a refusal
   of it has nothing left to put back. */
static void
scan_row_edit_note_width_superseded(const dsd_app_scan_row_edit_payload* p, unsigned int records_before) {
    if (p->field == (int32_t)DSD_SCAN_ROW_FIELD_WIDTH && g_width_request.pending
        && g_width_request.records == records_before && scan_row_edit_same_row(&g_width_request.row, p)) {
        g_width_request.count = 0;
    }
}

/* The baseline of the run a front end that kept @p kept_hz of analog @p kind ran: the newest that ran that width, else
   the oldest. */
static const scan_row_width_baseline*
scan_row_edit_kept_baseline(int kind, int kept_hz) {
    for (int i = g_width_request.count - 1; i > 0; i--) {
        if (scan_row_edit_runs_width(kind, g_width_request.baselines[i].width_hz, kept_hz)) {
            return &g_width_request.baselines[i];
        }
    }
    return &g_width_request.baselines[0];
}

/* After an edit on air: hand the receiver the new width -- a running RTL front end, when the width the decoder runs
   changed from @p width_before, or a rigctl peer that demodulates audio input, whose passband follows whether the row
   sets one of its own too. A refusal (the rate moved since the engine's check, a peer that will not take the passband)
   puts the row's previous width back. Returns 0, or -1 with why in @p why. */
static int
scan_row_edit_publish_width(dsd_opts* opts, dsd_state* state, const dsd_app_scan_row_edit_payload* p,
                            const dsd_scan_row_edit_result* result, int width_before, char* why, size_t why_size) {
    if (!result->publish_width) {
        return 0;
    }
    const int kind = opts->analog_demod;
    const int width_hz = dsd_opts_analog_width_hz(opts);
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        if (dsd_engine_scan_rigctl_apply_modulation(opts, state) != 0) {
            return 0;
        }
        dsd_scan_row_edit_result undo;
        (void)scan_row_edit_engine(opts, state, p, NULL, &result->previous, &undo);
        (void)dsd_engine_scan_rigctl_apply_modulation(opts, state);
        DSD_SNPRINTF(why, why_size, "%s", "the rigctl peer refused the passband");
        return -1;
    }
    if (width_hz == width_before) {
        return 0;
    }
    const int earlier_not_run = svc_row_width_request_not_run();
    const int published = svc_publish_row_analog_width(opts, state, kind);
    /* Only a request queued can be refused where it lands; one the front end has no use for (CQPSK asked for, say)
       records nothing, and the edit stands as any other that made no request. */
    if (published > 0) {
        scan_row_edit_record_width_request(p, &result->previous, width_before, earlier_not_run, 0);
    }
    if (published >= 0) {
        return 0;
    }
    dsd_scan_row_edit_result undo;
    (void)scan_row_edit_engine(opts, state, p, NULL, &result->previous, &undo);
    if (undo.publish_width) {
        (void)svc_publish_row_analog_width(opts, state, kind);
    }
    svc_describe_analog_refusal(opts, kind, width_hz, why, why_size);
    return -1;
}

int
dsd_app_scan_row_edit_settle_refusal(dsd_opts* opts, dsd_state* state, const svc_monitor_refusal* refusal, char* notice,
                                     size_t notice_size) {
    if (notice && notice_size > 0U) {
        notice[0] = '\0';
    }
    if (!opts || !state || !refusal || !refusal->row_edit || !notice || notice_size == 0U) {
        return 0;
    }
    /* With no edit recorded (a stream restarted since, say), the refusal is the ordinary one's to settle. */
    if (!g_width_request.pending) {
        return 0;
    }
    char why[DSD_SCAN_ROW_EDIT_ERROR_SIZE] = {0};
    svc_describe_analog_refusal(opts, refusal->kind, refusal->width_hz, why, sizeof why);
    g_width_request.pending = 0;
    const int kept_hz = refusal->kept_width_hz;
    dsd_app_scan_row_edit_payload width_row = g_width_request.row;
    width_row.field = (int32_t)DSD_SCAN_ROW_FIELD_WIDTH;
    /* The row's width goes back to the edit the front end ran the width it kept under, and only the width: an edit of
       another field made since stays. A width edit made since that stands without a request of its own
       (scan_row_edit_note_width_superseded()) leaves nothing to put back. */
    dsd_scan_row_edit_result undo;
    DSD_MEMSET(&undo, 0, sizeof undo);
    scan_row_width_baseline back;
    DSD_MEMSET(&back, 0, sizeof back);
    if (g_width_request.count > 0) {
        back = *scan_row_edit_kept_baseline(refusal->kind, kept_hz);
        (void)scan_row_edit_engine(opts, state, &width_row, NULL, &back.edit, &undo);
    }
    /* The edit put back can run a width the front end does not: one that follows a default changed while the request
       was out, which the row's own width kept from the front end. The front end is asked for it once; a refusal of
       that request is left as it stands. */
    if (!g_width_request.reconcile && undo.publish_width
        && !scan_row_edit_runs_width(refusal->kind, dsd_opts_analog_width_hz(opts), kept_hz)
        && svc_publish_row_analog_width(opts, state, refusal->kind) > 0) {
        scan_row_edit_record_width_request(&width_row, &back.edit, kept_hz, 0, 1);
    }
    char label[DSD_APP_SCAN_ROW_LABEL_SIZE];
    (void)dsd_app_scan_row_label(state, g_width_request.row.scanner, g_width_request.row.row,
                                 g_width_request.row.target_id, label, sizeof label);
    (void)dsd_app_scan_row_notice(label, DSD_SCAN_ROW_FIELD_WIDTH, (unsigned int)g_width_request.row.mode,
                                  DSD_SCAN_ROW_EDIT_SET, NULL, DSD_SCAN_ROW_EDIT_REFUSED, why, notice, notice_size);
    return 1;
}

#ifdef USE_RADIO
/* After an edit on air: reopen the stream, which is how a tuner takes a new gain. A reopen that fails puts the
   target's previous gain back and starts the input that ran again through the recovery path
   (svc_rtl_restart_recovery_locked()): without the I/Q capture, whose reopen would write over what it recorded, and
   with the input failure latched before. One hold of the P25 SM tick guard covers all of it, as for an Airspy reopen,
   since the watchdog reads the input under it. Returns 0, or -1. Only a trunk-scan target takes a gain, and without
   radio support none is editable (the engine takes it only on an RTL-family input), so nothing asks for a restart. */
static int
scan_row_edit_restart_gain(dsd_opts* opts, dsd_state* state, const dsd_app_scan_row_edit_payload* p,
                           const dsd_scan_row_edit_result* result) {
    if (!result->restart_gain) {
        return 0;
    }
    opts->rtl_needs_restart = 1;
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return 0;
    }
    p25_sm_tick_guard_enter();
    dsd_input_failure failure_before;
    dsd_input_failure_get(&failure_before);
    const int stream_stopped = state->rtl_ctx != NULL;
    if (svc_rtl_restart_locked(opts, state) == 0) {
        p25_sm_tick_guard_leave();
        return 0;
    }
    dsd_scan_row_edit_result undo;
    (void)dsd_engine_trunk_scan_restore_target_edit_locked(opts, state, p->session, p->target_id,
                                                           DSD_SCAN_ROW_FIELD_GAIN, &result->previous, &undo);
    (void)svc_rtl_restart_recovery_locked(opts, state, stream_stopped, &failure_before, NULL);
    p25_sm_tick_guard_leave();
    return -1;
}
#endif

/* Copy and check the payload. Returns 0, or -1 when it is malformed. */
static int
scan_row_edit_read_payload(const struct dsd_app_command* command, dsd_app_scan_row_edit_payload* p) {
    if (command->n != sizeof(*p)) {
        return -1;
    }
    DSD_MEMCPY(p, command->data, sizeof(*p));
    if (!scan_row_edit_terminated(p->target_id, sizeof p->target_id)
        || !scan_row_edit_terminated(p->tone_list, sizeof p->tone_list) || p->action < DSD_SCAN_ROW_EDIT_SET
        || p->action > DSD_SCAN_ROW_EDIT_RESET) {
        return -1;
    }
    return 0;
}

/* What an applied edit leaves to app-control. Returns 0 when done, else the notice to give (in @p notice). */
static int
scan_row_edit_follow_up(dsd_opts* opts, dsd_state* state, const dsd_app_scan_row_edit_payload* p,
                        const dsd_scan_row_edit_result* result, int width_before, const char* label, char* notice,
                        size_t notice_size) {
    char why[DSD_SCAN_ROW_EDIT_ERROR_SIZE] = {0};
    if (scan_row_edit_publish_width(opts, state, p, result, width_before, why, sizeof why) != 0) {
        (void)dsd_app_scan_row_notice(label, (uint32_t)p->field, (unsigned int)p->mode, p->action, NULL,
                                      DSD_SCAN_ROW_EDIT_REFUSED, why, notice, notice_size);
        return -1;
    }
#ifdef USE_RADIO
    if (scan_row_edit_restart_gain(opts, state, p, result) != 0) {
        DSD_SNPRINTF(notice, notice_size, "Failed: this channel's RTL gain did not apply; kept the previous gain");
        return -1;
    }
#endif
    return 0;
}

/* The notice for the engine's @p status, and what the command reports. */
static int
scan_row_edit_outcome(const char* label, const dsd_app_scan_row_edit_payload* p, const dsd_scan_row_edit_value* value,
                      int status, const dsd_scan_row_edit_result* result, char* notice, size_t notice_size,
                      int* ttl_s) {
    char value_text[64] = {0};
    if (p->action == DSD_SCAN_ROW_EDIT_SET) {
        (void)dsd_app_scan_row_value_text((uint32_t)p->field, value, value_text, sizeof value_text);
    }
    (void)dsd_app_scan_row_notice(label, (uint32_t)p->field, (unsigned int)p->mode, p->action, value_text, status,
                                  status == DSD_SCAN_ROW_EDIT_REFUSED ? result->err : NULL, notice, notice_size);
    if (status == DSD_SCAN_ROW_EDIT_APPLIED || status == DSD_SCAN_ROW_EDIT_STORED) {
        return DSD_APP_SCAN_ROW_EDIT_DONE;
    }
    *ttl_s = 5;
    return DSD_APP_SCAN_ROW_EDIT_REFUSED;
}

int
dsd_app_apply_scan_row_edit(dsd_opts* opts, dsd_state* state, const struct dsd_app_command* command, char* notice,
                            size_t notice_size, int* ttl_s) {
    if (!opts || !state || !command || !notice || notice_size == 0U || !ttl_s) {
        return DSD_APP_SCAN_ROW_EDIT_BAD_PAYLOAD;
    }
    notice[0] = '\0';
    *ttl_s = 3;
    dsd_app_scan_row_edit_payload p;
    if (scan_row_edit_read_payload(command, &p) != 0) {
        return DSD_APP_SCAN_ROW_EDIT_BAD_PAYLOAD;
    }
    char label[DSD_APP_SCAN_ROW_LABEL_SIZE];
    (void)dsd_app_scan_row_label(state, p.scanner, p.row, p.target_id, label, sizeof label);
    /* The class the editor saw names the width field in the notice. */
    if (p.mode < 0) {
        p.mode = 0;
    }
    char why[DSD_SCAN_ROW_EDIT_ERROR_SIZE] = {0};
    dsd_scan_row_edit_value value;
    if (scan_row_edit_value(&p, &value, why, sizeof why) != 0) {
        (void)dsd_app_scan_row_notice(label, (uint32_t)p.field, (unsigned int)p.mode, p.action, NULL,
                                      DSD_SCAN_ROW_EDIT_REFUSED, why, notice, notice_size);
        *ttl_s = 5;
        return DSD_APP_SCAN_ROW_EDIT_REFUSED;
    }
    /* The width the decoder runs before the edit: the RTL front end is asked for the new one only if it moved. */
    const int width_before = dsd_opts_analog_width_hz(opts);
    const unsigned int records_before = g_width_request.records;
    dsd_scan_row_edit_result result;
    const int status = scan_row_edit_engine(opts, state, &p, &value, NULL, &result);
    if (status == DSD_SCAN_ROW_EDIT_APPLIED
        && scan_row_edit_follow_up(opts, state, &p, &result, width_before, label, notice, notice_size) != 0) {
        *ttl_s = 5;
        return DSD_APP_SCAN_ROW_EDIT_REFUSED;
    }
    if (status == DSD_SCAN_ROW_EDIT_APPLIED || status == DSD_SCAN_ROW_EDIT_STORED) {
        scan_row_edit_note_width_superseded(&p, records_before);
    }
    return scan_row_edit_outcome(label, &p, &value, status, &result, notice, notice_size, ttl_s);
}
