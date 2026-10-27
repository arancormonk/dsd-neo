// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A SoapySDR receiver that stops delivering, or a reader that ends by itself, ends the session and is reported once.
 * The device's own reader runs against a stand-in driver (the stream seam): its setup, its locked read loop and its
 * read-result handling. Reads that keep timing out or overflowing, or one read that hangs, are silence: once it
 * reaches the limit (5000 ms; here 300 ms where a case must trip and 2000 ms where it must not, sampled every 20 ms)
 * the session ends for its receiver (-ETIMEDOUT), through the unplug-owner grace, inside which samples that resume
 * keep it running and a stop keeps it silent. A control transaction holding the driver lock is not silence. A setup
 * or buffer failure is reported at once as a device failure that is not the receiver's ending; an exception or a
 * fatal result from a read ends the session for its receiver; either wakes the demod waiting on the input ring. A
 * stop, during setup, during the grace or while streaming, is silent, and a monitor that cannot start fails the start.
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
#include <errno.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

struct rtl_device;

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

/* As test_io_usb_progress_monitor.cpp: a case that must trip runs under kLimitMs, one that must not under
 * kSteadyLimitMs, far beyond any time a loaded runner keeps a thread off the CPU; an event that must outlast the
 * steady limit takes kOutlastMs. */
constexpr uint32_t kLimitMs = 300U;
constexpr uint32_t kSteadyLimitMs = 2000U;
constexpr uint32_t kOutlastMs = kSteadyLimitMs + 500U;
constexpr uint32_t kPeriodMs = 20U;
constexpr uint32_t kBufLen = 16384U;

/* What the stand-in driver's reads return. */
enum read_mode : int {
    READ_SAMPLES = 0, /* 256 samples every 5 ms */
    READ_TIMEOUT,     /* a timeout after 10 ms */
    READ_OVERFLOW,    /* an overflow after 5 ms */
    READ_FAIL,        /* a fatal stream error at once */
    READ_THROW,       /* an exception at once */
    READ_HANG,        /* one read that blocks until released (5 s at most), then a timeout */
};

struct fake_soapy {
    std::atomic<int> setup_result{0};
    std::atomic<uint32_t> setup_hold_ms{0U};
    std::atomic<size_t> mtu{256U};
    std::atomic<int> setup_calls{0};
    std::atomic<int> setup_entered{0};
    std::atomic<int> mode{READ_SAMPLES};
    std::atomic<int> reads{0};
    std::atomic<int> sample_reads{0};
    std::atomic<int> hung{0};
    std::atomic<int> release{0};
};

fake_soapy g_fake;

int
fake_setup(void* ctx, size_t* mtu_elems) {
    auto* fake = static_cast<fake_soapy*>(ctx);
    fake->setup_calls.fetch_add(1, std::memory_order_acq_rel);
    fake->setup_entered.store(1, std::memory_order_release);
    const uint32_t hold_ms = fake->setup_hold_ms.load(std::memory_order_acquire);
    if (hold_ms > 0U) {
        dsd_sleep_ms(hold_ms);
    }
    *mtu_elems = fake->mtu.load(std::memory_order_acquire);
    return fake->setup_result.load(std::memory_order_acquire);
}

