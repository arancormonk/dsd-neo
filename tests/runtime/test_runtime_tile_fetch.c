// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Map tile fetch through the public API: request validation, the injected
 * transport, header parsing, status classification, the caps, the cancel token
 * and the User-Agent.
 *
 * Built twice. RUNTIME_TILE_FETCH links dsd-neo_runtime as shipped, so with
 * libcurl it also runs a stalled HTTPS transfer against a loopback socket and
 * cancels it. RUNTIME_TILE_FETCH_NO_CURL compiles tile_fetch.c alone without
 * USE_CURL, which pins the stub: unavailable, the built-in transport refuses,
 * and an injected transport still works.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/tile_fetch.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <curl/curl.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <netinet/in.h>

#include <sys/socket.h>
#include "test_env.h"
#ifdef DSD_TILE_FETCH_TEST_FAKE_RESOLVER
#include "fake_getaddrinfo.h"
#endif
#elif defined(USE_CURL)
#include <curl/curl.h>
#endif

#ifdef DSD_TILE_FETCH_TEST_STANDALONE
/* tile_fetch.c is compiled on its own here; the runtime library normally supplies these. */
const char GIT_TAG[] = "v1.2.3-4-gabcdef0";
const char GIT_HASH[] = "abcdef0";
#endif

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

#define TEST_URL "https://tiles.example.invalid/7/64/42.png"

/* ------------------------------------------------------------------------- */
/* Fake transport                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    int calls;
    int rc;                        /**< What perform() returns. */
    dsd_tile_fetch_status fail_as; /**< exchange->status when rc != 0. */
    const char* fail_text;
    long http_status;
    const char* headers;
    size_t body_len;
    char seen_inm[DSD_TILE_FETCH_ETAG_MAX];
    char seen_ims[DSD_TILE_FETCH_DATE_FIELD_MAX];
} fake_transport;

static char*
dup_bytes(const char* text, size_t len) {
    char* out = (char*)malloc(len + 1U);
    if (out != NULL) {
        DSD_MEMCPY(out, text, len);
        out[len] = '\0';
    }
    return out;
}

static int
fake_perform(void* ctx, const dsd_tile_fetch_request* req, dsd_tile_fetch_exchange* ex) {
    fake_transport* f = (fake_transport*)ctx;
    f->calls++;
    (void)DSD_SNPRINTF(f->seen_inm, sizeof(f->seen_inm), "%s", req->if_none_match != NULL ? req->if_none_match : "");
    (void)DSD_SNPRINTF(f->seen_ims, sizeof(f->seen_ims), "%s",
                       req->if_modified_since != NULL ? req->if_modified_since : "");
    ex->http_status = f->http_status;
    if (f->headers != NULL) {
        ex->headers = dup_bytes(f->headers, strlen(f->headers));
        ex->headers_len = strlen(f->headers);
    }
    if (f->body_len > 0U) {
        ex->body = (unsigned char*)calloc(1U, f->body_len);
        ex->body_len = (ex->body != NULL) ? f->body_len : 0U;
        if (ex->body != NULL) {
            ex->body[0] = 0x89U; /* PNG signature's first byte; content is not inspected */
        }
    }
    if (f->rc != 0) {
        ex->status = f->fail_as;
        if (f->fail_text != NULL) {
            (void)DSD_SNPRINTF(ex->error, sizeof(ex->error), "%s", f->fail_text);
        }
    }
    return f->rc;
}

static void
fake_init(fake_transport* f, dsd_tile_fetch_transport* t, long status, const char* headers, size_t body_len) {
    DSD_MEMSET(f, 0, sizeof(*f));
    f->http_status = status;
    f->headers = headers;
    f->body_len = body_len;
    t->perform = fake_perform;
    t->ctx = f;
}

static dsd_tile_fetch_status
fetch_with(const dsd_tile_fetch_transport* t, dsd_tile_fetch_response* resp) {
    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    req.transport = t;
    return dsd_tile_fetch(&req, resp);
}

/* ------------------------------------------------------------------------- */
/* Cases                                                                      */
/* ------------------------------------------------------------------------- */

static void
test_user_agent(void) {
    /* The product version is the tag without its leading "v". */
    const char* tag = GIT_TAG;
    if (memcmp(tag, "v", 1U) == 0) {
        tag++;
    }
    char want[160];
    (void)DSD_SNPRINTF(want, sizeof(want), "dsd-neo/%s (+https://github.com/arancormonk/dsd-neo)", tag);

    char ua[128];
    expect("user agent fits 128 bytes", dsd_tile_fetch_user_agent(ua, sizeof(ua)) == 0);
    expect_str("user agent text", ua, want);

    char small[16];
    expect("user agent refuses a short buffer", dsd_tile_fetch_user_agent(small, sizeof(small)) != 0);
    expect_str("short buffer left empty", small, "");
    expect("user agent refuses NULL", dsd_tile_fetch_user_agent(NULL, 64U) != 0);
}

static void
test_cancel_token(void) {
    dsd_tile_fetch_cancel* c = dsd_tile_fetch_cancel_create();
    expect("token allocated", c != NULL);
    expect("new token is clear", dsd_tile_fetch_cancel_requested(c) == 0);
    dsd_tile_fetch_cancel_request(c);
    expect("requested token reads set", dsd_tile_fetch_cancel_requested(c) == 1);
    dsd_tile_fetch_cancel_clear(c);
    expect("cleared token reads clear", dsd_tile_fetch_cancel_requested(c) == 0);
    expect("NULL token reads clear", dsd_tile_fetch_cancel_requested(NULL) == 0);
    dsd_tile_fetch_cancel_request(NULL);
    dsd_tile_fetch_cancel_clear(NULL);
    dsd_tile_fetch_cancel_destroy(NULL);
    dsd_tile_fetch_cancel_destroy(c);
}

