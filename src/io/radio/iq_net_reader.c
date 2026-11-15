// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The network I/Q reader: owns the socket of one `iqtcp:` / `iqudp:` input and turns what arrives into converted
 * samples and stream events for its consumer.
 *
 * It always drains the socket and never waits on its consumer's ring, so a sender that drops what it cannot send
 * (SDR++ ignores short writes) does not find dsd-neo's receive buffer full while dsd-neo keeps up. Every wait is a
 * DSD_IQ_NET_WAIT_SLICE_MS slice, never a bare blocking recv(), so a stop, the exit flag and a stall are seen at least
 * that often. A host name is looked up on a helper thread the reader waits on in slices (iq_net_resolve()), so a slow
 * resolver never holds up a stop.
 *
 * Socket handle protocol (as rtl_tcp's, rtl_device.cpp): the reader publishes its socket under sock_lock, a leaf
 * lock never held across a recv, connect, backoff or callback. A stop latches the flag and shuts down only the
 * published socket, which wakes the reader; the reader closes a socket only after unpublishing it, so a stop never
 * touches a closed descriptor number another thread may have been given since.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/iq_net.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "iq_net_internal.h"

/* A TCP read leaves room for a carried partial sample in front, so one call never exceeds the receive buffer. */
#define IQ_NET_TCP_READ_BYTES   (DSD_IQ_NET_RX_BYTES - 8U)
#define IQ_NET_BACKOFF_SLICE_MS 20U
#define IQ_NET_PEER_MAX         (DSD_IQ_NET_HOST_MAX + 8)
#define IQ_NET_SENDER_MAX       24

typedef struct {
    dsd_atomic_u64 bytes, samples, datagrams, empty_datagrams, malformed_datagrams, foreign_datagrams, known_losses,
        connect_attempts, reconnects, stalls, resumes, carry_discarded_bytes, rate_warnings;
} iq_net_counter_set;

struct dsd_iq_net_reader {
    dsd_iq_net_spec spec;
    dsd_iq_net_reader_config cfg;
    size_t sample_bytes;
    char peer[IQ_NET_PEER_MAX]; /* TCP: the server as the spec names it */

    dsd_mutex_t sock_lock;  /* leaf: guards published */
    dsd_socket_t published; /* the socket a stop shuts down; DSD_INVALID_SOCKET when none */
    dsd_socket_t sock;      /* the reader's own handle to the same socket */
    atomic_int stop;
    atomic_int state;
    atomic_int drop_noted;
    atomic_int local_port;
    atomic_int joinable; /* dsd_iq_net_reader_start() made a thread nobody joined yet */
    atomic_int runnable; /* open succeeded and no loop has been started on it yet */
    int opened;
    dsd_thread_t thread;

    uint8_t* rx;
    float* out;
    iq_net_carry carry;

    /* Reader thread only. */
    int up; /* TCP: connected; UDP: a sender is locked */
    int stalled;
    int delivered_since_up;
    uint64_t last_valid_ms;
    iq_net_rate_monitor rate;
    int ever_locked;
    struct sockaddr_in locked_from;
    char sender[IQ_NET_SENDER_MAX];
    int warned_malformed, noted_foreign, warned_recv_error;

    iq_net_counter_set counters;
};

/* ---- Small helpers ------------------------------------------------------------------------------------------- */

/* The cancellation token every wait, connect and backoff checks: a stop, or the process exiting, which latches the
   reader's own stop so a socket can no longer be published once either was seen. */
static int
iq_net_cancelled(void* context) {
    dsd_iq_net_reader* r = (dsd_iq_net_reader*)context;
    if (atomic_load(&r->stop)) {
        return 1;
    }
    if (dsd_exitflag_load()) {
        atomic_store(&r->stop, 1);
        return 1;
    }
    return 0;
}

static uint64_t
iq_net_now_ms(const dsd_iq_net_reader* r) {
    return r->cfg.clock_ms ? r->cfg.clock_ms(r->cfg.clock_ctx) : dsd_realtime_mono_ms();
}

static void
iq_net_count(dsd_atomic_u64* counter, uint64_t n) {
    (void)dsd_atomic_u64_fetch_add_relaxed(counter, n);
}

static void
iq_net_set_state(dsd_iq_net_reader* r, dsd_iq_net_state next) {
    atomic_store(&r->state, (int)next);
}

static void
iq_net_emit(dsd_iq_net_reader* r, dsd_iq_net_event_kind kind, const char* peer, int error_code, size_t bytes) {
    if (!r->cfg.on_event) {
        return;
    }
    dsd_iq_net_event event;
    event.kind = kind;
    event.peer = peer;
    event.error_code = error_code;
    event.bytes = bytes;
    r->cfg.on_event(r->cfg.ctx, &event);
}

int
iq_net_tcp_error_is_transient(int code) {
#if DSD_PLATFORM_WIN_NATIVE
    return code == WSAEWOULDBLOCK || code == WSAEINTR;
#else
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (code == EWOULDBLOCK) {
        return 1;
    }
#endif
    return code == EAGAIN || code == EINTR;
#endif
}

int
iq_net_udp_error_is_transient(int code) {
#if DSD_PLATFORM_WIN_NATIVE
    /* The UDP socket's receive timeout (DSD_IQ_NET_WAIT_SLICE_MS) ends a recvfrom() with WSAETIMEDOUT. */
    return code == WSAETIMEDOUT || iq_net_tcp_error_is_transient(code);
#else
    return iq_net_tcp_error_is_transient(code);
#endif
}

static int
iq_net_socket_local_port(dsd_socket_t sock) {
    struct sockaddr_in sa;
#if DSD_PLATFORM_WIN_NATIVE
    int len = (int)sizeof(sa);
#else
    socklen_t len = (socklen_t)sizeof(sa);
#endif
    DSD_MEMSET(&sa, 0, sizeof(sa));
    if (getsockname(sock, (struct sockaddr*)&sa, &len) != 0 || sa.sin_family != AF_INET) {
        return -1;
    }
    return (int)ntohs(sa.sin_port);
}

static void
iq_net_endpoint_text(const struct sockaddr_in* sa, char* out, size_t out_size) {
    const uint32_t a = ntohl(sa->sin_addr.s_addr);
    DSD_SNPRINTF(out, out_size, "%u.%u.%u.%u:%u", (unsigned)(a >> 24), (unsigned)((a >> 16) & 0xFFU),
                 (unsigned)((a >> 8) & 0xFFU), (unsigned)(a & 0xFFU), (unsigned)ntohs(sa->sin_port));
}

static int
iq_net_same_endpoint(const struct sockaddr_in* a, const struct sockaddr_in* b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

/* ---- Socket handle protocol ---------------------------------------------------------------------------------- */

/* Publish sock for a stop to shut down. Refused (-1) once a stop latched: the caller closes it. */
static int
iq_net_publish(dsd_iq_net_reader* r, dsd_socket_t sock) {
    (void)dsd_mutex_lock(&r->sock_lock);
    if (atomic_load(&r->stop)) {
        (void)dsd_mutex_unlock(&r->sock_lock);
        return -1;
    }
    r->published = sock;
    r->sock = sock;
    (void)dsd_mutex_unlock(&r->sock_lock);
    atomic_store(&r->local_port, iq_net_socket_local_port(sock));
    return 0;
}

/* Unpublish the reader's socket, then close it. */
static void
iq_net_close_socket(dsd_iq_net_reader* r) {
    (void)dsd_mutex_lock(&r->sock_lock);
    r->published = DSD_INVALID_SOCKET;
    (void)dsd_mutex_unlock(&r->sock_lock);
    if (r->sock != DSD_INVALID_SOCKET) {
        (void)dsd_socket_close(r->sock);
        r->sock = DSD_INVALID_SOCKET;
    }
    atomic_store(&r->local_port, -1);
}

/* ---- Opening ------------------------------------------------------------------------------------------------- */

typedef struct {
    int kind;
    int code;
} iq_net_failure;

static void
iq_net_open_error_fill(const dsd_iq_net_reader* r, const iq_net_failure* f, dsd_iq_net_open_error* err) {
    if (!err) {
        return;
    }
    err->failure_kind = f->kind;
    err->native_code = f->code;
    const char* what = r->spec.kind == DSD_IQ_NET_KIND_TCP ? "cannot connect to" : "cannot receive on";
    switch (f->kind) {
        case DSD_INPUT_FAILURE_RESOLVE:
            DSD_SNPRINTF(err->message, sizeof(err->message), "I/Q stream: cannot find the address of %s", r->spec.host);
            break;
        case DSD_INPUT_FAILURE_REFUSED:
            DSD_SNPRINTF(err->message, sizeof(err->message), "I/Q stream: %s %s (connection refused)", what, r->peer);
            break;
        case DSD_INPUT_FAILURE_TIMEOUT:
            DSD_SNPRINTF(err->message, sizeof(err->message), "I/Q stream: %s %s (no answer within %u s)", what, r->peer,
                         DSD_IQ_NET_CONNECT_TIMEOUT_MS / 1000U);
            break;
        default:
            DSD_SNPRINTF(err->message, sizeof(err->message), "I/Q stream: %s %s (error %d)", what, r->peer, f->code);
            break;
    }
}

/* A host name looked up off the reader thread: the reader and the helper each hold a reference, and whichever lets go
   last frees it, so a stop can abandon a lookup the resolver is still answering. */
typedef struct {
    atomic_int refs;
    atomic_int done;
    int rc;
    int port;
    struct sockaddr_in addr;
    char host[DSD_IQ_NET_HOST_MAX + 1];
} iq_net_lookup;

static void
iq_net_lookup_release(iq_net_lookup* job) {
    if (atomic_fetch_sub(&job->refs, 1) == 1) {
        free(job);
    }
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    iq_net_lookup_main(void* arg) {
    iq_net_lookup* job = (iq_net_lookup*)arg;
    job->rc = dsd_socket_resolve(job->host, job->port, &job->addr);
    atomic_store(&job->done, 1);
    iq_net_lookup_release(job);
    DSD_THREAD_RETURN;
}

enum { IQ_NET_RESOLVED = 0, IQ_NET_RESOLVE_FAILED = -1, IQ_NET_RESOLVE_CANCELLED = -2 };

/* An IPv4 address in the one form inet_pton() reads, four decimal fields of 0..255 with no leading zero, which
   dsd_socket_resolve() reads without asking a resolver. Anything else, "127.0.0.1." or "01.2.3.4" included, is a name
   the resolver is asked for, so it takes the cancellable path. */
static int
iq_net_host_is_numeric(const char* host) {
    int fields = 0;
    const char* c = host;
    for (;;) {
        unsigned value = 0U;
        int digits = 0;
        while (*c >= '0' && *c <= '9') {
            if (digits > 0 && value == 0U) {
                return 0; /* a leading zero */
            }
            value = value * 10U + (unsigned)(*c - '0');
            if (++digits > 3 || value > 255U) {
                return 0;
            }
            c++;
        }
        if (digits == 0) {
            return 0;
        }
        if (++fields == 4) {
            return *c == '\0';
        }
        if (*c != '.') {
            return 0;
        }
        c++;
    }
}

/* The spec's host and port as an address. A numeric address is read at once; a name is looked up on a helper thread
   while this one waits in IQ_NET_BACKOFF_SLICE_MS slices, giving up (and leaving the helper to finish on its own) on
   a stop or exit, which getaddrinfo() itself cannot be asked to. */
static int
iq_net_resolve(dsd_iq_net_reader* r, struct sockaddr_in* addr) {
    if (iq_net_host_is_numeric(r->spec.host)) {
        return dsd_socket_resolve(r->spec.host, r->spec.port, addr) == 0 ? IQ_NET_RESOLVED : IQ_NET_RESOLVE_FAILED;
    }
    iq_net_lookup* job = (iq_net_lookup*)calloc(1, sizeof(*job));
    if (!job) {
        return IQ_NET_RESOLVE_FAILED;
    }
    atomic_init(&job->refs, 2);
    atomic_init(&job->done, 0);
    job->port = r->spec.port;
    DSD_SNPRINTF(job->host, sizeof(job->host), "%s", r->spec.host);
    dsd_thread_t helper;
    if (dsd_thread_create(&helper, iq_net_lookup_main, job) != 0) {
        free(job);
        return IQ_NET_RESOLVE_FAILED;
    }
    (void)dsd_thread_detach(helper);
    while (!atomic_load(&job->done)) {
        if (iq_net_cancelled(r)) {
            iq_net_lookup_release(job);
            return IQ_NET_RESOLVE_CANCELLED;
        }
        dsd_sleep_ms(IQ_NET_BACKOFF_SLICE_MS);
    }
    const int rc = job->rc == 0 ? IQ_NET_RESOLVED : IQ_NET_RESOLVE_FAILED;
    if (rc == IQ_NET_RESOLVED) {
        *addr = job->addr;
    }
    iq_net_lookup_release(job);
    return rc;
}

static void
iq_net_set_rcvbuf(dsd_socket_t sock) {
    const int rcvbuf = DSD_IQ_NET_SO_RCVBUF_BYTES;
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, (int)sizeof(rcvbuf));
}

/* Resolve the host (again at every call) and connect within the bound; DSD_INVALID_SOCKET with *f on failure. */
static dsd_socket_t
iq_net_tcp_connect(dsd_iq_net_reader* r, iq_net_failure* f) {
    iq_net_count(&r->counters.connect_attempts, 1U);
    struct sockaddr_in addr;
    if (iq_net_resolve(r, &addr) != IQ_NET_RESOLVED) {
        /* The resolver reports through its own code (h_errno, getaddrinfo's result), which this call does not see. */
        f->kind = DSD_INPUT_FAILURE_RESOLVE;
        f->code = 0;
        return DSD_INVALID_SOCKET;
    }
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, 0);
    if (sock == DSD_INVALID_SOCKET) {
        f->kind = DSD_INPUT_FAILURE_NETWORK;
        f->code = dsd_socket_get_error();
        return DSD_INVALID_SOCKET;
    }
    /* Before the connect, so the window scale the handshake settles can use it. */
    iq_net_set_rcvbuf(sock);
    (void)dsd_socket_set_keepalive(sock, DSD_SOCKET_KEEPALIVE_IDLE_S, DSD_SOCKET_KEEPALIVE_INTERVAL_S,
                                   DSD_SOCKET_KEEPALIVE_COUNT);
    int code = 0;
    if (dsd_socket_connect_bounded(sock, (const struct sockaddr*)&addr, (int)sizeof(addr),
                                   DSD_IQ_NET_CONNECT_TIMEOUT_MS, iq_net_cancelled, r, &code)
        != 0) {
        (void)dsd_socket_close(sock);
        f->kind = (int)dsd_input_failure_classify_socket(code);
        f->code = code;
        return DSD_INVALID_SOCKET;
    }
    return sock;
}

static dsd_socket_t
iq_net_udp_bind(dsd_iq_net_reader* r, iq_net_failure* f) {
    struct sockaddr_in addr;
    if (iq_net_resolve(r, &addr) != IQ_NET_RESOLVED) {
        /* The resolver reports through its own code (h_errno, getaddrinfo's result), which this call does not see. */
        f->kind = DSD_INPUT_FAILURE_RESOLVE;
        f->code = 0;
        return DSD_INVALID_SOCKET;
    }
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == DSD_INVALID_SOCKET) {
        f->kind = DSD_INPUT_FAILURE_NETWORK;
        f->code = dsd_socket_get_error();
        return DSD_INVALID_SOCKET;
    }
    /* The port is this input's alone: a second socket on it would take some of the datagrams with no error on
       either side. Windows lets another socket's SO_REUSEADDR take a port nobody claimed, so claim it there; elsewhere
       a plain bind already refuses a port in use, and a UDP port is free again as soon as its socket closes (no
       TIME_WAIT), so SO_REUSEADDR is not needed for a rebind either. */
#if DSD_PLATFORM_WIN_NATIVE
    const int one = 1;
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &one, (int)sizeof(one));
#endif
    iq_net_set_rcvbuf(sock);
    (void)dsd_socket_set_recv_timeout(sock, DSD_IQ_NET_WAIT_SLICE_MS);
    if (dsd_socket_bind(sock, (const struct sockaddr*)&addr, (int)sizeof(addr)) != 0) {
        f->code = dsd_socket_get_error();
        f->kind = DSD_INPUT_FAILURE_NETWORK;
        (void)dsd_socket_close(sock);
        return DSD_INVALID_SOCKET;
    }
    return sock;
}

