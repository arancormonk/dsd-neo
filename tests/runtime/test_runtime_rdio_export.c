// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <errno.h>
#include <limits.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "rdio_export_internal.h"
#include "test_support.h"

#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#else
#endif

static void
remove_empty_dir(const char* path) {
    if (!path) {
        return;
    }
#if DSD_PLATFORM_WIN_NATIVE
    (void)_rmdir(path);
#else
    (void)rmdir(path);
#endif
}

static int
file_exists(const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }
    fclose(fp);
    return 1;
}

static int
write_dummy_wav(const char* path) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }

    unsigned char b = 0;
    for (int i = 0; i < 256; i++) {
        if (fwrite(&b, 1, 1, fp) != 1) {
            DSD_FPRINTF(stderr, "fwrite failed: %s\n", strerror(errno));
            fclose(fp);
            return 1;
        }
        b++;
    }

    if (fclose(fp) != 0) {
        DSD_FPRINTF(stderr, "fclose failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}

static int
write_pcm16_mono_wav(const char* path, int sample_rate, int duration_s) {
    if (!path || sample_rate <= 0 || duration_s <= 0) {
        return 1;
    }

    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }

    uint32_t sample_count = (uint32_t)sample_rate * (uint32_t)duration_s;
    uint32_t data_bytes = sample_count * 2U;
    uint32_t byte_rate = (uint32_t)sample_rate * 2U;
    uint32_t riff_size = 36U + data_bytes;

    unsigned char header[44];
    DSD_MEMCPY(header + 0, "RIFF", 4);
    header[4] = (unsigned char)(riff_size & 0xffU);
    header[5] = (unsigned char)((riff_size >> 8) & 0xffU);
    header[6] = (unsigned char)((riff_size >> 16) & 0xffU);
    header[7] = (unsigned char)((riff_size >> 24) & 0xffU);
    DSD_MEMCPY(header + 8, "WAVE", 4);
    DSD_MEMCPY(header + 12, "fmt ", 4);
    header[16] = 16;
    header[17] = 0;
    header[18] = 0;
    header[19] = 0;
    header[20] = 1;
    header[21] = 0;
    header[22] = 1;
    header[23] = 0;
    header[24] = (unsigned char)(sample_rate & 0xffU);
    header[25] = (unsigned char)((sample_rate >> 8) & 0xffU);
    header[26] = (unsigned char)((sample_rate >> 16) & 0xffU);
    header[27] = (unsigned char)((sample_rate >> 24) & 0xffU);
    header[28] = (unsigned char)(byte_rate & 0xffU);
    header[29] = (unsigned char)((byte_rate >> 8) & 0xffU);
    header[30] = (unsigned char)((byte_rate >> 16) & 0xffU);
    header[31] = (unsigned char)((byte_rate >> 24) & 0xffU);
    header[32] = 2;
    header[33] = 0;
    header[34] = 16;
    header[35] = 0;
    DSD_MEMCPY(header + 36, "data", 4);
    header[40] = (unsigned char)(data_bytes & 0xffU);
    header[41] = (unsigned char)((data_bytes >> 8) & 0xffU);
    header[42] = (unsigned char)((data_bytes >> 16) & 0xffU);
    header[43] = (unsigned char)((data_bytes >> 24) & 0xffU);

    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        DSD_FPRINTF(stderr, "fwrite header failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    unsigned char zeros[1024] = {0};
    uint32_t remaining = data_bytes;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(zeros) ? (size_t)remaining : sizeof(zeros);
        if (fwrite(zeros, 1, chunk, fp) != chunk) {
            DSD_FPRINTF(stderr, "fwrite data failed: %s\n", strerror(errno));
            fclose(fp);
            return 1;
        }
        remaining -= (uint32_t)chunk;
    }

    if (fclose(fp) != 0) {
        DSD_FPRINTF(stderr, "fclose failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}

static int
write_pcm16_mono_wav_with_extra_chunks(const char* path, int sample_rate, int duration_s) {
    if (!path || sample_rate <= 0 || duration_s <= 0) {
        return 1;
    }

    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }

    uint32_t sample_count = (uint32_t)sample_rate * (uint32_t)duration_s;
    uint32_t data_bytes = sample_count * 2U;
    uint32_t byte_rate = (uint32_t)sample_rate * 2U;
    uint32_t riff_size = 50U + data_bytes;

    unsigned char header[12];
    DSD_MEMCPY(header + 0, "RIFF", 4);
    header[4] = (unsigned char)(riff_size & 0xffU);
    header[5] = (unsigned char)((riff_size >> 8) & 0xffU);
    header[6] = (unsigned char)((riff_size >> 16) & 0xffU);
    header[7] = (unsigned char)((riff_size >> 24) & 0xffU);
    DSD_MEMCPY(header + 8, "WAVE", 4);
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        DSD_FPRINTF(stderr, "fwrite RIFF header failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    const unsigned char junk[12] = {'J', 'U', 'N', 'K', 3, 0, 0, 0, 'x', 'y', 'z', 0};
    if (fwrite(junk, 1, sizeof(junk), fp) != sizeof(junk)) {
        DSD_FPRINTF(stderr, "fwrite JUNK chunk failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    unsigned char fmt[26];
    DSD_MEMCPY(fmt + 0, "fmt ", 4);
    fmt[4] = 18;
    fmt[5] = 0;
    fmt[6] = 0;
    fmt[7] = 0;
    fmt[8] = 1;
    fmt[9] = 0;
    fmt[10] = 1;
    fmt[11] = 0;
    fmt[12] = (unsigned char)(sample_rate & 0xffU);
    fmt[13] = (unsigned char)((sample_rate >> 8) & 0xffU);
    fmt[14] = (unsigned char)((sample_rate >> 16) & 0xffU);
    fmt[15] = (unsigned char)((sample_rate >> 24) & 0xffU);
    fmt[16] = (unsigned char)(byte_rate & 0xffU);
    fmt[17] = (unsigned char)((byte_rate >> 8) & 0xffU);
    fmt[18] = (unsigned char)((byte_rate >> 16) & 0xffU);
    fmt[19] = (unsigned char)((byte_rate >> 24) & 0xffU);
    fmt[20] = 2;
    fmt[21] = 0;
    fmt[22] = 16;
    fmt[23] = 0;
    fmt[24] = 0;
    fmt[25] = 0;
    if (fwrite(fmt, 1, sizeof(fmt), fp) != sizeof(fmt)) {
        DSD_FPRINTF(stderr, "fwrite extended fmt chunk failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    unsigned char data_hdr[8];
    DSD_MEMCPY(data_hdr + 0, "data", 4);
    data_hdr[4] = (unsigned char)(data_bytes & 0xffU);
    data_hdr[5] = (unsigned char)((data_bytes >> 8) & 0xffU);
    data_hdr[6] = (unsigned char)((data_bytes >> 16) & 0xffU);
    data_hdr[7] = (unsigned char)((data_bytes >> 24) & 0xffU);
    if (fwrite(data_hdr, 1, sizeof(data_hdr), fp) != sizeof(data_hdr)) {
        DSD_FPRINTF(stderr, "fwrite data header failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    unsigned char zeros[1024] = {0};
    uint32_t remaining = data_bytes;
    while (remaining > 0) {
        size_t chunk = remaining < sizeof(zeros) ? (size_t)remaining : sizeof(zeros);
        if (fwrite(zeros, 1, chunk, fp) != chunk) {
            DSD_FPRINTF(stderr, "fwrite data failed: %s\n", strerror(errno));
            fclose(fp);
            return 1;
        }
        remaining -= (uint32_t)chunk;
    }

    if (fclose(fp) != 0) {
        DSD_FPRINTF(stderr, "fclose failed: %s\n", strerror(errno));
        return 1;
    }

    return 0;
}

static int
write_wav_with_custom_fmt(const char* path, uint32_t sample_rate, uint16_t block_align, uint32_t fmt_size) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }

    unsigned char header[12];
    DSD_MEMCPY(header + 0, "RIFF", 4);
    header[4] = 44;
    header[5] = 0;
    header[6] = 0;
    header[7] = 0;
    DSD_MEMCPY(header + 8, "WAVE", 4);
    if (fwrite(header, 1, sizeof(header), fp) != sizeof(header)) {
        DSD_FPRINTF(stderr, "fwrite RIFF header failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    unsigned char fmt[24];
    DSD_MEMSET(fmt, 0, sizeof(fmt));
    DSD_MEMCPY(fmt + 0, "fmt ", 4);
    fmt[4] = (unsigned char)(fmt_size & 0xffU);
    fmt[5] = (unsigned char)((fmt_size >> 8) & 0xffU);
    fmt[6] = (unsigned char)((fmt_size >> 16) & 0xffU);
    fmt[7] = (unsigned char)((fmt_size >> 24) & 0xffU);
    fmt[8] = 1;
    fmt[10] = 1;
    fmt[12] = (unsigned char)(sample_rate & 0xffU);
    fmt[13] = (unsigned char)((sample_rate >> 8) & 0xffU);
    fmt[14] = (unsigned char)((sample_rate >> 16) & 0xffU);
    fmt[15] = (unsigned char)((sample_rate >> 24) & 0xffU);
    fmt[20] = (unsigned char)(block_align & 0xffU);
    fmt[21] = (unsigned char)((block_align >> 8) & 0xffU);
    fmt[22] = 16;
    size_t fmt_payload = fmt_size < 16U ? (size_t)fmt_size : 16U;
    if (fwrite(fmt, 1, 8U + fmt_payload, fp) != 8U + fmt_payload) {
        DSD_FPRINTF(stderr, "fwrite custom fmt failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    const unsigned char data_hdr[8] = {'d', 'a', 't', 'a', 0x80, 0x3e, 0, 0};
    if (fwrite(data_hdr, 1, sizeof(data_hdr), fp) != sizeof(data_hdr)) {
        DSD_FPRINTF(stderr, "fwrite custom data header failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    if (fclose(fp) != 0) {
        DSD_FPRINTF(stderr, "fclose failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int
write_bytes_file(const char* path, const unsigned char* data, size_t len) {
    FILE* fp = dsd_fopen_private(path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }

    if (len > 0 && fwrite(data, 1, len, fp) != len) {
        DSD_FPRINTF(stderr, "fwrite bytes failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    }

    if (fclose(fp) != 0) {
        DSD_FPRINTF(stderr, "fclose failed: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}

static int
read_file(const char* path, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return 1;
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return 1;
    }

    size_t n = fread(out, 1, out_size - 1, fp);
    if (ferror(fp)) {
        fclose(fp);
        return 1;
    }

    out[n] = '\0';
    fclose(fp);
    return 0;
}

// The sidecar's start_time and stop_time are exactly @p start and @p stop. Returns non-zero, after printing the body,
// when either differs.
static int
expect_sidecar_times(const char* label, const char* body, long long start, long long stop) {
    char want_start[64];
    char want_stop[64];
    DSD_SNPRINTF(want_start, sizeof(want_start), "\"start_time\": %lld,", start);
    DSD_SNPRINTF(want_stop, sizeof(want_stop), "\"stop_time\": %lld,", stop);
    if (!body || !strstr(body, want_start) || !strstr(body, want_stop)) {
        DSD_FPRINTF(stderr, "%s: sidecar should carry %s %s\n%s\n", label, want_start, want_stop, body ? body : "");
        return 1;
    }
    return 0;
}

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
#define RDIO_TEST_REQUESTS_MAX 4

typedef struct {
    dsd_socket_t listen_sock;
    dsd_thread_t thread;
    int rc;
    int no_request_expected;
    int saw_request;
    unsigned int accept_timeout_ms;
    // Requests to answer before the server closes; 0 means one. rc is 0 only when every one arrived complete.
    int requests_expected;
    int requests_done;
    char response[512];
    // The upload's "meta" part (the sidecar JSON), or "" when the request carried none.
    char meta[4096];
    // The upload's "system" part, or "" when the request carried none.
    char system[32];
    // Per request, in arrival order (the first RDIO_TEST_REQUESTS_MAX): the "meta" part, the "audio" part's bytes, and
    // the file names the two parts carry.
    int requests_seen;
    char req_meta[RDIO_TEST_REQUESTS_MAX][1024];
    unsigned char req_audio[RDIO_TEST_REQUESTS_MAX][256];
    size_t req_audio_len[RDIO_TEST_REQUESTS_MAX];
    char req_audio_name[RDIO_TEST_REQUESTS_MAX][256];
    char req_meta_name[RDIO_TEST_REQUESTS_MAX][256];
    // When set, the first request is accepted (first_accepted) and left unread until release_first is set: the worker
    // stays inside that upload meanwhile.
    int hold_first;
    atomic_int first_accepted;
    atomic_int release_first;
} rdio_test_http_server;

// Find @p needle in the first @p hay_len bytes of @p hay. The request is not a string: the WAV in its "audio" part,
// which comes before "meta", holds NUL bytes.
static const char*
rdio_test_find_bytes(const char* hay, size_t hay_len, const char* needle) {
    const size_t needle_len = strlen(needle);
    if (needle_len == 0U || hay_len < needle_len) {
        return NULL;
    }
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

// The body of the request's multipart part named by @p name_attr (`name="meta"`), copied into @p out (at most
// @p out_size bytes, not terminated): its length, or 0 when the request has no such part.
static size_t
rdio_test_capture_part_bytes(const char* request, size_t used, const char* name_attr, unsigned char* out,
                             size_t out_size) {
    const char* part = rdio_test_find_bytes(request, used, name_attr);
    if (!part) {
        return 0;
    }
    const size_t part_off = (size_t)(part - request);
    const char* body = rdio_test_find_bytes(part, used - part_off, "\r\n\r\n");
    if (!body) {
        return 0;
    }
    body += 4;
    const size_t body_off = (size_t)(body - request);
    const char* end = rdio_test_find_bytes(body, used - body_off, "\r\n--");
    if (!end) {
        return 0;
    }
    size_t len = (size_t)(end - body);
    if (len > out_size) {
        len = out_size;
    }
    DSD_MEMCPY(out, body, len);
    return len;
}

// Copy the body of the request's multipart part named by @p name_attr (`name="meta"`) into @p out, as a string.
static void
rdio_test_capture_part(const char* request, size_t used, const char* name_attr, char* out, size_t out_size) {
    const size_t len = rdio_test_capture_part_bytes(request, used, name_attr, (unsigned char*)out, out_size - 1U);
    out[len] = '\0';
}

// The `filename="..."` the request's part named by @p name_attr carries, or "" when it carries none.
static void
rdio_test_capture_part_file_name(const char* request, size_t used, const char* name_attr, char* out, size_t out_size) {
    out[0] = '\0';
    const char* part = rdio_test_find_bytes(request, used, name_attr);
    if (!part) {
        return;
    }
    const size_t part_off = (size_t)(part - request);
    const char* line_end = rdio_test_find_bytes(part, used - part_off, "\r\n");
    const char* name = rdio_test_find_bytes(part, used - part_off, "filename=\"");
    if (!name || !line_end || name > line_end) {
        return;
    }
    name += strlen("filename=\"");
    size_t len = 0;
    while (name + len < line_end && name[len] != '"' && len + 1U < out_size) {
        len++;
    }
    DSD_MEMCPY(out, name, len);
    out[len] = '\0';
}

static int
rdio_test_content_length(const char* headers) {
    const char* p = strstr(headers, "Content-Length:");
    if (!p) {
        return -1;
    }
    p += strlen("Content-Length:");
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    errno = 0;
    char* end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < 0 || v > INT_MAX) {
        return -1;
    }
    return (int)v;
}

// Read one upload request from @p client and answer it. Returns 0 when the request arrived complete.
static int
rdio_test_http_serve_one(rdio_test_http_server* server, dsd_socket_t client) {
    int rc = 1;
    (void)dsd_socket_set_recv_timeout(client, 5000);

    const size_t request_capacity = 65536U;
    char* request = (char*)malloc(request_capacity);
    if (!request) {
        return 1;
    }

    size_t used = 0;
    size_t header_len = 0;
    int content_length = -1;
    while (used < request_capacity - 1U) {
        int n = dsd_socket_recv(client, request + used, request_capacity - 1U - used, 0);
        if (n <= 0) {
            break;
        }
        used += (size_t)n;
        request[used] = '\0';

        if (header_len == 0) {
            const char* header_end = strstr(request, "\r\n\r\n");
            if (header_end) {
                header_len = (size_t)(header_end - request) + 4U;
                content_length = rdio_test_content_length(request);
            }
        }

        if (header_len > 0 && content_length >= 0 && used >= header_len + (size_t)content_length) {
            rc = 0;
            break;
        }
    }
    rdio_test_capture_part(request, used, "name=\"meta\"", server->meta, sizeof(server->meta));
    rdio_test_capture_part(request, used, "name=\"system\"", server->system, sizeof(server->system));
    const int seen = server->requests_seen++;
    if (seen < RDIO_TEST_REQUESTS_MAX) {
        rdio_test_capture_part(request, used, "name=\"meta\"", server->req_meta[seen], sizeof(server->req_meta[seen]));
        server->req_audio_len[seen] = rdio_test_capture_part_bytes(
            request, used, "name=\"audio\"", server->req_audio[seen], sizeof(server->req_audio[seen]));
        rdio_test_capture_part_file_name(request, used, "name=\"audio\"", server->req_audio_name[seen],
                                         sizeof(server->req_audio_name[seen]));
        rdio_test_capture_part_file_name(request, used, "name=\"meta\"", server->req_meta_name[seen],
                                         sizeof(server->req_meta_name[seen]));
    }

    const char default_response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
    const char* response = server->response[0] ? server->response : default_response;
    (void)dsd_socket_send(client, response, strlen(response), 0);
    free(request);
    return rc;
}

// Wait up to @p timeout_ms for a connection on @p listen_sock. Returns select()'s result: > 0 when one is pending.
static int
rdio_test_wait_for_connection(dsd_socket_t listen_sock, unsigned int timeout_ms) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(listen_sock, &readfds);
    struct timeval tv;
    tv.tv_sec = (time_t)(timeout_ms / 1000U);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U);
    return select(listen_sock + 1, &readfds, NULL, NULL, &tv);
}

static DSD_THREAD_RETURN_TYPE
rdio_test_http_server_thread(void* arg) {
    rdio_test_http_server* server = (rdio_test_http_server*)arg;
    server->rc = 1;

    if (server->no_request_expected) {
        if (rdio_test_wait_for_connection(server->listen_sock, server->accept_timeout_ms) <= 0) {
            server->rc = 0;
            (void)dsd_socket_close(server->listen_sock);
            DSD_THREAD_RETURN;
        }
        dsd_socket_t client = dsd_socket_accept(server->listen_sock, NULL, NULL);
        if (client == DSD_INVALID_SOCKET) {
            server->rc = 0;
        } else {
            server->saw_request = 1;
            (void)dsd_socket_close(client);
        }
        (void)dsd_socket_close(server->listen_sock);
        DSD_THREAD_RETURN;
    }

    const int expected = server->requests_expected > 0 ? server->requests_expected : 1;
    for (int i = 0; i < expected; i++) {
        // Bounded, so an upload that never comes fails the test after accept_timeout_ms instead of at CTest's timeout.
        if (rdio_test_wait_for_connection(server->listen_sock, server->accept_timeout_ms) <= 0) {
            break;
        }
        dsd_socket_t client = dsd_socket_accept(server->listen_sock, NULL, NULL);
        if (client == DSD_INVALID_SOCKET) {
            break;
        }
        server->saw_request = 1;
        if (i == 0 && server->hold_first) {
            atomic_store(&server->first_accepted, 1);
            const uint64_t t0 = dsd_realtime_mono_ms();
            while (atomic_load(&server->release_first) == 0 && dsd_realtime_mono_ms() - t0 < 10000U) {
                dsd_sleep_ms(1U);
            }
        }
        if (rdio_test_http_serve_one(server, client) == 0) {
            server->requests_done++;
        }
        (void)dsd_socket_close(client);
    }
    if (server->requests_done == expected) {
        server->rc = 0;
    }
    (void)dsd_socket_close(server->listen_sock);
    DSD_THREAD_RETURN;
}

static int
rdio_test_http_server_start_cfg(rdio_test_http_server* server, char* out_api_url, size_t out_api_url_size,
                                const char* response, int no_request_expected, unsigned int accept_timeout_ms,
                                int requests_expected, int hold_first) {
    if (!server || !out_api_url || out_api_url_size == 0) {
        return 1;
    }

    DSD_MEMSET(server, 0, sizeof(*server));
    server->hold_first = hold_first;
    atomic_store(&server->first_accepted, 0);
    atomic_store(&server->release_first, 0);
    server->listen_sock = DSD_INVALID_SOCKET;
    server->rc = 1;
    server->no_request_expected = no_request_expected;
    server->requests_expected = requests_expected;
    server->accept_timeout_ms = accept_timeout_ms ? accept_timeout_ms : 5000U;
    if (response) {
        DSD_SNPRINTF(server->response, sizeof(server->response), "%s", response);
        server->response[sizeof(server->response) - 1] = '\0';
    }

    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "socket init failed\n");
        return 1;
    }

    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sock == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "socket create failed\n");
        return 1;
    }

    int one = 1;
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, (int)sizeof(one));
    (void)dsd_socket_set_recv_timeout(sock, server->accept_timeout_ms);

    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (dsd_socket_bind(sock, (const struct sockaddr*)&addr, (int)sizeof(addr)) != 0) {
        DSD_FPRINTF(stderr, "socket bind failed\n");
        (void)dsd_socket_close(sock);
        return 1;
    }
    if (dsd_socket_listen(sock, 1) != 0) {
        DSD_FPRINTF(stderr, "socket listen failed\n");
        (void)dsd_socket_close(sock);
        return 1;
    }

    socklen_t addr_len = (socklen_t)sizeof(addr);
    if (getsockname(sock, (struct sockaddr*)&addr, &addr_len) != 0) {
        DSD_FPRINTF(stderr, "getsockname failed\n");
        (void)dsd_socket_close(sock);
        return 1;
    }

    int n = DSD_SNPRINTF(out_api_url, out_api_url_size, "http://127.0.0.1:%u", (unsigned int)ntohs(addr.sin_port));
    if (n < 0 || (size_t)n >= out_api_url_size) {
        DSD_FPRINTF(stderr, "api url buffer too small\n");
        (void)dsd_socket_close(sock);
        return 1;
    }

    server->listen_sock = sock;
    if (dsd_thread_create(&server->thread, rdio_test_http_server_thread, server) != 0) {
        DSD_FPRINTF(stderr, "server thread create failed\n");
        (void)dsd_socket_close(sock);
        return 1;
    }

    return 0;
}

static int
rdio_test_http_server_start_n(rdio_test_http_server* server, char* out_api_url, size_t out_api_url_size,
                              const char* response, int no_request_expected, unsigned int accept_timeout_ms,
                              int requests_expected) {
    return rdio_test_http_server_start_cfg(server, out_api_url, out_api_url_size, response, no_request_expected,
                                           accept_timeout_ms, requests_expected, 0);
}

static int
rdio_test_http_server_start_ex(rdio_test_http_server* server, char* out_api_url, size_t out_api_url_size,
                               const char* response, int no_request_expected, unsigned int accept_timeout_ms) {
    return rdio_test_http_server_start_n(server, out_api_url, out_api_url_size, response, no_request_expected,
                                         accept_timeout_ms, 1);
}

static int
rdio_test_http_server_start(rdio_test_http_server* server, char* out_api_url, size_t out_api_url_size) {
    return rdio_test_http_server_start_ex(server, out_api_url, out_api_url_size, NULL, 0, 5000U);
}
#endif

static int
test_mode_parser(void) {
    int mode = -1;
    if (dsd_rdio_mode_from_string("off", NULL) == 0) {
        DSD_FPRINTF(stderr, "mode parser accepted null output\n");
        return 1;
    }
    if (dsd_rdio_mode_from_string(NULL, &mode) != 0 || mode != DSD_RDIO_MODE_OFF) {
        DSD_FPRINTF(stderr, "mode parser failed for null input\n");
        return 1;
    }
    mode = -1;
    if (dsd_rdio_mode_from_string("", &mode) != 0 || mode != DSD_RDIO_MODE_OFF) {
        DSD_FPRINTF(stderr, "mode parser failed for empty input\n");
        return 1;
    }
    if (dsd_rdio_mode_from_string("off", &mode) != 0 || mode != DSD_RDIO_MODE_OFF) {
        DSD_FPRINTF(stderr, "mode parser failed for off\n");
        return 1;
    }
    if (dsd_rdio_mode_from_string("dirwatch", &mode) != 0 || mode != DSD_RDIO_MODE_DIRWATCH) {
        DSD_FPRINTF(stderr, "mode parser failed for dirwatch\n");
        return 1;
    }
    if (dsd_rdio_mode_from_string("api", &mode) != 0 || mode != DSD_RDIO_MODE_API) {
        DSD_FPRINTF(stderr, "mode parser failed for api\n");
        return 1;
    }
    if (dsd_rdio_mode_from_string("both", &mode) != 0 || mode != DSD_RDIO_MODE_BOTH) {
        DSD_FPRINTF(stderr, "mode parser failed for both\n");
        return 1;
    }
    if (dsd_rdio_mode_from_string("DiRwAtCh", &mode) != 0 || mode != DSD_RDIO_MODE_DIRWATCH) {
        DSD_FPRINTF(stderr, "mode parser failed for mixed-case dirwatch\n");
        return 1;
    }
    if (dsd_rdio_mode_from_string("invalid", &mode) == 0) {
        DSD_FPRINTF(stderr, "mode parser accepted invalid value\n");
        return 1;
    }
    if (strcmp(dsd_rdio_mode_to_string(DSD_RDIO_MODE_OFF), "off") != 0
        || strcmp(dsd_rdio_mode_to_string(DSD_RDIO_MODE_DIRWATCH), "dirwatch") != 0
        || strcmp(dsd_rdio_mode_to_string(DSD_RDIO_MODE_API), "api") != 0
        || strcmp(dsd_rdio_mode_to_string(DSD_RDIO_MODE_BOTH), "both") != 0
        || strcmp(dsd_rdio_mode_to_string(99), "off") != 0) {
        DSD_FPRINTF(stderr, "mode to-string mapping failed\n");
        return 1;
    }
    return 0;
}

static int
test_dirwatch_sidecar_generation(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "call.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "call.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_dummy_wav(wav_path) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }
    opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts->rdio_system_id = 48;
    opts->rdio_upload_timeout_ms = 5000;
    opts->rdio_upload_retries = 1;

    // A 12-second call: the row's start, and its last activity, which by the time the WAV closes is the call's end.
    hist->Event_History_Items[0].event_start_time = (time_t)1699999988;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;
    hist->Event_History_Items[0].source_id = 660045;
    // The channel is the trunk channel number (here a P25 channel); the sidecar's freq is the row's frequency.
    hist->Event_History_Items[0].channel = 0x10C8;
    hist->Event_History_Items[0].freq_hz = 851012500;
    hist->Event_History_Items[0].enc = 1;
    DSD_SNPRINTF(hist->Event_History_Items[0].sysid_string, sizeof(hist->Event_History_Items[0].sysid_string), "%s",
                 "P25_TEST");
    DSD_SNPRINTF(hist->Event_History_Items[0].t_name, sizeof(hist->Event_History_Items[0].t_name), "%s", "FIRE DISP");

    if (dsd_rdio_export_call(opts, hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "dsd_rdio_export_call failed\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    char body[4096];
    if (read_file(json_path, body, sizeof(body)) != 0) {
        DSD_FPRINTF(stderr, "failed reading sidecar %s\n", json_path);
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    // The call is filed by its start. The dummy WAV has no RIFF header, so it adds no length to stop_time.
    int rc = expect_sidecar_times("dirwatch sidecar", body, 1699999988LL, 1699999988LL);
    if (strstr(body, "\"start_time\": 1700000000")) {
        DSD_FPRINTF(stderr, "sidecar start_time is the call's end\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"talkgroup\": 1201")) {
        DSD_FPRINTF(stderr, "sidecar missing talkgroup\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"srcList\": [{\"pos\":0,\"src\":660045}]")) {
        DSD_FPRINTF(stderr, "sidecar missing srcList\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"freq\": 851012500")) {
        DSD_FPRINTF(stderr, "sidecar missing freq\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"system\": 48")) {
        DSD_FPRINTF(stderr, "sidecar missing system\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"encrypted\": true")) {
        DSD_FPRINTF(stderr, "sidecar missing encrypted flag\n%s\n", body);
        rc = 1;
    }

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    free(hist);
    free(opts);

    return rc;
}

static int
test_mode_off_no_sidecar(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_off")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "call.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "call.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_dummy_wav(wav_path) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }
    opts->rdio_mode = DSD_RDIO_MODE_OFF;

    hist->Event_History_Items[0].event_time = time(NULL);
    hist->Event_History_Items[0].target_id = 1;

    int rc = 0;
    if (dsd_rdio_export_call(opts, hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "mode off should return success\n");
        rc = 1;
    }

    if (file_exists(json_path)) {
        DSD_FPRINTF(stderr, "sidecar should not be created when mode is off\n");
        rc = 1;
        (void)remove(json_path);
    }

    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    free(hist);
    free(opts);
    return rc;
}

static int
test_duration_uses_wav_samplerate(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_duration")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "call_48k.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "call_48k.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_pcm16_mono_wav(wav_path, 48000, 2) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }
    opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts->rdio_system_id = 48;

    hist->Event_History_Items[0].event_start_time = (time_t)1699999990;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    if (dsd_rdio_export_call(opts, hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "dsd_rdio_export_call failed for 48k wav\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    char body[4096];
    if (read_file(json_path, body, sizeof(body)) != 0) {
        DSD_FPRINTF(stderr, "failed reading sidecar %s\n", json_path);
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    // stop_time is the start plus the 2-second 48k recording.
    int rc = expect_sidecar_times("48k recording", body, 1699999990LL, 1699999992LL);

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    free(hist);
    free(opts);

    return rc;
}

static int
test_duration_skips_extra_wav_chunks(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_wav_chunks")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "chunked.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "chunked.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_pcm16_mono_wav_with_extra_chunks(wav_path, 8000, 3) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }
    opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts->rdio_system_id = 48;

    hist->Event_History_Items[0].event_start_time = (time_t)1700000090;
    hist->Event_History_Items[0].event_time = (time_t)1700000100;
    hist->Event_History_Items[0].target_id = 1202;

    if (dsd_rdio_export_call(opts, hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "dsd_rdio_export_call failed for chunked wav\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    char body[4096];
    if (read_file(json_path, body, sizeof(body)) != 0) {
        DSD_FPRINTF(stderr, "failed reading sidecar %s\n", json_path);
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    // The extra chunks are skipped: stop_time is the start plus the 3-second recording.
    int rc = expect_sidecar_times("chunked recording", body, 1700000090LL, 1700000093LL);

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    free(hist);
    free(opts);
    return rc;
}

static int
test_duration_rejects_invalid_wav_format_values(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_wav_invalid")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    struct {
        const char* wav_name;
        const char* json_name;
        uint32_t sample_rate;
        uint16_t block_align;
        uint32_t fmt_size;
    } cases[] = {
        {"short_fmt.wav", "short_fmt.json", 8000U, 2U, 15U},
        {"zero_rate.wav", "zero_rate.json", 0U, 2U, 16U},
        {"zero_align.wav", "zero_align.json", 8000U, 0U, 16U},
    };

    static dsd_opts opts;
    static Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = 48;
    hist.Event_History_Items[0].event_time = (time_t)1700000150;
    hist.Event_History_Items[0].target_id = 1203;

    int rc = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char wav_path[DSD_TEST_PATH_MAX] = {0};
        char json_path[DSD_TEST_PATH_MAX] = {0};
        if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, cases[i].wav_name) != 0
            || dsd_test_path_join(json_path, sizeof(json_path), dir_template, cases[i].json_name) != 0) {
            DSD_FPRINTF(stderr, "path join failed for invalid wav case %zu\n", i);
            rc = 1;
            break;
        }
        if (write_wav_with_custom_fmt(wav_path, cases[i].sample_rate, cases[i].block_align, cases[i].fmt_size) != 0) {
            (void)remove(wav_path);
            rc = 1;
            break;
        }
        if (dsd_rdio_export_call(&opts, &hist, wav_path, 0) != 0) {
            DSD_FPRINTF(stderr, "export failed for invalid wav duration case %zu\n", i);
            (void)remove(wav_path);
            rc = 1;
            break;
        }

        char body[4096];
        if (read_file(json_path, body, sizeof(body)) != 0) {
            DSD_FPRINTF(stderr, "failed reading invalid wav sidecar %s\n", json_path);
            (void)remove(wav_path);
            rc = 1;
            break;
        }
        if (!strstr(body, "\"stop_time\": 1700000150")) {
            DSD_FPRINTF(stderr, "invalid WAV format should produce zero-duration sidecar for case %zu\n%s\n", i, body);
            rc = 1;
        }
        (void)remove(json_path);
        (void)remove(wav_path);
        if (rc != 0) {
            break;
        }
    }

    remove_empty_dir(dir_template);
    return rc;
}

static int
test_duration_uses_zero_for_missing_or_truncated_wav(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_wav_unreadable")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    struct {
        const char* wav_name;
        const char* json_name;
        const unsigned char* bytes;
        size_t len;
    } cases[] = {
        {"missing.wav", "missing.json", NULL, 0U},
        {"truncated.wav", "truncated.json", (const unsigned char*)"RIFF", 4U},
    };

    static dsd_opts opts;
    static Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = 48;
    hist.Event_History_Items[0].event_time = (time_t)1700000160;
    hist.Event_History_Items[0].target_id = 1204;

    int rc = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char wav_path[DSD_TEST_PATH_MAX] = {0};
        char json_path[DSD_TEST_PATH_MAX] = {0};
        if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, cases[i].wav_name) != 0
            || dsd_test_path_join(json_path, sizeof(json_path), dir_template, cases[i].json_name) != 0) {
            DSD_FPRINTF(stderr, "path join failed for unreadable wav case %zu\n", i);
            rc = 1;
            break;
        }

        if (cases[i].bytes && write_bytes_file(wav_path, cases[i].bytes, cases[i].len) != 0) {
            rc = 1;
            break;
        }
        if (dsd_rdio_export_call(&opts, &hist, wav_path, 0) != 0) {
            DSD_FPRINTF(stderr, "export failed for unreadable wav duration case %zu\n", i);
            (void)remove(wav_path);
            rc = 1;
            break;
        }

        char body[4096];
        if (read_file(json_path, body, sizeof(body)) != 0) {
            DSD_FPRINTF(stderr, "failed reading unreadable wav sidecar %s\n", json_path);
            (void)remove(wav_path);
            rc = 1;
            break;
        }
        if (!strstr(body, "\"stop_time\": 1700000160")) {
            DSD_FPRINTF(stderr, "unreadable WAV should produce zero-duration sidecar for case %zu\n%s\n", i, body);
            rc = 1;
        }
        (void)remove(json_path);
        (void)remove(wav_path);
        if (rc != 0) {
            break;
        }
    }

    remove_empty_dir(dir_template);
    return rc;
}

static int
test_export_guards_and_invalid_mode_are_noops(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_guards")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "call.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "call.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_dummy_wav(wav_path) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    static dsd_opts opts;
    static Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = 99;
    hist.Event_History_Items[0].event_time = (time_t)1700000200;
    hist.Event_History_Items[0].target_id = 2;

    int rc = 0;
    if (dsd_rdio_export_call(NULL, &hist, wav_path, 0) == 0) {
        DSD_FPRINTF(stderr, "export accepted null opts\n");
        rc = 1;
    }
    if (dsd_rdio_export_call(&opts, &hist, NULL, 0) == 0) {
        DSD_FPRINTF(stderr, "export accepted null wav path\n");
        rc = 1;
    }
    if (dsd_rdio_export_call(&opts, &hist, "", 0) == 0) {
        DSD_FPRINTF(stderr, "export accepted empty wav path\n");
        rc = 1;
    }
    if (dsd_rdio_export_call(&opts, &hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "invalid rdio mode should be treated as disabled\n");
        rc = 1;
    }
    if (file_exists(json_path)) {
        DSD_FPRINTF(stderr, "invalid rdio mode should not create sidecar\n");
        (void)remove(json_path);
        rc = 1;
    }

    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    return rc;
}

static int
test_sidecar_escapes_strings_and_tgt_fallback(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_escape")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "escape.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "escape.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_dummy_wav(wav_path) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    static dsd_opts opts;
    static Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = -1;
    hist.Event_History_Items[0].event_time = (time_t)1700000400;
    hist.Event_History_Items[0].target_id = 44;
    hist.Event_History_Items[0].source_id = 0;
    hist.Event_History_Items[0].freq_hz = 12345;
    DSD_SNPRINTF(hist.Event_History_Items[0].tgt_str, sizeof(hist.Event_History_Items[0].tgt_str), "TG \"A\"\\B\n\t%c",
                 1);
    DSD_SNPRINTF(hist.Event_History_Items[0].sysid_string, sizeof(hist.Event_History_Items[0].sysid_string),
                 "SYS \"Q\"\\R\b\f\r");

    if (dsd_rdio_export_call(&opts, &hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "export failed for escaped string sidecar\n");
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    char body[4096];
    if (read_file(json_path, body, sizeof(body)) != 0) {
        DSD_FPRINTF(stderr, "failed reading escaped sidecar %s\n", json_path);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    int rc = 0;
    if (!strstr(body, "\"talkgroup_tag\": \"TG \\\"A\\\"\\\\B\\n\\t\\u0001\"")) {
        DSD_FPRINTF(stderr, "sidecar did not escape fallback talkgroup tag\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"srcList\": []")) {
        DSD_FPRINTF(stderr, "sidecar should omit zero source from srcList\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"freq\": 0")) {
        DSD_FPRINTF(stderr, "sidecar should clamp sub-MHz frequency to zero\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"system\": 0")) {
        DSD_FPRINTF(stderr, "sidecar should clamp negative system id to zero\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"short_name\": \"SYS \\\"Q\\\"\\\\R\\b\\f\\r\"")) {
        DSD_FPRINTF(stderr, "sidecar did not escape short_name\n%s\n", body);
        rc = 1;
    }

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    return rc;
}

static int
test_sidecar_private_fallback_and_malformed_wav_duration(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_private")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "private.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "private.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    FILE* fp = dsd_fopen_private(wav_path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", wav_path, strerror(errno));
        remove_empty_dir(dir_template);
        return 1;
    }
    if (fwrite("RIFF\012\0\0\0WAVE", 1, 12, fp) != 12U || fclose(fp) != 0) {
        DSD_FPRINTF(stderr, "failed writing malformed wav fixture\n");
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    static dsd_opts opts;
    static Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = 7;
    hist.Event_History_Items[0].event_time = (time_t)1700000500;
    hist.Event_History_Items[0].target_id = 77;
    hist.Event_History_Items[0].gi = 1;

    if (dsd_rdio_export_call(&opts, &hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "export failed for private fallback sidecar\n");
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    char body[4096];
    if (read_file(json_path, body, sizeof(body)) != 0) {
        DSD_FPRINTF(stderr, "failed reading private fallback sidecar %s\n", json_path);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    int rc = 0;
    if (!strstr(body, "\"talkgroup_tag\": \"PRIVATE\"")) {
        DSD_FPRINTF(stderr, "private call fallback tag missing\n%s\n", body);
        rc = 1;
    }
    if (!strstr(body, "\"stop_time\": 1700000500")) {
        DSD_FPRINTF(stderr, "malformed wav should produce zero-duration sidecar\n%s\n", body);
        rc = 1;
    }

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    return rc;
}

// A row heard while scanning a named channel has a name even when nothing else does: an encrypted
// call on a conventional list decodes no talkgroup at all, and "GROUP" tells the operator nothing
// the sidecar does not already say. The channel label is the last fallback before that default.
static int
test_sidecar_channel_label_fallback(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_label")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "label.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "label.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    FILE* fp = dsd_fopen_private(wav_path, "wb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", wav_path, strerror(errno));
        remove_empty_dir(dir_template);
        return 1;
    }
    if (fwrite("RIFF\012\0\0\0WAVE", 1, 12, fp) != 12U || fclose(fp) != 0) {
        DSD_FPRINTF(stderr, "failed writing malformed wav fixture\n");
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    static dsd_opts opts;
    static Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = 7;
    hist.Event_History_Items[0].event_time = (time_t)1700000600;
    // A numeric talkgroup with no name of its own -- an encrypted call, or one with no CSV entry.
    // The exporter skips a row whose talkgroup is zero outright, so the label is a fallback for
    // the nameless row rather than for the identity-less one.
    hist.Event_History_Items[0].target_id = 77;
    DSD_SNPRINTF(hist.Event_History_Items[0].channel_label, sizeof(hist.Event_History_Items[0].channel_label), "%s",
                 "Fire Dispatch");

    if (dsd_rdio_export_call(&opts, &hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "export failed for channel label sidecar\n");
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    char body[4096];
    if (read_file(json_path, body, sizeof(body)) != 0) {
        DSD_FPRINTF(stderr, "failed reading channel label sidecar %s\n", json_path);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    int rc = 0;
    if (!strstr(body, "\"talkgroup_tag\": \"Fire Dispatch\"")) {
        DSD_FPRINTF(stderr, "channel label fallback tag missing\n%s\n", body);
        rc = 1;
    }

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    return rc;
}

// What export_sidecar_body_with() saw: each outcome its own value, so a refusal is never mistaken for an export that
// wrote nothing, nor for a fixture that never reached the exporter.
enum {
    SIDECAR_EXPORTED = 0,      // the exporter succeeded, and its sidecar was read into the body
    SIDECAR_REFUSED = -1,      // the exporter refused, and left no sidecar
    SIDECAR_REFUSED_LEFT = -2, // the exporter refused, but left a sidecar behind
    SIDECAR_MISSING = -3,      // the exporter reported success, but no sidecar could be read
    SIDECAR_SETUP_FAILED = -4, // the fixture (directory, paths, recording) failed: the exporter never ran
};

// Export one row's sidecar into a fresh directory under @p with_opts (NULL: DirWatch, no system) and return the sidecar's
// body in @p body, with SIDECAR_EXPORTED, or another SIDECAR_* outcome. The recording is an 8 kHz WAV @p wav_seconds
// long, or with 0 a file with no RIFF header, which adds no length to stop_time, and was opened at decode time
// @p opened (0: unknown), its first sample stamped at @p started by its recorder (0: not). A negative @p allow_fallback
// exports through dsd_rdio_export_call_ex(), else through dsd_rdio_export_call_fallback() with it.
static int
export_sidecar_body_full(const dsd_opts* with_opts, const Event_History_I* hist, int wav_seconds, time_t opened,
                         time_t started, int allow_fallback, char* body, size_t body_size) {
    if (body_size > 0U) {
        body[0] = '\0'; // a failed export leaves an empty body, never an unread buffer
    }
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_freq")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return SIDECAR_SETUP_FAILED;
    }
    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "freq.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "freq.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return SIDECAR_SETUP_FAILED;
    }
    if ((wav_seconds > 0 ? write_pcm16_mono_wav(wav_path, 8000, wav_seconds) : write_dummy_wav(wav_path)) != 0) {
        remove_empty_dir(dir_template);
        return SIDECAR_SETUP_FAILED;
    }
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    const dsd_opts* use = with_opts ? with_opts : &opts;
    const int exported =
        allow_fallback < 0 ? dsd_rdio_export_call_ex(use, hist, wav_path, opened, started, NULL)
                           : dsd_rdio_export_call_fallback(use, hist, wav_path, opened, started, allow_fallback, NULL);
    int rc = SIDECAR_EXPORTED;
    if (exported != 0) {
        // A refused export leaves no sidecar behind.
        rc = file_exists(json_path) ? SIDECAR_REFUSED_LEFT : SIDECAR_REFUSED;
        if (rc == SIDECAR_REFUSED_LEFT) {
            DSD_FPRINTF(stderr, "a refused export left a sidecar\n");
        }
    } else if (read_file(json_path, body, body_size) != 0) {
        DSD_FPRINTF(stderr, "the export reported success but wrote no readable sidecar\n");
        rc = SIDECAR_MISSING;
    }
    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    return rc;
}

static int
export_sidecar_body_with(const dsd_opts* with_opts, const Event_History_I* hist, int wav_seconds, time_t opened,
                         time_t started, char* body, size_t body_size) {
    return export_sidecar_body_full(with_opts, hist, wav_seconds, opened, started, -1, body, body_size);
}

static int
export_sidecar_body(const Event_History_I* hist, int wav_seconds, time_t opened, time_t started, char* body,
                    size_t body_size) {
    const int rc = export_sidecar_body_with(NULL, hist, wav_seconds, opened, started, body, body_size);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "export failed for frequency sidecar\n");
    }
    return rc;
}

static int
test_sidecar_service_name_keeps_numeric_identity(void) {
    Event_History_I* hist = calloc(1, sizeof(*hist));
    if (!hist) {
        return 1;
    }
    Event_History* item = &hist->Event_History_Items[0];
    item->target_id = 77;
    item->source_id = 88;
    item->event_time = (time_t)1700000600;
    DSD_SNPRINTF(item->service_label, sizeof item->service_label, "%s", "Hospital West");
    DSD_SNPRINTF(item->channel_label, sizeof item->channel_label, "%s", "Base channel");
    DSD_SNPRINTF(item->t_name, sizeof item->t_name, "%s", "Numeric talkgroup name");
    char body[4096];
    int rc = export_sidecar_body(hist, 1, 0, 0, body, sizeof body);
    if (rc == 0 && (!strstr(body, "\"talkgroup_tag\": \"Hospital West\"") || !strstr(body, "\"talkgroup\": 77"))) {
        DSD_FPRINTF(stderr, "service name must label the unchanged numeric talkgroup\n%s\n", body);
        rc = 1;
    }
    free(hist);
    return rc;
}

// Issue #903 under the issue #888 fallback: a confirmed NXDN DCR recording names no party (target and source 0, "JPN
// DCR" its text identity), so it is exported as any digital call with no talkgroup is. With the fallback it is filed
// under its channel frequency in kHz, labelled by its protocol and frequency (not by the "JPN DCR" text: every
// unnumbered call on the frequency shares the talkgroup), its radio left unknown. With no known frequency and no
// --rdio-unknown-talkgroup, or without the fallback, it has no talkgroup and is refused.
static int
test_sidecar_dcr_files_under_channel_khz(void) {
    int rc = 0;
    for (int has_freq = 1; has_freq >= 0; has_freq--) {
        Event_History_I* hist = calloc(1, sizeof(*hist));
        if (!hist) {
            return 1;
        }
        Event_History* item = &hist->Event_History_Items[0];
        item->systype = DSD_SYNC_NXDN_POS;
        item->category = DSD_EVENT_CATEGORY_VOICE;
        item->gi = -1;
        item->event_time = (time_t)1700000600;
        item->freq_hz = has_freq ? 469012500 : 0;
        DSD_SNPRINTF(item->tgt_str, sizeof item->tgt_str, "%s", DSD_CALL_NXDN_DCR_TARGET_TEXT);
        char body[4096] = {0};
        const int exported = export_sidecar_body_full(NULL, hist, 1, 0, 0, 1, body, sizeof body);
        if (has_freq) {
            if (exported != SIDECAR_EXPORTED) {
                DSD_FPRINTF(stderr, "dcr sidecar: export failed (%d)\n", exported);
                rc = 1;
            } else if (!strstr(body, "\"talkgroup\": 469012")
                       || !strstr(body, "\"talkgroup_tag\": \"NXDN 469.0125 MHz\"")
                       || strstr(body, "\"src\":") != NULL) {
                DSD_FPRINTF(stderr, "dcr sidecar: want talkgroup 469012 tagged NXDN 469.0125 MHz, source 0\n%s\n",
                            body);
                rc = 1;
            }
        } else if (exported != SIDECAR_REFUSED) {
            /* Only the clean refusal: not an export, a refusal that left a sidecar, an export that wrote none, nor a
               fixture that never reached the exporter. */
            DSD_FPRINTF(stderr, "dcr sidecar with no frequency must be refused cleanly (got %d)\n", exported);
            rc = 1;
        }
        const int no_fallback = export_sidecar_body_full(NULL, hist, 1, 0, 0, 0, body, sizeof body);
        if (no_fallback != SIDECAR_REFUSED) {
            DSD_FPRINTF(stderr, "dcr sidecar without the fallback must be refused cleanly (got %d)\n", no_fallback);
            rc = 1;
        }
        free(hist);
    }
    return rc;
}

// Issue #903: an NXDN Type-D group's "pp-iiii" text only spells its numeric talkgroup, so it tags the upload as a bare
// number does: its list name, else the channel it was heard on, else the text. An M17 callsign keeps its own tag.
static int
test_sidecar_type_d_text_yields_to_names(void) {
    static const struct {
        int systype;
        const char* tgt_str;
        const char* t_name;
        const char* channel;
        const char* want;
    } cases[] = {
        {DSD_SYNC_NXDN_POS, "03-0005", "", "Fire Dispatch", "Fire Dispatch"},
        {DSD_SYNC_NXDN_POS, "03-0005", "Metro Fire", "Fire Dispatch", "Metro Fire"},
        {DSD_SYNC_NXDN_POS, "03-0005", "", "", "03-0005"},
        {DSD_SYNC_M17_LSF_POS, "N0CALL", "", "Fire Dispatch", "N0CALL"},
    };

    int rc = 0;
    for (size_t i = 0U; i < sizeof cases / sizeof cases[0]; i++) {
        Event_History_I* hist = calloc(1, sizeof(*hist));
        if (!hist) {
            return 1;
        }
        Event_History* item = &hist->Event_History_Items[0];
        item->systype = cases[i].systype;
        item->target_id = 5;
        item->source_id = 100;
        item->gi = 0;
        item->event_time = (time_t)1700000600;
        DSD_SNPRINTF(item->tgt_str, sizeof item->tgt_str, "%s", cases[i].tgt_str);
        DSD_SNPRINTF(item->t_name, sizeof item->t_name, "%s", cases[i].t_name);
        DSD_SNPRINTF(item->channel_label, sizeof item->channel_label, "%s", cases[i].channel);
        char body[4096] = {0};
        char want[128];
        DSD_SNPRINTF(want, sizeof want, "\"talkgroup_tag\": \"%s\"", cases[i].want);
        if (export_sidecar_body(hist, 1, 0, 0, body, sizeof body) != 0) {
            DSD_FPRINTF(stderr, "case %zu: export failed\n", i);
            rc = 1;
        } else if (!strstr(body, want) || !strstr(body, "\"talkgroup\": 5")) {
            DSD_FPRINTF(stderr, "case %zu: want %s\n%s\n", i, want, body);
            rc = 1;
        }
        free(hist);
    }
    return rc;
}

// The sidecar's freq is the frequency the call was heard on (Event_History::freq_hz). A row's channel is a trunk
// channel number, an LCN or a dPMR colour code, never Hz, and must not be read as one even when it is large enough to
// pass the 1 MHz floor. A frequency below 1 MHz, or one rdio-scanner's signed 32-bit frequency columns cannot store
// (issue #888 B5, B13), is reported as 0, and only a usable one is listed in freqList (D5, D8).
static int
test_sidecar_freq_comes_from_the_row_frequency(void) {
    static Event_History_I hist;

    struct {
        const char* label;
        uint32_t channel;
        int64_t freq_hz;
        const char* want;
        const char* want_list; /* NULL: no freqList key */
    } cases[] = {
        {"channel number without a frequency", 851012500U, 0, "\"freq\": 0,", NULL},
        {"row frequency beside a channel number", 0x10C8U, 453212500, "\"freq\": 453212500,",
         "\"freqList\": [{\"pos\": 0, \"freq\": 453212500}],"},
        {"frequency past 32 bits", 0U, INT64_C(5000000000), "\"freq\": 0,", NULL},
        {"negative frequency", 0U, -851012500, "\"freq\": 0,", NULL},
        {"sub-MHz frequency", 0U, 999999, "\"freq\": 0,", NULL},
        {"exactly 1 MHz", 0U, 1000000, "\"freq\": 1000000,", "\"freqList\": [{\"pos\": 0, \"freq\": 1000000}],"},
        {"signed 32-bit limit", 0U, INT64_C(2147483647), "\"freq\": 2147483647,",
         "\"freqList\": [{\"pos\": 0, \"freq\": 2147483647}],"},
        {"one above the signed 32-bit limit", 0U, INT64_C(2147483648), "\"freq\": 0,", NULL},
        {"2.4 GHz", 0U, INT64_C(2400000000), "\"freq\": 0,", NULL},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(&hist, 0, sizeof(hist));
        hist.Event_History_Items[0].event_time = (time_t)1700000800;
        hist.Event_History_Items[0].target_id = 1201;
        hist.Event_History_Items[0].channel = cases[i].channel;
        hist.Event_History_Items[0].freq_hz = cases[i].freq_hz;
        char body[4096];
        if (export_sidecar_body(&hist, 0, 0, 0, body, sizeof body) != 0) {
            return 1;
        }
        if (!strstr(body, cases[i].want)) {
            DSD_FPRINTF(stderr, "%s: sidecar missing %s\n%s\n", cases[i].label, cases[i].want, body);
            rc = 1;
        }
        if (cases[i].want_list ? !strstr(body, cases[i].want_list) : strstr(body, "freqList") != NULL) {
            DSD_FPRINTF(stderr, "%s: freqList should %s\n%s\n", cases[i].label,
                        cases[i].want_list ? cases[i].want_list : "be absent", body);
            rc = 1;
        }
    }
    return rc;
}

// start_time is when the recording began: the row's event_start_time, or the WAV's open time when per-call WAV was
// switched on after the call began. The row's event_time is its last activity -- the call's end once the WAV closes --
// and is only a fallback for a row with no start of its own, as a --playfiles row is, whose event_time comes from the
// played file and is on that file's clock, so the open time does not move it. A row with neither takes the decode
// clock. stop_time is always the start plus the recording, never a fallback stamp moved back by it: a played file's
// stamp need not be an end. A recorder that stamps its recording's first sample (the NFM call, issue #668) passes that
// start, which stands over the row's start and the open time: the row starts when its first block was accepted, a
// block's span after the audio began. The journal (dsd_rdio_recording_start()) takes the same start.
static int
test_sidecar_start_time_sources(void) {
    static Event_History_I hist;
    const time_t decode_now = (time_t)1700000900;

    struct {
        const char* label;
        time_t event_start_time;
        time_t event_time;
        time_t opened;
        time_t started;
        time_t want_start;
    } cases[] = {
        {"call start", (time_t)1700000810, (time_t)1700000830, 0, 0, (time_t)1700000810},
        {"recording opened before the call", (time_t)1700000810, (time_t)1700000830, (time_t)1700000700, 0,
         (time_t)1700000810},
        {
            "recording opened mid-call",
            (time_t)1700000810,
            (time_t)1700000830,
            (time_t)1700000822,
            0,
            (time_t)1700000822,
        },
        {"no start: last activity", 0, (time_t)1700000830, 0, 0, (time_t)1700000830},
        {"no start: open time does not move a played file's stamp", 0, (time_t)1600000000, (time_t)1700000822, 0,
         (time_t)1600000000},
        {"negative start: last activity", (time_t)-5, (time_t)1700000830, 0, 0, (time_t)1700000830},
        {"neither stamp: decode clock", 0, 0, 0, 0, decode_now},
        {
            "stamped first sample, a second before the row's start",
            (time_t)1700000810,
            (time_t)1700000830,
            (time_t)1700000810,
            (time_t)1700000809,
            (time_t)1700000809,
        },
        {
            "stamped first sample, over a later open",
            (time_t)1700000810,
            (time_t)1700000830,
            (time_t)1700000822,
            (time_t)1700000815,
            (time_t)1700000815,
        },
        {"stamped first sample, with no row start", 0, (time_t)1600000000, 0, (time_t)1700000805, (time_t)1700000805},
        {"a negative stamp is none", (time_t)1700000810, (time_t)1700000830, 0, (time_t)-1, (time_t)1700000810},
    };

    dsd_decode_clock_use_test((uint64_t)decode_now * UINT64_C(1000000000));
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(&hist, 0, sizeof(hist));
        hist.Event_History_Items[0].event_start_time = cases[i].event_start_time;
        hist.Event_History_Items[0].event_time = cases[i].event_time;
        hist.Event_History_Items[0].target_id = 1201;
        char body[4096];
        if (export_sidecar_body(&hist, 2, cases[i].opened, cases[i].started, body, sizeof body) != 0) {
            rc = 1;
            break;
        }
        rc |= expect_sidecar_times(cases[i].label, body, (long long)cases[i].want_start,
                                   (long long)cases[i].want_start + 2LL);
        const time_t journal_start =
            dsd_rdio_recording_start(&hist.Event_History_Items[0], cases[i].opened, cases[i].started);
        if (journal_start != cases[i].want_start) {
            DSD_FPRINTF(stderr, "%s: the journal's start %lld, want %lld\n", cases[i].label, (long long)journal_start,
                        (long long)cases[i].want_start);
            rc = 1;
        }
    }
    dsd_decode_clock_use_system();
    return rc;
}

// A conventional analog (NFM) row as the event layer stages it (issue #668): no talkgroup, no kind, the mode as its
// system string; heard on @p freq_hz (0: an audio input's unknown frequency).
static void
analog_row(Event_History_I* hist, int64_t freq_hz) {
    DSD_MEMSET(hist, 0, sizeof(*hist));
    Event_History* item = &hist->Event_History_Items[0];
    item->systype = DSD_SYNC_ANALOG;
    item->gi = -1;
    item->event_start_time = (time_t)1700001000;
    item->event_time = (time_t)1700001002;
    item->freq_hz = freq_hz;
    DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", "NFM");
}

static int
expect_body_has(const char* label, const char* body, const char* want) {
    if (!strstr(body, want)) {
        DSD_FPRINTF(stderr, "%s: sidecar missing %s\n%s\n", label, want, body);
        return 1;
    }
    return 0;
}

// Issue #668 D3/D6: an NFM call is exported under its frequency in kHz, rounded down, labelled by that frequency, with
// no radio, never encrypted, under the row's system string and frequency. Its per-tone service name is not the tag, and
// nothing is written back to the row: Event_History::target_id stays 0.
static int
test_analog_sidecar_fields(void) {
    static Event_History_I hist;
    analog_row(&hist, 154437500);
    Event_History* item = &hist.Event_History_Items[0];
    item->source_id = 99U; /* never an analog row's, and still not exported as one */
    item->enc = 1;
    DSD_SNPRINTF(item->service_label, sizeof item->service_label, "%s", "Fire North");
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = 7;
    char body[4096];
    if (export_sidecar_body_with(&opts, &hist, 2, 0, 0, body, sizeof body) != 0) {
        DSD_FPRINTF(stderr, "analog sidecar: export failed\n");
        return 1;
    }
    int rc = 0;
    rc |= expect_body_has("analog sidecar", body, "\"talkgroup\": 154437,");
    rc |= expect_body_has("analog sidecar", body, "\"talkgroup_tag\": \"NFM 154.4375 MHz\",");
    rc |= expect_body_has("analog sidecar", body, "\"srcList\": [],");
    rc |= expect_body_has("analog sidecar", body, "\"freq\": 154437500,");
    rc |= expect_body_has("analog sidecar", body, "\"system\": 7,");
    rc |= expect_body_has("analog sidecar", body, "\"short_name\": \"NFM\",");
    rc |= expect_body_has("analog sidecar", body, "\"encrypted\": false");
    rc |= expect_sidecar_times("analog sidecar", body, 1700001000LL, 1700001002LL);
    if (item->target_id != 0U) {
        DSD_FPRINTF(stderr, "analog sidecar: the export id was stored on the row (%u)\n", (unsigned)item->target_id);
        rc = 1;
    }
    return rc;
}

// Issue #668 D3/D5: the talkgroup comes from the frequency, in whole kHz; a call with none (audio input) takes
// --rdio-analog-talkgroup, and with neither it is not exported. The frequency wins over the fallback. Below 1 MHz the
// sidecar's freq is 0 but the call is still exported under its kHz; a frequency past signed 32 bits (issue #888 B13)
// still names its kHz with freq 0.
static int
test_analog_talkgroup_sources(void) {
    static const struct {
        const char* label;
        int64_t freq_hz;
        int fallback;
        int exported;
        const char* talkgroup;
        const char* freq;
        const char* tag;
    } cases[] = {
        {"frequency", 154437500, 0, 1, "\"talkgroup\": 154437,", "\"freq\": 154437500,", "NFM 154.4375 MHz"},
        {
            "frequency over the fallback",
            154437500,
            9000,
            1,
            "\"talkgroup\": 154437,",
            "\"freq\": 154437500,",
            "NFM 154.4375 MHz",
        },
        {"no frequency: the fallback", 0, 9000, 1, "\"talkgroup\": 9000,", "\"freq\": 0,", "NFM"},
        {"the largest fallback", 0, 2147483647, 1, "\"talkgroup\": 2147483647,", "\"freq\": 0,", "NFM"},
        {"below the 1 MHz floor", 530000, 0, 1, "\"talkgroup\": 530,", "\"freq\": 0,", "NFM 0.5300 MHz"},
        {"past 32 bits", INT64_C(5000000000), 0, 1, "\"talkgroup\": 5000000,", "\"freq\": 0,", "NFM 5000.0000 MHz"},
        {"exactly 1 MHz", 1000000, 0, 1, "\"talkgroup\": 1000,", "\"freq\": 1000000,", "NFM 1.0000 MHz"},
        {"past signed 32-bit Hz", INT64_C(3000000000), 0, 1, "\"talkgroup\": 3000000,", "\"freq\": 0,",
         "NFM 3000.0000 MHz"},
        {"no frequency, no fallback", 0, 0, 0, NULL, NULL, NULL},
        {"under 1 kHz, no fallback", 999, 0, 0, NULL, NULL, NULL},
        {"negative frequency, no fallback", -154437500, 0, 0, NULL, NULL, NULL},
    };

    static Event_History_I hist;
    static dsd_opts opts;
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        analog_row(&hist, cases[i].freq_hz);
        DSD_MEMSET(&opts, 0, sizeof(opts));
        opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        opts.rdio_analog_talkgroup = cases[i].fallback;
        char body[4096];
        const int got = export_sidecar_body_with(&opts, &hist, 1, 0, 0, body, sizeof body);
        const uint32_t id = dsd_rdio_analog_talkgroup(&opts, &hist.Event_History_Items[0]);
        if (!cases[i].exported) {
            if (got != SIDECAR_REFUSED || id != 0U) {
                DSD_FPRINTF(stderr, "%s: exported (%d) under %u, want no export\n", cases[i].label, got, (unsigned)id);
                rc = 1;
            }
            continue;
        }
        if (got != 0) {
            DSD_FPRINTF(stderr, "%s: export failed\n", cases[i].label);
            rc = 1;
            continue;
        }
        char tag[96];
        DSD_SNPRINTF(tag, sizeof tag, "\"talkgroup_tag\": \"%s\",", cases[i].tag);
        rc |= expect_body_has(cases[i].label, body, cases[i].talkgroup);
        rc |= expect_body_has(cases[i].label, body, cases[i].freq);
        rc |= expect_body_has(cases[i].label, body, tag);
    }
    // Only an analog row has one: a digital row with no talkgroup is never given the fallback.
    analog_row(&hist, 154437500);
    hist.Event_History_Items[0].systype = DSD_SYNC_P25P1_POS;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.rdio_analog_talkgroup = 9000;
    if (dsd_rdio_analog_talkgroup(&opts, &hist.Event_History_Items[0]) != 0U
        || dsd_rdio_analog_talkgroup(&opts, NULL) != 0U) {
        DSD_FPRINTF(stderr, "a digital row (or none) has no analog talkgroup\n");
        rc = 1;
    }
    analog_row(&hist, 0);
    if (dsd_rdio_analog_talkgroup(NULL, &hist.Event_History_Items[0]) != 0U) {
        DSD_FPRINTF(stderr, "no options, no fallback\n");
        rc = 1;
    }
    return rc;
}

// Issue #668 D6: the tag is the channel's name (a -Y or trunk-scan row's label), else the frequency to 4 decimals of
// MHz after the row's mode word, else the mode word alone; never the service a tone named, which would relabel the
// talkgroup the services on one frequency share. The same call always gets the same tag.
static int
test_analog_tag_is_stable(void) {
    static const struct {
        const char* label;
        int64_t freq_hz;
        const char* channel;
        const char* sysid;
        const char* want;
    } cases[] = {
        {"channel label", 154437500, "County Fire", "NFM", "County Fire"},
        {"channel label, no frequency", 0, "County Fire", "NFM", "County Fire"},
        {"frequency", 154430000, "", "NFM", "NFM 154.4300 MHz"},
        {"frequency rounded to 100 Hz", 462562550, "", "NFM", "NFM 462.5626 MHz"},
        {"the row's mode word", 121500000, "", "AM", "AM 121.5000 MHz"},
        {"nothing: the mode word", 0, "", "NFM", "NFM"},
        {"no mode word on the row", 0, "", "", "NFM"},
    };

    static Event_History_I hist;
    static dsd_opts opts;
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        for (int service = 0; service <= 1; service++) {
            analog_row(&hist, cases[i].freq_hz);
            Event_History* item = &hist.Event_History_Items[0];
            DSD_SNPRINTF(item->channel_label, sizeof item->channel_label, "%s", cases[i].channel);
            DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", cases[i].sysid);
            if (service) {
                DSD_SNPRINTF(item->service_label, sizeof item->service_label, "%s", "Fire North");
            }
            DSD_MEMSET(&opts, 0, sizeof(opts));
            opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
            opts.rdio_analog_talkgroup = 9000;
            char body[4096];
            char want[96];
            DSD_SNPRINTF(want, sizeof want, "\"talkgroup_tag\": \"%s\",", cases[i].want);
            if (export_sidecar_body_with(&opts, &hist, 1, 0, 0, body, sizeof body) != 0) {
                DSD_FPRINTF(stderr, "%s: export failed\n", cases[i].label);
                rc = 1;
                continue;
            }
            rc |= expect_body_has(cases[i].label, body, want);
        }
    }
    return rc;
}

// Issue #668 D4: --rdio-analog-system-id files an analog call under its own system in the sidecar, over the base one
// and without it; 0 keeps the base. A digital row always keeps the base.
static int
test_analog_system_id(void) {
    static const struct {
        const char* label;
        int analog;
        int base;
        int override;
        int want;
    } cases[] = {
        {"analog, no override", 1, 7, 0, 7}, {"analog override", 1, 7, 9, 9}, {"analog override, base 0", 1, 0, 9, 9},
        {"analog, neither", 1, 0, 0, 0},     {"digital row", 0, 7, 9, 7},
    };

    static Event_History_I hist;
    static dsd_opts opts;
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        analog_row(&hist, 154437500);
        if (!cases[i].analog) {
            hist.Event_History_Items[0].systype = DSD_SYNC_P25P1_POS;
            hist.Event_History_Items[0].target_id = 1201U;
        }
        DSD_MEMSET(&opts, 0, sizeof(opts));
        opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        opts.rdio_system_id = cases[i].base;
        opts.rdio_analog_system_id = cases[i].override;
        char body[4096];
        char want[48];
        DSD_SNPRINTF(want, sizeof want, "\"system\": %d,", cases[i].want);
        if (export_sidecar_body_with(&opts, &hist, 1, 0, 0, body, sizeof body) != 0) {
            DSD_FPRINTF(stderr, "%s: export failed\n", cases[i].label);
            rc = 1;
            continue;
        }
        rc |= expect_body_has(cases[i].label, body, want);
        if (dsd_rdio_call_system_id(&opts, &hist.Event_History_Items[0]) != cases[i].want) {
            DSD_FPRINTF(stderr, "%s: the upload's system differs from the sidecar's\n", cases[i].label);
            rc = 1;
        }
    }
    if (dsd_rdio_call_system_id(NULL, &hist.Event_History_Items[0]) != 0) {
        DSD_FPRINTF(stderr, "no options: system 0\n");
        rc = 1;
    }
    return rc;
}

// B3 (issue #668): with no name to show, the tag says PRIVATE only for a private call and GROUP only for a group call;
// a call of no known kind (gi -1: NXDN's other call types, EDACS without a flag, TETRA before setup, X2-TDMA) gets an
// empty tag, which rdio-scanner ignores as a label. A name, when the row has one, still wins.
static int
test_sidecar_tag_of_unknown_kind_is_empty(void) {
    static const struct {
        const char* label;
        int gi;
        const char* t_name;
        const char* want;
    } cases[] = {
        {"unknown kind", -1, "", "\"talkgroup_tag\": \"\","},
        {"group call", 0, "", "\"talkgroup_tag\": \"GROUP\","},
        {"private call", 1, "", "\"talkgroup_tag\": \"PRIVATE\","},
        {"unknown kind with a name", -1, "Ops", "\"talkgroup_tag\": \"Ops\","},
    };

    static Event_History_I hist;
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(&hist, 0, sizeof(hist));
        Event_History* item = &hist.Event_History_Items[0];
        item->systype = DSD_SYNC_NXDN_POS;
        item->event_time = (time_t)1700001100;
        item->target_id = 77U;
        item->gi = (int8_t)cases[i].gi;
        DSD_SNPRINTF(item->t_name, sizeof item->t_name, "%s", cases[i].t_name);
        char body[4096];
        if (export_sidecar_body(&hist, 1, 0, 0, body, sizeof body) != 0) {
            rc = 1;
            continue;
        }
        rc |= expect_body_has(cases[i].label, body, cases[i].want);
    }
    return rc;
}

/* The ring and options the issue #888 sidecar cases share, zeroed for each case that asks: one ring (about 3.7 MiB)
   for the run, not one a case, and the options on the heap. NULL when they cannot be had. Freed at the end of main(). */
static Event_History_I* g_rdio_case_hist = NULL;
static dsd_opts* g_rdio_case_opts = NULL;

static Event_History_I*
rdio_case_hist(void) {
    if (g_rdio_case_hist == NULL) {
        g_rdio_case_hist = (Event_History_I*)calloc(1, sizeof(*g_rdio_case_hist));
    } else {
        DSD_MEMSET(g_rdio_case_hist, 0, sizeof(*g_rdio_case_hist));
    }
    return g_rdio_case_hist;
}

static dsd_opts*
rdio_case_opts(void) {
    if (g_rdio_case_opts == NULL) {
        g_rdio_case_opts = (dsd_opts*)calloc(1, sizeof(*g_rdio_case_opts));
    } else {
        DSD_MEMSET(g_rdio_case_opts, 0, sizeof(*g_rdio_case_opts));
    }
    return g_rdio_case_opts;
}

/* Issue #931: the row and options a per-frequency system case exports: an analog (NFM) row, or a numbered P25 one,
   heard on @p freq_hz, under base system @p base and analog system @p analog_id, the option on or off. Analog calls
   with no frequency take talkgroup 77 so that every case is exported. */
static void
system_case_setup(Event_History_I* hist, dsd_opts* opts, int analog, int64_t freq_hz, int on, int base, int analog_id) {
    analog_row(hist, freq_hz);
    if (!analog) {
        Event_History* item = &hist->Event_History_Items[0];
        item->systype = DSD_SYNC_P25P1_POS;
        item->target_id = 1201U;
        item->gi = 0;
        DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", "P25");
    }
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts->rdio_system_by_frequency = on;
    opts->rdio_system_id = base;
    opts->rdio_analog_system_id = analog_id;
    opts->rdio_analog_talkgroup = 77;
}

/* The case set up in @p hist / @p opts resolves to @p want_id and @p want_rule, in the resolver and the public id
   function; with @p exported, its sidecar names that system and the rule's short_name (@p row_name unless the rule is
   FREQUENCY). */
static int
expect_system_case_ex(const char* label, Event_History_I* hist, dsd_opts* opts, int want_id,
                      dsd_rdio_system_rule want_rule, const char* row_name, int exported) {
    const Event_History* item = &hist->Event_History_Items[0];
    const dsd_rdio_system_choice got = dsd_rdio_resolve_call_system(opts, item);
    int rc = 0;
    if (got.id != want_id || got.rule != want_rule) {
        DSD_FPRINTF(stderr, "%s: resolved %d rule %d, want %d rule %d\n", label, got.id, (int)got.rule, want_id,
                    (int)want_rule);
        rc = 1;
    }
    if (dsd_rdio_call_system_id(opts, item) != want_id) {
        DSD_FPRINTF(stderr, "%s: dsd_rdio_call_system_id() differs from the resolver\n", label);
        rc = 1;
    }
    if (!exported) {
        return rc;
    }
    char body[4096];
    if (export_sidecar_body_with(opts, hist, 1, 0, 0, body, sizeof body) != 0) {
        DSD_FPRINTF(stderr, "%s: export failed\n", label);
        return 1;
    }
    char want[64];
    DSD_SNPRINTF(want, sizeof want, "\"system\": %d,", want_id);
    rc |= expect_body_has(label, body, want);
    if (want_rule == DSD_RDIO_SYSTEM_RULE_FREQUENCY) {
        DSD_SNPRINTF(want, sizeof want, "\"short_name\": \"System %d\",", want_id);
    } else {
        DSD_SNPRINTF(want, sizeof want, "\"short_name\": \"%s\",", row_name);
    }
    rc |= expect_body_has(label, body, want);
    return rc;
}

static int
expect_system_case(const char* label, Event_History_I* hist, dsd_opts* opts, int want_id,
                   dsd_rdio_system_rule want_rule, const char* row_name) {
    return expect_system_case_ex(label, hist, opts, want_id, want_rule, row_name, 1);
}

// Issue #931 D3: --rdio-system-by-frequency files a call under its frequency in whole kHz, rounded down (1 kHz up to
// 2147483647 kHz), as the sidecar's system, the upload's system and a "System <kHz>" short_name; an analog call with
// --rdio-analog-system-id keeps that system; a call with no usable frequency, and every call with the option off, keep
// today's system. Every combination of option, row kind, analog system, base system and frequency is resolved; the
// sidecar is written for each combination at 154.4375 MHz and for every frequency of a digital row over base 48 with
// the option on (each sidecar is synced to disk, so not all of them are).
static int
test_system_by_frequency_resolver(void) {
    static const struct {
        int64_t freq_hz;
        int khz; /* the kHz system, 0 for none */
    } freqs[] = {
        {0, 0},
        {999, 0},
        {1000, 1},
        {530000, 530},
        {154437500, 154437},
        {INT64_C(2147483647000), 2147483647},
        {INT64_C(2147483648000), 0},
    };

    static const int bases[] = {0, 48};
    static const int analog_ids[] = {0, 5};
    Event_History_I* const hist = rdio_case_hist();
    dsd_opts* const opts = rdio_case_opts();
    if (hist == NULL || opts == NULL) {
        return 1;
    }
    int rc = 0;
    for (int on = 0; on <= 1; on++) {
        for (int analog = 0; analog <= 1; analog++) {
            for (size_t a = 0; a < sizeof analog_ids / sizeof analog_ids[0]; a++) {
                for (size_t b = 0; b < sizeof bases / sizeof bases[0]; b++) {
                    for (size_t f = 0; f < sizeof freqs / sizeof freqs[0]; f++) {
                        system_case_setup(hist, opts, analog, freqs[f].freq_hz, on, bases[b], analog_ids[a]);
                        int want_id = bases[b];
                        dsd_rdio_system_rule want_rule = DSD_RDIO_SYSTEM_RULE_BASE;
                        if (analog && analog_ids[a] > 0) {
                            want_id = analog_ids[a];
                            want_rule = DSD_RDIO_SYSTEM_RULE_ANALOG;
                        } else if (on && freqs[f].khz > 0) {
                            want_id = freqs[f].khz;
                            want_rule = DSD_RDIO_SYSTEM_RULE_FREQUENCY;
                        }
                        char label[160];
                        DSD_SNPRINTF(label, sizeof label, "option %d, %s row, analog id %d, base %d, %lld Hz", on,
                                     analog ? "analog" : "digital", analog_ids[a], bases[b],
                                     (long long)freqs[f].freq_hz);
                        const int exported = freqs[f].freq_hz == 154437500 || (on && !analog && a == 0 && b == 1);
                        rc |= expect_system_case_ex(label, hist, opts, want_id, want_rule, analog ? "NFM" : "P25",
                                                    exported);
                    }
                }
            }
        }
    }

    // The label follows the rule, never the number: an analog system equal to the row's kHz is still the analog one,
    // a base system equal to it with the option off is still the base one, and with the option on a kHz equal to the
    // base system is still a frequency system.
    system_case_setup(hist, opts, 1, 5000, 1, 48, 5);
    rc |= expect_system_case("analog system equal to the kHz", hist, opts, 5, DSD_RDIO_SYSTEM_RULE_ANALOG, "NFM");
    system_case_setup(hist, opts, 0, 48000, 0, 48, 0);
    rc |= expect_system_case("option off, base equal to the kHz", hist, opts, 48, DSD_RDIO_SYSTEM_RULE_BASE, "P25");
    system_case_setup(hist, opts, 0, 48000, 1, 48, 0);
    rc |= expect_system_case("option on, kHz equal to the base", hist, opts, 48, DSD_RDIO_SYSTEM_RULE_FREQUENCY, "P25");

    // No row: the base system; no options: system 0.
    system_case_setup(hist, opts, 0, 154437500, 1, 48, 5);
    dsd_rdio_system_choice got = dsd_rdio_resolve_call_system(opts, NULL);
    if (got.id != 48 || got.rule != DSD_RDIO_SYSTEM_RULE_BASE || dsd_rdio_call_system_id(opts, NULL) != 48) {
        DSD_FPRINTF(stderr, "no row: resolved %d rule %d, want 48 BASE\n", got.id, (int)got.rule);
        rc = 1;
    }
    got = dsd_rdio_resolve_call_system(NULL, &hist->Event_History_Items[0]);
    if (got.id != 0 || got.rule != DSD_RDIO_SYSTEM_RULE_BASE) {
        DSD_FPRINTF(stderr, "no options: resolved %d rule %d, want 0 BASE\n", got.id, (int)got.rule);
        rc = 1;
    }
    return rc;
}

// Issue #931 D4: a frequency system's sidecar names it by number and by "System <kHz>", and the row's own system
// string is left as it was (the WAV name, the journal and the event feed use it). A 530 kHz call has no storable freq
// (0, no freqList) but still its 530 kHz system.
static int
test_system_by_frequency_sidecar(void) {
    Event_History_I* const hist = rdio_case_hist();
    dsd_opts* const opts = rdio_case_opts();
    if (hist == NULL || opts == NULL) {
        return 1;
    }
    int rc = 0;
    char body[4096];
    system_case_setup(hist, opts, 0, 154437500, 1, 0, 0);
    if (export_sidecar_body_with(opts, hist, 1, 0, 0, body, sizeof body) != 0) {
        DSD_FPRINTF(stderr, "154.4375 MHz sidecar: export failed\n");
        return 1;
    }
    rc |= expect_body_has("154.4375 MHz sidecar", body, "\"system\": 154437,");
    rc |= expect_body_has("154.4375 MHz sidecar", body, "\"short_name\": \"System 154437\",");
    rc |= expect_body_has("154.4375 MHz sidecar", body, "\"freq\": 154437500,");
    rc |= expect_body_has("154.4375 MHz sidecar", body, "\"talkgroup\": 1201,");
    if (strcmp(hist->Event_History_Items[0].sysid_string, "P25") != 0) {
        DSD_FPRINTF(stderr, "154.4375 MHz sidecar: the row's system string changed to '%s'\n",
                    hist->Event_History_Items[0].sysid_string);
        rc = 1;
    }

    system_case_setup(hist, opts, 0, 530000, 1, 48, 0);
    if (export_sidecar_body_with(opts, hist, 1, 0, 0, body, sizeof body) != 0) {
        DSD_FPRINTF(stderr, "530 kHz sidecar: export failed\n");
        return 1;
    }
    rc |= expect_body_has("530 kHz sidecar", body, "\"freq\": 0,");
    rc |= expect_body_has("530 kHz sidecar", body, "\"system\": 530,");
    rc |= expect_body_has("530 kHz sidecar", body, "\"short_name\": \"System 530\",");
    if (strstr(body, "freqList") != NULL) {
        DSD_FPRINTF(stderr, "530 kHz sidecar: freqList written\n%s\n", body);
        rc = 1;
    }
    return rc;
}

// Issue #888 B6: the sidecar's emergency is the row's own flag (an NXDN VCALL with the emergency bit, say), never a
// constant false.
static int
test_sidecar_emergency_from_the_row(void) {
    Event_History_I* const hist = rdio_case_hist();
    if (hist == NULL) {
        return 1;
    }
    int rc = 0;
    for (int emergency = 0; emergency <= 1; emergency++) {
        DSD_MEMSET(hist, 0, sizeof(*hist));
        Event_History* item = &hist->Event_History_Items[0];
        item->systype = DSD_SYNC_NXDN_POS;
        item->event_time = (time_t)1700001200;
        item->target_id = 301U;
        item->emergency = (uint8_t)emergency;
        char body[4096];
        if (export_sidecar_body(hist, 1, 0, 0, body, sizeof body) != 0) {
            rc = 1;
            continue;
        }
        rc |= expect_body_has(emergency ? "emergency row" : "routine row", body,
                              emergency ? "\"emergency\": true," : "\"emergency\": false,");
    }
    return rc;
}

// Issue #888 B8: short_name is the row's system string, else the protocol's word, so a row with no system string (an
// X2-TDMA call) still names a system for an rdio-scanner v8 DirWatch, which resolves the system from short_name.
static int
test_sidecar_short_name_falls_back_to_the_protocol(void) {
    static const struct {
        const char* label;
        int systype;
        const char* sysid;
        const char* want;
    } cases[] = {
        {"X2-TDMA, no system string", DSD_SYNC_X2TDMA_VOICE_POS, "", "\"short_name\": \"X2-TDMA\","},
        {"D-STAR, no system string", DSD_SYNC_DSTAR_VOICE_NEG, "", "\"short_name\": \"D-STAR\","},
        {"P25 phase 2, no system string", DSD_SYNC_P25P2_POS, "", "\"short_name\": \"P25\","},
        {"NXDN with a system string", DSD_SYNC_NXDN_NEG, "NXDN_RAN_9", "\"short_name\": \"NXDN_RAN_9\","},
        {"no protocol", DSD_SYNC_DIGITAL, "", "\"short_name\": \"\","},
    };

    Event_History_I* const hist = rdio_case_hist();
    if (hist == NULL) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(hist, 0, sizeof(*hist));
        Event_History* item = &hist->Event_History_Items[0];
        item->systype = (int8_t)cases[i].systype;
        item->event_time = (time_t)1700001300;
        item->target_id = 12U;
        DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", cases[i].sysid);
        char body[4096];
        if (export_sidecar_body(hist, 1, 0, 0, body, sizeof body) != 0) {
            rc = 1;
            continue;
        }
        rc |= expect_body_has(cases[i].label, body, cases[i].want);
    }
    return rc;
}

// Issue #888 B10: rdio-scanner discards a tag of exactly "-", so "-" is no name at any step of the chain: the next
// candidate labels the call. An analog channel label of "-" gives way to the frequency tag.
static int
test_sidecar_dash_tag_is_absent(void) {
    static const struct {
        const char* label;
        const char* service;
        const char* t_name;
        const char* tgt_str;
        const char* channel;
        const char* want;
    } cases[] = {
        {"dash service name", "-", "Ops", "", "", "\"talkgroup_tag\": \"Ops\","},
        {"dash talkgroup name", "", "-", "TG-A", "", "\"talkgroup_tag\": \"TG-A\","},
        {"dash target text", "", "", "-", "Dispatch", "\"talkgroup_tag\": \"Dispatch\","},
        {"dash everywhere", "-", "-", "-", "-", "\"talkgroup_tag\": \"GROUP\","},
        {"a name that starts with a dash", "-East", "Ops", "", "", "\"talkgroup_tag\": \"-East\","},
    };

    Event_History_I* const hist = rdio_case_hist();
    if (hist == NULL) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(hist, 0, sizeof(*hist));
        Event_History* item = &hist->Event_History_Items[0];
        item->systype = DSD_SYNC_DMR_BS_VOICE_POS;
        item->event_time = (time_t)1700001400;
        item->target_id = 77U;
        item->gi = 0;
        DSD_SNPRINTF(item->service_label, sizeof item->service_label, "%s", cases[i].service);
        DSD_SNPRINTF(item->t_name, sizeof item->t_name, "%s", cases[i].t_name);
        DSD_SNPRINTF(item->tgt_str, sizeof item->tgt_str, "%s", cases[i].tgt_str);
        DSD_SNPRINTF(item->channel_label, sizeof item->channel_label, "%s", cases[i].channel);
        char body[4096];
        if (export_sidecar_body(hist, 1, 0, 0, body, sizeof body) != 0) {
            rc = 1;
            continue;
        }
        rc |= expect_body_has(cases[i].label, body, cases[i].want);
    }
    analog_row(hist, 154437500);
    DSD_SNPRINTF(hist->Event_History_Items[0].channel_label, sizeof hist->Event_History_Items[0].channel_label, "%s",
                 "-");
    char body[4096];
    if (export_sidecar_body(hist, 1, 0, 0, body, sizeof body) != 0) {
        return 1;
    }
    rc |= expect_body_has("analog dash channel label", body, "\"talkgroup_tag\": \"NFM 154.4375 MHz\",");
    return rc;
}

// Issue #888 B11: sidecar strings are JSON, which is UTF-8. A name cut mid-character (a CSV import) or any other byte
// that is no part of a well-formed character becomes U+FFFD, one for each maximal ill-formed run; well-formed text,
// multibyte characters included, is written untouched.
static int
test_sidecar_strings_are_valid_utf8(void) {
    static const struct {
        const char* label;
        const char* in;
        const char* want;
    } cases[] = {
        {"well-formed text", "Caf\xC3\xA9 \xE6\x97\xA5 \xF0\x9F\x93\xBB", "Caf\xC3\xA9 \xE6\x97\xA5 \xF0\x9F\x93\xBB"},
        {"cut two-byte character", "Caf\xC3", "Caf\xEF\xBF\xBD"},
        {"cut three-byte character",
         "A\xE6\x97"
         "B",
         "A\xEF\xBF\xBD"
         "B"},
        {"lone continuation byte",
         "A\x80"
         "B",
         "A\xEF\xBF\xBD"
         "B"},
        {"overlong form", "\xC0\xAF", "\xEF\xBF\xBD\xEF\xBF\xBD"},
        {"surrogate", "\xED\xA0\x80", "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"},
        {"past U+10FFFF",
         "\xF4\x90\x80\x80"
         "Z",
         "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
         "Z"},
        {"escape after a bad byte", "\xFF\"", "\xEF\xBF\xBD\\\""},
    };

    Event_History_I* const hist = rdio_case_hist();
    if (hist == NULL) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(hist, 0, sizeof(*hist));
        Event_History* item = &hist->Event_History_Items[0];
        item->systype = DSD_SYNC_NXDN_POS;
        item->event_time = (time_t)1700001500;
        item->target_id = 77U;
        DSD_SNPRINTF(item->t_name, sizeof item->t_name, "%s", cases[i].in);
        DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", cases[i].in);
        char body[4096];
        if (export_sidecar_body(hist, 1, 0, 0, body, sizeof body) != 0) {
            rc = 1;
            continue;
        }
        char want[256];
        DSD_SNPRINTF(want, sizeof want, "\"talkgroup_tag\": \"%s\",", cases[i].want);
        rc |= expect_body_has(cases[i].label, body, want);
        DSD_SNPRINTF(want, sizeof want, "\"short_name\": \"%s\",", cases[i].want);
        rc |= expect_body_has(cases[i].label, body, want);
    }
    return rc;
}

// A digital voice row with no talkgroup number, heard on @p freq_hz with access code @p kind/@p code.
static void
unnumbered_row(Event_History_I* hist, int systype, int64_t freq_hz, uint8_t kind, uint16_t code) {
    DSD_MEMSET(hist, 0, sizeof(*hist));
    Event_History* item = &hist->Event_History_Items[0];
    item->systype = (int8_t)systype;
    item->category = DSD_EVENT_CATEGORY_VOICE;
    item->gi = -1;
    item->event_start_time = (time_t)1700002000;
    item->event_time = (time_t)1700002003;
    item->freq_hz = freq_hz;
    item->access_code_kind = kind;
    item->access_code = code;
}

// Issue #888: a digital call with no talkgroup number is never exported through the entries that predate the fallback,
// nor through the new one with the fallback off: no sidecar is written, whatever the row and options hold.
static int
test_unnumbered_digital_call_without_fallback_writes_nothing(void) {
    Event_History_I* const hist = rdio_case_hist();
    dsd_opts* const opts = rdio_case_opts();
    if (hist == NULL || opts == NULL) {
        return 1;
    }
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts->rdio_system_id = 48;
    opts->rdio_unknown_talkgroup = 9000;
    opts->rdio_analog_talkgroup = 8000;
    unnumbered_row(hist, DSD_SYNC_NXDN_POS, 452312500, DSD_ACCESS_CODE_RAN, 9);
    hist->Event_History_Items[0].source_id = 1234U;
    char body[4096];
    int rc = 0;
    if (export_sidecar_body_full(opts, hist, 1, 0, 0, -1, body, sizeof body) != SIDECAR_REFUSED) {
        DSD_FPRINTF(stderr, "dsd_rdio_export_call_ex() gave an unnumbered digital call a talkgroup\n");
        rc = 1;
    }
    if (export_sidecar_body_full(opts, hist, 1, 0, 0, 0, body, sizeof body) != SIDECAR_REFUSED) {
        DSD_FPRINTF(stderr, "the fallback entry with the fallback off exported an unnumbered digital call\n");
        rc = 1;
    }
    if (dsd_rdio_call_talkgroup(opts, &hist->Event_History_Items[0], 0) != 0U) {
        DSD_FPRINTF(stderr, "dsd_rdio_call_talkgroup() gave a number with the fallback off\n");
        rc = 1;
    }
    return rc;
}

// Issue #888 D2: the number dsd_rdio_call_talkgroup() exports a call under. A decoded target always; for a digital
// voice row without one, with the fallback on, the frequency in whole kHz (rounded down, 1..2147483647), else
// --rdio-unknown-talkgroup, else none. A row of no known protocol gets no fallback number (its label would be a bare
// frequency, and an rdio-scanner v8 DirWatch could pick no system for it). Analog rows keep their own rule (#668), whose
// kHz range runs to UINT32_MAX.
static int
test_call_talkgroup_number(void) {
    static const struct {
        const char* label;
        int64_t freq_hz;
        int systype;
        uint32_t target;
        int allow;
        int option;
        uint32_t want;
        uint8_t category;
    } cases[] = {
        {"decoded target", 452312500, DSD_SYNC_DMR_BS_VOICE_POS, 77U, 1, 9000, 77U, DSD_EVENT_CATEGORY_VOICE},
        {"decoded target, fallback off", 0, DSD_SYNC_DMR_BS_VOICE_POS, 77U, 0, 0, 77U, DSD_EVENT_CATEGORY_DATA},
        {"target above the signed range", 0, DSD_SYNC_M17_STR_POS, 0xFFFFFFF0U, 1, 0, 0xFFFFFFF0U,
         DSD_EVENT_CATEGORY_VOICE},
        {"kHz", 452312500, DSD_SYNC_NXDN_POS, 0U, 1, 0, 452312U, DSD_EVENT_CATEGORY_VOICE},
        {"kHz over the option", 452312999, DSD_SYNC_NXDN_POS, 0U, 1, 9000, 452312U, DSD_EVENT_CATEGORY_VOICE},
        {"no frequency: the option", 0, DSD_SYNC_YSF_POS, 0U, 1, 9000, 9000U, DSD_EVENT_CATEGORY_VOICE},
        {"no frequency, no option", 0, DSD_SYNC_YSF_POS, 0U, 1, 0, 0U, DSD_EVENT_CATEGORY_VOICE},
        {"999 Hz: the option", 999, DSD_SYNC_DSTAR_VOICE_POS, 0U, 1, 9000, 9000U, DSD_EVENT_CATEGORY_VOICE},
        {"999 Hz, no option", 999, DSD_SYNC_DSTAR_VOICE_POS, 0U, 1, 0, 0U, DSD_EVENT_CATEGORY_VOICE},
        {"1000 Hz", 1000, DSD_SYNC_DSTAR_VOICE_POS, 0U, 1, 9000, 1U, DSD_EVENT_CATEGORY_VOICE},
        {"kHz at the signed limit", INT64_C(2147483647999), DSD_SYNC_DPMR_FS1_POS, 0U, 1, 0, 2147483647U,
         DSD_EVENT_CATEGORY_VOICE},
        {"kHz over the signed limit", INT64_C(2147483648000), DSD_SYNC_DPMR_FS1_POS, 0U, 1, 9000, 9000U,
         DSD_EVENT_CATEGORY_VOICE},
        {"negative frequency", -452312500, DSD_SYNC_DPMR_FS1_POS, 0U, 1, 0, 0U, DSD_EVENT_CATEGORY_VOICE},
        {"fallback off", 452312500, DSD_SYNC_NXDN_POS, 0U, 0, 9000, 0U, DSD_EVENT_CATEGORY_VOICE},
        {"data row", 452312500, DSD_SYNC_NXDN_POS, 0U, 1, 9000, 0U, DSD_EVENT_CATEGORY_DATA},
        {"cleared row", 452312500, DSD_SYNC_NXDN_POS, 0U, 1, 9000, 0U, DSD_EVENT_CATEGORY_UNKNOWN},
        {"no protocol word: no kHz", 452312500, DSD_SYNC_DIGITAL, 0U, 1, 9000, 0U, DSD_EVENT_CATEGORY_VOICE},
        {"no sync: no option", 0, DSD_SYNC_NONE, 0U, 1, 9000, 0U, DSD_EVENT_CATEGORY_VOICE},
        {"unknown sync type", 452312500, 45, 0U, 1, 9000, 0U, DSD_EVENT_CATEGORY_VOICE},
        {"no protocol word, numbered", 452312500, DSD_SYNC_DIGITAL, 77U, 1, 9000, 77U, DSD_EVENT_CATEGORY_VOICE},
        {"analog kHz", 154437500, DSD_SYNC_ANALOG, 0U, 0, 9000, 154437U, DSD_EVENT_CATEGORY_VOICE},
        {"analog kHz past the signed range", INT64_C(3000000000000), DSD_SYNC_ANALOG, 0U, 1, 9000, 3000000000U,
         DSD_EVENT_CATEGORY_VOICE},
        {"analog never takes the digital option", 0, DSD_SYNC_ANALOG, 0U, 1, 9000, 0U, DSD_EVENT_CATEGORY_VOICE},
    };

    Event_History_I* const hist = rdio_case_hist();
    dsd_opts* const opts = rdio_case_opts();
    if (hist == NULL || opts == NULL) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unnumbered_row(hist, cases[i].systype, cases[i].freq_hz, DSD_ACCESS_CODE_NONE, 0);
        hist->Event_History_Items[0].category = cases[i].category;
        hist->Event_History_Items[0].target_id = cases[i].target;
        DSD_MEMSET(opts, 0, sizeof(*opts));
        opts->rdio_unknown_talkgroup = cases[i].option;
        const uint32_t got = dsd_rdio_call_talkgroup(opts, &hist->Event_History_Items[0], cases[i].allow);
        if (got != cases[i].want) {
            DSD_FPRINTF(stderr, "%s: talkgroup %u, want %u\n", cases[i].label, (unsigned)got, (unsigned)cases[i].want);
            rc = 1;
        }
    }
    if (dsd_rdio_call_talkgroup(opts, NULL, 1) != 0U) {
        DSD_FPRINTF(stderr, "no row, no talkgroup\n");
        rc = 1;
    }
    unnumbered_row(hist, DSD_SYNC_NXDN_POS, 452312500, DSD_ACCESS_CODE_NONE, 0);
    if (dsd_rdio_call_talkgroup(NULL, &hist->Event_History_Items[0], 1) != 452312U) {
        DSD_FPRINTF(stderr, "no options: still the kHz\n");
        rc = 1;
    }
    unnumbered_row(hist, DSD_SYNC_NXDN_POS, 0, DSD_ACCESS_CODE_NONE, 0);
    if (dsd_rdio_call_talkgroup(NULL, &hist->Event_History_Items[0], 1) != 0U) {
        DSD_FPRINTF(stderr, "no options and no frequency: no talkgroup\n");
        rc = 1;
    }
    return rc;
}

// Issue #888 B3: dsd_rdio_call_fallback_eligible() says whether the fallback numbers a row, or would with a frequency or
// --rdio-unknown-talkgroup: a digital VOICE row with no target whose sync type names a protocol and whose
// fallback_blocked is clear, with the fallback on. The WAV close spends its once-a-run "no number" line only on such a
// row. A row the fallback may not number (an ID its decoder could not read, a YSF call not ended on a terminator; issue
// #888) is not, and dsd_rdio_call_talkgroup() gives it none, though --rdio-unknown-talkgroup is set; a talkgroup it
// does have it keeps.
static int
test_call_fallback_eligible(void) {
    static const struct {
        const char* label;
        int systype;
        uint8_t category;
        uint32_t target;
        int allow;
        uint8_t blocked;
        int want;
    } cases[] = {
        {"NXDN voice, no target", DSD_SYNC_NXDN_POS, DSD_EVENT_CATEGORY_VOICE, 0U, 1, 0U, 1},
        {"X2-TDMA voice, no target", DSD_SYNC_X2TDMA_VOICE_POS, DSD_EVENT_CATEGORY_VOICE, 0U, 1, 0U, 1},
        {"fallback off", DSD_SYNC_NXDN_POS, DSD_EVENT_CATEGORY_VOICE, 0U, 0, 0U, 0},
        {"numbered: no fallback needed", DSD_SYNC_DMR_BS_VOICE_POS, DSD_EVENT_CATEGORY_VOICE, 77U, 1, 0U, 0},
        {"data row", DSD_SYNC_NXDN_POS, DSD_EVENT_CATEGORY_DATA, 0U, 1, 0U, 0},
        {"cleared row", DSD_SYNC_NXDN_POS, DSD_EVENT_CATEGORY_UNKNOWN, 0U, 1, 0U, 0},
        {"no protocol word", DSD_SYNC_DIGITAL, DSD_EVENT_CATEGORY_VOICE, 0U, 1, 0U, 0},
        {"no sync", DSD_SYNC_NONE, DSD_EVENT_CATEGORY_VOICE, 0U, 1, 0U, 0},
        {"analog: its own rule", DSD_SYNC_ANALOG, DSD_EVENT_CATEGORY_VOICE, 0U, 1, 0U, 0},
        {"YSF voice, blocked", DSD_SYNC_YSF_POS, DSD_EVENT_CATEGORY_VOICE, 0U, 1, 1U, 0},
        {"P25 voice, blocked", DSD_SYNC_P25P2_POS, DSD_EVENT_CATEGORY_VOICE, 0U, 1, 1U, 0},
        {"P25 voice, blocked, numbered", DSD_SYNC_P25P2_POS, DSD_EVENT_CATEGORY_VOICE, 1234U, 1, 1U, 0},
    };

    dsd_opts* const opts = rdio_case_opts();

    Event_History_I* const hist = rdio_case_hist();
    if (hist == NULL) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unnumbered_row(hist, cases[i].systype, 0, DSD_ACCESS_CODE_NONE, 0);
        hist->Event_History_Items[0].category = cases[i].category;
        hist->Event_History_Items[0].target_id = cases[i].target;
        hist->Event_History_Items[0].fallback_blocked = cases[i].blocked;
        const int got = dsd_rdio_call_fallback_eligible(&hist->Event_History_Items[0], cases[i].allow);
        if (got != cases[i].want) {
            DSD_FPRINTF(stderr, "%s: eligible %d, want %d\n", cases[i].label, got, cases[i].want);
            rc = 1;
        }
        if (cases[i].blocked != 0U && opts != NULL) {
            opts->rdio_unknown_talkgroup = 9000;
            const uint32_t tg = dsd_rdio_call_talkgroup(opts, &hist->Event_History_Items[0], cases[i].allow);
            if (tg != cases[i].target) {
                DSD_FPRINTF(stderr, "%s: talkgroup %u, want %u\n", cases[i].label, (unsigned)tg,
                            (unsigned)cases[i].target);
                rc = 1;
            }
            opts->rdio_unknown_talkgroup = 0;
        }
    }
    if (dsd_rdio_call_fallback_eligible(NULL, 1) != 0) {
        DSD_FPRINTF(stderr, "no row is not eligible\n");
        rc = 1;
    }
    return rc;
}

// Issue #888 D3: a call exported under the fallback number is labelled by its service name, else its channel label,
// else "<protocol> <access code> <MHz> MHz" with the unknown parts left out; never by per-call identity text (talkgroup
// name, target or source text) nor PRIVATE/GROUP. "-" is no label at any step (B10). Access codes are spelled and
// bounded as the app's views spell them: RAN, CC and CAN in decimal from 0, NAC as three uppercase hex digits from 001
// to FFE; a code out of its range, or of a kind not known, is left out.
static int
test_fallback_call_label(void) {
    static const struct {
        const char* label;
        int systype;
        int64_t freq_hz;
        uint8_t kind;
        uint16_t code;
        int gi;
        const char* service;
        const char* channel;
        const char* want;
    } cases[] = {
        {"NXDN", DSD_SYNC_NXDN_POS, 452312500, DSD_ACCESS_CODE_RAN, 9, -1, "", "", "NXDN RAN 9 452.3125 MHz"},
        {"DMR service name", DSD_SYNC_DMR_BS_VOICE_POS, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 3, -1, "Yard Crew",
         "Base", "Yard Crew"},
        {"channel label", DSD_SYNC_DMR_BS_VOICE_POS, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 3, -1, "", "Base", "Base"},
        {"dash everywhere", DSD_SYNC_DMR_MS_VOICE, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 3, -1, "-", "-",
         "DMR CC 3 452.3125 MHz"},
        {"unaddressed NXDN group call", DSD_SYNC_NXDN_NEG, 452312500, DSD_ACCESS_CODE_RAN, 9, 0, "", "",
         "NXDN RAN 9 452.3125 MHz"},
        {"source-only private call", DSD_SYNC_DMR_BS_VOICE_NEG, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 3, 1, "", "",
         "DMR CC 3 452.3125 MHz"},
        {"valid RAN 0", DSD_SYNC_NXDN_POS, 452312500, DSD_ACCESS_CODE_RAN, 0, -1, "", "", "NXDN RAN 0 452.3125 MHz"},
        {"RAN out of range", DSD_SYNC_NXDN_POS, 452312500, DSD_ACCESS_CODE_RAN, 64, -1, "", "", "NXDN 452.3125 MHz"},
        {"P25 phase 1 NAC", DSD_SYNC_P25P1_POS, 851012500, DSD_ACCESS_CODE_NAC, 0x2A3, -1, "", "",
         "P25 NAC 2A3 851.0125 MHz"},
        {"P25 phase 2 NAC", DSD_SYNC_P25P2_NEG, 851012500, DSD_ACCESS_CODE_NAC, 0x001, -1, "", "",
         "P25 NAC 001 851.0125 MHz"},
        {"NAC out of range", DSD_SYNC_P25P1_NEG, 851012500, DSD_ACCESS_CODE_NAC, 0xFFF, -1, "", "", "P25 851.0125 MHz"},
        {"NAC 000", DSD_SYNC_P25P1_NEG, 851012500, DSD_ACCESS_CODE_NAC, 0x000, -1, "", "", "P25 851.0125 MHz"},
        {"DMR CC 0", DSD_SYNC_DMR_BS_VOICE_POS, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 0, -1, "", "",
         "DMR CC 0 452.3125 MHz"},
        {"colour code out of range", DSD_SYNC_TETRA_NDB_POS, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 64, -1, "", "",
         "TETRA 452.3125 MHz"},
        {"TETRA CC 63", DSD_SYNC_TETRA_SB_NEG, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 63, -1, "", "",
         "TETRA CC 63 452.3125 MHz"},
        {"M17 CAN 15", DSD_SYNC_M17_STR_POS, 433475000, DSD_ACCESS_CODE_CAN, 15, -1, "", "", "M17 CAN 15 433.4750 MHz"},
        {"CAN out of range", DSD_SYNC_M17_LSF_NEG, 433475000, DSD_ACCESS_CODE_CAN, 16, -1, "", "", "M17 433.4750 MHz"},
        {"unknown code kind", DSD_SYNC_DPMR_FS2_POS, 446106250, 9, 3, -1, "", "", "dPMR 446.1063 MHz"},
        {"D-STAR", DSD_SYNC_DSTAR_VOICE_POS, 145670000, DSD_ACCESS_CODE_NONE, 0, -1, "", "", "D-STAR 145.6700 MHz"},
        {"YSF", DSD_SYNC_YSF_NEG, 145670000, DSD_ACCESS_CODE_NONE, 0, -1, "", "", "YSF 145.6700 MHz"},
        {"EDACS", DSD_SYNC_EDACS_POS, 851012500, DSD_ACCESS_CODE_NONE, 0, -1, "", "", "EDACS 851.0125 MHz"},
        {"ProVoice", DSD_SYNC_PROVOICE_NEG, 851012500, DSD_ACCESS_CODE_NONE, 0, -1, "", "", "ProVoice 851.0125 MHz"},
        {"protocol only", DSD_SYNC_X2TDMA_VOICE_POS, 0, DSD_ACCESS_CODE_NONE, 0, -1, "", "", "X2-TDMA"},
        {"protocol and code only", DSD_SYNC_NXDN_POS, 0, DSD_ACCESS_CODE_RAN, 9, -1, "", "", "NXDN RAN 9"},
        {"1000 Hz", DSD_SYNC_DSTAR_VOICE_NEG, 1000, DSD_ACCESS_CODE_NONE, 0, -1, "", "", "D-STAR 0.0010 MHz"},
        {"2.4 GHz", DSD_SYNC_M17_STR_NEG, INT64_C(2400000000), DSD_ACCESS_CODE_NONE, 0, -1, "", "",
         "M17 2400.0000 MHz"},
    };

    Event_History_I* const hist = rdio_case_hist();
    dsd_opts* const opts = rdio_case_opts();
    if (hist == NULL || opts == NULL) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unnumbered_row(hist, cases[i].systype, cases[i].freq_hz, cases[i].kind, cases[i].code);
        Event_History* item = &hist->Event_History_Items[0];
        item->gi = (int8_t)cases[i].gi;
        item->source_id = 4321U;
        DSD_SNPRINTF(item->service_label, sizeof item->service_label, "%s", cases[i].service);
        DSD_SNPRINTF(item->channel_label, sizeof item->channel_label, "%s", cases[i].channel);
        // Per-call identity text, never a fallback label.
        DSD_SNPRINTF(item->t_name, sizeof item->t_name, "%s", "Some Name");
        DSD_SNPRINTF(item->tgt_str, sizeof item->tgt_str, "%s", "CQCQCQ");
        DSD_SNPRINTF(item->src_str, sizeof item->src_str, "%s", "N0CALL");
        DSD_MEMSET(opts, 0, sizeof(*opts));
        opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        opts->rdio_unknown_talkgroup = 9000;
        char body[4096];
        if (export_sidecar_body_full(opts, hist, 1, 0, 0, 1, body, sizeof body) != 0) {
            DSD_FPRINTF(stderr, "%s: export failed\n", cases[i].label);
            rc = 1;
            continue;
        }
        char want[160];
        DSD_SNPRINTF(want, sizeof want, "\"talkgroup_tag\": \"%s\",", cases[i].want);
        rc |= expect_body_has(cases[i].label, body, want);
        rc |= expect_body_has(cases[i].label, body, "\"srcList\": [{\"pos\":0,\"src\":4321}],");
    }
    return rc;
}

// Issue #888 D3: a ProVoice call an EDACS grant named (its row carries the grant's LCN) is labelled, and short-named
// when it has no system string, "EDACS", as the analog calls on its channel are, so the kHz talkgroup they share keeps
// one label, whether or not a site was decoded. A standalone ProVoice call (channel 0) keeps "ProVoice", whatever its
// system string.
static int
test_fallback_edacs_provoice_label(void) {
    static const struct {
        const char* label;
        int systype;
        uint32_t channel;
        int64_t freq_hz;
        const char* sysid;
        const char* want_tag;
        const char* want_short_name;
    } cases[] = {
        {"EDACS ProVoice", DSD_SYNC_PROVOICE_POS, 4U, 851012500, "EDACS_SITE_012_Digital_Call", "EDACS 851.0125 MHz",
         "EDACS_SITE_012_Digital_Call"},
        {"EDACS ProVoice, inverted", DSD_SYNC_PROVOICE_NEG, 7U, 851012500, "EDACS_SITE_001_Digital_Emergency_Call",
         "EDACS 851.0125 MHz", "EDACS_SITE_001_Digital_Emergency_Call"},
        {"EDACS ProVoice, no site decoded", DSD_SYNC_PROVOICE_POS, 4U, 851012500, "EDACS_SITE_000_Digital_Call",
         "EDACS 851.0125 MHz", "EDACS_SITE_000_Digital_Call"},
        {"EDACS ProVoice, no system string", DSD_SYNC_PROVOICE_NEG, 4U, 0, "", "EDACS", "EDACS"},
        {"EDACS analog all-call", DSD_SYNC_EDACS_POS, 4U, 851012500, "EDACS_SITE_012_Analog_Call", "EDACS 851.0125 MHz",
         "EDACS_SITE_012_Analog_Call"},
        {"standalone ProVoice", DSD_SYNC_PROVOICE_POS, 0U, 851012500, "EDACS_SITE_000_Analog_Call",
         "ProVoice 851.0125 MHz", "EDACS_SITE_000_Analog_Call"},
        {"standalone ProVoice, no system string", DSD_SYNC_PROVOICE_NEG, 0U, 0, "", "ProVoice", "ProVoice"},
    };

    Event_History_I* const hist = rdio_case_hist();
    dsd_opts* const opts = rdio_case_opts();
    if (hist == NULL || opts == NULL) {
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unnumbered_row(hist, cases[i].systype, cases[i].freq_hz, DSD_ACCESS_CODE_NONE, 0);
        Event_History* item = &hist->Event_History_Items[0];
        item->source_id = 4321U;
        item->channel = cases[i].channel;
        DSD_SNPRINTF(item->sysid_string, sizeof item->sysid_string, "%s", cases[i].sysid);
        DSD_MEMSET(opts, 0, sizeof(*opts));
        opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
        opts->rdio_unknown_talkgroup = 9000;
        char body[4096];
        if (export_sidecar_body_full(opts, hist, 1, 0, 0, 1, body, sizeof body) != 0) {
            DSD_FPRINTF(stderr, "%s: export failed\n", cases[i].label);
            rc = 1;
            continue;
        }
        char want[160];
        DSD_SNPRINTF(want, sizeof want, "\"talkgroup_tag\": \"%s\",", cases[i].want_tag);
        rc |= expect_body_has(cases[i].label, body, want);
        DSD_SNPRINTF(want, sizeof want, "\"short_name\": \"%s\",", cases[i].want_short_name);
        rc |= expect_body_has(cases[i].label, body, want);
    }
    return rc;
}

// Issue #888 D2/D3/D5/D8: the whole sidecar of a fallback call. The kHz number, the generated label, the row's start,
// its Hz in freq and freqList, the row's own system (never the analog override), a protocol short_name when the row has
// no system string, and the rest as for a numbered call. With no frequency (audio input) --rdio-unknown-talkgroup is
// the number, freq is 0 and freqList is left out. A numbered row exported with the fallback on is unchanged.
static int
test_fallback_call_sidecar(void) {
    Event_History_I* const hist = rdio_case_hist();
    dsd_opts* const opts = rdio_case_opts();
    if (hist == NULL || opts == NULL) {
        return 1;
    }
    DSD_MEMSET(opts, 0, sizeof(*opts));
    opts->rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts->rdio_system_id = 5;
    opts->rdio_analog_system_id = 9;
    opts->rdio_unknown_talkgroup = 9000;
    unnumbered_row(hist, DSD_SYNC_NXDN_POS, 452312500, DSD_ACCESS_CODE_RAN, 9);
    hist->Event_History_Items[0].enc = 1;
    char body[4096];
    if (export_sidecar_body_full(opts, hist, 2, 0, 0, 1, body, sizeof body) != 0) {
        DSD_FPRINTF(stderr, "fallback sidecar: export failed\n");
        return 1;
    }
    int rc = 0;
    rc |= expect_body_has("fallback sidecar", body, "\"talkgroup\": 452312,");
    rc |= expect_body_has("fallback sidecar", body, "\"talkgroup_tag\": \"NXDN RAN 9 452.3125 MHz\",");
    rc |= expect_body_has("fallback sidecar", body, "\"srcList\": [],");
    rc |= expect_body_has("fallback sidecar", body, "\"freq\": 452312500,");
    rc |= expect_body_has("fallback sidecar", body, "\"freqList\": [{\"pos\": 0, \"freq\": 452312500}],");
    rc |= expect_body_has("fallback sidecar", body, "\"system\": 5,");
    rc |= expect_body_has("fallback sidecar", body, "\"short_name\": \"NXDN\",");
    rc |= expect_body_has("fallback sidecar", body, "\"encrypted\": true");
    rc |= expect_sidecar_times("fallback sidecar", body, 1700002000LL, 1700002002LL);
    if (hist->Event_History_Items[0].target_id != 0U) {
        DSD_FPRINTF(stderr, "fallback sidecar: the export number was stored on the row\n");
        rc = 1;
    }

    unnumbered_row(hist, DSD_SYNC_YSF_POS, 0, DSD_ACCESS_CODE_NONE, 0);
    DSD_SNPRINTF(hist->Event_History_Items[0].sysid_string, sizeof hist->Event_History_Items[0].sysid_string, "%s",
                 "YSF_DG-ID_0");
    if (export_sidecar_body_full(opts, hist, 1, 0, 0, 1, body, sizeof body) != 0) {
        DSD_FPRINTF(stderr, "audio-input fallback sidecar: export failed\n");
        return 1;
    }
    rc |= expect_body_has("audio-input fallback", body, "\"talkgroup\": 9000,");
    rc |= expect_body_has("audio-input fallback", body, "\"talkgroup_tag\": \"YSF\",");
    rc |= expect_body_has("audio-input fallback", body, "\"freq\": 0,");
    rc |= expect_body_has("audio-input fallback", body, "\"short_name\": \"YSF_DG-ID_0\",");
    if (strstr(body, "freqList") != NULL) {
        DSD_FPRINTF(stderr, "audio-input fallback: freqList without a usable frequency\n%s\n", body);
        rc = 1;
    }

    opts->rdio_unknown_talkgroup = 0;
    if (export_sidecar_body_full(opts, hist, 1, 0, 0, 1, body, sizeof body) != SIDECAR_REFUSED) {
        DSD_FPRINTF(stderr, "no frequency and no option: a fallback call was exported\n");
        rc = 1;
    }

    unnumbered_row(hist, DSD_SYNC_DMR_BS_VOICE_POS, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 3);
    hist->Event_History_Items[0].target_id = 77U;
    hist->Event_History_Items[0].gi = 0;
    if (export_sidecar_body_full(opts, hist, 1, 0, 0, 1, body, sizeof body) != 0) {
        DSD_FPRINTF(stderr, "numbered row with the fallback on: export failed\n");
        return 1;
    }
    rc |= expect_body_has("numbered row, fallback on", body, "\"talkgroup\": 77,");
    rc |= expect_body_has("numbered row, fallback on", body, "\"talkgroup_tag\": \"GROUP\",");
    return rc;
}

static int
test_api_shutdown_drains_queue(void) {
#ifndef USE_CURL
    dsd_rdio_upload_shutdown();
    dsd_rdio_upload_shutdown();
    return 0;
#else
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_api_shutdown")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "call_api.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "call_api.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_pcm16_mono_wav(wav_path, 8000, 1) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    opts->rdio_system_id = 48;
    opts->rdio_upload_timeout_ms = 100;
    opts->rdio_upload_retries = 1;
    DSD_SNPRINTF(opts->rdio_api_url, sizeof(opts->rdio_api_url), "%s", "http://127.0.0.1:1");
    opts->rdio_api_url[sizeof(opts->rdio_api_url) - 1] = '\0';
    DSD_SNPRINTF(opts->rdio_api_key, sizeof(opts->rdio_api_key), "%s", "test-key");
    opts->rdio_api_key[sizeof(opts->rdio_api_key) - 1] = '\0';

    hist->Event_History_Items[0].event_start_time = (time_t)1699999985;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    int rc = 0;
    if (dsd_rdio_export_call(opts, hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "api enqueue path failed\n");
        rc = 1;
    }

    dsd_rdio_upload_shutdown();
    dsd_rdio_upload_shutdown();

    char body[4096];
    if (read_file(json_path, body, sizeof(body)) != 0) {
        DSD_FPRINTF(stderr, "sidecar missing after API shutdown drain\n");
        rc = 1;
    } else {
        rc |= expect_sidecar_times("API shutdown drain sidecar", body, 1699999985LL, 1699999986LL);
    }

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    free(hist);
    free(opts);
    return rc;
#endif
}

/* One successful API-only upload with delete-after-upload on, in a run that names a recording journal when
   @p journaled; the WAV is gone after it unless @p journaled (issue #708: the journal's owner keeps it). */
static int
api_upload_with_delete_after(int journaled) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    (void)journaled;
    return 0;
#else
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_api_delete")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "call_api_delete.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "call_api_delete.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_pcm16_mono_wav(wav_path, 8000, 1) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    rdio_test_http_server server;
    char api_url[128] = {0};
    if (rdio_test_http_server_start(&server, api_url, sizeof(api_url)) != 0) {
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    opts->rdio_mode = DSD_RDIO_MODE_API;
    opts->rdio_system_id = 48;
    opts->rdio_upload_timeout_ms = 5000;
    opts->rdio_upload_retries = 1;
    opts->rdio_api_delete_after_upload = 1;
    if (journaled) {
        (void)dsd_test_path_join(opts->wav_journal_file, sizeof(opts->wav_journal_file), dir_template,
                                 "recordings.jsonl");
    }
    DSD_SNPRINTF(opts->rdio_api_url, sizeof(opts->rdio_api_url), "%s", api_url);
    opts->rdio_api_url[sizeof(opts->rdio_api_url) - 1] = '\0';
    DSD_SNPRINTF(opts->rdio_api_key, sizeof(opts->rdio_api_key), "%s", "test-key");
    opts->rdio_api_key[sizeof(opts->rdio_api_key) - 1] = '\0';

    hist->Event_History_Items[0].event_start_time = (time_t)1699999985;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    int rc = 0;
    if (dsd_rdio_export_call(opts, hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "api enqueue path failed\n");
        rc = 1;
    }

    dsd_rdio_upload_shutdown();

    if (dsd_thread_join(server.thread) != 0) {
        DSD_FPRINTF(stderr, "server thread join failed\n");
        rc = 1;
    }
    if (server.rc != 0) {
        DSD_FPRINTF(stderr, "test HTTP server did not receive a complete upload\n");
        rc = 1;
    }
    // API-only mode leaves no sidecar on disk; the uploaded meta part is the call's record.
    rc |= expect_sidecar_times("API upload meta part", server.meta, 1699999985LL, 1699999986LL);
    if (journaled && !file_exists(wav_path)) {
        DSD_FPRINTF(stderr, "a journaled run's WAV should be kept after its API upload\n");
        rc = 1;
    } else if (!journaled && file_exists(wav_path)) {
        DSD_FPRINTF(stderr, "WAV should be deleted after successful API upload\n");
        rc = 1;
    }
    (void)remove(wav_path);
    if (file_exists(json_path)) {
        DSD_FPRINTF(stderr, "metadata should be deleted after API-only upload\n");
        (void)remove(json_path);
        rc = 1;
    }

    remove_empty_dir(dir_template);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

static int
test_api_delete_after_successful_upload(void) {
    return api_upload_with_delete_after(0);
}

static int
test_api_journaled_upload_keeps_the_wav(void) {
    return api_upload_with_delete_after(1);
}

// With DirWatch and API both on, the upload's meta part is the sidecar DirWatch leaves on disk, byte for byte, so
// rdio-scanner files the call at its start whichever way it arrives.
static int
test_api_meta_part_matches_dirwatch_sidecar(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_api_meta")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "call_api_meta.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "call_api_meta.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_pcm16_mono_wav(wav_path, 8000, 2) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    rdio_test_http_server server;
    char api_url[128] = {0};
    if (rdio_test_http_server_start(&server, api_url, sizeof(api_url)) != 0) {
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    opts->rdio_system_id = 48;
    opts->rdio_upload_timeout_ms = 5000;
    opts->rdio_upload_retries = 1;
    DSD_SNPRINTF(opts->rdio_api_url, sizeof(opts->rdio_api_url), "%s", api_url);
    DSD_SNPRINTF(opts->rdio_api_key, sizeof(opts->rdio_api_key), "%s", "test-key");

    hist->Event_History_Items[0].event_start_time = (time_t)1699999970;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    int rc = 0;
    if (dsd_rdio_export_call(opts, hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "both-mode export failed\n");
        rc = 1;
    }

    dsd_rdio_upload_shutdown();

    if (dsd_thread_join(server.thread) != 0) {
        DSD_FPRINTF(stderr, "server thread join failed\n");
        rc = 1;
    }
    if (server.rc != 0) {
        DSD_FPRINTF(stderr, "test HTTP server did not receive a complete upload\n");
        rc = 1;
    }

    char body[4096];
    if (read_file(json_path, body, sizeof(body)) != 0) {
        DSD_FPRINTF(stderr, "DirWatch sidecar missing in both mode\n");
        rc = 1;
    } else {
        rc |= expect_sidecar_times("both-mode DirWatch sidecar", body, 1699999970LL, 1699999972LL);
        if (strcmp(server.meta, body) != 0) {
            DSD_FPRINTF(stderr, "uploaded meta part differs from the DirWatch sidecar\nmeta:\n%s\nsidecar:\n%s\n",
                        server.meta, body);
            rc = 1;
        }
    }

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

static int
test_api_upload_does_not_follow_redirect(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_redirect")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "call_redirect.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "call_redirect.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return 1;
    }

    if (write_pcm16_mono_wav(wav_path, 8000, 1) != 0) {
        remove_empty_dir(dir_template);
        return 1;
    }

    /* Destination server must stay idle when libcurl receives the redirect. */
    rdio_test_http_server destination_server;
    char destination_url[128] = {0};
    if (rdio_test_http_server_start_ex(&destination_server, destination_url, sizeof(destination_url), NULL, 1, 3000U)
        != 0) {
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        return 1;
    }

    char redirect_response[512] = {0};
    DSD_SNPRINTF(redirect_response, sizeof(redirect_response),
                 "HTTP/1.1 307 Temporary Redirect\r\n"
                 "Location: %s/redirected\r\n"
                 "Content-Length: 0\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 destination_url);

    /* Redirect server is the configured endpoint and should receive exactly one upload. */
    rdio_test_http_server redirect_server;
    char redirect_url[128] = {0};
    if (rdio_test_http_server_start_ex(&redirect_server, redirect_url, sizeof(redirect_url), redirect_response, 0,
                                       5000U)
        != 0) {
        (void)dsd_thread_join(destination_server.thread);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        dsd_socket_cleanup();
        return 1;
    }

    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist) {
        DSD_FPRINTF(stderr, "allocation failed\n");
        free(hist);
        free(opts);
        (void)dsd_thread_join(redirect_server.thread);
        (void)dsd_thread_join(destination_server.thread);
        (void)remove(wav_path);
        remove_empty_dir(dir_template);
        dsd_socket_cleanup();
        return 1;
    }

    opts->rdio_mode = DSD_RDIO_MODE_API;
    opts->rdio_system_id = 48;
    opts->rdio_upload_timeout_ms = 5000;
    opts->rdio_upload_retries = 1;
    DSD_SNPRINTF(opts->rdio_api_url, sizeof(opts->rdio_api_url), "%s", redirect_url);
    opts->rdio_api_url[sizeof(opts->rdio_api_url) - 1] = '\0';
    DSD_SNPRINTF(opts->rdio_api_key, sizeof(opts->rdio_api_key), "%s", "test-key");
    opts->rdio_api_key[sizeof(opts->rdio_api_key) - 1] = '\0';

    hist->Event_History_Items[0].event_start_time = (time_t)1699999985;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    /* Queue one API upload, then drain the worker before checking server observations. */
    int rc = 0;
    if (dsd_rdio_export_call(opts, hist, wav_path, 0) != 0) {
        DSD_FPRINTF(stderr, "api enqueue path failed\n");
        rc = 1;
    }

    dsd_rdio_upload_shutdown();

    if (dsd_thread_join(redirect_server.thread) != 0) {
        DSD_FPRINTF(stderr, "redirect server thread join failed\n");
        rc = 1;
    }
    if (dsd_thread_join(destination_server.thread) != 0) {
        DSD_FPRINTF(stderr, "destination server thread join failed\n");
        rc = 1;
    }
    /* The initial upload may fail after the 307, but it must not be replayed elsewhere. */
    if (redirect_server.rc != 0 || !redirect_server.saw_request) {
        DSD_FPRINTF(stderr, "redirect server did not receive the initial upload\n");
        rc = 1;
    }
    if (destination_server.rc != 0 || destination_server.saw_request) {
        DSD_FPRINTF(stderr, "redirect destination received an upload despite redirects being disabled\n");
        rc = 1;
    }

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
#define RDIO_CANCEL_JOBS_MAX 4

// The jobs one cancel test queues: a WAV and its sidecar per call, in one temporary directory.
typedef struct {
    char dir[DSD_TEST_PATH_MAX];
    int count;
    char wav[RDIO_CANCEL_JOBS_MAX][DSD_TEST_PATH_MAX];
    char json[RDIO_CANCEL_JOBS_MAX][DSD_TEST_PATH_MAX];
} rdio_cancel_jobs;

// An endpoint that accepts connections and never answers: the kernel completes the handshake from the listen backlog
// and buffers the request, and nothing ever reads it, so every attempt runs to its timeout.
static int
rdio_test_stalled_server_start(dsd_socket_t* out_sock, char* out_api_url, size_t out_api_url_size) {
    *out_sock = DSD_INVALID_SOCKET;
    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "socket init failed\n");
        return 1;
    }
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sock == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "socket create failed\n");
        return 1;
    }
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t addr_len = (socklen_t)sizeof(addr);
    if (dsd_socket_bind(sock, (const struct sockaddr*)&addr, (int)sizeof(addr)) != 0 || dsd_socket_listen(sock, 16) != 0
        || getsockname(sock, (struct sockaddr*)&addr, &addr_len) != 0) {
        DSD_FPRINTF(stderr, "stalled server setup failed\n");
        (void)dsd_socket_close(sock);
        return 1;
    }
    int n = DSD_SNPRINTF(out_api_url, out_api_url_size, "http://127.0.0.1:%u", (unsigned int)ntohs(addr.sin_port));
    if (n < 0 || (size_t)n >= out_api_url_size) {
        (void)dsd_socket_close(sock);
        return 1;
    }
    *out_sock = sock;
    return 0;
}

static void
rdio_cancel_opts(dsd_opts* opts, const char* api_url, int timeout_ms, int retries) {
    opts->rdio_mode = DSD_RDIO_MODE_API;
    opts->rdio_system_id = 48;
    opts->rdio_upload_timeout_ms = timeout_ms;
    opts->rdio_upload_retries = retries;
    opts->rdio_api_delete_after_upload = 1;
    DSD_SNPRINTF(opts->rdio_api_url, sizeof(opts->rdio_api_url), "%s", api_url);
    opts->rdio_api_url[sizeof(opts->rdio_api_url) - 1] = '\0';
    DSD_SNPRINTF(opts->rdio_api_key, sizeof(opts->rdio_api_key), "%s", "test-key");
    opts->rdio_api_key[sizeof(opts->rdio_api_key) - 1] = '\0';
}

// Write @p count one-second calls and hand each to the exporter, which queues its API upload.
static int
rdio_cancel_jobs_queue(rdio_cancel_jobs* jobs, const char* prefix, int count, const dsd_opts* opts) {
    DSD_MEMSET(jobs, 0, sizeof(*jobs));
    if (count > RDIO_CANCEL_JOBS_MAX || !dsd_test_mkdtemp(jobs->dir, sizeof(jobs->dir), prefix)) {
        DSD_FPRINTF(stderr, "cancel jobs: mkdtemp failed\n");
        return 1;
    }
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!hist) {
        return 1;
    }
    hist->Event_History_Items[0].event_start_time = (time_t)1699999985;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;
    int rc = 0;
    for (int i = 0; i < count && rc == 0; i++) {
        char leaf[64];
        DSD_SNPRINTF(leaf, sizeof(leaf), "call_cancel_%d.wav", i);
        char json_leaf[64];
        DSD_SNPRINTF(json_leaf, sizeof(json_leaf), "call_cancel_%d.json", i);
        if (dsd_test_path_join(jobs->wav[i], sizeof(jobs->wav[i]), jobs->dir, leaf) != 0
            || dsd_test_path_join(jobs->json[i], sizeof(jobs->json[i]), jobs->dir, json_leaf) != 0
            || write_pcm16_mono_wav(jobs->wav[i], 8000, 1) != 0) {
            rc = 1;
            break;
        }
        jobs->count = i + 1;
        if (dsd_rdio_export_call(opts, hist, jobs->wav[i], 0) != 0) {
            DSD_FPRINTF(stderr, "cancel jobs: enqueue %d failed\n", i);
            rc = 1;
        }
    }
    free(hist);
    return rc;
}

// Every queued call's WAV and sidecar must still be there (want_present) or must all be gone.
static int
rdio_cancel_jobs_expect_files(const rdio_cancel_jobs* jobs, int want_present, const char* label) {
    int rc = 0;
    for (int i = 0; i < jobs->count; i++) {
        if (file_exists(jobs->wav[i]) != want_present || file_exists(jobs->json[i]) != want_present) {
            DSD_FPRINTF(stderr, "%s: call %d files %s\n", label, i, want_present ? "were deleted" : "were kept");
            rc = 1;
        }
    }
    return rc;
}

static void
rdio_cancel_jobs_remove(rdio_cancel_jobs* jobs) {
    for (int i = 0; i < jobs->count; i++) {
        (void)remove(jobs->wav[i]);
        (void)remove(jobs->json[i]);
    }
    if (jobs->dir[0] != '\0') {
        remove_empty_dir(jobs->dir);
    }
}

static DSD_THREAD_RETURN_TYPE
rdio_test_cancel_later_thread(void* arg) {
    const unsigned int* delay_ms = (const unsigned int*)arg;
    dsd_sleep_ms(*delay_ms);
    dsd_rdio_upload_cancel();
    DSD_THREAD_RETURN;
}
#endif

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
// Upload jobs released, and how many of them still held a byte of their API key or URL copy when they were freed.
static int g_rdio_jobs_released = 0;
static int g_rdio_jobs_released_with_secrets = 0;

static void
rdio_count_job_release(const char* api_key, size_t api_key_size, const char* api_url, size_t api_url_size) {
    g_rdio_jobs_released++;
    int left = 0;
    for (size_t i = 0; i < api_key_size; ++i) {
        left |= api_key[i] != '\0';
    }
    for (size_t i = 0; i < api_url_size; ++i) {
        left |= api_url[i] != '\0';
    }
    g_rdio_jobs_released_with_secrets += left;
}
#endif

// The user's Stop against a stalled endpoint: cancel, then the cleanup's shutdown. Without the cancel the drain would
// run 4 jobs x 3 attempts x 3 s; with it the attempt in flight is aborted and the rest are never tried, well inside one
// attempt's timeout, and nothing is deleted. Every job, the one in flight and the ones still queued at the stop, has
// its copies of the API key and URL wiped before it is freed.
static int
test_api_cancel_before_shutdown_skips_backlog(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    return 0;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0) {
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 3000, 3);

    g_rdio_jobs_released = g_rdio_jobs_released_with_secrets = 0;
    dsd_rdio_upload_set_release_observer(rdio_count_job_release);
    rdio_cancel_jobs jobs;
    int rc = rdio_cancel_jobs_queue(&jobs, "dsdneo_rdio_cancel_before", 4, opts);
    // Let the worker get into its first transfer, so the cancel has one to abort.
    dsd_sleep_ms(300U);

    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    const uint64_t elapsed = dsd_realtime_mono_ms() - t0;
    dsd_rdio_upload_set_release_observer(NULL);
    if (elapsed >= 2000U) {
        DSD_FPRINTF(stderr, "cancel before shutdown: shutdown took %llu ms\n", (unsigned long long)elapsed);
        rc = 1;
    }
    if (g_rdio_jobs_released != 4 || g_rdio_jobs_released_with_secrets != 0) {
        DSD_FPRINTF(stderr, "cancel before shutdown: %d of 4 jobs released, %d still holding the API key or URL\n",
                    g_rdio_jobs_released, g_rdio_jobs_released_with_secrets);
        rc = 1;
    }
    rc |= rdio_cancel_jobs_expect_files(&jobs, 1, "cancel before shutdown");

    rdio_cancel_jobs_remove(&jobs);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(opts);
    return rc;
#endif
}

