// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/stream_notice_hooks.h>
#include <stddef.h>

/* The sink, under a lock: the stream's controller posts while app-control installs or clears it. It is called under the
   lock too, so once dsd_runtime_stream_notice_set() returned no post can still deliver to the sink it replaced: a post
   that took the old sink before a session's end has delivered before the end clears what the sink holds, and cannot
   carry the notice into the next session. A sink is short and never calls back into this module. */
static dsd_stream_notice_fn g_stream_notice_fn = NULL;
static dsd_mutex_t g_stream_notice_mu;
static atomic_int g_stream_notice_mu_state = 0; // 0=uninit, 1=initing, 2=init, -1=failed

static int
ensure_stream_notice_mu_init(void) {
    int state = atomic_load(&g_stream_notice_mu_state);
    if (state == 2 || state == -1) {
        return state == 2;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_stream_notice_mu_state, &expected, 1)) {
        const int ready = dsd_mutex_init(&g_stream_notice_mu) == 0;
        atomic_store(&g_stream_notice_mu_state, ready ? 2 : -1);
        return ready;
    }
    while ((state = atomic_load(&g_stream_notice_mu_state)) == 1) {
        dsd_thread_yield();
    }
    return state == 2;
}

void
dsd_runtime_stream_notice_set(dsd_stream_notice_fn fn) {
    if (!ensure_stream_notice_mu_init() || dsd_mutex_lock(&g_stream_notice_mu) != 0) {
        return;
    }
    g_stream_notice_fn = fn;
    dsd_mutex_unlock(&g_stream_notice_mu);
}

void
dsd_runtime_stream_notice_post(int kind) {
    if (kind == DSD_STREAM_NOTICE_NONE) {
        return;
    }
    if (!ensure_stream_notice_mu_init() || dsd_mutex_lock(&g_stream_notice_mu) != 0) {
        return;
    }
    const dsd_stream_notice_fn fn = g_stream_notice_fn;
    if (fn) {
        fn(kind);
    }
    dsd_mutex_unlock(&g_stream_notice_mu);
}
