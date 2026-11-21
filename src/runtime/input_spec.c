// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/spec_fields.h>
#include <dsd-neo/runtime/squelch.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "freq_parse_internal.h"

int
dsd_spec_squelch_field_parse(const char* input, const char* text, dsd_squelch_setting* out) {
    char why[96];
    if (dsd_squelch_setting_parse(text, out, why, sizeof why) == 0) {
        return 0;
    }
    /* The field is the user's own squelch text, which no key shares a spec place with; the reason never repeats it. */
    LOG_ERROR("Invalid squelch field \"%s\" in the %s input: %s.\n", text ? text : "", input ? input : "radio", why);
    return -1;
}

static int
looks_like_soapy_kv_args(const char* tok) {
    if (!tok) {
        return 0;
    }
    return (strchr(tok, '=') != NULL) || (strchr(tok, ',') != NULL);
}

static int
has_freq_hint_chars(const char* tok) {
    if (!tok) {
        return 0;
    }
    while (*tok) {
        if (*tok == '.' || *tok == 'k' || *tok == 'K' || *tok == 'm' || *tok == 'M' || *tok == 'g' || *tok == 'G') {
            return 1;
        }
        tok++;
    }
    return 0;
}

static int
parse_int_strict(const char* tok, int* out) {
    if (!tok || !out || tok[0] == '\0') {
        return -1;
    }
    errno = 0;
    char* end = NULL;
    long v = strtol(tok, &end, 10);
    if (errno != 0 || end == tok || *end != '\0' || v < INT_MIN || v > INT_MAX) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

/* A number with a K, M or G suffix ("851.375M", "5.8G", "+5.8G", "5.8e0G"): the text before the suffix letter is
   non-empty and all of it is a number strtod() reads, the grammar of dsd_parse_freq_hz(). Such a token is always a
   frequency, never device args. */
static int
is_suffixed_freq_token(const char* tok) {
    if (!tok) {
        return 0;
    }
    const size_t len = strlen(tok);
    if (len < 2) {
        return 0;
    }
    const char c = tok[len - 1];
    if (!(c == 'k' || c == 'K' || c == 'm' || c == 'M' || c == 'g' || c == 'G')) {
        return 0;
    }
    /* No number continues into a K, M or G (none is a hex digit or an exponent), so strtod() stopping at the suffix
       letter is strtod() consuming all of the text before it. */
    char* end = NULL;
    (void)strtod(tok, &end);
    return end == tok + len - 1;
}

static int
parse_freq_hz_token(const char* tok, uint32_t* out_hz) {
    if (!tok || !out_hz || tok[0] == '\0') {
        return -1;
    }
    uint32_t hz = dsd_parse_freq_hz(tok);
    /* Use a conservative floor to avoid mis-parsing colon-delimited numeric args (a suffixed token under it is an
       out-of-range frequency instead; see parse_soapy_head_tokens()). */
    if (hz < 1000000U) {
        return -1;
    }
    *out_hz = hz;
    return 0;
}

static void
restore_opaque_soapy_args(dsd_opts* opts, const char* raw_tail) {
    if (!opts || !raw_tail) {
        return;
    }

    const char prefix[] = "soapy:";
    const size_t prefix_len = sizeof(prefix) - 1U;
    if (sizeof opts->audio_in_dev <= prefix_len) {
        opts->audio_in_dev[0] = '\0';
        return;
    }

    DSD_MEMCPY(opts->audio_in_dev, prefix, prefix_len);
    size_t tail_room = sizeof opts->audio_in_dev - prefix_len - 1U;
    size_t tail_len = strnlen(raw_tail, tail_room);
    DSD_MEMCPY(opts->audio_in_dev + prefix_len, raw_tail, tail_len);
    opts->audio_in_dev[prefix_len + tail_len] = '\0';
}

enum { SOFTY_TOK_MAX = 16 };

/* The tail's fields, each in its place, empty ones included (dsd_spec_split_fields(), as every other spec reads its
   fields: skipping an empty one read every later field one place early). @p tail_copy keeps the tail as it was. At most
   SOFTY_TOK_MAX are stored; *out_count is how many the tail has, which can be more. */
static int
tokenize_soapy_tail(const char* raw_tail, char* tail_copy, size_t tail_copy_size, char* scratch, size_t scratch_size,
                    char* tok[SOFTY_TOK_MAX], size_t* out_count) {
    if (!raw_tail || !tail_copy || !scratch || !tok || !out_count || tail_copy_size == 0 || scratch_size == 0) {
        return -1;
    }

    DSD_SNPRINTF(tail_copy, tail_copy_size, "%s", raw_tail);
    tail_copy[tail_copy_size - 1] = '\0';
    const int count = dsd_spec_split_fields(tail_copy, scratch, scratch_size, tok, SOFTY_TOK_MAX);
    if (count < 0) {
        return -1;
    }
    *out_count = (size_t)count;
    return 0;
}

/* Whether every stored field is empty ("soapy::"): the default device, as "soapy:" is. */
static int
soapy_fields_all_empty(char* tok[SOFTY_TOK_MAX], size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (tok[i][0] != '\0') {
            return 0;
        }
    }
    return 1;
}