int
fake_read(void* ctx, void* buff, size_t elems, long timeout_us) {
    auto* fake = static_cast<fake_soapy*>(ctx);
    (void)buff;
    (void)timeout_us;
    fake->reads.fetch_add(1, std::memory_order_acq_rel);
    switch (fake->mode.load(std::memory_order_acquire)) {
        case READ_TIMEOUT: dsd_sleep_ms(10U); return RTL_DEVICE_TEST_SOAPY_TIMEOUT;
        case READ_OVERFLOW: dsd_sleep_ms(5U); return RTL_DEVICE_TEST_SOAPY_OVERFLOW;
        case READ_FAIL: return RTL_DEVICE_TEST_SOAPY_STREAM_ERROR;
        case READ_THROW: return RTL_DEVICE_TEST_SOAPY_THROW;
        case READ_HANG:
            fake->hung.store(1, std::memory_order_release);
            for (int waited_ms = 0; waited_ms < 5000 && !fake->release.load(std::memory_order_acquire); ++waited_ms) {
                dsd_sleep_ms(1U);
            }
            fake->hung.store(0, std::memory_order_release);
            fake->mode.store(READ_TIMEOUT, std::memory_order_release);
            return RTL_DEVICE_TEST_SOAPY_TIMEOUT;
        case READ_SAMPLES:
        default:
            dsd_sleep_ms(5U);
            fake->sample_reads.fetch_add(1, std::memory_order_acq_rel);
            return (int)(elems < 256U ? elems : 256U);
    }
}

rtl_device_test_soapy_driver g_driver = {fake_setup, fake_read, &g_fake, 1};

void
reset_fake(int mode) {
    g_fake.setup_result.store(0);
    g_fake.setup_hold_ms.store(0U);
    g_fake.mtu.store(256U);
    g_fake.setup_calls.store(0);
    g_fake.setup_entered.store(0);
    g_fake.mode.store(mode);
    g_fake.reads.store(0);
    g_fake.sample_reads.store(0);
    g_fake.hung.store(0);
    g_fake.release.store(0);
}

void
reset_session(void) {
    dsd_input_failure_clear();
    dsd_exitflag_store(0);
}

rtl_device*
make_device(input_ring_state* ring) {
    rtl_device* dev = rtl_device_test_soapy_device(ring, &g_driver);
    assert(dev);
    return dev;
}

rtl_device*
start_device(input_ring_state* ring) {
    rtl_device* dev = make_device(ring);
    const int started = rtl_device_start_async(dev, kBufLen);
    assert(started == 0);
    return dev;
}

/* The session's own stop, then the device's end: the monitor and the reader are joined, the stream closed. */
void
end_device(rtl_device* dev) {
    (void)rtl_device_stop_async(dev);
    rtl_device_test_soapy_state state{};
    rtl_device_test_soapy_state_get(dev, &state);
    assert(state.monitor_started == 0);
    assert(state.stream_open == 0);
    rtl_device_test_soapy_device_free(dev);
}

rtl_device_test_soapy_state
device_state(rtl_device* dev) {
    rtl_device_test_soapy_state state{};
    rtl_device_test_soapy_state_get(dev, &state);
    return state;
}

void
wait_reader_exited(rtl_device* dev) {
    for (int waited_ms = 0; waited_ms < 3000 && !device_state(dev).reader_exited; ++waited_ms) {
        dsd_sleep_ms(1U);
    }
    assert(device_state(dev).reader_exited);
}

/* Poll until the receiver's ending is committed: the time it was seen, or 0 when @p timeout_ms passed first. */
uint64_t
wait_receiver_ended(uint64_t timeout_ms) {
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + timeout_ms;
    while (dsd_realtime_mono_ms() < deadline_ms) {
        if (dsd_input_failure_receiver_ended()) {
            return dsd_realtime_mono_ms();
        }
        dsd_sleep_ms(2U);
    }
    return 0U;
}

/* Poll until the failure latch moved past @p generation: the time it was seen, or 0 after @p timeout_ms. */
uint64_t
wait_reported(unsigned int generation, uint64_t timeout_ms) {
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + timeout_ms;
    while (dsd_realtime_mono_ms() < deadline_ms) {
        if (dsd_input_failure_generation() != generation) {
            return dsd_realtime_mono_ms();
        }
        dsd_sleep_ms(2U);
    }
    return 0U;
}

/* The session was reported once as a device failure with @p native_code, the exit flag raised, and the receiver's
 * ending committed as @p receiver_ended says. */
void
expect_reported_once(unsigned int generation, int native_code, int receiver_ended) {
    assert(dsd_input_failure_generation() == generation + 1U);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    assert(failure.native_code == native_code);
    assert(dsd_exitflag_load() == 1);
    assert(dsd_input_failure_receiver_ended() == receiver_ended);
}

