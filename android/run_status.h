// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_ANDROID_RUN_STATUS_H_
#define DSD_NEO_ANDROID_RUN_STATUS_H_
#include <dsd-neo/runtime/input_failure.h>
#include <stdint.h>

#ifdef USE_RADIO
#include <dsd-neo/io/rtl_device.h>
#endif

namespace dsd_android {
// Values cross JNI in nativeLifecycleStatus; keep DsdNative.kt in sync.
enum RunReason { kRunPending = 0, kRunCompleted = 1, kRunCancelled = 2, kRunFailed = 3 };

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

    void
    begin(uint64_t id) {
        *this = RunStatus();
        session_id = id;
    }

    void
    mark_initialized() {
        initialized = true;
    }

    void
    finish(int code, bool cancelled, int native_device_error) {
        run_code = code;
        device_error = native_device_error;
        reason = cancelled ? kRunCancelled : (code == 0 ? kRunCompleted : kRunFailed);
    }
};

/** Collect the engine's terminal result after its input cleanup. The device
 * error is latched separately from device ownership and must be copied before
 * the next configure clears it. The caller holds the lifecycle-status lock. */
inline void
collect_run_result(RunStatus& status, int code, bool cancelled) {
    int device_error = 0;
#ifdef USE_RADIO
    device_error = rtl_device_last_open_error();
#endif
    status.finish(code, cancelled, device_error);
    dsd_input_failure_get(&status.input_failure);
}
} // namespace dsd_android
#endif
