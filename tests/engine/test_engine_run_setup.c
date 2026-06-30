// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/engine/engine.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_double_near(const char* tag, double got, double want, double tol) {
    if (fabs(got - want) > tol) {
        DSD_FPRINTF(stderr, "%s failed got=%f want=%f\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
init_test_runtime(dsd_opts** opts_out, dsd_state** state_out) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }

    initOpts(opts);
    initState(state);
    opts->playfiles = 1;
    opts->audio_in_type = AUDIO_IN_NULL;
    opts->audio_out_type = 9;
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");

    *opts_out = opts;
    *state_out = state;
    return 0;
}

static void
free_test_runtime(dsd_opts* opts, dsd_state* state) {
    if (state != NULL) {
        freeState(state);
    }
    free(state);
    free(opts);
}

typedef struct {
    int start_calls;
    int stop_calls;
    int failures;
} engine_lifecycle_test_ctx;

static int
test_lifecycle_start(dsd_opts* opts, dsd_state* state, void* context) {
    engine_lifecycle_test_ctx* ctx = (engine_lifecycle_test_ctx*)context;
    ctx->start_calls++;
    if (opts->udp_in_portno != 7355) {
        DSD_FPRINTF(stderr, "lifecycle start ran before UDP setup\n");
        ctx->failures++;
    }
    if (dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) == NULL) {
        DSD_FPRINTF(stderr, "lifecycle start missing live state extension\n");
        ctx->failures++;
    }
    return 0;
}

static void
test_lifecycle_stop(dsd_opts* opts, dsd_state* state, void* context) {
    engine_lifecycle_test_ctx* ctx = (engine_lifecycle_test_ctx*)context;
    ctx->stop_calls++;
    if (opts->udp_in_portno != 7355) {
        DSD_FPRINTF(stderr, "lifecycle stop saw state before setup completed\n");
        ctx->failures++;
    }
    if (dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) == NULL) {
        DSD_FPRINTF(stderr, "lifecycle stop ran after engine cleanup\n");
        ctx->failures++;
    }
}