/* ---- Lifecycle ----------------------------------------------------------------------------------------------- */

dsd_iq_net_reader*
dsd_iq_net_reader_create(const dsd_iq_net_spec* spec, const dsd_iq_net_reader_config* config) {
    if (!iq_net_spec_usable(spec)) {
        return NULL;
    }
    dsd_iq_net_reader* r = (dsd_iq_net_reader*)calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->rx = (uint8_t*)malloc(DSD_IQ_NET_RX_BYTES);
    r->out = (float*)malloc(sizeof(float) * 2U * DSD_IQ_NET_MAX_CALLBACK_SAMPLES);
    if (!r->rx || !r->out || dsd_mutex_init(&r->sock_lock) != 0) {
        free(r->rx);
        free(r->out);
        free(r);
        return NULL;
    }
    r->spec = *spec;
    if (config) {
        r->cfg = *config;
    }
    r->sample_bytes = dsd_iq_net_format_sample_bytes(spec->format);
    DSD_SNPRINTF(r->peer, sizeof(r->peer), "%s:%d", spec->host, spec->port);
    r->published = DSD_INVALID_SOCKET;
    r->sock = DSD_INVALID_SOCKET;
    atomic_init(&r->stop, 0);
    atomic_init(&r->state, DSD_IQ_NET_STATE_STOPPED);
    atomic_init(&r->drop_noted, 0);
    atomic_init(&r->local_port, -1);
    atomic_init(&r->joinable, 0);
    atomic_init(&r->runnable, 0);
    return r;
}

