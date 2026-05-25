// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RIGCTL_CLIENT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RIGCTL_CLIENT_H_H

#include <dsd-neo/platform/sockets.h>

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

dsd_socket_t Connect(char* hostname, int portno);
long int GetCurrentFreq(dsd_socket_t sockfd);
bool SetFreq(dsd_socket_t sockfd, long int freq);
bool SetModulation(dsd_socket_t sockfd, int bandwidth);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RIGCTL_CLIENT_H_H */