enum { SOAPY_HEAD_FREQ_OUT_OF_RANGE = -2, SOAPY_HEAD_FREQ_LATE = -3 };

/* Why the frequency place @p idx of the @p n stored fields holds no frequency: SOAPY_HEAD_FREQ_OUT_OF_RANGE for a
   suffixed number out of range, SOAPY_HEAD_FREQ_LATE when the place is empty and the field after it is a suffixed
   number (*out_idx at that number), else -1 (opaque args). A K, M or G number is never device args, so one after an
   empty frequency place (`driver=x::851M`, which skipping empty fields once read as tuning) is a frequency one place
   late. */
static int
soapy_head_not_a_frequency(char* tok[SOFTY_TOK_MAX], size_t n, size_t idx, size_t* out_idx) {
    if (is_suffixed_freq_token(tok[idx])) {
        *out_idx = idx;
        return SOAPY_HEAD_FREQ_OUT_OF_RANGE;
    }
    if (tok[idx][0] == '\0' && idx + 1U < n && is_suffixed_freq_token(tok[idx + 1U])) {
        *out_idx = idx + 1U;
        return SOAPY_HEAD_FREQ_LATE;
    }
    return -1;
}

/* Reads the optional device args and the frequency from the @p n stored fields. Returns 0 when they parse;
   SOAPY_HEAD_FREQ_OUT_OF_RANGE, with *out_idx at the frequency token, when that place holds a suffixed number out of
   range; SOAPY_HEAD_FREQ_LATE, with *out_idx at it, when the frequency place is empty and the field after it holds a
   suffixed number; and -1 when the head is not shorthand (opaque args). An empty first field is a device-args place
   left empty (`soapy::154.43M`): *out_arg_tokens counts only args that are there. */
static int
parse_soapy_head_tokens(char* tok[SOFTY_TOK_MAX], size_t n, size_t* out_arg_tokens, size_t* out_idx,
                        uint32_t* out_freq_hz) {
    if (!tok || !out_arg_tokens || !out_idx || !out_freq_hz || n == 0) {
        return -1;
    }

    size_t arg_tokens = 0;
    size_t idx = 0;
    if (looks_like_soapy_kv_args(tok[0])) {
        arg_tokens = 1;
        idx = 1;
    } else if (tok[0][0] == '\0') {
        idx = 1;
    }
    if (idx >= n) {
        return -1;
    }

    uint32_t freq_hz = 0;
    if (parse_freq_hz_token(tok[idx], &freq_hz) != 0) {
        return soapy_head_not_a_frequency(tok, n, idx, out_idx);
    }

    if (arg_tokens > 0 && (n - idx) == 1 && !has_freq_hint_chars(tok[idx])) {
        return -1;
    }

    *out_arg_tokens = arg_tokens;
    *out_idx = idx;
    *out_freq_hz = freq_hz;
    return 0;
}

