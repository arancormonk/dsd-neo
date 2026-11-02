// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <dsd-neo/runtime/log.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
#include <xmmintrin.h>
#endif

static dsdneoRuntimeConfig g_config;
static int g_config_available = 1;
static int g_config_init_calls;
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
dsd_neo_config_init(void) {
    ++g_config_init_calls;
    g_config_available = 1;
    DSD_MEMSET(&g_config, 0, sizeof g_config);
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
    g_log_notice_calls = 0;
    g_last_log[0] = '\0';
}

/* MXCSR as dsd_bootstrap_apply_fp_mode() leaves it, starting from @p start_bits; the caller's own value is put back. */
static unsigned int
apply_from(unsigned int start_bits) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    const unsigned int original = _mm_getcsr();
    _mm_setcsr((original & ~DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS) | start_bits);
    dsd_bootstrap_apply_fp_mode();
    const unsigned int after = _mm_getcsr();
    _mm_setcsr(original);
    return after & DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS;
#else
    dsd_bootstrap_apply_fp_mode();
    return start_bits;
#endif
}

static void
test_missing_config_initializes_and_leaves_mode(void) {
    reset_harness();
    g_config_available = 0;

    const unsigned int after = apply_from(DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);

    assert(g_config_init_calls == 1);
    assert(after == DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    assert(dsd_runtime_fp_mode_current() == DSD_FP_MODE_INHERIT);
    assert(g_log_notice_calls == 0);
}

/* Unset leaves whatever the thread inherited (a -ffast-math link or an embedding host may start it with FTZ/DAZ on). */
static void
test_unset_leaves_inherited_mode(void) {
    reset_harness();

    assert(apply_from(DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS) == DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    assert(apply_from(0U) == 0U);
    assert(dsd_runtime_fp_mode_current() == DSD_FP_MODE_INHERIT);
    assert(g_log_notice_calls == 0);
}

/* DSD_NEO_FTZ_DAZ=0 clears an inherited FTZ/DAZ: "off" means off. */
static void
test_off_clears_inherited_mode(void) {
    reset_harness();
    g_config.ftz_daz_is_set = 1;

    const unsigned int after = apply_from(DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);

    assert(dsd_runtime_fp_mode_current() == DSD_FP_MODE_OFF);
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    assert(after == 0U);
    assert(g_log_notice_calls == 1);
    assert(strstr(g_last_log, "FTZ/DAZ off") != NULL);
#else
    (void)after;
    assert(g_log_notice_calls == 1);
    assert(strstr(g_last_log, "ignored") != NULL);
#endif
}

static void
test_on_sets_ftz_daz_bits(void) {
    reset_harness();
    g_config.ftz_daz_is_set = 1;
    g_config.ftz_daz_enable = 1;

    const unsigned int after = apply_from(0U);

    assert(dsd_runtime_fp_mode_current() == DSD_FP_MODE_ON);
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    assert(after == DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    assert(g_log_notice_calls == 1);
    assert(strstr(g_last_log, "FTZ/DAZ on") != NULL);
#else
    (void)after;
    assert(g_log_notice_calls == 1);
#endif
}

int
main(void) {
    test_missing_config_initializes_and_leaves_mode();
    test_unset_leaves_inherited_mode();
    test_off_clears_inherited_mode();
    test_on_sets_ftz_daz_bits();
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_INHERIT);

    printf("RUNTIME_BOOTSTRAP_SYSTEM: OK\n");
    return 0;
}