/* Nothing reported since @p generation and nothing ended the session. */
void
expect_running(unsigned int generation) {
    assert(dsd_input_failure_generation() == generation);
    assert(dsd_exitflag_load() == 0);
    assert(dsd_input_failure_receiver_ended() == 0);
}

/* Wait until the verdict after @p in_grace began waiting out the unplug-owner grace, however late its thread runs. */
void
wait_verdict_in_grace(unsigned int in_grace) {
    for (int waited_ms = 0; waited_ms < 5000 && rtl_device_test_verdicts_in_grace() == in_grace; ++waited_ms) {
        dsd_sleep_ms(1U);
    }
    assert(rtl_device_test_verdicts_in_grace() == in_grace + 1U);
}

/* A demod waiting on the input ring's ready condition for samples, until a broadcast finds the exit flag set (5 s at
 * most). */
struct ring_waiter {
    input_ring_state* ring = nullptr;
    std::atomic<int> waiting{0};
    std::atomic<int> woken{0};
    dsd_thread_t thread{};
};

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
start_waiter(ring_waiter* waiter, input_ring_state* ring) {
    waiter->ring = ring;
    const int created = dsd_thread_create(&waiter->thread, wait_for_ring_wake, waiter);
    assert(created == 0);
    while (!waiter->waiting.load(std::memory_order_acquire)) {
        dsd_sleep_ms(1U);
    }
    /* The waiter holds the lock until its wait releases it. */
    dsd_mutex_lock(&ring->ready_m);
    dsd_mutex_unlock(&ring->ready_m);
}

void
expect_waiter_woken(ring_waiter* waiter) {
    dsd_thread_join(waiter->thread);
    assert(waiter->woken.load(std::memory_order_acquire) == 1);
}

/* A silence verdict of reads that keep returning @p mode: reported once with -ETIMEDOUT and the receiver's ending,
 * from the limit on, waking the demod; the reader then leaves by itself. */
void
expect_silence_ends_session(input_ring_state* ring, int mode) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    reset_fake(READ_SAMPLES);
    const unsigned int generation = dsd_input_failure_generation();
    ring_waiter waiter;
    start_waiter(&waiter, ring);
    rtl_device* dev = start_device(ring);
    while (g_fake.sample_reads.load() < 5) {
        dsd_sleep_ms(1U);
    }
    g_fake.mode.store(mode);
    const uint64_t silent_ms = dsd_realtime_mono_ms();
    const uint64_t ended_ms = wait_receiver_ended(3000U);
    assert(ended_ms != 0U);
    assert(ended_ms - silent_ms >= kLimitMs - kPeriodMs);
    assert(ended_ms - silent_ms < 1500U);
    expect_reported_once(generation, -ETIMEDOUT, 1);
    expect_waiter_woken(&waiter);
    wait_reader_exited(dev);
    dsd_sleep_ms(100U);
    expect_reported_once(generation, -ETIMEDOUT, 1);
    end_device(dev);
    expect_reported_once(generation, -ETIMEDOUT, 1);
}

void
timeouts_past_limit_end_session(input_ring_state* ring) {
    expect_silence_ends_session(ring, READ_TIMEOUT);
}

void
overflows_past_limit_end_session(input_ring_state* ring) {
    expect_silence_ends_session(ring, READ_OVERFLOW);
}

/* The verdict waits out the unplug-owner grace on the monitor while the reader keeps reading: samples that resume
 * inside it cancel the verdict, and the session runs on. */