int
dsd_iq_net_reader_open(dsd_iq_net_reader* reader, dsd_iq_net_open_error* err) {
    if (err) {
        DSD_MEMSET(err, 0, sizeof(*err));
    }
    if (!reader || reader->opened) {
        return -1;
    }
    reader->opened = 1;
    iq_net_set_state(reader, DSD_IQ_NET_STATE_CONNECTING);
    iq_net_failure f = {DSD_INPUT_FAILURE_NONE, 0};
    const dsd_socket_t sock =
        reader->spec.kind == DSD_IQ_NET_KIND_TCP ? iq_net_tcp_connect(reader, &f) : iq_net_udp_bind(reader, &f);
    if (sock != DSD_INVALID_SOCKET && iq_net_publish(reader, sock) != 0) {
        (void)dsd_socket_close(sock);
        f.kind = DSD_INPUT_FAILURE_NETWORK;
        f.code = 0;
    } else if (sock != DSD_INVALID_SOCKET) {
        iq_net_set_state(reader, DSD_IQ_NET_STATE_WAITING);
        atomic_store(&reader->runnable, 1);
        return 0;
    }
    iq_net_set_state(reader, DSD_IQ_NET_STATE_STOPPED);
    iq_net_open_error_fill(reader, &f, err);
    return -1;
}

