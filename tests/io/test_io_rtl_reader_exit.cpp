// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The RTL-SDR USB reader's return code: any return while the session still runs and no stop asked the reader to end
 * (-ENOMEM, a lost device's -1, a failed submission's -5, even 0) ends the session as a device failure of the input
 * and wakes the demod waiting on the input ring; a return a stop asked for is left alone. Every return but -ENOMEM
 * also commits the "receiver ended" fact, after the unplug-owner grace: while one is set (Android sets 2000 ms so the
 * USB detach broadcast can own an unplug), a stop or an exit request inside the grace keeps the return silent. The
 * cause is logged before the failure is reported. And one stop handshake: a stop or destroy that lands before the
 * reader streams still ends it and joins, and a reader that a stop beat to its run check never streams.
 */

#include <assert.h>
#include <atomic>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <dsd-neo/runtime/log.h>
#include <errno.h>
#include <memory>
#include <stdint.h>
#include <string.h>
#include "rtl_stream_test_support.h"

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

struct ring_waiter {
    input_ring_state* ring = nullptr;
    std::atomic<int> waiting{0};
    std::atomic<int> woken{0};
};

/* Wait on the ring's ready condition, as the demod does for samples, until a broadcast finds the exit flag set; give
 * up after 5 s without one. */
DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
wait_for_ring_wake(void* arg) {
    auto* waiter = static_cast<ring_waiter*>(arg);
    dsd_mutex_lock(&waiter->ring->ready_m);
    waiter->waiting.store(1, std::memory_order_release);
    for (;;) {
        const int rc = dsd_cond_timedwait(&waiter->ring->ready, &waiter->ring->ready_m, 5000U);
        if (rc != 0) {
            break;
        }
        if (dsd_exitflag_load() != 0) {
            waiter->woken.store(1, std::memory_order_release);
            break;
        }
    }
    dsd_mutex_unlock(&waiter->ring->ready_m);
    DSD_THREAD_RETURN;
}

/* What the session looked like when the reader's "device was lost" line was logged. */
struct cause_log {
    std::atomic<int> logged{0};
    std::atomic<unsigned int> generation{0U};
    std::atomic<int> exit_flag{0};
};

cause_log g_cause_log;

void
record_cause_log(dsd_neo_log_level_t level, const char* text, void* ctx) {
    auto* log = static_cast<cause_log*>(ctx);
    if (level != LOG_LEVEL_ERROR || !text || !strstr(text, "the USB device was lost")) {
        return;
    }
    log->generation.store(dsd_input_failure_generation(), std::memory_order_relaxed);
    log->exit_flag.store(dsd_exitflag_load(), std::memory_order_relaxed);
    log->logged.store(1, std::memory_order_release);
}

/* Raise the exit flag after 100 ms, as a session ending for another reason does. */
DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
raise_exit_flag_later(void*) {
    dsd_sleep_ms(100U);
    dsd_exitflag_store(1);
    DSD_THREAD_RETURN;
}

void
reset_session(void) {
    dsd_input_failure_clear();
    dsd_exitflag_store(0);
}

int
run_request(rtl_device_test_usb_reader_request& request) {
    return rtl_device_test_usb_reader_exit(&request);
}

int
run_reader(input_ring_state* ring, int reader_rc, int stop_while_reading) {
    rtl_device_test_usb_reader_request request{};
    request.ring = ring;
    request.reader_rc = reader_rc;
    request.stop_while_reading = stop_while_reading;
    return run_request(request);
}

/* Nothing reported, the exit flag as it was, and no receiver ending committed. */
void
expect_session_untouched(unsigned int generation, int exit_flag_before) {
    assert(dsd_input_failure_generation() == generation);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_NONE);
    assert(dsd_exitflag_load() == exit_flag_before);
    assert(dsd_input_failure_receiver_ended() == 0);
}

