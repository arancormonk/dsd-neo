// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "services.h"

/* Rigctl is on over an open socket. Socket 0 is what zeroed opts hold, never a rigctl connection to close. */
static int
rigctl_connection_live(const dsd_opts* opts) {
    return opts->use_rigctl == 1 && opts->rigctl_sockfd != DSD_INVALID_SOCKET && opts->rigctl_sockfd != 0;
}

int
svc_rigctl_connect(dsd_opts* opts, const char* host, int port) {
    if (!opts || !host || port <= 0) {
        return -1;
    }

    /* Nothing in opts changes until the outcome is known: a reconnect that fails keeps the connection in use. */
    char next_host[sizeof opts->rigctlhostname];
    DSD_SNPRINTF(next_host, sizeof next_host, "%s", host);

    /* The connect can block for long, so it runs outside the P25 SM tick guard; replacing the connection runs inside
       it, since the watchdog's retunes use the rigctl socket and its peer record (as svc_rtl_restart() quiesces them
       before it replaces the stream). */
    const int live = rigctl_connection_live(opts);
    const dsd_socket_t new_sockfd = Connect(next_host, port);
    if (new_sockfd == DSD_INVALID_SOCKET) {
        if (live) {
            /* Issue #589: dropping the connection would leave a peer a scan changed (an am row's AM, a row's
               passband) with nothing to put it back. */
            return -1;
        }
        p25_sm_tick_guard_enter();
        DSD_SNPRINTF(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", next_host);
        opts->rigctlportno = port;
        opts->rigctl_sockfd = DSD_INVALID_SOCKET;
        opts->use_rigctl = 0;
        p25_sm_tick_guard_leave();
        return -1;
    }

    /* Issue #589: the new socket opens while the old one is still open, so their numbers differ; it takes what the
       old socket knew of its peer (the same peer when the host and port are), and the old one is closed before
       anything is sent on the new one. The engine's legacy tune cache described the old connection. The old peer is
       sent nothing: it keeps what a scan last set on it. */
    const dsd_socket_t old_sockfd = live ? opts->rigctl_sockfd : DSD_INVALID_SOCKET;
    const int same_endpoint =
        live && dsd_strcasecmp(opts->rigctlhostname, next_host) == 0 && opts->rigctlportno == port;
    p25_sm_tick_guard_enter();
    RigctlRebindPeer(old_sockfd, new_sockfd, same_endpoint);
    if (old_sockfd != DSD_INVALID_SOCKET && old_sockfd != new_sockfd) {
        dsd_socket_close(old_sockfd);
    }
    dsd_engine_rigctl_tune_cache_forget();
    DSD_SNPRINTF(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", next_host);
    opts->rigctlportno = port;
    opts->rigctl_sockfd = new_sockfd;
    opts->use_rigctl = 1;
    p25_sm_tick_guard_leave();
    return 0;
}
