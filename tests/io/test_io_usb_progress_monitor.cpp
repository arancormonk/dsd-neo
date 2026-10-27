// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The USB sample-progress monitor: a receiver that stays enumerated but stops delivering samples ends the session as a
 * device failure (-ETIMEDOUT), and commits the receiver's ending, once its silence reaches the limit (5000 ms; here
 * 300 ms where a case must trip and 2000 ms where it must not, sampled every 20 ms). Only samples end a silence.
 * RTL-SDR: a reader that never calls back, or stops calling back, trips it; a steady stream never does, a stop inside
 * the unplug-owner grace keeps it silent, a reader exit that overlaps the monitor's verdict, or races it, is reported
 * once, and a monitor that cannot start fails the start.
 * Airspy: retunes that deliver nothing still accrue silence, while a stream that keeps delivering never trips, however
 * often it is retuned, however long a retune or a control change takes, or when a retune fails and rolls back. The
 * "stream stopped" verdict waits out the grace and is dropped only by samples, not by a restart alone.
 */

#include <assert.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <errno.h>
#include <memory>
#include <stddef.h>
#include <stdint.h>
#include "rtl_stream_test_support.h"

#ifdef DSD_TEST_USB_PROGRESS_AIRSPY
#include <atomic>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include "airspy_sdk_double.h"
#endif

struct rtl_device;

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

/* The silence limit of a case that must trip. A case that must not trip runs under kSteadyLimitMs, and so long is the
 * grace that a test step must land inside: seconds, far beyond any time a loaded runner keeps a thread off the CPU, so
 * a starved feeder or an overslept test step cannot pass for silence or outlast a grace. An event that must outlast
 * the steady limit (a slow retune or control change) takes kOutlastMs. */
constexpr uint32_t kLimitMs = 300U;
constexpr uint32_t kSteadyLimitMs = 2000U;
constexpr uint32_t kOutlastMs = kSteadyLimitMs + 500U;
constexpr uint32_t kPeriodMs = 20U;

void
reset_session(void) {
    dsd_input_failure_clear();
    dsd_exitflag_store(0);
}

/* Poll until the receiver's ending is committed: the time it was seen, or 0 when @p timeout_ms passed first. */
uint64_t
wait_receiver_ended(uint64_t timeout_ms) {
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + timeout_ms;
    while (dsd_realtime_mono_ms() < deadline_ms) {
        if (dsd_input_failure_receiver_ended()) {
            return dsd_realtime_mono_ms();
        }
        dsd_sleep_ms(2);
    }
    return 0U;
}

/* The session ended for its receiver with @p native_code. */
void
expect_receiver_ending(int native_code) {
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_DEVICE);
    assert(failure.native_code == native_code);
    assert(dsd_exitflag_load() == 1);
    assert(dsd_input_failure_receiver_ended() == 1);
}

/* The session still runs: nothing ended it and no receiver ending was committed. */
void
expect_running(void) {
    assert(dsd_exitflag_load() == 0);
    assert(dsd_input_failure_receiver_ended() == 0);
}

/* Wait until the verdict after @p in_grace began waiting out the unplug-owner grace, however late its thread runs. */
void
wait_verdict_in_grace(unsigned int in_grace) {
    for (int waited_ms = 0; waited_ms < 5000 && rtl_device_test_verdicts_in_grace() == in_grace; ++waited_ms) {
        dsd_sleep_ms(1);
    }
    assert(rtl_device_test_verdicts_in_grace() == in_grace + 1U);
}

/* ---- RTL-SDR ---- */

unsigned char g_rtl_buffer[512];

void
deliver_rtl(rtl_device* dev, int callbacks, unsigned int period_ms) {
    for (int i = 0; i < callbacks; ++i) {
        if (i > 0) {
            dsd_sleep_ms(period_ms);
        }
        rtl_device_test_usb_stream_deliver(dev, g_rtl_buffer, sizeof g_rtl_buffer);
    }
}

/* A reader that streams but never calls back: the silence runs from the start. The monitor ends the session and
 * cancels the reader. */
