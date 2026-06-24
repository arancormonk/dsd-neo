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
#include "test_support.h"

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
#endif

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

#if defined(USE_CURL) && !DSD_PLATFORM_WIN_NATIVE
typedef struct {
    dsd_socket_t listen_sock;
    dsd_thread_t thread;
    int rc;
    int no_request_expected;
    int saw_request;
    unsigned int accept_timeout_ms;
    char response[512];
} rdio_test_http_server;

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

static DSD_THREAD_RETURN_TYPE
rdio_test_http_server_thread(void* arg) {
    rdio_test_http_server* server = (rdio_test_http_server*)arg;
    server->rc = 1;

    if (server->no_request_expected) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server->listen_sock, &readfds);
        struct timeval tv;
        tv.tv_sec = (time_t)(server->accept_timeout_ms / 1000U);
        tv.tv_usec = (suseconds_t)((server->accept_timeout_ms % 1000U) * 1000U);
        int ready = select(server->listen_sock + 1, &readfds, NULL, NULL, &tv);
        if (ready <= 0) {
            server->rc = 0;
            (void)dsd_socket_close(server->listen_sock);
            DSD_THREAD_RETURN;
        }
    }

    dsd_socket_t client = dsd_socket_accept(server->listen_sock, NULL, NULL);
    if (client == DSD_INVALID_SOCKET) {
        if (server->no_request_expected) {
            server->rc = 0;
        }
        (void)dsd_socket_close(server->listen_sock);
        DSD_THREAD_RETURN;
    }
    server->saw_request = 1;

    if (server->no_request_expected) {
        (void)dsd_socket_close(client);
        (void)dsd_socket_close(server->listen_sock);
        DSD_THREAD_RETURN;
    }

    (void)dsd_socket_set_recv_timeout(client, 5000);

    const size_t request_capacity = 65536U;
    char* request = (char*)malloc(request_capacity);
    if (!request) {
        (void)dsd_socket_close(client);
        (void)dsd_socket_close(server->listen_sock);
        DSD_THREAD_RETURN;
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
            server->rc = 0;
            break;
        }
    }

    const char default_response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK";
    const char* response = server->response[0] ? server->response : default_response;
    (void)dsd_socket_send(client, response, strlen(response), 0);
    free(request);
    (void)dsd_socket_close(client);
    (void)dsd_socket_close(server->listen_sock);
    DSD_THREAD_RETURN;
}

