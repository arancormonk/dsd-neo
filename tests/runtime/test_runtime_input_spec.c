// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/spec_fields.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

static char g_log_text[4096];

static void
capture_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)level;
    (void)ctx;
    size_t used = strlen(g_log_text);
    if (text && used < sizeof g_log_text - 1U) {
        DSD_SNPRINTF(g_log_text + used, sizeof g_log_text - used, "%s", text);
    }
}

static dsd_opts*
alloc_seeded_opts(void) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts) {
        return NULL;
    }
    opts->rtl_gain_value = 11;
    opts->rtlsdr_ppm_error = -7;
    opts->rtl_dsp_bw_khz = 48;
    opts->rtl_squelch_level = 0.25;
    opts->rtl_volume_multiplier = 3;
    opts->rtlsdr_center_freq = 155340000U;
    return opts;
}

static int
test_non_soapy_noop(void) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        DSD_FPRINTF(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:851.375M:22:-2:24:0:2");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "non-soapy noop returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        DSD_FPRINTF(stderr, "non-soapy noop applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "rtl:0:851.375M:22:-2:24:0:2") != 0) {
        DSD_FPRINTF(stderr, "non-soapy audio_in_dev mutated: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 11 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        DSD_FPRINTF(stderr, "non-soapy tuning fields changed unexpectedly\n");
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

static int
test_soapy_args_only_noop(void) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        DSD_FPRINTF(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=airspy,serial=ABC123");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "soapy args-only returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        DSD_FPRINTF(stderr, "soapy args-only applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=airspy,serial=ABC123") != 0) {
        DSD_FPRINTF(stderr, "soapy args-only audio_in_dev mutated: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 11 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        DSD_FPRINTF(stderr, "soapy args-only tuning fields changed unexpectedly\n");
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

static int
test_soapy_args_with_full_tuning(void) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        DSD_FPRINTF(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s",
                 "soapy:driver=airspy,serial=ABC123:851.375M:30:5:16:-50:2");

    int applied = 0;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "soapy full tuning returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 1) {
        DSD_FPRINTF(stderr, "soapy full tuning applied=%d expected 1\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=airspy,serial=ABC123") != 0) {
        DSD_FPRINTF(stderr, "soapy full tuning audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 851375000U || opts->rtl_gain_value != 30 || opts->rtlsdr_ppm_error != 5
        || opts->rtl_dsp_bw_khz != 16 || opts->rtl_volume_multiplier != 2) {
        DSD_FPRINTF(stderr, "soapy full tuning numeric fields mismatch\n");
        free(opts);
        return 1;
    }
    /* The `sql` field means the same thing here as in an rtl: string: a negative
     * value is decibels. This path had its own copy of the conversion, so the
     * exact mapping is pinned rather than just its order of magnitude. */
    if (fabs(opts->rtl_squelch_level - pow(10.0, -5.0)) > 1.0e-12) {
        DSD_FPRINTF(stderr, "soapy full tuning squelch expected dB->power mapping, got %.12f\n",
                    opts->rtl_squelch_level);
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

static int
test_soapy_no_args_tuning(void) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        DSD_FPRINTF(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:935.0125M:44:-3:24:0:3");

    int applied = 0;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "soapy no-args tuning returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 1) {
        DSD_FPRINTF(stderr, "soapy no-args tuning applied=%d expected 1\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy") != 0) {
        DSD_FPRINTF(stderr, "soapy no-args tuning audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 935012500U || opts->rtl_gain_value != 44 || opts->rtlsdr_ppm_error != -3
        || opts->rtl_dsp_bw_khz != 24 || opts->rtl_volume_multiplier != 3 || fabs(opts->rtl_squelch_level) > 1e-12) {
        DSD_FPRINTF(stderr, "soapy no-args tuning fields mismatch\n");
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

static int
test_soapy_args_colon_fallback(void) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        DSD_FPRINTF(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=foo:bar");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "soapy colon fallback returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        DSD_FPRINTF(stderr, "soapy colon fallback applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=foo:bar") != 0) {
        DSD_FPRINTF(stderr, "soapy colon fallback audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 11 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        DSD_FPRINTF(stderr, "soapy colon fallback tuning fields changed unexpectedly\n");
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

static int
test_soapy_numeric_colon_tail_fallback(void) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        DSD_FPRINTF(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=foo:1234567");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "soapy numeric-tail fallback returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        DSD_FPRINTF(stderr, "soapy numeric-tail fallback applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=foo:1234567") != 0) {
        DSD_FPRINTF(stderr, "soapy numeric-tail fallback audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

static int
test_soapy_args_partial_tuning(void) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        DSD_FPRINTF(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=sdrplay:851.375M:22");

    int applied = 0;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "soapy partial tuning returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 1) {
        DSD_FPRINTF(stderr, "soapy partial tuning applied=%d expected 1\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=sdrplay") != 0) {
        DSD_FPRINTF(stderr, "soapy partial tuning audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 851375000U || opts->rtl_gain_value != 22 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        DSD_FPRINTF(stderr, "soapy partial tuning fields mismatch\n");
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

static int
test_soapy_invalid_tuning_field_fallback(void) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        DSD_FPRINTF(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=airspy:851.375M:not_a_gain");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "soapy invalid-field fallback returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        DSD_FPRINTF(stderr, "soapy invalid-field fallback applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=airspy:851.375M:not_a_gain") != 0) {
        DSD_FPRINTF(stderr, "soapy invalid-field fallback audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 11 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        DSD_FPRINTF(stderr, "soapy invalid-field fallback tuning fields changed unexpectedly\n");
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

/* The sql field in the squelch grammar (issue #518 follow-up): auto[+N] in an rtl:, rtltcp: or soapy: spec sets the
 * auto squelch and keeps the level for a switch back; off and a level set LEVEL; --squelch (rtl_squelch_cli_set) wins
 * over the field; a field that is not a squelch leaves the setting alone. */
static int
expect_squelch(const char* label, const dsd_opts* opts, int mode, double level, int margin) {
    const int level_ok = fabs(opts->rtl_squelch_level - level) <= 1e-12 + (1e-9 * fabs(level));
    if (opts->rtl_squelch_mode != mode || !level_ok
        || (dsd_squelch_mode_is_dynamic(mode) && opts->rtl_squelch_margin_db != margin)) {
        DSD_FPRINTF(stderr, "%s: mode=%d level=%g margin=%d, want mode=%d level=%g margin=%d\n", label,
                    opts->rtl_squelch_mode, opts->rtl_squelch_level, opts->rtl_squelch_margin_db, mode, level, margin);
        return 1;
    }
    return 0;
}

static int
test_spec_squelch_grammar(void) {
    int rc = 0;
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:auto+6:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtl: auto+6", opts, DSD_SQUELCH_MODE_AUTO, 0.25, 6);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:-60:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtl: -60", opts, DSD_SQUELCH_MODE_LEVEL, 1e-6, 0);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp:127.0.0.1:1234:162.475M:30:-2:48:auto:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtltcp: auto", opts, DSD_SQUELCH_MODE_AUTO, 1e-6, DSD_SQUELCH_MARGIN_DEFAULT_DB);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:off:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtl: off", opts, DSD_SQUELCH_MODE_LEVEL, 0.0, 0);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:noise+14:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtl: noise+14", opts, DSD_SQUELCH_MODE_NOISE, 0.0, 14);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:off:2");
    (void)dsd_rtl_input_spec_apply(opts);
    /* Not a squelch: nothing changes. */
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:loud:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtl: loud", opts, DSD_SQUELCH_MODE_LEVEL, 0.0, 0);
    /* --squelch wins. */
    opts->rtl_squelch_cli_set = 1;
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts->rtl_squelch_margin_db = 12;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:-60:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtl: -60 under --squelch", opts, DSD_SQUELCH_MODE_AUTO, 0.0, 12);
    free(opts);

    /* soapy: the same field, and the spec still parses as tuning with it. */
    dsd_opts* soapy = alloc_seeded_opts();
    if (!soapy) {
        return 1;
    }
    DSD_SNPRINTF(soapy->audio_in_dev, sizeof soapy->audio_in_dev, "%s",
                 "soapy:driver=rtlsdr:162.475M:30:-2:48:auto+8:2");
    int applied = 0;
    if (dsd_normalize_soapy_input_spec(soapy, &applied) != 0 || applied != 1
        || strcmp(soapy->audio_in_dev, "soapy:driver=rtlsdr") != 0 || soapy->rtl_volume_multiplier != 2) {
        DSD_FPRINTF(stderr, "soapy auto+8: applied=%d dev=%s vol=%d\n", applied, soapy->audio_in_dev,
                    soapy->rtl_volume_multiplier);
        rc = 1;
    }
    rc |= expect_squelch("soapy: auto+8", soapy, DSD_SQUELCH_MODE_AUTO, 0.25, 8);
    free(soapy);
    return rc;
}

/* The bandwidth field takes the selectable DSP bandwidths, 72 kHz (TETRA's) among them, and reads anything else as the
   48 kHz default, in rtl:, rtltcp: and soapy: specs alike. */
static int
test_spec_bandwidth_field(void) {
    static const struct {
        const char* token;
        int want_khz;
    } cases[] = {
        {"4", 4},   {"24", 24}, {"48", 48}, {"72", 72},  {"5", 48},
        {"54", 48}, {"96", 48}, {"0", 48},  {"-72", 48}, {"", 48},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const int got = dsd_rtl_spec_bw_khz_or_default(cases[i].token);
        if (got != cases[i].want_khz) {
            DSD_FPRINTF(stderr, "bandwidth field \"%s\": got %d, want %d\n", cases[i].token, got, cases[i].want_khz);
            rc = 1;
        }
    }
    if (dsd_rtl_spec_bw_khz_or_default(NULL) != 48) {
        DSD_FPRINTF(stderr, "bandwidth field NULL: want 48\n");
        rc = 1;
    }

    static const char* const specs[] = {"rtl:0:420.1M:30:0:72:0:2", "rtltcp:127.0.0.1:1234:420.1M:30:0:72:0:2"};
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        dsd_opts* opts = alloc_seeded_opts();
        if (!opts) {
            return 1;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", specs[i]);
        if (dsd_rtl_input_spec_apply(opts) != 1 || opts->rtl_dsp_bw_khz != 72) {
            DSD_FPRINTF(stderr, "%s: bw=%d, want 72\n", specs[i], opts->rtl_dsp_bw_khz);
            rc = 1;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", specs[i]);
        /* The same spec at an unsupported bandwidth runs at the default. */
        char* field = strstr(opts->audio_in_dev, ":72:");
        if (field) {
            field[1] = '6';
            field[2] = '0';
        }
        if (dsd_rtl_input_spec_apply(opts) != 1 || opts->rtl_dsp_bw_khz != 48) {
            DSD_FPRINTF(stderr, "%s at 60: bw=%d, want 48\n", specs[i], opts->rtl_dsp_bw_khz);
            rc = 1;
        }
        free(opts);
    }

    dsd_opts* soapy = alloc_seeded_opts();
    if (!soapy) {
        return 1;
    }
    DSD_SNPRINTF(soapy->audio_in_dev, sizeof soapy->audio_in_dev, "%s", "soapy:driver=rtlsdr:420.1M:30:0:72:0:2");
    int applied = 0;
    if (dsd_normalize_soapy_input_spec(soapy, &applied) != 0 || applied != 1 || soapy->rtl_dsp_bw_khz != 72) {
        DSD_FPRINTF(stderr, "soapy at 72: applied=%d bw=%d\n", applied, soapy->rtl_dsp_bw_khz);
        rc = 1;
    }
    DSD_SNPRINTF(soapy->audio_in_dev, sizeof soapy->audio_in_dev, "%s", "soapy:driver=rtlsdr:420.1M:30:0:60:0:2");
    if (dsd_normalize_soapy_input_spec(soapy, &applied) != 0 || applied != 1 || soapy->rtl_dsp_bw_khz != 48) {
        DSD_FPRINTF(stderr, "soapy at 60: applied=%d bw=%d\n", applied, soapy->rtl_dsp_bw_khz);
        rc = 1;
    }
    free(soapy);
    return rc;
}

/* A frequency field is rounded to the nearest Hz, never truncated: a decimal MHz value is rarely exact in binary, and
   "128.155625M" multiplies out to 128155624.99999999, which a cast tuned 1 Hz low. The Qt app saves frequencies to
   the Hz in this form. A value that does not fit a uint32_t reads as 0, as an unparsed one does. */
static int
test_spec_frequency_rounds_to_the_hz(void) {
    static const char* const prefixes[] = {"rtl:0:", "rtltcp:127.0.0.1:1234:"};
    int rc = 0;
    for (size_t p = 0; p < sizeof prefixes / sizeof prefixes[0]; p++) {
        dsd_opts* opts = alloc_seeded_opts();
        if (!opts) {
            return 1;
        }
        /* Every Hz across a stretch of the 128-256 MHz octave, where the products land just under the integer, and
           a 10 Hz raster of the five-decimal values saved before. */
        int misses = 0;
        for (uint32_t hz = 128150000U; hz < 128170000U; hz++) {
            DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s%u.%06uM:30:0:48:0:2", prefixes[p],
                         hz / 1000000U, hz % 1000000U);
            if (dsd_rtl_input_spec_apply(opts) != 1 || opts->rtlsdr_center_freq != hz) {
                if (misses++ < 3) {
                    DSD_FPRINTF(stderr, "%s: tuned %u, want %u\n", opts->audio_in_dev, opts->rtlsdr_center_freq, hz);
                }
            }
        }
        for (uint32_t hz = 128000000U; hz < 130000000U; hz += 10U) {
            DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s%u.%05uM:30:0:48:0:2", prefixes[p],
                         hz / 1000000U, (hz % 1000000U) / 10U);
            if (dsd_rtl_input_spec_apply(opts) != 1 || opts->rtlsdr_center_freq != hz) {
                if (misses++ < 6) {
                    DSD_FPRINTF(stderr, "%s: tuned %u, want %u\n", opts->audio_in_dev, opts->rtlsdr_center_freq, hz);
                }
            }
        }
        if (misses != 0) {
            DSD_FPRINTF(stderr, "%s: %d frequencies missed their Hz\n", prefixes[p], misses);
            rc = 1;
        }

        static const struct {
            const char* field;
            uint32_t want;
        } edges[] = {
            {"128155625", 128155625U},
            {"128155.625k", 128155625U},
            {"0.128155625G", 128155625U},
            {"4294967295", 4294967295U},
            {"4294967296", 0U},
            {"4294967295.4999999", 4294967295U},
            {"4294967295.5", 0U},
            {"-5M", 0U},
            {"nanM", 0U},
            {"junk", 0U},
        };

        for (size_t i = 0; i < sizeof edges / sizeof edges[0]; i++) {
            DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s%s:30:0:48:0:2", prefixes[p],
                         edges[i].field);
            opts->rtlsdr_center_freq = 155340000U;
            (void)dsd_rtl_input_spec_apply(opts);
            if (opts->rtlsdr_center_freq != edges[i].want) {
                DSD_FPRINTF(stderr, "%s: tuned %u, want %u\n", opts->audio_in_dev, opts->rtlsdr_center_freq,
                            edges[i].want);
                rc = 1;
            }
        }
        free(opts);
    }
    return rc;
}

/* Issue #669 B10: a monitor gain outside 0..3 in an rtl:, rtltcp: or soapy: spec tail reads as the default 1, as the
   local RTL setup and the menu always read it; 0..3 stay as written. */
static int
test_spec_monitor_volume_range(void) {
    int rc = 0;

    static const struct {
        const char* spec;
        int want;
    } cases[] = {
        {"rtl:0:162.475M:30:-2:48:off:10", 1},
        {"rtl:0:162.475M:30:-2:48:off:-1", 1},
        {"rtl:0:162.475M:30:-2:48:off:0", 0},
        {"rtl:0:162.475M:30:-2:48:off:3", 3},
        {"rtltcp:127.0.0.1:1234:162.475M:30:-2:48:off:9", 1},
        {"rtltcp:127.0.0.1:1234:162.475M:30:-2:48:off:2", 2},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_opts* opts = alloc_seeded_opts();
        if (!opts) {
            return 1;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", cases[i].spec);
        (void)dsd_rtl_input_spec_apply(opts);
        if (opts->rtl_volume_multiplier != cases[i].want) {
            DSD_FPRINTF(stderr, "%s: volume %d, want %d\n", cases[i].spec, opts->rtl_volume_multiplier, cases[i].want);
            rc |= 1;
        }
        free(opts);
    }

    static const struct {
        const char* spec;
        int want;
    } soapy[] = {{"soapy:935.0125M:44:-3:24:0:10", 1}, {"soapy:driver=rtlsdr:935.0125M:44:-3:24:0:2", 2}};

    for (size_t i = 0; i < sizeof soapy / sizeof soapy[0]; i++) {
        dsd_opts* opts = alloc_seeded_opts();
        if (!opts) {
            return 1;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", soapy[i].spec);
        int applied = 0;
        (void)dsd_normalize_soapy_input_spec(opts, &applied);
        if (applied != 1 || opts->rtl_volume_multiplier != soapy[i].want) {
            DSD_FPRINTF(stderr, "%s: applied %d volume %d, want %d\n", soapy[i].spec, applied,
                        opts->rtl_volume_multiplier, soapy[i].want);
            rc |= 1;
        }
        free(opts);
    }
    return rc;
}

/* Issue #670 B2: a number with a K, M or G suffix in the frequency place is a frequency, never device args. One out of
   range (refused, as 5.8G is above 4294967295 Hz, or under the 1 MHz floor, as 500k is) fails normalization with a
   message naming it, where it once fell back to opaque args and opened the device with "5.8G" as its arguments. The
   number is anything strtod() reads, as the frequency parser does: a sign or an exponent is still a frequency. */
static int
test_soapy_suffixed_frequency_out_of_range(void) {
    static const struct {
        const char* spec;
        const char* tok;
    } cases[] = {
        {"soapy:5.8G", "5.8G"},   {"soapy:driver=x:5.8G", "5.8G"},
        {"soapy:500k", "500k"},   {"soapy:driver=x:500k:30", "500k"},
        {"soapy:0.9M", "0.9M"},   {"soapy:4294.9672956M", "4294.9672956M"},
        {"soapy:+5.8G", "+5.8G"}, {"soapy:driver=hackrf:5.8e0G", "5.8e0G"},
        {"soapy:-5M", "-5M"},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsd_opts* opts = alloc_seeded_opts();
        if (!opts) {
            return 1;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", cases[i].spec);
        g_log_text[0] = '\0';
        int applied = 99;
        int nrc = dsd_normalize_soapy_input_spec(opts, &applied);
        char want[128];
        DSD_SNPRINTF(want, sizeof want, "Soapy frequency '%s' is out of range (1 to 4294.967295 MHz)", cases[i].tok);
        if (nrc == 0 || applied != 0) {
            DSD_FPRINTF(stderr, "%s: rc=%d applied=%d, want an error\n", cases[i].spec, nrc, applied);
            rc = 1;
        }
        if (!strstr(g_log_text, want)) {
            DSD_FPRINTF(stderr, "%s: log '%s' lacks '%s'\n", cases[i].spec, g_log_text, want);
            rc = 1;
        }
        if (strcmp(opts->audio_in_dev, cases[i].spec) != 0 || opts->rtlsdr_center_freq != 155340000U
            || opts->rtl_gain_value != 11) {
            DSD_FPRINTF(stderr, "%s: refused spec changed opts dev=%s freq=%u gain=%d\n", cases[i].spec,
                        opts->audio_in_dev, opts->rtlsdr_center_freq, opts->rtl_gain_value);
            rc = 1;
        }
        free(opts);
    }

    /* The in-range edges still tune. */
    static const struct {
        const char* spec;
        uint32_t hz;
    } ok[] = {
        {"soapy:1M", 1000000U},
        {"soapy:driver=x:4294.967295M", 4294967295U},
        {"soapy:driver=x:+851.375M", 851375000U},
    };

    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        dsd_opts* opts = alloc_seeded_opts();
        if (!opts) {
            return 1;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", ok[i].spec);
        int applied = 0;
        int nrc = dsd_normalize_soapy_input_spec(opts, &applied);
        if (nrc != 0 || applied != 1 || opts->rtlsdr_center_freq != ok[i].hz) {
            DSD_FPRINTF(stderr, "%s: rc=%d applied=%d freq=%u, want %u\n", ok[i].spec, nrc, applied,
                        opts->rtlsdr_center_freq, ok[i].hz);
            rc = 1;
        }
        free(opts);
    }
    return rc;
}

/* Apply @p spec (rtl:, rtltcp: or soapy:) to seeded opts with an empty log: whether it ran as one, the gain it left,
   its PPM and the log. */
static int
apply_gain_spec(const char* spec, int* out_gain, int* out_ppm) {
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        return -1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", spec);
    g_log_text[0] = '\0';
    int ok = 0;
    if (strncmp(spec, "soapy:", 6) == 0) {
        int applied = 0;
        ok = dsd_normalize_soapy_input_spec(opts, &applied) == 0 && applied == 1
             && opts->rtlsdr_center_freq == 851000000U;
    } else {
        ok = dsd_rtl_input_spec_apply(opts) == 1 && opts->rtlsdr_center_freq == 851000000U;
    }
    *out_gain = opts->rtl_gain_value;
    *out_ppm = opts->rtlsdr_ppm_error;
    free(opts);
    return ok ? 0 : -1;
}

/* A gain whose tenths (the gain times ten, as the tuner is set) do not fit an int is refused like a field that does not
   parse: the gain set before stays, with a warning naming the value, and the fields after it still apply. Anything
   that fits is taken as written, 0 and negatives (automatic) included. */
static int
test_spec_gain_overflow_refused(void) {
    static const char* const refused[] = {
        "rtl:0:851M:2147483647",
        "rtl:0:851M:214748365",
        "rtl:0:851M:2147483647:5:24:0:2",
        "rtltcp:h:1234:851M:2147483647",
        "rtltcp:h:1234:851M:2147483647:5:24:0:2",
        "soapy::851M:2147483647",
        "soapy:851M:214748365",
        "soapy:driver=rtlsdr:851M:2147483647:5:24:0:2",
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        int gain = 0;
        int ppm = 0;
        if (apply_gain_spec(refused[i], &gain, &ppm) != 0) {
            DSD_FPRINTF(stderr, "%s: did not apply\n", refused[i]);
            rc = 1;
            continue;
        }
        const int want_ppm = strstr(refused[i], ":5:24:") ? 5 : -7;
        if (gain != 11 || ppm != want_ppm) {
            DSD_FPRINTF(stderr, "%s: gain %d ppm %d, want 11 and %d\n", refused[i], gain, ppm, want_ppm);
            rc = 1;
        }
        const char* value = strstr(refused[i], "214748365") ? "214748365" : "2147483647";
        char want[96];
        DSD_SNPRINTF(want, sizeof want, "Ignoring gain '%s' (too large); gain left at 11 dB", value);
        if (!strstr(g_log_text, want)) {
            DSD_FPRINTF(stderr, "%s: log '%s' lacks '%s'\n", refused[i], g_log_text, want);
            rc = 1;
        }
    }

    static const struct {
        const char* spec;
        int want;
    } kept[] = {
        {"rtl:0:851M:214748364", 214748364},
        {"rtltcp:h:1234:851M:214748364", 214748364},
        {"soapy:851M:214748364", 214748364},
        {"rtl:0:851M:0", 0},
        {"rtl:0:851M:-5", -5},
        {"rtl:0:851M:-2147483648", INT_MIN},
        {"rtltcp:h:1234:851M:0", 0},
        {"rtltcp:h:1234:851M:-2147483648", INT_MIN},
        {"soapy:851M:0", 0},
        {"soapy:851M:-2147483648", INT_MIN},
        {"rtl:0:851M:30", 30},
    };

    for (size_t i = 0; i < sizeof kept / sizeof kept[0]; i++) {
        int gain = 0;
        int ppm = 0;
        if (apply_gain_spec(kept[i].spec, &gain, &ppm) != 0 || gain != kept[i].want) {
            DSD_FPRINTF(stderr, "%s: gain %d, want %d\n", kept[i].spec, gain, kept[i].want);
            rc = 1;
        }
        if (strstr(g_log_text, "Ignoring gain")) {
            DSD_FPRINTF(stderr, "%s: unexpected warning '%s'\n", kept[i].spec, g_log_text);
            rc = 1;
        }
    }

    /* Kept at automatic, the warning says so. */
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        return 1;
    }
    opts->rtl_gain_value = 0;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:851M:2147483647");
    g_log_text[0] = '\0';
    (void)dsd_rtl_input_spec_apply(opts);
    if (opts->rtl_gain_value != 0
        || !strstr(g_log_text, "Ignoring gain '2147483647' (too large); gain left automatic")) {
        DSD_FPRINTF(stderr, "automatic kept: gain %d log '%s'\n", opts->rtl_gain_value, g_log_text);
        rc = 1;
    }
    free(opts);
    return rc;
}

/* Issue #743 B7: an empty field keeps its place and leaves its option at the default the options hold: `rtl::851M`
   read the frequency as the device and every later field one place early (`rtl::851.0125M:20:3` tuned 20 Hz at gain
   3), and `rtltcp:host::851M` read the frequency as the port. */
static int
test_spec_empty_device_keeps_its_place(void) {
    int rc = 0;
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        return 1;
    }
    opts->rtl_dev_index = 2;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl::851.0125M:20:3");
    if (dsd_rtl_input_spec_apply(opts) != 1 || opts->rtl_dev_index != 2 || opts->rtlsdr_center_freq != 851012500U
        || opts->rtl_gain_value != 20 || opts->rtlsdr_ppm_error != 3 || opts->rtl_dsp_bw_khz != 48) {
        DSD_FPRINTF(stderr, "rtl::851.0125M:20:3: dev %d freq %u gain %d ppm %d bw %d\n", opts->rtl_dev_index,
                    opts->rtlsdr_center_freq, opts->rtl_gain_value, opts->rtlsdr_ppm_error, opts->rtl_dsp_bw_khz);
        rc = 1;
    }
    free(opts);
    return rc;
}

static int
test_spec_empty_port_keeps_its_place(void) {
    int rc = 0;
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        return 1;
    }
    opts->rtltcp_portno = 1234;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp:127.0.0.1::851.0125M:20:0");
    if (dsd_rtl_input_spec_apply(opts) != 1 || strcmp(opts->rtltcp_hostname, "127.0.0.1") != 0
        || opts->rtltcp_portno != 1234 || opts->rtlsdr_center_freq != 851012500U || opts->rtl_gain_value != 20
        || opts->rtlsdr_ppm_error != 0) {
        DSD_FPRINTF(stderr, "rtltcp:127.0.0.1::851.0125M:20:0: host %s port %d freq %u gain %d ppm %d\n",
                    opts->rtltcp_hostname, opts->rtltcp_portno, opts->rtlsdr_center_freq, opts->rtl_gain_value,
                    opts->rtlsdr_ppm_error);
        rc = 1;
    }
    free(opts);
    return rc;
}

/* Empty tuning fields between set ones: each keeps what the options held, the DSP bandwidth its source too. */
static int
test_spec_empty_tuning_fields_keep_the_options(void) {
    int rc = 0;
    dsd_opts* opts = alloc_seeded_opts();
    if (!opts) {
        return 1;
    }
    DSD_SNPRINTF(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", "seed.example");
    opts->rtltcp_portno = 1234;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp::5000::22::::2:");
    g_log_text[0] = '\0';
    if (dsd_rtl_input_spec_apply(opts) != 1 || strcmp(opts->rtltcp_hostname, "seed.example") != 0
        || opts->rtltcp_portno != 5000 || opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 22
        || opts->rtlsdr_ppm_error != -7 || opts->rtl_dsp_bw_khz != 48
        || opts->rtl_dsp_bw_source == DSD_RTL_DSP_BW_SOURCE_EXPLICIT || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 2 || g_log_text[0] != '\0') {
        DSD_FPRINTF(stderr, "rtltcp::5000::22::::2: host %s port %d freq %u gain %d ppm %d bw %d vol %d log '%s'\n",
                    opts->rtltcp_hostname, opts->rtltcp_portno, opts->rtlsdr_center_freq, opts->rtl_gain_value,
                    opts->rtlsdr_ppm_error, opts->rtl_dsp_bw_khz, opts->rtl_volume_multiplier, g_log_text);
        rc = 1;
    }
    free(opts);
    return rc;
}

static int
test_spec_empty_fields_keep_their_place(void) {
    return test_spec_empty_device_keeps_its_place() | test_spec_empty_port_keeps_its_place()
           | test_spec_empty_tuning_fields_keep_the_options();
}

/* Issue #743 B15: an rtl_tcp port that is not a whole number from 1 to 65535 is refused with a message naming it, and
   the spec changes nothing; it was narrowed to 16 bits (99999 connected to 34463), or read as its leading digits (12x
   as 12). An empty port keeps the default (test_spec_empty_fields_keep_their_place()). */
static int
test_spec_rtltcp_port_refused(void) {
    int rc = 0;
    static const char* const refused[] = {"12x", "99999", "65536", "0", "-1", "0x50", "abc"};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        dsd_opts* opts = alloc_seeded_opts();
        if (!opts) {
            return 1;
        }
        DSD_SNPRINTF(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", "seed.example");
        opts->rtltcp_portno = 1234;
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:radio.example:%s:851M", refused[i]);
        g_log_text[0] = '\0';
        char quoted[32];
        DSD_SNPRINTF(quoted, sizeof quoted, "\"%s\"", refused[i]);
        if (dsd_rtl_input_spec_apply(opts) != -1 || opts->rtltcp_portno != 1234
            || strcmp(opts->rtltcp_hostname, "seed.example") != 0 || opts->rtlsdr_center_freq != 155340000U
            || !strstr(g_log_text, quoted) || !strstr(g_log_text, "65535")) {
            DSD_FPRINTF(stderr, "port %s: host %s port %d freq %u log '%s'\n", refused[i], opts->rtltcp_hostname,
                        opts->rtltcp_portno, opts->rtlsdr_center_freq, g_log_text);
            rc = 1;
        }
        free(opts);
    }
    static const int accepted[] = {1, 65535};
    for (size_t i = 0; i < sizeof accepted / sizeof accepted[0]; i++) {
        dsd_opts* opts = alloc_seeded_opts();
        if (!opts) {
            return 1;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:radio.example:%d", accepted[i]);
        if (dsd_rtl_input_spec_apply(opts) != 1 || opts->rtltcp_portno != accepted[i]) {
            DSD_FPRINTF(stderr, "port %d: got %d\n", accepted[i], opts->rtltcp_portno);
            rc = 1;
        }
        free(opts);
    }
    return rc;
}

/* The field splitter every network and RTL-family spec reads through keeps empty fields in their places. */
static int
test_split_fields_keeps_empty_fields(void) {
    int rc = 0;

    static const struct {
        const char* spec;
        int count;
        const char* fields[4];
    } cases[] = {
        {"rtl::851M", 3, {"rtl", "", "851M", NULL}},
        {"udp", 1, {"udp", NULL, NULL, NULL}},
        {"", 1, {"", NULL, NULL, NULL}},
        {"tcp:", 2, {"tcp", "", NULL, NULL}},
        {"::", 3, {"", "", "", NULL}},
        {"a:b:c:d", 4, {"a", "b", "c", "d"}},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char buf[64];
        char* fields[4] = {NULL, NULL, NULL, NULL};
        const int count = dsd_spec_split_fields(cases[i].spec, buf, sizeof buf, fields, 4);
        int ok = count == cases[i].count;
        for (int f = 0; ok && f < count; f++) {
            ok = fields[f] && strcmp(fields[f], cases[i].fields[f]) == 0;
        }
        if (!ok) {
            DSD_FPRINTF(stderr, "split '%s': %d fields, want %d\n", cases[i].spec, count, cases[i].count);
            rc = 1;
        }
    }
    /* More fields than room: the count says so, and the first ones are stored. */
    char buf[64];
    char* fields[2] = {NULL, NULL};
    if (dsd_spec_split_fields("a:b:c", buf, sizeof buf, fields, 2) != 3 || strcmp(fields[0], "a") != 0
        || strcmp(fields[1], "b") != 0) {
        DSD_FPRINTF(stderr, "split past max_fields\n");
        rc = 1;
    }
    char small[4];
    if (dsd_spec_split_fields("abcd", small, sizeof small, fields, 2) != -1
        || dsd_spec_split_fields(NULL, buf, sizeof buf, fields, 2) != -1
        || dsd_spec_split_fields("a", buf, sizeof buf, fields, 0) != -1) {
        DSD_FPRINTF(stderr, "split refusals\n");
        rc = 1;
    }
    return rc;
}

static int
test_spec_parse_port(void) {
    int rc = 0;

    static const struct {
        const char* text;
        int min_port;
        int want;
    } accepted[] = {{"1", 1, 1}, {"65535", 1, 65535}, {"0", 0, 0}, {"007355", 1, 7355}};

    for (size_t i = 0; i < sizeof accepted / sizeof accepted[0]; i++) {
        int port = -5;
        if (dsd_spec_parse_port(accepted[i].text, accepted[i].min_port, &port) != 0 || port != accepted[i].want) {
            DSD_FPRINTF(stderr, "port '%s': got %d\n", accepted[i].text, port);
            rc = 1;
        }
    }
    static const char* const refused[] = {"", "0", "65536", "99999", "-1", "+80", " 80", "80 ", "12x", "0x50", "1e3"};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        int port = -5;
        if (dsd_spec_parse_port(refused[i], 1, &port) != -1 || port != -5) {
            DSD_FPRINTF(stderr, "port '%s' not refused (%d)\n", refused[i], port);
            rc = 1;
        }
    }
    return rc;
}

/* `kind[:host[:port]]`: an empty or missing field keeps the caller's default; a bad port refuses the spec unchanged. */
static int
test_endpoint_spec_read(void) {
    int rc = 0;

    static const struct {
        const char* spec;
        int min_port;
        int result;
        const char* host;
        int port;
    } cases[] = {
        {"tcp:radio.example:7356", 1, 0, "radio.example", 7356},
        {"udp::17355", 0, 0, "default", 17355},
        {"udp:0.0.0.0:", 0, 0, "0.0.0.0", 1000},
        {"udp:0.0.0.0:0", 0, 0, "0.0.0.0", 0},
        {"udp", 0, 0, "default", 1000},
        {"m17udp::", 1, 0, "default", 1000},
        {"tcp:h:7356:ignored", 1, 0, "h", 7356},
        {"tcp:h:0", 1, -1, "default", 1000},
        {"tcp:h:12x", 1, -1, "default", 1000},
        {"udp:h:105659", 0, -1, "default", 1000},
        {"tcp:a-host-name-longer-than-sixteen:1", 1, -1, "default", 1000},
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char host[16];
        DSD_SNPRINTF(host, sizeof host, "%s", "default");
        int port = 1000;
        g_log_text[0] = '\0';
        const int result = dsd_endpoint_spec_read(cases[i].spec, cases[i].min_port, host, sizeof host, &port);
        if (result != cases[i].result || strcmp(host, cases[i].host) != 0 || port != cases[i].port
            || (result != 0 && !strstr(g_log_text, cases[i].spec))) {
            DSD_FPRINTF(stderr, "endpoint '%s': %d host %s port %d log '%s'\n", cases[i].spec, result, host, port,
                        g_log_text);
            rc = 1;
        }
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    dsd_neo_log_set_tap(capture_log, NULL);
    rc |= test_spec_gain_overflow_refused();
    rc |= test_spec_frequency_rounds_to_the_hz();
    rc |= test_spec_squelch_grammar();
    rc |= test_spec_bandwidth_field();
    rc |= test_non_soapy_noop();
    rc |= test_soapy_args_only_noop();
    rc |= test_soapy_args_with_full_tuning();
    rc |= test_soapy_no_args_tuning();
    rc |= test_soapy_args_colon_fallback();
    rc |= test_soapy_numeric_colon_tail_fallback();
    rc |= test_soapy_args_partial_tuning();
    rc |= test_soapy_invalid_tuning_field_fallback();
    rc |= test_soapy_suffixed_frequency_out_of_range();
    rc |= test_spec_monitor_volume_range();
    rc |= test_spec_empty_fields_keep_their_place();
    rc |= test_spec_rtltcp_port_refused();
    rc |= test_split_fields_keeps_empty_fields();
    rc |= test_spec_parse_port();
    rc |= test_endpoint_spec_read();
    return rc;
}