void
test_rtl_starvation_from_start(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    const uint64_t started_ms = dsd_realtime_mono_ms();
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    const uint64_t ended_ms = wait_receiver_ended(3000U);
    assert(ended_ms != 0U);
    assert(ended_ms - started_ms >= kLimitMs);
    assert(ended_ms - started_ms < 1000U);
    expect_receiver_ending(-ETIMEDOUT);
    for (int waited_ms = 0; waited_ms < 1000 && !rtl_device_test_usb_stream_reader_exited(dev); ++waited_ms) {
        dsd_sleep_ms(1);
    }
    assert(rtl_device_test_usb_stream_reader_exited(dev));
    rtl_device_test_usb_stream_stop(dev);
    expect_receiver_ending(-ETIMEDOUT);
}

/* 50 callbacks 10 ms apart (longer than the trip limit), then none: the silence counts from the last one. The trip
 * limit applies once they stop, so a silence that had counted through them would trip at once. */
void
test_rtl_silence_after_progress(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    reset_session();
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    deliver_rtl(dev, 50, 10U);
    const uint64_t last_ms = dsd_realtime_mono_ms();
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    expect_running();
    const uint64_t ended_ms = wait_receiver_ended(3000U);
    assert(ended_ms != 0U);
    assert(ended_ms - last_ms >= kLimitMs - kPeriodMs);
    assert(ended_ms - last_ms < 1000U);
    expect_receiver_ending(-ETIMEDOUT);
    rtl_device_test_usb_stream_stop(dev);
}

/* A callback every 10 ms for 2.5 s (longer than the limit), then the session's own stop: nothing is reported. */
void
test_rtl_steady_stream_never_trips(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    reset_session();
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    deliver_rtl(dev, 250, 10U);
    expect_running();
    rtl_device_test_usb_stream_stop(dev);
    expect_running();
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.kind == DSD_INPUT_FAILURE_NONE);
}

/* A stalled transfer can still return callbacks with no bytes: those are not samples and do not end the silence. Zero
 * length buffers every 50 ms for longer than the limit end the session once; nonempty buffers among them keep it
 * running, since the silence counts from the last one. */
void
test_rtl_zero_length_callbacks_do_not_count_as_progress(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    for (int i = 0; i < 20 && !dsd_input_failure_receiver_ended(); ++i) {
        rtl_device_test_usb_stream_deliver(dev, g_rtl_buffer, 0U);
        dsd_sleep_ms(50U);
    }
    assert(dsd_input_failure_receiver_ended() == 1);
    expect_receiver_ending(-ETIMEDOUT);
    assert(dsd_input_failure_generation() == generation + 1U);
    rtl_device_test_usb_stream_stop(dev);
    assert(dsd_input_failure_generation() == generation + 1U);

    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    reset_session();
    dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    for (int i = 0; i < (int)(kOutlastMs / 50U); ++i) {
        rtl_device_test_usb_stream_deliver(dev, g_rtl_buffer, (i & 1) ? 0U : (uint32_t)sizeof g_rtl_buffer);
        dsd_sleep_ms(50U);
    }
    expect_running();
    rtl_device_test_usb_stream_stop(dev);
    expect_running();
}

/* The monitor's verdict waits out the unplug-owner grace like the reader's: a stop inside it (the detach broadcast's)
 * keeps the session silent and joins the monitor while it waits. */
void
test_rtl_stop_inside_grace(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    rtl_device_set_unplug_owner_grace_ms(kSteadyLimitMs);
    reset_session();
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    wait_verdict_in_grace(in_grace);
    expect_running();
    const uint64_t stop_ms = dsd_realtime_mono_ms();
    rtl_device_test_usb_stream_stop(dev);
    assert(dsd_realtime_mono_ms() - stop_ms < 500U);
    expect_running();
    rtl_device_set_unplug_owner_grace_ms(0U);
}

/* The reader returns by itself (-1, a lost device) while the monitor's silence verdict waits out the grace: the two
 * verdicts overlap, and the session is reported exactly once. The reader returns only once the monitor's verdict is
 * pending, however late the monitor runs. Which of the two waits ends first is the scheduler's (a monitor descheduled
 * past its deadline lets the reader's claim first), so either verdict's code is the session's; which one claims when
 * both pass their checks together is test_rtl_racing_verdicts_report_once's. */