static void iq_net_run_loop(dsd_iq_net_reader* r);

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    iq_net_thread_main(void* arg) {
    dsd_runtime_fp_mode_apply_thread("iq_net");
    iq_net_run_loop((dsd_iq_net_reader*)arg);
    DSD_THREAD_RETURN;
}

int
dsd_iq_net_reader_start(dsd_iq_net_reader* reader) {
    /* Only the caller's threads touch joinable and runnable here; the reader thread owns the socket fields. */
    if (!reader || atomic_load(&reader->joinable) || !atomic_exchange(&reader->runnable, 0)) {
        return -1;
    }
    if (dsd_thread_create(&reader->thread, iq_net_thread_main, reader) != 0) {
        atomic_store(&reader->runnable, 1);
        return -1;
    }
    atomic_store(&reader->joinable, 1);
    return 0;
}

void
dsd_iq_net_reader_stop(dsd_iq_net_reader* reader) {
    if (!reader) {
        return;
    }
    atomic_store(&reader->stop, 1);
    (void)dsd_mutex_lock(&reader->sock_lock);
    if (reader->published != DSD_INVALID_SOCKET) {
        (void)dsd_socket_shutdown(reader->published, SHUT_RDWR);
    }
    (void)dsd_mutex_unlock(&reader->sock_lock);
    if (atomic_exchange(&reader->joinable, 0)) {
        (void)dsd_thread_join(reader->thread);
    }
}