// Stop pressed while a normal end's drain is already running: the cancel from another thread cuts the drain short.
static int
test_api_cancel_during_drain_returns_promptly(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0) {
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 3000, 2);

    rdio_cancel_jobs jobs;
    int rc = rdio_cancel_jobs_queue(&jobs, "dsdneo_rdio_cancel_drain", 3, opts);

    static unsigned int delay_ms = 400U;
    dsd_thread_t canceller;
    const int canceller_started = dsd_thread_create(&canceller, rdio_test_cancel_later_thread, &delay_ms) == 0;
    if (!canceller_started) {
        DSD_FPRINTF(stderr, "cancel during drain: thread create failed\n");
        rc = 1;
        dsd_rdio_upload_cancel();
    }
    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_rdio_upload_shutdown();
    const uint64_t elapsed = dsd_realtime_mono_ms() - t0;
    if (canceller_started && dsd_thread_join(canceller) != 0) {
        rc = 1;
    }
    if (elapsed >= 2400U) {
        DSD_FPRINTF(stderr, "cancel during drain: shutdown took %llu ms\n", (unsigned long long)elapsed);
        rc = 1;
    }
    rc |= rdio_cancel_jobs_expect_files(&jobs, 1, "cancel during drain");

    rdio_cancel_jobs_remove(&jobs);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(opts);
    return rc;