void
test_rtl_reader_exit_inside_monitor_grace(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    rtl_device_set_unplug_owner_grace_ms(kLimitMs);
    reset_session();
    const unsigned int generation = dsd_input_failure_generation();
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    wait_verdict_in_grace(in_grace);
    expect_running();
    rtl_device_test_usb_stream_return(-1);
    assert(wait_receiver_ended(2000U) != 0U);
    dsd_sleep_ms(2U * kLimitMs);
    assert(rtl_device_test_usb_stream_reader_exited(dev));
    assert(dsd_input_failure_generation() == generation + 1U);
    dsd_input_failure failure{};
    dsd_input_failure_get(&failure);
    assert(failure.native_code == -ETIMEDOUT || failure.native_code == -1);
    expect_receiver_ending(failure.native_code);
    rtl_device_test_usb_stream_stop(dev);
    assert(dsd_input_failure_generation() == generation + 1U);
    rtl_device_set_unplug_owner_grace_ms(0U);
}

/* The monitor's verdict and a reader exit (-1) both pass their checks together: the monitor's is held between its
 * checks and the report while the reader returns. Only the first to claim it, the reader's, reports. */
void
test_rtl_racing_verdicts_report_once(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device_test_pause_next_verdict_ms(kSteadyLimitMs);
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    dsd_sleep_ms(kLimitMs + 100U);
    expect_running();
    rtl_device_test_usb_stream_return(-1);
    assert(wait_receiver_ended(2000U) != 0U);
    dsd_sleep_ms(kOutlastMs);
    assert(dsd_input_failure_generation() == generation + 1U);
    expect_receiver_ending(-1);
    rtl_device_test_usb_stream_stop(dev);
    assert(dsd_input_failure_generation() == generation + 1U);
}

/* A stall recovery is only for a receiver identified at open (issue #518 D2): this one was not, so with a recovery
 * callback registered all the same, the silence ends the session as before and the callback is never asked. */
int g_recovery_requests = 0;

void
count_recovery_request(void* ctx) {
    (void)ctx;
    g_recovery_requests++;
}

void
test_rtl_ineligible_receiver_ends_as_before(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    g_recovery_requests = 0;
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    assert(dev);
    assert(rtl_device_usb_recovery_eligible(dev) == 0);
    rtl_device_set_recovery_callback(dev, count_recovery_request, nullptr);
    assert(wait_receiver_ended(3000U) != 0U);
    expect_receiver_ending(-ETIMEDOUT);
    assert(g_recovery_requests == 0);
    assert(rtl_device_usb_recovery_take(dev) == 0);
    rtl_device_test_usb_stream_stop(dev);
    expect_receiver_ending(-ETIMEDOUT);
}

/* A monitor thread that cannot start fails the start: the reader is stopped and joined, and nothing is reported. */
void
test_rtl_monitor_start_refused(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kLimitMs);
    reset_session();
    const unsigned int generation = dsd_input_failure_generation();
    rtl_device_test_refuse_progress_monitor(1);
    const uint64_t started_ms = dsd_realtime_mono_ms();
    assert(rtl_device_test_usb_stream_start(ring) == nullptr);
    assert(dsd_realtime_mono_ms() - started_ms < 1000U);
    rtl_device_test_refuse_progress_monitor(0);
    expect_running();
    assert(dsd_input_failure_generation() == generation);
}

#ifdef DSD_TEST_USB_PROGRESS_AIRSPY
/* ---- Airspy ---- */

constexpr uint32_t kGraceMs = 200U;

/* Stands in for the receiver's sample flow: a transfer every 5 ms while feeding and the stream is up. */
struct airspy_feeder {
    std::atomic<int> run{1};
    std::atomic<int> feeding{1};
    std::atomic<int> delivered{0};
    dsd_thread_t thread{};
};

DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
feed_airspy(void* arg) {
    auto* feeder = static_cast<airspy_feeder*>(arg);
    static const float kSamples[64] = {0.25f, -0.25f};
    while (feeder->run.load(std::memory_order_acquire)) {
        if (feeder->feeding.load(std::memory_order_acquire) && airspy_double::deliver(kSamples, 32)) {
            feeder->delivered.fetch_add(1, std::memory_order_relaxed);
        }
        dsd_sleep_ms(5);
    }
    DSD_THREAD_RETURN;
}

void
start_feeder(airspy_feeder* feeder) {
    const int created = dsd_thread_create(&feeder->thread, feed_airspy, feeder);
    assert(created == 0);
}

