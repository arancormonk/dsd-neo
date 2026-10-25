// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "run_status.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_failure.h>
#include <stdint.h>
#include <stdio.h>

extern "C" int dsd_test_rtl_open_failure(void);

static int open_calls = 0;

// GNU ld --wrap requires external linkage and the reserved symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, misc-use-internal-linkage)
extern "C" int __wrap_rtlsdr_open(struct rtlsdr_dev** device, uint32_t index);

int
__wrap_rtlsdr_open(struct rtlsdr_dev** device, uint32_t index) {
    (void)device;
    (void)index;
    ++open_calls;
    return -6; // Inject LIBUSB_ERROR_BUSY only at the driver boundary.
}

// NOLINTEND(bugprone-reserved-identifier, misc-use-internal-linkage)

static int
expect(const char* tag, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "IO_RTL_RUN_RESULT: %s failed\n", tag);
        return 1;
    }
    return 0;
}

/* A session that has been configured and is running (the JNI's state between nativeConfigure and nativeRun's end). */
static dsd_android::RunStatus
running_session(uint64_t id) {
    dsd_android::RunStatus status;
    status.begin(id);
    status.note_configured(dsd_android::kReceiverRtl);
    dsd_input_failure_clear();
    return status;
}

/* #647: the first accepted ending of a run is kept, whichever path saw it first: nativeStop (the cause Kotlin passed),
 * a status poll or the run's own result collection (the committed receiver ending). */
static int
expect_end_causes(void) {
    using namespace dsd_android;
    int rc = 0;

    rc |=
        expect("wire causes",
               end_cause_from_wire("user") == kEndUser && end_cause_from_wire("detached") == kEndDetached
                   && end_cause_from_wire("lifecycle") == kEndLifecycle && end_cause_from_wire("USER") == kEndLifecycle
                   && end_cause_from_wire("") == kEndLifecycle && end_cause_from_wire(nullptr) == kEndLifecycle);

    // The user's Stop, then the reader's report of the unplug during cleanup: still the user's stop.
    RunStatus status = running_session(60);
    note_stop_cause(status, kEndUser);
    dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -1);
    dsd_input_failure_commit_receiver_end();
    note_receiver_fact(status);
    rc |= expect("user stop then unplug stays a user stop", status.end_cause == kEndUser);
    collect_run_result(status, 0, true);
    rc |= expect("and its result keeps it", status.end_cause == kEndUser && status.reason == kRunCancelled);

    // The detach broadcast's stop, then the user's Stop now: still an unplug.
    status = running_session(61);
    note_stop_cause(status, kEndDetached);
    note_stop_cause(status, kEndUser);
    rc |= expect("detach then Stop now stays a detach", status.end_cause == kEndDetached);
    collect_run_result(status, 0, true);
    rc |= expect("and its result keeps it", status.end_cause == kEndDetached);

    // The committed receiver ending, read by a poll while the run still goes, then the user's Stop.
    status = running_session(62);
    dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -5);
    note_receiver_fact(status);
    rc |= expect("a reported device failure alone is no receiver ending", status.end_cause == kEndNone);
    dsd_input_failure_commit_receiver_end();
    note_receiver_fact(status);
    rc |= expect("the committed ending is the receiver's failure", status.end_cause == kEndReceiverFailed);
    note_stop_cause(status, kEndUser);
    rc |= expect("a later Stop keeps the receiver's failure", status.end_cause == kEndReceiverFailed);

    // Committed with no poll in between: the stop sees it first, and so does the result collection.
    status = running_session(63);
    dsd_input_failure_commit_receiver_end();
    note_stop_cause(status, kEndUser);
    rc |= expect("a Stop after an unpolled ending keeps the ending", status.end_cause == kEndReceiverFailed);
    status = running_session(64);
    dsd_input_failure_commit_receiver_end();
    collect_run_result(status, 1, false);
    rc |= expect("an ending nobody polled reaches the result", status.end_cause == kEndReceiverFailed);

    // A finished run is closed, and a new session starts with none.
    dsd_input_failure_clear();
    note_stop_cause(status, kEndUser);
    rc |= expect("a finished run takes no later cause", status.end_cause == kEndReceiverFailed);
    status.begin(65);
    rc |= expect("a new session starts with no ending", status.end_cause == kEndNone);
    dsd_input_failure_clear();
    return rc;
}

