// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t g_scan_row_session;

uint32_t
dsd_scan_row_edit_new_session(void) {
    g_scan_row_session++;
    if (g_scan_row_session == 0U) {
        g_scan_row_session = 1U;
    }
    return g_scan_row_session;
}

static int
scan_row_edit_one_field(uint32_t field) {
    return field != 0U && (field & (field - 1U)) == 0U && (field & ~(uint32_t)DSD_SCAN_ROW_FIELDS_ALL) == 0U;
}

static int
scan_row_edit_reason(char* err, size_t err_size, const char* reason) {
    if (err && err_size > 0U) {
        DSD_SNPRINTF(err, err_size, "%s", reason);
    }
    return 0;
}

uint32_t
dsd_scan_row_edit_fields(unsigned int mode, int gain_editable) {
    if (mode == DSD_SCAN_MODE_INHERIT || mode > DSD_SCAN_MODE_LAST) {
        return 0U;
    }
    /* A trunk-scan target and a typed -Y row of an analog class are both conventional; a field a trunked class cannot
       set is no field of any edit either way. */
    const uint32_t options = dsd_scan_options_fields_for_mode(mode, 1);
    uint32_t fields = 0U;
    if (options & DSD_SCAN_OPT_SQUELCH) {
        fields |= DSD_SCAN_ROW_FIELD_SQUELCH;
    }
    if (options & DSD_SCAN_OPT_BANDWIDTH) {
        fields |= DSD_SCAN_ROW_FIELD_WIDTH;
    }
    if (options & DSD_SCAN_OPT_TONE) {
        fields |= DSD_SCAN_ROW_FIELD_TONE;
    }
    if (gain_editable) {
        fields |= DSD_SCAN_ROW_FIELD_GAIN;
    }
    return fields;
}

static int
scan_row_edit_width_valid(unsigned int mode, int width_hz, char* err, size_t err_size) {
    const int kind = dsd_scan_mode_analog_kind((dsd_scan_mode)mode);
    if (kind < 0) {
        return scan_row_edit_reason(err, err_size, "this channel has no analog width");
    }
    if (!dsd_analog_width_in_range(kind, width_hz)) {
        return scan_row_edit_reason(err, err_size, "width out of range");
    }
    return 1;
}

static int
scan_row_edit_tone_valid(const dsd_scan_row_edit_value* value, char* err, size_t err_size) {
    if (value->tone_filter != DSD_TONE_FILTER_OFF && value->tone_filter != DSD_TONE_FILTER_ALLOW
        && value->tone_filter != DSD_TONE_FILTER_BLOCK) {
        return scan_row_edit_reason(err, err_size, "unknown tone filter mode");
    }
    if (value->tone_filter != DSD_TONE_FILTER_OFF && dsd_tone_set_count(&value->tone_set) == 0) {
        return scan_row_edit_reason(err, err_size, "allow and block need a tone or code");
    }
    return 1;
}

/* Whether @p margin_db is a dynamic squelch's margin or threshold; a level edit carries none, or the one it last had. */
static int
scan_row_edit_margin_valid(int margin_db) {
    return margin_db >= DSD_SQUELCH_MARGIN_MIN_DB && margin_db <= DSD_SQUELCH_MARGIN_MAX_DB;
}

/* A squelch value: a level in whole dB, AUTO on an nfm or am row, NOISE on an nfm row, each margin 3..30. */
static int
scan_row_edit_squelch_valid(unsigned int mode, const dsd_scan_row_edit_value* value, char* err, size_t err_size) {
    const int kind = dsd_scan_mode_analog_kind((dsd_scan_mode)mode);
    if (value->squelch_mode == DSD_SQUELCH_MODE_AUTO) {
        if (kind < 0) {
            return scan_row_edit_reason(err, err_size, "auto squelch works on nfm and am channels only");
        }
        if (!scan_row_edit_margin_valid(value->squelch_margin_db)) {
            return scan_row_edit_reason(err, err_size, "auto squelch takes a margin of 3 to 30 dB");
        }
        return 1;
    }
    if (value->squelch_mode == DSD_SQUELCH_MODE_NOISE) {
        if (kind != DSD_ANALOG_DEMOD_FM) {
            return scan_row_edit_reason(err, err_size, "noise squelch works on nfm channels only");
        }
        if (!scan_row_edit_margin_valid(value->squelch_margin_db)) {
            return scan_row_edit_reason(err, err_size, "noise squelch takes 3 to 30 dB of quieting");
        }
        return 1;
    }
    if (value->squelch_db < -100 || value->squelch_db > 0) {
        return scan_row_edit_reason(err, err_size, "squelch takes whole dB from -100 to 0 (0 = off)");
    }
    return 1;
}

