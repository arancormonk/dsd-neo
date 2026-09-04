// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Shared libcurl helpers for runtime HTTP consumers (rdio-scanner uploads and the
 * RadioReference SOAP client).
 *
 * Deliberately module-private rather than public: dsd_curl_apply_hardening() names
 * a libcurl type, and every header under include/dsd-neo/runtime must compile
 * standalone against include/ alone with USE_CURL undefined. Include it as
 * "curl_common.h" from src/runtime and "../curl_common.h" from subdirectories.
 *
 * Availability: the four entry points below are always defined. Without USE_CURL
 * they are inert stubs, so callers need no #ifdef. The two that name curl types
 * exist only under USE_CURL and cannot be stubbed at all.
 */

#ifndef DSD_NEO_RUNTIME_CURL_COMMON_H
#define DSD_NEO_RUNTIME_CURL_COMMON_H

#include <stddef.h>

#ifdef USE_CURL
#include <curl/curl.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Growable heap buffer that accumulates an HTTP response body.
 *
 * Always NUL-terminated once anything has been written, so `data` is safe to hand
 * to a text parser. `cap_max` is a hard ceiling on the allocation including the
 * terminator; crossing it makes the write callback abort the transfer.
 */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
    size_t cap_max;
} dsd_curl_body_buf;

/**
 * @brief Initialize libcurl's global state exactly once, safely from any thread.
 *
 * @param  none
 * @return 0 when libcurl is ready, -1 when global init failed or curl is absent.
 */
int dsd_curl_global_ready(void);

/**
 * @brief Android system CA directory in OpenSSL's hashed-CApath layout.
 *
 * @return Path to the first trust store that exists, or NULL off Android and when
 *         none is present.
 */
const char* dsd_curl_android_ca_path(void);

/**
 * @brief Prepare a response-body accumulator.
 *
 * @param buf     Buffer to initialize.
 * @param cap_max Hard ceiling in bytes on the allocation, including the NUL
 *                terminator. 0 means unbounded.
 * @return 0 on success, -1 on invalid argument or when curl is absent.
 */
int dsd_curl_body_buf_init(dsd_curl_body_buf* buf, size_t cap_max);

/**
 * @brief Release a response-body accumulator. Safe on a zeroed or freed buffer.
 *
 * @param buf Buffer to release.
 */
void dsd_curl_body_buf_free(dsd_curl_body_buf* buf);

#ifdef USE_CURL
/**
 * @brief Apply the transport-neutral hardening every dsd-neo request shares.
 *
 * Sets connect/total timeouts, disables signals and redirects, restricts the
 * protocol allowlist to http/https, sets the user agent, and points libcurl at the
 * Android trust store when there is one. Deliberately does not touch CURLOPT_URL,
 * the request body, the write callback or CURLOPT_NOPROGRESS: those are per-caller.
 *
 * @param curl               Easy handle to configure.
 * @param connect_timeout_ms Connect phase timeout in milliseconds.
 * @param total_timeout_ms   Whole-transfer timeout in milliseconds.
 * @param user_agent         User-Agent string, or NULL to leave libcurl's default.
 */
void dsd_curl_apply_hardening(CURL* curl, int connect_timeout_ms, int total_timeout_ms, const char* user_agent);

/**
 * @brief CURLOPT_WRITEFUNCTION that appends into a dsd_curl_body_buf.
 *
 * Returns 0 on overflow or allocation failure, which aborts the transfer with
 * CURLE_WRITE_ERROR (not CURLE_ABORTED_BY_CALLBACK - callers map those differently).
 *
 * @param ptr      Incoming bytes.
 * @param size     Element size.
 * @param nmemb    Element count.
 * @param userdata The dsd_curl_body_buf to append into.
 * @return Number of bytes consumed, or 0 to abort the transfer.
 */
size_t dsd_curl_body_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_CURL_COMMON_H */
