// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
* dsd_events.c
* DSD-FME event history init, watchdog, push, and related functions
*
*
* LWVMOBILE
* 2025-05 DSD-FME Florida Man Edition
*-----------------------------------------------------------------------------*/

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/edacs/edacs_afs.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "call_state_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/runtime/call_alert.h"

enum {
    DSD_EVENT_SUBTYPE_DMR_DATA_BURST = 6,
    DSD_EVENT_SUBTYPE_EXPLICIT_DATA = INT8_MAX,
};

// Safe bounded copy helper that tolerates potential overlap
static inline void
copy_str_field(char* dst, const char* src, size_t cap) {
    if (dst == NULL || src == NULL || cap == 0) {
        return;
    }
    size_t n = strnlen(src, cap - 1);
    DSD_MEMMOVE(dst, src, n);
    dst[n] = '\0';
}

//init each event history struct passed into here
void
init_event_history(Event_History_I* event_struct, uint8_t start, uint8_t stop) {
    if (event_struct == NULL || start >= stop) {
        return;
    }

    for (uint8_t i = start; i < stop; i++) {
        event_struct->Event_History_Items[i].write = 0;
        event_struct->Event_History_Items[i].color_pair = 4;
        event_struct->Event_History_Items[i].severity = DSD_EVENT_SEVERITY_UNKNOWN;
        event_struct->Event_History_Items[i].category = DSD_EVENT_CATEGORY_UNKNOWN;
        event_struct->Event_History_Items[i].systype = -1;
        event_struct->Event_History_Items[i].subtype = -1;
        event_struct->Event_History_Items[i].sys_id1 = 0;
        event_struct->Event_History_Items[i].sys_id2 = 0;
        event_struct->Event_History_Items[i].sys_id3 = 0;
        event_struct->Event_History_Items[i].sys_id4 = 0;
        event_struct->Event_History_Items[i].sys_id5 = 0;
        event_struct->Event_History_Items[i].gi = 0;
        event_struct->Event_History_Items[i].enc = 0;
        event_struct->Event_History_Items[i].enc_alg = 0;
        event_struct->Event_History_Items[i].enc_key = 0;
        event_struct->Event_History_Items[i].mi = 0;
        event_struct->Event_History_Items[i].svc = 0;
        event_struct->Event_History_Items[i].source_id = 0;
        event_struct->Event_History_Items[i].target_id = 0;
        event_struct->Event_History_Items[i].src_str[0] = '\0';
        event_struct->Event_History_Items[i].tgt_str[0] = '\0';
        event_struct->Event_History_Items[i].t_name[0] = '\0';
        event_struct->Event_History_Items[i].s_name[0] = '\0';
        event_struct->Event_History_Items[i].t_mode[0] = '\0';
        event_struct->Event_History_Items[i].s_mode[0] = '\0';
        event_struct->Event_History_Items[i].channel = 0;
        event_struct->Event_History_Items[i].event_time = 0;

        DSD_MEMSET(event_struct->Event_History_Items[i].pdu, 0, sizeof(event_struct->Event_History_Items[0].pdu));
        event_struct->Event_History_Items[i].sysid_string[0] = '\0';
        event_struct->Event_History_Items[i].alias[0] = '\0';
        event_struct->Event_History_Items[i].gps_s[0] = '\0';
        event_struct->Event_History_Items[i].text_message[0] = '\0';
        event_struct->Event_History_Items[i].event_string[0] = '\0';
        event_struct->Event_History_Items[i].internal_str[0] = '\0';
    }
    dsd_event_history_mark_dirty(event_struct);
}

void
push_event_history(Event_History_I* event_struct) {
    if (event_struct == NULL) {
        return;
    }

    //Fixed, had it going in the wrong direction first time
    for (uint8_t i = 254; i >= 1; i--) {
        event_struct->Event_History_Items[i].write = event_struct->Event_History_Items[i - 1].write;
        event_struct->Event_History_Items[i].color_pair = event_struct->Event_History_Items[i - 1].color_pair;
        event_struct->Event_History_Items[i].severity = event_struct->Event_History_Items[i - 1].severity;
        event_struct->Event_History_Items[i].category = event_struct->Event_History_Items[i - 1].category;
        event_struct->Event_History_Items[i].systype = event_struct->Event_History_Items[i - 1].systype;
        event_struct->Event_History_Items[i].subtype = event_struct->Event_History_Items[i - 1].subtype;
        event_struct->Event_History_Items[i].sys_id1 = event_struct->Event_History_Items[i - 1].sys_id1;
        event_struct->Event_History_Items[i].sys_id2 = event_struct->Event_History_Items[i - 1].sys_id2;
        event_struct->Event_History_Items[i].sys_id3 = event_struct->Event_History_Items[i - 1].sys_id3;
        event_struct->Event_History_Items[i].sys_id4 = event_struct->Event_History_Items[i - 1].sys_id4;
        event_struct->Event_History_Items[i].sys_id5 = event_struct->Event_History_Items[i - 1].sys_id5;
        event_struct->Event_History_Items[i].gi = event_struct->Event_History_Items[i - 1].gi;
        event_struct->Event_History_Items[i].enc = event_struct->Event_History_Items[i - 1].enc;
        event_struct->Event_History_Items[i].enc_alg = event_struct->Event_History_Items[i - 1].enc_alg;
        event_struct->Event_History_Items[i].enc_key = event_struct->Event_History_Items[i - 1].enc_key;
        event_struct->Event_History_Items[i].mi = event_struct->Event_History_Items[i - 1].mi;
        event_struct->Event_History_Items[i].svc = event_struct->Event_History_Items[i - 1].svc;
        event_struct->Event_History_Items[i].source_id = event_struct->Event_History_Items[i - 1].source_id;
        event_struct->Event_History_Items[i].target_id = event_struct->Event_History_Items[i - 1].target_id;
        copy_str_field(event_struct->Event_History_Items[i].src_str, event_struct->Event_History_Items[i - 1].src_str,
                       sizeof event_struct->Event_History_Items[i].src_str);
        copy_str_field(event_struct->Event_History_Items[i].tgt_str, event_struct->Event_History_Items[i - 1].tgt_str,
                       sizeof event_struct->Event_History_Items[i].tgt_str);
        copy_str_field(event_struct->Event_History_Items[i].t_name, event_struct->Event_History_Items[i - 1].t_name,
                       sizeof event_struct->Event_History_Items[i].t_name);
        copy_str_field(event_struct->Event_History_Items[i].s_name, event_struct->Event_History_Items[i - 1].s_name,
                       sizeof event_struct->Event_History_Items[i].s_name);
        copy_str_field(event_struct->Event_History_Items[i].t_mode, event_struct->Event_History_Items[i - 1].t_mode,
                       sizeof event_struct->Event_History_Items[i].t_mode);
        copy_str_field(event_struct->Event_History_Items[i].s_mode, event_struct->Event_History_Items[i - 1].s_mode,
                       sizeof event_struct->Event_History_Items[i].s_mode);
        event_struct->Event_History_Items[i].channel = event_struct->Event_History_Items[i - 1].channel;
        event_struct->Event_History_Items[i].event_time = event_struct->Event_History_Items[i - 1].event_time;

        DSD_MEMCPY(event_struct->Event_History_Items[i].pdu, event_struct->Event_History_Items[i - 1].pdu,
                   sizeof(event_struct->Event_History_Items[0].pdu));
        copy_str_field(event_struct->Event_History_Items[i].sysid_string,
                       event_struct->Event_History_Items[i - 1].sysid_string,
                       sizeof event_struct->Event_History_Items[i].sysid_string);
        copy_str_field(event_struct->Event_History_Items[i].alias, event_struct->Event_History_Items[i - 1].alias,
                       sizeof event_struct->Event_History_Items[i].alias);
        copy_str_field(event_struct->Event_History_Items[i].gps_s, event_struct->Event_History_Items[i - 1].gps_s,
                       sizeof event_struct->Event_History_Items[i].gps_s);
        copy_str_field(event_struct->Event_History_Items[i].text_message,
                       event_struct->Event_History_Items[i - 1].text_message,
                       sizeof event_struct->Event_History_Items[i].text_message);
        copy_str_field(event_struct->Event_History_Items[i].event_string,
                       event_struct->Event_History_Items[i - 1].event_string,
                       sizeof event_struct->Event_History_Items[i].event_string);
        copy_str_field(event_struct->Event_History_Items[i].internal_str,
                       event_struct->Event_History_Items[i - 1].internal_str,
                       sizeof event_struct->Event_History_Items[i].internal_str);
    }
    event_struct->push_seq++;
    dsd_event_history_mark_dirty(event_struct);
}

// Optional per-row detail written as its own event-log line. Tracked across a merge so a
// continuation can report exactly what the reacquired segment contributed.
typedef struct {
    uint8_t alias;
    uint8_t gps;
    uint8_t text_message;
    uint8_t internal;
} watchdog_event_merge_added;

// One event-log entry: the rendered line, then whichever optional detail lines apply. A row's
// first commit and a reacquired segment's continuation are the same entry shape, so they share
// one writer and the log format keeps a single source of truth.
//
// `event_string` NULL suppresses the rendered line, for a continuation that only has new detail
// to report. `prefix` marks the entry as a continuation. `selection` limits the detail lines to
// what the caller just learned; NULL writes every non-empty one.
static void
watchdog_event_write_log_entry(const dsd_opts* opts, uint8_t slot, uint8_t swrite, const char* prefix,
                               const char* event_string, const Event_History* row,
                               const watchdog_event_merge_added* selection) {
    if (opts->event_out_file[0] == '\0') {
        return;
    }
    FILE* event_log_file = dsd_fopen_private(opts->event_out_file, "a");
    if (event_log_file == NULL) {
        return;
    }
    if (event_string != NULL) {
        DSD_FPRINTF(event_log_file, "%s%s ", prefix != NULL ? prefix : "", event_string);
        if (swrite == 1) {
            DSD_FPRINTF(event_log_file, "Slot %d; ", slot + 1);
        }
        DSD_FPRINTF(event_log_file, "\n");
    }
    if (selection != NULL ? selection->text_message != 0U : row->text_message[0] != '\0') {
        DSD_FPRINTF(event_log_file, "%s \n", row->text_message);
    }
    if (selection != NULL ? selection->alias != 0U : row->alias[0] != '\0') {
        DSD_FPRINTF(event_log_file, " Talker Alias: %s \n", row->alias);
    }
    if (selection != NULL ? selection->gps != 0U : row->gps_s[0] != '\0') {
        DSD_FPRINTF(event_log_file, " GPS: %s \n", row->gps_s);
    }
    if (selection != NULL ? selection->internal != 0U : row->internal_str[0] != '\0') {
        DSD_FPRINTF(event_log_file, " DSD-neo: %s \n", row->internal_str);
    }
    fflush(event_log_file);
    fclose(event_log_file);
}

void
write_event_to_log_file(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite,
                        const char* event_string) //pass completed event string here that is in the struct
{
    // The rendered line comes from the caller while the optional detail lines come from the
    // slot's staged row. The two are usually the same row and occasionally are not; preserved as
    // it stands rather than unified, since callers rely on passing a line for an already-pushed
    // row.
    watchdog_event_write_log_entry(opts, slot, swrite, NULL, event_string,
                                   &state->event_history_s[slot].Event_History_Items[0], NULL);
}

// Only the two-slot protocols annotate their log lines with a slot number. X2-TDMA belongs here for
// the same reason the other two do: it carries two timeslots and its callers attribute every
// observation through state->currentslot, so a log line without the annotation is ambiguous.
static uint8_t
watchdog_event_should_write_systype(int systype) {
    return (DSD_SYNC_IS_DMR_BS(systype) || DSD_SYNC_IS_P25P2(systype) || DSD_SYNC_IS_X2TDMA(systype)) ? 1u : 0u;
}

static uint8_t
watchdog_event_should_write_slot(const dsd_state* state) {
    return watchdog_event_should_write_systype(state->lastsynctype);
}

// Decoded per-transmission detail beyond identity: an alias, a position, a text message, or a
// notice. One list, shared by watchdog_event_item_has_content() and the identity-less drop check
// below -- a second private copy of these fields would let a future field pass one check and
// fail the other, and a row whose only content is that field would then be staged but silently
// dropped.
static int
watchdog_event_row_has_decoded_detail(const Event_History* item) {
    return item->alias[0] != '\0' || item->gps_s[0] != '\0' || item->text_message[0] != '\0'
           || item->internal_str[0] != '\0';
}

static int
watchdog_event_item_has_content(const Event_History* item) {
    if (item == NULL) {
        return 0;
    }
    return item->event_string[0] != '\0' || watchdog_event_row_has_decoded_detail(item);
}

// Crypto knowledge is decoded detail: a row whose epoch only ever learned "encrypted, this
// alg, this key" still records that an encrypted transmission occurred.
static int
watchdog_event_row_records_crypto(const Event_History* item) {
    return item->enc != 0U || item->enc_alg != 0U || item->enc_key != 0U || item->mi != 0U;
}