#endif
}

// The user's Stop while an attempt is stuck in its TLS handshake (an https endpoint that accepts and never answers).
// libcurl calls no progress callback in that phase, so with curl_easy_perform() the abort waited out the attempt's
// whole timeout (10 s here); it must land within about a second.
static int
test_api_cancel_during_tls_handshake(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char http_url[128] = {0};
    char api_url[160] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts || rdio_test_stalled_server_start(&stalled, http_url, sizeof(http_url)) != 0) {
        free(opts);
        return 1;
    }
    DSD_SNPRINTF(api_url, sizeof(api_url), "https://%s", http_url + strlen("http://"));
    rdio_cancel_opts(opts, api_url, 10000, 1);

    rdio_cancel_jobs jobs;
    int rc = rdio_cancel_jobs_queue(&jobs, "dsdneo_rdio_cancel_tls", 1, opts);
    // Let the worker connect and send its ClientHello, so the cancel meets a stalled handshake.
    dsd_sleep_ms(500U);

    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    const uint64_t elapsed = dsd_realtime_mono_ms() - t0;
    if (elapsed >= 1500U) {
        DSD_FPRINTF(stderr, "cancel during TLS handshake: shutdown took %llu ms\n", (unsigned long long)elapsed);
        rc = 1;
    }
    rc |= rdio_cancel_jobs_expect_files(&jobs, 1, "cancel during TLS handshake");

    rdio_cancel_jobs_remove(&jobs);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(opts);
    return rc;
