// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for USE_RADIO terminal UI menu labels and predicates.
 */

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/ui/menu_core.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"
#include "menu_env.h"
#include "menu_internal.h"
#include "menu_items.h"
#include "menu_labels.h"

static dsdneoRuntimeConfig g_cfg;
static int g_cfg_valid = 1;
static int g_env_int_value;
static int g_env_int_has_value;
static double g_env_double_value;
static int g_env_double_has_value;
static int g_rtl_cqpsk;
static int g_rtl_iq_balance;
static int g_rtl_iq_dc_on;
static int g_rtl_iq_dc_k;
static float g_rtl_ted_gain;
static int g_rtl_timing_bias;
static int g_rtl_auto_ppm;
static int g_rtl_tuner_autogain;
static int g_rtl_output_kind;
static int g_ted_child_visible;

static bool
ted_child_visible(const void* ctx) {
    (void)ctx;
    return g_ted_child_visible != 0;
}

const NcMenuItem DSP_TED_ITEMS[] = {
    {"ted-visible", "TED Visible", NULL, NULL, ted_child_visible, NULL, NULL, 0},
};
const size_t DSP_TED_ITEMS_LEN = sizeof DSP_TED_ITEMS / sizeof DSP_TED_ITEMS[0];

int
ui_submenu_has_visible(const NcMenuItem* items, size_t n, const void* ctx) {
    if (!items) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (!items[i].is_enabled || items[i].is_enabled(ctx)) {
            return 1;
        }
    }
    return 0;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return g_cfg_valid ? &g_cfg : NULL;
}

int
env_get_int(const char* name, int defv) {
    (void)name;
    return g_env_int_has_value ? g_env_int_value : defv;
}

double
env_get_double(const char* name, double defv) {
    (void)name;
    return g_env_double_has_value ? g_env_double_value : defv;
}

int
tcp_input_is_valid(const tcp_input_ctx* ctx) {
    (void)ctx;
    return 0;
}

int
dsd_stat_path(const char* path, dsd_stat_t* st) {
    (void)path;
    if (st) {
        DSD_MEMSET(st, 0, sizeof(*st));
    }
    return -1;
}

int
rtl_stream_get_output_kind(void) {
    return g_rtl_output_kind;
}

int
rtl_stream_get_cqpsk_status(int* cqpsk_enable, int* cqpsk_timing_active) {
    if (cqpsk_enable) {
        *cqpsk_enable = g_rtl_cqpsk;
    }
    if (cqpsk_timing_active) {
        *cqpsk_timing_active = g_rtl_cqpsk;
    }
    return 0;
}

int
rtl_stream_get_iq_balance(void) {
    return g_rtl_iq_balance;
}

int
rtl_stream_get_iq_dc(int* out_shift_k) {
    if (out_shift_k) {
        *out_shift_k = g_rtl_iq_dc_k;
    }
    return g_rtl_iq_dc_on;
}

float
rtl_stream_get_ted_gain(void) {
    return g_rtl_ted_gain;
}

int
rtl_stream_cqpsk_timing_bias(const RtlSdrContext* ctx) {
    (void)ctx;
    return g_rtl_timing_bias;
}

int
rtl_stream_get_auto_ppm(void) {
    return g_rtl_auto_ppm;
}

int
rtl_stream_get_tuner_autogain(void) {
    return g_rtl_tuner_autogain;
}

int
dsd_app_frontend_get_metrics(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out) {
    (void)opts;
    (void)state;
    DSD_MEMSET(out, 0, sizeof(*out));
    out->output_kind = g_rtl_output_kind;
    out->cqpsk_enable = g_rtl_cqpsk;
    out->cqpsk_timing_active = g_rtl_cqpsk;
    out->iq_balance = g_rtl_iq_balance;
    out->iq_dc_enabled = g_rtl_iq_dc_on;
    out->iq_dc_shift_k = g_rtl_iq_dc_k;
    out->ted_gain = g_rtl_ted_gain;
    out->cqpsk_timing_bias = g_rtl_timing_bias;
    out->auto_ppm_enabled = g_rtl_auto_ppm;
    out->tuner_autogain = g_rtl_tuner_autogain;
    return 0;
}

int
dsd_app_frontend_auto_ppm_enabled(const dsd_state* state, int configured) {
    if (state && state->rtl_ctx) {
        return g_rtl_auto_ppm;
    }
    return configured ? 1 : 0;
}

int
dsd_app_frontend_tuner_autogain_enabled(const dsd_state* state, int configured) {
    if (state && state->rtl_ctx) {
        return g_rtl_tuner_autogain;
    }
    return configured ? 1 : 0;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
reset_fixture(dsd_opts* opts, dsd_state* state, UiCtx* ctx) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&g_cfg, 0, sizeof(g_cfg));
    g_cfg_valid = 1;
    g_env_int_value = 0;
    g_env_int_has_value = 0;
    g_env_double_value = 0.0;
    g_env_double_has_value = 0;
    g_rtl_cqpsk = 0;
    g_rtl_iq_balance = 0;
    g_rtl_iq_dc_on = 0;
    g_rtl_iq_dc_k = 0;
    g_rtl_ted_gain = 0.0f;
    g_rtl_timing_bias = 0;
    g_rtl_auto_ppm = 0;
    g_rtl_tuner_autogain = 0;
    g_rtl_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    g_ted_child_visible = 0;
    ctx->opts = opts;
    ctx->state = state;
}

