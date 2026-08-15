// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * RadioReference client: transport, status mapping, worker thread, cancellation.
 *
 * One worker thread per client, one request in flight at a time. The
 * serialization is deliberate - RR publishes no rate limit anywhere, so a single
 * outstanding request is the conservative default, and it keeps the
 * zip -> county -> system -> sites -> talkgroups pipeline ordered for the UI.
 */

#include "rr_internal.h"
#include "rr_soap.h"

#include "../curl_common.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/radioreference.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef USE_CURL
#include <curl/curl.h>
#include <curl/system.h>
#endif

#define RR_QUEUE_MAX      128U
#define RR_BODY_CAP_BYTES ((size_t)32U * 1024U * 1024U)
#define RR_RETRY_DELAY_MS 500U

struct dsd_rr_cancel_token {
    atomic_int flag;
};

int
dsd_rr_cancel_requested(const dsd_rr_cancel_token* cancel) {
    if (cancel == NULL) {
        return 0;
    }
    /* Cast away const: the flag is atomic, and the read is the whole point.
     * A plain pointer cast, not one through uintptr_t — the integer round trip
     * reads as an int-to-pointer conversion and pessimizes the optimizer. */
    dsd_rr_cancel_token* token = (dsd_rr_cancel_token*)cancel;
    return atomic_load(&token->flag) != 0 ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Credential hygiene                                                         */
/* ------------------------------------------------------------------------- */

/**
 * @brief Overwrite a buffer in a way the optimizer may not discard.
 *
 * DSD_MEMSET is __builtin_memset and is a dead store here by definition, so a
 * compiler is free to delete it; this tree has no explicit_bzero or memset_s.
 * The volatile pointer is what makes the writes observable.
 *
 * @param p Buffer to clear.
 * @param n Length in bytes.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void
rr_secure_zero(void* p, size_t n) {
    volatile unsigned char* v = (volatile unsigned char*)p;
    while (n-- > 0U) {
        *v = 0;
        v++;
    }
}

/* ------------------------------------------------------------------------- */
/* Method table                                                               */
/* ------------------------------------------------------------------------- */

typedef enum {
    RR_M_USER_DATA,
    RR_M_ZIPCODE_INFO,
    RR_M_COUNTRIES,
    RR_M_COUNTRY_STATES,
    RR_M_STATE_COUNTIES,
    RR_M_STATE_TRS,
    RR_M_COUNTY_TRS,
    RR_M_TRS_DETAILS,
    RR_M_TRS_SITES,
    RR_M_TRS_TALKGROUPS,
    RR_M_TRS_TALKGROUP_CATS,
    RR_M_SUPPORT_TYPE,
    RR_M_SUPPORT_FLAVOR,
    RR_M_SUPPORT_VOICE
} rr_method;

typedef struct {
    const char* soap_method;
    const char* param_name; /**< NULL when the message takes no int part. */
    rr_shape shape;
    int needs_auth;
    size_t sink_size;
} rr_method_def;

static const rr_method_def k_methods[] = {
    [RR_M_USER_DATA] = {"getUserData", NULL, RR_SHAPE_USER_INFO, 1, sizeof(dsd_rr_user_info)},
    [RR_M_ZIPCODE_INFO] = {"getZipcodeInfo", "zipcode", RR_SHAPE_ZIP_INFO, 1, sizeof(dsd_rr_zip_info)},
    /* getCountryList is the one message with no parts at all, authInfo included. */
    [RR_M_COUNTRIES] = {"getCountryList", NULL, RR_SHAPE_COUNTRY_LIST, 0, sizeof(dsd_rr_country_list)},
    [RR_M_COUNTRY_STATES] = {"getCountryInfo", "coid", RR_SHAPE_STATE_LIST, 1, sizeof(dsd_rr_state_list)},
    [RR_M_STATE_COUNTIES] = {"getStateInfo", "stid", RR_SHAPE_COUNTY_LIST, 1, sizeof(dsd_rr_county_list)},
    [RR_M_STATE_TRS] = {"getStateInfo", "stid", RR_SHAPE_TRS_LIST, 1, sizeof(dsd_rr_trs_list)},
    [RR_M_COUNTY_TRS] = {"getCountyInfo", "ctid", RR_SHAPE_TRS_LIST, 1, sizeof(dsd_rr_trs_list)},
    [RR_M_TRS_DETAILS] = {"getTrsDetails", "sid", RR_SHAPE_TRS_DETAILS, 1, sizeof(dsd_rr_trs_details)},
    [RR_M_TRS_SITES] = {"getTrsSites", "sid", RR_SHAPE_SITE_LIST, 1, sizeof(dsd_rr_site_list)},
    [RR_M_TRS_TALKGROUPS] = {"getTrsTalkgroups", "sid", RR_SHAPE_TALKGROUP_LIST, 1, sizeof(dsd_rr_talkgroup_list)},
    [RR_M_TRS_TALKGROUP_CATS] = {"getTrsTalkgroupCats", "sid", RR_SHAPE_TALKGROUP_CAT_LIST, 1,
                                 sizeof(dsd_rr_talkgroup_cat_list)},
    /* id=0 returns every row; omitting the part is an empty-bodied HTTP 500. */
    [RR_M_SUPPORT_TYPE] = {"getTrsType", "id", RR_SHAPE_SUPPORT_TYPE, 1, sizeof(dsd_rr_support_list)},
    [RR_M_SUPPORT_FLAVOR] = {"getTrsFlavor", "id", RR_SHAPE_SUPPORT_FLAVOR, 1, sizeof(dsd_rr_support_list)},
    [RR_M_SUPPORT_VOICE] = {"getTrsVoice", "id", RR_SHAPE_SUPPORT_VOICE, 1, sizeof(dsd_rr_support_list)},
};

/* ------------------------------------------------------------------------- */
/* Client and job types                                                       */
/* ------------------------------------------------------------------------- */

typedef struct rr_job {
    uint64_t id;
    rr_method method;
    dsd_rr_auth auth;
    long iarg;
    dsd_rr_done_cb cb;
    void* user;
    dsd_rr_cancel_token cancel;
    struct rr_job* next;
} rr_job;

struct dsd_rr_client {
    dsd_rr_client_config config;

    dsd_rr_transport transport;
    int has_transport;

    dsd_mutex_t mutex;
    dsd_cond_t cond;
    dsd_thread_t worker;
    int worker_started;
    int stop_requested;

    rr_job* head;
    rr_job* tail;
    rr_job* running;
    size_t depth;
    uint64_t next_id;

    dsd_rr_support_maps support;
    int support_cached;
};

/* ------------------------------------------------------------------------- */
/* Built-in curl transport                                                    */
/* ------------------------------------------------------------------------- */

#ifdef USE_CURL

static int
rr_xferinfo_cb(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;
    return dsd_rr_cancel_requested((const dsd_rr_cancel_token*)clientp) ? 1 : 0;
}

/**
 * @brief Map a libcurl result to a dsd-neo failure class.
 *
 * The classes are not interchangeable: only NETWORK is retried, CANCELLED must
 * never be, and the write-callback overflow arrives as CURLE_WRITE_ERROR (not
 * CURLE_ABORTED_BY_CALLBACK) and must not trigger a 32 MB re-download.
 *
 * @param code libcurl result.
 * @return The matching dsd_rr_status.
 */
static dsd_rr_status
rr_status_from_curl(CURLcode code) {
    if (code == CURLE_ABORTED_BY_CALLBACK) {
        return DSD_RR_ERR_CANCELLED;
    }
    if (code == CURLE_WRITE_ERROR) {
        /* The body-buffer hard cap: the write callback returned 0. Never
         * retried — a retry would re-download up to the whole 32 MB. */
        return DSD_RR_ERR_PARSE;
    }
    /* Everything else is the retryable NETWORK class. Written as a fallthrough
     * rather than as a case list for CURLE_OPERATION_TIMEDOUT,
     * CURLE_COULDNT_CONNECT, CURLE_COULDNT_RESOLVE_HOST,
     * CURLE_COULDNT_RESOLVE_PROXY, CURLE_RECV_ERROR and CURLE_SEND_ERROR,
     * because an unknown transport failure gets the same treatment and a case
     * list plus an identical default is a branch clone. */
    return DSD_RR_ERR_NETWORK;
}

/**
 * @brief Install the request-specific curl options.
 *
 * CURLOPT_POSTFIELDS does not copy, so the envelope must outlive the transfer,
 * and without CURLOPT_POSTFIELDSIZE libcurl would strlen() it. CURLOPT_WRITEDATA
 * is mandatory here: omitting it sends the body to libcurl's default FILE* sink.
 * CURLOPT_NOPROGRESS defaults to 1, and while it is set libcurl never calls the
 * xferinfo callback - without the explicit 0 cancellation silently never fires.
 *
 * @param curl Easy handle.
 * @param req  Request being performed.
 * @param buf  Response accumulator.
 * @param hdrs Header list.
 */
static void
rr_apply_request_options(CURL* curl, const dsd_rr_request* req, dsd_curl_body_buf* buf, struct curl_slist* hdrs) {
    curl_easy_setopt(curl, CURLOPT_URL, req->config->endpoint_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req->body_len);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dsd_curl_body_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, buf);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, rr_xferinfo_cb);
    /* The callback only reads the token; CURLOPT_XFERINFODATA takes a void*. */
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void*)(dsd_rr_cancel_token*)req->cancel);
    dsd_curl_apply_hardening(curl, req->config->connect_timeout_ms, req->config->total_timeout_ms,
                             "dsd-neo/radioreference");
}

