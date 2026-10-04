// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/squelch.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

static int
is_valid_rtl_bw_khz(int bw) {
    return (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24 || bw == 48);
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

static int
parse_freq_hz_token(const char* tok, uint32_t* out_hz) {
    if (!tok || !out_hz || tok[0] == '\0') {
        return -1;
    }
    uint32_t hz = dsd_parse_freq_hz(tok);
    /* Use a conservative floor to avoid mis-parsing colon-delimited numeric args. */
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

static int
tokenize_soapy_tail(const char* raw_tail, char* tail_copy, size_t tail_copy_size, char* scratch, size_t scratch_size,
                    char* tok[SOFTY_TOK_MAX], size_t* out_count) {
    if (!raw_tail || !tail_copy || !scratch || !tok || !out_count || tail_copy_size == 0 || scratch_size == 0) {
        return -1;
    }

    DSD_SNPRINTF(tail_copy, tail_copy_size, "%s", raw_tail);
    tail_copy[tail_copy_size - 1] = '\0';
    DSD_SNPRINTF(scratch, scratch_size, "%s", raw_tail);
    scratch[scratch_size - 1] = '\0';

    size_t n = 0;
    char* saveptr = NULL;
    char* p = dsd_strtok_r(scratch, ":", &saveptr);
    while (p != NULL && n < SOFTY_TOK_MAX) {
        tok[n++] = p;
        p = dsd_strtok_r(NULL, ":", &saveptr);
    }
    if (p != NULL) {
        return -1;
    }

    *out_count = n;
    return 0;
}

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
    }
    if (idx >= n) {
        return -1;
    }

    uint32_t freq_hz = 0;
    if (parse_freq_hz_token(tok[idx], &freq_hz) != 0) {
        return -1;
    }

    if (arg_tokens > 0 && (n - idx) == 1 && !has_freq_hint_chars(tok[idx])) {
        return -1;
    }

    *out_arg_tokens = arg_tokens;
    *out_idx = idx;
    *out_freq_hz = freq_hz;
    return 0;
}

static int
parse_optional_int_at(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, int* out) {
    if (!tok || !cur || !out || *cur >= n) {
        return 0;
    }
    if (parse_int_strict(tok[*cur], out) != 0) {
        return -1;
    }
    (*cur)++;
    return 1;
}

static int
parse_soapy_optional_front(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, int* gain, int* ppm, int* bw) {
    int iv = 0;
    int rc = parse_optional_int_at(tok, n, cur, &iv);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        *gain = iv;
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
        *bw = is_valid_rtl_bw_khz(iv) ? iv : 48;
    }

    return 0;
}

/* The squelch field in the squelch grammar (off, a level, auto[+N], noise[+N]): @p out points at its text. */
static int
parse_optional_squelch_at(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, const char** out) {
    if (!tok || !cur || !out || *cur >= n) {
        return 0;
    }
    dsd_squelch_setting setting;
    if (dsd_squelch_setting_parse(tok[*cur], &setting, NULL, 0U) != 0) {
        return -1;
    }
    *out = tok[*cur];
    (*cur)++;
    return 1;
}

static int
parse_soapy_optional_tail(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, const char** sql, int* vol) {
    int rc = parse_optional_squelch_at(tok, n, cur, sql);
    if (rc < 0) {
        return -1;
    }

    int iv = 0;
    rc = parse_optional_int_at(tok, n, cur, &iv);
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
    size_t n = 0;
    if (tokenize_soapy_tail(raw_tail, tail_copy, sizeof tail_copy, scratch, sizeof scratch, tok, &n) != 0) {
        restore_opaque_soapy_args(opts, raw_tail);
        return 0;
    }
    if (n == 0) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
        return 0;
    }

    size_t arg_tokens = 0;
    size_t idx = 0;
    uint32_t freq_hz = 0;
    if (parse_soapy_head_tokens(tok, n, &arg_tokens, &idx, &freq_hz) != 0) {
        restore_opaque_soapy_args(opts, tail_copy);
        return 0;
    }

    int gain = opts->rtl_gain_value;
    int ppm = opts->rtlsdr_ppm_error;
    int bw = opts->rtl_dsp_bw_khz;
    const char* sql = NULL;
    int vol = opts->rtl_volume_multiplier;
    if (parse_soapy_optional_tokens(tok, n, idx + 1, &gain, &ppm, &bw, &sql, &vol) != 0) {
        restore_opaque_soapy_args(opts, tail_copy);
        return 0;
    }

    opts->rtlsdr_center_freq = freq_hz;
    opts->rtl_gain_value = gain;
    opts->rtlsdr_ppm_error = ppm;
    opts->rtl_dsp_bw_khz = bw;
    if (sql) {
        (void)dsd_squelch_spec_field_apply(opts, sql);
    }
    opts->rtl_volume_multiplier = vol;

    if (arg_tokens > 0) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "soapy:%s", tok[0]);
    } else {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
    }
    opts->audio_in_dev[sizeof opts->audio_in_dev - 1] = '\0';

    if (out_tuning_applied) {
        *out_tuning_applied = 1;
    }
    return 0;
}

