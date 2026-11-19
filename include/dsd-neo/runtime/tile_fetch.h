// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief One HTTPS GET for one map tile: conditional request, capped response, cancellable.
 *
 * UI-agnostic C API. The Qt map's tile worker is the consumer; it owns the
 * thread, the cache and the backoff, and this helper does one request per call.
 *
 * Shape rules, the same as radioreference.h:
 *
 * 1. Every entry point is declared unconditionally. Whether the built-in
 *    transport can be used is a runtime answer from dsd_tile_fetch_available(),
 *    never an #ifdef: this header is compiled standalone with no USE_* defines.
 * 2. It includes nothing but <stddef.h>.
 * 3. It names no curl or Qt type; the cancel token is opaque.
 *
 * Privacy: nothing here logs, and no error text ever carries the request URL, so
 * a caller may show dsd_tile_fetch_response::error without leaking the viewed
 * area or a key in the URL template.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TILE_FETCH_H
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TILE_FETCH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest response body accepted, in bytes. A bigger body fails the fetch. */
#define DSD_TILE_FETCH_BODY_CAP_BYTES             ((size_t)512U * 1024U)
/** Largest response header block accepted, in bytes. A bigger block fails the fetch. */
#define DSD_TILE_FETCH_HEADER_CAP_BYTES           ((size_t)32U * 1024U)
/** Longest request URL accepted, in bytes, without the terminator. */
#define DSD_TILE_FETCH_URL_MAX                    2048U
/** Connect-phase timeout used when the request leaves it at 0. */
#define DSD_TILE_FETCH_CONNECT_TIMEOUT_MS_DEFAULT 10000
/** Whole-transfer timeout used when the request leaves it at 0. */
#define DSD_TILE_FETCH_TOTAL_TIMEOUT_MS_DEFAULT   30000
/** Largest timeout a request may ask for, in milliseconds. */
#define DSD_TILE_FETCH_TIMEOUT_MS_MAX             600000

/* Capacity of each returned header value, terminator included. */
#define DSD_TILE_FETCH_ETAG_MAX                   256U
#define DSD_TILE_FETCH_DATE_FIELD_MAX             64U /**< Last-Modified, Expires, Date, Retry-After. */
#define DSD_TILE_FETCH_CACHE_CONTROL_MAX          512U
#define DSD_TILE_FETCH_AGE_MAX                    32U
#define DSD_TILE_FETCH_VARY_MAX                   256U
#define DSD_TILE_FETCH_ERROR_MAX                  128U

/** Outcome of one fetch. */
typedef enum {
    DSD_TILE_FETCH_OK = 0,              /**< 200 with a body; the body is in the response. */
    DSD_TILE_FETCH_NOT_MODIFIED = 1,    /**< 304: the cached copy is still good; headers carry new metadata. */
    DSD_TILE_FETCH_ERR_REDIRECT = 2,    /**< Any other 3xx. Redirects are never followed; treat as a failure. */
    DSD_TILE_FETCH_ERR_HTTP = 3,        /**< Any other status, or a 200 with no body. Headers are filled. */
    DSD_TILE_FETCH_ERR_NETWORK = 4,     /**< Connect, TLS, DNS, timeout or other transfer failure. */
    DSD_TILE_FETCH_ERR_CANCELLED = 5,   /**< The cancel token was set before or during the transfer. */
    DSD_TILE_FETCH_ERR_TOO_LARGE = 6,   /**< Body or header block over its cap. When only the body was, http_status
                                             and the header fields (Retry-After) are still filled. */
    DSD_TILE_FETCH_ERR_BAD_HEADER = 7,  /**< Cache-Control or Vary too long or malformed: cannot be cached safely. */
    DSD_TILE_FETCH_ERR_INVALID = 8,     /**< Bad request: not an https URL, bad conditional value, bad timeout, or
                                             a URL libcurl rejects as malformed (a bad host or port). */
    DSD_TILE_FETCH_ERR_UNSUPPORTED = 9, /**< Built-in transport absent: no libcurl, or no asynchronous resolver. */
    DSD_TILE_FETCH_ERR_SETUP = 10,      /**< libcurl failed to start or refused an option; nothing was sent. */
    DSD_TILE_FETCH_ERR_NOMEM = 11,      /**< Allocation failure. */
} dsd_tile_fetch_status;

/** Response header fields, as bits of dsd_tile_fetch_response::header_present / header_invalid. */
enum {
    DSD_TILE_FETCH_HDR_ETAG = 1U << 0,
    DSD_TILE_FETCH_HDR_LAST_MODIFIED = 1U << 1,
    DSD_TILE_FETCH_HDR_CACHE_CONTROL = 1U << 2,
    DSD_TILE_FETCH_HDR_EXPIRES = 1U << 3,
    DSD_TILE_FETCH_HDR_DATE = 1U << 4,
    DSD_TILE_FETCH_HDR_AGE = 1U << 5,
    DSD_TILE_FETCH_HDR_VARY = 1U << 6,
    DSD_TILE_FETCH_HDR_RETRY_AFTER = 1U << 7,
};

