// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/freq_parse.h>
#include <dsd-neo/runtime/input_spec.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

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
parse_optional_double_at(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, double* out) {
    if (!tok || !cur || !out || *cur >= n) {
        return 0;
    }
    if (parse_double_strict(tok[*cur], out) != 0) {
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

static int
parse_soapy_optional_tail(char* tok[SOFTY_TOK_MAX], size_t n, size_t* cur, double* sql, int* vol) {
    double dv = 0.0;
    int rc = parse_optional_double_at(tok, n, cur, &dv);
    if (rc < 0) {
        return -1;
    }
    if (rc > 0) {
        *sql = (dv < 0.0) ? local_dB_to_pwr(dv) : dv;
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
                            double* sql, int* vol) {
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
    double sql = opts->rtl_squelch_level;
    int vol = opts->rtl_volume_multiplier;
    if (parse_soapy_optional_tokens(tok, n, idx + 1, &gain, &ppm, &bw, &sql, &vol) != 0) {
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
