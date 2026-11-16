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
/* An input is being opened (dsd_input_backlog_opening()) and no queue has attached since. */
static int s_opening;
/* The held post was made while an input was being opened and no queue was attached: the queue that attaches next is
   the one it was posted for. */
static int s_post_opening;

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
    s_seq++;
    s_queue = queue;
    s_arrived = arrived;
    /* Posted while this input was being opened, with no queue attached: it is measured on this queue, the input it was
       posted for. What the queue holds now reached it before the post, or in the moment between the post and this
       call. One posted before the opening began (or with no opening) moved the peer before this input existed, so
       what it holds arrived after the post: it is retired. */
    if (s_post_held && !s_post.measured) {
        if (s_post_opening) {
            s_post.seq = s_seq;
            s_post.arrived = arrived(queue);
            s_post.measured = 1U;
        } else {
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

uint64_t
dsd_input_cut_age_ns(const dsd_input_cut* cut) {
    if (cut == NULL) {
        return 0U;
    }
    const uint64_t now = dsd_time_monotonic_ns();
    return now > cut->marked_ns ? now - cut->marked_ns : 0U;
}

void
dsd_input_backlog_opening(void) {
    backlog_lock();
    /* Posted while no queue was attached, before this input began: retired, as at the attach. */
    if (s_post_held && !s_post.measured) {
        s_post_held = 0;
    }
    s_opening = 1;
    backlog_unlock();
}

void
dsd_input_backlog_post(void) {
    backlog_lock();
    backlog_mark_locked(&s_post);
    s_post_held = 1;
    s_post_opening = s_opening && !s_post.measured;
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
    if (s_post_held && !(s_post.measured && s_queue != NULL && s_post.seq == s_seq)) {
        s_post_held = 0;
    }
    s_opening = 0;
    backlog_unlock();
}

void
dsd_input_backlog_retire(void) {
    backlog_lock();
    s_post_held = 0;
    s_opening = 0;
    backlog_unlock();
}
