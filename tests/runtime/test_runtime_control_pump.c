// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/control_pump.h>
#include <stddef.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static int s_calls = 0;
static atomic_int s_stress_calls = 0;
static atomic_int s_stress_stop = 0;

static void
test_pump(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    s_calls++;
}

static void
stress_pump(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    (void)atomic_fetch_add(&s_stress_calls, 1);
}

struct stress_setter_args {
    int iterations;
};

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    stress_setter_thread(void* arg) {
    const struct stress_setter_args* args = (const struct stress_setter_args*)arg;
    for (int i = 0; i < args->iterations; i++) {
        dsd_runtime_set_control_pump(stress_pump);
        dsd_runtime_pump_controls(NULL, NULL);
        dsd_runtime_set_control_pump(NULL);
    }
    atomic_store(&s_stress_stop, 1);
    DSD_THREAD_RETURN;
}

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    stress_pump_thread(void* arg) {
    (void)arg;
    while (!atomic_load(&s_stress_stop)) {
        dsd_runtime_pump_controls(NULL, NULL);
    }
    DSD_THREAD_RETURN;
}

static int
test_basic_behavior(void) {
    s_calls = 0;

    dsd_runtime_set_control_pump(NULL);

    // Default behavior is a safe no-op until a pump is registered.
    dsd_runtime_pump_controls(NULL, NULL);
    if (s_calls != 0) {
        return 1;
    }

    dsd_runtime_set_control_pump(test_pump);
    dsd_runtime_pump_controls(NULL, NULL);
    dsd_runtime_pump_controls(NULL, NULL);
    if (s_calls != 2) {
        return 2;
    }

    dsd_runtime_set_control_pump(NULL);
    dsd_runtime_pump_controls(NULL, NULL);
    if (s_calls != 2) {
        return 3;
    }

    // Re-register should work.
    dsd_runtime_set_control_pump(test_pump);
    dsd_runtime_pump_controls(NULL, NULL);
    if (s_calls != 3) {
        return 4;
    }

    dsd_runtime_set_control_pump(NULL);
    dsd_runtime_pump_controls(NULL, NULL);
    if (s_calls != 3) {
        return 5;
    }

    return 0;
}

static int
test_concurrent_set_and_pump(void) {
    enum { k_iterations = 20000 };

    atomic_store(&s_stress_calls, 0);
    atomic_store(&s_stress_stop, 0);
    dsd_runtime_set_control_pump(NULL);

    struct stress_setter_args args = {k_iterations};
    dsd_thread_t setter_thread;
    dsd_thread_t pump_thread;

    if (dsd_thread_create(&pump_thread, stress_pump_thread, NULL) != 0) {
        dsd_runtime_set_control_pump(NULL);
        return 10;
    }
    if (dsd_thread_create(&setter_thread, stress_setter_thread, &args) != 0) {
        atomic_store(&s_stress_stop, 1);
        (void)dsd_thread_join(pump_thread);
        dsd_runtime_set_control_pump(NULL);
        return 11;
    }

    if (dsd_thread_join(setter_thread) != 0) {
        atomic_store(&s_stress_stop, 1);
        (void)dsd_thread_join(pump_thread);
        dsd_runtime_set_control_pump(NULL);
        return 12;
    }
    if (dsd_thread_join(pump_thread) != 0) {
        dsd_runtime_set_control_pump(NULL);
        return 13;
    }

    dsd_runtime_set_control_pump(NULL);
    if (atomic_load(&s_stress_calls) < k_iterations) {
        return 14;
    }

    return 0;
}

int
main(void) {
    int rc = test_basic_behavior();
    if (rc != 0) {
        return rc;
    }
    rc = test_concurrent_set_and_pump();
    if (rc != 0) {
        return rc;
    }
    return 0;
}
