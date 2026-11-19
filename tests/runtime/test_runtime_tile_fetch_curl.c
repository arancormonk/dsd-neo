// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The built-in libcurl transport of the map tile fetch, without a network.
 *
 * tile_fetch.c is compiled into this test with DSD_TILE_FETCH_TEST_HOOKS, so
 * every option it sets, the transfer itself, the response code and the
 * library's feature bits go through the functions below. That shows which
 * options are set and to what, that a refusal of any one of them stops the
 * request before it is sent, and lets the fake transfer feed the real header,
 * body and progress callbacks the caps and the cancel live in.
 *
 * Built twice: RUNTIME_TILE_FETCH_CURL against the installed headers (either
 * protocol option, as those headers have it), and RUNTIME_TILE_FETCH_CURL_PRE_7_85
 * with the version forced below 7.85, which must take CURLOPT_PROTOCOLS instead
 * of CURLOPT_PROTOCOLS_STR.
 */

#include "tile_fetch_test_hooks.h"

#include <curl/curl.h>
#include <curl/curlver.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/tile_fetch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* tile_fetch.c and curl_common.c are compiled on their own here; the runtime
 * library normally supplies these. */
const char GIT_TAG[] = "v1.2.3";
const char GIT_HASH[] = "abcdef0";

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

static void
expect_str(const char* what, const char* got, const char* want) {
    if (got == NULL || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s (got \"%s\", want \"%s\")\n", what, got != NULL ? got : "(null)", want);
        g_failures++;
    }
}

/* The version tile_fetch.c compiles for: the forced one only when it is lower than the installed headers' (the same
 * rule as TF_CURL_VERSION_NUM there), so the default build works on every supported curl, 7.85 or not. */
#if defined(DSD_TILE_FETCH_TEST_CURL_VERSION_NUM) && DSD_TILE_FETCH_TEST_CURL_VERSION_NUM < LIBCURL_VERSION_NUM
#define TEST_CURL_VERSION_NUM DSD_TILE_FETCH_TEST_CURL_VERSION_NUM
#else
#define TEST_CURL_VERSION_NUM LIBCURL_VERSION_NUM
#endif

#define TEST_HOST "tiles.example.invalid"
#define TEST_URL  "https://" TEST_HOST "/7/64/42.png"

/* ------------------------------------------------------------------------- */
/* Recorded options and the fake transfer                                     */
/* ------------------------------------------------------------------------- */

typedef struct {
    CURLoption option;
    long lval;
    void* pval;
    curl_write_callback data_cb;
    curl_xferinfo_callback xferinfo_cb;
    char text[512]; /**< String options, and HTTPHEADER lines joined by '|'. */
} recorded_opt;

#define MAX_RECORDED 64

typedef struct {
    recorded_opt opts[MAX_RECORDED];
    int count;
    int setopt_calls;
    int refuse_call; /**< 1-based setopt call to refuse; 0 for none. */
    int perform_calls;
    int features;
    const char* const* protocols; /**< curl_version_info()'s protocol list. */
    int global_ready_rc;          /**< What the stand-in for dsd_curl_global_ready() returns. */
    int global_ready_calls;
    int probed_before_init; /**< curl_version_info() was read before the global init guard ran. */
    long response_code;
    CURLcode perform_result; /**< Returned when the fake transfer itself succeeds. */
    const char* headers;     /**< Fed to the header callback line by line. */
    size_t body_len;         /**< Fed to the write callback in 16 KiB pieces. */
    int stall;               /**< Tick the progress callback until it aborts (or 200 ticks). */
    int cancel_after_ticks;
    dsd_tile_fetch_cancel* cancel;
    int ticks;
    int sink_refused; /**< A write or header callback took less than it was given. */
} fake_curl;

static fake_curl g_fake;

static const char* const k_protocols_https[] = {"file", "http", "https", NULL};
static const char* const k_protocols_http_only[] = {"file", "http", NULL};

static void
fake_reset(void) {
    DSD_MEMSET(&g_fake, 0, sizeof(g_fake));
    g_fake.features = CURL_VERSION_ASYNCHDNS | CURL_VERSION_SSL;
    g_fake.protocols = k_protocols_https;
    g_fake.response_code = 200;
    g_fake.perform_result = CURLE_OK;
}