/**
 * @brief Opaque cancellation flag shared between the fetching thread and any other.
 *
 * Setting it makes a fetch that has not started return
 * DSD_TILE_FETCH_ERR_CANCELLED without sending anything, and stops an in-flight
 * built-in transfer:
 * - during connect, TLS or the transfer: within about 100 ms;
 * - during a name lookup: within about 100 ms with libcurl 8.20 or later, which
 *   leaves the stuck resolver thread to end on its own. With older libcurl the
 *   fetch returns only once the system resolver answers or gives up (its own
 *   timeout, seconds), so never wait for a fetch on a UI thread.
 */
typedef struct dsd_tile_fetch_cancel dsd_tile_fetch_cancel;

/** @return A new, clear token, or NULL on allocation failure. */
dsd_tile_fetch_cancel* dsd_tile_fetch_cancel_create(void);

/** @brief Free a token. NULL is a no-op. No fetch may still be using it. */
void dsd_tile_fetch_cancel_destroy(dsd_tile_fetch_cancel* cancel);

/** @brief Ask for cancellation. Safe from any thread; NULL is a no-op. */
void dsd_tile_fetch_cancel_request(dsd_tile_fetch_cancel* cancel);

/** @brief Clear the flag so the token can serve the next fetch. NULL is a no-op. */
void dsd_tile_fetch_cancel_clear(dsd_tile_fetch_cancel* cancel);

/**
 * @brief Whether cancellation was requested. Safe from any thread.
 *
 * @param cancel Token, or NULL (which reads as "not cancelled").
 * @return 1 when cancelled, 0 otherwise.
 */
int dsd_tile_fetch_cancel_requested(const dsd_tile_fetch_cancel* cancel);

/**
 * @brief Opaque connection-reuse context for one fetching thread.
 *
 * Successive fetches that pass the same session can reuse an open HTTPS
 * connection to the same host (and its TLS session) instead of a new connect and
 * handshake per tile: libcurl's connection cache, DNS cache and TLS session cache
 * live in the session (a curl share handle) rather than in the per-fetch handles.
 * Connection reuse needs libcurl 7.57 or later; older libcurl still shares the DNS
 * and TLS session caches. Caps, cancel and every option are unchanged.
 *
 * One thread at a time: a session has no locking, so only the thread that owns it
 * may fetch with it (the tile worker's thread). An injected transport ignores it.
 */
typedef struct dsd_tile_fetch_session dsd_tile_fetch_session;

/** @return A new session, or NULL on allocation failure (fetches then simply do not reuse connections). */
dsd_tile_fetch_session* dsd_tile_fetch_session_create(void);

/** @brief Close the session's connections and free it. NULL is a no-op. No fetch may still be using it. */
void dsd_tile_fetch_session_destroy(dsd_tile_fetch_session* session);

typedef struct dsd_tile_fetch_request dsd_tile_fetch_request;

/**
 * One completed or failed exchange, as a transport hands it back.
 *
 * `body` and `headers` are allocated with malloc() by the transport and owned by
 * dsd_tile_fetch() afterwards, whatever perform() returned. `headers` is the raw
 * header block exactly as received: status line(s) and field lines, each ended by
 * CRLF or LF, an empty line closing each block. Interim (1xx) and proxy CONNECT
 * blocks may come first; the last block is the response's.
 */
typedef struct {
    long http_status;
    unsigned char* body;
    size_t body_len;
    char* headers;
    size_t headers_len;
    dsd_tile_fetch_status status; /**< Failure class when perform() returns -1. */
    char error[DSD_TILE_FETCH_ERROR_MAX];
} dsd_tile_fetch_exchange;

/**
 * @brief Pluggable transport, so the fetch is testable without a network.
 *
 * perform() returns 0 when an HTTP exchange completed (any status), -1 when it did
 * not, with exchange->status set to the failure class (DSD_TILE_FETCH_ERR_NETWORK
 * when left at OK) and exchange->error to a message without the URL. It should
 * honour req->cancel. The request has already been validated.
 */
typedef struct {
    int (*perform)(void* ctx, const dsd_tile_fetch_request* req, dsd_tile_fetch_exchange* exchange);
    void* ctx;
} dsd_tile_fetch_transport;

/** One tile request. Zero-initialize, then fill. */
struct dsd_tile_fetch_request {
    const char* url;                     /**< Required. Must be https://, at most DSD_TILE_FETCH_URL_MAX bytes. */
    const char* if_none_match;           /**< Stored ETag to send as If-None-Match, or NULL/"" for none. */
    const char* if_modified_since;       /**< Stored Last-Modified to send as If-Modified-Since, or NULL/"" for none. */
    int connect_timeout_ms;              /**< 0 for the default; at most DSD_TILE_FETCH_TIMEOUT_MS_MAX. */
    int total_timeout_ms;                /**< 0 for the default; at most DSD_TILE_FETCH_TIMEOUT_MS_MAX. */
    const dsd_tile_fetch_cancel* cancel; /**< Optional. */
    const dsd_tile_fetch_transport* transport; /**< NULL for the built-in libcurl transport. */
    dsd_tile_fetch_session* session;           /**< Optional: reuse connections across fetches (one thread). */
};