void
resumed_samples_during_grace_cancel(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    rtl_device_set_unplug_owner_grace_ms(1000U);
    reset_session();
    reset_fake(READ_TIMEOUT);
    const unsigned int generation = dsd_input_failure_generation();
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    rtl_device* dev = start_device(ring);
    wait_verdict_in_grace(in_grace);
    g_fake.mode.store(READ_SAMPLES);
    const int before = g_fake.sample_reads.load();
    dsd_sleep_ms(1500U);
    assert(g_fake.sample_reads.load() > before);
    expect_running(generation);
    end_device(dev);
    expect_running(generation);
    rtl_device_set_unplug_owner_grace_ms(0U);
}

/* A control transaction holding the driver lock for longer than the limit keeps the reader from reading: that time is
 * the control's, not silence. */
void
long_control_hold_does_not_accrue(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    reset_session();
    reset_fake(READ_SAMPLES);
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device* dev = start_device(ring);
    while (g_fake.sample_reads.load() < 5) {
        dsd_sleep_ms(1U);
    }
    const int held = rtl_device_test_soapy_control_hold(dev, kOutlastMs);
    assert(held == 0);
    const int after_hold = g_fake.sample_reads.load();
    dsd_sleep_ms(300U);
    assert(g_fake.sample_reads.load() > after_hold);
    expect_running(generation);
    end_device(dev);
    expect_running(generation);
}

/* A control waiting for the driver lock behind a hung read. */
struct control_waiter {
    rtl_device* dev = nullptr;
    std::atomic<int> done{0};
    dsd_thread_t thread{};
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
run_control(void* arg) {
    auto* control = static_cast<control_waiter*>(arg);
    (void)rtl_device_test_soapy_control_hold(control->dev, 10U);
    control->done.store(1, std::memory_order_release);
    DSD_THREAD_RETURN;
}

/* A read that hangs holds the driver lock as a control does, and still accrues silence, however long a control waits
 * behind it: the session ends while the read is still hung. Released, the read returns and the reader leaves. */
void
timeouts_do_not_pause_accrual(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    reset_fake(READ_HANG);
    const unsigned int generation = dsd_input_failure_generation();
    const uint64_t started_ms = dsd_realtime_mono_ms();
    rtl_device* dev = start_device(ring);
    while (!g_fake.hung.load()) {
        dsd_sleep_ms(1U);
    }
    control_waiter control;
    control.dev = dev;
    const int created = dsd_thread_create(&control.thread, run_control, &control);
    assert(created == 0);
    const uint64_t ended_ms = wait_receiver_ended(3000U);
    assert(ended_ms != 0U);
    assert(ended_ms - started_ms >= kLimitMs - kPeriodMs);
    assert(g_fake.hung.load() == 1);
    assert(control.done.load() == 0);
    expect_reported_once(generation, -ETIMEDOUT, 1);
    g_fake.release.store(1);
    dsd_thread_join(control.thread);
    wait_reader_exited(dev);
    end_device(dev);
    expect_reported_once(generation, -ETIMEDOUT, 1);
}

/* A start that fails in the reader, with @p setup_result from the setup (and @p mtu elements per read): reported at
 * once as a device failure with @p native_code, even under an unplug-owner grace, without the receiver's ending, and
 * the demod waiting on the input ring woken. Nothing is read. */
void
expect_setup_failure(input_ring_state* ring, int setup_result, size_t mtu, int native_code) {
    rtl_device_set_unplug_owner_grace_ms(kSteadyLimitMs);
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    reset_fake(READ_SAMPLES);
    g_fake.setup_result.store(setup_result);
    g_fake.mtu.store(mtu);
    const unsigned int generation = dsd_input_failure_generation();
    ring_waiter waiter;
    start_waiter(&waiter, ring);
    const uint64_t started_ms = dsd_realtime_mono_ms();
    rtl_device* dev = start_device(ring);
    const uint64_t reported_ms = wait_reported(generation, 3000U);
    assert(reported_ms != 0U);
    assert(reported_ms - started_ms < 1000U);
    expect_reported_once(generation, native_code, 0);
    expect_waiter_woken(&waiter);
    wait_reader_exited(dev);
    dsd_sleep_ms(2U * kLimitMs);
    expect_reported_once(generation, native_code, 0);
    assert(g_fake.reads.load() == 0);
    end_device(dev);
    expect_reported_once(generation, native_code, 0);
    rtl_device_set_unplug_owner_grace_ms(0U);
}

void
setup_failure_reports_once_and_wakes_decoder(input_ring_state* ring) {
    expect_setup_failure(ring, RTL_DEVICE_TEST_SOAPY_STREAM_ERROR, 256U, RTL_DEVICE_TEST_SOAPY_STREAM_ERROR);
    expect_setup_failure(ring, RTL_DEVICE_TEST_SOAPY_THROW, 256U, -EIO);
    /* A buffer no allocator can give: the reader's own allocation fails. */
    expect_setup_failure(ring, 0, SIZE_MAX, -ENOMEM);
}

/* A reader that leaves its loop with the session running: reported once, for its receiver, with @p native_code, and
 * the demod waiting on the input ring woken. */
void
expect_read_failure(input_ring_state* ring, int mode, int native_code) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    reset_session();
    reset_fake(READ_SAMPLES);
    const unsigned int generation = dsd_input_failure_generation();
    ring_waiter waiter;
    start_waiter(&waiter, ring);
    rtl_device* dev = start_device(ring);
    while (g_fake.sample_reads.load() < 5) {
        dsd_sleep_ms(1U);
    }
    g_fake.mode.store(mode);
    const uint64_t ended = wait_receiver_ended(3000U);
    assert(ended != 0U);
    expect_reported_once(generation, native_code, 1);
    expect_waiter_woken(&waiter);
    wait_reader_exited(dev);
    dsd_sleep_ms(2U * kLimitMs);
    expect_reported_once(generation, native_code, 1);
    end_device(dev);
    expect_reported_once(generation, native_code, 1);
}

