// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief TCP JSON Lines server for the third-party control/telemetry API.
 *
 * One accept thread, one result-pump thread, and one thread per session. Every
 * socket is non-blocking: a session thread waits on its socket with a short
 * timeout, reads whole lines, answers them, and drains the session's outbox,
 * keeping any unsent tail so a line is never cut. The decode thread (through the
 * feed's observer) and the result pump only append to outboxes.
 *
 * Lifetime: g_srv is written under the lifecycle lock. Everything else that
 * reads it -- the feed observer, the result pump, the accept and session
 * threads -- is registered after it is set and unregistered or joined before it
 * is cleared, so they never see it change.
 */

#include <dsd-neo/api/api.h>
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/log.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include "api_internal.h"
#include "json.h"

/* A peer that has gone away must cost an error return, never a SIGPIPE: Linux takes the flag per send, macOS and the
   BSDs a socket option (set at accept), Windows has no such signal. */
#ifdef MSG_NOSIGNAL
#define DSD_API_SEND_FLAGS MSG_NOSIGNAL
#else
#define DSD_API_SEND_FLAGS 0
#endif

enum {
    DSD_API_MAX_LINE = 256 * 1024,
    DSD_API_OUTBOX_CAP = 1024 * 1024,
    DSD_API_DEFAULT_AUTH_TIMEOUT_MS = 10000,
    DSD_API_POLL_MS = 50,
    DSD_API_READ_CHUNK = 8192,
    DSD_API_READS_PER_WAKE = 8,
    DSD_API_RESULT_POLL_MS = 100,
    DSD_API_LINGER_MS = 1000,
    DSD_API_DEFAULT_MAX_CLIENTS = 8,
    DSD_API_HARD_MAX_CLIENTS = 32,
};

/* How a session ends: not yet, once its queued lines are sent (a refused auth, a malformed line, the peer's EOF), or
   at once (a failed socket, a client too slow to keep its outbox under the cap). */
enum { SESSION_OPEN = 0, SESSION_CLOSE_AFTER_FLUSH = 1, SESSION_CLOSE_NOW = 2 };

typedef struct api_session {
    dsd_socket_t sock;
    dsd_thread_t thread;
    struct api_server* srv;
    dsd_mutex_t mu; /* guards outbox, topics, authenticated and closing */
    dsd_json_buf outbox;
    uint32_t topics;
    int authenticated;
    int closing;
    /* Session thread only. */
    dsd_json_buf pending; /* received bytes not yet ending in a newline */
    dsd_json_buf sending; /* lines taken from the outbox, partly sent */
    uint64_t opened_ms;
    atomic_int finished; /* the thread has returned; the accept thread (or stop) joins and frees it */
    struct api_session* next;
} api_session;

typedef struct api_server {
    dsd_socket_t listen_sock;
    dsd_thread_t accept_thread;
    dsd_thread_t results_thread;
    int results_started;
    atomic_int stop;
    int port;
    dsd_api_config config;          /* as started, for the same-configuration check */
    char token[DSD_API_TOKEN_SIZE]; /* zero-padded, compared over its full width */
    size_t token_len;               /* 0 = no token required */
    dsd_mutex_t mu;                 /* guards sessions and session_count */
    api_session* sessions;
    int session_count; /* linked sessions, finished or not */
    int max_clients;
    uint64_t auth_timeout_ms;
    atomic_int authed_count;
    uint64_t last_tg_seq;  /* result pump only */
    uint64_t last_dec_seq; /* result pump only */
} api_server;

static api_server* g_srv = NULL;
static atomic_int g_topic_interest[DSD_API_TOPIC_COUNT];
static dsd_mutex_t g_lifecycle_mu;
static atomic_int g_lifecycle_mu_state = 0;

static void
ensure_lifecycle_mu(void) {
    if (atomic_load(&g_lifecycle_mu_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_lifecycle_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_lifecycle_mu);
        atomic_store(&g_lifecycle_mu_state, 2);
        return;
    }
    while (atomic_load(&g_lifecycle_mu_state) != 2) {
        dsd_thread_yield();
    }
}

static int
socket_would_block(int err) {
#if DSD_PLATFORM_WIN_NATIVE
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
    return err == EAGAIN || err == EWOULDBLOCK || err == EINTR;
#endif
}

/* Compares the whole token buffer whatever the input, so the time taken says nothing about the token. */
static int
token_matches(const api_server* srv, const char* given) {
    if (given == NULL) {
        return 0;
    }
    const size_t given_len = strlen(given);
    char padded[DSD_API_TOKEN_SIZE];
    DSD_MEMSET(padded, 0, sizeof padded);
    unsigned int diff = (given_len != srv->token_len) ? 1U : 0U;
    if (given_len < sizeof padded) {
        DSD_MEMCPY(padded, given, given_len);
    } else {
        diff = 1U;
    }
    for (size_t i = 0U; i < sizeof padded; i++) {
        diff |= (unsigned int)((unsigned char)padded[i] ^ (unsigned char)srv->token[i]);
    }
    DSD_SECURE_ZERO(padded, sizeof padded);
    return diff == 0U;
}

static int
is_loopback_addr(const char* addr) {
    struct in_addr in4;
    if (addr == NULL || inet_pton(AF_INET, addr, &in4) != 1) {
        return 0;
    }
    return (ntohl(in4.s_addr) >> 24) == 127U;
}

/*============================================================================
 * Topics
 *============================================================================*/

const char*
dsd_api_topic_name(uint32_t bit) {
    switch (bit) {
        case DSD_API_TOPIC_CALL: return "call";
        case DSD_API_TOPIC_EVENT: return "event";
        case DSD_API_TOPIC_SYSTEM: return "system";
        case DSD_API_TOPIC_METRICS: return "metrics";
        case DSD_API_TOPIC_QUALITY: return "quality";
        case DSD_API_TOPIC_STATUS: return "status";
        case DSD_API_TOPIC_RESULT: return "result";
        default: return NULL;
    }
}

int
dsd_api_topics_parse(const dsd_json_node* topics, uint32_t* out_mask) {
    if (topics == NULL || out_mask == NULL || topics->type != DSD_JSON_ARRAY || topics->count == 0U) {
        return -1;
    }
    uint32_t mask = 0U;
    for (size_t i = 0; i < topics->count; i++) {
        const char* name = dsd_json_as_str(topics->items[i]);
        if (name == NULL) {
            return -1;
        }
        if (strcmp(name, "all") == 0) {
            mask |= (1u << DSD_API_TOPIC_COUNT) - 1u;
            continue;
        }
        uint32_t bit = 0U;
        for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
            const char* tn = dsd_api_topic_name(1u << t);
            if (tn != NULL && strcmp(name, tn) == 0) {
                bit = 1u << t;
            }
        }
        if (bit == 0U) {
            return -1;
        }
        mask |= bit;
    }
    *out_mask = mask;
    return 0;
}

