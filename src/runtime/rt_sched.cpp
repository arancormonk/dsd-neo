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
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static const char*
role_or_default(const char* role) {
    return role ? role : "RT";
}

static const dsdneoRuntimeConfig*
runtime_config_ready(void) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(NULL);
        cfg = dsd_neo_get_config();
    }
    return cfg;
}

static int
resolve_role_rt_priority(const dsdneoRuntimeConfig* cfg, const char* role) {
    if (!cfg || !role) {
        return 0;
    }
    if (strcmp(role, "USB") == 0 && cfg->rt_prio_usb_is_set) {
        return cfg->rt_prio_usb;
    }
    if (strcmp(role, "DONGLE") == 0 && cfg->rt_prio_dongle_is_set) {
        return cfg->rt_prio_dongle;
    }
    if (strcmp(role, "DEMOD") == 0 && cfg->rt_prio_demod_is_set) {
        return cfg->rt_prio_demod;
    }
    return 0;
}

static int
resolve_role_cpu_affinity(const dsdneoRuntimeConfig* cfg, const char* role) {
    if (!cfg || !role) {
        return -1;
    }
    if (strcmp(role, "USB") == 0 && cfg->cpu_usb_is_set) {
        return cfg->cpu_usb;
    }
    if (strcmp(role, "DONGLE") == 0 && cfg->cpu_dongle_is_set) {
        return cfg->cpu_dongle;
    }
    if (strcmp(role, "DEMOD") == 0 && cfg->cpu_demod_is_set) {
        return cfg->cpu_demod;
    }
    return -1;
}

#ifdef DSD_NEO_TEST_HOOKS
const char*
dsd_neo_rt_sched_test_role_or_default(const char* role) {
    return role_or_default(role);
}

int
dsd_neo_rt_sched_test_resolve_role_rt_priority(const dsdneoRuntimeConfig* cfg, const char* role) {
    return resolve_role_rt_priority(cfg, role);
}

int
dsd_neo_rt_sched_test_resolve_role_cpu_affinity(const dsdneoRuntimeConfig* cfg, const char* role) {
    return resolve_role_cpu_affinity(cfg, role);
}
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
    const dsdneoRuntimeConfig* cfg = runtime_config_ready();
    const char* label = role_or_default(role);
    if (!cfg || !cfg->rt_sched_enable) {
        return;
    }

    int priority = resolve_role_rt_priority(cfg, role);

    if (dsd_thread_set_realtime_priority(priority) != 0) {
        int err = errno;
        LOG_WARN("WARNING: Failed to set %s thread to realtime priority (needs elevated privileges). errno=%d (%s)\n",
                 label, err, strerror(err));
    } else {
        LOG_INFO("%s thread realtime priority set to %d.\n", label, priority);
    }

    int cpu = resolve_role_cpu_affinity(cfg, role);
    if (cpu >= 0) {
        if (dsd_thread_set_affinity(cpu) != 0) {
            int err = errno;
            LOG_WARN("WARNING: Failed to set CPU affinity for %s thread to CPU %d. errno=%d (%s)\n", label, cpu, err,
                     strerror(err));
        } else {
            LOG_INFO("%s thread pinned to CPU %d.\n", label, cpu);
        }
    }
}