void
exception_reports_once(input_ring_state* ring) {
    expect_read_failure(ring, READ_THROW, -EIO);
}

void
fatal_read_reports_once(input_ring_state* ring) {
    expect_read_failure(ring, READ_FAIL, RTL_DEVICE_TEST_SOAPY_STREAM_ERROR);
}

/* Without its monitor a receiver that stops would go unnoticed: the start fails, and nothing is set up or reported. */
void
monitor_create_failure_fails_start(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    reset_fake(READ_SAMPLES);
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device* dev = make_device(ring);
    rtl_device_test_refuse_progress_monitor(1);
    const int started = rtl_device_start_async(dev, kBufLen);
    assert(started != 0);
    rtl_device_test_refuse_progress_monitor(0);
    dsd_sleep_ms(100U);
    assert(g_fake.setup_calls.load() == 0);
    assert(device_state(dev).monitor_started == 0);
    expect_running(generation);
    rtl_device_test_soapy_device_free(dev);
    expect_running(generation);
}

/* A stop while the reader sets the stream up waits for the setup, and the reader never reads: whether the setup then
 * succeeds or fails, nothing is reported, and the stream it opened is closed. */
void
expect_stop_during_setup(input_ring_state* ring, int setup_result) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    reset_fake(READ_SAMPLES);
    g_fake.setup_hold_ms.store(kLimitMs);
    g_fake.setup_result.store(setup_result);
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device* dev = start_device(ring);
    while (!g_fake.setup_entered.load()) {
        dsd_sleep_ms(1U);
    }
    const int stopped = rtl_device_stop_async(dev);
    assert(stopped == 0);
    const rtl_device_test_soapy_state state = device_state(dev);
    assert(state.reader_exited == 1);
    assert(state.monitor_started == 0);
    assert(state.stream_open == 0);
    assert(state.stream_closes == (setup_result == 0 ? 1U : 0U));
    assert(g_fake.reads.load() == 0);
    dsd_sleep_ms(2U * kLimitMs);
    expect_running(generation);
    rtl_device_test_soapy_device_free(dev);
    expect_running(generation);
}

