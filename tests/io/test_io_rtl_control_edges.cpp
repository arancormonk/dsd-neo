// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device.h>
#include <errno.h>
#include <stdio.h>
#include "rtl_stream_test_support.h"

static int
check(const char* label, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
    }
    return ok ? 0 : 1;
}

int
main(void) {
    int calls = -1;
    const int boundary = rtl_stream_test_boundary_without_center(&calls);
    int failed = check("uninitialized center refuses boundary", boundary == -EINVAL && calls == 0);
    const int activity = rtl_stream_test_activity_swaps();
    failed |= check("test stream swaps publish activity", activity == 7);
    rtl_device_test_usb_gain_fake gain{};
    struct rtl_device* dev = rtl_device_test_usb_gain_device(&gain);
    if (!dev) {
        return 1;
    }
    const int frequency = rtl_device_set_frequency(dev, 851012500U);
    failed |= check("gain fake refuses unsupported tuning", frequency < 0);
    rtl_device_test_usb_gain_device_free(dev);
    rtl_device_test_usb_tuner_fake tuner{};
    dev = rtl_device_test_usb_tuner_device(&tuner);
    if (!dev) {
        return 1;
    }
    const int gain_rc = rtl_device_set_gain(dev, 200);
    failed |= check("tuner fake refuses unsupported gain", gain_rc < 0);
    rtl_device_test_usb_tuner_device_free(dev);
    return failed;
}
