// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The run's floating-point mode (issue #672 B12): the config mapping, a thread's entry apply (not inheritance: the
 * creating thread holds the opposite mode) and the scope a library callback (Airspy) wraps dsd-neo's work in, which
 * puts the library thread's MXCSR back exactly. */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <stdio.h>
#include <string.h>

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
#include <xmmintrin.h>
#endif

#define FTZ_DAZ DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS

static unsigned int
csr_get(void) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    return _mm_getcsr();
#else
    return 0U;
#endif
}

static void
csr_set(unsigned int csr) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    _mm_setcsr(csr);
#else
    (void)csr;
#endif
}

static void
csr_set_ftz_daz(unsigned int bits) {
    csr_set((csr_get() & ~FTZ_DAZ) | bits);
}

static struct {
    int calls;
    char tag[32];
    unsigned int csr;
} g_seen;

static void
observe(const char* thread_tag, unsigned int csr) {
    g_seen.calls++;
    DSD_SNPRINTF(g_seen.tag, sizeof g_seen.tag, "%s", thread_tag);
    g_seen.csr = csr;
}

static void
test_mode_from_config(void) {
    dsdneoRuntimeConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    assert(dsd_runtime_fp_mode_from_config(NULL) == DSD_FP_MODE_INHERIT);
    assert(dsd_runtime_fp_mode_from_config(&cfg) == DSD_FP_MODE_INHERIT);
    cfg.ftz_daz_enable = 1; /* not set: the flag alone means nothing */
    assert(dsd_runtime_fp_mode_from_config(&cfg) == DSD_FP_MODE_INHERIT);
    cfg.ftz_daz_is_set = 1;
    assert(dsd_runtime_fp_mode_from_config(&cfg) == DSD_FP_MODE_ON);
    cfg.ftz_daz_enable = 0;
    assert(dsd_runtime_fp_mode_from_config(&cfg) == DSD_FP_MODE_OFF);
}

static void
test_publish_and_current(void) {
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_ON);
    assert(dsd_runtime_fp_mode_current() == DSD_FP_MODE_ON);
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_OFF);
    assert(dsd_runtime_fp_mode_current() == DSD_FP_MODE_OFF);
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_INHERIT);
    assert(dsd_runtime_fp_mode_supported() == DSD_RUNTIME_FP_MODE_HAVE_MXCSR);
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    fp_thread(void* arg) {
    (void)arg;
    dsd_runtime_fp_mode_apply_thread("fp_test");
    DSD_THREAD_RETURN;
}

/* A thread started while its creator holds the opposite mode reads the run's mode after its entry apply. */
static void
expect_thread_mode(dsd_fp_mode mode, unsigned int creator_bits, unsigned int want_bits) {
    const unsigned int original = csr_get();
    dsd_runtime_fp_mode_publish(mode);
    csr_set_ftz_daz(creator_bits);
    DSD_MEMSET(&g_seen, 0, sizeof g_seen);
    dsd_runtime_fp_mode_set_observer(observe);
    dsd_thread_t thread;
    assert(dsd_thread_create(&thread, fp_thread, NULL) == 0);
    assert(dsd_thread_join(thread) == 0);
    dsd_runtime_fp_mode_set_observer(NULL);
    csr_set(original);
    assert(g_seen.calls == 1);
    assert(strcmp(g_seen.tag, "fp_test") == 0);
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    assert((g_seen.csr & FTZ_DAZ) == want_bits);
#else
    (void)want_bits;
    assert(g_seen.csr == 0U);
#endif
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_INHERIT);
}

static void
test_thread_entry_applies_the_run_mode(void) {
    expect_thread_mode(DSD_FP_MODE_ON, 0U, FTZ_DAZ);
    expect_thread_mode(DSD_FP_MODE_OFF, FTZ_DAZ, 0U);
    /* Unset: the entry leaves the thread as it started (on Linux the creator's mode, inherited). */
    const unsigned int original = csr_get();
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_INHERIT);
    csr_set_ftz_daz(0U);
    dsd_runtime_fp_mode_apply_thread("caller");
    assert((csr_get() & FTZ_DAZ) == 0U);
    csr_set(original);
}

/* The Airspy callback's shape: the scope around dsd-neo's work. The fake callback records the mode it ran in. */
static unsigned int g_callback_csr;

static void
fake_callback(void) {
    g_callback_csr = csr_get();
}

static unsigned int
wrapped_callback(void) {
    dsd_fp_mode_scope scope;
    dsd_runtime_fp_mode_scope_enter(&scope);
    fake_callback();
    dsd_runtime_fp_mode_scope_leave(&scope);
    return scope.restore ? 1U : 0U;
}

static void
expect_scope(dsd_fp_mode mode, unsigned int library_bits, unsigned int want_inside, unsigned int want_restore) {
    const unsigned int original = csr_get();
    dsd_runtime_fp_mode_publish(mode);
    csr_set_ftz_daz(library_bits);
    const unsigned int library_csr = csr_get();
    g_callback_csr = 0U;
    const unsigned int restored = wrapped_callback();
    const unsigned int after = csr_get();
    csr_set(original);
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_INHERIT);
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    assert((g_callback_csr & FTZ_DAZ) == want_inside);
    assert(restored == want_restore);
    assert(after == library_csr);
#else
    (void)want_inside;
    (void)want_restore;
    (void)library_csr;
    assert(restored == 0U && after == 0U);
#endif
}

static void
test_scope_saves_applies_and_restores(void) {
    /* The mode differs from the library thread's: applied inside, the library's MXCSR back after. */
    expect_scope(DSD_FP_MODE_ON, 0U, FTZ_DAZ, 1U);
    expect_scope(DSD_FP_MODE_OFF, FTZ_DAZ, 0U, 1U);
    /* Already matching, or unset: nothing written. */
    expect_scope(DSD_FP_MODE_ON, FTZ_DAZ, FTZ_DAZ, 0U);
    expect_scope(DSD_FP_MODE_OFF, 0U, 0U, 0U);
    expect_scope(DSD_FP_MODE_INHERIT, FTZ_DAZ, FTZ_DAZ, 0U);
    expect_scope(DSD_FP_MODE_INHERIT, 0U, 0U, 0U);
    /* NULL is harmless. */
    dsd_runtime_fp_mode_scope_enter(NULL);
    dsd_runtime_fp_mode_scope_leave(NULL);
}

int
main(void) {
    test_mode_from_config();
    test_publish_and_current();
    test_thread_entry_applies_the_run_mode();
    test_scope_saves_applies_and_restores();
    printf("RUNTIME_FP_MODE: OK\n");
    return 0;
}
