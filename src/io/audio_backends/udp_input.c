// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* UDP PCM16LE input backend */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/exitflag.h>
#include <errno.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <sys/socket.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"

/** @brief Simple single-producer/single-consumer ring for PCM16 samples. */
typedef struct udp_input_ring {
    int16_t* buf;
    size_t cap; // in samples
    size_t head;
    size_t tail;
    /* Samples written so far, a backlog mark's count (issue #834): written under m, read by a mark on any thread. Those
       taken out, read or dropped, are written less those the ring holds. */
    dsd_atomic_u64 written;
    dsd_mutex_t m;
    dsd_cond_t cv;
} udp_input_ring;

/** @brief UDP input backend state shared across the reader thread and callers. */
typedef struct udp_input_ctx {
    dsd_socket_t sockfd;
    atomic_int running;
    udp_input_ring ring;
    dsd_thread_t th;
    int sample_rate;
    /* Counted by the receive thread and copied into the options by the decoder thread (udp_input_mirror_counters()).
       Never counted in the options themselves: the decoder copies them whole for every UI snapshot while this thread
       runs. */
    dsd_atomic_u64 packets;
    dsd_atomic_u64 bytes;
    dsd_atomic_u64 drops;
} udp_input_ctx;

/**
 * @brief Initialize the sample ring with the requested capacity.
 * @param r Ring to initialize.
 * @param cap_samples Capacity in samples (allocates backing buffer).
 */
static void
ring_init(udp_input_ring* r, size_t cap_samples) {
    r->buf = (int16_t*)malloc(cap_samples * sizeof(int16_t));
    r->cap = (r->buf ? cap_samples : 0);
    r->head = r->tail = 0;
    dsd_atomic_u64_init(&r->written, 0U);
    dsd_mutex_init(&r->m);
    dsd_cond_init(&r->cv);
}

/**
 * @brief Destroy the ring buffer and free resources.
 * @param r Ring to destroy.
 */
static void
ring_destroy(udp_input_ring* r) {
    free(r->buf);
    r->buf = NULL;
    r->cap = r->head = r->tail = 0;
    dsd_mutex_destroy(&r->m);
    dsd_cond_destroy(&r->cv);
}

/**
 * @brief Return number of samples currently buffered.
 * @param r Ring to query.
 */
static size_t
ring_used(const udp_input_ring* r) {
    return (r->head + r->cap - r->tail) % r->cap;
}

/**
 * @brief Return remaining free slots in the ring.
 * @param r Ring to query.
 */
static size_t
ring_free_space(const udp_input_ring* r) {
    return r->cap - 1 - ring_used(r);
}

/**
 * @brief Write as many samples as will fit into the ring.
 *
 * Drops trailing samples when the ring is full.
 *
 * @param r Ring to write into.
 * @param data Source samples.
 * @param count Number of samples to attempt to write.
 * @return Number of samples actually written.
 */
static size_t
ring_write(udp_input_ring* r, const int16_t* data, size_t count) {
    size_t w = 0;
    while (w < count && ring_free_space(r) > 0) {
        r->buf[r->head] = data[w++];
        r->head = (r->head + 1) % r->cap;
    }
    dsd_atomic_u64_store_relaxed(&r->written, dsd_atomic_u64_load_relaxed(&r->written) + (uint64_t)w);
    return w;
}

/** @brief Wake any thread blocked on the ring condition variable. */
static void
ring_signal(udp_input_ring* r) {
    dsd_mutex_lock(&r->m);
    dsd_cond_signal(&r->cv);
    dsd_mutex_unlock(&r->m);
}

/**
 * @brief Copy the receive thread's counters into the options the UI reads.
 *
 * The decoder thread only: it owns the options, and publishes their snapshots.
 *
 * @param opts Decoder options receiving the counts.
 * @param ctx UDP input whose counts they are.
 */
