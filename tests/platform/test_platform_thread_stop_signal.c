// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Ctrl+Z belongs to one thread. The kernel hands SIGTSTP to any thread that does not block it, and the terminal UI's
 * curses handler, run on a decoder or I/O thread while the UI thread drew, could leave the signal ignored for the rest
 * of the session. So every thread dsd_thread_create() starts begins with SIGTSTP blocked, whatever its creator's mask,
 * the creator keeps its own mask, and a thread a library starts from such a thread inherits the block.
 */

#include <assert.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>

int dsd_thread_create_impl(dsd_thread_t* thread, void* arg, dsd_thread_fn func);
/* The C++ form of dsd_thread_create() (test_platform_thread_stop_signal_cpp.cpp): 1 when its thread started with
   SIGTSTP blocked, 0 when not, -1 when it did not start. */
int thread_stop_signal_cpp_create_reports_blocked(void);

static int
stop_blocked_here(void) {
    sigset_t mask;
    assert(pthread_sigmask(SIG_BLOCK, NULL, &mask) == 0);
    return sigismember(&mask, SIGTSTP) == 1;
}

static DSD_THREAD_RETURN_TYPE
report_thread(void* arg) {
    atomic_store((atomic_int*)arg, stop_blocked_here());
    DSD_THREAD_RETURN;
}

static void*
library_thread(void* arg) {
    atomic_store((atomic_int*)arg, stop_blocked_here());
    return NULL;
}

/* A worker that starts a thread the way a library does, with pthread_create() itself. */
static DSD_THREAD_RETURN_TYPE
worker_starting_library_thread(void* arg) {
    pthread_t inner;
    if (pthread_create(&inner, NULL, library_thread, arg) == 0) {
        (void)pthread_join(inner, NULL);
    }
    DSD_THREAD_RETURN;
}

static int
started_blocked(int use_impl, dsd_thread_fn entry) {
    atomic_int seen;
    atomic_init(&seen, -1);
    dsd_thread_t thread;
    const int rc = use_impl ? dsd_thread_create_impl(&thread, &seen, entry) : dsd_thread_create(&thread, entry, &seen);
    assert(rc == 0);
    assert(dsd_thread_join(thread) == 0);
    return atomic_load(&seen);
}

static void
test_block_stop_signal_sets_the_calling_thread(void) {
    assert(dsd_thread_block_stop_signal(1) == 0);
    assert(stop_blocked_here() == 1);
    assert(dsd_thread_block_stop_signal(0) == 0);
    assert(stop_blocked_here() == 0);
}

/* The UI thread, and the main thread of a run without the terminal UI, leave SIGTSTP unblocked: what they start does
   not take a stop, and they keep taking it. */
static void
test_unblocked_creator_starts_blocked_threads(void) {
    assert(dsd_thread_block_stop_signal(0) == 0);
    assert(started_blocked(0, report_thread) == 1);
    assert(stop_blocked_here() == 0);
    assert(started_blocked(1, report_thread) == 1);
    assert(stop_blocked_here() == 0);
    assert(thread_stop_signal_cpp_create_reports_blocked() == 1);
    assert(stop_blocked_here() == 0);
    assert(started_blocked(0, worker_starting_library_thread) == 1);
    assert(stop_blocked_here() == 0);
}

/* The session thread under the terminal UI blocks SIGTSTP before the run; its creates leave it blocked. */
static void
test_blocked_creator_stays_blocked(void) {
    assert(dsd_thread_block_stop_signal(1) == 0);
    assert(started_blocked(0, report_thread) == 1);
    assert(stop_blocked_here() == 1);
    assert(started_blocked(1, report_thread) == 1);
    assert(stop_blocked_here() == 1);
    assert(thread_stop_signal_cpp_create_reports_blocked() == 1);
    assert(stop_blocked_here() == 1);
    assert(dsd_thread_block_stop_signal(0) == 0);
}

/* A refused create starts nothing and leaves the caller's mask as it was. */
static void
test_refused_create_keeps_the_mask(void) {
    dsd_thread_t thread;
    atomic_int seen;
    atomic_init(&seen, -1);
    assert(dsd_thread_block_stop_signal(0) == 0);
    assert(dsd_thread_create_impl(&thread, &seen, NULL) == EINVAL);
    assert(stop_blocked_here() == 0);
}

int
main(void) {
    test_block_stop_signal_sets_the_calling_thread();
    test_unblocked_creator_starts_blocked_threads();
    test_blocked_creator_stays_blocked();
    test_refused_create_keeps_the_mask();
    return 0;
}
