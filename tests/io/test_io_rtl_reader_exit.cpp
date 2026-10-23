// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The RTL-SDR USB reader's return code: a reader that could not allocate its USB transfers (-ENOMEM), or on a
 * desktop build lost its device (-1), ends the session as a device failure of the input and wakes the demod waiting on
 * the input ring, unless a stop already asked the reader to end. (On Android a lost device stays with the USB detach
 * broadcast; this host test covers the desktop rule.)
 */

#include <assert.h>
#include <atomic>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <errno.h>
#include <memory>
#include <stdint.h>
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

void
reset_session(void) {
    dsd_input_failure_clear();
    dsd_exitflag_store(0);
}

int
run_reader(input_ring_state* ring, int reader_rc, int stop_while_reading) {
    rtl_device_test_usb_reader_request request{};
    request.ring = ring;
    request.reader_rc = reader_rc;
    request.stop_while_reading = stop_while_reading;
    return rtl_device_test_usb_reader_exit(&request);
}

/* A reader whose return must leave the session alone: nothing reported, the exit flag as it was. */
void
expect_no_report(input_ring_state* ring, int reader_rc, int stop_while_reading, int exit_flag_before) {
    reset_session();
    dsd_exitflag_store((uint8_t)exit_flag_before);
    const unsigned int generation = dsd_input_failure_generation();
    assert(run_reader(ring, reader_rc, stop_while_reading) == 0);
    assert(dsd_input_failure_generation() == generation);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_NONE);
    assert(dsd_exitflag_load() == exit_flag_before);
}

/* A reader whose return is a device failure: reported with its code, the session ended, and a demod waiting on the
 * input ring woken. */
void
expect_report(input_ring_state* ring, int reader_rc) {
    reset_session();
    ring_waiter waiter;
    waiter.ring = ring;
    dsd_thread_t waiter_thread;
    assert(dsd_thread_create(&waiter_thread, wait_for_ring_wake, &waiter) == 0);
    while (!waiter.waiting.load(std::memory_order_acquire)) {
        dsd_sleep_ms(1);
    }
    /* The waiter holds the lock until its wait releases it. */
    dsd_mutex_lock(&ring->ready_m);
    dsd_mutex_unlock(&ring->ready_m);
    assert(run_reader(ring, reader_rc, 0) == 0);
    dsd_thread_join(waiter_thread);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    assert(failure.native_code == reader_rc);
    assert(dsd_exitflag_load() == 1);
    assert(waiter.woken.load(std::memory_order_acquire) == 1);
}

} // namespace

int
main(void) {
    auto ring = std::make_unique<input_ring_state>();
    assert(input_ring_init(ring.get(), 4096U) == 0);

    /* Transfers that could not be allocated, or a lost device on a desktop build, with no stop requested: a device
     * failure that ends the session and wakes the demod. */
    expect_report(ring.get(), -ENOMEM);
    expect_report(ring.get(), -1);

    /* The same failure once a stop asked the reader to end, or once the session is already ending: nothing new. */
    expect_no_report(ring.get(), -ENOMEM, 1, 0);
    expect_no_report(ring.get(), -ENOMEM, 0, 1);
    expect_no_report(ring.get(), -1, 1, 0);

    /* A cancelled reader is not reported. */
    expect_no_report(ring.get(), 0, 1, 0);

    reset_session();
    input_ring_destroy(ring.get());
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
