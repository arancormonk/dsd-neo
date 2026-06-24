// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Minimal 2-thread worker pool for intra-block demodulation tasks.
 *
 * Provides a tiny env-gated pool (set `DSD_NEO_MT=1`) addressed by `demod_state*`.
 * Used to run up to two inner-loop tasks in parallel per processing block.
 */

#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/worker_pool.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <utility>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/platform.h"

namespace {

/* Opaque handle keyed off demod_state* to avoid depending on its layout here */
struct WorkerCtx;

struct WorkerArg {
    WorkerCtx* ctx = nullptr;
    int id = 0;
};

struct WorkerCtx {
    bool enabled = false;
    dsd_thread_t threads[2] = {};
    WorkerArg* args = nullptr;
    dsd_mutex_t lock = {};
    dsd_cond_t cv = {};
    dsd_cond_t done_cv = {};
    bool should_exit = false;
    int epoch = 0;
    int completed_in_epoch = 0;
    int posted_count = 0;

    demod_mt_task tasks[2] = {};
};

std::unordered_map<const void*, WorkerCtx*> g_ctx_map;
std::mutex g_ctx_mu;

} // namespace

static WorkerCtx*
get_ctx(const void* key) {
    std::lock_guard<std::mutex> lg(g_ctx_mu);
    auto it = g_ctx_map.find(key);
    return (it == g_ctx_map.end()) ? nullptr : it->second;
}

static void
set_ctx(const void* key, WorkerCtx* ctx) {
    std::lock_guard<std::mutex> lg(g_ctx_mu);
    if (ctx) {
        g_ctx_map[key] = ctx;
    } else {
        g_ctx_map.erase(key);
    }
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    demod_mt_worker(void* arg) {
    WorkerArg* wa = static_cast<WorkerArg*>(arg);
    WorkerCtx* ctx = wa->ctx;
    const int id = wa->id;
    int local_epoch = 0;
    for (;;) {
        dsd_mutex_lock(&ctx->lock);
        while (!ctx->should_exit && ctx->epoch == local_epoch) {
            dsd_cond_wait(&ctx->cv, &ctx->lock);
        }
        if (ctx->should_exit) {
            dsd_mutex_unlock(&ctx->lock);
            break;
        }
        local_epoch = ctx->epoch;
        void (*fn)(void*) = nullptr;
        void* fn_arg = nullptr;
        bool assigned = id < ctx->posted_count;
        if (assigned) {
            fn = ctx->tasks[id].run;
            fn_arg = ctx->tasks[id].arg;
        }
        dsd_mutex_unlock(&ctx->lock);
        if (fn) {
            fn(fn_arg);
        }
        dsd_mutex_lock(&ctx->lock);
        if (assigned) {
            ctx->completed_in_epoch++;
            if (ctx->completed_in_epoch >= ctx->posted_count) {
                dsd_cond_signal(&ctx->done_cv);
            }
        }
        dsd_mutex_unlock(&ctx->lock);
    }
    DSD_THREAD_RETURN;
}

/**
 * @brief Initialize the minimal worker pool when `DSD_NEO_MT=1`.
 *
 * Safe to call multiple times per demodulator instance.
 * @param s Demodulator state used as a key for the worker context.
 * @note No-op when multithreading is disabled via environment.
 */
void
demod_mt_init(struct demod_state* s) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(NULL);
        cfg = dsd_neo_get_config();
    }
    bool enable = (cfg && cfg->mt_is_set && cfg->mt_enable) ? true : false;
    if (!enable) {
        // No context needed if disabled
        set_ctx(static_cast<const void*>(s), nullptr);
        return;
    }
    WorkerCtx* ctx = get_ctx(static_cast<const void*>(s));
    if (ctx) {
        return; // already initialized
    }
    ctx = new (std::nothrow) WorkerCtx;
    if (!ctx) {
        return;
    }
    ctx->enabled = true;
    ctx->should_exit = false;
    ctx->epoch = 0;
    ctx->completed_in_epoch = 0;
    ctx->posted_count = 0;
    dsd_mutex_init(&ctx->lock);
    dsd_cond_init(&ctx->cv);
    dsd_cond_init(&ctx->done_cv);
    ctx->args = static_cast<WorkerArg*>(calloc(2, sizeof(WorkerArg)));
    if (ctx->args == NULL) {
        DSD_FPRINTF(stderr, "Failed to allocate worker thread arguments\n");
        dsd_mutex_destroy(&ctx->lock);
        dsd_cond_destroy(&ctx->cv);
        dsd_cond_destroy(&ctx->done_cv);
        delete ctx;
        return;
    }
    for (int i = 0; i < 2; i++) {
        ctx->args[i].ctx = ctx;
        ctx->args[i].id = i;
        dsd_thread_create(&ctx->threads[i], demod_mt_worker, static_cast<void*>(&ctx->args[i]));
    }
    set_ctx(static_cast<const void*>(s), ctx);
    DSD_FPRINTF(stderr, "Intra-block multithreading enabled (DSD_NEO_MT=1), workers: 2.\n");
}

/**
 * @brief Tear down worker threads created by `demod_mt_init`.
 *
 * @param s Demodulator state used as a key for the worker context.
 * @note Safe no-op if the pool was never enabled/initialized.
 */
void
demod_mt_destroy(struct demod_state* s) {
    WorkerCtx* ctx = get_ctx(static_cast<const void*>(s));
    if (!ctx || !ctx->enabled) {
        set_ctx(static_cast<const void*>(s), nullptr);
        return;
    }
    dsd_mutex_lock(&ctx->lock);
    ctx->should_exit = true;
    dsd_cond_broadcast(&ctx->cv);
    dsd_mutex_unlock(&ctx->lock);
    for (int i = 0; i < 2; i++) {
        dsd_thread_join(ctx->threads[i]);
    }
    dsd_cond_destroy(&ctx->done_cv);
    dsd_cond_destroy(&ctx->cv);
    dsd_mutex_destroy(&ctx->lock);
    free(ctx->args);
    ctx->args = nullptr;
    delete ctx;
    set_ctx(static_cast<const void*>(s), nullptr);
}

/**
 * @brief Post up to two tasks and wait for completion.
 *
 * Runs synchronously in the caller thread when the pool is disabled.
 * @param s Demodulator state key for the worker context.
 * @param f0 Function pointer for the first task (may be NULL).
 * @param a0 Argument for the first task.
 * @param f1 Function pointer for the second task (may be NULL).
 * @param a1 Argument for the second task.
 */
void
demod_mt_run_two_impl(struct demod_state* s, demod_mt_task task0, demod_mt_task task1) {
    WorkerCtx* ctx = get_ctx(static_cast<const void*>(s));
    if (!ctx || !ctx->enabled) {
        if (task0.run) {
            task0.run(task0.arg);
        }
        if (task1.run) {
            task1.run(task1.arg);
        }
        return;
    }
    dsd_mutex_lock(&ctx->lock);
    ctx->tasks[0] = task0;
    ctx->tasks[1] = task1;
    ctx->posted_count = (task1.run != NULL) ? 2 : 1;
    ctx->completed_in_epoch = 0;
    ctx->epoch++;
    dsd_cond_broadcast(&ctx->cv);
    while (ctx->completed_in_epoch < ctx->posted_count) {
        dsd_cond_wait(&ctx->done_cv, &ctx->lock);
    }
    dsd_mutex_unlock(&ctx->lock);
}
