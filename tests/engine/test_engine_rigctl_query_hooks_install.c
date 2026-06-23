// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
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

static void
reset_stub(void) {
    g_get_freq_calls = 0;
    g_last_sockfd = DSD_INVALID_SOCKET;
    g_return_freq = 0;
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

    dsd_rigctl_query_hooks_set((dsd_rigctl_query_hooks){0});
    return 0;
}
