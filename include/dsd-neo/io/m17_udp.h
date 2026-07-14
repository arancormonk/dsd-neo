// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_M17_UDP_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_M17_UDP_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int udp_socket_connectM17(dsd_opts* opts, dsd_state* state);

/**
 * @brief Receive one M17 UDP datagram.
 *
 * @return Datagram size, 0 when no data is available yet or an interrupted
 *         receive should be retried, and -1 on a fatal socket error.
 */
int m17_socket_receiver(const dsd_opts* opts, void* data);
int m17_socket_blaster(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_M17_UDP_H_ */
