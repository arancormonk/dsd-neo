// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The RTL squelch setting's grammar, text and resolution (issue #518 follow-up). */

#include <ctype.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/edacs_session.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/fp_opaque.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/number_text.h>
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/squelch.h>
#include <dsd-neo/runtime/tuner_owner.h>
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

dsd_squelch_setting
dsd_squelch_setting_noise(int threshold_db) {
    dsd_squelch_setting s;
    s.mode = DSD_SQUELCH_MODE_NOISE;
    s.level = 0.0;
    s.margin_db = squelch_clamp_margin(threshold_db);
    return s;
}

dsd_squelch_setting
dsd_squelch_setting_dynamic(int mode, int margin_db) {
    return mode == DSD_SQUELCH_MODE_NOISE ? dsd_squelch_setting_noise(margin_db) : dsd_squelch_setting_auto(margin_db);
}

int
dsd_squelch_setting_is_dynamic(const dsd_squelch_setting* s) {
    return s && dsd_squelch_mode_is_dynamic(s->mode);
}

int
dsd_squelch_setting_is_off(const dsd_squelch_setting* s) {
    return !s || (!dsd_squelch_mode_is_dynamic(s->mode) && dsd_squelch_is_off(s->level));
}

int
dsd_squelch_setting_equal(const dsd_squelch_setting* a, const dsd_squelch_setting* b) {
    if (!a || !b || a->mode != b->mode) {
        return 0;
    }
    if (dsd_squelch_mode_is_dynamic(a->mode)) {
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

/* The index of the first non-space at or after @p i in [@p text, @p text + @p len). */
static size_t
squelch_skip_space(const char* text, size_t len, size_t i) {
    while (i < len && isspace((unsigned char)text[i])) {
        i++;
    }
    return i;
}

/* A whole number of dB from DSD_SQUELCH_MARGIN_MIN_DB to _MAX_DB that is all of [@p text, @p text + @p len): 1, 0 when
   it is not digits alone, -1 when it is digits out of range. */
static int
squelch_parse_margin(const char* text, size_t len, int* margin_db) {
    char digits[8];
    if (len == 0U || len >= sizeof digits) {
        return len == 0U ? 0 : -1;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isdigit((unsigned char)text[i])) {
            return 0;
        }
        digits[i] = text[i];
    }
    digits[len] = '\0';
    char* end = NULL;
    errno = 0;
    const long margin = strtol(digits, &end, 10);
    if (errno != 0 || !end || *end != '\0' || margin < DSD_SQUELCH_MARGIN_MIN_DB
        || margin > DSD_SQUELCH_MARGIN_MAX_DB) {
        return -1;
    }
    *margin_db = (int)margin;
    return 1;
}

/* A dynamic form's tail after its keyword: "" (the default) or "+N" / " + N" with N a whole 3..30. */
static int
squelch_parse_dynamic_tail(int mode, const char* tail, size_t len, dsd_squelch_setting* out, char* err,
                           size_t err_size) {
    const int noise = mode == DSD_SQUELCH_MODE_NOISE;
    const size_t i = squelch_skip_space(tail, len, 0U);
    if (i == len) {
        *out = dsd_squelch_setting_dynamic(mode, DSD_SQUELCH_MARGIN_DEFAULT_DB);
        return 0;
    }
    int margin = 0;
    int parsed = 0;
    if (tail[i] == '+') {
        const size_t digits = squelch_skip_space(tail, len, i + 1U);
        parsed = squelch_parse_margin(tail + digits, len - digits, &margin);
    }
    if (parsed == 0) {
        return squelch_reason(err, err_size,
                              noise ? "noise takes a threshold as noise+N (N in whole dB of quieting, 3 to 30)"
                                    : "auto takes a margin as auto+N (N in whole dB, 3 to 30)");
    }
    if (parsed < 0) {
        return squelch_reason(err, err_size,
                              noise ? "the noise threshold must be a whole number of dB from 3 to 30"
                                    : "the auto margin must be a whole number of dB from 3 to 30");
    }
    *out = dsd_squelch_setting_dynamic(mode, margin);
    return 0;
}

/* A level: a number, negative for dB, positive for a linear mean power, 0 for off. */
static int
squelch_parse_level(const char* start, size_t len, dsd_squelch_setting* out, char* err, size_t err_size) {
    char number[64];
    char* end = NULL;
    double value = 0.0;
    if (len < sizeof number) {
        DSD_MEMCPY(number, start, len);
        number[len] = '\0';
        errno = 0;
        value = strtod(number, &end);
    }
    if (len >= sizeof number || errno != 0 || !end || end == number || *end != '\0'
        || !isfinite(dsd_fp_opaque_d(value))) {
        return squelch_reason(err, err_size,
                              "expected off, a level in dB (negative), a linear power, auto[+N] or noise[+N]");
    }
    *out = dsd_squelch_setting_of_level(dsd_squelch_level_from_sql(value));
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
        return squelch_parse_dynamic_tail(DSD_SQUELCH_MODE_AUTO, start + 4, len - 4U, out, err, err_size);
    }
    if (squelch_starts_with(start, len, "noise")) {
        return squelch_parse_dynamic_tail(DSD_SQUELCH_MODE_NOISE, start + 5, len - 5U, out, err, err_size);
    }
    return squelch_parse_level(start, len, out, err, err_size);
}

