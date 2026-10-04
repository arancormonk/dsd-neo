// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/app_control/analog_width_view.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/scan_row_view.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

int
dsd_app_scan_row_label(const dsd_state* state, int scanner, int row, const char* target_id, char* out,
                       size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (scanner == DSD_SCAN_ROW_SCANNER_TRUNK_SCAN && target_id && target_id[0]) {
        DSD_SNPRINTF(out, out_size, "%s", target_id);
        return 0;
    }
    if (!state || row < 0 || row >= state->lcn_freq_count) {
        DSD_SNPRINTF(out, out_size, "%s", "this channel");
        return 0;
    }
    const char* name = dsd_state_trunk_lcn_name_get(state, (size_t)row);
    if (name && name[0]) {
        DSD_SNPRINTF(out, out_size, "%s", name);
        return 0;
    }
    const long freq = *dsd_state_trunk_lcn_slot_const(state, row);
    DSD_SNPRINTF(out, out_size, "Ch %d (%.6f MHz)", row + 1, (double)freq / 1000000.0);
    return 0;
}

int
dsd_app_scan_row_view_get(const dsd_opts* opts, const dsd_state* state, dsd_app_scan_row_view* out) {
    if (!out) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->row = -1;
    if (!opts || !state) {
        return -1;
    }
    const int scanner = (int)state->scan_row_scanner;
    /* The row belongs to the scanner running now: the trunk-scan coordinator, or the -Y scan. */
    const int running =
        (scanner == DSD_SCAN_ROW_SCANNER_TRUNK_SCAN && opts->trunk_scan_enabled == 1)
        || (scanner == DSD_SCAN_ROW_SCANNER_CHANNEL_SCAN && opts->scanner_mode == 1 && opts->trunk_scan_enabled != 1);
    if (!running || state->scan_row_session == 0U || state->scan_row_index < 0) {
        return 0;
    }
    out->active = 1;
    out->scanner = scanner;
    out->session = state->scan_row_session;
    out->row = (int)state->scan_row_index;
    out->mode = (unsigned int)dsd_scan_mode_row(state);
    out->editable = state->scan_row_editable;
    out->edited = state->scan_row_edited;
    out->listed = state->scan_row_listed;
    out->opts_match = opts->scan_row_scope_seq == state->scan_row_scope_seq;
    if (scanner == DSD_SCAN_ROW_SCANNER_TRUNK_SCAN) {
        DSD_SNPRINTF(out->target_id, sizeof out->target_id, "%s", state->trunk_scan_active_id);
    }
    (void)dsd_app_scan_row_label(state, scanner, out->row, out->target_id, out->label, sizeof out->label);
    return 0;
}

int
dsd_app_scan_row_view_offers(const dsd_app_scan_row_view* view, uint32_t field) {
    return view && view->active && view->opts_match && field != 0U && (view->editable & field) == field;
}

int
dsd_app_scan_row_view_payload(const dsd_app_scan_row_view* view, uint32_t field, int action,
                              dsd_app_scan_row_edit_payload* out) {
    if (!view || !out || !view->active) {
        return -1;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->session = view->session;
    out->scanner = view->scanner;
    out->row = view->row;
    out->mode = (int32_t)view->mode;
    out->field = (int32_t)field;
    out->action = action;
    DSD_SNPRINTF(out->target_id, sizeof out->target_id, "%s", view->target_id);
    return 0;
}

const char*
dsd_app_scan_row_field_name(uint32_t field, unsigned int mode) {
    switch (field) {
        case DSD_SCAN_ROW_FIELD_SQUELCH: return "squelch";
        case DSD_SCAN_ROW_FIELD_WIDTH:
            return dsd_scan_mode_analog_kind((dsd_scan_mode)mode) == DSD_ANALOG_DEMOD_AM ? "AM bandwidth"
                                                                                         : "NFM bandwidth";
        case DSD_SCAN_ROW_FIELD_TONE: return "tone filter";
        case DSD_SCAN_ROW_FIELD_GAIN: return "RTL gain";
        default: return "setting";
    }
}

int
dsd_app_scan_row_value_text(uint32_t field, const dsd_scan_row_edit_value* value, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!value) {
        return -1;
    }
    switch (field) {
        case DSD_SCAN_ROW_FIELD_SQUELCH:
            if (dsd_squelch_mode_is_dynamic(value->squelch_mode)) {
                DSD_SNPRINTF(out, out_size, "%s +%d dB",
                             value->squelch_mode == DSD_SQUELCH_MODE_NOISE ? "noise" : "auto",
                             value->squelch_margin_db);
            } else if (value->squelch_db == 0) {
                DSD_SNPRINTF(out, out_size, "%s", "off");
            } else {
                DSD_SNPRINTF(out, out_size, "%d dB", value->squelch_db);
            }
            return 0;
        case DSD_SCAN_ROW_FIELD_WIDTH: return dsd_app_analog_width_setting_format(value->width_hz, out, out_size);
        case DSD_SCAN_ROW_FIELD_TONE: {
            if (value->tone_filter == DSD_TONE_FILTER_OFF) {
                DSD_SNPRINTF(out, out_size, "%s", "off");
                return 0;
            }
            char list[48];
            (void)dsd_tone_set_format_display(&value->tone_set, list, sizeof list);
            dsd_tone_display_to_ascii(list);
            DSD_SNPRINTF(out, out_size, "%s %s", dsd_tone_filter_mode_name(value->tone_filter), list);
            return 0;
        }
        case DSD_SCAN_ROW_FIELD_GAIN:
            if (value->gain_db <= 0) {
                DSD_SNPRINTF(out, out_size, "%s", "AGC");
            } else {
                DSD_SNPRINTF(out, out_size, "%d dB", value->gain_db);
            }
            return 0;
        default: return -1;
    }
}