// Numeric or textual call identity carried by the row itself. System identifiers alone (a color
// code, a NAC) do not name a call, matching dsd_call_state_snapshot_has_identity(). This is a
// rescue check only -- it can keep a row, never doom one: renderer-built rows copy these fields
// from the canonical snapshot, whose named_call verdict is the authoritative gate, so a field
// this list misses costs nothing as long as the canonical helper counts it.
static int
watchdog_event_row_names_call(const Event_History* item) {
    return item->target_id != 0U || item->source_id != 0U || item->src_str[0] != '\0' || item->tgt_str[0] != '\0';
}

// A voice row that never learned who the call was: no numeric or textual call
// identity and no decoded detail. These render from the provisional epoch a
// stray voice sync opens before any header decodes, and a committed
// "TGT: 00000000; SRC: 00000000" row tells the operator nothing. Data and
// control rows are exempt: repeated LRRP, short data, and response notices are
// real traffic even when source-less.
static int
watchdog_event_voice_row_is_identityless(const Event_History* item) {
    if (item == NULL || item->category != DSD_EVENT_CATEGORY_VOICE) {
        return 0;
    }
    // Protocols whose voice traffic never carries identity keep their all-zero rows; the
    // capability list lives in the canonical layer beside the identity notion it qualifies.
    if (dsd_call_state_protocol_voice_is_anonymous(item->systype)) {
        return 0;
    }
    if (watchdog_event_row_records_crypto(item) || watchdog_event_row_names_call(item)) {
        return 0;
    }
    return !watchdog_event_row_has_decoded_detail(item);
}

// The staged epoch's render-time verdicts vouch for a row the field checks above cannot: it
// named a call in ways the row strings never carry (route text), or it was a real transmission
// on the air even though nothing named it -- decoded voice media ran and a terminator ended it.
// The media-plus-terminator arm is what keeps audible traffic on systems where link control
// rarely verifies (a RAS DMR kerchunk whose embedded LC never reassembled) from vanishing
// without a row, a log line, or an end alert, while a stray-sync noise epoch -- media but no
// terminator, ended by sync loss -- still drops.
static int
watchdog_event_staged_epoch_vouches(const dsd_call_event_render_env* env) {
    return env->named_call != 0U || (env->saw_media != 0U && env->ended_positively != 0U);
}

static int
watchdog_event_is_dmr_data_sync(int systype) {
    return systype == DSD_SYNC_DMR_BS_DATA_POS || systype == DSD_SYNC_DMR_BS_DATA_NEG || systype == DSD_SYNC_DMR_MS_DATA
           || systype == DSD_SYNC_DMR_RC_DATA;
}

static int
watchdog_event_is_explicit_data_event(const Event_History* item) {
    return item != NULL && item->subtype == DSD_EVENT_SUBTYPE_EXPLICIT_DATA;
}

static int
watchdog_event_is_data_event(const Event_History* item) {
    return watchdog_event_is_explicit_data_event(item)
           || (item != NULL && watchdog_event_is_dmr_data_sync(item->systype)
               && item->subtype == DSD_EVENT_SUBTYPE_DMR_DATA_BURST);
}

static void
watchdog_event_rotate_wav_if_needed(dsd_opts* opts, const Event_History_I* event_struct, uint8_t slot) {
    if (opts->static_wav_file != 0) {
        return;
    }

    if (slot == 0 && opts->wav_out_f != NULL) {
        opts->wav_out_f =
            close_and_rename_wav_file(opts->wav_out_f, opts, opts->wav_out_file, opts->wav_out_dir, event_struct);
        opts->wav_out_f = open_wav_file(opts->wav_out_dir, opts->wav_out_file, sizeof opts->wav_out_file, 8000, 0);
        return;
    }

    if (slot == 1 && opts->wav_out_fR != NULL) {
        opts->wav_out_fR =
            close_and_rename_wav_file(opts->wav_out_fR, opts, opts->wav_out_fileR, opts->wav_out_dir, event_struct);
        opts->wav_out_fR = open_wav_file(opts->wav_out_dir, opts->wav_out_fileR, sizeof opts->wav_out_fileR, 8000, 0);
    }
}

static void
watchdog_event_reset_post_push(dsd_state* state) {
    DSD_MEMSET(state->ysf_txt, 0, sizeof(state->ysf_txt));
    DSD_MEMSET(state->dstar_gps, 0, sizeof(state->dstar_gps));
    DSD_MEMSET(state->dstar_txt, 0, sizeof(state->dstar_txt));
}

static void
watchdog_event_maybe_beep_call_end(dsd_opts* opts, dsd_state* state, uint8_t slot, int last_event_is_data) {
    if (!last_event_is_data
        && dsd_call_alert_event_enabled(opts->call_alert, opts->call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_END)) {
        beeper(opts, state, slot, 40, 86, 3);
    }
}

/** How a commit should treat the end-of-call side effects. */
typedef enum {
    /** Not an end-of-call commit: no WAV rotation, no VOICE_END alert. */
    DSD_EVENT_END_NONE = 0,
    /** The transmission is over: rotate the WAV and alert now. */
    DSD_EVENT_END_FINAL,
    /**
     * Sync was lost and the transmission may still resume. Rotate the WAV -- each segment keeps
     * its own recording -- but hold the VOICE_END alert until the reacquisition window closes,
     * so a flapping call does not announce its end partway through.
     */
    DSD_EVENT_END_DEFERRED,
} dsd_event_end_disposition;

// Retire the staged row: rotate the WAV when the transmission is over, clear the row and the
// per-transmission scratch that fed it, then sound the end-of-call alert when a FINAL end asked
// for one. Every path that is done with the staged row -- commit, merge, or drop -- ends in this
// sequence; keeping the alert inside it means the end-of-call side effects cannot drift apart
// across call sites. The rotation must precede the clear: close_and_rename_wav_file() reads its
// rename metadata from Items[0]. `alert_on_final` is zero only on the drop path, whose rule is
// that nothing the operator saw is ending.
static void
watchdog_event_retire_staged_row(dsd_opts* opts, dsd_state* state, Event_History_I* event_struct, uint8_t slot,
                                 dsd_event_end_disposition end_disposition, int alert_on_final,
                                 int last_event_is_data) {
    if (end_disposition != DSD_EVENT_END_NONE) {
        watchdog_event_rotate_wav_if_needed(opts, event_struct, slot);
    }
    init_event_history(event_struct, 0, 1);
    watchdog_event_reset_post_push(state);
    if (alert_on_final && end_disposition == DSD_EVENT_END_FINAL) {
        watchdog_event_maybe_beep_call_end(opts, state, slot, last_event_is_data);
    }
}

static void
watchdog_event_handle_source_transition_ex(dsd_opts* opts, dsd_state* state, Event_History_I* event_struct,
                                           uint8_t slot, uint8_t swrite, int last_event_is_data,
                                           int reset_slot_identity, dsd_event_end_disposition end_disposition) {
    write_event_to_log_file(opts, state, slot, swrite, event_struct->Event_History_Items[0].event_string);

    event_struct->Event_History_Items[0].write = 1;
    push_event_history(event_struct);
    (void)reset_slot_identity;
    watchdog_event_retire_staged_row(opts, state, event_struct, slot, end_disposition, 1, last_event_is_data);
}

static void
watchdog_event_handle_source_transition(dsd_opts* opts, dsd_state* state, Event_History_I* event_struct, uint8_t slot,
                                        uint8_t swrite, int last_event_is_data, int reset_slot_identity) {
    watchdog_event_handle_source_transition_ex(opts, state, event_struct, slot, swrite, last_event_is_data,
                                               reset_slot_identity, DSD_EVENT_END_FINAL);
}

// True when the epoch this slot holds a VOICE_END for has since been positively terminated: a
// terminator or EOT decoded after the sync-loss end and tightened the reason to EXPLICIT. The
// reacquisition window is what the alert was waiting on, and that permission is now retracted,
// so there is nothing left to wait for.
static int
watchdog_event_end_is_positively_terminated(const dsd_call_snapshot* call, const dsd_call_event_lifecycle* lifecycle) {
    if (call == NULL || lifecycle == NULL || !lifecycle->end_alert_pending) {
        return 0;
    }
    return call->phase == DSD_CALL_PHASE_ENDED && call->epoch != 0U && call->epoch == lifecycle->epoch
           && call->end_reason == (uint8_t)DSD_CALL_END_EXPLICIT;
}

// Emit a VOICE_END alert that was held open across a possible reacquisition. `force` retires it
// immediately -- used when a genuinely new call is about to start on the slot, so the previous
// transmission's END is heard before the new one's START rather than interrupting it.
static void
watchdog_event_flush_pending_end_alert(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                       dsd_call_event_lifecycle* lifecycle, int force) {
    if (lifecycle == NULL || !lifecycle->end_alert_pending) {
        return;
    }
    if (!force && dsd_time_now_monotonic_s() < lifecycle->end_alert_due_m) {
        return;
    }
    lifecycle->end_alert_pending = 0U;
    lifecycle->end_alert_due_m = 0.0;
    watchdog_event_maybe_beep_call_end(opts, state, slot, 0);
}

// Snapshot the live decoder inputs the per-protocol builders read directly. Taken once per render
// -- with the row's content, not when that row is eventually pushed -- so a row can later be
// rebuilt against the same values, rather than against a decoder that has retuned, changed
// manufacturer feature id, or been reconfigured since. A staged row is sometimes committed only
// after the canonical layer has opened the next call, by which point the live values describe
// that call rather than the one the row is about.
static void
watchdog_event_capture_render_env(const dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                                  dsd_call_event_render_env* env) {
    env->mfid = slot == 0U ? state->dmr_fid : state->dmr_fidR;
    env->nxdn_grant_chan = state->nxdn_grant_chan;
    env->nxdn_grant_freq = state->nxdn_grant_freq;
    env->ea_mode = state->ea_mode;
    env->edacs_a_bits = state->edacs_a_bits;
    env->edacs_f_bits = state->edacs_f_bits;
    env->edacs_s_bits = state->edacs_s_bits;
    env->edacs_a_shift = state->edacs_a_shift;
    env->edacs_f_shift = state->edacs_f_shift;
    env->edacs_a_mask = state->edacs_a_mask;
    env->edacs_f_mask = state->edacs_f_mask;
    env->edacs_s_mask = state->edacs_s_mask;
    // The epoch verdicts the keep-or-drop decision reads at commit time, captured with the env
    // so they describe the same render. Identity accrues within an epoch, so plain assignment
    // is sticky for named_call; media_active is cleared when the call ends, so saw_media must
    // remember earlier renders itself; the end verdict only exists once the phase is ENDED, and
    // the epoch-boundary memset of the whole env resets all three.
    env->named_call = (uint8_t)(dsd_call_state_snapshot_has_identity(call) != 0);
    env->saw_media = (uint8_t)(env->saw_media != 0U || (call != NULL && call->media_active != 0U));
    env->ended_positively = (uint8_t)(call != NULL && call->phase == DSD_CALL_PHASE_ENDED
                                      && call->end_reason != (uint8_t)DSD_CALL_END_SYNC_LOSS);
}

// Depth of the row this slot last committed, or 0 when it can no longer be located.
// push_event_history() copies row 0 into row 1, so immediately after a commit the row
// sits at index 1 and every push since -- including interleaved data or system notices --
// has pushed it one deeper.
static uint8_t
watchdog_event_committed_row_index(const Event_History_I* event_struct, const dsd_call_event_lifecycle* lifecycle) {
    if (event_struct == NULL || lifecycle == NULL || !lifecycle->committed_valid) {
        return 0U;
    }
    if (event_struct->push_seq < lifecycle->committed_seq) {
        return 0U;
    }
    const uint64_t depth = 1U + (event_struct->push_seq - lifecycle->committed_seq);
    return depth <= 254U ? (uint8_t)depth : 0U;
}

static int
watchdog_event_text_is_empty(const char* text) {
    return text == NULL || text[0] == '\0';
}

// Identity and label fields: a later segment only fills a blank. These describe who the call is,
// and the first segment that decoded them is as authoritative as any later one.
static void
watchdog_event_merge_text(char* retained, const char* staged, size_t cap) {
    if (watchdog_event_text_is_empty(retained) && !watchdog_event_text_is_empty(staged)) {
        DSD_SNPRINTF(retained, cap, "%s", staged);
    }
}

// Progressive fields: the newest decode supersedes. A talker alias arrives over several blocks and
// each one extends it, a later LRRP report is a fresher position, and the newest notice detail is
// the one that just fired. Returns non-zero when the retained value actually changed, so the
// caller can log what the merge added. Keeping the longer alias covers the case where a segment
// re-starts the alias from scratch and only decodes a prefix before sync drops again.
static int
watchdog_event_merge_text_progressive(char* retained, const char* staged, size_t cap, int keep_longer) {
    if (watchdog_event_text_is_empty(staged) || strncmp(retained, staged, cap) == 0) {
        return 0;
    }
    if (keep_longer && strnlen(staged, cap) <= strnlen(retained, cap)) {
        return 0;
    }
    DSD_SNPRINTF(retained, cap, "%s", staged);
    return 1;
}

// Rank crypto knowledge so a later segment can upgrade the retained row but never
// downgrade it: a segment that decoded the PI/ESS header knows more than the late-entry
// segment that had to assume clear.
static int
watchdog_event_crypto_rank(const Event_History* item) {
    if (item->enc == 0U) {
        return item->enc_alg != 0U ? 1 : 0;
    }
    return item->enc_alg != 0U ? 3 : 2;
}

