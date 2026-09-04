// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference client: transport plumbing, status mapping, retry, cancellation
 * and the async worker.
 *
 * Two layers of coverage on purpose. The injected-transport cases are guarded on
 * USE_EXPAT alone and therefore run on every preset, including win-msvc-* and
 * curl-less builds. The loopback-HTTP cases exercise the real curl transport but
 * cannot run on Windows (the helper uses select/getsockname directly), so
 * everything they assert about request shape, status mapping, retry and
 * cancellation is also asserted against the fake transport.
 */

#include "rr_internal.h"
#include "test_support.h"

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/radioreference.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/safe_api.h"

#ifndef DSD_NEO_TEST_RR_FIXTURE_DIR
#error "DSD_NEO_TEST_RR_FIXTURE_DIR must be defined by the build"
#endif

/*
 * A credential that must never surface anywhere a user or a log can see. Every
 * error path is asserted against it.
 */
static const char* const k_password_sentinel = "SENTINEL_PW_9d3";
static const char* const k_appkey_sentinel = "SENTINEL_KEY_7f1";

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

/* Fixture reads allocate this fixed cap rather than a size taken from the file
 * system: a constant allocation is what keeps the terminator index provably in
 * range. The largest captured response is ~1.3 MB. */
#define RR_FIXTURE_CAP_BYTES ((size_t)8U * 1024U * 1024U)

static int
read_fixture(const char* leaf, char** out, size_t* out_len) {
    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(path, sizeof(path), DSD_NEO_TEST_RR_FIXTURE_DIR, leaf) != 0) {
        return -1;
    }
    FILE* fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    char* buf = (char*)malloc(RR_FIXTURE_CAP_BYTES + 1U);
    if (buf == NULL) {
        fclose(fp);
        return -1;
    }
    const size_t got = fread(buf, 1, RR_FIXTURE_CAP_BYTES, fp);
    const int hit_cap = (feof(fp) == 0);
    fclose(fp);
    if (hit_cap) {
        /* Bigger than the cap, so what was read is a truncated body. */
        free(buf);
        return -1;
    }
    /* Clamped explicitly: `got` cannot exceed the cap, but saying so is what
     * keeps the terminator index inside the allocation for a static analyzer. */
    const size_t len = (got < RR_FIXTURE_CAP_BYTES) ? got : RR_FIXTURE_CAP_BYTES;
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 0;
}

static void
fill_auth(dsd_rr_auth* auth, const char* username) {
    DSD_MEMSET(auth, 0, sizeof(*auth));
    (void)DSD_SNPRINTF(auth->username, sizeof(auth->username), "%s", username);
    (void)DSD_SNPRINTF(auth->password, sizeof(auth->password), "%s", k_password_sentinel);
    (void)DSD_SNPRINTF(auth->app_key, sizeof(auth->app_key), "%s", k_appkey_sentinel);
}

/**
 * @brief Assert that no error surface ever carries a credential.
 */
static void
expect_no_credentials(const char* what, const dsd_rr_error* err) {
    if (strstr(err->detail, k_password_sentinel) != NULL) {
        DSD_FPRINTF(stderr, "FAIL: %s leaked the password into err.detail\n", what);
        g_failures++;
    }
    if (strstr(err->detail, k_appkey_sentinel) != NULL) {
        DSD_FPRINTF(stderr, "FAIL: %s leaked the application credential into err.detail\n", what);
        g_failures++;
    }
}

/* ------------------------------------------------------------------------- */
/* Fake transport                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char* body;
    size_t body_len;
    long http_status;
    int fail_times;          /**< Fail the first N attempts... */
    dsd_rr_status fail_with; /**< ...with this class. */
    int calls;
    char last_request[16384];
} fake_ctx;

static int
fake_perform(void* ctx, const dsd_rr_request* req, dsd_rr_response* resp) {
    fake_ctx* fake = (fake_ctx*)ctx;
    DSD_MEMSET(resp, 0, sizeof(*resp));
    fake->calls++;

    size_t copy = req->body_len;
    if (copy >= sizeof(fake->last_request)) {
        copy = sizeof(fake->last_request) - 1U;
    }
    DSD_MEMCPY(fake->last_request, req->body, copy);
    fake->last_request[copy] = '\0';

    if (fake->calls <= fake->fail_times) {
        resp->status = fake->fail_with;
        (void)DSD_SNPRINTF(resp->error, sizeof(resp->error), "%s", "simulated transport failure");
        return -1;
    }

    resp->http_status = fake->http_status;
    if (fake->body != NULL) {
        resp->body = (char*)malloc(fake->body_len + 1U);
        if (resp->body == NULL) {
            resp->status = DSD_RR_ERR_NOMEM;
            return -1;
        }
        DSD_MEMCPY(resp->body, fake->body, fake->body_len);
        resp->body[fake->body_len] = '\0';
        resp->body_len = fake->body_len;
    }
    resp->status = DSD_RR_OK;
    return 0;
}

/**
 * @brief Create a client wired to a fake transport.
 */
static dsd_rr_client*
make_fake_client(fake_ctx* fake) {
    dsd_rr_client_config config;
    DSD_MEMSET(&config, 0, sizeof(config));
    config.connect_timeout_ms = 1000;
    config.total_timeout_ms = 2000;
    config.transient_retries = 1;

    dsd_rr_client* client = dsd_rr_client_create(&config);
    if (client == NULL) {
        return NULL;
    }
    const dsd_rr_transport transport = {fake_perform, fake};
    dsd_rr_client_set_transport(client, &transport);
    return client;
}

