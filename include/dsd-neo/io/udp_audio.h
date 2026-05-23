// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief UDP audio output helpers.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_AUDIO_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_AUDIO_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void udp_socket_blaster(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);
void udp_socket_blasterA(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_AUDIO_H_ */