void
dsd_iq_net_reader_destroy(dsd_iq_net_reader* reader) {
    if (!reader) {
        return;
    }
    dsd_iq_net_reader_stop(reader);
    iq_net_close_socket(reader);
    (void)dsd_mutex_destroy(&reader->sock_lock);
    free(reader->rx);
    free(reader->out);
    free(reader);
}

/* ---- Accessors ----------------------------------------------------------------------------------------------- */

dsd_iq_net_state
dsd_iq_net_reader_state(const dsd_iq_net_reader* reader) {
    return reader ? (dsd_iq_net_state)atomic_load(&reader->state) : DSD_IQ_NET_STATE_STOPPED;
}

int
dsd_iq_net_reader_available(const dsd_iq_net_reader* reader) {
    return dsd_iq_net_reader_state(reader) == DSD_IQ_NET_STATE_STREAMING ? 1 : 0;
}

void
dsd_iq_net_reader_note_written(dsd_iq_net_reader* reader) {
    if (!reader) {
        return;
    }
    int expected = DSD_IQ_NET_STATE_WAITING;
    (void)atomic_compare_exchange_strong(&reader->state, &expected, (int)DSD_IQ_NET_STATE_STREAMING);
}

void
dsd_iq_net_reader_note_drop(dsd_iq_net_reader* reader) {
    if (reader) {
        atomic_store(&reader->drop_noted, 1);
    }
}

int
dsd_iq_net_reader_local_port(const dsd_iq_net_reader* reader) {
    return reader ? atomic_load(&reader->local_port) : -1;
}

const dsd_iq_net_spec*
dsd_iq_net_reader_spec(const dsd_iq_net_reader* reader) {
    return reader ? &reader->spec : NULL;
}

void
dsd_iq_net_reader_get_counters(const dsd_iq_net_reader* reader, dsd_iq_net_counters* out) {
    if (!out) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!reader) {
        return;
    }
    const iq_net_counter_set* c = &reader->counters;
    out->bytes = dsd_atomic_u64_load_relaxed(&c->bytes);
    out->samples = dsd_atomic_u64_load_relaxed(&c->samples);
    out->datagrams = dsd_atomic_u64_load_relaxed(&c->datagrams);
    out->empty_datagrams = dsd_atomic_u64_load_relaxed(&c->empty_datagrams);
    out->malformed_datagrams = dsd_atomic_u64_load_relaxed(&c->malformed_datagrams);
    out->foreign_datagrams = dsd_atomic_u64_load_relaxed(&c->foreign_datagrams);
    out->known_losses = dsd_atomic_u64_load_relaxed(&c->known_losses);
    out->connect_attempts = dsd_atomic_u64_load_relaxed(&c->connect_attempts);
    out->reconnects = dsd_atomic_u64_load_relaxed(&c->reconnects);
    out->stalls = dsd_atomic_u64_load_relaxed(&c->stalls);
    out->resumes = dsd_atomic_u64_load_relaxed(&c->resumes);
    out->carry_discarded_bytes = dsd_atomic_u64_load_relaxed(&c->carry_discarded_bytes);
    out->rate_warnings = dsd_atomic_u64_load_relaxed(&c->rate_warnings);
}

/* ---- Shared stream logic -------------------------------------------------------------------------------------- */

static const char*
iq_net_current_peer(const dsd_iq_net_reader* r) {
    return r->spec.kind == DSD_IQ_NET_KIND_TCP ? r->peer : r->sender;
}

/* The connection (TCP) or locked sender (UDP) is up: measure from now. */
static void
iq_net_mark_up(dsd_iq_net_reader* r, uint64_t now_ms) {
    r->up = 1;
    r->stalled = 0;
    r->delivered_since_up = 0;
    r->last_valid_ms = now_ms;
    iq_net_rate_arm(&r->rate, r->spec.rate_hz, now_ms);
    iq_net_set_state(r, DSD_IQ_NET_STATE_WAITING);
}

