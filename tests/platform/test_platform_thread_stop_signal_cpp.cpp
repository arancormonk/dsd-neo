// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The C++ expansion of dsd_thread_create() (a lambda) for PLATFORM_THREAD_STOP_SIGNAL. */

#include <atomic>
#include <csignal>
#include <dsd-neo/platform/threading.h>

namespace {

std::atomic<int> g_seen{-1};

DSD_THREAD_RETURN_TYPE
report_thread(void* arg) {
    sigset_t mask;
    int blocked = -1;
    if (pthread_sigmask(SIG_BLOCK, nullptr, &mask) == 0) {
        blocked = sigismember(&mask, SIGTSTP) == 1 ? 1 : 0;
    }
    static_cast<std::atomic<int>*>(arg)->store(blocked);
    DSD_THREAD_RETURN;
}

} // namespace

extern "C" int thread_stop_signal_cpp_create_reports_blocked(void);

extern "C" int
thread_stop_signal_cpp_create_reports_blocked(void) {
    g_seen.store(-1);
    dsd_thread_t thread;
    if (dsd_thread_create(&thread, report_thread, &g_seen) != 0) {
        return -1;
    }
    if (dsd_thread_join(thread) != 0) {
        return -1;
    }
    return g_seen.load();
}