int
dsd_api_topic_interest(uint32_t topic) {
    for (int i = 0; i < DSD_API_TOPIC_COUNT; i++) {
        if ((topic & (1u << i)) != 0U && atomic_load(&g_topic_interest[i]) > 0) {
            return 1;
        }
    }
    return 0;
}

static void
topic_interest_apply(uint32_t changed, int add) {
    for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
        if ((changed & (1u << t)) != 0U) {
            (void)atomic_fetch_add(&g_topic_interest[t], add ? 1 : -1);
        }
    }
}

int
dsd_api_client_count(void) {
    const api_server* srv = g_srv;
    return (srv != NULL) ? atomic_load(&srv->authed_count) : 0;
}

/*============================================================================
 * Session outbox
 *============================================================================*/

static int
session_queue_locked(api_session* s, const char* line, size_t len) {
    if (s->closing == SESSION_CLOSE_NOW) {
        return -1;
    }
    if (s->outbox.len + len > (size_t)DSD_API_OUTBOX_CAP || dsd_json_buf_append(&s->outbox, line, len) != 0) {
        /* A client that stops reading is dropped, rather than shown a stream with silent holes in it. */
        s->closing = SESSION_CLOSE_NOW;
        return -1;
    }
    return 0;
}

static void
session_queue(api_session* s, const dsd_json_buf* line) {
    if (line->data == NULL || line->len == 0U) {
        return;
    }
    dsd_mutex_lock(&s->mu);
    (void)session_queue_locked(s, line->data, line->len);
    dsd_mutex_unlock(&s->mu);
}

static void
session_close(api_session* s, int how) {
    dsd_mutex_lock(&s->mu);
    if (how > s->closing) {
        s->closing = how;
    }
    dsd_mutex_unlock(&s->mu);
}

static int
session_closing(api_session* s) {
    dsd_mutex_lock(&s->mu);
    const int closing = s->closing;
    dsd_mutex_unlock(&s->mu);
    return closing;
}

void
dsd_api_broadcast(const char* line, size_t len, uint32_t topic) {
    api_server* srv = g_srv;
    if (srv == NULL || line == NULL || len == 0U) {
        return;
    }
    dsd_mutex_lock(&srv->mu);
    for (api_session* s = srv->sessions; s != NULL; s = s->next) {
        dsd_mutex_lock(&s->mu);
        if (s->authenticated && s->closing == SESSION_OPEN && (s->topics & topic) != 0U) {
            (void)session_queue_locked(s, line, len);
        }
        dsd_mutex_unlock(&s->mu);
    }
    dsd_mutex_unlock(&srv->mu);
}

/* Queue a response that carries only an error, and optionally end the session once it is sent. */
static void
session_error(api_session* s, const dsd_json_node* id, const char* code, const char* message, int close,
              dsd_json_buf* scratch) {
    dsd_json_writer w;
    dsd_api_response_begin(scratch, &w, id, 0);
    dsd_api_response_error(&w, code, message);
    if (dsd_api_response_end(scratch, &w) == 0) {
        session_queue(s, scratch);
    }
    if (close) {
        session_close(s, SESSION_CLOSE_AFTER_FLUSH);
    }
}

/*============================================================================
 * Request handlers
 *============================================================================*/

static void
handle_get(api_session* s, const dsd_json_node* id, const dsd_json_node* params, dsd_json_buf* scratch) {
    const dsd_json_node* what_node = dsd_json_obj_get(params, "what");
    const char* what = (what_node == NULL) ? "status" : dsd_json_as_str(what_node);
    dsd_json_buf cached;
    dsd_json_buf_init(&cached);
    const int have = (what != NULL) ? dsd_api_feed_latest(what, &cached) : -1;
    if (have < 0) {
        dsd_json_buf_free(&cached);
        session_error(s, id, "invalid_params", "what must be status, snapshot, call, system, metrics or quality", 0,
                      scratch);
        return;
    }
    dsd_json_writer w;
    dsd_api_response_begin(scratch, &w, id, 1);
    (void)dsd_json_kv_str(&w, "what", what);
    if (have > 0 && cached.data != NULL) {
        (void)dsd_json_key(&w, "data");
        (void)dsd_json_value_raw(&w, cached.data);
    } else {
        (void)dsd_json_kv_null(&w, "data");
    }
    if (dsd_api_response_end(scratch, &w) == 0) {
        session_queue(s, scratch);
    }
    dsd_json_buf_free(&cached);
}