static int
rdio_test_http_server_start_ex(rdio_test_http_server* server, char* out_api_url, size_t out_api_url_size,
                               const char* response, int no_request_expected, unsigned int accept_timeout_ms) {
    if (!server || !out_api_url || out_api_url_size == 0) {
        return 1;
    }

    DSD_MEMSET(server, 0, sizeof(*server));
    server->listen_sock = DSD_INVALID_SOCKET;
    server->rc = 1;
    server->no_request_expected = no_request_expected;
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

    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;
    hist->Event_History_Items[0].source_id = 660045;
    hist->Event_History_Items[0].channel = 851012500;
    hist->Event_History_Items[0].enc = 1;
    DSD_SNPRINTF(hist->Event_History_Items[0].sysid_string, sizeof(hist->Event_History_Items[0].sysid_string), "%s",
                 "P25_TEST");
    DSD_SNPRINTF(hist->Event_History_Items[0].t_name, sizeof(hist->Event_History_Items[0].t_name), "%s", "FIRE DISP");

    if (dsd_rdio_export_call(opts, hist, wav_path) != 0) {
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

    int rc = 0;
    if (!strstr(body, "\"start_time\": 1700000000")) {
        DSD_FPRINTF(stderr, "sidecar missing start_time\n%s\n", body);
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
    if (dsd_rdio_export_call(opts, hist, wav_path) != 0) {
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

    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    if (dsd_rdio_export_call(opts, hist, wav_path) != 0) {
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

    int rc = 0;
    if (!strstr(body, "\"stop_time\": 1700000002")) {
        DSD_FPRINTF(stderr, "stop_time should reflect 2-second 48k recording\n%s\n", body);
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

    hist->Event_History_Items[0].event_time = (time_t)1700000100;
    hist->Event_History_Items[0].target_id = 1202;

    if (dsd_rdio_export_call(opts, hist, wav_path) != 0) {
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

    int rc = 0;
    if (!strstr(body, "\"stop_time\": 1700000103")) {
        DSD_FPRINTF(stderr, "stop_time should skip extra WAV chunks and use 3-second duration\n%s\n", body);
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
    Event_History_I hist;
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
        if (dsd_rdio_export_call(&opts, &hist, wav_path) != 0) {
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
    Event_History_I hist;
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
        if (dsd_rdio_export_call(&opts, &hist, wav_path) != 0) {
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
    Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = 99;
    hist.Event_History_Items[0].event_time = (time_t)1700000200;
    hist.Event_History_Items[0].target_id = 2;

    int rc = 0;
    if (dsd_rdio_export_call(NULL, &hist, wav_path) == 0) {
        DSD_FPRINTF(stderr, "export accepted null opts\n");
        rc = 1;
    }
    if (dsd_rdio_export_call(&opts, &hist, NULL) == 0) {
        DSD_FPRINTF(stderr, "export accepted null wav path\n");
        rc = 1;
    }
    if (dsd_rdio_export_call(&opts, &hist, "") == 0) {
        DSD_FPRINTF(stderr, "export accepted empty wav path\n");
        rc = 1;
    }
    if (dsd_rdio_export_call(&opts, &hist, wav_path) != 0) {
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
    Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = 48;
    hist.Event_History_Items[0].event_time = (time_t)1700000300;

    int rc = 0;
    if (dsd_rdio_export_call(&opts, &hist, wav_path) == 0) {
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
    Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = -1;
    hist.Event_History_Items[0].event_time = (time_t)1700000400;
    hist.Event_History_Items[0].target_id = 44;
    hist.Event_History_Items[0].source_id = 0;
    hist.Event_History_Items[0].channel = 12345;
    DSD_SNPRINTF(hist.Event_History_Items[0].tgt_str, sizeof(hist.Event_History_Items[0].tgt_str), "TG \"A\"\\B\n\t%c",
                 1);
    DSD_SNPRINTF(hist.Event_History_Items[0].sysid_string, sizeof(hist.Event_History_Items[0].sysid_string),
                 "SYS \"Q\"\\R\b\f\r");

    if (dsd_rdio_export_call(&opts, &hist, wav_path) != 0) {
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
    Event_History_I hist;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&hist, 0, sizeof(hist));
    opts.rdio_mode = DSD_RDIO_MODE_DIRWATCH;
    opts.rdio_system_id = 7;
    hist.Event_History_Items[0].event_time = (time_t)1700000500;
    hist.Event_History_Items[0].target_id = 77;
    hist.Event_History_Items[0].gi = 1;

    if (dsd_rdio_export_call(&opts, &hist, wav_path) != 0) {
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

    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    int rc = 0;
    if (dsd_rdio_export_call(opts, hist, wav_path) != 0) {
        DSD_FPRINTF(stderr, "api enqueue path failed\n");
        rc = 1;
    }

    dsd_rdio_upload_shutdown();
    dsd_rdio_upload_shutdown();

    if (!file_exists(json_path)) {
        DSD_FPRINTF(stderr, "sidecar missing after API shutdown drain\n");
        rc = 1;
    }

    (void)remove(json_path);
    (void)remove(wav_path);
    remove_empty_dir(dir_template);
    free(hist);
    free(opts);
    return rc;
#endif
}

static int
test_api_delete_after_successful_upload(void) {
#if !defined(USE_CURL) || DSD_PLATFORM_WIN_NATIVE
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
    DSD_SNPRINTF(opts->rdio_api_url, sizeof(opts->rdio_api_url), "%s", api_url);
    opts->rdio_api_url[sizeof(opts->rdio_api_url) - 1] = '\0';
    DSD_SNPRINTF(opts->rdio_api_key, sizeof(opts->rdio_api_key), "%s", "test-key");
    opts->rdio_api_key[sizeof(opts->rdio_api_key) - 1] = '\0';

    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    int rc = 0;
    if (dsd_rdio_export_call(opts, hist, wav_path) != 0) {
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
    if (file_exists(wav_path)) {
        DSD_FPRINTF(stderr, "WAV should be deleted after successful API upload\n");
        (void)remove(wav_path);
        rc = 1;
    }
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

    hist->Event_History_Items[0].event_time = (time_t)1700000000;
    hist->Event_History_Items[0].target_id = 1201;

    /* Queue one API upload, then drain the worker before checking server observations. */
    int rc = 0;
    if (dsd_rdio_export_call(opts, hist, wav_path) != 0) {
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
    rc |= test_missing_talkgroup_rejects_sidecar();
    rc |= test_sidecar_escapes_strings_and_tgt_fallback();
    rc |= test_sidecar_private_fallback_and_malformed_wav_duration();
    rc |= test_api_shutdown_drains_queue();
    rc |= test_api_delete_after_successful_upload();
    rc |= test_api_upload_does_not_follow_redirect();
    return rc;
}
