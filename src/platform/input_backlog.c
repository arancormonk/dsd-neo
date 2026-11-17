// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The attached input queue and the held off-thread retune (issue #834; <dsd-neo/platform/input_backlog.h>).
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/input_backlog.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <stddef.h>
#include <stdint.h>

static dsd_mutex_t s_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. */
static atomic_int s_mu_state;

/* Read and written with the lock held. */
static const void* s_queue;
static dsd_input_arrived_fn s_arrived;
static uint64_t s_seq; /* moves at every attach and every detach of the attached queue */
static dsd_input_cut s_post;
static int s_post_held;
/* An input is being opened (dsd_input_backlog_opening()): its connection is the attached input, counted until its
   queue attaches. */
static int s_opening;

static void
backlog_lock(void) {
    if (atomic_load(&s_mu_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&s_mu_state, &expected, 1)) {
            (void)dsd_mutex_init(&s_mu);
            atomic_store(&s_mu_state, 2);
        } else {
            while (atomic_load(&s_mu_state) != 2) {
                dsd_thread_yield();
            }
        }
    }
    dsd_mutex_lock(&s_mu);
}

static void
backlog_unlock(void) {
    dsd_mutex_unlock(&s_mu);
}

static void
backlog_mark_locked(dsd_input_cut* out) {
    out->seq = s_seq;
    out->measured = (uint8_t)(s_queue != NULL && s_arrived != NULL);
    out->arrived = out->measured ? s_arrived(s_queue) : 0U;
    out->marked_ns = dsd_time_monotonic_ns();
}

void
dsd_input_backlog_attach(const void* queue, dsd_input_arrived_fn arrived) {
    if (queue == NULL || arrived == NULL) {
        return;
    }
    backlog_lock();
    const uint64_t opened = s_seq;
    const int continues = s_opening;
    s_seq++;
    s_queue = queue;
    s_arrived = arrived;
    if (s_post_held) {
        if (continues && s_post.measured && s_post.seq == opened) {
            /* Measured when it was posted, on the connection this queue reads: nothing read the connection before its
               queue attached, so the queue's count goes on from the connection's and the post stands on it. */
            s_post.seq = s_seq;
        } else if (!s_post.measured) {
            /* Posted while no input was attached: the peer moved before this input existed, so nothing it holds was
               queued before the post. */
            s_post_held = 0;
        }
    }
    s_opening = 0;
    backlog_unlock();
}

void
dsd_input_backlog_detach(const void* queue) {
    if (queue == NULL) {
        return;
    }
    backlog_lock();
    if (s_queue == queue) {
        s_queue = NULL;
        s_arrived = NULL;
        s_seq++;
    }
    backlog_unlock();
}

void
dsd_input_backlog_mark(dsd_input_cut* out) {
    if (out == NULL) {
        return;
    }
    backlog_lock();
    backlog_mark_locked(out);
    backlog_unlock();
}

int
dsd_input_backlog_cut_applies(const dsd_input_cut* cut, const void* queue) {
    if (cut == NULL || queue == NULL || !cut->measured) {
        return 0;
    }
    backlog_lock();
    const int applies = s_queue == queue && cut->seq == s_seq;
    backlog_unlock();
    return applies;
}

/* The connection an opening attached is no input any more (its open failed, or the run ended): detached. */
static void
backlog_end_opening_locked(void) {
    if (s_opening) {
        s_queue = NULL;
        s_arrived = NULL;
        s_seq++;
        s_opening = 0;
    }
}

void
dsd_input_backlog_opening(const void* conn, dsd_input_arrived_fn arrived) {
    backlog_lock();
    /* Posted while no input was attached, before this connection existed: nothing it holds was queued before the post,
       as at an attach. */
    if (s_post_held && !s_post.measured) {
        s_post_held = 0;
    }
    if (conn != NULL && arrived != NULL) {
        s_seq++;
        s_queue = conn;
        s_arrived = arrived;
        s_opening = 1;
    }
    backlog_unlock();
}

void
dsd_input_backlog_post(void) {
    backlog_lock();
    backlog_mark_locked(&s_post);
    s_post_held = 1;
    backlog_unlock();
}

int
dsd_input_backlog_take(dsd_input_cut* out) {
    backlog_lock();
    const int held = s_post_held;
    if (held && out != NULL) {
        *out = s_post;
    }
    s_post_held = 0;
    backlog_unlock();
    return held;
}

void
dsd_input_backlog_forget(void) {
    backlog_lock();
    /* A connection still opening has no queue: a post measured on it is no input's drop. */
    if (s_post_held && !(s_post.measured && s_queue != NULL && s_post.seq == s_seq && !s_opening)) {
        s_post_held = 0;
    }
    backlog_end_opening_locked();
    backlog_unlock();
}

void
dsd_input_backlog_retire(void) {
    backlog_lock();
    s_post_held = 0;
    backlog_end_opening_locked();
    backlog_unlock();
}