static void
udp_input_mirror_counters(dsd_opts* opts, const udp_input_ctx* ctx) {
    opts->udp_in_packets = (unsigned long long)dsd_atomic_u64_load_relaxed(&ctx->packets);
    opts->udp_in_bytes = (unsigned long long)dsd_atomic_u64_load_relaxed(&ctx->bytes);
    opts->udp_in_drops = (unsigned long long)dsd_atomic_u64_load_relaxed(&ctx->drops);
}

/**
 * @brief Background UDP receive thread that widens PCM16LE datagrams.
 *
 * Receives UDP datagrams, converts to int16 samples, and pushes them into the
 * ring while tracking drop statistics on overflow. It never touches the
 * decoder's options.
 *
 * @param arg Pointer to the owning `udp_input_ctx`.
 * @return NULL on exit.
 */
static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    udp_rx_thread(void* arg) {
    udp_input_ctx* ctx = (udp_input_ctx*)arg;
    const size_t max_bytes = 65536;
    uint8_t* buf = (uint8_t*)malloc(max_bytes);
    if (!buf) {
        DSD_THREAD_RETURN;
    }

    while (atomic_load(&ctx->running)) {
        int n = dsd_socket_recv(ctx->sockfd, buf, max_bytes, 0);
        if (n < 0) {
            int err = dsd_socket_get_error();
#if DSD_PLATFORM_WIN_NATIVE
            if (err == WSAEINTR) {
                continue;
            }
            if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT) {
                dsd_sleep_ms(1);
                continue;
            }
#else
            if (err == EINTR) {
                continue;
            }
            if (err == EAGAIN || (EWOULDBLOCK != EAGAIN && err == EWOULDBLOCK)) {
                dsd_sleep_ms(1);
                continue;
            }
#endif
            // fatal error -> stop
            break;
        }
        if (n == 0) {
            // no data
            continue;
        }
        (void)dsd_atomic_u64_fetch_add_relaxed(&ctx->packets, 1U);
        (void)dsd_atomic_u64_fetch_add_relaxed(&ctx->bytes, (uint64_t)n);
        // Must be even number of bytes for int16 samples
        size_t nsamp = (size_t)(n / 2);
        if (nsamp == 0) {
            continue;
        }

        // Write into ring, account for drops
        const int16_t* s = (const int16_t*)buf; // assumes little-endian input
        size_t wrote = 0;
        dsd_mutex_lock(&ctx->ring.m);
        wrote = ring_write(&ctx->ring, s, nsamp);
        if (wrote < nsamp) {
            (void)dsd_atomic_u64_fetch_add_relaxed(&ctx->drops, (uint64_t)(nsamp - wrote));
        }
        dsd_mutex_unlock(&ctx->ring.m);
        ring_signal(&ctx->ring);
    }

    free(buf);
    DSD_THREAD_RETURN;
}

/* The address to bind on @p port: 0.0.0.0 is any address, an empty or NULL @p bindaddr loopback, anything else a
   numeric address. A port htons() would narrow is refused here, before the binds that take no lookup to refuse it
   (issue #743 B15). */