// System identity: the numeric ids drive both the rendered string and every structured consumer,
// so they have to come across. A late-entry first segment renders a placeholder ("P25_000",
// "DMR_CC_0") from all-zero ids, which is non-empty and would otherwise block the string forever
// -- so the string follows whenever the ids themselves were upgraded.
static void
watchdog_event_merge_system_identity(Event_History* retained, const Event_History* staged) {
    int sys_ids_upgraded = 0;
    uint32_t* retained_sys[5] = {&retained->sys_id1, &retained->sys_id2, &retained->sys_id3, &retained->sys_id4,
                                 &retained->sys_id5};
    const uint32_t staged_sys[5] = {staged->sys_id1, staged->sys_id2, staged->sys_id3, staged->sys_id4,
                                    staged->sys_id5};
    for (size_t i = 0; i < 5U; i++) {
        if (*retained_sys[i] == 0U && staged_sys[i] != 0U) {
            *retained_sys[i] = staged_sys[i];
            sys_ids_upgraded = 1;
        }
    }
    if (sys_ids_upgraded) {
        (void)watchdog_event_merge_text_progressive(retained->sysid_string, staged->sysid_string,
                                                    sizeof(retained->sysid_string), 0);
        return;
    }
    watchdog_event_merge_text(retained->sysid_string, staged->sysid_string, sizeof(retained->sysid_string));
}

// Scalar identity a later segment may only fill in, never overwrite: the first segment that
// decoded a value is as authoritative as any later one.
static void
watchdog_event_merge_identity_fields(Event_History* retained, const Event_History* staged) {
    if (retained->source_id == 0U && staged->source_id != 0U) {
        retained->source_id = staged->source_id;
    }
    if (retained->target_id == 0U && staged->target_id != 0U) {
        retained->target_id = staged->target_id;
    }
    if (retained->channel == 0U && staged->channel != 0U) {
        retained->channel = staged->channel;
    }
    if (retained->gi < 0 && staged->gi >= 0) {
        retained->gi = staged->gi;
    }
    if (retained->svc == 0U && staged->svc != 0U) {
        retained->svc = staged->svc;
    }
    watchdog_event_merge_text(retained->src_str, staged->src_str, sizeof(retained->src_str));
    watchdog_event_merge_text(retained->tgt_str, staged->tgt_str, sizeof(retained->tgt_str));
    watchdog_event_merge_text(retained->t_name, staged->t_name, sizeof(retained->t_name));
    watchdog_event_merge_text(retained->s_name, staged->s_name, sizeof(retained->s_name));
    watchdog_event_merge_text(retained->t_mode, staged->t_mode, sizeof(retained->t_mode));
    watchdog_event_merge_text(retained->s_mode, staged->s_mode, sizeof(retained->s_mode));
}

// Fold the staged row into the row already in history: this is the same transmission, and
// everything the reacquired segment learned -- a late-decoded SRC, the system identifiers, an
// alias, GPS, a text message, the crypto header that finally arrived -- has to survive.
// Identity is fill-if-blank; progressive detail is superseded by the newer decode.
static void
watchdog_event_merge_staged_into(Event_History* retained, const Event_History* staged,
                                 watchdog_event_merge_added* added) {
    DSD_MEMSET(added, 0, sizeof(*added));
    watchdog_event_merge_identity_fields(retained, staged);
    watchdog_event_merge_system_identity(retained, staged);

    added->alias =
        (uint8_t)watchdog_event_merge_text_progressive(retained->alias, staged->alias, sizeof(retained->alias), 1);
    added->gps =
        (uint8_t)watchdog_event_merge_text_progressive(retained->gps_s, staged->gps_s, sizeof(retained->gps_s), 0);
    added->text_message = (uint8_t)watchdog_event_merge_text_progressive(retained->text_message, staged->text_message,
                                                                         sizeof(retained->text_message), 0);
    added->internal = (uint8_t)watchdog_event_merge_text_progressive(retained->internal_str, staged->internal_str,
                                                                     sizeof(retained->internal_str), 0);
    if (watchdog_event_crypto_rank(staged) > watchdog_event_crypto_rank(retained)) {
        retained->enc = staged->enc;
        retained->enc_alg = staged->enc_alg;
        retained->enc_key = staged->enc_key;
        retained->mi = staged->mi;
    }
    // event_string is left alone here. The caller re-renders it from the merged fields, and that
    // render needs the existing string: it is the fallback source for the row's date/time prefix
    // when no stamped event_time is available. A failed render therefore keeps what was displayed
    // rather than blanking a committed row.
}

// The first segment already logged its line, so a merge appends a continuation rather than a new
// entry. Same writer as a normal commit, so it carries the same slot annotation and the same
// optional-detail lines: nothing a reacquired segment decoded is visible in the UI but missing
// from the log. Only what this segment actually contributed is reported.
static void
watchdog_event_log_merge_continuation(const dsd_opts* opts, uint8_t slot, uint8_t swrite, const Event_History* retained,
                                      const watchdog_event_merge_added* added, int rendered_changed) {
    if (!rendered_changed && !added->alias && !added->gps && !added->text_message && !added->internal) {
        return;
    }
    // A re-render that produced no string, or one identical to what the row already showed, has
    // nothing new to say; the detail lines below may still.
    const char* rendered = (rendered_changed && retained->event_string[0] != '\0') ? retained->event_string : NULL;
    watchdog_event_write_log_entry(opts, slot, swrite, " Reacquired: ", rendered, retained, added);
}

// True when the staged row may be folded into the row already committed for this slot: the
// canonical layer flagged this epoch as one sync-loss-interrupted transmission being
// reacquired, that row is the one the interrupted epoch itself committed, and both rows are
// voice. A data staged row never merges into a voice row.
static int
watchdog_event_staged_row_merges(const Event_History_I* event_struct, const dsd_call_event_lifecycle* lifecycle,
                                 const Event_History* staged, uint8_t retained_index) {
    if (lifecycle == NULL || lifecycle->epoch == 0U || lifecycle->reacquired_epoch != lifecycle->epoch) {
        return 0;
    }
    // Two rows are legitimate targets. The row the interrupted epoch committed is the one this
    // segment is resuming. This epoch's own row is the second: once a commit lands -- a merge, or a
    // push because the interrupted epoch never got a row of its own -- every later commit in the
    // same epoch is still the same transmission and belongs in it. Without that case an epoch that
    // commits twice, as a mid-segment call notice followed by its end does, would push a duplicate.
    //
    // The reopened epoch may have ended without ever pushing a row -- it staged nothing, or a
    // reset intervened -- in which case committed_epoch names something older than either. Folding
    // into that would put this transmission inside an unrelated call's row, so commit a new one.
    if (!lifecycle->committed_valid
        || (lifecycle->committed_epoch != lifecycle->reacquired_from_epoch
            && lifecycle->committed_epoch != lifecycle->epoch)) {
        return 0;
    }
    if (retained_index == 0U || staged->category != DSD_EVENT_CATEGORY_VOICE) {
        return 0;
    }
    return event_struct->Event_History_Items[retained_index].category == DSD_EVENT_CATEGORY_VOICE;
}

// Rebuild the merged row's user-legible string from its now-complete fields. Returns non-zero
// when a string could be produced, and sets *changed when that string differs from the one the
// row already carried. Defined below, beside the per-protocol builders it reuses.
static int watchdog_event_rerender_row(const dsd_call_event_render_env* env, Event_History* item, int* changed);

// Commit the staged row, or merge it into the row this slot already committed when the
// canonical layer says the two are the same transmission. Returns non-zero when the staged
// row's information reached history -- pushed as a new row or folded into the committed one --
// and zero when nothing did.
static int
watchdog_event_commit_staged_row(dsd_opts* opts, dsd_state* state, Event_History_I* event_struct, uint8_t slot,
                                 dsd_call_event_lifecycle* lifecycle, int last_event_is_data, int reset_slot_identity,
                                 dsd_event_end_disposition end_disposition) {
    const Event_History* staged = &event_struct->Event_History_Items[0];
    // A voice row whose epoch was never anything the operator could name or hear end is dropped
    // rather than committed as "TGT: 00000000; SRC: 00000000". The staged verdicts -- recorded
    // at render time, so every commit path gets the same answer, including the epoch-change
    // path where the staged row's canonical snapshot is already gone -- vouch for what the row
    // fields cannot: route-text-only identity, or audible media that a terminator positively
    // ended. Without a lifecycle there are no verdicts to consult, so the row is kept: the
    // paths that commit without one (a notice for a non-canonical call) stage real traffic,
    // and a duplicate row is recoverable where a deleted transmission is not. The WAV still
    // rotates on a drop: voice frames may have run for the epoch, and leaving the file open
    // would let the noise segment's audio lead the next transmission's recording. No end alert
    // either -- like the empty-staged-row case, nothing the operator saw is ending.
    if (watchdog_event_voice_row_is_identityless(staged) && lifecycle != NULL
        && !watchdog_event_staged_epoch_vouches(&lifecycle->staged_env)) {
        watchdog_event_retire_staged_row(opts, state, event_struct, slot, end_disposition, 0, last_event_is_data);
        return 0;
    }
    const uint8_t retained_index = watchdog_event_committed_row_index(event_struct, lifecycle);
    if (watchdog_event_staged_row_merges(event_struct, lifecycle, staged, retained_index)) {
        Event_History* retained = &event_struct->Event_History_Items[retained_index];
        watchdog_event_merge_added added;
        watchdog_event_merge_staged_into(retained, staged, &added);
        // Re-render against the environment the row was committed under, not the live decoder.
        // A protocol with no builder, or a row with no recoverable timestamp, keeps its string.
        int rendered_changed = 0;
        (void)watchdog_event_rerender_row(&lifecycle->committed_env, retained, &rendered_changed);
        // Annotation taken from the row, not from the live decoder: by the time a reacquired
        // segment merges, lastsynctype may name a system the decoder moved on to, or have been
        // cleared entirely by no_carrier_reset_decode_state(). The row's own systype is what its
        // first commit was annotated from, so both halves of one transmission agree in the log.
        watchdog_event_log_merge_continuation(opts, slot, watchdog_event_should_write_systype(retained->systype),
                                              retained, &added, rendered_changed);
        dsd_event_history_mark_dirty(event_struct);
        // The merged row is now this epoch's row too, so late enrichment for the reacquired
        // epoch resolves to it and the next segment in the chain has a valid merge target.
        lifecycle->committed_epoch = lifecycle->epoch;
        // Each segment keeps its own recording; leaving the WAV open would let it absorb the
        // next transmission's audio and be exported under that call's metadata instead.
        watchdog_event_retire_staged_row(opts, state, event_struct, slot, end_disposition, 1, last_event_is_data);
        return 1;
    }
    watchdog_event_handle_source_transition_ex(opts, state, event_struct, slot, watchdog_event_should_write_slot(state),
                                               last_event_is_data, reset_slot_identity, end_disposition);
    if (lifecycle != NULL) {
        lifecycle->committed_seq = event_struct->push_seq;
        lifecycle->committed_epoch = lifecycle->epoch;
        lifecycle->committed_valid = 1U;
        // Promoted from the render, not re-read from the decoder: this commit may be running
        // because the epoch changed, in which case the live values already describe the incoming
        // call rather than the row being pushed.
        lifecycle->committed_env = lifecycle->staged_env;
    }
    return 1;
}

static int
watchdog_event_call_is_authoritative(const dsd_call_snapshot* call, const dsd_call_event_lifecycle* lifecycle) {
    if (call == NULL || call->epoch == 0U) {
        return 0;
    }
    if (lifecycle == NULL || call->phase == DSD_CALL_PHASE_ACTIVE) {
        return 1;
    }
    return call->phase == DSD_CALL_PHASE_ENDED && (lifecycle->epoch != call->epoch || !lifecycle->ended_committed);
}

static uint32_t
watchdog_event_call_target_id(const dsd_call_snapshot* call) {
    if (call->ota_target_id != 0U && call->ota_target_id <= UINT32_MAX) {
        return (uint32_t)call->ota_target_id;
    }
    if (call->policy_target_id != 0U && call->policy_target_id <= UINT32_MAX) {
        return (uint32_t)call->policy_target_id;
    }
    return 0U;
}

static int
watchdog_event_history_matches_call(const Event_History* item, const dsd_call_snapshot* call) {
    if (item == NULL || call == NULL || item->category != DSD_EVENT_CATEGORY_VOICE || item->systype != call->protocol) {
        return 0;
    }

    const int expected_gi = call->kind == DSD_CALL_KIND_GROUP_VOICE     ? 0
                            : call->kind == DSD_CALL_KIND_PRIVATE_VOICE ? 1
                                                                        : -1;
    if (expected_gi < 0 || item->gi != expected_gi) {
        return 0;
    }

    const uint32_t target_id = watchdog_event_call_target_id(call);
    if (item->target_id != 0U && item->target_id != target_id) {
        return 0;
    }
    return item->source_id == 0U || (call->ota_source_id <= UINT32_MAX && item->source_id == call->ota_source_id);
}