static int
rr_builtin_perform(void* ctx, const dsd_rr_request* req, dsd_rr_response* resp) {
    (void)ctx;
    if (req == NULL || resp == NULL || req->config == NULL) {
        return -1;
    }
    DSD_MEMSET(resp, 0, sizeof(*resp));
    resp->status = DSD_RR_ERR_NETWORK;

    if (dsd_curl_global_ready() != 0) {
        resp->status = DSD_RR_ERR_UNSUPPORTED;
        rr_copy_field(resp->error, sizeof(resp->error), "libcurl global init failed");
        return -1;
    }

    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        rr_copy_field(resp->error, sizeof(resp->error), "libcurl handle allocation failed");
        return -1;
    }

    struct curl_slist* hdrs = curl_slist_append(NULL, "Content-Type: text/xml; charset=utf-8");
    if (hdrs == NULL) {
        curl_easy_cleanup(curl);
        resp->status = DSD_RR_ERR_NOMEM;
        rr_copy_field(resp->error, sizeof(resp->error), "out of memory");
        return -1;
    }

    dsd_curl_body_buf buf;
    if (dsd_curl_body_buf_init(&buf, RR_BODY_CAP_BYTES) != 0) {
        curl_slist_free_all(hdrs);
        curl_easy_cleanup(curl);
        resp->status = DSD_RR_ERR_NOMEM;
        rr_copy_field(resp->error, sizeof(resp->error), "out of memory");
        return -1;
    }

    rr_apply_request_options(curl, req, &buf, hdrs);
    const CURLcode code = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->http_status);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (code != CURLE_OK) {
        dsd_curl_body_buf_free(&buf);
        resp->status = rr_status_from_curl(code);
        if (resp->status == DSD_RR_ERR_PARSE) {
            rr_copy_field(resp->error, sizeof(resp->error), "response too large");
        } else if (resp->status == DSD_RR_ERR_CANCELLED) {
            rr_copy_field(resp->error, sizeof(resp->error), "cancelled");
        } else {
            rr_copy_field(resp->error, sizeof(resp->error), curl_easy_strerror(code));
        }
        return -1;
    }

    resp->body = buf.data;
    resp->body_len = buf.len;
    resp->status = DSD_RR_OK;
    return 0;
}

