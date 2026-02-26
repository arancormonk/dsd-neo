// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/input_spec.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"

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
        fprintf(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0:851.375M:22:-2:24:0:2");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        fprintf(stderr, "non-soapy noop returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        fprintf(stderr, "non-soapy noop applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "rtl:0:851.375M:22:-2:24:0:2") != 0) {
        fprintf(stderr, "non-soapy audio_in_dev mutated: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 11 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        fprintf(stderr, "non-soapy tuning fields changed unexpectedly\n");
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
        fprintf(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=airspy,serial=ABC123");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        fprintf(stderr, "soapy args-only returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        fprintf(stderr, "soapy args-only applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=airspy,serial=ABC123") != 0) {
        fprintf(stderr, "soapy args-only audio_in_dev mutated: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 11 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        fprintf(stderr, "soapy args-only tuning fields changed unexpectedly\n");
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
        fprintf(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s",
             "soapy:driver=airspy,serial=ABC123:851.375M:30:5:16:-50:2");

    int applied = 0;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        fprintf(stderr, "soapy full tuning returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 1) {
        fprintf(stderr, "soapy full tuning applied=%d expected 1\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=airspy,serial=ABC123") != 0) {
        fprintf(stderr, "soapy full tuning audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 851375000U || opts->rtl_gain_value != 30 || opts->rtlsdr_ppm_error != 5
        || opts->rtl_dsp_bw_khz != 16 || opts->rtl_volume_multiplier != 2) {
        fprintf(stderr, "soapy full tuning numeric fields mismatch\n");
        free(opts);
        return 1;
    }
    if (!(opts->rtl_squelch_level > 0.0 && opts->rtl_squelch_level < 1.0e-4)) {
        fprintf(stderr, "soapy full tuning squelch expected dB->power mapping, got %.12f\n", opts->rtl_squelch_level);
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
        fprintf(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:935.0125M:44:-3:24:0:5");

    int applied = 0;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        fprintf(stderr, "soapy no-args tuning returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 1) {
        fprintf(stderr, "soapy no-args tuning applied=%d expected 1\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy") != 0) {
        fprintf(stderr, "soapy no-args tuning audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 935012500U || opts->rtl_gain_value != 44 || opts->rtlsdr_ppm_error != -3
        || opts->rtl_dsp_bw_khz != 24 || opts->rtl_volume_multiplier != 5 || fabs(opts->rtl_squelch_level) > 1e-12) {
        fprintf(stderr, "soapy no-args tuning fields mismatch\n");
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
        fprintf(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=foo:bar");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        fprintf(stderr, "soapy colon fallback returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        fprintf(stderr, "soapy colon fallback applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=foo:bar") != 0) {
        fprintf(stderr, "soapy colon fallback audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 11 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        fprintf(stderr, "soapy colon fallback tuning fields changed unexpectedly\n");
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
        fprintf(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=foo:1234567");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        fprintf(stderr, "soapy numeric-tail fallback returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        fprintf(stderr, "soapy numeric-tail fallback applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=foo:1234567") != 0) {
        fprintf(stderr, "soapy numeric-tail fallback audio_in_dev mismatch: %s\n", opts->audio_in_dev);
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
        fprintf(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=sdrplay:851.375M:22");

    int applied = 0;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        fprintf(stderr, "soapy partial tuning returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 1) {
        fprintf(stderr, "soapy partial tuning applied=%d expected 1\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=sdrplay") != 0) {
        fprintf(stderr, "soapy partial tuning audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 851375000U || opts->rtl_gain_value != 22 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        fprintf(stderr, "soapy partial tuning fields mismatch\n");
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
        fprintf(stderr, "allocation failed in %s\n", __func__);
        return 1;
    }
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "soapy:driver=airspy:851.375M:not_a_gain");

    int applied = 99;
    int rc = dsd_normalize_soapy_input_spec(opts, &applied);
    if (rc != 0) {
        fprintf(stderr, "soapy invalid-field fallback returned rc=%d\n", rc);
        free(opts);
        return 1;
    }
    if (applied != 0) {
        fprintf(stderr, "soapy invalid-field fallback applied=%d expected 0\n", applied);
        free(opts);
        return 1;
    }
    if (strcmp(opts->audio_in_dev, "soapy:driver=airspy:851.375M:not_a_gain") != 0) {
        fprintf(stderr, "soapy invalid-field fallback audio_in_dev mismatch: %s\n", opts->audio_in_dev);
        free(opts);
        return 1;
    }
    if (opts->rtlsdr_center_freq != 155340000U || opts->rtl_gain_value != 11 || opts->rtlsdr_ppm_error != -7
        || opts->rtl_dsp_bw_khz != 48 || fabs(opts->rtl_squelch_level - 0.25) > 1e-12
        || opts->rtl_volume_multiplier != 3) {
        fprintf(stderr, "soapy invalid-field fallback tuning fields changed unexpectedly\n");
        free(opts);
        return 1;
    }
    free(opts);
    return 0;
}

int
main(void) {
    int rc = 0;
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