int
dsd_scan_row_edit_value_valid(unsigned int mode, uint32_t field, const dsd_scan_row_edit_value* value, char* err,
                              size_t err_size) {
    if (!value) {
        return scan_row_edit_reason(err, err_size, "no value");
    }
    switch (field) {
        case DSD_SCAN_ROW_FIELD_SQUELCH: return scan_row_edit_squelch_valid(mode, value, err, err_size);
        case DSD_SCAN_ROW_FIELD_WIDTH: return scan_row_edit_width_valid(mode, value->width_hz, err, err_size);
        case DSD_SCAN_ROW_FIELD_TONE: return scan_row_edit_tone_valid(value, err, err_size);
        case DSD_SCAN_ROW_FIELD_GAIN:
            if (value->gain_db < 0 || value->gain_db > DSD_SCAN_ROW_EDIT_GAIN_MAX_DB) {
                return scan_row_edit_reason(err, err_size, "gain takes 0 (AGC) to 49 dB");
            }
            return 1;
        default: return scan_row_edit_reason(err, err_size, "unknown field");
    }
}

static void
scan_row_edit_copy_value(dsd_scan_row_edit_value* dst, uint32_t field, const dsd_scan_row_edit_value* src) {
    switch (field) {
        case DSD_SCAN_ROW_FIELD_SQUELCH:
            dst->squelch_db = src->squelch_db;
            dst->squelch_mode = src->squelch_mode;
            /* A level without a margin keeps the one the field had, which Auto starts from again. */
            if (scan_row_edit_margin_valid(src->squelch_margin_db)) {
                dst->squelch_margin_db = src->squelch_margin_db;
            }
            break;
        case DSD_SCAN_ROW_FIELD_WIDTH: dst->width_hz = src->width_hz; break;
        case DSD_SCAN_ROW_FIELD_TONE:
            dst->tone_filter = src->tone_filter;
            dst->tone_set = src->tone_set;
            break;
        default: dst->gain_db = src->gain_db; break;
    }
}

int
dsd_scan_row_edit_change(dsd_scan_row_edit* edit, uint32_t field, dsd_scan_row_edit_action action,
                         const dsd_scan_row_edit_value* value) {
    if (!edit || !scan_row_edit_one_field(field)) {
        return -1;
    }
    switch (action) {
        case DSD_SCAN_ROW_EDIT_SET:
            if (!value) {
                return -1;
            }
            scan_row_edit_copy_value(&edit->value, field, value);
            edit->set |= field;
            edit->inherit &= ~field;
            return 0;
        case DSD_SCAN_ROW_EDIT_INHERIT:
            edit->inherit |= field;
            edit->set &= ~field;
            return 0;
        case DSD_SCAN_ROW_EDIT_RESET:
            edit->set &= ~field;
            edit->inherit &= ~field;
            return 0;
        default: return -1;
    }
}

void
dsd_scan_row_edit_take_fields(dsd_scan_row_edit* edit, const dsd_scan_row_edit* from, uint32_t fields) {
    if (!edit || !from) {
        return;
    }
    fields &= (uint32_t)DSD_SCAN_ROW_FIELDS_ALL;
    for (uint32_t field = 1U; field <= (uint32_t)DSD_SCAN_ROW_FIELDS_ALL; field <<= 1U) {
        if (fields & field) {
            scan_row_edit_copy_value(&edit->value, field, &from->value);
        }
    }
    edit->set = (edit->set & ~fields) | (from->set & fields);
    edit->inherit = (edit->inherit & ~fields) | (from->inherit & fields);
}