#else /* !USE_CURL */

static int
rr_builtin_perform(void* ctx, const dsd_rr_request* req, dsd_rr_response* resp) {
    (void)ctx;
    (void)req;
    if (resp == NULL) {
        return -1;
    }
    DSD_MEMSET(resp, 0, sizeof(*resp));
    resp->status = DSD_RR_ERR_UNSUPPORTED;
    rr_copy_field(resp->error, sizeof(resp->error), "built without libcurl: no HTTP transport");
    return -1;
}

#endif /* USE_CURL */

/* ------------------------------------------------------------------------- */
/* Request execution                                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief Fill the request parts for one method.
 *
 * Never drops a declared part: getTrsTalkgroups sends explicit zero filters
 * because omitting them returns an empty-bodied HTTP 500 rather than a fault.
 *
 * @param method Method to build for.
 * @param iarg   Integer argument (sid/stid/ctid/coid/zip/id).
 * @param params Destination array, at least four entries.
 * @return Number of parts written.
 */
static size_t
rr_build_params(rr_method method, long iarg, rr_soap_param* params) {
    const rr_method_def* def = &k_methods[method];
    if (def->param_name == NULL) {
        return 0;
    }

    params[0].name = def->param_name;
    params[0].kind = RR_PARAM_INT;
    params[0].ivalue = iarg;
    params[0].svalue = NULL;

    if (method != RR_M_TRS_TALKGROUPS) {
        return 1;
    }

    static const char* const k_filters[] = {"tgCid", "tgTag", "tgDec"};
    for (size_t i = 0; i < 3U; i++) {
        params[i + 1U].name = k_filters[i];
        params[i + 1U].kind = RR_PARAM_INT;
        params[i + 1U].ivalue = 0;
        params[i + 1U].svalue = NULL;
    }
    return 4;
}

/**
 * @brief Decide the final status once the transfer itself succeeded.
 *
 * The rule that matters: RR returns faults as HTTP 500 with a text/xml body, so a
 * non-empty body is ALWAYS parsed first, whatever the HTTP status. Only a body
 * that carries neither a fault nor a result lets a non-2xx status decide.
 *
 * @param resp    Completed response.
 * @param shape   Expected response shape.
 * @param sink    Destination struct.
 * @param err     Receives failure detail.
 * @return 0 on success, -1 on failure.
 */
static int
rr_classify_response(const dsd_rr_response* resp, rr_shape shape, void* sink, dsd_rr_error* err) {
    err->http_status = resp->http_status;

    if (resp->body == NULL || resp->body_len == 0U) {
        err->status = DSD_RR_ERR_HTTP;
        rr_copy_field(err->detail, sizeof(err->detail), "server returned an empty response");
        return -1;
    }

    rr_parse_outcome outcome = RR_PARSE_MALFORMED;
    const int rc = rr_soap_parse(resp->body, resp->body_len, shape, sink, err, &outcome);
    err->http_status = resp->http_status;
    if (rc == 0) {
        err->status = DSD_RR_OK;
        return 0;
    }

    if (outcome == RR_PARSE_NO_RESULT && (resp->http_status < 200 || resp->http_status >= 300)) {
        err->status = DSD_RR_ERR_HTTP;
        rr_copy_field(err->detail, sizeof(err->detail), "server returned an unexpected response");
    }
    return -1;
}

