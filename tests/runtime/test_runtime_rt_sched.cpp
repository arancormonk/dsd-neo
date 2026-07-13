// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#define DSD_NEO_TEST_HOOKS 1

#include <cassert>
#include <cstdarg>
#include <cstring>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rt_sched.h>
#include <errno.h>

#include "dsd-neo/core/opts_fwd.h"

const char* dsd_neo_rt_sched_test_role_or_default(const char* role);
int dsd_neo_rt_sched_test_resolve_role_rt_priority(const dsdneoRuntimeConfig* cfg, const char* role);
int dsd_neo_rt_sched_test_resolve_role_cpu_affinity(const dsdneoRuntimeConfig* cfg, const char* role);

static dsdneoRuntimeConfig g_config;
static const dsdneoRuntimeConfig* g_config_ptr = &g_config;
static int g_config_init_calls = 0;
static int g_rt_calls = 0;
static int g_affinity_calls = 0;
static int g_last_priority = -1;
static int g_last_cpu = -1;
static int g_rt_rc = 0;
static int g_affinity_rc = 0;
static int g_rt_errno = EPERM;
static int g_affinity_errno = EINVAL;
static int g_log_counts[4];

extern "C" const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return g_config_ptr;
}

extern "C" void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
    ++g_config_init_calls;
}

extern "C" int
dsd_thread_set_realtime_priority(int priority) {
    ++g_rt_calls;
    g_last_priority = priority;
    errno = g_rt_errno;
    return g_rt_rc;
}

extern "C" int
dsd_thread_set_affinity(int cpu_index) {
    ++g_affinity_calls;
    g_last_cpu = cpu_index;
    errno = g_affinity_errno;
    return g_affinity_rc;
}

extern "C" void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    assert(format != nullptr);
    if (level >= LOG_LEVEL_ERROR && level <= LOG_LEVEL_DEBUG) {
        ++g_log_counts[level];
    }
    va_list ap;
    va_start(ap, format);
    va_end(ap);
}

static void
reset_state(void) {
    DSD_MEMSET(&g_config, 0, sizeof(g_config));
    g_config_ptr = &g_config;
    g_config_init_calls = 0;
    g_rt_calls = 0;
    g_affinity_calls = 0;
    g_last_priority = -1;
    g_last_cpu = -1;
    g_rt_rc = 0;
    g_affinity_rc = 0;
    g_rt_errno = EPERM;
    g_affinity_errno = EINVAL;
    DSD_MEMSET(g_log_counts, 0, sizeof(g_log_counts));
}

static void
enable_config(void) {
    g_config.rt_sched_enable = 1;
    g_config.rt_prio_usb_is_set = 1;
    g_config.rt_prio_usb = 80;
    g_config.rt_prio_dongle_is_set = 1;
    g_config.rt_prio_dongle = 81;
    g_config.rt_prio_demod_is_set = 1;
    g_config.rt_prio_demod = 82;
    g_config.cpu_usb_is_set = 1;
    g_config.cpu_usb = 1;
    g_config.cpu_dongle_is_set = 1;
    g_config.cpu_dongle = 2;
    g_config.cpu_demod_is_set = 1;
    g_config.cpu_demod = 3;
}

static void
test_role_resolvers(void) {
    reset_state();
    enable_config();

    assert(strcmp(dsd_neo_rt_sched_test_role_or_default("USB"), "USB") == 0);
    assert(strcmp(dsd_neo_rt_sched_test_role_or_default(nullptr), "RT") == 0);
    assert(dsd_neo_rt_sched_test_resolve_role_rt_priority(nullptr, "USB") == 0);
    assert(dsd_neo_rt_sched_test_resolve_role_rt_priority(&g_config, nullptr) == 0);
    assert(dsd_neo_rt_sched_test_resolve_role_rt_priority(&g_config, "USB") == 80);
    assert(dsd_neo_rt_sched_test_resolve_role_rt_priority(&g_config, "DONGLE") == 81);
    assert(dsd_neo_rt_sched_test_resolve_role_rt_priority(&g_config, "DEMOD") == 82);
    assert(dsd_neo_rt_sched_test_resolve_role_rt_priority(&g_config, "OTHER") == 0);

    assert(dsd_neo_rt_sched_test_resolve_role_cpu_affinity(nullptr, "USB") == -1);
    assert(dsd_neo_rt_sched_test_resolve_role_cpu_affinity(&g_config, nullptr) == -1);
    assert(dsd_neo_rt_sched_test_resolve_role_cpu_affinity(&g_config, "USB") == 1);
    assert(dsd_neo_rt_sched_test_resolve_role_cpu_affinity(&g_config, "DONGLE") == 2);
    assert(dsd_neo_rt_sched_test_resolve_role_cpu_affinity(&g_config, "DEMOD") == 3);
    assert(dsd_neo_rt_sched_test_resolve_role_cpu_affinity(&g_config, "OTHER") == -1);
}

static void
test_disabled_and_missing_config_paths(void) {
    reset_state();
    g_config_ptr = nullptr;
    maybe_set_thread_realtime_and_affinity("USB");
    assert(g_config_init_calls == 1);
    assert(g_rt_calls == 0);
    assert(g_affinity_calls == 0);

    reset_state();
    maybe_set_thread_realtime_and_affinity("USB");
    assert(g_config_init_calls == 0);
    assert(g_rt_calls == 0);
    assert(g_affinity_calls == 0);
}

static void
test_public_call_success_and_failure_paths(void) {
    reset_state();
    enable_config();
    maybe_set_thread_realtime_and_affinity("USB");
    assert(g_rt_calls == 1);
    assert(g_last_priority == 80);
    assert(g_affinity_calls == 1);
    assert(g_last_cpu == 1);
    assert(g_log_counts[LOG_LEVEL_INFO] == 2);

    reset_state();
    enable_config();
    g_rt_rc = EINVAL;
    g_affinity_rc = EPERM;
    maybe_set_thread_realtime_and_affinity("DONGLE");
    assert(g_rt_calls == 1);
    assert(g_last_priority == 81);
    assert(g_affinity_calls == 1);
    assert(g_last_cpu == 2);
    assert(g_log_counts[LOG_LEVEL_WARN] == 2);

    reset_state();
    enable_config();
    g_config.cpu_demod_is_set = 0;
    maybe_set_thread_realtime_and_affinity("DEMOD");
    assert(g_rt_calls == 1);
    assert(g_last_priority == 82);
    assert(g_affinity_calls == 0);

    reset_state();
    enable_config();
    maybe_set_thread_realtime_and_affinity(nullptr);
    assert(g_rt_calls == 1);
    assert(g_last_priority == 0);
    assert(g_affinity_calls == 0);
}

int
main(void) {
    test_role_resolvers();
    test_disabled_and_missing_config_paths();
    test_public_call_success_and_failure_paths();
    return 0;
}
