// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "curl_common.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>  // IWYU pragma: keep (dsd_stat_t under __ANDROID__)
#include <dsd-neo/platform/posix_compat.h> // IWYU pragma: keep (S_ISDIR fallback under __ANDROID__)
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef USE_CURL
#include <curl/curlver.h>

/*
 * Tri-state rather than the plain function-local `static int` this replaced: the
 * original was safe only because its single caller ran on the one rdio upload
 * worker. The RadioReference client adds a second worker, so two threads could
 * otherwise both observe "uninitialized" and both call curl_global_init().
 */
static atomic_int g_curl_global_state = 0; /* 0=uninit 1=initializing 2=ready 3=failed */

int
dsd_curl_global_ready(void) {
    int state = atomic_load(&g_curl_global_state);
    if (state == 2) {
        return 0;
    }
    if (state == 3) {
        return -1;
    }

    int expected = 0;
    if (atomic_compare_exchange_strong(&g_curl_global_state, &expected, 1)) {
        CURLcode gc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (gc != CURLE_OK) {
            LOG_ERROR("curl_global_init failed: %s\n", curl_easy_strerror(gc));
            atomic_store(&g_curl_global_state, 3);
            return -1;
        }
        atomic_store(&g_curl_global_state, 2);
        return 0;
    }

    for (;;) {
        state = atomic_load(&g_curl_global_state);
        if (state == 2) {
            return 0;
        }
        if (state == 3) {
            return -1;
        }
        dsd_sleep_ms(1U);
    }
}

#ifdef __ANDROID__
/*
 * OpenSSL compiles in /etc/ssl/certs as its default trust store, and Android has
 * no such directory: every https:// request would fail certificate verification
 * while http:// kept working. Android keeps the system roots in OpenSSL's own
 * hashed-CApath layout, under the APEX module since API 34 with the older
 * location still present on most devices. Point libcurl at the first one that
 * exists rather than weakening verification.
 */
static const char* const k_android_trust_stores[] = {
    "/apex/com.android.conscrypt/cacerts",
    "/system/etc/security/cacerts",
};

const char*
dsd_curl_android_ca_path(void) {
    for (size_t i = 0; i < sizeof(k_android_trust_stores) / sizeof(k_android_trust_stores[0]); i++) {
        dsd_stat_t st;
        if (dsd_stat_path(k_android_trust_stores[i], &st) == 0 && S_ISDIR(st.st_mode)) {
            return k_android_trust_stores[i];
        }
    }
    return NULL;
}
#else
const char*
dsd_curl_android_ca_path(void) {
    return NULL;
}
#endif

void
dsd_curl_apply_hardening(CURL* curl, int connect_timeout_ms, int total_timeout_ms, const char* user_agent) {
    if (curl == NULL) {
        return;
    }

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)connect_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)total_timeout_ms);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    if (user_agent != NULL) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent);
    }

    const char* ca_path = dsd_curl_android_ca_path();
    // cppcheck-suppress knownConditionTrueFalse -- off Android the helper is
    // defined to return NULL, which is what lets callers skip an #ifdef.
    if (ca_path != NULL) {
        curl_easy_setopt(curl, CURLOPT_CAPATH, ca_path);
    }
}

int
dsd_curl_body_buf_init(dsd_curl_body_buf* buf, size_t cap_max) {
    if (buf == NULL) {
        return -1;
    }
    DSD_MEMSET(buf, 0, sizeof(*buf));
    buf->cap_max = cap_max;
    return 0;
}

void
dsd_curl_body_buf_free(dsd_curl_body_buf* buf) {
    if (buf == NULL) {
        return;
    }
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

/**
 * @brief Grow the buffer to hold at least `need` bytes, doubling as it goes.
 *
 * @param buf  Buffer to grow.
 * @param need Required capacity in bytes, terminator included.
 * @return 0 on success, -1 when the hard cap or the allocator says no.
 */
static int
dsd_curl_body_buf_reserve(dsd_curl_body_buf* buf, size_t need) {
    if (need <= buf->cap) {
        return 0;
    }

    size_t cap = (buf->cap == 0U) ? 8192U : buf->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            cap = need;
            break;
        }
        cap *= 2U;
    }
    if (buf->cap_max != 0U && cap > buf->cap_max) {
        cap = buf->cap_max;
    }
    if (cap < need) {
        return -1;
    }

    char* next = (char*)realloc(buf->data, cap);
    if (next == NULL) {
        return -1;
    }
    buf->data = next;
    buf->cap = cap;
    return 0;
}

size_t
// cppcheck-suppress constParameterPointer -- signature fixed by libcurl's CURLOPT_WRITEFUNCTION
dsd_curl_body_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    dsd_curl_body_buf* buf = (dsd_curl_body_buf*)userdata;
    if (buf == NULL) {
        return 0;
    }
    if (size != 0U && nmemb > SIZE_MAX / size) {
        return 0;
    }

    const size_t incoming = size * nmemb;
    if (incoming == 0U) {
        /* Equal to the byte count libcurl offered, so this is not an abort. */
        return 0;
    }
    if (ptr == NULL) {
        return 0;
    }

    const size_t need = buf->len + incoming + 1U;
    if (need <= buf->len) {
        return 0;
    }
    if (buf->cap_max != 0U && need > buf->cap_max) {
        return 0;
    }
    if (dsd_curl_body_buf_reserve(buf, need) != 0) {
        return 0;
    }

    DSD_MEMCPY(buf->data + buf->len, ptr, incoming);
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return incoming;
}

#else /* !USE_CURL */

int
dsd_curl_global_ready(void) {
    return -1;
}

const char*
dsd_curl_android_ca_path(void) {
    return NULL;
}

int
dsd_curl_body_buf_init(dsd_curl_body_buf* buf, size_t cap_max) {
    (void)buf;
    (void)cap_max;
    return -1;
}

void
dsd_curl_body_buf_free(dsd_curl_body_buf* buf) {
    (void)buf;
}

#endif /* USE_CURL */