int
dsd_scan_row_edit_width_request_differs(const dsd_scan_option_values* before, const dsd_scan_option_values* after) {
    const int owns_before = before && (before->present & DSD_SCAN_OPT_BANDWIDTH);
    const int owns_after = after && (after->present & DSD_SCAN_OPT_BANDWIDTH);
    if (owns_before != owns_after) {
        return 1;
    }
    return owns_before
           && (before->channel_bw_hz != after->channel_bw_hz || before->channel_bw_kind != after->channel_bw_kind);
}

uint32_t
dsd_scan_row_edit_fields_listed(const dsd_scan_option_values* row, int list_gain_is_set) {
    uint32_t fields = list_gain_is_set ? (uint32_t)DSD_SCAN_ROW_FIELD_GAIN : 0U;
    if (row && (row->present & DSD_SCAN_OPT_SQUELCH)) {
        fields |= DSD_SCAN_ROW_FIELD_SQUELCH;
    }
    if (row && (row->present & DSD_SCAN_OPT_BANDWIDTH)) {
        fields |= DSD_SCAN_ROW_FIELD_WIDTH;
    }
    if (row && (row->present & DSD_SCAN_OPT_TONE)) {
        fields |= DSD_SCAN_ROW_FIELD_TONE;
    }
    return fields;
}

uint32_t
dsd_scan_row_edit_fields_edited(const dsd_scan_row_edit* edit) {
    return edit ? (edit->set | edit->inherit) : 0U;
}

void
dsd_scan_row_edit_apply(const dsd_scan_option_values* row, const dsd_scan_row_edit* edit, unsigned int mode,
                        dsd_scan_option_values* out) {
    if (!out) {
        return;
    }
    if (row) {
        *out = *row;
    } else {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    if (!edit) {
        return;
    }
    if (edit->set & DSD_SCAN_ROW_FIELD_SQUELCH) {
        out->present |= DSD_SCAN_OPT_SQUELCH;
        out->squelch_mode = dsd_squelch_mode_or_level(edit->value.squelch_mode);
        /* A level edit without a margin keeps the row's own (its list's dynamic margin), as the setting keeps one. */
        if (scan_row_edit_margin_valid(edit->value.squelch_margin_db)) {
            out->squelch_margin_db = edit->value.squelch_margin_db;
        }
        out->squelch_db = dsd_squelch_mode_is_dynamic(out->squelch_mode) ? 0 : edit->value.squelch_db;
    } else if (edit->inherit & DSD_SCAN_ROW_FIELD_SQUELCH) {
        out->present &= ~(uint32_t)DSD_SCAN_OPT_SQUELCH;
    }
    const int kind = dsd_scan_mode_analog_kind((dsd_scan_mode)mode);
    if ((edit->set & DSD_SCAN_ROW_FIELD_WIDTH) && kind >= 0) {
        out->present |= DSD_SCAN_OPT_BANDWIDTH;
        out->channel_bw_hz = edit->value.width_hz;
        out->channel_bw_kind = kind;
    } else if (edit->inherit & DSD_SCAN_ROW_FIELD_WIDTH) {
        out->present &= ~(uint32_t)DSD_SCAN_OPT_BANDWIDTH;
    }
    if (edit->set & DSD_SCAN_ROW_FIELD_TONE) {
        out->present |= DSD_SCAN_OPT_TONE;
        out->tone_filter = edit->value.tone_filter;
        out->tone_set = edit->value.tone_set;
    } else if (edit->inherit & DSD_SCAN_ROW_FIELD_TONE) {
        out->present &= ~(uint32_t)DSD_SCAN_OPT_TONE;
    }
}

void
dsd_scan_row_edit_gain(int list_is_set, int list_db, const dsd_scan_row_edit* edit, int* out_is_set, int* out_db) {
    int is_set = list_is_set ? 1 : 0;
    int db = list_is_set ? list_db : 0;
    if (edit && (edit->set & DSD_SCAN_ROW_FIELD_GAIN)) {
        is_set = 1;
        db = edit->value.gain_db;
    } else if (edit && (edit->inherit & DSD_SCAN_ROW_FIELD_GAIN)) {
        is_set = 0;
        db = 0;
    }
    if (out_is_set) {
        *out_is_set = is_set;
    }
    if (out_db) {
        *out_db = db;
    }
}