/* Declare a stall once no valid sample came for DSD_IQ_NET_STALL_MS while up. */
static void
iq_net_check_stall(dsd_iq_net_reader* r, uint64_t now_ms) {
    if (!r->up || r->stalled || now_ms < r->last_valid_ms || now_ms - r->last_valid_ms < DSD_IQ_NET_STALL_MS) {
        return;
    }
    r->stalled = 1;
    iq_net_count(&r->counters.stalls, 1U);
    iq_net_set_state(r, DSD_IQ_NET_STATE_STALLED);
    iq_net_rate_pause(&r->rate);
    LOG_WARN("I/Q stream: no samples from %s for %u s\n", iq_net_current_peer(r), DSD_IQ_NET_STALL_MS / 1000U);
    if (r->spec.kind == DSD_IQ_NET_KIND_UDP) {
        /* The next valid sender is locked again, this one or another. */
        r->up = 0;
    }
    iq_net_emit(r, DSD_IQ_NET_EVENT_STALL, iq_net_current_peer(r), 0, 0U);
}

static void
iq_net_take_drop_note(dsd_iq_net_reader* r) {
    if (atomic_load(&r->drop_noted) && atomic_exchange(&r->drop_noted, 0)) {
        iq_net_rate_discard(&r->rate);
    }
}

/* Advance the rate telemetry to now_ms with samples received then, and warn when it says so. */
static void
iq_net_rate_feed(dsd_iq_net_reader* r, uint64_t now_ms, uint64_t samples) {
    uint64_t measured = 0;
    if (iq_net_rate_update(&r->rate, now_ms, samples, &measured)) {
        char text[256];
        iq_net_rate_warning_text(text, sizeof(text), measured, r->spec.rate_hz);
        iq_net_count(&r->counters.rate_warnings, 1U);
        LOG_WARN("%s", text);
    }
}

/* One pass of the idle checks a wait slice runs: stall, rate windows, a drop the consumer noted. */
static void
iq_net_tick(dsd_iq_net_reader* r) {
    const uint64_t now_ms = iq_net_now_ms(r);
    iq_net_check_stall(r, now_ms);
    iq_net_take_drop_note(r);
    iq_net_rate_feed(r, now_ms, 0U);
}

/* Convert and hand over samples whole samples at src; a resume after a stall is announced first. */
static void
iq_net_deliver(dsd_iq_net_reader* r, const uint8_t* src, size_t samples) {
    const uint64_t now_ms = iq_net_now_ms(r);
    if (r->stalled) {
        r->stalled = 0;
        iq_net_count(&r->counters.resumes, 1U);
        iq_net_set_state(r, DSD_IQ_NET_STATE_WAITING);
        iq_net_rate_restart(&r->rate, now_ms);
        LOG_INFO("I/Q stream: samples from %s again\n", iq_net_current_peer(r));
        iq_net_emit(r, DSD_IQ_NET_EVENT_RESUME, iq_net_current_peer(r), 0, 0U);
    }
    r->last_valid_ms = now_ms;
    r->delivered_since_up = 1;
    (void)dsd_iq_net_convert(r->spec.format, src, samples, r->out);
    iq_net_count(&r->counters.samples, samples);
    iq_net_take_drop_note(r);
    iq_net_rate_feed(r, now_ms, samples);
    if (r->cfg.on_samples) {
        r->cfg.on_samples(r->cfg.ctx, r->out, samples);
    }
    iq_net_take_drop_note(r);
}

/* ---- TCP ----------------------------------------------------------------------------------------------------- */

enum { IQ_NET_READ_OK = 0, IQ_NET_READ_LOST = -1, IQ_NET_READ_CANCELLED = -2 };

/* Wait one slice and read what came. IQ_NET_READ_LOST with *code (0: the sender closed the connection). */
static int
iq_net_tcp_read_once(dsd_iq_net_reader* r, int* code) {
    iq_net_tick(r);
    const int ready = dsd_socket_wait(r->sock, DSD_SOCKET_WAIT_READ, DSD_IQ_NET_WAIT_SLICE_MS);
    if (iq_net_cancelled(r)) {
        return IQ_NET_READ_CANCELLED;
    }
    if (ready == 0) {
        return IQ_NET_READ_OK;
    }
    if (ready < 0) {
        *code = dsd_socket_get_error();
        return iq_net_tcp_error_is_transient(*code) ? IQ_NET_READ_OK : IQ_NET_READ_LOST;
    }
    /* The carried bytes go in front of the new ones; the carry itself changes only once the read succeeded. */
    const size_t head = iq_net_carry_restore(&r->carry, r->rx);
    const int n = dsd_socket_recv(r->sock, r->rx + head, IQ_NET_TCP_READ_BYTES, 0);
    if (iq_net_cancelled(r)) {
        return IQ_NET_READ_CANCELLED;
    }
    if (n <= 0) {
        *code = n == 0 ? 0 : dsd_socket_get_error();
        return (n < 0 && iq_net_tcp_error_is_transient(*code)) ? IQ_NET_READ_OK : IQ_NET_READ_LOST;
    }
    iq_net_count(&r->counters.bytes, (uint64_t)n);
    /* The wait may have begun before the stall's deadline and ended after it: a pause that long is a stall even
       though no slice saw it, so the samples after it resume (iq_net_deliver()) with the boundary. */
    iq_net_check_stall(r, iq_net_now_ms(r));
    const size_t whole = iq_net_carry_keep(&r->carry, r->rx, head + (size_t)n, r->sample_bytes);
    if (whole > 0) {
        iq_net_deliver(r, r->rx, whole);
    }
    return IQ_NET_READ_OK;
}