static int
test_lifecycle_hooks_start_after_setup_and_stop_before_cleanup(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "udp");
    state->debug_mode = 1;

    engine_lifecycle_test_ctx ctx = {0};
    dsd_engine_lifecycle_hooks hooks = {
        .start = test_lifecycle_start,
        .stop = test_lifecycle_stop,
        .context = &ctx,
    };
    int rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);

    int test_rc = 0;
    test_rc |= expect_true("lifecycle run ok", rc == 0);
    test_rc |= expect_true("lifecycle start called once", ctx.start_calls == 1);
    test_rc |= expect_true("lifecycle stop called once", ctx.stop_calls == 1);
    test_rc |= expect_true("lifecycle ordering checks", ctx.failures == 0);
    test_rc |= expect_true("cleanup ran after lifecycle stop",
                           dsd_state_ext_get(state, DSD_STATE_EXT_ENGINE_START_MS) == NULL);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_conflicting_scan_modes_fail_before_live_setup(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    opts->trunk_scan_enabled = 1;
    opts->scanner_mode = 1;
    int rc = dsd_engine_run(opts, state);

    int test_rc = expect_true("conflicting scan modes rejected", rc != 0);
    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_m17_udp_input_and_output_specs(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "m17udp:rx.example:17000");
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "m17udp:tx.example:17001");
    int rc = dsd_engine_run(opts, state);

    int test_rc = 0;
    test_rc |= expect_true("m17 run ok", rc == 0);
    test_rc |= expect_true("m17 output enables ip", opts->m17_use_ip == 1);
    test_rc |= expect_true("m17 output type", opts->audio_out_type == 9);
    test_rc |= expect_true("m17 host parsed", strcmp(opts->m17_hostname, "tx.example") == 0);
    test_rc |= expect_true("m17 port parsed", opts->m17_portno == 17001);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_m17_userdata_is_normalized_during_common_setup(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(state->m17dat, sizeof state->m17dat, "%s", "M17:31:n0call:all:16000:7");
    int rc = dsd_engine_run(opts, state);

    int test_rc = 0;
    test_rc |= expect_true("m17 userdata run ok", rc == 0);
    test_rc |= expect_true("m17 CAN clamps to maximum", state->m17_can_en == 15);
    test_rc |= expect_true("m17 source uppercased", strcmp(state->str50c, "N0CALL") == 0);
    test_rc |= expect_true("m17 destination uppercased", strcmp(state->str50b, "ALL") == 0);
    test_rc |= expect_true("m17 rate parsed", state->m17_rate == 16000);
    test_rc |= expect_true("m17 vox clamps to enabled", state->m17_vox == 1);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_udp_input_defaults_and_null_output(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "udp");
    int rc = dsd_engine_run(opts, state);

    int test_rc = 0;
    test_rc |= expect_true("udp run ok", rc == 0);
    test_rc |= expect_true("udp default bind address", strcmp(opts->udp_in_bindaddr, "127.0.0.1") == 0);
    test_rc |= expect_true("udp default port", opts->udp_in_portno == 7355);
    test_rc |= expect_true("null output disables audio", opts->audio_out_type == 9 && opts->audio_out == 0);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_rtltcp_tuning_tokens_and_bias(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s",
                 "rtltcp:radio.local:1234:769.00625M:28:3:24:-47:5:bias=off");
    opts->rtl_bias_tee = 1;
    int rc = dsd_engine_run(opts, state);

    int test_rc = 0;
    test_rc |= expect_true("rtltcp run ok", rc == 0);
    test_rc |= expect_true("rtltcp host", strcmp(opts->rtltcp_hostname, "radio.local") == 0);
    test_rc |= expect_true("rtltcp port", opts->rtltcp_portno == 1234);
    test_rc |=
        expect_true("rtltcp enables rtl input", opts->rtltcp_enabled == 1 && opts->audio_in_type == AUDIO_IN_RTL);
    test_rc |= expect_true("rtltcp frequency", opts->rtlsdr_center_freq == 769006250U);
    test_rc |=
        expect_true("rtltcp gain ppm bw volume", opts->rtl_gain_value == 28 && opts->rtlsdr_ppm_error == 3
                                                     && opts->rtl_dsp_bw_khz == 24 && opts->rtl_volume_multiplier == 5);
    test_rc |= expect_true("rtltcp bias off", opts->rtl_bias_tee == 0);
    test_rc |= expect_double_near("rtltcp squelch", opts->rtl_squelch_level, dB_to_pwr(-47.0), 1e-12);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_rtltcp_invalid_and_partial_tuning_tokens(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s",
                 "rtltcp:bad-port.example:not-a-port:not-a-freq:bad-gain:bad-ppm:bogus-bw:-35:7:bias=0");
    opts->rtl_gain_value = 14;
    opts->rtlsdr_ppm_error = -2;
    opts->rtl_squelch_level = 0.25;
    opts->rtl_bias_tee = 1;
    int rc = dsd_engine_run(opts, state);

    int test_rc = 0;
    test_rc |= expect_true("rtltcp invalid run ok", rc == 0);
    test_rc |= expect_true("rtltcp invalid host", strcmp(opts->rtltcp_hostname, "bad-port.example") == 0);
    test_rc |= expect_true("rtltcp invalid port defaults", opts->rtltcp_portno == 1234);
    test_rc |= expect_true("rtltcp invalid freq becomes zero", opts->rtlsdr_center_freq == 0U);
    test_rc |= expect_true("rtltcp invalid gain and ppm preserved",
                           opts->rtl_gain_value == 14 && opts->rtlsdr_ppm_error == -2);
    test_rc |= expect_true("rtltcp invalid bandwidth defaults", opts->rtl_dsp_bw_khz == 48);
    test_rc |= expect_true("rtltcp invalid bias off", opts->rtl_bias_tee == 0);
    test_rc |= expect_true("rtltcp invalid volume parsed", opts->rtl_volume_multiplier == 7);
    test_rc |= expect_double_near("rtltcp invalid squelch", opts->rtl_squelch_level, dB_to_pwr(-35.0), 1e-12);

    free_test_runtime(opts, state);

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtltcp:short.example:7777:450M:11:bias=on");
    opts->rtl_bias_tee = 0;
    rc = dsd_engine_run(opts, state);

    test_rc |= expect_true("rtltcp partial run ok", rc == 0);
    test_rc |= expect_true("rtltcp partial host", strcmp(opts->rtltcp_hostname, "short.example") == 0);
    test_rc |= expect_true("rtltcp partial port", opts->rtltcp_portno == 7777);
    test_rc |= expect_true("rtltcp partial freq and gain",
                           opts->rtlsdr_center_freq == 450000000U && opts->rtl_gain_value == 11);
    test_rc |= expect_true("rtltcp partial stops before bias", opts->rtl_bias_tee == 0);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_soapy_setup_normalizes_args_and_tuning(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s",
                 "soapy:driver=test,serial=ABC:450.5M:7:2:12:-33:3");
    int rc = dsd_engine_run(opts, state);

    int test_rc = 0;
    test_rc |= expect_true("soapy run ok", rc == 0);
    test_rc |= expect_true("soapy normalizes args", strcmp(opts->audio_in_dev, "soapy:driver=test,serial=ABC") == 0);
    test_rc |= expect_true("soapy selects rtl input", opts->audio_in_type == AUDIO_IN_RTL && opts->rtltcp_enabled == 0);
    test_rc |= expect_true("soapy rtl-style tuning", opts->rtlsdr_center_freq == 450500000U && opts->rtl_gain_value == 7
                                                         && opts->rtlsdr_ppm_error == 2 && opts->rtl_dsp_bw_khz == 12
                                                         && opts->rtl_volume_multiplier == 3);
    test_rc |= expect_double_near("soapy squelch", opts->rtl_squelch_level, dB_to_pwr(-33.0), 1e-12);

    free_test_runtime(opts, state);
    return test_rc;
}

static int
test_iq_replay_guard_and_requested_setup(void) {
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }

    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "iqreplay:/tmp/capture.iq");
    int rc = dsd_engine_run(opts, state);
    int test_rc = expect_true("direct iqreplay rejected", rc != 0);
    free_test_runtime(opts, state);

    if (init_test_runtime(&opts, &state) != 0) {
        return 1;
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "iqreplay:/tmp/capture.iq");
    opts->iq_replay_requested = 1;
    rc = dsd_engine_run(opts, state);
    test_rc |= expect_true("requested iqreplay accepted", rc == 0);
    test_rc |= expect_true("requested iqreplay state", opts->iq_replay_active == 1 && opts->rtltcp_enabled == 0
                                                           && opts->audio_in_type == AUDIO_IN_RTL);

    free_test_runtime(opts, state);
    return test_rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_conflicting_scan_modes_fail_before_live_setup();
    rc |= test_m17_udp_input_and_output_specs();
    rc |= test_m17_userdata_is_normalized_during_common_setup();
    rc |= test_udp_input_defaults_and_null_output();
    rc |= test_rtltcp_tuning_tokens_and_bias();
    rc |= test_rtltcp_invalid_and_partial_tuning_tokens();
    rc |= test_soapy_setup_normalizes_args_and_tuning();
    rc |= test_iq_replay_guard_and_requested_setup();
    rc |= test_lifecycle_hooks_start_after_setup_and_stop_before_cleanup();

    if (rc == 0) {
        printf("ENGINE_RUN_SETUP: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
