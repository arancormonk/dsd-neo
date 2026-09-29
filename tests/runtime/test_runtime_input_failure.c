// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #578: the session's input failure latch counts its writes (dsd_input_failure_generation()), so a caller can
 * tell whether anything wrote the latch between two reads even when the write repeats the failure latched, which a
 * compare of the values cannot. Every write counts, a clear included, a read does not, and reports that device threads
 * make at the same time are each counted.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/input_failure.h>
#include <stdio.h>

static int
expect_uint(const char* label, unsigned int got, unsigned int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %u want %u\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
test_every_write_counts(void) {
    int rc = 0;
    dsd_input_failure_clear();
    const unsigned int start = dsd_input_failure_generation();
    dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -7);
    rc |= expect_uint("a report counts", dsd_input_failure_generation(), start + 1U);
    dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -7);
    rc |= expect_uint("a report of the failure latched counts", dsd_input_failure_generation(), start + 2U);
    dsd_input_failure failure;
    dsd_input_failure_get(&failure);
    rc |= expect_uint("the latch holds the report",
                      (unsigned int)(failure.kind == DSD_INPUT_FAILURE_DEVICE && failure.native_code == -7), 1U);
    rc |= expect_uint("a read does not count", dsd_input_failure_generation(), start + 2U);
    dsd_input_failure_clear();
    rc |= expect_uint("a clear counts", dsd_input_failure_generation(), start + 3U);
    dsd_input_failure_clear();
    rc |= expect_uint("a clear of a clear latch counts", dsd_input_failure_generation(), start + 4U);
    return rc;
}

enum { k_reporters = 4, k_reports = 5000 };

static DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
    __stdcall
#endif
    reporter_thread(void* arg) {
    (void)arg;
    for (int i = 0; i < k_reports; ++i) {
        dsd_input_failure_report(DSD_INPUT_FAILURE_DEVICE, -1);
    }
    DSD_THREAD_RETURN;
}

static int
test_concurrent_reports_each_count(void) {
    dsd_input_failure_clear();
    const unsigned int start = dsd_input_failure_generation();
    dsd_thread_t threads[k_reporters];
    int started = 0;
    int rc = 0;
    for (; started < k_reporters; ++started) {
        if (dsd_thread_create(&threads[started], reporter_thread, NULL) != 0) {
            rc = 1;
            break;
        }
    }
    for (int i = 0; i < started; ++i) {
        if (dsd_thread_join(threads[i]) != 0) {
            rc = 1;
        }
    }
    if (rc != 0) {
        DSD_FPRINTF(stderr, "reporter threads: create or join failed\n");
        return rc;
    }
    rc |= expect_uint("concurrent reports each count", dsd_input_failure_generation(),
                      start + (unsigned int)(k_reporters * k_reports));
    dsd_input_failure_clear();
    return rc;
}

int
main(void) {
    int rc = test_every_write_counts();
    rc |= test_concurrent_reports_each_count();
    return rc;
}