static void
expect_invalid(const char* what, const dsd_tile_fetch_request* req, const fake_transport* f, int calls_before) {
    dsd_tile_fetch_response resp;
    const dsd_tile_fetch_status st = dsd_tile_fetch(req, &resp);
    expect_ll(what, (long long)st, (long long)DSD_TILE_FETCH_ERR_INVALID);
    expect("invalid request sends nothing", f->calls == calls_before);
    expect("invalid request has a reason", resp.error[0] != '\0');
    expect("reason never carries the URL", strstr(resp.error, "example") == NULL);
    dsd_tile_fetch_response_free(&resp);
}

static void
test_validation(void) {
    fake_transport f;
    dsd_tile_fetch_transport t;
    fake_init(&f, &t, 200, "HTTP/1.1 200 OK\r\n\r\n", 8U);

    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.transport = &t;

    expect_invalid("NULL url is invalid", &req, &f, 0);
    req.url = "http://tiles.example.invalid/1/0/0.png";
    expect_invalid("plain http is refused", &req, &f, 0);
    req.url = "ftp://tiles.example.invalid/1/0/0.png";
    expect_invalid("other schemes are refused", &req, &f, 0);
    req.url = "https://";
    expect_invalid("scheme alone is refused", &req, &f, 0);
    req.url = "https:///1/0/0.png";
    expect_invalid("empty host is refused", &req, &f, 0);
    req.url = "https://tiles.example.invalid/1/0 /0.png";
    expect_invalid("a space is refused", &req, &f, 0);
    req.url = "https://tiles.example.invalid/1/0/0.png\r\nX: y";
    expect_invalid("CRLF in the URL is refused", &req, &f, 0);

    char* longurl = (char*)malloc(DSD_TILE_FETCH_URL_MAX + 2U);
    expect("long url buffer", longurl != NULL);
    if (longurl != NULL) {
        DSD_MEMSET(longurl, 'a', DSD_TILE_FETCH_URL_MAX + 1U);
        DSD_MEMCPY(longurl, "https://", 8U);
        longurl[DSD_TILE_FETCH_URL_MAX + 1U] = '\0';
        req.url = longurl;
        expect_invalid("an over-long URL is refused", &req, &f, 0);
        longurl[DSD_TILE_FETCH_URL_MAX] = '\0';
        dsd_tile_fetch_response resp;
        expect_ll("a URL at the limit is accepted", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_OK);
        dsd_tile_fetch_response_free(&resp);
        free(longurl);
    }

    req.url = "HTTPS://tiles.example.invalid/1/0/0.png";
    dsd_tile_fetch_response resp;
    const int calls = f.calls;
    expect_ll("scheme is case-insensitive", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_OK);
    expect("valid request reaches the transport", f.calls == calls + 1);
    dsd_tile_fetch_response_free(&resp);

    req.url = TEST_URL;
    req.if_none_match = "\"abc\"\r\nX-Injected: 1";
    expect_invalid("CRLF in a stored ETag is refused", &req, &f, f.calls);
    req.if_none_match = NULL;
    req.if_modified_since = "Mon, 01 Jan 2024 00:00:00 GMT\n";
    expect_invalid("LF in a stored Last-Modified is refused", &req, &f, f.calls);
    req.if_modified_since = NULL;
    req.connect_timeout_ms = -1;
    expect_invalid("a negative timeout is refused", &req, &f, f.calls);
    req.connect_timeout_ms = 0;
    req.total_timeout_ms = DSD_TILE_FETCH_TIMEOUT_MS_MAX + 1;
    expect_invalid("an over-long timeout is refused", &req, &f, f.calls);

    expect_ll("NULL request", (long long)dsd_tile_fetch(NULL, &resp), DSD_TILE_FETCH_ERR_INVALID);
    dsd_tile_fetch_response_free(&resp);
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    expect_ll("NULL response", (long long)dsd_tile_fetch(&req, NULL), DSD_TILE_FETCH_ERR_INVALID);
}

static void
test_conditional_values_pass_through(void) {
    fake_transport f;
    dsd_tile_fetch_transport t;
    fake_init(&f, &t, 304, "HTTP/1.1 304 Not Modified\r\nETag: \"v2\"\r\n\r\n", 0U);
    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    req.transport = &t;
    req.if_none_match = "\"v1\"";
    req.if_modified_since = "Mon, 01 Jan 2024 00:00:00 GMT";
    dsd_tile_fetch_response resp;
    expect_ll("304 is NOT_MODIFIED", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_NOT_MODIFIED);
    expect_str("transport saw If-None-Match", f.seen_inm, "\"v1\"");
    expect_str("transport saw If-Modified-Since", f.seen_ims, "Mon, 01 Jan 2024 00:00:00 GMT");
    expect_str("304 carries the new ETag", resp.etag, "\"v2\"");
    expect("304 has no body", resp.body == NULL && resp.body_len == 0U);
    expect_ll("304 status kept", resp.http_status, 304);
    dsd_tile_fetch_response_free(&resp);
}

static void
test_cancel_before_send(void) {
    fake_transport f;
    dsd_tile_fetch_transport t;
    fake_init(&f, &t, 200, NULL, 8U);
    dsd_tile_fetch_cancel* c = dsd_tile_fetch_cancel_create();
    dsd_tile_fetch_cancel_request(c);
    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    req.transport = &t;
    req.cancel = c;
    dsd_tile_fetch_response resp;
    expect_ll("set token cancels before sending", (long long)dsd_tile_fetch(&req, &resp), DSD_TILE_FETCH_ERR_CANCELLED);
    expect("cancelled request sends nothing", f.calls == 0);
    dsd_tile_fetch_response_free(&resp);
    dsd_tile_fetch_cancel_destroy(c);
}

