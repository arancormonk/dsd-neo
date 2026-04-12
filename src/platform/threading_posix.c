// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Enable GNU extensions for pthread_setaffinity_np and CPU_* macros.
 * Must be defined before ANY system headers are included. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

/* Include sched.h early:
 * - Linux: CPU_* affinity macros (requires _GNU_SOURCE)
 * - macOS: struct sched_param/SCHED_* constants for pthread scheduling */
#if defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
// IWYU can incorrectly suggest libc-internal bits headers for sched_param.
// Keep the portable public header explicitly.
// IWYU pragma: no_include <bits/types/struct_sched_param.h>
#include <sched.h> // IWYU pragma: keep
#endif

#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h> // IWYU pragma: keep (macOS monotonic/realtime time declarations)
#include <pthread.h>

#include "dsd-neo/platform/platform.h"

#if !DSD_PLATFORM_WIN_NATIVE

#include <errno.h>
#include <stdint.h>
#include <time.h>
struct timespec;

static void
timespec_from_ns(uint64_t ns, struct timespec* ts) {
    if (!ts) {
        return;
    }
    ts->tv_sec = (time_t)(ns / 1000000000ULL);
    ts->tv_nsec = (long)(ns % 1000000000ULL);
}

/*============================================================================
 * Thread Functions
 *============================================================================*/

int
dsd_thread_create(dsd_thread_t* thread, dsd_thread_fn func, void* arg) {
    if (!thread || !func) {
        return EINVAL;
    }
    return pthread_create(thread, NULL, func, arg);
}

int
dsd_thread_join(dsd_thread_t thread) {
    return pthread_join(thread, NULL);
}

dsd_thread_t
dsd_thread_self(void) {
    return pthread_self();
}

/*============================================================================
 * Mutex Functions
 *============================================================================*/

int
dsd_mutex_init(dsd_mutex_t* mutex) {
    if (!mutex) {
        return EINVAL;
    }
    return pthread_mutex_init(mutex, NULL);
}

int
dsd_mutex_destroy(dsd_mutex_t* mutex) {
    if (!mutex) {
        return EINVAL;
    }
    return pthread_mutex_destroy(mutex);
}

int
dsd_mutex_lock(dsd_mutex_t* mutex) {
    if (!mutex) {
        return EINVAL;
    }
    return pthread_mutex_lock(mutex);
}

int
dsd_mutex_unlock(dsd_mutex_t* mutex) {
    if (!mutex) {
        return EINVAL;
    }
    return pthread_mutex_unlock(mutex);
}

/*============================================================================
 * Condition Variable Functions
 *============================================================================*/

int
dsd_cond_init(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
    return pthread_cond_init(cond, NULL);
}

int
dsd_cond_destroy(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
    return pthread_cond_destroy(cond);
}

int
dsd_cond_wait(dsd_cond_t* cond, dsd_mutex_t* mutex) {
    if (!cond || !mutex) {
        return EINVAL;
    }
    return pthread_cond_wait(cond, mutex);
}

int
dsd_cond_timedwait(dsd_cond_t* cond, dsd_mutex_t* mutex, unsigned int timeout_ms) {
    if (!cond || !mutex) {
        return EINVAL;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;

    /* Normalize nanoseconds */
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    return pthread_cond_timedwait(cond, mutex, &ts);
}

int
dsd_cond_init_monotonic(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
#if defined(__APPLE__) && defined(__MACH__)
    return pthread_cond_init(cond, NULL);
#elif defined(CLOCK_MONOTONIC)
    pthread_condattr_t attr;
    int rc = pthread_condattr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    rc = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    if (rc != 0) {
        (void)pthread_condattr_destroy(&attr);
        return rc;
    }
    rc = pthread_cond_init(cond, &attr);
    (void)pthread_condattr_destroy(&attr);
    return rc;
#else
    return pthread_cond_init(cond, NULL);
#endif
}

int
dsd_cond_timedwait_monotonic(dsd_cond_t* cond, dsd_mutex_t* mutex, uint64_t deadline_ns) {
    if (!cond || !mutex) {
        return EINVAL;
    }

#if defined(__APPLE__) && defined(__MACH__)
    uint64_t now_ns = dsd_time_monotonic_ns();
    if (deadline_ns <= now_ns) {
        return ETIMEDOUT;
    }
    uint64_t rel_ns = deadline_ns - now_ns;
    struct timespec rel;
    timespec_from_ns(rel_ns, &rel);
    return pthread_cond_timedwait_relative_np(cond, mutex, &rel);
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    timespec_from_ns(deadline_ns, &ts);
    return pthread_cond_timedwait(cond, mutex, &ts);
#else
    const uint64_t k_max_wait_ns = 50ULL * 1000000ULL;
    for (;;) {
        uint64_t now_ns = dsd_time_monotonic_ns();
        if (now_ns >= deadline_ns) {
            return ETIMEDOUT;
        }

        uint64_t wait_ns = deadline_ns - now_ns;
        if (wait_ns > k_max_wait_ns) {
            wait_ns = k_max_wait_ns;
        }

        uint64_t rt_deadline_ns = dsd_time_realtime_ns() + wait_ns;
        struct timespec ts;
        timespec_from_ns(rt_deadline_ns, &ts);
        int rc = pthread_cond_timedwait(cond, mutex, &ts);
        if (rc == ETIMEDOUT) {
            continue;
        }
        return rc;
    }
#endif
}

int
dsd_cond_signal(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
    return pthread_cond_signal(cond);
}

int
dsd_cond_broadcast(dsd_cond_t* cond) {
    if (!cond) {
        return EINVAL;
    }
    return pthread_cond_broadcast(cond);
}

/*============================================================================
 * Thread Priority / Scheduling
 *============================================================================*/

int
dsd_thread_set_realtime_priority(int priority) {
#if DSD_PLATFORM_LINUX || DSD_PLATFORM_MACOS
    struct sched_param sp;
    int policy = SCHED_FIFO;
    int pmax = sched_get_priority_max(policy);
    int pmin = sched_get_priority_min(policy);

    if (priority < pmin) {
        priority = pmin;
    }
    if (priority > pmax) {
        priority = pmax;
    }

    sp.sched_priority = priority;
    return pthread_setschedparam(pthread_self(), policy, &sp);
#else
    (void)priority;
    return ENOSYS;
#endif
}

int
dsd_thread_set_affinity(int cpu_index) {
#if DSD_PLATFORM_LINUX
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET((unsigned)cpu_index, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
#else
    (void)cpu_index;
    return ENOSYS; /* Not supported */
#endif
}

#endif /* !DSD_PLATFORM_WIN_NATIVE */
