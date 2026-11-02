// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief TCP PCM16LE audio input backend.
 *
 * Provides a cross-platform abstraction for TCP audio input over one buffered
 * PCM reader on the socket (<dsd-neo/platform/pcm_stream.h>).
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_TCP_INPUT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_TCP_INPUT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/platform/sockets.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open TCP audio input on an already-connected socket.
 *
 * Takes ownership of the socket for reading. The socket should already be
 * connected to a TCP server streaming PCM16LE audio data.
 *
 * @param sockfd Connected TCP socket.
 * @param samplerate Expected sample rate in Hz (e.g., 48000).
 * @return Context pointer on success; NULL on error.
 */
tcp_input_ctx* tcp_input_open(dsd_socket_t sockfd, int samplerate);

/**
 * @brief Close TCP audio input and release resources.
 *
 * Does NOT close the underlying socket; caller retains socket ownership.
 *
 * @param ctx Context to close (may be NULL).
 */
void tcp_input_close(tcp_input_ctx* ctx);

/**
 * @brief Read one PCM16 sample from TCP input, waiting at most @p timeout_ms.
 *
 * A sender that is quiet (a squelched network audio source) is no disconnect:
 * the read times out and the connection stays (issue #667 B7). Only the
 * connection's end or a socket error ends it.
 *
 * @param ctx TCP input context.
 * @param out [out] Receives one sample.
 * @param timeout_ms Longest wait for a whole sample.
 * @return 1 with a sample, 0 on EOF/error/disconnect, -1 when none came in time.
 */
int tcp_input_read_sample_wait(tcp_input_ctx* ctx, int16_t* out, unsigned int timeout_ms);

/**
 * @brief Check if TCP input context is valid and connected.
 *
 * @param ctx TCP input context (may be NULL).
 * @return 1 if valid and ready, 0 otherwise.
 */
int tcp_input_is_valid(const tcp_input_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_TCP_INPUT_H_ */
