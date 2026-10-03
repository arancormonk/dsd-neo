// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief TCP JSON Lines server for the third-party control/telemetry API.
 */

#include <dsd-neo/api/api.h>
#include <dsd-neo/api/json.h>
#include <dsd-neo/app_control/commands.h>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !DSD_PLATFORM_WIN_NATIVE
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "api_internal.h"

enum {
    DSD_API_MAX_LINE = 256 * 1024,
    DSD_API_OUTBOX_CAP = 1024 * 1024,
    DSD_API_IDLE_TIMEOUT_MS = 600000,
    DSD_API_READ_CHUNK = 4096,
    DSD_API_DEFAULT_MAX_CLIENTS = 8,
    DSD_API_HARD_MAX_CLIENTS = 32,
};

typedef struct api_session {
    dsd_socket_t sock;
    dsd_thread_t thread;
    struct api_server* srv;
    dsd_mutex_t mu;
    dsd_json_buf outbox;
    dsd_json_buf pending;
    uint32_t topics;
    unsigned char authenticated;
    unsigned char closing;
    unsigned long long last_activity_ms;
    struct api_session* next;
} api_session;

typedef struct api_server {
    dsd_socket_t listen_sock;
    dsd_thread_t accept_thread;
    dsd_thread_t results_thread;
    int results_started;
    atomic_int stop;
    int port;
    int max_clients;
    char bindaddr[DSD_API_BIND_ADDR_SIZE];
    char token[DSD_API_TOKEN_SIZE];
    int token_required;
    api_session* sessions;
    dsd_mutex_t mu;
    atomic_int client_count;
    unsigned long long last_tg_seq;
    unsigned long long last_dec_seq;
} api_server;

static api_server* g_srv = NULL;
static atomic_int g_running = 0;
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
token_matches(const char* a, const char* b) {
    const size_t la = strlen(a);
    const size_t lb = strlen(b);
    unsigned char diff = (unsigned char)((la ^ lb) & 0xFFU);
    const size_t n = (la < lb) ? la : lb;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
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
 * Topic registry
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

uint32_t
dsd_api_topics_parse(const dsd_json_node* topics) {
    if (topics == NULL || topics->type != DSD_JSON_ARRAY || topics->count == 0U) {
        return 0U;
    }
    uint32_t mask = 0U;
    for (size_t i = 0; i < topics->count; i++) {
        const char* name = dsd_json_as_str(topics->items[i]);
        if (name == NULL) {
            continue;
        }
        if (strcmp(name, "all") == 0) {
            mask |= ((1u << DSD_API_TOPIC_COUNT) - 1u);
            continue;
        }
        for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
            const char* tn = dsd_api_topic_name(1u << t);
            if (tn != NULL && strcmp(name, tn) == 0) {
                mask |= 1u << t;
            }
        }
    }
    return mask;
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
        uint32_t bit = 1u << t;
        if ((changed & bit) != 0U) {
            (void)atomic_fetch_add(&g_topic_interest[t], add ? 1 : -1);
        }
    }
}

/*============================================================================
 * Session outbox
 *============================================================================*/

static void
session_queue_locked(api_session* s, const char* line, size_t len) {
    if (s->closing) {
        return;
    }
    if (s->outbox.len + len > DSD_API_OUTBOX_CAP) {
        /* Best-effort telemetry: drop this line rather than stall the decoder
         * or grow without bound. Periodic topics self-heal on the next tick. */
        return;
    }
    (void)dsd_json_buf_append(&s->outbox, line, len);
}

static void
session_queue(api_session* s, const char* line, size_t len) {
    dsd_mutex_lock(&s->mu);
    session_queue_locked(s, line, len);
    dsd_mutex_unlock(&s->mu);
}