static void
handle_subscribe(api_session* s, const dsd_json_node* id, const dsd_json_node* params, int add, dsd_json_buf* scratch) {
    uint32_t mask = 0U;
    if (dsd_api_topics_parse(dsd_json_obj_get(params, "topics"), &mask) != 0) {
        session_error(s, id, "invalid_params", "topics must be a non-empty array of topic names", 0, scratch);
        return;
    }
    dsd_mutex_lock(&s->mu);
    const uint32_t changed = add ? (mask & ~s->topics) : (mask & s->topics);
    s->topics = add ? (s->topics | mask) : (s->topics & ~mask);
    topic_interest_apply(changed, add);
    const uint32_t now = s->topics;
    dsd_mutex_unlock(&s->mu);

    dsd_json_writer w;
    dsd_api_response_begin(scratch, &w, id, 1);
    (void)dsd_json_key(&w, "topics");
    (void)dsd_json_arr_begin(&w);
    for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
        if ((now & (1u << t)) != 0U) {
            (void)dsd_json_value_str(&w, dsd_api_topic_name(1u << t));
        }
    }
    (void)dsd_json_arr_end(&w);
    if (dsd_api_response_end(scratch, &w) == 0) {
        session_queue(s, scratch);
    }
}

static void
handle_list_commands(api_session* s, const dsd_json_node* id, dsd_json_buf* scratch) {
    dsd_json_buf catalog;
    dsd_json_buf_init(&catalog);
    if (dsd_api_command_catalog(&catalog) != 0 || catalog.data == NULL) {
        dsd_json_buf_free(&catalog);
        session_error(s, id, "internal_error", "could not build the command catalog", 0, scratch);
        return;
    }
    dsd_json_writer w;
    dsd_api_response_begin(scratch, &w, id, 1);
    (void)dsd_json_key(&w, "commands");
    (void)dsd_json_value_raw(&w, catalog.data);
    if (dsd_api_response_end(scratch, &w) == 0) {
        session_queue(s, scratch);
    }
    dsd_json_buf_free(&catalog);
}

static void
handle_auth(api_session* s, const dsd_json_node* id, const dsd_json_node* params, dsd_json_buf* scratch) {
    api_server* srv = s->srv;
    const int ok = (srv->token_len == 0U) || token_matches(srv, dsd_json_as_str(dsd_json_obj_get(params, "token")));
    if (!ok) {
        /* One wrong guess ends the connection; a client cannot probe the token over one session. */
        session_error(s, id, "unauthorized", "invalid token", 1, scratch);
        return;
    }
    dsd_mutex_lock(&s->mu);
    if (!s->authenticated) {
        s->authenticated = 1;
        (void)atomic_fetch_add(&srv->authed_count, 1);
    }
    dsd_mutex_unlock(&s->mu);
    dsd_json_writer w;
    dsd_api_response_begin(scratch, &w, id, 1);
    (void)dsd_json_kv_bool(&w, "authenticated", 1);
    if (dsd_api_response_end(scratch, &w) == 0) {
        session_queue(s, scratch);
    }
}

static int
line_is_blank(const char* line) {
    for (const char* p = line; *p != '\0'; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r') {
            return 0;
        }
    }
    return 1;
}

static void
handle_ping(api_session* s, const dsd_json_node* id, int authenticated, dsd_json_buf* scratch) {
    dsd_json_writer w;
    dsd_api_response_begin(scratch, &w, id, 1);
    (void)dsd_json_kv_i64(&w, "protocol", DSD_API_PROTOCOL_VERSION);
    (void)dsd_json_kv_bool(&w, "authenticated", authenticated);
    if (dsd_api_response_end(scratch, &w) == 0) {
        session_queue(s, scratch);
    }
}

/* The commands only an authenticated session may send: the session commands, then the command table. */
static void
handle_authenticated(api_session* s, const char* cmd, const dsd_json_node* root, const dsd_json_node* fields,
                     dsd_json_buf* scratch) {
    const dsd_json_node* id = dsd_json_obj_get(root, "id");
    if (strcmp(cmd, "subscribe") == 0 || strcmp(cmd, "unsubscribe") == 0) {
        handle_subscribe(s, id, fields, cmd[0] == 's', scratch);
    } else if (strcmp(cmd, "get") == 0) {
        handle_get(s, id, fields, scratch);
    } else if (strcmp(cmd, "list_commands") == 0) {
        handle_list_commands(s, id, scratch);
    } else if (dsd_api_command_execute(root, scratch) == 0) {
        session_queue(s, scratch);
    } else {
        session_error(s, id, "internal_error", "command dispatch failed", 0, scratch);
    }
}

