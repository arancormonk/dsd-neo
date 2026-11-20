// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The input backlog's opening of a replacement connection (issue #834; <dsd-neo/platform/input_backlog.h>).
 *
 * Apart from input_backlog.h because it takes a socket: <dsd-neo/platform/sockets.h> brings in the system socket
 * headers (<winsock2.h> and <windows.h> on Windows, with their near and far macros), which the many headers that
 * include input_backlog.h, <dsd-neo/core/state.h> among them, must not pass on. Only the TCP reconnect and the backlog
 * itself include this one.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_INPUT_BACKLOG_OPENING_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_INPUT_BACKLOG_OPENING_H_

#include <dsd-neo/platform/sockets.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How much input the connected socket @p sock has taken in so far, in the unit the queue that will read it counts.
 * Called with the backlog's lock held. */
typedef uint64_t (*dsd_input_socket_arrived_fn)(dsd_socket_t sock);

/** A replacement input begins: its connected socket @p sock exists, and is the measured input until its queue attaches
 * next, counted by @p arrived in the queue's unit as the queue will count it (nothing reads the connection before then),
 * so a mark taken meanwhile is measured on it. The backlog keeps @p sock itself, never an address of the caller's: the
 * caller's copy may change or go away while the opening lasts. An invalid @p sock or a NULL @p arrived opens nothing.
 * A post held now that measured no input is retired.
 * Decoder thread. The next attach ends the opening; dsd_input_backlog_forget() or dsd_input_backlog_retire() end it with
 * the connection detached and a post measured on it retired. */
void dsd_input_backlog_opening(dsd_socket_t sock, dsd_input_socket_arrived_fn arrived);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_INPUT_BACKLOG_OPENING_H_ */