void
dsd_api_broadcast(const char* line, size_t len, uint32_t topic) {
    api_server* srv = g_srv;
    if (srv == NULL || line == NULL || len == 0U || atomic_load(&g_running) == 0) {
        return;
    }
    dsd_mutex_lock(&srv->mu);
    for (api_session* s = srv->sessions; s != NULL; s = s->next) {
        if (s->authenticated && (s->topics & topic) != 0U) {
            dsd_mutex_lock(&s->mu);
            session_queue_locked(s, line, len);
            dsd_mutex_unlock(&s->mu);
        }
    }
    dsd_mutex_unlock(&srv->mu);
}

/*============================================================================
 * Response helpers
 *============================================================================*/

static void
json_error(dsd_json_writer* w, const char* code, const char* message) {
    (void)dsd_json_key(w, "error");
    if (dsd_json_obj_begin(w) == 0) {
        (void)dsd_json_kv_str(w, "code", code);
        (void)dsd_json_kv_str(w, "message", message ? message : "");
        (void)dsd_json_obj_end(w);
    }
}

static void
response_begin(dsd_json_buf* out, dsd_json_writer* w, const dsd_json_node* id, int ok) {
    dsd_json_buf_reset(out);
    dsd_json_writer_init(w, out);
    (void)dsd_json_obj_begin(w);
    (void)dsd_json_key(w, "id");
    if (id == NULL || id->type == DSD_JSON_NULL) {
        (void)dsd_json_value_null(w);
    } else if (id->type == DSD_JSON_NUMBER) {
        (void)dsd_json_value_double(w, id->number);
    } else if (id->type == DSD_JSON_STRING) {
        (void)dsd_json_value_str(w, id->string);
    } else if (id->type == DSD_JSON_BOOL) {
        (void)dsd_json_value_bool(w, id->boolean);
    } else {
        (void)dsd_json_value_null(w);
    }
    (void)dsd_json_kv_bool(w, "ok", ok);
}

static void
response_end(dsd_json_buf* out, dsd_json_writer* w) {
    (void)dsd_json_obj_end(w);
    (void)dsd_json_buf_putc(out, '\n');
}

/*============================================================================
 * Request handlers
 *============================================================================*/

static void
handle_get(api_session* s, const dsd_json_node* id, const dsd_json_node* params, dsd_json_buf* scratch) {
    const char* what = dsd_json_as_str(dsd_json_obj_get(params, "what"));
    if (what == NULL || what[0] == '\0') {
        what = "status";
    }
    dsd_json_buf cached;
    dsd_json_buf_init(&cached);
    const int have = dsd_api_feed_latest(what, &cached) == 0 && cached.data != NULL;

    dsd_json_writer w;
    response_begin(scratch, &w, id, 1);
    (void)dsd_json_kv_str(&w, "what", what);
    if (have) {
        (void)dsd_json_key(&w, "data");
        (void)dsd_json_value_raw(&w, cached.data);
    } else {
        (void)dsd_json_kv_null(&w, "data");
    }
    response_end(scratch, &w);
    session_queue(s, scratch->data, scratch->len);
    dsd_json_buf_free(&cached);
}

static void
handle_subscribe(api_session* s, const dsd_json_node* id, const dsd_json_node* params, int add, dsd_json_buf* scratch) {
    const uint32_t mask = dsd_api_topics_parse(dsd_json_obj_get(params, "topics"));
    uint32_t changed;
    if (add) {
        changed = mask & ~s->topics;
        s->topics |= mask;
    } else {
        changed = mask & s->topics;
        s->topics &= ~mask;
    }
    topic_interest_apply(changed, add);

    dsd_json_writer w;
    response_begin(scratch, &w, id, 1);
    (void)dsd_json_key(&w, "topics");
    (void)dsd_json_arr_begin(&w);
    for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
        if ((s->topics & (1u << t)) != 0U) {
            const char* name = dsd_api_topic_name(1u << t);
            if (name != NULL) {
                (void)dsd_json_value_str(&w, name);
            }
        }
    }
    (void)dsd_json_arr_end(&w);
    response_end(scratch, &w);
    session_queue(s, scratch->data, scratch->len);
}