static void
handle_request(api_session* s, const char* line, dsd_json_buf* scratch) {
    char err[96] = "";
    dsd_json_node* root = NULL;
    if (dsd_json_parse(line, &root, err, sizeof err) != 0) {
        /* Not NDJSON at all -- an HTTP request a web page aimed at this port, say -- so nothing further on this
           connection is trusted to be a request: answer once and hang up. */
        session_error(s, NULL, "parse_error", err, 1, scratch);
        return;
    }
    const dsd_json_node* id = dsd_json_obj_get(root, "id");
    const char* cmd = dsd_json_as_str(dsd_json_obj_get(root, "cmd"));
    const dsd_json_node* params = dsd_json_obj_get(root, "params");
    dsd_mutex_lock(&s->mu);
    const int authenticated = s->authenticated;
    dsd_mutex_unlock(&s->mu);
    if (root->type != DSD_JSON_OBJECT || cmd == NULL || (params != NULL && params->type != DSD_JSON_OBJECT)) {
        session_error(s, id, "bad_request", "expected an object with a \"cmd\" string and an object \"params\"", 0,
                      scratch);
    } else if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "ping") == 0) {
        handle_ping(s, id, authenticated, scratch);
    } else if (strcmp(cmd, "auth") == 0) {
        /* Session commands also take their fields at the top level: {"cmd":"auth","token":"..."}. */
        handle_auth(s, id, params != NULL ? params : root, scratch);
    } else if (!authenticated) {
        session_error(s, id, "unauthorized", "authenticate first", 0, scratch);
    } else {
        handle_authenticated(s, cmd, root, params != NULL ? params : root, scratch);
    }
    dsd_json_free(root);
}

/*============================================================================
 * Session thread
 *============================================================================*/

/* Answer every complete line in the pending buffer. */
static void
process_pending(api_session* s, dsd_json_buf* scratch) {
    size_t start = 0U;
    while (start < s->pending.len && session_closing(s) == SESSION_OPEN) {
        char* line = s->pending.data + start;
        char* nl = (char*)memchr(line, '\n', s->pending.len - start);
        if (nl == NULL) {
            break;
        }
        const size_t line_len = (size_t)(nl - line);
        if (line_len > (size_t)DSD_API_MAX_LINE) {
            break;
        }
        *nl = '\0';
        if (memchr(line, '\0', line_len) != NULL) {
            /* The parser reads C strings: what follows a raw NUL would be silently ignored. */
            session_error(s, NULL, "parse_error", "NUL byte in request", 1, scratch);
        } else if (!line_is_blank(line)) {
            handle_request(s, line, scratch);
        }
        start += line_len + 1U;
    }
    /* Consumed request bytes (keys among them) are wiped, not left behind in the buffer. */
    dsd_json_buf_consume(&s->pending, start);
    if (s->pending.len > (size_t)DSD_API_MAX_LINE && session_closing(s) == SESSION_OPEN) {
        session_error(s, NULL, "line_too_long", "request line exceeds 262144 bytes", 1, scratch);
    }
}

/* Read what has arrived. Returns -1 when the connection failed; a peer that finished sending is answered, then
   closed. */
static int
session_read(api_session* s, dsd_json_buf* scratch) {
    char buf[DSD_API_READ_CHUNK];
    for (int i = 0; i < DSD_API_READS_PER_WAKE && session_closing(s) == SESSION_OPEN; i++) {
        const int n = dsd_socket_recv(s->sock, buf, sizeof buf, 0);
        if (n > 0) {
            const int rc = dsd_json_buf_append(&s->pending, buf, (size_t)n);
            DSD_SECURE_ZERO(buf, (size_t)n);
            if (rc != 0) {
                return -1;
            }
            process_pending(s, scratch);
            continue;
        }
        if (n == 0) {
            session_close(s, SESSION_CLOSE_AFTER_FLUSH);
            return 0;
        }
        return socket_would_block(dsd_socket_get_error()) ? 0 : -1;
    }
    return 0;
}

/* Send what is queued, keeping an unsent tail for the next call. Returns -1 when the connection failed. */
static int
session_flush(api_session* s) {
    if (s->sending.len == 0U) {
        dsd_mutex_lock(&s->mu);
        if (s->outbox.len > 0U) {
            const dsd_json_buf queued = s->outbox;
            s->outbox = s->sending;
            s->sending = queued;
            dsd_json_buf_reset(&s->outbox);
        }
        dsd_mutex_unlock(&s->mu);
    }
    while (s->sending.len > 0U) {
        const size_t chunk = (s->sending.len > 65536U) ? 65536U : s->sending.len;
        const int n = dsd_socket_send(s->sock, s->sending.data, chunk, DSD_API_SEND_FLAGS);
        if (n > 0) {
            dsd_json_buf_consume(&s->sending, (size_t)n);
            continue;
        }
        return (n < 0 && socket_would_block(dsd_socket_get_error())) ? 0 : -1;
    }
    return 0;
}

static int
session_has_output(api_session* s) {
    if (s->sending.len > 0U) {
        return 1;
    }
    dsd_mutex_lock(&s->mu);
    const int queued = s->outbox.len > 0U;
    dsd_mutex_unlock(&s->mu);
    return queued;
}

/* End an orderly close (the last reply sent): send our FIN, then read and drop what the peer still sends until it
   closes too or the linger time runs out. Closing a socket with unread input makes the stack send a reset, which can
   destroy the reply before the peer reads it -- the parse_error or unauthorized line that explains the close. */
static void
session_linger(api_session* s) {
    char drain[DSD_API_READ_CHUNK];
    (void)dsd_socket_shutdown(s->sock, SHUT_WR);
    const uint64_t deadline = dsd_realtime_mono_ms() + (uint64_t)DSD_API_LINGER_MS;
    while (atomic_load(&s->srv->stop) == 0 && dsd_realtime_mono_ms() < deadline) {
        const int ready = dsd_socket_wait(s->sock, DSD_SOCKET_WAIT_READ, (unsigned int)DSD_API_POLL_MS);
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }
        const int n = dsd_socket_recv(s->sock, drain, sizeof drain, 0);
        if (n == 0 || (n < 0 && !socket_would_block(dsd_socket_get_error()))) {
            break;
        }
    }
    DSD_SECURE_ZERO(drain, sizeof drain);
}