static recorded_opt*
record(CURLoption option) {
    g_fake.setopt_calls++;
    if (g_fake.refuse_call == g_fake.setopt_calls || g_fake.count >= MAX_RECORDED) {
        return NULL;
    }
    recorded_opt* r = &g_fake.opts[g_fake.count++];
    DSD_MEMSET(r, 0, sizeof(*r));
    r->option = option;
    return r;
}

static const recorded_opt*
find_opt(CURLoption option) {
    const recorded_opt* found = NULL;
    for (int i = 0; i < g_fake.count; i++) {
        if (g_fake.opts[i].option == option) {
            found = &g_fake.opts[i];
        }
    }
    return found;
}

CURLcode
dsd_tile_fetch_test_setopt_long(CURL* curl, CURLoption option, long value) {
    (void)curl;
    recorded_opt* r = record(option);
    if (r == NULL) {
        return CURLE_UNKNOWN_OPTION;
    }
    r->lval = value;
    return CURLE_OK;
}

CURLcode
dsd_tile_fetch_test_setopt_ptr(CURL* curl, CURLoption option, void* value) {
    (void)curl;
    recorded_opt* r = record(option);
    if (r == NULL) {
        return CURLE_UNKNOWN_OPTION;
    }
    r->pval = value;
    if (value != NULL
        && (option == CURLOPT_URL || option == CURLOPT_USERAGENT
#if LIBCURL_VERSION_NUM >= 0x075500
            || option == CURLOPT_PROTOCOLS_STR
#endif
            )) {
        (void)DSD_SNPRINTF(r->text, sizeof(r->text), "%s", (const char*)value);
    } else if (option == CURLOPT_HTTPHEADER) {
        size_t used = 0;
        for (const struct curl_slist* s = (const struct curl_slist*)value; s != NULL; s = s->next) {
            const int n = DSD_SNPRINTF(r->text + used, sizeof(r->text) - used, "%s%s", used > 0U ? "|" : "", s->data);
            if (n > 0 && (size_t)n < sizeof(r->text) - used) {
                used += (size_t)n;
            }
        }
    }
    return CURLE_OK;
}

CURLcode
dsd_tile_fetch_test_setopt_data_cb(CURL* curl, CURLoption option, curl_write_callback value) {
    (void)curl;
    recorded_opt* r = record(option);
    if (r == NULL) {
        return CURLE_UNKNOWN_OPTION;
    }
    r->data_cb = value;
    return CURLE_OK;
}

CURLcode
dsd_tile_fetch_test_setopt_xferinfo_cb(CURL* curl, CURLoption option, curl_xferinfo_callback value) {
    (void)curl;
    recorded_opt* r = record(option);
    if (r == NULL) {
        return CURLE_UNKNOWN_OPTION;
    }
    r->xferinfo_cb = value;
    return CURLE_OK;
}

/** Hands one buffer to a recorded data callback the way libcurl would. */
static CURLcode
feed(CURLoption fn_opt, CURLoption data_opt, char* bytes, size_t len) {
    const recorded_opt* fn = find_opt(fn_opt);
    const recorded_opt* data = find_opt(data_opt);
    if (fn == NULL || fn->data_cb == NULL || data == NULL) {
        return CURLE_OK; /* No sink installed: libcurl would write to stdout; the test notices the missing data. */
    }
    if (fn->data_cb(bytes, 1U, len, data->pval) != len) {
        g_fake.sink_refused = 1;
        return CURLE_WRITE_ERROR;
    }
    return CURLE_OK;
}

static CURLcode
feed_headers(void) {
    const char* h = g_fake.headers;
    while (h != NULL && *h != '\0') {
        const char* nl = strchr(h, '\n');
        const size_t len = (nl != NULL) ? (size_t)(nl - h) + 1U : strlen(h);
        char* line = (char*)malloc(len);
        if (line == NULL) {
            return CURLE_OUT_OF_MEMORY;
        }
        DSD_MEMCPY(line, h, len);
        const CURLcode rc = feed(CURLOPT_HEADERFUNCTION, CURLOPT_HEADERDATA, line, len);
        free(line);
        if (rc != CURLE_OK) {
            return rc;
        }
        h += len;
    }
    return CURLE_OK;
}

