// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
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
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
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

// Copy the body of the request's "meta" multipart part into server->meta.
static void
rdio_test_capture_meta_part(rdio_test_http_server* server, const char* request, size_t used) {
    server->meta[0] = '\0';
    const char* part = rdio_test_find_bytes(request, used, "name=\"meta\"");
    if (!part) {
        return;
    }
    const size_t part_off = (size_t)(part - request);
    const char* body = rdio_test_find_bytes(part, used - part_off, "\r\n\r\n");
    if (!body) {
        return;
    }
    body += 4;
    const size_t body_off = (size_t)(body - request);
    const char* end = rdio_test_find_bytes(body, used - body_off, "\r\n--");
    if (!end) {
        return;
    }
    size_t len = (size_t)(end - body);
    if (len >= sizeof(server->meta)) {
        len = sizeof(server->meta) - 1U;
    }
    DSD_MEMCPY(server->meta, body, len);
    server->meta[len] = '\0';
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
    rdio_test_capture_meta_part(server, request, used);

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
rdio_test_http_server_start_n(rdio_test_http_server* server, char* out_api_url, size_t out_api_url_size,
                              const char* response, int no_request_expected, unsigned int accept_timeout_ms,
                              int requests_expected) {
    if (!server || !out_api_url || out_api_url_size == 0) {
        return 1;
    }

    DSD_MEMSET(server, 0, sizeof(*server));
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
test_missing_talkgroup_rejects_sidecar(void) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_missing_tg")) {
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
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = 48;
    hist.Event_History_Items[0].event_time = (time_t)1700000300;

    int rc = 0;
    if (dsd_rdio_export_call(&opts, &hist, wav_path, 0) == 0) {
        DSD_FPRINTF(stderr, "export accepted missing talkgroup\n");
        rc = 1;
    }
    if (file_exists(json_path)) {
        DSD_FPRINTF(stderr, "missing talkgroup should not create sidecar\n");
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

// Export one row's sidecar into a fresh directory and return the sidecar's body in @p body, or -1. The recording is an
// 8 kHz WAV @p wav_seconds long, or with 0 a file with no RIFF header, which adds no length to stop_time, and was
// opened at decode time @p opened (0: unknown).
static int
export_sidecar_body(const Event_History_I* hist, int wav_seconds, time_t opened, char* body, size_t body_size) {
    char dir_template[DSD_TEST_PATH_MAX] = {0};
    if (!dsd_test_mkdtemp(dir_template, sizeof(dir_template), "dsdneo_rdio_export_freq")) {
        DSD_FPRINTF(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return -1;
    }
    char wav_path[DSD_TEST_PATH_MAX] = {0};
    char json_path[DSD_TEST_PATH_MAX] = {0};
    if (dsd_test_path_join(wav_path, sizeof(wav_path), dir_template, "freq.wav") != 0
        || dsd_test_path_join(json_path, sizeof(json_path), dir_template, "freq.json") != 0) {
        DSD_FPRINTF(stderr, "path join failed\n");
        remove_empty_dir(dir_template);
        return -1;
    }
    if ((wav_seconds > 0 ? write_pcm16_mono_wav(wav_path, 8000, wav_seconds) : write_dummy_wav(wav_path)) != 0) {
        remove_empty_dir(dir_template);
        return -1;
    }
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    int rc = 0;
    if (dsd_rdio_export_call(&opts, hist, wav_path, opened) != 0 || read_file(json_path, body, body_size) != 0) {
        DSD_FPRINTF(stderr, "export failed for frequency sidecar\n");
        rc = -1;
    }
    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
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
    int rc = export_sidecar_body(hist, 1, 0, body, sizeof body);
    if (rc == 0 && (!strstr(body, "\"talkgroup_tag\": \"Hospital West\"") || !strstr(body, "\"talkgroup\": 77"))) {
        DSD_FPRINTF(stderr, "service name must label the unchanged numeric talkgroup\n%s\n", body);
        rc = 1;
    }
    free(hist);
    return rc;
}

// The sidecar's freq is the frequency the call was heard on (Event_History::freq_hz). A row's channel is a trunk
// channel number, an LCN or a dPMR colour code, never Hz, and must not be read as one even when it is large enough to
// pass the 1 MHz floor. A frequency that does not fit the field, or sits below the floor, is reported as 0.
static int
test_sidecar_freq_comes_from_the_row_frequency(void) {
    static Event_History_I hist;

    struct {
        const char* label;
        uint32_t channel;
        int64_t freq_hz;
        const char* want;
    } cases[] = {
        {"channel number without a frequency", 851012500U, 0, "\"freq\": 0,"},
        {"row frequency beside a channel number", 0x10C8U, 453212500, "\"freq\": 453212500,"},
        {"frequency past 32 bits", 0U, INT64_C(5000000000), "\"freq\": 0,"},
        {"negative frequency", 0U, -851012500, "\"freq\": 0,"},
        {"sub-MHz frequency", 0U, 999999, "\"freq\": 0,"},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(&hist, 0, sizeof(hist));
        hist.Event_History_Items[0].event_time = (time_t)1700000800;
        hist.Event_History_Items[0].target_id = 1201;
        hist.Event_History_Items[0].channel = cases[i].channel;
        hist.Event_History_Items[0].freq_hz = cases[i].freq_hz;
        char body[4096];
        if (export_sidecar_body(&hist, 0, 0, body, sizeof body) != 0) {
            return 1;
        }
        if (!strstr(body, cases[i].want)) {
            DSD_FPRINTF(stderr, "%s: sidecar missing %s\n%s\n", cases[i].label, cases[i].want, body);
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
// stamp need not be an end.
static int
test_sidecar_start_time_sources(void) {
    static Event_History_I hist;
    const time_t decode_now = (time_t)1700000900;

    struct {
        const char* label;
        time_t event_start_time;
        time_t event_time;
        time_t opened;
        time_t want_start;
    } cases[] = {
        {"call start", (time_t)1700000810, (time_t)1700000830, 0, (time_t)1700000810},
        {"recording opened before the call", (time_t)1700000810, (time_t)1700000830, (time_t)1700000700,
         (time_t)1700000810},
        {"recording opened mid-call", (time_t)1700000810, (time_t)1700000830, (time_t)1700000822, (time_t)1700000822},
        {"no start: last activity", 0, (time_t)1700000830, 0, (time_t)1700000830},
        {"no start: open time does not move a played file's stamp", 0, (time_t)1600000000, (time_t)1700000822,
         (time_t)1600000000},
        {"negative start: last activity", (time_t)-5, (time_t)1700000830, 0, (time_t)1700000830},
        {"neither stamp: decode clock", 0, 0, 0, decode_now},
    };

    dsd_decode_clock_use_test((uint64_t)decode_now * UINT64_C(1000000000));
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DSD_MEMSET(&hist, 0, sizeof(hist));
        hist.Event_History_Items[0].event_start_time = cases[i].event_start_time;
        hist.Event_History_Items[0].event_time = cases[i].event_time;
        hist.Event_History_Items[0].target_id = 1201;
        char body[4096];
        if (export_sidecar_body(&hist, 2, cases[i].opened, body, sizeof body) != 0) {
            rc = 1;
            break;
        }
        rc |= expect_sidecar_times(cases[i].label, body, (long long)cases[i].want_start,
                                   (long long)cases[i].want_start + 2LL);
    }
    dsd_decode_clock_use_system();
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

// The user's Stop against a stalled endpoint: cancel, then the cleanup's shutdown. Without the cancel the drain would
// run 4 jobs x 3 attempts x 3 s; with it the attempt in flight is aborted and the rest are never tried, well inside one
// attempt's timeout, and nothing is deleted.
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

    rdio_cancel_jobs jobs;
    int rc = rdio_cancel_jobs_queue(&jobs, "dsdneo_rdio_cancel_before", 4, opts);
    // Let the worker get into its first transfer, so the cancel has one to abort.
    dsd_sleep_ms(300U);

    const uint64_t t0 = dsd_realtime_mono_ms();
    dsd_rdio_upload_cancel();
    dsd_rdio_upload_shutdown();
    const uint64_t elapsed = dsd_realtime_mono_ms() - t0;
    if (elapsed >= 2000U) {
        DSD_FPRINTF(stderr, "cancel before shutdown: shutdown took %llu ms\n", (unsigned long long)elapsed);
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
        (void)dsd_rdio_export_call_ex(opts, hist, wav[i], 0, &queued[i]);
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
    rc |= test_missing_talkgroup_rejects_sidecar();
    rc |= test_sidecar_escapes_strings_and_tgt_fallback();
    rc |= test_sidecar_private_fallback_and_malformed_wav_duration();
    rc |= test_sidecar_channel_label_fallback();
    rc |= test_sidecar_freq_comes_from_the_row_frequency();
    rc |= test_sidecar_start_time_sources();
    rc |= test_api_shutdown_drains_queue();
    rc |= test_api_delete_after_successful_upload();
    rc |= test_api_journaled_upload_keeps_the_wav();
    rc |= test_api_meta_part_matches_dirwatch_sidecar();
    rc |= test_api_upload_does_not_follow_redirect();
    rc |= test_api_cancel_before_shutdown_skips_backlog();
    rc |= test_api_cancel_during_drain_returns_promptly();
    rc |= test_api_cancel_covers_uploads_queued_after_it();
    rc |= test_api_drain_uploads_every_job_after_cancel_clears();
    rc |= test_upload_holds_name_rule();
    rc |= test_api_upload_holds_queued_and_in_flight_files();
    rc |= test_api_upload_holds_until_the_delete_is_over();
    return rc;
}