#endif
}

// A cancel with no worker running yet still covers the uploads queued after it, such as the final call that cleanup
// closes: the worker starts for it and sends nothing.
static int
test_api_cancel_covers_uploads_queued_after_it(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0) {
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 3000, 3);

    dsd_rdio_upload_cancel();
    rdio_cancel_jobs jobs;
    int rc = rdio_cancel_jobs_queue(&jobs, "dsdneo_rdio_cancel_after", 2, opts);
    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_rdio_upload_shutdown();
    const uint64_t elapsed = dsd_realtime_mono_ms() - t0;
    if (elapsed >= 2000U) {
        DSD_FPRINTF(stderr, "uploads queued after cancel: shutdown took %llu ms\n", (unsigned long long)elapsed);
        rc = 1;
    }
    rc |= rdio_cancel_jobs_expect_files(&jobs, 1, "uploads queued after cancel");

    rdio_cancel_jobs_remove(&jobs);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(opts);
    return rc;
#endif
}

// A normal end drains against a healthy server: every job is uploaded and deleted as configured. Run after the cancel
// tests, it also shows their shutdowns cleared the cancel; a cancel cleared by dsd_rdio_upload_cancel_reset() before
// the session starts does not hold back its uploads either.
static int
test_api_drain_uploads_every_job_after_cancel_clears(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_cancel_reset();
    return 0;
#else
    int rc = 0;
    for (int pass = 0; pass < 2; pass++) {
        rdio_test_http_server server;
        char api_url[128] = {0};
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
        if (!opts || rdio_test_http_server_start_n(&server, api_url, sizeof(api_url), NULL, 0, 5000U, 3) != 0) {
            free(opts);
            return 1;
        }
        rdio_cancel_opts(opts, api_url, 5000, 1);
        if (pass == 1) {
            // The leak an embedding host guards against: a cancel that landed after the last session's shutdown.
            dsd_rdio_upload_cancel();
            dsd_rdio_upload_cancel_reset();
        }

        rdio_cancel_jobs jobs;
        rc |= rdio_cancel_jobs_queue(&jobs, "dsdneo_rdio_drain_all", 3, opts);
        dsd_rdio_upload_shutdown();
        if (dsd_thread_join(server.thread) != 0 || server.rc != 0) {
            DSD_FPRINTF(stderr, "drain pass %d: server saw %d of 3 uploads\n", pass, server.requests_done);
            rc = 1;
        }
        rc |= rdio_cancel_jobs_expect_files(
            &jobs, 0, pass == 0 ? "drain after shutdown cleared cancel" : "drain after cancel reset");
        rdio_cancel_jobs_remove(&jobs);
        dsd_socket_cleanup();
        free(opts);
    }
    return rc;
#endif
}