static CURLcode
feed_body(void) {
    static char chunk[16384];
    size_t left = g_fake.body_len;
    while (left > 0U) {
        const size_t n = left < sizeof(chunk) ? left : sizeof(chunk);
        DSD_MEMSET(chunk, 0x5A, n);
        const CURLcode rc = feed(CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA, chunk, n);
        if (rc != CURLE_OK) {
            return rc;
        }
        left -= n;
    }
    return CURLE_OK;
}

/* libcurl calls the progress callback only while CURLOPT_NOPROGRESS is 0. */
static CURLcode
stall(void) {
    const recorded_opt* noprogress = find_opt(CURLOPT_NOPROGRESS);
    const recorded_opt* fn = find_opt(CURLOPT_XFERINFOFUNCTION);
    const recorded_opt* data = find_opt(CURLOPT_XFERINFODATA);
    const int progress_on = noprogress != NULL && noprogress->lval == 0L && fn != NULL && fn->xferinfo_cb != NULL;
    for (int i = 0; i < 200; i++) {
        if (g_fake.ticks == g_fake.cancel_after_ticks) {
            dsd_tile_fetch_cancel_request(g_fake.cancel);
        }
        g_fake.ticks++;
        if (progress_on && fn->xferinfo_cb(data != NULL ? data->pval : NULL, 0, 0, 0, 0) != 0) {
            return CURLE_ABORTED_BY_CALLBACK;
        }
    }
    return CURLE_OPERATION_TIMEDOUT;
}

CURLcode
dsd_tile_fetch_test_perform(CURL* curl) {
    (void)curl;
    g_fake.perform_calls++;
    if (g_fake.stall) {
        return stall();
    }
    CURLcode rc = feed_headers();
    if (rc == CURLE_OK) {
        rc = feed_body();
    }
    return rc != CURLE_OK ? rc : g_fake.perform_result;
}

CURLcode
dsd_tile_fetch_test_response_code(CURL* curl, long* out) {
    (void)curl;
    *out = g_fake.response_code;
    return CURLE_OK;
}

const curl_version_info_data*
dsd_tile_fetch_test_curl_version_info(void) {
    static curl_version_info_data info;
    if (g_fake.global_ready_calls == 0) {
        g_fake.probed_before_init = 1;
    }
    DSD_MEMSET(&info, 0, sizeof(info));
    info.features = g_fake.features;
    info.protocols = g_fake.protocols;
    return &info;
}

int
dsd_tile_fetch_test_global_ready(void) {
    g_fake.global_ready_calls++;
    return g_fake.global_ready_rc;
}

/* ------------------------------------------------------------------------- */
/* Cases                                                                      */
/* ------------------------------------------------------------------------- */

static dsd_tile_fetch_status
fetch_plain(dsd_tile_fetch_response* resp) {
    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    return dsd_tile_fetch(&req, resp);
}

static void
expect_long_opt(const char* what, CURLoption option, long want) {
    const recorded_opt* r = find_opt(option);
    expect(what, r != NULL);
    if (r != NULL) {
        expect_ll(what, r->lval, want);
    }
}

