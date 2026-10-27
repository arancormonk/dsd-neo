// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * #782: which Android USB descriptor the engine holds. The app closes a retired connection once its own descriptor is
 * no longer held, so the query names the descriptor taken at open, keeps naming it after the slot moves on to a
 * replacement, and lets go only when the device is closed or its open failed. The "reset first" request the app hands
 * with a descriptor is recorded for the open path.
 */
#include <cstdio>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_ring.h>
#include <memory>
#include "rtl_stream_test_support.h"

extern "C" void rtl_device_test_set_open_fd_failure_hook(int (*hook)(int));
static int failures;
static int open_answer;
static int held_during_open = -2;

static void
check(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        ++failures;
    }
}

/* The descriptor driver: what the engine says it holds while the wrap runs, and how the open ends. */
static int
open_fd(int fd) {
    (void)fd;
    held_during_open = rtl_device_preopened_fd_held();
    return open_answer;
}

int
main() {
    auto ring = std::make_unique<input_ring_state>();
    check(rtl_device_preopened_fd_held() == -1, "nothing is held at rest");
    rtl_device_test_set_open_fd_failure_hook(open_fd);
    rtl_device_set_preopened_fd(31);
    check(rtl_device_preopened_fd_held() == -1, "a recorded descriptor is not held");

    /* An open that fails gives the descriptor straight back. */
    open_answer = -1;
    {
        std::unique_ptr<rtl_device, decltype(&rtl_device_destroy)> dev(rtl_device_create(0, ring.get()),
                                                                       rtl_device_destroy);
        check(dev == nullptr, "the failed open returns no device");
    }
    check(held_during_open == 31, "the descriptor is held before the wrap");
    check(rtl_device_preopened_fd_held() == -1, "a failed open holds nothing");

    /* An open that succeeds (the test dongle stands behind the descriptor) holds it until the device closes. */
    auto dongle = std::make_unique<rtl_device_test_usb_dongle>();
    rtl_device_test_usb_dongle_install(dongle.get());
    open_answer = 1;
    held_during_open = -2;
    rtl_device* dev = rtl_device_create(0, ring.get());
    check(dev != nullptr, "the descriptor opens");
    check(held_during_open == 31, "the descriptor is held before the wrap");
    check(rtl_device_preopened_fd_held() == 31 && rtl_device_preopened_fd_in_use(), "the open device holds it");

    /* The app retires the connection and publishes a replacement: the retired descriptor stays held, and the
     * replacement is not, so an owner waiting for 31 waits and one closing 45 would not have to. */
    rtl_device_set_preopened_fd(-1);
    check(rtl_device_preopened_fd_held() == 31, "clearing the slot does not release the held descriptor");
    rtl_device_set_preopened_fd(45);
    check(rtl_device_preopened_fd_held() == 31, "a replacement in the slot is not the held descriptor");
    rtl_device_destroy(dev);
    check(rtl_device_preopened_fd_held() == -1 && !rtl_device_preopened_fd_in_use(),
          "closing the device releases its descriptor");

    /* The next open takes the replacement, not the retired one. */
    held_during_open = -2;
    dev = rtl_device_create(0, ring.get());
    check(dev != nullptr && held_during_open == 45 && rtl_device_preopened_fd_held() == 45,
          "the next open holds the replacement");
    rtl_device_destroy(dev);
    check(rtl_device_preopened_fd_held() == -1, "and lets it go on close");
    rtl_device_test_usb_dongle_install(nullptr);

    /* The reset request travels with the descriptor; false unless the host asks. */
    check(rtl_device_preopened_fd_reset_first() == 0, "no reset is asked by default");
    rtl_device_set_preopened_fd_reset_first(1);
    check(rtl_device_preopened_fd_reset_first() == 1, "the host's reset request is recorded");
    rtl_device_set_preopened_fd_reset_first(0);
    check(rtl_device_preopened_fd_reset_first() == 0, "and withdrawn");

    /* The Airspy's descriptor is not held by anything here. */
    check(rtl_device_airspy_preopened_fd_held() == -1, "no Airspy descriptor is held");

    rtl_device_set_preopened_fd(-1);
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    return failures ? 1 : 0;
}
