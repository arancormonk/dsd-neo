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
    dsd_event_history_mark_dirty(event_struct);
}

void
write_event_to_log_file(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t swrite,
                        char* event_string) //pass completed event string here that is in the struct
{

    //open log file
    FILE* event_log_file;
    event_log_file = dsd_fopen_private(opts->event_out_file, "a");

    if (event_log_file != NULL) {
        DSD_FPRINTF(event_log_file, "%s ", event_string);
        if (swrite == 1) {
            DSD_FPRINTF(event_log_file, "Slot %d; ", slot + 1);
        }
        DSD_FPRINTF(event_log_file, "\n");

        if (state->event_history_s[slot].Event_History_Items[0].text_message[0] != '\0') {
            DSD_FPRINTF(event_log_file, "%s \n", state->event_history_s[slot].Event_History_Items[0].text_message);
        }
        if (state->event_history_s[slot].Event_History_Items[0].alias[0] != '\0') {
            DSD_FPRINTF(event_log_file, " Talker Alias: %s \n",
                        state->event_history_s[slot].Event_History_Items[0].alias);
        }
        if (state->event_history_s[slot].Event_History_Items[0].gps_s[0] != '\0') {
            DSD_FPRINTF(event_log_file, " GPS: %s \n", state->event_history_s[slot].Event_History_Items[0].gps_s);
        }
        if (state->event_history_s[slot].Event_History_Items[0].internal_str[0] != '\0') {
            DSD_FPRINTF(event_log_file, " DSD-neo: %s \n",
                        state->event_history_s[slot].Event_History_Items[0].internal_str);
        }

        //flush and close log file
        fflush(event_log_file);
        fclose(event_log_file);
    }
}

static uint8_t
watchdog_event_should_write_slot(const dsd_state* state) {
    return (DSD_SYNC_IS_DMR_BS(state->lastsynctype) || DSD_SYNC_IS_P25P2(state->lastsynctype)) ? 1u : 0u;
}