static void
watchdog_event_history_authoritative(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                                     dsd_call_event_lifecycle* lifecycle) {
    Event_History_I* event_struct = &state->event_history_s[slot];
    const int epoch_changed = lifecycle->epoch != call->epoch;
    if (!epoch_changed) {
        return;
    }

    const Event_History* current = &event_struct->Event_History_Items[0];
    const int has_content = watchdog_event_item_has_content(current);
    const int promotes_current = lifecycle->epoch == 0U && watchdog_event_history_matches_call(current, call);
    if (has_content && !promotes_current) {
        // The staged row belongs to the outgoing epoch; the staged verdicts still describe it,
        // so the incoming call's identity cannot vouch for the leftover row.
        (void)watchdog_event_commit_staged_row(opts, state, event_struct, slot, lifecycle,
                                               watchdog_event_is_data_event(current), 0, DSD_EVENT_END_FINAL);
    } else if (has_content) {
        init_event_history(event_struct, 0, 1);
    }

    lifecycle->epoch = call->epoch;
    lifecycle->ended_committed = 0U;
    // Set after the commit above, which belongs to the outgoing epoch. Rows staged directly by a
    // protocol never pass through the renderer, so without this they would inherit whichever
    // environment -- and epoch verdicts -- the previous epoch's last render left behind.
    DSD_MEMSET(&lifecycle->staged_env, 0, sizeof(lifecycle->staged_env));
    lifecycle->notice_epoch = 0U;
    lifecycle->notice_target_id = 0U;
    lifecycle->notice_kind = DSD_CALL_KIND_UNKNOWN;
    lifecycle->notice_handled = 0U;
    // A reacquired segment is the same transmission resuming, not a new one: beeping START
    // again would leave a flapping call with several STARTs against its single END. It also
    // means the previous segment's held VOICE_END was premature, so it is simply dropped.
    const int reacquired = lifecycle->reacquired_epoch == call->epoch;
    if (reacquired) {
        lifecycle->end_alert_pending = 0U;
        lifecycle->end_alert_due_m = 0.0;
    } else {
        // A different call is taking the slot. Retire any held END now so the operator hears it
        // before this call's START rather than partway into it.
        watchdog_event_flush_pending_end_alert(opts, state, slot, lifecycle, 1);
    }
    if (call->phase == DSD_CALL_PHASE_ACTIVE && call->kind != DSD_CALL_KIND_DATA && !reacquired
        && dsd_call_alert_event_enabled(opts->call_alert, opts->call_alert_events, DSD_CALL_ALERT_EVENT_VOICE_START)) {
        beeper(opts, state, slot, 40, 86, 3);
    }
}

static void
watchdog_event_history_impl(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                            dsd_call_event_lifecycle* lifecycle) {
    if (!opts || !state || !state->event_history_s || slot > 1U) {
        return;
    }

    Event_History_I* event_struct = &state->event_history_s[slot];
    const Event_History* last_event = &event_struct->Event_History_Items[0];
    const uint8_t swrite = watchdog_event_should_write_slot(state);
    const int last_event_forces_history = watchdog_event_is_explicit_data_event(last_event);
    const int last_event_is_data = watchdog_event_is_data_event(last_event);
    const int last_event_has_content = watchdog_event_item_has_content(last_event);
    if (last_event_forces_history && last_event_has_content) {
        watchdog_event_handle_source_transition(opts, state, event_struct, slot, swrite, last_event_is_data, 1);
        return;
    }
    if (lifecycle != NULL && watchdog_event_call_is_authoritative(call, lifecycle)) {
        watchdog_event_history_authoritative(opts, state, slot, call, lifecycle);
        return;
    }
}

// run once per loop to check for and push and update event history
void
watchdog_event_history(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    if (!opts || !state || !state->event_history_s || slot > 1U) {
        return;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return;
    }
    dsd_call_state_ext_lock(ext);
    watchdog_event_history_impl(opts, state, slot, &ext->calls.slots[slot], &ext->events[slot]);
    dsd_call_state_ext_unlock(ext);
}

//similar to above, but constantly testing and checking the most recent event only
//this will hopefully be more useful when dealing with an ongoing event with
//features that update over time with embedded signalling, etc
typedef struct {
    dsd_event_severity severity;
    dsd_event_category category;
    int protocol;
    dsd_call_kind kind;
    dsd_call_crypto_state crypto;
    uint32_t source_id;
    uint32_t target_id;
    char src_str[200];
    char tgt_str[200];
    char t_name[200];
    char s_name[200];
    char t_mode[200];
    char s_mode[200];
    uint16_t svc_opts;
    uint8_t subtype;
    /* Live decoder inputs the builders below still need. Captured alongside the committed row
     * so a re-render reproduces the context the row was built under. */
    dsd_call_event_render_env env;
    uint32_t sys_id1;
    uint32_t sys_id2;
    uint32_t sys_id3;
    uint32_t sys_id4;
    uint32_t sys_id5;
    uint32_t channel;
    uint8_t enc;
    uint8_t alg_id;
    uint16_t key_id;
    unsigned long long int mi;
    char sysid_string[200];
    uint8_t t_name_loaded;
    uint8_t s_name_loaded;
} watchdog_event_current_ctx;

typedef struct {
    uint16_t bit;
    const char* token_underscore;
    const char* token_space;
} watchdog_event_edacs_flag;

static const watchdog_event_edacs_flag k_watchdog_event_edacs_flags[] = {
    {0x04, "Emergency_", "Emergency "},
    {0x08, "Group_", "Group "},
    {0x10, "I_", "I "},
    {0x20, "ALL_", "ALL "},
    {0x40, "INTER_", "INTER "},
    {0x80, "TEST_", "TEST "},
    {0x100, "AGENCY_", "AGENCY "},
    {0x200, "FLEET_", "FLEET "},
    {0x01, "Voice_", "Voice "},
};

static void
watchdog_event_str_append(char* dst, size_t dst_sz, const char* src) {
    if (dst == NULL || src == NULL || dst_sz == 0) {
        return;
    }

    size_t dst_len = strnlen(dst, dst_sz);
    if (dst_len >= dst_sz) {
        return;
    }

    size_t rem = dst_sz - dst_len - 1U;
    if (rem > 0) {
        dsd_strncat_s(dst, dst_sz, src, rem);
    }
}

static void
watchdog_event_build_edacs_sup_str(uint16_t svc_opts, int use_underscore, char* out, size_t out_sz) {
    if (out == NULL || out_sz == 0) {
        return;
    }

    DSD_MEMSET(out, 0, out_sz);
    if (use_underscore) {
        watchdog_event_str_append(out, out_sz, "_");
    }

    if (svc_opts & 0x02) {
        watchdog_event_str_append(out, out_sz, use_underscore ? "Digital_" : "Digital ");
    } else {
        watchdog_event_str_append(out, out_sz, use_underscore ? "Analog_" : "Analog ");
    }

    size_t count = sizeof(k_watchdog_event_edacs_flags) / sizeof(k_watchdog_event_edacs_flags[0]);
    for (size_t i = 0; i < count; i++) {
        if (svc_opts & k_watchdog_event_edacs_flags[i].bit) {
            watchdog_event_str_append(out, out_sz,
                                      use_underscore ? k_watchdog_event_edacs_flags[i].token_underscore
                                                     : k_watchdog_event_edacs_flags[i].token_space);
        }
    }

    watchdog_event_str_append(out, out_sz, "Call");
}

static void
watchdog_event_set_ysf_text_message(dsd_state* state, Event_History* item) {
    char ysf_emp[21][21];
    DSD_MEMSET(ysf_emp, 0, sizeof(ysf_emp));

    if (memcmp(ysf_emp, state->ysf_txt, sizeof(state->ysf_txt)) != 0) {
        uint8_t k = 0;
        for (uint8_t i = 4; i < 8; i++) {
            for (uint8_t j = 0; j < 20; j++) {
                if (state->ysf_txt[i][j] != 0x2A) {
                    item->text_message[k++] = state->ysf_txt[i][j];
                } else {
                    item->text_message[k++] = 0x20;
                }
            }
            item->text_message[k] = 0;
        }
    } else {
        item->text_message[0] = '\0';
    }
}

static void
watchdog_event_current_init_base(const dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                                 watchdog_event_current_ctx* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->severity = DSD_EVENT_SEVERITY_INFO;
    ctx->category = DSD_EVENT_CATEGORY_VOICE;
    ctx->protocol = DSD_SYNC_NONE;
    ctx->kind = DSD_CALL_KIND_UNKNOWN;
    ctx->subtype = slot == 0U ? state->dmrburstL : state->dmrburstR;
    watchdog_event_capture_render_env(state, slot, call, &ctx->env);

    if (!call || call->epoch == 0U) {
        return;
    }
    ctx->protocol = call->protocol;
    ctx->kind = call->kind;
    ctx->category = call->kind == DSD_CALL_KIND_DATA ? DSD_EVENT_CATEGORY_DATA : DSD_EVENT_CATEGORY_VOICE;
    ctx->crypto = call->crypto;
    ctx->source_id = call->ota_source_id <= UINT32_MAX ? (uint32_t)call->ota_source_id : 0U;
    ctx->target_id = watchdog_event_call_target_id(call);
    ctx->svc_opts = call->service_options;
    ctx->enc = call->crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING || call->crypto == DSD_CALL_CRYPTO_ENCRYPTED
               || call->crypto == DSD_CALL_CRYPTO_DECRYPTABLE;
    ctx->alg_id = call->algid;
    ctx->key_id = call->kid;
    ctx->mi = call->mi;
    ctx->channel = call->channel;
    DSD_SNPRINTF(ctx->src_str, sizeof ctx->src_str, "%s", call->source_text);
    DSD_SNPRINTF(ctx->tgt_str, sizeof ctx->tgt_str, "%s", call->target_text);

    ctx->sys_id1 = state->p2_wacn;
    ctx->sys_id2 = state->p2_sysid;
    if (state->nac != 0) {
        ctx->sys_id3 = state->nac;
    } else {
        ctx->sys_id3 = state->p2_cc;
    }
    ctx->sys_id4 = state->p2_rfssid;
    ctx->sys_id5 = state->p2_siteid;

    if (ctx->sys_id1) {
        DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "P25_%05X%03X%03X_%d_%d", ctx->sys_id1, ctx->sys_id2,
                     ctx->sys_id3, ctx->sys_id4, ctx->sys_id5);
    } else {
        DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "P25_%03X", ctx->sys_id3);
    }

    if (DSD_SYNC_IS_DMR(ctx->protocol)) {
        ctx->sys_id1 = state->dmr_t3_syscode;
        ctx->sys_id2 = state->dmr_color_code;
        if (ctx->sys_id1) {
            DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "DMR_%X_CC_%d", ctx->sys_id1, ctx->sys_id2);
        } else {
            DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "DMR_CC_%d", ctx->sys_id2);
        }
    }
}

static void
watchdog_event_current_apply_nxdn(const dsd_state* state, watchdog_event_current_ctx* ctx) {
    ctx->sys_id1 = state->nxdn_location_site_code;
    ctx->sys_id2 = state->nxdn_location_sys_code;
    ctx->sys_id3 = state->nxdn_last_ran;

    if (ctx->sys_id1) {
        DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "NXDN_%d_%d_RAN_%d", ctx->sys_id2, ctx->sys_id1,
                     ctx->sys_id3);
    } else {
        DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "NXDN_RAN_%d", ctx->sys_id3);
    }
}

static void
watchdog_event_current_apply_ysf(dsd_state* state, Event_History* item, watchdog_event_current_ctx* ctx) {
    watchdog_event_set_ysf_text_message(state, item);

    DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "%s", "YSF");
}

static void
watchdog_event_current_apply_m17(const dsd_state* state, watchdog_event_current_ctx* ctx) {
    ctx->sys_id1 = ctx->svc_opts & 0xFU;

    DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "M17_CAN_%d", ctx->sys_id1);
    (void)state;
}

static void
watchdog_event_current_apply_dstar(const dsd_state* state, watchdog_event_current_ctx* ctx) {
    DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "%s", "DSTAR");
    (void)state;
}

static void
watchdog_event_current_apply_dpmr(const dsd_state* state, watchdog_event_current_ctx* ctx) {
    DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "DPMR_CC_%d", state->dpmr_color_code);
}

static void
watchdog_event_current_apply_edacs(const dsd_state* state, watchdog_event_current_ctx* ctx) {
    ctx->sys_id1 = state->edacs_site_id;
    ctx->sys_id2 = state->edacs_area_code;
    ctx->sys_id3 = state->edacs_sys_id;

    char sup_str[200];
    watchdog_event_build_edacs_sup_str(ctx->svc_opts, 1, sup_str, sizeof(sup_str));

    DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "EDACS_SITE_%03u", (unsigned)ctx->sys_id1);
    watchdog_event_str_append(ctx->sysid_string, sizeof(ctx->sysid_string), sup_str);

    if (state->ea_mode == 0) {
        int afs = (int)ctx->target_id;
        int a = (afs >> state->edacs_a_shift) & state->edacs_a_mask;
        int f = (afs >> state->edacs_f_shift) & state->edacs_f_mask;
        int s = afs & state->edacs_s_mask;

        DSD_SNPRINTF(ctx->tgt_str, sizeof(ctx->tgt_str), "%03d_AFS_%02d_%02d%01d", afs, a, f, s);
        if (ctx->source_id != 0U) {
            DSD_SNPRINTF(ctx->src_str, sizeof(ctx->src_str), "LID_%u", ctx->source_id);
        } else {
            DSD_SNPRINTF(ctx->src_str, sizeof(ctx->src_str), "LID_UNK");
        }
    }
}

