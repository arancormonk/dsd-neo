// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief UDP-based remote control interface API.
 *
 * Declares the opaque handle and functions to start and stop a background UDP
 * listener that accepts retune commands and invokes a user-supplied callback.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_CONTROL_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_CONTROL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct udp_control;

/**
 * @brief Callback invoked on valid retune command.
 *
 * @param new_frequency_hz Parsed requested center frequency in Hz.
 */
typedef void (*udp_control_retune_cb)(uint32_t new_frequency_hz);

/**
 * @brief Start UDP control thread bound to a specific IPv4 address.
 *
 * Starts a background UDP listener on the specified numeric IPv4 address and
 * port. Passing NULL or an empty bind address uses 127.0.0.1.
 *
 * @param bindaddr Numeric IPv4 address to bind.
 * @param udp_port UDP port to bind and listen on (0 disables/start no-op).
 * @param cb Callback invoked upon receiving a valid retune command.
 * @return Opaque handle on success; NULL on failure.
 */
struct udp_control* udp_control_start_bound(const char* bindaddr, int udp_port, udp_control_retune_cb cb);

/**
 * @brief Stop UDP control thread and free resources.
 *
 * Closes the socket, joins the worker thread, and releases the handle.
 *
 * @param ctrl Opaque handle returned by `udp_control_start_bound` (safe to pass NULL).
 */
void udp_control_stop(struct udp_control* ctrl);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_CONTROL_H_ */