void
stop_feeder(airspy_feeder* feeder) {
    feeder->run.store(0, std::memory_order_release);
    dsd_thread_join(feeder->thread);
}

uint32_t
retune_target(int i) {
    return (i & 1) ? 852000000U : 851000000U;
}

rtl_device*
start_airspy(input_ring_state* ring) {
    dsd_airspy_config cfg;
    dsd_airspy_config_defaults(&cfg);
    rtl_device* dev = rtl_device_create_airspy(&cfg, ring);
    assert(dev);
    assert(rtl_device_set_frequency(dev, retune_target(0)) == 0);
    reset_session();
    assert(rtl_device_start_async(dev, 16384U) == 0);
    return dev;
}

void
end_airspy(rtl_device* dev) {
    assert(rtl_device_stop_async(dev) == 0);
    rtl_device_destroy(dev);
    assert(!airspy_double::device_open);
}

/* Samples keep flowing while the stream is retuned every 50 ms for 2 s, and through one retune that keeps the stream
 * down for longer than the limit: a restart is not silence, and none of it trips. */
void
test_airspy_retune_storm_healthy(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    rtl_device* dev = start_airspy(ring);
    airspy_feeder feeder;
    start_feeder(&feeder);
    for (int i = 1; i <= 40; ++i) {
        assert(rtl_device_set_frequency(dev, retune_target(i)) == 0);
        dsd_sleep_ms(50U);
    }
    airspy_double::start_delay_ms = (int)kOutlastMs;
    assert(rtl_device_set_frequency(dev, retune_target(41)) == 0);
    airspy_double::start_delay_ms = 0;
    dsd_sleep_ms(kSteadyLimitMs);
    expect_running();
    assert(feeder.delivered.load(std::memory_order_relaxed) > 100);
    stop_feeder(&feeder);
    end_airspy(dev);
    expect_running();
}

/* A stream restarted every 100 ms that never delivers: the restarts do not reset the silence, and the verdict, which
 * restarts do not drop either, ends the session once the limit and then the grace ran out. The trip limit applies from
 * the fourth restart on, however late the test thread makes them, so a silence that restarts reset never reaches it. */
void
test_airspy_restarts_without_samples_trip(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    rtl_device_set_unplug_owner_grace_ms(kGraceMs);
    rtl_device* dev = start_airspy(ring);
    const uint64_t started_ms = dsd_realtime_mono_ms();
    uint64_t ended_ms = 0U;
    const int starts_before = airspy_double::starts;
    for (int i = 1; i <= 30 && ended_ms == 0U; ++i) {
        assert(rtl_device_set_frequency(dev, retune_target(i)) == 0);
        if (airspy_double::starts - starts_before >= 4) {
            rtl_device_test_set_progress_limit_ms(kLimitMs);
        }
        dsd_sleep_ms(100U);
        if (dsd_input_failure_receiver_ended()) {
            ended_ms = dsd_realtime_mono_ms();
        }
    }
    assert(ended_ms != 0U);
    assert(airspy_double::starts - starts_before >= 4);
    assert(ended_ms - started_ms >= kLimitMs + kGraceMs);
    assert(ended_ms - started_ms < 2500U);
    expect_receiver_ending(-ETIMEDOUT);
    end_airspy(dev);
    rtl_device_set_unplug_owner_grace_ms(0U);
}

/* A control change (gains, bias tee) holds the adapter's lock for longer than the limit, without the retune flag,
 * while no samples come; they resume 150 ms after it, several monitor periods, so the monitor samples the source
 * first. The monitor waits on the lock meanwhile, and that wait is the change's time, not silence: nothing trips. */
void
test_airspy_control_change_is_not_silence(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    rtl_device* dev = start_airspy(ring);
    airspy_feeder feeder;
    start_feeder(&feeder);
    dsd_sleep_ms(100U);
    feeder.feeding.store(0, std::memory_order_release);
    dsd_airspy_config cfg;
    dsd_airspy_config_defaults(&cfg);
    airspy_double::control_delay_ms = (int)kOutlastMs;
    assert(rtl_device_airspy_controls(dev, &cfg) == 0);
    airspy_double::control_delay_ms = 0;
    dsd_sleep_ms(150U);
    expect_running();
    feeder.feeding.store(1, std::memory_order_release);
    dsd_sleep_ms(100U);
    expect_running();
    stop_feeder(&feeder);
    end_airspy(dev);
}