static void
session_retire(api_session* s) {
    dsd_mutex_lock(&s->mu);
    topic_interest_apply(s->topics, 0);
    s->topics = 0U;
    if (s->authenticated) {
        (void)atomic_fetch_add(&s->srv->authed_count, -1);
    }
    s->authenticated = 0;
    s->closing = SESSION_CLOSE_NOW;
    dsd_mutex_unlock(&s->mu);
    (void)dsd_socket_shutdown(s->sock, SHUT_RDWR);
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    session_fn(void* arg) {
    api_session* s = (api_session*)arg;
    dsd_json_buf scratch;
    dsd_json_buf_init(&scratch);
    while (atomic_load(&s->srv->stop) == 0) {
        dsd_mutex_lock(&s->mu);
        const int closing = s->closing;
        const int authenticated = s->authenticated;
        dsd_mutex_unlock(&s->mu);
        if (closing == SESSION_CLOSE_NOW) {
            break;
        }
        const int have_output = session_has_output(s);
        if (closing == SESSION_CLOSE_AFTER_FLUSH && !have_output) {
            session_linger(s);
            break;
        }
        if (!authenticated && dsd_realtime_mono_ms() - s->opened_ms >= s->srv->auth_timeout_ms) {
            /* An idle unauthenticated connection would hold a client slot. */
            break;
        }
        const int events =
            ((closing == SESSION_OPEN) ? DSD_SOCKET_WAIT_READ : 0) | (have_output ? DSD_SOCKET_WAIT_WRITE : 0);
        const int ready = dsd_socket_wait(s->sock, events, (unsigned int)DSD_API_POLL_MS);
        if (ready < 0) {
            break;
        }
        if ((ready & DSD_SOCKET_WAIT_READ) != 0 && session_read(s, &scratch) != 0) {
            break;
        }
        if (session_flush(s) != 0) {
            break;
        }
    }
    session_retire(s);
    dsd_json_buf_free(&scratch);
    atomic_store(&s->finished, 1);
    DSD_THREAD_RETURN;
}

/*============================================================================
 * Accept / results threads
 *============================================================================*/

static void
session_free(api_session* s) {
    (void)dsd_socket_close(s->sock);
    dsd_mutex_destroy(&s->mu);
    dsd_json_buf_free(&s->outbox);
    dsd_json_buf_free(&s->pending);
    dsd_json_buf_free(&s->sending);
    free(s);
}

/* Unlink and free finished sessions, or every session when @p all (the threads must be stopping). */
static void
reap_sessions(api_server* srv, int all) {
    api_session* dead = NULL;
    dsd_mutex_lock(&srv->mu);
    api_session** pp = &srv->sessions;
    while (*pp != NULL) {
        api_session* s = *pp;
        if (all || atomic_load(&s->finished) != 0) {
            *pp = s->next;
            s->next = dead;
            dead = s;
            srv->session_count--;
        } else {
            pp = &s->next;
        }
    }
    dsd_mutex_unlock(&srv->mu);
    while (dead != NULL) {
        api_session* s = dead;
        dead = s->next;
        dsd_thread_join(s->thread);
        session_free(s);
    }
}

static void
queue_welcome(api_session* s, int auth_required) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, &b);
    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_kv_str(&w, "type", "welcome");
    (void)dsd_json_kv_i64(&w, "protocol", DSD_API_PROTOCOL_VERSION);
    (void)dsd_json_kv_bool(&w, "auth_required", auth_required);
    (void)dsd_json_key(&w, "topics");
    (void)dsd_json_arr_begin(&w);
    for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
        (void)dsd_json_value_str(&w, dsd_api_topic_name(1u << t));
    }
    (void)dsd_json_arr_end(&w);
    (void)dsd_json_kv_i64(&w, "commands", dsd_api_command_count_total());
    (void)dsd_json_kv_i64(&w, "max_line", DSD_API_MAX_LINE);
    (void)dsd_json_obj_end(&w);
    if (!dsd_json_writer_failed(&w) && dsd_json_buf_putc(&b, '\n') == 0) {
        session_queue(s, &b);
    }
    dsd_json_buf_free(&b);
}

static void
refuse_busy(dsd_socket_t c) {
    static const char k_busy[] =
        "{\"type\":\"error\",\"error\":{\"code\":\"busy\",\"message\":\"too many API clients\"}}\n";
    (void)dsd_socket_set_nonblocking(c, 1);
    (void)dsd_socket_send(c, k_busy, sizeof k_busy - 1U, DSD_API_SEND_FLAGS);
    (void)dsd_socket_shutdown(c, SHUT_RDWR);
    (void)dsd_socket_close(c);
}

