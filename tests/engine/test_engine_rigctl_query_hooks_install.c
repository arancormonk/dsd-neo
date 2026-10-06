// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <limits.h>
#include <stddef.h>

#include "dsd-neo/core/opts_fwd.h"
#include "src/engine/engine_hooks_install.h"

static int g_get_freq_calls = 0;
static dsd_socket_t g_last_sockfd = DSD_INVALID_SOCKET;
static long int g_return_freq = 0;

long int
GetCurrentFreq(dsd_socket_t sockfd) {
    ++g_get_freq_calls;
    g_last_sockfd = sockfd;
    return g_return_freq;
}

/* The peer record (issue #628): what CachedModulation() reports, and the P25 tick guard around it. */
static int g_cached_calls = 0;
static int g_cached_bandwidth = 0;
static int g_in_tick = 0;
static int g_guard_busy = 0;
static int g_guard_held = 0;

dsd_rigctl_modulation
CachedModulation(dsd_socket_t sockfd) {
    ++g_cached_calls;
    g_last_sockfd = sockfd;
    dsd_rigctl_modulation m = {0, g_cached_bandwidth};
    return m;
}

int
p25_sm_in_tick(void) {
    return g_in_tick;
}

int
p25_sm_tick_guard_try_enter(void) {
    if (g_guard_busy) {
        return 0;
    }
    g_guard_held++;
    return 1;
}

void
p25_sm_tick_guard_leave(void) {
    g_guard_held--;
}

static void
reset_stub(void) {
    g_get_freq_calls = 0;
    g_last_sockfd = DSD_INVALID_SOCKET;
    g_return_freq = 0;
    g_cached_calls = 0;
    g_cached_bandwidth = 0;
    g_in_tick = 0;
    g_guard_busy = 0;
    g_guard_held = 0;
}

int
main(void) {
    static dsd_opts opts = {0};

    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    dsd_engine_rigctl_query_hooks_install();

    reset_stub();
    assert(dsd_rigctl_query_hook_get_current_freq_hz(NULL) == 0);
    assert(g_get_freq_calls == 0);

    reset_stub();
    opts.use_rigctl = 0;
    opts.rigctl_sockfd = 77;
    assert(dsd_rigctl_query_hook_get_current_freq_hz(&opts) == 0);
    assert(g_get_freq_calls == 0);

    reset_stub();
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    assert(dsd_rigctl_query_hook_get_current_freq_hz(&opts) == 0);
    assert(g_get_freq_calls == 0);

    reset_stub();
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = 88;
    g_return_freq = 851012500L;
    assert(dsd_rigctl_query_hook_get_current_freq_hz(&opts) == 851012500L);
    assert(g_get_freq_calls == 1);
    assert(g_last_sockfd == 88);

    /* The peer's passband (issue #628): 0 without rigctl or a socket, the record's under the tick guard (unknown after
       a lost reply), busy -- not unknown -- while a tick holds the guard, and read as is from inside a tick. */
    reset_stub();
    opts.use_rigctl = 0;
    opts.rigctl_sockfd = 88;
    assert(dsd_rigctl_query_hook_get_passband_hz(&opts) == 0 && g_cached_calls == 0);
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    assert(dsd_rigctl_query_hook_get_passband_hz(&opts) == 0 && g_cached_calls == 0);
    assert(dsd_rigctl_query_hook_get_passband_hz(NULL) == 0);
    opts.rigctl_sockfd = 88;
    g_cached_bandwidth = 12500;
    assert(dsd_rigctl_query_hook_get_passband_hz(&opts) == 12500);
    assert(g_cached_calls == 1 && g_last_sockfd == 88 && g_guard_held == 0);
    g_cached_bandwidth = INT_MIN;
    assert(dsd_rigctl_query_hook_get_passband_hz(&opts) == DSD_RIGCTL_PASSBAND_UNKNOWN);
    g_cached_bandwidth = 6000;
    g_guard_busy = 1;
    g_cached_calls = 0;
    assert(dsd_rigctl_query_hook_get_passband_hz(&opts) == DSD_RIGCTL_PASSBAND_BUSY && g_cached_calls == 0);
    g_in_tick = 1;
    assert(dsd_rigctl_query_hook_get_passband_hz(&opts) == 6000 && g_cached_calls == 1 && g_guard_held == 0);

    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    return 0;
}