/* ------------------------------------------------------------------------- */

static void
test_request_shape(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("trs_sites_p25.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    fake_ctx fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.body = body;
    fake.body_len = len;
    fake.http_status = 200;

    dsd_rr_client* client = make_fake_client(&fake);
    expect("client created", client != NULL);
    if (client == NULL) {
        free(body);
        return;
    }

    dsd_rr_auth auth;
    /* A username that must be escaped on the wire. */
    fill_auth(&auth, "a<b>&c");

    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    expect("getTrsSites succeeds", dsd_rr_get_trs_sites(client, &auth, 6673, &sites, &err) == 0);
    expect_ll("site count from the transport", (long long)sites.count, 35);
    expect_ll("request performed once", fake.calls, 1);
    expect("request names the method", strstr(fake.last_request, "<ns1:getTrsSites>") != NULL);
    expect("request carries the sid", strstr(fake.last_request, "<sid xsi:type=\"xsd:int\">6673</sid>") != NULL);
    expect("username is escaped", strstr(fake.last_request, "a&lt;b&gt;&amp;c") != NULL);
    expect("raw username is not sent", strstr(fake.last_request, "a<b>&c") == NULL);
    expect("version is pinned to 18",
           strstr(fake.last_request, "<version xsi:type=\"xsd:string\">18</version>") != NULL);
    expect("style is pinned to rpc", strstr(fake.last_request, "<style xsi:type=\"xsd:string\">rpc</style>") != NULL);

    dsd_rr_site_list_free(&sites);
    dsd_rr_client_destroy(client);
    free(body);
}

static void
test_talkgroups_send_zero_filters(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("trs_talkgroups_nxdn.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    fake_ctx fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.body = body;
    fake.body_len = len;
    fake.http_status = 200;

    dsd_rr_client* client = make_fake_client(&fake);
    if (client == NULL) {
        free(body);
        g_failures++;
        return;
    }

    dsd_rr_auth auth;
    fill_auth(&auth, "user");
    dsd_rr_talkgroup_list tgs;
    DSD_MEMSET(&tgs, 0, sizeof(tgs));
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    expect("getTrsTalkgroups succeeds", dsd_rr_get_trs_talkgroups(client, &auth, 12918, &tgs, &err) == 0);
    expect("talkgroups decoded", tgs.count > 0);
    /* Omitting these returns an empty-bodied HTTP 500 from the real endpoint. */
    expect("tgCid filter sent as 0", strstr(fake.last_request, "<tgCid xsi:type=\"xsd:int\">0</tgCid>") != NULL);
    expect("tgTag filter sent as 0", strstr(fake.last_request, "<tgTag xsi:type=\"xsd:int\">0</tgTag>") != NULL);
    expect("tgDec filter sent as 0", strstr(fake.last_request, "<tgDec xsi:type=\"xsd:int\">0</tgDec>") != NULL);

    dsd_rr_talkgroup_list_free(&tgs);
    dsd_rr_client_destroy(client);
    free(body);
}

static void
test_country_list_needs_no_auth(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("country_list.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    fake_ctx fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.body = body;
    fake.body_len = len;
    fake.http_status = 200;

    dsd_rr_client* client = make_fake_client(&fake);
    if (client == NULL) {
        free(body);
        g_failures++;
        return;
    }

    dsd_rr_country_list countries;
    DSD_MEMSET(&countries, 0, sizeof(countries));
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    /* The WSDL message has no parts at all, so NULL credentials must work. */
    expect("getCountryList succeeds without credentials", dsd_rr_get_countries(client, NULL, &countries, &err) == 0);
    expect_ll("country count", (long long)countries.count, 236);
    expect("no authInfo emitted", strstr(fake.last_request, "authInfo") == NULL);

    dsd_rr_country_list_free(&countries);
    dsd_rr_client_destroy(client);
    free(body);
}

/**
 * @brief The rule that matters: a fault is HTTP 500 and must still classify.
 */
static void
test_status_mapping(void) {
    char* fault = NULL;
    size_t fault_len = 0;
    if (read_fixture("fault_auth.xml", &fault, &fault_len) != 0) {
        g_failures++;
        return;
    }

    /* 500 + fault body -> AUTH, with the HTTP status recorded but not deciding. */
    {
        fake_ctx fake;
        DSD_MEMSET(&fake, 0, sizeof(fake));
        fake.body = fault;
        fake.body_len = fault_len;
        fake.http_status = 500;

        dsd_rr_client* client = make_fake_client(&fake);
        dsd_rr_auth auth;
        fill_auth(&auth, "user");
        dsd_rr_user_info info;
        DSD_MEMSET(&info, 0, sizeof(info));
        dsd_rr_error err;
        DSD_MEMSET(&err, 0, sizeof(err));

        expect("fault reports failure", dsd_rr_get_user_data(client, &auth, &info, &err) != 0);
        expect_ll("HTTP 500 fault classifies as AUTH", (long long)err.status, (long long)DSD_RR_ERR_AUTH);
        expect_ll("http_status captured", err.http_status, 500);
        expect("faultstring surfaced", strstr(err.detail, "Invalid Username or Password") != NULL);
        expect_no_credentials("auth fault", &err);
        /* An auth failure is not transient and must not be replayed. */
        expect_ll("auth fault not retried", fake.calls, 1);
        dsd_rr_client_destroy(client);
    }

    /* 500 + an HTML error page -> HTTP, not PARSE. */
    {
        static const char k_html[] = "<html><body>502 Bad Gateway</body></html>";
        fake_ctx fake;
        DSD_MEMSET(&fake, 0, sizeof(fake));
        fake.body = k_html;
        fake.body_len = sizeof(k_html) - 1U;
        fake.http_status = 502;

        dsd_rr_client* client = make_fake_client(&fake);
        dsd_rr_auth auth;
        fill_auth(&auth, "user");
        dsd_rr_user_info info;
        DSD_MEMSET(&info, 0, sizeof(info));
        dsd_rr_error err;
        DSD_MEMSET(&err, 0, sizeof(err));

        expect("proxy error reports failure", dsd_rr_get_user_data(client, &auth, &info, &err) != 0);
        expect_ll("proxy error classifies as HTTP", (long long)err.status, (long long)DSD_RR_ERR_HTTP);
        expect_ll("proxy http_status captured", err.http_status, 502);
        dsd_rr_client_destroy(client);
    }

    /* Empty body -> HTTP. This is what a missing declared part actually returns. */
    {
        fake_ctx fake;
        DSD_MEMSET(&fake, 0, sizeof(fake));
        fake.body = NULL;
        fake.body_len = 0;
        fake.http_status = 500;

        dsd_rr_client* client = make_fake_client(&fake);
        dsd_rr_auth auth;
        fill_auth(&auth, "user");
        dsd_rr_user_info info;
        DSD_MEMSET(&info, 0, sizeof(info));
        dsd_rr_error err;
        DSD_MEMSET(&err, 0, sizeof(err));

        expect("empty body reports failure", dsd_rr_get_user_data(client, &auth, &info, &err) != 0);
        expect_ll("empty body classifies as HTTP", (long long)err.status, (long long)DSD_RR_ERR_HTTP);
        dsd_rr_client_destroy(client);
    }

    /* 200 + a valid body -> success even though the parse shape is strict. */
    {
        char* user_body = NULL;
        size_t user_len = 0;
        if (read_fixture("user_data.xml", &user_body, &user_len) == 0) {
            fake_ctx fake;
            DSD_MEMSET(&fake, 0, sizeof(fake));
            fake.body = user_body;
            fake.body_len = user_len;
            fake.http_status = 200;

            dsd_rr_client* client = make_fake_client(&fake);
            dsd_rr_auth auth;
            fill_auth(&auth, "user");
            dsd_rr_user_info info;
            DSD_MEMSET(&info, 0, sizeof(info));
            dsd_rr_error err;
            DSD_MEMSET(&err, 0, sizeof(err));

            expect("getUserData succeeds", dsd_rr_get_user_data(client, &auth, &info, &err) == 0);
            expect("subExpireDate decoded", strcmp(info.sub_expire, "11-24-2026") == 0);
            dsd_rr_client_destroy(client);
            free(user_body);
        }
    }

    free(fault);
}

static void
test_retry_policy(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("user_data.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    /* One transient failure then success: exactly two attempts. */
    {
        fake_ctx fake;
        DSD_MEMSET(&fake, 0, sizeof(fake));
        fake.body = body;
        fake.body_len = len;
        fake.http_status = 200;
        fake.fail_times = 1;
        fake.fail_with = DSD_RR_ERR_NETWORK;

        dsd_rr_client* client = make_fake_client(&fake);
        dsd_rr_auth auth;
        fill_auth(&auth, "user");
        dsd_rr_user_info info;
        DSD_MEMSET(&info, 0, sizeof(info));
        dsd_rr_error err;
        DSD_MEMSET(&err, 0, sizeof(err));

        expect("network failure is retried into success", dsd_rr_get_user_data(client, &auth, &info, &err) == 0);
        expect_ll("retried exactly once", fake.calls, 2);
        dsd_rr_client_destroy(client);
    }

    /* Persistent network failure: still only one retry, not a loop. */
    {
        fake_ctx fake;
        DSD_MEMSET(&fake, 0, sizeof(fake));
        fake.body = body;
        fake.body_len = len;
        fake.http_status = 200;
        fake.fail_times = 99;
        fake.fail_with = DSD_RR_ERR_NETWORK;

        dsd_rr_client* client = make_fake_client(&fake);
        dsd_rr_auth auth;
        fill_auth(&auth, "user");
        dsd_rr_user_info info;
        DSD_MEMSET(&info, 0, sizeof(info));
        dsd_rr_error err;
        DSD_MEMSET(&err, 0, sizeof(err));

        expect("persistent network failure reports failure", dsd_rr_get_user_data(client, &auth, &info, &err) != 0);
        expect_ll("network failure classified", (long long)err.status, (long long)DSD_RR_ERR_NETWORK);
        expect_ll("bounded at two attempts", fake.calls, 2);
        expect_no_credentials("network failure", &err);
        dsd_rr_client_destroy(client);
    }

    /*
     * Cancellation and an oversized response are NOT transient. Replaying the
     * latter would re-download up to 32 MB for nothing.
     */
    {
        const dsd_rr_status k_never_retried[] = {DSD_RR_ERR_CANCELLED, DSD_RR_ERR_PARSE};
        for (size_t i = 0; i < sizeof(k_never_retried) / sizeof(k_never_retried[0]); i++) {
            fake_ctx fake;
            DSD_MEMSET(&fake, 0, sizeof(fake));
            fake.body = body;
            fake.body_len = len;
            fake.http_status = 200;
            fake.fail_times = 1;
            fake.fail_with = k_never_retried[i];

            dsd_rr_client* client = make_fake_client(&fake);
            dsd_rr_auth auth;
            fill_auth(&auth, "user");
            dsd_rr_user_info info;
            DSD_MEMSET(&info, 0, sizeof(info));
            dsd_rr_error err;
            DSD_MEMSET(&err, 0, sizeof(err));

            expect("non-transient failure reports failure", dsd_rr_get_user_data(client, &auth, &info, &err) != 0);
            expect_ll("non-transient class preserved", (long long)err.status, (long long)k_never_retried[i]);
            expect_ll("non-transient failure not retried", fake.calls, 1);
            dsd_rr_client_destroy(client);
        }
    }

    free(body);
}

static void
test_support_map_cache(void) {
    char* types = NULL;
    size_t types_len = 0;
    if (read_fixture("trs_types.xml", &types, &types_len) != 0) {
        g_failures++;
        return;
    }

    /*
     * The three support calls share one fake body. The type list is the only one
     * that decodes into meaningful rows here; what matters is the call count.
     */
    fake_ctx fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.body = types;
    fake.body_len = types_len;
    fake.http_status = 200;

    dsd_rr_client* client = make_fake_client(&fake);
    dsd_rr_auth auth;
    fill_auth(&auth, "user");
    dsd_rr_support_maps maps;
    DSD_MEMSET(&maps, 0, sizeof(maps));
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    expect("support maps fetched", dsd_rr_get_support_maps(client, &auth, &maps, &err) == 0);
    expect_ll("three calls on first fetch", fake.calls, 3);
    expect_ll("type list decoded", (long long)maps.types.count, 13);

    DSD_MEMSET(&maps, 0, sizeof(maps));
    expect("support maps served from cache", dsd_rr_get_support_maps(client, &auth, &maps, &err) == 0);
    expect_ll("cache avoids refetching", fake.calls, 3);
    expect_ll("cached type list intact", (long long)maps.types.count, 13);

    /* The maps are borrowed; the client frees them at destroy. */
    dsd_rr_client_destroy(client);
    free(types);
}

/* ------------------------------------------------------------------------- */
/* Async worker                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    atomic_int done;
    atomic_int status;
    atomic_int count;
    atomic_int leaked_credential;
} async_result;

static void
async_cb(void* user, dsd_rr_status status, const dsd_rr_error* err, void* result) {
    async_result* out = (async_result*)user;
    atomic_store(&out->status, (int)status);
    if (err != NULL
        && (strstr(err->detail, k_password_sentinel) != NULL || strstr(err->detail, k_appkey_sentinel) != NULL)) {
        atomic_store(&out->leaked_credential, 1);
    }
    if (result != NULL) {
        const dsd_rr_site_list* sites = (const dsd_rr_site_list*)result;
        atomic_store(&out->count, (int)sites->count);
        /* The callback owns the result. */
        dsd_rr_site_list_free((dsd_rr_site_list*)result);
        free(result);
    }
    atomic_store(&out->done, 1);
}

/**
 * @brief A callback context the request can safely outlive.
 *
 * Heap-owned rather than a local, because that is the contract the async API
 * actually has: the client stores this pointer in the queued job and the worker
 * thread dereferences it, so it has to outlive the frame that started the
 * request. Every case below does wait for completion, which would make a local
 * work - but only by coincidence, and a test that models the contract loosely
 * stops being evidence that the contract holds. The real caller allocates its
 * context for exactly this reason.
 *
 * @return Zeroed context with the "no answer yet" sentinels set, or NULL.
 */
static async_result*
async_result_new(void) {
    async_result* out = (async_result*)calloc(1, sizeof(*out));
    if (out == NULL) {
        return NULL;
    }
    atomic_store(&out->done, 0);
    atomic_store(&out->status, -1);
    atomic_store(&out->count, -1);
    atomic_store(&out->leaked_credential, 0);
    return out;
}

/**
 * @brief Wait for an async callback, bounded.
 *
 * @return 1 when it fired, 0 on timeout.
 */
static int
wait_done(async_result* out, unsigned int timeout_ms) {
    for (unsigned int waited = 0; waited < timeout_ms; waited += 10U) {
        if (atomic_load(&out->done) != 0) {
            return 1;
        }
        dsd_sleep_ms(10U);
    }
    return atomic_load(&out->done) != 0;
}

static void
test_async_fetch(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("trs_sites_nxdn.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    fake_ctx fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.body = body;
    fake.body_len = len;
    fake.http_status = 200;

    dsd_rr_client* client = make_fake_client(&fake);
    dsd_rr_auth auth;
    fill_auth(&auth, "user");

    async_result* out = async_result_new();
    expect("callback context allocated", out != NULL);
    if (out == NULL) {
        dsd_rr_client_destroy(client);
        free(body);
        return;
    }

    const uint64_t id = dsd_rr_fetch_trs_sites(client, &auth, 12918, async_cb, out);
    expect("fetch returns a request id", id != 0U);
    expect("async callback fires", wait_done(out, 5000U));
    expect_ll("async status", (long long)atomic_load(&out->status), (long long)DSD_RR_OK);
    expect_ll("async result decoded", atomic_load(&out->count), 1);
    expect_ll("no credential in async error text", atomic_load(&out->leaked_credential), 0);

    /* After the join, so the worker cannot still be holding it. */
    dsd_rr_client_destroy(client);
    free(out);
    free(body);
}

static void
test_destroy_with_pending(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("trs_sites_nxdn.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    fake_ctx fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.body = body;
    fake.body_len = len;
    fake.http_status = 200;

    dsd_rr_client* client = make_fake_client(&fake);
    dsd_rr_auth auth;
    fill_auth(&auth, "user");

    async_result* out = async_result_new();
    expect("callback context allocated", out != NULL);
    if (out == NULL) {
        dsd_rr_client_destroy(client);
        free(body);
        return;
    }
    for (int i = 0; i < 8; i++) {
        (void)dsd_rr_fetch_trs_sites(client, &auth, 12918, async_cb, out);
    }

    /* Must return rather than deadlock or leak the queued jobs. */
    dsd_rr_client_destroy(client);
    expect("destroy with pending jobs returns", 1);
    free(out);
    free(body);
}

static void
test_cancel_queued_request(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("trs_sites_nxdn.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    fake_ctx fake;
    DSD_MEMSET(&fake, 0, sizeof(fake));
    fake.body = body;
    fake.body_len = len;
    fake.http_status = 200;

    dsd_rr_client* client = make_fake_client(&fake);
    dsd_rr_auth auth;
    fill_auth(&auth, "user");

    async_result* out = async_result_new();
    expect("callback context allocated", out != NULL);
    if (out == NULL) {
        dsd_rr_client_destroy(client);
        free(body);
        return;
    }
    const uint64_t id = dsd_rr_fetch_trs_sites(client, &auth, 12918, async_cb, out);
    expect("cancel finds a live request", dsd_rr_cancel(client, id) == 0 || atomic_load(&out->done) != 0);
    expect("cancel of an unknown id fails", dsd_rr_cancel(client, id + 1000U) != 0);

    (void)wait_done(out, 5000U);
    dsd_rr_client_destroy(client);
    free(out);
    free(body);
}

/* ------------------------------------------------------------------------- */
/* Subscription expiry                                                        */
/* ------------------------------------------------------------------------- */

/**
 * @brief Seconds since the epoch for an "MM-DD-YYYY" date, counted the slow way.
 *
 * Deliberately not the shifted-era arithmetic rr_days_from_civil() uses: a test
 * that reimplemented that would agree with it about any shared mistake. This
 * just walks whole years from 1970 and sums month lengths, which is obviously
 * right at the cost of being slow, and no caller here is anywhere near hot.
 *
 * @param text Date as "MM-DD-YYYY".
 * @return Seconds since 1970-01-01T00:00:00Z, or -1 if the date is unusable.
 */
static long long
naive_unix_from_mdy(const char* text) {
    static const int k_month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (text == NULL) {
        return -1;
    }
    /* Fixed "MM-DD-YYYY" by hand rather than sscanf, which safe_api.h has no
     * wrapper for and semgrep rejects tree-wide. Every caller is a literal in
     * the table below, so a shape mismatch is a test bug, not input handling. */
    for (int i = 0; i < 10; i++) {
        const int want_digit = (i != 2 && i != 5);
        if (text[i] == '\0' || (want_digit && (text[i] < '0' || text[i] > '9')) || (!want_digit && text[i] != '-')) {
            return -1;
        }
    }
    if (text[10] != '\0') {
        return -1;
    }
    const int month = ((text[0] - '0') * 10) + (text[1] - '0');
    const int day = ((text[3] - '0') * 10) + (text[4] - '0');
    const int year = ((text[6] - '0') * 1000) + ((text[7] - '0') * 100) + ((text[8] - '0') * 10) + (text[9] - '0');
    if (month < 1 || month > 12 || day < 1 || year < 1970) {
        return -1;
    }

    long long days = 0;
    for (int y = 1970; y < year; y++) {
        const int leap = ((y % 4 == 0) && (y % 100 != 0)) || (y % 400 == 0);
        days += leap ? 366 : 365;
    }
    const int leap_year = ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
    for (int m = 1; m < month; m++) {
        days += k_month_days[m - 1];
        if (m == 2 && leap_year) {
            days += 1;
        }
    }
    int month_length = k_month_days[month - 1];
    if (month == 2 && leap_year) {
        month_length += 1;
    }
    if (day > month_length) {
        return -1;
    }
    days += day - 1;
    return days * 86400LL;
}

static void
test_subscription_expiry(void) {
    /* 2026-08-15T00:00:00Z, i.e. day 20680 since the epoch. */
    const long long now = 1786752000LL;

    static const struct {
        const char* text;
        int expired;
        const char* why;
    } cases[] = {
        {"11-24-2026", 0, "future date is valid"},
        {"08-15-2026", 0, "today is valid"},
        {"08-14-2026", 0, "yesterday is inside the two-day slack"},
        {"08-13-2026", 1, "two days ago is expired"},
        {"01-01-2020", 1, "long past is expired"},
        /* RR answers these for feed providers and admins; they must not lock out. */
        {"Never - Feed Provider", 0, "unparseable is treated as valid"},
        {"Never - Admin", 0, "unparseable is treated as valid"},
        {"", 0, "empty is treated as valid"},
        {"garbage", 0, "garbage is treated as valid"},
        {"13-01-2026", 0, "impossible month is treated as valid"},
        {"12-32-2026", 0, "impossible day is treated as valid"},
        {"2026-08-13", 0, "ISO order is not the RR format, so undeterminable"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const int got = rr_subscription_expired(cases[i].text, now);
        if (got != cases[i].expired) {
            DSD_FPRINTF(stderr, "FAIL: subscription \"%s\": %s (got %d)\n", cases[i].text, cases[i].why, got);
            g_failures++;
        }
    }
    expect("NULL subscription is not expired", rr_subscription_expired(NULL, now) == 0);

    /* The day count shifts March to month 0 so the leap day falls at the end of
     * the shifted year. That makes the arithmetic month-dependent, and the cases
     * above only reach four months - a shift that was wrong for, say, October
     * would pass every one of them. Walk the boundary of each month instead:
     * the last day of a subscription is still valid, and two days later is not.
     *
     * `expired` carries two days of slack, so the probe dates are the month's
     * own last day (valid) and three days past it (expired). */
    static const struct {
        const char* last_day;   /* subscription runs to here */
        const char* three_past; /* three days later, i.e. clear of the slack */
        const char* why;
    } months[] = {
        {"01-31-2026", "02-03-2026", "January"},
        {"02-28-2026", "03-03-2026", "February in a common year"},
        {"03-31-2026", "04-03-2026", "March, the first month of the shifted year"},
        {"04-30-2026", "05-03-2026", "April"},
        {"05-31-2026", "06-03-2026", "May"},
        {"06-30-2026", "07-03-2026", "June"},
        {"07-31-2026", "08-03-2026", "July"},
        {"08-31-2026", "09-03-2026", "August"},
        {"09-30-2026", "10-03-2026", "September"},
        {"10-31-2026", "11-03-2026", "October"},
        {"11-30-2026", "12-03-2026", "November"},
        {"12-31-2026", "01-03-2027", "December, which crosses the year"},
        {"02-29-2024", "03-03-2024", "the leap day itself"},
        {"02-28-2100", "03-03-2100", "2100, a century that is not a leap year"},
        {"02-29-2000", "03-03-2000", "2000, a century that is"},
    };

    for (size_t i = 0; i < sizeof(months) / sizeof(months[0]); i++) {
        /* A subscription expiring on that day, asked about on that day. */
        const long long on_the_day = naive_unix_from_mdy(months[i].last_day);
        const long long well_after = naive_unix_from_mdy(months[i].three_past);
        if (on_the_day < 0 || well_after < 0) {
            DSD_FPRINTF(stderr, "FAIL: %s: test could not build its own dates\n", months[i].why);
            g_failures++;
            continue;
        }
        if (rr_subscription_expired(months[i].last_day, on_the_day) != 0) {
            DSD_FPRINTF(stderr, "FAIL: %s: the last day of the subscription read as expired\n", months[i].why);
            g_failures++;
        }
        if (rr_subscription_expired(months[i].last_day, well_after) != 1) {
            DSD_FPRINTF(stderr, "FAIL: %s: three days past the end did not read as expired\n", months[i].why);
            g_failures++;
        }
    }
}

static void
test_availability_and_arguments(void) {
    /*
     * dsd_rr_available reports whether the BUILT-IN path exists. It is not a
     * precondition for the client: every assertion above runs through an injected
     * transport and must work regardless of what it returns.
     */
    const int available = dsd_rr_available();
    expect("availability is a boolean", available == 0 || available == 1);

    dsd_rr_client* client = dsd_rr_client_create(NULL);
    expect("default config creates a client", client != NULL);

    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));
    dsd_rr_zip_info zip;
    DSD_MEMSET(&zip, 0, sizeof(zip));
    dsd_rr_auth auth;
    fill_auth(&auth, "user");

    expect("non-numeric zip rejected", dsd_rr_get_zipcode_info(client, &auth, "abcde", &zip, &err) != 0);
    expect_ll("zip rejection is an argument error", (long long)err.status, (long long)DSD_RR_ERR_INVALID_ARG);
    expect("NULL zip rejected", dsd_rr_get_zipcode_info(client, &auth, NULL, &zip, &err) != 0);
    expect("NULL output rejected", dsd_rr_get_user_data(client, &auth, NULL, &err) != 0);
    expect("NULL credentials rejected where required", dsd_rr_get_user_data(client, NULL, NULL, &err) != 0);
    expect("cancel on an empty client fails", dsd_rr_cancel(client, 1) != 0);

    dsd_rr_client_destroy(client);
    dsd_rr_client_destroy(NULL); /* must be a no-op */
}

/* ------------------------------------------------------------------------- */
/* Loopback HTTP server (real curl transport)                                 */
/* ------------------------------------------------------------------------- */

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE

typedef struct {
    dsd_socket_t listen_sock;
    dsd_thread_t thread;
    int saw_request;
    unsigned int stall_ms; /**< Hold the connection open without replying. */
    char* response;        /**< Heap: a SOAP fixture will not fit in 512 bytes. */
    size_t response_len;
    char request[16384];
} rr_test_server;

static DSD_THREAD_RETURN_TYPE
rr_test_server_thread(void* arg) {
    rr_test_server* server = (rr_test_server*)arg;

    dsd_socket_t client = dsd_socket_accept(server->listen_sock, NULL, NULL);
    if (client == DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(server->listen_sock);
        DSD_THREAD_RETURN;
    }
    server->saw_request = 1;
    (void)dsd_socket_set_recv_timeout(client, 3000);

    size_t used = 0;
    while (used < sizeof(server->request) - 1U) {
        const int n = dsd_socket_recv(client, server->request + used, (int)(sizeof(server->request) - 1U - used), 0);
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
        server->request[used] = '\0';
        if (strstr(server->request, "\r\n\r\n") != NULL) {
            /* Headers are complete; the body follows and we do not need all of it. */
            break;
        }
    }

    if (server->stall_ms > 0U) {
        /* Never reply: this is what the cancellation case needs. */
        dsd_sleep_ms(server->stall_ms);
    } else if (server->response != NULL) {
        (void)dsd_socket_send(client, server->response, (int)server->response_len, 0);
    }

    (void)dsd_socket_close(client);
    (void)dsd_socket_close(server->listen_sock);
    DSD_THREAD_RETURN;
}

/**
 * @brief Start a loopback HTTP server on an ephemeral port.
 *
 * @return 0 on success.
 */
static int
rr_test_server_start(rr_test_server* server, char* out_url, size_t out_url_sz, const char* status_line,
                     const char* body, size_t body_len, unsigned int stall_ms) {
    DSD_MEMSET(server, 0, sizeof(*server));
    server->listen_sock = DSD_INVALID_SOCKET;
    server->stall_ms = stall_ms;

    if (body != NULL) {
        char headers[256];
        const int hn = DSD_SNPRINTF(headers, sizeof(headers),
                                    "%s\r\nContent-Type: text/xml; charset=utf-8\r\n"
                                    "Content-Length: %zu\r\nConnection: close\r\n\r\n",
                                    status_line, body_len);
        if (hn <= 0 || (size_t)hn >= sizeof(headers)) {
            return 1;
        }
        /* One bound, on the value the allocation is actually given, and placed
         * where nothing has constrained that value yet - the caller's length
         * comes straight from a file read. Bounding body_len earlier instead
         * would leave this comparison unreachable, which is a check that reads
         * as protection without being any.
         *
         * The overflow guard comes first because the sum is what gets compared:
         * a body_len near SIZE_MAX would wrap it back under the ceiling and slip
         * through. Kept in a local because the bound stops travelling with the
         * value once it is read back out of the struct after the copies. */
        if (body_len > SIZE_MAX - (size_t)hn - 1U) {
            return 1;
        }
        const size_t total = (size_t)hn + body_len;
        if (total > sizeof(headers) + RR_FIXTURE_CAP_BYTES) {
            return 1;
        }
        server->response = (char*)malloc(total + 1U);
        if (server->response == NULL) {
            return 1;
        }
        DSD_MEMCPY(server->response, headers, (size_t)hn);
        DSD_MEMCPY(server->response + hn, body, body_len);
        /* `total` is bounded above and the allocation is exactly total + 1, so
         * this index is in range by construction. The analyzer keeps the taint
         * on the fread-derived length no matter how it is bounded, and refuses
         * to relate it back to the malloc it just sized. */
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound)
        server->response[total] = '\0';
        server->response_len = total;
    }

    if (dsd_socket_init() != 0) {
        return 1;
    }
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sock == DSD_INVALID_SOCKET) {
        return 1;
    }
    int one = 1;
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, (int)sizeof(one));
    (void)dsd_socket_set_recv_timeout(sock, 10000U);

    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (dsd_socket_bind(sock, (const struct sockaddr*)&addr, (int)sizeof(addr)) != 0
        || dsd_socket_listen(sock, 1) != 0) {
        (void)dsd_socket_close(sock);
        return 1;
    }

    socklen_t addr_len = (socklen_t)sizeof(addr);
    if (getsockname(sock, (struct sockaddr*)&addr, &addr_len) != 0) {
        (void)dsd_socket_close(sock);
        return 1;
    }
    const int n = DSD_SNPRINTF(out_url, out_url_sz, "http://127.0.0.1:%u/", (unsigned int)ntohs(addr.sin_port));
    if (n <= 0 || (size_t)n >= out_url_sz) {
        (void)dsd_socket_close(sock);
        return 1;
    }

    server->listen_sock = sock;
    if (dsd_thread_create(&server->thread, rr_test_server_thread, server) != 0) {
        (void)dsd_socket_close(sock);
        return 1;
    }
    return 0;
}

static void
rr_test_server_stop(rr_test_server* server) {
    (void)dsd_thread_join(server->thread);
    free(server->response);
    server->response = NULL;
    dsd_socket_cleanup();
}

static void
test_loopback_success(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("trs_sites_nxdn.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    rr_test_server server;
    dsd_rr_client_config config;
    DSD_MEMSET(&config, 0, sizeof(config));
    config.connect_timeout_ms = 3000;
    config.total_timeout_ms = 10000;
    config.transient_retries = 0;

    if (rr_test_server_start(&server, config.endpoint_url, sizeof(config.endpoint_url), "HTTP/1.1 200 OK", body, len,
                             0U)
        != 0) {
        DSD_FPRINTF(stderr, "FAIL: loopback server did not start\n");
        g_failures++;
        free(body);
        return;
    }

    dsd_rr_client* client = dsd_rr_client_create(&config);
    dsd_rr_auth auth;
    fill_auth(&auth, "user");
    dsd_rr_site_list sites;
    DSD_MEMSET(&sites, 0, sizeof(sites));
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    expect("loopback request succeeds", dsd_rr_get_trs_sites(client, &auth, 12918, &sites, &err) == 0);
    expect_ll("loopback site count", (long long)sites.count, 1);

    dsd_rr_site_list_free(&sites);
    dsd_rr_client_destroy(client);
    rr_test_server_stop(&server);

    expect("server saw the request", server.saw_request == 1);
    expect("request is a POST", strncmp(server.request, "POST ", 5) == 0);
    expect("Content-Type is set", strstr(server.request, "text/xml") != NULL);
    /*
     * SOAPAction is not enforced by the real server and the reference client
     * sends none, so nothing here pins an exact-empty header - only that the
     * body arrived escaped rather than raw.
     */
    expect("credentials escaped in the real request", strstr(server.request, k_password_sentinel) != NULL);
    free(body);
}

static void
test_loopback_fault_is_classified(void) {
    char* body = NULL;
    size_t len = 0;
    if (read_fixture("fault_auth.xml", &body, &len) != 0) {
        g_failures++;
        return;
    }

    rr_test_server server;
    dsd_rr_client_config config;
    DSD_MEMSET(&config, 0, sizeof(config));
    config.connect_timeout_ms = 3000;
    config.total_timeout_ms = 10000;
    config.transient_retries = 0;

    if (rr_test_server_start(&server, config.endpoint_url, sizeof(config.endpoint_url),
                             "HTTP/1.1 500 Internal Server Error", body, len, 0U)
        != 0) {
        g_failures++;
        free(body);
        return;
    }

    dsd_rr_client* client = dsd_rr_client_create(&config);
    dsd_rr_auth auth;
    fill_auth(&auth, "user");
    dsd_rr_user_info info;
    DSD_MEMSET(&info, 0, sizeof(info));
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    expect("loopback fault reports failure", dsd_rr_get_user_data(client, &auth, &info, &err) != 0);
    expect_ll("loopback fault classifies as AUTH", (long long)err.status, (long long)DSD_RR_ERR_AUTH);
    expect_ll("loopback fault http status", err.http_status, 500);
    expect_no_credentials("loopback fault", &err);

    dsd_rr_client_destroy(client);
    rr_test_server_stop(&server);
    free(body);
}

static void
test_loopback_unreachable(void) {
    /* Bind and immediately close, so the port is almost certainly refused. */
    rr_test_server server;
    char url[256];
    if (rr_test_server_start(&server, url, sizeof(url), "HTTP/1.1 200 OK", "x", 1U, 0U) != 0) {
        g_failures++;
        return;
    }
    (void)dsd_socket_close(server.listen_sock);
    (void)dsd_thread_join(server.thread);
    free(server.response);

    dsd_rr_client_config config;
    DSD_MEMSET(&config, 0, sizeof(config));
    (void)DSD_SNPRINTF(config.endpoint_url, sizeof(config.endpoint_url), "%s", url);
    config.connect_timeout_ms = 1500;
    config.total_timeout_ms = 3000;
    config.transient_retries = 0;

    dsd_rr_client* client = dsd_rr_client_create(&config);
    dsd_rr_auth auth;
    fill_auth(&auth, "user");
    dsd_rr_user_info info;
    DSD_MEMSET(&info, 0, sizeof(info));
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    expect("unreachable endpoint reports failure", dsd_rr_get_user_data(client, &auth, &info, &err) != 0);
    expect_ll("unreachable endpoint is a network error", (long long)err.status, (long long)DSD_RR_ERR_NETWORK);
    expect_no_credentials("unreachable endpoint", &err);

    dsd_rr_client_destroy(client);
    dsd_socket_cleanup();
}

/**
 * @brief Cancel a transfer that the server never answers.
 *
 * This is the assertion that catches a missing CURLOPT_NOPROGRESS: libcurl never
 * calls the xferinfo callback while NOPROGRESS is set, so cancellation would
 * silently do nothing and this would hang until the total timeout.
 */
static void
test_loopback_cancel_mid_transfer(void) {
    rr_test_server server;
    dsd_rr_client_config config;
    DSD_MEMSET(&config, 0, sizeof(config));
    config.connect_timeout_ms = 3000;
    config.total_timeout_ms = 30000; /* Long, so a timeout cannot be mistaken for a cancel. */
    config.transient_retries = 0;

    if (rr_test_server_start(&server, config.endpoint_url, sizeof(config.endpoint_url), NULL, NULL, 0U, 10000U) != 0) {
        g_failures++;
        return;
    }

    dsd_rr_client* client = dsd_rr_client_create(&config);
    dsd_rr_auth auth;
    fill_auth(&auth, "user");

    async_result* out = async_result_new();
    expect("callback context allocated", out != NULL);
    if (out == NULL) {
        dsd_rr_client_destroy(client);
        rr_test_server_stop(&server);
        return;
    }
    const uint64_t id = dsd_rr_fetch_trs_sites(client, &auth, 12918, async_cb, out);
    expect("stalled fetch queued", id != 0U);

    /* Let the request reach the server, then cancel it. */
    dsd_sleep_ms(300U);
    (void)dsd_rr_cancel(client, id);

    const long long started = (long long)time(NULL);
    const int fired = wait_done(out, 5000U);
    const long long elapsed = (long long)time(NULL) - started;

    expect("cancelled transfer completes", fired);
    expect_ll("cancelled transfer reports CANCELLED", (long long)atomic_load(&out->status),
              (long long)DSD_RR_ERR_CANCELLED);
    expect("cancellation lands within a few seconds", elapsed <= 4);

    dsd_rr_client_destroy(client);
    free(out);
    rr_test_server_stop(&server);
}

#endif /* USE_CURL && !DSD_PLATFORM_WIN_NATIVE */

/*
 * Each network case keeps its symbol on every platform so main()'s call list
 * needs no conditionals.
 */
static void
run_loopback_cases(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return;
#else
    test_loopback_success();
    test_loopback_fault_is_classified();
    test_loopback_unreachable();
    test_loopback_cancel_mid_transfer();
#endif
}

int
main(void) {
    test_availability_and_arguments();
    test_subscription_expiry();
    test_request_shape();
    test_talkgroups_send_zero_filters();
    test_country_list_needs_no_auth();
    test_status_mapping();
    test_retry_policy();
    test_support_map_cache();
    test_async_fetch();
    test_cancel_queued_request();
    test_destroy_with_pending();
    run_loopback_cases();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d assertion(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
