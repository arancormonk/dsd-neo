// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_ring.h>
#include <memory>

struct rtl_device;

// input_ring_state contains C++ atomics. Keep the existing C API test in C,
// and supply a real ring here so the open reaches the wrapped driver call.
extern "C" int dsd_test_rtl_open_failure(void);

int
dsd_test_rtl_open_failure(void) {
    auto ring = std::make_unique<input_ring_state>();
    std::unique_ptr<rtl_device, decltype(&rtl_device_destroy)> dev(rtl_device_create(0, ring.get()),
                                                                   rtl_device_destroy);
    // Even an unexpected successful open must release its borrowed ring before this probe returns.
    return dev == nullptr;
}
