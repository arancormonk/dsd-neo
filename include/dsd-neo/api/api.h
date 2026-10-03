// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Third-party control and telemetry API (TCP JSON Lines).
 *
 * A desktop-only server that wraps the app-control command queue and the
 * multi-consumer telemetry observers. One newline-delimited JSON object per
 * line flows in both directions. It is independent of any frontend, so it can
 * run alongside the terminal/Qt UI or headless.
 *
 * Lifecycle: call dsd_api_start() after options are parsed and before/around
 * the engine run; call dsd_api_stop() at shutdown. Starting is idempotent and
 * a port of 0 disables the server.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_API_API_H_
#define DSD_NEO_INCLUDE_DSD_NEO_API_API_H_

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol version carried in the welcome/hello exchange. */
#define DSD_API_PROTOCOL_VERSION 1

/** Longest accepted bind address (numeric IPv4). */
enum { DSD_API_BIND_ADDR_SIZE = 64 };

/** Longest accepted shared-secret token. */
enum { DSD_API_TOKEN_SIZE = 256 };

typedef struct dsd_api_config {
    int port;                               /**< 0 disables the server. */
    char bind_addr[DSD_API_BIND_ADDR_SIZE]; /**< Numeric IPv4; defaults to 127.0.0.1. */
    char token[DSD_API_TOKEN_SIZE];         /**< Shared secret; empty means no auth. */
    int max_clients;                        /**< Concurrent session cap; 0 for a default. */
} dsd_api_config;

/**
 * @brief Start the API server.
 *
 * Fails (returns non-zero) on an invalid configuration, a non-loopback bind
 * without a token, a socket failure, or when the server is already running with
 * a different configuration. Safe to call again with the same configuration.
 *
 * @return 0 on success (including port 0 / disabled), non-zero on failure.
 */
int dsd_api_start(const dsd_api_config* config);

/** Stop the server, disconnecting clients and unregistering observers. Safe when stopped. */
void dsd_api_stop(void);

/** Nonzero when the server is listening. */
int dsd_api_is_running(void);

/** The bound TCP port, or 0 when not running. Useful when port 0 requested an ephemeral port. */
int dsd_api_bound_port(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_API_API_H_ */