static void
test_options_set(void) {
    fake_reset();
    g_fake.headers = "HTTP/2 200\r\nETag: \"e1\"\r\n\r\n";
    g_fake.body_len = 100U;
    dsd_tile_fetch_cancel* cancel = dsd_tile_fetch_cancel_create();

    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    req.if_none_match = "\"e0\"";
    req.if_modified_since = "Mon, 01 Jan 2024 00:00:00 GMT";
    req.cancel = cancel;
    dsd_tile_fetch_response resp;
    expect_ll("built-in transport fetches", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_OK);
    expect_ll("body through the write callback", (long long)resp.body_len, 100);
    expect_str("ETag through the header callback", resp.etag, "\"e1\"");
    dsd_tile_fetch_response_free(&resp);

    expect_long_opt("default connect timeout", CURLOPT_CONNECTTIMEOUT_MS, DSD_TILE_FETCH_CONNECT_TIMEOUT_MS_DEFAULT);
    expect_long_opt("default total timeout", CURLOPT_TIMEOUT_MS, DSD_TILE_FETCH_TOTAL_TIMEOUT_MS_DEFAULT);
    expect_long_opt("no signals", CURLOPT_NOSIGNAL, 1L);
    expect_long_opt("no redirects", CURLOPT_FOLLOWLOCATION, 0L);
    expect_long_opt("peer verified", CURLOPT_SSL_VERIFYPEER, 1L);
    expect_long_opt("host verified", CURLOPT_SSL_VERIFYHOST, 2L);
    expect_long_opt("GET", CURLOPT_HTTPGET, 1L);
    expect_long_opt("declared length capped", CURLOPT_MAXFILESIZE, (long)DSD_TILE_FETCH_BODY_CAP_BYTES);
    expect_long_opt("progress callback enabled", CURLOPT_NOPROGRESS, 0L);

#if TEST_CURL_VERSION_NUM < 0x075500
    expect_long_opt("pre-7.85: https only by bitmask", CURLOPT_PROTOCOLS, (long)CURLPROTO_HTTPS);
#if LIBCURL_VERSION_NUM >= 0x075500
    expect("pre-7.85: no protocol string", find_opt(CURLOPT_PROTOCOLS_STR) == NULL);
#endif
#else
    const recorded_opt* proto = find_opt(CURLOPT_PROTOCOLS_STR);
    expect("https only by protocol string", proto != NULL);
    if (proto != NULL) {
        expect_str("protocol string", proto->text, "https");
    }
    expect("no protocol bitmask", find_opt(CURLOPT_PROTOCOLS) == NULL);
#endif

    const recorded_opt* ua = find_opt(CURLOPT_USERAGENT);
    expect("user agent set", ua != NULL);
    if (ua != NULL) {
        expect_str("user agent", ua->text, "dsd-neo/1.2.3 (+https://github.com/arancormonk/dsd-neo)");
    }
    const recorded_opt* url = find_opt(CURLOPT_URL);
    expect("url set", url != NULL);
    if (url != NULL) {
        expect_str("url", url->text, TEST_URL);
    }
    const recorded_opt* hdrs = find_opt(CURLOPT_HTTPHEADER);
    expect("conditional headers set", hdrs != NULL);
    if (hdrs != NULL) {
        expect_str("conditional headers", hdrs->text,
                   "If-None-Match: \"e0\"|If-Modified-Since: Mon, 01 Jan 2024 00:00:00 GMT");
    }
    const recorded_opt* xi = find_opt(CURLOPT_XFERINFOFUNCTION);
    expect("progress callback installed", xi != NULL && xi->xferinfo_cb != NULL);
    const recorded_opt* xid = find_opt(CURLOPT_XFERINFODATA);
    expect("progress callback reads this request's token", xid != NULL && xid->pval == (void*)cancel);
    const recorded_opt* wf = find_opt(CURLOPT_WRITEFUNCTION);
    const recorded_opt* hf = find_opt(CURLOPT_HEADERFUNCTION);
    expect("body and header sinks installed", wf != NULL && wf->data_cb != NULL && hf != NULL && hf->data_cb != NULL);
    expect("no Accept-Encoding asked for", find_opt(CURLOPT_ACCEPT_ENCODING) == NULL);
    expect("redirects never re-enabled", find_opt(CURLOPT_MAXREDIRS) == NULL);

    dsd_tile_fetch_cancel_destroy(cancel);

    /* Without validators no header list is sent, and given timeouts are used. */
    fake_reset();
    g_fake.body_len = 10U;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    req.connect_timeout_ms = 1500;
    req.total_timeout_ms = 4500;
    expect_ll("plain fetch", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_OK);
    dsd_tile_fetch_response_free(&resp);
    hdrs = find_opt(CURLOPT_HTTPHEADER);
    expect("no header list without validators", hdrs != NULL && hdrs->pval == NULL);
    expect_long_opt("given connect timeout", CURLOPT_CONNECTTIMEOUT_MS, 1500L);
    expect_long_opt("given total timeout", CURLOPT_TIMEOUT_MS, 4500L);
}