/* A reader whose return must leave the session alone. */
void
expect_no_report(input_ring_state* ring, int reader_rc, int stop_while_reading, int exit_flag_before) {
    reset_session();
    dsd_exitflag_store((uint8_t)exit_flag_before);
    const unsigned int generation = dsd_input_failure_generation();
    assert(run_reader(ring, reader_rc, stop_while_reading) == 0);
    expect_session_untouched(generation, exit_flag_before);
}

/* Run @p request with a demod waiting on the input ring; returns how long the device took from start to join. */
uint64_t
run_with_waiter(input_ring_state* ring, rtl_device_test_usb_reader_request& request, ring_waiter* waiter) {
    waiter->ring = ring;
    dsd_thread_t waiter_thread;
    const int created = dsd_thread_create(&waiter_thread, wait_for_ring_wake, waiter);
    assert(created == 0);
    while (!waiter->waiting.load(std::memory_order_acquire)) {
        dsd_sleep_ms(1);
    }
    /* The waiter holds the lock until its wait releases it. */
    dsd_mutex_lock(&ring->ready_m);
    dsd_mutex_unlock(&ring->ready_m);
    const uint64_t started_ms = dsd_realtime_mono_ms();
    assert(run_request(request) == 0);
    const uint64_t elapsed_ms = dsd_realtime_mono_ms() - started_ms;
    dsd_thread_join(waiter_thread);
    return elapsed_ms;
}

/* A reader whose return is a device failure: reported with its code, the session ended, a demod waiting on the input
 * ring woken, and the receiver ending committed unless @p receiver_ended says the return was not the receiver's.
 * Returns how long the device took from start to join. */
uint64_t
expect_report(input_ring_state* ring, int reader_rc, int receiver_ended) {
    reset_session();
    rtl_device_test_usb_reader_request request{};
    request.ring = ring;
    request.reader_rc = reader_rc;
    ring_waiter waiter;
    const uint64_t elapsed_ms = run_with_waiter(ring, request, &waiter);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    assert(failure.native_code == reader_rc);
    assert(dsd_exitflag_load() == 1);
    assert(waiter.woken.load(std::memory_order_acquire) == 1);
    assert(dsd_input_failure_receiver_ended() == receiver_ended);
    return elapsed_ms;
}

/* The unplug-owner grace: a stop or an exit request inside it keeps a lost device silent; without either the failure
 * is reported once the grace ran out; an allocation failure is not the receiver's and is reported at once. */
void
test_unplug_owner_grace(input_ring_state* ring) {
    assert(rtl_device_unplug_owner_grace_ms() == 0U);
    /* Long enough that a stop 100 ms in lands inside it however late the test runs. */
    rtl_device_set_unplug_owner_grace_ms(1000U);
    assert(rtl_device_unplug_owner_grace_ms() == 1000U);

    /* The device is lost, and the session's own stop (as the detach broadcast's) comes 100 ms later. */
    reset_session();
    rtl_device_test_usb_reader_request request{};
    request.ring = ring;
    request.reader_rc = -1;
    request.stop_after_ms = 100;
    unsigned int generation = dsd_input_failure_generation();
    assert(run_request(request) == 0);
    expect_session_untouched(generation, 0);

    /* The device is lost and nothing stops it, but the session starts ending for another reason 100 ms into the grace:
     * the verdict notices at once rather than at the grace's end, and stays silent. */
    reset_session();
    rtl_device_test_usb_reader_request ending{};
    ending.ring = ring;
    ending.reader_rc = -1;
    generation = dsd_input_failure_generation();
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    dsd_thread_t raiser;
    const int raiser_created = dsd_thread_create(&raiser, raise_exit_flag_later, nullptr);
    assert(raiser_created == 0);
    const uint64_t ending_started_ms = dsd_realtime_mono_ms();
    assert(run_request(ending) == 0);
    assert(dsd_realtime_mono_ms() - ending_started_ms < 800U);
    dsd_thread_join(raiser);
    assert(rtl_device_test_verdicts_in_grace() == in_grace + 1U);
    expect_session_untouched(generation, 1);

    /* No stop: the session ends only once the grace ran out. */
    rtl_device_set_unplug_owner_grace_ms(300U);
    assert(expect_report(ring, -1, 1) >= 300U);

    /* A session already ending when the reader returns is not reported (the grace is never entered). */
    expect_no_report(ring, -1, 0, 1);

    /* An allocation failure does not wait out the grace, however long. */
    rtl_device_set_unplug_owner_grace_ms(3000U);
    assert(expect_report(ring, -ENOMEM, 0) < 1500U);

    rtl_device_set_unplug_owner_grace_ms(0U);
    assert(rtl_device_unplug_owner_grace_ms() == 0U);
}