static void
accept_one(api_server* srv) {
    struct sockaddr_in addr;
    int addrlen = (int)sizeof(addr);
    dsd_socket_t c = dsd_socket_accept(srv->listen_sock, (struct sockaddr*)&addr, &addrlen);
    if (c == DSD_INVALID_SOCKET) {
        return;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    (void)dsd_socket_setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, (int)sizeof(one));
#endif
    dsd_mutex_lock(&srv->mu);
    const int full = srv->session_count >= srv->max_clients;
    dsd_mutex_unlock(&srv->mu);
    /* Accepted sockets inherit the listener's non-blocking mode on Windows and the BSDs, but not on Linux. */
    api_session* s = full ? NULL : (api_session*)calloc(1U, sizeof(*s));
    if (s == NULL || dsd_socket_set_nonblocking(c, 1) != 0) {
        free(s);
        refuse_busy(c);
        return;
    }
    if (dsd_mutex_init(&s->mu) != 0) {
        free(s);
        refuse_busy(c);
        return;
    }
    s->sock = c;
    s->srv = srv;
    dsd_json_buf_init(&s->outbox);
    dsd_json_buf_init(&s->pending);
    dsd_json_buf_init(&s->sending);
    atomic_store(&s->finished, 0);
    s->opened_ms = dsd_realtime_mono_ms();
    if (srv->token_len == 0U) {
        s->authenticated = 1;
        (void)atomic_fetch_add(&srv->authed_count, 1);
    }
    queue_welcome(s, srv->token_len != 0U);

    dsd_mutex_lock(&srv->mu);
    s->next = srv->sessions;
    srv->sessions = s;
    srv->session_count++;
    dsd_mutex_unlock(&srv->mu);
    if (dsd_thread_create(&s->thread, session_fn, s) != 0) {
        /* No thread to join: retire it here and unlink it before anyone else can reach it. */
        session_retire(s);
        dsd_mutex_lock(&srv->mu);
        api_session** pp = &srv->sessions;
        while (*pp != NULL && *pp != s) {
            pp = &(*pp)->next;
        }
        if (*pp == s) {
            *pp = s->next;
            srv->session_count--;
        }
        dsd_mutex_unlock(&srv->mu);
        session_free(s);
    }
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    accept_fn(void* arg) {
    api_server* srv = (api_server*)arg;
    while (atomic_load(&srv->stop) == 0) {
        reap_sessions(srv, 0);
        const int ready = dsd_socket_wait(srv->listen_sock, DSD_SOCKET_WAIT_READ, (unsigned int)DSD_API_POLL_MS);
        if (ready < 0) {
            dsd_sleep_ms((unsigned int)DSD_API_POLL_MS);
            continue;
        }
        if (ready > 0) {
            accept_one(srv);
        }
    }
    DSD_THREAD_RETURN;
}

enum { DSD_API_RESULT_CHUNK = 8 };

/* Results the history no longer held when the pump looked: said, not silently skipped. */
static void
results_emit_gap(dsd_json_buf* b, const char* command, uint64_t missed) {
    dsd_json_buf_reset(b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, b);
    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_kv_str(&w, "type", "result_gap");
    (void)dsd_json_kv_str(&w, "command", command);
    (void)dsd_json_kv_u64(&w, "missed", missed);
    (void)dsd_json_obj_end(&w);
    if (!dsd_json_writer_failed(&w) && dsd_json_buf_putc(b, '\n') == 0) {
        dsd_api_broadcast(b->data, b->len, DSD_API_TOPIC_RESULT);
    }
}

static void
results_emit_tg_one(const dsd_app_tg_export_result* r, dsd_json_buf* b) {
    dsd_json_buf_reset(b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, b);
    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_kv_str(&w, "type", "result");
    (void)dsd_json_kv_str(&w, "command", "tg_list_export");
    (void)dsd_json_kv_u64(&w, "sequence", r->sequence);
    (void)dsd_json_kv_bool(&w, "ok", r->success ? 1 : 0);
    (void)dsd_json_kv_u64_str(&w, "policy_context", r->policy_context);
    (void)dsd_json_kv_u64(&w, "policy_generation", r->policy_generation);
    (void)dsd_json_kv_strn(&w, "path", r->path, sizeof r->path);
    (void)dsd_json_obj_end(&w);
    if (!dsd_json_writer_failed(&w) && dsd_json_buf_putc(b, '\n') == 0) {
        dsd_api_broadcast(b->data, b->len, DSD_API_TOPIC_RESULT);
    }
}

static void
results_emit_tg(api_server* srv, dsd_json_buf* b) {
    dsd_app_tg_export_result results[DSD_API_RESULT_CHUNK];
    int n = DSD_API_RESULT_CHUNK;
    while (n == DSD_API_RESULT_CHUNK) {
        n = dsd_app_tg_export_results_since(srv->last_tg_seq, results, DSD_API_RESULT_CHUNK);
        for (int i = 0; i < n; i++) {
            const int interested = dsd_api_topic_interest(DSD_API_TOPIC_RESULT);
            if (interested && results[i].sequence > srv->last_tg_seq + 1U) {
                results_emit_gap(b, "tg_list_export", results[i].sequence - srv->last_tg_seq - 1U);
            }
            srv->last_tg_seq = results[i].sequence;
            if (interested) {
                results_emit_tg_one(&results[i], b);
            }
        }
    }
}

static void
results_emit_dec_one(const dsd_app_decryption_result* r, dsd_json_buf* b) {
    dsd_json_buf_reset(b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, b);
    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_kv_str(&w, "type", "result");
    (void)dsd_json_kv_str(&w, "command", "decryption_apply");
    (void)dsd_json_kv_u64(&w, "sequence", r->sequence);
    (void)dsd_json_kv_u64_str(&w, "request_id", r->request_id);
    (void)dsd_json_kv_i64(&w, "status", r->status);
    (void)dsd_json_kv_bool(&w, "ok", r->status == DSD_APP_KEY_APPLIED);
    (void)dsd_json_kv_str(&w, "scope", r->scope == DSD_APP_KEY_SCOPE_TARGET ? "target" : "defaults");
    (void)dsd_json_obj_end(&w);
    if (!dsd_json_writer_failed(&w) && dsd_json_buf_putc(b, '\n') == 0) {
        dsd_api_broadcast(b->data, b->len, DSD_API_TOPIC_RESULT);
    }
}

static void
results_emit_dec(api_server* srv, dsd_json_buf* b) {
    dsd_app_decryption_result results[DSD_API_RESULT_CHUNK];
    int n = DSD_API_RESULT_CHUNK;
    while (n == DSD_API_RESULT_CHUNK) {
        n = dsd_app_decryption_results_since(srv->last_dec_seq, results, DSD_API_RESULT_CHUNK);
        for (int i = 0; i < n; i++) {
            const int interested = dsd_api_topic_interest(DSD_API_TOPIC_RESULT);
            if (interested && results[i].sequence > srv->last_dec_seq + 1U) {
                /* Some of these may be a frontend's; the client cannot tell, so it is told they all went. */
                results_emit_gap(b, "decryption_apply", results[i].sequence - srv->last_dec_seq - 1U);
            }
            srv->last_dec_seq = results[i].sequence;
            /* A frontend's own requests are its business; only the API's (tagged ids) are reported. */
            if (interested && (results[i].request_id & DSD_API_REQUEST_ID_TAG) != 0U) {
                results_emit_dec_one(&results[i], b);
            }
        }
    }
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    results_fn(void* arg) {
    api_server* srv = (api_server*)arg;
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    while (atomic_load(&srv->stop) == 0) {
        results_emit_tg(srv, &b);
        results_emit_dec(srv, &b);
        dsd_sleep_ms((unsigned int)DSD_API_RESULT_POLL_MS);
    }
    dsd_json_buf_free(&b);
    DSD_THREAD_RETURN;
}

/*============================================================================
 * Lifecycle
 *============================================================================*/

static int
config_same(const dsd_api_config* a, const dsd_api_config* b) {
    return a->port == b->port && a->max_clients == b->max_clients && a->auth_timeout_ms == b->auth_timeout_ms
           && strcmp(a->bind_addr, b->bind_addr) == 0 && strcmp(a->token, b->token) == 0;
}

/* Normalize @p in into @p out, or return -1 with the reason logged. */
static int
config_resolve(const dsd_api_config* in, dsd_api_config* out) {
    DSD_MEMSET(out, 0, sizeof(*out));
    if (in->port != DSD_API_PORT_EPHEMERAL && (in->port < 1 || in->port > 65535)) {
        LOG_ERROR("API: invalid port %d\n", in->port);
        return -1;
    }
    if (in->max_clients < 0 || in->max_clients > DSD_API_HARD_MAX_CLIENTS) {
        LOG_ERROR("API: max_clients must be 1..%d\n", DSD_API_HARD_MAX_CLIENTS);
        return -1;
    }
    if (in->auth_timeout_ms < 0) {
        LOG_ERROR("API: invalid auth timeout %d ms\n", in->auth_timeout_ms);
        return -1;
    }
    if (memchr(in->bind_addr, '\0', sizeof in->bind_addr) == NULL
        || memchr(in->token, '\0', sizeof in->token) == NULL) {
        LOG_ERROR("API: unterminated bind address or token\n");
        return -1;
    }
    out->port = in->port;
    out->max_clients = (in->max_clients > 0) ? in->max_clients : DSD_API_DEFAULT_MAX_CLIENTS;
    out->auth_timeout_ms = (in->auth_timeout_ms > 0) ? in->auth_timeout_ms : DSD_API_DEFAULT_AUTH_TIMEOUT_MS;
    DSD_SNPRINTF(out->bind_addr, sizeof out->bind_addr, "%s", in->bind_addr[0] != '\0' ? in->bind_addr : "127.0.0.1");
    /* Only the text: the token is compared over the whole zero-padded buffer, so stale bytes after a caller's
       terminator must not come along. */
    DSD_SNPRINTF(out->token, sizeof out->token, "%s", in->token);
    struct in_addr probe;
    if (inet_pton(AF_INET, out->bind_addr, &probe) != 1) {
        LOG_ERROR("API: invalid bind address %s (expected numeric IPv4)\n", out->bind_addr);
        return -1;
    }
    if (!is_loopback_addr(out->bind_addr) && out->token[0] == '\0') {
        LOG_ERROR("API: refusing non-loopback bind %s without a token\n", out->bind_addr);
        return -1;
    }
    return 0;
}

static dsd_socket_t
open_listener(const dsd_api_config* cfg, int* out_port) {
    struct sockaddr_in addr;
    DSD_MEMSET(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)(cfg->port == DSD_API_PORT_EPHEMERAL ? 0 : cfg->port));
    if (inet_pton(AF_INET, cfg->bind_addr, &addr.sin_addr) != 1) {
        return DSD_INVALID_SOCKET;
    }
    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == DSD_INVALID_SOCKET) {
        LOG_ERROR("API: failed to create listening socket\n");
        return DSD_INVALID_SOCKET;
    }
    int one = 1;
#if DSD_PLATFORM_WIN_NATIVE
    /* Windows' SO_REUSEADDR would let another process bind the same port and take connections; claim it instead. */
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, &one, (int)sizeof(one));
#else
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, (int)sizeof(one));
#endif
    if (dsd_socket_bind(sock, (struct sockaddr*)&addr, (int)sizeof(addr)) != 0) {
        LOG_ERROR("API: failed to bind %s:%d\n", cfg->bind_addr, cfg->port);
        (void)dsd_socket_close(sock);
        return DSD_INVALID_SOCKET;
    }
    if (dsd_socket_listen(sock, 16) != 0 || dsd_socket_set_nonblocking(sock, 1) != 0) {
        LOG_ERROR("API: failed to listen on %s:%d\n", cfg->bind_addr, cfg->port);
        (void)dsd_socket_close(sock);
        return DSD_INVALID_SOCKET;
    }
    struct sockaddr_in bound;
    DSD_MEMSET(&bound, 0, sizeof bound);
