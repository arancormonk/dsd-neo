// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/runtime/input_spec.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"

static int
is_valid_rtl_bw_khz(int bw) {
    return (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24 || bw == 48);
}

static double
local_dB_to_pwr(double dB) {
    return pow(10.0, dB / 10.0);
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
parse_double_strict(const char* tok, double* out) {
    if (!tok || !out || tok[0] == '\0') {
        return -1;
    }
    errno = 0;
    char* end = NULL;
    double v = strtod(tok, &end);
    if (errno != 0 || end == tok || *end != '\0') {
        return -1;
    }
    *out = v;
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
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "soapy:%s", raw_tail);
    opts->audio_in_dev[sizeof opts->audio_in_dev - 1] = '\0';
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
        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
        return 0;
    }

    char tail_copy[sizeof opts->audio_in_dev];
    snprintf(tail_copy, sizeof tail_copy, "%s", raw_tail);
    tail_copy[sizeof tail_copy - 1] = '\0';

    char scratch[sizeof opts->audio_in_dev];
    snprintf(scratch, sizeof scratch, "%s", raw_tail);
    scratch[sizeof scratch - 1] = '\0';

    enum { TOK_MAX = 16 };

    char* tok[TOK_MAX];
    size_t n = 0;
    char* saveptr = NULL;
    char* p = dsd_strtok_r(scratch, ":", &saveptr);
    while (p != NULL && n < TOK_MAX) {
        tok[n++] = p;
        p = dsd_strtok_r(NULL, ":", &saveptr);
    }
    if (p != NULL) {
        restore_opaque_soapy_args(opts, tail_copy);
        return 0;
    }
    if (n == 0) {
        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
        return 0;
    }

    size_t arg_tokens = 0;
    size_t idx = 0;
    if (looks_like_soapy_kv_args(tok[0])) {
        arg_tokens = 1;
        idx = 1;
    }

    if (idx >= n) {
        restore_opaque_soapy_args(opts, tail_copy);
        return 0;
    }

    uint32_t freq_hz = 0;
    if (parse_freq_hz_token(tok[idx], &freq_hz) != 0) {
        restore_opaque_soapy_args(opts, tail_copy);
        return 0;
    }

    const size_t trailing_count = n - idx;
    if (arg_tokens > 0 && trailing_count == 1 && !has_freq_hint_chars(tok[idx])) {
        restore_opaque_soapy_args(opts, tail_copy);
        return 0;
    }

    int gain = opts->rtl_gain_value;
    int ppm = opts->rtlsdr_ppm_error;
    int bw = opts->rtl_dsp_bw_khz;
    double sql = opts->rtl_squelch_level;
    int vol = opts->rtl_volume_multiplier;

    size_t cur = idx + 1;
    int iv = 0;
    double dv = 0.0;

    if (cur < n) {
        if (parse_int_strict(tok[cur], &iv) != 0) {
            restore_opaque_soapy_args(opts, tail_copy);
            return 0;
        }
        gain = iv;
        cur++;
    }
    if (cur < n) {
        if (parse_int_strict(tok[cur], &iv) != 0) {
            restore_opaque_soapy_args(opts, tail_copy);
            return 0;
        }
        ppm = iv;
        cur++;
    }
    if (cur < n) {
        if (parse_int_strict(tok[cur], &iv) != 0) {
            restore_opaque_soapy_args(opts, tail_copy);
            return 0;
        }
        bw = is_valid_rtl_bw_khz(iv) ? iv : 48;
        cur++;
    }
    if (cur < n) {
        if (parse_double_strict(tok[cur], &dv) != 0) {
            restore_opaque_soapy_args(opts, tail_copy);
            return 0;
        }
        sql = (dv < 0.0) ? local_dB_to_pwr(dv) : dv;
        cur++;
    }
    if (cur < n) {
        if (parse_int_strict(tok[cur], &iv) != 0) {
            restore_opaque_soapy_args(opts, tail_copy);
            return 0;
        }
        vol = iv;
        cur++;
    }
    if (cur != n) {
        restore_opaque_soapy_args(opts, tail_copy);
        return 0;
    }

    opts->rtlsdr_center_freq = freq_hz;
    opts->rtl_gain_value = gain;
    opts->rtlsdr_ppm_error = ppm;
    opts->rtl_dsp_bw_khz = bw;
    opts->rtl_squelch_level = sql;
    opts->rtl_volume_multiplier = vol;

    if (arg_tokens > 0) {
        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "soapy:%s", tok[0]);
    } else {
        snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy");
    }
    opts->audio_in_dev[sizeof opts->audio_in_dev - 1] = '\0';

    if (out_tuning_applied) {
        *out_tuning_applied = 1;
    }
    return 0;
}
