// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression: DMR Tier III return-to-CC must retune even when only
 * trunk_enable is set (p25_trunk disabled), and must not apply P25-only
 * CC symbol/modulation overrides when no P25 CC is active.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/config.h>

/*
 * Local stubs for trunk_tuning.c dependencies.
 * Keep behavior minimal and deterministic for regression coverage.
 */
static int g_setfreq_calls = 0;
static long int g_last_setfreq_hz = 0;

void
dmr_reset_blocks(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}

void
dsd_frame_sync_reset_mod_state(void) {}

void
p25_p2_frame_reset(void) {}

int
p25_sm_in_tick(void) {
    return 0;
}

void
dsd_drain_audio_output(dsd_opts* opts) {
    (void)opts;
}

bool
SetFreq(dsd_socket_t sockfd, long int freq) {
    (void)sockfd;
    g_setfreq_calls++;
    g_last_setfreq_hz = freq;
    return true;
}

bool
SetModulation(dsd_socket_t sockfd, int bandwidth) {
    (void)sockfd;
    (void)bandwidth;
    return true;
}

uint32_t
rtl_stream_output_rate(const RtlSdrContext* ctx) {
    (void)ctx;
    return 0;
}

int
rtl_stream_tune(RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

void
rtl_stream_toggle_cqpsk(int onoff) {
    (void)onoff;
}

void
rtl_stream_set_ted_sps(int sps) {
    (void)sps;
}

void
rtl_stream_clear_ted_sps_override(void) {}

void
rtl_stream_set_ted_sps_no_override(int sps) {
    (void)sps;
}

uint64_t
dsd_time_monotonic_ns(void) {
    return 1234500000000ULL;
}

void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return NULL;
}

int
main(void) {
    dsd_opts opts;
    dsd_state state;
    memset(&opts, 0, sizeof(opts));
    memset(&state, 0, sizeof(state));

    /* DMR trunking active via protocol-agnostic flag only. */
    opts.trunk_enable = 1;
    opts.p25_trunk = 0;
    opts.trunk_is_tuned = 1;
    opts.p25_is_tuned = 1;
    opts.audio_in_type = AUDIO_IN_PULSE; /* avoid RTL path in this regression */
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = 1;

    state.trunk_cc_freq = 851000000;
    state.p25_cc_freq = 0;
    state.trunk_vc_freq[0] = 852000000;
    state.trunk_vc_freq[1] = 852000000;
    state.last_cc_sync_time = 0;
    state.last_cc_sync_time_m = 0.0;

    /* DMR/GFSK-ish demod settings should remain unchanged on DMR return. */
    state.samplesPerSymbol = 17;
    state.symbolCenter = 8;
    state.rf_mod = 2;

    g_setfreq_calls = 0;
    g_last_setfreq_hz = 0;

    dsd_engine_return_to_cc(&opts, &state);

    /* Core return semantics. */
    assert(opts.trunk_is_tuned == 0);
    assert(opts.p25_is_tuned == 0);
    assert(state.trunk_vc_freq[0] == 0);
    assert(state.trunk_vc_freq[1] == 0);

    /* Critical regression check: DMR return must still issue a retune to CC. */
    assert(g_setfreq_calls == 1);
    assert(g_last_setfreq_hz == state.trunk_cc_freq);

    /* Critical regression check: DMR return still updates CC retune bookkeeping. */
    assert(state.last_cc_sync_time != 0);
    assert(state.last_cc_sync_time_m > 0.0);

    /* Critical regression check: no P25-specific modulation/timing override in DMR path. */
    assert(state.samplesPerSymbol == 17);
    assert(state.symbolCenter == 8);
    assert(state.rf_mod == 2);

    printf("DMR_T3_RETURN_TO_CC_REGRESSION: OK\n");
    return 0;
}