/**
 * @brief Perform one request attempt, without retry.
 *
 * @param client Client.
 * @param body   Envelope bytes.
 * @param len    Envelope length.
 * @param cancel Cancellation token, or NULL.
 * @param shape  Expected response shape.
 * @param sink   Destination struct.
 * @param err    Receives failure detail.
 * @return 0 on success, -1 on failure.
 */
static int
rr_attempt(dsd_rr_client* client, const char* body, size_t len, const dsd_rr_cancel_token* cancel, rr_shape shape,
           void* sink, dsd_rr_error* err) {
    dsd_rr_request req;
    DSD_MEMSET(&req, 0, sizeof(req));
    req.config = &client->config;
    req.body = body;
    req.body_len = len;
    req.cancel = cancel;

    dsd_rr_response resp;
    DSD_MEMSET(&resp, 0, sizeof(resp));

    const int rc = client->transport.perform(client->transport.ctx, &req, &resp);
    if (rc != 0) {
        err->status = (resp.status != DSD_RR_OK) ? resp.status : DSD_RR_ERR_NETWORK;
        err->http_status = resp.http_status;
        rr_copy_field(err->detail, sizeof(err->detail), resp.error);
        free(resp.body);
        return -1;
    }

    const int classified = rr_classify_response(&resp, shape, sink, err);
    free(resp.body);
    return classified;
}

/**
 * @brief Build, send and decode one request, retrying transient failures once.
 *
 * @param client Client.
 * @param auth   Credentials, or NULL for the one method that takes none.
 * @param method Method to call.
 * @param iarg   Integer argument.
 * @param sink   Destination struct, zeroed by the caller.
 * @param err    Receives failure detail.
 * @param cancel Cancellation token, or NULL.
 * @return 0 on success, -1 on failure.
 */
