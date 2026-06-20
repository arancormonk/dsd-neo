// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

float
// NOLINTNEXTLINE(misc-use-internal-linkage)
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    (void)opts;
    (void)state;
    (void)have_sync;
    return 0.0f;
}

uint64_t
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_time_monotonic_ns(void) {
    return 0;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ms(unsigned int ms) {
    (void)ms;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_ns(uint64_t ns) {
    (void)ns;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
dsd_sleep_us(uint64_t us) {
    (void)us;
}

void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    static dsdneoRuntimeConfig cfg;
    return &cfg;
}

int
dsd_rtl_stream_metrics_hook_output_kind(void) {
    return 1; /* RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR */
}

int
dsd_rtl_stream_metrics_hook_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 4800;
    }
    if (out_levels) {
        *out_levels = 4;
    }
    if (out_channel_profile) {
        *out_channel_profile = 2;
    }
    return 0;
}

int
dsd_rtl_stream_metrics_hook_stream_active(void) {
    return 1;
}

double
dsd_rtl_stream_metrics_hook_snr_c4fm_db(void) {
    return 25.0;
}

double
dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db(void) {
    return 25.0;
}

double
dsd_rtl_stream_metrics_hook_snr_gfsk_db(void) {
    return 25.0;
}

double
dsd_rtl_stream_metrics_hook_snr_cqpsk_db(void) {
    return 25.0;
}

int
dsd_rtl_stream_metrics_hook_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    if (out_cqpsk_enable) {
        *out_cqpsk_enable = 0;
    }
    if (out_cqpsk_timing_active) {
        *out_cqpsk_timing_active = 0;
    }
    return 0;
}

int
main(void) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.rf_mod = 0;
    state.min = -30000.0f;
    state.max = 30000.0f;
    state.center = 0.0f;
    state.lmid = -20000.0f;
    state.umid = 20000.0f;

    uint8_t reliability = dmr_compute_reliability(&state, 30000.0f);

    if (reliability < 200U) {
        DSD_FPRINTF(stderr, "RTL FSK discriminator reliability clipped to %u\n", reliability);
        return 1;
    }
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
