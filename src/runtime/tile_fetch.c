// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Map tile fetch: request validation, the built-in libcurl transport, response
 * header parsing and status classification.
 *
 * The parsing and classification run on whatever the transport hands back, so an
 * injected transport goes through exactly the checks the built-in one does.
 * Nothing in this file logs: a tile URL names the area being viewed.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/tile_fetch.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef USE_CURL
#include "curl_common.h"

#include <curl/curl.h>
#include <curl/curlver.h>
#include <curl/system.h>
#ifdef DSD_TILE_FETCH_TEST_HOOKS
#include "tile_fetch_test_hooks.h"
#endif
#endif

#define TF_UA_SUFFIX " (+https://github.com/arancormonk/dsd-neo)"

#ifdef DSD_TILE_FETCH_TEST_PLAIN_HTTP
/* Test builds only (RUNTIME_TILE_FETCH_PLAIN_HTTP): the same transport over plain http to a loopback server, so a real
 * libcurl's header and size handling can be driven without TLS. The shipped library is https only. */
#define TF_SCHEME       "http://"
#define TF_PROTOCOL     "http"
#define TF_PROTOCOL_BIT CURLPROTO_HTTP
#else
#define TF_SCHEME       "https://"
#define TF_PROTOCOL     "https"
#define TF_PROTOCOL_BIT CURLPROTO_HTTPS
#endif
#define TF_UA_VERSION_MAX 64U
#define TF_UNAVAILABLE    "online map tiles are not available in this build"

struct dsd_tile_fetch_cancel {
    atomic_int flag;
};

/* The share handle is made on first use, on the fetching thread, after libcurl's global init. */
struct dsd_tile_fetch_session {
    void* share;     /* CURLSH*, kept opaque here so the struct compiles without curl */
    int share_tried; /* creation was attempted (once; a failure means no reuse, never a failed fetch) */
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Copy text into a fixed buffer, truncating, always terminated.
 *
 * @param dst Destination.
 * @param cap Capacity of @p dst.
 * @param src Source text, or NULL for "".
 */
static void
tf_copy_text(char* dst, size_t cap, const char* src) {
    if (dst == NULL || cap == 0U) {
        return;
    }
    (void)DSD_SNPRINTF(dst, cap, "%s", (src != NULL) ? src : "");
}

/** @return The ASCII lower case of @p c. */
static char
tf_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/**
 * @brief Case-insensitive ASCII comparison of a counted span with a C string.
 *
 * @param span Text, not necessarily terminated.
 * @param len  Length of @p span.
 * @param name Terminated text to compare with.
 * @return 1 when they are equal ignoring ASCII case, 0 otherwise.
 */
static int
tf_span_equals_nocase(const char* span, size_t len, const char* name) {
    size_t i = 0;
    for (; i < len; i++) {
        if (name[i] == '\0' || tf_lower(span[i]) != tf_lower(name[i])) {
            return 0;
        }
    }
    return name[i] == '\0' ? 1 : 0;
}

/** @return 1 when @p c is an RFC 9110 tchar (token character). */
static int
tf_is_tchar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return 1;
    }
    return (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL) ? 1 : 0;
}

/**
 * @brief Whether a request header value is safe to send: visible ASCII and spaces only.
 *
 * @param value Value, terminated.
 * @param cap   The value must be shorter than this.
 * @return 1 when safe, 0 otherwise.
 */
