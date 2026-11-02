// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The run's floating-point mode (SSE FTZ/DAZ), applied by each float thread at its entry.
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <stddef.h>

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
#include <xmmintrin.h>
#endif

static atomic_int g_fp_mode = DSD_FP_MODE_INHERIT;
/* Set by tests while no observed thread runs (dsd_runtime_fp_mode_set_observer()). */
static dsd_runtime_fp_mode_observer_fn g_fp_mode_observer = NULL;

int
dsd_runtime_fp_mode_supported(void) {
    return DSD_RUNTIME_FP_MODE_HAVE_MXCSR;
}

dsd_fp_mode
dsd_runtime_fp_mode_from_config(const dsdneoRuntimeConfig* cfg) {
    if (!cfg || !cfg->ftz_daz_is_set) {
        return DSD_FP_MODE_INHERIT;
    }
    return cfg->ftz_daz_enable ? DSD_FP_MODE_ON : DSD_FP_MODE_OFF;
}

void
dsd_runtime_fp_mode_publish(dsd_fp_mode mode) {
    if (mode != DSD_FP_MODE_OFF && mode != DSD_FP_MODE_ON) {
        mode = DSD_FP_MODE_INHERIT;
    }
    atomic_store(&g_fp_mode, (int)mode);
}

dsd_fp_mode
dsd_runtime_fp_mode_current(void) {
    return (dsd_fp_mode)atomic_load(&g_fp_mode);
}

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
/* @p csr with the run's @p mode applied. */
static unsigned int
fp_mode_csr(unsigned int csr, int mode) {
    if (mode == DSD_FP_MODE_ON) {
        return csr | DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS;
    }
    if (mode == DSD_FP_MODE_OFF) {
        return csr & ~DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS;
    }
    return csr;
}
#endif

void
dsd_runtime_fp_mode_apply_thread(const char* thread_tag) {
    unsigned int csr = 0U;
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    csr = _mm_getcsr();
    const unsigned int want = fp_mode_csr(csr, atomic_load(&g_fp_mode));
    if (want != csr) {
        _mm_setcsr(want);
        csr = want;
    }
#endif
    const dsd_runtime_fp_mode_observer_fn observer = g_fp_mode_observer;
    if (observer) {
        observer(thread_tag ? thread_tag : "", csr);
    }
}

void
dsd_runtime_fp_mode_scope_enter(dsd_fp_mode_scope* scope) {
    if (!scope) {
        return;
    }
    scope->saved_csr = 0U;
    scope->restore = 0;
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    const int mode = atomic_load(&g_fp_mode);
    if (mode == DSD_FP_MODE_INHERIT) {
        return;
    }
    const unsigned int csr = _mm_getcsr();
    const unsigned int want = fp_mode_csr(csr, mode);
    if (want != csr) {
        scope->saved_csr = csr;
        scope->restore = 1;
        _mm_setcsr(want);
    }
#endif
}

void
dsd_runtime_fp_mode_scope_leave(const dsd_fp_mode_scope* scope) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    if (scope && scope->restore) {
        _mm_setcsr(scope->saved_csr);
    }
#else
    (void)scope;
#endif
}

void
dsd_runtime_fp_mode_set_observer(dsd_runtime_fp_mode_observer_fn observer) {
    g_fp_mode_observer = observer;
}