#if DSD_PLATFORM_WIN_NATIVE
    int bound_len = (int)sizeof(bound);
#else
    socklen_t bound_len = (socklen_t)sizeof(bound);
#endif
    if (getsockname(sock, (struct sockaddr*)&bound, &bound_len) != 0) {
        (void)dsd_socket_close(sock);
        return DSD_INVALID_SOCKET;
    }
    *out_port = (int)ntohs(bound.sin_port);
    return sock;
}

static void
server_free(api_server* srv) {
    dsd_mutex_destroy(&srv->mu);
    DSD_SECURE_ZERO(srv, sizeof(*srv));
    free(srv);
}

/* A server for @p cfg around @p sock, or NULL (with @p sock closed) when it cannot be allocated. */
static api_server*
server_create(const dsd_api_config* cfg, dsd_socket_t sock, int port) {
    api_server* srv = (api_server*)calloc(1U, sizeof(*srv));
    if (srv == NULL || dsd_mutex_init(&srv->mu) != 0) {
        free(srv);
        (void)dsd_socket_close(sock);
        return NULL;
    }
    srv->listen_sock = sock;
    srv->port = port;
    srv->config = *cfg;
    srv->max_clients = cfg->max_clients;
    srv->auth_timeout_ms = (uint64_t)cfg->auth_timeout_ms;
    DSD_MEMCPY(srv->token, cfg->token, sizeof srv->token);
    srv->token_len = strlen(srv->token);
    atomic_store(&srv->stop, 0);
    atomic_store(&srv->authed_count, 0);
    /* Results already retained when the server starts belong to someone else's earlier request. */
    dsd_app_tg_export_result tg_result;
    srv->last_tg_seq = dsd_app_tg_export_result_get(&tg_result) ? tg_result.sequence : 0U;
    dsd_app_decryption_result dec_result;
    srv->last_dec_seq = dsd_app_decryption_result_get(&dec_result) ? dec_result.sequence : 0U;
    return srv;
}