static int
tf_request_value_ok(const char* value, size_t cap) {
    size_t i = 0;
    for (; value[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)value[i];
        if (i + 1U >= cap || c < 0x20U || c > 0x7EU) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Cancel token                                                               */
/* ------------------------------------------------------------------------- */

dsd_tile_fetch_cancel*
dsd_tile_fetch_cancel_create(void) {
    dsd_tile_fetch_cancel* cancel = (dsd_tile_fetch_cancel*)calloc(1U, sizeof(*cancel));
    if (cancel != NULL) {
        atomic_store(&cancel->flag, 0);
    }
    return cancel;
}

void
dsd_tile_fetch_cancel_destroy(dsd_tile_fetch_cancel* cancel) {
    free(cancel);
}

dsd_tile_fetch_session*
dsd_tile_fetch_session_create(void) {
    return (dsd_tile_fetch_session*)calloc(1U, sizeof(dsd_tile_fetch_session));
}

void
dsd_tile_fetch_session_destroy(dsd_tile_fetch_session* session) {
    if (session == NULL) {
        return;
    }
#ifdef USE_CURL
    if (session->share != NULL) {
        /* Every per-fetch handle is cleaned up before its fetch returns, so the share is not in use. */
        (void)curl_share_cleanup((CURLSH*)session->share);
    }
#endif
    free(session);
}

void
dsd_tile_fetch_cancel_request(dsd_tile_fetch_cancel* cancel) {
    if (cancel != NULL) {
        atomic_store(&cancel->flag, 1);
    }
}

void
dsd_tile_fetch_cancel_clear(dsd_tile_fetch_cancel* cancel) {
    if (cancel != NULL) {
        atomic_store(&cancel->flag, 0);
    }
}

int
dsd_tile_fetch_cancel_requested(const dsd_tile_fetch_cancel* cancel) {
    if (cancel == NULL) {
        return 0;
    }
    /* Cast away const: the flag is atomic, and the read is the whole point. */
    dsd_tile_fetch_cancel* token = (dsd_tile_fetch_cancel*)cancel;
    return atomic_load(&token->flag) != 0 ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* User-Agent                                                                 */
/* ------------------------------------------------------------------------- */

int
dsd_tile_fetch_user_agent(char* out, size_t cap) {
    if (out == NULL || cap == 0U) {
        return -1;
    }
    out[0] = '\0';

    /* The tag is "v2.12.1" or "v2.12.1-3-gabc1234"; the product version drops the "v". */
    const char* version = GIT_TAG;
    if (version[0] == 'v' && version[1] >= '0' && version[1] <= '9') {
        version++;
    }
    size_t len = 0;
    while (len <= TF_UA_VERSION_MAX && version[len] != '\0' && tf_is_tchar(version[len])) {
        len++;
    }
    if (len == 0U || len > TF_UA_VERSION_MAX || version[len] != '\0') {
        version = "unknown";
    }

    const int n = DSD_SNPRINTF(out, cap, "dsd-neo/%s%s", version, TF_UA_SUFFIX);
    if (n <= 0 || (size_t)n >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Built-in libcurl transport                                                 */
/* ------------------------------------------------------------------------- */

#ifdef USE_CURL

#ifdef DSD_TILE_FETCH_TEST_HOOKS
#define TF_SETOPT_LONG           dsd_tile_fetch_test_setopt_long
#define TF_SETOPT_PTR            dsd_tile_fetch_test_setopt_ptr
#define TF_SETOPT_DATA_CB        dsd_tile_fetch_test_setopt_data_cb
#define TF_SETOPT_XFERINFO_CB    dsd_tile_fetch_test_setopt_xferinfo_cb
#define TF_PERFORM(curl, cancel) ((void)(cancel), dsd_tile_fetch_test_perform(curl))
#define TF_RESPONSE_CODE         dsd_tile_fetch_test_response_code
#define TF_CURL_INFO             dsd_tile_fetch_test_curl_version_info
#define TF_GLOBAL_READY          dsd_tile_fetch_test_global_ready
#if defined(DSD_TILE_FETCH_TEST_CURL_VERSION_NUM) && DSD_TILE_FETCH_TEST_CURL_VERSION_NUM < LIBCURL_VERSION_NUM
/* Lets a test build take the pre-7.85 protocol branch against newer headers; it only ever lowers the version. */
#define TF_CURL_VERSION_NUM DSD_TILE_FETCH_TEST_CURL_VERSION_NUM
#endif
#else
static CURLcode
tf_setopt_long(CURL* curl, CURLoption option, long value) {
    return curl_easy_setopt(curl, option, value);
}

static CURLcode
tf_setopt_ptr(CURL* curl, CURLoption option, void* value) {
    return curl_easy_setopt(curl, option, value);
}

static CURLcode
tf_setopt_data_cb(CURL* curl, CURLoption option, curl_write_callback value) {
    return curl_easy_setopt(curl, option, value);
}

static CURLcode
tf_setopt_xferinfo_cb(CURL* curl, CURLoption option, curl_xferinfo_callback value) {
    return curl_easy_setopt(curl, option, value);
}

static CURLcode
tf_response_code(CURL* curl, long* out) {
    return curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out);
}

/** Cancel predicate for dsd_curl_perform_cancellable(). */
static int
tf_cancel_requested(const void* ctx) {
    return dsd_tile_fetch_cancel_requested((const dsd_tile_fetch_cancel*)ctx);
}

#define TF_PERFORM(curl, cancel) dsd_curl_perform_cancellable((curl), tf_cancel_requested, (cancel))
#define TF_SETOPT_LONG           tf_setopt_long
#define TF_SETOPT_PTR            tf_setopt_ptr
#define TF_SETOPT_DATA_CB        tf_setopt_data_cb
#define TF_SETOPT_XFERINFO_CB    tf_setopt_xferinfo_cb
#define TF_RESPONSE_CODE         tf_response_code
#define TF_CURL_INFO             dsd_curl_version_info
#define TF_GLOBAL_READY          dsd_curl_global_ready
#endif

#ifndef TF_CURL_VERSION_NUM
#define TF_CURL_VERSION_NUM LIBCURL_VERSION_NUM
#endif

/** @return 1 when @p protocols (curl_version_info()'s NULL-ended list) names https. */
static int
tf_lists_https(const char* const* protocols) {
    for (const char* const* p = protocols; p != NULL && *p != NULL; p++) {
        if (strcmp(*p, TF_PROTOCOL) == 0) {
            return 1;
        }
    }
    return 0;
}

int
dsd_tile_fetch_available(void) {
    /* curl_version_info() is not thread-safe (before curl_global_init() it sets up static data, and each call rewrites
     * static buffers): read the one result taken inside the shared init guard (dsd_curl_version_info()). */
    if (TF_GLOBAL_READY() != 0) {
        return 0;
    }
    const curl_version_info_data* info = TF_CURL_INFO();
    if (info == NULL || (info->features & CURL_VERSION_ASYNCHDNS) == 0) {
        return 0;
    }
    /* Every tile request is https: a library without it would fail every request, so offer nothing. */
    return tf_lists_https(info->protocols);
}

static int
tf_xferinfo_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return dsd_tile_fetch_cancel_requested((const dsd_tile_fetch_cancel*)clientp) ? 1 : 0;
}

typedef enum {
    TF_OPT_LONG,
    TF_OPT_PTR,
    TF_OPT_DATA_CB,
    TF_OPT_XFERINFO_CB,
} tf_opt_kind;

/** One easy-handle option, so the whole set is applied and checked by one loop. */
typedef struct {
    CURLoption option;
    tf_opt_kind kind;
    long lval;
    void* pval;
    curl_write_callback data_cb;
    curl_xferinfo_callback xferinfo_cb;
} tf_opt;

/** Everything one transfer's options point at; it must outlive the transfer. */
typedef struct {
    const dsd_tile_fetch_request* req;
    char user_agent[128];
    struct curl_slist* headers;
    dsd_curl_body_buf body;
    dsd_curl_body_buf head;
    CURLSH* share; /* the session's caches, or NULL */
} tf_transfer;

#define TF_OPT_L(o, v)  {(o), TF_OPT_LONG, (long)(v), NULL, NULL, NULL}
#define TF_OPT_P(o, v)  {(o), TF_OPT_PTR, 0L, (void*)(v), NULL, NULL}
#define TF_OPT_CB(o, v) {(o), TF_OPT_DATA_CB, 0L, NULL, (v), NULL}
#define TF_OPT_XI(o, v) {(o), TF_OPT_XFERINFO_CB, 0L, NULL, NULL, (v)}

/**
 * @brief Set every option the tile request needs, failing closed on the first refusal.
 *
 * dsd_curl_apply_hardening() runs first, for the trust store, but it drops its
 * own results, so the options that bound the request (timeouts, no signals, no
 * redirects, HTTPS only, peer and host verification) are set again here, where a
 * refusal is seen. The hardening allows plain http for rdio uploads; this
 * narrows it to https, because Android's cleartext policy does not reach a
 * native socket. CURLOPT_NOPROGRESS defaults to 1, and while it is set libcurl
 * never calls the xferinfo callback, so without the explicit 0 cancellation
 * would silently never fire. No Accept-Encoding is sent.
 *
 * @param curl Easy handle.
 * @param t    Transfer state the options point at.
 * @return 0 when every option was accepted, -1 at the first one refused.
 */
static int
tf_apply_options(CURL* curl, tf_transfer* t) {
    const dsd_tile_fetch_request* req = t->req;
    const long connect_ms =
        (req->connect_timeout_ms > 0) ? req->connect_timeout_ms : DSD_TILE_FETCH_CONNECT_TIMEOUT_MS_DEFAULT;
    const long total_ms = (req->total_timeout_ms > 0) ? req->total_timeout_ms : DSD_TILE_FETCH_TOTAL_TIMEOUT_MS_DEFAULT;

    dsd_curl_apply_hardening(curl, (int)connect_ms, (int)total_ms, t->user_agent);

    const tf_opt opts[] = {
        TF_OPT_L(CURLOPT_CONNECTTIMEOUT_MS, connect_ms),
        TF_OPT_L(CURLOPT_TIMEOUT_MS, total_ms),
        TF_OPT_L(CURLOPT_NOSIGNAL, 1L),
        TF_OPT_L(CURLOPT_FOLLOWLOCATION, 0L),
#if TF_CURL_VERSION_NUM >= 0x075500 /* CURLOPT_PROTOCOLS_STR landed in 7.85.0 */
        TF_OPT_P(CURLOPT_PROTOCOLS_STR, TF_PROTOCOL),
#else
        TF_OPT_L(CURLOPT_PROTOCOLS, TF_PROTOCOL_BIT),
#endif
        TF_OPT_P(CURLOPT_USERAGENT, t->user_agent),
        TF_OPT_L(CURLOPT_SSL_VERIFYPEER, 1L),
        TF_OPT_L(CURLOPT_SSL_VERIFYHOST, 2L),
        TF_OPT_P(CURLOPT_URL, req->url),
        TF_OPT_L(CURLOPT_HTTPGET, 1L),
        TF_OPT_P(CURLOPT_HTTPHEADER, t->headers),
        TF_OPT_L(CURLOPT_MAXFILESIZE, DSD_TILE_FETCH_BODY_CAP_BYTES),
        TF_OPT_CB(CURLOPT_WRITEFUNCTION, dsd_curl_body_write_cb),
        TF_OPT_P(CURLOPT_WRITEDATA, &t->body),
        TF_OPT_CB(CURLOPT_HEADERFUNCTION, dsd_curl_body_write_cb),
        TF_OPT_P(CURLOPT_HEADERDATA, &t->head),
        TF_OPT_L(CURLOPT_NOPROGRESS, 0L),
        TF_OPT_XI(CURLOPT_XFERINFOFUNCTION, tf_xferinfo_cb),
        /* The callback only reads the token; CURLOPT_XFERINFODATA takes a void*. */
        TF_OPT_P(CURLOPT_XFERINFODATA, (dsd_tile_fetch_cancel*)req->cancel),
    };

    if (t->share != NULL && TF_SETOPT_PTR(curl, CURLOPT_SHARE, t->share) != CURLE_OK) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(opts) / sizeof(opts[0]); i++) {
        const tf_opt* o = &opts[i];
        CURLcode rc = CURLE_OK;
        switch (o->kind) {
            case TF_OPT_LONG: rc = TF_SETOPT_LONG(curl, o->option, o->lval); break;
            case TF_OPT_PTR: rc = TF_SETOPT_PTR(curl, o->option, o->pval); break;
            case TF_OPT_DATA_CB: rc = TF_SETOPT_DATA_CB(curl, o->option, o->data_cb); break;
            default: rc = TF_SETOPT_XFERINFO_CB(curl, o->option, o->xferinfo_cb); break;
        }
        if (rc != CURLE_OK) {
            return -1;
        }
    }
    return 0;
}

/**
 * @brief Append one conditional request header when its value is given.
 *
 * @param list  Header list, updated in place.
 * @param name  Field name with its colon and space.
 * @param value Value, or NULL/"" for none.
 * @return 0 on success or when there is nothing to add, -1 on allocation failure.
 */
static int
tf_add_conditional(struct curl_slist** list, const char* name, const char* value) {
    if (value == NULL || value[0] == '\0') {
        return 0;
    }
    char line[DSD_TILE_FETCH_ETAG_MAX + 32U];
    const int n = DSD_SNPRINTF(line, sizeof(line), "%s%s", name, value);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        return -1;
    }
    struct curl_slist* next = curl_slist_append(*list, line);
    if (next == NULL) {
        return -1;
    }
    *list = next;
    return 0;
}

/**
 * @brief Map a libcurl transfer result to a fetch status.
 *
 * @param code libcurl result.
 * @return The matching status.
 */
static dsd_tile_fetch_status
tf_status_from_curl(CURLcode code) {
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        return DSD_TILE_FETCH_ERR_CANCELLED;
    }
    if (code == CURLE_OUT_OF_MEMORY) {
        return DSD_TILE_FETCH_ERR_NOMEM;
    }
    if (code == CURLE_URL_MALFORMAT) {
        /* The address itself cannot work (a bad host or port): a settings problem, never retried as an outage. */
        return DSD_TILE_FETCH_ERR_INVALID;
    }
    if (code == CURLE_WRITE_ERROR || code == CURLE_FILESIZE_EXCEEDED) {
        /* A write or header callback hit its cap and returned 0, or the declared
         * length was already over the body cap. */
        return DSD_TILE_FETCH_ERR_TOO_LARGE;
    }
    return DSD_TILE_FETCH_ERR_NETWORK;
}

