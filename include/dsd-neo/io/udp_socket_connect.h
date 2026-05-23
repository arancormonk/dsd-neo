// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_SOCKET_CONNECT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_SOCKET_CONNECT_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

int udp_socket_connect(dsd_opts* opts, dsd_state* state);
int udp_socket_connectA(dsd_opts* opts, dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_SOCKET_CONNECT_H_H */
