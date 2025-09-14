// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Realtime scheduling and CPU affinity utilities for critical threads.
 *
 * Provides SCHED_FIFO priority control and optional CPU pinning, driven by
 * environment variables (e.g., `DSD_NEO_RT_SCHED`, `DSD_NEO_RT_PRIO_<ROLE>`,
 * and `DSD_NEO_CPU_<ROLE>`).
 */

#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rt_sched.h>

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Optionally enable realtime scheduling and set CPU affinity for the current thread.
 *
 * Controlled by environment variables. When `DSD_NEO_RT_SCHED=1`, attempts to switch
 * the calling thread to SCHED_FIFO with a priority derived from `DSD_NEO_RT_PRIO_<ROLE>`
 * if present. If `DSD_NEO_CPU_<ROLE>` is set to a valid CPU index, pins the thread
 * to that CPU.
 *
 * @param role Optional role label (e.g. "DEMOD", "DONGLE", "USB").
 */
void
maybe_set_thread_realtime_and_affinity(const char* role) {
    const char* enable = getenv("DSD_NEO_RT_SCHED");
    if (!enable || enable[0] != '1') {
        return;
    }

    int policy = SCHED_FIFO;
    struct sched_param sp;
    int pmax = sched_get_priority_max(policy);
    int pmin = sched_get_priority_min(policy);
    int def = (pmax > 10) ? (pmax - 10) : pmax; /* default near top, but safe */
    char envname[64];

    sp.sched_priority = def;
    if (role) {
        /* e.g., DSD_NEO_RT_PRIO_DEMOD, DSD_NEO_RT_PRIO_DONGLE, DSD_NEO_RT_PRIO_USB */
        snprintf(envname, sizeof(envname), "DSD_NEO_RT_PRIO_%s", role);
        const char* prio_str = getenv(envname);
        if (prio_str && prio_str[0] != '\0') {
            int pr = atoi(prio_str);
            if (pr < pmin) {
                pr = pmin;
            }
            if (pr > pmax) {
                pr = pmax;
            }
            sp.sched_priority = pr;
        }
    }

    if (pthread_setschedparam(pthread_self(), policy, &sp) != 0) {
        int err = errno;
        LOG_WARNING("Failed to set %s thread to SCHED_FIFO (needs CAP_SYS_NICE). errno=%d (%s)\n", role ? role : "RT",
                    err, strerror(err));
    } else {
        LOG_INFO("%s thread SCHED_FIFO priority set to %d.\n", role ? role : "RT", sp.sched_priority);
    }

    if (role) {
        snprintf(envname, sizeof(envname), "DSD_NEO_CPU_%s", role);
        const char* cpu_str = getenv(envname);
        if (cpu_str && cpu_str[0] != '\0') {
            int cpu = atoi(cpu_str);
            if (cpu >= 0) {
#if defined(__linux__) && !defined(__CYGWIN__)
                cpu_set_t cpuset;
                CPU_ZERO(&cpuset);
                CPU_SET((unsigned)cpu, &cpuset);
                if (pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset) != 0) {
                    int err = errno;
                    LOG_WARNING("Failed to set CPU affinity for %s thread to CPU %d. errno=%d (%s)\n", role, cpu, err,
                                strerror(err));
                } else {
                    LOG_INFO("%s thread pinned to CPU %d.\n", role, cpu);
                }
#else
                (void)cpu;
                LOG_NOTICE("CPU affinity not supported on this platform.\n");
#endif
            }
        }
    }
}