/**
 * @brief Reason text for a failed transfer. libcurl's own text names the error
 *        class only, never the URL.
 *
 * @param status Mapped status.
 * @param code   libcurl result.
 * @return Static text.
 */
static const char*
tf_curl_error_text(dsd_tile_fetch_status status, CURLcode code) {
    if (status == DSD_TILE_FETCH_ERR_TOO_LARGE) {
        return "response too large";
    }
    if (status == DSD_TILE_FETCH_ERR_CANCELLED) {
        return "cancelled";
    }
    return curl_easy_strerror(code);
}

/**
 * @brief Prepare the per-transfer state: user agent, conditional headers, buffers.
 *
 * @param t   State to fill; zeroed by the caller.
 * @param req Validated request.
 * @return 0 on success, -1 on allocation failure.
 */
static int
tf_transfer_init(tf_transfer* t, const dsd_tile_fetch_request* req) {
    t->req = req;
    if (dsd_tile_fetch_user_agent(t->user_agent, sizeof(t->user_agent)) != 0
        || tf_add_conditional(&t->headers, "If-None-Match: ", req->if_none_match) != 0
        || tf_add_conditional(&t->headers, "If-Modified-Since: ", req->if_modified_since) != 0
        || dsd_curl_body_buf_init(&t->body, DSD_TILE_FETCH_BODY_CAP_BYTES + 1U) != 0
        || dsd_curl_body_buf_init(&t->head, DSD_TILE_FETCH_HEADER_CAP_BYTES + 1U) != 0) {
        return -1;
    }
    return 0;
}