static void
handle_list_commands(api_session* s, const dsd_json_node* id, dsd_json_buf* scratch) {
    dsd_json_writer w;
    response_begin(scratch, &w, id, 1);
    dsd_json_buf catalog;
    dsd_json_buf_init(&catalog);
    if (dsd_api_command_catalog(&catalog) == 0 && catalog.data != NULL) {
        (void)dsd_json_key(&w, "commands");
        (void)dsd_json_value_raw(&w, catalog.data);
    } else {
        (void)dsd_json_key(&w, "commands");
        (void)dsd_json_arr_begin(&w);
        (void)dsd_json_arr_end(&w);
    }
    dsd_json_buf_free(&catalog);
    response_end(scratch, &w);
    session_queue(s, scratch->data, scratch->len);
}

static void
handle_request(api_session* s, const char* line, dsd_json_buf* scratch) {
    char err[96] = "";
    dsd_json_node* root = NULL;
    if (dsd_json_parse(line, &root, err, sizeof err) != 0) {
        dsd_json_writer w;
        response_begin(scratch, &w, NULL, 0);
        json_error(&w, "parse_error", err);
        response_end(scratch, &w);
        session_queue(s, scratch->data, scratch->len);
        return;
    }
    if (root->type != DSD_JSON_OBJECT) {
        dsd_json_writer w;
        response_begin(scratch, &w, NULL, 0);
        json_error(&w, "bad_request", "expected a JSON object");
        response_end(scratch, &w);
        session_queue(s, scratch->data, scratch->len);
        dsd_json_free(root);
        return;
    }

    const dsd_json_node* id = dsd_json_obj_get(root, "id");
    const char* cmd = dsd_json_as_str(dsd_json_obj_get(root, "cmd"));
    const dsd_json_node* params = dsd_json_obj_get(root, "params");
    if (cmd == NULL) {
        cmd = "get";
    }
    /* Session-control commands also accept their fields at the top level, so
     * {"cmd":"subscribe","topics":[...]} works without a params wrapper. */
    const dsd_json_node* effective = (params != NULL) ? params : root;

    if (strcmp(cmd, "hello") == 0 || strcmp(cmd, "ping") == 0) {
        dsd_json_writer w;
        response_begin(scratch, &w, id, 1);
        (void)dsd_json_kv_i64(&w, "protocol", DSD_API_PROTOCOL_VERSION);
        (void)dsd_json_kv_bool(&w, "authenticated", s->authenticated);
        response_end(scratch, &w);
        session_queue(s, scratch->data, scratch->len);
        dsd_json_free(root);
        return;
    }

    if (strcmp(cmd, "auth") == 0) {
        const char* tok = dsd_json_as_str(dsd_json_obj_get(params, "token"));
        const int ok = (!s->srv->token_required) || (tok != NULL && token_matches(tok, s->srv->token));
        dsd_json_writer w;
        if (ok) {
            s->authenticated = 1;
            response_begin(scratch, &w, id, 1);
            (void)dsd_json_kv_bool(&w, "authenticated", 1);
        } else {
            response_begin(scratch, &w, id, 0);
            json_error(&w, "unauthorized", "invalid token");
        }
        response_end(scratch, &w);
        session_queue(s, scratch->data, scratch->len);
        if (!ok) {
            s->closing = 1;
        }
        dsd_json_free(root);
        return;
    }

    if (!s->authenticated) {
        dsd_json_writer w;
        response_begin(scratch, &w, id, 0);
        json_error(&w, "unauthorized", "authenticate first");
        response_end(scratch, &w);
        session_queue(s, scratch->data, scratch->len);
        dsd_json_free(root);
        return;
    }

    if (strcmp(cmd, "subscribe") == 0) {
        handle_subscribe(s, id, effective, 1, scratch);
    } else if (strcmp(cmd, "unsubscribe") == 0) {
        handle_subscribe(s, id, effective, 0, scratch);
    } else if (strcmp(cmd, "get") == 0) {
        handle_get(s, id, effective, scratch);
    } else if (strcmp(cmd, "list_commands") == 0) {
        handle_list_commands(s, id, scratch);
    } else {
        dsd_json_buf_reset(scratch);
        if (dsd_api_command_execute(root, scratch) != 0) {
            dsd_json_writer w;
            response_begin(scratch, &w, id, 0);
            json_error(&w, "internal_error", "command dispatch failed");
            response_end(scratch, &w);
        }
        session_queue(s, scratch->data, scratch->len);
    }
    dsd_json_free(root);
}