/* Each option in turn is refused; every refusal must stop the request unsent. */
static void
test_every_setopt_checked(void) {
    fake_reset();
    g_fake.body_len = 10U;
    dsd_tile_fetch_response resp;
    (void)fetch_plain(&resp);
    dsd_tile_fetch_response_free(&resp);
    const int total = g_fake.setopt_calls;
    expect("options were set", total >= 19);

    for (int n = 1; n <= total; n++) {
        fake_reset();
        g_fake.body_len = 10U;
        g_fake.refuse_call = n;
        char what[96];
        (void)DSD_SNPRINTF(what, sizeof(what), "refusing option %d of %d fails closed", n, total);
        expect_ll(what, (long long)fetch_plain(&resp), DSD_TILE_FETCH_ERR_SETUP);
        expect("nothing sent after a refusal", g_fake.perform_calls == 0);
        expect("no option set after a refusal", g_fake.setopt_calls == n);
        expect("setup failure hands out no body", resp.body == NULL);
        dsd_tile_fetch_response_free(&resp);
    }
}

static void
test_caps(void) {
    dsd_tile_fetch_response resp;

    fake_reset();
    g_fake.body_len = DSD_TILE_FETCH_BODY_CAP_BYTES;
    expect_ll("body at the cap", (long long)fetch_plain(&resp), DSD_TILE_FETCH_OK);
    expect_ll("whole body kept", (long long)resp.body_len, (long long)DSD_TILE_FETCH_BODY_CAP_BYTES);
    dsd_tile_fetch_response_free(&resp);

    fake_reset();
    g_fake.body_len = DSD_TILE_FETCH_BODY_CAP_BYTES + 1U;
    expect_ll("body over the cap aborts the transfer", (long long)fetch_plain(&resp), DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect("the body sink itself stops at the cap", g_fake.sink_refused == 1);
    expect("no body over the cap", resp.body == NULL);
    dsd_tile_fetch_response_free(&resp);

    /* A body over the cap after whole headers: the status and Retry-After still say it was a rate limit. */
    fake_reset();
    g_fake.response_code = 429;
    g_fake.headers = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 120\r\n\r\n";
    g_fake.body_len = DSD_TILE_FETCH_BODY_CAP_BYTES + 1U;
    expect_ll("an oversized 429 is too large", (long long)fetch_plain(&resp), DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect_ll("with its status", resp.http_status, 429);
    expect("and its Retry-After",
           (resp.header_present & DSD_TILE_FETCH_HDR_RETRY_AFTER) != 0U && strcmp(resp.retry_after, "120") == 0);
    dsd_tile_fetch_response_free(&resp);

    /* A declared Content-Length over the cap: libcurl 8.5 and older stop before the closing blank line reaches the
     * header callback, so the lines seen so far are kept as they are. */
    fake_reset();
    g_fake.response_code = 503;
    g_fake.headers = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 600000\r\nRetry-After: 30\r\n";
    g_fake.perform_result = CURLE_FILESIZE_EXCEEDED;
    expect_ll("a declared length over the cap is too large", (long long)fetch_plain(&resp),
              DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect_ll("with its status", resp.http_status, 503);
    expect("and the Retry-After seen before libcurl stopped", strcmp(resp.retry_after, "30") == 0);
    dsd_tile_fetch_response_free(&resp);

    /* A write refusal can be the header cap: a block without its blank line is not read then. */
    fake_reset();
    g_fake.response_code = 503;
    g_fake.headers = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 30\r\n";
    g_fake.perform_result = CURLE_WRITE_ERROR;
    expect_ll("a cut header block is too large", (long long)fetch_plain(&resp), DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect("and is not read", resp.header_present == 0U);
    dsd_tile_fetch_response_free(&resp);

    /* 33 lines of 1 KiB: over the 32 KiB header cap. */
    const size_t line = 1024U;
    const size_t lines = DSD_TILE_FETCH_HEADER_CAP_BYTES / line + 1U;
    char* headers = (char*)malloc(lines * line + 1U);
    expect("header buffer", headers != NULL);
    if (headers != NULL) {
        for (size_t i = 0; i < lines; i++) {
            char* l = headers + i * line;
            DSD_MEMSET(l, 'h', line);
            DSD_MEMCPY(l, "X-Pad: ", 7U);
            l[line - 2U] = '\r';
            l[line - 1U] = '\n';
        }
        headers[lines * line] = '\0';
        fake_reset();
        g_fake.headers = headers;
        g_fake.body_len = 10U;
        expect_ll("headers over the cap abort the transfer", (long long)fetch_plain(&resp),
                  DSD_TILE_FETCH_ERR_TOO_LARGE);
        expect("the header sink itself stops at the cap", g_fake.sink_refused == 1);
        expect("headers cut by their cap are not read", resp.header_present == 0U);
        dsd_tile_fetch_response_free(&resp);

        /* One line fewer fits. */
        headers[(lines - 1U) * line] = '\0';
        fake_reset();
        g_fake.headers = headers;
        g_fake.body_len = 10U;
        expect_ll("headers under the cap pass", (long long)fetch_plain(&resp), DSD_TILE_FETCH_OK);
        expect("headers under the cap all taken", g_fake.sink_refused == 0);
        dsd_tile_fetch_response_free(&resp);
        free(headers);
    }
}

static void
test_stalled_transfer_cancelled(void) {
    fake_reset();
    g_fake.stall = 1;
    g_fake.cancel_after_ticks = 3;
    g_fake.cancel = dsd_tile_fetch_cancel_create();
    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    req.cancel = g_fake.cancel;
    dsd_tile_fetch_response resp;
    expect_ll("stalled transfer cancelled through the progress callback", (long long)dsd_tile_fetch(&req, &resp),
              DSD_TILE_FETCH_ERR_CANCELLED);
    expect_ll("aborted on the first tick after the cancel", g_fake.ticks, 4);
    expect_str("cancel reason", resp.error, "cancelled");
    dsd_tile_fetch_response_free(&resp);
    dsd_tile_fetch_cancel_destroy(g_fake.cancel);
    g_fake.cancel = NULL;
}

static void
test_result_mapping(void) {
    static const struct {
        CURLcode code;
        dsd_tile_fetch_status want;
    } k_cases[] = {
        {CURLE_COULDNT_RESOLVE_HOST, DSD_TILE_FETCH_ERR_NETWORK},
        {CURLE_OPERATION_TIMEDOUT, DSD_TILE_FETCH_ERR_NETWORK},
        {CURLE_PEER_FAILED_VERIFICATION, DSD_TILE_FETCH_ERR_NETWORK},
        {CURLE_UNSUPPORTED_PROTOCOL, DSD_TILE_FETCH_ERR_NETWORK},
        {CURLE_FILESIZE_EXCEEDED, DSD_TILE_FETCH_ERR_TOO_LARGE},
        {CURLE_WRITE_ERROR, DSD_TILE_FETCH_ERR_TOO_LARGE},
        {CURLE_ABORTED_BY_CALLBACK, DSD_TILE_FETCH_ERR_CANCELLED},
        {CURLE_OUT_OF_MEMORY, DSD_TILE_FETCH_ERR_NOMEM},
        {CURLE_URL_MALFORMAT, DSD_TILE_FETCH_ERR_INVALID}, /* a bad host or port: a settings problem, not an outage */
    };

    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        fake_reset();
        g_fake.perform_result = k_cases[i].code;
        dsd_tile_fetch_response resp;
        expect_ll("libcurl result mapping", (long long)fetch_plain(&resp), (long long)k_cases[i].want);
        expect("failure reason has no URL", resp.error[0] != '\0' && strstr(resp.error, TEST_HOST) == NULL);
        dsd_tile_fetch_response_free(&resp);
    }

    /* A redirect completes as a transfer but is never followed. */
    fake_reset();
    g_fake.response_code = 302;
    g_fake.headers = "HTTP/1.1 302 Found\r\nLocation: https://elsewhere.example.invalid/\r\n\r\n";
    dsd_tile_fetch_response resp;
    expect_ll("30x is a failure", (long long)fetch_plain(&resp), DSD_TILE_FETCH_ERR_REDIRECT);
    expect_ll("30x transferred once", g_fake.perform_calls, 1);
    dsd_tile_fetch_response_free(&resp);
}

static void
test_capability(void) {
    fake_reset();
    expect("available with an asynchronous resolver", dsd_tile_fetch_available() == 1);
    expect("the probe runs after libcurl's global initialization", g_fake.probed_before_init == 0);

    fake_reset();
    g_fake.protocols = k_protocols_http_only;
    expect("unavailable without https", dsd_tile_fetch_available() == 0);
    {
        dsd_tile_fetch_response noHttps;
        expect_ll("built-in transport refuses without https", (long long)fetch_plain(&noHttps),
                  DSD_TILE_FETCH_ERR_UNSUPPORTED);
        dsd_tile_fetch_response_free(&noHttps);
    }

    fake_reset();
    g_fake.global_ready_rc = -1;
    expect("unavailable when libcurl cannot be initialized", dsd_tile_fetch_available() == 0);
    expect("and nothing probed", g_fake.probed_before_init == 0);
    {
        dsd_tile_fetch_response failed;
        expect_ll("an init failure is a setup failure", (long long)fetch_plain(&failed), DSD_TILE_FETCH_ERR_SETUP);
        dsd_tile_fetch_response_free(&failed);
    }

    fake_reset();
    g_fake.features = CURL_VERSION_SSL; /* synchronous resolver */
    expect("unavailable with a synchronous resolver", dsd_tile_fetch_available() == 0);
    dsd_tile_fetch_response resp;
    expect_ll("built-in transport refuses with a synchronous resolver", (long long)fetch_plain(&resp),
              DSD_TILE_FETCH_ERR_UNSUPPORTED);
    expect_str("unavailable reason", resp.error, "online map tiles are not available in this build");
    expect("refused before any option", g_fake.setopt_calls == 0 && g_fake.perform_calls == 0);
    dsd_tile_fetch_response_free(&resp);
}

/* A session's share handle goes on every fetch that names it, the same one each time; without one, none. */
static void
test_session_share(void) {
    dsd_tile_fetch_session* session = dsd_tile_fetch_session_create();
    expect("session created", session != NULL);
    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    req.session = session;
    dsd_tile_fetch_response resp;

    fake_reset();
    g_fake.body_len = 100U;
    expect_ll("first fetch in a session", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_OK);
    dsd_tile_fetch_response_free(&resp);
    const recorded_opt* first = find_opt(CURLOPT_SHARE);
    void* share = first != NULL ? first->pval : NULL;
    expect("the session's share handle is set", share != NULL);

    fake_reset();
    g_fake.body_len = 100U;
    expect_ll("second fetch in the session", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_OK);
    dsd_tile_fetch_response_free(&resp);
    const recorded_opt* second = find_opt(CURLOPT_SHARE);
    expect("the same share handle again, so its connections are reused", second != NULL && second->pval == share);

    fake_reset();
    g_fake.body_len = 100U;
    expect_ll("a fetch without a session", (long long)fetch_plain(&resp), DSD_TILE_FETCH_OK);
    dsd_tile_fetch_response_free(&resp);
    expect("no share handle without a session", find_opt(CURLOPT_SHARE) == NULL);

    fake_reset();
    g_fake.body_len = 100U;
    g_fake.refuse_call = 1; /* the share option is the first one set */
    expect_ll("a refused share option fails closed", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_ERR_SETUP);
    expect("nothing performed", g_fake.perform_calls == 0);
    dsd_tile_fetch_response_free(&resp);
    dsd_tile_fetch_session_destroy(session);
    dsd_tile_fetch_session_destroy(NULL);
}

int
main(void) {
    test_options_set();
    test_every_setopt_checked();
    test_caps();
    test_stalled_transfer_cancelled();
    test_result_mapping();
    test_capability();
    test_session_share();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