// Issue #708: the name rule behind dsd_rdio_upload_holds(), with POSIX and Windows rules on any host: the final path
// components must be equal, whatever the folders; Windows also splits at a backslash and ignores ASCII case.
static int
test_upload_holds_name_rule(void) {
    static const struct {
        const char* job;
        const char* path;
        int win;
        int want;
        const char* what;
    } cases[] = {
        {"/rec/a/c.wav", "/elsewhere/c.wav", 0, 1, "posix: another folder, same name"},
        {"/rec/a/c.wav", "c.wav", 0, 1, "posix: a bare name"},
        {"/rec/a/c.wav", "/rec/a/d.wav", 0, 0, "posix: another name"},
        {"/rec/a/c.wav", "/rec/a/C.wav", 0, 0, "posix: case counts"},
        {"/rec/a\\c.wav", "c.wav", 0, 0, "posix: a backslash is part of the name"},
        {"/rec/a\\c.wav", "/x/a\\c.wav", 0, 1, "posix: the name with its backslash"},
        {"/rec/a/c.wav", "/rec/a/", 0, 0, "posix: a folder names no file"},
        {"/rec/a/c.wav", "", 0, 0, "posix: empty"},
        {"/rec/a/", "/rec/a/", 0, 0, "posix: two folders are not one file"},
        {"C:\\rec\\c.wav", "D:/other/c.wav", 1, 1, "windows: either separator"},
        {"C:\\rec\\c.wav", "c:\\REC\\C.WAV", 1, 1, "windows: without ASCII case"},
        {"C:\\rec\\c.wav", "c.wav", 1, 1, "windows: a bare name"},
        {"C:\\rec\\c.wav", "C:\\rec\\d.wav", 1, 0, "windows: another name"},
        {"C:\\rec\\c.wav", "C:\\rec\\", 1, 0, "windows: a folder names no file"},
        {"C:\\rec\\\xC3\xA9.wav", "\xC3\x89.wav", 1, 0, "windows: case beyond ASCII counts"},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const int got = dsd_rdio_upload_names_same_file(cases[i].job, cases[i].path, cases[i].win);
        if (got != cases[i].want) {
            DSD_FPRINTF(stderr, "holds name rule: %s: got %d, want %d\n", cases[i].what, got, cases[i].want);
            rc = 1;
        }
    }
    if (dsd_rdio_upload_names_same_file(NULL, "c.wav", 0) != 0
        || dsd_rdio_upload_names_same_file("c.wav", NULL, 1) != 0) {
        DSD_FPRINTF(stderr, "holds name rule: NULL names no file\n");
        rc = 1;
    }
    return rc;
}

