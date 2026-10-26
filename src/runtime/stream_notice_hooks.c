// SPDX-License-Identifier: GPL-3.0-or-later

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/stream_notice_hooks.h>
#include <stddef.h>

/* The sink, under a lock: the stream's controller posts while app-control installs or clears it. It is called outside
   the lock; a sink is a function of the process, so a post that took it just before a clear still calls valid code. */
static dsd_stream_notice_fn g_stream_notice_fn = NULL;
static dsd_mutex_t g_stream_notice_mu;
static atomic_int g_stream_notice_mu_state = 0; // 0=uninit, 1=initing, 2=init

static void
ensure_stream_notice_mu_init(void) {
    if (atomic_load(&g_stream_notice_mu_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_stream_notice_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_stream_notice_mu);
        atomic_store(&g_stream_notice_mu_state, 2);
        return;
    }
    while (atomic_load(&g_stream_notice_mu_state) != 2) {
        dsd_thread_yield();
    }
}

void
dsd_runtime_stream_notice_set(dsd_stream_notice_fn fn) {
    ensure_stream_notice_mu_init();
    dsd_mutex_lock(&g_stream_notice_mu);
    g_stream_notice_fn = fn;
    dsd_mutex_unlock(&g_stream_notice_mu);
}

void
dsd_runtime_stream_notice_post(int kind) {
    if (kind == DSD_STREAM_NOTICE_NONE) {
        return;
    }
    ensure_stream_notice_mu_init();
    dsd_mutex_lock(&g_stream_notice_mu);
    const dsd_stream_notice_fn fn = g_stream_notice_fn;
    dsd_mutex_unlock(&g_stream_notice_mu);
    if (fn) {
        fn(kind);
    }
}
