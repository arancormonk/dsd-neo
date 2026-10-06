// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/runtime/rigctl_query_hooks.h>
#include <stdint.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "engine_hooks_install.h"

static long int
dsd_engine_rigctl_get_current_freq_hz(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    if (opts->use_rigctl != 1) {
        return 0;
    }
    if (opts->rigctl_sockfd == DSD_INVALID_SOCKET) {
        return 0;
    }
    return GetCurrentFreq(opts->rigctl_sockfd);
}

/* The peer record CachedModulation() reads is shared with the P25 watchdog's retunes, so it is read under the tick
   guard; when a tick holds it, the passband cannot be read just now (DSD_RIGCTL_PASSBAND_BUSY: the PCM noise squelch
   keeps the one it read last). A passband the record does not know (a lost reply) reads DSD_RIGCTL_PASSBAND_UNKNOWN. */
static int32_t
dsd_engine_rigctl_get_passband_hz(const dsd_opts* opts) {
    if (!opts || opts->use_rigctl != 1 || opts->rigctl_sockfd == DSD_INVALID_SOCKET) {
        return 0;
    }
    if (p25_sm_in_tick()) {
        return (int32_t)CachedModulation(opts->rigctl_sockfd).bandwidth;
    }
    if (!p25_sm_tick_guard_try_enter()) {
        return DSD_RIGCTL_PASSBAND_BUSY;
    }
    const int bandwidth = CachedModulation(opts->rigctl_sockfd).bandwidth;
    p25_sm_tick_guard_leave();
    return (int32_t)bandwidth;
}

void
dsd_engine_rigctl_query_hooks_install(void) {
    dsd_rigctl_query_hooks hooks = {0};
    hooks.get_current_freq_hz = dsd_engine_rigctl_get_current_freq_hz;
    hooks.get_passband_hz = dsd_engine_rigctl_get_passband_hz;
    dsd_rigctl_query_hooks_set(hooks);
}