static int
rr_execute(dsd_rr_client* client, const dsd_rr_auth* auth, rr_method method, long iarg, void* sink, dsd_rr_error* err,
           const dsd_rr_cancel_token* cancel) {
    const rr_method_def* def = &k_methods[method];
    rr_soap_param params[4];
    DSD_MEMSET(params, 0, sizeof(params));
    const size_t nparams = rr_build_params(method, iarg, params);

    char* body = NULL;
    size_t len = 0;
    if (rr_soap_build_request(def->soap_method, params, nparams, def->needs_auth ? auth : NULL, &body, &len) != 0) {
        err->status = DSD_RR_ERR_NOMEM;
        rr_copy_field(err->detail, sizeof(err->detail), "could not build the request");
        return -1;
    }

    int attempts = 1;
    if (client->config.transient_retries > 0) {
        attempts += client->config.transient_retries;
    }

    int rc = -1;
    for (int i = 0; i < attempts; i++) {
        if (dsd_rr_cancel_requested(cancel)) {
            err->status = DSD_RR_ERR_CANCELLED;
            rr_copy_field(err->detail, sizeof(err->detail), "cancelled");
            break;
        }
        rc = rr_attempt(client, body, len, cancel, def->shape, sink, err);
        /* Only the NETWORK class is transient; cancellation and an oversized
         * response must never be replayed. */
        if (rc == 0 || err->status != DSD_RR_ERR_NETWORK || i + 1 >= attempts) {
            break;
        }
        dsd_sleep_ms(RR_RETRY_DELAY_MS);
    }

    /* The envelope carries the password in cleartext. */
    rr_secure_zero(body, len);
    free(body);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Subscription date                                                          */
/* ------------------------------------------------------------------------- */

/**
 * @brief Days since 1970-01-01 for a proleptic Gregorian date.
 *
 * @param y Year.
 * @param m Month, 1-12.
 * @param d Day, 1-31.
 * @return Day number, negative before the epoch.
 */
static long long
rr_days_from_civil(long long y, unsigned m, unsigned d) {
    y -= (m <= 2U) ? 1 : 0;
    const long long era = ((y >= 0) ? y : (y - 399)) / 400;
    const unsigned yoe = (unsigned)(y - (era * 400));
    const unsigned doy = (((153U * (m + ((m > 2U) ? -3U : 9U))) + 2U) / 5U) + d - 1U;
    const unsigned doe = (yoe * 365U) + (yoe / 4U) - (yoe / 100U) + doy;
    return (era * 146097LL) + (long long)doe - 719468LL;
}

/**
 * @brief Consume one unsigned field of a hyphen-separated date.
 *
 * @param p     Address of the cursor, advanced past the digits.
 * @param out   Receives the field value.
 * @return 0 on success, -1 when no digit was present.
 */
static int
rr_take_date_field(const char** p, unsigned* out) {
    if (**p < '0' || **p > '9') {
        return -1;
    }
    unsigned value = 0;
    size_t digits = 0;
    while (**p >= '0' && **p <= '9' && digits < 4U) {
        value = (value * 10U) + (unsigned)(**p - '0');
        (*p)++;
        digits++;
    }
    *out = value;
    return 0;
}

/**
 * @brief Parse MM-DD-YYYY into its three fields.
 *
 * @param text  Candidate date text.
 * @param parts Receives month, day, year.
 * @return 0 when the whole string is a plausible date, -1 otherwise.
 */
static int
rr_parse_mmddyyyy(const char* text, unsigned* parts) {
    const char* p = text;
    for (size_t i = 0; i < 3U; i++) {
        if (rr_take_date_field(&p, &parts[i]) != 0) {
            return -1;
        }
        if (i < 2U) {
            if (*p != '-') {
                return -1;
            }
            p++;
        }
    }
    if (*p != '\0') {
        return -1;
    }
    if (parts[0] < 1U || parts[0] > 12U || parts[1] < 1U || parts[1] > 31U || parts[2] < 1970U) {
        return -1;
    }
    return 0;
}

int
rr_subscription_expired(const char* sub_expire, long long now_epoch_seconds) {
    if (sub_expire == NULL) {
        return 0;
    }

    /*
     * An unparseable value is VALID, not expired: RR answers "Never - Feed
     * Provider" and "Never - Admin" for those accounts, and locking them out
     * would be worse than trusting them.
     */
    unsigned parts[3] = {0, 0, 0};
    if (rr_parse_mmddyyyy(sub_expire, parts) != 0) {
        return 0;
    }

    const long long expire_day = rr_days_from_civil((long long)parts[2], parts[0], parts[1]);
    const long long today = now_epoch_seconds / 86400LL;
    /* Two days of slack absorbs time-zone skew between the client and the server. */
    return (expire_day <= (today - 2LL)) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Worker thread                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Allocate the heap sink an async result is delivered in.
 *
 * @param method Method whose result shape is wanted.
 * @return Zeroed sink, or NULL on allocation failure.
 */
static void*
rr_alloc_sink(rr_method method) {
    return calloc(1, k_methods[method].sink_size);
}

/**
 * @brief Release a sink and everything it owns.
 *
 * @param method Method the sink belongs to.
 * @param sink   Sink to release; NULL is a no-op.
 */
static void
rr_free_sink(rr_method method, void* sink) {
    if (sink == NULL) {
        return;
    }
    switch (k_methods[method].shape) {
        case RR_SHAPE_COUNTRY_LIST: dsd_rr_country_list_free((dsd_rr_country_list*)sink); break;
        case RR_SHAPE_STATE_LIST: dsd_rr_state_list_free((dsd_rr_state_list*)sink); break;
        case RR_SHAPE_COUNTY_LIST: dsd_rr_county_list_free((dsd_rr_county_list*)sink); break;
        case RR_SHAPE_TRS_LIST: dsd_rr_trs_list_free((dsd_rr_trs_list*)sink); break;
        case RR_SHAPE_TRS_DETAILS: dsd_rr_trs_details_free((dsd_rr_trs_details*)sink); break;
        case RR_SHAPE_SITE_LIST: dsd_rr_site_list_free((dsd_rr_site_list*)sink); break;
        case RR_SHAPE_TALKGROUP_LIST: dsd_rr_talkgroup_list_free((dsd_rr_talkgroup_list*)sink); break;
        case RR_SHAPE_TALKGROUP_CAT_LIST: dsd_rr_talkgroup_cat_list_free((dsd_rr_talkgroup_cat_list*)sink); break;
        case RR_SHAPE_SUPPORT_TYPE:
        case RR_SHAPE_SUPPORT_FLAVOR:
        case RR_SHAPE_SUPPORT_VOICE: dsd_rr_support_list_free((dsd_rr_support_list*)sink); break;
        default: break;
    }
    free(sink);
}

/**
 * @brief Run one queued job and hand the result to its callback.
 *
 * The callback runs on this worker thread; a GUI consumer must marshal.
 *
 * @param client Client.
 * @param job    Job to run; freed here.
 */
static void
rr_run_job(dsd_rr_client* client, rr_job* job) {
    dsd_rr_error err;
    DSD_MEMSET(&err, 0, sizeof(err));

    void* sink = rr_alloc_sink(job->method);
    int rc = -1;
    if (sink == NULL) {
        err.status = DSD_RR_ERR_NOMEM;
        rr_copy_field(err.detail, sizeof(err.detail), "out of memory");
    } else {
        rc = rr_execute(client, &job->auth, job->method, job->iarg, sink, &err, &job->cancel);
    }

    if (job->cb != NULL) {
        job->cb(job->user, (rc == 0) ? DSD_RR_OK : err.status, &err, (rc == 0) ? sink : NULL);
    }
    if (rc != 0 || job->cb == NULL) {
        rr_free_sink(job->method, sink);
    }

    rr_secure_zero(&job->auth, sizeof(job->auth));
    free(job);
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    rr_client_worker_thread(void* arg) {
    dsd_rr_client* client = (dsd_rr_client*)arg;

    for (;;) {
        dsd_mutex_lock(&client->mutex);
        while (client->head == NULL && !client->stop_requested) {
            dsd_cond_wait(&client->cond, &client->mutex);
        }
        if (client->head == NULL && client->stop_requested) {
            dsd_mutex_unlock(&client->mutex);
            break;
        }

        rr_job* job = client->head;
        client->head = job->next;
        if (client->head == NULL) {
            client->tail = NULL;
        }
        if (client->depth > 0U) {
            client->depth--;
        }
        client->running = job;
        dsd_mutex_unlock(&client->mutex);

        rr_run_job(client, job);

        dsd_mutex_lock(&client->mutex);
        client->running = NULL;
        dsd_mutex_unlock(&client->mutex);
    }

    DSD_THREAD_RETURN;
}

/**
 * @brief Free every queued job. Caller holds the mutex.
 *
 * @param client Client.
 */
static void
rr_clear_queue_locked(dsd_rr_client* client) {
    while (client->head != NULL) {
        rr_job* next = client->head->next;
        rr_secure_zero(&client->head->auth, sizeof(client->head->auth));
        free(client->head);
        client->head = next;
    }
    client->tail = NULL;
    client->depth = 0;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Apply defaults to a caller-supplied configuration.
 *
 * @param out    Destination.
 * @param config Caller configuration, or NULL.
 */
static void
rr_config_init(dsd_rr_client_config* out, const dsd_rr_client_config* config) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (config != NULL) {
        *out = *config;
    }
    if (out->endpoint_url[0] == '\0') {
        rr_copy_field(out->endpoint_url, sizeof(out->endpoint_url), "https://api.radioreference.com/soap2/");
    }
    if (out->connect_timeout_ms <= 0) {
        out->connect_timeout_ms = 10000;
    }
    if (out->total_timeout_ms <= 0) {
        out->total_timeout_ms = 30000;
    }
    if (config == NULL) {
        out->transient_retries = 1;
    }
    if (out->transient_retries < 0) {
        out->transient_retries = 0;
    }
}

dsd_rr_client*
dsd_rr_client_create(const dsd_rr_client_config* config) {
    dsd_rr_client* client = (dsd_rr_client*)calloc(1, sizeof(*client));
    if (client == NULL) {
        return NULL;
    }

    rr_config_init(&client->config, config);
    client->transport.perform = rr_builtin_perform;
    client->transport.ctx = NULL;
    client->next_id = 1;

    if (dsd_mutex_init(&client->mutex) != 0) {
        free(client);
        return NULL;
    }
    if (dsd_cond_init(&client->cond) != 0) {
        (void)dsd_mutex_destroy(&client->mutex);
        free(client);
        return NULL;
    }
    if (dsd_thread_create(&client->worker, rr_client_worker_thread, client) != 0) {
        (void)dsd_cond_destroy(&client->cond);
        (void)dsd_mutex_destroy(&client->mutex);
        free(client);
        return NULL;
    }
    client->worker_started = 1;
    return client;
}

void
dsd_rr_client_destroy(dsd_rr_client* client) {
    if (client == NULL) {
        return;
    }

    dsd_mutex_lock(&client->mutex);
    client->stop_requested = 1;
    for (rr_job* job = client->head; job != NULL; job = job->next) {
        atomic_store(&job->cancel.flag, 1);
    }
    if (client->running != NULL) {
        /*
         * The in-flight transfer unwinds on the next libcurl progress tick, which
         * fires at least once a second, so this join can block for about a second.
         * That is deliberate and differs from dsd_rdio_upload_shutdown, which lets
         * its in-flight upload finish.
         */
        atomic_store(&client->running->cancel.flag, 1);
    }
    dsd_cond_broadcast(&client->cond);
    dsd_mutex_unlock(&client->mutex);

    if (client->worker_started) {
        if (dsd_thread_join(client->worker) != 0) {
            LOG_ERROR("RadioReference: failed to join the request worker during shutdown\n");
        }
    }

    dsd_mutex_lock(&client->mutex);
    rr_clear_queue_locked(client);
    dsd_mutex_unlock(&client->mutex);

    (void)dsd_cond_destroy(&client->cond);
    (void)dsd_mutex_destroy(&client->mutex);
    dsd_rr_support_maps_free(&client->support);
    free(client);
}

void
dsd_rr_client_set_transport(dsd_rr_client* client, const dsd_rr_transport* transport) {
    if (client == NULL) {
        return;
    }
    dsd_mutex_lock(&client->mutex);
    if (transport != NULL && transport->perform != NULL) {
        client->transport = *transport;
        client->has_transport = 1;
    } else {
        client->transport.perform = rr_builtin_perform;
        client->transport.ctx = NULL;
        client->has_transport = 0;
    }
    dsd_mutex_unlock(&client->mutex);
}

/* ------------------------------------------------------------------------- */
/* Blocking getters                                                           */
/* ------------------------------------------------------------------------- */

/**
 * @brief Shared entry for every blocking getter.
 *
 * @param client Client.
 * @param auth   Credentials.
 * @param method Method to call.
 * @param iarg   Integer argument.
 * @param out    Destination struct, zeroed here.
 * @param err    Receives failure detail; may be NULL.
 * @return 0 on success, -1 on failure.
 */
static int
rr_call(dsd_rr_client* client, const dsd_rr_auth* auth, rr_method method, long iarg, void* out, dsd_rr_error* err) {
    dsd_rr_error local;
    DSD_MEMSET(&local, 0, sizeof(local));
    dsd_rr_error* target = (err != NULL) ? err : &local;
    DSD_MEMSET(target, 0, sizeof(*target));

    if (client == NULL || out == NULL || (k_methods[method].needs_auth && auth == NULL)) {
        target->status = DSD_RR_ERR_INVALID_ARG;
        rr_copy_field(target->detail, sizeof(target->detail), "invalid argument");
        return -1;
    }

    DSD_MEMSET(out, 0, k_methods[method].sink_size);
    return rr_execute(client, auth, method, iarg, out, target, NULL);
}

int
dsd_rr_get_user_data(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_user_info* out, dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_USER_DATA, 0, out, err);
}

int
dsd_rr_get_zipcode_info(dsd_rr_client* client, const dsd_rr_auth* auth, const char* zip, dsd_rr_zip_info* out,
                        dsd_rr_error* err) {
    long value = 0;
    /* A leading-zero ZIP resolves correctly as a plain int: 02134 -> 2134. */
    if (rr_parse_long_strict(zip, &value) != 0 || value <= 0 || value > 99999) {
        if (err != NULL) {
            DSD_MEMSET(err, 0, sizeof(*err));
            err->status = DSD_RR_ERR_INVALID_ARG;
            rr_copy_field(err->detail, sizeof(err->detail), "that is not a valid ZIP code");
        }
        return -1;
    }
    return rr_call(client, auth, RR_M_ZIPCODE_INFO, value, out, err);
}

int
dsd_rr_get_countries(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_country_list* out, dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_COUNTRIES, 0, out, err);
}

int
dsd_rr_get_country_states(dsd_rr_client* client, const dsd_rr_auth* auth, int coid, dsd_rr_state_list* out,
                          dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_COUNTRY_STATES, coid, out, err);
}