/* Retunes that fail and roll back the frequency while samples flow keep the stream: nothing is committed (the raw
 * input-failure latch belongs to whoever reports the retune's failure). */
void
test_airspy_rollback_keeps_running(input_ring_state* ring) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    rtl_device* dev = start_airspy(ring);
    airspy_feeder feeder;
    start_feeder(&feeder);
    airspy_double::failure = 4;
    for (int i = 1; i <= 20; ++i) {
        assert(rtl_device_set_frequency(dev, 853000000U + (uint32_t)i * 12500U) != 0);
        assert(airspy_double::streaming && airspy_double::frequency == retune_target(0));
        dsd_sleep_ms(50U);
    }
    airspy_double::failure = 0;
    dsd_sleep_ms(kSteadyLimitMs);
    expect_running();
    stop_feeder(&feeder);
    end_airspy(dev);
    expect_running();
}

/* The SDK loses the stream, and a retune inside the grace restarts it: with no samples on the new stream the verdict
 * stands, and the session ends once the grace ran out; with samples it is dropped and the session keeps running.
 * Either way the verdict is the stream stop's, not the silence's, so the silence limit is the steady one. */
void
test_airspy_stopped_verdict_grace(input_ring_state* ring, int samples_after_restart) {
    rtl_device_test_set_progress_limit_ms(kSteadyLimitMs);
    rtl_device_set_unplug_owner_grace_ms(kSteadyLimitMs);
    rtl_device* dev = start_airspy(ring);
    airspy_feeder feeder;
    start_feeder(&feeder);
    dsd_sleep_ms(100U);
    feeder.feeding.store(0, std::memory_order_release);
    dsd_sleep_ms(10U);
    const unsigned int in_grace = rtl_device_test_verdicts_in_grace();
    const uint64_t lost_ms = dsd_realtime_mono_ms();
    airspy_double::streaming = false;
    wait_verdict_in_grace(in_grace);
    expect_running();
    assert(rtl_device_set_frequency(dev, retune_target(1)) == 0);
    assert(airspy_double::streaming);
    if (samples_after_restart) {
        feeder.feeding.store(1, std::memory_order_release);
        dsd_sleep_ms(kOutlastMs);
        expect_running();
    } else {
        const uint64_t ended_ms = wait_receiver_ended(5000U);
        assert(ended_ms != 0U);
        assert(ended_ms - lost_ms >= kSteadyLimitMs);
        expect_receiver_ending(-1);
    }
    stop_feeder(&feeder);
    end_airspy(dev);
    rtl_device_set_unplug_owner_grace_ms(0U);
}
#endif

} // namespace

int
main(void) {
    for (size_t i = 0; i < sizeof g_rtl_buffer; ++i) {
        g_rtl_buffer[i] = (unsigned char)(127U + (i & 1U));
    }
    auto ring = std::make_unique<input_ring_state>();
    assert(input_ring_init(ring.get(), 1U << 16) == 0);
    rtl_device_test_set_progress_period_ms(kPeriodMs);

    test_rtl_starvation_from_start(ring.get());
    test_rtl_silence_after_progress(ring.get());
    test_rtl_steady_stream_never_trips(ring.get());
    test_rtl_zero_length_callbacks_do_not_count_as_progress(ring.get());
    test_rtl_stop_inside_grace(ring.get());
    test_rtl_reader_exit_inside_monitor_grace(ring.get());
    test_rtl_racing_verdicts_report_once(ring.get());
    test_rtl_monitor_start_refused(ring.get());
    test_rtl_ineligible_receiver_ends_as_before(ring.get());

#ifdef DSD_TEST_USB_PROGRESS_AIRSPY
    test_airspy_retune_storm_healthy(ring.get());
    test_airspy_restarts_without_samples_trip(ring.get());
    test_airspy_control_change_is_not_silence(ring.get());
    test_airspy_rollback_keeps_running(ring.get());
    test_airspy_stopped_verdict_grace(ring.get(), 0);
    test_airspy_stopped_verdict_grace(ring.get(), 1);
#endif

    rtl_device_test_set_progress_limit_ms(0U);
    rtl_device_test_set_progress_period_ms(0U);
    reset_session();
    input_ring_destroy(ring.get());
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
