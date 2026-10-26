// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_ANDROID_RUN_STATUS_H_
#define DSD_NEO_ANDROID_RUN_STATUS_H_
#include <dsd-neo/core/opts.h>
#include <dsd-neo/runtime/input_failure.h>
#include <stdint.h>
#include <string.h>

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_device.h>
#endif

namespace dsd_android {
// Values cross JNI in nativeLifecycleStatus; keep DsdNative.kt in sync.
enum RunReason { kRunPending = 0, kRunCompleted = 1, kRunCancelled = 2, kRunFailed = 3 };

/** Why a run ended (#647). The first one accepted is kept for the rest of the run. */
enum RunEndCause {
    kEndNone = 0,
    kEndUser = 1,           // StopCause.USER
    kEndDetached = 2,       // StopCause.DEVICE_DETACHED: the USB detach broadcast stopped it
    kEndReceiverFailed = 3, // the radio layer committed the receiver's ending (dsd_input_failure_receiver_ended())
    kEndLifecycle = 4       // StopCause.LIFECYCLE, and any cause a stop did not name
};

/** The local receiver a session was configured with; rtl_tcp and every non-radio input are kReceiverNone. */
enum ReceiverSource { kReceiverNone = 0, kReceiverRtl = 1, kReceiverAirspy = 2 };

/**
 * How long the USB detach broadcast owns an unplug before the reader's own report of it ends the session (#647).
 * Android delivers the broadcast on its main thread, often after librtlsdr's reader has already returned -1; the grace
 * lets the broadcast's stop arrive first, so the session ends as "detached" rather than as a receiver failure.
 */
constexpr uint32_t kUnplugOwnerGraceMs = 2000;

/** Process-wide radio policy for the Android host; nativeInit installs it before any session is configured. */
inline void
install_receiver_policy() {
#ifdef USE_RADIO
    rtl_device_set_unplug_owner_grace_ms(kUnplugOwnerGraceMs);
#endif
}

/** A StopCause wire value. Anything else is a teardown, as StopCause.fromWire() reads it. */
inline RunEndCause
end_cause_from_wire(const char* wire) {
    if (wire == nullptr) {
        return kEndLifecycle;
    }
    if (strcmp(wire, "user") == 0) {
        return kEndUser;
    }
    if (strcmp(wire, "detached") == 0) {
        return kEndDetached;
    }
    return kEndLifecycle;
}

/** The local receiver an input spec names, as the bootstrap leaves it in dsd_opts::audio_in_dev. */
inline ReceiverSource
receiver_source_for_input(const char* dev) {
    if (dsd_opts_audio_in_dev_is_rtl_spec(dev)) {
        return kReceiverRtl;
    }
    if (dsd_opts_audio_in_dev_is_airspy_spec(dev)) {
        return kReceiverAirspy;
    }
    return kReceiverNone;
}

/** One session's latched lifecycle facts, independent of the snapshot consumer.
 * The JNI owner protects this with its own short-held mutex, never the configure
 * lock: a status poll must not block the UI behind a slow device open. */
struct RunStatus {
    uint64_t session_id = 0;
    bool initialized = false;
    RunReason reason = kRunPending;
    int run_code = 0;
    int device_error = 0;
    dsd_input_failure input_failure = {};
    RunEndCause end_cause = kEndNone;
    ReceiverSource receiver_source = kReceiverNone;

    void
    begin(uint64_t id) {
        *this = RunStatus();
        session_id = id;
    }

    /** The bootstrap accepted the session's configuration; @p source is its configured receiver. */
    void
    note_configured(ReceiverSource source) {
        receiver_source = source;
    }

    void
    mark_initialized() {
        initialized = true;
    }

    /**
     * Whether the session is configured or running with a local RTL-SDR or Airspy. Ends with the run's result: a
     * USB detach after that concerns no session.
     */
    bool
    uses_local_receiver() const {
        return session_id != 0 && reason == kRunPending && receiver_source != kReceiverNone;
    }

    /**
     * Accept @p cause as the run's ending unless one was already accepted. A committed receiver ending
     * (@p receiver_ended) is older than any cause still arriving, so it is taken first. A finished run takes nothing.
     */
    void
    note_end(RunEndCause cause, bool receiver_ended) {
        if (session_id == 0 || reason != kRunPending || end_cause != kEndNone) {
            return;
        }
        end_cause = receiver_ended ? kEndReceiverFailed : cause;
    }

    void
    finish(int code, bool cancelled, int native_device_error) {
        run_code = code;
        device_error = native_device_error;
        reason = cancelled ? kRunCancelled : (code == 0 ? kRunCompleted : kRunFailed);
    }
};

/**
 * nativeStop: latch the stop's @p cause before the exit flag is raised. A user Stop latched here wins over a receiver
 * ending the radio layer commits later (a stop and an unplug report can cross in rtl_device_end_for_receiver()), and
 * an ending committed earlier wins over the stop. The caller holds the lifecycle-status lock.
 */
inline void
note_stop_cause(RunStatus& status, RunEndCause cause) {
    status.note_end(cause, dsd_input_failure_receiver_ended() != 0);
}

/**
 * Publish the radio layer's committed receiver ending while the run still goes, so the UI hears of it before the
 * engine's cleanup ends. Not the raw failure latch: an Airspy reconfigure can latch a device failure and roll back.
 * The caller holds the lifecycle-status lock.
 */
inline void
note_receiver_fact(RunStatus& status) {
    status.note_end(kEndNone, dsd_input_failure_receiver_ended() != 0);
}

/** Collect the engine's terminal result after its input cleanup. The device
 * error is latched separately from device ownership and must be copied before
 * the next configure clears it. The caller holds the lifecycle-status lock. */
inline void
collect_run_result(RunStatus& status, int code, bool cancelled) {
    int device_error = 0;
#ifdef USE_RADIO
    device_error = rtl_device_last_open_error();
#endif
    // An ending no poll saw is still this run's.
    note_receiver_fact(status);
    status.finish(code, cancelled, device_error);
    dsd_input_failure_get(&status.input_failure);
}
} // namespace dsd_android
#endif