static void
tf_transfer_free(tf_transfer* t) {
    curl_slist_free_all(t->headers);
    t->headers = NULL;
    dsd_curl_body_buf_free(&t->body);
    dsd_curl_body_buf_free(&t->head);
}

/** @return 1 when @p head ends with the blank line that closes a header block (it was not cut by its cap). */
static int
tf_head_complete(const dsd_curl_body_buf* head) {
    const size_t n = head->len;
    if (head->data == NULL || n < 2U) {
        return 0;
    }
    const char* d = head->data;
    return (d[n - 1U] == '\n' && (d[n - 2U] == '\n' || (n >= 3U && d[n - 2U] == '\r' && d[n - 3U] == '\n'))) ? 1 : 0;
}

/**
 * @brief The session's share handle, made on first use: connections (7.57+), DNS and TLS sessions.
 *
 * Only the session's own thread uses it, so it needs no lock callbacks. A failure
 * leaves the session without a share for good: fetches go on without reuse.
 *
 * @param session Session, or NULL.
 * @return The share handle, or NULL.
 */
static CURLSH*
tf_session_share(dsd_tile_fetch_session* session) {
    if (session == NULL) {
        return NULL;
    }
    if (!session->share_tried) {
        session->share_tried = 1;
        CURLSH* share = curl_share_init();
        int shared = 0; /* each cache the library can share; one that it cannot (no TLS backend) is skipped */
        if (share != NULL) {
#if LIBCURL_VERSION_NUM >= 0x073900 /* CURL_LOCK_DATA_CONNECT landed in 7.57.0 */
            shared += curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT) == CURLSHE_OK;
#endif
            shared += curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS) == CURLSHE_OK;
            shared += curl_share_setopt(share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION) == CURLSHE_OK;
        }
        if (shared == 0 && share != NULL) {
            (void)curl_share_cleanup(share);
            share = NULL;
        }
        session->share = share;
    }
    return (CURLSH*)session->share;
}

/**
 * @brief The built-in transport: one HTTPS GET through libcurl.
 *
 * @param ctx      Unused.
 * @param req      Validated request.
 * @param exchange Receives the exchange.
 * @return 0 when an HTTP exchange completed, -1 otherwise.
 */
