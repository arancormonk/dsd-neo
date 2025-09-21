// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UDP PCM16LE input backend */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/io/udp_input.h>

typedef struct udp_input_ring {
    int16_t* buf;
    size_t cap; // in samples
    size_t head;
    size_t tail;
    pthread_mutex_t m;
    pthread_cond_t cv;
} udp_input_ring;

typedef struct udp_input_ctx {
    int sockfd;
    int running;
    udp_input_ring ring;
    pthread_t th;
    int sample_rate;
} udp_input_ctx;

static void
ring_init(udp_input_ring* r, size_t cap_samples) {
    r->buf = (int16_t*)malloc(cap_samples * sizeof(int16_t));
    r->cap = (r->buf ? cap_samples : 0);
    r->head = r->tail = 0;
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->cv, NULL);
}

static void
ring_destroy(udp_input_ring* r) {
    if (r->buf) {
        free(r->buf);
    }
    r->buf = NULL;
    r->cap = r->head = r->tail = 0;
    pthread_mutex_destroy(&r->m);
    pthread_cond_destroy(&r->cv);
}

static size_t
ring_used(const udp_input_ring* r) {
    return (r->head + r->cap - r->tail) % r->cap;
}

static size_t
ring_free_space(const udp_input_ring* r) {
    return r->cap - 1 - ring_used(r);
}

static size_t
ring_write(udp_input_ring* r, const int16_t* data, size_t count) {
    size_t w = 0;
    while (w < count && ring_free_space(r) > 0) {
        r->buf[r->head] = data[w++];
        r->head = (r->head + 1) % r->cap;
    }
    return w;
}

static int
ring_read_block(udp_input_ring* r, int16_t* out) {
    pthread_mutex_lock(&r->m);
    while (ring_used(r) == 0) {
        if (exitflag) {
            pthread_mutex_unlock(&r->m);
            return 0;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000L; // +100ms
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000;
        }
        int ret = pthread_cond_timedwait(&r->cv, &r->m, &ts);
        (void)ret; // ignore spuriously
    }
    *out = r->buf[r->tail];
    r->tail = (r->tail + 1) % r->cap;
    pthread_mutex_unlock(&r->m);
    return 1;
}

static void
ring_signal(udp_input_ring* r) {
    pthread_mutex_lock(&r->m);
    pthread_cond_signal(&r->cv);
    pthread_mutex_unlock(&r->m);
}

static int
ring_try_read(udp_input_ring* r, int16_t* out) {
    int ok = 0;
    pthread_mutex_lock(&r->m);
    if (ring_used(r) > 0) {
        *out = r->buf[r->tail];
        r->tail = (r->tail + 1) % r->cap;
        ok = 1;
    }
    pthread_mutex_unlock(&r->m);
    return ok;
}

static void*
udp_rx_thread(void* arg) {
    dsd_opts* opts = (dsd_opts*)arg;
    udp_input_ctx* ctx = (udp_input_ctx*)opts->udp_in_ctx;
    const size_t max_bytes = 65536;
    uint8_t* buf = (uint8_t*)malloc(max_bytes);
    if (!buf) {
        return NULL;
    }

    while (ctx->running) {
        ssize_t n = recv(ctx->sockfd, buf, max_bytes, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            // fatal error -> stop
            break;
        }
        if (n == 0) {
            // no data
            continue;
        }
        opts->udp_in_packets++;
        opts->udp_in_bytes += (unsigned long long)n;
        // Must be even number of bytes for int16 samples
        size_t nsamp = (size_t)(n / 2);
        if (nsamp == 0) {
            continue;
        }

        // Write into ring, account for drops
        const int16_t* s = (const int16_t*)buf; // assumes little-endian input
        size_t wrote = 0;
        pthread_mutex_lock(&ctx->ring.m);
        wrote = ring_write(&ctx->ring, s, nsamp);
        if (wrote < nsamp) {
            opts->udp_in_drops += (unsigned long long)(nsamp - wrote);
        }
        pthread_mutex_unlock(&ctx->ring.m);
        ring_signal(&ctx->ring);
    }

    free(buf);
    return NULL;
}

int
udp_input_start(dsd_opts* opts, const char* bindaddr, int port, int samplerate) {
    if (!opts) {
        return -1;
    }
    if (opts->udp_in_ctx) {
        return 0; // already started
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) {
        fprintf(stderr, "Error creating UDP input socket\n");
        return -1;
    }

    // Increase OS receive buffer if possible
    int rcvbuf = 4 * 1024 * 1024;
    (void)setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Set a short receive timeout so thread can notice stop requests
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200000; // 200 ms
    (void)setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (bindaddr && strlen(bindaddr) > 0) {
        if (strcmp(bindaddr, "0.0.0.0") == 0) {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            if (inet_aton(bindaddr, &addr.sin_addr) == 0) {
                fprintf(stderr, "Invalid UDP bind address: %s\n", bindaddr);
                close(sockfd);
                return -1;
            }
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Failed to bind UDP %s:%d\n", bindaddr ? bindaddr : "127.0.0.1", port);
        close(sockfd);
        return -1;
    }

    udp_input_ctx* ctx = (udp_input_ctx*)calloc(1, sizeof(udp_input_ctx));
    if (!ctx) {
        close(sockfd);
        return -1;
    }
    ctx->sockfd = sockfd;
    ctx->running = 1;
    ctx->sample_rate = samplerate;

    // Ring capacity: ~500ms at samplerate
    size_t cap = (size_t)(samplerate / 2);
    if (cap < 48000) {
        cap = 48000; // minimum
    }
    ring_init(&ctx->ring, cap);
    if (ctx->ring.cap == 0) {
        close(sockfd);
        free(ctx);
        return -1;
    }

    opts->udp_in_ctx = ctx;
    opts->udp_in_sockfd = sockfd;
    int rc = pthread_create(&ctx->th, NULL, udp_rx_thread, opts);
    if (rc != 0) {
        ring_destroy(&ctx->ring);
        close(sockfd);
        free(ctx);
        opts->udp_in_ctx = NULL;
        opts->udp_in_sockfd = 0;
        return -1;
    }
    return 0;
}

void
udp_input_stop(dsd_opts* opts) {
    if (!opts || !opts->udp_in_ctx) {
        return;
    }
    udp_input_ctx* ctx = (udp_input_ctx*)opts->udp_in_ctx;
    ctx->running = 0;
    if (ctx->sockfd > 0) {
        shutdown(ctx->sockfd, SHUT_RD);
        close(ctx->sockfd);
    }
    ctx->sockfd = -1;
    // wake any blocked reader
    ring_signal(&ctx->ring);
    pthread_join(ctx->th, NULL);
    ring_destroy(&ctx->ring);
    free(ctx);
    opts->udp_in_ctx = NULL;
    opts->udp_in_sockfd = 0;
}

int
udp_input_read_sample(dsd_opts* opts, int16_t* out) {
    if (!opts || !opts->udp_in_ctx || !out) {
        return 0;
    }
    udp_input_ctx* ctx = (udp_input_ctx*)opts->udp_in_ctx;
    if (!ctx->running) {
        return 0;
    }

    // Non-blocking try first
    if (ring_try_read(&ctx->ring, out)) {
        return 1;
    }

    // No data available: honor exit, otherwise synthesize silence and throttle slightly
    if (exitflag) {
        return 0;
    }
    *out = 0;
    // throttle ~1ms to avoid busy spin when idle
    usleep(1000);
    return 1;
}