/*============================================================================
 * Session thread
 *============================================================================*/

static void
session_flush(api_session* s) {
    dsd_json_buf take;
    dsd_json_buf_init(&take);
    dsd_mutex_lock(&s->mu);
    take = s->outbox;
    dsd_json_buf_init(&s->outbox);
    dsd_mutex_unlock(&s->mu);
    if (take.data != NULL && take.len > 0U) {
        size_t sent = 0U;
        while (sent < take.len) {
            const int n = dsd_socket_send(s->sock, take.data + sent, take.len - sent, 0);
            if (n <= 0) {
                break;
            }
            sent += (size_t)n;
        }
    }
    dsd_json_buf_free(&take);
}

static int
process_pending(api_session* s, dsd_json_buf* scratch) {
    for (;;) {
        if (s->pending.data == NULL) {
            return 0;
        }
        char* nl = (char*)memchr(s->pending.data, '\n', s->pending.len);
        if (nl == NULL) {
            return (s->pending.len > DSD_API_MAX_LINE) ? -1 : 0;
        }
        const size_t line_len = (size_t)(nl - s->pending.data);
        *nl = '\0';
        const size_t consumed = line_len + 1U;
        char* line = s->pending.data;
        if (line_len > 0U && line[line_len - 1U] == '\r') {
            line[line_len - 1U] = '\0';
        }
        if (line[0] != '\0') {
            handle_request(s, line, scratch);
        }
        const size_t remaining = s->pending.len - consumed;
        memmove(s->pending.data, nl + 1, remaining);
        s->pending.len = remaining;
        s->pending.data[remaining] = '\0';
    }
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    session_fn(void* arg) {
    api_session* s = (api_session*)arg;
    dsd_json_buf scratch;
    dsd_json_buf_init(&scratch);
    char buf[DSD_API_READ_CHUNK];

    while (atomic_load(&s->srv->stop) == 0 && !s->closing) {
        session_flush(s);
        const int n = dsd_socket_recv(s->sock, buf, sizeof buf, 0);
        if (n > 0) {
            s->last_activity_ms = dsd_realtime_mono_ms();
            if (s->pending.len + (size_t)n > DSD_API_MAX_LINE) {
                break;
            }
            if (dsd_json_buf_append(&s->pending, buf, (size_t)n) != 0) {
                break;
            }
            if (process_pending(s, &scratch) != 0) {
                break;
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        const int err = dsd_socket_get_error();
#if DSD_PLATFORM_WIN_NATIVE
        const int would_block = WSAEWOULDBLOCK;
#else
        const int would_block = EWOULDBLOCK;
#endif
        if (err != would_block && err != EAGAIN && err != EINTR) {
            break;
        }
        if (dsd_realtime_mono_ms() - s->last_activity_ms > DSD_API_IDLE_TIMEOUT_MS) {
            break;
        }
        dsd_sleep_ms(5);
    }

    dsd_json_buf_free(&scratch);
    dsd_mutex_lock(&s->mu);
    s->closing = 1;
    dsd_mutex_unlock(&s->mu);
    DSD_THREAD_RETURN;
}

/*============================================================================
 * Accept / results threads
 *============================================================================*/

static void
send_welcome(api_session* s) {
    dsd_json_buf b;
    dsd_json_buf_init(&b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, &b);
    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_kv_str(&w, "type", "welcome");
    (void)dsd_json_kv_i64(&w, "protocol", DSD_API_PROTOCOL_VERSION);
    (void)dsd_json_kv_bool(&w, "auth_required", s->srv->token_required);
    (void)dsd_json_key(&w, "topics");
    (void)dsd_json_arr_begin(&w);
    for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
        const char* name = dsd_api_topic_name(1u << t);
        if (name != NULL) {
            (void)dsd_json_value_str(&w, name);
        }
    }
    (void)dsd_json_arr_end(&w);
    (void)dsd_json_kv_i64(&w, "commands", dsd_api_command_count_total());
    (void)dsd_json_obj_end(&w);
    (void)dsd_json_buf_putc(&b, '\n');
    if (!dsd_json_writer_failed(&w) && b.data != NULL) {
        session_queue(s, b.data, b.len);
    }
    dsd_json_buf_free(&b);
}

static void
unlink_and_free_session(api_server* srv, api_session* s) {
    dsd_mutex_lock(&srv->mu);
    api_session** pp = &srv->sessions;
    while (*pp != NULL && *pp != s) {
        pp = &(*pp)->next;
    }
    if (*pp == s) {
        *pp = s->next;
    }
    dsd_mutex_unlock(&srv->mu);
    (void)atomic_fetch_add(&srv->client_count, -1);
    dsd_socket_close(s->sock);
    dsd_mutex_destroy(&s->mu);
    dsd_json_buf_free(&s->outbox);
    dsd_json_buf_free(&s->pending);
    free(s);
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    accept_fn(void* arg) {
    api_server* srv = (api_server*)arg;
    while (atomic_load(&srv->stop) == 0) {
        struct sockaddr_in addr;
        int addrlen = (int)sizeof(addr);
        dsd_socket_t c = dsd_socket_accept(srv->listen_sock, (struct sockaddr*)&addr, &addrlen);
        if (c == DSD_INVALID_SOCKET) {
            dsd_sleep_ms(20);
            continue;
        }
        if (atomic_load(&srv->client_count) >= srv->max_clients) {
            dsd_socket_close(c);
            continue;
        }
        api_session* s = (api_session*)calloc(1U, sizeof(*s));
        if (s == NULL) {
            dsd_socket_close(c);
            continue;
        }
        s->sock = c;
        s->srv = srv;
        (void)dsd_mutex_init(&s->mu);
        dsd_json_buf_init(&s->outbox);
        dsd_json_buf_init(&s->pending);
        s->authenticated = srv->token_required ? 0U : 1U;
        s->last_activity_ms = dsd_realtime_mono_ms();
        (void)dsd_socket_set_recv_timeout(c, 100);
        (void)dsd_socket_set_send_timeout(c, 500);

        dsd_mutex_lock(&srv->mu);
        s->next = srv->sessions;
        srv->sessions = s;
        dsd_mutex_unlock(&srv->mu);
        (void)atomic_fetch_add(&srv->client_count, 1);

        send_welcome(s);
        if (dsd_thread_create(&s->thread, session_fn, s) != 0) {
            unlink_and_free_session(srv, s);
        }
    }
    DSD_THREAD_RETURN;
}

static void
results_emit_tg(api_server* srv, dsd_json_buf* b) {
    dsd_app_tg_export_result r;
    if (!dsd_app_tg_export_result_get(&r) || r.sequence == 0U || r.sequence == srv->last_tg_seq) {
        return;
    }
    srv->last_tg_seq = r.sequence;
    dsd_json_buf_reset(b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, b);
    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_kv_str(&w, "type", "result");
    (void)dsd_json_kv_str(&w, "command", "talkgroup_export");
    (void)dsd_json_kv_u64(&w, "sequence", r.sequence);
    (void)dsd_json_kv_bool(&w, "ok", r.success ? 1 : 0);
    (void)dsd_json_kv_str(&w, "path", r.path);
    (void)dsd_json_obj_end(&w);
    (void)dsd_json_buf_putc(b, '\n');
    if (!dsd_json_writer_failed(&w) && b->data != NULL) {
        dsd_api_broadcast(b->data, b->len, DSD_API_TOPIC_RESULT);
    }
}

static void
results_emit_dec(api_server* srv, dsd_json_buf* b) {
    dsd_app_decryption_result r;
    if (!dsd_app_decryption_result_get(&r) || r.sequence == 0U || r.sequence == srv->last_dec_seq) {
        return;
    }
    srv->last_dec_seq = r.sequence;
    dsd_json_buf_reset(b);
    dsd_json_writer w;
    dsd_json_writer_init(&w, b);
    (void)dsd_json_obj_begin(&w);
    (void)dsd_json_kv_str(&w, "type", "result");
    (void)dsd_json_kv_str(&w, "command", "decryption_apply");
    (void)dsd_json_kv_u64(&w, "sequence", r.sequence);
    (void)dsd_json_kv_u64(&w, "request_id", r.request_id);
    (void)dsd_json_kv_i64(&w, "status", r.status);
    (void)dsd_json_obj_end(&w);
    (void)dsd_json_buf_putc(b, '\n');
    if (!dsd_json_writer_failed(&w) && b->data != NULL) {
        dsd_api_broadcast(b->data, b->len, DSD_API_TOPIC_RESULT);
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
        dsd_sleep_ms(200);
    }
    dsd_json_buf_free(&b);
    DSD_THREAD_RETURN;
}

/*============================================================================
 * Lifecycle
 *============================================================================*/

static void
detach_session(api_server* srv, api_session* s) {
    dsd_mutex_lock(&srv->mu);
    api_session** pp = &srv->sessions;
    while (*pp != NULL && *pp != s) {
        pp = &(*pp)->next;
    }
    if (*pp == s) {
        *pp = s->next;
    }
    dsd_mutex_unlock(&srv->mu);
    /* Return the session's topic interest before tearing it down. */
    topic_interest_apply(s->topics, 0);
    (void)dsd_socket_shutdown(s->sock, SHUT_RDWR);
    (void)dsd_socket_close(s->sock);
    dsd_thread_join(s->thread);
    (void)atomic_fetch_add(&srv->client_count, -1);
    dsd_mutex_destroy(&s->mu);
    dsd_json_buf_free(&s->outbox);
    dsd_json_buf_free(&s->pending);
    free(s);
}

int
dsd_api_start(const dsd_api_config* config) {
    if (config == NULL || config->port <= 0) {
        return 0; /* disabled */
    }
    ensure_lifecycle_mu();
    dsd_mutex_lock(&g_lifecycle_mu);
    if (g_srv != NULL) {
        dsd_mutex_unlock(&g_lifecycle_mu);
        return 0; /* already running */
    }
    const char* bind = (config->bind_addr[0] != '\0') ? config->bind_addr : "127.0.0.1";
    if (!is_loopback_addr(bind) && config->token[0] == '\0') {
        LOG_ERROR("API: refusing non-loopback bind %s without a token\n", bind);
        dsd_mutex_unlock(&g_lifecycle_mu);
        return -1;
    }

    (void)dsd_socket_init();

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)config->port);
    if (inet_pton(AF_INET, bind, &addr.sin_addr) != 1) {
        LOG_ERROR("API: invalid bind address %s\n", bind);
        dsd_mutex_unlock(&g_lifecycle_mu);
        return -1;
    }

    dsd_socket_t sock = dsd_socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == DSD_INVALID_SOCKET) {
        LOG_ERROR("API: failed to create listening socket\n");
        dsd_mutex_unlock(&g_lifecycle_mu);
        return -1;
    }
    int one = 1;
    (void)dsd_socket_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, (int)sizeof(one));
    if (dsd_socket_bind(sock, (struct sockaddr*)&addr, (int)sizeof(addr)) != 0) {
        LOG_ERROR("API: failed to bind %s:%d\n", bind, config->port);
        dsd_socket_close(sock);
        dsd_mutex_unlock(&g_lifecycle_mu);
        return -1;
    }
    if (dsd_socket_listen(sock, 8) != 0) {
        LOG_ERROR("API: failed to listen on %s:%d\n", bind, config->port);
        dsd_socket_close(sock);
        dsd_mutex_unlock(&g_lifecycle_mu);
        return -1;
    }
    (void)dsd_socket_set_nonblocking(sock, 1);

    int actual_port = config->port;
    struct sockaddr_in bound;