static void
iq_net_tcp_lost(dsd_iq_net_reader* r, int code) {
    iq_net_close_socket(r);
    r->up = 0;
    r->stalled = 0;
    iq_net_rate_pause(&r->rate);
    iq_net_set_state(r, DSD_IQ_NET_STATE_CONNECTING);
    if (r->carry.len > 0) {
        /* A new connection starts on the sender's next whole block: the old partial sample has no other half. */
        iq_net_count(&r->counters.carry_discarded_bytes, (uint64_t)r->carry.len);
        r->carry.len = 0;
    }
    if (code == 0) {
        LOG_WARN("I/Q stream: lost the connection to %s (the sender closed it); reconnecting\n", r->peer);
    } else {
        LOG_WARN("I/Q stream: lost the connection to %s (error %d); reconnecting\n", r->peer, code);
    }
    iq_net_emit(r, DSD_IQ_NET_EVENT_DISCONNECTED, r->peer, code, 0U);
}

/* Wait ms of real time in slices, giving up early on a stop or exit. Returns 0 when the wait ran out. */
static int
iq_net_backoff(dsd_iq_net_reader* r, unsigned ms) {
    const uint64_t until_ms = dsd_realtime_mono_ms() + ms;
    while (!iq_net_cancelled(r)) {
        const uint64_t now_ms = dsd_realtime_mono_ms();
        if (now_ms >= until_ms) {
            return 0;
        }
        const uint64_t left = until_ms - now_ms;
        dsd_sleep_ms(left < IQ_NET_BACKOFF_SLICE_MS ? (unsigned)left : IQ_NET_BACKOFF_SLICE_MS);
    }
    return -1;
}

/* Reconnect until a connection is published (1) or the reader is cancelled (0). *attempt counts the attempts since
   the last connection that delivered samples; each one after the first waits its backoff. */
static int
iq_net_tcp_reconnect(dsd_iq_net_reader* r, unsigned* attempt) {
    while (!iq_net_cancelled(r)) {
        if (*attempt > 0) {
            const unsigned step = DSD_IQ_NET_BACKOFF_STEP_MS * *attempt;
            if (iq_net_backoff(r, step < DSD_IQ_NET_BACKOFF_MAX_MS ? step : DSD_IQ_NET_BACKOFF_MAX_MS) != 0) {
                return 0;
            }
        }
        (*attempt)++;
        iq_net_failure f = {DSD_INPUT_FAILURE_NONE, 0};
        const dsd_socket_t sock = iq_net_tcp_connect(r, &f);
        if (sock == DSD_INVALID_SOCKET) {
            continue;
        }
        if (iq_net_publish(r, sock) != 0) {
            (void)dsd_socket_close(sock);
            return 0;
        }
        return 1;
    }
    return 0;
}

static void
iq_net_tcp_run(dsd_iq_net_reader* r) {
    iq_net_mark_up(r, iq_net_now_ms(r));
    iq_net_emit(r, DSD_IQ_NET_EVENT_CONNECTED, r->peer, 0, 0U);
    unsigned attempt = 0;
    for (;;) {
        int code = 0;
        const int rc = iq_net_tcp_read_once(r, &code);
        if (rc == IQ_NET_READ_OK) {
            continue;
        }
        if (rc == IQ_NET_READ_CANCELLED || iq_net_cancelled(r)) {
            return;
        }
        if (r->delivered_since_up) {
            attempt = 0;
        }
        iq_net_tcp_lost(r, code);
        if (!iq_net_tcp_reconnect(r, &attempt)) {
            return;
        }
        iq_net_count(&r->counters.reconnects, 1U);
        iq_net_mark_up(r, iq_net_now_ms(r));
        LOG_INFO("I/Q stream: reconnected to %s\n", r->peer);
        iq_net_emit(r, DSD_IQ_NET_EVENT_RECONNECT, r->peer, 0, 0U);
    }
}

/* ---- UDP ----------------------------------------------------------------------------------------------------- */

/* Lock the sender of a valid datagram. The first lock is CONNECTED; after a stall, the same sender resumes (announced
   by iq_net_deliver()) and another one is a RECONNECT. */
static void
iq_net_udp_lock(dsd_iq_net_reader* r, const struct sockaddr_in* from) {
    const int first = !r->ever_locked;
    const int same = r->ever_locked && iq_net_same_endpoint(from, &r->locked_from);
    r->ever_locked = 1;
    r->locked_from = *from;
    iq_net_endpoint_text(from, r->sender, sizeof(r->sender));
    if (same) {
        r->up = 1;
        return;
    }
    iq_net_mark_up(r, iq_net_now_ms(r));
    if (first) {
        LOG_INFO("I/Q stream: receiving from %s\n", r->sender);
        iq_net_emit(r, DSD_IQ_NET_EVENT_CONNECTED, r->sender, 0, 0U);
        return;
    }
    iq_net_count(&r->counters.reconnects, 1U);
    LOG_INFO("I/Q stream: now receiving from %s\n", r->sender);
    iq_net_emit(r, DSD_IQ_NET_EVENT_RECONNECT, r->sender, 0, 0U);
}