static int
udp_input_bind_address(const char* bindaddr, int port, struct sockaddr_in* addr) {
    if (port < 0 || port > 65535) {
        DSD_FPRINTF(stderr, "Invalid UDP input port %d\n", port);
        return -1;
    }
    DSD_MEMSET(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
    if (!bindaddr || bindaddr[0] == '\0') {
        addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return 0;
    }
    if (strcmp(bindaddr, "0.0.0.0") == 0) {
        addr->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }
    if (dsd_socket_resolve(bindaddr, port, addr) != 0) {
        DSD_FPRINTF(stderr, "Invalid UDP bind address: %s\n", bindaddr);
        return -1;
    }
    return 0;
}

/* The ring's count for a backlog mark (<dsd-neo/platform/input_backlog.h>): the samples written into it so far. The
   datagrams the socket holds that the receive thread has not taken yet count as later. */
static uint64_t
udp_input_arrived(const void* queue) {
    const udp_input_ctx* ctx = (const udp_input_ctx*)queue;
    return dsd_atomic_u64_load_relaxed(&ctx->ring.written);
}

/**
 * @brief Start UDP input thread and ring.
 *
 * Creates the UDP socket, configures timeouts/buffers, spawns the reader
 * thread, and installs the context into `opts->udp_in_ctx`.
 *
 * @param opts Decoder options receiving backend context and counters.
 * @param bindaddr Address to bind (IPv4/IPv6 string).
 * @param port UDP port number.
 * @param samplerate Expected sample rate (Hz) used for ring sizing.
 * @return 0 on success; negative on error.
 */
int
udp_input_start(dsd_opts* opts, const char* bindaddr, int port, int samplerate) {
    if (!opts) {
        return -1;
    }
    if (opts->udp_in_ctx) {
        return 0; // already started
    }

    struct sockaddr_in addr;
    if (udp_input_bind_address(bindaddr, port, &addr) != 0) {
        return -1;
    }
    dsd_socket_t sockfd = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd == DSD_INVALID_SOCKET) {
        DSD_FPRINTF(stderr, "Error creating UDP input socket\n");
        return -1;
    }

    // Increase OS receive buffer if possible
    int rcvbuf = 4 * 1024 * 1024;
    (void)dsd_socket_setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    // Set a short receive timeout so thread can notice stop requests
    (void)dsd_socket_set_recv_timeout(sockfd, 200); // 200ms

    if (dsd_socket_bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        DSD_FPRINTF(stderr, "Failed to bind UDP %s:%d\n", bindaddr ? bindaddr : "127.0.0.1", port);
        dsd_socket_close(sockfd);
        return -1;
    }

    udp_input_ctx* ctx = (udp_input_ctx*)calloc(1, sizeof(udp_input_ctx));
    if (!ctx) {
        dsd_socket_close(sockfd);
        return -1;
    }
    ctx->sockfd = sockfd;
    atomic_store(&ctx->running, 1);
    ctx->sample_rate = samplerate;
    /* Counting on from the options' counts, so a restarted input keeps the totals it showed. */
    dsd_atomic_u64_init(&ctx->packets, (uint64_t)opts->udp_in_packets);
    dsd_atomic_u64_init(&ctx->bytes, (uint64_t)opts->udp_in_bytes);
    dsd_atomic_u64_init(&ctx->drops, (uint64_t)opts->udp_in_drops);

    // Ring capacity: ~500ms at samplerate
    size_t cap = (size_t)(samplerate / 2);
    if (cap < 48000) {
        cap = 48000; // minimum
    }
    ring_init(&ctx->ring, cap);
    if (ctx->ring.cap == 0) {
        dsd_socket_close(sockfd);
        free(ctx);
        return -1;
    }

    opts->udp_in_ctx = ctx;
    opts->udp_in_sockfd = sockfd;
    /* The input the decoder reads from now on: a rigctl retune's drop is measured on its ring. */
    dsd_input_backlog_attach(ctx, udp_input_arrived);
    int rc = dsd_thread_create(&ctx->th, udp_rx_thread, ctx);
    if (rc != 0) {
        dsd_input_backlog_detach(ctx);
        ring_destroy(&ctx->ring);
        dsd_socket_close(sockfd);
        free(ctx);
        opts->udp_in_ctx = NULL;
        opts->udp_in_sockfd = DSD_INVALID_SOCKET;
        return -1;
    }
    dsd_opts_reset_pcm_input_state(opts);
    return 0;
}

/**
 * @brief Stop UDP input backend and reclaim resources.
 * @param opts Decoder options containing the UDP context.
 */
void
udp_input_stop(dsd_opts* opts) {
    if (!opts || !opts->udp_in_ctx) {
        return;
    }
    udp_input_ctx* ctx = (udp_input_ctx*)opts->udp_in_ctx;
    dsd_input_backlog_detach(ctx);
    dsd_socket_t sockfd = ctx->sockfd;
    atomic_store(&ctx->running, 0);
    // wake any blocked reader
    ring_signal(&ctx->ring);
    dsd_thread_join(ctx->th);
    /* The receive thread is gone: the last counts it made are final. */
    udp_input_mirror_counters(opts, ctx);
    if (sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_close(sockfd);
        ctx->sockfd = DSD_INVALID_SOCKET;
    }
    ring_destroy(&ctx->ring);
    free(ctx);
    opts->udp_in_ctx = NULL;
    opts->udp_in_sockfd = DSD_INVALID_SOCKET;
}

