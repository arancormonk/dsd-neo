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
#include <dsd-neo/platform/sockets.h>
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
/* Every attachment number given out so far (backlog_new_attachment_locked()): s_seq never takes one of them again, so a
   cut measured on an attachment that ended never applies to a later one, at the same address or not. */
static uint64_t s_seq_issued;
/* The marks taken so far: the order of the last one (dsd_input_cut::order). */
static uint64_t s_marks;
static dsd_input_cut s_post;
static int s_post_held;
/* An input is being opened (dsd_input_backlog_opening()): its connection is the measured input, counted by
   s_opening_arrived until its queue attaches. The backlog keeps the connection itself (s_opening_sock), never the
   address the caller holds it at, so it counts the same connection whatever becomes of the caller's copy; no queue is
   attached meanwhile (s_queue NULL). */
static int s_opening;
static dsd_socket_t s_opening_sock = DSD_INVALID_SOCKET;
static dsd_input_socket_arrived_fn s_opening_arrived;
/* The last attach went on from an opening's connection (s_continued_to != 0): a mark measured on that connection
   (attachment s_continued_from) stands on the queue attached then (attachment s_continued_to) while it stays
   attached, since the queue's count goes on from the connection's. */
static uint64_t s_continued_from;
static uint64_t s_continued_to;
/* What the last attach replaced, kept while its queue is the one attached on that attachment (s_seq == s_undo_seq): an
   input whose start fails after its queue attached (dsd_input_backlog_attach_failed()) puts it back. */
static int s_undo_held;
static uint64_t s_undo_seq;
static const void* s_undo_queue;
static dsd_input_arrived_fn s_undo_arrived;
static uint64_t s_undo_prior_seq;
static uint64_t s_undo_continued_from;
static uint64_t s_undo_continued_to;

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

/* A new attachment: a number no attachment had before. */
static void
backlog_new_attachment_locked(void) {
    s_seq = ++s_seq_issued;
}

/* No input is opening any more: the backlog's copy of its connection is cleared. */
static void
backlog_clear_opening_locked(void) {
    s_opening = 0;
    s_opening_sock = DSD_INVALID_SOCKET;
    s_opening_arrived = NULL;
}

static void
backlog_mark_locked(dsd_input_cut* out) {
    out->seq = s_seq;
    if (s_opening) {
        out->measured = 1U;
        out->arrived = s_opening_arrived(s_opening_sock);
    } else {
        out->measured = (uint8_t)(s_queue != NULL && s_arrived != NULL);
        out->arrived = out->measured ? s_arrived(s_queue) : 0U;
    }
    out->marked_ns = dsd_time_monotonic_ns();
    out->order = ++s_marks;
}

void
dsd_input_backlog_attach(const void* queue, dsd_input_arrived_fn arrived) {
    if (queue == NULL || arrived == NULL) {
        return;
    }
    backlog_lock();
    const uint64_t opened = s_seq;
    const int continues = s_opening;
    /* The registration this attach replaces, for an input that fails to start after it (not one that was opening, whose
       connection this queue goes on from). */
    s_undo_held = !continues;
    s_undo_queue = s_queue;
    s_undo_arrived = s_arrived;
    s_undo_prior_seq = s_seq;
    s_undo_continued_from = s_continued_from;
    s_undo_continued_to = s_continued_to;
    backlog_new_attachment_locked();
    s_undo_seq = s_seq;
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
    s_continued_from = opened;
    s_continued_to = continues ? s_seq : 0U;
    backlog_clear_opening_locked();
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
        backlog_new_attachment_locked();
    }
    backlog_unlock();
}

void
dsd_input_backlog_attach_failed(const void* queue) {
    if (queue == NULL) {
        return;
    }
    backlog_lock();
    if (s_queue == queue) {
        if (s_undo_held && s_seq == s_undo_seq) {
            /* Nothing attached, detached or opened since this queue attached, and nothing read the queue it replaced
               meanwhile: that queue is the input again, on its own attachment, so a mark measured on it before still
               counts it. A post measured on the queue that failed is no input's drop. */
            if (s_post_held && s_post.measured && s_post.seq == s_undo_seq) {
                s_post_held = 0;
            }
            s_queue = s_undo_queue;
            s_arrived = s_undo_arrived;
            s_seq = s_undo_prior_seq;
            s_continued_from = s_undo_continued_from;
            s_continued_to = s_undo_continued_to;
        } else {
            s_queue = NULL;
            s_arrived = NULL;
            backlog_new_attachment_locked();
        }
        s_undo_held = 0;
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

/* The connection an opening measured is no input any more (its open failed, or the run ended): detached. */
static void
backlog_end_opening_locked(void) {
    if (s_opening) {
        backlog_new_attachment_locked();
        backlog_clear_opening_locked();
    }
}

void
dsd_input_backlog_opening(dsd_socket_t sock, dsd_input_socket_arrived_fn arrived) {
    backlog_lock();
    /* Posted while no input was attached, before this connection existed: nothing it holds was queued before the post,
       as at an attach. */
    if (s_post_held && !s_post.measured) {
        s_post_held = 0;
    }
    if (sock != DSD_INVALID_SOCKET && arrived != NULL) {
        /* The queue attached before, if any, is no longer the input: the connection is, until its queue attaches. */
        backlog_new_attachment_locked();
        s_queue = NULL;
        s_arrived = NULL;
        s_opening = 1;
        s_opening_sock = sock;
        s_opening_arrived = arrived;
    }
    backlog_unlock();
}

void
dsd_input_backlog_post(const dsd_input_cut* cut) {
    if (cut == NULL) {
        return;
    }
    backlog_lock();
    dsd_input_cut held = *cut;
    /* Marked on a connection that was opening, whose queue has attached since and goes on from its count. */
    if (held.measured && s_continued_to != 0U && held.seq == s_continued_from && s_seq == s_continued_to) {
        held.seq = s_seq;
    }
    /* Of two posts the later mark is held, whatever order they come in. */
    if (!s_post_held || held.order > s_post.order) {
        s_post = held;
        s_post_held = 1;
    }
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
