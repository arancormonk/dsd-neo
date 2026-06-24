// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"

#if defined(__SSE__) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

static dsdneoRuntimeConfig g_config;
static int g_config_available = 1;
static int g_config_init_calls;
static int g_config_init_ftz_daz_enable;
static int g_log_notice_calls;
static char g_last_log[128];

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    if (level == LOG_LEVEL_INFO) {
        ++g_log_notice_calls;
    }
    va_list ap;
    va_start(ap, format);
    (void)DSD_VSNPRINTF(g_last_log, sizeof g_last_log, format, ap);
    va_end(ap);
}

void
dsd_neo_config_init(const dsd_opts* opts) {
    (void)opts;
    ++g_config_init_calls;
    g_config_available = 1;
    DSD_MEMSET(&g_config, 0, sizeof g_config);
    g_config.ftz_daz_enable = g_config_init_ftz_daz_enable;
}

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return g_config_available ? &g_config : NULL;
}

static void
reset_harness(void) {
    DSD_MEMSET(&g_config, 0, sizeof g_config);
    g_config_available = 1;
    g_config_init_calls = 0;
    g_config_init_ftz_daz_enable = 0;
    g_log_notice_calls = 0;
    g_last_log[0] = '\0';
}

static void
test_missing_config_initializes_and_honors_disabled_default(void) {
    reset_harness();
    g_config_available = 0;

    dsd_bootstrap_enable_ftz_daz_if_enabled();

#if defined(__SSE__) || defined(__SSE2__)
    assert(g_config_init_calls == 1);
    assert(g_log_notice_calls == 0);
#else
    assert(g_config_init_calls == 0);
#endif
}

static void
test_disabled_config_does_not_log_or_change_mode(void) {
    reset_harness();

#if defined(__SSE__) || defined(__SSE2__)
    unsigned int original = _mm_getcsr();
    dsd_bootstrap_enable_ftz_daz_if_enabled();
    assert(_mm_getcsr() == original);
    assert(g_log_notice_calls == 0);
#else
    dsd_bootstrap_enable_ftz_daz_if_enabled();
#endif
}

static void
test_enabled_config_sets_ftz_daz_bits(void) {
    reset_harness();
    g_config.ftz_daz_enable = 1;

#if defined(__SSE__) || defined(__SSE2__)
    const unsigned int ftz_daz_bits = (1u << 15) | (1u << 6);
    unsigned int original = _mm_getcsr();
    _mm_setcsr(original & ~ftz_daz_bits);

    dsd_bootstrap_enable_ftz_daz_if_enabled();

    assert((_mm_getcsr() & ftz_daz_bits) == ftz_daz_bits);
    assert(g_log_notice_calls == 1);
    assert(strstr(g_last_log, "FTZ/DAZ") != NULL);
    _mm_setcsr(original);
#else
    dsd_bootstrap_enable_ftz_daz_if_enabled();
    assert(g_log_notice_calls == 0);
#endif
}

int
main(void) {
    test_missing_config_initializes_and_honors_disabled_default();
    test_disabled_config_does_not_log_or_change_mode();
    test_enabled_config_sets_ftz_daz_bits();

    printf("RUNTIME_BOOTSTRAP_SYSTEM: OK\n");
    return 0;
}