static int
tf_curl_perform(void* ctx, const dsd_tile_fetch_request* req, dsd_tile_fetch_exchange* exchange) {
    (void)ctx;
    if (TF_GLOBAL_READY() != 0) {
        /* libcurl failed to start at run time (checked first: the capability probe needs it). */
        exchange->status = DSD_TILE_FETCH_ERR_SETUP;
        tf_copy_text(exchange->error, sizeof(exchange->error), "libcurl could not be initialized");
        return -1;
    }
    if (!dsd_tile_fetch_available()) {
        exchange->status = DSD_TILE_FETCH_ERR_UNSUPPORTED;
        tf_copy_text(exchange->error, sizeof(exchange->error), TF_UNAVAILABLE);
        return -1;
    }

    CURL* curl = curl_easy_init();
    tf_transfer t;
    DSD_MEMSET(&t, 0, sizeof(t));
    t.share = tf_session_share(req->session);
    if (curl == NULL || tf_transfer_init(&t, req) != 0) {
        tf_transfer_free(&t);
        if (curl != NULL) {
            curl_easy_cleanup(curl);
        }
        exchange->status = DSD_TILE_FETCH_ERR_NOMEM;
        tf_copy_text(exchange->error, sizeof(exchange->error), "out of memory");
        return -1;
    }

    if (tf_apply_options(curl, &t) != 0) {
        curl_easy_cleanup(curl);
        tf_transfer_free(&t);
        exchange->status = DSD_TILE_FETCH_ERR_SETUP;
        tf_copy_text(exchange->error, sizeof(exchange->error), "libcurl refused a request option");
        return -1;
    }

    const CURLcode code = TF_PERFORM(curl, req->cancel);
    long http_status = 0;
    if (TF_RESPONSE_CODE(curl, &http_status) != CURLE_OK) {
        http_status = 0;
    }
    curl_easy_cleanup(curl);

    exchange->http_status = http_status;
    if (code != CURLE_OK) {
        exchange->status = tf_status_from_curl(code);
        if (exchange->status == DSD_TILE_FETCH_ERR_TOO_LARGE
            && (code == CURLE_FILESIZE_EXCEEDED || tf_head_complete(&t.head)) && t.head.data != NULL) {
            /* Only the body was over its cap: the headers (status, Retry-After) still say what answer it was. A
             * declared Content-Length over the cap (CURLE_FILESIZE_EXCEEDED, never the header cap) stops older libcurl
             * before the closing blank line reaches the callback (8.5, 7.81: every line but it), so the lines seen so
             * far are kept as they are. A write refusal (CURLE_WRITE_ERROR) can be the header cap, so there the block
             * must be whole. */
            exchange->headers = t.head.data;
            exchange->headers_len = t.head.len;
            t.head.data = NULL;
        }
        tf_transfer_free(&t);
        tf_copy_text(exchange->error, sizeof(exchange->error), tf_curl_error_text(exchange->status, code));
        return -1;
    }

    /* Ownership moves to the exchange; the slist goes with tf_transfer_free(). */
    exchange->body = (unsigned char*)t.body.data;
    exchange->body_len = t.body.len;
    exchange->headers = t.head.data;
    exchange->headers_len = t.head.len;
    t.body.data = NULL;
    t.head.data = NULL;
    tf_transfer_free(&t);
    return 0;
}

#else /* !USE_CURL */

int
dsd_tile_fetch_available(void) {
    return 0;
}

static int
tf_curl_perform(void* ctx, const dsd_tile_fetch_request* req, dsd_tile_fetch_exchange* exchange) {
    (void)ctx;
    (void)req;
    exchange->status = DSD_TILE_FETCH_ERR_UNSUPPORTED;
    tf_copy_text(exchange->error, sizeof(exchange->error), TF_UNAVAILABLE);
    return -1;
}

#endif /* USE_CURL */

/* ------------------------------------------------------------------------- */
/* Response header parsing                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
    const char* name;
    size_t offset;
    size_t cap;
    unsigned int bit;
    int is_list; /**< Cache-Control and Vary: repeated lines join; loss fails the fetch. */
} tf_field;

#define TF_FIELD(name, bit, member, list)                                                                              \
    {(name), offsetof(dsd_tile_fetch_response, member), sizeof(((dsd_tile_fetch_response*)0)->member), (bit), (list)}

static const tf_field k_fields[] = {
    TF_FIELD("ETag", DSD_TILE_FETCH_HDR_ETAG, etag, 0),
    TF_FIELD("Last-Modified", DSD_TILE_FETCH_HDR_LAST_MODIFIED, last_modified, 0),
    TF_FIELD("Cache-Control", DSD_TILE_FETCH_HDR_CACHE_CONTROL, cache_control, 1),
    TF_FIELD("Expires", DSD_TILE_FETCH_HDR_EXPIRES, expires, 0),
    TF_FIELD("Date", DSD_TILE_FETCH_HDR_DATE, date, 0),
    TF_FIELD("Age", DSD_TILE_FETCH_HDR_AGE, age, 0),
    TF_FIELD("Vary", DSD_TILE_FETCH_HDR_VARY, vary, 1),
    TF_FIELD("Retry-After", DSD_TILE_FETCH_HDR_RETRY_AFTER, retry_after, 0),
};

#define TF_FIELD_COUNT (sizeof(k_fields) / sizeof(k_fields[0]))