/**
 * The result of dsd_tile_fetch(). Release with dsd_tile_fetch_response_free().
 *
 * Header values are trimmed and NUL-terminated, from the final response's header
 * block. A field's bit in `header_present` says the server sent it. Its bit in
 * `header_invalid` says it was sent but cannot be carried (repeated where only one
 * is allowed, empty, too long for its buffer, or holding control or non-ASCII
 * bytes); the value is then "". Repeated Cache-Control or Vary lines are joined
 * with ", " as HTTP allows; one of those two that cannot be carried fails the
 * whole fetch with DSD_TILE_FETCH_ERR_BAD_HEADER instead, because a lost
 * no-store or Vary: * would let the tile be cached against the server's wishes.
 * Interpretation (freshness, Retry-After as seconds or date) is the caller's.
 *
 * Earlier blocks (1xx, proxy CONNECT) are dropped, with one deliberate
 * exception: a Cache-Control or Vary in them that cannot be carried still fails
 * the fetch with DSD_TILE_FETCH_ERR_BAD_HEADER. Such a response is malformed
 * anyway, and failing closed is the safe side.
 */
typedef struct {
    dsd_tile_fetch_status status;
    long http_status;    /**< 0 when no exchange completed. */
    unsigned char* body; /**< Only for DSD_TILE_FETCH_OK; malloc()ed, never more than the body cap. */
    size_t body_len;
    char etag[DSD_TILE_FETCH_ETAG_MAX];
    char last_modified[DSD_TILE_FETCH_DATE_FIELD_MAX];
    char cache_control[DSD_TILE_FETCH_CACHE_CONTROL_MAX];
    char expires[DSD_TILE_FETCH_DATE_FIELD_MAX];
    char date[DSD_TILE_FETCH_DATE_FIELD_MAX];
    char age[DSD_TILE_FETCH_AGE_MAX];
    char vary[DSD_TILE_FETCH_VARY_MAX];
    char retry_after[DSD_TILE_FETCH_DATE_FIELD_MAX];
    unsigned int header_present;          /**< DSD_TILE_FETCH_HDR_* bits. */
    unsigned int header_invalid;          /**< DSD_TILE_FETCH_HDR_* bits. */
    char error[DSD_TILE_FETCH_ERROR_MAX]; /**< Short reason on failure, never the URL. */
} dsd_tile_fetch_response;

/**
 * @brief Whether the BUILT-IN transport can fetch tiles in this build.
 *
 * True only with libcurl that supports https AND has an asynchronous resolver
 * (CURL_VERSION_ASYNCHDNS). With signals off a synchronous resolver ignores
 * every timeout while it looks a name up, so a fetch could hang without bound;
 * a library without https fails every request. Such builds report false and the
 * UI says online map tiles are not available in this build. The probe runs after
 * libcurl's global initialization (the shared guard), since curl_version_info()
 * is not thread-safe before it; if that initialization fails, false as well. An
 * injected transport works regardless.
 *
 * @return 1 when available, 0 otherwise.
 */
int dsd_tile_fetch_available(void);

/**
 * @brief The User-Agent every tile request sends.
 *
 * "dsd-neo/<version> (+https://github.com/arancormonk/dsd-neo)", the version being
 * the build's tag without its leading "v" ("unknown" if the tag is not a valid
 * HTTP token).
 *
 * @param out Destination.
 * @param cap Capacity of @p out; 128 bytes is always enough.
 * @return 0 on success, -1 when @p out is NULL or too small (then "" if cap > 0).
 */
int dsd_tile_fetch_user_agent(char* out, size_t cap);

/**
 * @brief Perform one GET, blocking the calling thread.
 *
 * Validates the request first and sends nothing for an invalid one or a token
 * that is already set. The built-in transport is HTTPS only, follows no
 * redirect, sends If-None-Match / If-Modified-Since when given, sends no
 * Accept-Encoding, caps the body and the headers, and fails closed when any
 * libcurl option is refused. Bounded by the request's timeouts once the name
 * is resolved; a name lookup is bounded by the system resolver. After the cancel
 * token is set it returns as described for dsd_tile_fetch_cancel.
 *
 * @param req  Request.
 * @param resp Receives the result; always initialized, even on failure.
 * @return resp->status.
 */
dsd_tile_fetch_status dsd_tile_fetch(const dsd_tile_fetch_request* req, dsd_tile_fetch_response* resp);

/** @brief Free the body and clear the response. Safe on a zeroed or freed response; NULL is a no-op. */
void dsd_tile_fetch_response_free(dsd_tile_fetch_response* resp);

/**
 * @brief Short English name of a status, for diagnostics.
 *
 * @param status Status.
 * @return Static text, never NULL.
 */
const char* dsd_tile_fetch_status_name(dsd_tile_fetch_status status);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_TILE_FETCH_H */
