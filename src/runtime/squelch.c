// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The RTL squelch setting's grammar, text and resolution (issue #518 follow-up). */

#include <ctype.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/squelch.h>
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Relative difference below which two levels are the same threshold (the scan scope's push-once rule uses the same). */
static const double k_level_same_relative = 1e-9;

static int
squelch_reason(char* err, size_t err_size, const char* reason) {
    if (err && err_size > 0U) {
        DSD_SNPRINTF(err, err_size, "%s", reason);
    }
    return -1;
}

static int
squelch_clamp_margin(int margin_db) {
    if (margin_db < DSD_SQUELCH_MARGIN_MIN_DB) {
        return DSD_SQUELCH_MARGIN_MIN_DB;
    }
    if (margin_db > DSD_SQUELCH_MARGIN_MAX_DB) {
        return DSD_SQUELCH_MARGIN_MAX_DB;
    }
    return margin_db;
}

dsd_squelch_setting
dsd_squelch_setting_of_level(double level) {
    dsd_squelch_setting s;
    s.mode = DSD_SQUELCH_MODE_LEVEL;
    s.level = (level > 0.0) ? level : 0.0;
    s.margin_db = DSD_SQUELCH_MARGIN_DEFAULT_DB;
    return s;
}

dsd_squelch_setting
dsd_squelch_setting_auto(int margin_db) {
    dsd_squelch_setting s;
    s.mode = DSD_SQUELCH_MODE_AUTO;
    s.level = 0.0;
    s.margin_db = squelch_clamp_margin(margin_db);
    return s;
}

int
dsd_squelch_setting_is_dynamic(const dsd_squelch_setting* s) {
    return s && s->mode == DSD_SQUELCH_MODE_AUTO;
}

int
dsd_squelch_setting_is_off(const dsd_squelch_setting* s) {
    return !s || (s->mode != DSD_SQUELCH_MODE_AUTO && dsd_squelch_is_off(s->level));
}

int
dsd_squelch_setting_equal(const dsd_squelch_setting* a, const dsd_squelch_setting* b) {
    if (!a || !b || a->mode != b->mode) {
        return 0;
    }
    if (a->mode == DSD_SQUELCH_MODE_AUTO) {
        return a->margin_db == b->margin_db;
    }
    const int a_off = dsd_squelch_is_off(a->level);
    const int b_off = dsd_squelch_is_off(b->level);
    if (a_off || b_off) {
        return a_off == b_off;
    }
    const double scale = fmax(fabs(a->level), fabs(b->level));
    return fabs(a->level - b->level) <= k_level_same_relative * scale;
}

/* @p text with leading and trailing spaces dropped, as [*start, *start + *len). */
static void
squelch_trim(const char* text, const char** start, size_t* len) {
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    size_t n = strlen(text);
    while (n > 0U && isspace((unsigned char)text[n - 1U])) {
        n--;
    }
    *start = text;
    *len = n;
}

/* Whether [@p text, @p text + @p len) begins with @p word, ignoring case. */
static int
squelch_starts_with(const char* text, size_t len, const char* word) {
    const size_t wlen = strlen(word);
    if (len < wlen) {
        return 0;
    }
    for (size_t i = 0; i < wlen; i++) {
        if (tolower((unsigned char)text[i]) != word[i]) {
            return 0;
        }
    }
    return 1;
}

/* The "auto" form's tail after the keyword: "" (the default margin) or "+N" / " + N" with N a whole 3..30. */
static int
squelch_parse_auto_tail(const char* tail, size_t len, dsd_squelch_setting* out, char* err, size_t err_size) {
    size_t i = 0U;
    while (i < len && isspace((unsigned char)tail[i])) {
        i++;
    }
    if (i == len) {
        *out = dsd_squelch_setting_auto(DSD_SQUELCH_MARGIN_DEFAULT_DB);
        return 0;
    }
    if (tail[i] != '+') {
        return squelch_reason(err, err_size, "auto takes a margin as auto+N (N in whole dB, 3 to 30)");
    }
    i++;
    while (i < len && isspace((unsigned char)tail[i])) {
        i++;
    }
    char digits[8];
    size_t n = 0U;
    while (i < len && n + 1U < sizeof digits && isdigit((unsigned char)tail[i])) {
        digits[n++] = tail[i++];
    }
    digits[n] = '\0';
    if (n == 0U || i != len) {
        return squelch_reason(err, err_size, "auto takes a margin as auto+N (N in whole dB, 3 to 30)");
    }
    char* end = NULL;
    errno = 0;
    const long margin = strtol(digits, &end, 10);
    if (errno != 0 || !end || *end != '\0' || margin < DSD_SQUELCH_MARGIN_MIN_DB
        || margin > DSD_SQUELCH_MARGIN_MAX_DB) {
        return squelch_reason(err, err_size, "the auto margin must be a whole number of dB from 3 to 30");
    }
    *out = dsd_squelch_setting_auto((int)margin);
    return 0;
}

int
dsd_squelch_setting_parse(const char* text, dsd_squelch_setting* out, char* err, size_t err_size) {
    if (err && err_size > 0U) {
        err[0] = '\0';
    }
    if (!text || !out) {
        return squelch_reason(err, err_size, "no squelch setting");
    }
    const char* start = NULL;
    size_t len = 0U;
    squelch_trim(text, &start, &len);
    if (len == 0U) {
        return squelch_reason(err, err_size, "empty squelch setting");
    }
    if (len == 3U && squelch_starts_with(start, len, "off")) {
        *out = dsd_squelch_setting_of_level(0.0);
        return 0;
    }
    if (squelch_starts_with(start, len, "auto")) {
        return squelch_parse_auto_tail(start + 4, len - 4U, out, err, err_size);
    }
    if (squelch_starts_with(start, len, "noise")) {
        return squelch_reason(err, err_size, "the noise squelch is not available; use auto[+N] or a level in dB");
    }
    char number[64];
    if (len >= sizeof number) {
        return squelch_reason(err, err_size, "expected off, a level in dB (negative), a linear power or auto[+N]");
    }
    DSD_MEMCPY(number, start, len);
    number[len] = '\0';
    char* end = NULL;
    errno = 0;
    const double value = strtod(number, &end);
    if (errno != 0 || !end || end == number || *end != '\0' || !isfinite(value)) {
        return squelch_reason(err, err_size, "expected off, a level in dB (negative), a linear power or auto[+N]");
    }
    *out = dsd_squelch_setting_of_level(dsd_squelch_level_from_sql(value));
    return 0;
}

int
dsd_squelch_setting_format(const dsd_squelch_setting* s, char* out, size_t out_size) {
    if (!out || out_size == 0U) {
        return -1;
    }
    out[0] = '\0';
    if (!s) {
        return -1;
    }
    if (s->mode == DSD_SQUELCH_MODE_AUTO) {
        DSD_SNPRINTF(out, out_size, "auto +%d dB", squelch_clamp_margin(s->margin_db));
        return 0;
    }
    if (dsd_squelch_is_off(s->level)) {
        DSD_SNPRINTF(out, out_size, "%s", "off");
        return 0;
    }
    /* pwr_to_dB() lives in the core object the runtime library does not link: the same clamp, inline. */
    double db = 10.0 * log10(s->level);
    if (db > 0.0) {
        db = 0.0;
    }
    if (db < -120.0) {
        db = -120.0;
    }
    DSD_SNPRINTF(out, out_size, "%.1f dB", db);
    return 0;
}

int
dsd_squelch_setting_resolve(const dsd_squelch_setting* configured, int radio_input, int digital,
                            dsd_squelch_setting* out) {
    dsd_squelch_setting resolved = configured ? *configured : dsd_squelch_setting_of_level(0.0);
    int why = DSD_SQUELCH_RESOLVED_AS_SET;
    if (resolved.mode == DSD_SQUELCH_MODE_AUTO) {
        if (!radio_input) {
            why = DSD_SQUELCH_RESOLVED_NO_RADIO;
        } else if (digital) {
            why = DSD_SQUELCH_RESOLVED_DIGITAL;
        }
        if (why != DSD_SQUELCH_RESOLVED_AS_SET) {
            resolved = dsd_squelch_setting_of_level(0.0);
        }
    }
    if (out) {
        *out = resolved;
    }
    return why;
}

int
dsd_squelch_dynamic_in_force(const dsd_opts* opts) {
    return dsd_opts_input_is_radio(opts) && opts->rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO;
}

double
dsd_squelch_level_in_force(const dsd_opts* opts) {
    if (!opts || opts->rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO) {
        return 0.0;
    }
    return opts->rtl_squelch_level;
}

int
dsd_squelch_level_open(const dsd_opts* opts) {
    return opts && opts->rtl_pwr > dsd_squelch_level_in_force(opts);
}

int
dsd_squelch_gate_open(const dsd_opts* opts, uint8_t flag) {
    if (dsd_squelch_dynamic_in_force(opts)) {
        return (flag & (uint8_t)DSD_SQUELCH_FLAG_CLOSED) == 0U;
    }
    return dsd_squelch_level_open(opts);
}

dsd_squelch_setting
dsd_squelch_setting_of_opts(const dsd_opts* opts) {
    if (!opts) {
        return dsd_squelch_setting_of_level(0.0);
    }
    if (opts->rtl_squelch_mode == DSD_SQUELCH_MODE_AUTO) {
        return dsd_squelch_setting_auto(opts->rtl_squelch_margin_db);
    }
    return dsd_squelch_setting_of_level(opts->rtl_squelch_level);
}

void
dsd_squelch_setting_store(dsd_opts* opts, const dsd_squelch_setting* setting) {
    if (!opts || !setting) {
        return;
    }
    if (setting->mode == DSD_SQUELCH_MODE_AUTO) {
        opts->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
        opts->rtl_squelch_margin_db = squelch_clamp_margin(setting->margin_db);
        return;
    }
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts->rtl_squelch_level = setting->level > 0.0 ? setting->level : 0.0;
}

int
dsd_squelch_spec_field_apply(dsd_opts* opts, const char* text) {
    dsd_squelch_setting setting;
    if (!opts || !text || dsd_squelch_setting_parse(text, &setting, NULL, 0U) != 0) {
        return -1;
    }
    if (opts->rtl_squelch_cli_set) {
        return 1;
    }
    dsd_squelch_setting_store(opts, &setting);
    return 0;
}

void
dsd_squelch_publish_status(dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_rtl_squelch_status st;
    (void)dsd_rtl_stream_io_hook_squelch_status(state, &st);
    state->squelch_auto_active = st.active ? 1U : 0U;
    state->squelch_auto_state = (uint8_t)(st.state >= 0 && st.state <= 255 ? st.state : 0);
    state->squelch_auto_gate_open = st.gate_open ? 1U : 0U;
    state->squelch_auto_plan_valid = st.plan_valid ? 1U : 0U;
    /* The level squelch compares the channel power, half the mean |z|^2 of the channel samples. */
    const double floor_level = st.floor_power / 2.0;
    state->squelch_auto_floor_cdb = floor_level > 1e-30 ? (int32_t)lround(1000.0 * log10(floor_level)) : 0;
}