static void
watchdog_event_current_apply_protocol_metadata(dsd_state* state, Event_History* item, watchdog_event_current_ctx* ctx) {
    if (DSD_SYNC_IS_NXDN(ctx->protocol)) {
        watchdog_event_current_apply_nxdn(state, ctx);
    }

    if (DSD_SYNC_IS_YSF(ctx->protocol)) {
        watchdog_event_current_apply_ysf(state, item, ctx);
    }

    if (DSD_SYNC_IS_M17(ctx->protocol)) {
        watchdog_event_current_apply_m17(state, ctx);
    }

    if (DSD_SYNC_IS_DSTAR(ctx->protocol)) {
        watchdog_event_current_apply_dstar(state, ctx);
    }

    if (DSD_SYNC_IS_DPMR(ctx->protocol)) {
        watchdog_event_current_apply_dpmr(state, ctx);
    }

    if (DSD_SYNC_IS_EDACS(ctx->protocol)) {
        watchdog_event_current_apply_edacs(state, ctx);
    }
}

static void
watchdog_event_current_load_labels(const dsd_state* state, watchdog_event_current_ctx* ctx) {
    ctx->t_name_loaded = 0;
    ctx->s_name_loaded = 0;

    if (ctx->target_id != 0
        && dsd_tg_policy_lookup_label(state, ctx->target_id, ctx->t_mode, sizeof(ctx->t_mode), ctx->t_name,
                                      sizeof(ctx->t_name))) {
        ctx->t_name_loaded = 1;
    }

    if (ctx->source_id != 0
        && dsd_tg_policy_lookup_label(state, ctx->source_id, ctx->s_mode, sizeof(ctx->s_mode), ctx->s_name,
                                      sizeof(ctx->s_name))) {
        ctx->s_name_loaded = 1;
    }
}

static void
watchdog_event_current_update_item(const dsd_opts* opts, dsd_state* state, uint8_t slot, Event_History* item,
                                   const watchdog_event_current_ctx* ctx, time_t now) {
    item->write = 0;
    dsd_event_history_item_set_metadata(item, ctx->severity, ctx->category);
    if (ctx->protocol != DSD_SYNC_NONE) {
        item->systype = ctx->protocol;
    } else {
        item->systype = 39;
    }
    item->subtype = ctx->subtype;
    item->gi = ctx->kind == DSD_CALL_KIND_GROUP_VOICE ? 0 : ctx->kind == DSD_CALL_KIND_PRIVATE_VOICE ? 1 : -1;
    item->sys_id1 = ctx->sys_id1;
    item->sys_id2 = ctx->sys_id2;
    item->sys_id3 = ctx->sys_id3;
    item->sys_id4 = ctx->sys_id4;
    item->sys_id5 = ctx->sys_id5;
    item->enc = ctx->enc;
    item->enc_alg = ctx->alg_id;
    item->enc_key = ctx->key_id;
    item->mi = ctx->mi;
    item->svc = ctx->svc_opts;
    item->source_id = ctx->source_id;
    item->target_id = ctx->target_id;
    item->channel = ctx->channel;
    if (opts->playfiles == 0) {
        item->event_time = now;
    }

    DSD_SNPRINTF(item->sysid_string, sizeof(item->sysid_string), "%s", ctx->sysid_string);
    DSD_SNPRINTF(item->src_str, sizeof(item->src_str), "%s", ctx->src_str);
    DSD_SNPRINTF(item->tgt_str, sizeof(item->tgt_str), "%s", ctx->tgt_str);
    DSD_SNPRINTF(item->t_name, sizeof(item->t_name), "%s", ctx->t_name);
    DSD_SNPRINTF(item->s_name, sizeof(item->s_name), "%s", ctx->s_name);
    DSD_SNPRINTF(item->t_mode, sizeof(item->t_mode), "%s", ctx->t_mode);
    DSD_SNPRINTF(item->s_mode, sizeof(item->s_mode), "%s", ctx->s_mode);
    (void)state;
    (void)slot;
}

static void
watchdog_event_current_build_event_text_ids(const watchdog_event_current_ctx* ctx, const char* datestr,
                                            const char* timestr, const char* sys_string, char* event_string,
                                            size_t event_size) {
    DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %s SRC: %s ", datestr, timestr, sys_string, ctx->tgt_str,
                 ctx->src_str);
}

static void
watchdog_event_current_build_event_m17(const watchdog_event_current_ctx* ctx, const char* datestr, const char* timestr,
                                       const char* sys_string, char* event_string, size_t event_size) {
    DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %s SRC: %s CAN: %02u;", datestr, timestr, sys_string,
                 ctx->tgt_str, ctx->src_str, ctx->sys_id1);
}

static void
watchdog_event_current_build_event_dpmr(const watchdog_event_current_ctx* ctx, const char* datestr, const char* timestr,
                                        const char* sys_string, char* event_string, size_t event_size) {
    DSD_SNPRINTF(event_string, event_size, "%s %s %s CC: %02u; TGT: %s; SRC: %s; ", datestr, timestr, sys_string,
                 ctx->channel, ctx->tgt_str, ctx->src_str);
    if (ctx->enc) {
        watchdog_event_str_append(event_string, event_size, "Scrambler Enc; ");
    }
}

static void
watchdog_event_current_build_event_edacs(const watchdog_event_current_ctx* ctx, const char* datestr,
                                         const char* timestr, const char* sys_string, char* event_string,
                                         size_t event_size) {
    char sup_str[200];
    watchdog_event_build_edacs_sup_str(ctx->svc_opts, 0, sup_str, sizeof(sup_str));

    if (ctx->env.ea_mode == 1) {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %07d; SRC: %07d; LCN: %02d; SITE: %d:%d.%04X; %s;",
                     datestr, timestr, sys_string, ctx->target_id, ctx->source_id, ctx->channel, ctx->sys_id1,
                     ctx->sys_id2, ctx->sys_id3, sup_str);
        return;
    }

    int afs = (int)ctx->target_id;
    int a = (afs >> ctx->env.edacs_a_shift) & ctx->env.edacs_a_mask;
    int f = (afs >> ctx->env.edacs_f_shift) & ctx->env.edacs_f_mask;
    int s = afs & ctx->env.edacs_s_mask;
    char afs_str[8];
    getAfsStringFromBits(ctx->env.edacs_a_bits, ctx->env.edacs_f_bits, ctx->env.edacs_s_bits, afs_str, a, f, s);

    char lid_str[20];
    DSD_MEMSET(lid_str, 0, sizeof(lid_str));
    if (ctx->source_id != 0U) {
        DSD_SNPRINTF(lid_str, sizeof(lid_str), "LID: %05u;", ctx->source_id);
    } else {
        DSD_SNPRINTF(lid_str, sizeof(lid_str), "LID: __UNK;");
    }

    DSD_SNPRINTF(event_string, event_size, "%s %s %s AFS: %s (%04d); %s LCN: %02d; Site: %d; %s; ", datestr, timestr,
                 sys_string, afs_str, afs, lid_str, ctx->channel, ctx->sys_id1, sup_str);
}

// The Group/Private annotation, shared by every builder that renders one. Kept in one place so a
// new dsd_call_kind, or a change to either token, cannot be applied to some protocols and missed on
// others.
static void
watchdog_event_append_call_kind(const watchdog_event_current_ctx* ctx, char* event_string, size_t event_size) {
    if (ctx->kind == DSD_CALL_KIND_GROUP_VOICE) {
        watchdog_event_str_append(event_string, event_size, "Group; ");
    } else if (ctx->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        watchdog_event_str_append(event_string, event_size, "Private; ");
    }
}

static void
watchdog_event_current_build_event_dmr(const watchdog_event_current_ctx* ctx, const char* datestr, const char* timestr,
                                       const char* sys_string, char* event_string, size_t event_size) {
    if (ctx->sys_id1) {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; CC: %02d; SYS: %X; ", datestr, timestr,
                     sys_string, ctx->target_id, ctx->source_id, ctx->sys_id2, ctx->sys_id1);
    } else {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; CC: %02d; ", datestr, timestr,
                     sys_string, ctx->target_id, ctx->source_id, ctx->sys_id2);
    }

    if (ctx->enc) {
        watchdog_event_str_append(event_string, event_size, "ENC; ");
    }
    if (ctx->alg_id != 0) {
        char ess_str[30];
        DSD_SNPRINTF(ess_str, sizeof(ess_str), "ALG: %02X; KID: %02X; ", ctx->alg_id, ctx->key_id);
        watchdog_event_str_append(event_string, event_size, ess_str);
    }

    if (ctx->svc_opts & 0x80) {
        watchdog_event_str_append(event_string, event_size, "Emergency; ");
    }
    if (ctx->svc_opts & 0x08) {
        watchdog_event_str_append(event_string, event_size, "Broadcast; ");
    }
    if (ctx->svc_opts & 0x04) {
        watchdog_event_str_append(event_string, event_size, "OVCM; ");
    }

    watchdog_event_append_call_kind(ctx, event_string, event_size);

    if (ctx->env.mfid == 0x10) {
        if (ctx->svc_opts & 0x30) {
            watchdog_event_str_append(event_string, event_size, "TXI; ");
        }

        if (ctx->svc_opts & 0x03) {
            watchdog_event_str_append(event_string, event_size, "PRIORITY; ");
        }
    }
}

// The ALG/KID annotation carried by the protocols that use P25-style ESS. A row persists only the
// derived enc flag, so a merged row that lost the live classification still reports encryption.
static void
watchdog_event_append_ess_crypto(const watchdog_event_current_ctx* ctx, char* event_string, size_t event_size) {
    if (ctx->alg_id != 0 && ctx->alg_id != 0x80) {
        char ess_str[30];
        DSD_SNPRINTF(ess_str, sizeof(ess_str), "ENC; ALG: %02X; KID: %04X; ", ctx->alg_id, ctx->key_id);
        watchdog_event_str_append(event_string, event_size, ess_str);
    } else if (ctx->crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING || ctx->crypto == DSD_CALL_CRYPTO_ENCRYPTED
               || ctx->enc) {
        watchdog_event_str_append(event_string, event_size, "ENC; ");
    }
}

static void
watchdog_event_current_build_event_p25(const watchdog_event_current_ctx* ctx, const char* datestr, const char* timestr,
                                       const char* sys_string, char* event_string, size_t event_size) {
    if (ctx->sys_id1) {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; NAC: %03X; NET_STS: %05X:%03X:%d.%d; ",
                     datestr, timestr, sys_string, ctx->target_id, ctx->source_id, ctx->sys_id3, ctx->sys_id1,
                     ctx->sys_id2, ctx->sys_id4, ctx->sys_id5);
    } else {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; NAC: %03X; ", datestr, timestr,
                     sys_string, ctx->target_id, ctx->source_id, ctx->sys_id3);
    }

    watchdog_event_append_ess_crypto(ctx, event_string, event_size);
    if (ctx->svc_opts & 0x80) {
        watchdog_event_str_append(event_string, event_size, "Emergency; ");
    }
    watchdog_event_append_call_kind(ctx, event_string, event_size);
}

// X2-TDMA parses no call identity: the link control it collects is never decoded into a talkgroup
// or a source, so those fields stay zero for the life of the call. The row still has to exist --
// without one the transmission is absent from history and from the log entirely, and the operator
// has no record that the slot carried voice at all. Deliberately not routed through the P25
// builder despite the shared ESS crypto model: X2-TDMA has no NAC, and rendering "NAC: 000" would
// assert something the decoder never read.
static void
watchdog_event_current_build_event_x2tdma(const watchdog_event_current_ctx* ctx, const char* datestr,
                                          const char* timestr, const char* sys_string, char* event_string,
                                          size_t event_size) {
    DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; ", datestr, timestr, sys_string,
                 ctx->target_id, ctx->source_id);
    watchdog_event_append_ess_crypto(ctx, event_string, event_size);
    watchdog_event_append_call_kind(ctx, event_string, event_size);
}

