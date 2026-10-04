// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/input_spec.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

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
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:935.0125M:44:-3:24:0:5");

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
        || opts->rtl_dsp_bw_khz != 24 || opts->rtl_volume_multiplier != 5 || fabs(opts->rtl_squelch_level) > 1e-12) {
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
        || (mode == DSD_SQUELCH_MODE_AUTO && opts->rtl_squelch_margin_db != margin)) {
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
    /* Not a squelch: nothing changes. */
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:noise:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtl: noise", opts, DSD_SQUELCH_MODE_LEVEL, 0.0, 0);
    /* --squelch wins. */
    opts->rtl_squelch_cli_set = 1;
    opts->rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts->rtl_squelch_margin_db = 12;
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:162.475M:30:-2:48:-60:2");
    (void)dsd_rtl_input_spec_apply(opts);
    rc |= expect_squelch("rtl: -60 under --squelch", opts, DSD_SQUELCH_MODE_AUTO, 0.0, 12);
    free(opts);

    /* soapy: the same field, and the spec still parses as tuning with it. */
    opts = alloc_seeded_opts();
    if (!opts) {
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=rtlsdr:162.475M:30:-2:48:auto+8:2");
    int applied = 0;
    if (dsd_normalize_soapy_input_spec(opts, &applied) != 0 || applied != 1
        || strcmp(opts->audio_in_dev, "soapy:driver=rtlsdr") != 0 || opts->rtl_volume_multiplier != 2) {
        DSD_FPRINTF(stderr, "soapy auto+8: applied=%d dev=%s vol=%d\n", applied, opts->audio_in_dev,
                    opts->rtl_volume_multiplier);
        rc = 1;
    }
    rc |= expect_squelch("soapy: auto+8", opts, DSD_SQUELCH_MODE_AUTO, 0.25, 8);
    free(opts);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_spec_squelch_grammar();
    rc |= test_non_soapy_noop();
    rc |= test_soapy_args_only_noop();
    rc |= test_soapy_args_with_full_tuning();
    rc |= test_soapy_no_args_tuning();
    rc |= test_soapy_args_colon_fallback();
    rc |= test_soapy_numeric_colon_tail_fallback();
    rc |= test_soapy_args_partial_tuning();
    rc |= test_soapy_invalid_tuning_field_fallback();
    return rc;
}