void
stop_during_setup(input_ring_state* ring) {
    expect_stop_during_setup(ring, 0);
    expect_stop_during_setup(ring, RTL_DEVICE_TEST_SOAPY_STREAM_ERROR);
}

/* A stop while the silence verdict waits out the unplug-owner grace keeps the session silent and joins the monitor at
 * once. */
void
stop_during_grace(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    rtl_device_set_unplug_owner_grace_ms(kSteadyLimitMs);
    reset_session();
    reset_fake(READ_TIMEOUT);
    const unsigned int generation = dsd_input_failure_generation();
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    rtl_device* dev = start_device(ring);
    wait_verdict_in_grace(in_grace);
    expect_running(generation);
    const uint64_t stop_ms = dsd_realtime_mono_ms();
    end_device(dev);
    assert(dsd_realtime_mono_ms() - stop_ms < 500U);
    dsd_sleep_ms(100U);
    expect_running(generation);
    rtl_device_set_unplug_owner_grace_ms(0U);
}

/* The session's own stop, or a destroy with no stop first, ends a streaming or timing-out reader silently. */
void
intentional_stop_silent(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    reset_session();
    reset_fake(READ_SAMPLES);
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device* dev = start_device(ring);
    while (g_fake.sample_reads.load() < 5) {
        dsd_sleep_ms(1U);
    }
    assert(device_state(dev).reader_entered == 1);
    const int stopped = rtl_device_stop_async(dev);
    assert(stopped == 0);
    rtl_device_test_soapy_state state = device_state(dev);
    assert(state.reader_exited == 1);
    assert(state.monitor_started == 0);
    assert(state.stream_open == 0);
    assert(state.stream_closes == 1U);
    rtl_device_test_soapy_device_free(dev);
    expect_running(generation);

    reset_fake(READ_TIMEOUT);
    dev = start_device(ring);
    while (g_fake.reads.load() < 5) {
        dsd_sleep_ms(1U);
    }
    rtl_device_test_soapy_device_free(dev);
    dsd_sleep_ms(100U);
    expect_running(generation);
}

void
internal_read_failure_is_not_receiver_loss(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    rtl_device_set_unplug_owner_grace_ms(kSteadyLimitMs);
    reset_session();
    reset_fake(READ_SAMPLES);
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device_test_soapy_internal_read_failure(1);
    const uint64_t start = dsd_realtime_mono_ms();
    rtl_device* dev = start_device(ring);
    const uint64_t reported = wait_reported(generation, 3000U);
    assert(reported > 0U && reported - start < 1000U);
    expect_reported_once(generation, -EIO, 0);
    end_device(dev);
    rtl_device_test_soapy_internal_read_failure(0);
    rtl_device_set_unplug_owner_grace_ms(0U);
}

void
restart_clears_stream_counters(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    reset_session();
    reset_fake(READ_TIMEOUT);
    rtl_device* dev = start_device(ring);
    for (int i = 0; i < 1000 && g_fake.reads.load() < 6; i++) {
        dsd_sleep_ms(1);
    }
    const int timeout_reads = g_fake.reads.load();
    g_fake.mode.store(READ_OVERFLOW);
    for (int i = 0; i < 1000 && g_fake.reads.load() < timeout_reads + 7; i++) {
        dsd_sleep_ms(1);
    }
    const int stopped = rtl_device_stop_async(dev);
    assert(stopped == 0);
    const auto before = device_state(dev);
    assert(before.timeouts >= 5U && before.overflows >= 5U);
    reset_fake(READ_SAMPLES);
    const int started = rtl_device_start_async(dev, kBufLen);
    assert(started == 0);
    for (int i = 0; i < 1000 && g_fake.sample_reads.load() < 5; i++) {
        dsd_sleep_ms(1);
    }
    assert(g_fake.sample_reads.load() >= 5);
    const int stopped_again = rtl_device_stop_async(dev);
    assert(stopped_again == 0);
    const auto after = device_state(dev);
    assert(after.timeouts == 0U && after.overflows == 0U);
    end_device(dev);
}

