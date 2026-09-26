// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/udp_audio.h>
#include <dsd-neo/io/udp_socket_connect.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stddef.h>
#include "dsd-neo/core/opts_fwd.h"
#include "engine_hooks_install.h"

/* udp_socket_connectA() can fail after it created the socket; a half-configured socket must not look open. */
static int
udp_audio_connect_analog(dsd_opts* opts) {
    if (!opts) {
        return -1;
    }
    if (udp_socket_connectA(opts, NULL) == 0) {
        return 0;
    }
    if (opts->udp_sockfdA != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(opts->udp_sockfdA);
        opts->udp_sockfdA = DSD_INVALID_SOCKET;
    }
    return -1;
}

void
dsd_engine_udp_audio_hooks_install(void) {
    dsd_udp_audio_hooks hooks = {0};
    hooks.blast = udp_socket_blaster;
    hooks.blast_analog = udp_socket_blasterA;
    hooks.connect_analog = udp_audio_connect_analog;
    dsd_udp_audio_hooks_set(hooks);
}