int
dsd_rr_get_state_counties(dsd_rr_client* client, const dsd_rr_auth* auth, int stid, dsd_rr_county_list* out,
                          dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_STATE_COUNTIES, stid, out, err);
}

int
dsd_rr_get_state_trs(dsd_rr_client* client, const dsd_rr_auth* auth, int stid, dsd_rr_trs_list* out,
                     dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_STATE_TRS, stid, out, err);
}

int
dsd_rr_get_county_trs(dsd_rr_client* client, const dsd_rr_auth* auth, int ctid, dsd_rr_trs_list* out,
                      dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_COUNTY_TRS, ctid, out, err);
}

int
dsd_rr_get_trs_details(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_trs_details* out,
                       dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_TRS_DETAILS, sid, out, err);
}

int
dsd_rr_get_trs_sites(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_site_list* out,
                     dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_TRS_SITES, sid, out, err);
}

int
dsd_rr_get_trs_talkgroups(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_talkgroup_list* out,
                          dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_TRS_TALKGROUPS, sid, out, err);
}

int
dsd_rr_get_trs_talkgroup_cats(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_talkgroup_cat_list* out,
                              dsd_rr_error* err) {
    return rr_call(client, auth, RR_M_TRS_TALKGROUP_CATS, sid, out, err);
}