/* The squelch field of a shorthand tuning tail whose frequency is at @p freq_idx, read before anything else of it: once
   the tail is shorthand, one --squelch would refuse refuses the spec (issue #899 B14), however many fields follow; it
   turned the whole tail into device args, which tuned the default frequency. Empty, or not there: nothing to read. */
static int
soapy_check_squelch(char* tok[SOFTY_TOK_MAX], size_t n, size_t freq_idx) {
    const size_t at = freq_idx + 4U;
    dsd_squelch_setting setting;
    if (at >= n || tok[at][0] == '\0') {
        return 0;
    }
    return dsd_spec_squelch_field_parse("soapy:", tok[at], &setting);
}

/* An integer field at *cur: 1 read into @p out, 0 when the tail stops before it or leaves it empty (the option stays),
   -1 when it does not parse. */
static int
parse_optional_int_at(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, int* out) {
    if (!tok || !cur || !out || *cur >= n) {
        return 0;
    }
    if (tok[*cur][0] == '\0') {
        (*cur)++;
        return 0;
    }
    if (parse_int_strict(tok[*cur], out) != 0) {
        return -1;
    }
    (*cur)++;
    return 1;
}

/* A spec's gain field, read as @p value: taken into *gain when its tenths (the gain times ten, as the tuner is set) fit
   an int, else ignored like a field that does not parse, with a warning naming @p token. A gain at or under 0 is
   automatic and is never multiplied. */
static void
spec_take_gain(int* gain, const char* token, int value) {
    if (value <= INT_MAX / 10) {
        *gain = value;
    } else if (*gain > 0) {
        LOG_WARN("WARNING: Ignoring gain '%s' (too large); gain left at %d dB\n", token, *gain);
    } else {
        LOG_WARN("WARNING: Ignoring gain '%s' (too large); gain left automatic\n", token);
    }
}

static int
parse_soapy_optional_front(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, int* gain, int* ppm, int* bw) {
    int iv = 0;
    int rc = parse_optional_int_at(tok, n, cur, &iv);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        spec_take_gain(gain, tok[*cur - 1], iv);
    }

    rc = parse_optional_int_at(tok, n, cur, &iv);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        *ppm = iv;
    }

    rc = parse_optional_int_at(tok, n, cur, &iv);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        *bw = dsd_analog_rtl_dsp_bw_or_default(iv);
    }

    return 0;
}

/* The squelch field in the squelch grammar (off, a level, auto[+N], noise[+N]), which soapy_check_squelch() has read:
   @p out points at its text, and stays NULL for an empty field or one the tail stops before. */
static void
parse_optional_squelch_at(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, const char** out) {
    if (!tok || !cur || !out || *cur >= n) {
        return;
    }
    if (tok[*cur][0] != '\0') {
        *out = tok[*cur];
    }
    (*cur)++;
}

static int
parse_soapy_optional_tail(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, const char** sql, int* vol) {
    parse_optional_squelch_at(tok, n, cur, sql);

    int iv = 0;
    const int rc = parse_optional_int_at(tok, n, cur, &iv);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        *vol = iv;
    }

    return 0;
}

static int
parse_soapy_optional_tokens(char* tok[SOFTY_TOK_MAX], size_t n, size_t start_idx, int* gain, int* ppm, int* bw,
                            const char** sql, int* vol) {
    if (!tok || !gain || !ppm || !bw || !sql || !vol) {
        return -1;
    }

    size_t cur = start_idx;
    if (parse_soapy_optional_front(tok, n, &cur, gain, ppm, bw) != 0) {
        return -1;
    }
    if (parse_soapy_optional_tail(tok, n, &cur, sql, vol) != 0) {
        return -1;
    }

    return (cur == n) ? 0 : -1;
}

/* What a Soapy tail is: the default device, opaque device args, a shorthand tuning tail, or refused. */
enum { SOAPY_TAIL_DEFAULT, SOAPY_TAIL_OPAQUE, SOAPY_TAIL_TUNING, SOAPY_TAIL_REFUSED };

/* Classify the tail of @p count fields, of which @p tok holds the first n (at most SOFTY_TOK_MAX). A tuning tail's
   device args count and frequency place go to *arg_tokens and *idx, its frequency to *freq_hz. */