/* Publish @p srv and start its feed and threads; on failure everything is undone and -1 returned. */
static int
server_launch(api_server* srv) {
    for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
        atomic_store(&g_topic_interest[t], 0);
    }
    g_srv = srv;
    dsd_api_feed_start();
    if (dsd_thread_create(&srv->accept_thread, accept_fn, srv) != 0) {
        LOG_ERROR("API: failed to start accept thread\n");
        dsd_api_feed_stop();
        g_srv = NULL;
        (void)dsd_socket_close(srv->listen_sock);
        server_free(srv);
        return -1;
    }
    if (dsd_thread_create(&srv->results_thread, results_fn, srv) == 0) {
        srv->results_started = 1;
    } else {
        LOG_WARN("API: failed to start result pump thread; async results will not be reported\n");
    }
    LOG_INFO("API: listening on %s:%d%s\n", srv->config.bind_addr, srv->port,
             srv->token_len != 0U ? " (token required)" : "");
    return 0;
}

int
dsd_api_start(const dsd_api_config* config) {
    if (config == NULL || config->port == 0) {
        return 0; /* disabled */
    }
    dsd_api_config cfg;
    int rc = config_resolve(config, &cfg);
    ensure_lifecycle_mu();
    dsd_mutex_lock(&g_lifecycle_mu);
    if (rc == 0 && g_srv != NULL) {
        rc = config_same(&g_srv->config, &cfg) ? 0 : -1;
        if (rc != 0) {
            LOG_ERROR("API: already running with a different configuration\n");
        }
    } else if (rc == 0) {
        int port = 0;
        dsd_socket_t sock = DSD_INVALID_SOCKET;
        if (dsd_socket_init() != 0) {
            LOG_ERROR("API: socket subsystem unavailable\n");
        } else {
            sock = open_listener(&cfg, &port);
        }
        api_server* srv = (sock != DSD_INVALID_SOCKET) ? server_create(&cfg, sock, port) : NULL;
        rc = (srv != NULL) ? server_launch(srv) : -1;
    }
    dsd_mutex_unlock(&g_lifecycle_mu);
    DSD_SECURE_ZERO(&cfg, sizeof cfg);
    return rc;
}

void
dsd_api_stop(void) {
    ensure_lifecycle_mu();
    dsd_mutex_lock(&g_lifecycle_mu);
    api_server* srv = g_srv;
    if (srv == NULL) {
        dsd_mutex_unlock(&g_lifecycle_mu);
        return;
    }
    /* Synchronous: no feed callback runs past this, so nothing on the decode thread reaches srv again. */
    dsd_api_feed_stop();
    atomic_store(&srv->stop, 1);
    dsd_thread_join(srv->accept_thread);
    if (srv->results_started) {
        dsd_thread_join(srv->results_thread);
    }
    reap_sessions(srv, 1);
    (void)dsd_socket_close(srv->listen_sock);
    g_srv = NULL;
    for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
        atomic_store(&g_topic_interest[t], 0);
    }
    server_free(srv);
    dsd_mutex_unlock(&g_lifecycle_mu);
    LOG_INFO("API: stopped\n");
}

int
dsd_api_is_running(void) {
    ensure_lifecycle_mu();
    dsd_mutex_lock(&g_lifecycle_mu);
    const int running = g_srv != NULL;
    dsd_mutex_unlock(&g_lifecycle_mu);
    return running;
}

int
dsd_api_bound_port(void) {
    ensure_lifecycle_mu();
    dsd_mutex_lock(&g_lifecycle_mu);
    const int port = (g_srv != NULL) ? g_srv->port : 0;
    dsd_mutex_unlock(&g_lifecycle_mu);
    return port;
}