int
dsd_rr_get_support_maps(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_support_maps* out, dsd_rr_error* err) {
    if (client == NULL || out == NULL) {
        if (err != NULL) {
            DSD_MEMSET(err, 0, sizeof(*err));
            err->status = DSD_RR_ERR_INVALID_ARG;
        }
        return -1;
    }
    if (client->support_cached) {
        *out = client->support;
        return 0;
    }

    dsd_rr_support_maps maps;
    DSD_MEMSET(&maps, 0, sizeof(maps));
    if (rr_call(client, auth, RR_M_SUPPORT_TYPE, 0, &maps.types, err) != 0
        || rr_call(client, auth, RR_M_SUPPORT_FLAVOR, 0, &maps.flavors, err) != 0
        || rr_call(client, auth, RR_M_SUPPORT_VOICE, 0, &maps.voices, err) != 0) {
        dsd_rr_support_maps_free(&maps);
        return -1;
    }

    /* Cached per client instance: RR adds rows over time, so never bake a table,
     * but re-fetching three lists on every classification would be wasteful. */
    client->support = maps;
    client->support_cached = 1;
    *out = maps;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Async API                                                                  */
/* ------------------------------------------------------------------------- */

/**
 * @brief Queue one job for the worker.
 *
 * @param client Client.
 * @param auth   Credentials, copied into the job.
 * @param method Method to call.
 * @param iarg   Integer argument.
 * @param cb     Completion callback, invoked on the worker thread.
 * @param user   Opaque callback argument.
 * @return Request id, or 0 when the job could not be queued.
 */
static uint64_t
rr_submit(dsd_rr_client* client, const dsd_rr_auth* auth, rr_method method, long iarg, dsd_rr_done_cb cb, void* user) {
    if (client == NULL || (k_methods[method].needs_auth && auth == NULL)) {
        return 0;
    }

    rr_job* job = (rr_job*)calloc(1, sizeof(*job));
    if (job == NULL) {
        return 0;
    }
    job->method = method;
    job->iarg = iarg;
    job->cb = cb;
    job->user = user;
    if (auth != NULL) {
        job->auth = *auth;
    }

    dsd_mutex_lock(&client->mutex);
    if (client->stop_requested || client->depth >= RR_QUEUE_MAX) {
        dsd_mutex_unlock(&client->mutex);
        rr_secure_zero(&job->auth, sizeof(job->auth));
        free(job);
        return 0;
    }

    job->id = client->next_id;
    client->next_id++;
    if (client->tail != NULL) {
        client->tail->next = job;
    } else {
        client->head = job;
    }
    client->tail = job;
    client->depth++;
    const uint64_t id = job->id;
    dsd_cond_signal(&client->cond);
    dsd_mutex_unlock(&client->mutex);
    return id;
}

int
dsd_rr_cancel(dsd_rr_client* client, uint64_t request_id) {
    if (client == NULL || request_id == 0U) {
        return -1;
    }

    int found = 0;
    dsd_mutex_lock(&client->mutex);
    if (client->running != NULL && client->running->id == request_id) {
        atomic_store(&client->running->cancel.flag, 1);
        found = 1;
    }
    for (rr_job* job = client->head; job != NULL; job = job->next) {
        if (job->id == request_id) {
            atomic_store(&job->cancel.flag, 1);
            found = 1;
        }
    }
    dsd_mutex_unlock(&client->mutex);
    return found ? 0 : -1;
}

/* cppcheck-suppress-begin funcArgNamesDifferentUnnamed
 *
 * Every one of these is declared in radioreference.h with the same parameter
 * names it is defined with; cppcheck reports the declaration as unnamed
 * whatever the header actually says (verified by renaming it and watching the
 * message not change). The common factor is the dsd_rr_done_cb function-pointer
 * parameter. Scoped to this block and to that one check id. */
uint64_t
dsd_rr_fetch_user_data(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_USER_DATA, 0, cb, user);
}

