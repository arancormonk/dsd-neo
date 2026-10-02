// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// LLVM 22/GCC 16 misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <atomic>
#include <cassert>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/worker_pool.h>
#include <thread>

struct demod_state;

struct CounterArg {
    std::atomic<int>* counter;
    std::thread::id* thread_seen;
};

static void
increment_counter(void* arg) {
    CounterArg* counter_arg = static_cast<CounterArg*>(arg);
    counter_arg->counter->fetch_add(1, std::memory_order_relaxed);
    if (counter_arg->thread_seen != nullptr) {
        *counter_arg->thread_seen = std::this_thread::get_id();
    }
}

static demod_state*
state_key(void* storage) {
    return static_cast<demod_state*>(storage);
}

static void
set_mt_config(const char* value) {
    if (value != nullptr) {
        dsd_setenv("DSD_NEO_MT", value, 1);
    } else {
        dsd_unsetenv("DSD_NEO_MT");
    }
    dsd_neo_config_init();
}

static void
test_disabled_mode_runs_synchronously(void) {
    int key_storage = 0;
    std::atomic<int> counter{0};
    std::thread::id task0_thread;
    std::thread::id task1_thread;
    CounterArg arg0{&counter, &task0_thread};
    CounterArg arg1{&counter, &task1_thread};
    std::thread::id caller = std::this_thread::get_id();

    set_mt_config(nullptr);
    demod_mt_destroy(state_key(&key_storage));
    demod_mt_init(state_key(&key_storage));
    demod_mt_run_two(state_key(&key_storage), increment_counter, &arg0, increment_counter, &arg1);

    assert(counter.load(std::memory_order_relaxed) == 2);
    assert(task0_thread == caller);
    assert(task1_thread == caller);
    demod_mt_destroy(state_key(&key_storage));
}

static void
test_enabled_mode_runs_and_tears_down(void) {
    int key_storage = 0;
    std::atomic<int> counter{0};
    std::thread::id task0_thread;
    std::thread::id task1_thread;
    CounterArg arg0{&counter, &task0_thread};
    CounterArg arg1{&counter, &task1_thread};
    std::thread::id caller = std::this_thread::get_id();

    set_mt_config("1");
    demod_mt_destroy(state_key(&key_storage));
    demod_mt_init(state_key(&key_storage));
    demod_mt_init(state_key(&key_storage));

    demod_mt_run_two(state_key(&key_storage), increment_counter, &arg0, nullptr, nullptr);
    assert(counter.load(std::memory_order_relaxed) == 1);
    assert(task0_thread != caller);

    demod_mt_run_two(state_key(&key_storage), increment_counter, &arg0, increment_counter, &arg1);
    assert(counter.load(std::memory_order_relaxed) == 3);
    assert(task0_thread != caller);
    assert(task1_thread != caller);

    demod_mt_destroy(state_key(&key_storage));
    demod_mt_destroy(state_key(&key_storage));

    task0_thread = std::thread::id();
    task1_thread = std::thread::id();
    demod_mt_run_two(state_key(&key_storage), increment_counter, &arg0, increment_counter, &arg1);
    assert(counter.load(std::memory_order_relaxed) == 5);
    assert(task0_thread == caller);
    assert(task1_thread == caller);
}

int
main(void) {
    test_disabled_mode_runs_synchronously();
    test_enabled_mode_runs_and_tears_down();
    dsd_unsetenv("DSD_NEO_MT");
    dsd_neo_config_init();
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
// NOLINTEND(cert-dcl03-c,misc-static-assert)
