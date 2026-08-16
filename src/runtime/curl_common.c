// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "curl_common.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h> // IWYU pragma: keep (dsd_stat_t, dsd_fopen_existing_regular_file under __ANDROID__)
#include <dsd-neo/platform/posix_compat.h> // IWYU pragma: keep (S_ISDIR fallback under __ANDROID__)
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/log.h>
#include <stdint.h>
#include <stdio.h> // IWYU pragma: keep (FILE, fread, fclose under __ANDROID__)
#include <stdlib.h>
#include <string.h> // IWYU pragma: keep (strstr under __ANDROID__)

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
#include <dirent.h>

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

/*
 * CURLOPT_CAPATH alone does not work here, which cost a device debugging session
 * to establish: the path is set, the directory exists and is readable, no SELinux
 * denial is logged, and the very same store verifies the same chain on a desktop
 * with the same OpenSSL 3.6.3 the APK links - yet in the app process the hashed
 * directory lookup resolves nothing and every https:// request fails with
 * CURLE_PEER_FAILED_VERIFICATION. Not a RadioReference problem: rdio-scanner
 * uploads use this same helper and had the same defect.
 *
 * So the roots are handed to libcurl directly instead. The store is read once
 * into one PEM blob and kept for the life of the process; CURL_BLOB_NOCOPY means
 * libcurl borrows it rather than copying it per handle. CAPATH is still set below
 * as a fallback for the case where this assembly fails.
 *
 * Android's files are a PEM certificate followed by a human-readable text dump,
 * so only the BEGIN..END block of each is taken - a straight concatenation would
 * hand libcurl a bundle padded with fingerprint listings.
 */
#if LIBCURL_VERSION_NUM >= 0x074D00 /* CURLOPT_CAINFO_BLOB landed in 7.77.0 */

#define DSD_CA_FILE_MAX (128U * 1024U)
#define DSD_CA_BLOB_MAX (4U * 1024U * 1024U)

static const char k_pem_begin[] = "-----BEGIN CERTIFICATE-----";
static const char k_pem_end[] = "-----END CERTIFICATE-----";

/** Growable byte buffer for the assembled bundle. */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
} dsd_ca_buf;

/**
 * @brief Append bytes, growing by doubling.
 *
 * @param buf  Destination.
 * @param text Bytes to append.
 * @param n    Length in bytes.
 * @return 0 on success, -1 on allocation failure or when the cap would be passed.
 */
static int
dsd_ca_buf_append(dsd_ca_buf* buf, const char* text, size_t n) {
    if (n == 0U) {
        return 0;
    }
    if (buf->len > DSD_CA_BLOB_MAX - n) {
        return -1;
    }

    const size_t needed = buf->len + n;
    if (needed > buf->cap) {
        size_t next = (buf->cap == 0U) ? 65536U : buf->cap;
        while (next < needed) {
            next *= 2U;
        }
        char* grown = (char*)realloc(buf->data, next);
        if (grown == NULL) {
            return -1;
        }
        buf->data = grown;
        buf->cap = next;
    }

    DSD_MEMCPY(buf->data + buf->len, text, n);
    buf->len += n;
    return 0;
}

/**
 * @brief Append one store file's PEM block, ignoring anything that is not one.
 *
 * @param buf  Destination bundle.
 * @param path Absolute path of the candidate certificate file.
 * @return 0 when a certificate was appended, -1 otherwise. A miss is not fatal:
 *         one unreadable root must not cost the whole trust store.
 */
static int
dsd_ca_append_file(dsd_ca_buf* buf, const char* path) {
    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
    if (fp == NULL) {
        return -1;
    }

    char* text = (char*)malloc(DSD_CA_FILE_MAX + 1U);
    if (text == NULL) {
        (void)fclose(fp);
        return -1;
    }
    const size_t got = fread(text, 1U, DSD_CA_FILE_MAX, fp);
    (void)fclose(fp);
    text[got] = '\0';

    int rc = -1;
    const char* begin = strstr(text, k_pem_begin);
    const char* end = (begin != NULL) ? strstr(begin, k_pem_end) : NULL;
    if (end != NULL) {
        const size_t span = (size_t)(end - begin) + sizeof(k_pem_end) - 1U;
        rc = dsd_ca_buf_append(buf, begin, span);
        if (rc == 0) {
            rc = dsd_ca_buf_append(buf, "\n", 1U);
        }
    }

    free(text);
    return rc;
}

/**
 * @brief Read every certificate in the system trust store into @p buf.
 *
 * @param buf Destination bundle.
 * @return How many certificates were appended.
 */
static int
dsd_ca_scan_store(dsd_ca_buf* buf) {
    const char* dir_path = dsd_curl_android_ca_path();
    if (dir_path == NULL) {
        return 0;
    }
    DIR* dir = opendir(dir_path);
    if (dir == NULL) {
        return 0;
    }

    int count = 0;
    const struct dirent* entry = readdir(dir);
    while (entry != NULL) {
        char path[512];
        if (entry->d_name[0] != '.' && DSD_SNPRINTF(path, sizeof(path), "%s/%s", dir_path, entry->d_name) > 0
            && dsd_ca_append_file(buf, path) == 0) {
            count++;
        }
        entry = readdir(dir);
    }
    (void)closedir(dir);
    return count;
}

static atomic_int g_ca_blob_state = 0; /* 0=unbuilt 1=building 2=ready 3=failed */
static char* g_ca_blob = NULL;
static size_t g_ca_blob_len = 0;

/**
 * @brief The system roots as one PEM blob, assembled once.
 *
 * Same atomic tri-state as dsd_curl_global_ready(): two worker threads (rdio's
 * and RadioReference's) can reach this concurrently.
 *
 * @param out_len Receives the blob length.
 * @return Borrowed blob, or NULL when the store could not be read.
 */
static const char*
dsd_curl_android_ca_blob(size_t* out_len) {
    for (;;) {
        const int state = atomic_load(&g_ca_blob_state);
        if (state == 2) {
            *out_len = g_ca_blob_len;
            return g_ca_blob;
        }
        if (state == 3) {
            return NULL;
        }
        if (state == 0) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_ca_blob_state, &expected, 1)) {
                break;
            }
        }
        dsd_sleep_ms(1U);
    }

    dsd_ca_buf buf = {NULL, 0U, 0U};
    const int count = dsd_ca_scan_store(&buf);
    if (count <= 0 || buf.len == 0U) {
        free(buf.data);
        LOG_ERROR("Android trust store yielded no certificates; https will fail\n");
        atomic_store(&g_ca_blob_state, 3);
        return NULL;
    }

    g_ca_blob = buf.data;
    g_ca_blob_len = buf.len;
    atomic_store(&g_ca_blob_state, 2);
    *out_len = g_ca_blob_len;
    return g_ca_blob;
}
#endif /* LIBCURL_VERSION_NUM >= 0x074D00 */
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

#if defined(__ANDROID__) && LIBCURL_VERSION_NUM >= 0x074D00
    size_t ca_blob_len = 0U;
    const char* ca_blob = dsd_curl_android_ca_blob(&ca_blob_len);
    if (ca_blob != NULL && ca_blob_len > 0U) {
        /* NOCOPY: the blob outlives every handle, so libcurl borrows it rather
         * than copying a few hundred kilobytes per request. */
        struct curl_blob blob = {(void*)ca_blob, ca_blob_len, CURL_BLOB_NOCOPY};
        curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    }
#endif
    /* Still set, as the fallback for a build or a device where the blob above
     * could not be assembled. libcurl prefers the blob when both are present. */
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