static const char k_full_headers[] = "HTTP/2 200\r\n"
                                     "content-type: image/png\r\n"
                                     "ETAG:   \"tile-7-64-42\"  \r\n"
                                     "Last-Modified: Tue, 02 Jan 2024 03:04:05 GMT\r\n"
                                     "Cache-Control: max-age=86400\r\n"
                                     "Cache-Control: stale-while-revalidate=60\r\n"
                                     "Expires: Wed, 03 Jan 2024 03:04:05 GMT\r\n"
                                     "date: Tue, 02 Jan 2024 03:04:05 GMT\r\n"
                                     "Age: 120\r\n"
                                     "Vary: Accept-Encoding\r\n"
                                     "vary: User-Agent\r\n"
                                     "Retry-After: 30\r\n"
                                     "\r\n";

static void
test_ok_with_headers(void) {
    fake_transport f;
    dsd_tile_fetch_transport t;
    fake_init(&f, &t, 200, k_full_headers, 1234U);
    dsd_tile_fetch_response resp;
    expect_ll("200 with body is OK", (long long)fetch_with(&t, &resp), DSD_TILE_FETCH_OK);
    expect("body moved to the response", resp.body != NULL && resp.body_len == 1234U && resp.body[0] == 0x89U);
    expect_str("ETag trimmed, any case", resp.etag, "\"tile-7-64-42\"");
    expect_str("Last-Modified", resp.last_modified, "Tue, 02 Jan 2024 03:04:05 GMT");
    expect_str("repeated Cache-Control joined", resp.cache_control, "max-age=86400, stale-while-revalidate=60");
    expect_str("Expires", resp.expires, "Wed, 03 Jan 2024 03:04:05 GMT");
    expect_str("Date", resp.date, "Tue, 02 Jan 2024 03:04:05 GMT");
    expect_str("Age", resp.age, "120");
    expect_str("repeated Vary joined", resp.vary, "Accept-Encoding, User-Agent");
    expect_str("Retry-After", resp.retry_after, "30");
    expect_ll("all eight present", (long long)resp.header_present, 0xFF);
    expect_ll("none invalid", (long long)resp.header_invalid, 0);
    dsd_tile_fetch_response_free(&resp);
    expect("free clears the body", resp.body == NULL && resp.body_len == 0U);
    dsd_tile_fetch_response_free(&resp);
    dsd_tile_fetch_response_free(NULL);
}

static void
test_status_classes(void) {
    static const struct {
        long code;
        size_t body;
        dsd_tile_fetch_status want;
    } k_cases[] = {
        {301, 10U, DSD_TILE_FETCH_ERR_REDIRECT}, {302, 0U, DSD_TILE_FETCH_ERR_REDIRECT},
        {307, 0U, DSD_TILE_FETCH_ERR_REDIRECT},  {308, 0U, DSD_TILE_FETCH_ERR_REDIRECT},
        {300, 0U, DSD_TILE_FETCH_ERR_REDIRECT},  {304, 10U, DSD_TILE_FETCH_NOT_MODIFIED},
        {200, 0U, DSD_TILE_FETCH_ERR_HTTP},      {204, 0U, DSD_TILE_FETCH_ERR_HTTP},
        {206, 10U, DSD_TILE_FETCH_ERR_HTTP},     {404, 10U, DSD_TILE_FETCH_ERR_HTTP},
        {429, 0U, DSD_TILE_FETCH_ERR_HTTP},      {503, 0U, DSD_TILE_FETCH_ERR_HTTP},
    };

    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        fake_transport f;
        dsd_tile_fetch_transport t;
        fake_init(&f, &t, k_cases[i].code, "HTTP/1.1 999 X\r\nRetry-After: 120\r\nLocation: https://x.invalid/\r\n\r\n",
                  k_cases[i].body);
        dsd_tile_fetch_response resp;
        char what[64];
        (void)DSD_SNPRINTF(what, sizeof(what), "HTTP %ld classification", k_cases[i].code);
        expect_ll(what, (long long)fetch_with(&t, &resp), (long long)k_cases[i].want);
        expect_ll("http status kept", resp.http_status, k_cases[i].code);
        expect("non-OK has no body", resp.body == NULL);
        expect_str("Retry-After kept on every status", resp.retry_after, "120");
        if (k_cases[i].want != DSD_TILE_FETCH_NOT_MODIFIED) {
            expect("failure has a reason", resp.error[0] != '\0');
        }
        dsd_tile_fetch_response_free(&resp);
    }
}

