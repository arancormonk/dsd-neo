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
/**
 * @brief Ask the rigctl peer to demodulate @p kind (dsd_analog_demod: DSD_ANALOG_DEMOD_FM or DSD_ANALOG_DEMOD_AM) at
 * passband @p bandwidth Hz (issue #526).
 *
 * FM sends "M NFM <bandwidth>", then "M FM <bandwidth>" if the peer refuses that token (SetModulation() is this call for
 * FM); AM sends "M AM <bandwidth>". Each socket caches the demodulator and passband its peer last accepted, keyed on
 * both, and a request for what the peer runs skips the I/O. An FM @p bandwidth of 0 asks for the peer's normal passband
 * (Hamlib's 0): it is sent only to undo a request this client made on the socket (an AM scan row's demodulator, or a
 * row's own passband), and skipped on a socket nothing was asked of, whose peer keeps its own settings. Returns false
 * when the peer refuses the request or the I/O fails. After a refusal the cache keeps what the peer last accepted, so
 * the same request asks again. After an I/O failure the request may have reached the peer, so what it runs is not
 * known: the next request on the socket is sent whatever it asks for, the FM undo included.
 */
bool SetModulationKind(dsd_socket_t sockfd, int kind, int bandwidth);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RIGCTL_CLIENT_H_H */