static void
watchdog_event_current_build_event_nxdn(const watchdog_event_current_ctx* ctx, const char* datestr, const char* timestr,
                                        const char* sys_string, char* event_string, size_t event_size) {
    if (ctx->sys_id1) {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; RAN: %02d; SYS: %d.%d; ", datestr,
                     timestr, sys_string, ctx->target_id, ctx->source_id, ctx->sys_id3, ctx->sys_id2, ctx->sys_id1);
    } else {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; RAN: %02d; ", datestr, timestr,
                     sys_string, ctx->target_id, ctx->source_id, ctx->sys_id3);
    }

    if (ctx->env.nxdn_grant_chan != 0) {
        char ch_str[96];
        if (ctx->env.nxdn_grant_freq != 0) {
            DSD_SNPRINTF(ch_str, sizeof(ch_str), "CH: %u; FREQ: %.6lf MHz; ", ctx->env.nxdn_grant_chan,
                         (double)ctx->env.nxdn_grant_freq / 1000000.0);
        } else {
            DSD_SNPRINTF(ch_str, sizeof(ch_str), "CH: %u; ", ctx->env.nxdn_grant_chan);
        }
        watchdog_event_str_append(event_string, event_size, ch_str);
    }

    if (ctx->enc) {
        watchdog_event_str_append(event_string, event_size, "ENC; ");
    }
    if (ctx->alg_id != 0) {
        char ess_str[30];
        DSD_SNPRINTF(ess_str, sizeof(ess_str), "ALG: %d; KID: %02X; ", ctx->alg_id, ctx->key_id);
        watchdog_event_str_append(event_string, event_size, ess_str);
    }

    watchdog_event_append_call_kind(ctx, event_string, event_size);
}