#if DSD_PLATFORM_WIN_NATIVE
    int bound_len = (int)sizeof(bound);
#else
    socklen_t bound_len = (socklen_t)sizeof(bound);
#endif
    if (getsockname(sock, (struct sockaddr*)&bound, &bound_len) == 0) {
        actual_port = ntohs(bound.sin_port);
    }

    api_server* srv = (api_server*)calloc(1U, sizeof(*srv));
    if (srv == NULL) {
        dsd_socket_close(sock);
        dsd_mutex_unlock(&g_lifecycle_mu);
        return -1;
    }
    srv->listen_sock = sock;
    srv->port = actual_port;
    srv->max_clients = (config->max_clients > 0) ? config->max_clients : DSD_API_DEFAULT_MAX_CLIENTS;
    if (srv->max_clients > DSD_API_HARD_MAX_CLIENTS) {
        srv->max_clients = DSD_API_HARD_MAX_CLIENTS;
    }
    snprintf(srv->bindaddr, sizeof srv->bindaddr, "%s", bind);
    snprintf(srv->token, sizeof srv->token, "%s", config->token);
    srv->token_required = (config->token[0] != '\0');
    (void)dsd_mutex_init(&srv->mu);
    atomic_store(&srv->stop, 0);
    atomic_store(&srv->client_count, 0);
    for (int t = 0; t < DSD_API_TOPIC_COUNT; t++) {
        atomic_store(&g_topic_interest[t], 0);
    }

    g_srv = srv;
    atomic_store(&g_running, 1);
    dsd_api_feed_start();

    if (dsd_thread_create(&srv->accept_thread, accept_fn, srv) != 0) {
        LOG_ERROR("API: failed to start accept thread\n");
        atomic_store(&g_running, 0);
        g_srv = NULL;
        dsd_api_feed_stop();
        dsd_socket_close(sock);
        dsd_mutex_destroy(&srv->mu);
        free(srv);
        dsd_mutex_unlock(&g_lifecycle_mu);
        return -1;
    }
    if (dsd_thread_create(&srv->results_thread, results_fn, srv) == 0) {
        srv->results_started = 1;
    } else {
        LOG_WARN("API: failed to start result pump thread\n");
    }

    LOG_INFO("API: listening on %s:%d%s\n", bind, actual_port, srv->token_required ? " (token required)" : "");
    dsd_mutex_unlock(&g_lifecycle_mu);
    return 0;
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
    atomic_store(&g_running, 0);
    atomic_store(&srv->stop, 1);
    g_srv = NULL;
    dsd_api_feed_stop();

    dsd_socket_shutdown(srv->listen_sock, SHUT_RDWR);
    dsd_socket_close(srv->listen_sock);
    dsd_thread_join(srv->accept_thread);
    if (srv->results_started) {
        dsd_thread_join(srv->results_thread);
    }

    for (;;) {
        dsd_mutex_lock(&srv->mu);
        api_session* s = srv->sessions;
        dsd_mutex_unlock(&srv->mu);
        if (s == NULL) {
            break;
        }
        detach_session(srv, s);
    }

    dsd_mutex_destroy(&srv->mu);
    free(srv);
    dsd_mutex_unlock(&g_lifecycle_mu);
    LOG_INFO("API: stopped\n");
}

int
dsd_api_is_running(void) {
    return atomic_load(&g_running) != 0;
}

int
dsd_api_client_count(void) {
    api_server* srv = g_srv;
    return (srv != NULL) ? atomic_load(&srv->client_count) : 0;
}

int
dsd_api_bound_port(void) {
    api_server* srv = g_srv;
    return (srv != NULL) ? srv->port : 0;
}