static void
test_caps(void) {
    fake_transport f;
    dsd_tile_fetch_transport t;
    dsd_tile_fetch_response resp;

    fake_init(&f, &t, 200, NULL, DSD_TILE_FETCH_BODY_CAP_BYTES);
    expect_ll("body at the cap is accepted", (long long)fetch_with(&t, &resp), DSD_TILE_FETCH_OK);
    dsd_tile_fetch_response_free(&resp);

    fake_init(&f, &t, 200, NULL, DSD_TILE_FETCH_BODY_CAP_BYTES + 1U);
    expect_ll("body over the cap is refused", (long long)fetch_with(&t, &resp), DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect("refused body is not handed out", resp.body == NULL);
    dsd_tile_fetch_response_free(&resp);

    /* An outage page over the cap is still an outage: its status and Retry-After are kept. */
    fake_init(&f, &t, 503, "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 30\r\n\r\n",
              DSD_TILE_FETCH_BODY_CAP_BYTES + 1U);
    expect_ll("an oversized 503 is too large", (long long)fetch_with(&t, &resp), DSD_TILE_FETCH_ERR_TOO_LARGE);
    expect("with its status and Retry-After", resp.http_status == 503 && strcmp(resp.retry_after, "30") == 0
                                                  && (resp.header_present & DSD_TILE_FETCH_HDR_RETRY_AFTER) != 0U);
    dsd_tile_fetch_response_free(&resp);

    const size_t big = DSD_TILE_FETCH_HEADER_CAP_BYTES + 1U;
    char* headers = (char*)malloc(big + 1U);
    expect("header buffer", headers != NULL);
    if (headers != NULL) {
        DSD_MEMSET(headers, 'x', big);
        DSD_MEMCPY(headers, "HTTP/1.1 200 OK\r\nX-Pad: ", 24U);
        headers[big] = '\0';
        fake_init(&f, &t, 200, headers, 10U);
        expect_ll("header block over the cap is refused", (long long)fetch_with(&t, &resp),
                  DSD_TILE_FETCH_ERR_TOO_LARGE);
        expect("and none of it is read", resp.header_present == 0U);
        dsd_tile_fetch_response_free(&resp);
        free(headers);
    }
}

typedef struct {
    const char* what;
    const char* headers;
    unsigned int want_present;
    unsigned int want_invalid;
    const char* want_etag;
    const char* want_cache_control;
    const char* want_expires;
} header_case;

static void
run_header_case(const header_case* c) {
    fake_transport f;
    dsd_tile_fetch_transport t;
    fake_init(&f, &t, 200, c->headers, 16U);
    dsd_tile_fetch_response resp;
    char what[160];
    (void)DSD_SNPRINTF(what, sizeof(what), "%s: status", c->what);
    expect_ll(what, (long long)fetch_with(&t, &resp), DSD_TILE_FETCH_OK);
    (void)DSD_SNPRINTF(what, sizeof(what), "%s: present", c->what);
    expect_ll(what, (long long)resp.header_present, (long long)c->want_present);
    (void)DSD_SNPRINTF(what, sizeof(what), "%s: invalid", c->what);
    expect_ll(what, (long long)resp.header_invalid, (long long)c->want_invalid);
    (void)DSD_SNPRINTF(what, sizeof(what), "%s: ETag", c->what);
    expect_str(what, resp.etag, c->want_etag);
    (void)DSD_SNPRINTF(what, sizeof(what), "%s: Cache-Control", c->what);
    expect_str(what, resp.cache_control, c->want_cache_control);
    (void)DSD_SNPRINTF(what, sizeof(what), "%s: Expires", c->what);
    expect_str(what, resp.expires, c->want_expires);
    dsd_tile_fetch_response_free(&resp);
}

static void
test_header_parsing(void) {
    enum {
        E = DSD_TILE_FETCH_HDR_ETAG,
        CC = DSD_TILE_FETCH_HDR_CACHE_CONTROL,
        EX = DSD_TILE_FETCH_HDR_EXPIRES,
        D = DSD_TILE_FETCH_HDR_DATE,
        LM = DSD_TILE_FETCH_HDR_LAST_MODIFIED,
    };

    static const header_case k_cases[] = {
        {"interim block dropped",
         "HTTP/1.1 100 Continue\r\nETag: \"old\"\r\nCache-Control: no-store\r\n\r\n"
         "HTTP/1.1 200 OK\r\nETag: \"new\"\r\n\r\n",
         E, 0U, "\"new\"", "", ""},
        {"proxy CONNECT block dropped",
         "HTTP/1.1 200 Connection established\r\nExpires: 0\r\n\r\n"
         "HTTP/2 200\r\nCache-Control: max-age=60\r\n\r\n",
         CC, 0U, "", "max-age=60", ""},
        {"trailers after the block ignored",
         "HTTP/1.1 200 OK\r\nCache-Control: max-age=60\r\n\r\n"
         "Cache-Control: no-store\r\nETag: \"t\"\r\n",
         CC, 0U, "", "max-age=60", ""},
        {"no status line still parses", "ETag: \"x\"\r\n", E, 0U, "\"x\"", "", ""},
        {"LF-only line ends", "HTTP/1.1 200 OK\nETag: \"lf\"\nExpires: Thu, 01 Jan 1970 00:00:00 GMT\n\n", E | EX, 0U,
         "\"lf\"", "", "Thu, 01 Jan 1970 00:00:00 GMT"},
        {"repeated ETag is invalid", "HTTP/1.1 200 OK\r\nETag: \"a\"\r\nETag: \"b\"\r\n\r\n", E, E, "", "", ""},
        {"empty Expires is invalid", "HTTP/1.1 200 OK\r\nExpires:   \r\n\r\n", EX, EX, "", "", ""},
        {"malformed Expires kept as text", "HTTP/1.1 200 OK\r\nExpires: 0\r\n\r\n", EX, 0U, "", "", "0"},
        {"a tab in ETag cannot be sent back: invalid", "HTTP/1.1 200 OK\r\nETag: \"a\tb\"\r\n\r\n", E, E, "", "", ""},
        {"a tab in Last-Modified cannot be sent back: invalid",
         "HTTP/1.1 200 OK\r\nLast-Modified: Wed,\t03 Jan 2024 03:04:05 GMT\r\n\r\n", LM, LM, "", "", ""},
        {"control byte in Date is invalid", "HTTP/1.1 200 OK\r\nDate: Tue,\x01 02 Jan\r\n\r\n", D, D, "", "", ""},
        {"obs-fold joins a list field", "HTTP/1.1 200 OK\r\nCache-Control: max-age=60,\r\n no-cache\r\n\r\n", CC, 0U,
         "", "max-age=60, no-cache", ""},
        {"obs-fold joins a single field", "HTTP/1.1 200 OK\r\nExpires: Wed, 03 Jan 2024\r\n\t03:04:05 GMT\r\n\r\n", EX,
         0U, "", "", "Wed, 03 Jan 2024 03:04:05 GMT"},
        {"line without a colon ignored", "HTTP/1.1 200 OK\r\nCache-Control no-store\r\n\r\n", 0U, 0U, "", "", ""},
        {"space before the colon ignored", "HTTP/1.1 200 OK\r\nETag : \"s\"\r\n\r\n", 0U, 0U, "", "", ""},
        {"similar names ignored", "HTTP/1.1 200 OK\r\nETags: \"s\"\r\nX-Cache-Control: no-store\r\n\r\n", 0U, 0U, "",
         "", ""},
        {"empty Cache-Control present", "HTTP/1.1 200 OK\r\nCache-Control:\r\n\r\n", CC, 0U, "", "", ""},
    };
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        run_header_case(&k_cases[i]);
    }

    /* An ETag longer than its buffer is invalid, not truncated: a cut ETag would
     * be sent back as a validator that never matches. */
    char headers[512];
    char etag[DSD_TILE_FETCH_ETAG_MAX + 8U];
    DSD_MEMSET(etag, 'e', sizeof(etag) - 1U);
    etag[sizeof(etag) - 1U] = '\0';
    (void)DSD_SNPRINTF(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nETag: %s\r\n\r\n", etag);
    const header_case long_etag = {"over-long ETag is invalid", headers, E, E, "", "", ""};
    run_header_case(&long_etag);

    /* An ETag that just fits is kept. */
    etag[DSD_TILE_FETCH_ETAG_MAX - 1U] = '\0';
    (void)DSD_SNPRINTF(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nETag: %s\r\n\r\n", etag);
    const header_case fit_etag = {"ETag at the limit is kept", headers, E, 0U, etag, "", ""};
    run_header_case(&fit_etag);
}

static void
expect_bad_header(const char* what, const char* headers) {
    fake_transport f;
    dsd_tile_fetch_transport t;
    fake_init(&f, &t, 200, headers, 16U);
    dsd_tile_fetch_response resp;
    expect_ll(what, (long long)fetch_with(&t, &resp), DSD_TILE_FETCH_ERR_BAD_HEADER);
    expect("bad header hands out no body", resp.body == NULL);
    expect("bad header leaves no fields", resp.header_present == 0U && resp.cache_control[0] == '\0');
    dsd_tile_fetch_response_free(&resp);
}

static void
test_bad_list_headers(void) {
    char headers[2048];
    char value[DSD_TILE_FETCH_CACHE_CONTROL_MAX + 8U];
    DSD_MEMSET(value, 'a', sizeof(value) - 1U);
    value[sizeof(value) - 1U] = '\0';
    (void)DSD_SNPRINTF(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nCache-Control: no-store, %s\r\n\r\n", value);
    expect_bad_header("over-long Cache-Control fails the fetch", headers);

    /* Two lines that fit alone but not joined. */
    value[DSD_TILE_FETCH_CACHE_CONTROL_MAX / 2U + 8U] = '\0';
    (void)DSD_SNPRINTF(headers, sizeof(headers), "HTTP/1.1 200 OK\r\nCache-Control: %s\r\nCache-Control: %s\r\n\r\n",
                       value, value);
    expect_bad_header("Cache-Control over the limit once joined fails the fetch", headers);

    expect_bad_header("non-ASCII in Vary fails the fetch", "HTTP/1.1 200 OK\r\nVary: User-Agent\xC3\xA9\r\n\r\n");
    expect_bad_header("control byte in Cache-Control fails the fetch",
                      "HTTP/1.1 200 OK\r\nCache-Control: no-\x7Fstore\r\n\r\n");
}

static void
test_transport_failures(void) {
    static const struct {
        dsd_tile_fetch_status reported;
        dsd_tile_fetch_status want;
    } k_cases[] = {
        {DSD_TILE_FETCH_ERR_CANCELLED, DSD_TILE_FETCH_ERR_CANCELLED},
        {DSD_TILE_FETCH_ERR_TOO_LARGE, DSD_TILE_FETCH_ERR_TOO_LARGE},
        {DSD_TILE_FETCH_ERR_UNSUPPORTED, DSD_TILE_FETCH_ERR_UNSUPPORTED},
        {DSD_TILE_FETCH_ERR_SETUP, DSD_TILE_FETCH_ERR_SETUP},
        {DSD_TILE_FETCH_ERR_NOMEM, DSD_TILE_FETCH_ERR_NOMEM},
        {DSD_TILE_FETCH_OK, DSD_TILE_FETCH_ERR_NETWORK},
        {DSD_TILE_FETCH_NOT_MODIFIED, DSD_TILE_FETCH_ERR_NETWORK},
        {DSD_TILE_FETCH_ERR_HTTP, DSD_TILE_FETCH_ERR_NETWORK},
    };

    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        fake_transport f;
        dsd_tile_fetch_transport t;
        fake_init(&f, &t, 0, "HTTP/1.1 200 OK\r\nETag: \"x\"\r\n\r\n", 32U);
        f.rc = -1;
        f.fail_as = k_cases[i].reported;
        f.fail_text = "connection reset";
        dsd_tile_fetch_response resp;
        expect_ll("transport failure class", (long long)fetch_with(&t, &resp), (long long)k_cases[i].want);
        expect_str("transport message kept", resp.error, "connection reset");
        expect("failed exchange hands out no body", resp.body == NULL);
        if (k_cases[i].want == DSD_TILE_FETCH_ERR_TOO_LARGE) {
            /* Only the body was over its cap: the whole header block is still read (status, Retry-After). */
            expect("an oversized body keeps its headers", resp.header_present == DSD_TILE_FETCH_HDR_ETAG);
        } else {
            expect("failed exchange parses no headers", resp.header_present == 0U);
        }
        dsd_tile_fetch_response_free(&resp);
    }
}

static void
test_status_names(void) {
    for (int s = DSD_TILE_FETCH_OK; s <= DSD_TILE_FETCH_ERR_NOMEM; s++) {
        const char* name = dsd_tile_fetch_status_name((dsd_tile_fetch_status)s);
        expect("every status has a name", name != NULL && name[0] != '\0' && strcmp(name, "unknown") != 0);
    }
    expect_str("out of range status", dsd_tile_fetch_status_name((dsd_tile_fetch_status)99), "unknown");
}

static void
test_availability(void) {
#ifdef USE_CURL
    /* After dsd_tile_fetch_available(): it runs libcurl's global init first, which curl_version_info() needs. */
    const int available = dsd_tile_fetch_available();
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    const int async_dns = (info != NULL && (info->features & CURL_VERSION_ASYNCHDNS) != 0) ? 1 : 0;
    int https = 0;
    for (const char* const* p = (info != NULL) ? info->protocols : NULL; p != NULL && *p != NULL; p++) {
        https = https || strcmp(*p, "https") == 0;
    }
    expect_ll("available exactly when libcurl resolves asynchronously and speaks https", available, async_dns && https);
#else
    expect_ll("unavailable without libcurl", dsd_tile_fetch_available(), 0);
#endif
    if (dsd_tile_fetch_available()) {
        return;
    }
    /* The built-in transport refuses before touching the network. */
    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = TEST_URL;
    dsd_tile_fetch_response resp;
    expect_ll("built-in transport refuses when unavailable", (long long)dsd_tile_fetch(&req, &resp),
              DSD_TILE_FETCH_ERR_UNSUPPORTED);
    expect_str("unavailable reason", resp.error, "online map tiles are not available in this build");
    dsd_tile_fetch_response_free(&resp);
}

/* ------------------------------------------------------------------------- */
/* Stalled HTTPS transfer on a loopback socket (real libcurl)                 */
/* ------------------------------------------------------------------------- */

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE

typedef struct {
    dsd_socket_t listen_sock;
    dsd_thread_t thread;
    atomic_int stop;
    atomic_int accepted;
} stall_server;

/* Accepts one connection and never answers it: the TLS handshake waits forever. */
static DSD_THREAD_RETURN_TYPE
stall_server_thread(void* arg) {
    stall_server* s = (stall_server*)arg;
    dsd_socket_t client = dsd_socket_accept(s->listen_sock, NULL, NULL);
    if (client != DSD_INVALID_SOCKET) {
        atomic_store(&s->accepted, 1);
        while (atomic_load(&s->stop) == 0) {
            dsd_sleep_ms(10U);
        }
        (void)dsd_socket_close(client);
    }
    DSD_THREAD_RETURN;
}

static int
stall_server_start(stall_server* s, char* url, size_t url_cap) {
    DSD_MEMSET(s, 0, sizeof(*s));
    atomic_store(&s->stop, 0);
    atomic_store(&s->accepted, 0);
    if (dsd_socket_init() != 0) {
        return -1;
    }
    s->listen_sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (s->listen_sock == DSD_INVALID_SOCKET) {
        return -1;
    }
    (void)dsd_socket_set_recv_timeout(s->listen_sock, 10000U); /* bounds accept() if curl never connects */
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t addr_len = (socklen_t)sizeof(addr);
    if (dsd_socket_bind(s->listen_sock, (const struct sockaddr*)&addr, (int)sizeof(addr)) != 0
        || dsd_socket_listen(s->listen_sock, 1) != 0
        || getsockname(s->listen_sock, (struct sockaddr*)&addr, &addr_len) != 0) {
        (void)dsd_socket_close(s->listen_sock);
        return -1;
    }
    const int n = DSD_SNPRINTF(url, url_cap, "https://127.0.0.1:%u/7/64/42.png", (unsigned int)ntohs(addr.sin_port));
    if (n <= 0 || (size_t)n >= url_cap || dsd_thread_create(&s->thread, stall_server_thread, s) != 0) {
        (void)dsd_socket_close(s->listen_sock);
        return -1;
    }
    return 0;
}

static void
stall_server_stop(stall_server* s) {
    atomic_store(&s->stop, 1);
    (void)dsd_thread_join(s->thread);
    (void)dsd_socket_close(s->listen_sock);
    dsd_socket_cleanup();
}

typedef struct {
    dsd_tile_fetch_cancel* cancel;
    stall_server* server;
} canceller_arg;

/* Waits for the connection to be in the handshake, then cancels. */
static DSD_THREAD_RETURN_TYPE
canceller_thread(void* arg) {
    canceller_arg* c = (canceller_arg*)arg;
    for (int i = 0; i < 500 && atomic_load(&c->server->accepted) == 0; i++) {
        dsd_sleep_ms(10U);
    }
    dsd_sleep_ms(200U);
    dsd_tile_fetch_cancel_request(c->cancel);
    DSD_THREAD_RETURN;
}

/*
 * A real stalled TLS handshake, where libcurl calls no progress callback: the
 * cancel must still land promptly through the shared multi-handle poll. (The
 * NOPROGRESS / xferinfo wiring itself is pinned by RUNTIME_TILE_FETCH_CURL.)
 */
static void
test_loopback_stalled_transfer_cancelled(void) {
    if (!dsd_tile_fetch_available()) {
        DSD_FPRINTF(stderr, "SKIP: stalled-transfer cancel (the built-in transport is unavailable: no https or no "
                            "asynchronous resolver)\n");
        return;
    }
    stall_server server;
    char url[128];
    if (stall_server_start(&server, url, sizeof(url)) != 0) {
        expect("loopback server started", 0);
        return;
    }
    dsd_tile_fetch_cancel* cancel = dsd_tile_fetch_cancel_create();
    canceller_arg carg = {cancel, &server};
    dsd_thread_t canceller;
    const int canceller_ok = dsd_thread_create(&canceller, canceller_thread, &carg) == 0;
    expect("canceller started", canceller_ok);
    if (!canceller_ok) {
        /* Nothing would cancel the 30 s transfer, and there is no thread to join. */
        dsd_tile_fetch_cancel_destroy(cancel);
        stall_server_stop(&server);
        return;
    }
    dsd_tile_fetch_session* session = dsd_tile_fetch_session_create();

    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.url = url;
    req.connect_timeout_ms = 30000; /* Long, so a timeout cannot pass for a cancel. */
    req.total_timeout_ms = 30000;
    req.cancel = cancel;
    req.session = session; /* the reused-connection path cancels the same way */
    dsd_tile_fetch_response resp;
    const uint64_t started = dsd_realtime_mono_ms();
    const dsd_tile_fetch_status st = dsd_tile_fetch(&req, &resp);
    const uint64_t elapsed = dsd_realtime_mono_ms() - started;

    expect("curl reached the stalled server", atomic_load(&server.accepted) == 1);
    expect_ll("stalled transfer reports CANCELLED", (long long)st, DSD_TILE_FETCH_ERR_CANCELLED);
    expect("cancel lands within a few seconds", elapsed < 5000U);
    expect("cancel reason has no URL", strstr(resp.error, "127.0.0.1") == NULL);
    dsd_tile_fetch_response_free(&resp);

    (void)dsd_thread_join(canceller);
    dsd_tile_fetch_session_destroy(session);
    dsd_tile_fetch_cancel_destroy(cancel);
    stall_server_stop(&server);
}

#ifdef DSD_TILE_FETCH_TEST_FAKE_RESOLVER

#if LIBCURL_VERSION_NUM >= 0x081400
typedef struct {
    dsd_tile_fetch_cancel* cancel;
    atomic_int saw_held;
    atomic_int fetch_done;
    atomic_int released_early; /**< The safety release fired before the fetch returned. */
} lookup_stall_arg;

/* Cancels once the lookup is held in getaddrinfo(), or gives up after 2 s. */
static DSD_THREAD_RETURN_TYPE
lookup_canceller_thread(void* arg) {
    lookup_stall_arg* a = (lookup_stall_arg*)arg;
    for (int i = 0; i < 200 && dsd_fake_resolver_held() == 0; i++) {
        dsd_sleep_ms(10U);
    }
    atomic_store(&a->saw_held, dsd_fake_resolver_held() > 0 ? 1 : 0);
    dsd_sleep_ms(100U);
    dsd_tile_fetch_cancel_request(a->cancel);
    DSD_THREAD_RETURN;
}

/* Safety net: answers the held lookup after 3 s, so a cleanup that joins the
 * resolver thread (no quick exit) ends the test as a failure, not a hang. */
static DSD_THREAD_RETURN_TYPE
lookup_releaser_thread(void* arg) {
    lookup_stall_arg* a = (lookup_stall_arg*)arg;
    for (int i = 0; i < 300 && atomic_load(&a->fetch_done) == 0; i++) {
        dsd_sleep_ms(10U);
    }
    atomic_store(&a->released_early, atomic_load(&a->fetch_done) == 0 ? 1 : 0);
    dsd_fake_resolver_hold(0);
    DSD_THREAD_RETURN;
}

/*
 * A cancel while the name lookup hangs. With libcurl 8.20 or later the helper
 * sets CURLMOPT_QUICK_EXIT, so cleanup leaves the stuck resolver thread instead
 * of joining it and the fetch returns promptly; without it the fetch would wait
 * for getaddrinfo() (here, the 3 s safety release). This executable answers its
 * own getaddrinfo() (fake_getaddrinfo.c) and holds it; it skips where libcurl is
 * older than 8.20 or does not look names up through getaddrinfo().
 */
static void
test_lookup_stall_cancelled(void) {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!dsd_tile_fetch_available() || info == NULL || info->version_num < 0x081400U) {
        DSD_FPRINTF(stderr, "SKIP: lookup-stall cancel (needs libcurl 8.20+ with an asynchronous resolver)\n");
        return;
    }
    dsd_fake_resolver_reset(0x7F000001U, 0x7F000001U);
    dsd_fake_resolver_hold(1);

    lookup_stall_arg arg;
    DSD_MEMSET(&arg, 0, sizeof(arg));
    arg.cancel = dsd_tile_fetch_cancel_create();
    atomic_store(&arg.saw_held, 0);
    atomic_store(&arg.fetch_done, 0);
    atomic_store(&arg.released_early, 0);
    dsd_thread_t canceller;
    dsd_thread_t releaser;
    const int canceller_ok = dsd_thread_create(&canceller, lookup_canceller_thread, &arg) == 0;
    const int releaser_ok = canceller_ok && dsd_thread_create(&releaser, lookup_releaser_thread, &arg) == 0;
    expect("lookup-stall threads started", canceller_ok && releaser_ok);
    if (!canceller_ok || !releaser_ok) {
        /* A thread that did start still holds &arg and its cancel token: let it finish before both go. */
        dsd_fake_resolver_hold(0);
        atomic_store(&arg.fetch_done, 1);
        if (canceller_ok) {
            (void)dsd_thread_join(canceller);
        }
        dsd_tile_fetch_cancel_destroy(arg.cancel);
        return;
    }

    dsd_tile_fetch_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    char url[128];
    (void)DSD_SNPRINTF(url, sizeof(url), "https://%s/7/64/42.png", DSD_FAKE_RESOLVER_HOST);
    req.url = url;
    req.cancel = arg.cancel;
    dsd_tile_fetch_response resp;
    const uint64_t started = dsd_realtime_mono_ms();
    const dsd_tile_fetch_status st = dsd_tile_fetch(&req, &resp);
    const uint64_t elapsed = dsd_realtime_mono_ms() - started;
    atomic_store(&arg.fetch_done, 1);
    (void)dsd_thread_join(canceller);
    (void)dsd_thread_join(releaser);

    if (atomic_load(&arg.saw_held) == 0) {
        DSD_FPRINTF(stderr, "SKIP: lookup-stall cancel (libcurl did not resolve through getaddrinfo())\n");
    } else {
        expect_ll("cancelled lookup reports CANCELLED", (long long)st, DSD_TILE_FETCH_ERR_CANCELLED);
        expect("cancel during a held lookup returns within ~1 s", elapsed < 1500U);
        expect("the fetch did not wait for the lookup to be answered", atomic_load(&arg.released_early) == 0);
    }
    dsd_tile_fetch_response_free(&resp);
    dsd_tile_fetch_cancel_destroy(arg.cancel);
    /* Let the abandoned resolver thread see the release and end before exit. */
    for (int i = 0; i < 200 && dsd_fake_resolver_held() > 0; i++) {
        dsd_sleep_ms(10U);
    }
    dsd_sleep_ms(50U);
}
#else
/* dsd_curl_multi_quick_exit() was compiled without CURLMOPT_QUICK_EXIT (the same compile-time gate as production), so
 * a newer loaded library changes nothing: cleanup still joins the held lookup, and the case cannot apply. */
static void
test_lookup_stall_cancelled(void) {
    DSD_FPRINTF(stderr, "SKIP: lookup-stall cancel (built with libcurl headers older than 8.20)\n");
}
#endif

#endif /* DSD_TILE_FETCH_TEST_FAKE_RESOLVER */

#endif /* USE_CURL && !DSD_PLATFORM_WIN_NATIVE */

static void
run_loopback_cases(void) {
#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
    test_loopback_stalled_transfer_cancelled();
#ifdef DSD_TILE_FETCH_TEST_FAKE_RESOLVER
    test_lookup_stall_cancelled();
#endif
#endif
}

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
typedef struct {
    atomic_int* go;
    int result;
} first_use_arg;

static DSD_THREAD_RETURN_TYPE
first_use_thread(void* arg) {
    first_use_arg* a = (first_use_arg*)arg;
    while (atomic_load(a->go) == 0) {}
    a->result = dsd_tile_fetch_available();
    DSD_THREAD_RETURN;
}

/* The first libcurl use in the process, from several threads at once (the map's probe beside an upload): the probe
 * goes through the shared global-init guard, so every thread gets the same answer (run under tsan-debug too). */
static void
test_first_use_concurrent(void) {
    enum { kThreads = 4 };

    atomic_int go;
    atomic_store(&go, 0);
    first_use_arg args[kThreads];
    dsd_thread_t threads[kThreads];
    int started = 0;
    for (int i = 0; i < kThreads; i++) {
        args[i].go = &go;
        args[i].result = -1;
        if (dsd_thread_create(&threads[i], first_use_thread, &args[i]) != 0) {
            break;
        }
        started++;
    }
    atomic_store(&go, 1);
    for (int i = 0; i < started; i++) {
        (void)dsd_thread_join(threads[i]);
    }
    expect("first-use threads started", started == kThreads);
    for (int i = 1; i < started; i++) {
        expect("every first-use probe agrees", args[i].result == args[0].result);
    }
}
#endif

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
typedef struct {
    int refused; /* fetches that ended as a network failure, as a refused connect should */
} session_thread_arg;

/* Two fetches through one session, each refused at once (nothing listens on port 1). */
static DSD_THREAD_RETURN_TYPE
session_thread(void* arg) {
    session_thread_arg* a = (session_thread_arg*)arg;
    dsd_tile_fetch_session* session = dsd_tile_fetch_session_create();
    for (int i = 0; i < 2; i++) {
        dsd_tile_fetch_request req;
        DSD_MEMSET(&req, 0, sizeof(req));
        req.url = "https://127.0.0.1:1/7/64/42.png";
        req.session = session;
        req.connect_timeout_ms = 2000;
        dsd_tile_fetch_response resp;
        a->refused += dsd_tile_fetch(&req, &resp) == DSD_TILE_FETCH_ERR_NETWORK ? 1 : 0;
        dsd_tile_fetch_response_free(&resp);
    }
    dsd_tile_fetch_session_destroy(session);
    DSD_THREAD_RETURN;
}

/* Sessions are per thread: two workers fetching at once, each with its own, share nothing (run under tsan-debug). */
static void
test_sessions_on_two_threads(void) {
    if (!dsd_tile_fetch_available()) {
        DSD_FPRINTF(stderr, "SKIP: sessions on two threads (the built-in transport is unavailable)\n");
        return;
    }
    session_thread_arg args[2] = {{0}, {0}};
    dsd_thread_t threads[2];
    int started = 0;
    for (int i = 0; i < 2; i++) {
        if (dsd_thread_create(&threads[i], session_thread, &args[i]) != 0) {
            break;
        }
        started++;
    }
    for (int i = 0; i < started; i++) {
        (void)dsd_thread_join(threads[i]);
    }
    expect("session threads started", started == 2);
    expect("every fetch in each session ended as refused", args[0].refused == 2 && args[1].refused == 2);
}
#endif

int
main(void) {
#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
    /* The loopback and fake-resolver cases talk to this process: a proxy the runner exports (HTTPS_PROXY) must not
     * take them elsewhere. Set before any thread starts and before libcurl reads the environment. */
    (void)dsd_test_setenv("no_proxy", "*", 1);
    (void)dsd_test_setenv("NO_PROXY", "*", 1);
    test_first_use_concurrent(); /* first: no libcurl use may come before it */
#endif
    test_user_agent();
    test_cancel_token();
    test_validation();
    test_conditional_values_pass_through();
    test_cancel_before_send();
    test_ok_with_headers();
    test_status_classes();
    test_caps();
    test_header_parsing();
    test_bad_list_headers();
    test_transport_failures();
    test_status_names();
    test_availability();
    run_loopback_cases();
#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
    test_sessions_on_two_threads();
#endif

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