int
dsd_squelch_setting_level_finite(const dsd_squelch_setting* s) {
    if (!s) {
        return 0;
    }
    /* IEEE here (src/runtime/CMakeLists.txt); the round trip keeps the test when LTO inlines this into fast-math code. */
    return dsd_squelch_mode_is_dynamic(s->mode) || isfinite(dsd_fp_opaque_d(s->level));
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
    if (dsd_squelch_mode_is_dynamic(s->mode)) {
        DSD_SNPRINTF(out, out_size, "%s +%d dB", s->mode == DSD_SQUELCH_MODE_NOISE ? "noise" : "auto",
                     squelch_clamp_margin(s->margin_db));
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
    char num[32];
    (void)dsd_number_text(num, sizeof num, db, 0, 1, 0);
    DSD_SNPRINTF(out, out_size, "%s dB", num);
    return 0;
}

/* Why a dynamic setting runs as it does on @p input (DSD_SQUELCH_RESOLVED_AS_SET: as written, or as AUTO on a radio
   input's AM channel). */
static int
squelch_dynamic_resolution(int mode, int input, int digital, int am) {
    /* AUTO never runs off a radio input, whatever the channel. */
    if ((input != DSD_SQUELCH_INPUT_RADIO && input != DSD_SQUELCH_INPUT_AUDIO)
        || (input == DSD_SQUELCH_INPUT_AUDIO && mode != DSD_SQUELCH_MODE_NOISE)) {
        return DSD_SQUELCH_RESOLVED_NO_RADIO;
    }
    if (digital) {
        return DSD_SQUELCH_RESOLVED_DIGITAL;
    }
    if (input == DSD_SQUELCH_INPUT_AUDIO) {
        return am ? DSD_SQUELCH_RESOLVED_AUDIO_AM : DSD_SQUELCH_RESOLVED_AS_SET;
    }
    return (am && mode == DSD_SQUELCH_MODE_NOISE) ? DSD_SQUELCH_RESOLVED_AM_AUTO : DSD_SQUELCH_RESOLVED_AS_SET;
}

int
dsd_squelch_setting_resolve(const dsd_squelch_setting* configured, int input, int digital, int am,
                            dsd_squelch_setting* out) {
    dsd_squelch_setting resolved = configured ? *configured : dsd_squelch_setting_of_level(0.0);
    int why = DSD_SQUELCH_RESOLVED_AS_SET;
    if (dsd_squelch_mode_is_dynamic(resolved.mode)) {
        why = squelch_dynamic_resolution(resolved.mode, input, digital, am);
        if (why == DSD_SQUELCH_RESOLVED_AM_AUTO) {
            resolved = dsd_squelch_setting_auto(resolved.margin_db);
        } else if (why != DSD_SQUELCH_RESOLVED_AS_SET) {
            resolved = dsd_squelch_setting_of_level(0.0);
        }
    }
    if (out) {
        *out = resolved;
    }
    return why;
}

int
dsd_squelch_input_kind(const dsd_opts* opts) {
    if (dsd_opts_input_is_radio(opts)) {
        return DSD_SQUELCH_INPUT_RADIO;
    }
    /* The one set of audio inputs a rigctl peer's passband applies to as well (issue #621). */
    return dsd_opts_input_is_pcm_audio(opts) ? DSD_SQUELCH_INPUT_AUDIO : DSD_SQUELCH_INPUT_OTHER;
}

int
dsd_squelch_pcm_noise_in_force(const dsd_opts* opts) {
    /* The input type first: the monitor asks for every sample. On audio input, dsd_analog_tone_detection_active()'s FM
       monitor is these three options (every audio input carries audio). */
    return dsd_squelch_input_kind(opts) == DSD_SQUELCH_INPUT_AUDIO && opts->rtl_squelch_mode == DSD_SQUELCH_MODE_NOISE
           && opts->analog_only == 1 && opts->monitor_input_audio == 1 && opts->analog_demod == DSD_ANALOG_DEMOD_FM;
}

/* Whether the PCM noise squelch @p state publishes gates: learning, or holding a reference. */
static int
squelch_pcm_noise_available(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    switch (state->squelch_noise_state) {
        case DSD_SQUELCH_NOISE_STATE_LEARNING:
        case DSD_SQUELCH_NOISE_STATE_PROVISIONAL:
        case DSD_SQUELCH_NOISE_STATE_KNOWN: return 1;
        default: return 0;
    }
}

int
dsd_squelch_dynamic_in_force(const dsd_opts* opts, const dsd_state* state) {
    if (dsd_opts_input_is_radio(opts)) {
        return dsd_squelch_mode_is_dynamic(opts->rtl_squelch_mode);
    }
    return dsd_squelch_pcm_noise_in_force(opts) && squelch_pcm_noise_available(state);
}

int
dsd_squelch_noise_has_no_fm(const dsd_opts* opts, const dsd_state* state) {
    return opts && dsd_opts_is_analog_family(opts) && opts->analog_demod == DSD_ANALOG_DEMOD_AM
           && opts->scanner_mode != 1 && !dsd_trunk_scan_owns_tuner(opts, state);
}

int
dsd_squelch_edacs_analog_voice(const dsd_opts* opts, const dsd_state* state) {
    return dsd_edacs_session(opts, state) && opts->trunk_enable == 1 && !dsd_opts_is_analog_family(opts)
           && !dsd_opts_runs_m17_encoder(opts);
}

int
dsd_squelch_channel_digital(const dsd_opts* opts, const dsd_state* state) {
    return !(dsd_opts_is_analog_family(opts) || dsd_opts_runs_m17_encoder(opts)
             || dsd_squelch_edacs_analog_voice(opts, state));
}

int
dsd_squelch_dynamic_input_kind(const dsd_opts* opts, const dsd_state* state) {
    const int input = dsd_squelch_input_kind(opts);
    if (input == DSD_SQUELCH_INPUT_AUDIO
        && (dsd_opts_runs_m17_encoder(opts) || dsd_squelch_edacs_analog_voice(opts, state))) {
        return DSD_SQUELCH_INPUT_OTHER;
    }
    return input;
}

int
dsd_squelch_stream_gate_running(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || dsd_squelch_input_kind(opts) != DSD_SQUELCH_INPUT_RADIO
        || !dsd_squelch_mode_is_dynamic(opts->rtl_squelch_mode)) {
        return 0;
    }
    dsd_rtl_squelch_status st;
    return dsd_rtl_stream_io_hook_squelch_status(state, &st) == 0 && st.active && st.plan_valid;
}

