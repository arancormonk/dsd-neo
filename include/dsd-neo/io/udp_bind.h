// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_BIND_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_BIND_H_

#include <dsd-neo/platform/sockets.h>

#ifdef __cplusplus
extern "C" {
#endif

dsd_socket_t UDPBind(char* hostname, int portno);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_UDP_BIND_H_ */
