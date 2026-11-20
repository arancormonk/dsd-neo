// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Test seam for dsd_curl_perform_cancellable() in curl_common.c.
 *
 * Only a test target that compiles curl_common.c itself with
 * DSD_CURL_COMMON_TEST_HOOKS (and USE_CURL) sees this: the helper's
 * multi-interface calls, the loaded library's version and its sleep then go to
 * these functions, which the test defines, so every error branch can be driven
 * without a network. DSD_CURL_COMMON_TEST_CURL_VERSION_NUM forces the branches
 * for older headers. The shipped library has no seam.
 *
 * Module-private because it names curl types.
 */

#ifndef DSD_NEO_RUNTIME_CURL_COMMON_TEST_HOOKS_H
#define DSD_NEO_RUNTIME_CURL_COMMON_TEST_HOOKS_H

#include <curl/curl.h>

#ifdef __cplusplus
extern "C" {
#endif

CURLM* dsd_curl_test_multi_init(void);
CURLMcode dsd_curl_test_multi_add_handle(CURLM* multi, CURL* curl);
CURLMcode dsd_curl_test_multi_remove_handle(CURLM* multi, CURL* curl);
CURLMcode dsd_curl_test_multi_cleanup(CURLM* multi);
CURLMcode dsd_curl_test_multi_perform(CURLM* multi, int* running);
CURLMsg* dsd_curl_test_multi_info_read(CURLM* multi, int* left);
CURLMcode dsd_curl_test_multi_poll(CURLM* multi, struct curl_waitfd* fds, unsigned int nfds, int timeout_ms,
                                   int* numfds);
CURLMcode dsd_curl_test_multi_wait(CURLM* multi, struct curl_waitfd* fds, unsigned int nfds, int timeout_ms,
                                   int* numfds);
CURLMcode dsd_curl_test_multi_timeout(CURLM* multi, long* timeout_ms);
CURLMcode dsd_curl_test_multi_setopt_long(CURLM* multi, CURLMoption option, long value);
/** Stands in for curl_version_info(CURLVERSION_NOW)->version_num. */
unsigned int dsd_curl_test_runtime_version_num(void);
void dsd_curl_test_sleep_ms(unsigned int ms);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_RUNTIME_CURL_COMMON_TEST_HOOKS_H */
