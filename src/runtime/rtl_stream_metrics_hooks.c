// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "dsd-neo/core/input_level.h"

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>

#include <stdint.h>

static dsd_rtl_stream_metrics_hooks g_rtl_stream_metrics_hooks = {0};
static atomic_int g_rtl_symbol_cache_pending;

void
dsd_rtl_stream_metrics_hooks_set(const dsd_rtl_stream_metrics_hooks* hooks) {
    g_rtl_stream_metrics_hooks = hooks ? *hooks : (dsd_rtl_stream_metrics_hooks){0};
}

unsigned int
dsd_rtl_stream_metrics_hook_output_rate_hz(void) {
    if (g_rtl_stream_metrics_hooks.output_rate_hz) {
        return g_rtl_stream_metrics_hooks.output_rate_hz();
    }
    return 0;
}

int
dsd_rtl_stream_metrics_hook_output_kind(void) {
    if (g_rtl_stream_metrics_hooks.output_kind) {
        return g_rtl_stream_metrics_hooks.output_kind();
    }
    return 0;
}

int
dsd_rtl_stream_metrics_hook_symbol_profile(int* out_symbol_rate_hz, int* out_levels, int* out_channel_profile) {
    if (g_rtl_stream_metrics_hooks.symbol_profile) {
        return g_rtl_stream_metrics_hooks.symbol_profile(out_symbol_rate_hz, out_levels, out_channel_profile);
    }
    if (out_symbol_rate_hz) {
        *out_symbol_rate_hz = 0;
    }
    if (out_levels) {
        *out_levels = 0;
    }
    if (out_channel_profile) {
        *out_channel_profile = 0;
    }
    return 0;
}

uint32_t
dsd_rtl_stream_metrics_hook_stream_generation(void) {
    if (g_rtl_stream_metrics_hooks.stream_generation) {
        return g_rtl_stream_metrics_hooks.stream_generation();
    }
    return 0;
}

int
dsd_rtl_stream_metrics_hook_stream_active(void) {
    if (g_rtl_stream_metrics_hooks.stream_active) {
        return g_rtl_stream_metrics_hooks.stream_active() ? 1 : 0;
    }
    return 0;
}

int
dsd_rtl_stream_metrics_hook_input_level(dsd_input_level_snapshot* out) {
    if (g_rtl_stream_metrics_hooks.input_level) {
        return g_rtl_stream_metrics_hooks.input_level(out);
    }
    if (out) {
        *out = (dsd_input_level_snapshot){0};
    }
    return 0;
}

int
dsd_rtl_stream_metrics_hook_apply_demod_profile(int cqpsk_enable, int symbol_rate_hz, int levels, int channel_profile,
                                                int ted_sps) {
    if (g_rtl_stream_metrics_hooks.apply_demod_profile) {
        return g_rtl_stream_metrics_hooks.apply_demod_profile(cqpsk_enable, symbol_rate_hz, levels, channel_profile,
                                                              ted_sps);
    }
    (void)cqpsk_enable;
    (void)symbol_rate_hz;
    (void)levels;
    (void)channel_profile;
    (void)ted_sps;
    return -1;
}

int
dsd_rtl_stream_metrics_hook_cqpsk_status(int* out_cqpsk_enable, int* out_cqpsk_timing_active) {
    if (g_rtl_stream_metrics_hooks.cqpsk_status) {
        return g_rtl_stream_metrics_hooks.cqpsk_status(out_cqpsk_enable, out_cqpsk_timing_active);
    }
    if (out_cqpsk_enable) {
        *out_cqpsk_enable = 0;
    }
    if (out_cqpsk_timing_active) {
        *out_cqpsk_timing_active = 0;
    }
    return 0;
}

int
dsd_rtl_stream_metrics_hook_cqpsk_timing_bias(void) {
    if (g_rtl_stream_metrics_hooks.cqpsk_timing_bias) {
        return g_rtl_stream_metrics_hooks.cqpsk_timing_bias();
    }
    return 0;
}

double
dsd_rtl_stream_metrics_hook_snr_bias_evm(void) {
    if (g_rtl_stream_metrics_hooks.snr_bias_evm) {
        return g_rtl_stream_metrics_hooks.snr_bias_evm();
    }
    return 2.43;
}

double
dsd_rtl_stream_metrics_hook_snr_c4fm_db(void) {
    if (g_rtl_stream_metrics_hooks.snr_c4fm_db) {
        return g_rtl_stream_metrics_hooks.snr_c4fm_db();
    }
    return -100.0;
}

double
dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db(void) {
    if (g_rtl_stream_metrics_hooks.snr_c4fm_eye_db) {
        return g_rtl_stream_metrics_hooks.snr_c4fm_eye_db();
    }
    return -100.0;
}

double
dsd_rtl_stream_metrics_hook_snr_cqpsk_db(void) {
    if (g_rtl_stream_metrics_hooks.snr_cqpsk_db) {
        return g_rtl_stream_metrics_hooks.snr_cqpsk_db();
    }
    return -100.0;
}

double
dsd_rtl_stream_metrics_hook_snr_gfsk_db(void) {
    if (g_rtl_stream_metrics_hooks.snr_gfsk_db) {
        return g_rtl_stream_metrics_hooks.snr_gfsk_db();
    }
    return -100.0;
}

double
dsd_rtl_stream_metrics_hook_snr_gfsk_eye_db(void) {
    if (g_rtl_stream_metrics_hooks.snr_gfsk_eye_db) {
        return g_rtl_stream_metrics_hooks.snr_gfsk_eye_db();
    }
    return -100.0;
}

double
dsd_rtl_stream_metrics_hook_snr_qpsk_const_db(void) {
    if (g_rtl_stream_metrics_hooks.snr_qpsk_const_db) {
        return g_rtl_stream_metrics_hooks.snr_qpsk_const_db();
    }
    return -100.0;
}

void
dsd_rtl_stream_metrics_hook_p25p1_ber_update(int ok_delta, int err_delta) {
    if (g_rtl_stream_metrics_hooks.p25p1_ber_update) {
        g_rtl_stream_metrics_hooks.p25p1_ber_update(ok_delta, err_delta);
    }
}

void
dsd_rtl_stream_metrics_hook_p25p2_err_update(int slot, int facch_ok, int facch_err, int sacch_ok, int sacch_err,
                                             int voice_err) {
    if (g_rtl_stream_metrics_hooks.p25p2_err_update) {
        g_rtl_stream_metrics_hooks.p25p2_err_update(slot, facch_ok, facch_err, sacch_ok, sacch_err, voice_err);
    }
}

int
dsd_rtl_stream_metrics_hook_symbol_cache_pending(void) {
    int pending = atomic_load(&g_rtl_symbol_cache_pending);
    return pending > 0 ? pending : 0;
}

void
dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(int delta) {
    if (delta == 0) {
        return;
    }
    int next = atomic_fetch_add(&g_rtl_symbol_cache_pending, delta) + delta;
    if (next < 0) {
        atomic_store(&g_rtl_symbol_cache_pending, 0);
    }
}

void
dsd_rtl_stream_metrics_hook_symbol_cache_pending_reset(void) {
    atomic_store(&g_rtl_symbol_cache_pending, 0);
}