static int
watchdog_event_item_has_content(const Event_History* item) {
    if (item == NULL) {
        return 0;
    }
    return item->event_string[0] != '\0' || item->text_message[0] != '\0' || item->alias[0] != '\0'
           || item->gps_s[0] != '\0' || item->internal_str[0] != '\0';
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

static void
watchdog_event_handle_source_transition_ex(dsd_opts* opts, dsd_state* state, Event_History_I* event_struct,
                                           uint8_t slot, uint8_t swrite, int last_event_is_data,
                                           int reset_slot_identity, int call_end_side_effects) {
    if (opts->event_out_file[0] != 0) {
        write_event_to_log_file(opts, state, slot, swrite, event_struct->Event_History_Items[0].event_string);
    }

    event_struct->Event_History_Items[0].write = 1;
    if (call_end_side_effects) {
        watchdog_event_rotate_wav_if_needed(opts, event_struct, slot);
    }
    push_event_history(event_struct);
    init_event_history(event_struct, 0, 1);
    (void)reset_slot_identity;
    watchdog_event_reset_post_push(state);
    if (call_end_side_effects) {
        watchdog_event_maybe_beep_call_end(opts, state, slot, last_event_is_data);
    }
}

static void
watchdog_event_handle_source_transition(dsd_opts* opts, dsd_state* state, Event_History_I* event_struct, uint8_t slot,
                                        uint8_t swrite, int last_event_is_data, int reset_slot_identity) {
    watchdog_event_handle_source_transition_ex(opts, state, event_struct, slot, swrite, last_event_is_data,
                                               reset_slot_identity, 1);
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
        const int is_data = watchdog_event_is_data_event(current);
        watchdog_event_handle_source_transition(opts, state, event_struct, slot,
                                                watchdog_event_should_write_slot(state), is_data, 0);
    } else if (has_content) {
        init_event_history(event_struct, 0, 1);
    }

    lifecycle->epoch = call->epoch;
    lifecycle->ended_committed = 0U;
    lifecycle->notice_epoch = 0U;
    lifecycle->notice_target_id = 0U;
    lifecycle->notice_kind = DSD_CALL_KIND_UNKNOWN;
    lifecycle->notice_handled = 0U;
    if (call->phase == DSD_CALL_PHASE_ACTIVE
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
    uint8_t mfid;
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
    ctx->mfid = slot == 0U ? state->dmr_fid : state->dmr_fidR;

    if (!call || call->epoch == 0U) {
        return;
    }
    ctx->protocol = call->protocol;
    ctx->kind = call->kind;
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
watchdog_event_current_build_event_edacs(const dsd_state* state, const watchdog_event_current_ctx* ctx,
                                         const char* datestr, const char* timestr, const char* sys_string,
                                         char* event_string, size_t event_size) {
    char sup_str[200];
    watchdog_event_build_edacs_sup_str(ctx->svc_opts, 0, sup_str, sizeof(sup_str));

    if (state->ea_mode == 1) {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %07d; SRC: %07d; LCN: %02d; SITE: %d:%d.%04X; %s;",
                     datestr, timestr, sys_string, ctx->target_id, ctx->source_id, ctx->channel, ctx->sys_id1,
                     ctx->sys_id2, ctx->sys_id3, sup_str);
        return;
    }

    int afs = (int)ctx->target_id;
    int a = (afs >> state->edacs_a_shift) & state->edacs_a_mask;
    int f = (afs >> state->edacs_f_shift) & state->edacs_f_mask;
    int s = afs & state->edacs_s_mask;
    char afs_str[8];
    getAfsString((dsd_state*)state, afs_str, a, f, s);

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

    if (ctx->kind == DSD_CALL_KIND_GROUP_VOICE) {
        watchdog_event_str_append(event_string, event_size, "Group; ");
    } else if (ctx->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        watchdog_event_str_append(event_string, event_size, "Private; ");
    }

    if (ctx->mfid == 0x10) {
        if (ctx->svc_opts & 0x30) {
            watchdog_event_str_append(event_string, event_size, "TXI; ");
        }

        if (ctx->svc_opts & 0x03) {
            watchdog_event_str_append(event_string, event_size, "PRIORITY; ");
        }
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

    if (ctx->alg_id != 0 && ctx->alg_id != 0x80) {
        char ess_str[30];
        DSD_SNPRINTF(ess_str, sizeof(ess_str), "ENC; ALG: %02X; KID: %04X; ", ctx->alg_id, ctx->key_id);
        watchdog_event_str_append(event_string, event_size, ess_str);
    } else if (ctx->crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING || ctx->crypto == DSD_CALL_CRYPTO_ENCRYPTED
               || ctx->enc) {
        watchdog_event_str_append(event_string, event_size, "ENC; ");
    }
    if (ctx->svc_opts & 0x80) {
        watchdog_event_str_append(event_string, event_size, "Emergency; ");
    }
    if (ctx->kind == DSD_CALL_KIND_GROUP_VOICE) {
        watchdog_event_str_append(event_string, event_size, "Group; ");
    } else if (ctx->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        watchdog_event_str_append(event_string, event_size, "Private; ");
    }
}

static void
watchdog_event_current_build_event_nxdn(const dsd_state* state, const watchdog_event_current_ctx* ctx,
                                        const char* datestr, const char* timestr, const char* sys_string,
                                        char* event_string, size_t event_size) {
    if (ctx->sys_id1) {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; RAN: %02d; SYS: %d.%d; ", datestr,
                     timestr, sys_string, ctx->target_id, ctx->source_id, ctx->sys_id3, ctx->sys_id2, ctx->sys_id1);
    } else {
        DSD_SNPRINTF(event_string, event_size, "%s %s %s TGT: %08d; SRC: %08d; RAN: %02d; ", datestr, timestr,
                     sys_string, ctx->target_id, ctx->source_id, ctx->sys_id3);
    }

    if (state->nxdn_grant_chan != 0) {
        char ch_str[96];
        if (state->nxdn_grant_freq != 0) {
            DSD_SNPRINTF(ch_str, sizeof(ch_str), "CH: %u; FREQ: %.6lf MHz; ", state->nxdn_grant_chan,
                         (double)state->nxdn_grant_freq / 1000000.0);
        } else {
            DSD_SNPRINTF(ch_str, sizeof(ch_str), "CH: %u; ", state->nxdn_grant_chan);
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

    if (ctx->kind == DSD_CALL_KIND_GROUP_VOICE) {
        watchdog_event_str_append(event_string, event_size, "Group; ");
    } else if (ctx->kind == DSD_CALL_KIND_PRIVATE_VOICE) {
        watchdog_event_str_append(event_string, event_size, "Private; ");
    }
}

static void
watchdog_event_current_build_event_string(const dsd_state* state, const watchdog_event_current_ctx* ctx,
                                          const char* datestr, const char* timestr, const char* sys_string,
                                          char* event_string, size_t event_size) {
    if (DSD_SYNC_IS_YSF(ctx->protocol) || DSD_SYNC_IS_DSTAR(ctx->protocol)) {
        watchdog_event_current_build_event_text_ids(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_M17(ctx->protocol)) {
        watchdog_event_current_build_event_m17(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_DPMR(ctx->protocol)) {
        watchdog_event_current_build_event_dpmr(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_EDACS(ctx->protocol)) {
        watchdog_event_current_build_event_edacs(state, ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_DMR(ctx->protocol)) {
        watchdog_event_current_build_event_dmr(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_P25(ctx->protocol)) {
        watchdog_event_current_build_event_p25(ctx, datestr, timestr, sys_string, event_string, event_size);
    } else if (DSD_SYNC_IS_NXDN(ctx->protocol)) {
        watchdog_event_current_build_event_nxdn(state, ctx, datestr, timestr, sys_string, event_string, event_size);
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
    if (watchdog_event_item_has_content(&event_struct->Event_History_Items[0])) {
        watchdog_event_handle_source_transition((dsd_opts*)opts, state, event_struct, slot,
                                                watchdog_event_should_write_slot(state),
                                                call->kind == DSD_CALL_KIND_DATA, 1);
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
    watchdog_event_current_build_event_string(state, &ctx, datestr, timestr, sys_string, event_string,
                                              sizeof(event_string));
    watchdog_event_current_append_policy_labels(&ctx, event_string, sizeof(event_string));

    DSD_SNPRINTF(candidate.event_string, sizeof(candidate.event_string), "%s", event_string);

    watchdog_event_commit_candidate(event_struct, &candidate);
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
    watchdog_event_handle_source_transition_ex(opts, state, event_struct, slot, watchdog_event_should_write_slot(state),
                                               call->kind == DSD_CALL_KIND_DATA, finalize_call, finalize_call);
    watchdog_event_notice_mark_handled(lifecycle, call);
    if (canonical_lifecycle != NULL && call->phase == DSD_CALL_PHASE_ENDED) {
        canonical_lifecycle->ended_committed = 1U;
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
    const uint8_t history_index = lifecycle->epoch == epoch && lifecycle->ended_committed ? 1U : 0U;
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

int
dsd_event_emit_data_notice(dsd_opts* opts, dsd_state* state, uint8_t slot, const dsd_call_observation* observation,
                           const char* notice) {
    if (opts == NULL || state == NULL || state->event_history_s == NULL || observation == NULL || notice == NULL
        || slot > 1U || observation->slot != slot || observation->kind != DSD_CALL_KIND_DATA) {
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
    dsd_event_history_item_set_metadata(item, DSD_EVENT_SEVERITY_INFO, DSD_EVENT_CATEGORY_DATA);
    item->systype = observation->protocol;
    item->subtype = DSD_EVENT_SUBTYPE_EXPLICIT_DATA;
    item->gi = -1;
    item->source_id = observation->ota_source_id <= UINT32_MAX ? (uint32_t)observation->ota_source_id : 0U;
    item->target_id = observation->ota_target_id <= UINT32_MAX ? (uint32_t)observation->ota_target_id : 0U;
    item->channel = observation->channel;
    item->event_time = time(NULL);
    DSD_SNPRINTF(item->src_str, sizeof(item->src_str), "%s", observation->source_text);
    DSD_SNPRINTF(item->tgt_str, sizeof(item->tgt_str), "%s", observation->target_text);
    dsd_event_copy_data_payload(item, &active);
    dsd_event_clear_data_payload(&active);

    char timestr[9];
    char datestr[11];
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_TIME_COLON, timestr, sizeof timestr);
    (void)dsd_format_local_datetime(item->event_time, DSD_LOCAL_DATETIME_DATE_HYPHEN, datestr, sizeof datestr);
    DSD_SNPRINTF(item->event_string, sizeof(item->event_string), "%s %s %s", datestr, timestr, notice);

    if (opts->event_out_file[0] != '\0') {
        write_event_to_log_file(opts, state, slot, 0U, item->event_string);
    }
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

    if (opts->event_out_file[0] != '\0') {
        write_event_to_log_file(opts, state, slot, 0U, item->event_string);
    }
    push_event_history(event_struct);
    DSD_MEMCPY(&event_struct->Event_History_Items[0], &active, sizeof(active));
    dsd_event_history_mark_dirty(event_struct);
    dsd_event_history_transaction_end(&transaction);

    dsd_frame_logf(opts, "FRAME SYSTEM slot=%d %s", slot + 1, notice);
    return 0;
}