/** Parser state across the lines of one header block. */
typedef struct {
    dsd_tile_fetch_response* resp;
    int in_block;   /**< Between a status line (or the start) and the empty line that ends the block. */
    int last_field; /**< Index into k_fields of the previous line's field, for obs-fold; -1 for none. */
    int bad_list;   /**< A Cache-Control or Vary value could not be carried. */
} tf_parse;

static void
tf_reset_fields(dsd_tile_fetch_response* resp) {
    for (size_t i = 0; i < TF_FIELD_COUNT; i++) {
        ((char*)resp + k_fields[i].offset)[0] = '\0';
    }
    resp->header_present = 0U;
    resp->header_invalid = 0U;
}

/** @return 1 when every byte is visible ASCII, space or tab. */
static int
tf_value_bytes_ok(const char* value, size_t len) {
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)value[i];
        if ((c < 0x20U && c != 0x09U) || c > 0x7EU) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Store one value of a list field (Cache-Control, Vary), joining repeats with ", ".
 *
 * @param p     Parser state.
 * @param f     Field.
 * @param value Trimmed value.
 * @param len   Length of @p value.
 * @param sep   Separator to put before it when the field already has text.
 */
static void
tf_store_list(tf_parse* p, const tf_field* f, const char* value, size_t len, const char* sep) {
    char* dst = (char*)p->resp + f->offset;
    p->resp->header_present |= f->bit;
    if (len == 0U) {
        return;
    }
    const size_t used = strlen(dst);
    const size_t sep_len = (used > 0U) ? strlen(sep) : 0U;
    if (!tf_value_bytes_ok(value, len) || used + sep_len + len >= f->cap) {
        p->bad_list = 1;
        return;
    }
    DSD_MEMCPY(dst + used, sep, sep_len);
    DSD_MEMCPY(dst + used + sep_len, value, len);
    dst[used + sep_len + len] = '\0';
}

/**
 * @brief Whether @p value can be carried for the single-valued field @p bit.
 *
 * A validator goes back in If-None-Match / If-Modified-Since, which refuse a tab
 * (tf_request_value_ok()): one that could not be sent is dropped on receipt, so
 * the next request goes out unconditionally instead of failing as invalid.
 */
static int
tf_single_value_ok(unsigned int bit, const char* value, size_t len) {
    const int validator = (bit & (DSD_TILE_FETCH_HDR_ETAG | DSD_TILE_FETCH_HDR_LAST_MODIFIED)) != 0U;
    if (validator && len > 0U && memchr(value, '\t', len) != NULL) {
        return 0;
    }
    return tf_value_bytes_ok(value, len);
}

/**
 * @brief Store the value of a single-valued field; a repeat, an empty value or one
 *        that cannot be carried marks the field invalid instead.
 *
 * @param p            Parser state.
 * @param f            Field.
 * @param value        Trimmed value.
 * @param len          Length of @p value.
 * @param continuation 1 for an obs-fold continuation of the same field.
 */
static void
tf_store_single(tf_parse* p, const tf_field* f, const char* value, size_t len, int continuation) {
    char* dst = (char*)p->resp + f->offset;
    dsd_tile_fetch_response* resp = p->resp;
    if ((resp->header_invalid & f->bit) != 0U) {
        return;
    }
    const int repeat = !continuation && (resp->header_present & f->bit) != 0U;
    resp->header_present |= f->bit;
    const size_t used = continuation ? strlen(dst) : 0U;
    const size_t sep_len = (continuation && used > 0U && len > 0U) ? 1U : 0U;
    if (repeat || (!continuation && len == 0U) || !tf_single_value_ok(f->bit, value, len)
        || used + sep_len + len >= f->cap) {
        resp->header_invalid |= f->bit;
        dst[0] = '\0';
        return;
    }
    if (sep_len > 0U) {
        dst[used] = ' ';
    }
    DSD_MEMCPY(dst + used + sep_len, value, len);
    dst[used + sep_len + len] = '\0';
}

/**
 * @brief Trim optional whitespace (space and tab) from both ends of a span.
 *
 * @param text In: start of the span. Out: start of the trimmed span.
 * @param len  In: length. Out: trimmed length.
 */
static void
tf_trim(const char** text, size_t* len) {
    const char* s = *text;
    size_t n = *len;
    while (n > 0U && (s[0] == ' ' || s[0] == '\t')) {
        s++;
        n--;
    }
    while (n > 0U && (s[n - 1U] == ' ' || s[n - 1U] == '\t')) {
        n--;
    }
    *text = s;
    *len = n;
}

/**
 * @brief Parse one field line ("Name: value") of the current block.
 *
 * A line with no colon, or with whitespace before it, is not a valid field line
 * and is skipped; so is any field this helper does not return.
 *
 * @param p    Parser state.
 * @param line Line without its terminator.
 * @param len  Length of @p line.
 */
static void
tf_parse_field_line(tf_parse* p, const char* line, size_t len) {
    p->last_field = -1;
    const char* colon = (const char*)memchr(line, ':', len);
    if (colon == NULL || colon == line || colon[-1] == ' ' || colon[-1] == '\t') {
        return;
    }
    const size_t name_len = (size_t)(colon - line);
    for (size_t i = 0; i < TF_FIELD_COUNT; i++) {
        if (tf_span_equals_nocase(line, name_len, k_fields[i].name)) {
            const char* value = colon + 1;
            size_t value_len = len - name_len - 1U;
            tf_trim(&value, &value_len);
            if (k_fields[i].is_list) {
                tf_store_list(p, &k_fields[i], value, value_len, ", ");
            } else {
                tf_store_single(p, &k_fields[i], value, value_len, 0);
            }
            p->last_field = (int)i;
            return;
        }
    }
}