// Issue #708: the app's recording store never deletes a WAV the upload queue still owns. Against a blocked uploader,
// the file in flight and every queued one are held through more than a full queue of later publications, the ones the
// full queue refused are not, and after the session's shutdown nothing is.
#define RDIO_HOLDS_LATER 140

static int
test_api_upload_holds_queued_and_in_flight_files(void) {
    int rc = 0;
    if (dsd_rdio_upload_holds(NULL) != 0 || dsd_rdio_upload_holds("") != 0 || dsd_rdio_upload_holds("/") != 0) {
        DSD_FPRINTF(stderr, "holds: no file name held\n");
        rc = 1;
    }
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    if (dsd_rdio_upload_holds("/tmp/call.wav") != 0) {
        DSD_FPRINTF(stderr, "holds: nothing is held without an upload queue\n");
        rc = 1;
    }
    return rc;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    static char wav[RDIO_HOLDS_LATER + 1][DSD_TEST_PATH_MAX];
    static int queued[RDIO_HOLDS_LATER + 1];
    char dir[DSD_TEST_PATH_MAX] = {0};
    if (!opts || !hist || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0
        || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_holds")) {
        free(hist);
        free(opts);
        return 1;
    }
    // One attempt that outlasts the test: the worker stays inside the first upload until the cancel aborts it.
    rdio_cancel_opts(opts, api_url, 30000, 1);
    hist->Event_History_Items[0].event_start_time = (time_t)1699999985;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    int count = 0;
    for (int i = 0; i <= RDIO_HOLDS_LATER && rc == 0; i++) {
        char leaf[64];
        DSD_SNPRINTF(leaf, sizeof(leaf), "call_hold_%03d.wav", i);
        if (dsd_test_path_join(wav[i], sizeof(wav[i]), dir, leaf) != 0 || write_pcm16_mono_wav(wav[i], 8000, 1) != 0) {
            rc = 1;
            break;
        }
        count = i + 1;
        queued[i] = 0;
        (void)dsd_rdio_export_call_ex(opts, hist, wav[i], 0, 0, &queued[i]);
        // The worker takes the first job before the rest are published: its connection waits in the stalled backlog.
        if (i == 0 && (!queued[0] || rdio_test_wait_for_connection(stalled, 5000U) <= 0)) {
            DSD_FPRINTF(stderr, "holds: the first upload never started\n");
            rc = 1;
        }
    }

    int held_count = 0;
    for (int i = 0; i < count; i++) {
        const int held = dsd_rdio_upload_holds(wav[i]);
        held_count += held;
        if (held != queued[i]) {
            DSD_FPRINTF(stderr, "holds: publication %d held %d, queued %d\n", i, held, queued[i]);
            rc = 1;
        }
    }
    // The one in flight and a full queue behind it; the full queue refused every later one.
    if (count == RDIO_HOLDS_LATER + 1 && (held_count != 129 || !queued[0] || !queued[128] || queued[129])) {
        DSD_FPRINTF(stderr, "holds: %d of %d held, want the one in flight and 128 queued\n", held_count, count);
        rc = 1;
    }
    char other[DSD_TEST_PATH_MAX];
    char unpublished[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(other, sizeof(other), "/elsewhere/./sub", "call_hold_000.wav") != 0
        || dsd_test_path_join(unpublished, sizeof(unpublished), dir, "call_hold_999.wav") != 0
        || dsd_rdio_upload_holds(other) != 1 || dsd_rdio_upload_holds("call_hold_005.wav") != 1
        || dsd_rdio_upload_holds(unpublished) != 0) {
        DSD_FPRINTF(stderr, "holds: a job names its file by its file name, whatever the folder's spelling\n");
        rc = 1;
    }

    // The user's Stop: the session's queue is dropped with it, and its files are kept and no longer held.
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    for (int i = 0; i < count; i++) {
        if (dsd_rdio_upload_holds(wav[i]) != 0 || !file_exists(wav[i])) {
            DSD_FPRINTF(stderr, "holds: publication %d after the session ended: held or deleted\n", i);
            rc = 1;
        }
        char json[DSD_TEST_PATH_MAX];
        DSD_SNPRINTF(json, sizeof(json), "%.*s.json", (int)(strlen(wav[i]) - 4U), wav[i]);
        (void)remove(wav[i]);
        (void)remove(json);
    }
    remove_empty_dir(dir);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

// A successful upload releases its file only after the delete that follows it: by the time it is no longer held, it is
// gone, so the store never sees an unheld file the worker is about to delete.
static int
test_api_upload_holds_until_the_delete_is_over(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    rdio_test_http_server server;
    char api_url[128] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    if (!opts || rdio_test_http_server_start(&server, api_url, sizeof(api_url)) != 0) {
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 5000, 1);
    rdio_cancel_jobs jobs;
    int rc = rdio_cancel_jobs_queue(&jobs, "dsdneo_rdio_holds_delete", 1, opts);
    if (rc == 0 && dsd_rdio_upload_holds(jobs.wav[0]) != 1 && file_exists(jobs.wav[0])) {
        DSD_FPRINTF(stderr, "holds: a queued upload's file is not held\n");
        rc = 1;
    }
    const uint64_t t0 = dsd_realtime_mono_ms();
    while (rc == 0 && dsd_rdio_upload_holds(jobs.wav[0]) != 0) {
        if (dsd_realtime_mono_ms() - t0 > 5000U) {
            DSD_FPRINTF(stderr, "holds: the uploaded file was never released\n");
            rc = 1;
            break;
        }
        dsd_sleep_ms(1U);
    }
    if (rc == 0 && file_exists(jobs.wav[0])) {
        DSD_FPRINTF(stderr, "holds: the file was released before the delete after its upload\n");
        rc = 1;
    }
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.rc != 0) {
        rc = 1;
    }
    rdio_cancel_jobs_remove(&jobs);
    dsd_socket_cleanup();
    free(opts);
    return rc;
#endif
}

// Issue #668 and the #708 contract, for an NFM call's upload. One analog call with no base system (0) and an analog
// system of 5, delete-after-upload on, against a server that answers @p fail ? 500 : 200: the upload is not refused
// for the missing base system, its form "system" part and its meta part both name system 5 and the kHz talkgroup, and
// with DirWatch on the meta part is the sidecar left on disk. The WAV is deleted only after a successful API-only
// upload in a run with no journal; a journal's owner keeps it, a failed upload keeps it, and DirWatch keeps it.
// Afterwards nothing is held.
static int
analog_api_upload(const char* label, int mode, int journaled, int fail, int want_wav_kept) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    (void)label;
    (void)mode;
    (void)journaled;
    (void)fail;
    (void)want_wav_kept;
    return 0;
#else
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_export_analog_api")
        || dsd_test_path_join(wav_path, sizeof(wav_path), dir, "nfm_api.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir, "nfm_api.json") != 0
        || write_pcm16_mono_wav(wav_path, 8000, 1) != 0) {
        DSD_FPRINTF(stderr, "%s: setup failed\n", label);
        return 1;
    }
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    rdio_test_http_server server;
    char api_url[128] = {0};
    const char* response =
        fail ? "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" : NULL;
    if (!opts || !hist || rdio_test_http_server_start_ex(&server, api_url, sizeof(api_url), response, 0, 5000U) != 0) {
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 5000, 1);
    opts->rdio_mode = mode;
    opts->rdio_system_id = 0;
    opts->rdio_analog_system_id = 5;
    if (journaled) {
        (void)dsd_test_path_join(opts->wav_journal_file, sizeof(opts->wav_journal_file), dir, "recordings.jsonl");
    }
    analog_row(hist, 154437500);

    int rc = 0;
    int queued = 0;
    if (dsd_rdio_export_call_ex(opts, hist, wav_path, 0, 0, &queued) != 0 || !queued) {
        DSD_FPRINTF(stderr, "%s: the upload was not queued\n", label);
        rc = 1;
    }
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.rc != 0) {
        DSD_FPRINTF(stderr, "%s: the server did not receive a complete upload\n", label);
        rc = 1;
    }
    if (strcmp(server.system, "5") != 0) {
        DSD_FPRINTF(stderr, "%s: form system part '%s', want 5\n", label, server.system);
        rc = 1;
    }
    rc |= expect_body_has(label, server.meta, "\"system\": 5,");
    rc |= expect_body_has(label, server.meta, "\"talkgroup\": 154437,");
    rc |= expect_body_has(label, server.meta, "\"talkgroup_tag\": \"NFM 154.4375 MHz\",");
    if (mode == DSD_RDIO_MODE_BOTH) {
        char body[4096];
        if (read_file(json_path, body, sizeof(body)) != 0 || strcmp(body, server.meta) != 0) {
            DSD_FPRINTF(stderr, "%s: the meta part is not the DirWatch sidecar\n", label);
            rc = 1;
        }
    } else if (file_exists(json_path) == !fail) {
        DSD_FPRINTF(stderr, "%s: an API-only sidecar is removed after a successful upload only\n", label);
        rc = 1;
    }
    if (file_exists(wav_path) != want_wav_kept) {
        DSD_FPRINTF(stderr, "%s: WAV %s\n", label, want_wav_kept ? "deleted" : "kept");
        rc = 1;
    }
    if (dsd_rdio_upload_holds(wav_path) != 0) {
        DSD_FPRINTF(stderr, "%s: held after the session\n", label);
        rc = 1;
    }
    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

// Issue #888: an API upload of a fallback call carries the same number and label in its meta part, and the row's
// system in its system part.
static int
test_api_fallback_call_meta_and_system(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav_path[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_export_fallback_api")
        || dsd_test_path_join(wav_path, sizeof(wav_path), dir, "fallback_api.wav") != 0
        || write_pcm16_mono_wav(wav_path, 8000, 1) != 0) {
        DSD_FPRINTF(stderr, "fallback API upload: setup failed\n");
        return 1;
    }
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    rdio_test_http_server server;
    char api_url[128] = {0};
    if (!opts || !hist || rdio_test_http_server_start(&server, api_url, sizeof(api_url)) != 0) {
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 5000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_API;
    opts->rdio_system_id = 5;
    opts->rdio_analog_system_id = 9;
    unnumbered_row(hist, DSD_SYNC_DMR_BS_VOICE_POS, 452312500, DSD_ACCESS_CODE_COLOR_CODE, 3);

    int rc = 0;
    int queued = 0;
    if (dsd_rdio_export_call_fallback(opts, hist, wav_path, 0, 0, 1, &queued) != 0 || !queued) {
        DSD_FPRINTF(stderr, "fallback API upload: the upload was not queued\n");
        rc = 1;
    }
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.rc != 0) {
        DSD_FPRINTF(stderr, "fallback API upload: the server did not receive a complete upload\n");
        rc = 1;
    }
    if (strcmp(server.system, "5") != 0) {
        DSD_FPRINTF(stderr, "fallback API upload: form system part '%s', want 5\n", server.system);
        rc = 1;
    }
    rc |= expect_body_has("fallback API upload", server.meta, "\"talkgroup\": 452312,");
    rc |= expect_body_has("fallback API upload", server.meta, "\"talkgroup_tag\": \"DMR CC 3 452.3125 MHz\",");
    rc |= expect_body_has("fallback API upload", server.meta, "\"system\": 5,");
    (void)remove(wav_path);
    remove_empty_dir(dir);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

// Issue #931 D3/D5: with --rdio-system-by-frequency and no base system, a 154.4375 MHz call uploads to system 154437 in
// its form system part and its meta part alike, named "System 154437"; a call with no frequency still has no system
// and is not queued, as before.
static int
test_api_system_by_frequency(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_export_freq_system_api")
        || dsd_test_path_join(wav_path, sizeof(wav_path), dir, "freq_system.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir, "freq_system.json") != 0
        || write_pcm16_mono_wav(wav_path, 8000, 1) != 0) {
        DSD_FPRINTF(stderr, "per-frequency API upload: setup failed\n");
        return 1;
    }
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    rdio_test_http_server server;
    char api_url[128] = {0};
    if (!opts || !hist || rdio_test_http_server_start(&server, api_url, sizeof(api_url)) != 0) {
        free(hist);
        free(opts);
        (void)remove(wav_path);
        remove_empty_dir(dir);
        return 1;
    }
    system_case_setup(hist, opts, 0, 154437500, 1, 0, 0);
    rdio_cancel_opts(opts, api_url, 5000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_API;
    opts->rdio_system_by_frequency = 1;
    opts->rdio_system_id = 0;
    opts->rdio_api_delete_after_upload = 0; /* the WAV serves the second call too */

    int rc = 0;
    int queued = 0;
    if (dsd_rdio_export_call_ex(opts, hist, wav_path, 0, 0, &queued) != 0 || !queued) {
        DSD_FPRINTF(stderr, "per-frequency API upload: the upload was not queued\n");
        rc = 1;
    }
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.rc != 0) {
        DSD_FPRINTF(stderr, "per-frequency API upload: the server did not receive a complete upload\n");
        rc = 1;
    }
    if (strcmp(server.system, "154437") != 0) {
        DSD_FPRINTF(stderr, "per-frequency API upload: form system part '%s', want 154437\n", server.system);
        rc = 1;
    }
    rc |= expect_body_has("per-frequency API upload", server.meta, "\"system\": 154437,");
    rc |= expect_body_has("per-frequency API upload", server.meta, "\"short_name\": \"System 154437\",");
    (void)remove(json_path);

    // No frequency, no base system: nothing to upload to, so no job, as without the option.
    hist->Event_History_Items[0].freq_hz = 0;
    queued = 0;
    (void)dsd_rdio_export_call_ex(opts, hist, wav_path, 0, 0, &queued);
    if (queued) {
        DSD_FPRINTF(stderr, "per-frequency API upload: a call with no frequency and no system was queued\n");
        rc = 1;
    }
    dsd_rdio_upload_shutdown();
    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

static int
test_analog_api_upload_contract(void) {
    int rc = 0;
    rc |= analog_api_upload("NFM API upload, no journal", DSD_RDIO_MODE_API, 0, 0, 0);
    rc |= analog_api_upload("NFM API upload, journaled", DSD_RDIO_MODE_API, 1, 0, 1);
    rc |= analog_api_upload("NFM API upload failed", DSD_RDIO_MODE_API, 0, 1, 1);
    rc |= analog_api_upload("NFM upload beside DirWatch", DSD_RDIO_MODE_BOTH, 0, 0, 1);
    return rc;
}

// Issue #668 and #708: an NFM call's upload holds its WAV from the queue on, as a digital call's does: against a
// stalled endpoint the one in flight and the one queued behind it are both held, and after the user's Stop neither is,
// and both files are kept.
static int
test_analog_api_upload_holds(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[2][DSD_TEST_PATH_MAX] = {{0}};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0
        || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_analog_holds")) {
        if (stalled != DSD_INVALID_SOCKET) {
            (void)dsd_socket_close(stalled);
        }
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 30000, 1);
    analog_row(hist, 154437500);
    int rc = 0;
    for (int i = 0; i < 2 && rc == 0; i++) {
        char leaf[32];
        DSD_SNPRINTF(leaf, sizeof(leaf), "nfm_hold_%d.wav", i);
        int queued = 0;
        if (dsd_test_path_join(wav[i], sizeof(wav[i]), dir, leaf) != 0 || write_pcm16_mono_wav(wav[i], 8000, 1) != 0
            || dsd_rdio_export_call_ex(opts, hist, wav[i], 0, 0, &queued) != 0 || !queued) {
            DSD_FPRINTF(stderr, "analog holds: NFM upload %d not queued\n", i);
            rc = 1;
        }
        if (rc == 0 && i == 0 && rdio_test_wait_for_connection(stalled, 5000U) <= 0) {
            DSD_FPRINTF(stderr, "analog holds: the first upload never started\n");
            rc = 1;
        }
    }
    if (rc == 0 && (dsd_rdio_upload_holds(wav[0]) != 1 || dsd_rdio_upload_holds(wav[1]) != 1)) {
        DSD_FPRINTF(stderr, "analog holds: the uploads in flight and queued are not held\n");
        rc = 1;
    }
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    for (int i = 0; i < 2 && rc == 0; i++) {
        if (dsd_rdio_upload_holds(wav[i]) != 0 || !file_exists(wav[i])) {
            DSD_FPRINTF(stderr, "analog holds: NFM upload %d after the Stop: held or deleted\n", i);
            rc = 1;
        }
    }
    /* Whatever failed above, the files and the folder go. */
    for (int i = 0; i < 2; i++) {
        if (strlen(wav[i]) < 4U) {
            continue;
        }
        char json[DSD_TEST_PATH_MAX];
        DSD_SNPRINTF(json, sizeof(json), "%.*s.json", (int)(strlen(wav[i]) - 4U), wav[i]);
        (void)remove(wav[i]);
        (void)remove(json);
    }
    remove_empty_dir(dir);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
static int
rdio_test_has_suffix(const char* name, const char* suffix) {
    const size_t n = strlen(name);
    const size_t k = strlen(suffix);
    return n > k && strcmp(name + n - k, suffix) == 0;
}

// dsd_dir_list() visitor: counts a folder's files, and among them the upload's private inputs, told by the suffixes
// dsd-neo gives them.
typedef struct {
    int files;
    int private_inputs;
    size_t longest_private; /* the longest private input name */
} rdio_dir_census;

static int
rdio_census_visit(const char* name, void* user) {
    rdio_dir_census* census = (rdio_dir_census*)user;
    census->files++;
    if (rdio_test_has_suffix(name, ".rdio-upload-audio") || rdio_test_has_suffix(name, ".rdio-upload-meta")) {
        census->private_inputs++;
        census->longest_private = strlen(name) > census->longest_private ? strlen(name) : census->longest_private;
    }
    return 0;
}

// The private upload inputs in @p dir, or -1 when it cannot be listed; @p out_files, when given, gets every file.
static int
rdio_test_private_inputs_in(const char* dir, int* out_files) {
    rdio_dir_census census = {0, 0, 0U};
    if (dsd_dir_list(dir, rdio_census_visit, &census) != 0) {
        return -1;
    }
    if (out_files) {
        *out_files = census.files;
    }
    return census.private_inputs;
}

// The longest private upload input name in @p dir, 0 for none.
static size_t
rdio_test_longest_private_in(const char* dir) {
    rdio_dir_census census = {0, 0, 0U};
    return dsd_dir_list(dir, rdio_census_visit, &census) == 0 ? census.longest_private : 0U;
}

// Remove every file in a test folder, then the folder.
static int
rdio_test_remove_visit(const char* name, void* user) {
    char path[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(path, sizeof(path), (const char*)user, name) == 0) {
        (void)remove(path);
    }
    return 0;
}

static void
rdio_test_remove_dir_tree(const char* dir) {
    if (dir && dir[0] != '\0') {
        (void)dsd_dir_list(dir, rdio_test_remove_visit, (void*)dir);
        remove_empty_dir(dir);
    }
}

// One "both" call in @p dir: @p leaf.wav holding @p audio, published with talkgroup @p tg; 0 when its upload was queued.
static int
rdio_both_publish(const dsd_opts* opts, Event_History_I* hist, const char* dir, const char* leaf, const char* audio,
                  uint32_t tg, char* out_wav, char* out_json) {
    char wav_leaf[320];
    char json_leaf[320];
    DSD_SNPRINTF(wav_leaf, sizeof(wav_leaf), "%s.wav", leaf);
    DSD_SNPRINTF(json_leaf, sizeof(json_leaf), "%s.json", leaf);
    if (dsd_test_path_join(out_wav, DSD_TEST_PATH_MAX, dir, wav_leaf) != 0
        || dsd_test_path_join(out_json, DSD_TEST_PATH_MAX, dir, json_leaf) != 0
        || write_bytes_file(out_wav, (const unsigned char*)audio, strlen(audio)) != 0) {
        return 1;
    }
    hist->Event_History_Items[0].event_start_time = (time_t)1699999985;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = tg;
    int queued = 0;
    if (dsd_rdio_export_call_ex(opts, hist, out_wav, 0, 0, &queued) != 0 || !queued) {
        DSD_FPRINTF(stderr, "both: %s was not queued\n", leaf);
        return 1;
    }
    return 0;
}

// The server's request @p i carried @p audio under @p audio_name, and a meta part naming talkgroup @p tg under
// @p meta_name.
static int
rdio_both_expect_request(const char* label, const rdio_test_http_server* server, int i, const char* audio,
                         const char* audio_name, const char* meta_name, uint32_t tg) {
    int rc = 0;
    if (server->req_audio_len[i] != strlen(audio) || memcmp(server->req_audio[i], audio, strlen(audio)) != 0) {
        DSD_FPRINTF(stderr, "%s: request %d audio part has %zu bytes, not the call's %zu\n", label, i,
                    server->req_audio_len[i], strlen(audio));
        rc = 1;
    }
    char want_tg[48];
    DSD_SNPRINTF(want_tg, sizeof(want_tg), "\"talkgroup\": %u,", tg);
    rc |= expect_body_has(label, server->req_meta[i], want_tg);
    if (strcmp(server->req_audio_name[i], audio_name) != 0 || strcmp(server->req_meta_name[i], meta_name) != 0) {
        DSD_FPRINTF(stderr, "%s: request %d parts named '%s' and '%s', want '%s' and '%s'\n", label, i,
                    server->req_audio_name[i], server->req_meta_name[i], audio_name, meta_name);
        rc = 1;
    }
    return rc;
}
#endif

// Issue #888 B15: in --rdio-mode both a DirWatch with delete-after on the same folder takes each public pair once its
// sidecar is there. With the first upload held by the server, the second call is published and its public pair
// deleted, as that watch does: its queued upload still carries its own audio and metadata, under the public file
// names. Delete-after-upload is on and ignored beside DirWatch: the first call's public pair stays, and once the
// uploads are over no private input is left and nothing is held.
static int
test_both_mode_upload_survives_dirwatch_delete(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    static const char* const audio[2] = {"RIFF-both-call-one-audio", "RIFF-both-call-two-audio-bytes"};
    rdio_test_http_server server;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[2][DSD_TEST_PATH_MAX] = {{0}};
    char json[2][DSD_TEST_PATH_MAX] = {{0}};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_race")
        || rdio_test_http_server_start_cfg(&server, api_url, sizeof(api_url), NULL, 0, 5000U, 2, 1) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 5000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;

    int rc = rdio_both_publish(opts, hist, dir, "both_race_0", audio[0], 1201, wav[0], json[0]);
    const uint64_t t0 = dsd_realtime_mono_ms();
    while (rc == 0 && atomic_load(&server.first_accepted) == 0) {
        if (dsd_realtime_mono_ms() - t0 > 5000U) {
            DSD_FPRINTF(stderr, "both race: the first upload never started\n");
            rc = 1;
        }
        dsd_sleep_ms(1U);
    }
    rc |= rdio_both_publish(opts, hist, dir, "both_race_1", audio[1], 1202, wav[1], json[1]);
    if (rc == 0 && dsd_rdio_upload_holds(wav[1]) != 1) {
        DSD_FPRINTF(stderr, "both race: the queued call's public WAV is not held\n");
        rc = 1;
    }
    // The watch ingests the second pair and deletes it while the first upload is still going.
    (void)remove(json[1]);
    (void)remove(wav[1]);
    atomic_store(&server.release_first, 1);
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.rc != 0 || server.requests_seen != 2) {
        DSD_FPRINTF(stderr, "both race: the server saw %d of 2 complete uploads\n", server.requests_done);
        rc = 1;
    } else {
        rc |= rdio_both_expect_request("both race", &server, 0, audio[0], "both_race_0.wav", "both_race_0.json", 1201);
        rc |= rdio_both_expect_request("both race", &server, 1, audio[1], "both_race_1.wav", "both_race_1.json", 1202);
    }
    if (!file_exists(wav[0]) || !file_exists(json[0])) {
        DSD_FPRINTF(stderr, "both race: the first call's public pair was not kept\n");
        rc = 1;
    }
    int files = 0;
    if (rdio_test_private_inputs_in(dir, &files) != 0 || files != 2) {
        DSD_FPRINTF(stderr, "both race: %d files left, want the first call's public pair only\n", files);
        rc = 1;
    }
    if (dsd_rdio_upload_holds(wav[0]) != 0 || dsd_rdio_upload_holds(wav[1]) != 0) {
        DSD_FPRINTF(stderr, "both race: held after the session\n");
        rc = 1;
    }
    rdio_test_remove_dir_tree(dir);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
// A DirWatch with delete-after that takes the pair the moment its sidecar is in place (the sidecar-published seam):
// the private inputs it found then, and the public pair removed, as that watch does.
static struct {
    char dir[DSD_TEST_PATH_MAX];
    char wav[DSD_TEST_PATH_MAX];
    int seen;
    int private_inputs;
} g_rdio_eager_watch;

/* The eager watch armed for the pair @p wav in @p dir, with nothing seen yet. */
static void
rdio_eager_watch_arm(const char* dir, const char* wav) {
    DSD_MEMSET(&g_rdio_eager_watch, 0, sizeof g_rdio_eager_watch);
    DSD_SNPRINTF(g_rdio_eager_watch.dir, sizeof g_rdio_eager_watch.dir, "%s", dir);
    DSD_SNPRINTF(g_rdio_eager_watch.wav, sizeof g_rdio_eager_watch.wav, "%s", wav);
}

static void
rdio_eager_watch_takes_pair(const char* meta_path) {
    g_rdio_eager_watch.seen++;
    g_rdio_eager_watch.private_inputs = rdio_test_private_inputs_in(g_rdio_eager_watch.dir, NULL);
    (void)remove(meta_path);
    (void)remove(g_rdio_eager_watch.wav);
}
#endif

// Issue #888 B15: in --rdio-mode both the upload's private inputs exist before the public sidecar does, so a DirWatch
// with delete-after that takes the pair as soon as the sidecar appears (before a slow WAV copy where the drive has no
// hard links, could finish) still leaves the upload its audio and metadata.
// Before, the sidecar was published first: the watch's delete left the job no WAV to link, and nothing was uploaded.
static int
test_both_mode_private_inputs_precede_the_sidecar(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    static const char* const audio = "RIFF-both-eager-watch-audio";
    rdio_test_http_server server;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[DSD_TEST_PATH_MAX] = {0};
    char json[DSD_TEST_PATH_MAX] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_eager")
        || dsd_test_path_join(wav, sizeof(wav), dir, "both_eager.wav") != 0
        || rdio_test_http_server_start_n(&server, api_url, sizeof(api_url), NULL, 0, 5000U, 1) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 5000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    rdio_eager_watch_arm(dir, wav);
    dsd_rdio_set_sidecar_published_observer(rdio_eager_watch_takes_pair);
    int rc = rdio_both_publish(opts, hist, dir, "both_eager", audio, 1203, wav, json);
    dsd_rdio_set_sidecar_published_observer(NULL);
    if (g_rdio_eager_watch.seen != 1 || g_rdio_eager_watch.private_inputs != 2) {
        DSD_FPRINTF(stderr, "both eager watch: the sidecar appeared %d time(s) with %d private inputs, want 1 with 2\n",
                    g_rdio_eager_watch.seen, g_rdio_eager_watch.private_inputs);
        rc = 1;
    }
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.rc != 0 || server.requests_seen != 1) {
        DSD_FPRINTF(stderr, "both eager watch: the server saw %d of 1 complete upload\n", server.requests_done);
        rc = 1;
    } else {
        rc |=
            rdio_both_expect_request("both eager watch", &server, 0, audio, "both_eager.wav", "both_eager.json", 1203);
    }
    int files = -1;
    if (rdio_test_private_inputs_in(dir, &files) != 0 || files != 0) {
        DSD_FPRINTF(stderr, "both eager watch: %d files left, want none\n", files);
        rc = 1;
    }
    rdio_test_remove_dir_tree(dir);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

// Issue #888 B15: a "both" upload the server refuses, after every attempt, leaves the public pair for DirWatch and no
// private input. A call whose WAV is gone before the export queues nothing, leaves its sidecar, and makes no private
// input either.
static int
test_both_mode_failed_upload_removes_private_inputs(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    rdio_test_http_server server;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[DSD_TEST_PATH_MAX] = {0};
    char json[DSD_TEST_PATH_MAX] = {0};
    char gone_wav[DSD_TEST_PATH_MAX] = {0};
    char gone_json[DSD_TEST_PATH_MAX] = {0};
    const char* refused = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_fail")
        || rdio_test_http_server_start_n(&server, api_url, sizeof(api_url), refused, 0, 5000U, 2) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 5000, 2);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;

    int rc = rdio_both_publish(opts, hist, dir, "both_fail", "RIFF-both-refused", 1201, wav, json);
    int queued = 0;
    if (dsd_test_path_join(gone_wav, sizeof(gone_wav), dir, "both_gone.wav") != 0
        || dsd_test_path_join(gone_json, sizeof(gone_json), dir, "both_gone.json") != 0
        || dsd_rdio_export_call_ex(opts, hist, gone_wav, 0, 0, &queued) == 0 || queued) {
        DSD_FPRINTF(stderr, "both failure: a call with no WAV was queued\n");
        rc = 1;
    }
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.requests_seen != 2) {
        DSD_FPRINTF(stderr, "both failure: the server saw %d of 2 attempts\n", server.requests_seen);
        rc = 1;
    }
    int files = 0;
    if (!file_exists(wav) || !file_exists(json) || !file_exists(gone_json)
        || rdio_test_private_inputs_in(dir, &files) != 0 || files != 3) {
        DSD_FPRINTF(stderr, "both failure: %d files left, want the public pair and the lone sidecar\n", files);
        rc = 1;
    }
    rdio_test_remove_dir_tree(dir);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

// Issue #888 B15 against a stalled endpoint, for @p mode: while the first upload hangs, every public WAV is held, and
// "both" has made each queued call its two private inputs (API-only none). The user's Stop aborts the one in flight
// and the shutdown drops the rest: every public pair stays, none is held, and no private input is left.
static int
stalled_uploads_then_stop(const char* label, int mode) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    (void)label;
    (void)mode;
    return 0;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[3][DSD_TEST_PATH_MAX] = {{0}};
    char json[3][DSD_TEST_PATH_MAX] = {{0}};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_stop")
        || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 30000, 1);
    opts->rdio_api_delete_after_upload = 0;
    opts->rdio_mode = mode;
    int rc = 0;
    for (int i = 0; i < 3 && rc == 0; i++) {
        char leaf[32];
        DSD_SNPRINTF(leaf, sizeof(leaf), "stop_%d", i);
        rc |= rdio_both_publish(opts, hist, dir, leaf, "RIFF-stalled-call", (uint32_t)(1201 + i), wav[i], json[i]);
        if (rc == 0 && i == 0 && rdio_test_wait_for_connection(stalled, 5000U) <= 0) {
            DSD_FPRINTF(stderr, "%s: the first upload never started\n", label);
            rc = 1;
        }
    }
    const int want_private = mode == DSD_RDIO_MODE_BOTH ? 6 : 0;
    const int private_inputs = rdio_test_private_inputs_in(dir, NULL);
    if (private_inputs != want_private) {
        DSD_FPRINTF(stderr, "%s: %d private inputs while uploading, want %d\n", label, private_inputs, want_private);
        rc = 1;
    }
    for (int i = 0; i < 3 && rc == 0; i++) {
        if (dsd_rdio_upload_holds(wav[i]) != 1) {
            DSD_FPRINTF(stderr, "%s: public WAV %d is not held\n", label, i);
            rc = 1;
        }
    }
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    int files = 0;
    if (rdio_test_private_inputs_in(dir, &files) != 0 || files != 6) {
        DSD_FPRINTF(stderr, "%s: %d files left after the Stop, want the three public pairs only\n", label, files);
        rc = 1;
    }
    for (int i = 0; i < 3; i++) {
        if (!file_exists(wav[i]) || !file_exists(json[i]) || dsd_rdio_upload_holds(wav[i]) != 0) {
            DSD_FPRINTF(stderr, "%s: public pair %d after the Stop: deleted or held\n", label, i);
            rc = 1;
        }
    }
    rdio_test_remove_dir_tree(dir);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