uint64_t
dsd_rr_fetch_zipcode_info(dsd_rr_client* client, const dsd_rr_auth* auth, const char* zip, dsd_rr_done_cb cb,
                          void* user) {
    long value = 0;
    if (rr_parse_long_strict(zip, &value) != 0 || value <= 0 || value > 99999) {
        return 0;
    }
    return rr_submit(client, auth, RR_M_ZIPCODE_INFO, value, cb, user);
}

uint64_t
dsd_rr_fetch_countries(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_COUNTRIES, 0, cb, user);
}

uint64_t
dsd_rr_fetch_country_states(dsd_rr_client* client, const dsd_rr_auth* auth, int coid, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_COUNTRY_STATES, coid, cb, user);
}

uint64_t
dsd_rr_fetch_state_counties(dsd_rr_client* client, const dsd_rr_auth* auth, int stid, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_STATE_COUNTIES, stid, cb, user);
}

uint64_t
dsd_rr_fetch_state_trs(dsd_rr_client* client, const dsd_rr_auth* auth, int stid, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_STATE_TRS, stid, cb, user);
}

uint64_t
dsd_rr_fetch_county_trs(dsd_rr_client* client, const dsd_rr_auth* auth, int ctid, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_COUNTY_TRS, ctid, cb, user);
}

uint64_t
dsd_rr_fetch_trs_details(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_TRS_DETAILS, sid, cb, user);
}

uint64_t
dsd_rr_fetch_trs_sites(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_TRS_SITES, sid, cb, user);
}

uint64_t
dsd_rr_fetch_trs_talkgroups(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_done_cb cb, void* user) {
    return rr_submit(client, auth, RR_M_TRS_TALKGROUPS, sid, cb, user);
}

uint64_t
dsd_rr_fetch_trs_talkgroup_cats(dsd_rr_client* client, const dsd_rr_auth* auth, int sid, dsd_rr_done_cb cb,
                                void* user) {
    return rr_submit(client, auth, RR_M_TRS_TALKGROUP_CATS, sid, cb, user);
}

uint64_t
dsd_rr_fetch_support_maps(dsd_rr_client* client, const dsd_rr_auth* auth, dsd_rr_done_cb cb, void* user) {
    /* The three support lists are fetched individually; the type list is the one
     * the UI blocks on, and the Qt model asks for the rest through the blocking
     * getter once it has a worker-free moment. */
    return rr_submit(client, auth, RR_M_SUPPORT_TYPE, 0, cb, user);
}

/* cppcheck-suppress-end funcArgNamesDifferentUnnamed */
