// SPDX-License-Identifier: GPL-3.0-or-later
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

#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rt_sched.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !DSD_PLATFORM_WIN_NATIVE
#include <sched.h>
#endif

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

    char envname[64];
    int priority = 0; /* default priority for abstraction layer */

    if (role) {
        /* e.g., DSD_NEO_RT_PRIO_DEMOD, DSD_NEO_RT_PRIO_DONGLE, DSD_NEO_RT_PRIO_USB */
        snprintf(envname, sizeof(envname), "DSD_NEO_RT_PRIO_%s", role);
        const char* prio_str = getenv(envname);
        if (prio_str && prio_str[0] != '\0') {
            priority = atoi(prio_str);
        }
    }

    if (dsd_thread_set_realtime_priority(priority) != 0) {
        int err = errno;
        LOG_WARNING("Failed to set %s thread to realtime priority (needs elevated privileges). errno=%d (%s)\n",
                    role ? role : "RT", err, strerror(err));
    } else {
        LOG_INFO("%s thread realtime priority set to %d.\n", role ? role : "RT", priority);
    }

    if (role) {
        snprintf(envname, sizeof(envname), "DSD_NEO_CPU_%s", role);
        const char* cpu_str = getenv(envname);
        if (cpu_str && cpu_str[0] != '\0') {
            int cpu = atoi(cpu_str);
            if (cpu >= 0) {
                if (dsd_thread_set_affinity(cpu) != 0) {
                    int err = errno;
                    LOG_WARNING("Failed to set CPU affinity for %s thread to CPU %d. errno=%d (%s)\n", role, cpu, err,
                                strerror(err));
                } else {
                    LOG_INFO("%s thread pinned to CPU %d.\n", role, cpu);
                }
            }
        }
    }
}