static int
expect_receiver_sources(void) {
    using namespace dsd_android;
    int rc = 0;
    rc |= expect("RTL-SDR input", receiver_source_for_input("rtl") == kReceiverRtl
                                      && receiver_source_for_input("rtl:0:851.0125M:0:0:24") == kReceiverRtl);
    rc |= expect("Airspy input", receiver_source_for_input("airspy") == kReceiverAirspy
                                     && receiver_source_for_input("airspy:26a464dc2b3c5c63") == kReceiverAirspy);
    rc |= expect("rtl_tcp and other inputs are not local receivers",
                 receiver_source_for_input("rtltcp:192.168.1.2:1234") == kReceiverNone
                     && receiver_source_for_input("udp:0.0.0.0:7355") == kReceiverNone
                     && receiver_source_for_input("") == kReceiverNone
                     && receiver_source_for_input(nullptr) == kReceiverNone);

    RunStatus status;
    rc |= expect("no session uses no receiver", !status.uses_local_receiver());
    status.begin(70);
    rc |= expect("a session still configuring uses none yet", !status.uses_local_receiver());
    status.note_configured(kReceiverRtl);
    rc |= expect("a configured RTL session uses one",
                 status.uses_local_receiver() && status.receiver_source == kReceiverRtl);
    dsd_input_failure_clear();
    collect_run_result(status, 0, false);
    rc |= expect("a finished session uses none, and keeps its receiver",
                 !status.uses_local_receiver() && status.receiver_source == kReceiverRtl);
    status.begin(71);
    status.note_configured(kReceiverNone);
    rc |= expect("an rtl_tcp session uses none", !status.uses_local_receiver());
    status.begin(72);
    rc |= expect("a new session forgets the old receiver", status.receiver_source == kReceiverNone);
    return rc;
}

int
main(void) {
    dsd_android::RunStatus result;
    result.begin(42);
    rtl_device_clear_open_error();

    // The real rtl_device_create failure path frees the device before returning.
    const int run_code = dsd_test_rtl_open_failure();
    int rc = expect("driver claim failure exercised", open_calls == 1 && run_code == 1);
    // Call the same collector as nativeRun, without supplying a device-error value.
    dsd_android::collect_run_result(result, run_code, false);
    rc |= expect("claim error reaches retained run result after cleanup", result.device_error == -6);
    rc |= expect("failed run identity and reason retained", result.session_id == 42 && !result.initialized
                                                                && result.reason == dsd_android::kRunFailed
                                                                && result.run_code == run_code);

    // Starting another configure clears the device latch, but the published result
    // owns its error value until its own begin() resets the session.
    rtl_device_clear_open_error();
    rc |= expect("retained result is independent of the device latch", result.device_error == -6);
    result.begin(43);
    rc |= expect("next session starts with no terminal result",
                 result.session_id == 43 && result.device_error == 0 && result.run_code == 0
                     && result.reason == dsd_android::kRunPending && !result.initialized);
    dsd_android::collect_run_result(result, 0, false);
    rc |= expect("next non-USB run has no stale claim error", result.device_error == 0
                                                                  && result.reason == dsd_android::kRunCompleted
                                                                  && result.run_code == 0 && result.session_id == 43);

    rc |= expect_end_causes();
    rc |= expect_receiver_sources();

    // #647: the JNI's init installs the unplug-owner grace through this helper before any session starts.
    rtl_device_set_unplug_owner_grace_ms(0);
    dsd_android::install_receiver_policy();
    rc |= expect("Android gives the detach broadcast 2 s to own an unplug",
                 rtl_device_unplug_owner_grace_ms() == 2000U && dsd_android::kUnplugOwnerGraceMs == 2000U);
    rtl_device_set_unplug_owner_grace_ms(0);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "IO_RTL_RUN_RESULT: OK\n");
    }
    return rc;
}
