// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Low-level RIGCTL command helpers.
 *
 * Declares functions implemented by the RIGCTL client that operate directly on
 * the control socket.
 */

#pragma once

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/sockets.h>

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

dsd_socket_t Connect(char* hostname, int portno);
dsd_socket_t UDPBind(char* hostname, int portno);
long int GetCurrentFreq(dsd_socket_t sockfd);
bool SetFreq(dsd_socket_t sockfd, long int freq);
bool SetModulation(dsd_socket_t sockfd, int bandwidth);
int udp_socket_connect(dsd_opts* opts, dsd_state* state);
int udp_socket_connectA(dsd_opts* opts, dsd_state* state);
int udp_socket_connectM17(dsd_opts* opts, dsd_state* state);
int m17_socket_receiver(dsd_opts* opts, void* data);
int m17_socket_blaster(dsd_opts* opts, dsd_state* state, size_t nsam, void* data);

#ifdef __cplusplus
}
#endif