/* A datagram that is not whole samples: dropped whole. From the locked sender it is a known loss. */
static void
iq_net_udp_malformed(dsd_iq_net_reader* r, const struct sockaddr_in* from, size_t n) {
    iq_net_count(&r->counters.malformed_datagrams, 1U);
    if (!r->warned_malformed) {
        char who[IQ_NET_SENDER_MAX];
        iq_net_endpoint_text(from, who, sizeof(who));
        r->warned_malformed = 1;
        LOG_WARN("I/Q stream: dropped a %zu-byte datagram from %s that does not hold whole %s samples; set the "
                 "sender's packet size to a multiple of %zu bytes\n",
                 n, who, dsd_iq_net_format_name(r->spec.format), r->sample_bytes);
    }
    if (r->up) {
        iq_net_count(&r->counters.known_losses, 1U);
        iq_net_rate_discard(&r->rate);
        iq_net_emit(r, DSD_IQ_NET_EVENT_KNOWN_LOSS, r->sender, 0, n);
    }
}

static void
iq_net_udp_datagram(dsd_iq_net_reader* r, const struct sockaddr_in* from, size_t n) {
    if (n == 0) {
        /* GNU Radio and gqrx mark a stop with empty datagrams: no data, and no sign of life either. */
        iq_net_count(&r->counters.empty_datagrams, 1U);
        return;
    }
    iq_net_count(&r->counters.datagrams, 1U);
    iq_net_count(&r->counters.bytes, (uint64_t)n);
    /* As for TCP (iq_net_tcp_read_once()): a pause the wait spanned is a stall, seen before the sender is chosen, so
       the next valid sender is locked again. */
    iq_net_check_stall(r, iq_net_now_ms(r));
    if (r->up && !iq_net_same_endpoint(from, &r->locked_from)) {
        iq_net_count(&r->counters.foreign_datagrams, 1U);
        if (!r->noted_foreign) {
            char who[IQ_NET_SENDER_MAX];
            iq_net_endpoint_text(from, who, sizeof(who));
            r->noted_foreign = 1;
            LOG_INFO("I/Q stream: ignoring datagrams from %s; only one sender is read, now %s\n", who, r->sender);
        }
        return;
    }
    if (n % r->sample_bytes != 0U) {
        iq_net_udp_malformed(r, from, n);
        return;
    }
    if (!r->up) {
        iq_net_udp_lock(r, from);
    }
    iq_net_deliver(r, r->rx, n / r->sample_bytes);
}

static void
iq_net_udp_recv_error(dsd_iq_net_reader* r, int code) {
    if (!r->warned_recv_error) {
        r->warned_recv_error = 1;
        LOG_WARN("I/Q stream: receive error %d on %s; still listening\n", code, r->peer);
    }
    (void)iq_net_backoff(r, IQ_NET_BACKOFF_SLICE_MS);
}

static void
iq_net_udp_run(dsd_iq_net_reader* r) {
    while (!iq_net_cancelled(r)) {
        iq_net_tick(r);
        const int ready = dsd_socket_wait(r->sock, DSD_SOCKET_WAIT_READ, DSD_IQ_NET_WAIT_SLICE_MS);
        if (iq_net_cancelled(r)) {
            return;
        }
        if (ready <= 0) {
            const int code = ready < 0 ? dsd_socket_get_error() : 0;
            if (ready < 0 && !iq_net_udp_error_is_transient(code)) {
                iq_net_udp_recv_error(r, code);
            }
            continue;
        }
        struct sockaddr_in from;
        DSD_MEMSET(&from, 0, sizeof(from));
        int from_len = (int)sizeof(from);
        const int n = dsd_socket_recvfrom(r->sock, r->rx, DSD_IQ_NET_RX_BYTES, 0, (struct sockaddr*)&from, &from_len);
        if (iq_net_cancelled(r)) {
            return;
        }
        if (n < 0) {
            const int code = dsd_socket_get_error();
            if (!iq_net_udp_error_is_transient(code)) {
                iq_net_udp_recv_error(r, code);
            }
            continue;
        }
        iq_net_udp_datagram(r, &from, (size_t)n);
    }
}

static void
iq_net_run_loop(dsd_iq_net_reader* r) {
    if (r->spec.kind == DSD_IQ_NET_KIND_TCP) {
        iq_net_tcp_run(r);
    } else {
        iq_net_udp_run(r);
    }
    iq_net_close_socket(r);
    iq_net_set_state(r, DSD_IQ_NET_STATE_STOPPED);
}

void
dsd_iq_net_reader_run(dsd_iq_net_reader* reader) {
    if (reader && atomic_exchange(&reader->runnable, 0)) {
        iq_net_run_loop(reader);
    }
}
