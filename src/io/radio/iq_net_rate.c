// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Network I/Q rate telemetry: whether the samples arriving match the rate the input is set to. Nothing on the wire
 * states the rate, so a wrong RATE is the likeliest mistake, and it decodes nothing without saying why. The clock is
 * the caller's; every comparison is in whole samples and milliseconds.
 */

#include <dsd-neo/core/safe_api.h>
#include <stddef.h>
#include <stdint.h>

#include "iq_net_internal.h"

void
iq_net_rate_arm(iq_net_rate_monitor* m, uint32_t rate_hz, uint64_t now_ms) {
    DSD_MEMSET(m, 0, sizeof(*m));
    m->rate_hz = rate_hz;
    iq_net_rate_restart(m, now_ms);
}

void
iq_net_rate_restart(iq_net_rate_monitor* m, uint64_t now_ms) {
    m->measuring = 1;
    m->window_start_ms = now_ms + IQ_NET_RATE_WARMUP_MS;
    m->window_samples = 0;
    m->window_discarded = 0;
    m->streak = 0;
    m->streak_side = 0;
}

void
iq_net_rate_pause(iq_net_rate_monitor* m) {
    m->measuring = 0;
    m->streak = 0;
    m->streak_side = 0;
}

void
iq_net_rate_discard(iq_net_rate_monitor* m) {
    m->window_discarded = 1;
}

/* -1 when samples over one window are strictly below 0.9x the rate, +1 when strictly above 1.1x, else 0. */
static int
iq_net_rate_window_side(const iq_net_rate_monitor* m, uint64_t samples) {
    const uint64_t expected = (uint64_t)m->rate_hz * IQ_NET_RATE_WINDOW_MS / 1000U;
    if (samples * 10U < expected * 9U) {
        return -1;
    }
    if (samples * 10U > expected * 11U) {
        return 1;
    }
    return 0;
}

/* Fold one closed window into the streak. Returns 1 when it completes the streak that warns. */
static int
iq_net_rate_close_window(iq_net_rate_monitor* m, uint64_t* out_rate) {
    const int side = m->window_discarded ? 0 : iq_net_rate_window_side(m, m->window_samples);
    if (side == 0) {
        m->streak = 0;
        m->streak_side = 0;
        return 0;
    }
    if (side != m->streak_side) {
        m->streak = 0;
        m->streak_side = side;
    }
    m->streak_samples[m->streak % IQ_NET_RATE_WINDOWS] = m->window_samples;
    m->streak++;
    if (m->streak < IQ_NET_RATE_WINDOWS || m->warned) {
        return 0;
    }
    uint64_t sum = 0;
    for (int i = 0; i < IQ_NET_RATE_WINDOWS; i++) {
        sum += m->streak_samples[i];
    }
    const uint64_t span_s = (uint64_t)IQ_NET_RATE_WINDOWS * IQ_NET_RATE_WINDOW_MS / 1000U;
    m->warned = 1;
    if (out_rate) {
        *out_rate = (sum + span_s / 2U) / span_s;
    }
    return 1;
}

int
iq_net_rate_update(iq_net_rate_monitor* m, uint64_t now_ms, uint64_t samples, uint64_t* out_rate) {
    if (!m->measuring) {
        return 0;
    }
    int warn = 0;
    while (now_ms >= m->window_start_ms + IQ_NET_RATE_WINDOW_MS) {
        warn |= iq_net_rate_close_window(m, out_rate);
        m->window_start_ms += IQ_NET_RATE_WINDOW_MS;
        m->window_samples = 0;
        m->window_discarded = 0;
    }
    if (now_ms >= m->window_start_ms) {
        m->window_samples += samples;
    }
    return warn;
}

void
iq_net_rate_warning_text(char* out, size_t out_size, uint64_t measured_rate, uint32_t rate_hz) {
    DSD_SNPRINTF(out, out_size,
                 "I/Q stream: about %llu samples/s are arriving, but the input is set to %u. If nothing decodes, check "
                 "that the sample rate matches the sender.\n",
                 (unsigned long long)measured_rate, (unsigned)rate_hz);
}
