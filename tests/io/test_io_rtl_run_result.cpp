// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "run_status.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <stdint.h>
#include <stdio.h>

extern "C" int dsd_test_rtl_open_failure(void);

static int open_calls = 0;

struct rtlsdr_dev;
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

    if (rc == 0) {
        DSD_FPRINTF(stderr, "IO_RTL_RUN_RESULT: OK\n");
    }
    return rc;
}
