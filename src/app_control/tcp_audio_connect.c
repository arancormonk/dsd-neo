// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stddef.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "services.h"

static int
tcp_audio_connect_cancelled(void* context) {
    (void)context;
    return dsd_exitflag_load() == 1;
}

int
svc_tcp_connect_audio(dsd_opts* opts, dsd_state* state, const char* host, int port) {
    if (!opts || !state || !host || !*host || port <= 0 || port > 65535) {
        return DSD_AUDIO_INPUT_KEPT;
    }

    char next_host[sizeof opts->tcp_hostname];
    DSD_SNPRINTF(next_host, sizeof next_host, "%s", host);

    /* Within a bound: a host that drops the connection attempt cannot hold the decoder thread for the system's
       connect timeout (issue #634). The address it resolves is the one a reconnect uses again. */
    const dsd_socket_t sockfd = ConnectBounded(next_host, port, 1, tcp_audio_connect_cancelled, NULL);
    if (sockfd == DSD_INVALID_SOCKET) {
        return DSD_AUDIO_INPUT_KEPT;
    }

    dsd_audio_input_request request;
    DSD_MEMSET(&request, 0, sizeof request);
    request.kind = DSD_AUDIO_INPUT_TCP;
    request.host = next_host;
    request.port = port;
    request.tcp_sockfd = sockfd;
    const int rc = dsd_audio_switch_input(opts, state, &request);
    if (rc != DSD_AUDIO_INPUT_SWITCHED) {
        dsd_socket_close(sockfd);
        return rc;
    }
    /* The input runs on this connection: its reconnect goes back to the address it went to. */
    ConnectKeepTcpAudioAddress(next_host, port);
    return rc;
}