/* RTL-SDR and rtl_tcp specs ------------------------------------------------------------------------------------------
 * The parsing the engine used when the input opened, kept as it was (a number with trailing text keeps its leading
 * digits, as strtol() reads it), now shared with --print-config. */

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

static int
rtl_spec_parse_double(const char* token, double* out) {
    if (!token || !out || token[0] == '\0') {
        return -1;
    }
    errno = 0;
    char* end = NULL;
    const double value = strtod(token, &end);
    if (errno != 0 || end == token) {
        return -1;
    }
    *out = value;
    return 0;
}

/* A frequency field: Hz, or a number with a k, M or G suffix; 0 when it does not parse. */
static double
rtl_spec_parse_freq(const char* s) {
    const size_t len = strlen(s);
    if (len == 0) {
        return 0.0;
    }
    double factor = 1.0;
    switch (s[len - 1]) {
        case 'g':
        case 'G': factor = 1e9; break;
        case 'm':
        case 'M': factor = 1e6; break;
        case 'k':
        case 'K': factor = 1e3; break;
        default: {
            double value = 0.0;
            return rtl_spec_parse_double(s, &value) == 0 ? value : 0.0;
        }
    }
    if (len == 1) {
        return 0.0;
    }
    char unitless[1024];
    if (len >= sizeof(unitless)) {
        return 0.0;
    }
    DSD_MEMCPY(unitless, s, len - 1);
    unitless[len - 1] = '\0';
    double value = 0.0;
    if (rtl_spec_parse_double(unitless, &value) != 0) {
        return 0.0;
    }
    return value * factor;
}

int
dsd_rtl_spec_bw_khz_or_default(const char* token) {
    int bw = 0;
    if (!token || rtl_spec_parse_int(token, &bw) != 0) {
        return 48;
    }
    if (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24 || bw == 48) {
        return bw;
    }
    return 48;
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

/* The tuning fields after the device or endpoint, in order. Returns 1 when all six were there (bias tokens may
   follow), 0 when the spec stopped early. */
static int
rtl_spec_apply_tuning_tokens(dsd_opts* opts, char** saveptr) {
    const char* curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    opts->rtlsdr_center_freq = (uint32_t)rtl_spec_parse_freq(curr);

    int parsed = 0;
    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    if (rtl_spec_parse_int(curr, &parsed) == 0) {
        opts->rtl_gain_value = parsed;
    }
    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    if (rtl_spec_parse_int(curr, &parsed) == 0) {
        opts->rtlsdr_ppm_error = parsed;
    }
    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    opts->rtl_dsp_bw_khz = dsd_rtl_spec_bw_khz_or_default(curr);
    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    /* The squelch grammar (off, a level, auto[+N], noise[+N]); a field that is not a squelch says nothing about it, and
       --squelch wins over the spec. */
    (void)dsd_squelch_spec_field_apply(opts, curr);
    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    if (rtl_spec_parse_int(curr, &parsed) == 0) {
        opts->rtl_volume_multiplier = parsed;
    }
    return 1;
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
    /* The 1023 characters the engine has always read a spec to (dsd_engine_setup_rtl_spec_bw_khz() too). */
    char inbuf[1024];
    DSD_SNPRINTF(inbuf, sizeof inbuf, "%s", opts->audio_in_dev);
    char* saveptr = NULL;
    if (dsd_strtok_r(inbuf, ":", &saveptr) == NULL) {
        return 1;
    }
    const char* curr = dsd_strtok_r(NULL, ":", &saveptr);
    if (!curr) {
        return 1;
    }
    int parsed = 0;
    if (rtltcp) {
        DSD_SNPRINTF(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", curr);
        curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (!curr) {
            return 1;
        }
        if (rtl_spec_parse_int(curr, &parsed) == 0) {
            opts->rtltcp_portno = parsed;
        }
    } else if (rtl_spec_parse_int(curr, &parsed) == 0) {
        opts->rtl_dev_index = parsed;
    }
    if (rtl_spec_apply_tuning_tokens(opts, &saveptr)) {
        while ((curr = dsd_strtok_r(NULL, ":", &saveptr)) != NULL) {
            rtl_spec_apply_bias_token(opts, curr);
        }
    }
    return 1;
}