/**
 * @brief Classify and parse one header line.
 *
 * A status line starts a new block and drops what earlier blocks (1xx, proxy
 * CONNECT) said; an empty line ends the block, so trailer fields after the body
 * are ignored. A line starting with whitespace is an obs-fold continuation and is
 * joined to the previous field with one space, as RFC 9112 section 5.2 says.
 *
 * @param p    Parser state.
 * @param line Line without its terminator.
 * @param len  Length of @p line.
 */
static void
tf_parse_line(tf_parse* p, const char* line, size_t len) {
    if (len >= 5U && memcmp(line, "HTTP/", 5U) == 0) {
        tf_reset_fields(p->resp);
        p->in_block = 1;
        p->last_field = -1;
        return;
    }
    if (len == 0U) {
        p->in_block = 0;
        p->last_field = -1;
        return;
    }
    if (!p->in_block) {
        return;
    }
    if (line[0] == ' ' || line[0] == '\t') {
        if (p->last_field >= 0) {
            const tf_field* f = &k_fields[p->last_field];
            const char* value = line;
            size_t value_len = len;
            tf_trim(&value, &value_len);
            if (f->is_list) {
                tf_store_list(p, f, value, value_len, " ");
            } else {
                tf_store_single(p, f, value, value_len, 1);
            }
        }
        return;
    }
    tf_parse_field_line(p, line, len);
}

/**
 * @brief Fill the response's header fields from a raw header block.
 *
 * @param resp    Response to fill.
 * @param block   Raw header block, possibly NULL.
 * @param len     Length of @p block.
 * @return 0 on success, -1 when a Cache-Control or Vary value could not be carried.
 */