static int
test_both_mode_stop_removes_private_inputs(void) {
    int rc = 0;
    rc |= stalled_uploads_then_stop("both Stop", DSD_RDIO_MODE_BOTH);
    rc |= stalled_uploads_then_stop("API-only Stop", DSD_RDIO_MODE_API);
    return rc;
}

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
// The folder a sidecar-published observer counts private inputs in, and the count it found there; -1 before.
static char g_rdio_count_dir[DSD_TEST_PATH_MAX];
static int g_rdio_private_at_sidecar = -1;

static void
rdio_count_private_at_sidecar(const char* meta_path) {
    (void)meta_path;
    g_rdio_private_at_sidecar = rdio_test_private_inputs_in(g_rdio_count_dir, NULL);
}
#endif

// Issue #888 B15: an upload the full queue refuses leaves no private input. Behind a stalled upload, the one in
// flight and a full queue hold two private inputs each; the next call is refused with its public pair alone, and its
// own copies are never made: when its sidecar appears, the folder holds the queued jobs' private inputs alone. Before,
// the refused call's WAV was linked or copied (a whole WAV on a drive without hard links) and its sidecar copied, then
// both removed when the full queue refused the job.
static int
test_both_mode_refused_upload_leaves_no_private_inputs(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    enum { calls = 130 };

    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_full")
        || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 30000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    hist->Event_History_Items[0].target_id = 1201;
    int rc = 0;
    int queued_count = 0;
    int last_queued = 1;
    for (int i = 0; i < calls && rc == 0; i++) {
        char leaf[32];
        char wav[DSD_TEST_PATH_MAX];
        DSD_SNPRINTF(leaf, sizeof(leaf), "full_%03d.wav", i);
        if (dsd_test_path_join(wav, sizeof(wav), dir, leaf) != 0
            || write_bytes_file(wav, (const unsigned char*)"RIFF-full", 9U) != 0) {
            rc = 1;
            break;
        }
        int queued = 0;
        if (i == calls - 1) {
            DSD_SNPRINTF(g_rdio_count_dir, sizeof g_rdio_count_dir, "%s", dir);
            dsd_rdio_set_sidecar_published_observer(rdio_count_private_at_sidecar);
        }
        (void)dsd_rdio_export_call_ex(opts, hist, wav, 0, 0, &queued);
        dsd_rdio_set_sidecar_published_observer(NULL);
        queued_count += queued;
        last_queued = queued;
        if (i == 0 && (!queued || rdio_test_wait_for_connection(stalled, 5000U) <= 0)) {
            DSD_FPRINTF(stderr, "both full queue: the first upload never started\n");
            rc = 1;
        }
    }
    int files = 0;
    const int private_inputs = rdio_test_private_inputs_in(dir, &files);
    if (rc == 0 && (queued_count != 129 || last_queued || private_inputs != 2 * 129 || files != 2 * calls + 2 * 129)) {
        DSD_FPRINTF(stderr, "both full queue: %d queued, %d private inputs of %d files\n", queued_count, private_inputs,
                    files);
        rc = 1;
    }
    if (rc == 0 && g_rdio_private_at_sidecar != 2 * 129) {
        DSD_FPRINTF(stderr, "both full queue: %d private inputs when the refused call's sidecar appeared, want %d\n",
                    g_rdio_private_at_sidecar, 2 * 129);
        rc = 1;
    }
    g_rdio_private_at_sidecar = -1;
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    if (rdio_test_private_inputs_in(dir, &files) != 0 || files != 2 * calls) {
        DSD_FPRINTF(stderr, "both full queue: %d files left after the Stop, want the public pairs only\n", files);
        rc = 1;
    }
    rdio_test_remove_dir_tree(dir);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
// The eager watch's view of the private inputs: their longest name when the sidecar appeared.
static size_t g_rdio_eager_longest_private = 0U;

static void
rdio_eager_watch_measures(const char* meta_path) {
    g_rdio_eager_longest_private = rdio_test_longest_private_in(g_rdio_eager_watch.dir);
    rdio_eager_watch_takes_pair(meta_path);
}
#endif

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
/* The longest stem whose temp sidecar (<stem>.json.tmp.XXXXXX, 16 characters more) still fits the name limit of
   folder @p dir, at most 255 characters; 0 when that limit leaves no room for the test (the private names' 45
   characters, and a stem longer than them). */
static size_t
rdio_test_longest_stem_in(const char* dir) {
    errno = 0;
    const long name_max = pathconf(dir, _PC_NAME_MAX);
    const long limit = (name_max < 0 || name_max > 255) ? 255 : name_max;
    return limit >= 64 ? (size_t)(limit - 16) : 0U;
}
#endif

// Issue #888: a "both" recording whose public names fit the file system's name limit is uploaded, however long its
// stem: the private inputs are named by this run's id and a number, at most 45 characters, not after the public stem.
// Before, they were the stem with the id and suffix appended, 35 characters more, so the longest stem that fits the
// WAV, the sidecar and its temp file (239 characters under the usual 255-byte limit) had no private inputs and was
// never uploaded. The stem is sized from the test folder's own limit (an eCryptfs folder takes 143).
static int
test_both_mode_private_names_do_not_grow_with_the_stem(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    static const char* const audio = "RIFF-both-long-stem-audio";
    rdio_test_http_server server;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[DSD_TEST_PATH_MAX] = {0};
    char json[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_long")) {
        return 1;
    }
    const size_t stem_len = rdio_test_longest_stem_in(dir);
    if (stem_len == 0U) {
        DSD_FPRINTF(stderr, "both long stem: skipped, the test folder's name limit is too short\n");
        rdio_test_remove_dir_tree(dir);
        return 0;
    }
    char stem[256];
    DSD_MEMSET(stem, 'L', stem_len);
    stem[stem_len] = '\0';
    char want_wav[264];
    char want_json[264];
    DSD_SNPRINTF(want_wav, sizeof(want_wav), "%s.wav", stem);
    DSD_SNPRINTF(want_json, sizeof(want_json), "%s.json", stem);
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || dsd_test_path_join(wav, sizeof(wav), dir, want_wav) != 0
        || rdio_test_http_server_start_n(&server, api_url, sizeof(api_url), NULL, 0, 5000U, 1) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 5000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    rdio_eager_watch_arm(dir, wav);
    g_rdio_eager_longest_private = 0U;
    dsd_rdio_set_sidecar_published_observer(rdio_eager_watch_measures);
    int rc = rdio_both_publish(opts, hist, dir, stem, audio, 1204, wav, json);
    dsd_rdio_set_sidecar_published_observer(NULL);
    if (g_rdio_eager_watch.private_inputs != 2 || g_rdio_eager_longest_private == 0U
        || g_rdio_eager_longest_private > 45U) {
        DSD_FPRINTF(stderr,
                    "both long stem: %d private inputs, the longest name %zu characters, want 2 of at most 45\n",
                    g_rdio_eager_watch.private_inputs, g_rdio_eager_longest_private);
        rc = 1;
    }
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.rc != 0 || server.requests_seen != 1) {
        DSD_FPRINTF(stderr, "both long stem: the server saw %d of 1 complete upload\n", server.requests_done);
        rc = 1;
    } else {
        rc |= rdio_both_expect_request("both long stem", &server, 0, audio, want_wav, want_json, 1204);
    }
    int files = -1;
    if (rdio_test_private_inputs_in(dir, &files) != 0 || files != 0) {
        DSD_FPRINTF(stderr, "both long stem: %d files left, want none\n", files);
        rc = 1;
    }
    rdio_test_remove_dir_tree(dir);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

#ifdef DSD_TEST_WRAP_RDIO_SEAMS
/* Link seams (GNU ld --wrap, POSIX only): test doubles of the calls. Each simulated failure is set for one call on a
   private input name, so every other caller, the test's own included, reaches the real call. */
// GNU ld --wrap entry points must keep the reserved __wrap_* and __real_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
int __real_dsd_hard_link(const char* existing, const char* new_path);
FILE* __real_dsd_fopen_private(const char* path, const char* mode);
FILE* __real_dsd_fopen_private_ex(const char* path, const char* mode, int* out_opened);
int __real_pthread_join(pthread_t thread, void** ret);
int __wrap_dsd_hard_link(const char* existing, const char* new_path);
FILE* __wrap_dsd_fopen_private(const char* path, const char* mode);
FILE* __wrap_dsd_fopen_private_ex(const char* path, const char* mode, int* out_opened);
int __wrap_pthread_join(pthread_t thread, void** ret);

static int
seam_names_private(const char* path, const char* suffix) {
    const size_t n = path != NULL ? strlen(path) : 0U;
    const size_t k = strlen(suffix);
    return n > k && strcmp(path + n - k, suffix) == 0;
}

/* The private WAV's hard link fails as on a drive with none, and the user's Stop arrives as its copy begins. */
static atomic_int g_seam_link_then_stop = 0;
/* The private sidecar copy's open creates its file and then cannot make the stream (as fdopen() out of memory). */
static atomic_int g_seam_stream_fails = 0;
/* The worker's join reports failure, after the worker has ended. */
static atomic_int g_seam_join_fails = 0;
/* With g_seam_stream_fails: the folder made read-only once the open has made its file, so removing that file fails.
   Empty when off. */
static char g_seam_lock_dir[DSD_TEST_PATH_MAX];
/* The private WAV's hard link fails as on a drive with none, its folder made read-only first: the copy that follows
   cannot be made, nor the private sidecar copy already made removed. Empty when off. */
static char g_seam_link_locks_dir[DSD_TEST_PATH_MAX];

int
__wrap_dsd_hard_link(const char* existing, const char* new_path) {
    if (seam_names_private(new_path, ".rdio-upload-audio") && atomic_exchange(&g_seam_link_then_stop, 0) != 0) {
        dsd_rdio_upload_cancel();
        errno = EPERM;
        return -1;
    }
    if (seam_names_private(new_path, ".rdio-upload-audio") && g_seam_link_locks_dir[0] != '\0') {
        (void)chmod(g_seam_link_locks_dir, 0500);
        g_seam_link_locks_dir[0] = '\0';
        errno = EPERM;
        return -1;
    }
    return __real_dsd_hard_link(existing, new_path);
}

/* An open that made its file and no stream: the file is there, empty, and NULL comes back. */
static FILE*
seam_open_without_stream(const char* path, int* out_opened) {
    FILE* made = __real_dsd_fopen_private_ex(path, "wbx", NULL);
    // Asked before the close: a closed stream's pointer has no value to compare.
    const int created = made != NULL;
    if (made != NULL) {
        (void)fclose(made);
    }
    if (g_seam_lock_dir[0] != '\0') {
        (void)chmod(g_seam_lock_dir, 0500);
        g_seam_lock_dir[0] = '\0';
    }
    if (out_opened != NULL) {
        *out_opened = created;
    }
    errno = ENOMEM;
    return NULL;
}

FILE*
__wrap_dsd_fopen_private(const char* path, const char* mode) {
    if (seam_names_private(path, ".rdio-upload-meta") && atomic_exchange(&g_seam_stream_fails, 0) != 0) {
        return seam_open_without_stream(path, NULL);
    }
    return __real_dsd_fopen_private(path, mode);
}

FILE*
__wrap_dsd_fopen_private_ex(const char* path, const char* mode, int* out_opened) {
    if (seam_names_private(path, ".rdio-upload-meta") && atomic_exchange(&g_seam_stream_fails, 0) != 0) {
        return seam_open_without_stream(path, out_opened);
    }
    return __real_dsd_fopen_private_ex(path, mode, out_opened);
}

int
__wrap_pthread_join(pthread_t thread, void** ret) {
    const int rc = __real_pthread_join(thread, ret);
    return atomic_exchange(&g_seam_join_fails, 0) != 0 ? ESRCH : rc;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
// One "both" export of @p leaf in @p dir that is not to be queued: its public pair is published and it leaves no private
// input. 0 when so.
static int
rdio_both_expect_not_queued(const char* label, const dsd_opts* opts, Event_History_I* hist, const char* dir,
                            const char* leaf) {
    char wav[DSD_TEST_PATH_MAX];
    char json[DSD_TEST_PATH_MAX];
    char wav_leaf[64];
    char json_leaf[64];
    DSD_SNPRINTF(wav_leaf, sizeof(wav_leaf), "%s.wav", leaf);
    DSD_SNPRINTF(json_leaf, sizeof(json_leaf), "%s.json", leaf);
    if (dsd_test_path_join(wav, sizeof(wav), dir, wav_leaf) != 0
        || dsd_test_path_join(json, sizeof(json), dir, json_leaf) != 0 || write_pcm16_mono_wav(wav, 8000, 1) != 0) {
        return 1;
    }
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1205;
    int queued = -1;
    const int exported = dsd_rdio_export_call_ex(opts, hist, wav, 0, 0, &queued);
    int files = 0;
    const int private_inputs = rdio_test_private_inputs_in(dir, &files);
    if (queued != 0 || !file_exists(wav) || !file_exists(json) || private_inputs != 0
        || dsd_rdio_upload_holds(wav) != 0) {
        DSD_FPRINTF(stderr,
                    "%s: export %d, queued %d, public pair %s, %d private inputs, held %d, want unqueued, kept, none, "
                    "unheld\n",
                    label, exported, queued, file_exists(wav) && file_exists(json) ? "kept" : "missing", private_inputs,
                    dsd_rdio_upload_holds(wav));
        return 1;
    }
    return 0;
}

// The user's Stop, the moment a sidecar is in place: after the private inputs are made, before the job is queued.
static void
rdio_stop_at_sidecar(const char* meta_path) {
    (void)meta_path;
    dsd_rdio_upload_cancel();
}
#endif

// Issue #888: after the user's Stop a "both" export makes no private inputs and queues nothing (no upload of the run is
// sent any more), and a Stop that comes while the WAV is being copied (a drive with no hard links) ends the copy and
// removes what it made. The public pair is DirWatch's all the same. Before, the copy ran to the end and the upload was
// queued for a worker that would never send it.
static int
test_both_mode_stop_ends_private_copies(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    char dir[DSD_TEST_PATH_MAX] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_cancel")) {
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, "http://127.0.0.1:9", 5000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    dsd_rdio_upload_cancel();
    int rc = rdio_both_expect_not_queued("both after a Stop", opts, hist, dir, "after_stop");
    dsd_rdio_upload_shutdown();
#ifdef DSD_TEST_WRAP_RDIO_SEAMS
    atomic_store(&g_seam_link_then_stop, 1);
    rc |= rdio_both_expect_not_queued("both Stop during the copy", opts, hist, dir, "stop_in_copy");
    rc |= atomic_load(&g_seam_link_then_stop) == 0 ? 0 : 1;
    atomic_store(&g_seam_link_then_stop, 0);
    dsd_rdio_upload_shutdown();
#endif
    rdio_test_remove_dir_tree(dir);
    free(hist);
    free(opts);
    return rc;
#endif
}

// Issue #888: a Stop that comes after a "both" job's private inputs are made but before it is queued (here as its
// sidecar appears) leaves it unqueued: its private inputs removed, its WAV not held, the public pair DirWatch's. Before,
// the job was queued for a worker that would never send it, held the WAV and kept its private inputs until the
// shutdown, and the journal said it was queued.
static int
test_both_mode_stop_before_queueing_releases_the_job(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    char dir[DSD_TEST_PATH_MAX] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_admit")) {
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, "http://127.0.0.1:9", 5000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    dsd_rdio_set_sidecar_published_observer(rdio_stop_at_sidecar);
    int rc = rdio_both_expect_not_queued("both Stop before queueing", opts, hist, dir, "stop_at_admission");
    dsd_rdio_set_sidecar_published_observer(NULL);
    dsd_rdio_upload_shutdown();
    rdio_test_remove_dir_tree(dir);
    free(hist);
    free(opts);
    return rc;
#endif
}

// Issue #888: a private sidecar copy whose open made the file but no stream leaves no file behind, and an existing file
// of that name is never removed. Before, the empty file stayed, owned by no job. Needs the link seams.
static int
test_both_mode_failed_private_open_leaves_nothing(void) {
#if !defined(DSD_TEST_WRAP_RDIO_SEAMS) || !defined(USE_CURL)
    return 0;
#else
    char dir[DSD_TEST_PATH_MAX] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_open")) {
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, "http://127.0.0.1:9", 5000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    atomic_store(&g_seam_stream_fails, 1);
    int rc = rdio_both_expect_not_queued("both private open without a stream", opts, hist, dir, "no_stream");
    rc |= atomic_load(&g_seam_stream_fails) == 0 ? 0 : 1;
    atomic_store(&g_seam_stream_fails, 0);
    dsd_rdio_upload_shutdown();
    rdio_test_remove_dir_tree(dir);
    free(hist);
    free(opts);
    return rc;
#endif
}

#if defined(DSD_TEST_WRAP_RDIO_SEAMS) && defined(USE_CURL)
/* A private sidecar copy whose open made its file and no stream, in a folder that then turns read-only: the file cannot
   be removed, and that is said (the run's one warning), with the open's own error kept for the copy's failure. 0 when
   so, with *@p said set. Before, the failed removal was silent. */
static int
rdio_partial_copy_removal_said(const dsd_opts* opts, Event_History_I* hist, int* said) {
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_partial")
        || dsd_test_path_join(wav, sizeof(wav), dir, "partial.wav") != 0 || write_pcm16_mono_wav(wav, 8000, 1) != 0) {
        rdio_test_remove_dir_tree(dir);
        return 1;
    }
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1205;
    DSD_SNPRINTF(g_seam_lock_dir, sizeof g_seam_lock_dir, "%s", dir);
    atomic_store(&g_seam_stream_fails, 1);
    dsd_test_capture_stderr cap;
    const int capturing = dsd_test_capture_stderr_begin(&cap, "rdio_partial") == 0;
    int queued = -1;
    (void)dsd_rdio_export_call_ex(opts, hist, wav, 0, 0, &queued);
    char out[4096] = "";
    if (capturing) {
        dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    }
    (void)chmod(dir, 0700);
    g_seam_lock_dir[0] = '\0';
    atomic_store(&g_seam_stream_fails, 0);
    *said = 0;
    for (const char* p = out; (p = strstr(p, "failed to delete the private upload input")) != NULL; p++) {
        (*said)++;
    }
    const int left = rdio_test_private_inputs_in(dir, NULL);
    int rc = 0;
    if (!capturing || queued != 0 || *said != 1 || left != 1 || strstr(out, strerror(ENOMEM)) == NULL) {
        DSD_FPRINTF(stderr,
                    "both partial copy left: queued %d, said %d time(s), %d private inputs left, want 0, once, 1, and "
                    "the open's error\n%s\n",
                    queued, *said, left, out);
        rc = 1;
    }
    rdio_test_remove_dir_tree(dir);
    return rc;
}

/* The private sidecar copy made, then the WAV's link and copy failing in a folder that turned read-only: that copy
   cannot be removed, and its removal is a private input's, said by the run's one warning (already said here, so not
   again), not as an upload's own file. 0 when so. Before, it was said again as "private upload metadata", with no
   word on removing it by hand. */
static int
rdio_meta_left_after_failed_wav_said_once(const dsd_opts* opts, Event_History_I* hist) {
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_wav_fails")
        || dsd_test_path_join(wav, sizeof(wav), dir, "wav_fails.wav") != 0 || write_pcm16_mono_wav(wav, 8000, 1) != 0) {
        rdio_test_remove_dir_tree(dir);
        return 1;
    }
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1208;
    DSD_SNPRINTF(g_seam_link_locks_dir, sizeof g_seam_link_locks_dir, "%s", dir);
    dsd_test_capture_stderr cap;
    const int capturing = dsd_test_capture_stderr_begin(&cap, "rdio_wav_fails") == 0;
    int queued = -1;
    (void)dsd_rdio_export_call_ex(opts, hist, wav, 0, 0, &queued);
    char out[4096] = "";
    if (capturing) {
        dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    }
    (void)chmod(dir, 0700);
    const int double_unused = g_seam_link_locks_dir[0] != '\0';
    g_seam_link_locks_dir[0] = '\0';
    const int left = rdio_test_private_inputs_in(dir, NULL);
    int rc = 0;
    if (!capturing || double_unused || queued != 0 || left != 1 || strstr(out, "private upload metadata") != NULL
        || strstr(out, "failed to delete the private upload input") != NULL) {
        DSD_FPRINTF(stderr,
                    "both WAV copy failed: queued %d, %d private inputs left, seam %s, want 0, 1, used, and no second "
                    "removal warning\n%s\n",
                    queued, left, double_unused ? "unused" : "used", out);
        rc = 1;
    }
    rdio_test_remove_dir_tree(dir);
    return rc;
}
#endif

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
/* Whether a read-only folder (chmod 0500) keeps a file in @p dir from being removed here: not as root, nor on a mount
   whose permissions are fixed by its options (FAT, CIFS), where chmod fails or changes nothing. The folder is left
   writable and empty of the probe. */