static int
test_radio_modulation_predicates(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    rc |= expect_int("fallback current modulation", ui_current_mod(&ctx), 0);
    rc |= expect_int("fallback not qpsk", is_not_qpsk(&ctx), 1);
    state.rf_mod = 2;
    rc |= expect_int("rf mod gfsk", ui_current_mod(&ctx), 2);
    opts.mod_cli_lock = 1;
    opts.mod_qpsk = 1;
    rc |= expect_int("cli qpsk overrides rf", ui_current_mod(&ctx), 1);
    rc |= expect_int("qpsk predicate", is_mod_qpsk(&ctx), 1);
    g_rtl_cqpsk = 1;
    opts.mod_qpsk = 0;
    opts.mod_gfsk = 1;
    rc |= expect_int("live cqpsk snaps qpsk", ui_current_mod(&ctx), 1);
    rc |= expect_int("cq predicate", dsp_cq_on(&ctx), 1);

    g_rtl_output_kind = RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
    rc |= expect_int("ted hidden for fsk discriminator", is_ted_allowed(&ctx), 0);
    g_rtl_output_kind = RTL_STREAM_OUTPUT_SYMBOL_CQPSK;
    rc |= expect_int("ted allowed for qpsk symbol output", is_ted_allowed(&ctx), 1);
    g_ted_child_visible = 0;
    rc |= expect_int("ted submenu hidden", dsp_ted_any(&ctx), 0);
    g_ted_child_visible = 1;
    rc |= expect_int("ted submenu visible", dsp_ted_any(&ctx), 1);

    return rc;
}

static int
test_radio_dsp_labels(void) {
    int rc = 0;
    char b[160];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    rc |= expect_str("cq inactive", lbl_onoff_cq(&ctx, b, sizeof(b)), "Toggle CQPSK [Inactive]");
    g_rtl_cqpsk = 1;
    rc |= expect_str("cq active", lbl_onoff_cq(&ctx, b, sizeof(b)), "Toggle CQPSK [Active]");
    g_rtl_iq_balance = 1;
    rc |= expect_str("iq balance active", lbl_onoff_iqbal(&ctx, b, sizeof(b)), "Toggle IQ Balance [Active]");
    g_rtl_iq_dc_on = 1;
    g_rtl_iq_dc_k = 5;
    rc |= expect_str("iq dc on", lbl_iq_dc(&ctx, b, sizeof(b)), "IQ DC Block [On]");
    rc |= expect_str("iq dc k", lbl_iq_dc_k(&ctx, b, sizeof(b)), "IQ DC Shift k: 5 (+/-)");
    g_rtl_ted_gain = 0.0746f;
    rc |= expect_str("ted gain milli rounds", lbl_ted_gain(&ctx, b, sizeof(b)), "CQPSK Timing Gain: 75 (x0.001, +/-)");
    g_rtl_timing_bias = -123;
    rc |= expect_str("timing bias", lbl_cqpsk_timing_bias(&ctx, b, sizeof(b)), "CQPSK Timing Bias (EMA): -123");

    opts.show_dsp_panel = 1;
    opts.rtl_bias_tee = 1;
    opts.rtltcp_autotune = 1;
    rc |= expect_str("dsp panel on", lbl_dsp_panel(&ctx, b, sizeof(b)), "Show DSP Panel [On]");
    rc |= expect_str("bias tee on", lbl_rtl_bias(&ctx, b, sizeof(b)), "Bias Tee: On");
    rc |= expect_str("rtl tcp autotune on", lbl_rtl_rtltcp_autotune(&ctx, b, sizeof(b)),
                     "RTL-TCP Adaptive Networking: On");

    return rc;
}

static int
test_radio_config_labels(void) {
    int rc = 0;
    char b[160];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    opts.rtl_auto_ppm = 1;
    rc |= expect_str("auto ppm opts fallback", lbl_rtl_auto_ppm(&ctx, b, sizeof(b)), "Auto-PPM (Spectrum): On");
    state.rtl_ctx = (RtlSdrContext*)0x1;
    g_rtl_auto_ppm = 0;
    rc |= expect_str("auto ppm live off", lbl_rtl_auto_ppm(&ctx, b, sizeof(b)), "Auto-PPM (Spectrum): Off");
    g_rtl_auto_ppm = 1;
    rc |= expect_str("auto ppm live on", lbl_rtl_auto_ppm(&ctx, b, sizeof(b)), "Auto-PPM (Spectrum): On");

    state.rtl_ctx = NULL;
    g_cfg.tuner_autogain_enable = 1;
    rc |= expect_str("tuner autogain config", lbl_rtl_tuner_autogain(&ctx, b, sizeof(b)), "Tuner Autogain: On");
    state.rtl_ctx = (RtlSdrContext*)0x1;
    g_rtl_tuner_autogain = 0;
    rc |= expect_str("tuner autogain live off", lbl_rtl_tuner_autogain(&ctx, b, sizeof(b)), "Tuner Autogain: Off");
    g_rtl_tuner_autogain = 1;
    rc |= expect_str("tuner autogain live on", lbl_rtl_tuner_autogain(&ctx, b, sizeof(b)), "Tuner Autogain: On");

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_radio_modulation_predicates();
    rc |= test_radio_dsp_labels();
    rc |= test_radio_config_labels();
    return rc ? 1 : 0;
}