static int
soapy_classify_tail(char* tok[SOFTY_TOK_MAX], size_t count, size_t* arg_tokens, size_t* idx, uint32_t* freq_hz) {
    /* The fields stored: a tail with more than a tuning tail holds is device args, once its squelch has been read. */
    const size_t n = count < SOFTY_TOK_MAX ? count : SOFTY_TOK_MAX;
    if (soapy_fields_all_empty(tok, n) && count <= SOFTY_TOK_MAX) {
        return SOAPY_TAIL_DEFAULT;
    }
    const int head_rc = parse_soapy_head_tokens(tok, n, arg_tokens, idx, freq_hz);
    if (head_rc == SOAPY_HEAD_FREQ_OUT_OF_RANGE) {
        LOG_ERROR("Soapy frequency '%s' is out of range (1 to 4294.967295 MHz)\n", tok[*idx]);
        return SOAPY_TAIL_REFUSED;
    }
    if (head_rc == SOAPY_HEAD_FREQ_LATE) {
        LOG_ERROR("Soapy frequency must follow the device args (field %zu, '%s', comes after an empty field): write "
                  "soapy:<args>:<freq>\n",
                  *idx + 1U, tok[*idx]);
        return SOAPY_TAIL_REFUSED;
    }
    if (head_rc == 0 && soapy_check_squelch(tok, n, *idx) != 0) {
        return SOAPY_TAIL_REFUSED;
    }
    return (head_rc != 0 || count > n) ? SOAPY_TAIL_OPAQUE : SOAPY_TAIL_TUNING;
}

/* A tuning tail's fields after the frequency into @p opts, and @p opts->audio_in_dev to its device args alone: 1, or 0
   with nothing changed when one does not parse (the caller keeps the tail as device args). */
static int
soapy_apply_tuning(dsd_opts* opts, char* tok[SOFTY_TOK_MAX], size_t n, size_t arg_tokens, size_t idx,
                   uint32_t freq_hz) {
    int gain = opts->rtl_gain_value;
    int ppm = opts->rtlsdr_ppm_error;
    int bw = -1; /* stays -1 when the spec has no bandwidth field */
    const char* sql = NULL;
    int vol = opts->rtl_volume_multiplier;
    if (parse_soapy_optional_tokens(tok, n, idx + 1, &gain, &ppm, &bw, &sql, &vol) != 0) {
        return 0;
    }

    opts->rtlsdr_center_freq = freq_hz;
    opts->rtl_gain_value = gain;
    opts->rtlsdr_ppm_error = ppm;
    if (bw != -1) {
        dsd_opts_set_rtl_dsp_bw(opts, bw, DSD_RTL_DSP_BW_SOURCE_EXPLICIT);
    }
    if (sql) {
        (void)dsd_squelch_spec_field_apply(opts, sql);
    }
    opts->rtl_volume_multiplier = dsd_opts_rtl_volume_multiplier_or_default(vol);

    if (arg_tokens > 0) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "soapy:%s", tok[0]);
    } else {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
    }
    opts->audio_in_dev[sizeof opts->audio_in_dev - 1] = '\0';
    return 1;
}

int
dsd_normalize_soapy_input_spec(dsd_opts* opts, int* out_tuning_applied) {
    if (out_tuning_applied) {
        *out_tuning_applied = 0;
    }
    if (!opts) {
        return -1;
    }

    if (strcmp(opts->audio_in_dev, "soapy") == 0) {
        return 0;
    }
    if (strncmp(opts->audio_in_dev, "soapy:", 6) != 0) {
        return 0;
    }

    const char* raw_tail = opts->audio_in_dev + 6;
    if (raw_tail[0] == '\0') {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
        return 0;
    }

    char tail_copy[sizeof opts->audio_in_dev];
    char scratch[sizeof opts->audio_in_dev];
    char* tok[SOFTY_TOK_MAX];
    size_t count = 0;
    if (tokenize_soapy_tail(raw_tail, tail_copy, sizeof tail_copy, scratch, sizeof scratch, tok, &count) != 0) {
        return 0;
    }

    size_t arg_tokens = 0;
    size_t idx = 0;
    uint32_t freq_hz = 0;
    switch (soapy_classify_tail(tok, count, &arg_tokens, &idx, &freq_hz)) {
        case SOAPY_TAIL_DEFAULT: DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy"); return 0;
        case SOAPY_TAIL_REFUSED: return -1;
        case SOAPY_TAIL_TUNING:
            if (soapy_apply_tuning(opts, tok, count, arg_tokens, idx, freq_hz)) {
                if (out_tuning_applied) {
                    *out_tuning_applied = 1;
                }
                return 0;
            }
            break;
        default: break;
    }
    restore_opaque_soapy_args(opts, tail_copy);
    return 0;
}