/* A stop (or a destroy) that lands while the reader thread is starting: held before the reader announced itself, or
 * after it checked that the session runs but before it streams. Either way the reader ends, the stop joins it within
 * a second, and nothing is reported. Held before its run check, the reader never streams for the stop that came
 * first; held after it, it streams until the stop's next cancel. */
void
expect_stop_while_starting(input_ring_state* ring, int hold_before_entry, int destroy) {
    reset_session();
    rtl_device_test_usb_reader_request request{};
    request.ring = ring;
    request.stop_before_reader_entry = hold_before_entry;
    request.stop_before_read_async = !hold_before_entry;
    request.destroy_instead_of_stop = destroy;
    const unsigned int generation = dsd_input_failure_generation();
    const uint64_t started_ms = dsd_realtime_mono_ms();
    assert(run_request(request) == 0);
    assert(dsd_realtime_mono_ms() - started_ms < 1000U);
    expect_session_untouched(generation, 0);
    assert(request.reader_streamed == !hold_before_entry);
}

/* The reader's cause line is logged before the failure is reported, so it reads ahead of the teardown it starts. */
void
test_cause_logged_before_report(input_ring_state* ring) {
    g_cause_log.logged.store(0, std::memory_order_relaxed);
    reset_session();
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device_test_usb_reader_request request{};
    request.ring = ring;
    request.reader_rc = -1;
    assert(run_request(request) == 0);
    assert(dsd_input_failure_receiver_ended() == 1);
    assert(g_cause_log.logged.load(std::memory_order_acquire) == 1);
    assert(g_cause_log.generation.load(std::memory_order_relaxed) == generation);
    assert(g_cause_log.exit_flag.load(std::memory_order_relaxed) == 0);
    assert(dsd_input_failure_generation() == generation + 1U);
}

} // namespace

int
main(void) {
    auto ring = std::make_unique<input_ring_state>();
    assert(input_ring_init(ring.get(), 4096U) == 0);
    dsd_neo_log_set_tap(record_cause_log, &g_cause_log);

    /* Transfers that could not be allocated, a lost device, a failed submission, or a reader that returned with
     * nothing asking it to: a device failure that ends the session and wakes the demod. Only an allocation failure,
     * which is the host's, leaves the receiver ending uncommitted. */
    expect_report(ring.get(), -ENOMEM, 0);
    expect_report(ring.get(), -1, 1);
    expect_report(ring.get(), -5, 1);
    expect_report(ring.get(), 0, 1);

    /* The same returns once a stop asked the reader to end, or once the session is already ending: nothing new. */
    expect_no_report(ring.get(), -ENOMEM, 1, 0);
    expect_no_report(ring.get(), -ENOMEM, 0, 1);
    expect_no_report(ring.get(), -1, 1, 0);
    expect_no_report(ring.get(), -5, 1, 0);
    expect_no_report(ring.get(), -1, 0, 1);

    /* A cancelled reader is not reported. */
    expect_no_report(ring.get(), 0, 1, 0);

    test_unplug_owner_grace(ring.get());
    test_cause_logged_before_report(ring.get());

    /* A stop, or a destroy, between the reader thread's start and its streaming. */
    expect_stop_while_starting(ring.get(), 1, 0);
    expect_stop_while_starting(ring.get(), 0, 0);
    expect_stop_while_starting(ring.get(), 1, 1);
    expect_stop_while_starting(ring.get(), 0, 1);

    reset_session();
    input_ring_destroy(ring.get());
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
