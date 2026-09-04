// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine RTL enumeration short-circuit for a pre-opened USB descriptor.
 *
 * On Android the app opens the dongle and hands down the descriptor, and libusb
 * device discovery is disabled for the process. rtlsdr_get_device_count() then
 * reports zero, which the engine otherwise treats as "no supported devices" and
 * aborts the run. These cases pin that an injected descriptor bypasses enumeration
 * and that removing it restores the normal failure.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/io/rtl_stream_fwd.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

static int g_device_count = 0;
static int g_device_count_calls = 0;
static int g_usb_strings_calls = 0;
static int g_rtl_create_calls = 0;
static int g_fake_rtl_context = 0;

// GNU ld --wrap entry points must keep the reserved __wrap_* symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
uint32_t
__wrap_rtlsdr_get_device_count(void) {
    g_device_count_calls++;
    return (uint32_t)g_device_count;
}

int
__wrap_rtlsdr_get_device_usb_strings(uint32_t index, char* manufact, char* product, char* serial) {
    (void)index;
    g_usb_strings_calls++;
    DSD_SNPRINTF(manufact, 256, "%s", "fake");
    DSD_SNPRINTF(product, 256, "%s", "fake");
    DSD_SNPRINTF(serial, 256, "%s", "0001");
    return 0;
}

int
__wrap_rtl_stream_create(dsd_opts* opts, RtlSdrContext** out_ctx) {
    (void)opts;
    g_rtl_create_calls++;
    *out_ctx = (RtlSdrContext*)&g_fake_rtl_context;
    return 0;
}

int
__wrap_rtl_stream_start(RtlSdrContext* ctx) {
    (void)ctx;
    return 0;
}

int
__wrap_rtl_stream_destroy(RtlSdrContext* ctx) {
    (void)ctx;
    return 0;
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

/**
 * @brief Runs one local-USB setup attempt and reports what the engine decided.
 *
 * playfiles keeps the run out of the decode loop, so what is measured is the
 * input-setup decision and nothing after it — the stream wrappers are there only so
 * that a future change cannot quietly reach a real dongle from a unit test.
 */
static int
run_local_rtl_setup(int* out_rc, int* out_in_type, int* out_dev_index) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    if (opts == NULL || state == NULL) {
        DSD_FPRINTF(stderr, "alloc-failed: runtime\n");
        free(opts);
        free(state);
        return 1;
    }

    initOpts(opts);
    initState(state);
    opts->playfiles = 1;
    opts->audio_out_type = 9;
    DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "rtl:0");

    g_device_count_calls = 0;
    g_usb_strings_calls = 0;
    g_rtl_create_calls = 0;
    /* A rejected run leaves the shutdown flag raised for the next one. */
    dsd_exitflag_store(0);

    *out_rc = dsd_engine_run_with_lifecycle(opts, state, NULL);
    *out_in_type = opts->audio_in_type;
    *out_dev_index = opts->rtl_dev_index;

    freeState(state);
    free(opts);
    free(state);
    return 0;
}

static int
test_empty_enumeration_rejects_local_usb(void) {
    g_device_count = 0;
    rtl_device_set_preopened_fd(-1);

    int rc = 0;
    int in_type = 0;
    int dev_index = 0;
    if (run_local_rtl_setup(&rc, &in_type, &dev_index) != 0) {
        return 1;
    }

    int test_rc = 0;
    test_rc |= expect_true("no devices rejects setup", rc != 0);
    test_rc |= expect_true("enumeration ran", g_device_count_calls == 1);
    test_rc |= expect_true("RTL input not selected", in_type != AUDIO_IN_RTL);
    return test_rc;
}

static int
test_preopened_descriptor_skips_enumeration(void) {
    /* Exactly the device-discovery-disabled shape: the count would be zero. */
    g_device_count = 0;
    rtl_device_set_preopened_fd(11);

    int rc = 0;
    int in_type = 0;
    int dev_index = -1;
    if (run_local_rtl_setup(&rc, &in_type, &dev_index) != 0) {
        rtl_device_set_preopened_fd(-1);
        return 1;
    }
    rtl_device_set_preopened_fd(-1);

    int test_rc = 0;
    test_rc |= expect_true("descriptor accepts setup", rc == 0);
    test_rc |= expect_true("enumeration skipped", g_device_count_calls == 0);
    test_rc |= expect_true("usb strings skipped", g_usb_strings_calls == 0);
    test_rc |= expect_true("RTL input selected", in_type == AUDIO_IN_RTL);
    test_rc |= expect_true("device index unchanged", dev_index == 0);
    return test_rc;
}

static int
test_clearing_descriptor_restores_enumeration(void) {
    /* Guards against a latched short-circuit: after a USB detach the engine must go
     * back to enumerating, and back to failing when there is nothing to find. */
    rtl_device_set_preopened_fd(11);
    rtl_device_set_preopened_fd(-1);
    g_device_count = 0;

    int rc = 0;
    int in_type = 0;
    int dev_index = 0;
    if (run_local_rtl_setup(&rc, &in_type, &dev_index) != 0) {
        return 1;
    }

    int test_rc = 0;
    test_rc |= expect_true("cleared descriptor rejects setup", rc != 0);
    test_rc |= expect_true("enumeration ran again", g_device_count_calls == 1);
    test_rc |= expect_true("RTL input not selected", in_type != AUDIO_IN_RTL);
    return test_rc;
}

static int
test_normal_enumeration_still_selects_a_device(void) {
    g_device_count = 2;
    rtl_device_set_preopened_fd(-1);

    int rc = 0;
    int in_type = 0;
    int dev_index = -1;
    if (run_local_rtl_setup(&rc, &in_type, &dev_index) != 0) {
        return 1;
    }

    int test_rc = 0;
    test_rc |= expect_true("populated enumeration accepts setup", rc == 0);
    test_rc |= expect_true("enumeration ran", g_device_count_calls == 1);
    test_rc |= expect_true("device strings read per device", g_usb_strings_calls == 2);
    test_rc |= expect_true("RTL input selected", in_type == AUDIO_IN_RTL);
    test_rc |= expect_true("device index unchanged", dev_index == 0);
    return test_rc;
}

int
main(void) {
    int rc = 0;

    rc |= test_empty_enumeration_rejects_local_usb();
    rc |= test_preopened_descriptor_skips_enumeration();
    rc |= test_clearing_descriptor_restores_enumeration();
    rc |= test_normal_enumeration_still_selects_a_device();

    rtl_device_set_preopened_fd(-1);
    dsd_exitflag_store(0);

    if (rc == 0) {
        DSD_FPRINTF(stderr, "ENGINE_RTL_PREOPENED_FD: OK\n");
    }
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
