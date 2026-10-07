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
 * run alongside the terminal UI or headless. The protocol is documented in
 * docs/api.md.
 *
 * Lifecycle: call dsd_api_start() once the app-control frontend runtime is open
 * (dsd_app_frontend_runtime_start()) and dsd_api_stop() after it closes
 * (dsd_app_frontend_runtime_stop()): closing the runtime cancels the commands
 * still queued, and the stop reports those results, with any other completion
 * not yet sent, before it disconnects the clients. While no runtime is open,
 * commands are refused. Neither may be called from a
 * telemetry observer callback. Start it before the decoder runs and publish
 * telemetry once right after (dsd_telemetry_publish_both_and_redraw()): that
 * first look is the event feed's starting point, so every event-history row
 * committed afterwards reaches the clients subscribed when it is published.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_API_API_H_
#define DSD_NEO_INCLUDE_DSD_NEO_API_API_H_

#ifdef __cplusplus
extern "C" {
#endif

/** Protocol version carried in the welcome/hello exchange. */
#define DSD_API_PROTOCOL_VERSION 1

/** Room for a numeric IPv4 bind address. */
enum { DSD_API_BIND_ADDR_SIZE = 64 };

/** Room for the shared-secret token, terminator included (tokens are at most 255 bytes). */
enum { DSD_API_TOKEN_SIZE = 256 };

/** dsd_api_config::port value that binds an OS-chosen port (read it back with dsd_api_bound_port()). */
enum { DSD_API_PORT_EPHEMERAL = -1 };

typedef struct dsd_api_config {
    int port;                               /**< 1..65535, DSD_API_PORT_EPHEMERAL, or 0 to disable. */
    char bind_addr[DSD_API_BIND_ADDR_SIZE]; /**< Numeric IPv4; empty means 127.0.0.1. */
    char token[DSD_API_TOKEN_SIZE];         /**< Shared secret; empty means no auth (loopback only). */
    int max_clients;                        /**< Concurrent session cap, 1..32; 0 for the default of 8. */
    int auth_timeout_ms; /**< A connection that has not authenticated by then is closed; 0 for the default 10000. */
} dsd_api_config;

/**
 * @brief Start the API server.
 *
 * Fails (returns non-zero) on an invalid configuration, a non-loopback bind
 * without a token, a socket failure, or when the server is already running with
 * a different configuration. Starting again with the same configuration is a
 * no-op that succeeds.
 *
 * @return 0 on success (including port 0 / disabled), non-zero on failure.
 */
int dsd_api_start(const dsd_api_config* config);

/**
 * @brief Stop the server. Safe when stopped.
 *
 * Unregisters the feed, queues the results that completed since the last poll,
 * lets each client receive what is queued (bounded), then disconnects them.
 */
void dsd_api_stop(void);

/** Nonzero when the server is listening. */
int dsd_api_is_running(void);

/** The bound TCP port, or 0 when not running. Reports the port DSD_API_PORT_EPHEMERAL picked. */
int dsd_api_bound_port(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_API_API_H_ */
