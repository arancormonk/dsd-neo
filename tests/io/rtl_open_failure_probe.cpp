// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_ring.h>

// input_ring_state contains C++ atomics. Keep the existing C API test in C,
// and supply a real ring here so the open reaches the wrapped driver call.
extern "C" int dsd_test_rtl_open_failure(void);

int
dsd_test_rtl_open_failure(void) {
    input_ring_state ring{};
    return rtl_device_create(0, &ring) == nullptr;
}