void
reader_create_failure_joins_monitor(input_ring_state* ring) {
    reset_session();
    reset_fake(READ_SAMPLES);
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device* dev = make_device(ring);
    rtl_device_test_soapy_refuse_reader(1);
    const int started = rtl_device_start_async(dev, kBufLen);
    rtl_device_test_soapy_refuse_reader(0);
    assert(started != 0);
    const auto state = device_state(dev);
    assert(state.monitor_started == 0 && state.reader_entered == 0 && state.stream_open == 0);
    assert(g_fake.setup_calls.load() == 0);
    expect_running(generation);
    end_device(dev);
}

void
exception_during_monitor_grace_reports_once(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    rtl_device_set_unplug_owner_grace_ms(kLimitMs);
    reset_session();
    reset_fake(READ_TIMEOUT);
    const unsigned int generation = dsd_input_failure_generation();
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    rtl_device* dev = start_device(ring);
    wait_verdict_in_grace(in_grace);
    g_fake.mode.store(READ_THROW);
    const uint64_t ended = wait_receiver_ended(3000U);
    assert(ended != 0U);
    end_device(dev);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.native_code == -ETIMEDOUT || failure.native_code == -EIO);
    expect_reported_once(generation, failure.native_code, 1);
    rtl_device_set_unplug_owner_grace_ms(0U);
}

struct test_case {
    const char* name;
    void (*run)(input_ring_state* ring);
};

const test_case kCases[] = {
    {"internal_read_failure_is_not_receiver_loss", internal_read_failure_is_not_receiver_loss},
    {"restart_clears_stream_counters", restart_clears_stream_counters},
    {"reader_create_failure_joins_monitor", reader_create_failure_joins_monitor},
    {"exception_during_monitor_grace_reports_once", exception_during_monitor_grace_reports_once},
    {"timeouts_past_limit_end_session", timeouts_past_limit_end_session},
    {"overflows_past_limit_end_session", overflows_past_limit_end_session},
    {"resumed_samples_during_grace_cancel", resumed_samples_during_grace_cancel},
    {"long_control_hold_does_not_accrue", long_control_hold_does_not_accrue},
    {"timeouts_do_not_pause_accrual", timeouts_do_not_pause_accrual},
    {"setup_failure_reports_once_and_wakes_decoder", setup_failure_reports_once_and_wakes_decoder},
    {"exception_reports_once", exception_reports_once},
    {"fatal_read_reports_once", fatal_read_reports_once},
    {"monitor_create_failure_fails_start", monitor_create_failure_fails_start},
    {"stop_during_setup", stop_during_setup},
    {"stop_during_grace", stop_during_grace},
    {"intentional_stop_silent", intentional_stop_silent},
};

} // namespace

/* With no argument every case runs; with one, only the case of that name. */
int
main(int argc, char** argv) {
    auto ring = std::make_unique<input_ring_state>();
    const int initialized = input_ring_init(ring.get(), 1U << 16);
    assert(initialized == 0);
    rtl_device* probe = rtl_device_test_soapy_device(ring.get(), &g_driver);
    if (!probe) {
        DSD_FPRINTF(stderr, "SKIP: SoapySDR stream loss: no SoapySDR in this build\n");
        input_ring_destroy(ring.get());
        return 0;
    }
    rtl_device_test_soapy_device_free(probe);
    rtl_device_test_set_progress_period_ms(kPeriodMs);

    int ran = 0;
    for (const test_case& c : kCases) {
        if (argc > 1 && strcmp(argv[1], c.name) != 0) {
            continue;
        }
        DSD_FPRINTF(stderr, "case %s\n", c.name);
        c.run(ring.get());
        ran++;
    }
    assert(ran > 0);

    rtl_device_test_set_progress_limit_ms(0U);
    rtl_device_test_set_progress_period_ms(0U);
    reset_session();
    input_ring_destroy(ring.get());
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