/* RTL-SDR and rtl_tcp specs ------------------------------------------------------------------------------------------
 * The parsing the engine used when the input opened, shared with --print-config. A tuning number with trailing text
 * keeps its leading digits, as strtol() reads it; the rtl_tcp port is read strictly (issue #743 B15). */

static int
rtl_spec_parse_int(const char* token, int* out) {
    if (!token || !out || token[0] == '\0') {
        return -1;
    }
    errno = 0;
    char* end = NULL;
    const long value = strtol(token, &end, 10);
    if (errno != 0 || end == token || value < INT_MIN || value > INT_MAX) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

/* A number at the start of token, as strtod() reads it; *consumed is the length of its text. */
static int
rtl_spec_parse_double(const char* token, double* out, size_t* consumed) {
    if (!token || !out || !consumed || token[0] == '\0') {
        return -1;
    }
    errno = 0;
    char* end = NULL;
    const double value = strtod(token, &end);
    if (errno != 0 || end == token) {
        return -1;
    }
    *out = value;
    *consumed = (size_t)(end - token);
    return 0;
}

/* The number at the start of text times 10^places (factor), rounded to the nearest Hz, not truncated; 0 when it does
   not parse or does not fit. A plain decimal number rounds half up in exact integer arithmetic. Any other number
   (a sign, an exponent, hex) rounds in binary64: a decimal MHz value is rarely exact in binary, and "128.155625M"
   multiplies out to 128155624.99999999, which a cast would tune 1 Hz low. */
static uint32_t
rtl_spec_round_hz(const char* text, unsigned places, double factor) {
    double value = 0.0;
    size_t consumed = 0;
    if (rtl_spec_parse_double(text, &value, &consumed) != 0) {
        return 0U;
    }
    uint32_t hz = 0;
    if (dsd_freq_decimal_round_hz(text, consumed, places, &hz)) {
        return hz;
    }
    const double rounded = value * factor + 0.5;
    if (!(rounded >= 0.5 && rounded < 4294967296.0)) {
        return 0U;
    }
    return (uint32_t)rounded;
}

/* A frequency field in Hz: Hz, or a number with a k, M or G suffix; 0 when it does not parse or does not fit. */
static uint32_t
rtl_spec_parse_freq(const char* s) {
    const size_t len = strlen(s);
    if (len == 0) {
        return 0U;
    }
    unsigned places = 0;
    double factor = 1.0;
    switch (s[len - 1]) {
        case 'g':
        case 'G':
            places = 9;
            factor = 1e9;
            break;
        case 'm':
        case 'M':
            places = 6;
            factor = 1e6;
            break;
        case 'k':
        case 'K':
            places = 3;
            factor = 1e3;
            break;
        default: return rtl_spec_round_hz(s, 0, 1.0);
    }
    if (len == 1) {
        return 0U;
    }
    char unitless[1024];
    if (len >= sizeof(unitless)) {
        return 0U;
    }
    DSD_MEMCPY(unitless, s, len - 1);
    unitless[len - 1] = '\0';
    return rtl_spec_round_hz(unitless, places, factor);
}

int
dsd_rtl_spec_bw_khz_or_default(const char* token) {
    int bw = 0;
    if (!token || rtl_spec_parse_int(token, &bw) != 0) {
        return DSD_ANALOG_RTL_DSP_BW_DEFAULT_KHZ;
    }
    return dsd_analog_rtl_dsp_bw_or_default(bw);
}

/* A trailing token: `bias` or `b` alone (or with an empty value) turns the bias tee on, and `bias=<value>` /
   `b=<value>` sets it from a whole boolean word (on/off, 1/0, true/false, yes/no). The bias tee puts DC on the antenna
   port, so a value or token it cannot read changes nothing and says so. */
static void
rtl_spec_apply_bias_token(dsd_opts* opts, const char* token) {
    const char* eq = strchr(token, '=');
    const size_t name_len = eq ? (size_t)(eq - token) : strlen(token);
    const int is_bias = (name_len == 4 && strncmp(token, "bias", 4) == 0) || (name_len == 1 && token[0] == 'b');
    if (!is_bias) {
        LOG_WARN("WARNING: Ignoring unknown RTL input option '%s' (expected bias[=on|off])\n", token);
        return;
    }
    if (!eq || eq[1] == '\0') {
        opts->rtl_bias_tee = 1;
        return;
    }
    int on = 0;
    if (dsd_parse_bool_strict(eq + 1, &on) != 0) {
        LOG_WARN("WARNING: Ignoring bias value '%s' (expected on/off, 1/0, true/false or yes/no); bias tee left %s\n",
                 eq + 1, opts->rtl_bias_tee ? "on" : "off");
        return;
    }
    opts->rtl_bias_tee = on;
}

/* The most fields an rtl or rtltcp spec is read to: the kind, the device or the endpoint, six tuning fields and bias
   tokens. */
#define RTL_SPEC_FIELDS_MAX 64

/* Field @p index when the spec has it and it is not empty; NULL when the spec leaves that place at its default. */
static const char*
rtl_spec_field(char* const* fields, int count, int index) {
    return (index < count && fields[index][0] != '\0') ? fields[index] : NULL;
}

/* 1 with field @p index read into @p out as rtl_spec_parse_int() reads it; 0 when the spec leaves the field out or it
   does not parse, which leaves its option. */
static int
rtl_spec_int_field(char* const* fields, int count, int index, int* out) {
    const char* field = rtl_spec_field(fields, count, index);
    return field && rtl_spec_parse_int(field, out) == 0;
}

/* The tuning fields from @p first on, in order (frequency, gain, PPM, DSP bandwidth, squelch, volume), then the bias
   tokens. An empty or missing field leaves its option, the DSP bandwidth its source too. */
static void
rtl_spec_apply_tuning_fields(dsd_opts* opts, char* const* fields, int count, int first) {
    const char* field = rtl_spec_field(fields, count, first);
    if (field) {
        opts->rtlsdr_center_freq = rtl_spec_parse_freq(field);
    }
    int parsed = 0;
    if (rtl_spec_int_field(fields, count, first + 1, &parsed)) {
        spec_take_gain(&opts->rtl_gain_value, fields[first + 1], parsed);
    }
    if (rtl_spec_int_field(fields, count, first + 2, &parsed)) {
        opts->rtlsdr_ppm_error = parsed;
    }
    field = rtl_spec_field(fields, count, first + 3);
    if (field) {
        dsd_opts_set_rtl_dsp_bw(opts, dsd_rtl_spec_bw_khz_or_default(field), DSD_RTL_DSP_BW_SOURCE_EXPLICIT);
    }
    /* The squelch grammar (off, a level, auto[+N], noise[+N]), which rtl_spec_check_squelch() has read; --squelch
       wins over the spec. */
    field = rtl_spec_field(fields, count, first + 4);
    if (field) {
        (void)dsd_squelch_spec_field_apply(opts, field);
    }
    if (rtl_spec_int_field(fields, count, first + 5, &parsed)) {
        opts->rtl_volume_multiplier = dsd_opts_rtl_volume_multiplier_or_default(parsed);
    }
    for (int i = first + 6; i < count; i++) {
        if (fields[i][0] != '\0') {
            rtl_spec_apply_bias_token(opts, fields[i]);
        }
    }
}

/* The squelch field of a spec whose tuning fields start at @p first, read before anything is set: one --squelch would
   refuse refuses the spec, which then changes nothing (issue #899 B14; it was ignored). An empty field leaves the
   squelch. */
static int
rtl_spec_check_squelch(const char* input, char* const* fields, int count, int first) {
    const char* field = rtl_spec_field(fields, count, first + 4);
    dsd_squelch_setting setting;
    return field && dsd_spec_squelch_field_parse(input, field, &setting) != 0 ? -1 : 0;
}

/* The endpoint of an rtltcp spec. The port is read before anything is set, so a spec it refuses changes nothing. */
static int
rtl_spec_apply_endpoint(dsd_opts* opts, char* const* fields, int count) {
    int port = opts->rtltcp_portno;
    const char* field = rtl_spec_field(fields, count, 2);
    if (field && dsd_spec_parse_port(field, 1, &port) != 0) {
        LOG_ERROR("Invalid port \"%s\" in %s (expected 1..%d)\n", field, opts->audio_in_dev, DSD_SPEC_PORT_MAX);
        return -1;
    }
    field = rtl_spec_field(fields, count, 1);
    if (field) {
        DSD_SNPRINTF(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", field);
    }
    opts->rtltcp_portno = port;
    return 0;
}

int
dsd_rtl_input_spec_apply(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    const int rtltcp = dsd_opts_audio_in_dev_is_rtltcp_spec(opts->audio_in_dev);
    if (!rtltcp && !dsd_opts_audio_in_dev_is_rtl_spec(opts->audio_in_dev)) {
        return 0;
    }
    char buf[sizeof opts->audio_in_dev];
    char* fields[RTL_SPEC_FIELDS_MAX];
    int count = dsd_spec_split_fields(opts->audio_in_dev, buf, sizeof buf, fields, RTL_SPEC_FIELDS_MAX);
    if (count < 0) {
        return -1;
    }
    if (count > RTL_SPEC_FIELDS_MAX) {
        count = RTL_SPEC_FIELDS_MAX;
    }
    const int first = rtltcp ? 3 : 2;
    if (rtl_spec_check_squelch(rtltcp ? "rtltcp:" : "rtl:", fields, count, first) != 0) {
        return -1;
    }
    if (rtltcp) {
        if (rtl_spec_apply_endpoint(opts, fields, count) != 0) {
            return -1;
        }
        rtl_spec_apply_tuning_fields(opts, fields, count, first);
        return 1;
    }
    int parsed = 0;
    if (rtl_spec_int_field(fields, count, 1, &parsed)) {
        opts->rtl_dev_index = parsed;
    }
    rtl_spec_apply_tuning_fields(opts, fields, count, first);
    return 1;
}

/* The longest spec an endpoint is read from: -i holds 2047 characters. */
#define ENDPOINT_SPEC_MAX 2048

int
dsd_endpoint_spec_read(const char* spec, int min_port, char* host, size_t host_size, int* port) {
    if (!spec || !host || host_size == 0U || !port) {
        return -1;
    }
    char buf[ENDPOINT_SPEC_MAX];
    char* fields[3];
    const int count = dsd_spec_split_fields(spec, buf, sizeof buf, fields, 3);
    if (count < 0) {
        LOG_ERROR("Invalid spec (longer than %d characters)\n", ENDPOINT_SPEC_MAX - 1);
        return -1;
    }
    const char* host_field = count > 1 ? fields[1] : "";
    const char* port_field = count > 2 ? fields[2] : "";
    int parsed = *port;
    if (port_field[0] != '\0' && dsd_spec_parse_port(port_field, min_port, &parsed) != 0) {
        LOG_ERROR("Invalid port \"%s\" in %s (expected %d..%d)\n", port_field, spec, min_port, DSD_SPEC_PORT_MAX);
        return -1;
    }
    if (strlen(host_field) >= host_size) {
        LOG_ERROR("Invalid host in %s (longer than %zu characters)\n", spec, host_size - 1U);
        return -1;
    }
    if (host_field[0] != '\0') {
        DSD_SNPRINTF(host, host_size, "%s", host_field);
    }
    *port = parsed;
    return 0;
}