int
dsd_squelch_analog_calls_allowed(const dsd_opts* opts, const dsd_state* state) {
    if (!opts) {
        return 0;
    }
    if (!dsd_opts_input_is_radio(opts) || dsd_squelch_dynamic_in_force(opts, state)) {
        return 1;
    }
    /* The startup level as dB_to_pwr() makes it, within the relative tolerance two equal levels share. */
    const double startup = dsd_squelch_level_from_sql((double)DSD_SQUELCH_STARTUP_DB);
    if (dsd_squelch_level_in_force(opts) > startup * (1.0 + k_level_same_relative)) {
        return 1;
    }
    return opts->analog_tone_filter == DSD_TONE_FILTER_ALLOW && dsd_tone_set_count(&opts->analog_tone_set) > 0
           && dsd_analog_tone_detection_active(opts);
}

double
dsd_squelch_level_in_force(const dsd_opts* opts) {
    if (!opts || dsd_squelch_mode_is_dynamic(opts->rtl_squelch_mode)) {
        return 0.0;
    }
    return opts->rtl_squelch_level;
}

int
dsd_squelch_level_open(const dsd_opts* opts) {
    return opts && opts->rtl_pwr > dsd_squelch_level_in_force(opts);
}

int
dsd_squelch_flags_in_force(const dsd_opts* opts, const dsd_state* state) {
    return dsd_squelch_dynamic_in_force(opts, state) || dsd_squelch_pcm_noise_in_force(opts);
}