static int
rdio_test_read_only_dir_denies_removal(const char* dir) {
    char probe[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(probe, sizeof(probe), dir, "ro_probe") != 0) {
        return 0;
    }
    FILE* f = dsd_fopen_private(probe, "wb");
    if (f == NULL) {
        return 0;
    }
    (void)fclose(f);
    // The folder made read-only and writable again through one descriptor, so both act on the folder opened here.
    const int dir_fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    const int denied = dir_fd >= 0 && dsd_fchmod(dir_fd, 0500) == 0 && remove(probe) != 0;
    if (dir_fd >= 0) {
        (void)dsd_fchmod(dir_fd, 0700);
        (void)dsd_close(dir_fd);
    }
    (void)remove(probe);
    return denied;
}
#endif

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
/* The user's Stop of a run whose uploads keep private inputs in @p dir, made read-only meanwhile so none can be
   removed: how many times the run said a removal failed, or -1 when stderr could not be captured or @p setup_rc says
   the run's setup failed (the run is stopped all the same). What was said is printed when it is not once. */
static int
rdio_read_only_stop_said(const char* dir, int setup_rc) {
    dsd_test_capture_stderr cap;
    const int capturing = setup_rc == 0 && chmod(dir, 0500) == 0 && dsd_test_capture_stderr_begin(&cap, "rdio_ro") == 0;
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    char out[4096] = "";
    if (capturing) {
        dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
    }
    (void)chmod(dir, 0700);
    int said = 0;
    for (const char* p = out; (p = strstr(p, "failed to delete the private upload input")) != NULL; p++) {
        said++;
    }
    if (capturing && said != 1) {
        DSD_FPRINTF(stderr, "%s\n", out);
    }
    return capturing ? said : -1;
}
#endif

// Issue #888 (Windows limit, here by a read-only folder): private inputs whose removal fails are left in place and said
// once a run, not once a job: a partial copy's (where the link seams can make one) and those of the jobs' end. Issue
// #931: once a run, not once a process, so the next run (dsd_rdio_session_reset(), as a second Android session starts)
// says it again. Skipped where a read-only folder does not stop a removal (as root, or on a mount that ignores the
// folder's mode).
static int
test_both_mode_failed_private_removal_said_once(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[3][DSD_TEST_PATH_MAX] = {{0}};
    char json[3][DSD_TEST_PATH_MAX] = {{0}};
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_both_ro")) {
        return 1;
    }
    if (!rdio_test_read_only_dir_denies_removal(dir)) {
        DSD_FPRINTF(stderr, "both read-only folder: skipped, a read-only folder does not stop a removal here\n");
        rdio_test_remove_dir_tree(dir);
        return 0;
    }
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 30000, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    int rc = 0;
    int said_before = 0;
#ifdef DSD_TEST_WRAP_RDIO_SEAMS
    rc |= rdio_partial_copy_removal_said(opts, hist, &said_before);
    rc |= rdio_meta_left_after_failed_wav_said_once(opts, hist);
#endif
    for (int i = 0; i < 2 && rc == 0; i++) {
        char leaf[32];
        DSD_SNPRINTF(leaf, sizeof(leaf), "ro_%d", i);
        rc |= rdio_both_publish(opts, hist, dir, leaf, "RIFF-read-only", (uint32_t)(1206 + i), wav[i], json[i]);
    }
    int said = rdio_read_only_stop_said(dir, rc);
    int left = rdio_test_private_inputs_in(dir, NULL);
    if (said < 0 || said_before + said != 1 || left != 4) {
        DSD_FPRINTF(stderr,
                    "both read-only folder: said %d time(s) (%d before), %d private inputs left, want once and 4\n",
                    said, said_before, left);
        rc = 1;
    }
    if (rc == 0) {
        dsd_rdio_session_reset();
        rc |= rdio_both_publish(opts, hist, dir, "ro_2", "RIFF-read-only", 1208U, wav[2], json[2]);
        said = rdio_read_only_stop_said(dir, rc);
        left = rdio_test_private_inputs_in(dir, NULL);
        if (said != 1 || left != 6) {
            DSD_FPRINTF(stderr,
                        "both read-only folder, next run: said %d time(s), %d private inputs left, want once and 6\n",
                        said, left);
            rc = 1;
        }
    }

    rdio_test_remove_dir_tree(dir);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
#define RDIO_NO_SYSTEM_WARNING "Rdio API upload skipped: no system ID (set --rdio-system-id)"

/* Call @p leaf (a WAV in @p dir, talkgroup 1201) exported as @p opts say, its export's stderr captured: the export's
   result, with *@p queued whether its upload was queued and *@p warned how many times it said @p needle; *@p warned is
   -1 when stderr could not be captured. */
static int
rdio_export_saying(const dsd_opts* opts, Event_History_I* hist, const char* dir, const char* leaf, const char* needle,
                   int* queued, int* warned) {
    char wav[DSD_TEST_PATH_MAX];
    *queued = -1;
    *warned = -1;
    if (dsd_test_path_join(wav, sizeof(wav), dir, leaf) != 0 || write_pcm16_mono_wav(wav, 8000, 1) != 0) {
        return -2;
    }
    hist->Event_History_Items[0].event_start_time = (time_t)1699999985;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;
    dsd_test_capture_stderr cap;
    const int capturing = dsd_test_capture_stderr_begin(&cap, "rdio_nosys") == 0;
    const int rc = dsd_rdio_export_call_ex(opts, hist, wav, 0, 0, queued);
    char out[4096] = "";
    if (capturing) {
        dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        *warned = 0;
        for (const char* p = out; (p = strstr(p, needle)) != NULL; p++) {
            (*warned)++;
        }
    }
    return rc;
}

/* rdio_export_saying() of the no-system warning. */
static int
rdio_no_system_export(const dsd_opts* opts, Event_History_I* hist, const char* dir, const char* leaf, int* queued,
                      int* warned) {
    return rdio_export_saying(opts, hist, dir, leaf, RDIO_NO_SYSTEM_WARNING, queued, warned);
}

/* One export of call @p leaf: it succeeds, queues an upload when @p want_queued, and says the no-system warning
   @p want_warned times. */
static int
rdio_no_system_expect(const char* label, const dsd_opts* opts, Event_History_I* hist, const char* dir, const char* leaf,
                      int want_queued, int want_warned) {
    int queued = 0;
    int warned = 0;
    const int rc = rdio_no_system_export(opts, hist, dir, leaf, &queued, &warned);
    if (rc != 0 || queued != want_queued || warned != want_warned) {
        DSD_FPRINTF(stderr, "%s: %s returned %d, queued %d, warned %d time(s); want 0, %d, %d\n", label, leaf, rc,
                    queued, warned, want_queued, want_warned);
        return 1;
    }
    return 0;
}
#endif

// Issue #931 B4: an API upload of a call with no system ID is skipped, not failed: the export succeeds, queues
// nothing, keeps its sidecar, and the run says so once, the first time. A successful upload of a call that has one
// between two skips leaves the second silent. Each decoder run (dsd_rdio_session_reset()) says it once again, a run
// made of skips alone (no upload worker started) and a run whose worker ran alike.
static int
test_api_no_system_skip_said_once_a_run(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    char dir[DSD_TEST_PATH_MAX] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    rdio_test_http_server server;
    char api_url[128] = {0};
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_no_system")) {
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, "http://127.0.0.1:1", 5000, 1);
    opts->rdio_api_delete_after_upload = 0;
    opts->rdio_system_id = 0;
    int rc = 0;

    dsd_rdio_session_reset();
    rc |= rdio_no_system_expect("skips-only run", opts, hist, dir, "skip_a.wav", 0, 1);
    rc |= rdio_no_system_expect("skips-only run", opts, hist, dir, "skip_b.wav", 0, 0);
    char json[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(json, sizeof(json), dir, "skip_a.json") != 0 || !file_exists(json)) {
        DSD_FPRINTF(stderr, "no system ID: the skipped call's sidecar is not in place\n");
        rc = 1;
    }
    dsd_rdio_upload_shutdown();

    dsd_rdio_session_reset();
    if (rdio_test_http_server_start(&server, api_url, sizeof(api_url)) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    DSD_SNPRINTF(opts->rdio_api_url, sizeof(opts->rdio_api_url), "%s", api_url);
    rc |= rdio_no_system_expect("second run", opts, hist, dir, "skip_c.wav", 0, 1);
    opts->rdio_system_id = 48;
    rc |= rdio_no_system_expect("second run", opts, hist, dir, "sent.wav", 1, 0);
    opts->rdio_system_id = 0;
    rc |= rdio_no_system_expect("second run", opts, hist, dir, "skip_d.wav", 0, 0);
    dsd_rdio_upload_shutdown();
    if (dsd_thread_join(server.thread) != 0 || server.rc != 0 || server.requests_seen != 1
        || strcmp(server.system, "48") != 0) {
        DSD_FPRINTF(stderr, "no system ID: %d upload(s) with system '%s', want the one with 48\n", server.requests_seen,
                    server.system);
        rc = 1;
    }

    dsd_rdio_session_reset();
    rc |= rdio_no_system_expect("third run", opts, hist, dir, "skip_e.wav", 0, 1);
    dsd_rdio_upload_shutdown();

    rdio_test_remove_dir_tree(dir);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

/* One export of a new WAV @p leaf in @p dir with stderr captured: the export's result, and in *@p warned how many
   times it said @p needle (-1 when stderr could not be captured). */
static int
rdio_export_counting(const dsd_opts* opts, Event_History_I* hist, const char* dir, const char* leaf, const char* needle,
                     int* warned) {
    char wav[DSD_TEST_PATH_MAX];
    *warned = -1;
    if (dsd_test_path_join(wav, sizeof(wav), dir, leaf) != 0 || write_pcm16_mono_wav(wav, 8000, 1) != 0) {
        return -2;
    }
    hist->Event_History_Items[0].event_start_time = (time_t)1699999985;
    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;
    dsd_test_capture_stderr cap;
    const int capturing = dsd_test_capture_stderr_begin(&cap, "rdio_nosys_any") == 0;
    int queued = 0;
    const int rc = dsd_rdio_export_call_ex(opts, hist, wav, 0, 0, &queued);
    if (capturing) {
        char out[4096] = "";
        dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, out, sizeof(out));
        *warned = 0;
        for (const char* p = out; (p = strstr(p, needle)) != NULL; p++) {
            (*warned)++;
        }
    }
    return rc;
}

// Issue #931 (gate r1): a call with no system ID is skipped, not failed, in a build without libcurl too: its export
// succeeds with its sidecar in place, and the run says so once. Without libcurl, an upload with a system ID (one the
// build would have to send) and a call with no API key still fail, as before.
static int
test_api_no_system_skip_in_every_build(void) {
    static const char* const k_no_system = "Rdio API upload skipped: no system ID (set --rdio-system-id)";
    static const int modes[] = {DSD_RDIO_MODE_API, DSD_RDIO_MODE_BOTH};
    int rc = 0;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        char dir[DSD_TEST_PATH_MAX] = {0};
        char json[DSD_TEST_PATH_MAX] = {0};
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
        Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
        if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_no_system_any")
            || dsd_test_path_join(json, sizeof(json), dir, "skip_a.json") != 0) {
            free(hist);
            free(opts);
            return 1;
        }
        opts->rdio_mode = modes[i];
        opts->rdio_system_id = 0;
        opts->rdio_upload_timeout_ms = 100;
        opts->rdio_upload_retries = 1;
        DSD_SNPRINTF(opts->rdio_api_url, sizeof(opts->rdio_api_url), "%s", "http://127.0.0.1:1");
        DSD_SNPRINTF(opts->rdio_api_key, sizeof(opts->rdio_api_key), "%s", "test-key");
        dsd_rdio_session_reset();
        int warned = 0;
        int got = rdio_export_counting(opts, hist, dir, "skip_a.wav", k_no_system, &warned);
        if (got != 0 || warned != 1 || !file_exists(json)) {
            DSD_FPRINTF(stderr, "mode %d, no system ID: returned %d, warned %d, sidecar %d; want 0, 1, 1\n", modes[i],
                        got, warned, file_exists(json));
            rc = 1;
        }
        got = rdio_export_counting(opts, hist, dir, "skip_b.wav", k_no_system, &warned);
        if (got != 0 || warned != 0) {
            DSD_FPRINTF(stderr, "mode %d, second call: returned %d, warned %d; want 0, 0\n", modes[i], got, warned);
            rc = 1;
        }
#ifndef USE_CURL
        opts->rdio_system_id = 5;
        got = rdio_export_counting(opts, hist, dir, "needs_curl.wav", "compiled without libcurl", &warned);
        if (got != -1 || warned != 1) {
            DSD_FPRINTF(stderr, "mode %d, no libcurl, system 5: returned %d, warned %d; want -1, 1\n", modes[i], got,
                        warned);
            rc = 1;
        }
        opts->rdio_system_id = 0;
        opts->rdio_api_key[0] = '\0';
        got = rdio_export_counting(opts, hist, dir, "no_key.wav", "compiled without libcurl", &warned);
        if (got != -1) {
            DSD_FPRINTF(stderr, "mode %d, no libcurl, no API key: returned %d; want -1\n", modes[i], got);
            rc = 1;
        }
#endif
        dsd_rdio_upload_shutdown();
        static const char* const k_leaves[] = {"skip_a.wav",     "skip_a.json",     "skip_b.wav", "skip_b.json",
                                               "needs_curl.wav", "needs_curl.json", "no_key.wav", "no_key.json"};
        for (size_t k = 0; k < sizeof(k_leaves) / sizeof(k_leaves[0]); k++) {
            char leaf[DSD_TEST_PATH_MAX];
            if (dsd_test_path_join(leaf, sizeof(leaf), dir, k_leaves[k]) == 0) {
                (void)remove(leaf);
            }
        }
        remove_empty_dir(dir);
        free(hist);
        free(opts);
    }
    return rc;
}

// Issue #931 B4: in both modes with an API side, a call skipped for having no system ID still publishes its sidecar,
// makes no private upload inputs (--rdio-mode both) and queues nothing; a sidecar that cannot be put in place (its name
// taken by a folder) is still an export failure on that path. The API key is checked first: a call with neither a key
// nor a system ID is an export failure, not a skip, and says nothing about the system ID.
static int
test_api_no_system_skip_keeps_the_sidecar_rule(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    static const int modes[] = {DSD_RDIO_MODE_API, DSD_RDIO_MODE_BOTH};
    int rc = 0;
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]) && rc == 0; i++) {
        char dir[DSD_TEST_PATH_MAX] = {0};
        char json[DSD_TEST_PATH_MAX] = {0};
        dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
        Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
        if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_no_system_sidecar")
            || dsd_test_path_join(json, sizeof(json), dir, "blocked.json") != 0) {
            if (dir[0] != '\0') {
                rdio_test_remove_dir_tree(dir);
            }
            free(hist);
            free(opts);
            return 1;
        }
        rdio_cancel_opts(opts, "http://127.0.0.1:1", 5000, 1);
        opts->rdio_mode = modes[i];
        opts->rdio_system_id = 0;
        const char* label = modes[i] == DSD_RDIO_MODE_BOTH ? "no system ID, both" : "no system ID, api";
        dsd_rdio_session_reset();
        rc |= rdio_no_system_expect(label, opts, hist, dir, "skip.wav", 0, 1);
        int files = 0;
        const int private_inputs = rdio_test_private_inputs_in(dir, &files);
        if (private_inputs != 0 || files != 2) {
            DSD_FPRINTF(stderr, "%s: %d private inputs of %d files, want none of 2\n", label, private_inputs, files);
            rc = 1;
        }
        int queued = 0;
        int warned = 0;
        const int failed =
            mkdir(json, 0700) == 0 ? rdio_no_system_export(opts, hist, dir, "blocked.wav", &queued, &warned) : 0;
        if (failed != -1 || queued != 0 || rdio_test_private_inputs_in(dir, NULL) != 0) {
            DSD_FPRINTF(stderr, "%s: a sidecar that cannot be put in place returned %d, queued %d\n", label, failed,
                        queued);
            rc = 1;
        }
        opts->rdio_api_key[0] = '\0';
        dsd_rdio_session_reset();
        const int no_key = rdio_no_system_export(opts, hist, dir, "no_key.wav", &queued, &warned);
        if (no_key != -1 || queued != 0 || warned != 0 || rdio_test_private_inputs_in(dir, NULL) != 0) {
            DSD_FPRINTF(stderr, "%s: no API key nor system ID returned %d, queued %d, warned %d; want -1, 0, 0\n",
                        label, no_key, queued, warned);
            rc = 1;
        }
        remove_empty_dir(json);
        rdio_test_remove_dir_tree(dir);
        dsd_rdio_upload_shutdown();
        free(hist);
        free(opts);
    }
    return rc;
#endif
}

// Issue #931: delete-after-upload beside DirWatch (--rdio-mode both) is ignored with one warning a decoder run, not one
// a process: the second call of a run is silent, and the next run (dsd_rdio_session_reset(), as a second Android
// session starts) warns again. The uploads go to a closed port and fail; their files are kept.
static int
test_both_mode_delete_after_warning_once_a_run(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
    return 0;
#else
    static const char* const k_warning = "Rdio API delete-after-upload ignored when DirWatch export is active";
    static const char* const k_leaf[2][2] = {{"run1_a.wav", "run1_b.wav"}, {"run2_a.wav", "run2_b.wav"}};
    char dir[DSD_TEST_PATH_MAX] = {0};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_delete_after_dirwatch")) {
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, "http://127.0.0.1:1", 100, 1);
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    int rc = 0;
    for (int run = 0; run < 2; run++) {
        dsd_rdio_session_reset();
        for (int call = 0; call < 2; call++) {
            int queued = 0;
            int warned = 0;
            const int exported = rdio_export_saying(opts, hist, dir, k_leaf[run][call], k_warning, &queued, &warned);
            if (exported != 0 || queued != 1 || warned != (call == 0 ? 1 : 0)) {
                DSD_FPRINTF(stderr, "delete-after beside DirWatch: run %d call %d returned %d, queued %d, warned %d\n",
                            run + 1, call + 1, exported, queued, warned);
                rc = 1;
            }
        }
        /* The uploads to the closed port fail as the shutdown drains them: their log lines are the test's own. */
        dsd_test_capture_stderr cap;
        const int capturing = dsd_test_capture_stderr_begin(&cap, "rdio_delete_after_drain") == 0;
        dsd_rdio_upload_shutdown();
        if (capturing) {
            dsd_test_capture_stderr_end(&cap);
            char drained[512];
            (void)dsd_test_capture_stderr_read(&cap, drained, sizeof(drained)); /* removes the capture file */
        }
    }
    rdio_test_remove_dir_tree(dir);
    free(hist);
    free(opts);
    return rc;
#endif
}

// Issue #888: a shutdown whose join of the upload worker fails (not expected; forced here, after the worker ended)
// releases the jobs still queued: their private inputs go and their public WAVs are no longer held. The job in flight
// is the worker's and is not touched. Before, the queued jobs stayed linked and held their files for the rest of the
// session. Leaves the upload worker failed for good, so it runs last. Needs the link seams.
static int
test_failed_join_releases_queued_jobs(void) {
#if !defined(DSD_TEST_WRAP_RDIO_SEAMS) || !defined(USE_CURL)
    return 0;
#else
    dsd_socket_t stalled = DSD_INVALID_SOCKET;
    char api_url[128] = {0};
    char dir[DSD_TEST_PATH_MAX] = {0};
    char wav[3][DSD_TEST_PATH_MAX] = {{0}};
    char json[3][DSD_TEST_PATH_MAX] = {{0}};
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    Event_History_I* hist = (Event_History_I*)calloc(1, sizeof(*hist));
    if (!opts || !hist || !dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_rdio_join_fail")
        || rdio_test_stalled_server_start(&stalled, api_url, sizeof(api_url)) != 0) {
        rdio_test_remove_dir_tree(dir);
        free(hist);
        free(opts);
        return 1;
    }
    rdio_cancel_opts(opts, api_url, 30000, 1);
    opts->rdio_api_delete_after_upload = 0;
    opts->rdio_mode = DSD_RDIO_MODE_BOTH;
    int rc = 0;
    for (int i = 0; i < 3 && rc == 0; i++) {
        char leaf[32];
        DSD_SNPRINTF(leaf, sizeof(leaf), "join_%d", i);
        rc |= rdio_both_publish(opts, hist, dir, leaf, "RIFF-join-fails", (uint32_t)(1208 + i), wav[i], json[i]);
        if (rc == 0 && i == 0 && rdio_test_wait_for_connection(stalled, 5000U) <= 0) {
            DSD_FPRINTF(stderr, "failed join: the first upload never started\n");
            rc = 1;
        }
    }
    atomic_store(&g_seam_join_fails, 1);
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    atomic_store(&g_seam_join_fails, 0);
    const int left = rdio_test_private_inputs_in(dir, NULL);
    const int held = dsd_rdio_upload_holds(wav[1]) + dsd_rdio_upload_holds(wav[2]);
    if (left != 0 || held != 0) {
        DSD_FPRINTF(stderr, "failed join: %d private inputs left and %d public WAVs held, want none\n", left, held);
        rc = 1;
    }
    for (int i = 0; i < 3; i++) {
        if (!file_exists(wav[i]) || !file_exists(json[i])) {
            DSD_FPRINTF(stderr, "failed join: public pair %d is gone\n", i);
            rc = 1;
        }
    }
    rdio_test_remove_dir_tree(dir);
    (void)dsd_socket_close(stalled);
    dsd_socket_cleanup();
    free(hist);
    free(opts);
    return rc;
#endif
}

int
main(void) {
    int rc = 0;
    rc |= test_mode_parser();
    rc |= test_dirwatch_sidecar_generation();
    rc |= test_mode_off_no_sidecar();
    rc |= test_duration_uses_wav_samplerate();
    rc |= test_duration_skips_extra_wav_chunks();
    rc |= test_duration_rejects_invalid_wav_format_values();
    rc |= test_duration_uses_zero_for_missing_or_truncated_wav();
    rc |= test_export_guards_and_invalid_mode_are_noops();
    rc |= test_sidecar_service_name_keeps_numeric_identity();
    rc |= test_sidecar_escapes_strings_and_tgt_fallback();
    rc |= test_sidecar_private_fallback_and_malformed_wav_duration();
    rc |= test_sidecar_channel_label_fallback();
    rc |= test_sidecar_type_d_text_yields_to_names();
    rc |= test_sidecar_dcr_files_under_channel_khz();
    rc |= test_sidecar_freq_comes_from_the_row_frequency();
    rc |= test_sidecar_start_time_sources();
    rc |= test_analog_sidecar_fields();
    rc |= test_analog_talkgroup_sources();
    rc |= test_analog_tag_is_stable();
    rc |= test_analog_system_id();
    rc |= test_system_by_frequency_resolver();
    rc |= test_system_by_frequency_sidecar();
    rc |= test_sidecar_tag_of_unknown_kind_is_empty();
    rc |= test_sidecar_emergency_from_the_row();
    rc |= test_sidecar_short_name_falls_back_to_the_protocol();
    rc |= test_sidecar_dash_tag_is_absent();
    rc |= test_sidecar_strings_are_valid_utf8();
    rc |= test_unnumbered_digital_call_without_fallback_writes_nothing();
    rc |= test_call_talkgroup_number();
    rc |= test_call_fallback_eligible();
    rc |= test_fallback_call_label();
    rc |= test_fallback_edacs_provoice_label();
    rc |= test_fallback_call_sidecar();
    rc |= test_api_shutdown_drains_queue();
    rc |= test_api_delete_after_successful_upload();
    rc |= test_api_journaled_upload_keeps_the_wav();
    rc |= test_api_meta_part_matches_dirwatch_sidecar();
    rc |= test_api_fallback_call_meta_and_system();
    rc |= test_api_system_by_frequency();
    rc |= test_api_upload_does_not_follow_redirect();
    rc |= test_api_cancel_before_shutdown_skips_backlog();
    rc |= test_api_cancel_during_drain_returns_promptly();
    rc |= test_api_cancel_during_tls_handshake();
    rc |= test_api_cancel_covers_uploads_queued_after_it();
    rc |= test_api_drain_uploads_every_job_after_cancel_clears();
    rc |= test_upload_holds_name_rule();
    rc |= test_api_upload_holds_queued_and_in_flight_files();
    rc |= test_api_upload_holds_until_the_delete_is_over();
    rc |= test_analog_api_upload_contract();
    rc |= test_analog_api_upload_holds();
    rc |= test_both_mode_upload_survives_dirwatch_delete();
    rc |= test_both_mode_private_inputs_precede_the_sidecar();
    rc |= test_both_mode_failed_upload_removes_private_inputs();
    rc |= test_both_mode_stop_removes_private_inputs();
    rc |= test_both_mode_refused_upload_leaves_no_private_inputs();
    rc |= test_both_mode_private_names_do_not_grow_with_the_stem();
    rc |= test_both_mode_stop_ends_private_copies();
    rc |= test_both_mode_stop_before_queueing_releases_the_job();
    rc |= test_both_mode_failed_private_open_leaves_nothing();
    rc |= test_both_mode_failed_private_removal_said_once();
    rc |= test_api_no_system_skip_said_once_a_run();
    rc |= test_api_no_system_skip_keeps_the_sidecar_rule();
    rc |= test_api_no_system_skip_in_every_build();
    rc |= test_both_mode_delete_after_warning_once_a_run();
    /* Last: it leaves the upload worker failed for good. */
    rc |= test_failed_join_releases_queued_jobs();
    free(g_rdio_case_hist);
    free(g_rdio_case_opts);
    return rc;
}