static int
tf_parse_headers(dsd_tile_fetch_response* resp, const char* block, size_t len) {
    tf_parse p = {resp, 1, -1, 0};
    tf_reset_fields(resp);
    size_t pos = 0;
    while (block != NULL && pos < len) {
        const char* line = block + pos;
        const char* nl = (const char*)memchr(line, '\n', len - pos);
        size_t line_len = (nl != NULL) ? (size_t)(nl - line) : len - pos;
        pos += line_len + ((nl != NULL) ? 1U : 0U);
        if (line_len > 0U && line[line_len - 1U] == '\r') {
            line_len--;
        }
        tf_parse_line(&p, line, line_len);
    }
    return p.bad_list ? -1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Request validation and classification                                      */
/* ------------------------------------------------------------------------- */

/**
 * @brief Check the URL: https, a host, visible ASCII only, within the length cap.
 *
 * @param url Terminated URL.
 * @return NULL when valid, otherwise the reason (no URL text in it).
 */
static const char*
tf_validate_url(const char* url) {
    static const char k_scheme[] = TF_SCHEME;
    const size_t scheme_len = sizeof(k_scheme) - 1U;
    size_t len = 0;
    for (; url[len] != '\0'; len++) {
        const unsigned char c = (unsigned char)url[len];
        if (len >= DSD_TILE_FETCH_URL_MAX || c <= 0x20U || c > 0x7EU) {
            return "tile URL is too long or has spaces or control characters";
        }
    }
    if (len <= scheme_len || !tf_span_equals_nocase(url, scheme_len, k_scheme) || url[scheme_len] == '/') {
        return "tile URL is not https";
    }
    return NULL;
}

/** @return 1 when @p ms is 0 (default) or a timeout within range. */
static int
tf_timeout_ok(int ms) {
    return ms >= 0 && ms <= DSD_TILE_FETCH_TIMEOUT_MS_MAX ? 1 : 0;
}

/**
 * @brief Check a request before anything is sent.
 *
 * @param req Request.
 * @return NULL when valid, otherwise the reason (no URL text in it).
 */
static const char*
tf_validate(const dsd_tile_fetch_request* req) {
    if (req == NULL || req->url == NULL) {
        return "no tile URL";
    }
    const char* url_problem = tf_validate_url(req->url);
    if (url_problem != NULL) {
        return url_problem;
    }
    if ((req->if_none_match != NULL && !tf_request_value_ok(req->if_none_match, DSD_TILE_FETCH_ETAG_MAX))
        || (req->if_modified_since != NULL
            && !tf_request_value_ok(req->if_modified_since, DSD_TILE_FETCH_DATE_FIELD_MAX))) {
        return "stored validator is not sendable";
    }
    if (!tf_timeout_ok(req->connect_timeout_ms) || !tf_timeout_ok(req->total_timeout_ms)) {
        return "timeout out of range";
    }
    return NULL;
}

/**
 * @brief Turn a completed exchange into the response, taking its buffers.
 *
 * @param resp     Response, header fields already parsed.
 * @param exchange Completed exchange; its body is moved out on success.
 */
static void
tf_classify(dsd_tile_fetch_response* resp, dsd_tile_fetch_exchange* exchange) {
    const long code = exchange->http_status;
    resp->http_status = code;
    if (code == 200 && exchange->body != NULL && exchange->body_len > 0U) {
        resp->status = DSD_TILE_FETCH_OK;
        resp->body = exchange->body;
        resp->body_len = exchange->body_len;
        exchange->body = NULL;
        exchange->body_len = 0U;
        return;
    }
    if (code == 304) {
        resp->status = DSD_TILE_FETCH_NOT_MODIFIED;
        return;
    }
    if (code >= 300 && code <= 399) {
        resp->status = DSD_TILE_FETCH_ERR_REDIRECT;
        (void)DSD_SNPRINTF(resp->error, sizeof(resp->error), "server redirected (HTTP %ld); redirects are not followed",
                           code);
        return;
    }
    resp->status = DSD_TILE_FETCH_ERR_HTTP;
    if (code == 200) {
        tf_copy_text(resp->error, sizeof(resp->error), "server sent an empty tile");
    } else {
        (void)DSD_SNPRINTF(resp->error, sizeof(resp->error), "server answered HTTP %ld", code);
    }
}

/**
 * @brief For a response whose body was over its cap: keep the header fields when the header block itself was within
 *        its cap, so the caller can tell a 429 or 503 (back off, honour Retry-After) from an oversized tile.
 *
 * @param resp     Response; its status stays ERR_TOO_LARGE.
 * @param exchange Exchange with the header block, if any.
 */
static void
tf_keep_headers(dsd_tile_fetch_response* resp, const dsd_tile_fetch_exchange* exchange) {
    if (exchange->headers == NULL || exchange->headers_len > DSD_TILE_FETCH_HEADER_CAP_BYTES) {
        return;
    }
    if (tf_parse_headers(resp, exchange->headers, exchange->headers_len) != 0) {
        tf_reset_fields(resp);
    }
}

/** @return 1 when @p status is a class a transport may report for a failed exchange. */
static int
tf_is_transport_failure(dsd_tile_fetch_status status) {
    return status >= DSD_TILE_FETCH_ERR_NETWORK && status <= DSD_TILE_FETCH_ERR_NOMEM ? 1 : 0;
}

/**
 * @brief Finish a completed exchange: caps, headers, then status.
 *
 * The caps are checked here too, so an injected transport cannot hand back more
 * than the built-in one would accept.
 *
 * @param resp     Response.
 * @param exchange Completed exchange.
 */
static void
tf_finish(dsd_tile_fetch_response* resp, dsd_tile_fetch_exchange* exchange) {
    resp->http_status = exchange->http_status;
    if (exchange->body_len > DSD_TILE_FETCH_BODY_CAP_BYTES || exchange->headers_len > DSD_TILE_FETCH_HEADER_CAP_BYTES) {
        resp->status = DSD_TILE_FETCH_ERR_TOO_LARGE;
        tf_copy_text(resp->error, sizeof(resp->error), "response too large");
        tf_keep_headers(resp, exchange);
        return;
    }
    if (tf_parse_headers(resp, exchange->headers, exchange->headers_len) != 0) {
        tf_reset_fields(resp);
        resp->status = DSD_TILE_FETCH_ERR_BAD_HEADER;
        tf_copy_text(resp->error, sizeof(resp->error), "Cache-Control or Vary header is too long or malformed");
        return;
    }
    tf_classify(resp, exchange);
}

dsd_tile_fetch_status
dsd_tile_fetch(const dsd_tile_fetch_request* req, dsd_tile_fetch_response* resp) {
    if (resp == NULL) {
        return DSD_TILE_FETCH_ERR_INVALID;
    }
    DSD_MEMSET(resp, 0, sizeof(*resp));

    const char* invalid = tf_validate(req);
    if (invalid != NULL) {
        resp->status = DSD_TILE_FETCH_ERR_INVALID;
        tf_copy_text(resp->error, sizeof(resp->error), invalid);
        return resp->status;
    }
    if (dsd_tile_fetch_cancel_requested(req->cancel)) {
        resp->status = DSD_TILE_FETCH_ERR_CANCELLED;
        tf_copy_text(resp->error, sizeof(resp->error), "cancelled");
        return resp->status;
    }

    dsd_tile_fetch_transport transport = {tf_curl_perform, NULL};
    if (req->transport != NULL && req->transport->perform != NULL) {
        transport = *req->transport;
    }

    dsd_tile_fetch_exchange exchange;
    DSD_MEMSET(&exchange, 0, sizeof(exchange));
    if (transport.perform(transport.ctx, req, &exchange) != 0) {
        resp->status = tf_is_transport_failure(exchange.status) ? exchange.status : DSD_TILE_FETCH_ERR_NETWORK;
        resp->http_status = exchange.http_status;
        exchange.error[sizeof(exchange.error) - 1U] = '\0';
        tf_copy_text(resp->error, sizeof(resp->error), exchange.error[0] != '\0' ? exchange.error : "transfer failed");
        if (resp->status == DSD_TILE_FETCH_ERR_TOO_LARGE) {
            tf_keep_headers(resp, &exchange);
        }
    } else {
        tf_finish(resp, &exchange);
    }
    free(exchange.body);
    free(exchange.headers);
    return resp->status;
}

void
dsd_tile_fetch_response_free(dsd_tile_fetch_response* resp) {
    if (resp == NULL) {
        return;
    }
    free(resp->body);
    DSD_MEMSET(resp, 0, sizeof(*resp));
}

const char*
dsd_tile_fetch_status_name(dsd_tile_fetch_status status) {
    static const char* const k_names[] = {
        "ok",          "not modified",  "redirect not followed", "HTTP error",      "network error",
        "cancelled",   "too large",     "bad cache header",      "invalid request", "not available in this build",
        "setup error", "out of memory",
    };
    const int index = (int)status;
    if (index < 0 || (size_t)index >= sizeof(k_names) / sizeof(k_names[0])) {
        return "unknown";
    }
    return k_names[index];
}
