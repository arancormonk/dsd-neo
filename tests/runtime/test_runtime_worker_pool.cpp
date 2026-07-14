// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(misc-use-internal-linkage)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <atomic>
#include <cassert>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/worker_pool.h>
#include <pthread.h>
#include <stdlib.h>

struct demod_state;

struct CounterArg {
    std::atomic<int>* counter;
    pthread_t* thread_seen;
};

static void
increment_counter(void* arg) {
    CounterArg* counter_arg = static_cast<CounterArg*>(arg);
    counter_arg->counter->fetch_add(1, std::memory_order_relaxed);
    if (counter_arg->thread_seen != nullptr) {
        *counter_arg->thread_seen = pthread_self();
    }
}

static demod_state*
state_key(void* storage) {
    return static_cast<demod_state*>(storage);
}

static void
set_mt_config(const char* value) {
    if (value != nullptr) {
        setenv("DSD_NEO_MT", value, 1);
    } else {
        unsetenv("DSD_NEO_MT");
    }
    dsd_neo_config_init();
}

static void
test_disabled_mode_runs_synchronously(void) {
    int key_storage = 0;
    std::atomic<int> counter{0};
    pthread_t task0_thread = {};
    pthread_t task1_thread = {};
    CounterArg arg0{&counter, &task0_thread};
    CounterArg arg1{&counter, &task1_thread};
    pthread_t caller = pthread_self();

    set_mt_config(nullptr);
    demod_mt_destroy(state_key(&key_storage));
    demod_mt_init(state_key(&key_storage));
    demod_mt_run_two(state_key(&key_storage), increment_counter, &arg0, increment_counter, &arg1);

    assert(counter.load(std::memory_order_relaxed) == 2);
    assert(pthread_equal(task0_thread, caller) != 0);
    assert(pthread_equal(task1_thread, caller) != 0);
    demod_mt_destroy(state_key(&key_storage));
}

static void
test_enabled_mode_runs_and_tears_down(void) {
    int key_storage = 0;
    std::atomic<int> counter{0};
    pthread_t task0_thread = {};
    pthread_t task1_thread = {};
    CounterArg arg0{&counter, &task0_thread};
    CounterArg arg1{&counter, &task1_thread};
    pthread_t caller = pthread_self();

    set_mt_config("1");
    demod_mt_destroy(state_key(&key_storage));
    demod_mt_init(state_key(&key_storage));
    demod_mt_init(state_key(&key_storage));

    demod_mt_run_two(state_key(&key_storage), increment_counter, &arg0, nullptr, nullptr);
    assert(counter.load(std::memory_order_relaxed) == 1);
    assert(!pthread_equal(task0_thread, caller));

    demod_mt_run_two(state_key(&key_storage), increment_counter, &arg0, increment_counter, &arg1);
    assert(counter.load(std::memory_order_relaxed) == 3);
    assert(!pthread_equal(task0_thread, caller));
    assert(!pthread_equal(task1_thread, caller));

    demod_mt_destroy(state_key(&key_storage));
    demod_mt_destroy(state_key(&key_storage));

    task0_thread = {};
    task1_thread = {};
    demod_mt_run_two(state_key(&key_storage), increment_counter, &arg0, increment_counter, &arg1);
    assert(counter.load(std::memory_order_relaxed) == 5);
    assert(pthread_equal(task0_thread, caller) != 0);
    assert(pthread_equal(task1_thread, caller) != 0);
}

int
main(void) {
    test_disabled_mode_runs_synchronously();
    test_enabled_mode_runs_and_tears_down();
    unsetenv("DSD_NEO_MT");
    dsd_neo_config_init();
    return 0;
}

// NOLINTEND(misc-use-internal-linkage)