/**
 * @brief Read a single sample from the UDP ring.
 *
 * Blocks (with a short timed wait) until a real sample is available or the
 * decoder is shutting down. This avoids inserting synthetic samples, which
 * skews symbol timing and breaks demod/framesync.
 *
 * @param opts Decoder options containing UDP context.
 * @param out [out] Receives one PCM16 sample.
 * @return 1 on success, 0 on shutdown.
 */
int
udp_input_read_sample(dsd_opts* opts, int16_t* out) {
    // Block until we have a real sample (do not synthesize silence; it breaks symbol timing).
    for (;;) {
        const int got = udp_input_read_sample_wait(opts, out, 100U);
        if (got >= 0) {
            return got;
        }
    }
}

long
udp_input_discard_through(dsd_opts* opts, const dsd_input_cut* cut) {
    if (!opts || !opts->udp_in_ctx || !dsd_input_backlog_cut_applies(cut, opts->udp_in_ctx)) {
        return 0;
    }
    udp_input_ctx* ctx = (udp_input_ctx*)opts->udp_in_ctx;
    dsd_mutex_lock(&ctx->ring.m);
    const size_t used = ring_used(&ctx->ring);
    /* The samples read or dropped so far are those written less those the ring still holds. */
    const uint64_t gone = dsd_atomic_u64_load_relaxed(&ctx->ring.written) - (uint64_t)used;
    const uint64_t owed = cut->arrived > gone ? cut->arrived - gone : 0U;
    const size_t dropped = owed < (uint64_t)used ? (size_t)owed : used;
    ctx->ring.tail = (ctx->ring.tail + dropped) % ctx->ring.cap;
    dsd_mutex_unlock(&ctx->ring.m);
    return (long)dropped;
}

/**
 * @brief Read one sample from the UDP ring, waiting at most @p timeout_ms for one.
 *
 * One timed wait on an empty ring, so a caller can do something else while the input is silent (the decoder applies
 * queued commands; issue #634). A spurious wakeup can end the wait early, as a timeout.
 *
 * @param opts Decoder options containing UDP context; their packet, byte and drop counts are brought up to date.
 * @param out [out] Receives one PCM16 sample.
 * @param timeout_ms Longest wait for a sample.
 * @return 1 with a sample, 0 on shutdown or a stopped input, -1 when none arrived in time.
 */
int
udp_input_read_sample_wait(dsd_opts* opts, int16_t* out, unsigned int timeout_ms) {
    if (!opts || !opts->udp_in_ctx || !out) {
        return 0;
    }
    udp_input_ctx* ctx = (udp_input_ctx*)opts->udp_in_ctx;
    /* Each read brings the counts the UI shows up to date, on the decoder's thread. */
    udp_input_mirror_counters(opts, ctx);
    if (!atomic_load(&ctx->running)) {
        return 0;
    }
    dsd_mutex_lock(&ctx->ring.m);
    if (ring_used(&ctx->ring) == 0) {
        if (dsd_exitflag_load() || !atomic_load(&ctx->running)) {
            dsd_mutex_unlock(&ctx->ring.m);
            return 0;
        }
        (void)dsd_cond_timedwait(&ctx->ring.cv, &ctx->ring.m, timeout_ms);
        if (ring_used(&ctx->ring) == 0) {
            const int stopped = dsd_exitflag_load() || !atomic_load(&ctx->running);
            dsd_mutex_unlock(&ctx->ring.m);
            return stopped ? 0 : -1;
        }
    }
    *out = ctx->ring.buf[ctx->ring.tail];
    ctx->ring.tail = (ctx->ring.tail + 1) % ctx->ring.cap;
    dsd_mutex_unlock(&ctx->ring.m);
    return 1;
}
