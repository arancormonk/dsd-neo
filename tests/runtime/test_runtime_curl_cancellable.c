// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * dsd_curl_perform_cancellable(), the multi-handle loop the tile fetch, the
 * RadioReference client and rdio uploads perform through, driven branch by
 * branch with no network.
 *
 * curl_common.c is compiled into this test with DSD_CURL_COMMON_TEST_HOOKS, so
 * every multi-interface call, the loaded library's version and the sleep come
 * here. Built twice: RUNTIME_CURL_CANCELLABLE against the installed headers
 * (curl_multi_poll, CURLMOPT_QUICK_EXIT), and RUNTIME_CURL_CANCELLABLE_PRE_7_66
 * with the version forced to 7.65 (curl_multi_wait, no quick exit).
 */

#include "curl_common.h"
#include "curl_common_test_hooks.h"

#include <curl/curl.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/log.h>
#include <stdio.h>

#ifdef DSD_CURL_COMMON_TEST_CURL_VERSION_NUM
#define TEST_OLD_HEADERS 1
#else
#define TEST_OLD_HEADERS 0
#endif

/* curl_common.c is compiled on its own here; the runtime library normally supplies this. */
void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

static int g_failures = 0;

static void
expect(const char* what, int ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

static void
expect_ll(const char* what, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s (got %lld, want %lld)\n", what, got, want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* Fake multi interface                                                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    /* Behaviour */
    int init_fails;
    CURLMcode add_rc;
    CURLMcode perform_rc;
    int performs_until_done; /**< running stays 1 for this many performs, then 0. */
    CURLMcode idle_rc;       /**< poll or wait result. */
    int wait_ready;          /**< numfds a wait reports. */
    long multi_timeout;      /**< what curl_multi_timeout() reports. */
    int has_done_msg;
    CURLcode done_result;
    CURLMcode setopt_rc;
    unsigned int runtime_version;
    int cancel_after_performs; /**< -1 for never. */
    /* Counters */
    int inits, adds, removes, cleanups, performs, polls, waits, timeouts, sleeps, setopts, msgs_read;
    CURLMoption setopt_option;
    long setopt_value;
} fake_multi;

static fake_multi g_fake;
static int g_multi_token;
static CURLMsg g_msg;

static void
fake_reset(void) {
    DSD_MEMSET(&g_fake, 0, sizeof(g_fake));
    g_fake.add_rc = CURLM_OK;
    g_fake.perform_rc = CURLM_OK;
    g_fake.performs_until_done = 2;
    g_fake.idle_rc = CURLM_OK;
    g_fake.multi_timeout = 50;
    g_fake.has_done_msg = 1;
    g_fake.done_result = CURLE_OK;
    g_fake.setopt_rc = CURLM_OK;
    g_fake.runtime_version = 0x081700U;
    g_fake.cancel_after_performs = -1;
}

CURLM*
dsd_curl_test_multi_init(void) {
    g_fake.inits++;
    return g_fake.init_fails ? NULL : (CURLM*)&g_multi_token;
}

CURLMcode
dsd_curl_test_multi_add_handle(CURLM* multi, CURL* curl) {
    (void)multi;
    (void)curl;
    g_fake.adds++;
    return g_fake.add_rc;
}

CURLMcode
dsd_curl_test_multi_remove_handle(CURLM* multi, CURL* curl) {
    (void)multi;
    (void)curl;
    g_fake.removes++;
    return CURLM_OK;
}

CURLMcode
dsd_curl_test_multi_cleanup(CURLM* multi) {
    (void)multi;
    g_fake.cleanups++;
    return CURLM_OK;
}

CURLMcode
dsd_curl_test_multi_perform(CURLM* multi, int* running) {
    (void)multi;
    g_fake.performs++;
    *running = (g_fake.performs <= g_fake.performs_until_done) ? 1 : 0;
    return g_fake.perform_rc;
}

CURLMsg*
dsd_curl_test_multi_info_read(CURLM* multi, int* left) {
    (void)multi;
    *left = 0;
    g_fake.msgs_read++;
    if (!g_fake.has_done_msg || g_fake.msgs_read > 1) {
        return NULL;
    }
    DSD_MEMSET(&g_msg, 0, sizeof(g_msg));
    g_msg.msg = CURLMSG_DONE;
    g_msg.data.result = g_fake.done_result;
    return &g_msg;
}

CURLMcode
dsd_curl_test_multi_poll(CURLM* multi, struct curl_waitfd* fds, unsigned int nfds, int timeout_ms, int* numfds) {
    (void)multi;
    (void)fds;
    (void)nfds;
    (void)numfds;
    expect_ll("poll waits 100 ms at most", timeout_ms, 100);
    g_fake.polls++;
    return g_fake.idle_rc;
}

CURLMcode
dsd_curl_test_multi_wait(CURLM* multi, struct curl_waitfd* fds, unsigned int nfds, int timeout_ms, int* numfds) {
    (void)multi;
    (void)fds;
    (void)nfds;
    expect_ll("wait waits 100 ms at most", timeout_ms, 100);
    g_fake.waits++;
    if (numfds != NULL) {
        *numfds = g_fake.wait_ready;
    }
    return g_fake.idle_rc;
}

CURLMcode
dsd_curl_test_multi_timeout(CURLM* multi, long* timeout_ms) {
    (void)multi;
    g_fake.timeouts++;
    *timeout_ms = g_fake.multi_timeout;
    return CURLM_OK;
}

CURLMcode
dsd_curl_test_multi_setopt_long(CURLM* multi, CURLMoption option, long value) {
    (void)multi;
    g_fake.setopts++;
    g_fake.setopt_option = option;
    g_fake.setopt_value = value;
    return g_fake.setopt_rc;
}

unsigned int
dsd_curl_test_runtime_version_num(void) {
    return g_fake.runtime_version;
}

void
dsd_curl_test_sleep_ms(unsigned int ms) {
    expect_ll("sleep is 10 ms", ms, 10);
    g_fake.sleeps++;
}

static int
cancel_pred(const void* ctx) {
    (void)ctx;
    return g_fake.cancel_after_performs >= 0 && g_fake.performs >= g_fake.cancel_after_performs;
}

static CURLcode
run(void) {
    return dsd_curl_perform_cancellable((CURL*)&g_multi_token, cancel_pred, NULL);
}

/* ------------------------------------------------------------------------- */
/* Cases                                                                      */
/* ------------------------------------------------------------------------- */

static void
expect_detached(const char* what) {
    char buf[128];
    (void)DSD_SNPRINTF(buf, sizeof(buf), "%s: handle removed once", what);
    expect_ll(buf, g_fake.removes, 1);
    (void)DSD_SNPRINTF(buf, sizeof(buf), "%s: multi cleaned up once", what);
    expect_ll(buf, g_fake.cleanups, 1);
}

static void
test_completion(void) {
    fake_reset();
    expect_ll("finished transfer's result", (long long)run(), CURLE_OK);
    expect_ll("performed until done", g_fake.performs, 3);
    expect_detached("success");

    fake_reset();
    g_fake.done_result = CURLE_COULDNT_CONNECT;
    expect_ll("transfer error passed through", (long long)run(), CURLE_COULDNT_CONNECT);

    fake_reset();
    g_fake.has_done_msg = 0;
    expect_ll("no done message", (long long)run(), CURLE_GOT_NOTHING);
    expect_detached("no done message");

    fake_reset();
    g_fake.performs_until_done = 0;
    expect_ll("NULL predicate never cancels",
              (long long)dsd_curl_perform_cancellable((CURL*)&g_multi_token, NULL, NULL), CURLE_OK);
}

static void
test_setup_failures(void) {
    fake_reset();
    g_fake.init_fails = 1;
    expect_ll("multi init failure", (long long)run(), CURLE_OUT_OF_MEMORY);
    expect("nothing added or cleaned after init failure", g_fake.adds == 0 && g_fake.cleanups == 0);

    fake_reset();
    g_fake.add_rc = CURLM_OUT_OF_MEMORY;
    expect_ll("add out of memory", (long long)run(), CURLE_OUT_OF_MEMORY);
    expect("add failure cleans up without removing", g_fake.cleanups == 1 && g_fake.removes == 0);
    expect_ll("add failure performs nothing", g_fake.performs, 0);

    fake_reset();
    g_fake.add_rc = CURLM_BAD_EASY_HANDLE;
    expect_ll("add other failure", (long long)run(), CURLE_FAILED_INIT);
}

static void
test_loop_failures(void) {
    fake_reset();
    g_fake.perform_rc = CURLM_OUT_OF_MEMORY;
    expect_ll("perform out of memory", (long long)run(), CURLE_OUT_OF_MEMORY);
    expect_detached("perform failure");

    fake_reset();
    g_fake.perform_rc = CURLM_INTERNAL_ERROR;
    expect_ll("perform other failure", (long long)run(), CURLE_FAILED_INIT);

    fake_reset();
    g_fake.idle_rc = CURLM_OUT_OF_MEMORY;
    expect_ll("wait out of memory", (long long)run(), CURLE_OUT_OF_MEMORY);
    expect_detached("wait failure");

    fake_reset();
    g_fake.idle_rc = CURLM_BAD_HANDLE;
    expect_ll("wait other failure", (long long)run(), CURLE_FAILED_INIT);
}

static void
test_cancel(void) {
    fake_reset();
    g_fake.cancel_after_performs = 0;
    expect_ll("cancel before the first perform", (long long)run(), CURLE_ABORTED_BY_CALLBACK);
    expect_ll("nothing performed after an early cancel", g_fake.performs, 0);
    expect_detached("early cancel");

    fake_reset();
    g_fake.performs_until_done = 1000;
    g_fake.cancel_after_performs = 2;
    expect_ll("cancel between waits", (long long)run(), CURLE_ABORTED_BY_CALLBACK);
    expect_ll("cancel seen on the next pass", g_fake.performs, 2);
    expect_detached("cancel");
}

static void
test_quick_exit(void) {
    fake_reset();
    (void)run();
#if TEST_OLD_HEADERS
    expect_ll("no quick exit with headers older than 8.20", g_fake.setopts, 0);
#else
    expect_ll("quick exit set once on 8.20+", g_fake.setopts, 1);
    expect("quick exit option", g_fake.setopt_option == CURLMOPT_QUICK_EXIT && g_fake.setopt_value == 1L);

    fake_reset();
    g_fake.runtime_version = 0x081300U;
    (void)run();
    expect_ll("no quick exit when the loaded library is older than 8.20", g_fake.setopts, 0);

    fake_reset();
    g_fake.runtime_version = 0x081400U;
    g_fake.setopt_rc = CURLM_UNKNOWN_OPTION;
    expect_ll("a refused quick exit leaves the transfer running", (long long)run(), CURLE_OK);
    expect_ll("refused quick exit still performs", g_fake.performs, 3);
#endif
}

static void
test_idle(void) {
#if TEST_OLD_HEADERS
    fake_reset();
    g_fake.wait_ready = 0;
    (void)run();
    expect_ll("pre-7.66 waits between performs", g_fake.waits, 2);
    expect_ll("pre-7.66 sleeps when the wait saw nothing", g_fake.sleeps, 2);
    expect_ll("pre-7.66 never polls", g_fake.polls, 0);

    fake_reset();
    g_fake.wait_ready = 1;
    (void)run();
    expect_ll("pre-7.66 no sleep when a socket was ready", g_fake.sleeps, 0);

    fake_reset();
    g_fake.multi_timeout = 0;
    (void)run();
    expect("pre-7.66 runs at once when libcurl asks to", g_fake.waits == 0 && g_fake.sleeps == 0);
#else
    fake_reset();
    (void)run();
    expect_ll("polls between performs", g_fake.polls, 2);
    expect_ll("never sleeps with curl_multi_poll", g_fake.sleeps, 0);
    expect_ll("never uses curl_multi_wait", g_fake.waits, 0);
#endif
}

int
main(void) {
    test_completion();
    test_setup_failures();
    test_loop_failures();
    test_cancel();
    test_quick_exit();
    test_idle();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
