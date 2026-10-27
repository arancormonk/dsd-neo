// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <initializer_list>
#include <memory>
#include "rtl_stream_test_support.h"

struct rtl_device;

extern "C" void rtl_device_test_set_open_fd_failure_hook(int (*hook)(int));
static int result;
static int calls;
static int failures;

static void
check(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        ++failures;
    }
}

static int
open_fd_failure(int fd) {
    ++calls;
    check(fd == 31 && rtl_device_preopened_fd_in_use(), "descriptor claimed before driver invocation");
    return result;
}

/* #782: the session's input failure is a device failure with the open's code, so the UI says the receiver could not
 * be opened rather than the generic line. */
static bool
reported_device_failure(int code) {
    dsd_input_failure failure = {};
    dsd_input_failure_get(&failure);
    return failure.kind == DSD_INPUT_FAILURE_DEVICE && failure.native_code == code;
}

int
main() {
    auto ring = std::make_unique<input_ring_state>();
    rtl_device_test_set_open_fd_failure_hook(open_fd_failure);
    rtl_device_set_preopened_fd(31);
    for (const int code : {-6, -3, -1}) {
        result = code;
        rtl_device_clear_open_error();
        dsd_input_failure_clear();
        std::unique_ptr<rtl_device, decltype(&rtl_device_destroy)> dev(rtl_device_create(0, ring.get()),
                                                                       rtl_device_destroy);
        check(dev == nullptr, "descriptor open failure returns no device");
        check(rtl_device_last_open_error() == code, "descriptor error classification retained");
        check(reported_device_failure(code), "a descriptor open failure is the session's device failure");
        check(!rtl_device_preopened_fd_in_use(), "failed descriptor claim released for host cleanup");
        check(rtl_device_preopened_fd_is_set(), "host still owns descriptor setting");
    }
    check(calls == 3, "every open reached descriptor driver path");
    rtl_device_set_preopened_fd(-1);
    rtl_device_clear_open_error();
    rtl_device_test_set_open_fd_failure_hook(nullptr);

    /* The open by index, over a dongle that refuses to open. */
    auto dongle = std::make_unique<rtl_device_test_usb_dongle>();
    dongle->faults[RTL_TEST_DONGLE_OPEN].fail_from_call = 1;
    rtl_device_test_usb_dongle_install(dongle.get());
    dsd_input_failure_clear();
    {
        std::unique_ptr<rtl_device, decltype(&rtl_device_destroy)> dev(rtl_device_create(0, ring.get()),
                                                                       rtl_device_destroy);
        check(dev == nullptr, "an index open failure returns no device");
        check(dongle->calls[RTL_TEST_DONGLE_OPEN] == 1, "the open reached the dongle");
        check(rtl_device_last_open_error() == -6, "index error classification retained");
        check(reported_device_failure(-6), "an index open failure is the session's device failure");
    }
    rtl_device_test_usb_dongle_install(nullptr);
    rtl_device_clear_open_error();
    dsd_input_failure_clear();
    return failures ? 1 : 0;
}
