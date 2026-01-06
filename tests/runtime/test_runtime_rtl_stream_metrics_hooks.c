// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>

#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>

static int g_output_rate_calls = 0;
static int g_dsp_get_calls = 0;
static int g_snr_bias_calls = 0;

static unsigned int
fake_output_rate_hz(void) {
    g_output_rate_calls++;
    return 24000U;
}

static int
fake_dsp_get(int* out_cqpsk_enable, int* out_fll_enable, int* out_ted_enable) {
    g_dsp_get_calls++;
    if (out_cqpsk_enable) {
        *out_cqpsk_enable = 1;
    }
    if (out_fll_enable) {
        *out_fll_enable = 2;
    }
    if (out_ted_enable) {
        *out_ted_enable = 3;
    }
    return -7;
}

static double
fake_snr_bias_evm(void) {
    g_snr_bias_calls++;
    return 9.87;
}

int
main(void) {
    // Default behavior with hooks unset
    dsd_rtl_stream_metrics_hooks_set((dsd_rtl_stream_metrics_hooks){0});

    assert(dsd_rtl_stream_metrics_hook_output_rate_hz() == 0U);

    int cqpsk = -1;
    int fll = -1;
    int ted = -1;
    assert(dsd_rtl_stream_metrics_hook_dsp_get(&cqpsk, &fll, &ted) == 0);
    assert(cqpsk == 0);
    assert(fll == 0);
    assert(ted == 0);

    assert(dsd_rtl_stream_metrics_hook_snr_bias_evm() == 2.43);

    // Installed hooks should be invoked through wrappers
    g_output_rate_calls = 0;
    g_dsp_get_calls = 0;
    g_snr_bias_calls = 0;

    dsd_rtl_stream_metrics_hooks hooks = {0};
    hooks.output_rate_hz = fake_output_rate_hz;
    hooks.dsp_get = fake_dsp_get;
    hooks.snr_bias_evm = fake_snr_bias_evm;
    dsd_rtl_stream_metrics_hooks_set(hooks);

    assert(dsd_rtl_stream_metrics_hook_output_rate_hz() == 24000U);
    assert(g_output_rate_calls == 1);

    cqpsk = fll = ted = 0;
    assert(dsd_rtl_stream_metrics_hook_dsp_get(&cqpsk, &fll, &ted) == -7);
    assert(g_dsp_get_calls == 1);
    assert(cqpsk == 1);
    assert(fll == 2);
    assert(ted == 3);

    assert(dsd_rtl_stream_metrics_hook_snr_bias_evm() == 9.87);
    assert(g_snr_bias_calls == 1);

    return 0;
}