/* @p row in @p shown, cut to @p room bytes with "..." when longer, on a UTF-8 character boundary. */
static void
scan_row_notice_fit_label(const char* row, size_t room, char* shown, size_t shown_size) {
    DSD_SNPRINTF(shown, shown_size, "%s", row);
    if (strlen(shown) <= room) {
        return;
    }
    size_t cut = room > 3U ? room - 3U : 0U;
    while (cut > 0U && (((unsigned char)shown[cut]) & 0xC0U) == 0x80U) {
        cut--;
    }
    DSD_SNPRINTF(shown + cut, shown_size - cut, "%s", room > 3U ? "..." : "");
}

/* The notice of an edit that was applied or stored. The outcome comes whole: a row name too long for the toast gives
   way, cut short. */
static void
scan_row_notice_done(const char* row, const char* name, int action, const char* value_text, int stored, char* out,
                     size_t out_size) {
    const char* when = stored ? " from its next visit" : "";
    char tail[DSD_APP_SCAN_ROW_NOTICE_SIZE];
    if (action == DSD_SCAN_ROW_EDIT_INHERIT) {
        DSD_SNPRINTF(tail, sizeof tail, "%s follows the default%s", name, when);
    } else if (action == DSD_SCAN_ROW_EDIT_RESET) {
        DSD_SNPRINTF(tail, sizeof tail, "%s back to the list value%s", name, when);
    } else {
        DSD_SNPRINTF(tail, sizeof tail, "%s %s%s", name, value_text ? value_text : "",
                     stored ? when : " for this session");
    }
    static const char k_head[] = "This channel (";
    static const char k_mid[] = "): ";
    const size_t fixed = (sizeof k_head - 1U) + (sizeof k_mid - 1U) + strlen(tail) + 1U;
    char shown[DSD_APP_SCAN_ROW_LABEL_SIZE];
    scan_row_notice_fit_label(row, out_size > fixed ? out_size - fixed : 0U, shown, sizeof shown);
    DSD_SNPRINTF(out, out_size, "%s%s%s%s", k_head, shown, k_mid, tail);
}

int
dsd_app_scan_row_notice(const char* label, uint32_t field, unsigned int mode, int action, const char* value_text,
                        int status, const char* why, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    const char* name = dsd_app_scan_row_field_name(field, mode);
    const char* row = (label && label[0]) ? label : "this channel";
    switch (status) {
        case DSD_SCAN_ROW_EDIT_APPLIED:
        case DSD_SCAN_ROW_EDIT_STORED:
            scan_row_notice_done(row, name, action, value_text, status == DSD_SCAN_ROW_EDIT_STORED, out, out_size);
            return 0;
        case DSD_SCAN_ROW_EDIT_REFUSED:
            DSD_SNPRINTF(out, out_size, "Refused: this channel's %s: %s", name, (why && why[0]) ? why : "not allowed");
            return 0;
        case DSD_SCAN_ROW_EDIT_STALE:
            DSD_SNPRINTF(out, out_size, "%s", "Refused: the scan changed; nothing applied");
            return 0;
        case DSD_SCAN_ROW_EDIT_BUSY:
            DSD_SNPRINTF(out, out_size, "%s", "Busy: the scan is retuning; try again");
            return 0;
        default: DSD_SNPRINTF(out, out_size, "%s", "Refused: no scan is running"); return 0;
    }
}