// Every builder renders purely from ctx -- the identity and metadata copied off the call or the
// row, plus the render env captured with it. Nothing here reads live decoder state, so the same
// ctx always produces the same string no matter when it is rebuilt.
static void
watchdog_event_current_build_event_string(const watchdog_event_current_ctx* ctx, const char* datestr,
                                          const char* timestr, const char* sys_string, char* event_string,
                                          size_t event_size) {
    if (DSD_SYNC_IS_YSF(ctx->protocol) || DSD_SYNC_IS_DSTAR(ctx->protocol)) {
        watchdog_event_current_build_event_text_ids(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_M17(ctx->protocol)) {
        watchdog_event_current_build_event_m17(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_DPMR(ctx->protocol)) {
        watchdog_event_current_build_event_dpmr(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_EDACS(ctx->protocol)) {
        watchdog_event_current_build_event_edacs(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_DMR(ctx->protocol)) {
        watchdog_event_current_build_event_dmr(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_P25(ctx->protocol)) {
        watchdog_event_current_build_event_p25(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_X2TDMA(ctx->protocol)) {
        watchdog_event_current_build_event_x2tdma(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_NXDN(ctx->protocol)) {
        watchdog_event_current_build_event_nxdn(ctx, datestr, timestr, sys_string, event_string, event_size);
    }
}

static void
watchdog_event_current_append_policy_labels(const watchdog_event_current_ctx* ctx, char* event_string,
                                            size_t event_size) {
    if (ctx->t_name_loaded) {
        char group[420];
        DSD_SNPRINTF(group, sizeof(group), "TName: %s; Mode: %s; ", ctx->t_name, ctx->t_mode);
        watchdog_event_str_append(event_string, event_size, group);
    }

    if (ctx->s_name_loaded) {
        char private[420];
        DSD_SNPRINTF(private, sizeof(private), "SName: %s; Mode: %s; ", ctx->s_name, ctx->s_mode);
        watchdog_event_str_append(event_string, event_size, private);
    }
}

// Recover the render kind a row was built from. watchdog_event_current_update_item() folds kind
// down to gi, and the per-protocol builders only ever test for group or private, so anything
// else maps back to plain voice.
static dsd_call_kind
watchdog_event_row_kind(const Event_History* item) {
    if (item->gi == 0) {
        return DSD_CALL_KIND_GROUP_VOICE;
    }
    if (item->gi == 1) {
        return DSD_CALL_KIND_PRIVATE_VOICE;
    }
    return DSD_CALL_KIND_VOICE;
}

// Rebuild the render context from a history row plus the environment captured when that row was
// committed. Used when a reacquired segment merges into a row already in history: the row's
// fields have just gained information and its event_string has to say so. Nothing is read from
// live decoder state, so a retune or a manufacturer-id change between the commit and the merge
// cannot rewrite the row with a context the call never ran under.
static void
watchdog_event_ctx_from_row(const dsd_call_event_render_env* env, const Event_History* item,
                            watchdog_event_current_ctx* ctx) {
    DSD_MEMSET(ctx, 0, sizeof(*ctx));
    ctx->severity = (dsd_event_severity)item->severity;
    ctx->category = (dsd_event_category)item->category;
    // Synctype ids are stored signed and negative sentinels are meaningful, so the widening
    // must sign-extend; the cast is explicit to say so.
    ctx->protocol = (int)item->systype;
    ctx->kind = watchdog_event_row_kind(item);
    ctx->subtype = (uint8_t)item->subtype;
    ctx->env = *env;
    ctx->source_id = item->source_id;
    ctx->target_id = item->target_id;
    ctx->svc_opts = item->svc;
    ctx->enc = item->enc;
    // A row persists only the derived flag, so the classification is rebuilt from it rather than
    // left at UNKNOWN. The P25 builder tests crypto and enc together; leaving crypto zero made
    // its first two disjuncts dead on every merged row, so any future classification that set
    // crypto without setting enc would have dropped the encryption marker from a merged row while
    // the unmerged path still showed it.
    ctx->crypto = item->enc ? DSD_CALL_CRYPTO_ENCRYPTED : DSD_CALL_CRYPTO_UNKNOWN;
    ctx->alg_id = item->enc_alg;
    ctx->key_id = item->enc_key;
    ctx->mi = item->mi;
    ctx->channel = item->channel;
    ctx->sys_id1 = item->sys_id1;
    ctx->sys_id2 = item->sys_id2;
    ctx->sys_id3 = item->sys_id3;
    ctx->sys_id4 = item->sys_id4;
    ctx->sys_id5 = item->sys_id5;
    DSD_SNPRINTF(ctx->sysid_string, sizeof(ctx->sysid_string), "%s", item->sysid_string);
    DSD_SNPRINTF(ctx->src_str, sizeof(ctx->src_str), "%s", item->src_str);
    DSD_SNPRINTF(ctx->tgt_str, sizeof(ctx->tgt_str), "%s", item->tgt_str);
    DSD_SNPRINTF(ctx->t_name, sizeof(ctx->t_name), "%s", item->t_name);
    DSD_SNPRINTF(ctx->s_name, sizeof(ctx->s_name), "%s", item->s_name);
    DSD_SNPRINTF(ctx->t_mode, sizeof(ctx->t_mode), "%s", item->t_mode);
    DSD_SNPRINTF(ctx->s_mode, sizeof(ctx->s_mode), "%s", item->s_mode);
    ctx->t_name_loaded = item->t_name[0] != '\0' ? 1U : 0U;
    ctx->s_name_loaded = item->s_name[0] != '\0' ? 1U : 0U;
}

static int
watchdog_event_char_is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Recover the "YYYY-MM-DD HH:MM:SS " prefix every builder emits, straight from the string the row
// is already displaying, and only fall back to item->event_time when that string carries no
// parseable prefix -- a row a protocol staged directly rather than rendering.
//
// The displayed string is preferred rather than the stamp because the two are not always the same
// clock. Every builder renders the prefix from time(NULL), but watchdog_event_current_update_item()
// only stamps event_time when opts->playfiles == 0; under --playfiles over an sdrtrunk recording it
// is left as the recording's own timestamp instead (dsd_file.c). Formatting the stamp there would
// rewrite a committed row's visible date and time to a value none of its siblings use, purely
// because it was merged -- and would make the re-render report a change on every merge. Stamped or
// not, a merge must never restamp the row. Returns non-zero on success.
static int
watchdog_event_row_datetime(const Event_History* item, char* datestr, size_t datestr_size, char* timestr,
                            size_t timestr_size) {
    // "YYYY-MM-DD HH:MM:SS": separators at offsets 4, 7, 10, 13, 16; digits everywhere else.
    static const char k_watchdog_event_datetime_separators[19] = {0,   0, 0, 0,   '-', 0, 0,   '-', 0, 0,
                                                                  ' ', 0, 0, ':', 0,   0, ':', 0,   0};
    const char* s = item->event_string;

    // Field widths of the prefix, including the terminator each is written with -- not the caller's
    // buffer capacities. The size parameters below only reject a buffer too small to hold a field;
    // widening one must not copy more of the timestamp than the field itself.
    enum { k_datestr_len = 11U, k_timestr_len = 9U };

    if (datestr_size < k_datestr_len || timestr_size < k_timestr_len) {
        return 0;
    }
    if (strnlen(s, sizeof(item->event_string)) >= 19U) {
        int parsed = 1;
        for (size_t i = 0; i < 19U; i++) {
            if (k_watchdog_event_datetime_separators[i] != 0 ? s[i] != k_watchdog_event_datetime_separators[i]
                                                             : !watchdog_event_char_is_digit(s[i])) {
                parsed = 0;
                break;
            }
        }
        if (parsed) {
            DSD_SNPRINTF(datestr, 11U, "%s", s);
            DSD_SNPRINTF(timestr, 9U, "%s", s + 11);
            return 1;
        }
    }

    // No prefix to preserve. A stamp is better than nothing; a zero one would render 1970-01-01,
    // so leave the row alone instead.
    if (item->event_time > 0) {
        (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_TIME_COLON, timestr, timestr_size);
        (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_DATE_HYPHEN, datestr, datestr_size);
        return 1;
    }
    return 0;
}

static int
watchdog_event_rerender_row(const dsd_call_event_render_env* env, Event_History* item, int* changed) {
    char timestr[9];
    char datestr[11];
    if (changed != NULL) {
        *changed = 0;
    }
    // The row's own timestamp, not the merge instant: it still describes the transmission
    // that opened at the first commit. Without a trustworthy one, leave the row as it stands
    // rather than restamping it with an invented time.
    if (!watchdog_event_row_datetime(item, datestr, sizeof datestr, timestr, sizeof timestr)) {
        return 0;
    }

    watchdog_event_current_ctx ctx;
    watchdog_event_ctx_from_row(env, item, &ctx);

    char event_string[2000];
    DSD_MEMSET(event_string, 0, sizeof(event_string));
    watchdog_event_current_build_event_string(&ctx, datestr, timestr, dsd_synctype_to_string(ctx.protocol),
                                              event_string, sizeof(event_string));
    watchdog_event_current_append_policy_labels(&ctx, event_string, sizeof(event_string));
    if (event_string[0] == '\0') {
        return 0;
    }
    // Compared here, against the buffer that already exists, so the caller does not have to keep
    // a second full-size copy of the row's string alive across the merge.
    if (changed != NULL) {
        *changed = strncmp(item->event_string, event_string, sizeof(item->event_string)) != 0;
    }
    DSD_SNPRINTF(item->event_string, sizeof(item->event_string), "%s", event_string);
    return 1;
}

static int
watchdog_event_current_args_valid(const dsd_opts* opts, const dsd_state* state, uint8_t slot) {
    return opts != NULL && state != NULL && state->event_history_s != NULL && slot <= 1U;
}

static int
watchdog_event_current_skip_ended(const dsd_call_snapshot* call, const dsd_call_event_lifecycle* lifecycle) {
    if (!call || !lifecycle || call->phase != DSD_CALL_PHASE_ENDED) {
        return 0;
    }
    return lifecycle->epoch == call->epoch && lifecycle->ended_committed;
}

static void
watchdog_event_commit_candidate(Event_History_I* event_struct, const Event_History* candidate) {
    // The candidate starts as an exact byte copy of the current row, so padding bytes remain identical.
    // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison,cert-exp42-c,cert-flp37-c)
    if (memcmp(candidate, &event_struct->Event_History_Items[0], sizeof(*candidate)) != 0) {
        DSD_MEMCPY(&event_struct->Event_History_Items[0], candidate, sizeof(*candidate));
        dsd_event_history_mark_dirty(event_struct);
    }
}

static void
watchdog_event_finalize_ended(const dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                              dsd_call_event_lifecycle* lifecycle, Event_History_I* event_struct, int finalize_ended) {
    if (!finalize_ended || !call || !lifecycle) {
        return;
    }
    if (call->phase != DSD_CALL_PHASE_ENDED || lifecycle->epoch != call->epoch || lifecycle->ended_committed) {
        return;
    }
    // A recoverable end -- a sync loss, or a terminator whose link control failed verification
    // -- may be the middle of a transmission rather than its end, so its VOICE_END alert is
    // held until the reacquisition window closes. Each further segment re-arms it, and the
    // alert lands once the call really has stopped flapping; a corroborating terminator repeat
    // tightens the reason to EXPLICIT, which releases the hold at the next sync pass. A
    // verified terminator is unambiguous and alerts immediately.
    //
    // This describes what happened on the air, so it is derived from the air alone. Whether the
    // operator wants to hear the result is a separate question, answered where the beep is
    // actually emitted -- folding the alert setting in here would make a disposition that other
    // side effects hang off silently change with an unrelated preference.
    const int deferred_end = (call->end_reason == (uint8_t)DSD_CALL_END_SYNC_LOSS
                              || call->end_reason == (uint8_t)DSD_CALL_END_UNVERIFIED_TERMINATOR)
                             && call->kind != DSD_CALL_KIND_DATA;
    const dsd_event_end_disposition disposition = deferred_end ? DSD_EVENT_END_DEFERRED : DSD_EVENT_END_FINAL;
    if (watchdog_event_item_has_content(&event_struct->Event_History_Items[0])) {
        // A new row, a merge into the row the interrupted transmission already owns, or -- for
        // a voice epoch that never learned an identity -- a drop. The empty-staged-row branch
        // has nothing to commit; enrichment resolves that case by push sequence and declines.
        const int committed = watchdog_event_commit_staged_row((dsd_opts*)opts, state, event_struct, slot, lifecycle,
                                                               call->kind == DSD_CALL_KIND_DATA, 1, disposition);
        if (deferred_end && committed) {
            // Armed only where a row reached history, which is what makes this the same rule the
            // FINAL disposition follows: its alert is emitted inside the commit above, so it too
            // cannot fire for a staged row with nothing in it. A signal that renders no event
            // string -- one never classified into a protocol with a builder -- would otherwise beep
            // the end of a transmission the operator never saw.
            lifecycle->end_alert_pending = 1U;
            // Deliberately the local monotonic clock rather than call->ended_m: the deadline is
            // only ever compared against this same clock, and ended_m carries whatever timeline
            // the caller supplied. In production the two coincide; keeping both endpoints on one
            // clock means a caller-supplied timeline can never make the alert fire early.
            lifecycle->end_alert_due_m = dsd_time_now_monotonic_s() + DSD_CALL_REACQUIRE_GAP_S;
        }
    } else {
        init_event_history(event_struct, 0, 1);
    }
    lifecycle->ended_committed = 1U;
}

static void
watchdog_event_current_impl(const dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                            dsd_call_event_lifecycle* lifecycle, int finalize_ended) {
    if (!watchdog_event_current_args_valid(opts, state, slot)) {
        return;
    }

    const int authoritative = watchdog_event_call_is_authoritative(call, lifecycle);
    if (!authoritative) {
        return;
    }
    if (watchdog_event_current_skip_ended(call, lifecycle)) {
        return;
    }
    const dsd_call_snapshot* effective_call = call;

    Event_History_I* event_struct = &state->event_history_s[slot];
    Event_History candidate;
    DSD_MEMCPY(&candidate, &event_struct->Event_History_Items[0], sizeof(candidate));

    watchdog_event_current_ctx ctx;
    watchdog_event_current_init_base(state, slot, effective_call, &ctx);

    watchdog_event_current_apply_protocol_metadata(state, &candidate, &ctx);
    watchdog_event_current_load_labels(state, &ctx);

    const char* sys_string = dsd_synctype_to_string(ctx.protocol);

    char timestr[9];
    char datestr[11];
    time_t now = time(NULL);
    (void)dsd_format_local_datetime(now, DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(now, DSD_LOCAL_DATETIME_DATE_HYPHEN, datestr, sizeof datestr);

    watchdog_event_current_update_item(opts, state, slot, &candidate, &ctx, now);

    char event_string[2000];
    DSD_MEMSET(event_string, 0, sizeof(event_string));
    watchdog_event_current_build_event_string(&ctx, datestr, timestr, sys_string, event_string, sizeof(event_string));
    watchdog_event_current_append_policy_labels(&ctx, event_string, sizeof(event_string));

    DSD_SNPRINTF(candidate.event_string, sizeof(candidate.event_string), "%s", event_string);

    watchdog_event_commit_candidate(event_struct, &candidate);
    // The staged row, the decoder inputs that produced it, and the epoch verdicts the
    // keep-or-drop decision reads are captured together, so whenever that row is committed --
    // now, or on a later pass once the epoch has already changed and the snapshot is gone -- it
    // carries the environment and the verdicts it was actually rendered under.
    if (lifecycle != NULL) {
        watchdog_event_capture_render_env(state, slot, effective_call, &lifecycle->staged_env);
    }
    watchdog_event_finalize_ended(opts, state, slot, effective_call, lifecycle, event_struct, finalize_ended);

    /* stack buffers; no free */
}

void
watchdog_event_current(const dsd_opts* opts, dsd_state* state, uint8_t slot) {
    if (!opts || !state || !state->event_history_s || slot > 1U) {
        return;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return;
    }
    dsd_call_state_ext_lock(ext);
    watchdog_event_current_impl(opts, state, slot, &ext->calls.slots[slot], &ext->events[slot], 1);
    dsd_call_state_ext_unlock(ext);
}

void
dsd_event_sync_slot(dsd_opts* opts, dsd_state* state, uint8_t slot) {
    if (!opts || !state || !state->event_history_s || slot > 1U) {
        return;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return;
    }
    dsd_call_state_ext_lock(ext);
    const dsd_call_snapshot* call = &ext->calls.slots[slot];
    dsd_call_event_lifecycle* lifecycle = &ext->events[slot];
    watchdog_event_history_impl(opts, state, slot, call, lifecycle);
    watchdog_event_current_impl(opts, state, slot, call, lifecycle, 1);
    // This runs on every frame-sync pass, so it is also where a VOICE_END alert held open across
    // a possible reacquisition is finally emitted once that window has closed with no resumption.
    // A terminator that decoded after the sync-loss end retracts the reacquisition permission by
    // tightening the reason to EXPLICIT; the hold has nothing left to wait for, so it retires now
    // rather than a further half second later. Read from canonical state rather than signalled
    // from it: call_state.c does not know the event layer exists.
    watchdog_event_flush_pending_end_alert(opts, state, slot, lifecycle,
                                           watchdog_event_end_is_positively_terminated(call, lifecycle));
    dsd_call_state_ext_unlock(ext);
}

void
dsd_event_flush_pending_alerts(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || !state->event_history_s) {
        return;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return;
    }
    dsd_call_state_ext_lock(ext);
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        watchdog_event_flush_pending_end_alert(opts, state, (uint8_t)slot, &ext->events[slot], 1);
    }
    dsd_call_state_ext_unlock(ext);
}

static void
watchdog_event_lock_if_present(const dsd_call_state_ext* ext) {
    if (ext) {
        dsd_call_state_ext_lock(ext);
    }
}

static void
watchdog_event_unlock_if_present(const dsd_call_state_ext* ext) {
    if (ext) {
        dsd_call_state_ext_unlock(ext);
    }
}

static uint32_t
watchdog_event_notice_target(const dsd_call_snapshot* call) {
    return watchdog_event_call_target_id(call);
}

static int
watchdog_event_notice_already_handled(const dsd_call_event_lifecycle* lifecycle, const dsd_call_snapshot* call) {
    return lifecycle != NULL && lifecycle->notice_handled && lifecycle->notice_epoch == call->epoch
           && lifecycle->notice_target_id == watchdog_event_notice_target(call)
           && lifecycle->notice_kind == (uint8_t)call->kind;
}

static int
watchdog_event_notice_matches_canonical(const dsd_call_snapshot* canonical, const dsd_call_snapshot* call) {
    return canonical != NULL && canonical->epoch == call->epoch && canonical->phase == call->phase
           && canonical->kind == call->kind
           && watchdog_event_notice_target(canonical) == watchdog_event_notice_target(call);
}

static void
watchdog_event_notice_mark_handled(dsd_call_event_lifecycle* lifecycle, const dsd_call_snapshot* call) {
    if (lifecycle == NULL) {
        return;
    }
    lifecycle->notice_epoch = call->epoch;
    lifecycle->notice_target_id = watchdog_event_notice_target(call);
    lifecycle->notice_kind = (uint8_t)call->kind;
    lifecycle->notice_handled = 1U;
}

static int
watchdog_event_notice_matches_history(const Event_History_I* event_struct, const dsd_call_snapshot* call,
                                      const char* detail) {
    if (event_struct == NULL || call == NULL || detail == NULL) {
        return 0;
    }
    const Event_History* previous = &event_struct->Event_History_Items[1];
    const int expected_gi = call->kind == DSD_CALL_KIND_GROUP_VOICE     ? 0
                            : call->kind == DSD_CALL_KIND_PRIVATE_VOICE ? 1
                                                                        : previous->gi;
    return previous->target_id == watchdog_event_notice_target(call) && previous->gi == expected_gi
           && strncmp(previous->internal_str, detail, sizeof(previous->internal_str)) == 0;
}

static int
watchdog_event_notice_already_committed(const dsd_call_state_ext* ext, const dsd_call_event_lifecycle* lifecycle,
                                        const Event_History_I* event_struct, const dsd_call_snapshot* call,
                                        const char* detail) {
    if (watchdog_event_notice_already_handled(lifecycle, call)) {
        return 1;
    }
    return ext == NULL && watchdog_event_notice_matches_history(event_struct, call, detail);
}

static int
dsd_event_emit_call_notice_impl(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                                const char* detail, int finalize_call) {
    if (!opts || !state || !state->event_history_s || !call || call->epoch == 0U || !detail || slot > 1U) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    watchdog_event_lock_if_present(ext);
    dsd_call_event_lifecycle* lifecycle = ext ? &ext->events[slot] : NULL;
    Event_History_I* event_struct = &state->event_history_s[slot];
    if (watchdog_event_notice_already_committed(ext, lifecycle, event_struct, call, detail)) {
        watchdog_event_unlock_if_present(ext);
        return 0;
    }

    dsd_call_event_lifecycle* canonical_lifecycle = NULL;
    if (ext != NULL && watchdog_event_notice_matches_canonical(&ext->calls.slots[slot], call)) {
        canonical_lifecycle = lifecycle;
        watchdog_event_history_authoritative(opts, state, slot, call, canonical_lifecycle);
    }
    watchdog_event_current_impl(opts, state, slot, call, canonical_lifecycle, 0);
    DSD_SNPRINTF(event_struct->Event_History_Items[0].internal_str,
                 sizeof(event_struct->Event_History_Items[0].internal_str), "%s", detail);
    dsd_event_history_mark_dirty(event_struct);
    // Routed through the commit path rather than pushing directly: a notice raised during a
    // reacquired segment -- P25 encryption first detected after the gap, say -- describes the
    // transmission that is already in history, so it has to fold into that row. Pushing here
    // unconditionally would give one transmission two rows and leave the first one orphaned.
    // The commit path keeps the notice detail: internal_str is merged progressively, so the
    // detail that just fired supersedes whatever the row carried.
    (void)watchdog_event_commit_staged_row(opts, state, event_struct, slot, canonical_lifecycle,
                                           call->kind == DSD_CALL_KIND_DATA, finalize_call,
                                           finalize_call ? DSD_EVENT_END_FINAL : DSD_EVENT_END_NONE);
    watchdog_event_notice_mark_handled(lifecycle, call);
    if (canonical_lifecycle != NULL) {
        // committed_seq/committed_epoch are maintained by the commit path itself; only the
        // end-of-epoch marker is this function's to set.
        if (call->phase == DSD_CALL_PHASE_ENDED) {
            canonical_lifecycle->ended_committed = 1U;
        }
    }
    watchdog_event_unlock_if_present(ext);
    return 1;
}

int
dsd_event_emit_call_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                           const char* detail) {
    return dsd_event_emit_call_notice_impl(opts, state, slot, call, detail, 1);
}

int
dsd_event_emit_call_notice_nonfinalizing(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_snapshot* call,
                                         const char* detail) {
    return dsd_event_emit_call_notice_impl(opts, state, slot, call, detail, 0);
}

typedef enum {
    DSD_EVENT_ENRICH_ALIAS,
    DSD_EVENT_ENRICH_GPS,
    DSD_EVENT_ENRICH_TEXT,
} dsd_event_enrichment_kind;

static int
dsd_event_enrich_epoch(dsd_state* state, uint8_t slot, uint64_t epoch, const char* value,
                       dsd_event_enrichment_kind kind) {
    if (!state || !state->event_history_s || slot >= DSD_CALL_STATE_SLOT_COUNT || epoch == 0U || !value) {
        return -1;
    }
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    if (!ext) {
        return 0;
    }
    dsd_call_state_ext_lock(ext);
    const dsd_call_snapshot* call = &ext->calls.slots[slot];
    const dsd_call_event_lifecycle* lifecycle = &ext->events[slot];
    if (call->epoch != epoch) {
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    // Once this epoch's row has been committed the row has to be located by push sequence: an
    // interleaved data or system notice pushes it deeper than index 1. The test is on
    // committed_epoch rather than the lifecycle's current epoch, so an epoch that never pushed a
    // row of its own cannot enrich an older epoch's row that committed_seq still points at.
    uint8_t history_index = 0U;
    if (lifecycle->committed_valid && lifecycle->committed_epoch == epoch) {
        history_index = watchdog_event_committed_row_index(&state->event_history_s[slot], lifecycle);
        if (history_index == 0U) {
            dsd_call_state_ext_unlock(ext);
            return 0;
        }
    } else if (lifecycle->epoch == epoch && lifecycle->ended_committed) {
        // Committed but not locatable (the row aged out of the ring, or a context restore
        // invalidated the reference). Enriching row 0 would write into an unrelated staged row.
        dsd_call_state_ext_unlock(ext);
        return 0;
    }
    Event_History* item = &state->event_history_s[slot].Event_History_Items[history_index];
    if (kind == DSD_EVENT_ENRICH_ALIAS) {
        DSD_SNPRINTF(item->alias, sizeof(item->alias), "%s", value);
        DSD_SNPRINTF(state->generic_talker_alias[slot], sizeof(state->generic_talker_alias[slot]), "%s", value);
    } else if (kind == DSD_EVENT_ENRICH_GPS) {
        DSD_SNPRINTF(item->gps_s, sizeof(item->gps_s), "%s", value);
    } else {
        DSD_SNPRINTF(item->text_message, sizeof(item->text_message), "%s", value);
    }
    dsd_event_history_mark_dirty(&state->event_history_s[slot]);
    dsd_call_state_ext_unlock(ext);
    return 1;
}

int
dsd_event_enrich_alias(dsd_state* state, uint8_t slot, uint64_t epoch, const char* alias) {
    return dsd_event_enrich_epoch(state, slot, epoch, alias, DSD_EVENT_ENRICH_ALIAS);
}

int
dsd_event_enrich_gps(dsd_state* state, uint8_t slot, uint64_t epoch, const char* gps) {
    return dsd_event_enrich_epoch(state, slot, epoch, gps, DSD_EVENT_ENRICH_GPS);
}

int
dsd_event_enrich_text(dsd_state* state, uint8_t slot, uint64_t epoch, const char* text) {
    return dsd_event_enrich_epoch(state, slot, epoch, text, DSD_EVENT_ENRICH_TEXT);
}

void
dsd_event_history_reset(dsd_state* state) {
    if (state == NULL || state->event_history_s == NULL) {
        return;
    }
    (void)dsd_call_state_ensure(state);
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    // The transaction already holds the call-state mutex that guards ext->events, so the rows
    // and the bookkeeping that points into them are cleared together.
    dsd_call_state_ext* ext = dsd_call_state_ext_get(state, 0);
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        init_event_history(&state->event_history_s[slot], 0, 255);
        if (ext != NULL) {
            // epoch and ended_committed are deliberately left alone. They say which call the slot
            // has already finished rendering, not which row it landed in: clearing them would
            // make an ended-but-still-retained call look unrendered, and the next sync pass would
            // commit it all over again -- resurrecting, in a freshly cleared history, the very row
            // the operator just deleted.
            dsd_call_state_invalidate_event_lifecycle(&ext->events[slot]);
        }
    }
    dsd_event_history_transaction_end(&transaction);
}

int
dsd_event_history_copy_snapshot(const dsd_state* state, Event_History_I out[2]) {
    if (!state || !out) {
        return -1;
    }
    if (!state->event_history_s) {
        DSD_MEMSET(out, 0, sizeof(Event_History_I) * 2U);
        return 0;
    }
    const dsd_call_state_ext* ext = dsd_call_state_ext_peek(state);
    if (ext) {
        dsd_call_state_ext_lock(ext);
    }
    DSD_MEMCPY(out, state->event_history_s, sizeof(Event_History_I) * 2U);
    if (ext) {
        dsd_call_state_ext_unlock(ext);
    }
    return 1;
}

static int
dsd_event_history_copy_incremental_locked(const dsd_state* src, Event_History_I event_history[2],
                                          const uint64_t source_revisions[2], int force_copy, uint8_t copied[2]) {
    if (src->event_history_s == NULL) {
        DSD_MEMSET(event_history, 0, sizeof(Event_History_I) * 2U);
        return 0;
    }
    for (size_t slot = 0; slot < 2U; slot++) {
        const uint64_t revision = src->event_history_s[slot].revision;
        if (force_copy || source_revisions == NULL || source_revisions[slot] != revision) {
            DSD_MEMCPY(&event_history[slot], &src->event_history_s[slot], sizeof(Event_History_I));
            copied[slot] = 1U;
        }
    }
    return 1;
}

int
dsd_event_state_copy_snapshot_incremental(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2],
                                          const uint64_t source_revisions[2], int force_copy, uint8_t copied[2]) {
    if (!dst || !src || !event_history || !copied) {
        return -1;
    }
    copied[0] = 0U;
    copied[1] = 0U;
    const dsd_call_state_ext* src_ext = dsd_call_state_ext_peek(src);
    if (dst == src) {
        if (src_ext) {
            dsd_call_state_ext_lock(src_ext);
        }
        const int copied_history =
            dsd_event_history_copy_incremental_locked(src, event_history, source_revisions, force_copy, copied);
        if (src_ext) {
            dsd_call_state_ext_unlock(src_ext);
        }
        return copied_history;
    }
    dsd_call_state_ext* dst_ext = NULL;
    if (src_ext) {
        dst_ext = dsd_call_state_ext_get(dst, 1);
        if (!dst_ext) {
            (void)dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_CALL_STATE, NULL, NULL);
        }
        dsd_call_state_ext_lock(src_ext);
        if (dst_ext) {
            dsd_call_state_ext_lock(dst_ext);
            dst_ext->calls = src_ext->calls;
            dst_ext->recent = src_ext->recent;
            DSD_MEMCPY(dst_ext->events, src_ext->events, sizeof(dst_ext->events));
            DSD_MEMCPY(dst_ext->epoch_sequence, src_ext->epoch_sequence, sizeof(dst_ext->epoch_sequence));
        }
    } else {
        (void)dsd_state_ext_set(dst, DSD_STATE_EXT_CORE_CALL_STATE, NULL, NULL);
    }

    const int copied_history =
        dsd_event_history_copy_incremental_locked(src, event_history, source_revisions, force_copy, copied);
    if (dst_ext) {
        dsd_call_state_ext_unlock(dst_ext);
    }
    if (src_ext) {
        dsd_call_state_ext_unlock(src_ext);
    }
    return dst_ext || !src_ext ? copied_history : -1;
}

int
dsd_event_state_copy_snapshot(dsd_state* dst, const dsd_state* src, Event_History_I event_history[2]) {
    uint8_t copied[2];
    return dsd_event_state_copy_snapshot_incremental(dst, src, event_history, NULL, 1, copied);
}

void
watchdog_event_status(dsd_state* state, const char* status_string, uint8_t slot) {
    if (state == NULL || state->event_history_s == NULL || status_string == NULL || slot >= 2) {
        return;
    }

    (void)dsd_call_state_ensure(state);
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    Event_History_I* event_struct = &state->event_history_s[slot];
    init_event_history(event_struct, 0, 1);

    Event_History* item = &event_struct->Event_History_Items[0];
    item->write = 0;
    dsd_event_history_item_set_metadata(item, DSD_EVENT_SEVERITY_INFO, DSD_EVENT_CATEGORY_STATUS);
    item->systype = -1;
    item->subtype = -1;
    item->source_id = 0;
    item->target_id = 0;

    time_t now = time(NULL);
    item->event_time = now;

    char timestr[9];
    char datestr[11];
    (void)dsd_format_local_datetime(now, DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(now, DSD_LOCAL_DATETIME_DATE_HYPHEN, datestr, sizeof datestr);

    DSD_SNPRINTF(item->event_string, sizeof item->event_string, "%s %s %s", datestr, timestr, status_string);
    dsd_event_history_mark_dirty(event_struct);
    dsd_event_history_transaction_end(&transaction);
}

static void
dsd_event_copy_data_payload(Event_History* dst, const Event_History* src) {
    DSD_MEMCPY(dst->pdu, src->pdu, sizeof(dst->pdu));
    DSD_SNPRINTF(dst->gps_s, sizeof(dst->gps_s), "%s", src->gps_s);
    DSD_SNPRINTF(dst->text_message, sizeof(dst->text_message), "%s", src->text_message);
}

static void
dsd_event_clear_data_payload(Event_History* item) {
    DSD_MEMSET(item->pdu, 0, sizeof(item->pdu));
    item->gps_s[0] = '\0';
    item->text_message[0] = '\0';
}

static int
dsd_event_data_notice_args_valid(const dsd_opts* opts, const dsd_state* state, uint8_t slot,
                                 const dsd_call_observation* observation, dsd_event_category category,
                                 const char* notice) {
    if (opts == NULL || state == NULL || state->event_history_s == NULL || observation == NULL || notice == NULL
        || slot > 1U || observation->slot != slot || observation->kind != DSD_CALL_KIND_DATA) {
        return 0;
    }
    return category == DSD_EVENT_CATEGORY_DATA || category == DSD_EVENT_CATEGORY_CONTROL;
}

static int
dsd_event_emit_data_notice_impl(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                                dsd_event_category category, const char* notice, const char* gps,
                                int consume_staged_payload) {
    if (!dsd_event_data_notice_args_valid(opts, state, slot, observation, category, notice)) {
        return -1;
    }

    (void)dsd_call_state_ensure(state);
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    Event_History_I* event_struct = &state->event_history_s[slot];
    Event_History active;
    DSD_MEMCPY(&active, &event_struct->Event_History_Items[0], sizeof(active));
    init_event_history(event_struct, 0, 1);

    Event_History* item = &event_struct->Event_History_Items[0];
    item->write = 1;
    dsd_event_history_item_set_metadata(item, DSD_EVENT_SEVERITY_INFO, category);
    item->systype = observation->protocol;
    item->subtype = DSD_EVENT_SUBTYPE_EXPLICIT_DATA;
    item->gi = -1;
    item->source_id = observation->ota_source_id <= UINT32_MAX ? (uint32_t)observation->ota_source_id : 0U;
    item->target_id = observation->ota_target_id <= UINT32_MAX ? (uint32_t)observation->ota_target_id : 0U;
    item->channel = observation->channel;
    item->event_time = time(NULL);
    DSD_SNPRINTF(item->src_str, sizeof(item->src_str), "%s", observation->source_text);
    DSD_SNPRINTF(item->tgt_str, sizeof(item->tgt_str), "%s", observation->target_text);
    if (consume_staged_payload) {
        dsd_event_copy_data_payload(item, &active);
        dsd_event_clear_data_payload(&active);
    } else {
        DSD_SNPRINTF(item->gps_s, sizeof(item->gps_s), "%s", gps);
    }

    char timestr[9];
    char datestr[11];
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_DATE_HYPHEN, datestr, sizeof datestr);
    DSD_SNPRINTF(item->event_string, sizeof(item->event_string), "%s %s %s", datestr, timestr, notice);

    write_event_to_log_file(opts, state, slot, 0U, item->event_string);
    push_event_history(event_struct);
    DSD_MEMCPY(&event_struct->Event_History_Items[0], &active, sizeof(active));
    dsd_event_history_mark_dirty(event_struct);
    dsd_event_history_transaction_end(&transaction);

    dsd_frame_logf(opts, "FRAME DATA slot=%d src=%llu dst=%llu %s", slot + 1,
                   (unsigned long long)observation->ota_source_id, (unsigned long long)observation->ota_target_id,
                   notice);

    if (dsd_call_alert_event_enabled(opts->call_alert, opts->call_alert_events, DSD_CALL_ALERT_EVENT_DATA)) {
        beeper(opts, state, slot, 80, 20, 3);
    }
    return 0;
}

int
dsd_event_emit_data_notice_classified(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                      const dsd_call_observation* observation, dsd_event_category category,
                                      const char* notice) {
    return dsd_event_emit_data_notice_impl(opts, state, slot, observation, category, notice, NULL, 1);
}

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    return dsd_event_emit_data_notice_classified(opts, state, slot, observation, DSD_EVENT_CATEGORY_DATA, notice);
}

int
dsd_event_emit_data_notice_classified_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                               const dsd_call_observation* observation, dsd_event_category category,
                                               const char* notice, const char* gps) {
    if (gps == NULL) {
        return -1;
    }
    return dsd_event_emit_data_notice_impl(opts, state, slot, observation, category, notice, gps, 0);
}

int
dsd_event_emit_data_notice_with_gps(dsd_opts* opts, dsd_state* state, uint8_t slot,
                                    const dsd_call_observation* observation, const char* notice, const char* gps) {
    return dsd_event_emit_data_notice_classified_with_gps(opts, state, slot, observation, DSD_EVENT_CATEGORY_DATA,
                                                          notice, gps);
}

int
dsd_event_emit_system_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const char* notice) {
    if (opts == NULL || state == NULL || state->event_history_s == NULL || notice == NULL || slot > 1U) {
        return -1;
    }

    (void)dsd_call_state_ensure(state);
    dsd_event_history_transaction transaction;
    dsd_event_history_transaction_begin(state, &transaction);
    Event_History_I* event_struct = &state->event_history_s[slot];
    Event_History active;
    DSD_MEMCPY(&active, &event_struct->Event_History_Items[0], sizeof(active));
    init_event_history(event_struct, 0, 1);

    Event_History* item = &event_struct->Event_History_Items[0];
    item->write = 1;
    dsd_event_history_item_set_metadata(item, DSD_EVENT_SEVERITY_INFO, DSD_EVENT_CATEGORY_SYSTEM);
    item->systype = DSD_SYNC_NONE;
    item->subtype = -1;
    item->gi = -1;
    item->event_time = time(NULL);

    char timestr[9];
    char datestr[11];
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_DATE_HYPHEN, datestr, sizeof datestr);
    DSD_SNPRINTF(item->event_string, sizeof(item->event_string), "%s %s %s", datestr, timestr, notice);

    write_event_to_log_file(opts, state, slot, 0U, item->event_string);
    push_event_history(event_struct);
    DSD_MEMCPY(&event_struct->Event_History_Items[0], &active, sizeof(active));
    dsd_event_history_mark_dirty(event_struct);
    dsd_event_history_transaction_end(&transaction);

    dsd_frame_logf(opts, "FRAME SYSTEM slot=%d %s", slot + 1, notice);
    return 0;
}