int
dsd_squelch_gate_open(const dsd_opts* opts, const dsd_state* state, uint8_t flag) {
    if (dsd_squelch_flags_in_force(opts, state)) {
        return (flag & (uint8_t)DSD_SQUELCH_FLAG_CLOSED) == 0U;
    }
    return dsd_squelch_level_open(opts);
}

dsd_squelch_setting
dsd_squelch_setting_of_opts(const dsd_opts* opts) {
    if (!opts) {
        return dsd_squelch_setting_of_level(0.0);
    }
    if (dsd_squelch_mode_is_dynamic(opts->rtl_squelch_mode)) {
        return dsd_squelch_setting_dynamic(opts->rtl_squelch_mode, opts->rtl_squelch_margin_db);
    }
    return dsd_squelch_setting_of_level(opts->rtl_squelch_level);
}

void
dsd_squelch_setting_store(dsd_opts* opts, const dsd_squelch_setting* setting) {
    if (!opts || !setting) {
        return;
    }
    dsd_squelch_analog_default_note_written(opts);
    if (dsd_squelch_mode_is_dynamic(setting->mode)) {
        opts->rtl_squelch_mode = setting->mode;
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
    /* `0` is how a spec leaves its squelch field unset (the frontends write it there); any other setting is the
       squelch someone chose, and the analog default stays out for the session (issue #899). */
    if (strcmp(text, "0") != 0) {
        dsd_squelch_analog_default_end(opts);
    }
    dsd_squelch_setting_store(opts, &setting);
    return 0;
}

/* The rtl_sql key's span for an active level: -120 dB (pwr_to_dB()'s floor) to -1 (0 is off). */
enum { SQUELCH_WHOLE_DB_MIN = -120, SQUELCH_WHOLE_DB_MAX = -1 };

/* How close in dB a level is to a whole dB's own to be it: far wider than a dB-to-power-to-dB round trip's error, far
   narrower than any level a person writes. */
static const double k_whole_db_tolerance = 1e-9;

int
dsd_squelch_level_whole_db(double level, int* out_db) {
    if (!out_db) {
        return -1;
    }
    if (dsd_squelch_is_off(level)) {
        *out_db = 0;
        return 0;
    }
    /* IEEE here (src/runtime/CMakeLists.txt): an infinite level reads as above -1 dB, and the comparisons hold the
       rounding to the span before lround() sees the figure. */
    const double db = 10.0 * log10(level);
    if (!(db < (double)SQUELCH_WHOLE_DB_MAX)) {
        *out_db = SQUELCH_WHOLE_DB_MAX;
    } else if (!(db > (double)SQUELCH_WHOLE_DB_MIN)) {
        *out_db = SQUELCH_WHOLE_DB_MIN;
    } else {
        *out_db = (int)lround(db);
    }
    return 0;
}

int
dsd_squelch_level_is_whole_db(double level, int* out_db) {
    int db = 0;
    (void)dsd_squelch_level_whole_db(level, &db);
    if (db != 0 && !(fabs(10.0 * log10(level) - (double)db) <= k_whole_db_tolerance)) {
        return 0;
    }
    if (out_db) {
        *out_db = db;
    }
    return 1;
}

/* ---- The analog default squelch's standing (issue #899); dsd_squelch_analog_default_sync() moves it. ---- */

/* Whether @p text (leading blanks skipped) names a dynamic mode, ignoring ASCII case: then the grammar's own reason
   (a margin out of range) says what is wrong with it. */
static int
squelch_default_text_names_dynamic(const char* text) {
    while (text && (*text == ' ' || *text == '\t')) {
        text++;
    }
    if (!text) {
        return 0;
    }
    static const char* const names[] = {"auto", "noise"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        size_t k = 0;
        while (names[i][k] != '\0' && tolower((unsigned char)text[k]) == names[i][k]) {
            k++;
        }
        if (names[i][k] == '\0') {
            return 1;
        }
    }
    return 0;
}

static void
squelch_default_refuse(char* err, size_t err_size, const char* what) {
    if (err && err_size > 0U) {
        DSD_SNPRINTF(err, err_size, "expects auto[+N] or noise[+N] (N from 3 to 30)%s", what);
    }
}

int
dsd_squelch_analog_default_parse(dsd_opts* opts, const char* text, char* err, size_t err_size) {
    dsd_squelch_setting setting;
    if (dsd_squelch_setting_parse(text, &setting, err, err_size) != 0) {
        if (!squelch_default_text_names_dynamic(text)) {
            squelch_default_refuse(err, err_size, "");
        }
        return -1;
    }
    if (dsd_squelch_setting_is_off(&setting)) {
        /* No default is the option left out: an Off here would read as a setting the session runs. */
        squelch_default_refuse(err, err_size, ", not off: leave the option out for no default");
        return -1;
    }
    if (!dsd_squelch_setting_is_dynamic(&setting)) {
        squelch_default_refuse(err, err_size, ", not a level");
        return -1;
    }
    if (!opts) {
        return 0;
    }
    opts->squelch_default_mode = setting.mode;
    opts->squelch_default_margin_db = setting.margin_db;
    return 0;
}

int
dsd_squelch_analog_default_pending(const dsd_opts* opts) {
    return opts && dsd_squelch_mode_is_dynamic(opts->squelch_default_mode)
           && opts->squelch_default_state != DSD_SQUELCH_DEFAULT_ENDED;
}

dsd_squelch_setting
dsd_squelch_analog_default_setting(const dsd_opts* opts) {
    if (!opts || !dsd_squelch_mode_is_dynamic(opts->squelch_default_mode)) {
        return dsd_squelch_setting_of_level(0.0);
    }
    return dsd_squelch_setting_dynamic(opts->squelch_default_mode, opts->squelch_default_margin_db);
}

/* Write the squelch fields as given, without what dsd_squelch_setting_store() notes about an applied default. */
static void
squelch_default_write(dsd_opts* opts, int mode, double level, int margin_db) {
    opts->rtl_squelch_mode = mode;
    opts->rtl_squelch_level = level;
    opts->rtl_squelch_margin_db = margin_db;
}

int
dsd_squelch_analog_default_release(dsd_opts* opts) {
    if (!opts || opts->squelch_default_state != DSD_SQUELCH_DEFAULT_APPLIED) {
        return 0;
    }
    const dsd_squelch_setting before = dsd_squelch_setting_of_opts(opts);
    squelch_default_write(opts, opts->squelch_default_base_mode, opts->squelch_default_base_level,
                          opts->squelch_default_base_margin_db);
    opts->squelch_default_state = DSD_SQUELCH_DEFAULT_IDLE;
    const dsd_squelch_setting after = dsd_squelch_setting_of_opts(opts);
    return !dsd_squelch_setting_equal(&before, &after);
}

void
dsd_squelch_analog_default_end(dsd_opts* opts) {
    if (!opts) {
        return;
    }
    (void)dsd_squelch_analog_default_release(opts);
    opts->squelch_default_state = DSD_SQUELCH_DEFAULT_ENDED;
}

void
dsd_squelch_analog_default_note_written(dsd_opts* opts) {
    if (opts && opts->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED) {
        opts->squelch_default_state = DSD_SQUELCH_DEFAULT_IDLE;
        opts->squelch_default_moved = 1;
    }
}

void
dsd_squelch_configured_fields(const dsd_opts* opts, double* level, int* mode, int* margin_db) {
    if (!opts) {
        return;
    }
    const int applied = opts->squelch_default_state == DSD_SQUELCH_DEFAULT_APPLIED;
    if (level) {
        *level = applied ? opts->squelch_default_base_level : opts->rtl_squelch_level;
    }
    if (mode) {
        *mode = applied ? opts->squelch_default_base_mode : opts->rtl_squelch_mode;
    }
    if (margin_db) {
        *margin_db = applied ? opts->squelch_default_base_margin_db : opts->rtl_squelch_margin_db;
    }
}

/* Nothing running: every field at rest. */
static void
squelch_clear_status(dsd_state* state) {
    state->squelch_auto_active = 0U;
    state->squelch_auto_state = 0U;
    state->squelch_auto_gate_open = 0U;
    state->squelch_auto_plan_valid = 0U;
    state->squelch_auto_floor_cdb = 0;
    state->squelch_noise_active = 0U;
    state->squelch_noise_measured = 0U;
    state->squelch_noise_quieting_cdb = 0;
    state->squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NONE;
}

/* Radio input: what the RTL stream's squelch reports. */
static void
squelch_publish_rtl_status(dsd_state* state) {
    state->squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NONE;
    dsd_rtl_squelch_status st;
    (void)dsd_rtl_stream_io_hook_squelch_status(state, &st);
    state->squelch_auto_active = st.active ? 1U : 0U;
    state->squelch_auto_state = (uint8_t)(st.state >= 0 && st.state <= 255 ? st.state : 0);
    state->squelch_auto_gate_open = st.gate_open ? 1U : 0U;
    state->squelch_auto_plan_valid = st.plan_valid ? 1U : 0U;
    /* The level squelch compares the channel power, half the mean |z|^2 of the channel samples. */
    const double floor_level = st.floor_power / 2.0;
    state->squelch_auto_floor_cdb = floor_level > 1e-30 ? (int32_t)lround(1000.0 * log10(floor_level)) : 0;
    state->squelch_noise_active = st.active && st.noise ? 1U : 0U;
    state->squelch_noise_measured = state->squelch_noise_active && st.quieting_valid ? 1U : 0U;
    double quieting = state->squelch_noise_measured ? st.quieting_db : 0.0;
    if (quieting > 200.0) {
        quieting = 200.0;
    } else if (quieting < -200.0) {
        quieting = -200.0;
    }
    state->squelch_noise_quieting_cdb = (int32_t)lround(100.0 * quieting);
}

void
dsd_squelch_publish_status(const dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    if (opts && !dsd_opts_input_is_radio(opts)) {
        /* Audio input: the PCM noise squelch's own fields while it runs, nothing otherwise. */
        if (!dsd_squelch_pcm_noise_in_force(opts)) {
            squelch_clear_status(state);
        }
        return;
    }
    squelch_publish_rtl_status(state);
}
