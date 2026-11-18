// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test seam for the built-in libcurl transport in tile_fetch.c.
 *
 * Only a test target that compiles tile_fetch.c itself with
 * DSD_TILE_FETCH_TEST_HOOKS (and USE_CURL) sees this: every libcurl call the
 * transport makes on the easy handle then goes to these functions, which the
 * test defines. That is what lets a test refuse the Nth option, read back the
 * options that were set, and drive the write, header and progress callbacks
 * without a network. The shipped library has no seam at all.
 *
 * Module-private because it names curl types.
 */

#ifndef DSD_NEO_RUNTIME_TILE_FETCH_TEST_HOOKS_H
#define DSD_NEO_RUNTIME_TILE_FETCH_TEST_HOOKS_H

#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif

CURLcode dsd_tile_fetch_test_setopt_long(CURL* curl, CURLoption option, long value);
CURLcode dsd_tile_fetch_test_setopt_ptr(CURL* curl, CURLoption option, void* value);
CURLcode dsd_tile_fetch_test_setopt_data_cb(CURL* curl, CURLoption option, curl_write_callback value);
CURLcode dsd_tile_fetch_test_setopt_xferinfo_cb(CURL* curl, CURLoption option, curl_xferinfo_callback value);
CURLcode dsd_tile_fetch_test_perform(CURL* curl);
CURLcode dsd_tile_fetch_test_response_code(CURL* curl, long* out);
/** Stands in for curl_version_info(CURLVERSION_NOW)->features. */
int dsd_tile_fetch_test_curl_features(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_TILE_FETCH_TEST_HOOKS_H */
